/*
 * dsound_buffer_model.c -- a DirectSound buffer's play state on the host clock.
 *
 * The buffer half of doaxbv-re's dsound_service_model.c
 * (https://github.com/NoRain211/doaxbv-re, recomp-runtime/), GPL-3.0.
 * Unchanged apart from this header and the dropped device-level functions.
 */
#include "dsound_buffer_model.h"

#include <stddef.h>

static int buffer_configured(const RecompDsoundBufferModel *model)
{
    return model != NULL && model->size_bytes != 0u &&
        model->sample_rate != 0u && model->block_align != 0u &&
        model->size_bytes % model->block_align == 0u &&
        model->loop_start_bytes < model->size_bytes &&
        model->loop_start_bytes % model->block_align == 0u;
}

uint32_t recomp_dsound_buffer_configure(
    RecompDsoundBufferModel *model, uint32_t size_bytes,
    uint32_t sample_rate, uint32_t block_align, uint64_t now_ms)
{
    if (model == NULL || size_bytes == 0u || sample_rate == 0u ||
        block_align == 0u || size_bytes % block_align != 0u) {
        return RECOMP_DSOUND_INVALID_PARAM;
    }
    *model = (RecompDsoundBufferModel){
        .size_bytes = size_bytes,
        .sample_rate = sample_rate,
        .original_sample_rate = sample_rate,
        .block_align = block_align,
        .last_ms = now_ms,
    };
    return RECOMP_DSOUND_OK;
}

uint32_t recomp_dsound_buffer_cursor(
    RecompDsoundBufferModel *model, uint64_t now_ms)
{
    if (!buffer_configured(model)) {
        return 0u;
    }
    if (!model->playing || now_ms <= model->last_ms) {
        return model->cursor_bytes;
    }
    uint64_t elapsed = now_ms - model->last_ms;
    uint64_t seconds = elapsed / 1000u;
    uint64_t fraction =
        (elapsed % 1000u) * model->sample_rate + model->frame_remainder;
    uint64_t frames = model->size_bytes / model->block_align;
    uint64_t position = model->cursor_bytes / model->block_align;
    model->last_ms = now_ms;
    model->frame_remainder = (uint32_t)(fraction % 1000u);
    uint64_t remaining = frames - position;
    if (seconds <= remaining / model->sample_rate &&
        seconds * model->sample_rate + fraction / 1000u < remaining) {
        model->cursor_bytes = (uint32_t)((position + seconds * model->sample_rate +
            fraction / 1000u) * model->block_align);
        return model->cursor_bytes;
    }
    if (!(model->play_flags & RECOMP_DSOUND_PLAY_LOOPING)) {
        model->playing = 0u;
        model->cursor_bytes = 0u;
        model->frame_remainder = 0u;
        return 0u;
    }
    uint64_t loop_start = model->loop_start_bytes / model->block_align;
    uint64_t loop_frames = frames - loop_start;
    /* Subtract the first passage to the end before wrapping within the tail.
       Reduce before multiplying so even a uint64_t elapsed time cannot overflow. */
    uint64_t advance =
        ((seconds % loop_frames) * model->sample_rate) % loop_frames + fraction / 1000u;
    model->cursor_bytes = (uint32_t)((loop_start +
        (advance + loop_frames - remaining % loop_frames) % loop_frames) *
        model->block_align);
    return model->cursor_bytes;
}

uint32_t recomp_dsound_buffer_consume(
    RecompDsoundBufferModel *model, uint64_t now_ms, uint32_t *offset_bytes)
{
    if (!buffer_configured(model) || offset_bytes == NULL ||
        !model->playing || now_ms <= model->last_ms) {
        return 0u;
    }
    uint64_t elapsed = now_ms - model->last_ms;
    uint32_t position = model->cursor_bytes;
    uint32_t remainder = model->frame_remainder;
    recomp_dsound_buffer_cursor(model, now_ms);
    /* ponytail: discard stalls over 100 ms instead of replaying stale ring data;
       use a device-driven producer when continuous output across stalls matters. */
    if (elapsed > 100u) {
        return 0u;
    }
    uint64_t frames = (elapsed * model->sample_rate + remainder) / 1000u;
    if (!(model->play_flags & RECOMP_DSOUND_PLAY_LOOPING)) {
        uint32_t remaining = (model->size_bytes - position) / model->block_align;
        if (frames > remaining) {
            frames = remaining;
        }
    }
    if (frames > UINT32_MAX / model->block_align) {
        return 0u;
    }
    *offset_bytes = position;
    return (uint32_t)frames * model->block_align;
}

uint32_t recomp_dsound_buffer_play(
    RecompDsoundBufferModel *model, uint32_t flags, uint64_t now_ms)
{
    if (!buffer_configured(model) ||
        (flags & ~(RECOMP_DSOUND_PLAY_LOOPING | RECOMP_DSOUND_PLAY_FROMSTART))) {
        return RECOMP_DSOUND_INVALID_PARAM;
    }
    recomp_dsound_buffer_cursor(model, now_ms);
    if (flags & RECOMP_DSOUND_PLAY_FROMSTART) {
        model->cursor_bytes = 0u;
        model->frame_remainder = 0u;
    }
    model->play_flags = flags;
    model->playing = 1u;
    if (now_ms > model->last_ms) {
        model->last_ms = now_ms;
    }
    return RECOMP_DSOUND_OK;
}

void recomp_dsound_buffer_stop(
    RecompDsoundBufferModel *model, uint64_t now_ms)
{
    if (model != NULL) {
        recomp_dsound_buffer_cursor(model, now_ms);
        model->playing = 0u;
    }
}

uint32_t recomp_dsound_buffer_set_position(
    RecompDsoundBufferModel *model, uint32_t position_bytes, uint64_t now_ms)
{
    if (!buffer_configured(model) || position_bytes >= model->size_bytes ||
        position_bytes % model->block_align != 0u) {
        return RECOMP_DSOUND_INVALID_PARAM;
    }
    recomp_dsound_buffer_cursor(model, now_ms);
    model->cursor_bytes = position_bytes;
    model->frame_remainder = 0u;
    return RECOMP_DSOUND_OK;
}

uint32_t recomp_dsound_buffer_set_frequency(
    RecompDsoundBufferModel *model, uint32_t sample_rate, uint64_t now_ms)
{
    if (!buffer_configured(model)) {
        return RECOMP_DSOUND_INVALID_PARAM;
    }
    if (sample_rate == 0u) {
        sample_rate = model->original_sample_rate;
    }
    if (sample_rate == 0u) {
        return RECOMP_DSOUND_INVALID_PARAM;
    }
    recomp_dsound_buffer_cursor(model, now_ms);
    model->sample_rate = sample_rate;
    return RECOMP_DSOUND_OK;
}

static int32_t clamp_level(int64_t level)
{
    if (level < -10000) return -10000;
    if (level > 0) return 0;
    return (int32_t)level;
}

int32_t recomp_dsound_mix_adjust(const uint8_t *bins, uint32_t bin_count,
                                 const int32_t bin_volumes[32],
                                 const int32_t *voice3d)
{
    int32_t d = 0, front = 0, back = 0, center = 0, i3dl2 = 0;
    int32_t best_dry = -10000, best_send = -10000;
    int any_dry = 0, any_send = 0;
    uint32_t i;

    if (bins == NULL || bin_volumes == NULL || bin_count == 0u || bin_count > 8u)
        return 0;
    if (voice3d != NULL) {
        d      = clamp_level((int64_t)voice3d[1] + voice3d[2]);
        front  = clamp_level((int64_t)voice3d[7] + voice3d[5] + voice3d[3] + d);
        back   = clamp_level((int64_t)voice3d[7] + voice3d[4] + d);
        center = clamp_level((int64_t)voice3d[7] + voice3d[6] + voice3d[3] + d);
        i3dl2  = clamp_level((int64_t)voice3d[8] + d);
    }
    for (i = 0; i < bin_count; i++) {
        uint32_t bin = bins[i];
        int32_t level;

        if (bin > 31u)
            continue;
        if (bin <= 9u) {
            int32_t at = bin == 2u ? center : bin <= 5u ? d : bin <= 7u ? front : back;
            level = clamp_level((int64_t)bin_volumes[bin] + at);
            if (level > best_dry) best_dry = level;
            any_dry = 1;
        } else {
            level = clamp_level((int64_t)bin_volumes[bin] + (bin == 10u ? i3dl2 : d));
            if (level > best_send) best_send = level;
            any_send = 1;
        }
    }
    /* No audible dry bin but an audible send (I3DL2 or an FX send): the
     * console hears that voice through its DSP effect, which is not run
     * here, so it is passed through at the send's level rather than lost. */
    if (any_send && (!any_dry || best_dry <= -10000) && best_send > -10000)
        return best_send;
    return any_dry ? best_dry : 0;
}
