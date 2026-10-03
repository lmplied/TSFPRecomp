/*
 * hle_dsound.c -- DirectSound buffers replaced by name.
 *
 * Adapted from doaxbv-re (https://github.com/NoRain211/doaxbv-re,
 * recomp-runtime/dsound_service_adapter.c), GPL-3.0.
 *
 * The game's own DirectSound drives an emulated MCPX audio chip, and every
 * piece of chip behaviour it relies on has to be found before it works: the
 * codec-ready bit, the DSP doorbells, starting the chip, and then its
 * interrupt, whose handler still declined it. Burnout 2 stops a sound before
 * its attract movie and waits for IDirectSoundBuffer_GetStatus to stop saying
 * PLAYING -- 4.5 million polls, forever.
 *
 * doaxbv-re's answer, reused here: replace the buffer functions and keep each
 * buffer's play state (playing, cursor, loop, frequency) in a model on the
 * host clock (dsound_buffer_model.c), sending the samples to XAudio2
 * (audio_output_xaudio2.cpp). GetStatus then never waits on hardware.
 *
 * Differences from doaxbv-re:
 *  - keyed by XDK function name through tools.xdk_symbols, not by one title's
 *    addresses, so any title on a covered XDK gets it;
 *  - the public IDirectSoundBuffer_* entry points are replaced; creation,
 *    SetVolume and the rest still run the game's own code, which keeps the
 *    settings object this reads current. SetFormat and SetBufferData are
 *    replaced too, but only for a buffer whose settings this file can read
 *    (settings_in_layout); any other buffer gets the game's own code;
 *  - a buffer whose format cannot be played still gets a clock, so status and
 *    position always advance: doaxbv-re fell back to the original function,
 *    which here is the path that hangs;
 *  - a lock, since nothing guarantees one guest thread.
 *
 * Where the fields live (XDK 5344, Burnout 2). The interface pointer the game
 * holds is the buffer object + 0x1C, and the object keeps two settings
 * pointers: +0x1C for the buffer (data, size, play and loop regions -- read
 * by CDirectSoundBufferSettings_SetBufferData, SetLoopRegion and the
 * play-region helper) and +0x10 for the voice (format, rate, alignment,
 * volume -- written by the format packer and CDirectSoundVoice_SetVolume).
 * doaxbv-re reads all of it through the first, which is only right if both
 * point at one object; each field is read here from the one the game's own
 * code writes it to. The offsets inside match doaxbv-re's.
 */
/* The NT type vocabulary: <windows.h> on Windows, the POSIX primitives
 * (critical sections, GetTickCount64) elsewhere. */
#include "platform/xbox_winnt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "dsound_buffer_model.h"
#include "xbox_adpcm.h"
#include "audio_output.h"

void hle_dsound_stream_tick(uint64_t now);   /* hle_dsound_stream.c */

/* Reads a RECOMP_* switch; see xbox_memory_layout.h. Declared here rather
 * than including the kernel header, which this file has no other need of. */
int xbox_EnvSwitch(const char *name, int default_on);

enum {
    SET_FORMAT     = 0x0Cu,   /* tag | channels << 16 | bits << 24 */
    SET_RATE       = 0x10u,
    SET_ALIGN      = 0x14u,
    SET_VOLUME     = 0x1Cu,   /* hundredths of a dB, headroom applied */
    SET_DATA       = 0xB8u,
    SET_SIZE       = 0xBCu,
    SET_PLAY_START = 0xC0u,
    SET_PLAY_LEN   = 0xC4u,
    SET_LOOP_START = 0xC8u,
    SET_LOOP_LEN   = 0xCCu,

    TAG_PCM   = 0x0001u,
    TAG_ADPCM = 0x0069u,
    SLOTS     = 256u,
};

typedef struct Buffer {
    uint32_t iface;              /* the IDirectSoundBuffer the game holds */
    uint32_t data, size, format;
    int      output;             /* samples we can send to the host */
    /* IDirectSoundBuffer_Pause: the model is stopped with its cursor kept,
     * and paused_flags holds the play flags to resume with. */
    int      paused;
    uint32_t paused_flags;
    RecompDsoundBufferModel model;        /* what the game is told */
    RecompDsoundBufferModel output_model; /* what has been sent */
    /* Latency bookkeeping: when the sound was started (QPC microseconds,
     * 0 = nothing awaited), and whether the host voice has been fed since
     * it was last reset (a dry voice after that is an underrun). */
    uint64_t play_us;
    int      fed;
    int32_t  level;              /* the volume last sent, hundredths of a dB */
} Buffer;

static Buffer g_buffers[SLOTS];
static CRITICAL_SECTION g_lock;
static INIT_ONCE g_lock_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_lock(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_lock);
    return TRUE;
}

static void lock(void)
{
    InitOnceExecuteOnce(&g_lock_once, init_lock, NULL, NULL);
    EnterCriticalSection(&g_lock);
}

static void unlock(void) { LeaveCriticalSection(&g_lock); }

/* ── The clock and the lead ────────────────────────────────────────
 *
 * The output model used to submit exactly what the host clock had consumed
 * since the last pump: a sound started by Play reached the host only at the
 * next pump at least 10 ms later, with nothing queued behind it, and every
 * later chunk arrived as the one before it ran out. On the GetTickCount64
 * clock (15.6 ms steps) and a title that calls DirectSoundDoWork once a frame
 * (Future Perfect: 60 a second), each pump fed ~16 ms of sound just in time,
 * so the voice ran dry whenever a pump came late, and every dry spell pushed
 * the rest of the sound later still.
 *
 * Now the output model runs LEAD milliseconds ahead of the game's model, on
 * a millisecond performance-counter clock, and Play feeds the host at once:
 * the sound starts within XAudio2's own latency and the queue always holds
 * the lead. What the title sees (status, cursor) is the game model, as
 * before. RECOMP_DSOUND_LEAD_MS sets the lead (default 40); 0 restores the
 * old behaviour exactly (tick clock, no lead), for comparison. */
static uint32_t g_lead_ms = 0xFFFFFFFFu;

static uint32_t lead_ms(void)
{
    if (g_lead_ms == 0xFFFFFFFFu) {
        const char *v = getenv("RECOMP_DSOUND_LEAD_MS");
        g_lead_ms = 40u;
        if (v && *v) {
            long ms = strtol(v, NULL, 10);
            g_lead_ms = ms < 0 ? 0u : ms > 90 ? 90u : (uint32_t)ms;
        }
        fprintf(stderr, "[DSOUND] buffers: output %u ms ahead of the clock%s\n",
                g_lead_ms, g_lead_ms ? " (performance counter)"
                                     : " -- the old just-in-time feed (tick clock)");
        fflush(stderr);
    }
    return g_lead_ms;
}

static uint64_t qpc_us(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart * 1000000.0 / (double)freq.QuadPart);
}

static uint64_t now_ms(void)
{
    return lead_ms() ? qpc_us() / 1000u : GetTickCount64();
}

/* What the buffers did since start, for the five-second line. */
static struct {
    uint64_t plays, first_sent, first_us_sum, first_us_max;
    uint64_t submits, underruns, resets;
    uint64_t freq_calls, freq_same, freq_changed, freq_changed_playing;
    uint64_t commits_3d, commits_original;
} g_stat;

/* Buffer settings: [iface] = [object + 0x1C]. Voice settings: [object + 0x10],
 * i.e. [iface - 0x0C]. */
static int voice_field(uint32_t offset)
{
    return offset == SET_FORMAT || offset == SET_RATE ||
           offset == SET_ALIGN || offset == SET_VOLUME;
}

/* A guest address worth dereferencing: title RAM, or the contiguous window
 * where MmAllocateContiguousMemory memory lives (xbox_memory_layout.c). */
static int guest_readable(uint32_t va, uint32_t bytes)
{
    if (va >= 0x00010000u && (uint64_t)va + bytes <= g_xbox_total_ram)
        return 1;
    return va >= 0x80000000u && (uint64_t)va + bytes <= 0x80000000ull + 0x04000000ull;
}

/* 0 when either pointer on the way is not guest memory. Buffer Release is not
 * replaced, so a buffer the title freed keeps its slot here until a change is
 * noticed, and its settings pointers then hold whatever reused that memory.
 * Burnout 2 crashed reading one (guest 0xFFFFD9AC) from StopEx while loading a
 * race. A 0 reads as "the buffer changed", and pump() and model_for() retire
 * the slot. */
static uint32_t setting(uint32_t iface, uint32_t offset)
{
    uint32_t holder = voice_field(offset) ? iface - 0x0Cu : iface;
    uint32_t object;

    if (!guest_readable(holder, 4u))
        return 0u;
    object = HLE_MEM32(holder);
    if (!guest_readable(object + offset, 4u))
        return 0u;
    return HLE_MEM32(object + offset);
}

static uint32_t slot_of(const Buffer *b) { return (uint32_t)(b - g_buffers); }

/* Drop what the host voice holds; the next submission starts a new one. */
static void reset_output(Buffer *b)
{
    recomp_audio_output_reset_voice(slot_of(b));
    b->fed = 0;
    g_stat.resets++;
}

static void retire(Buffer *b)
{
    recomp_audio_output_reset_voice(slot_of(b));
    memset(b, 0, sizeof *b);
}

/* ── Mix bins and 3D ──────────────────────────────────────────────
 *
 * A voice is heard at its volume plus what its mix bins and, for a 3D voice,
 * its 3D state add (recomp_dsound_mix_adjust). On the console both reach the
 * sound only through the chip: CMcpxVoiceClient_SetVolume works the voice's
 * per-bin hardware volumes out of the settings object and the 3D data block,
 * and CMcpxVoiceClient_Commit3dSettings computes that block -- but only for a
 * voice that is running on the chip ((client+0x12 & 3) == 3), and none is
 * here, since IDirectSoundBuffer_Play is replaced. So every 3D sound played
 * at its plain volume wherever it was: Future Perfect's effects were all as
 * loud near as far (play-test 3).
 *
 * Commit3dSettings is replaced below to compute the block for a 3D voice
 * anyway, by the XDK's own CDirectSound3DCalculator, and the level is read
 * at every submission. Both read the voice layout of the XDK they were
 * written against (5849, Future Perfect): the replacement acts, and the
 * level includes the mix bins, only when the title's own code for these
 * functions is that code byte for byte (call displacements aside), checked
 * once. Any other title keeps the old behaviour exactly. */
HLE_IMPORT_VAR(CMcpxVoiceClient_Commit3dSettings_0);
HLE_IMPORT_VAR(CMcpxVoiceClient_SetVolume);
HLE_IMPORT_VAR(CDirectSoundVoice_SetVolume);
HLE_IMPORT_VAR(CDirectSoundStream_SetVolume);
HLE_IMPORT_VAR(CDirectSound3DCalculator_Calculate3D);
HLE_IMPORT_VAR(CDirectSound3DCalculator_GetVoiceData);

/* CMcpxVoiceClient_Commit3dSettings_0: client+0x12 & 3 == 3, then
 * Calculate3D(listener = [[client+8]+0xC]+0x30, params = [settings+0xB4]) and
 * GetVoiceData(listener, params, [[client+8]+0xC]+0x80, params+0x7C,
 * [settings+0xB8]), settings = [client+0x70]; then the change flags cleared
 * and the chip programmed. */
static const char k_commit_code[] =
    "558bec51568bf18d4dfce8........8a461224033c0375628b4670ffb0b40000008b4608"
    "8b400c83c03050e8........8b4670ffb0b80000008b90b40000008b4e088b490c8d427c"
    "508d8180000000505283c13051e8........8b46708b80b40000008320008b46708b80b4"
    "00000083607c008bcee8........";
/* CMcpxVoiceClient_SetVolume's first 38 bytes: the call at +0x21 is the
 * helper that computes the per-bin hardware volumes. */
static const char k_setvol_code[] =
    "558bec83ec2c8365fc00568bf18d4df8e8........f646120174708d45d4508bcee8........";
/* That helper up to its per-bin store: the 3D sums over the data block at
 * [settings+0xB8] (when settings+0xB bit 0 is set), the mix bins at
 * settings+0x24 (count), +0x28 (bins), +0x30 (volumes by bin), the volume at
 * +0x1C, and which sum each bin takes. recomp_dsound_mix_adjust mirrors it. */
static const char k_helper_code[] =
    "558bec83ec4c8365dc00538b597083bbb8000000005657894dd40f84cc000000f6430b01"
    "0f84c20000008bc38b80b80000008b7808037804b8f0d8ffff3bf8897df07d048bf8eb06"
    "85ff7e0533ff897df08b51708b92b80000008b721c03721403720c03f73bf08975f47d05"
    "8945f4eb0885f67e048365f4008b51708bb2b80000008b561c03561003d73bd08955f87d"
    "058945f8eb0885d27e048365f8008b51708b92b80000008b721c03721803720c03f73bf0"
    "8975ec7d058945eceb0885f67e048365ec008b93b80000008b522003d73bd08955fc7d05"
    "8945fceb0885d27e048365fc00c745dc010000000fb679648b432433d2f7f733f685ff89"
    "75e48945e00f863d010000bfff0f000033db8bc60faf45e0895de88945d8eb038b45d83b"
    "d80f82240100008d46010faf45e03bd80f83150100008b51700fb6441a288b4482300342"
    "1c8d749db4f7d8837ddc00890674368a5c132880fb02742880fb05761e80fb07761480fb"
    "09760a80fb0a750f2b45fceb122b45f8eb0d2b45f4eb082b45f0eb032b45ec8906";
/* CDirectSoundVoice_SetVolume: settings = [voice+0x10], volume at +0x1C is
 * the argument less the headroom at +0x20, client = [voice+0xC]. */
static const char k_voice_setvol_code[] =
    "8b4c24048b41108b5424082b502089501c8b490ce8........";
/* CDirectSoundStream_SetVolume at +0x28: the stream's voice is stream+4. */
static const char k_stream_setvol_code[] = "8b44240857ff74241083c00450e8........";

static int hex_digit(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* 1 when the guest code at `va` is `pattern` (hex pairs, ".." = any byte). */
static int code_matches(uint32_t va, const char *pattern)
{
    uint32_t n = (uint32_t)(strlen(pattern) / 2u), i;

    if (!va || !guest_readable(va, n))
        return 0;
    for (i = 0; i < n; i++) {
        const char *p = pattern + 2u * i;
        if (p[0] == '.')
            continue;
        if (((const volatile uint8_t *)HLE_PTR(va))[i] !=
            (uint8_t)(hex_digit(p[0]) << 4 | hex_digit(p[1])))
            return 0;
    }
    return 1;
}

static uint32_t call_target(uint32_t call_va)
{
    return call_va + 5u + HLE_MEM32(call_va + 1u);
}

static int g_layout = -1;          /* the voice layout above: 1 yes, 0 no */
static int g_stream_layout = -1;   /* and streams' voice at +4 */

static int voice_layout(void)
{
    if (g_layout < 0) {
        uint32_t commit = hle_var_CMcpxVoiceClient_Commit3dSettings_0;
        uint32_t setvol = hle_var_CMcpxVoiceClient_SetVolume, helper = 0u;
        const char *why = NULL;

        if (!commit || !setvol || !hle_var_CDirectSoundVoice_SetVolume ||
            !hle_var_CDirectSound3DCalculator_Calculate3D ||
            !hle_var_CDirectSound3DCalculator_GetVoiceData)
            why = "one of the functions is not in this title";
        else if (!code_matches(commit, k_commit_code) ||
                 call_target(commit + 0x2Bu) != hle_var_CDirectSound3DCalculator_Calculate3D ||
                 call_target(commit + 0x55u) != hle_var_CDirectSound3DCalculator_GetVoiceData)
            why = "Commit3dSettings is other code";
        else if (!code_matches(setvol, k_setvol_code) ||
                 !code_matches(helper = call_target(setvol + 0x21u), k_helper_code))
            why = "the volume helper is other code";
        else if (!code_matches(hle_var_CDirectSoundVoice_SetVolume, k_voice_setvol_code))
            why = "CDirectSoundVoice_SetVolume is other code";
        g_layout = why == NULL;
        if (why)
            fprintf(stderr, "[DSOUND] 3D voices: not the XDK 5849 voice layout (%s); "
                            "mix bins and 3D settings are not applied\n", why);
        else
            fprintf(stderr, "[DSOUND] 3D voices: XDK 5849 voice layout (Commit3dSettings "
                            "0x%08X, volume helper 0x%08X): 3D attenuation and mix-bin "
                            "volumes reach the host\n", commit, helper);
        fflush(stderr);
    }
    return g_layout;
}

static int stream_voice_layout(void)
{
    if (g_stream_layout < 0)
        g_stream_layout = voice_layout() &&
            code_matches(hle_var_CDirectSoundStream_SetVolume + 0x28u, k_stream_setvol_code);
    return g_stream_layout;
}

/* What the voice whose settings object is `settings` adds to its volume for
 * its mix bins and 3D state; 0 outside the checked layout. `is3d`, when
 * given, says whether its 3D data counted. */
static int32_t voice_adjust(uint32_t settings, int *is3d)
{
    uint8_t bins[8];
    int32_t volumes[32], data3d[9];
    uint32_t count, vd;
    int use3d = 0;

    if (is3d)
        *is3d = 0;
    if (!voice_layout() || !guest_readable(settings, 0xBCu))
        return 0;
    count = HLE_MEM32(settings + 0x24u);
    if (count == 0u || count > 8u)
        return 0;
    memcpy(bins, HLE_PTR(settings + 0x28u), sizeof bins);
    memcpy(volumes, HLE_PTR(settings + 0x30u), sizeof volumes);
    vd = HLE_MEM32(settings + 0xB8u);
    if (vd && (((const volatile uint8_t *)HLE_PTR(settings))[0x0B] & 1u) &&
        guest_readable(vd, sizeof data3d)) {
        memcpy(data3d, HLE_PTR(vd), sizeof data3d);
        use3d = 1;
    }
    if (is3d)
        *is3d = use3d;
    return recomp_dsound_mix_adjust(bins, count, volumes, use3d ? data3d : NULL);
}

/* A stream's adjustment, for hle_dsound_stream.c: its voice is stream+4,
 * whose settings object is at voice+0x10. */
int32_t hle_dsound_stream_adjust(uint32_t stream_object, int *is3d)
{
    if (is3d)
        *is3d = 0;
    if (!stream_voice_layout() || !guest_readable(stream_object + 0x14u, 4u))
        return 0;
    return voice_adjust(HLE_MEM32(stream_object + 0x14u), is3d);
}

/* The level a buffer is heard at: its volume (the settings word the XDK's
 * SetVolume keeps, headroom applied) plus its mix bins and 3D state. */
static int32_t buffer_level(const Buffer *b, int *is3d)
{
    int32_t volume = (int32_t)setting(b->iface, SET_VOLUME);
    uint32_t settings = guest_readable(b->iface - 0x0Cu, 4u) ? HLE_MEM32(b->iface - 0x0Cu) : 0u;

    return volume + (settings ? voice_adjust(settings, is3d) : 0);
}

/* Send what the output clock has consumed since the last pump: up to the
 * lead ahead of the game's clock (see lead_ms). */
static void pump(Buffer *b, uint64_t now)
{
    RecompDsoundBufferModel *out = &b->output_model;
    uint32_t channels, bits, offset = 0u, bytes, queued = 0u;
    uint64_t until = now + lead_ms();
    int adpcm, is3d;
    static uint8_t pcm[80000];

    if (!b->output || !out->playing || until <= out->last_ms ||
        until - out->last_ms < 10u)
        return;
    if (b->data != setting(b->iface, SET_DATA) ||
        b->size != setting(b->iface, SET_SIZE) ||
        b->format != setting(b->iface, SET_FORMAT)) {
        retire(b);
        return;
    }
    channels = (b->format >> 16) & 0xFFu;
    bits = b->format >> 24;
    adpcm = (b->format & 0xFFFFu) == TAG_ADPCM;
    bytes = recomp_dsound_buffer_consume(out, until, &offset);
    if (bytes == 0u || b->data == 0u || bytes > sizeof pcm)
        return;

    if (adpcm) {
        uint32_t block_bytes = XBOX_ADPCM_BLOCK_BYTES * channels;
        uint32_t decoded_bytes = XBOX_ADPCM_BLOCK_SAMPLES * channels * 2u;
        uint32_t written;
        for (written = 0u; written < bytes;) {
            int16_t decoded[XBOX_ADPCM_BLOCK_SAMPLES * 2];
            uint32_t within = offset % decoded_bytes;
            uint32_t count = decoded_bytes - within;
            if (count > bytes - written) count = bytes - written;
            if (!xbox_adpcm_decode_block(
                    (const uint8_t *)HLE_PTR(b->data + offset / decoded_bytes * block_bytes),
                    block_bytes, channels, decoded, XBOX_ADPCM_BLOCK_SAMPLES * 2u)) {
                b->output = 0;          /* bad data: keep the clock, stop the sound */
                reset_output(b);
                return;
            }
            memcpy(pcm + written, (uint8_t *)decoded + within, count);
            written += count;
            offset += count;
            if (offset == out->size_bytes) offset = out->loop_start_bytes;
        }
        bits = 16u;
    } else {
        uint32_t written;
        for (written = 0u; written < bytes;) {
            uint32_t count = out->size_bytes - offset;
            if (count > bytes - written) count = bytes - written;
            memcpy(pcm + written, HLE_PTR(b->data + offset), count);
            written += count;
            offset += count;
            if (offset == out->size_bytes) offset = out->loop_start_bytes;
        }
    }
    /* A voice that was fed and now holds nothing ran dry: an underrun, a gap
     * that makes the rest of the sound late. */
    if (b->fed && recomp_audio_output_position(slot_of(b), NULL, &queued) && queued == 0u)
        g_stat.underruns++;
    b->level = buffer_level(b, &is3d);
    if (!recomp_audio_output_submit(slot_of(b), pcm, bytes, out->sample_rate,
                                    channels, bits, b->level))
        return;
    b->fed = 1;
    g_stat.submits++;
    if (b->play_us) {
        /* The first sound of a Play: how long after the call it was sent. */
        static unsigned said, said3d;
        uint64_t us = qpc_us() - b->play_us;
        uint32_t frame = channels * (bits / 8u);
        g_stat.first_sent++;
        g_stat.first_us_sum += us;
        if (us > g_stat.first_us_max)
            g_stat.first_us_max = us;
        b->play_us = 0;
        {
            uint32_t settings = HLE_MEM32(b->iface - 0x0Cu);
            /* In full: the first 40 and then powers of two, and the first
             * 16 3D voices and 6 others with their mix bins and 3D data. */
            int detail = is3d ? said3d < 16u : said < 6u;
            if (said < 40u || (said & (said - 1u)) == 0u || detail)
                fprintf(stderr, "[DSOUND] play #%llu slot %u: first sound sent %.1f ms after "
                        "Play (%u ms of it), %u Hz, level %d (volume %d, %s %d)\n",
                        (unsigned long long)g_stat.plays, slot_of(b), (double)us / 1000.0,
                        frame && out->sample_rate ? bytes / frame * 1000u / out->sample_rate : 0u,
                        out->sample_rate, b->level, (int32_t)setting(b->iface, SET_VOLUME),
                        is3d ? "3D + mix bins" : "mix bins",
                        b->level - (int32_t)setting(b->iface, SET_VOLUME));
            if (detail && guest_readable(settings, 0xBCu)) {
                uint32_t k, n = HLE_MEM32(settings + 0x24u);
                if (is3d)
                    said3d++;
                fprintf(stderr, "[DSOUND]   flags %08X, %u mix bins:", HLE_MEM32(settings + 8u), n);
                for (k = 0; k < n && k < 8u; k++) {
                    uint32_t bin = ((const volatile uint8_t *)HLE_PTR(settings + 0x28u))[k];
                    fprintf(stderr, " %u@%d", bin, bin < 32u ?
                            (int32_t)HLE_MEM32(settings + 0x30u + 4u * bin) : 0);
                }
                if (is3d) {
                    uint32_t params = HLE_MEM32(settings + 0xB4u), vd = HLE_MEM32(settings + 0xB8u);
                    if (guest_readable(params, 0xA4u) && guest_readable(vd, 0x38u)) {
                        float dist, mn, mx;
                        memcpy(&dist, HLE_PTR(params + 0x68u), 4);
                        memcpy(&mn, HLE_PTR(params + 0x38u), 4);
                        memcpy(&mx, HLE_PTR(params + 0x3Cu), 4);
                        fprintf(stderr, "; 3D mode %u, distance %.2f (min %.2f max %.2f), "
                                "data %d %d %d %d %d %d %d %d",
                                HLE_MEM32(params + 0x40u), (double)dist, (double)mn, (double)mx,
                                (int32_t)HLE_MEM32(vd + 4u), (int32_t)HLE_MEM32(vd + 8u),
                                (int32_t)HLE_MEM32(vd + 0xCu), (int32_t)HLE_MEM32(vd + 0x10u),
                                (int32_t)HLE_MEM32(vd + 0x14u), (int32_t)HLE_MEM32(vd + 0x18u),
                                (int32_t)HLE_MEM32(vd + 0x1Cu), (int32_t)HLE_MEM32(vd + 0x20u));
                    }
                }
                fprintf(stderr, "\n");
            }
            fflush(stderr);
        }
        said++;
    }
}

static Buffer *find(uint32_t iface)
{
    uint32_t i;
    if (iface == 0u) return NULL;
    for (i = 0; i < SLOTS; i++)
        if (g_buffers[i].iface == iface)
            return &g_buffers[i];
    return NULL;
}

/* The buffer's model, created or refreshed from the settings object. NULL when
 * nothing about it can be clocked (no rate or alignment worth trusting).
 *
 * `known` is the buffer if the caller already has it, else NULL to look it up.
 * DirectSoundDoWork walks all 256 slots and used to pass only the iface, so
 * every active buffer cost another full scan to find the slot the loop was
 * already standing on -- 256 slots squared, every call, on a function the
 * title calls far more often than once a frame. It was 22.7% of Dino Crisis
 * 3's on-CPU time as a leaf, which is the shape of a scan rather than of the
 * work it was meant to be doing. */
static Buffer *model_for_known(Buffer *known, uint32_t iface, uint64_t now)
{
    Buffer *b = known ? known : find(iface);
    uint32_t size, data, format, tag, channels, bits, align, rate, loop_start;
    uint32_t decoded_size, decoded_loop, block_align;
    int output;
    uint32_t i;

    if (iface == 0u || !guest_readable(iface - 0x0Cu, 0x10u) ||
        HLE_MEM32(iface) == 0u || HLE_MEM32(iface - 0x0Cu) == 0u)
        return NULL;
    size       = setting(iface, SET_SIZE);
    data       = setting(iface, SET_DATA);
    format     = setting(iface, SET_FORMAT);
    align      = setting(iface, SET_ALIGN);
    rate       = setting(iface, SET_RATE);
    loop_start = setting(iface, SET_LOOP_START);
    tag        = format & 0xFFFFu;
    channels   = (format >> 16) & 0xFFu;
    bits       = format >> 24;

    if (b && (b->data != data || b->size != size || b->format != format)) {
        retire(b);
        b = NULL;
    }
    if (align == 0u || rate < 1000u || rate > 200000u)
        return NULL;
    /* Clock whole blocks only. Burnout 2's attract-movie stream is a stereo
     * ADPCM ring of 7,876,608 bytes -- not a multiple of its 72-byte block --
     * and requiring an exact multiple left the one buffer the game waits on
     * with no model and no sound. The tail is less than one block. b->size
     * below keeps the raw size, so a real change is still noticed. */
    {
        uint32_t raw_size = size;
        size -= size % align;
        if (size == 0u)
            return NULL;
        if (b) {
            b->size = raw_size;
        }
        if (loop_start >= size)
            loop_start = 0u;
        (void)raw_size;
    }
    if (loop_start >= size || loop_start % align != 0u)
        loop_start = 0u;

    output = channels >= 1u && channels <= 2u &&
        ((tag == TAG_PCM && (bits == 8u || bits == 16u) &&
          align == channels * bits / 8u) ||
         (tag == TAG_ADPCM && bits == 4u &&
          align == XBOX_ADPCM_BLOCK_BYTES * channels));

    /* ADPCM is clocked in decoded PCM bytes, as the output consumes it. */
    if (tag == TAG_ADPCM && output) {
        uint64_t d = (uint64_t)(size / align) * XBOX_ADPCM_BLOCK_SAMPLES * channels * 2u;
        if (d > UINT32_MAX) return NULL;
        decoded_size = (uint32_t)d;
        decoded_loop = loop_start / align * XBOX_ADPCM_BLOCK_SAMPLES * channels * 2u;
        block_align = channels * 2u;
    } else {
        decoded_size = size;
        decoded_loop = loop_start;
        block_align = align;
    }

    if (b) {
        if (b->model.loop_start_bytes != decoded_loop) {
            pump(b, now);
            recomp_dsound_buffer_cursor(&b->model, now);
            reset_output(b);
            b->model.loop_start_bytes = decoded_loop;
            b->output_model = b->model;
        }
        return b;
    }
    for (i = 0; i < SLOTS; i++) {
        if (g_buffers[i].iface == 0u) {
            b = &g_buffers[i];
            if (recomp_dsound_buffer_configure(&b->model, decoded_size, rate,
                                               block_align, now) != 0u)
                return NULL;
            b->iface = iface;
            b->data = data;
            b->size = size;
            b->format = format;
            b->output = output;
            b->model.loop_start_bytes = decoded_loop;
            b->output_model = b->model;
            return b;
        }
    }
    {
        static int said;
        if (!said++) {
            fprintf(stderr, "[DSOUND] more than %u buffers in play; "
                            "the rest are not modelled\n", SLOTS);
            fflush(stderr);
        }
    }
    return NULL;
}

static Buffer *model_for(uint32_t iface, uint64_t now)
{
    return model_for_known(NULL, iface, now);
}

/* After a control change, restart the output from the model unless this is a
 * Play that simply continues a sound already playing -- and feed the host at
 * once, so a sound starts within the host's own latency rather than at the
 * next pump. */
static void resync_output(Buffer *b, int continuing, uint64_t now)
{
    if (!continuing)
        reset_output(b);
    if (continuing)
        b->output_model.play_flags = b->model.play_flags;
    else
        b->output_model = b->model;
    if (b->model.playing && lead_ms())
        pump(b, now);
}

/* HRESULT IDirectSoundBuffer_Play(this, reserved1, reserved2, flags) */
/* How often the title calls each of these.
 *
 * DirectSoundDoWork alone turned out to be 68.8 million calls in 85 seconds
 * of Dino Crisis 3 -- some 62,000 per presented frame against a console
 * budget of one -- and a count on its own does not say what the title is
 * waiting for. The ratio does: a poll loop shows up as one other entry point
 * keeping pace with DoWork, and everything else near zero.
 *
 * Reported on powers of ten of the DoWork count, so this costs a handful of
 * lines across a whole run. */
extern uint32_t g_xbox_code_lo;
extern uint32_t g_xbox_code_hi;

enum {
    DS_CALL_BUF_PLAY,
    DS_CALL_BUF_STOP,
    DS_CALL_BUF_STOPEX,
    DS_CALL_BUF_PAUSE,
    DS_CALL_BUF_GETSTATUS,
    DS_CALL_BUF_GETCURRENTPOSITION,
    DS_CALL_BUF_SETCURRENTPOSITION,
    DS_CALL_BUF_SETFREQUENCY,
    DS_CALL_BUF_SETFORMAT,
    DS_CALL_BUF_SETBUFFERDATA,
    DS_CALL_DIRECTSOUNDDOWORK,
    DS_CALL_COUNT
};
static uint64_t g_ds_calls[DS_CALL_COUNT];
static const char *const g_ds_call_names[DS_CALL_COUNT] = {
    "IDirectSoundBuffer_Play",
    "IDirectSoundBuffer_Stop",
    "IDirectSoundBuffer_StopEx",
    "IDirectSoundBuffer_Pause/PauseEx",
    "IDirectSoundBuffer_GetStatus",
    "IDirectSoundBuffer_GetCurrentPosition",
    "IDirectSoundBuffer_SetCurrentPosition",
    "IDirectSoundBuffer_SetFrequency",
    "IDirectSoundBuffer_SetFormat",
    "IDirectSoundBuffer_SetBufferData",
    "DirectSoundDoWork",
};

/* `caller` is the guest return address, which is the thing that actually
 * identifies the loop. DirectSoundDoWork at 810,000 calls a second is not
 * paired with any other entry point here -- GetCurrentPosition, the next
 * busiest, runs at an eighth of it -- so the counts alone say only that
 * something outside this file is calling it, and not what. */
static void ds_count_report(uint32_t caller)
{
    int i;
    fprintf(stderr, "[DSOUND] calls so far (DoWork from 0x%08X):", caller);
    for (i = 0; i < DS_CALL_COUNT; i++)
        if (g_ds_calls[i])
            fprintf(stderr, " %s=%llu", g_ds_call_names[i],
                    (unsigned long long)g_ds_calls[i]);
    fprintf(stderr, "\n");
    /* And the chain above it. The immediate caller is a game tick that
     * happens to include this call; what matters is the loop spinning on
     * that tick 58,000 times per presented frame. Same heuristic the ICALL
     * logger uses: code-range values above esp, first one most reliable. */
    {
        const uint8_t *stk = (const uint8_t *)g_xbox_mem_offset + g_esp;
        int k, shown = 0;
        fprintf(stderr, "[DSOUND]   guest stack:");
        for (k = 0; g_esp && k < 200 && shown < 8; k++) {
            uint32_t v;
            memcpy(&v, stk + (size_t)k * 4, sizeof v);
            if (v >= g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "%s 0x%08X", shown ? " <-" : "", v);
                shown++;
            }
        }
        if (!shown)
            fprintf(stderr, " (none)");
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

HLE_EXPORT(IDirectSoundBuffer_Play)
{
    g_ds_calls[DS_CALL_BUF_PLAY]++;
    uint32_t iface = HLE_ARG(0), flags = HLE_ARG(3), result;
    uint64_t now = now_ms();
    static unsigned said;
    Buffer *b;
    int continuing;

    lock();
    b = model_for(iface, now);
    if (said < 16) {
        said++;
        fprintf(stderr, "[DSOUND] Play buffer=%08X format=%08X bytes=%u rate=%u "
                        "flags=%u %s\n", iface,
                iface && HLE_MEM32(iface) ? setting(iface, SET_FORMAT) : 0u,
                iface && HLE_MEM32(iface) ? setting(iface, SET_SIZE) : 0u,
                iface && HLE_MEM32(iface) ? setting(iface, SET_RATE) : 0u, flags,
                !b ? "not modelled" : b->output ? "output" : "clock only");
        /* RECOMP_DSOUND_DUMP=1 -- the settings object, as words.
         *
         * The offsets above are one XDK's layout. When a title reports a rate
         * of 29433088 and a format of 0x01C10D60 -- both of which are guest
         * pointers, not a rate and a packed format -- the table is being read
         * against a different layout, and the only way to say which is to look
         * at what is actually there. Tony Hawk's Pro Skater 2X (XDK 3947) is
         * the first title here to disagree with it. */
        if (!b && xbox_EnvSwitch("RECOMP_DSOUND_DUMP", 0)) {
            uint32_t object = (iface && guest_readable(iface, 4u))
                            ? HLE_MEM32(iface) : 0u;
            uint32_t voice  = (iface >= 0x0Cu && guest_readable(iface - 0x0Cu, 4u))
                            ? HLE_MEM32(iface - 0x0Cu) : 0u;
            unsigned w;
            fprintf(stderr, "[DSOUND]   object=0x%08X voice=0x%08X\n",
                    object, voice);
            for (w = 0; w < 0x34u; w += 4u) {
                if (object && guest_readable(object + w, 4u))
                    fprintf(stderr, "[DSOUND]     object+%02X = %08X\n",
                            w, HLE_MEM32(object + w));
            }
            for (w = 0; w < 0x20u; w += 4u) {
                if (voice && guest_readable(voice + w, 4u))
                    fprintf(stderr, "[DSOUND]     voice +%02X = %08X\n",
                            w, HLE_MEM32(voice + w));
            }
            /* Follow whatever sits where the format and rate are expected.
             * If this build keeps a WAVEFORMATEX pointer there, its first
             * words read as tag | channels << 16, then samples per second. */
            for (w = 0x0Cu; w <= 0x10u; w += 4u) {
                uint32_t p = (voice && guest_readable(voice + w, 4u))
                           ? HLE_MEM32(voice + w) : 0u;
                unsigned k;
                if (!p || !guest_readable(p, 0x18u))
                    continue;
                fprintf(stderr, "[DSOUND]     *(voice+%02X)=0x%08X:", w, p);
                for (k = 0; k < 0x18u; k += 4u)
                    fprintf(stderr, " %08X", HLE_MEM32(p + k));
                fprintf(stderr, "\n");
            }
        }
        fflush(stderr);
    }
    if (!b) {
        unlock();
        HLE_RETURN(iface ? RECOMP_DSOUND_OK : RECOMP_DSOUND_POINTER_ERROR);
    }
    pump(b, now);
    recomp_dsound_buffer_cursor(&b->model, now);
    continuing = b->model.playing && b->output_model.playing &&
                 !(flags & RECOMP_DSOUND_PLAY_FROMSTART);
    result = recomp_dsound_buffer_play(&b->model, flags, now);
    if (result == RECOMP_DSOUND_OK) {
        b->paused = 0;
        if (!continuing && b->output) {
            g_stat.plays++;
            b->play_us = qpc_us();   /* the first submission reports the delay */
        }
        resync_output(b, continuing, now);
    }
    unlock();
    HLE_RETURN(result);
}

static void stop(uint32_t iface)
{
    uint64_t now = now_ms();
    Buffer *b;

    lock();
    b = find(iface);
    if (b) {
        pump(b, now);
        recomp_dsound_buffer_stop(&b->model, now);
        b->paused = 0;
        resync_output(b, 0, now);
    }
    unlock();
}

/* HRESULT IDirectSoundBuffer_Stop(this) */
HLE_EXPORT(IDirectSoundBuffer_Stop)
{
    g_ds_calls[DS_CALL_BUF_STOP]++;
    stop(HLE_ARG(0));
    HLE_RETURN(HLE_ARG(0) ? RECOMP_DSOUND_OK : RECOMP_DSOUND_POINTER_ERROR);
}

/* HRESULT IDirectSoundBuffer_StopEx(this, REFERENCE_TIME rtTimeStamp, flags).
 * The time stamp and envelope flags ask for a scheduled or released stop; an
 * immediate one is what the game can observe, and all it waits for. */
HLE_EXPORT(IDirectSoundBuffer_StopEx)
{
    g_ds_calls[DS_CALL_BUF_STOPEX]++;
    stop(HLE_ARG(0));
    HLE_RETURN(HLE_ARG(0) ? RECOMP_DSOUND_OK : RECOMP_DSOUND_POINTER_ERROR);
}

/* DSBPAUSE_RESUME, DSBPAUSE_PAUSE, DSBPAUSE_SYNCHPLAYBACK. */
enum { DSBPAUSE_RESUME = 0u, DSBPAUSE_PAUSE = 1u, DSBPAUSE_SYNCHPLAYBACK = 2u };

/* Pause keeps the cursor and stops the clock; resume continues from it.
 *
 * Dino Crisis 3 (XDK 5558) fades its music out by setting the volume to
 * -64 dB, calling Pause(DSBPAUSE_PAUSE), then spinning on DirectSoundDoWork
 * until GetStatus stops saying PLAYING, and only then calling Stop. Pause was
 * not replaced, so the model never heard of it, GetStatus said PLAYING for
 * ever, and the title hung for good straight after its difficulty select.
 *
 * SYNCHPLAYBACK pauses too: it holds the buffer so that several can be
 * resumed together, which from the title's side looks the same. */
static uint32_t pause_buffer(uint32_t iface, uint32_t how)
{
    uint64_t now = now_ms();
    Buffer *b;

    if (iface == 0u)
        return RECOMP_DSOUND_POINTER_ERROR;
    if (how > DSBPAUSE_SYNCHPLAYBACK)
        return RECOMP_DSOUND_INVALID_PARAM;
    lock();
    b = find(iface);
    if (b) {
        pump(b, now);
        recomp_dsound_buffer_cursor(&b->model, now);
        if (how == DSBPAUSE_RESUME) {
            if (b->paused) {
                b->paused = 0;
                if (recomp_dsound_buffer_play(&b->model, b->paused_flags, now)
                        == RECOMP_DSOUND_OK)
                    resync_output(b, 0, now);
            }
        } else if (b->model.playing) {
            b->paused = 1;
            b->paused_flags = b->model.play_flags & RECOMP_DSOUND_PLAY_LOOPING;
            recomp_dsound_buffer_stop(&b->model, now);
            resync_output(b, 0, now);
        }
    }
    unlock();
    return RECOMP_DSOUND_OK;
}

/* HRESULT IDirectSoundBuffer_Pause(this, DWORD dwPause) */
HLE_EXPORT(IDirectSoundBuffer_Pause)
{
    g_ds_calls[DS_CALL_BUF_PAUSE]++;
    HLE_RETURN(pause_buffer(HLE_ARG(0), HLE_ARG(1)));
}

/* HRESULT IDirectSoundBuffer_PauseEx(this, REFERENCE_TIME rtTimestamp,
 * DWORD dwPause). A scheduled pause is taken now, as StopEx's stop is. */
HLE_EXPORT(IDirectSoundBuffer_PauseEx)
{
    g_ds_calls[DS_CALL_BUF_PAUSE]++;
    HLE_RETURN(pause_buffer(HLE_ARG(0), HLE_ARG(3)));
}

/* HRESULT IDirectSoundBuffer_GetStatus(this, DWORD *status)
 * DSBSTATUS_PLAYING 1, DSBSTATUS_PAUSED 2, DSBSTATUS_LOOPING 4. */
HLE_EXPORT(IDirectSoundBuffer_GetStatus)
{
    g_ds_calls[DS_CALL_BUF_GETSTATUS]++;
    uint32_t iface = HLE_ARG(0), out = HLE_ARG(1), status = 0u;
    uint64_t now = now_ms();
    Buffer *b;

    if (out == 0u)
        HLE_RETURN(RECOMP_DSOUND_POINTER_ERROR);
    lock();
    b = find(iface);
    if (b) {
        pump(b, now);
        recomp_dsound_buffer_cursor(&b->model, now);
        if (b->model.playing)
            status = 1u | ((b->model.play_flags & RECOMP_DSOUND_PLAY_LOOPING) ? 4u : 0u);
        else if (b->paused)
            status = 2u | ((b->paused_flags & RECOMP_DSOUND_PLAY_LOOPING) ? 4u : 0u);
    }
    unlock();
    HLE_MEM32(out) = status;          /* never played here: not playing */
    HLE_RETURN(RECOMP_DSOUND_OK);
}

/* HRESULT IDirectSoundBuffer_GetCurrentPosition(this, DWORD *play, DWORD *write) */
HLE_EXPORT(IDirectSoundBuffer_GetCurrentPosition)
{
    g_ds_calls[DS_CALL_BUF_GETCURRENTPOSITION]++;
    uint32_t iface = HLE_ARG(0), cursor = 0u, i;
    uint64_t now = now_ms();
    Buffer *b;

    lock();
    b = model_for(iface, now);
    if (b) {
        pump(b, now);
        cursor = recomp_dsound_buffer_cursor(&b->model, now);
        /* ADPCM is clocked in decoded bytes; the game asks in source bytes. */
        if ((b->format & 0xFFFFu) == TAG_ADPCM && b->output)
            cursor = cursor / (XBOX_ADPCM_BLOCK_SAMPLES * b->model.block_align) *
                     (XBOX_ADPCM_BLOCK_BYTES * b->model.block_align / 2u);
    }
    unlock();
    for (i = 1u; i <= 2u; i++) {
        uint32_t out = HLE_ARG(i);
        if (out != 0u)
            HLE_MEM32(out) = cursor;
    }
    HLE_RETURN(RECOMP_DSOUND_OK);
}

/* HRESULT IDirectSoundBuffer_SetCurrentPosition(this, DWORD position) */
HLE_EXPORT(IDirectSoundBuffer_SetCurrentPosition)
{
    g_ds_calls[DS_CALL_BUF_SETCURRENTPOSITION]++;
    uint32_t iface = HLE_ARG(0), position = HLE_ARG(1), result = RECOMP_DSOUND_OK;
    uint64_t now = now_ms();
    Buffer *b;

    lock();
    b = model_for(iface, now);
    if (b) {
        pump(b, now);
        if ((b->format & 0xFFFFu) == TAG_ADPCM && b->output) {
            uint32_t block = XBOX_ADPCM_BLOCK_BYTES * b->model.block_align / 2u;
            if (position >= b->size || position % block != 0u)
                result = RECOMP_DSOUND_INVALID_PARAM;
            else
                position = position / block * XBOX_ADPCM_BLOCK_SAMPLES *
                           b->model.block_align;
        }
        if (result == RECOMP_DSOUND_OK)
            result = recomp_dsound_buffer_set_position(&b->model, position, now);
        if (result == RECOMP_DSOUND_OK)
            resync_output(b, 0, now);
    }
    unlock();
    HLE_RETURN(result);
}

/* HRESULT IDirectSoundBuffer_SetFrequency(this, DWORD frequency); 0 = default */
HLE_EXPORT(IDirectSoundBuffer_SetFrequency)
{
    g_ds_calls[DS_CALL_BUF_SETFREQUENCY]++;
    uint32_t iface = HLE_ARG(0), frequency = HLE_ARG(1), result = RECOMP_DSOUND_OK;
    uint64_t now = now_ms();
    Buffer *b;

    lock();
    b = model_for(iface, now);
    if (b) {
        /* The rate it already plays at changes nothing. Future Perfect sets
         * the frequency of its sounds over and over (sub_000274A0, 45,324
         * calls in the user's 52-minute session against 5,111 Plays), and
         * each call used to drop the host voice and start it again from the
         * model, with an empty queue: a gap and a late restart every time. */
        uint32_t want = frequency ? frequency : b->model.original_sample_rate;
        int same = want == b->model.sample_rate;
        g_stat.freq_calls++;
        if (same)
            g_stat.freq_same++;
        if (!same || !lead_ms()) {     /* lead 0: the old behaviour, for comparison */
            pump(b, now);
            result = recomp_dsound_buffer_set_frequency(&b->model, frequency, now);
            if (result == RECOMP_DSOUND_OK) {
                if (!same) {
                    g_stat.freq_changed++;
                    if (b->model.playing)
                        g_stat.freq_changed_playing++;
                }
                resync_output(b, 0, now);
            }
        }
    }
    unlock();
    HLE_RETURN(result);
}

/* Whether the buffer's settings are in the layout this file reads: a rate
 * and an alignment worth trusting, the test model_for applies before it
 * models a buffer. Not every XDK keeps them there. Halo's DSOUND (XDK 3936)
 * has a guest pointer where the rate should be (its Play log line says
 * "rate=18895392 ... not modelled"), so none of its buffers is modelled --
 * and the two replacements below must not write this layout into its
 * objects either. They hand such a buffer to the game's own code, which is
 * what ran before they existed and what Halo needs: with the replacement,
 * its SetBufferData wrote 0xB8..0xCC of the wrong object and its Bink intro
 * movie waited on its audio for ever, 22 s in. */
static int settings_in_layout(uint32_t iface)
{
    uint32_t rate = setting(iface, SET_RATE), align = setting(iface, SET_ALIGN);

    return align != 0u && rate >= 1000u && rate <= 200000u;
}

/* The game's own body, for a buffer settings_in_layout() rejects. */
static void original_body(void (*const body)(void), const char *name)
{
    static unsigned said;

    if (!body) {
        fprintf(stderr, "[DSOUND] %s: the buffer is not in the layout this file "
                        "reads and the original body was not kept -- lift again\n", name);
        fflush(stderr);
        g_eax = 0x80004005u;            /* E_FAIL */
        return;
    }
    if (said < 4) {
        said++;
        fprintf(stderr, "[DSOUND] %s: buffer settings not in the layout this file "
                        "reads; the game's own code runs\n", name);
        fflush(stderr);
    }
    {
        uint32_t esp = g_esp;

        body();
        g_esp = esp;
    }
}

/* HRESULT IDirectSoundBuffer_SetFormat(this, LPCWAVEFORMATEX format)
 *
 * The game's own SetFormat ends in CMcpxBuffer_SetBufferData, which first
 * waits for the buffer's hardware voice to be released by the chip -- a voice
 * the replaced Play never put on the chip, so the release never comes.
 * Outrun 2 calls it at the start of every race and spun there for ever, its
 * 0.5 s retry re-arming the same command each time.
 *
 * So the format goes where the game's packer would put it, the voice
 * settings this file reads (tag | channels << 16 | bits << 24, rate, block
 * alignment), and the model notices the change and rebuilds -- for a buffer
 * in that layout (settings_in_layout). */
HLE_ORIGINAL(IDirectSoundBuffer_SetFormat);
HLE_EXPORT(IDirectSoundBuffer_SetFormat)
{
    g_ds_calls[DS_CALL_BUF_SETFORMAT]++;
    uint32_t iface = HLE_ARG(0), wfx = HLE_ARG(1), result = RECOMP_DSOUND_OK;
    uint64_t now = now_ms();
    uint32_t holder = iface - 0x0Cu, object;
    static unsigned said;

    if (!settings_in_layout(iface)) {
        original_body(hle_original_IDirectSoundBuffer_SetFormat,
                      "IDirectSoundBuffer_SetFormat");
        return;
    }
    if (!wfx || !guest_readable(wfx, 16u) || !guest_readable(holder, 4u)
            || !guest_readable(HLE_MEM32(holder) + SET_ALIGN, 4u))
        HLE_RETURN(RECOMP_DSOUND_INVALID_PARAM);
    object = HLE_MEM32(holder);
    {
        uint32_t tag      = HLE_MEM32(wfx) & 0xFFFFu;
        uint32_t channels = HLE_MEM32(wfx) >> 16;
        uint32_t rate     = HLE_MEM32(wfx + 4u);
        uint32_t align    = HLE_MEM32(wfx + 12u) & 0xFFFFu;
        uint32_t bits     = HLE_MEM32(wfx + 12u) >> 16;

        if (said < 8) {
            said++;
            fprintf(stderr, "[DSOUND] SetFormat buffer=%08X tag %04X, %u ch, %u Hz, "
                            "%u bit, align %u\n", iface, tag, channels, rate,
                    bits, align);
        }
        lock();
        HLE_MEM32(object + SET_FORMAT) = tag | (channels & 0xFFu) << 16 | (bits & 0xFFu) << 24;
        HLE_MEM32(object + SET_RATE) = rate;
        HLE_MEM32(object + SET_ALIGN) = align;
        if (model_for(iface, now))
            ;   /* rebuilt from the new settings */
        unlock();
    }
    HLE_RETURN(result);
}

/* HRESULT IDirectSoundBuffer_SetBufferData(this, LPVOID data, DWORD bytes)
 *
 * The game's own version stops the buffer through CMcpxBuffer_Stop_Ex, which
 * waits for the chip to release a hardware voice the replaced Play never
 * used -- the same wait as SetFormat above. Outrun 2 swaps a buffer's data
 * mid-race and lost about 40 seconds to that wait each time.
 *
 * So: stop the modelled buffer, as the replaced Stop does, and write what the
 * game's settings code would -- the data and size, and play and loop regions
 * back to the whole buffer (zero). The model rebuilds from them. Only for a
 * buffer in that layout (settings_in_layout). */
HLE_ORIGINAL(IDirectSoundBuffer_SetBufferData);
HLE_EXPORT(IDirectSoundBuffer_SetBufferData)
{
    g_ds_calls[DS_CALL_BUF_SETBUFFERDATA]++;
    uint32_t iface = HLE_ARG(0), data = HLE_ARG(1), bytes = HLE_ARG(2);
    uint64_t now = now_ms();
    uint32_t object;
    static unsigned said;

    if (!settings_in_layout(iface)) {
        original_body(hle_original_IDirectSoundBuffer_SetBufferData,
                      "IDirectSoundBuffer_SetBufferData");
        return;
    }
    if (!guest_readable(iface, 4u) || !guest_readable(HLE_MEM32(iface) + SET_LOOP_LEN, 4u))
        HLE_RETURN(RECOMP_DSOUND_INVALID_PARAM);
    if (said < 8) {
        said++;
        fprintf(stderr, "[DSOUND] SetBufferData buffer=%08X data 0x%08X, %u bytes\n",
                iface, data, bytes);
    }
    stop(iface);
    lock();
    object = HLE_MEM32(iface);
    HLE_MEM32(object + SET_DATA) = data;
    HLE_MEM32(object + SET_SIZE) = bytes;
    HLE_MEM32(object + SET_PLAY_START) = 0u;
    HLE_MEM32(object + SET_PLAY_LEN) = 0u;
    HLE_MEM32(object + SET_LOOP_START) = 0u;
    HLE_MEM32(object + SET_LOOP_LEN) = 0u;
    (void)model_for(iface, now);
    unlock();
    HLE_RETURN(RECOMP_DSOUND_OK);
}

/* Every five seconds while buffers play: what the output did, so latency and
 * levels can be read from a (muted) run's log rather than by ear. */
static void report(uint64_t now)
{
    static uint64_t last;
    RecompAudioEngineStats es;
    uint32_t i, playing = 0, shown = 0;
    char voices3d[512];
    size_t used = 0;

    if (now - last < 5000u)
        return;
    last = now;
    if (!g_stat.plays && !g_stat.submits)
        return;
    voices3d[0] = '\0';
    for (i = 0; i < SLOTS; i++) {
        Buffer *b = &g_buffers[i];
        int is3d = 0;
        int32_t level;
        uint32_t settings, params;
        float dist = 0.0f;

        if (!b->iface || !b->model.playing)
            continue;
        playing++;
        level = buffer_level(b, &is3d);
        if (!is3d || shown >= 6u || used >= sizeof voices3d - 80u)
            continue;
        settings = HLE_MEM32(b->iface - 0x0Cu);
        params = guest_readable(settings + 0xB4u, 4u) ? HLE_MEM32(settings + 0xB4u) : 0u;
        if (params && guest_readable(params + 0x68u, 4u))
            memcpy(&dist, HLE_PTR(params + 0x68u), 4);
        used += (size_t)snprintf(voices3d + used, sizeof voices3d - used,
                                 " slot %u level %d (volume %d) at %.1f;", i, level,
                                 (int32_t)setting(b->iface, SET_VOLUME), (double)dist);
        shown++;
    }
    recomp_audio_output_engine_stats(&es);
    fprintf(stderr, "[DSOUND] buffers: %llu plays, first sound sent %.1f ms after Play on "
            "average (worst %.1f); %llu submissions, %llu underruns; SetFrequency %llu "
            "(%llu at the same rate, %llu changes, %llu of them while playing); %llu voice "
            "resets; 3D commits %llu (XDK's own %llu); %u playing\n",
            (unsigned long long)g_stat.plays,
            g_stat.first_sent ? (double)g_stat.first_us_sum / 1000.0 / (double)g_stat.first_sent : 0.0,
            (double)g_stat.first_us_max / 1000.0,
            (unsigned long long)g_stat.submits, (unsigned long long)g_stat.underruns,
            (unsigned long long)g_stat.freq_calls, (unsigned long long)g_stat.freq_same,
            (unsigned long long)g_stat.freq_changed,
            (unsigned long long)g_stat.freq_changed_playing,
            (unsigned long long)g_stat.resets, (unsigned long long)g_stat.commits_3d,
            (unsigned long long)g_stat.commits_original, playing);
    fprintf(stderr, "[DSOUND] host: latency %.1f ms, %u glitches, %u of %u voices active; "
            "voices created %llu (%.2f ms each, worst %.2f), destroyed %llu (%.2f ms each, "
            "worst %.2f)\n",
            es.sample_rate ? (double)es.latency_samples * 1000.0 / es.sample_rate : 0.0,
            es.glitches, es.active_voices, es.total_voices,
            (unsigned long long)es.voices_created,
            es.voices_created ? es.create_ms_total / (double)es.voices_created : 0.0,
            es.create_ms_max, (unsigned long long)es.voices_destroyed,
            es.voices_destroyed ? es.destroy_ms_total / (double)es.voices_destroyed : 0.0,
            es.destroy_ms_max);
    if (shown)
        fprintf(stderr, "[DSOUND] 3D buffers playing:%s\n", voices3d);
    fflush(stderr);
}

/* void DirectSoundDoWork(void) -- the title's regular audio tick. The game's
 * own version services the emulated chip's deferred commands, which the
 * replaced buffers no longer need; here it feeds the host output. */
HLE_EXPORT(DirectSoundDoWork)
{
    g_ds_calls[DS_CALL_DIRECTSOUNDDOWORK]++;
    uint64_t now = now_ms();
    uint32_t i;
    static int said;

    if (!said) {
        said = 1;
        fprintf(stderr, "[DSOUND] DirectSoundDoWork replaced by name: buffers "
                        "run on the host clock and play through XAudio2\n");
        fflush(stderr);
    }
    /* How often the title asks. On the console this is a once-a-frame call;
     * anything wildly above the frame rate means the work below is being paid
     * for at a rate nobody chose, and that is worth seeing in the log rather
     * than inferring from a profile. */
    {
        /* Powers of ten, like the ICALL loggers, because a fixed interval
         * is a bug here: at 100,000 this printed 688 lines in 85 seconds
         * and NtWriteFile became 100% of the sampling profile -- the
         * counter measuring the problem became the problem. The
         * progression is the useful part anyway. The title calls this
         * 68.8 million times in 85 seconds, about 62,000 per presented
         * frame; on the console it is a once-a-frame call. */
        static uint64_t calls, next = 1;
        if (++calls >= next) {
            next *= 10;
            ds_count_report(HLE_MEM32(g_esp));
        }
    }
    lock();
    for (i = 0; i < SLOTS; i++) {
        if (g_buffers[i].iface != 0u) {
            Buffer *b = model_for_known(&g_buffers[i],
                                        g_buffers[i].iface, now);
            if (b)
                pump(b, now);
        }
    }
    report(now);
    unlock();
    /* Streams: feed the host, complete packets -- on their own clock (the
     * tick count every other stream entry point reads), not this file's. */
    hle_dsound_stream_tick(GetTickCount64());
    HLE_RETURN(0);
}

/* ── The chip side of a voice's settings ──────────────────────────
 *
 * CDirectSoundVoice_SetVolume, SetHeadroom, SetPitch, SetMixBins and
 * SetMixBinVolumes each write the voice settings object (the volume this
 * file reads at every submission, see SET_VOLUME) and then call one of
 * these three to program the MCPX voice: descriptor writes and MMIO register
 * writes, each register write an exception trap here, each trap waking the
 * APU thread. The chip voice is never started -- IDirectSoundBuffer_Play is
 * replaced above and keeps the sound in the host model -- so the programming
 * was pure cost. Outrun 2 sets volume, headroom, pitch and mix bins on every
 * voice every frame: CMcpxVoiceClient_SetVolume and SetMixBins were 15% of
 * the guest thread's time in a race, the exception dispatcher under them 6%
 * and the APU wake-ups 2% (1 Oct 2026, caller-attributed sampling).
 *
 * Replaced here, below the setters, so every setter still keeps its own
 * settings exactly as the XDK does and only the chip part is skipped. All
 * three are thiscall with no stack arguments and return S_OK.
 *
 * Pitch is therefore recorded by the XDK but not yet heard: nothing in this
 * file applies it, and the XAudio2 backend recreates a source voice when its
 * rate changes, which a per-frame engine pitch would do every frame. Hearing
 * it needs a frequency-ratio path in the backend first. It was not heard
 * before this change either.
 *
 * Volume, headroom and mix-bin volumes are heard: the setters keep them in
 * the settings object, which the level (buffer_level, voice_adjust) reads
 * at every submission. A 3D voice's attenuation is not a setter's: the XDK
 * computes it into the voice's 3D data block in Commit3dSettings, below. */
HLE_EXPORT(CMcpxVoiceClient_SetVolume)  { HLE_RETURN(0); }
HLE_EXPORT(CMcpxVoiceClient_SetMixBins) { HLE_RETURN(0); }
HLE_EXPORT(CMcpxVoiceClient_SetPitch)   { HLE_RETURN(0); }

/* kernel_bridge.c: the dispatch table, to call the XDK's own 3D code. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);

/* Call a lifted stdcall function with `n` arguments; it pops them. */
static int call_stdcall(uint32_t va, uint32_t n, const uint32_t *args)
{
    recomp_func_t fn = va ? recomp_lookup(va) : NULL;
    uint32_t i;

    if (!fn)
        return 0;
    for (i = n; i > 0; i--) {
        g_esp -= 4u;
        HLE_MEM32(g_esp) = args[i - 1u];
    }
    g_esp -= 4u;
    HLE_MEM32(g_esp) = 0u;               /* return address, popped by its ret */
    fn();
    return 1;
}

/* void CMcpxVoiceClient::Commit3dSettings(void), thiscall.
 *
 * The XDK's own body computes a 3D voice's data block only while the voice
 * runs on the chip ((client+0x12 & 3) == 3: a chip voice allocated and
 * started) -- never here -- and then programs the chip from it. For a 3D
 * voice off the chip this does the computing half with the XDK's own
 * calculator, exactly as that body calls it, and clears the change flags as
 * the body and its chip half do; the chip half itself (volume and pitch
 * reach the stubs above; the HRTF filter registers have no use here) is
 * skipped. Called through CDirectSoundVoice_CommitDeferredSettings for each
 * 3D voice -- the DirectSound object lists them from creation -- by the
 * listener's CommitDeferredSettings (Future Perfect: once a frame) and by an
 * immediate 3D setter, while the listener's change flags are still set. A
 * voice on the chip, and any title whose code is not the checked layout,
 * gets the XDK's own body. */
/* The first commits, in full: what the calculator made of the voice. */
static void commit_note(uint32_t client, uint32_t settings, uint32_t params,
                        uint32_t vdata)
{
    static unsigned said;
    float dist, mn, mx;

    if (said >= 12u)
        return;
    said++;
    memcpy(&dist, HLE_PTR(params + 0x68u), 4);
    memcpy(&mn, HLE_PTR(params + 0x38u), 4);
    memcpy(&mx, HLE_PTR(params + 0x3Cu), 4);
    fprintf(stderr, "[DSOUND] 3D commit #%u: client %08X flags %04X, mode %u, distance "
            "%.2f (min %.2f, max %.2f) -> distance %d cone %d front %d back %d "
            "I3DL2 %d/%d, mix-bin adjust %d\n", said, client,
            ((const volatile uint16_t *)HLE_PTR(client + 0x12u))[0],
            HLE_MEM32(params + 0x40u), (double)dist, (double)mn, (double)mx,
            (int32_t)HLE_MEM32(vdata + 4u), (int32_t)HLE_MEM32(vdata + 8u),
            (int32_t)HLE_MEM32(vdata + 0xCu), (int32_t)HLE_MEM32(vdata + 0x10u),
            (int32_t)HLE_MEM32(vdata + 0x1Cu), (int32_t)HLE_MEM32(vdata + 0x20u),
            voice_adjust(settings, NULL));
    fflush(stderr);
}

HLE_ORIGINAL(CMcpxVoiceClient_Commit3dSettings_0);
HLE_EXPORT(CMcpxVoiceClient_Commit3dSettings_0)
{
    uint32_t client = g_ecx, settings, params, vdata, owner, ds;

    if (!voice_layout() || !guest_readable(client, 0x74u) ||
        (((const volatile uint8_t *)HLE_PTR(client))[0x12] & 3u) == 3u) {
        g_stat.commits_original++;
        if (hle_original_CMcpxVoiceClient_Commit3dSettings_0)
            HLE_CALL_ORIGINAL(CMcpxVoiceClient_Commit3dSettings_0);
        return;
    }
    settings = HLE_MEM32(client + 0x70u);
    owner = HLE_MEM32(client + 8u);
    if (!guest_readable(settings, 0xBCu) || !guest_readable(owner + 0xCu, 4u))
        return;
    /* A 3D voice by its settings (DSBCAPS_CTRL3D in the flags at +8), as the
     * XDK's own play path asks before its first commit (sub_0040DC46 in FP).
     * Not the client's status bits: bit 0 is "has a chip voice" and bit 1
     * "started on the chip" (both set by that play path), so neither is ever
     * set here and the first version of this, testing bit 1, never computed
     * a thing (run 29: "3D commits 0"). */
    if (!(((const volatile uint8_t *)HLE_PTR(settings))[8] & 0x10u))
        return;                          /* not a 3D voice: nothing to commit */
    params = HLE_MEM32(settings + 0xB4u);
    vdata = HLE_MEM32(settings + 0xB8u);
    ds = HLE_MEM32(owner + 0xCu);
    if (!guest_readable(params, 0xA4u) || !guest_readable(vdata, 0x38u) ||
        !guest_readable(ds, 0xA4u))
        return;
    {
        uint32_t calc[2] = { ds + 0x30u, params };
        uint32_t data[5] = { ds + 0x30u, params, ds + 0x80u, params + 0x7Cu, vdata };
        if (!call_stdcall(hle_var_CDirectSound3DCalculator_Calculate3D, 2u, calc) ||
            !call_stdcall(hle_var_CDirectSound3DCalculator_GetVoiceData, 5u, data))
            return;
    }
    HLE_MEM32(params) = 0u;
    HLE_MEM32(params + 0x7Cu) = 0u;
    HLE_MEM32(vdata) = 0u;
    commit_note(client, settings, params, vdata);
    g_stat.commits_3d++;
}
