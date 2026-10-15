/*
 * launcher_bindings.c -- see launcher_bindings.h.
 *
 * The reader is small and forgiving, like the runtime's: it looks for
 * the keys it knows and steps over everything else, so a file written
 * by the Python tool with sources this does not model is read, kept and
 * written back rather than thrown away.
 */
#include "launcher_bindings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <direct.h>
#  define BIND_MKDIR(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  define BIND_MKDIR(p) mkdir((p), 0755)
#endif

/* The runtime's table, in the runtime's order. Copied rather than
 * shared because the runtime's copy is static to a file that resolves
 * these to indices; the test keeps the two honest. */
const char *const bind_control_names[BIND_CONTROLS] = {
    "a", "b", "x", "y", "black", "white", "start", "back",
    "ltrigger", "rtrigger",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "lstick_up", "lstick_down", "lstick_left", "lstick_right", "lthumb",
    "rstick_up", "rstick_down", "rstick_left", "rstick_right", "rthumb"
};

const char *const bind_control_labels[BIND_CONTROLS] = {
    "A", "B", "X", "Y", "Black", "White", "Start", "Back",
    "Left trigger", "Right trigger",
    "D-pad up", "D-pad down", "D-pad left", "D-pad right",
    "Left stick up", "Left stick down", "Left stick left", "Left stick right",
    "Left stick press",
    "Right stick up", "Right stick down", "Right stick left", "Right stick right",
    "Right stick press"
};

/* The runtime's DEFAULTS table. Sticks have no keyboard default: every
 * comfortable key is already a button. */
static const struct { const char *pad, *key; } k_defaults[BIND_CONTROLS] = {
    { "pad:a",          "key:Z"      },   /* a */
    { "pad:b",          "key:X"      },   /* b */
    { "pad:x",          "key:A"      },   /* x */
    { "pad:y",          "key:S"      },   /* y */
    { "pad:lshoulder",  "key:W"      },   /* black */
    { "pad:rshoulder",  "key:Q"      },   /* white */
    { "pad:start",      "key:RETURN" },   /* start */
    { "pad:back",       "key:BACK"   },   /* back */
    { "pad:lt",         "key:E"      },   /* ltrigger */
    { "pad:rt",         "key:R"      },   /* rtrigger */
    { "pad:dpad_up",    "key:UP"     },
    { "pad:dpad_down",  "key:DOWN"   },
    { "pad:dpad_left",  "key:LEFT"   },
    { "pad:dpad_right", "key:RIGHT"  },
    { "pad:ly+",        ""           },
    { "pad:ly-",        ""           },
    { "pad:lx-",        ""           },
    { "pad:lx+",        ""           },
    { "pad:lthumb",     ""           },
    { "pad:ry+",        ""           },
    { "pad:ry-",        ""           },
    { "pad:rx-",        ""           },
    { "pad:rx+",        ""           },
    { "pad:rthumb",     ""           }
};

void bind_defaults(BindConfig *c)
{
    int p, i;

    memset(c, 0, sizeof *c);
    for (p = 0; p < BIND_PORTS; p++) {
        c->port[p].device   = BIND_DEV_PAD;
        c->port[p].pad      = p;
        c->port[p].deadzone = 7849;          /* XInput's own left-stick value */
        /* The runtime's MouseFeel defaults; not written until something
         * sets them. */
        c->port[p].mouse.sensitivity = 1.0;
        c->port[p].mouse.exponent    = 1.0;
        c->port[p].mouse.y_scale     = 1.0;
        c->port[p].mouse.window_ms   = 16;
        c->port[p].mouse.zoom_match  = 1;
        for (i = 0; i < BIND_CONTROLS; i++) {
            snprintf(c->port[p].pad_src[i], BIND_SOURCE_LEN, "%s", k_defaults[i].pad);
            /* The keyboard column is port 1 only, as in the runtime. */
            snprintf(c->port[p].key_src[i], BIND_SOURCE_LEN, "%s",
                     p == 0 ? k_defaults[i].key : "");
        }
    }
}

int bind_load_with_labels(const char *path, const char *preset_path, BindConfig *c)
{
    int ok = path ? bind_load(path, c) : (bind_defaults(c), 0);

    if (preset_path && *preset_path && (!path || strcmp(path, preset_path) != 0)) {
        static BindConfig preset;          /* large: not on the stack */
        int i;

        if (bind_load(preset_path, &preset))
            for (i = 0; i < BIND_CONTROLS; i++)
                if (!c->action[i][0] && preset.action[i][0])
                    snprintf(c->action[i], sizeof c->action[i], "%s", preset.action[i]);
    }
    return ok;
}

const char *bind_control_title(const BindConfig *c, int control)
{
    if (control < 0 || control >= BIND_CONTROLS)
        return "";
    if (c && c->action[control][0])
        return c->action[control];
    return bind_control_labels[control];
}

int bind_is_key_or_mouse(const char *src)
{
    return src && (strncmp(src, "key:", 4) == 0 || strncmp(src, "mouse:", 6) == 0);
}

void bind_drop_hidden_keys(BindPort *bp, int control)
{
    int e, kept = 0;
    char keep[BIND_EXTRA][BIND_SOURCE_LEN];

    if (control < 0 || control >= BIND_CONTROLS)
        return;
    for (e = 0; e < BIND_EXTRA; e++) {
        const char *s = bp->extra_src[control][e];
        if (*s && !bind_is_key_or_mouse(s))
            snprintf(keep[kept++], BIND_SOURCE_LEN, "%s", s);
    }
    memset(bp->extra_src[control], 0, sizeof bp->extra_src[control]);
    for (e = 0; e < kept; e++)
        snprintf(bp->extra_src[control][e], BIND_SOURCE_LEN, "%s", keep[e]);
}

int bind_hidden_count(const BindPort *bp, int control)
{
    int e, n = 0;

    if (control < 0 || control >= BIND_CONTROLS)
        return 0;
    for (e = 0; e < BIND_EXTRA; e++)
        if (bp->extra_src[control][e][0])
            n++;
    return n;
}

/* ------------------------------------------------------------ reading */

static const char *ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return p;
}

static const char *jstring(const char *p, char *out, size_t n)
{
    size_t k = 0;

    p = ws(p);
    if (*p != '"')
        return NULL;
    p++;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            if (k + 1 < n) out[k++] = *p;
        } else if (k + 1 < n) {
            out[k++] = *p;
        }
        p++;
    }
    if (*p != '"')
        return NULL;
    out[k] = '\0';
    return p + 1;
}

/* Step over any one value, whatever it is: string, number, literal,
 * object or array. Needed because the file may carry keys this does not
 * model and stepping over them is how they survive being read. */
static const char *jskip(const char *p)
{
    char dummy[2];
    int depth = 0;

    p = ws(p);
    if (*p == '"')
        return jstring(p, dummy, sizeof dummy);

    if (*p == '{' || *p == '[') {
        while (*p) {
            if (*p == '"') {
                const char *q = jstring(p, dummy, sizeof dummy);

                if (!q)
                    return NULL;
                p = q;
                continue;
            }
            if (*p == '{' || *p == '[')
                depth++;
            else if (*p == '}' || *p == ']') {
                depth--;
                if (depth == 0)
                    return p + 1;
            }
            p++;
        }
        return NULL;
    }

    /* A number, or true/false/null: runs until the value ends. */
    while (*p && *p != ',' && *p != '}' && *p != ']')
        p++;
    return p;
}

static int control_index(const char *name)
{
    int i;

    for (i = 0; i < BIND_CONTROLS; i++)
        if (strcmp(bind_control_names[i], name) == 0)
            return i;
    return -1;
}

static void set_device(BindPort *bp, const char *name)
{
    if (strcmp(name, "keyboard") == 0) {
        bp->device = BIND_DEV_KEYBOARD;
        bp->pad = -1;
    } else if (strncmp(name, "gamepad:", 8) == 0 || strncmp(name, "xinput:", 7) == 0) {
        /* "xinput:N" is what files written before SDL3 say; same slot. */
        bp->device = BIND_DEV_PAD;
        bp->pad = (int)strtol(strchr(name, ':') + 1, NULL, 10);
        if (bp->pad < 0 || bp->pad >= BIND_PORTS)
            bp->pad = 0;
    } else {
        bp->device = BIND_DEV_NONE;
        bp->pad = -1;
    }
}

/* The sources for one control. The first pad source and the first
 * keyboard-or-mouse source are the two the launcher shows; up to
 * BIND_EXTRA more are kept in extra_src and written back. */
static const char *parse_sources(const char *p, BindPort *bp, int k)
{
    char value[BIND_SOURCE_LEN];
    int got_pad = 0, got_key = 0, extra = 0;

    p = ws(p);
    if (k >= 0) {
        bp->pad_src[k][0] = '\0';
        bp->key_src[k][0] = '\0';
        memset(bp->extra_src[k], 0, sizeof bp->extra_src[k]);
    }
    if (*p == '[') {
        p = ws(p + 1);
        while (*p && *p != ']') {
            p = jstring(p, value, sizeof value);
            if (!p)
                return NULL;
            if (k >= 0 && value[0]) {
                if (strncmp(value, "pad:", 4) == 0 && !got_pad) {
                    snprintf(bp->pad_src[k], BIND_SOURCE_LEN, "%s", value);
                    got_pad = 1;
                } else if (bind_is_key_or_mouse(value) && !got_key) {
                    snprintf(bp->key_src[k], BIND_SOURCE_LEN, "%s", value);
                    got_key = 1;
                } else if (extra < BIND_EXTRA) {
                    snprintf(bp->extra_src[k][extra++], BIND_SOURCE_LEN, "%s", value);
                }
            }
            p = ws(p);
            if (*p == ',')
                p = ws(p + 1);
        }
        if (*p != ']')
            return NULL;
        return p + 1;
    }
    /* A bare string is allowed too. */
    p = jstring(p, value, sizeof value);
    if (!p)
        return NULL;
    if (k >= 0) {
        if (bind_is_key_or_mouse(value))
            snprintf(bp->key_src[k], BIND_SOURCE_LEN, "%s", value);
        else if (strcmp(value, "none") != 0)
            snprintf(bp->pad_src[k], BIND_SOURCE_LEN, "%s", value);
    }
    return p;
}

static const char *parse_bindings_obj(const char *p, BindPort *bp)
{
    char key[48];

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
        p = parse_sources(ws(p + 1), bp, control_index(key));
        if (!p)
            return NULL;
        p = ws(p);
        if (*p == ',')
            p = ws(p + 1);
    }
    return *p == '}' ? p + 1 : NULL;
}

static const char *parse_controller(const char *p, BindConfig *c, int index)
{
    BindPort tmp;
    char key[48], value[64];
    int port = index;

    if (index < BIND_PORTS)
        tmp = c->port[index];
    else
        tmp = c->port[0];

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
            port = (int)strtol(p, NULL, 10) - 1;        /* the file is 1-based */
            p = jskip(p);
        } else if (strcmp(key, "device") == 0) {
            p = jstring(p, value, sizeof value);
            if (!p) return NULL;
            set_device(&tmp, value);
        } else if (strcmp(key, "deadzone") == 0) {
            tmp.deadzone = (int)strtol(p, NULL, 10);
            p = jskip(p);
        } else if (strncmp(key, "mouse_", 6) == 0) {
            /* The runtime's ranges (input_bindings.c); outside them it
             * keeps its default, and so does this. */
            BindMouse *m = &tmp.mouse;
            double v = strtod(p, NULL);
            int took = 1;

            if (strcmp(key, "mouse_sensitivity") == 0 && v > 0.0 && v <= 100.0)
                m->sensitivity = v;
            else if (strcmp(key, "mouse_deadzone") == 0 && v >= 0 && v < 32767)
                m->deadzone = (int)v;
            else if (strcmp(key, "mouse_exponent") == 0 && v >= 0.1 && v <= 10.0)
                m->exponent = v;
            else if (strcmp(key, "mouse_y_scale") == 0 && v >= 0.1 && v <= 10.0)
                m->y_scale = v;
            else if (strcmp(key, "mouse_invert_y") == 0)
                m->invert_y = strncmp(p, "true", 4) == 0 || *p == '1';
            else if (strcmp(key, "mouse_zoom_match") == 0)
                m->zoom_match = strncmp(p, "true", 4) == 0 || *p == '1';
            else if (strcmp(key, "mouse_window_ms") == 0 && v >= 1 && v <= 250)
                m->window_ms = (int)v;
            else
                took = 0;
            if (took)
                m->present = 1;
            p = jskip(p);
            if (!p) return NULL;
        } else if (strcmp(key, "bindings") == 0) {
            p = parse_bindings_obj(p, &tmp);
            if (!p) return NULL;
        } else if (strcmp(key, "hold") == 0 && *p == '[') {
            /* ["rthumb", "lthumb"]: keys that hold a toggle on */
            p = ws(p + 1);
            tmp.hold = 0;
            while (*p && *p != ']') {
                int k;
                p = jstring(p, value, sizeof value);
                if (!p) return NULL;
                k = control_index(value);
                if (k >= 0)
                    tmp.hold |= 1u << k;
                p = ws(p);
                if (*p == ',')
                    p = ws(p + 1);
            }
            if (*p != ']') return NULL;
            p++;
        } else {
            p = jskip(p);
            if (!p) return NULL;
        }
        p = ws(p);
        if (*p == ',')
            p = ws(p + 1);
    }
    if (*p != '}')
        return NULL;
    if (port >= 0 && port < BIND_PORTS)
        c->port[port] = tmp;
    return p + 1;
}

int bind_load(const char *path, BindConfig *c)
{
    FILE *f;
    long len;
    char *text;
    const char *p;
    char key[48];
    int index = 0, ok = 0;

    bind_defaults(c);
    f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 1 << 20) { fclose(f); return 0; }
    text = (char *)malloc((size_t)len + 1);
    if (!text) { fclose(f); return 0; }
    if (fread(text, 1, (size_t)len, f) != (size_t)len) {
        fclose(f); free(text); return 0;
    }
    fclose(f);
    text[len] = '\0';

    p = ws(text);
    if (*p != '{') { free(text); return 0; }
    p = ws(p + 1);
    while (*p && *p != '}') {
        p = jstring(p, key, sizeof key);
        if (!p) break;
        p = ws(p);
        if (*p != ':') break;
        p = ws(p + 1);
        if (strcmp(key, "pad_api") == 0 && *p == '"') {
            char api[16];
            p = jstring(p, api, sizeof api);
            if (!p) break;
            c->pad_api = strcmp(api, "xinput") == 0 ? BIND_PAD_API_XINPUT : BIND_PAD_API_SDL;
        } else if (strcmp(key, "labels") == 0 && *p == '{') {
            /* { "rtrigger": "Fire", ... } */
            char name[48], label[32];
            p = ws(p + 1);
            while (*p && *p != '}') {
                int k;
                p = jstring(p, name, sizeof name);
                if (!p) break;
                p = ws(p);
                if (*p != ':') { p = NULL; break; }
                p = ws(p + 1);
                p = jstring(p, label, sizeof label);
                if (!p) break;
                k = control_index(name);
                if (k >= 0)
                    snprintf(c->action[k], sizeof c->action[k], "%s", label);
                p = ws(p);
                if (*p == ',')
                    p = ws(p + 1);
            }
            if (!p) break;
            if (*p == '}') p++;
        } else if (strcmp(key, "controllers") == 0 && *p == '[') {
            p = ws(p + 1);
            while (*p && *p != ']') {
                p = parse_controller(p, c, index++);
                if (!p) break;
                p = ws(p);
                if (*p == ',')
                    p = ws(p + 1);
            }
            if (!p) break;
            if (*p == ']') p++;
            ok = 1;
        } else {
            p = jskip(p);
            if (!p) break;
        }
        p = ws(p);
        if (*p == ',')
            p = ws(p + 1);
    }
    free(text);
    return ok;
}

/* ------------------------------------------------------------ writing */

static void make_parents(const char *path)
{
    char buf[1024];
    size_t i;

    snprintf(buf, sizeof buf, "%s", path);
    for (i = 0; buf[i]; i++)
        if (buf[i] == '\\' || buf[i] == '/') {
            char ch = buf[i];
            buf[i] = '\0';
            if (i > 0) BIND_MKDIR(buf);
            buf[i] = ch;
        }
}

int bind_save(const char *path, const BindConfig *c)
{
    FILE *f;
    int p, i;

    make_parents(path);
    f = fopen(path, "wb");
    if (!f)
        return 0;

    fprintf(f, "{\n  \"version\": 1,\n  \"pad_api\": \"%s\",\n",
            c->pad_api == BIND_PAD_API_XINPUT ? "xinput" : "sdl");
    /* The title's action names, kept so a save does not lose them. */
    {
        int any = 0, first = 1;

        for (i = 0; i < BIND_CONTROLS; i++)
            if (c->action[i][0])
                any = 1;
        if (any) {
            fprintf(f, "  \"labels\": {");
            for (i = 0; i < BIND_CONTROLS; i++)
                if (c->action[i][0]) {
                    fprintf(f, "%s\n    \"%s\": \"%s\"", first ? "" : ",",
                            bind_control_names[i], c->action[i]);
                    first = 0;
                }
            fprintf(f, "\n  },\n");
        }
    }
    fprintf(f, "  \"controllers\": [\n");
    for (p = 0; p < BIND_PORTS; p++) {
        const BindPort *bp = &c->port[p];
        int first = 1;

        fprintf(f, "    {\n      \"port\": %d,\n", p + 1);
        if (bp->device == BIND_DEV_KEYBOARD)
            fprintf(f, "      \"device\": \"keyboard\",\n");
        else if (bp->device == BIND_DEV_PAD)
            fprintf(f, "      \"device\": \"gamepad:%d\",\n", bp->pad);
        else
            fprintf(f, "      \"device\": \"none\",\n");
        fprintf(f, "      \"deadzone\": %d,\n", bp->deadzone);
        if (bp->mouse.present)
            fprintf(f, "      \"mouse_sensitivity\": %.3f,\n"
                       "      \"mouse_deadzone\": %d,\n"
                       "      \"mouse_exponent\": %.3f,\n"
                       "      \"mouse_y_scale\": %.3f,\n"
                       "      \"mouse_invert_y\": %s,\n"
                       "      \"mouse_zoom_match\": %s,\n"
                       "      \"mouse_window_ms\": %d,\n",
                    bp->mouse.sensitivity, bp->mouse.deadzone, bp->mouse.exponent,
                    bp->mouse.y_scale, bp->mouse.invert_y ? "true" : "false",
                    bp->mouse.zoom_match ? "true" : "false", bp->mouse.window_ms);
        if (bp->hold) {
            int any = 0;

            fprintf(f, "      \"hold\": [");
            for (i = 0; i < BIND_CONTROLS; i++)
                if ((bp->hold >> i) & 1u)
                    fprintf(f, "%s\"%s\"", any++ ? ", " : "", bind_control_names[i]);
            fprintf(f, "],\n");
        }
        fprintf(f, "      \"bindings\": {\n");

        for (i = 0; i < BIND_CONTROLS; i++) {
            const char *srcs[2 + BIND_EXTRA];
            int n = 0, s, e;

            srcs[n++] = bp->pad_src[i];
            srcs[n++] = bp->key_src[i];
            for (e = 0; e < BIND_EXTRA; e++)
                srcs[n++] = bp->extra_src[i][e];

            /* Every control is written, including one with no sources.
             * Leaving it out does not unbind it: a control the file does
             * not name keeps its default, here and in the runtime, so an
             * omitted control comes back bound. An empty array is how
             * "deliberately nothing" is said. */
            if (!first)
                fprintf(f, ",\n");
            first = 0;
            fprintf(f, "        \"%s\": [", bind_control_names[i]);
            for (s = 0, e = 0; s < n; s++)
                if (*srcs[s])
                    fprintf(f, "%s\"%s\"", e++ ? ", " : "", srcs[s]);
            fprintf(f, "]");
        }
        fprintf(f, "\n      }\n    }%s\n", p + 1 < BIND_PORTS ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    return 1;
}

/* ------------------------------------------------------------- labels */

static const struct { const char *src, *label; } k_labels[] = {
    { "pad:a", "A" }, { "pad:b", "B" }, { "pad:x", "X" }, { "pad:y", "Y" },
    { "pad:lshoulder", "Left shoulder" }, { "pad:rshoulder", "Right shoulder" },
    { "pad:start", "Start" }, { "pad:back", "Back" },
    { "pad:lthumb", "Left stick press" }, { "pad:rthumb", "Right stick press" },
    { "pad:dpad_up", "D-pad up" }, { "pad:dpad_down", "D-pad down" },
    { "pad:dpad_left", "D-pad left" }, { "pad:dpad_right", "D-pad right" },
    { "pad:lt", "Left trigger" }, { "pad:rt", "Right trigger" },
    { "pad:lx-", "Left stick left" }, { "pad:lx+", "Left stick right" },
    { "pad:ly+", "Left stick up" }, { "pad:ly-", "Left stick down" },
    { "pad:rx-", "Right stick left" }, { "pad:rx+", "Right stick right" },
    { "pad:ry+", "Right stick up" }, { "pad:ry-", "Right stick down" },
    { "key:RETURN", "Enter" }, { "key:BACK", "Backspace" },
    { "key:UP", "Up arrow" }, { "key:DOWN", "Down arrow" },
    { "key:LEFT", "Left arrow" }, { "key:RIGHT", "Right arrow" },
    { "key:SPACE", "Space" }, { "key:ESCAPE", "Escape" },
    { "key:LSHIFT", "Left shift" }, { "key:RSHIFT", "Right shift" },
    { "key:LCTRL", "Left ctrl" }, { "key:RCTRL", "Right ctrl" },
    { "key:TAB", "Tab" }, { "key:LALT", "Left alt" }, { "key:RALT", "Right alt" },
    { "mouse:left", "Left click" }, { "mouse:right", "Right click" },
    { "mouse:middle", "Middle click" },
    { "mouse:x1", "Mouse back" }, { "mouse:x2", "Mouse forward" },
    { "mouse:wheel_up", "Wheel up" }, { "mouse:wheel_down", "Wheel down" },
    { "mouse:x+", "Mouse right" }, { "mouse:x-", "Mouse left" },
    { "mouse:y+", "Mouse forward" }, { "mouse:y-", "Mouse back" },
    { NULL, NULL }
};

const char *bind_source_label(const char *src)
{
    static char buf[48];
    int i;

    if (!src || !*src)
        return "Not bound";
    for (i = 0; k_labels[i].src; i++)
        if (strcmp(k_labels[i].src, src) == 0)
            return k_labels[i].label;
    if (strncmp(src, "key:", 4) == 0) {
        snprintf(buf, sizeof buf, "%s", src + 4);
        return buf;
    }
    snprintf(buf, sizeof buf, "%s", src);
    return buf;
}

/* ------------------------------------------------------------- capture */

#if defined(_WIN32)

/* The key names the runtime knows by name (its KEYS table); everything
 * else is a letter or a digit and spells itself. Not offered: Escape, which
 * is how a person gets out of the capture; F1, the in-game menu's key; and
 * F9-F12, the game's frame counter, frame cap, capture and screenshot keys. */
static const struct { int vk; const char *name; } k_named_keys[] = {
    { VK_RETURN, "RETURN" }, { VK_BACK, "BACK" }, { VK_SPACE, "SPACE" },
    { VK_TAB, "TAB" }, { VK_UP, "UP" }, { VK_DOWN, "DOWN" },
    { VK_LEFT, "LEFT" }, { VK_RIGHT, "RIGHT" },
    { VK_LSHIFT, "LSHIFT" }, { VK_RSHIFT, "RSHIFT" },
    { VK_LCONTROL, "LCTRL" }, { VK_RCONTROL, "RCTRL" },
    { VK_LMENU, "LALT" }, { VK_RMENU, "RALT" }, { VK_CAPITAL, "CAPSLOCK" },
    { VK_INSERT, "INSERT" }, { VK_DELETE, "DELETE" }, { VK_HOME, "HOME" },
    { VK_END, "END" }, { VK_PRIOR, "PAGEUP" }, { VK_NEXT, "PAGEDOWN" },
    { VK_F2, "F2" }, { VK_F3, "F3" }, { VK_F4, "F4" },
    { VK_F5, "F5" }, { VK_F6, "F6" }, { VK_F7, "F7" }, { VK_F8, "F8" },
    { VK_NUMPAD0, "NUMPAD0" }, { VK_NUMPAD1, "NUMPAD1" }, { VK_NUMPAD2, "NUMPAD2" },
    { VK_NUMPAD3, "NUMPAD3" }, { VK_NUMPAD4, "NUMPAD4" }, { VK_NUMPAD5, "NUMPAD5" },
    { VK_NUMPAD6, "NUMPAD6" }, { VK_NUMPAD7, "NUMPAD7" }, { VK_NUMPAD8, "NUMPAD8" },
    { VK_NUMPAD9, "NUMPAD9" },
    { VK_OEM_1, "SEMICOLON" }, { VK_OEM_PLUS, "EQUALS" }, { VK_OEM_COMMA, "COMMA" },
    { VK_OEM_MINUS, "MINUS" }, { VK_OEM_PERIOD, "PERIOD" }, { VK_OEM_2, "SLASH" },
    { VK_OEM_3, "GRAVE" }, { VK_OEM_4, "LBRACKET" }, { VK_OEM_5, "BACKSLASH" },
    { VK_OEM_6, "RBRACKET" }, { VK_OEM_7, "QUOTE" },
    { 0, NULL }
};

static const struct { int vk; const char *name; } k_mouse_buttons[] = {
    { VK_LBUTTON, "left" }, { VK_RBUTTON, "right" }, { VK_MBUTTON, "middle" },
    { VK_XBUTTON1, "x1" }, { VK_XBUTTON2, "x2" },
    { 0, NULL }
};

void bind_capture_begin(BindCapture *c)
{
    POINT p;

    memset(c, 0, sizeof *c);
    if (GetCursorPos(&p)) {
        c->from_x = p.x;
        c->from_y = p.y;
    }
}

void bind_capture_wheel(BindCapture *c, int delta)
{
    if (delta)
        c->wheel = delta > 0 ? 1 : -1;
}

const char *bind_capture_poll(BindCapture *c, int control)
{
    static char out[BIND_SOURCE_LEN];
    int i, vk;

    for (i = 0; k_named_keys[i].name; i++)
        if (GetAsyncKeyState(k_named_keys[i].vk) & 0x8000) {
            snprintf(out, sizeof out, "key:%s", k_named_keys[i].name);
            return out;
        }
    for (vk = 'A'; vk <= 'Z'; vk++)
        if (GetAsyncKeyState(vk) & 0x8000) {
            snprintf(out, sizeof out, "key:%c", (char)vk);
            return out;
        }
    for (vk = '0'; vk <= '9'; vk++)
        if (GetAsyncKeyState(vk) & 0x8000) {
            snprintf(out, sizeof out, "key:%c", (char)vk);
            return out;
        }
    for (i = 0; k_mouse_buttons[i].name; i++)
        if (GetAsyncKeyState(k_mouse_buttons[i].vk) & 0x8000) {
            snprintf(out, sizeof out, "mouse:%s", k_mouse_buttons[i].name);
            return out;
        }
    if (c->wheel) {
        snprintf(out, sizeof out, "mouse:%s", c->wheel > 0 ? "wheel_up" : "wheel_down");
        return out;
    }
    /* Moving the mouse answers only for a stick direction: anywhere else a
     * hand drifting to the mouse would bind it. Pushed well past a nudge,
     * and screen up is away from the player, which is stick up. */
    if (control >= 0 && control < BIND_CONTROLS &&
        strstr(bind_control_names[control], "stick_")) {
        POINT now;
        const long reach = 80;    /* pixels; "far" is a windows.h macro */

        if (GetCursorPos(&now)) {
            long dx = now.x - c->from_x, dy = now.y - c->from_y;

            if (dx > reach)  { snprintf(out, sizeof out, "mouse:x+"); return out; }
            if (dx < -reach) { snprintf(out, sizeof out, "mouse:x-"); return out; }
            if (dy < -reach) { snprintf(out, sizeof out, "mouse:y+"); return out; }
            if (dy > reach)  { snprintf(out, sizeof out, "mouse:y-"); return out; }
        }
    }
    return NULL;
}

#else

void bind_capture_begin(BindCapture *c) { memset(c, 0, sizeof *c); }
void bind_capture_wheel(BindCapture *c, int delta) { (void)c; (void)delta; }
const char *bind_capture_poll(BindCapture *c, int control) { (void)c; (void)control; return NULL; }

#endif
