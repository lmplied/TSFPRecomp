/*
 * input_mouse.c -- see input_mouse.h.
 *
 * Motion is read as raw input (WM_INPUT), not from the pointer: the pointer
 * stops at the edge of the window it is held in, accelerates with the
 * Windows pointer speed setting, and moves in whole pixels, none of which a
 * camera should inherit. Raw counts are what the mouse itself reports.
 *
 * The counts go into a ring of one-millisecond buckets, and a poll reads the
 * speed over the last few milliseconds from it. The obvious alternative --
 * hand each poll whatever arrived since the previous one -- makes the answer
 * depend on how often a title polls: Future Perfect reads its pad several
 * times a frame, and most of those reads would see nothing and the rest a
 * burst. A speed over a fixed window gives every poll in a frame the same
 * stick, which is what a title that integrates stick * rate * time expects.
 */
#include "input_mouse.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define RING            256     /* buckets of 1 ms: the longest window */
#define MIN_PRESS_MS    40      /* a click shorter than this still lasts it */
#define WHEEL_PRESS_MS  50
#define WHEEL_GAP_MS    30
#define WHEEL_QUEUE_MAX 8
#define BUTTONS         5

typedef struct { int64_t ms; long dx, dy; } Bucket;

static SRWLOCK  g_lock = SRWLOCK_INIT;
static Bucket   g_ring[RING];
static unsigned g_held;                    /* buttons physically down */
static unsigned g_release_due;             /* up, but not yet MIN_PRESS_MS old */
static int64_t  g_down_at[BUTTONS];
static int      g_wheel_queue[2];          /* notches not yet pressed */
static int      g_wheel_rest;              /* part of a notch (fine wheels) */
static struct { int phase; int64_t until; } g_wheel[2];   /* 0 idle, 1 down, 2 gap */

static volatile LONG g_captured;
static volatile LONG g_wanted;
static volatile LONG g_suspended;
static volatile LONG64 g_direct_until;     /* now_ms() the direct claim runs out */
static volatile LONG64 g_pointer_until;    /* now_ms() the title's pointer runs out */
static volatile LONG g_pointer_took;       /* 0 not pointing, 2 pointing, 1 and took a capture */
static volatile LONG g_pointer_clicks;     /* buttons pressed while pointing, unread */
static volatile LONG g_pointer_wheel;      /* wheel while pointing, unread (120 a notch) */
static volatile LONG g_take_dx, g_take_dy;  /* counts since the last take */
static volatile LONG g_focused = 1;        /* until a window is attached */
static HWND g_hwnd;
static int  g_fullscreen;
static int  g_said_capture;

/* Milliseconds from the performance counter. GetTickCount64 steps in
 * 15.6 ms on most machines, which is coarser than the window. */
static int64_t now_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;

    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (int64_t)(t.QuadPart / (freq.QuadPart / 1000));
}

static void forget_everything(void)
{
    AcquireSRWLockExclusive(&g_lock);
    memset(g_ring, 0, sizeof g_ring);
    g_held = 0;
    g_release_due = 0;
    memset(g_wheel_queue, 0, sizeof g_wheel_queue);
    memset(g_wheel, 0, sizeof g_wheel);
    g_wheel_rest = 0;
    ReleaseSRWLockExclusive(&g_lock);
    InterlockedExchange(&g_take_dx, 0);
    InterlockedExchange(&g_take_dy, 0);
}

int recomp_mouse_captured(void)
{
    return g_captured != 0;
}

void recomp_mouse_take_counts(long *dx, long *dy)
{
    *dx = InterlockedExchange(&g_take_dx, 0);
    *dy = InterlockedExchange(&g_take_dy, 0);
}

/* A claim lasts DIRECT_MS: the title renews it every frame its camera hook
 * steers, so a stretch where the hook stops running -- a vehicle, a
 * cutscene, a menu -- hands the mouse back to the stick by itself. */
#define DIRECT_MS 250

static volatile LONG g_direct_ever;

static void pointer_follow(void);

void recomp_mouse_set_direct(int on)
{
    pointer_follow();
    InterlockedExchange64(&g_direct_until, on ? now_ms() + DIRECT_MS : 0);
    if (on)
        InterlockedExchange(&g_direct_ever, 1);
}

int recomp_mouse_direct(void)
{
    return now_ms() < g_direct_until;
}

int recomp_mouse_aims_directly(void)
{
    return g_direct_ever != 0;
}

void recomp_mouse_declare_direct(void)
{
    InterlockedExchange(&g_direct_ever, 1);
}

void recomp_mouse_feed(long dx, long dy, unsigned down, unsigned up, int wheel)
{
    int64_t ms;
    Bucket *b;
    int i;

    if (!g_captured)
        return;
    InterlockedExchangeAdd(&g_take_dx, (LONG)dx);
    InterlockedExchangeAdd(&g_take_dy, (LONG)dy);
    ms = now_ms();
    AcquireSRWLockExclusive(&g_lock);
    b = &g_ring[ms % RING];
    if (b->ms != ms) {
        b->ms = ms;
        b->dx = 0;
        b->dy = 0;
    }
    b->dx += dx;
    b->dy += dy;

    for (i = 0; i < BUTTONS; i++) {
        unsigned bit = 1u << i;
        if (down & bit) {
            g_held |= bit;
            g_release_due &= ~bit;
            g_down_at[i] = ms;
        }
        if (up & bit) {
            /* A press and its release can arrive in one report, or 5 ms
             * apart from a macro; a title that samples once a frame would
             * see neither. Hold the press for MIN_PRESS_MS whatever. */
            if (ms - g_down_at[i] < MIN_PRESS_MS && (g_held & bit))
                g_release_due |= bit;
            else
                g_held &= ~bit;
        }
    }

    if (wheel) {
        int notches;
        g_wheel_rest += wheel;
        notches = g_wheel_rest / 120;
        g_wheel_rest -= notches * 120;
        if (notches > 0) {
            g_wheel_queue[0] += notches;
            if (g_wheel_queue[0] > WHEEL_QUEUE_MAX)
                g_wheel_queue[0] = WHEEL_QUEUE_MAX;
        } else if (notches < 0) {
            g_wheel_queue[1] -= notches;
            if (g_wheel_queue[1] > WHEEL_QUEUE_MAX)
                g_wheel_queue[1] = WHEEL_QUEUE_MAX;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

unsigned recomp_mouse_buttons(void)
{
    unsigned held;
    int64_t ms;
    int i;

    pointer_follow();
    if (!g_captured)
        return 0;
    ms = now_ms();
    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; i < BUTTONS; i++) {
        unsigned bit = 1u << i;
        if ((g_release_due & bit) && ms - g_down_at[i] >= MIN_PRESS_MS) {
            g_release_due &= ~bit;
            g_held &= ~bit;
        }
    }
    held = g_held;
    ReleaseSRWLockExclusive(&g_lock);
    return held;
}

void recomp_mouse_velocity(unsigned window_ms, double *vx, double *vy)
{
    int64_t now;
    long sx = 0, sy = 0;
    unsigned i;

    *vx = 0;
    *vy = 0;
    if (!g_captured)
        return;
    if (window_ms < 1)
        window_ms = 1;
    if (window_ms > RING - 1)
        window_ms = RING - 1;
    now = now_ms();
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < window_ms; i++) {
        int64_t ms = now - (int64_t)i;
        const Bucket *b = &g_ring[ms % RING];
        if (b->ms == ms) {
            sx += b->dx;
            sy += b->dy;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    *vx = (double)sx / (double)window_ms;
    *vy = (double)sy / (double)window_ms;
}

int recomp_mouse_wheel(int dir)
{
    int64_t ms;
    int down;

    if (!g_captured || dir < 0 || dir > 1)
        return 0;
    ms = now_ms();
    AcquireSRWLockExclusive(&g_lock);
    switch (g_wheel[dir].phase) {
    case 1:
        if (ms >= g_wheel[dir].until) {
            g_wheel[dir].phase = 2;
            g_wheel[dir].until = ms + WHEEL_GAP_MS;
        }
        break;
    case 2:
        if (ms >= g_wheel[dir].until)
            g_wheel[dir].phase = 0;
        break;
    }
    if (g_wheel[dir].phase == 0 && g_wheel_queue[dir] > 0) {
        g_wheel_queue[dir]--;
        g_wheel[dir].phase = 1;
        g_wheel[dir].until = ms + WHEEL_PRESS_MS;
    }
    down = g_wheel[dir].phase == 1;
    ReleaseSRWLockExclusive(&g_lock);
    return down;
}

int recomp_input_focused(void)
{
    return g_focused != 0 && !g_suspended;
}

int recomp_input_window_active(void)
{
    return g_focused != 0;
}

static void refresh_cursor(void);
static void set_capture(HWND hwnd, int on);
static volatile LONG g_suspend_took;        /* suspending let a capture go */

void recomp_input_suspend(int on)
{
    InterlockedExchange(&g_suspended, on ? 1 : 0);
    /* Released from whichever thread asks: ClipCursor is not the window
     * thread's alone, and the pointer is shown at once (refresh_cursor). */
    if (on && g_captured) {
        InterlockedExchange(&g_captured, 0);
        ClipCursor(NULL);
        forget_everything();
        InterlockedExchange(&g_suspend_took, 1);
    }
    /* Given back: the mouse too, if suspending took it and the window is
     * still in front -- closing the settings panel goes straight back to
     * aiming, with no click to take the mouse again. */
    if (!on && InterlockedExchange(&g_suspend_took, 0) && g_hwnd && g_focused)
        set_capture(g_hwnd, 1);
    refresh_cursor();
}

int recomp_input_suspended(void)
{
    return g_suspended != 0;
}

void recomp_mouse_set_wanted(int wanted)
{
    InterlockedExchange(&g_wanted, wanted ? 1 : 0);
}

void recomp_mouse_test_capture(int on)
{
    if (!on)
        forget_everything();
    InterlockedExchange(&g_captured, on ? 1 : 0);
}

/* ---- pointing at the title's menus -------------------------------------- */

/* A title renews its pointer every frame its menu shows (POINTER_MS), so a
 * stretch where it stops asking -- play resumed, a cutscene -- ends it by
 * itself. */
#define POINTER_MS 250

static int pointing(void)
{
    return now_ms() < g_pointer_until;
}

static int pointer_in_window(void)
{
    POINT pt;
    RECT rc;

    return g_hwnd && GetCursorPos(&pt) && ScreenToClient(g_hwnd, &pt) &&
           GetClientRect(g_hwnd, &rc) && pt.x >= 0 && pt.y >= 0 &&
           pt.x < rc.right && pt.y < rc.bottom;
}

/* The pointer's shape is the window thread's to set, at a WM_SETCURSOR,
 * which Windows sends only when the mouse moves: one now, so the pointer
 * shows (or hides) as the menu comes and goes and not at the next nudge. */
static void refresh_cursor(void)
{
    if (g_hwnd && pointer_in_window())
        PostMessageW(g_hwnd, WM_SETCURSOR, (WPARAM)g_hwnd, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
}

int recomp_mouse_pointing(void)
{
    return pointing();
}

/* The pointer ran out -- play resumed: the game gets the mouse without
 * another click, if pointing took it from a capture or the pointer was left
 * over the window, as long as the window is still in front. */
static void pointer_follow(void)
{
    LONG took;

    if (!g_pointer_took || pointing())
        return;
    took = InterlockedExchange(&g_pointer_took, 0);
    if (g_hwnd && g_focused && !g_suspended && (took == 1 || pointer_in_window()))
        set_capture(g_hwnd, 1);
    refresh_cursor();
}

void recomp_mouse_pointer_mode(int on)
{
    static int last = -1, said;

    if (on != last && said < 20) {
        said++;
        fprintf(stderr, "[INPUT] %s\n", on ? "a menu has the pointer"
                                           : "the menu's pointer is put away");
        fflush(stderr);
    }
    last = on;
    if (!on) {
        InterlockedExchange64(&g_pointer_until, 0);
        pointer_follow();
        return;
    }
    if (!pointing()) {
        InterlockedExchange(&g_pointer_clicks, 0);   /* nothing from before it began */
        InterlockedExchange(&g_pointer_wheel, 0);
        InterlockedExchange64(&g_pointer_until, now_ms() + POINTER_MS);
        refresh_cursor();
    }
    InterlockedExchange64(&g_pointer_until, now_ms() + POINTER_MS);
    /* 2: pointing, so when it ends the game may take the mouse (pointer_follow);
     * 1: and it took a capture to point. */
    InterlockedCompareExchange(&g_pointer_took, 2, 0);
    if (g_captured) {
        /* Released from this thread, as recomp_input_suspend does; the
         * pointer shows again at the window's next WM_SETCURSOR. */
        InterlockedExchange(&g_captured, 0);
        ClipCursor(NULL);
        forget_everything();
        InterlockedExchange(&g_pointer_took, 1);
    }
}

int recomp_mouse_pointer(float *x, float *y)
{
    POINT pt;
    RECT rc;

    if (!g_hwnd || !g_focused || g_suspended || !pointing())
        return 0;
    if (!GetCursorPos(&pt) || !ScreenToClient(g_hwnd, &pt) || !GetClientRect(g_hwnd, &rc))
        return 0;
    if (rc.right <= 0 || rc.bottom <= 0 || pt.x < 0 || pt.y < 0 ||
        pt.x >= rc.right || pt.y >= rc.bottom)
        return 0;
    *x = ((float)pt.x + 0.5f) / (float)rc.right;
    *y = ((float)pt.y + 0.5f) / (float)rc.bottom;
    return 1;
}

unsigned recomp_mouse_pointer_clicks(int *wheel)
{
    LONG w = InterlockedExchange(&g_pointer_wheel, 0);

    if (wheel)
        *wheel = (int)(w / 120);
    if (w % 120)
        InterlockedExchangeAdd(&g_pointer_wheel, w % 120);   /* part of a notch stays */
    return (unsigned)InterlockedExchange(&g_pointer_clicks, 0);
}

/* ---- the window side ---------------------------------------------------- */

static void clip_to_client(HWND hwnd)
{
    RECT r;
    POINT tl, br;

    GetClientRect(hwnd, &r);
    tl.x = r.left;
    tl.y = r.top;
    br.x = r.right;
    br.y = r.bottom;
    ClientToScreen(hwnd, &tl);
    ClientToScreen(hwnd, &br);
    r.left = tl.x;
    r.top = tl.y;
    r.right = br.x;
    r.bottom = br.y;
    ClipCursor(&r);
}

static void set_capture(HWND hwnd, int on)
{
    if (on && (!g_wanted || g_suspended || pointing()))
        return;
    if ((g_captured != 0) == (on != 0))
        return;
    if (on) {
        clip_to_client(hwnd);
        SetCursor(NULL);
        forget_everything();
        InterlockedExchange(&g_captured, 1);
        if (!g_said_capture) {
            g_said_capture = 1;
            fprintf(stderr, "[INPUT] the game has the mouse (Alt+Tab gives it back; "
                            "click in the window to hand it over again)\n");
            fflush(stderr);
        }
    } else {
        InterlockedExchange(&g_captured, 0);
        ClipCursor(NULL);
        forget_everything();
    }
}

static void read_raw(HRAWINPUT handle)
{
    RAWINPUT ri;
    UINT size = sizeof ri;
    const RAWMOUSE *m;
    unsigned f, down = 0, up = 0;
    long dx, dy;
    int wheel = 0;

    if (GetRawInputData(handle, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1)
        return;
    if (ri.header.dwType != RIM_TYPEMOUSE)
        return;
    m = &ri.data.mouse;
    if (m->usFlags & MOUSE_MOVE_ABSOLUTE) {
        /* Remote desktop, some virtual machines and tablets report where
         * the pointer is, 0..65535 across the screen, not how far it went. */
        static int have;
        static long last_x, last_y;
        int virt = (m->usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
        long x = MulDiv(m->lLastX, GetSystemMetrics(virt ? SM_CXVIRTUALSCREEN : SM_CXSCREEN), 65535);
        long y = MulDiv(m->lLastY, GetSystemMetrics(virt ? SM_CYVIRTUALSCREEN : SM_CYSCREEN), 65535);
        dx = have ? x - last_x : 0;
        dy = have ? y - last_y : 0;
        last_x = x;
        last_y = y;
        have = 1;
    } else {
        dx = m->lLastX;
        dy = m->lLastY;
    }
    f = m->usButtonFlags;
    if (f & RI_MOUSE_LEFT_BUTTON_DOWN)   down |= RECOMP_MOUSE_LEFT;
    if (f & RI_MOUSE_LEFT_BUTTON_UP)     up   |= RECOMP_MOUSE_LEFT;
    if (f & RI_MOUSE_RIGHT_BUTTON_DOWN)  down |= RECOMP_MOUSE_RIGHT;
    if (f & RI_MOUSE_RIGHT_BUTTON_UP)    up   |= RECOMP_MOUSE_RIGHT;
    if (f & RI_MOUSE_MIDDLE_BUTTON_DOWN) down |= RECOMP_MOUSE_MIDDLE;
    if (f & RI_MOUSE_MIDDLE_BUTTON_UP)   up   |= RECOMP_MOUSE_MIDDLE;
    if (f & RI_MOUSE_BUTTON_4_DOWN)      down |= RECOMP_MOUSE_X1;
    if (f & RI_MOUSE_BUTTON_4_UP)        up   |= RECOMP_MOUSE_X1;
    if (f & RI_MOUSE_BUTTON_5_DOWN)      down |= RECOMP_MOUSE_X2;
    if (f & RI_MOUSE_BUTTON_5_UP)        up   |= RECOMP_MOUSE_X2;
    if (f & RI_MOUSE_WHEEL)
        wheel = (short)m->usButtonData;
    /* Pointing at a menu: the presses and the wheel are the title's to read
     * (recomp_mouse_pointer_clicks), and nothing reaches the bindings. */
    if (!g_captured && pointing() && g_focused && !g_suspended) {
        if (down)
            InterlockedOr(&g_pointer_clicks, (LONG)down);
        if (wheel)
            InterlockedExchangeAdd(&g_pointer_wheel, wheel);
    }
    recomp_mouse_feed(dx, dy, down, up, wheel);
}

void recomp_input_attach_window(void *hwnd_)
{
    HWND hwnd = (HWND)hwnd_;
    RAWINPUTDEVICE rid;

    g_hwnd = hwnd;
    /* No RIDEV_INPUTSINK: raw input arrives only while this window is in
     * front, which is the focus rule for the mouse at no cost. */
    rid.usUsagePage = 0x01;     /* generic desktop */
    rid.usUsage     = 0x02;     /* mouse */
    rid.dwFlags     = 0;
    rid.hwndTarget  = hwnd;
    if (!RegisterRawInputDevices(&rid, 1, sizeof rid))
        fprintf(stderr, "[INPUT] raw mouse input unavailable (error %lu); the mouse "
                        "will not reach the game\n", GetLastError());
    InterlockedExchange(&g_focused, GetForegroundWindow() == hwnd);
}

void recomp_input_window_fullscreen(void *hwnd, int on)
{
    g_fullscreen = on;
    if (on && GetForegroundWindow() == (HWND)hwnd)
        set_capture((HWND)hwnd, 1);
    else if (!on)
        set_capture((HWND)hwnd, 0);   /* a window can be dragged again */
}

int recomp_input_window_message(void *hwnd_, unsigned msg, uintptr_t wp,
                                intptr_t lp, intptr_t *result)
{
    HWND hwnd = (HWND)hwnd_;

    switch (msg) {
    case WM_INPUT:
        read_raw((HRAWINPUT)lp);
        return 0;                       /* DefWindowProc frees it */
    case WM_SETFOCUS:
        InterlockedExchange(&g_focused, 1);
        if (g_fullscreen)
            set_capture(hwnd, 1);
        return 0;
    case WM_KILLFOCUS:
        InterlockedExchange(&g_focused, 0);
        set_capture(hwnd, 0);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE)
            set_capture(hwnd, 0);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        /* The click that hands the mouse over is not a shot: raw input
         * delivered its press before this, outside the capture, where it
         * was dropped. A click on a menu the title is pointing at is the
         * menu's, and captures nothing. */
        if (g_wanted && !g_captured && !g_suspended && !pointing()) {
            set_capture(hwnd, 1);
            *result = msg == WM_XBUTTONDOWN ? TRUE : 0;
            return 1;
        }
        return 0;
    case WM_MOVE:
    case WM_SIZE:
        if (g_captured)
            clip_to_client(hwnd);
        return 0;
    case WM_SETCURSOR:
        if (g_captured && LOWORD(lp) == HTCLIENT) {
            SetCursor(NULL);
            *result = TRUE;
            return 1;
        }
        return 0;
    }
    return 0;
}

#else   /* !_WIN32: no window layer yet, so no mouse; the keyboard reads as before */

void recomp_input_attach_window(void *hwnd) { (void)hwnd; }
int  recomp_input_window_message(void *hwnd, unsigned msg, uintptr_t wp,
                                 intptr_t lp, intptr_t *result)
{
    (void)hwnd; (void)msg; (void)wp; (void)lp; (void)result;
    return 0;
}
void recomp_input_window_fullscreen(void *hwnd, int on) { (void)hwnd; (void)on; }
static int g_suspended;
int  recomp_input_focused(void) { return !g_suspended; }
int  recomp_input_window_active(void) { return 1; }
void recomp_input_suspend(int on) { g_suspended = on; }
int  recomp_input_suspended(void) { return g_suspended; }
void recomp_mouse_set_wanted(int wanted) { (void)wanted; }
unsigned recomp_mouse_buttons(void) { return 0; }
void recomp_mouse_velocity(unsigned window_ms, double *vx, double *vy)
{
    (void)window_ms;
    *vx = 0;
    *vy = 0;
}
int  recomp_mouse_wheel(int dir) { (void)dir; return 0; }
void recomp_mouse_feed(long dx, long dy, unsigned down, unsigned up, int wheel)
{
    (void)dx; (void)dy; (void)down; (void)up; (void)wheel;
}
void recomp_mouse_test_capture(int on) { (void)on; }
int  recomp_mouse_captured(void) { return 0; }
void recomp_mouse_take_counts(long *dx, long *dy) { *dx = 0; *dy = 0; }
void recomp_mouse_set_direct(int on) { (void)on; }
int  recomp_mouse_direct(void) { return 0; }
int  recomp_mouse_aims_directly(void) { return 0; }
void recomp_mouse_declare_direct(void) { }
void recomp_mouse_pointer_mode(int on) { (void)on; }
int  recomp_mouse_pointing(void) { return 0; }
int  recomp_mouse_pointer(float *x, float *y) { (void)x; (void)y; return 0; }
unsigned recomp_mouse_pointer_clicks(int *wheel) { if (wheel) *wheel = 0; return 0; }

#endif
