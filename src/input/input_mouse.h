/*
 * input_mouse.h -- the host mouse, and whether the game window has the focus.
 *
 * Two sides. The game window's thread hands its messages to
 * recomp_input_window_message(), which reads raw mouse input and decides when
 * the game owns the mouse; the binding sampler (input_bindings.c), on
 * whichever guest thread polls a pad, asks what the mouse is doing and
 * whether the keyboard is the game's to read.
 *
 * The game owns the mouse only while it is "captured": the pointer hidden and
 * held inside the window. A click in the window captures it, Alt+Tab or
 * anything else that takes the focus releases it, and a fullscreen window
 * captures it as soon as it is active. Outside a capture the mouse reads as
 * still and unpressed, so moving the pointer to another window never turns
 * the camera and the click that captures never fires a weapon. Nothing is
 * captured unless a binding names a mouse source, so a title played with a
 * pad behaves exactly as before.
 */
#ifndef XBOXRECOMP_INPUT_MOUSE_H
#define XBOXRECOMP_INPUT_MOUSE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mouse buttons, as bits of recomp_mouse_buttons(). */
enum {
    RECOMP_MOUSE_LEFT   = 1u << 0,
    RECOMP_MOUSE_RIGHT  = 1u << 1,
    RECOMP_MOUSE_MIDDLE = 1u << 2,
    RECOMP_MOUSE_X1     = 1u << 3,
    RECOMP_MOUSE_X2     = 1u << 4
};

/* ---- the window side: call on the game window's own thread ---- */

/* The window the game draws into. From here on the keyboard and mouse are
 * read only while it has the focus. Registers for raw mouse input. */
void recomp_input_attach_window(void *hwnd);

/* Offer a window message. Returns 1 when it was consumed, with the value
 * the window procedure should return in *result; 0 to carry on with it as
 * before (WM_INPUT is read and still returns 0, because DefWindowProc must
 * see it too). */
int recomp_input_window_message(void *hwnd, unsigned msg, uintptr_t wp,
                                intptr_t lp, intptr_t *result);

/* The window went fullscreen or back. A fullscreen window takes the mouse
 * as soon as it is active; a windowed one waits for a click, so the frame
 * can still be dragged. */
void recomp_input_window_fullscreen(void *hwnd, int on);

/* ---- the sampler side: any thread ---- */

/* 1 while the keyboard and mouse belong to the game: its window has the
 * focus, or no window has been attached (a run without the shadow renderer
 * keeps the old behaviour of reading the keyboard regardless), and the input
 * is not suspended. */
int recomp_input_focused(void);

/* 1 while the game window has the focus, menu or not (or no window has been
 * attached): what muting in the background follows. */
int recomp_input_window_active(void);

/* Take every input away from the game, or give it back: the in-game menu is
 * open. While suspended every port samples as an untouched pad, the keyboard
 * and mouse read as idle, and the mouse is released and not captured again.
 * Any thread. */
void recomp_input_suspend(int on);
int  recomp_input_suspended(void);

/* Some binding names a mouse source, so the window should capture on a
 * click. Set by the binding loader. */
void recomp_mouse_set_wanted(int wanted);

/* Buttons held, RECOMP_MOUSE_* bits; 0 outside a capture. */
unsigned recomp_mouse_buttons(void);

/* The mouse's speed over the last window_ms milliseconds, in raw counts per
 * millisecond, x right and y down as the device reports them; 0 outside a
 * capture. Averaging over a window rather than reading what arrived since
 * the last poll makes the answer the same however often a title polls. */
void recomp_mouse_velocity(unsigned window_ms, double *vx, double *vy);

/* 1 while a wheel notch reads as a press: dir 0 up (away from the user),
 * 1 down. Each notch is one press of about 50 ms and a release of 30, so a
 * title that acts on the press edge sees every notch. */
int recomp_mouse_wheel(int dir);

/* ---- for a title that turns its camera by the mouse itself ---- */

/* Whether the game has the mouse right now. */
int  recomp_mouse_captured(void);

/* The raw counts that arrived since the last call, x right and y toward the
 * player as the device reports them; 0 outside a capture. One caller: the
 * title's camera hook, once a frame. */
void recomp_mouse_take_counts(long *dx, long *dy);

/* The title turns its camera by the mouse itself (true 1:1 aim, from
 * recomp_mouse_take_counts): mouse motion then reaches no stick, or the view
 * would turn twice. Buttons and the wheel are unaffected. A claim lasts a
 * quarter of a second, so the title renews it every frame it steers, and
 * the mouse is back on the stick soon after the title stops (a vehicle, a
 * cutscene). */
void recomp_mouse_set_direct(int on);
int  recomp_mouse_direct(void);

/* 1 once the title has claimed the mouse that way at all, lapsed or not, or
 * said at start that it will (declare): how the settings menu describes the
 * mouse, before the first claim as well. */
int  recomp_mouse_aims_directly(void);
void recomp_mouse_declare_direct(void);

/* ---- for tests ---- */

/* What one raw mouse report carries: motion in counts, buttons that went
 * down and up (RECOMP_MOUSE_* bits), wheel in WHEEL_DELTA units (120 a
 * notch, positive away from the user). Ignored outside a capture. */
void recomp_mouse_feed(long dx, long dy, unsigned down, unsigned up, int wheel);

/* Take or give back the mouse as a click or Alt+Tab would, without a
 * window. */
void recomp_mouse_test_capture(int on);

#ifdef __cplusplus
}
#endif

#endif
