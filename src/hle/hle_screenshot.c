/*
 * hle_screenshot.c -- F12: the picture on screen, saved as a PNG.
 *
 * The renderer reads the frame back on the presenting thread
 * (xbox_D3D8RequestShot); everything after that -- finding the folder,
 * encoding, writing -- happens on a thread started for the one picture, so
 * the game never waits on a disk. The PNG encoder is SDL's, which the pads
 * already bring in.
 *
 * Where: Pictures\<the game's name, from its XBE>\<name> <date> <time>.png,
 * or a "screenshots" folder beside the executable when there is no Pictures
 * folder; RECOMP_SCREENSHOT_DIR=<folder> says somewhere else. For a scripted
 * run, which cannot press F12, RECOMP_SCREENSHOT_AT=<ms>[,<ms>...] takes one
 * at those times on the input script's clock. The swap chain's alpha is whatever the resolve left there, so it
 * is set opaque before saving: a PNG that is see-through in places is not a
 * screenshot.
 */
#include "hle_screenshot.h"
#include "d3d8_xbox.h"
#include "../kernel/xbox_memory_layout.h"   /* xbox_XbeTitleName */

#include <SDL3/SDL.h>
#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

#define NOTICE_MS 2500

unsigned long long recomp_input_script_ms(void);     /* input_host.c */

typedef struct {
    uint8_t *px;
    UINT w, h;
} Shot;

static SRWLOCK g_result_lock = SRWLOCK_INIT;
static char g_result[200];              /* the last save's line, under the lock */
static volatile LONG g_result_seq;      /* bumped as each save finishes */
static LONG g_seen_seq;
static char g_notice[200];
static ULONGLONG g_notice_until;

/* The game's name as a folder name: anything Windows refuses in one becomes
 * a space, and trailing dots and spaces go. */
static void game_folder_name(wchar_t *out, size_t n)
{
    const char *name = xbox_XbeTitleName();
    size_t i;

    if (!name || !*name || MultiByteToWideChar(CP_UTF8, 0, name, -1, out, (int)n) <= 0)
        wcsncpy(out, L"Xbox game", n);
    out[n - 1] = L'\0';
    for (i = 0; out[i]; i++)
        if (out[i] < 32 || wcschr(L"<>:\"/\\|?*", out[i]))
            out[i] = L' ';
    for (i = wcslen(out); i > 0 && (out[i - 1] == L' ' || out[i - 1] == L'.'); i--)
        out[i - 1] = L'\0';
    if (!out[0])
        wcsncpy(out, L"Xbox game", n);
}

/* The folder to save in, made if need be, and how to name it to the player.
 * 0 when there is nowhere to write. */
static int shot_folder(wchar_t *dir, size_t n, const wchar_t *name, char *shown, size_t shown_n)
{
    PWSTR pics = NULL;
    char name8[200];
    const char *over = getenv("RECOMP_SCREENSHOT_DIR");

    WideCharToMultiByte(CP_UTF8, 0, name, -1, name8, (int)sizeof name8, NULL, NULL);
    if (over && *over) {
        MultiByteToWideChar(CP_UTF8, 0, over, -1, dir, (int)n);
        snprintf(shown, shown_n, "%s", over);
    } else if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Pictures, 0, NULL, &pics)) && pics) {
        _snwprintf(dir, n, L"%s\\%s", pics, name);
        CoTaskMemFree(pics);
        snprintf(shown, shown_n, "Pictures\\%s", name8);
    } else {
        wchar_t *slash;

        if (!GetModuleFileNameW(NULL, dir, (DWORD)n))
            return 0;
        slash = wcsrchr(dir, L'\\');
        if (slash)
            slash[1] = L'\0';
        wcsncat(dir, L"screenshots", n - wcslen(dir) - 1);
        snprintf(shown, shown_n, "the screenshots folder beside the game");
    }
    dir[n - 1] = L'\0';
    CreateDirectoryW(dir, NULL);
    return (GetFileAttributesW(dir) & FILE_ATTRIBUTE_DIRECTORY) &&
           GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES;
}

static DWORD WINAPI save_shot(LPVOID arg)
{
    Shot *s = (Shot *)arg;
    wchar_t dir[MAX_PATH], name[96], wpath[MAX_PATH + 96];
    char path[3 * (MAX_PATH + 96)], shown[160], when[32], line[200];
    SDL_Surface *surf;
    struct tm tm;
    time_t now = time(NULL);
    size_t i, count = (size_t)s->w * s->h;
    int ok = 0, n;

    game_folder_name(name, sizeof name / sizeof name[0]);
    localtime_s(&tm, &now);
    strftime(when, sizeof when, "%Y-%m-%d %H-%M-%S", &tm);
    snprintf(line, sizeof line, "Screenshot not saved: nowhere to write it");
    if (shot_folder(dir, sizeof dir / sizeof dir[0], name, shown, sizeof shown)) {
        /* Two in one second get (2), (3) after the time. */
        for (n = 1; n < 100; n++) {
            if (n == 1)
                _snwprintf(wpath, sizeof wpath / sizeof wpath[0], L"%s\\%s %hs.png", dir, name, when);
            else
                _snwprintf(wpath, sizeof wpath / sizeof wpath[0], L"%s\\%s %hs (%d).png", dir, name,
                           when, n);
            wpath[sizeof wpath / sizeof wpath[0] - 1] = L'\0';
            if (GetFileAttributesW(wpath) == INVALID_FILE_ATTRIBUTES)
                break;
        }
        WideCharToMultiByte(CP_UTF8, 0, wpath, -1, path, (int)sizeof path, NULL, NULL);
        for (i = 0; i < count; i++)
            s->px[i * 4 + 3] = 0xFF;
        surf = SDL_CreateSurfaceFrom((int)s->w, (int)s->h, SDL_PIXELFORMAT_RGBA32, s->px,
                                     (int)(s->w * 4u));
        ok = surf && SDL_SavePNG(surf, path);
        if (ok)
            snprintf(line, sizeof line, "Screenshot saved in %s", shown);
        else
            snprintf(line, sizeof line, "Screenshot not saved: %s", SDL_GetError());
        if (surf)
            SDL_DestroySurface(surf);
        fprintf(stderr, "[SHOT] %ux%u %s %s\n", s->w, s->h, ok ? "->" : "failed:",
                ok ? path : SDL_GetError());
        fflush(stderr);
    }
    free(s->px);
    free(s);
    AcquireSRWLockExclusive(&g_result_lock);
    snprintf(g_result, sizeof g_result, "%s", line);
    ReleaseSRWLockExclusive(&g_result_lock);
    InterlockedIncrement(&g_result_seq);
    return 0;
}

int hle_screenshot_request(void)
{
    xbox_D3D8RequestShot();
    return 1;
}

/* RECOMP_SCREENSHOT_AT: 1 on the first frame past each time. */
int hle_screenshot_scripted(void)
{
    static int parsed, count, next;
    static unsigned long at[16];
    unsigned long long t;

    if (!parsed) {
        const char *s = getenv("RECOMP_SCREENSHOT_AT");
        char *end;

        parsed = 1;
        while (s && *s && count < 16) {
            at[count++] = strtoul(s, &end, 10);
            if (end == s)
                break;
            s = *end == ',' ? end + 1 : end;
        }
    }
    if (next >= count)
        return 0;
    t = recomp_input_script_ms();
    if (t < at[next])
        return 0;
    next++;
    return 1;
}

const char *hle_screenshot_frame(void)
{
    UINT w = 0, h = 0;
    uint8_t *px = xbox_D3D8TakeShot(&w, &h);
    LONG seq;

    if (px) {
        Shot *s = (Shot *)malloc(sizeof *s);
        HANDLE t = NULL;

        if (s) {
            s->px = px;
            s->w = w;
            s->h = h;
            t = CreateThread(NULL, 0, save_shot, s, 0, NULL);
        }
        if (t) {
            CloseHandle(t);
        } else {
            free(px);
            free(s);
        }
    }
    seq = InterlockedCompareExchange(&g_result_seq, 0, 0);
    if (seq != g_seen_seq) {
        g_seen_seq = seq;
        AcquireSRWLockShared(&g_result_lock);
        snprintf(g_notice, sizeof g_notice, "%s", g_result);
        ReleaseSRWLockShared(&g_result_lock);
        g_notice_until = GetTickCount64() + NOTICE_MS;
    }
    return GetTickCount64() < g_notice_until ? g_notice : NULL;
}
