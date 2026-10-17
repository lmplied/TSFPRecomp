/*
 * recomp_pad.c -- the host's game pads through SDL3, or XInput on Windows.
 * recomp_pad.h says what this is for; the notes here are about how.
 *
 * SDL3 is started with the gamepad subsystem only: no window, no event
 * loop. So nothing here waits for SDL events. The pad list is re-read every
 * SCAN_MS, pad state is refreshed with SDL_UpdateGamepads at most once a
 * millisecond however often a title polls, and the joystick and gamepad
 * event types are switched off so an event queue nobody drains cannot
 * grow. On Windows SDL's raw-input work runs on its own thread
 * (SDL_HINT_JOYSTICK_THREAD), because the threads that poll here are the
 * title's.
 */
#include "recomp_pad.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <xinput.h>
#endif

#define SCAN_MS       250u     /* how often the SDL3 pad list is re-read */
#define RUMBLE_MS   30000u     /* SDL rumble has a duration; XInput's does not */

static int g_api = -1;         /* resolved on first use */
static int g_api_asked = RECOMP_PAD_API_SDL;

static int env_api(void)
{
    const char *v = getenv("RECOMP_PAD_API");
    if (!v || !*v)
        return -1;
    if (SDL_strcasecmp(v, "xinput") == 0)
        return RECOMP_PAD_API_XINPUT;
    if (SDL_strcasecmp(v, "sdl") == 0 || SDL_strcasecmp(v, "sdl3") == 0)
        return RECOMP_PAD_API_SDL;
    fprintf(stderr, "[INPUT] RECOMP_PAD_API=%s is not sdl or xinput; ignored\n", v);
    return -1;
}

void recomp_pad_set_api(int api)
{
    g_api_asked = api == RECOMP_PAD_API_XINPUT ? RECOMP_PAD_API_XINPUT : RECOMP_PAD_API_SDL;
    g_api = -1;         /* resolved again on the next read: the launcher flips it live */
}

/* ---- SDL3 ------------------------------------------------------------- */

typedef struct {
    SDL_Gamepad   *pad;
    SDL_JoystickID id;
    uint16_t       rumble_low, rumble_high;
    uint64_t       rumble_at;
} SdlSlot;

static SdlSlot         g_sdl[RECOMP_PAD_SLOTS];
static SDL_SpinLock    g_lock;
static int             g_sdl_state;   /* 0 not tried, 1 running, -1 failed */
static uint64_t        g_scan_at, g_update_at;

static int sdl_start(void)
{
    Uint32 t;
    int v;

    if (g_sdl_state)
        return g_sdl_state > 0;
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    /* The window the player looks at may not be SDL's, so SDL cannot know
     * it has focus: read the pads regardless, as XInput always did. */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "[INPUT] SDL3 could not start its gamepad support (%s)\n",
                SDL_GetError());
        g_sdl_state = -1;
        return 0;
    }
    for (t = SDL_EVENT_JOYSTICK_AXIS_MOTION; t < SDL_EVENT_FINGER_DOWN; t++)
        SDL_SetEventEnabled(t, false);
    v = SDL_GetVersion();
    fprintf(stderr, "[INPUT] pads through SDL3 %d.%d.%d (RECOMP_PAD_API=xinput for XInput)\n",
            SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
    fflush(stderr);
    g_sdl_state = 1;
    return 1;
}

/* Called with g_lock held. Drops pads that went away and seats new ones in
 * the first free slot. */
static void sdl_scan(uint64_t now)
{
    SDL_JoystickID *ids;
    int n = 0, i, s;

    for (s = 0; s < RECOMP_PAD_SLOTS; s++)
        if (g_sdl[s].pad && !SDL_GamepadConnected(g_sdl[s].pad)) {
            fprintf(stderr, "[INPUT] pad %d disconnected\n", s + 1);
            SDL_CloseGamepad(g_sdl[s].pad);
            memset(&g_sdl[s], 0, sizeof g_sdl[s]);
        }
    ids = SDL_GetGamepads(&n);
    for (i = 0; ids && i < n; i++) {
        int seated = 0, free_slot = -1;
        for (s = 0; s < RECOMP_PAD_SLOTS; s++) {
            if (g_sdl[s].pad && g_sdl[s].id == ids[i])
                seated = 1;
            else if (!g_sdl[s].pad && free_slot < 0)
                free_slot = s;
        }
        if (seated || free_slot < 0)
            continue;
        g_sdl[free_slot].pad = SDL_OpenGamepad(ids[i]);
        if (g_sdl[free_slot].pad) {
            const char *name = SDL_GetGamepadName(g_sdl[free_slot].pad);
            g_sdl[free_slot].id = ids[i];
            fprintf(stderr, "[INPUT] pad %d: %s\n", free_slot + 1, name ? name : "(unnamed)");
        }
    }
    SDL_free(ids);
    fflush(stderr);
    g_scan_at = now + SCAN_MS;
}

static int16_t flip_y(Sint16 v)
{
    return v == -32768 ? (int16_t)32767 : (int16_t)-v;
}

static int sdl_read(int slot, RecompPadState *out)
{
    SDL_Gamepad *p;
    uint64_t now;
    uint16_t b = 0;

    SDL_LockSpinlock(&g_lock);          /* first use starts SDL, once */
    if (!sdl_start()) {
        SDL_UnlockSpinlock(&g_lock);
        return 0;
    }
    now = SDL_GetTicks();
    if (now != g_update_at) {
        SDL_UpdateGamepads();
        g_update_at = now;
    }
    if (now >= g_scan_at)
        sdl_scan(now);
    p = g_sdl[slot].pad;
    if (p && (g_sdl[slot].rumble_low || g_sdl[slot].rumble_high) &&
        now - g_sdl[slot].rumble_at > RUMBLE_MS / 2) {
        SDL_RumbleGamepad(p, g_sdl[slot].rumble_low, g_sdl[slot].rumble_high, RUMBLE_MS);
        g_sdl[slot].rumble_at = now;
    }
    SDL_UnlockSpinlock(&g_lock);
    if (!p)
        return 0;

    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_UP))        b |= RECOMP_PAD_DPAD_UP;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_DOWN))      b |= RECOMP_PAD_DPAD_DOWN;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_LEFT))      b |= RECOMP_PAD_DPAD_LEFT;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_RIGHT))     b |= RECOMP_PAD_DPAD_RIGHT;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_START))          b |= RECOMP_PAD_START;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_BACK))           b |= RECOMP_PAD_BACK;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_LEFT_STICK))     b |= RECOMP_PAD_LEFT_THUMB;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_RIGHT_STICK))    b |= RECOMP_PAD_RIGHT_THUMB;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))  b |= RECOMP_PAD_LEFT_SHOULDER;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) b |= RECOMP_PAD_RIGHT_SHOULDER;
    /* By position, as SDL3 names them: the bottom face button is A on an
     * Xbox pad, Cross on a PlayStation one and B on a Nintendo one, and it
     * is the button a title asking for A expects under the right thumb. */
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_SOUTH))          b |= RECOMP_PAD_A;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_EAST))           b |= RECOMP_PAD_B;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_WEST))           b |= RECOMP_PAD_X;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_NORTH))          b |= RECOMP_PAD_Y;
    out->buttons = b;
    /* Triggers are 0..32767 in SDL, 0..255 in XInput. */
    out->left_trigger  = (uint8_t)(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7);
    out->right_trigger = (uint8_t)(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7);
    /* SDL's Y axes point down and XInput's up. Negated, with -32768 (full
     * down in SDL) clamped to 32767 so it does not overflow, and 0 staying 0
     * at rest. */
    out->lx = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX);
    out->ly = flip_y(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY));
    out->rx = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTX);
    out->ry = flip_y(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTY));
    return 1;
}

/* ---- XInput (Windows) ----------------------------------------------------- */

#if defined(_WIN32)

/* XInputGetState on an empty slot costs about a millisecond, and a title
 * polls its pads several times a frame: an unplugged port would cost more
 * than the game. An empty slot is re-checked once a second, a full one
 * every poll. */
static int xinput_read(int slot, RecompPadState *out)
{
    static int connected[RECOMP_PAD_SLOTS];
    static ULONGLONG retry_at[RECOMP_PAD_SLOTS];
    ULONGLONG now = GetTickCount64();
    XINPUT_STATE st;

    if (!connected[slot] && now < retry_at[slot])
        return 0;
    memset(&st, 0, sizeof st);
    if (XInputGetState((DWORD)slot, &st) != ERROR_SUCCESS) {
        connected[slot] = 0;
        retry_at[slot] = now + 1000;
        return 0;
    }
    connected[slot] = 1;
    out->buttons = st.Gamepad.wButtons;
    out->left_trigger = st.Gamepad.bLeftTrigger;
    out->right_trigger = st.Gamepad.bRightTrigger;
    out->lx = st.Gamepad.sThumbLX;
    out->ly = st.Gamepad.sThumbLY;
    out->rx = st.Gamepad.sThumbRX;
    out->ry = st.Gamepad.sThumbRY;
    return 1;
}

#endif

/* ---- the interface ---------------------------------------------------------- */

int recomp_pad_api(void)
{
    if (g_api < 0) {
        int e = env_api();
        g_api = e >= 0 ? e : g_api_asked;
#if !defined(_WIN32)
        g_api = RECOMP_PAD_API_SDL;
#endif
        if (g_api == RECOMP_PAD_API_XINPUT) {
            fprintf(stderr, "[INPUT] pads through XInput (Xbox controllers only)\n");
            fflush(stderr);
        }
    }
    return g_api;
}

const char *recomp_pad_api_name(void)
{
    return recomp_pad_api() == RECOMP_PAD_API_XINPUT ? "XInput" : "SDL3";
}

/* recomp_pad_test_set: slots a test has filled. */
static int            g_test_set[RECOMP_PAD_SLOTS];
static RecompPadState g_test_state[RECOMP_PAD_SLOTS];

void recomp_pad_test_set(int slot, const RecompPadState *state)
{
    if (slot < 0 || slot >= RECOMP_PAD_SLOTS)
        return;
    g_test_set[slot] = state != NULL;
    if (state)
        g_test_state[slot] = *state;
}

int recomp_pad_read(int slot, RecompPadState *out)
{
    if (!out)
        return 0;
    memset(out, 0, sizeof *out);
    if (slot < 0 || slot >= RECOMP_PAD_SLOTS)
        return 0;
    if (g_test_set[slot]) {
        *out = g_test_state[slot];
        return 1;
    }
#if defined(_WIN32)
    if (recomp_pad_api() == RECOMP_PAD_API_XINPUT)
        return xinput_read(slot, out);
#else
    (void)recomp_pad_api();
#endif
    return sdl_read(slot, out);
}

void recomp_pad_name(int slot, char *out, size_t n)
{
    if (!out || !n)
        return;
    out[0] = '\0';
    if (slot < 0 || slot >= RECOMP_PAD_SLOTS)
        return;
#if defined(_WIN32)
    if (recomp_pad_api() == RECOMP_PAD_API_XINPUT) {
        RecompPadState st;
        if (xinput_read(slot, &st))
            snprintf(out, n, "Xbox controller");
        return;
    }
#endif
    {
        RecompPadState st;
        const char *name = NULL;
        if (!sdl_read(slot, &st))
            return;
        SDL_LockSpinlock(&g_lock);
        if (g_sdl[slot].pad)
            name = SDL_GetGamepadName(g_sdl[slot].pad);
        snprintf(out, n, "%s", name ? name : "Gamepad");
        SDL_UnlockSpinlock(&g_lock);
    }
}

void recomp_pad_rumble(int slot, uint16_t low, uint16_t high)
{
    if (slot < 0 || slot >= RECOMP_PAD_SLOTS)
        return;
#if defined(_WIN32)
    if (recomp_pad_api() == RECOMP_PAD_API_XINPUT) {
        XINPUT_VIBRATION v;
        v.wLeftMotorSpeed = low;
        v.wRightMotorSpeed = high;
        XInputSetState((DWORD)slot, &v);
        return;
    }
#endif
    SDL_LockSpinlock(&g_lock);
    if (!sdl_start()) {
        SDL_UnlockSpinlock(&g_lock);
        return;
    }
    g_sdl[slot].rumble_low = low;
    g_sdl[slot].rumble_high = high;
    if (g_sdl[slot].pad) {
        SDL_RumbleGamepad(g_sdl[slot].pad, low, high, (low || high) ? RUMBLE_MS : 0);
        g_sdl[slot].rumble_at = SDL_GetTicks();
    }
    SDL_UnlockSpinlock(&g_lock);
}
