/*
 * dsound_buffer_model.h -- a DirectSound buffer's play state on the host clock.
 *
 * The buffer half of doaxbv-re's dsound_service_model
 * (https://github.com/NoRain211/doaxbv-re, recomp-runtime/), GPL-3.0.
 * Guest-visible play state (playing, cursor, loop, frequency) lives here and
 * advances with host time, so "has this sound stopped?" is answered without
 * waiting on the emulated audio chip.
 */
#ifndef XBOXRECOMP_DSOUND_BUFFER_MODEL_H
#define XBOXRECOMP_DSOUND_BUFFER_MODEL_H

#include <stdint.h>

enum {
    RECOMP_DSOUND_OK = 0x00000000u,
    RECOMP_DSOUND_POINTER_ERROR = 0x80004003u,
    RECOMP_DSOUND_INVALID_PARAM = 0x80070057u,
    RECOMP_DSOUND_PLAY_LOOPING = 1u,
    RECOMP_DSOUND_PLAY_FROMSTART = 2u,
};

typedef struct RecompDsoundBufferModel {
    uint32_t size_bytes;
    uint32_t sample_rate;
    uint32_t original_sample_rate;
    uint32_t block_align;
    uint32_t cursor_bytes;
    uint32_t loop_start_bytes;
    uint32_t frame_remainder;
    uint32_t play_flags;
    uint32_t playing;
    uint64_t last_ms;
} RecompDsoundBufferModel;

uint32_t recomp_dsound_buffer_configure(
    RecompDsoundBufferModel *model, uint32_t size_bytes,
    uint32_t sample_rate, uint32_t block_align, uint64_t now_ms);
uint32_t recomp_dsound_buffer_cursor(
    RecompDsoundBufferModel *model, uint64_t now_ms);
/* Consume elapsed PCM on a separate output clock; wrap at size_bytes to
   loop_start_bytes when looping. A span can cover multiple loop iterations. */
uint32_t recomp_dsound_buffer_consume(
    RecompDsoundBufferModel *model, uint64_t now_ms, uint32_t *offset_bytes);
uint32_t recomp_dsound_buffer_play(
    RecompDsoundBufferModel *model, uint32_t flags, uint64_t now_ms);
void recomp_dsound_buffer_stop(
    RecompDsoundBufferModel *model, uint64_t now_ms);
uint32_t recomp_dsound_buffer_set_position(
    RecompDsoundBufferModel *model, uint32_t position_bytes, uint64_t now_ms);
uint32_t recomp_dsound_buffer_set_frequency(
    RecompDsoundBufferModel *model, uint32_t sample_rate, uint64_t now_ms);

/* What a voice's mix bins and 3D state add to its volume, in hundredths of
   a decibel (0 = nothing, -10000 = silent): the loudest of the voice's dry
   mix bins (0-9; the I3DL2 send and the FX sends are not heard directly),
   each at its mix-bin volume plus, for a 3D voice, the attenuation the XDK's
   chip programming adds for that bin. That programming (XDK 5849,
   CMcpxVoiceClient's volume helper) sums the voice's 3D data as
     d      = distance + cone                 (words 1 and 2, clamped)
     front  = d + I3DL2 direct + XTLK front + front    (bins 6, 7)
     back   = d + I3DL2 direct + back                  (bins 8, 9)
     center = d + I3DL2 direct + center + front        (bin 2)
     d alone for the speaker bins 0, 1, 3, 4, 5,
   each clamped to -10000..0. `voice3d` is the 3D data block as words (at
   least 9: flags, then +4..+0x20) or NULL for a 2D voice. A voice whose dry
   bins are all silent but which feeds a send (I3DL2 at its own sum, I3DL2
   room + d; an FX send at d) gets the loudest send instead: the console
   hears it through a DSP effect this host does not run. No bins: 0. */
int32_t recomp_dsound_mix_adjust(const uint8_t *bins, uint32_t bin_count,
                                 const int32_t bin_volumes[32],
                                 const int32_t *voice3d);

#endif
