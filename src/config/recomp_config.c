/*
 * recomp_config.c -- see recomp_config.h for what this is and why.
 *
 * The parser is deliberately small. Blank lines and lines starting with
 * '#' or ';' are comments, everything else is `key = value` with the
 * spaces optional, and a key it does not recognise is ignored rather than
 * refused -- a config written by a newer launcher must not stop an older
 * build from running.
 */
#include "recomp_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>     /* GetModuleFileNameA: the bindings preset */
#include <direct.h>
#define RECOMP_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>      /* readlink: the bindings preset */
#define RECOMP_MKDIR(p) mkdir((p), 0755)
#endif

#define MAX_ENTRIES 64
#define MAX_KEY     64
#define MAX_VALUE   512

typedef struct {
    char key[MAX_KEY];
    char value[MAX_VALUE];
} Entry;

static Entry   g_entries[MAX_ENTRIES];
static int     g_entry_count;
static int     g_loaded;
static uint32_t g_title_id;
static char    g_path[1024];
static int     g_have_path;

void recomp_config_set_title(uint32_t title_id)
{
    g_title_id = title_id;
}

uint32_t recomp_config_title_id(void)
{
    return g_title_id;
}

/* Defined below, beside the lookups it belongs to; needed by the
 * settings reader above it. */
static const char *from_table(const char *key);

/* ----------------------------------------------------------------- text */

/* Case-insensitive compare, spelled out rather than borrowed: stricmp is
 * MSVC's and strcasecmp is POSIX's, and this file is built for both. */
static int ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++; b++;
    }
    return *a == *b;
}

/* The words that mean off, everywhere in the runtime. Anything else,
 * including an empty string, means on. */
static int off_word(const char *v)
{
    return strcmp(v, "0") == 0 || ieq(v, "off") || ieq(v, "no") || ieq(v, "false");
}

static char *trim(char *s)
{
    char *end;

    while (*s && isspace((unsigned char)*s))
        s++;
    if (!*s)
        return s;
    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end))
        *end-- = '\0';
    return s;
}

static void store(const char *key, const char *value)
{
    int i;

    for (i = 0; i < g_entry_count; i++)
        if (strcmp(g_entries[i].key, key) == 0) {
            snprintf(g_entries[i].value, sizeof g_entries[i].value, "%s", value);
            return;
        }
    if (g_entry_count >= MAX_ENTRIES)
        return;
    snprintf(g_entries[g_entry_count].key, MAX_KEY, "%s", key);
    snprintf(g_entries[g_entry_count].value, MAX_VALUE, "%s", value);
    g_entry_count++;
}

static void parse(FILE *f)
{
    char line[MAX_VALUE + MAX_KEY + 8];

    while (fgets(line, sizeof line, f)) {
        char *p = trim(line), *eq;

        if (!*p || *p == '#' || *p == ';')
            continue;
        eq = strchr(p, '=');
        if (!eq)
            continue;                    /* not a setting; say nothing */
        *eq = '\0';
        store(trim(p), trim(eq + 1));
    }
}

/* ----------------------------------------------------------------- paths */

/* Per-user, beside the input bindings, because a setting is a judgement
 * about this machine rather than about this copy of the game. */
static int user_dir(char *out, size_t n)
{
#if defined(_WIN32)
    const char *home = getenv("APPDATA");
    const char *tail = "\\xboxrecomp";
#else
    const char *home = getenv("XDG_CONFIG_HOME");
    const char *tail = "/xboxrecomp";
    char buf[512];

    if (!home || !*home) {
        const char *h = getenv("HOME");

        if (!h)
            return 0;
        snprintf(buf, sizeof buf, "%s/.config", h);
        home = buf;
    }
#endif
    if (!home || !*home)
        return 0;
    snprintf(out, n, "%s%s", home, tail);
    return 1;
}

int recomp_config_user_dir(char *out, size_t n)
{
    return user_dir(out, n);
}

static void title_file(char *out, size_t n, const char *dir)
{
#if defined(_WIN32)
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    if (g_title_id)
        snprintf(out, n, "%s%ctitles%c%08X.conf", dir, sep, sep, g_title_id);
    else
        snprintf(out, n, "%s%ctitles%cdefault.conf", dir, sep, sep);
}

static int readable(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (!f)
        return 0;
    fclose(f);
    return 1;
}

/* ------------------------------------------------------------ settings */

void recomp_settings_defaults(RecompSettings *s)
{
    if (!s)
        return;
    memset(s, 0, sizeof *s);
    s->resolution_scale  = 1;
    s->widescreen        = 0;
    s->hor_plus          = 0.0;
    s->hor_plus_register = 60;
    snprintf(s->widescreen_2d, sizeof s->widescreen_2d, "auto");
    s->anisotropy        = 1;
    s->fps_overlay       = 0;
    s->fullscreen        = 0;
    s->vrr               = 1;
    s->frame_interp      = 1;
    s->brightness        = 1.0;
    s->mute_unfocused    = 0;
    s->pause_unfocused   = 1;
    s->volume            = 100;
    s->fxaa              = 0;
    s->prompts           = 0;
    s->skip_intro        = 0;
    s->fov               = 0.0;
    snprintf(s->frame_cap, sizeof s->frame_cap, "adaptive");
}

static int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

int recomp_settings_read(const char *path, RecompSettings *s)
{
    Entry saved[MAX_ENTRIES];
    int saved_count = g_entry_count, saved_loaded = g_loaded;
    FILE *f;

    if (!path || !s)
        return 0;
    recomp_settings_defaults(s);

    f = fopen(path, "rb");
    if (!f)
        return 0;

    /* The table is the lookup path's; borrow it, then put it back, so a
     * launcher reading a file does not disturb a runtime that has
     * already loaded its own. */
    memcpy(saved, g_entries, sizeof saved);
    g_entry_count = 0;
    parse(f);
    fclose(f);

    {
        const char *v;

        if ((v = from_table("resolution_scale")) != NULL)
            s->resolution_scale = clamp_int(atoi(v), 1, 8);
        if ((v = from_table("widescreen")) != NULL)
            s->widescreen = !off_word(v);
        if ((v = from_table("wide_fill")) != NULL)
            s->wide_fill = !off_word(v);
        if ((v = from_table("hor_plus")) != NULL)
            s->hor_plus = atof(v);
        if ((v = from_table("hor_plus_register")) != NULL)
            s->hor_plus_register = clamp_int(atoi(v), 0, 191);
        if ((v = from_table("widescreen_2d")) != NULL)
            snprintf(s->widescreen_2d, sizeof s->widescreen_2d, "%s", v);
        if ((v = from_table("anisotropy")) != NULL)
            s->anisotropy = clamp_int(atoi(v), 1, 16);
        if ((v = from_table("frame_cap")) != NULL)
            snprintf(s->frame_cap, sizeof s->frame_cap, "%s", v);
        if ((v = from_table("fps_overlay")) != NULL)
            s->fps_overlay = !off_word(v);
        if ((v = from_table("fullscreen")) != NULL)
            s->fullscreen = !off_word(v);
        if ((v = from_table("vrr")) != NULL)
            s->vrr = !off_word(v);
        if ((v = from_table("frame_interp")) != NULL)
            s->frame_interp = clamp_int(atoi(v), 1, 4);
        if ((v = from_table("brightness")) != NULL) {
            double b = atof(v);
            s->brightness = b < 0.5 ? 0.5 : (b > 2.0 ? 2.0 : b);
        }
        if ((v = from_table("mute_unfocused")) != NULL)
            s->mute_unfocused = !off_word(v);
        if ((v = from_table("pause_unfocused")) != NULL)
            s->pause_unfocused = !off_word(v);
        if ((v = from_table("volume")) != NULL)
            s->volume = clamp_int(atoi(v), 0, 100);
        if ((v = from_table("fxaa")) != NULL)
            s->fxaa = !off_word(v);
        if ((v = from_table("prompts")) != NULL)
            s->prompts = strcmp(v, "keyboard") == 0 ? 1 : strcmp(v, "controller") == 0 ? 2 : 0;
        if ((v = from_table("skip_intro")) != NULL)
            s->skip_intro = !off_word(v);
        if ((v = from_table("fov")) != NULL) {
            double d = atof(v);
            s->fov = d <= 0.0 ? 0.0 : (d < RECOMP_FOV_MIN ? RECOMP_FOV_MIN
                                     : (d > RECOMP_FOV_MAX ? RECOMP_FOV_MAX : d));
        }
        if ((v = from_table("game_dir")) != NULL)
            snprintf(s->game_dir, sizeof s->game_dir, "%s", v);
    }

    memcpy(g_entries, saved, sizeof saved);
    g_entry_count = saved_count;
    g_loaded = saved_loaded;
    return 1;
}

/* Everything above the file's directory, made if it is not there. */
static void make_parents(const char *path)
{
    char buf[1024];
    size_t i;

    snprintf(buf, sizeof buf, "%s", path);
    for (i = 0; buf[i]; i++) {
        if (buf[i] == '\\' || buf[i] == '/') {
            char c = buf[i];

            buf[i] = '\0';
            if (i > 0)
                RECOMP_MKDIR(buf);
            buf[i] = c;
        }
    }
}

/* The one place the file's text is written: the runtime's first-run
 * default and the launcher's save are the same function, so the comments
 * a player reads cannot drift from the keys the runtime looks for. */
int recomp_settings_write(const char *path, const RecompSettings *s)
{
    RecompSettings d;
    FILE *f;

    if (!path)
        return 0;
    if (!s) {
        recomp_settings_defaults(&d);
        s = &d;
    }

    make_parents(path);
    f = fopen(path, "wb");
    if (!f)
        return 0;

    fprintf(f,
        "# Settings for this title, written by the launcher and read when the\n"
        "# game starts. Every line can be overridden by the environment\n"
        "# variable named beside it, so a .bat or a command line still wins.\n"
        "#\n"
        "# Delete this file to go back to the defaults.\n"
        "\n"
        "# How much larger than the console the game is rendered, before being\n"
        "# filtered back down: supersampling. 1 is the console's own size. 2 is\n"
        "# a good default and costs little. Up to 8.        [RECOMP_RES_SCALE]\n"
        "resolution_scale = %d\n"
        "\n"
        "# Present at 16:9 rather than 4:3, and tell the game the console is\n"
        "# widescreen. Only right for a game with a widescreen mode of its own;\n"
        "# one without draws 4:3 and will look stretched.  [RECOMP_WIDESCREEN]\n"
        "widescreen = %d\n"
        "\n"
        "# Widen the camera's horizontal field of view to match, so you see\n"
        "# more to the sides instead of the same view stretched. 0.75 is the\n"
        "# 4:3-to-16:9 figure; 0 leaves the camera alone.     [RECOMP_HOR_PLUS]\n"
        "hor_plus = %g\n"
        "\n"
        "# Which vertex constant register holds the projection, for the line\n"
        "# above. Per game; 60 for TimeSplitters 2.       [RECOMP_HOR_PLUS_REG]\n"
        "hor_plus_register = %d\n"
        "\n"
        "# In widescreen, where the game's 2D layer (menus, HUD, text) goes.\n"
        "# auto keeps it at 4:3 in the middle but lets anything most of the\n"
        "# screen wide span it. centre keeps all of it at 4:3, backdrops too,\n"
        "# and lets only whole-screen fades and effects span: no seams where\n"
        "# a game's menu layers meet, at the price of bars beside 2D-only\n"
        "# screens.                                      [RECOMP_WIDESCREEN_2D]\n"
        "widescreen_2d = %s\n"
        "\n"
        "# Sharpen textures seen at a glancing angle, 1 to 16. The flat layer\n"
        "# is left alone, which wants no filtering.              [RECOMP_ANISO]\n"
        "anisotropy = %d\n"
        "\n"
        "# Frame pacing: adaptive, 60, 30, or 0 for uncapped.  [RECOMP_FPS_CAP]\n"
        "frame_cap = %s\n"
        "\n"
        "# Show the frame rate from the moment the game starts. F9 toggles it\n"
        "# while playing either way.                      [RECOMP_FPS_OVERLAY]\n"
        "fps_overlay = %d\n"
        "\n"
        "# Fill the screen the window is on, without borders. Alt+Enter\n"
        "# switches while playing either way.             [RECOMP_FULLSCREEN]\n"
        "fullscreen = %d\n"
        "\n"
        "# Variable refresh rate (G-SYNC, FreeSync): in fullscreen, hand each\n"
        "# frame to the display the moment it is ready, so a 60 fps game is\n"
        "# shown at an even 60 on a 144 Hz screen. The display and its driver\n"
        "# must have it switched on. 0 turns it off.             [RECOMP_VRR]\n"
        "vrr = %d\n"
        "\n"
        "# Frame interpolation: draw frames between the game's own, which still\n"
        "# runs at 60. 2 for a 120 Hz monitor, 3 for 180 Hz, 4 for 240 Hz.\n"
        "# 1 is off.                                     [RECOMP_FRAME_INTERP]\n"
        "frame_interp = %d\n"
        "\n",
        s->resolution_scale, s->widescreen, s->hor_plus, s->hor_plus_register,
        s->widescreen_2d[0] ? s->widescreen_2d : "auto",
        s->anisotropy, s->frame_cap[0] ? s->frame_cap : "adaptive", s->fps_overlay,
        s->fullscreen, s->vrr, s->frame_interp < 1 ? 1 : s->frame_interp);
    fprintf(f,
        "# Brightness, as a gamma on the finished picture: above 1 lifts the\n"
        "# shadows, below 1 deepens them. 0.5 to 2.        [RECOMP_BRIGHTNESS]\n"
        "brightness = %.2f\n"
        "\n"
        "# Edge smoothing (FXAA) on the finished picture: cheap, and most use\n"
        "# below a resolution scale of 4, where supersampling has not already\n"
        "# smoothed the edges. 0 is off.                         [RECOMP_FXAA]\n"
        "fxaa = %d\n"
        "\n"
        "# Silence the game while another window has the focus.\n"
        "#                                             [RECOMP_MUTE_UNFOCUSED]\n"
        "mute_unfocused = %d\n"
        "\n"
        "# Pause the game when its window loses the focus during play (Alt+Tab),\n"
        "# for a game whose project says how. 0 keeps it running.\n"
        "#                                            [RECOMP_PAUSE_UNFOCUSED]\n"
        "pause_unfocused = %d\n"
        "\n"
        "# Volume of everything the game plays, 0 to 100 percent.\n"
        "#                                                     [RECOMP_VOLUME]\n"
        "volume = %d\n"
        "\n"
        "# Button prompts, for a game whose project knows its button icons:\n"
        "# auto shows the keys while the keyboard or mouse was used last and\n"
        "# the pad's buttons while a pad was; keyboard; controller.\n"
        "#                                                    [RECOMP_PROMPTS]\n"
        "prompts = %s\n"
        "\n"
        "# With widescreen on, fill the window whatever its shape -- 21:9,\n"
        "# 32:9, 16:10 -- rather than a 16:9 picture between bars. Only for a\n"
        "# game whose project can make it draw any shape.  [RECOMP_WIDE_FILL]\n"
        "wide_fill = %d\n"
        "\n"
        "# Skip the logo videos the game plays before its title screen.\n"
        "#                                                 [RECOMP_SKIP_INTRO]\n"
        "skip_intro = %d\n"
        "\n"
        "# Field of view, in degrees from the top of the screen to the bottom,\n"
        "# 40 to 90; 0 keeps the game's own. Only for a game whose project can\n"
        "# set its camera.                                         [RECOMP_FOV]\n"
        "fov = %g\n"
        "\n"
        "# Where the game's files are, if they are not beside the executable.\n"
        "#                                                   [RECOMP_GAME_DIR]\n",
        s->brightness >= 0.5 ? s->brightness : 1.0, s->fxaa ? 1 : 0, s->mute_unfocused,
        s->pause_unfocused, s->volume < 0 ? 0 : (s->volume > 100 ? 100 : s->volume),
        s->prompts == 1 ? "keyboard" : s->prompts == 2 ? "controller" : "auto",
        s->wide_fill, s->skip_intro,
        s->fov > 0.0 ? s->fov : 0.0);

    if (s->game_dir[0])
        fprintf(f, "game_dir = %s\n", s->game_dir);
    else
        fprintf(f, "# game_dir =\n");

    fclose(f);
    return 1;
}

int recomp_settings_path(uint32_t title_id, char *out, size_t n)
{
    const char *env = getenv("RECOMP_DISPLAY_CONFIG");
    char dir[1024];

    if (!out || !n)
        return 0;
    if (env && *env) {
        snprintf(out, n, "%s", env);
        return 1;
    }
    if (!user_dir(dir, sizeof dir))
        return 0;
    {
        uint32_t saved = g_title_id;

        g_title_id = title_id;
        title_file(out, n, dir);
        g_title_id = saved;
    }
    return 1;
}

/* ------------------------------------------------------------------ fov */

static double g_fov_game;               /* 0: the title cannot set it */
static volatile double g_fov = -1.0;    /* -1 until read from the settings */

void recomp_fov_support(double game_degrees)
{
    g_fov_game = game_degrees > 0.0 ? game_degrees : 0.0;
}

double recomp_fov_game(void)
{
    return g_fov_game;
}

void recomp_fov_set(double degrees)
{
    g_fov = degrees <= 0.0 ? 0.0 : (degrees < RECOMP_FOV_MIN ? RECOMP_FOV_MIN
                                   : (degrees > RECOMP_FOV_MAX ? RECOMP_FOV_MAX : degrees));
}

double recomp_fov(void)
{
    if (g_fov < 0.0) {
        const char *v = recomp_config_lookup("RECOMP_FOV", "fov");
        recomp_fov_set(v && *v ? atof(v) : 0.0);
    }
    return g_fov;
}

/* ------------------------------------------------------------ bindings */

#if defined(_WIN32)
#  define SEP "\\"
#else
#  define SEP "/"
#endif

int recomp_bindings_preset_path(char *out, size_t n)
{
    char exe[1024];
    size_t len = 0;

    if (!out || !n)
        return 0;
#if defined(_WIN32)
    len = GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe);
#else
    {
        ssize_t got = readlink("/proc/self/exe", exe, sizeof exe - 1);
        len = got > 0 ? (size_t)got : 0;
        exe[len] = '\0';
    }
#endif
    if (len > 0 && len < sizeof exe) {
        char *slash = strrchr(exe, SEP[0]);
        if (slash) {
            slash[1] = '\0';
            snprintf(out, n, "%sinput_bindings.json", exe);
            if (readable(out))
                return 1;
        }
    }
    /* The working directory: what this used to be, and still the answer for
     * a run.bat that starts the title from somewhere else. */
    snprintf(out, n, "input_bindings.json");
    return readable(out);
}

static int title_bindings_file(uint32_t title_id, char *out, size_t n)
{
    char dir[1024];

    if (!title_id || !user_dir(dir, sizeof dir))
        return 0;
    snprintf(out, n, "%s" SEP "titles" SEP "%08X.bindings.json", dir, (unsigned)title_id);
    return 1;
}

static int shared_bindings_file(char *out, size_t n)
{
    char dir[1024];

    if (!user_dir(dir, sizeof dir))
        return 0;
    snprintf(out, n, "%s" SEP "input_bindings.json", dir);
    return 1;
}

int recomp_bindings_read_path(uint32_t title_id, char *out, size_t n)
{
    const char *env = getenv("RECOMP_INPUT_CONFIG");

    if (!out || !n)
        return 0;
    if (env && *env) {
        snprintf(out, n, "%s", env);
        return 1;
    }
    if (title_bindings_file(title_id, out, n) && readable(out))
        return 1;
    if (shared_bindings_file(out, n) && readable(out))
        return 1;
    return recomp_bindings_preset_path(out, n);
}

int recomp_bindings_write_path(uint32_t title_id, char *out, size_t n)
{
    const char *env = getenv("RECOMP_INPUT_CONFIG");

    if (!out || !n)
        return 0;
    if (env && *env) {
        snprintf(out, n, "%s", env);
        return 1;
    }
    if (title_bindings_file(title_id, out, n))
        return 1;
    return shared_bindings_file(out, n);
}

#undef SEP

static void load(void)
{
    const char *env = getenv("RECOMP_DISPLAY_CONFIG");
    char dir[1024], path[1024];
    FILE *f;

    g_loaded = 1;

    /* Named outright: use it or say so, and do not quietly fall back --
     * a mistyped path should not look like the settings were ignored. */
    if (env && *env) {
        f = fopen(env, "rb");
        if (f) {
            parse(f);
            fclose(f);
            snprintf(g_path, sizeof g_path, "%s", env);
            g_have_path = 1;
        } else {
            fprintf(stderr, "[CONFIG] RECOMP_DISPLAY_CONFIG=%s cannot be read; "
                            "using the defaults\n", env);
        }
        return;
    }

    if (user_dir(dir, sizeof dir)) {
        title_file(path, sizeof path, dir);
        if (!readable(path))
            recomp_settings_write(path, NULL);
        f = fopen(path, "rb");
        if (f) {
            parse(f);
            fclose(f);
            snprintf(g_path, sizeof g_path, "%s", path);
            g_have_path = 1;
            return;
        }
    }

    /* Beside the executable, which is where a portable copy would keep
     * it -- the working directory a launcher starts the game in. */
    f = fopen("recomp.conf", "rb");
    if (f) {
        parse(f);
        fclose(f);
        snprintf(g_path, sizeof g_path, "recomp.conf");
        g_have_path = 1;
    }
}

/* Said once, because "which file did it read" is the first question when a
 * setting chosen in the launcher does not seem to apply. */
static void load_and_report(void)
{
    load();
    if (g_have_path)
        fprintf(stderr, "[CONFIG] settings from %s (title %08X)\n", g_path, g_title_id);
    else
        fprintf(stderr, "[CONFIG] no settings file (title %08X); using the defaults\n",
                g_title_id);
}

/* ---------------------------------------------------------------- lookup */

/* The table as it stands, without loading anything. */
static const char *from_table(const char *key)
{
    int i;

    if (!key)
        return NULL;
    for (i = 0; i < g_entry_count; i++)
        if (strcmp(g_entries[i].key, key) == 0)
            return g_entries[i].value[0] ? g_entries[i].value : NULL;
    return NULL;
}

static const char *from_file(const char *key)
{
    if (!key)
        return NULL;
    if (!g_loaded)
        load_and_report();
    return from_table(key);
}

const char *recomp_config_lookup(const char *env_name, const char *key)
{
    const char *v = env_name ? getenv(env_name) : NULL;

    if (v && *v)
        return v;
    return from_file(key);
}

const char *recomp_config_path(void)
{
    if (!g_loaded)
        load_and_report();
    return g_have_path ? g_path : NULL;
}

int recomp_config_int(const char *env_name, const char *key, int fallback)
{
    const char *v = recomp_config_lookup(env_name, key);
    char *end = NULL;
    long n;

    if (!v || !*v)
        return fallback;
    n = strtol(v, &end, 10);
    if (end == v || (end && *end))
        return fallback;
    return (int)n;
}

double recomp_config_float(const char *env_name, const char *key, double fallback)
{
    const char *v = recomp_config_lookup(env_name, key);
    char *end = NULL;
    double d;

    if (!v || !*v)
        return fallback;
    d = strtod(v, &end);
    if (end == v || (end && *end))
        return fallback;
    return d;
}

int recomp_config_bool(const char *env_name, const char *key, int fallback)
{
    const char *env = env_name ? getenv(env_name) : NULL;
    const char *v;

    /* An empty environment variable means on, as it does everywhere else
     * in the runtime: `set RECOMP_WIDESCREEN=` turns a switch on. */
    if (env)
        return !off_word(env);

    v = from_file(key);
    if (!v || !*v)
        return fallback;
    return !off_word(v);
}
