/*
 * input_host.c -- the host controller behind the replaced XAPI input.
 *
 * Adapted from doaxbv-re (https://github.com/NoRain211/doaxbv-re,
 * recomp-runtime/input_host_win32.c), GPL-3.0: XInput pad 0 and the keyboard,
 * mapped to the Xbox gamepad. Added here: RECOMP_FAKE_INPUT, the scripted
 * presses src/usb/usb_gamepad.c already understood, so an unattended run can
 * still get past "Press START"; and RECOMP_INPUT_SEQ, a one-shot timed
 * sequence that walks a menu path the same way every run.
 *
 * Which host device drives which of the four ports, and which key or pad
 * control drives each Xbox input, is src/input/input_bindings.c reading the
 * config file the input UI writes (`py -3 -m tools.input_ui`). With no config
 * file the defaults are what this file used to hard-code: XInput pad 0 plus
 * the keyboard on port 1 -- arrows = D-pad, Enter = START, Backspace = BACK,
 * Z = A, X = B, A = X, S = Y, Q = White, W = Black, E/R = triggers -- and
 * XInput pad N on port N.
 *
 * RECOMP_FAKE_INPUT and RECOMP_INPUT_SEQ still press buttons directly, on
 * port 1 only: they are a substitute for a person at the keyboard, so they
 * are deliberately unaffected by what that person bound.
 */
#include "input_host.h"
#include "input_bindings.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    XBOX_DPAD_UP = 0x0001u,
    XBOX_DPAD_DOWN = 0x0002u,
    XBOX_DPAD_LEFT = 0x0004u,
    XBOX_DPAD_RIGHT = 0x0008u,
    XBOX_START = 0x0010u,
    XBOX_BACK = 0x0020u,
    XBOX_LTHUMB = 0x0040u,
    XBOX_RTHUMB = 0x0080u,
};

/* RECOMP_FAKE_INPUT=start,a  presses each listed button in turn, one per
 * cycle; RECOMP_FAKE_INPUT_MS (default 500) is how long each press and each
 * gap lasts. Pressed and released rather than held: menus act on the edge. */
/* kind: 0 a button, 1 an analog button, 2 a stick pushed fully one way.
 * The sticks are here because a script that can only press buttons can reach
 * a menu but not a place in a level, and "walk to where it looks wrong" is
 * how a rendering bug gets reproduced without a person at the pad. The names
 * match the controls in the binding config (src/input/input_bindings.c). */
enum { FAKE_BUTTON, FAKE_ANALOG, FAKE_STICK };
enum { STICK_LX, STICK_LY, STICK_RX, STICK_RY };

static const struct { const char *name; int kind; unsigned value; int sign; } FAKE[] = {
    { "start", FAKE_BUTTON, XBOX_START, 0 },      { "back",  FAKE_BUTTON, XBOX_BACK, 0 },
    { "up",    FAKE_BUTTON, XBOX_DPAD_UP, 0 },    { "down",  FAKE_BUTTON, XBOX_DPAD_DOWN, 0 },
    { "left",  FAKE_BUTTON, XBOX_DPAD_LEFT, 0 },  { "right", FAKE_BUTTON, XBOX_DPAD_RIGHT, 0 },
    { "lthumb", FAKE_BUTTON, XBOX_LTHUMB, 0 },    { "rthumb", FAKE_BUTTON, XBOX_RTHUMB, 0 },
    { "a",   FAKE_ANALOG, HOST_ANALOG_A, 0 },   { "b",     FAKE_ANALOG, HOST_ANALOG_B, 0 },
    { "x",     FAKE_ANALOG, HOST_ANALOG_X, 0 },   { "y",     FAKE_ANALOG, HOST_ANALOG_Y, 0 },
    { "white", FAKE_ANALOG, HOST_ANALOG_WHITE, 0 }, { "black", FAKE_ANALOG, HOST_ANALOG_BLACK, 0 },
    { "lt",    FAKE_ANALOG, HOST_ANALOG_LTRIG, 0 }, { "rt",    FAKE_ANALOG, HOST_ANALOG_RTRIG, 0 },
    { "lstick_up",    FAKE_STICK, STICK_LY,  1 }, { "lstick_down",  FAKE_STICK, STICK_LY, -1 },
    { "lstick_left",  FAKE_STICK, STICK_LX, -1 }, { "lstick_right", FAKE_STICK, STICK_LX,  1 },
    { "rstick_up",    FAKE_STICK, STICK_RY,  1 }, { "rstick_down",  FAKE_STICK, STICK_RY, -1 },
    { "rstick_left",  FAKE_STICK, STICK_RX, -1 }, { "rstick_right", FAKE_STICK, STICK_RX,  1 },
};

/* Name -> FAKE index, or -1. `n` is the name's length inside a longer spec. */
static int fake_lookup(const char *p, size_t n)
{
    int i;
    for (i = 0; i < (int)(sizeof FAKE / sizeof FAKE[0]); i++)
        if (strlen(FAKE[i].name) == n && strncmp(p, FAKE[i].name, n) == 0)
            return i;
    return -1;
}

/* pct: how far a stick is pushed, 1-100; buttons ignore it. */
static void fake_apply(RecompInputGamepad *g, int i, int pct)
{
    int16_t push = (int16_t)((FAKE[i].sign > 0 ? 32767 : -32767) * pct / 100);

    switch (FAKE[i].kind) {
    case FAKE_ANALOG:
        g->analog_buttons[FAKE[i].value] = 0xFFu;
        break;
    case FAKE_STICK:
        switch (FAKE[i].value) {
        case STICK_LX: g->thumb_lx = push; break;
        case STICK_LY: g->thumb_ly = push; break;
        case STICK_RX: g->thumb_rx = push; break;
        default:       g->thumb_ry = push; break;
        }
        break;
    default:
        g->buttons |= (uint16_t)FAKE[i].value;
        break;
    }
}

/* RECOMP_INPUT_SEQ=1500:start,4000:a,6000:down+a:400 -- a one-shot script.
 *
 * Each step is <ms>:<control>[+<control>...][:<hold ms>]. A control is a
 * button, an analog button, or a stick direction held all the way over
 * (lstick_up, rstick_left, ...), so a script can walk and turn, not only
 * press. A stick direction takes @<percent> for a partial push
 * (rstick_right@20), which is how a title's own stick deadzone is measured:
 * hold a little more each time until the camera moves. The clock starts the
 * first time the title reads the pad, so a slow boot does not eat the script;
 * hold defaults to 200 ms, long enough for a title polling at 60 Hz to see the
 * press and the release. Steps may overlap, which is how a chord is held.
 *
 * This exists because RECOMP_FAKE_INPUT cycles forever: it is right for
 * "get past Press START" and wrong for a menu, where it keeps pressing things
 * at whatever screen it reached. A menu path is a fixed sequence, and there is
 * no save state to restore a native process to -- lifted code is mid-flight
 * on real threads with host GPU objects behind it -- so replaying the presses
 * is how a run gets back to the same screen. The sequence is the save state.
 * Nothing is pressed after the last step ends. */
#define SEQ_MAX 128
static struct seq_step { unsigned long at, hold; int keys[4], pct[4]; int nkeys; } s_seq[SEQ_MAX];
static int s_seq_count = -1;
static unsigned long long s_seq_t0;

static void seq_parse(const char *env)
{
    const char *p = env;

    while (*p && s_seq_count < SEQ_MAX) {
        struct seq_step *s = &s_seq[s_seq_count];
        char *end;
        const char *step_end = strchr(p, ',');
        size_t len = step_end ? (size_t)(step_end - p) : strlen(p);
        const char *q;

        memset(s, 0, sizeof *s);
        s->hold = 200;
        s->at = strtoul(p, &end, 10);
        if (end == p || *end != ':') {
            fprintf(stderr, "  [PAD] RECOMP_INPUT_SEQ: bad step at '%.*s' (want <ms>:<button>)\n",
                    (int)len, p);
            return;
        }
        q = end + 1;
        while (q < p + len) {
            size_t n = 0;
            int k;
            int pct = 100;
            while (q + n < p + len && q[n] != '+' && q[n] != ':' && q[n] != '@')
                n++;
            k = fake_lookup(q, n);
            if (k < 0) {
                fprintf(stderr, "  [PAD] RECOMP_INPUT_SEQ: unknown button '%.*s'\n", (int)n, q);
                return;
            }
            q += n;
            if (q < p + len && *q == '@') {
                pct = (int)strtol(q + 1, &end, 10);
                if (pct < 1 || pct > 100)
                    pct = 100;
                q = end;
            }
            if (s->nkeys < 4) {
                s->pct[s->nkeys] = pct;
                s->keys[s->nkeys++] = k;
            }
            if (q < p + len && *q == ':') {
                s->hold = strtoul(q + 1, NULL, 10);
                if (!s->hold)
                    s->hold = 200;
                break;
            }
            if (q < p + len && *q == '+')
                q++;
        }
        s_seq_count++;
        if (!step_end)
            break;
        p = step_end + 1;
    }
}

/* Whether an input script is driving this run (RECOMP_INPUT_SEQ), parsing
 * it the first time. */
static int seq_active(void)
{
    if (s_seq_count < 0) {
        const char *env = getenv("RECOMP_INPUT_SEQ");
        s_seq_count = 0;
        if (env && *env) {
            seq_parse(env);
            fprintf(stderr, "  [PAD] input script: %d steps from %s\n", s_seq_count, env);
            fflush(stderr);
        }
    }
    return s_seq_count > 0;
}

/* A script is for a run that goes the same way every time, and a pad left
 * connected on the desk does not: a resting stick's drift walked
 * TimeSplitters 2's name-entry cursor off END, so the script's A typed
 * letters instead of finishing and the run never left the menus. While a
 * script runs it is the only input on port 0, unless
 * RECOMP_INPUT_SEQ_PAD=1 asks for the pad as well. */
static int seq_owns_pad(void)
{
    static int with_pad = -1;

    if (!seq_active())
        return 0;
    if (with_pad < 0) {
        const char *v = getenv("RECOMP_INPUT_SEQ_PAD");

        with_pad = v && *v && strcmp(v, "0") != 0;
        if (!with_pad)
            fprintf(stderr, "  [PAD] the input script replaces the pad on port 0 "
                    "(RECOMP_INPUT_SEQ_PAD=1 to have both)\n");
    }
    return !with_pad;
}

/* RECOMP_RAM_SNAPSHOT=<prefix> with RECOMP_RAM_SNAPSHOT_AT=<ms>[,<ms>...]:
 * guest memory to <prefix><n>.bin at those times on the input script's clock,
 * so a snapshot lands between known presses. RECOMP_RAM_SNAPSHOT_RANGE=
 * <va>:<bytes> picks the range (default 0x00010000:0x03FF0000, the image and
 * the heap). This is how a variable is found by what it does: snapshot,
 * turn right, snapshot, turn right again, snapshot, look up, snapshot -- the
 * view's yaw is the value that moved by the same amount twice and then held.
 * A snapshot stalls the title for the length of the write. */
extern ptrdiff_t g_xbox_mem_offset;

static void ram_snapshots(unsigned long long t)
{
    static int configured, next, count;
    static unsigned long at[16];
    static const char *prefix;
    static uint32_t va = 0x00010000u, len = 0x03FF0000u;

    if (!configured) {
        const char *spec = getenv("RECOMP_RAM_SNAPSHOT_AT"), *range;
        char *end;

        configured = 1;
        prefix = getenv("RECOMP_RAM_SNAPSHOT");
        while (spec && *spec && count < 16) {
            at[count++] = strtoul(spec, &end, 10);
            if (end == spec)
                break;
            spec = *end == ',' ? end + 1 : end;
        }
        range = getenv("RECOMP_RAM_SNAPSHOT_RANGE");
        if (range && *range) {
            va = (uint32_t)strtoul(range, &end, 0);
            if (*end == ':')
                len = (uint32_t)strtoul(end + 1, NULL, 0);
        }
    }
    if (!prefix || !*prefix || next >= count || t < at[next])
        return;
    {
        char path[512];
        FILE *f;

        snprintf(path, sizeof path, "%s%d.bin", prefix, next);
        f = fopen(path, "wb");
        if (f) {
            fwrite((const uint8_t *)g_xbox_mem_offset + va, 1, len, f);
            fclose(f);
            fprintf(stderr, "  [PAD] RAM snapshot %d at %llu ms: 0x%08X+0x%X -> %s\n",
                    next, t, va, len, path);
            fflush(stderr);
        }
        next++;
    }
}

/* The input script's clock, for a title project's own scripted test (0
 * before the script has started, or without one). */
unsigned long long recomp_input_script_ms(void)
{
    return s_seq_t0 ? GetTickCount64() - s_seq_t0 : 0;
}

static void seq_input(RecompInputGamepad *g)
{
    unsigned long long t;
    int i, k;

    if (!seq_active())
        return;
    if (!s_seq_t0)
        s_seq_t0 = GetTickCount64();
    t = GetTickCount64() - s_seq_t0;
    ram_snapshots(t);
    for (i = 0; i < s_seq_count; i++) {
        if (t < s_seq[i].at || t >= (unsigned long long)s_seq[i].at + s_seq[i].hold)
            continue;
        for (k = 0; k < s_seq[i].nkeys; k++)
            fake_apply(g, s_seq[i].keys[k], s_seq[i].pct[k]);
    }
}

static void fake_input(RecompInputGamepad *g)
{
    static int checked;
    static char spec[64];
    static unsigned long period;
    const char *p;
    unsigned long long now;
    int count = 1, want, index = 0, i;

    if (!checked) {
        const char *env = getenv("RECOMP_FAKE_INPUT");
        const char *ms = getenv("RECOMP_FAKE_INPUT_MS");
        checked = 1;
        if (env) {
            strncpy(spec, env, sizeof spec - 1);
            fprintf(stderr, "  [PAD] synthetic input: %s\n", spec);
            fflush(stderr);
        }
        period = ms ? strtoul(ms, NULL, 0) : 0;
        if (!period)
            period = 500;
    }
    if (!spec[0])
        return;
    now = GetTickCount64();
    if ((now / period) % 2 == 0)
        return;                                 /* the gap half of the cycle */
    for (p = spec; *p; p++)
        if (*p == ',')
            count++;
    want = (int)((now / (period * 2)) % (unsigned long long)count);
    p = spec;
    while (index < want && (p = strchr(p, ',')) != NULL) {
        p++;
        index++;
    }
    if (!p)
        return;
    {
        const char *e = strchr(p, ',');
        i = fake_lookup(p, e ? (size_t)(e - p) : strlen(p));
        if (i >= 0)
            fake_apply(g, i, 100);
    }
}

/* One port, through its bindings. The bound sample arrives as the layout
 * src/input already speaks (XBOX_GAMEPAD); the two structures are the same
 * eight analog bytes and four axes in the same order, so this is a copy. */
bool recomp_input_host_sample_port(unsigned port, RecompInputGamepad *gamepad)
{
    XBOX_GAMEPAD pad;
    int i;

    if (gamepad == NULL)
        return false;
    memset(gamepad, 0, sizeof *gamepad);
    if (!(port == 0 && seq_owns_pad()) && recomp_bindings_sample(port, &pad)) {
        gamepad->buttons = (uint16_t)pad.wButtons;
        for (i = 0; i < RECOMP_INPUT_ANALOG_BUTTON_COUNT; i++)
            gamepad->analog_buttons[i] = pad.bAnalogButtons[i];
        gamepad->thumb_lx = (int16_t)pad.sThumbLX;
        gamepad->thumb_ly = (int16_t)pad.sThumbLY;
        gamepad->thumb_rx = (int16_t)pad.sThumbRX;
        gamepad->thumb_ry = (int16_t)pad.sThumbRY;
    }
    if (port == 0) {
        fake_input(gamepad);
        seq_input(gamepad);
    }
    return true;
}

bool recomp_input_host_sample(RecompInputGamepad *gamepad)
{
    return recomp_input_host_sample_port(0u, gamepad);
}
