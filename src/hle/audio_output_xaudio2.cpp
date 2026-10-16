/*
 * audio_output_xaudio2.cpp -- XAudio2 output for replaced DirectSound buffers.
 *
 * From doaxbv-re (https://github.com/NoRain211/doaxbv-re,
 * recomp-runtime/audio_output_xaudio2.cpp), GPL-3.0. Changed: an unset
 * RECOMP_AUDIO_GAIN means full gain here rather than muted.
 */
#include "audio_output.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <xaudio2.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" int recomp_input_window_active(void);                 /* input_mouse.c */
extern "C" const char *recomp_config_lookup(const char *env, const char *key);
extern "C" unsigned long long recomp_input_script_ms(void);      /* input_host.c */

namespace {

/* 0..255 are DirectSound buffers (hle_dsound.c), 256..271 streams
 * (hle_dsound_stream.c), 272 a movie's sound (hle_xmv.c). */
constexpr uint32_t kVoiceCount = 256 + 16 + 1;
/* Submissions in flight per voice. A stream keeps LEAD_MS of sound ahead of
   its clock in CHUNK_MS pieces (hle_dsound_stream.c), so four was one short
   of what the music path asks for and every frame past the first dropped a
   whole chunk: the sound fell behind the picture by 100 ms at a time, and a
   two-minute run lost 42 seconds of music. */
constexpr uint32_t kQueueSize = 12;
constexpr uint32_t kMaxBufferBytes = 160000;

struct PcmBuffer {
    uint8_t *data;
    uint32_t capacity;
};

struct OutputVoice {
    IXAudio2SourceVoice *source;
    PcmBuffer buffers[kQueueSize];
    uint32_t sample_rate, channels, bits_per_sample;
    uint32_t front, queued;
};

std::atomic<HRESULT> critical_error{S_OK};

struct EngineCallback : IXAudio2EngineCallback {
    void STDMETHODCALLTYPE OnProcessingPassStart() override {}
    void STDMETHODCALLTYPE OnProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnCriticalError(HRESULT error) override
    {
        critical_error.store(error, std::memory_order_relaxed);
    }
};

EngineCallback callback;
IXAudio2 *engine;
IXAudio2MasteringVoice *master;
OutputVoice voices[kVoiceCount];
bool reported_nonzero[kVoiceCount];
bool attempted, com_initialized, callback_registered, summary_printed;
unsigned long long submitted_buffers, submitted_bytes, nonzero_buffers;
unsigned long long dropped_buffers;
/* The master gain as configured, and the background mute over it: the
 * setting (-1 until read), and whether the master is silenced for it now. */
float master_gain = 1.0f;
std::atomic<int> mute_unfocused{-1};
std::atomic<int> background_muted{0};
/* The volume setting, percent (-1 until read), and the gain it and the
 * mute last put on the master. master_gain is RECOMP_AUDIO_GAIN's, before
 * either. */
std::atomic<int> volume_percent{-1};
float applied_gain = -1.0f;
/* What creating and destroying source voices costs the calling thread. */
unsigned long long voices_created, voices_destroyed;
double create_ms_total, create_ms_max, destroy_ms_total, destroy_ms_max;

double nowMs()
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1000.0 /
           static_cast<double>(freq.QuadPart);
}

void destroyVoice(OutputVoice &voice)
{
    /* DestroyVoice waits until XAudio2 no longer reads these buffers. */
    if (voice.source) {
        const double t0 = nowMs();
        voice.source->DestroyVoice();
        const double ms = nowMs() - t0;
        ++voices_destroyed;
        destroy_ms_total += ms;
        if (ms > destroy_ms_max) destroy_ms_max = ms;
    }
    for (auto &buffer : voice.buffers) std::free(buffer.data);
    voice = {};
}

void releaseOutput()
{
    for (auto &voice : voices) destroyVoice(voice);
    if (master) master->DestroyVoice();
    master = nullptr;
    if (engine) {
        if (callback_registered) engine->UnregisterForCallbacks(&callback);
        engine->Release();
    }
    engine = nullptr;
    callback_registered = false;
    if (com_initialized) CoUninitialize();
    com_initialized = false;
}

void disableOutput(const char *operation, HRESULT error)
{
    std::fprintf(stderr,
        "[audio-output] disabled operation=%s error=0x%08lx\n",
        operation, static_cast<unsigned long>(error));
    releaseOutput();
}

void checkCriticalError()
{
    const HRESULT error = critical_error.load(std::memory_order_relaxed);
    if (engine && FAILED(error)) disableOutput("device", error);
}

/* The volume setting as a gain: percent squared, so equal steps of the
 * setting sound like equal steps (50% is about -12 dB). */
float volumeGain()
{
    const float v = static_cast<float>(recomp_audio_output_volume()) / 100.0f;
    return v * v;
}

/* mute_unfocused, silent while another window has the focus, and the
 * volume. Followed as sound is submitted, which is all the time, on a
 * thread that owns the engine -- never from the window's or the menu's
 * thread, which could catch it being released. */
void followFocus()
{
    const int want = recomp_audio_output_mute_unfocused() && !recomp_input_window_active();
    int had = background_muted.load(std::memory_order_relaxed);
    float gain;

    if (want != had && background_muted.compare_exchange_strong(had, want))
        std::fprintf(stderr, "[audio-output] %s\n", want
            ? "muted: the game window is in the background (mute_unfocused)"
            : "sound back: the game window has the focus");
    gain = background_muted.load(std::memory_order_relaxed) ? 0.0f : master_gain * volumeGain();
    if (master && gain != applied_gain) {
        master->SetVolume(gain);
        if (gain > 0.0f && applied_gain > 0.0f)
            std::fprintf(stderr, "[audio-output] volume %d%% (master gain %.3f)\n",
                         recomp_audio_output_volume(), gain);
        applied_gain = gain;
    }
}

/* RECOMP_AUDIO_DUMP=<folder>: everything each voice is given, as it is
 * submitted, in WAV files -- slotNNN_KKK.wav, a new KKK each time the voice
 * is reset or changes format -- and submissions.csv, one line a submission
 * on the input script's clock (ms, slot, file, offset, bytes, rate, channels,
 * level). What reaches the speakers, to be read rather than listened to: a
 * repeat, a gap, noise or two voices at once all show in the files. */
struct DumpFile {
    FILE *f;
    uint32_t rate, channels, bits, bytes;
    int index;
};
DumpFile dumps[kVoiceCount];
int dump_state = -1;                        /* -1 unread, 0 off, 1 on */
char dump_dir[512];
FILE *dump_log;
CRITICAL_SECTION dump_lock;

bool dumpOn()
{
    if (dump_state < 0) {
        const char *v = std::getenv("RECOMP_AUDIO_DUMP");
        dump_state = 0;
        if (v && *v) {
            std::snprintf(dump_dir, sizeof dump_dir, "%s", v);
            CreateDirectoryA(dump_dir, nullptr);
            char path[600];
            std::snprintf(path, sizeof path, "%s\\submissions.csv", dump_dir);
            dump_log = std::fopen(path, "w");
            InitializeCriticalSection(&dump_lock);
            if (dump_log) {
                std::fprintf(dump_log, "ms,slot,file,offset,bytes,rate,channels,level\n");
                dump_state = 1;
                std::fprintf(stderr, "[audio-output] RECOMP_AUDIO_DUMP: every voice to %s\n",
                             dump_dir);
            }
        }
    }
    return dump_state == 1;
}

void dumpHeader(DumpFile &d)
{
    uint8_t h[44];
    const uint32_t align = d.channels * d.bits / 8, rate_bytes = d.rate * align;
    std::memcpy(h, "RIFF", 4);
    const uint32_t riff = 36 + d.bytes, fmt_len = 16, data = d.bytes;
    const uint16_t pcm = 1, ch = (uint16_t)d.channels, al = (uint16_t)align,
                   bits = (uint16_t)d.bits;
    std::memcpy(h + 4, &riff, 4);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    std::memcpy(h + 16, &fmt_len, 4);
    std::memcpy(h + 20, &pcm, 2);
    std::memcpy(h + 22, &ch, 2);
    std::memcpy(h + 24, &d.rate, 4);
    std::memcpy(h + 28, &rate_bytes, 4);
    std::memcpy(h + 32, &al, 2);
    std::memcpy(h + 34, &bits, 2);
    std::memcpy(h + 36, "data", 4);
    std::memcpy(h + 40, &data, 4);
    std::fseek(d.f, 0, SEEK_SET);
    std::fwrite(h, 1, sizeof h, d.f);
    std::fseek(d.f, 0, SEEK_END);
}

void dumpClose(uint32_t slot)
{
    if (dump_state != 1 || slot >= kVoiceCount) return;
    EnterCriticalSection(&dump_lock);
    if (dumps[slot].f) {
        std::fclose(dumps[slot].f);
        dumps[slot].f = nullptr;
    }
    LeaveCriticalSection(&dump_lock);
}

void dumpWrite(uint32_t slot, const uint8_t *pcm, uint32_t bytes, uint32_t rate,
               uint32_t channels, uint32_t bits, int32_t level)
{
    if (!dumpOn() || slot >= kVoiceCount) return;
    EnterCriticalSection(&dump_lock);
    DumpFile &d = dumps[slot];
    if (d.f && (d.rate != rate || d.channels != channels || d.bits != bits)) {
        std::fclose(d.f);
        d.f = nullptr;
    }
    if (!d.f) {
        char path[600];
        std::snprintf(path, sizeof path, "%s\\slot%03u_%03d.wav", dump_dir, slot, d.index++);
        d.f = std::fopen(path, "w+b");
        d.rate = rate;
        d.channels = channels;
        d.bits = bits;
        d.bytes = 0;
        if (d.f) dumpHeader(d);
    }
    if (d.f) {
        std::fprintf(dump_log, "%llu,%u,%d,%u,%u,%u,%u,%d\n", recomp_input_script_ms(), slot,
                     d.index - 1, d.bytes, bytes, rate, channels, level);
        std::fwrite(pcm, 1, bytes, d.f);
        d.bytes += bytes;
        dumpHeader(d);
        std::fflush(d.f);
        std::fflush(dump_log);
    }
    LeaveCriticalSection(&dump_lock);
}

void dropBuffer(uint32_t slot, const char *reason)
{
    ++dropped_buffers;
    if (dropped_buffers <= 4 ||
        (dropped_buffers & (dropped_buffers - 1)) == 0) {
        std::fprintf(stderr,
            "[audio-output] dropped=%llu slot=%u reason=%s\n",
            dropped_buffers, slot, reason);
    }
}

} // namespace

extern "C" void recomp_audio_output_initialize(void)
{
    checkCriticalError();
    if (attempted) return;
    attempted = true;
    double gain = 1.0;
    const char *setting = std::getenv("RECOMP_AUDIO_GAIN");
    if (setting) {
        char *end;
        gain = std::strtod(setting, &end);
        if (end == setting || *end != '\0' || !std::isfinite(gain) ||
            gain < 0.0 || gain > 1.0) {
            std::fprintf(stderr,
                "[audio-output] muted invalid RECOMP_AUDIO_GAIN (expected 0..1)\n");
            return;
        }
    }
    if (gain == 0.0) {
        std::fprintf(stderr, "[audio-output] muted RECOMP_AUDIO_GAIN=0\n");
        return;
    }
    HRESULT error = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(error)) {
        disableOutput("CoInitializeEx", error);
        return;
    }
    com_initialized = true;
    error = XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(error)) {
        disableOutput("XAudio2Create", error);
        return;
    }
    error = engine->RegisterForCallbacks(&callback);
    if (FAILED(error)) {
        disableOutput("RegisterForCallbacks", error);
        return;
    }
    callback_registered = true;
    error = engine->CreateMasteringVoice(&master);
    if (FAILED(error)) {
        disableOutput("CreateMasteringVoice", error);
        return;
    }
    /* RECOMP_MUTE: silent, but everything else as usual. RECOMP_AUDIO_GAIN=0
     * opens no device at all, and then nothing reports a play position: the
     * DirectSound streams and the movie's sound fall back to other clocks, so
     * a muted run would not be the run being tested. */
    {
        const char *mute = std::getenv("RECOMP_MUTE");
        if (mute && *mute && std::strcmp(mute, "0") != 0) {
            gain = 0.0;
            std::fprintf(stderr, "[audio-output] RECOMP_MUTE: playing silently\n");
        }
    }
    master_gain = static_cast<float>(gain);
    applied_gain = master_gain * volumeGain();
    error = master->SetVolume(applied_gain);
    if (FAILED(error)) {
        disableOutput("MasterSetVolume", error);
        return;
    }
    std::fprintf(stderr,
        "[audio-output] initialized backend=xaudio2_9 master_gain=%.6f\n", gain);
}

extern "C" void recomp_audio_output_set_mute_unfocused(int on)
{
    mute_unfocused.store(on ? 1 : 0, std::memory_order_relaxed);
}

extern "C" int recomp_audio_output_mute_unfocused(void)
{
    if (mute_unfocused.load(std::memory_order_relaxed) < 0) {
        const char *v = recomp_config_lookup("RECOMP_MUTE_UNFOCUSED", "mute_unfocused");
        int setting = v && *v && std::strcmp(v, "0") != 0, unread = -1;
        mute_unfocused.compare_exchange_strong(unread, setting);
    }
    return mute_unfocused.load(std::memory_order_relaxed);
}

extern "C" void recomp_audio_output_set_volume_percent(int percent)
{
    volume_percent.store(percent < 0 ? 0 : (percent > 100 ? 100 : percent),
                         std::memory_order_relaxed);
}

extern "C" int recomp_audio_output_volume(void)
{
    if (volume_percent.load(std::memory_order_relaxed) < 0) {
        const char *v = recomp_config_lookup("RECOMP_VOLUME", "volume");
        int setting = v && *v ? std::atoi(v) : 100, unread = -1;
        if (setting < 0) setting = 0;
        if (setting > 100) setting = 100;
        volume_percent.compare_exchange_strong(unread, setting);
    }
    return volume_percent.load(std::memory_order_relaxed);
}

extern "C" void recomp_audio_output_shutdown(void)
{
    if (summary_printed) return;
    checkCriticalError();
    releaseOutput();
    attempted = true;
    summary_printed = true;
    std::fprintf(stderr,
        "[audio-output] summary submitted_buffers=%llu submitted_bytes=%llu "
        "nonzero_buffers=%llu dropped_buffers=%llu\n",
        submitted_buffers, submitted_bytes, nonzero_buffers, dropped_buffers);
}

extern "C" void recomp_audio_output_reset_voice(uint32_t slot)
{
    checkCriticalError();
    if (slot < kVoiceCount) destroyVoice(voices[slot]);
    dumpClose(slot);
}

extern "C" int recomp_audio_output_position(uint32_t slot, uint64_t *played_bytes,
                                            uint32_t *queued_buffers)
{
    if (!engine || slot >= kVoiceCount) return 0;
    OutputVoice &voice = voices[slot];
    if (!voice.source) return 0;

    XAUDIO2_VOICE_STATE state{};
    voice.source->GetState(&state, 0);     /* SamplesPlayed wanted here */
    const uint32_t block_align = voice.channels * (voice.bits_per_sample / 8);
    if (played_bytes)
        *played_bytes = static_cast<uint64_t>(state.SamplesPlayed) * block_align;
    if (queued_buffers) *queued_buffers = state.BuffersQueued;
    return 1;
}

static float gainOf(int32_t volume_hundredth_db)
{
    if (volume_hundredth_db < -10000) volume_hundredth_db = -10000;
    if (volume_hundredth_db > 0) volume_hundredth_db = 0;
    return volume_hundredth_db == -10000 ? 0.0f :
        std::pow(10.0f, volume_hundredth_db / 2000.0f);
}

extern "C" int recomp_audio_output_set_volume(uint32_t slot, int32_t volume_hundredth_db)
{
    if (!engine || slot >= kVoiceCount) return 0;
    OutputVoice &voice = voices[slot];
    if (!voice.source) return 0;
    if (FAILED(voice.source->SetVolume(gainOf(volume_hundredth_db)))) return 0;
    return 1;
}

extern "C" int recomp_audio_output_engine_stats(RecompAudioEngineStats *stats)
{
    if (!stats) return 0;
    *stats = {};
    stats->voices_created = voices_created;
    stats->voices_destroyed = voices_destroyed;
    stats->create_ms_total = create_ms_total;
    stats->create_ms_max = create_ms_max;
    stats->destroy_ms_total = destroy_ms_total;
    stats->destroy_ms_max = destroy_ms_max;
    if (!engine || !master) return 0;
    XAUDIO2_PERFORMANCE_DATA perf{};
    engine->GetPerformanceData(&perf);
    XAUDIO2_VOICE_DETAILS details{};
    master->GetVoiceDetails(&details);
    stats->latency_samples = perf.CurrentLatencyInSamples;
    stats->sample_rate = details.InputSampleRate;
    stats->glitches = perf.GlitchesSinceEngineStarted;
    stats->active_voices = perf.ActiveSourceVoiceCount;
    stats->total_voices = perf.TotalSourceVoiceCount;
    return 1;
}

extern "C" int recomp_audio_output_submit(
    uint32_t slot, const uint8_t *pcm, uint32_t bytes,
    uint32_t sample_rate, uint32_t channels, uint32_t bits_per_sample,
    int32_t volume_hundredth_db)
{
    recomp_audio_output_initialize();
    if (!engine || bytes == 0) return 0;
    followFocus();
    if (slot >= kVoiceCount || !pcm || bytes > kMaxBufferBytes ||
        sample_rate < XAUDIO2_MIN_SAMPLE_RATE ||
        sample_rate > XAUDIO2_MAX_SAMPLE_RATE ||
        (channels != 1 && channels != 2) ||
        (bits_per_sample != 8 && bits_per_sample != 16)) {
        dropBuffer(slot, "invalid-pcm");
        return 0;
    }
    const uint32_t block_align = channels * (bits_per_sample / 8);
    if (bytes % block_align != 0) {
        dropBuffer(slot, "partial-frame");
        return 0;
    }

    OutputVoice &voice = voices[slot];
    if (voice.source && (voice.sample_rate != sample_rate ||
        voice.channels != channels || voice.bits_per_sample != bits_per_sample)) {
        destroyVoice(voice);
    }
    if (!voice.source) {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = static_cast<WORD>(channels);
        format.nSamplesPerSec = sample_rate;
        format.nAvgBytesPerSec = sample_rate * block_align;
        format.nBlockAlign = static_cast<WORD>(block_align);
        format.wBitsPerSample = static_cast<WORD>(bits_per_sample);
        const double t0 = nowMs();
        HRESULT error = engine->CreateSourceVoice(
            &voice.source, &format, XAUDIO2_VOICE_NOPITCH, 1.0f);
        {
            const double ms = nowMs() - t0;
            ++voices_created;
            create_ms_total += ms;
            if (ms > create_ms_max) create_ms_max = ms;
        }
        if (FAILED(error)) {
            dropBuffer(slot, "create-voice");
            disableOutput("CreateSourceVoice", error);
            return 0;
        }
        voice.sample_rate = sample_rate;
        voice.channels = channels;
        voice.bits_per_sample = bits_per_sample;
        error = voice.source->Start();
        if (FAILED(error)) {
            dropBuffer(slot, "start-voice");
            disableOutput("Start", error);
            return 0;
        }
    }

    const float gain = gainOf(volume_hundredth_db);
    HRESULT error = voice.source->SetVolume(gain);
    if (FAILED(error)) {
        dropBuffer(slot, "volume");
        disableOutput("SetVolume", error);
        return 0;
    }
    XAUDIO2_VOICE_STATE state{};
    voice.source->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    /* BuffersQueued includes the active buffer; only retire the FIFO prefix. */
    while (voice.queued > state.BuffersQueued) {
        voice.front = (voice.front + 1) % kQueueSize;
        --voice.queued;
    }
    if (voice.queued == kQueueSize) {
        dropBuffer(slot, "queue-full");
        return 0;
    }
    PcmBuffer &owned = voice.buffers[(voice.front + voice.queued) % kQueueSize];
    if (owned.capacity < bytes) {
        auto *data = static_cast<uint8_t *>(std::realloc(owned.data, bytes));
        if (!data) {
            dropBuffer(slot, "allocation");
            return 0;
        }
        owned.data = data;
        owned.capacity = bytes;
    }
    std::memcpy(owned.data, pcm, bytes);
    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = bytes;
    buffer.pAudioData = owned.data;
    error = voice.source->SubmitSourceBuffer(&buffer);
    if (FAILED(error)) {
        dropBuffer(slot, "submit");
        disableOutput("SubmitSourceBuffer", error);
        return 0;
    }
    ++voice.queued;
    ++submitted_buffers;
    submitted_bytes += bytes;
    dumpWrite(slot, pcm, bytes, sample_rate, channels, bits_per_sample, volume_hundredth_db);
    bool nonzero = false;
    const uint8_t silence = bits_per_sample == 8 ? 0x80 : 0;
    for (uint32_t i = 0; i < bytes && !nonzero; ++i)
        nonzero = owned.data[i] != silence;
    if (nonzero) ++nonzero_buffers;
    if (submitted_buffers == 1 || (nonzero && !reported_nonzero[slot])) {
        std::fprintf(stderr,
            "[audio-output] submitted slot=%u bytes=%u rate=%u channels=%u "
            "bits=%u nonzero=%u gain=%.6f\n",
            slot, bytes, sample_rate, channels, bits_per_sample,
            nonzero ? 1u : 0u, static_cast<double>(gain));
    }
    if (nonzero) reported_nonzero[slot] = true;
    return 1;
}
