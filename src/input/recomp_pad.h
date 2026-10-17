/*
 * recomp_pad.h -- the host's game pads, whichever API reads them.
 *
 * Two APIs sit behind this. SDL3 is the default on every platform: it
 * reads Xbox pads and everything else -- DualSense and DualShock 4,
 * Switch Pro, 8BitDo, the Steam Deck's controls, generic pads through
 * SDL's mapping database -- with hotplug and rumble for all of them.
 * XInput is kept on Windows behind a switch (RECOMP_PAD_API=xinput, or
 * "pad_api": "xinput" in the bindings file the launcher writes), for a pad
 * or a machine where SDL3 misbehaves.
 *
 * State comes back in XInput's layout whichever API read it: its button
 * bits, triggers 0..255, sticks -32768..32767 with up positive. Everything
 * above this -- the bindings, the launcher's menus and its press capture --
 * was written against that layout and does not change.
 *
 * Slots are 0..3. With SDL3 a pad takes the first free slot when it is
 * connected and keeps it until it is unplugged, so a second pad plugged in
 * mid-game is pad 2 and pad 1 does not move. With XInput a slot is XInput's
 * own user index.
 */
#ifndef XBOXRECOMP_RECOMP_PAD_H
#define XBOXRECOMP_RECOMP_PAD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RECOMP_PAD_SLOTS 4

/* XINPUT_GAMEPAD_* values, spelled out so this compiles without <xinput.h>. */
enum {
    RECOMP_PAD_DPAD_UP    = 0x0001, RECOMP_PAD_DPAD_DOWN   = 0x0002,
    RECOMP_PAD_DPAD_LEFT  = 0x0004, RECOMP_PAD_DPAD_RIGHT  = 0x0008,
    RECOMP_PAD_START      = 0x0010, RECOMP_PAD_BACK        = 0x0020,
    RECOMP_PAD_LEFT_THUMB = 0x0040, RECOMP_PAD_RIGHT_THUMB = 0x0080,
    RECOMP_PAD_LEFT_SHOULDER = 0x0100, RECOMP_PAD_RIGHT_SHOULDER = 0x0200,
    RECOMP_PAD_A = 0x1000, RECOMP_PAD_B = 0x2000,
    RECOMP_PAD_X = 0x4000, RECOMP_PAD_Y = 0x8000
};

/* XINPUT_GAMEPAD's fields, in its order. */
typedef struct RecompPadState {
    uint16_t buttons;
    uint8_t  left_trigger, right_trigger;
    int16_t  lx, ly, rx, ry;
} RecompPadState;

enum { RECOMP_PAD_API_SDL = 0, RECOMP_PAD_API_XINPUT = 1 };

/* Which API reads the pads. RECOMP_PAD_API (sdl or xinput) wins; then
 * whatever recomp_pad_set_api was told, which is the bindings file's
 * "pad_api"; then SDL3. XInput on a host without it is SDL3. */
int         recomp_pad_api(void);
const char *recomp_pad_api_name(void);          /* "SDL3" or "XInput" */
void        recomp_pad_set_api(int api);        /* takes effect on the next read */

/* A slot's state. 1 if a pad is connected there, else 0 with *out zeroed.
 * Safe from any thread; cheap to call many times a frame. */
int  recomp_pad_read(int slot, RecompPadState *out);

/* The pad's own name ("DualSense Wireless Controller"), or "" if none. */
void recomp_pad_name(int slot, char *out, size_t n);

/* Rumble, 0..65535 per motor, as XInputSetState takes it. Held until the
 * next call. A pad without motors ignores it. */
void recomp_pad_rumble(int slot, uint16_t low, uint16_t high);

/* For tests: a pad in this slot with this state, standing in for whatever
 * is plugged in; NULL takes it out again. */
void recomp_pad_test_set(int slot, const RecompPadState *state);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_RECOMP_PAD_H */
