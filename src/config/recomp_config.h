/*
 * recomp_config.h -- the settings a player chooses, and where they live.
 *
 * Everything the runtime can be told is an environment variable, which is
 * right for a developer and no use to someone who just wants the game in
 * widescreen. This is the other half: a small file the launcher writes and
 * the runtime reads, so a setting survives being chosen once.
 *
 * The environment still wins. A variable set on the command line, in a
 * .bat, or by CI overrides the file, so every existing way of driving the
 * runtime keeps working and the file is what happens when nothing said
 * otherwise. Callers say both names at the point of use:
 *
 *     v = recomp_config_lookup("RECOMP_RES_SCALE", "resolution_scale");
 *
 * which keeps the precedence visible where the setting is read rather
 * than buried here.
 *
 * The file is per title, keyed by the title id out of the XBE
 * certificate: a resolution scale is a judgement about one game on one
 * machine, and the Hor+ register is meaningless to any other game. Input
 * bindings are deliberately not here -- those belong to the person, not
 * the title, and they already have a file of their own.
 *
 * It is an INI rather than JSON because the settings are flat and because
 * a generated INI can explain itself. The file a player opens says what
 * each line does; JSON has nowhere to put that.
 */
#ifndef XBOXRECOMP_RECOMP_CONFIG_H
#define XBOXRECOMP_RECOMP_CONFIG_H

#include <stddef.h>
#include <stdint.h>

/* The running title, from its XBE certificate, which decides the file's
 * name. Called during start-up before anything reads a setting; without
 * it the lookups below still work and simply find no file. */
void recomp_config_set_title(uint32_t title_id);
/* The title id given above, 0 before it is known. */
uint32_t recomp_config_title_id(void);
/* The per-user directory settings live in (%APPDATA%\xboxrecomp on
 * Windows); 0 if there is none. Caches go beneath it. */
int recomp_config_user_dir(char *out, size_t n);

/* The value for a setting: the environment variable if it is set and not
 * empty, then the config file, then NULL. Either name may be NULL to skip
 * that source. The returned string is owned here and valid for the life
 * of the process. */
const char *recomp_config_lookup(const char *env_name, const char *key);

/* The same, parsed, with a fallback for absent or unreadable values.
 * Booleans accept 1/0, on/off, yes/no, true/false, and an empty
 * environment variable counts as on -- the rule xbox_EnvSwitch already
 * uses, so a variable behaves the same whether or not this file exists. */
int    recomp_config_int(const char *env_name, const char *key, int fallback);
int    recomp_config_bool(const char *env_name, const char *key, int fallback);
double recomp_config_float(const char *env_name, const char *key, double fallback);

/* The file the lookups read, for a log line or a launcher; NULL when no
 * file was found. */
const char *recomp_config_path(void);

/* ------------------------------------------------------------ settings */

/* The settings a player chooses, as one thing, so the launcher that
 * writes the file and the runtime that reads it cannot drift apart on
 * what the file contains. The runtime still reads values one at a time
 * through the lookups above -- that keeps the environment override
 * visible where each setting is used -- but the file's shape is here. */
typedef struct RecompSettings {
    int  resolution_scale;      /* 1..8 */
    int  widescreen;            /* 0 or 1 */
    int  wide_fill;             /* 0 or 1: widescreen fills the window at any shape,
                                 * for a title that can draw any (ultrawide) */
    double hor_plus;            /* 0 leaves the camera alone; 0.75 is 4:3->16:9 */
    int  hor_plus_register;     /* per title; 60 for TimeSplitters 2 */
    char widescreen_2d[16];     /* "auto" or "centre": where 2D goes in widescreen */
    int  anisotropy;            /* 1..16 */
    char frame_cap[16];         /* "adaptive", "60", "30", "0" */
    int  fps_overlay;           /* 0 or 1 */
    int  fullscreen;            /* 0 or 1: borderless, on the window's monitor */
    int  vrr;                   /* 0 or 1: present for variable refresh when fullscreen */
    int  frame_interp;          /* 1 (off) to 4: frames shown for each the game draws */
    double brightness;          /* 0.5..2.0, 1 as the game made it: a gamma on the final picture */
    int  mute_unfocused;        /* 0 or 1: silence while another window has the focus */
    int  pause_unfocused;       /* 0 or 1: pause the game when the window loses the focus in play */
    int  volume;                /* 0 to 100: percent, for everything the game plays */
    int  fxaa;                  /* 0 or 1: edge smoothing as the picture is resolved */
    int  prompts;               /* button prompts: 0 auto, 1 keyboard, 2 controller */
    int  skip_intro;            /* 0 or 1: skip the videos a title plays before its menu */
    double fov;                 /* vertical field of view in degrees, 0 the game's own;
                                 * only for a title that can set it (recomp_fov_support) */
    double weapon_fov;          /* the held weapon's, the same way, 0 the game's own; only
                                 * for a title that can set it (recomp_weapon_fov_support) */
    char game_dir[512];         /* empty means beside the executable */
} RecompSettings;

void recomp_settings_defaults(RecompSettings *s);

/* Read the file at `path` into `s`, starting from the defaults. Absent
 * keys keep their default, so an older file gains new settings rather
 * than losing them. Returns 0 if the file could not be read at all. */
int  recomp_settings_read(const char *path, RecompSettings *s);

/* Write `s` to `path`, comments and all, creating the directories above
 * it. Returns 0 on failure. This is the only place the file's text is
 * written, by the launcher and by the runtime's first-run default
 * alike. */
int  recomp_settings_write(const char *path, const RecompSettings *s);

/* Where this title's file belongs: RECOMP_DISPLAY_CONFIG if set,
 * otherwise the per-user path for `title_id`. Returns 0 if there is
 * nowhere sensible to put it. */
int  recomp_settings_path(uint32_t title_id, char *out, size_t n);

/* The field of view, for a title project that can set its game's camera
 * (it says so, with the game's own vertical angle, before the game starts).
 * The player's choice is the settings file's `fov` (RECOMP_FOV), in vertical
 * degrees, 0 for the game's own; the in-game menu changes it while the game
 * runs, and the title reads it once a frame. */
#define RECOMP_FOV_MIN 40.0
#define RECOMP_FOV_MAX 90.0
void   recomp_fov_support(double game_degrees);
double recomp_fov_game(void);           /* 0 when the title cannot set it */
double recomp_fov(void);                /* the player's, or 0 */
void   recomp_fov_set(double degrees);

/* The held weapon's field of view, for a title whose first-person weapon is
 * drawn with an angle of its own (a shooter's usually is, narrower than the
 * view's, and widened with it). The setting is `weapon_fov`
 * (RECOMP_WEAPON_FOV): the weapon drawn as at that angle whatever the
 * view's, 0 for the game's own -- the weapon as the game draws it at its own
 * view, so widening the view leaves the weapon alone. */
#define RECOMP_WEAPON_FOV_MIN 30.0
#define RECOMP_WEAPON_FOV_MAX 90.0
void   recomp_weapon_fov_support(double game_degrees);
double recomp_weapon_fov_game(void);    /* 0 when the title cannot set it */
double recomp_weapon_fov(void);         /* the player's, or 0 */
void   recomp_weapon_fov_set(double degrees);

/* The input bindings (src/input/input_bindings.c reads them, the launcher
 * edits them). They are per title, because a keyboard layout is a judgement
 * about one game's controls: the key that throws a grenade in one shooter
 * means nothing in a racer.
 *
 * Read, the first that exists:
 *   1. RECOMP_INPUT_CONFIG (returned even if it does not exist, so the
 *      caller can say so -- it was asked for by name);
 *   2. this title's own, <user dir>\titles\<TITLE ID>.bindings.json, which
 *      the launcher writes;
 *   3. the per-user <user dir>\input_bindings.json every title shares, which
 *      tools.input_ui writes and which was the only one before this;
 *   4. input_bindings.json beside the executable, then in the working
 *      directory: the preset a title project ships with its build.
 * Returns 0 when there is none, which means the built-in defaults.
 *
 * Write: RECOMP_INPUT_CONFIG, else the title's own file (the shared one when
 * the title is not known). Returns 0 if there is nowhere to write.
 *
 * Preset: (4) alone, which is what "reset to defaults" goes back to. */
int  recomp_bindings_read_path(uint32_t title_id, char *out, size_t n);
int  recomp_bindings_write_path(uint32_t title_id, char *out, size_t n);
int  recomp_bindings_preset_path(char *out, size_t n);

#endif /* XBOXRECOMP_RECOMP_CONFIG_H */
