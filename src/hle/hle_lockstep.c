/*
 * hle_lockstep.c -- see hle_lockstep.h.
 */
#include "hle_lockstep.h"
#include <windows.h>               /* Sleep, GetTickCount: the jitter */
#include "hle.h"
#include "xbox_memory_layout.h"   /* the lockstep vblank clock */
#include "hle_netplay.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static volatile unsigned long g_frame;

unsigned long hle_lockstep_frame(void)
{
    return g_frame;
}

/* The ranges guest state lives in: the image, stacks and heap in the low
 * 64 MB, and the contiguous window, which has storage of its own. */
static const struct { uint32_t va, size; } k_ranges[] = {
    { 0x00010000u, 0x03FF0000u },
    { 0x80000000u, 0x04000000u },
};

/* RECOMP_STATE_HASH_RANGE=<va>:<size>[,...]: hash only those, to look every
 * frame at the parts that matter without the time to hash 128 MB. */
static uint32_t g_only_va[8], g_only_size[8];
static int      g_only_count;

#define CHUNK 0x10000u

/* FNV-1a over 64-bit words, folded to 32: quick, and a difference in any
 * word shows. */
static uint32_t chunk_hash(const uint64_t *p, size_t words)
{
    uint64_t h = 0xCBF29CE484222325ull;
    size_t i;

    for (i = 0; i < words; i++) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return (uint32_t)(h ^ (h >> 32));
}

static void write_hashes(FILE *f, unsigned long frame)
{
    size_t r;

    fprintf(f, "F %lu free %ld", frame, xbox_LockstepFreeVblanks());
    if (g_only_count) {
        int k;
        for (k = 0; k < g_only_count; k++) {
            uint32_t off;
            for (off = 0; off < g_only_size[k]; off += CHUNK) {
                const uint64_t *p = (const uint64_t *)((uintptr_t)(g_only_va[k] + off) +
                                                       g_xbox_mem_offset);
                fprintf(f, " %08X", chunk_hash(p, CHUNK / 8u));
            }
        }
        fputc('\n', f);
        fflush(f);
        return;
    }
    for (r = 0; r < sizeof k_ranges / sizeof k_ranges[0]; r++) {
        uint32_t off;
        for (off = 0; off < k_ranges[r].size; off += CHUNK) {
            const uint64_t *p = (const uint64_t *)((uintptr_t)(k_ranges[r].va + off) +
                                                   g_xbox_mem_offset);
            fprintf(f, " %08X", chunk_hash(p, CHUNK / 8u));
        }
    }
    fputc('\n', f);
    fflush(f);
}

/* RECOMP_STATE_DUMP_AT=<frame>[,...]: the whole of both ranges at that
 * frame's end, to <RECOMP_STATE_HASH>.<frame>.bin, for comparing two runs
 * byte by byte where their hashes part. */
static void dump_at(const char *hash_path, unsigned long frame)
{
    static int parsed, count;
    static unsigned long at[16];
    char path[600];
    FILE *f;
    size_t r;
    int i, due = 0;

    if (!parsed) {
        const char *s = getenv("RECOMP_STATE_DUMP_AT");
        char *end;
        parsed = 1;
        while (s && *s && count < 16) {
            at[count++] = strtoul(s, &end, 10);
            if (end == s)
                break;
            s = *end == ',' ? end + 1 : end;
        }
    }
    for (i = 0; i < count; i++)
        if (at[i] == frame)
            due = 1;
    if (!due || !hash_path)
        return;
    snprintf(path, sizeof path, "%s.%lu.bin", hash_path, frame);
    f = fopen(path, "wb");
    if (!f)
        return;
    for (r = 0; r < sizeof k_ranges / sizeof k_ranges[0]; r++)
        fwrite((const void *)((uintptr_t)k_ranges[r].va + g_xbox_mem_offset), 1,
               k_ranges[r].size, f);
    fclose(f);
    fprintf(stderr, "[LOCKSTEP] frame %lu: guest memory to %s\n", frame, path);
    fflush(stderr);
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

/* Under the lockstep clock a frame is a sixtieth of a second of the game's
 * time whatever the host does (one vblank each), so the game runs at its own
 * speed only while frames end sixty times a second: uncapped at 120 it would
 * run twice as fast -- which is what a player whose frame rate was uncapped
 * saw in co-op. Paced here on the real clock, whatever the frame cap says,
 * after the frame and before the next frame's controls are taken. Late by
 * more than two frames (a level loading, the other machine waited on), the
 * count starts again rather than racing to catch up. */
static void lockstep_pace(void)
{
    static LARGE_INTEGER f;
    static LONGLONG next;
    static HANDLE timer;
    LARGE_INTEGER now;
    LONGLONG period;

    if (!f.QuadPart) {
        QueryPerformanceFrequency(&f);
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_ALL_ACCESS);
        if (!timer)                                 /* before Windows 10 1803 */
            timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    }
    period = f.QuadPart / 60;
    QueryPerformanceCounter(&now);
    if (!next || now.QuadPart - next > 2 * period) {
        next = now.QuadPart + period;
        return;
    }
    while (now.QuadPart < next) {
        LONGLONG us = (next - now.QuadPart) * 1000000 / f.QuadPart;

        if (us > 1500) {
            LARGE_INTEGER due;
            due.QuadPart = -(us - 1000) * 10;       /* 100 ns units, relative */
            if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
                WaitForSingleObject(timer, INFINITE);
            else
                Sleep(1);
        } else {
            YieldProcessor();
        }
        QueryPerformanceCounter(&now);
    }
    next += period;
}

void hle_lockstep_swap(void)
{
    static int configured;
    static FILE *hash_file;
    static const char *hash_path;
    static unsigned long every = 60;
    static DWORD main_thread;
    unsigned long frame;

    /* The game's frames are its main thread's: the one that swapped first.
     * A loading screen drawn on a thread of its own while the main thread
     * loads (Future Perfect's) swaps as often as time allows, a different
     * number of times on every machine -- counted, they would move every
     * later frame number, and with it every input and hash pinned to one. */
    if (!main_thread)
        main_thread = GetCurrentThreadId();
    if (GetCurrentThreadId() != main_thread)
        return;
    frame = ++g_frame;

    xbox_LockstepFrameEnded();
    if (xbox_LockstepClockOn())
        lockstep_pace();
    hle_net_frame_end(frame);
    if (!configured) {
        const char *path = getenv("RECOMP_STATE_HASH");
        const char *e = getenv("RECOMP_STATE_HASH_EVERY");
        const char *range = getenv("RECOMP_STATE_HASH_RANGE");

        configured = 1;
        if (e && atol(e) > 0)
            every = (unsigned long)atol(e);
        while (range && *range && g_only_count < 8) {
            char *end;
            g_only_va[g_only_count] = (uint32_t)strtoul(range, &end, 0) & ~(CHUNK - 1u);
            if (*end != ':')
                break;
            g_only_size[g_only_count++] =
                ((uint32_t)strtoul(end + 1, &end, 0) + CHUNK - 1u) & ~(CHUNK - 1u);
            range = *end == ',' ? end + 1 : NULL;
        }
        if (path && *path) {
            hash_path = path;
            hash_file = fopen(path, "w");
            if (hash_file) {
                fprintf(hash_file, "# chunks of 0x%X: 0x%08X+0x%X then 0x%08X+0x%X\n", CHUNK,
                        k_ranges[0].va, k_ranges[0].size, k_ranges[1].va, k_ranges[1].size);
                fprintf(stderr, "[LOCKSTEP] guest memory hashed every %lu frames to %s\n",
                        every, path);
            } else {
                fprintf(stderr, "[LOCKSTEP] RECOMP_STATE_HASH=%s cannot be written\n", path);
            }
            fflush(stderr);
        }
    }
    if (hash_file && frame % every == 0)
        write_hashes(hash_file, frame);
    dump_at(hash_path, frame);
    /* RECOMP_LOCKSTEP_JITTER=<ms>: a random pause of up to that long after
     * each frame -- a slower machine, or a busier one, in miniature. Whatever
     * makes two runs part under it is something two machines would part on. */
    {
        static int jitter = -1;
        static unsigned long from, to = (unsigned long)-1;
        if (jitter < 0) {
            const char *j = getenv("RECOMP_LOCKSTEP_JITTER");
            const char *w = getenv("RECOMP_LOCKSTEP_JITTER_FRAMES");   /* <from>-<to> */
            jitter = j ? atoi(j) : 0;
            if (w && *w) {
                char *end;
                from = strtoul(w, &end, 10);
                if (*end == '-')
                    to = strtoul(end + 1, NULL, 10);
            }
            srand((unsigned)GetTickCount());
        }
        if (jitter > 0 && frame >= from && frame <= to)
            Sleep((DWORD)(rand() % (jitter + 1)));
    }
}
