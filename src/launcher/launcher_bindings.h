/*
 * launcher_bindings.h -- the input config, as the launcher edits it.
 *
 * The runtime reads this same file (src/input/input_bindings.c) and
 * resolves every string to a key code or a pad index at load. The
 * launcher needs the strings themselves, to show them and to change
 * them, so it keeps its own view: the file's text, not the runtime's
 * resolved form.
 *
 * The vocabulary is the runtime's and nothing here may invent any of
 * it. A control the runtime does not know is a binding that silently
 * does nothing, which is worse than refusing to write it, so the names
 * below are the runtime's table copied deliberately and checked against
 * it by tools/input_ui/test_runtime_vocabulary.py.
 *
 * Bindings belong to the person, not the title: one file, shared by
 * every game the toolkit builds, in the place the runtime looks.
 */
#ifndef XBOXRECOMP_LAUNCHER_BINDINGS_H
#define XBOXRECOMP_LAUNCHER_BINDINGS_H

#include <stddef.h>

#define BIND_PORTS      4
#define BIND_CONTROLS   24
#define BIND_SOURCE_LEN 32
#define BIND_EXTRA      2       /* sources past the two shown; the runtime takes 4 */

/* The 24 Xbox inputs, in the order the launcher lists them, by the names
 * the config file uses. */
extern const char *const bind_control_names[BIND_CONTROLS];
/* The same, as a player would read them. */
extern const char *const bind_control_labels[BIND_CONTROLS];

typedef enum { BIND_DEV_NONE, BIND_DEV_PAD, BIND_DEV_KEYBOARD } BindDevice;

/* How mouse motion becomes a stick: the file's mouse_* keys, which the
 * runtime's MouseFeel reads. Written only when `present`, so a file that
 * never had them does not grow them. */
typedef struct BindMouse {
    int    present;
    double sensitivity;
    int    deadzone;
    double exponent;
    double y_scale;
    int    invert_y;
    int    window_ms;
    int    zoom_match;      /* zoomed in, turn less by the zoom (a title that
                             * aims by the mouse itself) */
} BindMouse;

typedef struct BindPort {
    BindDevice device;
    int        pad;                       /* pad slot, 0..3 */
    int        deadzone;
    /* One pad source and one keyboard-or-mouse source per control, which
     * is the shape the defaults have and the shape a player thinks in. */
    char pad_src[BIND_CONTROLS][BIND_SOURCE_LEN];
    char key_src[BIND_CONTROLS][BIND_SOURCE_LEN];
    /* Any more the file had -- a preset that answers Enter, Space and E as
     * the same button -- kept and written back, so a save from the launcher
     * does not quietly drop what it does not show. */
    char extra_src[BIND_CONTROLS][BIND_EXTRA][BIND_SOURCE_LEN];
    BindMouse mouse;
    /* The file's "hold": bit k set when control k's keys and mouse buttons
     * hold a toggle on rather than flip it (input_bindings.h). */
    unsigned hold;
} BindPort;

/* Which API reads the pads, for every port and every game: SDL3 reads
 * every kind of controller, XInput only Xbox ones. The file's "pad_api". */
enum { BIND_PAD_API_SDL = 0, BIND_PAD_API_XINPUT = 1 };

typedef struct BindConfig {
    BindPort port[BIND_PORTS];
    int      pad_api;                     /* BIND_PAD_API_* */
    /* What each control does in this game ("Fire", "Reload"), from the
     * file's "labels" -- a title's preset carries them -- or empty. Shown
     * instead of the Xbox name, which a player at a keyboard does not
     * think in; the runtime never reads them. */
    char     action[BIND_CONTROLS][32];
} BindConfig;

/* The built-in mapping, the same one the runtime falls back to with no
 * file at all. Where the file lives is recomp_bindings_read_path and
 * recomp_bindings_write_path (src/config), which the runtime uses too. */
void bind_defaults(BindConfig *c);

/* Read and write that schema. A read that finds no file leaves the
 * defaults in place and returns 0. */
int  bind_load(const char *path, BindConfig *c);
int  bind_save(const char *path, const BindConfig *c);

/* Read the file at path and, from the title's preset beside the executable,
 * the action names it lacks: they describe the game, not anyone's choice, so
 * a file saved before the preset named them still shows them. Returns what
 * bind_load returned for path. */
int  bind_load_with_labels(const char *path, const char *preset_path, BindConfig *c);

/* A control as a player would know it: its action in this game when the
 * file names one ("Fire"), else its Xbox name ("Right trigger"). */
const char *bind_control_title(const BindConfig *c, int control);

/* A keyboard or mouse source: the launcher's second column. */
int  bind_is_key_or_mouse(const char *src);

/* Drop the keyboard and mouse sources of one control that the launcher
 * does not show, for when a player rebinds that column: what they see is
 * then what the control does. */
void bind_drop_hidden_keys(BindPort *bp, int control);

/* How many sources a control has that the launcher does not show. */
int  bind_hidden_count(const BindPort *bp, int control);

/* Capturing a keyboard or mouse binding: what a person presses next. Used by
 * the launcher and the in-game menu alike, so they offer the same keys.
 * Windows only (it reads GetAsyncKeyState); elsewhere it never answers. */
typedef struct BindCapture {
    long from_x, from_y;                  /* the pointer when it began */
    int  wheel;                           /* a notch since: 1 up, -1 down */
} BindCapture;

/* Begin: remember where the pointer is, for a stick direction bound by
 * moving the mouse that way. */
void bind_capture_begin(BindCapture *c);
/* A wheel notch arrived (WM_MOUSEWHEEL's delta). */
void bind_capture_wheel(BindCapture *c, int delta);
/* What is pressed now as a source -- a key, a mouse button, a wheel notch,
 * or for a stick direction (control naming one) the mouse pushed that way
 * -- or NULL. Never Escape, which cancels, nor F1, the in-game menu's own
 * key, nor F9-F12, the game's frame counter, frame cap, capture and
 * screenshot keys. */
const char *bind_capture_poll(BindCapture *c, int control);

/* A source string as a player would read it: "pad:lx+" -> "Left stick
 * left", "key:RETURN" -> "Enter", "mouse:left" -> "Left click",
 * "" -> "Not bound". */
const char *bind_source_label(const char *src);

#endif /* XBOXRECOMP_LAUNCHER_BINDINGS_H */
