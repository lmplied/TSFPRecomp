/*
 * input_bindings.c -- read the binding config, and sample the four ports.
 *
 * Structure:
 *   CONTROLS[]   the 24 Xbox inputs, by the names the config file uses
 *   DEFAULTS[]   what each control reads with no config file: one pad source
 *                and, on port 1, one keyboard source
 *   a small JSON reader (no dependency is worth one file's worth of braces)
 *   resolve      binding strings -> Source records, once, at load
 *   sample       Source records -> an XBOX_GAMEPAD, every poll
 *
 * Sources are resolved to key codes and pad indices at load time, so a poll
 * does no string work. A poll reads each distinct key once (KeyCache) and
 * each pad once, because a title polls its pads several times a frame.
 *
 * Pads are read through recomp_pad.h: SDL3 by default, XInput on Windows
 * when the file's "pad_api" or RECOMP_PAD_API says so. The keyboard is
 * read with GetAsyncKeyState on Windows and not yet anywhere else. The
 * mouse is raw input on the game window (input_mouse.c), and both it and the
 * keyboard read as untouched while the game window does not have the focus.
 */
#include "input_bindings.h"
#include "input_mouse.h"
#include "recomp_config.h"   /* where the bindings file is, per title */
#include "recomp_pad.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <time.h>
#endif

#define MAX_SOURCES 4
#define PORTS XBOX_MAX_CONTROLLERS

/* XInput's own left-stick default. A stick is bound as two halves, so the
 * deadzone is applied per half rather than radially: a title that wants a
 * circular one applies its own on top, as it did on the console. */
#define DEADZONE_DEFAULT 7849

/* ---- the controls a title can read ------------------------------------- */

enum { K_DIGITAL, K_ANALOG, K_AXIS };

/* XINPUT_GAMEPAD_TRIGGER_THRESHOLD: the value XInput documents as the point
 * a trigger counts as pressed. */
#define TRIGGER_THRESHOLD 30
enum { AXIS_LX, AXIS_LY, AXIS_RX, AXIS_RY };

static const struct {
    const char *name;
    unsigned char kind;
    unsigned short arg;        /* digital: button bit; analog: index; axis: id */
    signed char sign;          /* axis only: which half of the axis */
} CONTROLS[] = {
    { "a",            K_ANALOG,  XBOX_BUTTON_A,            0 },
    { "b",            K_ANALOG,  XBOX_BUTTON_B,            0 },
    { "x",            K_ANALOG,  XBOX_BUTTON_X,            0 },
    { "y",            K_ANALOG,  XBOX_BUTTON_Y,            0 },
    { "black",        K_ANALOG,  XBOX_BUTTON_BLACK,        0 },
    { "white",        K_ANALOG,  XBOX_BUTTON_WHITE,        0 },
    { "start",        K_DIGITAL, XBOX_GAMEPAD_START,       0 },
    { "back",         K_DIGITAL, XBOX_GAMEPAD_BACK,        0 },
    { "ltrigger",     K_ANALOG,  XBOX_BUTTON_LTRIGGER,     0 },
    { "rtrigger",     K_ANALOG,  XBOX_BUTTON_RTRIGGER,     0 },
    { "dpad_up",      K_DIGITAL, XBOX_GAMEPAD_DPAD_UP,     0 },
    { "dpad_down",    K_DIGITAL, XBOX_GAMEPAD_DPAD_DOWN,   0 },
    { "dpad_left",    K_DIGITAL, XBOX_GAMEPAD_DPAD_LEFT,   0 },
    { "dpad_right",   K_DIGITAL, XBOX_GAMEPAD_DPAD_RIGHT,  0 },
    { "lstick_up",    K_AXIS,    AXIS_LY,                  1 },
    { "lstick_down",  K_AXIS,    AXIS_LY,                 -1 },
    { "lstick_left",  K_AXIS,    AXIS_LX,                 -1 },
    { "lstick_right", K_AXIS,    AXIS_LX,                  1 },
    { "lthumb",       K_DIGITAL, XBOX_GAMEPAD_LEFT_THUMB,  0 },
    { "rstick_up",    K_AXIS,    AXIS_RY,                  1 },
    { "rstick_down",  K_AXIS,    AXIS_RY,                 -1 },
    { "rstick_left",  K_AXIS,    AXIS_RX,                 -1 },
    { "rstick_right", K_AXIS,    AXIS_RX,                  1 },
    { "rthumb",       K_DIGITAL, XBOX_GAMEPAD_RIGHT_THUMB, 0 },
};
#define CONTROL_COUNT ((int)(sizeof CONTROLS / sizeof CONTROLS[0]))

/* The built-in mapping, and the one the UI offers as "reset to defaults".
 * The keyboard column is what this runtime has always used: arrows = D-pad,
 * Enter = START, Backspace = BACK, Z = A, X = B, A = X, S = Y, Q = White,
 * W = Black, E/R = triggers. It is applied to port 1 only. Sticks have no
 * keyboard default: every comfortable key is already a button here.
 *
 * tools/input_ui/test_runtime_vocabulary.py reads this table out of this file
 * and fails if the UI's defaults have drifted from it. */
static const struct { const char *control, *pad, *key; } DEFAULTS[] = {
    { "a",            "pad:a",          "key:Z"      },
    { "b",            "pad:b",          "key:X"      },
    { "x",            "pad:x",          "key:A"      },
    { "y",            "pad:y",          "key:S"      },
    { "black",        "pad:lshoulder",  "key:W"      },
    { "white",        "pad:rshoulder",  "key:Q"      },
    { "ltrigger",     "pad:lt",         "key:E"      },
    { "rtrigger",     "pad:rt",         "key:R"      },
    { "start",        "pad:start",      "key:RETURN" },
    { "back",         "pad:back",       "key:BACK"   },
    { "dpad_up",      "pad:dpad_up",    "key:UP"     },
    { "dpad_down",    "pad:dpad_down",  "key:DOWN"   },
    { "dpad_left",    "pad:dpad_left",  "key:LEFT"   },
    { "dpad_right",   "pad:dpad_right", "key:RIGHT"  },
    { "lstick_up",    "pad:ly+",        NULL         },
    { "lstick_down",  "pad:ly-",        NULL         },
    { "lstick_left",  "pad:lx-",        NULL         },
    { "lstick_right", "pad:lx+",        NULL         },
    { "lthumb",       "pad:lthumb",     NULL         },
    { "rstick_up",    "pad:ry+",        NULL         },
    { "rstick_down",  "pad:ry-",        NULL         },
    { "rstick_left",  "pad:rx-",        NULL         },
    { "rstick_right", "pad:rx+",        NULL         },
    { "rthumb",       "pad:rthumb",     NULL         },
};

/* ---- the sources a control can be bound to ----------------------------- */

enum { SRC_NONE, SRC_KEY, SRC_PAD_BUTTON, SRC_PAD_TRIGGER, SRC_PAD_AXIS,
       SRC_MOUSE_BUTTON, SRC_MOUSE_WHEEL, SRC_MOUSE_AXIS };

/* What a "mouse:" source may name. The buttons first, in RECOMP_MOUSE_* bit
 * order; then the wheel, each notch a short press; then motion, as the four
 * halves of two axes the way a stick is bound: x+ is right, y+ is away from
 * the player, which is "up" on a stick. Bind mouse:x+ to rstick_right and the
 * mouse turns the camera through the title's own stick handling.
 *
 * tools/input_ui/test_runtime_vocabulary.py reads this table out of this file
 * and fails if the UI's copy has drifted from it. */
enum { MS_LEFT, MS_RIGHT, MS_MIDDLE, MS_X1, MS_X2, MS_WHEEL_UP, MS_WHEEL_DOWN,
       MS_XPOS, MS_XNEG, MS_YPOS, MS_YNEG, MS_COUNT };

static const char *const MOUSE_SOURCES[MS_COUNT] = {
    "left", "right", "middle", "x1", "x2", "wheel_up", "wheel_down",
    "x+", "x-", "y+", "y-"
};

/* Mouse speed, in raw counts a millisecond, that reads as a full stick at
 * sensitivity 1: about 4.5 inches a second at 800 dpi. A player's
 * sensitivity scales it, and most will want to. */
#define MOUSE_FULL_SPEED 6.0
#define MOUSE_WINDOW_DEFAULT 16             /* ms: one frame at 60 */

/* Pad buttons, in the order PAD_BUTTONS names them. */
enum {
    PB_A, PB_B, PB_X, PB_Y, PB_LSHOULDER, PB_RSHOULDER, PB_START, PB_BACK,
    PB_LTHUMB, PB_RTHUMB, PB_DPAD_UP, PB_DPAD_DOWN, PB_DPAD_LEFT, PB_DPAD_RIGHT,
    PB_COUNT
};

static const char *const PAD_BUTTONS[PB_COUNT] = {
    "a", "b", "x", "y", "lshoulder", "rshoulder", "start", "back",
    "lthumb", "rthumb", "dpad_up", "dpad_down", "dpad_left", "dpad_right"
};

typedef struct {
    unsigned char kind;
    short arg;              /* key: virtual-key code; pad: button/axis index */
    signed char sign;       /* axis only */
} Source;

typedef struct {
    unsigned char count;
    Source src[MAX_SOURCES];
} Binding;

enum { DEV_NONE, DEV_KEYBOARD, DEV_PAD };

/* How mouse motion becomes a stick. The file's "mouse_*" keys, per port. */
typedef struct {
    double sensitivity;     /* mouse_sensitivity: 1 = MOUSE_FULL_SPEED is full */
    int deadzone;           /* mouse_deadzone: the title's own stick deadzone, in
                             * stick units; any motion starts just past it */
    double exponent;        /* mouse_exponent: response curve, 1 = linear */
    double y_scale;         /* mouse_y_scale: up and down against left and right.
                             * Console games look up and down slower than they
                             * turn (Future Perfect: 0.47 times as fast at the
                             * same stick), which a mouse feels as two
                             * sensitivities; this evens them out. */
    int invert_y;           /* mouse_invert_y */
    int zoom_match;         /* mouse_zoom_match: for a title that aims by the
                             * mouse itself, slower when zoomed in, by the zoom */
    int window_ms;          /* mouse_window_ms: the speed is averaged over this */
} MouseFeel;

typedef struct {
    int device;
    int pad;                /* recomp_pad slot when device == DEV_PAD */
    int deadzone;
    int has_key;            /* any key: source, so the port works with no pad */
    int has_mouse;          /* any mouse: source */
    unsigned hold_mask;     /* "hold": controls whose keys and mouse buttons
                             * hold a toggle on rather than flip it */
    MouseFeel mouse;
    Binding bind[CONTROL_COUNT];
    char label[48];
} Controller;

static int pad_of(const Controller *c, int port);

static Controller g_ctl[PORTS];
/* Where a load builds the ports before they replace g_ctl in one copy:
 * a title polling its pads during a reload (the in-game menu rebinding a
 * key) must never see a port half-parsed, or for a moment with no device,
 * which it would take for a pad pulled out. */
static Controller g_next[PORTS];
static char g_path[520];
static int g_have_path;
static int g_ready;

/* ---- names -> codes ---------------------------------------------------- */

/* Virtual-key names the config file may use. Single letters and digits are
 * their own code on Windows and are handled without a table entry; "key:#13"
 * passes a raw decimal code through for anything named nowhere. */
static const struct { const char *name; int vk; } KEYS[] = {
    { "UP", 0x26 }, { "DOWN", 0x28 }, { "LEFT", 0x25 }, { "RIGHT", 0x27 },
    { "RETURN", 0x0D }, { "ENTER", 0x0D }, { "BACK", 0x08 },
    { "BACKSPACE", 0x08 }, { "SPACE", 0x20 }, { "TAB", 0x09 },
    { "ESCAPE", 0x1B }, { "SHIFT", 0x10 }, { "LSHIFT", 0xA0 },
    { "RSHIFT", 0xA1 }, { "CTRL", 0x11 }, { "LCTRL", 0xA2 }, { "RCTRL", 0xA3 },
    { "ALT", 0x12 }, { "LALT", 0xA4 }, { "RALT", 0xA5 },
    { "CAPSLOCK", 0x14 }, { "INSERT", 0x2D }, { "DELETE", 0x2E },
    { "HOME", 0x24 }, { "END", 0x23 }, { "PAGEUP", 0x21 }, { "PAGEDOWN", 0x22 },
    { "F1", 0x70 }, { "F2", 0x71 }, { "F3", 0x72 }, { "F4", 0x73 },
    { "F5", 0x74 }, { "F6", 0x75 }, { "F7", 0x76 }, { "F8", 0x77 },
    { "F9", 0x78 }, { "F10", 0x79 }, { "F11", 0x7A }, { "F12", 0x7B },
    { "NUMPAD0", 0x60 }, { "NUMPAD1", 0x61 }, { "NUMPAD2", 0x62 },
    { "NUMPAD3", 0x63 }, { "NUMPAD4", 0x64 }, { "NUMPAD5", 0x65 },
    { "NUMPAD6", 0x66 }, { "NUMPAD7", 0x67 }, { "NUMPAD8", 0x68 },
    { "NUMPAD9", 0x69 }, { "MULTIPLY", 0x6A }, { "ADD", 0x6B },
    { "SUBTRACT", 0x6D }, { "DECIMAL", 0x6E }, { "DIVIDE", 0x6F },
    { "SEMICOLON", 0xBA }, { "EQUALS", 0xBB }, { "COMMA", 0xBC },
    { "MINUS", 0xBD }, { "PERIOD", 0xBE }, { "SLASH", 0xBF },
    { "GRAVE", 0xC0 }, { "LBRACKET", 0xDB }, { "BACKSLASH", 0xDC },
    { "RBRACKET", 0xDD }, { "QUOTE", 0xDE },
};

static int control_index(const char *name)
{
    int i;
    for (i = 0; i < CONTROL_COUNT; i++)
        if (strcmp(CONTROLS[i].name, name) == 0)
            return i;
    return -1;
}

static int key_code(const char *name)
{
    size_t i;

    if (name[0] == '#')
        return (int)strtol(name + 1, NULL, 10);
    if (name[1] == '\0') {
        char c = name[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            return (int)(unsigned char)c;
        return 0;
    }
    for (i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++)
        if (strcmp(KEYS[i].name, name) == 0)
            return KEYS[i].vk;
    return 0;
}

/* "pad:lx-", "key:RETURN", "none" -> a Source. Returns 0 on a name this
 * build does not know, which is reported once rather than per control. */
static int parse_source(const char *text, Source *out)
{
    memset(out, 0, sizeof *out);
    if (!text || !*text || strcmp(text, "none") == 0)
        return 1;                                     /* deliberately unbound */
    if (strncmp(text, "key:", 4) == 0) {
        int vk = key_code(text + 4);
        if (!vk)
            return 0;
        out->kind = SRC_KEY;
        out->arg = (short)vk;
        return 1;
    }
    if (strncmp(text, "pad:", 4) == 0) {
        const char *name = text + 4;
        int i;

        if (strcmp(name, "lt") == 0 || strcmp(name, "rt") == 0) {
            out->kind = SRC_PAD_TRIGGER;
            out->arg = (short)(name[0] == 'r');
            return 1;
        }
        for (i = 0; i < PB_COUNT; i++)
            if (strcmp(PAD_BUTTONS[i], name) == 0) {
                out->kind = SRC_PAD_BUTTON;
                out->arg = (short)i;
                return 1;
            }
        /* half axes: lx+ ly- rx+ ry- */
        if (strlen(name) == 3 && (name[2] == '+' || name[2] == '-') &&
            (name[0] == 'l' || name[0] == 'r') &&
            (name[1] == 'x' || name[1] == 'y')) {
            out->kind = SRC_PAD_AXIS;
            out->arg = (short)((name[0] == 'r' ? 2 : 0) + (name[1] == 'y'));
            out->sign = (signed char)(name[2] == '+' ? 1 : -1);
            return 1;
        }
    }
    if (strncmp(text, "mouse:", 6) == 0) {
        int i;
        for (i = 0; i < MS_COUNT; i++)
            if (strcmp(MOUSE_SOURCES[i], text + 6) == 0)
                break;
        if (i <= MS_X2) {
            out->kind = SRC_MOUSE_BUTTON;
            out->arg = (short)(1 << (i - MS_LEFT));
        } else if (i <= MS_WHEEL_DOWN) {
            out->kind = SRC_MOUSE_WHEEL;
            out->arg = (short)(i - MS_WHEEL_UP);
        } else if (i < MS_COUNT) {
            out->kind = SRC_MOUSE_AXIS;
            out->arg = (short)(i >= MS_YPOS);                /* 0 x, 1 y */
            out->sign = (signed char)((i == MS_XPOS || i == MS_YPOS) ? 1 : -1);
        } else {
            return 0;
        }
        return 1;
    }
    return 0;
}

static void bind_add(Binding *b, const char *text, int *bad)
{
    Source s;

    if (b->count >= MAX_SOURCES)
        return;
    if (!parse_source(text, &s)) {
        (*bad)++;
        return;
    }
    if (s.kind == SRC_NONE)
        return;
    b->src[b->count++] = s;
}

/* ---- defaults ---------------------------------------------------------- */

static void defaults_for(Controller *c, int port)
{
    int i, bad = 0;

    memset(c, 0, sizeof *c);
    c->device = DEV_PAD;
    c->pad = port;
    c->deadzone = DEADZONE_DEFAULT;
    c->mouse.sensitivity = 1.0;
    c->mouse.exponent = 1.0;
    c->mouse.y_scale = 1.0;
    c->mouse.window_ms = MOUSE_WINDOW_DEFAULT;
    c->mouse.zoom_match = 1;
    for (i = 0; i < (int)(sizeof DEFAULTS / sizeof DEFAULTS[0]); i++) {
        int k = control_index(DEFAULTS[i].control);
        if (k < 0)
            continue;
        bind_add(&c->bind[k], DEFAULTS[i].pad, &bad);
        if (port == 0 && DEFAULTS[i].key)
            bind_add(&c->bind[k], DEFAULTS[i].key, &bad);
    }
    c->has_key = (port == 0);
}

/* ---- a small JSON reader ----------------------------------------------- */

static const char *ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return p;
}

/* p is at the opening quote. Copies at most n-1 characters; the escapes a
 * config file can contain are handled and \u is left as written. */
static const char *jstring(const char *p, char *out, size_t n)
{
    size_t i = 0;

    if (*p != '"')
        return NULL;
    p++;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            char e = *p++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            default:  c = e;    break;
            }
        }
        if (i + 1 < n)
            out[i++] = c;
    }
    if (*p != '"')
        return NULL;
    out[i] = '\0';
    return p + 1;
}

static const char *jskip(const char *p)
{
    p = ws(p);
    if (*p == '"') {
        char discard[8];
        const char *q = p;
        /* jstring with a tiny buffer still walks the whole string. */
        return jstring(q, discard, sizeof discard);
    }
    if (*p == '{' || *p == '[') {
        char open = *p, close = (char)(open == '{' ? '}' : ']');
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                char discard[8];
                const char *q = jstring(p, discard, sizeof discard);
                if (!q)
                    return NULL;
                p = q;
                continue;
            }
            if (*p == open)
                depth++;
            else if (*p == close && --depth == 0)
                return p + 1;
            p++;
        }
        return NULL;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']')
        p++;
    return p;
}

/* { "a": "pad:a", "b": ["pad:b", "key:X"] } */
static const char *parse_bindings(const char *p, Controller *c, int *bad)
{
    char key[48], value[64];

    p = ws(p);
    if (*p != '{')
        return NULL;
    p = ws(p + 1);
    if (*p == '}')
        return p + 1;
    for (;;) {
        int k;

        p = jstring(p, key, sizeof key);
        if (!p)
            return NULL;
        p = ws(p);
        if (*p != ':')
            return NULL;
        p = ws(p + 1);
        k = control_index(key);
        if (k >= 0)
            c->bind[k].count = 0;      /* a named control replaces its default */
        if (*p == '[') {
            p = ws(p + 1);
            while (*p && *p != ']') {
                p = jstring(p, value, sizeof value);
                if (!p)
                    return NULL;
                if (k >= 0)
                    bind_add(&c->bind[k], value, bad);
                p = ws(p);
                if (*p == ',')
                    p = ws(p + 1);
            }
            if (*p != ']')
                return NULL;
            p++;
        } else if (*p == '"') {
            p = jstring(p, value, sizeof value);
            if (!p)
                return NULL;
            if (k >= 0)
                bind_add(&c->bind[k], value, bad);
        } else {
            p = jskip(p);                        /* null, or something odd */
            if (!p)
                return NULL;
        }
        if (k < 0)
            (*bad)++;
        p = ws(p);
        if (*p == ',') {
            p = ws(p + 1);
            continue;
        }
        if (*p != '}')
            return NULL;
        return p + 1;
    }
}

/* "hold": ["rthumb", "lthumb"] -- see hold_pulse. */
static const char *parse_hold(const char *p, Controller *c)
{
    char value[48];

    p = ws(p);
    if (*p != '[')
        return jskip(p);
    p = ws(p + 1);
    while (*p && *p != ']') {
        int k;

        p = jstring(p, value, sizeof value);
        if (!p)
            return NULL;
        k = control_index(value);
        if (k >= 0)
            c->hold_mask |= 1u << k;
        p = ws(p);
        if (*p == ',')
            p = ws(p + 1);
    }
    if (*p != ']')
        return NULL;
    return p + 1;
}

static void set_device(Controller *c, const char *name)
{
    if (strcmp(name, "keyboard") == 0) {
        c->device = DEV_KEYBOARD;
        c->pad = -1;
    } else if (strcmp(name, "none") == 0 || !*name) {
        c->device = DEV_NONE;
        c->pad = -1;
    } else if (strncmp(name, "gamepad:", 8) == 0 || strncmp(name, "xinput:", 7) == 0) {
        /* "gamepad:N" is the N-th pad through whichever API reads pads;
         * "xinput:N" is the name files written before SDL3 used, and means
         * the same slot. */
        int n = (int)strtol(strchr(name, ':') + 1, NULL, 10);
        c->device = DEV_PAD;
        c->pad = (n >= 0 && n < PORTS) ? n : 0;
    }
}

/* One element of "controllers". Starts from the defaults for whichever port
 * it names, so a file that lists two bindings is a two-binding change. */
static const char *parse_controller(const char *p, int index, int *bad)
{
    Controller tmp;
    char key[48], value[64];
    int port = index, seen_port = 0, i;

    defaults_for(&tmp, index < PORTS ? index : 0);
    p = ws(p);
    if (*p != '{')
        return NULL;
    p = ws(p + 1);
    while (*p && *p != '}') {
        p = jstring(p, key, sizeof key);
        if (!p)
            return NULL;
        p = ws(p);
        if (*p != ':')
            return NULL;
        p = ws(p + 1);
        if (strcmp(key, "port") == 0) {
            port = (int)strtol(p, NULL, 10) - 1;       /* the file is 1-based */
            seen_port = 1;
            p = jskip(p);
        } else if (strcmp(key, "device") == 0) {
            p = jstring(p, value, sizeof value);
            if (!p)
                return NULL;
            set_device(&tmp, value);
        } else if (strcmp(key, "deadzone") == 0) {
            tmp.deadzone = (int)strtol(p, NULL, 10);
            p = jskip(p);
        } else if (strcmp(key, "mouse_sensitivity") == 0) {
            double v = strtod(p, NULL);
            if (v > 0.0 && v <= 100.0)
                tmp.mouse.sensitivity = v;
            p = jskip(p);
        } else if (strcmp(key, "mouse_deadzone") == 0) {
            long v = strtol(p, NULL, 10);
            if (v >= 0 && v < 32767)
                tmp.mouse.deadzone = (int)v;
            p = jskip(p);
        } else if (strcmp(key, "mouse_exponent") == 0) {
            double v = strtod(p, NULL);
            if (v >= 0.1 && v <= 10.0)
                tmp.mouse.exponent = v;
            p = jskip(p);
        } else if (strcmp(key, "mouse_y_scale") == 0) {
            double v = strtod(p, NULL);
            if (v >= 0.1 && v <= 10.0)
                tmp.mouse.y_scale = v;
            p = jskip(p);
        } else if (strcmp(key, "mouse_invert_y") == 0) {
            tmp.mouse.invert_y = strncmp(p, "true", 4) == 0 || *p == '1';
            p = jskip(p);
        } else if (strcmp(key, "mouse_zoom_match") == 0) {
            tmp.mouse.zoom_match = strncmp(p, "true", 4) == 0 || *p == '1';
            p = jskip(p);
        } else if (strcmp(key, "mouse_window_ms") == 0) {
            long v = strtol(p, NULL, 10);
            if (v >= 1 && v <= 250)
                tmp.mouse.window_ms = (int)v;
            p = jskip(p);
        } else if (strcmp(key, "bindings") == 0) {
            p = parse_bindings(p, &tmp, bad);
            if (!p)
                return NULL;
        } else if (strcmp(key, "hold") == 0) {
            p = parse_hold(p, &tmp);
            if (!p)
                return NULL;
        } else {
            p = jskip(p);
            if (!p)
                return NULL;
        }
        p = ws(p);
        if (*p == ',')
            p = ws(p + 1);
    }
    if (*p != '}')
        return NULL;
    if (port < 0 || port >= PORTS)
        return p + 1;                             /* a port nothing can read */
    /* "port" may come after "bindings", so the defaults this started from
     * could be the wrong port's. Only the pad index differs, and only when
     * the file did not say. */
    if (seen_port && tmp.device == DEV_PAD && tmp.pad == index && index != port)
        tmp.pad = port;
    tmp.has_key = 0;
    tmp.has_mouse = 0;
    for (i = 0; i < CONTROL_COUNT; i++) {
        int s;
        for (s = 0; s < tmp.bind[i].count; s++) {
            int kind = tmp.bind[i].src[s].kind;
            if (kind == SRC_KEY)
                tmp.has_key = 1;
            if (kind == SRC_MOUSE_BUTTON || kind == SRC_MOUSE_WHEEL || kind == SRC_MOUSE_AXIS)
                tmp.has_mouse = 1;
        }
    }
    g_next[port] = tmp;
    return p + 1;
}

static int parse_config(const char *text)
{
    const char *p = ws(text);
    char key[48];
    int bad = 0, index = 0;

    if (*p != '{')
        return 0;
    p = ws(p + 1);
    while (*p && *p != '}') {
        p = jstring(p, key, sizeof key);
        if (!p)
            return 0;
        p = ws(p);
        if (*p != ':')
            return 0;
        p = ws(p + 1);
        if (strcmp(key, "pad_api") == 0 && *p == '"') {
            char api[16];
            p = jstring(p, api, sizeof api);
            if (!p)
                return 0;
            recomp_pad_set_api(strcmp(api, "xinput") == 0 ? RECOMP_PAD_API_XINPUT
                                                          : RECOMP_PAD_API_SDL);
        } else if (strcmp(key, "controllers") == 0 && *p == '[') {
            p = ws(p + 1);
            while (*p && *p != ']') {
                p = parse_controller(p, index++, &bad);
                if (!p)
                    return 0;
                p = ws(p);
                if (*p == ',')
                    p = ws(p + 1);
            }
            if (*p != ']')
                return 0;
            p++;
        } else {
            p = jskip(p);
            if (!p)
                return 0;
        }
        p = ws(p);
        if (*p == ',')
            p = ws(p + 1);
    }
    if (bad)
        fprintf(stderr, "[INPUT] %d binding(s) in the config file name a "
                        "control or a source this build does not know; "
                        "they are ignored\n", bad);
    return 1;
}

/* ---- where the config lives -------------------------------------------- */

static int readable(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

/* RECOMP_INPUT_CONFIG; this title's own file, which the launcher writes; the
 * per-user file every title shares; a preset beside the executable. The
 * order is recomp_bindings_read_path's (src/config), so the launcher edits
 * the file this reads. */
static int find_config(char *out, size_t n)
{
    const char *env = getenv("RECOMP_INPUT_CONFIG");

    if (!recomp_bindings_read_path(recomp_config_title_id(), out, n))
        return 0;
    if (env && *env && !readable(out)) {
        fprintf(stderr, "[INPUT] RECOMP_INPUT_CONFIG=%s cannot be read; "
                        "using the built-in bindings\n", env);
        return 0;
    }
    return 1;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long size;
    char *buf;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || size > (1 << 20)) {
        fclose(f);
        return NULL;
    }
    buf = (char *)malloc((size_t)size + 1);
    if (buf) {
        size_t got = fread(buf, 1, (size_t)size, f);
        buf[got] = '\0';
    }
    fclose(f);
    return buf;
}

static void describe(Controller *c, char *out, size_t n)
{
    const char *extra = c->has_mouse ? (c->has_key ? " + keyboard and mouse" : " + mouse")
                                     : (c->has_key ? " + keyboard" : "");
    if (c->device == DEV_KEYBOARD)
        snprintf(out, n, "keyboard%s", c->has_mouse ? " and mouse" : "");
    else if (c->device == DEV_PAD)
        snprintf(out, n, "%s pad %d%s", recomp_pad_api_name(), c->pad + 1, extra);
    else
        snprintf(out, n, "nothing");
}

static void load_bindings(void)
{
    char *text;
    int i;

    g_have_path = 0;
    for (i = 0; i < PORTS; i++)
        defaults_for(&g_next[i], i);
    if (find_config(g_path, sizeof g_path) && (text = read_file(g_path)) != NULL) {
        if (parse_config(text)) {
            g_have_path = 1;
        } else {
            fprintf(stderr, "[INPUT] %s is not a binding config this build "
                            "can read; using the built-in bindings\n", g_path);
            for (i = 0; i < PORTS; i++)
                defaults_for(&g_next[i], i);
        }
        free(text);
    }
    for (i = 0; i < PORTS; i++)
        describe(&g_next[i], g_next[i].label, sizeof g_next[i].label);
    memcpy(g_ctl, g_next, sizeof g_ctl);
    fprintf(stderr, "[INPUT] bindings from %s\n",
            g_have_path ? g_path : "the built-in defaults (no config file)");
    for (i = 0; i < PORTS; i++)
        fprintf(stderr, "[INPUT]   controller %d: %s\n", i + 1, g_ctl[i].label);
    {
        int wanted = 0;
        for (i = 0; i < PORTS; i++) {
            const MouseFeel *m = &g_ctl[i].mouse;
            if (!g_ctl[i].has_mouse || g_ctl[i].device == DEV_NONE)
                continue;
            wanted = 1;
            fprintf(stderr, "[INPUT]   controller %d mouse: sensitivity %.2f (up and down "
                            "x%.2f), deadzone %d, exponent %.2f, %s, averaged over %d ms\n",
                    i + 1, m->sensitivity, m->y_scale, m->deadzone, m->exponent,
                    m->invert_y ? "inverted" : "not inverted", m->window_ms);
        }
        recomp_mouse_set_wanted(wanted);
    }
    fflush(stderr);
}

/* Counts loads, so whatever shows a binding (button prompts) knows to look
 * again. */
static volatile unsigned g_bind_generation;

void recomp_bindings_init(void)
{
    if (g_ready)
        return;
    g_ready = 1;
    load_bindings();
    g_bind_generation++;
}

void recomp_bindings_reload(void)
{
    if (!g_ready) {
        recomp_bindings_init();
        return;
    }
    load_bindings();
    g_bind_generation++;
}

unsigned recomp_bindings_generation(void)
{
    recomp_bindings_init();
    return g_bind_generation;
}

/* A key's name as a prompt shows it: short, upper case, the names a keycap
 * carries. UTF-8 (the arrows). */
static void key_prompt_name(int vk, char *out, size_t n)
{
    static const struct { int vk; const char *name; } NAMES[] = {
        { 0x0D, "ENTER" }, { 0x08, "BKSP" }, { 0x20, "SPACE" }, { 0x09, "TAB" },
        { 0x1B, "ESC" }, { 0x10, "SHIFT" }, { 0xA0, "SHIFT" }, { 0xA1, "RSHIFT" },
        { 0x11, "CTRL" }, { 0xA2, "CTRL" }, { 0xA3, "RCTRL" }, { 0x12, "ALT" },
        { 0xA4, "ALT" }, { 0xA5, "RALT" }, { 0x14, "CAPS" }, { 0x2D, "INS" },
        { 0x2E, "DEL" }, { 0x24, "HOME" }, { 0x23, "END" }, { 0x21, "PGUP" },
        { 0x22, "PGDN" }, { 0x26, "\xE2\x86\x91" }, { 0x28, "\xE2\x86\x93" },
        { 0x25, "\xE2\x86\x90" }, { 0x27, "\xE2\x86\x92" },
    };
    size_t i;

    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        snprintf(out, n, "%c", vk);
        return;
    }
    for (i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++)
        if (NAMES[i].vk == vk) {
            snprintf(out, n, "%s", NAMES[i].name);
            return;
        }
    for (i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++)
        if (KEYS[i].vk == vk) {
            snprintf(out, n, "%s", KEYS[i].name);
            return;
        }
    snprintf(out, n, "#%d", vk);
}

int recomp_bindings_prompt_label(unsigned port, const char *control, char *out, size_t n)
{
    int k = control ? control_index(control) : -1, s;

    recomp_bindings_init();
    if (port >= PORTS || k < 0 || !out || !n)
        return 0;
    for (s = 0; s < g_ctl[port].bind[k].count; s++) {
        const Source *src = &g_ctl[port].bind[k].src[s];

        switch (src->kind) {
        case SRC_KEY:
            key_prompt_name(src->arg, out, n);
            return 1;
        case SRC_MOUSE_BUTTON:
            snprintf(out, n, "%s", src->arg == 1 ? "LMB" : src->arg == 2 ? "RMB"
                                 : src->arg == 4 ? "MMB" : src->arg == 8 ? "MB4" : "MB5");
            return 1;
        case SRC_MOUSE_WHEEL:
            snprintf(out, n, "%s", src->arg == 0 ? "WHEEL\xE2\x86\x91" : "WHEEL\xE2\x86\x93");
            return 1;
        default:
            break;
        }
    }
    return 0;
}

/* Which the player last used, the keyboard and mouse (1) or a pad (0), from
 * the sources that pressed something: a key or a click on one side; a pad
 * button, a trigger, or a stick pushed well past any drift on the other. */
static volatile int g_last_keyboard = 1;

int recomp_bindings_last_input_keyboard(void)
{
    return g_last_keyboard;
}

const char *recomp_bindings_source_path(void)
{
    recomp_bindings_init();
    return g_have_path ? g_path : NULL;
}

const char *recomp_bindings_device_name(unsigned port)
{
    recomp_bindings_init();
    return port < PORTS ? g_ctl[port].label : "nothing";
}

/* ---- reading the host -------------------------------------------------- */

static unsigned long long now_ms(void)
{
#if defined(_WIN32)
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
#endif
}

static const unsigned short PAD_BITS[PB_COUNT] = {
    RECOMP_PAD_A, RECOMP_PAD_B, RECOMP_PAD_X, RECOMP_PAD_Y,
    RECOMP_PAD_LEFT_SHOULDER, RECOMP_PAD_RIGHT_SHOULDER,
    RECOMP_PAD_START, RECOMP_PAD_BACK,
    RECOMP_PAD_LEFT_THUMB, RECOMP_PAD_RIGHT_THUMB,
    RECOMP_PAD_DPAD_UP, RECOMP_PAD_DPAD_DOWN,
    RECOMP_PAD_DPAD_LEFT, RECOMP_PAD_DPAD_RIGHT
};

/* One answer per key per sample: 24 controls can name the same key twice and
 * GetAsyncKeyState is a call into the window manager.
 *
 * Per sample is not enough on its own. A title is free to read the pad far
 * more often than it draws, and each read was costing a window-manager
 * transition per distinct key: Tony Hawk's Pro Skater 2X polls about 372,000
 * times a second and spent 47% of its main thread inside
 * NtUserGetAsyncKeyState -- four times what it spent in its own lifted code.
 *
 * So the answers also persist between samples for a short while. A keyboard
 * cannot change faster than a person can move, the console's own pad read is
 * a USB transfer that returns state captured up to a frame earlier, and one
 * millisecond is below anything a player can perceive. It is deliberately
 * not tied to the frame: a title that polls without drawing still has to get
 * fresh input eventually, or a menu that spins waiting for a key would never
 * see one.
 */
#define KEY_CACHE_TTL_MS 1u

typedef struct { unsigned char state[256]; } KeyCache;

static unsigned char g_key_state[256];
static unsigned long long g_key_state_at;

/* GetAsyncKeyState answers for the whole desktop, not for a window: with no
 * check here, typing into a chat window while the game sat behind it walked
 * the player forward and fired. The keyboard is the game's only while its
 * window has the focus (input_mouse.c tracks it; with no window attached it
 * reads as before). Refreshed with the key cache, so it costs nothing a
 * poll. */
static int g_keys_live = 1;

static int key_down(KeyCache *kc, int vk)
{
    (void)kc;
    if (vk <= 0 || vk > 255 || !g_keys_live)
        return 0;
#if defined(_WIN32)
    if (!g_key_state[vk])
        g_key_state[vk] = (GetAsyncKeyState(vk) & 0x8000) ? 2 : 1;
    /* Windows' own: Alt+Tab, Alt+Enter, Alt+Esc, Alt+Space, Alt+F4. The key
     * goes down while the window still has the focus, and these are keys a
     * game binds -- Future Perfect's Tab is Back, its Enter A, its Esc Start
     * -- so Alt+Tab on its main menu pressed Back and went to Change
     * Players. With Alt held they are Windows', not the game's. */
    if (g_key_state[vk] == 2 &&
        (vk == VK_TAB || vk == VK_RETURN || vk == VK_ESCAPE || vk == VK_SPACE || vk == VK_F4) &&
        key_down(kc, VK_MENU))
        return 0;
    return g_key_state[vk] == 2;
#else
    return 0;           /* no keyboard source off Windows yet */
#endif
}

/* Start of a sample: drop answers older than the window above. */
static void key_cache_tick(void)
{
    unsigned long long now = now_ms();
    if (now - g_key_state_at >= KEY_CACHE_TTL_MS) {
        memset(g_key_state, 0, sizeof g_key_state);
        g_key_state_at = now;
        g_keys_live = recomp_input_focused();
    }
}

/* What the mouse is doing, read once per sample of a port that binds it. */
typedef struct {
    double vx, vy;          /* counts a millisecond; y as the device reports it */
    unsigned buttons;
} MouseSample;

/* One half of one mouse axis as a 0..255 magnitude, the way a key or a stick
 * half reads. Any motion at all lands just past the title's own deadzone,
 * because a title discards a small stick deflection and a slow mouse would
 * otherwise do nothing; from there it rises with speed, through the
 * response curve, to a full stick. */
static int mouse_axis_magnitude(const Source *s, const MouseSample *ms, const MouseFeel *f)
{
    double v, frac;
    int floor_mag;

    /* The title turns its camera by the mouse itself (recomp_mouse_set_direct). */
    if (recomp_mouse_direct())
        return 0;
    if (s->arg == 0)
        v = ms->vx;
    else                                        /* the device's y grows downward */
        v = (f->invert_y ? ms->vy : -ms->vy) * f->y_scale;
    v *= s->sign;
    if (v <= 0.0)
        return 0;
    frac = v * f->sensitivity / MOUSE_FULL_SPEED;
    if (frac > 1.0)
        frac = 1.0;
    if (f->exponent != 1.0)
        frac = pow(frac, f->exponent);
    floor_mag = f->deadzone * 255 / 32767;
    return floor_mag + 1 + (int)(frac * (254 - floor_mag));
}

static int axis_value(const RecompPadState *g, int axis)
{
    switch (axis) {
    case AXIS_LX: return g->lx;
    case AXIS_LY: return g->ly;
    case AXIS_RX: return g->rx;
    default:      return g->ry;
    }
}

/* Every source reads as a 0..255 magnitude, whatever it is: that is what
 * makes a trigger bindable to a key and a button bindable to a stick. */
static int source_magnitude(const Source *s, KeyCache *kc,
                            const RecompPadState *pad, int have_pad, int deadzone,
                            const MouseSample *ms, const MouseFeel *feel)
{
    switch (s->kind) {
    case SRC_KEY:
        return key_down(kc, s->arg) ? 255 : 0;
    case SRC_MOUSE_BUTTON:
        return (ms->buttons & (unsigned)s->arg) ? 255 : 0;
    case SRC_MOUSE_WHEEL:
        return recomp_mouse_wheel(s->arg) ? 255 : 0;
    case SRC_MOUSE_AXIS:
        return mouse_axis_magnitude(s, ms, feel);
    case SRC_PAD_BUTTON:
        if (!have_pad)
            return 0;
        return (pad->buttons & PAD_BITS[s->arg]) ? 255 : 0;
    case SRC_PAD_TRIGGER: {
        /* A resting trigger can read a few units above zero, and a title
         * that treats any non-zero analog button as pressed would fire
         * forever. Below XInput's own recommended threshold is "not
         * pressed"; above it the value passes through untouched. */
        int t;
        if (!have_pad)
            return 0;
        t = s->arg ? pad->right_trigger : pad->left_trigger;
        return t < TRIGGER_THRESHOLD ? 0 : t;
    }
    case SRC_PAD_AXIS: {
        int v, span;
        if (!have_pad)
            return 0;
        v = axis_value(pad, s->arg) * s->sign;
        if (v <= deadzone)
            return 0;
        if (v > 32767)
            v = 32767;
        span = 32767 - deadzone;
        if (span <= 0)
            return 255;
        return (v - deadzone) * 255 / span;
    }
    default:
        return 0;
    }
}

/* The first thing pressed on a port, named once. A binding that is wrong is
 * otherwise invisible: the title just never responds, and there is no way to
 * tell a mis-bound key from a title that is not reading the pad at all. */
static void say_first_press(unsigned port, const char *control)
{
    static int said[PORTS];

    if (port < PORTS && !said[port]) {
        char name[64];
        said[port] = 1;
        name[0] = '\0';
        if (pad_of(&g_ctl[port], (int)port) >= 0)
            recomp_pad_name(pad_of(&g_ctl[port], (int)port), name, sizeof name);
        fprintf(stderr, "[INPUT] port %u first press: %s (%s%s%s)\n", port + 1,
                control, g_ctl[port].label, name[0] ? ": " : "", name);
        fflush(stderr);
    }
}

/* ---- hold, for a control the title toggles ------------------------------
 *
 * A console title toggles some things on a press -- Future Perfect's aim
 * mode is a click of the right stick -- where a PC player expects to hold
 * a button for as long as they want it. For a control in the file's "hold"
 * list, and only while the title says how the toggle stands
 * (recomp_bindings_toggle_reader), a key or mouse button bound to it is
 * read as "on while held": whenever held and the title's state disagree,
 * one short press is sent, then a wait for the state to follow. A press the
 * title ignores (it cannot aim just now) is tried again a few times, then
 * left until the button changes. Pad buttons keep the console's toggle. */
#define HOLD_PRESS_MS 50
#define HOLD_WAIT_MS  150
#define HOLD_TRIES    3

static int (*g_toggle_reader[CONTROL_COUNT])(unsigned port);

typedef struct {
    int phase;              /* 0 idle, 1 pressing, 2 waiting for the title */
    unsigned long long until;
    int tries, last_held;
} HoldState;

static HoldState g_hold[PORTS][CONTROL_COUNT];

/* recomp_bindings_press: until when each control reads as held. */
static volatile unsigned long long g_press_until[PORTS][CONTROL_COUNT];

void recomp_bindings_press(unsigned port, const char *control, unsigned ms)
{
    int k = control ? control_index(control) : -1;

    recomp_bindings_init();
    if (port >= PORTS || k < 0)
        return;
    g_press_until[port][k] = now_ms() + ms;
}

/* Pausing when the window loses the focus (pause_unfocused): the title's
 * pause control, whether a port is in play, the setting (-1 until read),
 * and each port's focus at its last sample. */
static int g_pause_control = -1;
static int (*g_pause_in_play)(unsigned port);
static volatile int g_pause_setting = -1;
static int g_pause_was_active[PORTS];

void recomp_bindings_pause_on_focus_loss(const char *control, int (*in_play)(unsigned port))
{
    g_pause_control = control ? control_index(control) : -1;
    g_pause_in_play = in_play;
}

void recomp_bindings_set_pause_unfocused(int on)
{
    g_pause_setting = on ? 1 : 0;
}

int recomp_bindings_pause_unfocused(void)
{
    if (g_pause_setting < 0)
        g_pause_setting = recomp_config_bool("RECOMP_PAUSE_UNFOCUSED", "pause_unfocused", 1);
    return g_pause_setting != 0;
}

/* keyboard_alone: the keyboard and mouse are player 1 on their own, and the
 * pads start at player 2 -- a friend on Steam Remote Play or Parsec, whose
 * pad is this machine's first, joins as player 2 rather than steering
 * player 1 beside the keyboard. -1 until read. */
static volatile int g_kb_alone = -1;

void recomp_bindings_set_keyboard_alone(int on)
{
    g_kb_alone = on ? 1 : 0;
}

int recomp_bindings_keyboard_alone(void)
{
    if (g_kb_alone < 0)
        g_kb_alone = recomp_config_bool("RECOMP_KEYBOARD_ALONE", "keyboard_alone", 0);
    return g_kb_alone != 0;
}

/* The pad slot a port reads, or -1: its own, unless keyboard_alone moves the
 * pads along by one -- player 1 (with keys or the mouse bound) reads none,
 * and each later player the pad before the one it names. A port the file
 * gave the first pad keeps it. */
static int pad_of(const Controller *c, int port)
{
    if (c->device != DEV_PAD)
        return -1;
    if (!recomp_bindings_keyboard_alone())
        return c->pad;
    if (port == 0)
        return (c->has_key || c->has_mouse) ? -1 : c->pad;
    return c->pad > 0 ? c->pad - 1 : c->pad;
}

int recomp_bindings_pause_available(void)
{
    return g_pause_control >= 0 && g_pause_in_play != NULL;
}

/* At each sample: the window has just lost the focus, the player is in
 * play, so the title's pause is pressed for it -- Alt+Tab in a fight comes
 * back to the pause menu, not to the fight's end. Only in play: pressed in
 * the pause menu, the same control would resume. */
static void follow_focus_pause(unsigned port)
{
    int active = recomp_input_window_active() ? 1 : 2, was = g_pause_was_active[port];

    g_pause_was_active[port] = active;
    if (was != 1 || active != 2 || !recomp_bindings_pause_available() ||
        !recomp_bindings_pause_unfocused() || g_pause_in_play(port) != 1)
        return;
    g_press_until[port][g_pause_control] = now_ms() + 120;
    fprintf(stderr, "[INPUT] the window lost the focus in play: paused (pause_unfocused)\n");
    fflush(stderr);
}

void recomp_bindings_toggle_reader(const char *control, int (*is_on)(unsigned port))
{
    int k = control ? control_index(control) : -1;

    if (k >= 0)
        g_toggle_reader[k] = is_on;
}

int recomp_bindings_hold_available(const char *control)
{
    int k = control ? control_index(control) : -1;

    return k >= 0 && g_toggle_reader[k] != NULL;
}

int recomp_bindings_get_hold(unsigned port, const char *control)
{
    int k = control ? control_index(control) : -1;

    recomp_bindings_init();
    return port < PORTS && k >= 0 && ((g_ctl[port].hold_mask >> k) & 1u);
}

void recomp_bindings_set_hold(unsigned port, const char *control, int on)
{
    int k = control ? control_index(control) : -1;

    recomp_bindings_init();
    if (port >= PORTS || k < 0)
        return;
    if (on)
        g_ctl[port].hold_mask |= 1u << k;
    else
        g_ctl[port].hold_mask &= ~(1u << k);
}

/* The pulse to send this sample, 0 or 255. */
static int hold_pulse(unsigned port, int k, int held)
{
    HoldState *h = &g_hold[port][k];
    unsigned long long now = now_ms();
    int on;

    if (held != h->last_held) {
        h->last_held = held;
        h->tries = 0;
    }
    if (h->phase == 1) {
        if (now < h->until)
            return 255;
        h->phase = 2;
        h->until = now + HOLD_WAIT_MS;
        return 0;
    }
    on = g_toggle_reader[k](port);
    if (on < 0) {                   /* the title has nothing to toggle now */
        h->phase = 0;
        h->tries = 0;
        return 0;
    }
    if ((on != 0) == held) {
        h->phase = 0;
        h->tries = 0;
        return 0;
    }
    if (h->phase == 2 && now < h->until)
        return 0;
    h->phase = 0;
    if (h->tries >= HOLD_TRIES)
        return 0;
    h->tries++;
    h->phase = 1;
    h->until = now + HOLD_PRESS_MS;
    return 255;
}

int recomp_bindings_sample(unsigned port, XBOX_GAMEPAD *out)
{
    Controller *c;
    KeyCache kc;
    RecompPadState pad;
    MouseSample ms;
    int have_pad = 0, i;
    int axis[4] = { 0, 0, 0, 0 };

    if (!out)
        return 0;
    recomp_bindings_init();
    memset(out, 0, sizeof *out);
    if (port >= PORTS)
        return 0;
    c = &g_ctl[port];
    if (c->device == DEV_NONE)
        return 0;
    /* The in-game menu has the input: the pad is there, and untouched. */
    if (recomp_input_suspended())
        return 1;
    follow_focus_pause(port);
    memset(&kc, 0, sizeof kc);
    key_cache_tick();
    memset(&pad, 0, sizeof pad);
    if (pad_of(c, (int)port) >= 0)
        have_pad = recomp_pad_read(pad_of(c, (int)port), &pad);
    memset(&ms, 0, sizeof ms);
    if (c->has_mouse) {
        recomp_mouse_velocity((unsigned)c->mouse.window_ms, &ms.vx, &ms.vy);
        ms.buttons = recomp_mouse_buttons();
    }

    for (i = 0; i < CONTROL_COUNT; i++) {
        int mag = 0, s;
        int hold = ((c->hold_mask >> i) & 1u) && g_toggle_reader[i], held = 0;

        for (s = 0; s < c->bind[i].count; s++) {
            const Source *src = &c->bind[i].src[s];
            int m = source_magnitude(src, &kc, &pad, have_pad,
                                     c->deadzone, &ms, &c->mouse);
            if (m >= 64) {
                if (src->kind == SRC_KEY || src->kind == SRC_MOUSE_BUTTON ||
                    src->kind == SRC_MOUSE_WHEEL)
                    g_last_keyboard = 1;
                else if (src->kind == SRC_PAD_BUTTON || src->kind == SRC_PAD_TRIGGER ||
                         (src->kind == SRC_PAD_AXIS && m >= 160))
                    g_last_keyboard = 0;
            }
            if (hold && (src->kind == SRC_KEY || src->kind == SRC_MOUSE_BUTTON ||
                         src->kind == SRC_MOUSE_WHEEL || src->kind == SRC_MOUSE_AXIS)) {
                if (m >= 64)
                    held = 1;
                continue;
            }
            if (m > mag)
                mag = m;
        }
        if (hold) {
            int pulse = hold_pulse(port, i, held);
            if (pulse > mag)
                mag = pulse;
        }
        if (g_press_until[port][i] && now_ms() < g_press_until[port][i])
            mag = 255;                  /* recomp_bindings_press */
        if (!mag)
            continue;
        say_first_press(port, CONTROLS[i].name);
        switch (CONTROLS[i].kind) {
        case K_DIGITAL:
            if (mag >= 64)
                out->wButtons |= CONTROLS[i].arg;
            break;
        case K_ANALOG:
            if (mag > out->bAnalogButtons[CONTROLS[i].arg])
                out->bAnalogButtons[CONTROLS[i].arg] = (BYTE)mag;
            break;
        default:
            axis[CONTROLS[i].arg] += CONTROLS[i].sign * mag;
            break;
        }
    }
    for (i = 0; i < 4; i++) {
        long v;
        if (axis[i] > 255)  axis[i] = 255;
        if (axis[i] < -255) axis[i] = -255;
        v = (long)axis[i] * 32767 / 255;
        switch (i) {
        case AXIS_LX: out->sThumbLX = (SHORT)v; break;
        case AXIS_LY: out->sThumbLY = (SHORT)v; break;
        case AXIS_RX: out->sThumbRX = (SHORT)v; break;
        default:      out->sThumbRY = (SHORT)v; break;
        }
    }
    return 1;
}

unsigned recomp_bindings_present_mask(void)
{
    static unsigned long long next;
    static unsigned mask;
    unsigned long long now;
    int i;

    recomp_bindings_init();
    now = now_ms();
    if (mask && now < next)
        return mask;
    next = now + 250;
    mask = 0;
    for (i = 0; i < PORTS; i++) {
        Controller *c = &g_ctl[i];
        if (c->device == DEV_KEYBOARD ||
            (c->device == DEV_PAD && (c->has_key || c->has_mouse))) {
            mask |= 1u << i;
        } else if (pad_of(c, i) >= 0) {
            RecompPadState st;
            if (recomp_pad_read(pad_of(c, i), &st))
                mask |= 1u << i;
        }
    }
    return mask;
}

int recomp_bindings_mouse_port(void)
{
    int i;

    recomp_bindings_init();
    for (i = 0; i < PORTS; i++)
        if (g_ctl[i].has_mouse && g_ctl[i].device != DEV_NONE)
            return i;
    return -1;
}

int recomp_bindings_get_mouse(unsigned port, RecompMouseSettings *out)
{
    recomp_bindings_init();
    if (port >= PORTS || !out)
        return 0;
    out->sensitivity = g_ctl[port].mouse.sensitivity;
    out->y_scale = g_ctl[port].mouse.y_scale;
    out->invert_y = g_ctl[port].mouse.invert_y;
    out->zoom_match = g_ctl[port].mouse.zoom_match;
    return 1;
}

/* Plain stores of a double and an int, read by the sampler on guest threads:
 * a poll that lands mid-change reads one value old and one new, for one
 * sample, which nobody can feel. */
void recomp_bindings_set_mouse(unsigned port, const RecompMouseSettings *in)
{
    recomp_bindings_init();
    if (port >= PORTS || !in)
        return;
    if (in->sensitivity > 0.0 && in->sensitivity <= 100.0)
        g_ctl[port].mouse.sensitivity = in->sensitivity;
    if (in->y_scale >= 0.1 && in->y_scale <= 10.0)
        g_ctl[port].mouse.y_scale = in->y_scale;
    g_ctl[port].mouse.invert_y = in->invert_y ? 1 : 0;
    g_ctl[port].mouse.zoom_match = in->zoom_match ? 1 : 0;
}

void recomp_bindings_rumble(unsigned port, unsigned short low, unsigned short high)
{
    recomp_bindings_init();
    if (port < PORTS && pad_of(&g_ctl[port], (int)port) >= 0)
        recomp_pad_rumble(pad_of(&g_ctl[port], (int)port), low, high);
}
