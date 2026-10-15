/*
 * input_bindings.h -- host devices and per-control bindings, from a config
 * file, for all four Xbox controller ports.
 *
 * One place decides what the four guest pads read: which host device stands
 * in for each port, and which key or pad control drives each of the title's
 * 24 inputs. Both input paths use it -- the XAPI replacement in src/hle and
 * the USB gamepad in src/usb -- so a binding changed once applies to a title
 * whichever way it reads its pads.
 *
 * The file is JSON, written by `py -3 -m tools.input_ui`; see
 * docs/technical/input-binding.md for the format and the search order. With
 * no file present the built-in defaults are what the runtime always did:
 * XInput pad 0 plus the keyboard on port 1, XInput pad N on port N.
 */
#ifndef XBOXRECOMP_INPUT_BINDINGS_H
#define XBOXRECOMP_INPUT_BINDINGS_H

#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load the config, once. Safe to call from anywhere and from any thread that
 * reaches it first; later calls do nothing. Logs one line naming the file it
 * read (or the built-in defaults) and the device on each port. */
void recomp_bindings_init(void);

/* Read the file again: the in-game menu has just written it. */
void recomp_bindings_reload(void);

/* The config file actually read, or NULL when the built-in defaults are in
 * use. Valid after recomp_bindings_init(). */
const char *recomp_bindings_source_path(void);

/* A human name for what drives this port: "SDL3 pad 1 + keyboard",
 * "keyboard", "XInput pad 3", "nothing". Never NULL. */
const char *recomp_bindings_device_name(unsigned port);

/* Bitmask of ports whose device is present right now, bit 0 = port 1. A
 * keyboard port is always present; a pad port is present when that pad
 * answers, or when the port also has keyboard bindings. Re-checks the pads
 * at most a few times a second, so a pad plugged in mid-game shows up. */
unsigned recomp_bindings_present_mask(void);

/* Sample one port through its bindings. Returns 0 (and a neutral pad) when
 * the port has no device. */
int recomp_bindings_sample(unsigned port, XBOX_GAMEPAD *out);

/* Rumble on the pad this port reads, 0..65535 per motor as XInputSetState
 * takes it. Nothing happens for a keyboard port or a pad without motors. */
void recomp_bindings_rumble(unsigned port, unsigned short low, unsigned short high);

/* How the mouse feels on a port, for the in-game menu to show and change
 * while the title runs (the file's mouse_sensitivity, mouse_y_scale and
 * mouse_invert_y). The first port that binds the mouse, or -1. */
typedef struct RecompMouseSettings {
    double sensitivity;
    double y_scale;
    int    invert_y;
    int    zoom_match;      /* mouse_zoom_match: zoomed in, turn less by the zoom */
} RecompMouseSettings;

int  recomp_bindings_mouse_port(void);
int  recomp_bindings_get_mouse(unsigned port, RecompMouseSettings *out);
void recomp_bindings_set_mouse(unsigned port, const RecompMouseSettings *in);

/* Hold rather than toggle. A title that toggles something on a press (aim,
 * crouch) says how the toggle stands for a port: is_on returns 1 on, 0 off,
 * -1 when there is nothing to toggle just now (a menu, a cutscene). Then a
 * control in the file's per-port "hold" list reads its keys and mouse
 * buttons as "on while held" (input_bindings.c, hold_pulse); its pad buttons
 * keep the toggle. Register before the game starts. */
void recomp_bindings_toggle_reader(const char *control, int (*is_on)(unsigned port));
int  recomp_bindings_hold_available(const char *control);

/* Whether a port's control is in its "hold" list, for the in-game menu;
 * set takes effect at the next sample. */
int  recomp_bindings_get_hold(unsigned port, const char *control);
void recomp_bindings_set_hold(unsigned port, const char *control, int on);

#ifdef __cplusplus
}
#endif

#endif
