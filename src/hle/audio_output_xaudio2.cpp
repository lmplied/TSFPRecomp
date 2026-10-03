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
    error = master->SetVolume(static_cast<float>(gain));
    if (FAILED(error)) {
        disableOutput("MasterSetVolume", error);
        return;
    }
    std::fprintf(stderr,
        "[audio-output] initialized backend=xaudio2_9 master_gain=%.6f\n", gain);
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
