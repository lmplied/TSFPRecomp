#include "input_bindings.h"
#include "input_mouse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static int g_fail;

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL " __VA_ARGS__); \
                                              fputc('\n', stderr); g_fail = 1; } } while (0)

static XBOX_GAMEPAD sample(void)
{
    XBOX_GAMEPAD pad;
    recomp_bindings_sample(0, &pad);
    return pad;
}

/* A game with an aim mode toggled by clicking the right stick, polling its
 * pad once a frame; `refusing` it ignores the click (it cannot aim now). */
static int g_aim, g_refusing, g_clicks;

static int game_aim_on(unsigned port)
{
    (void)port;
    return g_aim;
}

static void play(int ms)
{
    static int was;
    ULONGLONG end = GetTickCount64() + (ULONGLONG)ms;

    while (GetTickCount64() < end) {
        XBOX_GAMEPAD pad = sample();
        int down = (pad.wButtons & 0x0080) != 0;     /* XINPUT right thumb */

        if (down && !was) {
            g_clicks++;
            if (!g_refusing)
                g_aim = !g_aim;
        }
        was = down;
        Sleep(16);
    }
}

int main(void)
{
    char dir[MAX_PATH], path[MAX_PATH];
    FILE *f;
    XBOX_GAMEPAD pad;
    int stick_at_deadzone;

    GetTempPathA(MAX_PATH, dir);
    snprintf(path, sizeof path, "%sxbr-mouse-%lu.json", dir, GetCurrentProcessId());
    f = fopen(path, "wb");
    if (!f)
        return 10;
    /* A title whose own stick deadzone is 8000: any motion must clear it. */
    fputs("{ \"version\": 1, \"controllers\": [ { \"port\": 1, \"device\": \"keyboard\",\n"
          "  \"mouse_sensitivity\": 1.0, \"mouse_deadzone\": 8000, \"mouse_window_ms\": 16,\n"
          "  \"bindings\": { \"rstick_right\": \"mouse:x+\", \"rstick_left\": \"mouse:x-\",\n"
          "    \"rstick_up\": \"mouse:y+\", \"rstick_down\": \"mouse:y-\",\n"
          "    \"rtrigger\": \"mouse:left\", \"y\": \"mouse:wheel_up\",\n"
          "    \"rthumb\": \"mouse:right\" },\n"
          "  \"hold\": [\"rthumb\"] } ] }\n", f);
    fclose(f);
    SetEnvironmentVariableA("RECOMP_INPUT_CONFIG", path);
    _putenv_s("RECOMP_INPUT_CONFIG", path);
    recomp_bindings_init();

    /* Outside a capture the mouse is not the game's. */
    recomp_mouse_feed(500, 0, RECOMP_MOUSE_LEFT, 0, 120);
    pad = sample();
    CHECK(pad.sThumbRX == 0 && pad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 0 &&
          pad.bAnalogButtons[XBOX_BUTTON_Y] == 0,
          "an uncaptured mouse moved the stick (%d) or pressed (%d, %d)", pad.sThumbRX,
          pad.bAnalogButtons[XBOX_BUTTON_RTRIGGER], pad.bAnalogButtons[XBOX_BUTTON_Y]);

    recomp_mouse_test_capture(1);

    /* 48 counts in a 16 ms window is 3 a millisecond: half of full speed. */
    recomp_mouse_feed(48, 0, 0, 0, 0);
    pad = sample();
    CHECK(pad.sThumbRX > 18000 && pad.sThumbRX < 23000,
          "half speed right gave rx %d, expected about 20400", pad.sThumbRX);
    CHECK(pad.sThumbRY == 0, "horizontal motion moved ry to %d", pad.sThumbRY);

    /* Faster than full speed saturates rather than wrapping. */
    recomp_mouse_feed(-5000, 0, 0, 0, 0);
    pad = sample();
    CHECK(pad.sThumbRX < -32000, "a fast flick left gave rx %d", pad.sThumbRX);

    /* Once the window has passed with no motion, the stick is centred. */
    Sleep(40);
    pad = sample();
    CHECK(pad.sThumbRX == 0, "a still mouse left rx at %d", pad.sThumbRX);

    /* The slowest motion lands just past the title's deadzone. */
    recomp_mouse_feed(1, 0, 0, 0, 0);
    pad = sample();
    stick_at_deadzone = pad.sThumbRX;
    CHECK(stick_at_deadzone > 8000 && stick_at_deadzone < 8600,
          "one count gave rx %d, expected just past 8000", stick_at_deadzone);

    /* Moving the mouse away from the player is stick up; the device's y
     * grows toward the player. */
    Sleep(40);
    recomp_mouse_feed(0, -48, 0, 0, 0);
    pad = sample();
    CHECK(pad.sThumbRY > 18000 && pad.sThumbRX == 0,
          "mouse forward gave ry %d rx %d, expected ry about +20400",
          pad.sThumbRY, pad.sThumbRX);

    /* A click whose release arrives at once still lasts long enough to see. */
    recomp_mouse_feed(0, 0, RECOMP_MOUSE_LEFT, RECOMP_MOUSE_LEFT, 0);
    pad = sample();
    CHECK(pad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 255,
          "a quick click did not pull the trigger (%d)", pad.bAnalogButtons[XBOX_BUTTON_RTRIGGER]);
    Sleep(60);
    pad = sample();
    CHECK(pad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 0,
          "the trigger was still pulled 60 ms after the click (%d)",
          pad.bAnalogButtons[XBOX_BUTTON_RTRIGGER]);

    /* Held is held. */
    recomp_mouse_feed(0, 0, RECOMP_MOUSE_LEFT, 0, 0);
    Sleep(60);
    pad = sample();
    CHECK(pad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 255, "a held button let go");
    recomp_mouse_feed(0, 0, 0, RECOMP_MOUSE_LEFT, 0);
    pad = sample();
    CHECK(pad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 0, "a released button stayed down");

    /* One wheel notch: one press, then nothing. */
    recomp_mouse_feed(0, 0, 0, 0, 120);
    pad = sample();
    CHECK(pad.bAnalogButtons[XBOX_BUTTON_Y] == 255, "a wheel notch did not press Y");
    Sleep(100);
    pad = sample();
    CHECK(pad.bAnalogButtons[XBOX_BUTTON_Y] == 0, "a wheel notch held Y for 100 ms");

    /* A title turning its camera by the counts: motion reaches no stick, the
     * counts are there to take, and the claim lapses unless renewed. */
    {
        long tdx, tdy;

        recomp_mouse_take_counts(&tdx, &tdy);
        recomp_mouse_set_direct(1);
        recomp_mouse_feed(48, -7, 0, 0, 0);
        pad = sample();
        CHECK(pad.sThumbRX == 0 && pad.sThumbRY == 0,
              "direct, the mouse still moved the stick to %d,%d", pad.sThumbRX, pad.sThumbRY);
        recomp_mouse_take_counts(&tdx, &tdy);
        CHECK(tdx == 48 && tdy == -7, "took %ld,%ld counts, expected 48,-7", tdx, tdy);
        recomp_mouse_take_counts(&tdx, &tdy);
        CHECK(tdx == 0 && tdy == 0, "counts taken twice: %ld,%ld", tdx, tdy);
        Sleep(300);
        CHECK(!recomp_mouse_direct(), "a direct claim not renewed for 300 ms still held");
        recomp_mouse_feed(48, 0, 0, 0, 0);
        pad = sample();
        CHECK(pad.sThumbRX > 18000, "the claim lapsed, but the mouse gave rx %d", pad.sThumbRX);
        Sleep(40);
    }

    /* The in-game menu has the input: the game sees an untouched pad. */
    recomp_mouse_feed(48, 0, RECOMP_MOUSE_LEFT, 0, 0);
    recomp_input_suspend(1);
    pad = sample();
    CHECK(pad.sThumbRX == 0 && pad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 0,
          "suspended, the game still read rx %d trigger %d", pad.sThumbRX,
          pad.bAnalogButtons[XBOX_BUTTON_RTRIGGER]);
    CHECK(!recomp_input_focused(), "suspended, the keyboard still read as the game's");
    recomp_input_suspend(0);
    recomp_mouse_test_capture(1);
    recomp_mouse_feed(0, 0, 0, RECOMP_MOUSE_LEFT, 0);
    Sleep(60);

    /* Giving the mouse back drops what it was doing. */
    recomp_mouse_feed(48, 0, RECOMP_MOUSE_LEFT, 0, 0);
    recomp_mouse_test_capture(0);
    pad = sample();
    CHECK(pad.sThumbRX == 0 && pad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 0,
          "released, the mouse still read rx %d trigger %d", pad.sThumbRX,
          pad.bAnalogButtons[XBOX_BUTTON_RTRIGGER]);

    /* Hold to aim: the right button held is aim on, let go is aim off, and
     * each is one click of the stick however long the button stays. */
    recomp_bindings_toggle_reader("rthumb", game_aim_on);
    recomp_mouse_test_capture(1);
    recomp_mouse_feed(0, 0, RECOMP_MOUSE_RIGHT, 0, 0);
    play(400);
    CHECK(g_aim == 1 && g_clicks == 1, "holding: aim %d after %d clicks", g_aim, g_clicks);
    play(300);
    CHECK(g_aim == 1 && g_clicks == 1, "still holding: aim %d after %d clicks", g_aim, g_clicks);
    recomp_mouse_feed(0, 0, 0, RECOMP_MOUSE_RIGHT, 0);
    play(400);
    CHECK(g_aim == 0 && g_clicks == 2, "let go: aim %d after %d clicks", g_aim, g_clicks);

    /* The game will not aim just now: a few tries, then nothing until the
     * button changes. */
    g_clicks = 0;
    g_refusing = 1;
    recomp_mouse_feed(0, 0, RECOMP_MOUSE_RIGHT, 0, 0);
    play(1500);
    CHECK(g_clicks == 3, "a refused aim was clicked %d times, expected 3", g_clicks);
    g_refusing = 0;
    recomp_mouse_feed(0, 0, 0, RECOMP_MOUSE_RIGHT, 0);
    play(300);
    CHECK(g_aim == 0, "after the refusal and a release, aim %d", g_aim);

    /* Hold off: the button is the stick click itself, a toggle. */
    g_clicks = 0;
    recomp_bindings_set_hold(0, "rthumb", 0);
    recomp_mouse_feed(0, 0, RECOMP_MOUSE_RIGHT, 0, 0);
    play(200);
    recomp_mouse_feed(0, 0, 0, RECOMP_MOUSE_RIGHT, 0);
    play(200);
    CHECK(g_aim == 1 && g_clicks == 1, "toggle: aim %d after %d clicks", g_aim, g_clicks);
    recomp_mouse_test_capture(0);

    DeleteFileA(path);
    if (g_fail)
        return 1;
    puts("PASS: mouse motion, buttons and wheel reach the bound controls only while captured; "
         "speed scales the stick and any motion clears the title's deadzone; a direct claim "
         "keeps motion off the stick until it lapses; a held button holds a toggle");
    return 0;
}
