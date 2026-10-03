/*
 * audio_output.h -- host audio output for replaced DirectSound buffers.
 *
 * From doaxbv-re (https://github.com/NoRain211/doaxbv-re,
 * recomp-runtime/audio_output.h), GPL-3.0. One behaviour change: output is on
 * at full gain unless RECOMP_AUDIO_GAIN says otherwise (doaxbv-re defaults to
 * muted while it validates its decoders).
 */
#ifndef XBOXRECOMP_AUDIO_OUTPUT_H
#define XBOXRECOMP_AUDIO_OUTPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialization is attempted once per process; an unavailable device or
   shutdown leaves guest audio timing unchanged. RECOMP_AUDIO_GAIN is the host
   master gain (0..1); absent means 1, zero mutes without opening a device. */
#define RECOMP_AUDIO_SLOT_MOVIE 272u

void recomp_audio_output_initialize(void);
void recomp_audio_output_shutdown(void);
void recomp_audio_output_reset_voice(uint32_t slot);

/* Copies PCM before returning; never retains a guest-memory pointer.
   Slots: 0..255 for buffers, 256..271 for streams, 272
   (RECOMP_AUDIO_SLOT_MOVIE) for a movie's sound. PCM: 1000..200000 Hz,
   mono/stereo, unsigned 8 or signed 16 bit.
   Each submission is frame-aligned and at most 160000 bytes.

   Returns 1 when the sound was taken and 0 when it was not -- a full queue,
   a device that is gone, PCM it cannot play. A caller feeding a continuous
   stream must not treat 0 as delivered: the sound it describes will never be
   heard, so it has to be offered again rather than stepped over, or the
   stream slips behind by exactly what was dropped. */
int recomp_audio_output_submit(
    uint32_t slot, const uint8_t *pcm, uint32_t bytes,
    uint32_t sample_rate, uint32_t channels, uint32_t bits_per_sample,
    int32_t volume_hundredth_db);

/* Where this slot actually is: bytes of PCM the device has played since the
   voice was created (reset_voice destroys it, so the count restarts at zero
   with the next submission), and how many submissions are still queued.
   Returns 0 when there is no voice, when output is off, or when the count
   cannot be read -- the caller must then fall back to its own clock.

   This is the device's clock, not the wall clock, and it is the one the
   listener hears: a stream whose packets complete on the wall clock drifts
   away from what is coming out of the speakers, by the device's rate error
   and by every gap the queue ever ran dry for. */
int recomp_audio_output_position(uint32_t slot, uint64_t *played_bytes,
                                 uint32_t *queued_buffers);

/* The slot's volume, now, for everything it has queued: a voice whose level
   changes while it plays (a 3D sound moving) follows without waiting for its
   next submission. 0 when there is no voice. */
int recomp_audio_output_set_volume(uint32_t slot, int32_t volume_hundredth_db);

/* What the engine says about itself, for the DirectSound diagnostics:
   latency is XAudio2's CurrentLatencyInSamples (from a source buffer to the
   speakers) at the mastering rate; the create/destroy times are what
   CreateSourceVoice and DestroyVoice cost the calling (game) thread. */
typedef struct RecompAudioEngineStats {
    uint32_t latency_samples, sample_rate, glitches;
    uint32_t active_voices, total_voices;
    uint64_t voices_created, voices_destroyed;
    double   create_ms_total, create_ms_max, destroy_ms_total, destroy_ms_max;
} RecompAudioEngineStats;
int recomp_audio_output_engine_stats(RecompAudioEngineStats *stats);

#ifdef __cplusplus
}
#endif

#endif
