/*
 * test_config -- the settings file, parsed without a game.
 *
 * Covers the things that decide whether a player's choice actually
 * reaches the runtime: that a value is found, that the environment beats
 * the file, that a boolean reads the way the rest of the runtime reads
 * booleans, and that rubbish falls back instead of failing.
 */
#include "recomp_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("  FAIL: %s\n", what);
        failures++;
    }
}

static void check_str(const char *got, const char *want, const char *what)
{
    int ok = (got && want) ? strcmp(got, want) == 0 : got == want;

    if (!ok) {
        printf("  FAIL: %s (got %s, want %s)\n", what,
               got ? got : "(null)", want ? want : "(null)");
        failures++;
    }
}

static void check_int(int got, int want, const char *what)
{
    if (got != want) {
        printf("  FAIL: %s (got %d, want %d)\n", what, got, want);
        failures++;
    }
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");

    if (!f) {
        printf("  FAIL: cannot write %s\n", path);
        failures++;
        return;
    }
    fputs(text, f);
    fclose(f);
}

int main(void)
{
    const char *path = "test_config_tmp.conf";

    write_file(path,
        "# a comment\n"
        "; another\n"
        "\n"
        "resolution_scale = 2\n"
        "widescreen = 1\n"
        "hor_plus=0.75\n"                 /* no spaces */
        "   anisotropy   =   16   \n"     /* lots of spaces */
        "frame_cap = adaptive\n"
        "empty =\n"
        "nonsense line with no equals\n"
        "unknown_key = 7\n");

    /* The named file wins outright, which is how the test reaches the
     * parser without a per-user directory or a title id. */
#if defined(_WIN32)
    _putenv_s("RECOMP_DISPLAY_CONFIG", path);
#else
    setenv("RECOMP_DISPLAY_CONFIG", path, 1);
#endif

    printf("test_config\n");

    check_str(recomp_config_lookup(NULL, "resolution_scale"), "2", "plain value");
    check_str(recomp_config_lookup(NULL, "hor_plus"), "0.75", "no spaces around =");
    check_str(recomp_config_lookup(NULL, "anisotropy"), "16", "spaces trimmed");
    check_str(recomp_config_lookup(NULL, "frame_cap"), "adaptive", "word value");
    check_str(recomp_config_lookup(NULL, "empty"), NULL, "empty value reads as absent");
    check_str(recomp_config_lookup(NULL, "missing"), NULL, "absent key");
    check(recomp_config_path() != NULL, "path reported");

    check_int(recomp_config_int(NULL, "resolution_scale", 1), 2, "int");
    check_int(recomp_config_int(NULL, "frame_cap", 99), 99, "int fallback on a word");
    check_int(recomp_config_int(NULL, "missing", 5), 5, "int fallback when absent");
    check(recomp_config_float(NULL, "hor_plus", 0.0) > 0.74 &&
          recomp_config_float(NULL, "hor_plus", 0.0) < 0.76, "float");

    /* The boolean is the one that decides widescreen, so it gets the
     * whole vocabulary the rest of the runtime accepts. */
    check_int(recomp_config_bool(NULL, "widescreen", 0), 1, "bool 1 from file");
    check_int(recomp_config_bool(NULL, "missing", 0), 0, "bool fallback off");
    check_int(recomp_config_bool(NULL, "missing", 1), 1, "bool fallback on");

    /* Environment beats the file, and an empty variable means on. */
#if defined(_WIN32)
    _putenv_s("RECOMP_TEST_SCALE", "4");
    _putenv_s("RECOMP_TEST_WIDE", "0");
    _putenv_s("RECOMP_TEST_EMPTY", "");
#else
    setenv("RECOMP_TEST_SCALE", "4", 1);
    setenv("RECOMP_TEST_WIDE", "0", 1);
    setenv("RECOMP_TEST_EMPTY", "", 1);
#endif
    check_str(recomp_config_lookup("RECOMP_TEST_SCALE", "resolution_scale"), "4",
              "environment beats the file");
    check_int(recomp_config_bool("RECOMP_TEST_WIDE", "widescreen", 0), 0,
              "environment off beats file on");
    check_int(recomp_config_bool("RECOMP_TEST_EMPTY", "widescreen", 0), 1,
              "empty environment variable means on");
    check_str(recomp_config_lookup("RECOMP_TEST_UNSET", "resolution_scale"), "2",
              "unset variable falls through to the file");

    /* The round trip is the contract between the launcher, which writes
     * the file, and the runtime, which reads it. Write settings out, read
     * them back, and they must be the same settings. */
    {
        const char *rt = "test_config_roundtrip.conf";
        RecompSettings out, back;

        recomp_settings_defaults(&out);
        out.resolution_scale  = 3;
        out.widescreen        = 1;
        out.hor_plus          = 0.75;
        out.hor_plus_register = 60;
        out.anisotropy        = 16;
        out.fps_overlay       = 1;
        out.brightness        = 1.35;
        out.mute_unfocused    = 1;
        out.pause_unfocused   = 0;
        out.volume            = 65;
        out.fxaa              = 1;
        out.prompts           = 2;
        out.skip_intro        = 1;
        out.fov               = 75.0;
        out.weapon_fov        = 60.0;
        snprintf(out.frame_cap, sizeof out.frame_cap, "60");
        snprintf(out.game_dir, sizeof out.game_dir, "D:\\games\\ts2");

        check(recomp_settings_write(rt, &out) != 0, "settings written");
        check(recomp_settings_read(rt, &back) != 0, "settings read back");
        check_int(back.resolution_scale, 3, "round trip: scale");
        check_int(back.widescreen, 1, "round trip: widescreen");
        check(back.hor_plus > 0.74 && back.hor_plus < 0.76, "round trip: hor_plus");
        check_int(back.hor_plus_register, 60, "round trip: register");
        check_int(back.anisotropy, 16, "round trip: anisotropy");
        check_int(back.fps_overlay, 1, "round trip: overlay");
        check(back.brightness > 1.34 && back.brightness < 1.36, "round trip: brightness");
        check_int(back.mute_unfocused, 1, "round trip: mute in the background");
        check_int(back.pause_unfocused, 0, "round trip: pause in the background");
        check_int(back.volume, 65, "round trip: volume");
        check_int(back.fxaa, 1, "round trip: edge smoothing");
        check_int(back.prompts, 2, "round trip: button prompts");
        check_int(back.skip_intro, 1, "round trip: skip intro");
        check(back.fov > 74.9 && back.fov < 75.1, "round trip: field of view");
        check(back.weapon_fov > 59.9 && back.weapon_fov < 60.1, "round trip: weapon field of view");
        check_str(back.frame_cap, "60", "round trip: frame cap");
        check_str(back.game_dir, "D:\\games\\ts2", "round trip: game dir");

        /* A file written by an older build, missing newer keys, must keep
         * the defaults for them rather than zeroing them. */
        write_file(rt, "resolution_scale = 4\n");
        check(recomp_settings_read(rt, &back) != 0, "partial file read");
        check_int(back.resolution_scale, 4, "partial: the key that is there");
        check_int(back.anisotropy, 1, "partial: absent key keeps its default");
        check_str(back.frame_cap, "adaptive", "partial: absent word keeps default");
        check(back.brightness == 1.0, "partial: brightness defaults to as drawn");
        check_int(back.skip_intro, 0, "partial: intros play by default");
        check_int(back.pause_unfocused, 1, "partial: pausing in the background is on by default");
        check_int(back.volume, 100, "partial: full volume by default");
        check(back.weapon_fov == 0.0, "partial: the weapon is the game's own by default");

        /* Out-of-range values are clamped, not taken literally: a hand
         * edited 99 must not ask for a 99x render. */
        write_file(rt, "resolution_scale = 99\nanisotropy = 0\nbrightness = 9\n"
                       "weapon_fov = 10\n");
        check(recomp_settings_read(rt, &back) != 0, "clamped file read");
        check_int(back.resolution_scale, 8, "clamp: scale to 8");
        check_int(back.anisotropy, 1, "clamp: anisotropy to 1");
        check(back.brightness == 2.0, "clamp: brightness to 2");
        check(back.weapon_fov == RECOMP_WEAPON_FOV_MIN, "clamp: weapon field of view to 30");

        remove(rt);
    }

    /* Reading settings must not disturb the lookup table the runtime is
     * already using -- the launcher does both in one process. */
    check_str(recomp_config_lookup(NULL, "resolution_scale"), "2",
              "lookups survive a settings read");

    remove(path);
    if (failures)
        printf("test_config: %d FAILURE(S)\n", failures);
    else
        printf("test_config: all checks passed\n");
    return failures ? 1 : 0;
}
