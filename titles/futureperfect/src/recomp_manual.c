/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>   /* getenv, exit: the spin verdict below */
#include <string.h>   /* strcmp: RECOMP_ICALL_FATAL */
#include <time.h>     /* time: the stick probe's report interval */
#include <math.h>     /* atan2: the look test's yaw */
#include "input_bindings.h"   /* the mouse settings, for 1:1 aim */
#include "input_mouse.h"      /* the mouse's counts, for 1:1 aim */
#include "hle_xmv_play.h"     /* the intro videos to skip */
#include "hle_netplay.h"      /* online co-op: what both machines must agree on */
#include "recomp_config.h"    /* the field of view */

/* One diagnostic report, one piece.
 *
 * Each report below is a dozen fprintf calls and the CRT takes its lock per
 * call, so with more than one guest thread running the lines interleave in
 * the middle of a report. Dino Crisis 3 runs six workers once it is past its
 * menus, and its log came out like this:
 *
 *   callers: 0x004DB424 <- 0x002705B4 <- 0x002705B4[ICALL] unresolved jump
 *   target 0x41B0B870 -- 1 time(s) (total calls: 6930990)
 *
 * -- two threads' reports spliced together, with a caller list that stops
 * mid-sentence and an address that belongs to neither line as read. These
 * diagnostics exist to be read during exactly the multi-threaded bring-up
 * that breaks them.
 *
 * The CRT exposes the lock these calls were already taking one at a time, so
 * hold it across the whole report instead. */
#if defined(_MSC_VER)
#  define RECOMP_DIAG_LOCK()   _lock_file(stderr)
#  define RECOMP_DIAG_UNLOCK() _unlock_file(stderr)
#elif defined(__unix__) || defined(__APPLE__)
#  define RECOMP_DIAG_LOCK()   flockfile(stderr)
#  define RECOMP_DIAG_UNLOCK() funlockfile(stderr)
#else
#  define RECOMP_DIAG_LOCK()   ((void)0)
#  define RECOMP_DIAG_UNLOCK() ((void)0)
#endif

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

/* These are defined thread-local in xbox_memory_layout.c. Declaring them
 * without the same storage class here does not fail to link -- it silently
 * resolves to different storage, so every read gets 0. That is why the log
 * below reported no call site: not because the guest esp was stale, but
 * because this file was not reading the guest esp at all. */
#if defined(_MSC_VER)
#  define RECOMP_MANUAL_TLS __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#  define RECOMP_MANUAL_TLS __thread
#else
#  define RECOMP_MANUAL_TLS _Thread_local
#endif

extern RECOMP_MANUAL_TLS uint32_t g_eax;
/* The other volatile registers, saved around a call this file makes into the
 * game (fp_ui_select). */
extern RECOMP_MANUAL_TLS uint32_t g_ecx, g_edx;
/* The guest stack pointer. At the moment an indirect call is refused, the
 * caller has already pushed its guest return address, so the top of the
 * guest stack is the call site -- the one thing the old log did not say. */
extern RECOMP_MANUAL_TLS uint32_t g_esp;
extern ptrdiff_t g_xbox_mem_offset;
/* The lifted code sections, so a value on the guest stack can be told
 * apart from data when naming the callers of a refused call. */
extern uint32_t g_xbox_code_lo;
extern uint32_t g_xbox_code_hi;
/* The esp the dispatch macro captured. Not g_esp, which is stale by the time
 * a refused call is reported. Zero when the title's generated header predates
 * this, and the log then says it has no callers rather than inventing them. */
extern RECOMP_MANUAL_TLS uint32_t g_icall_saved_esp;
/* Which dispatch form was refused: 0 unknown, 1 call, 2 jump. Unknown
 * means this title was lifted before the macros published it. */
extern RECOMP_MANUAL_TLS uint32_t g_icall_dispatch_form;

/* -- Future Perfect: the MMX memcpy at 0x000245C0 ----------------------- */

/*
 * sub_000245C0 is the title's MMX block copy: void *(void *dst, const void *src,
 * size_t n), cdecl, returning dst (five callers, among them sub_0011A930).
 *
 * It enters two slides of string moves through computed jumps:
 *
 *   mov ecx, 8; sub ecx, edi; and ecx, 7; ... neg ecx; add ecx, 0x2460C; jmp ecx
 *       -> 0 to 7 movsb (0x00024604..0x0002460B) to align the destination
 *   shr ecx, 2; and ecx, 0xF; neg ecx; add ecx, 0x24784; jmp ecx
 *       -> 0 to 15 movsd (0x00024774..0x00024780) for the tail
 *
 * The lifter turns `jmp <reg>` into gotos only when the register was loaded with
 * `mov reg, imm` (tools/recomp/translator.py, imm_code_refs), and the
 * disassembler ends the function at 0x00024772, before the movsd slide; its
 * epilogue became a function of its own, sub_00024784. Lifted, the first jump
 * resolves to nothing: run 5 reported "[ICALL] unresolved jump target
 * 0x0002460C" from 0x0011ACAC at 182 s, in the 2401 intro cutscene, and the
 * title faulted at once in sub_00383678 on the data the copy never wrote. It is
 * the only `jmp <reg>` in the XBE whose target is computed by `add reg, imm`.
 *
 * A copy has no behaviour beyond the bytes it moves, so this replacement does
 * exactly what the original does; memmove, because overlap was never defined.
 * Delete it once the lifter follows computed jumps into slides and the
 * disassembler keeps the slide inside the function.
 */
#define FP_GUEST(va) ((uint8_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define FP_MEM32(va) (*(volatile uint32_t *)FP_GUEST(va))
#define FP_MEM8(va) (*(volatile uint8_t *)FP_GUEST(va))

void sub_000245C0(void)
{
    uint32_t dst = FP_MEM32(g_esp + 4u);
    uint32_t src = FP_MEM32(g_esp + 8u);
    uint32_t n   = FP_MEM32(g_esp + 12u);

    if (n && dst != src)
        memmove(FP_GUEST(dst), FP_GUEST(src), n);
    g_eax = dst;
    g_esp += 4u;            /* ret: the caller pops the arguments (cdecl) */
}

/* -- Future Perfect: the stick reader at 0x00042970 --------------------- */

/*
 * float sub_00042970(int player, int axis), cdecl, the result on the x87
 * stack: every read of a stick by the game's own logic goes through it. The
 * controller poll (sub_0018FF70) has already turned each XInput axis into a
 * byte centred on 0x80 (+0x1C left X, +0x20 left Y, +0x24 right X, +0x28
 * right Y, so axis 3 is right X and 4 right Y) in the player's controller
 * record, 0x3C bytes each from 0x006B93D0. This turns the byte back into
 * -1..1 and takes off the record's own deadzone (the float at +0, which the
 * game sets per player through sub_00042920): inside it 0, outside it
 * rescaled to start from 0. While 0x006B9888 is set -- input locked -- the
 * answer is 0.
 *
 * Replaced by the same arithmetic, in the same single precision with the
 * same constants read from the XBE's own .rdata, so the game cannot tell;
 * it is here to be a place to stand. RECOMP_FP_AXIS_PROBE=1 reports which
 * call sites read the right stick, every five seconds, which is how the
 * camera code that turns the view was found.
 */
#define FP_MEMF(va) (*(volatile float *)FP_GUEST(va))

extern RECOMP_MANUAL_TLS double g_fp_stack[8];
extern RECOMP_MANUAL_TLS int    g_fp_top;

static void fp_axis_probe(uint32_t axis, uint32_t caller, float v)
{
    static int on = -1;
    static uint32_t sites[16];
    static unsigned long counts[16], moved[16];
    static int n;
    static time_t last;
    int i;

    if (on < 0) {
        const char *e = getenv("RECOMP_FP_AXIS_PROBE");
        on = e && *e && strcmp(e, "0") != 0;
    }
    if (!on || (axis != 3 && axis != 4))
        return;
    for (i = 0; i < n && sites[i] != caller; i++)
        ;
    if (i == n && n < 16) {
        sites[n] = caller;
        n++;
    }
    if (i < 16) {
        counts[i]++;
        if (v != 0.0f)
            moved[i]++;
    }
    {
        time_t now = time(NULL);
        if (now - last >= 5) {
            last = now;
            RECOMP_DIAG_LOCK();
            fprintf(stderr, "[FP-AXIS] right-stick reads in the last 5 s:");
            for (i = 0; i < n; i++)
                fprintf(stderr, " 0x%08X x%lu (%lu non-zero)", sites[i], counts[i], moved[i]);
            fprintf(stderr, "\n");
            RECOMP_DIAG_UNLOCK();
            fflush(stderr);
            memset(counts, 0, sizeof counts);
            memset(moved, 0, sizeof moved);
        }
    }
}

void sub_00042970(void)
{
    uint32_t caller = FP_MEM32(g_esp);
    uint32_t player = FP_MEM32(g_esp + 4u);
    uint32_t axis   = FP_MEM32(g_esp + 8u);
    float v = 0.0f;

    if (FP_MEM32(0x006B9888u) == 0) {
        uint32_t ctl = FP_MEM32(0x0076B0FCu)
                     ? 0x006B94C8u + player * 0x3Cu
                     : 0x006B93D0u + FP_MEM32(0x006B93C0u + player * 4u) * 0x3Cu;
        float dz = FP_MEMF(ctl);
        int32_t b = (int32_t)FP_MEM32(ctl + 0x18u + axis * 4u);

        v = (float)(b - 0x80) * (b < 0x80 ? FP_MEMF(0x004783A8u) : FP_MEMF(0x00476038u));
        if (dz > 0.0f) {
            float span = FP_MEMF(0x00475C78u) - dz;   /* 1.0 - dz, in single precision */
            if (0.0f - dz > v)
                v = (v + dz) / span;
            else if (v > dz)
                v = (v - dz) / span;
            else
                v = 0.0f;
        }
    }
    fp_axis_probe(axis, caller, v);

    g_fp_top = (g_fp_top + 7) & 7;              /* fld: the result on st0 */
    g_fp_stack[g_fp_top] = (double)v;
    g_esp += 4u;                                /* ret; the caller pops (cdecl) */
}

/* -- Future Perfect: the look vector, sub_0025CE80 ---------------------- */

/*
 * void sub_0025CE80(float *look, float *move), from the player update
 * sub_000D5EB0 once a frame: the look vector the camera turns by, from the
 * right stick through the control layout (sub_000CF200, actions 0x11 and
 * 0x10), with a 0.25 deadzone of its own set each frame (sub_00042920) and a
 * squared response (direction from a sine table, length capped at 1 and
 * squared), then Inverse Look; and the move vector, from the left stick.
 *
 * Wrapped (the generated body is sub_0025CE80_gen) so the mouse can replace
 * the look vector after the game has made it -- past the deadzone and the
 * curve, and only where the game asked for one, so menus, cutscenes and
 * every special mode keep their own handling.
 *
 * RECOMP_FP_LOOK_TEST=<ms>:<x>:<ms long>[,...] sets look x to a constant
 * for a while on the input script's clock, and logs the view's yaw every
 * frame (from the view matrix the D3D library keeps at 0x003E32A0, m00 and
 * m20), which is how the turn this vector produces was measured.
 */
extern void sub_0025CE80_gen(void);
extern unsigned long long recomp_input_script_ms(void);

static void fp_look_test(uint32_t look_va)
{
    static int parsed;
    static int count;
    static struct { unsigned long at, len; float x, y; } steps[16];
    static float last_yaw;
    static int have_last;
    unsigned long long t;
    float x = 999.0f, y = 999.0f, yaw;
    int i, nearby = 0;

    if (!parsed) {
        const char *s = getenv("RECOMP_FP_LOOK_TEST");
        char *end;
        parsed = 1;
        while (s && *s && count < 16) {
            steps[count].at = strtoul(s, &end, 10);
            if (*end != ':') break;
            steps[count].x = (float)strtod(end + 1, &end);
            if (*end != ':') break;
            steps[count].y = (float)strtod(end + 1, &end);
            if (*end != ':') break;
            steps[count].len = strtoul(end + 1, &end, 10);
            count++;
            s = *end == ',' ? end + 1 : end;
        }
    }
    if (!count)
        return;
    t = recomp_input_script_ms();
    for (i = 0; i < count; i++) {
        if (t >= steps[i].at && t < steps[i].at + steps[i].len) {
            x = steps[i].x;
            y = steps[i].y;
        }
        if (t + 300 >= steps[i].at && t < steps[i].at + steps[i].len + 600)
            nearby = 1;
    }
    /* 999: leave that axis as the game made it, and only log. */
    if (x != 999.0f)
        FP_MEMF(look_va) = x;
    if (y != 999.0f)
        FP_MEMF(look_va + 4u) = y;
    yaw = (float)(atan2((double)FP_MEMF(0x003E32C0u), (double)FP_MEMF(0x003E32A0u))
                  * 57.29577951308232);
    if (nearby) {
        float d = have_last ? yaw - last_yaw : 0.0f;
        if (d > 180.0f) d -= 360.0f;
        if (d < -180.0f) d += 360.0f;
        fprintf(stderr, "[FP-LOOK] t=%llu look=%.3f,%.3f yaw=%.3f dyaw=%.4f "
                "m01=%.4f m11=%.4f m21=%.4f m12=%.4f m10=%.4f\n",
                t, FP_MEMF(look_va), FP_MEMF(look_va + 4u), yaw, d,
                FP_MEMF(0x003E32A4u), FP_MEMF(0x003E32B4u), FP_MEMF(0x003E32C4u),
                FP_MEMF(0x003E32B8u), FP_MEMF(0x003E32B0u));
    }
    last_yaw = yaw;
    have_last = 1;
}

/* -- Future Perfect: true 1:1 mouse aim ---------------------------------- */

/*
 * The player's view angles, in degrees: yaw at [player+0x100] (growing as
 * the view turns left) and pitch at [player+0x104] (growing looking up).
 * Found by recording the player each frame through stick turns
 * (RECOMP_FP_SCAN) against the view matrix: both follow it 1:1. The look
 * consumer sub_0025D520 is what moves them -- yaw by look x times a turn
 * acceleration ([player+0xF80]) and the world step, then wrapped into
 * 0..360; pitch by look y (or eased toward level, with the game's
 * auto-centre option), then held within +-[0x007B0C80].
 *
 * So the mouse turns the angles themselves, here, just before the consumer
 * runs: counts times the sensitivity, added to yaw and pitch, and the
 * consumer then adds the stick's turn, wraps and clamps as it always does.
 * Nothing is measured or learned, so nothing can run away: 1:1 with no
 * deadzone, curve, acceleration or speed cap, the turn lands in the frame
 * it was made in, and the in-game Inverse Look stays out of it (the mouse
 * has its own).
 *
 * (The first version steered the look vector toward a target and learned
 * its gain from the view matrix each frame. In real play that loop
 * oscillated -- the gun whipping about -- and its guard against a view
 * turning the wrong way read the oscillation as a reversed axis and turned
 * the axis round at random. Scripted runs, feeding steady counts, never
 * showed it.)
 *
 * Only where the consumer turns the view by the look vector (fp_look_reader);
 * elsewhere -- the states the consumer handles otherwise, and anywhere the
 * look builder is not called, menus and cutscenes -- the mouse goes back to
 * the right stick (recomp_mouse_set_direct lapses).
 *
 * RECOMP_FP_MOUSE_AIM=0 goes back to the mouse driving the right stick.
 * RECOMP_FP_MOUSE_TEST=<ms>:<dx>:<dy>:<ms long>[,...] feeds counts each
 * frame on the input script's clock, as if the mouse were captured, and logs
 * every frame of it ([FP-AIM]).
 */
#define FP_DEG_PER_COUNT  0.04     /* at sensitivity 1: 360 degrees in about 29 cm at 800 dpi */

/* How sub_0025D520 is about to treat the player at [0x007B0C7C]. */
typedef struct {
    uint32_t pl;            /* the player */
    uint32_t state;         /* [player+0x114]: 2 on foot */
    int aim;                /* [player+0xF5C]: aim mode */
} FpLook;

/* 0 when, in the player's state, the look vector turns nothing. The tests
 * are sub_0025D520's own, down to 0x0025D5EE; constants from the XBE. */
static int fp_look_reader(FpLook *s)
{
    uint32_t pl = FP_MEM32(0x007B0C7Cu), o;

    if (!pl || !FP_MEM32(pl + 0x1500u))
        return 0;
    s->pl = pl;
    s->state = FP_MEM32(pl + 0x114u);
    if (s->state == 0x80)
        return 0;                   /* its own handling, from 0x0025D55F */
    if (!(FP_MEM8(pl + 0xCu) & 0x80) || s->state == 0x100) {
        o = FP_MEM32(0x007B0CC0u);
        if (!o || !(o = FP_MEM32(o + 0x26Cu)) || FP_MEM32(o) != 0x18 ||
            s->state == 0x400 || s->state == 0x200)
            return 0;
    }
    s->aim = FP_MEM32(pl + 0xF5Cu) != 0;
    return 1;
}

/* Zoomed in, the mouse turns the view by less, in proportion: the same
 * movement crosses the same part of the screen at any zoom (the "monitor
 * distance 0" convention PC shooters use). The zoom is the camera's own
 * vertical angle now ([player+8] +0x384, which eases from 55 degrees on foot
 * to the zoomed angle in aim mode), against what it was when last not
 * aiming, so a field-of-view setting moves both. Not zoomed in, or switched
 * off (mouse_zoom_match), 1. */
static double fp_zoom_scale(uint32_t pl, int aiming, int match)
{
    static double unzoomed = 0.0;
    uint32_t cam = FP_MEM32(pl + 8u);
    double fov, t;

    if (!cam)
        return 1.0;
    fov = FP_MEMF(cam + 0x384u);
    if (!(fov > 1.0 && fov < 170.0))
        return 1.0;
    t = tan(fov * 3.14159265358979 / 360.0);
    if (!aiming) {
        unzoomed = t;
        return 1.0;
    }
    if (!match || unzoomed <= 0.0 || t >= unzoomed)
        return 1.0;
    return t / unzoomed < 0.05 ? 0.05 : t / unzoomed;
}

/* Yaw grows turning left; pitch grows looking up (m21 grows as the view
 * tips down). From the view matrix the D3D library keeps; for the tests. */
static void fp_view_angles(float *yaw, float *pitch)
{
    double m21 = FP_MEMF(0x003E32C4u);

    if (m21 > 1.0) m21 = 1.0;
    if (m21 < -1.0) m21 = -1.0;
    *yaw = (float)(atan2((double)FP_MEMF(0x003E32C0u), (double)FP_MEMF(0x003E32C8u))
                   * 57.29577951308232);
    *pitch = (float)(-asin(m21) * 57.29577951308232);
}

/* RECOMP_FP_MOUSE_TEST: counts to feed this frame, and whether a step is on. */
static int fp_mouse_test(long *dx, long *dy, int *logging)
{
    static int parsed, count;
    static struct { unsigned long at, len; long dx, dy; } steps[16];
    unsigned long long t;
    int i;

    if (!parsed) {
        const char *s = getenv("RECOMP_FP_MOUSE_TEST");
        char *end;
        parsed = 1;
        while (s && *s && count < 16) {
            steps[count].at = strtoul(s, &end, 10);
            if (*end != ':') break;
            steps[count].dx = strtol(end + 1, &end, 10);
            if (*end != ':') break;
            steps[count].dy = strtol(end + 1, &end, 10);
            if (*end != ':') break;
            steps[count].len = strtoul(end + 1, &end, 10);
            count++;
            s = *end == ',' ? end + 1 : end;
        }
    }
    *logging = 0;
    if (!count)
        return 0;
    t = recomp_input_script_ms();
    for (i = 0; i < count; i++) {
        if (t >= steps[i].at && t < steps[i].at + steps[i].len) {
            *dx += steps[i].dx;
            *dy += steps[i].dy;
        }
        if (t + 200 >= steps[i].at && t < steps[i].at + steps[i].len + 400)
            *logging = 1;
    }
    return 1;                       /* a test is configured: treat as captured */
}

/* In a session: for the player being looked at, this machine's own turn is
 * worked out as offline (counts times sensitivity and zoom, inverted or not)
 * and handed to netplay, and every player's turn for this frame -- both
 * machines have the same -- is added to their angles. */
static void fp_mouse_aim_net(uint32_t look_va)
{
    RecompMouseSettings ms;
    FpLook rd;
    float ay, ap, yaw, pitch;
    int me = hle_net_local_port(), port = recomp_bindings_mouse_port(), idx;

    (void)look_va;
    if (!fp_look_reader(&rd)) {
        recomp_mouse_set_direct(0);
        return;
    }
    idx = (int)FP_MEM32(rd.pl + 4u);
    /* This machine's player, turned by this machine's mouse -- unless the
     * mouse is not this copy's (player 2's, both games on one PC). */
    if (idx == me && port == hle_net_local_input_port()) {
        long dx = 0, dy = 0;
        int logging, testing = fp_mouse_test(&dx, &dy, &logging);
        if (recomp_mouse_captured()) {
            long rx, ry;
            recomp_mouse_take_counts(&rx, &ry);
            dx += rx;
            dy += ry;
        }
        if (port >= 0 && (recomp_mouse_captured() || testing) &&
            recomp_bindings_get_mouse((unsigned)port, &ms)) {
            double k = FP_DEG_PER_COUNT * ms.sensitivity *
                       fp_zoom_scale(rd.pl, rd.aim, ms.zoom_match);
            recomp_mouse_set_direct(1);
            if (dx || dy)
                hle_net_add_local_aim((float)(-dx * k),
                                      (float)((ms.invert_y ? dy : -dy) * k * ms.y_scale));
        }
    }
    if (idx < 0 || !hle_net_port_aim((unsigned)idx, &ay, &ap) || (ay == 0.0f && ap == 0.0f))
        return;
    yaw = FP_MEMF(rd.pl + 0x100u) + ay;
    pitch = FP_MEMF(rd.pl + 0x104u) + ap;
    if (yaw >= 360.0f || yaw < 0.0f)
        yaw -= 360.0f * floorf(yaw / 360.0f);
    if (pitch > 89.0f) pitch = 89.0f;
    if (pitch < -89.0f) pitch = -89.0f;
    if (ay != 0.0f)
        FP_MEMF(rd.pl + 0x100u) = yaw;
    if (ap != 0.0f)
        FP_MEMF(rd.pl + 0x104u) = pitch;
}

static void fp_mouse_aim(uint32_t look_va)
{
    static int enabled = -1;
    RecompMouseSettings ms;
    FpLook rd;
    long dx = 0, dy = 0;
    int port, testing, logging = 0;
    double k;
    float yaw, pitch;

    if (enabled < 0) {
        const char *e = getenv("RECOMP_FP_MOUSE_AIM");
        enabled = !(e && *e && strcmp(e, "0") == 0);
        fprintf(stderr, "[FP-AIM] mouse aim %s\n", enabled
                ? "1:1 into the view angles (RECOMP_FP_MOUSE_AIM=0 for the stick)"
                : "through the right stick (RECOMP_FP_MOUSE_AIM=0)");
        fflush(stderr);
    }
    if (!enabled)
        return;
    /* Online: this machine's turn goes out with its controls, and each
     * player's turn is applied from what both machines agree on -- written
     * here directly, it would turn one machine's player only (hle_netplay.h). */
    if (hle_net_local_port() >= 0) {
        fp_mouse_aim_net(look_va);
        return;
    }
    port = recomp_bindings_mouse_port();
    testing = fp_mouse_test(&dx, &dy, &logging);
    if (recomp_mouse_captured()) {
        long rx, ry;
        recomp_mouse_take_counts(&rx, &ry);     /* taken even when unused: no backlog */
        dx += rx;
        dy += ry;
    }
    if (port < 0 || (!recomp_mouse_captured() && !testing) ||
        !recomp_bindings_get_mouse((unsigned)port, &ms))
        return;
    if (!fp_look_reader(&rd)) {
        recomp_mouse_set_direct(0);             /* the stick turns this state */
        return;
    }
    recomp_mouse_set_direct(1);

    /* Right (dx > 0) turns right, which is yaw going down. Away from the
     * player (dy < 0) looks up, which is pitch going up. The consumer wraps
     * yaw and holds pitch to its limit after adding the stick's turn. */
    k = FP_DEG_PER_COUNT * ms.sensitivity * fp_zoom_scale(rd.pl, rd.aim, ms.zoom_match);
    yaw = FP_MEMF(rd.pl + 0x100u) - (float)(dx * k);
    pitch = FP_MEMF(rd.pl + 0x104u) + (float)((ms.invert_y ? dy : -dy) * k * ms.y_scale);
    if (yaw >= 360.0f || yaw < 0.0f)
        yaw -= 360.0f * floorf(yaw / 360.0f);
    if (pitch > 89.0f) pitch = 89.0f;
    if (pitch < -89.0f) pitch = -89.0f;
    if (dx)
        FP_MEMF(rd.pl + 0x100u) = yaw;
    if (dy)
        FP_MEMF(rd.pl + 0x104u) = pitch;

    if (logging) {
        float vy, vp;
        fp_view_angles(&vy, &vp);
        fprintf(stderr, "[FP-AIM] t=%llu in %ld,%ld  angles %.3f,%.3f  view %.3f,%.3f  "
                "look %.3f,%.3f  state %x%s  zoom %.3f\n",
                recomp_input_script_ms(), dx, dy, FP_MEMF(rd.pl + 0x100u),
                FP_MEMF(rd.pl + 0x104u), vy, vp, FP_MEMF(look_va), FP_MEMF(look_va + 4u),
                rd.state, rd.aim ? " aim" : "",
                fp_zoom_scale(rd.pl, rd.aim, ms.zoom_match));
    }
}

/* RECOMP_FP_POKE=<ms>:<ms long>:<va hex>:<float>[,...]: write a float every
 * frame of a window on the input script's clock -- how a variable is found
 * by what changing it does. */
static void fp_poke(void)
{
    static int parsed, count;
    static struct { unsigned long at, len; uint32_t va; float v; } p[16];
    unsigned long long t;
    int i;

    if (!parsed) {
        const char *s = getenv("RECOMP_FP_POKE");
        char *end;

        parsed = 1;
        while (s && *s && count < 16) {
            p[count].at = strtoul(s, &end, 10);
            if (*end != ':') break;
            p[count].len = strtoul(end + 1, &end, 10);
            if (*end != ':') break;
            p[count].va = (uint32_t)strtoul(end + 1, &end, 16);
            if (*end != ':') break;
            p[count].v = (float)strtod(end + 1, &end);
            count++;
            s = *end == ',' ? end + 1 : end;
        }
    }
    if (!count)
        return;
    t = recomp_input_script_ms();
    for (i = 0; i < count; i++)
        if (t >= p[i].at && t < p[i].at + p[i].len)
            FP_MEMF(p[i].va) = p[i].v;
}

/* The field of view (recomp_fov). The camera ([player+0x8], the block its
 * matrices start at) keeps its angles as floats, vertical, in degrees:
 *   +0x37C tan(vertical/2)    +0x380 tan(horizontal/2), from the aspect
 *   +0x384 the angle now      +0x388 the angle it eases toward
 *   +0x38C the zoomed angle
 * Found by RAM snapshot (55, 55, 30.25 on foot) and by holding each at 75
 * (RECOMP_FP_POKE): +0x384 moves the view at once and is eased back; +0x388
 * moves it and stays. The game writes +0x388 only when the view changes
 * mode -- the zoomed angle going into aim mode, its own 55 coming out -- so
 * the setting is +0x388 whenever the player is not aiming, put back the frame
 * after the game sets its own. The game derives everything else from the
 * angle now, its culling included, so the edges of a wider view are drawn.
 * Only while the look builder runs: never in a cutscene. */
static void fp_apply_fov(void)
{
    static float game_fov, written;
    uint32_t pl = FP_MEM32(0x007B0C7Cu), cam;
    /* Online: the game's own angle on both machines (a player's setting
     * would turn only their machine's camera). */
    double user = hle_net_local_port() >= 0 ? 0.0 : recomp_fov();
    FpLook rd;
    float target, want;

    if (!pl || !(cam = FP_MEM32(pl + 8u)) || !fp_look_reader(&rd) || rd.aim)
        return;
    target = FP_MEMF(cam + 0x388u);
    if (!(target > 10.0f && target < 120.0f))
        return;                             /* not the camera this was measured on */
    if (target != written)
        game_fov = target;                  /* the game chose it: its own angle */
    want = user > 0.0 ? (float)user : game_fov;
    if (want != target)
        FP_MEMF(cam + 0x388u) = want;
    written = want;
}

/* The held weapon's field of view (recomp_weapon_fov). The weapon is drawn
 * with a projection of its own (sub_001B9120 sets it, draws the weapon, puts
 * the view's back), built by sub_001B73F0 from the camera's angle now
 * (+0x384) times 1/55 times 45: 45 degrees against the view's 55, and
 * narrower with it when zoomed. Widening the view widened the weapon as well
 * -- shrunk, arms showing. The 1/55 is a constant at 0x0047E298 read only
 * there and once more in the weapon's code (0x001C3406, the same product);
 * it is made (weapon / 45) / the view's own angle (the player's, or the
 * game's 55), so the weapon keeps its angle whatever the view's and still
 * zooms as it did. Found by the projection each draw uses (two: 1/tan of
 * 27.5 and of 22.5 degrees, the weapon's last) and then the guest stack of
 * the call that set the second. */
static void fp_apply_weapon_fov(void)
{
    int online = hle_net_local_port() >= 0;      /* the game's own, as fp_apply_fov */
    double view = !online && recomp_fov() > 0.0 ? recomp_fov() : recomp_fov_game();
    double weapon = !online && recomp_weapon_fov() > 0.0 ? recomp_weapon_fov()
                                                         : recomp_weapon_fov_game();

    if (view > 0.0 && weapon > 0.0)
        FP_MEMF(0x0047E298u) = (float)(weapon / 45.0 / view);
}

/* When the look builder last ran (sub_0025CE80, below): the player is in
 * play, not in a menu, a pause or a cutscene. */
static volatile long g_fp_look_tick;

/* -- Future Perfect: any screen shape (ultrawide) ------------------------- */

/*
 * Booted widescreen, Future Perfect keeps its screen's shape as two whole
 * numbers -- 16 at 0x004C0458, 9 at 0x004C045C -- and the squeeze its 2D layer
 * is drawn with, (4/3)/(16/9) = 0.75 at 0x004C0460, all three set once by its
 * device set-up (0x00023A9A). The camera builds its horizontal angle from the
 * two numbers each frame (tan(h/2) = tan(v/2) * 16/9, camera +0x380), the HUD
 * and menus place themselves by the squeeze, and 27 places ask sub_0003C420
 * whether the screen is 16:9 -- exactly 16 and 9, which anything else fails.
 *
 * With the picture filling a window of another shape (xbox_D3D8FillShape,
 * the wide_fill setting), the three take the window's shape and the question
 * becomes "wider than 4:3". The game then draws 21:9 itself: its own camera,
 * its own culling, its own 2D layout. Kept up to date from the three
 * accessors below, which the game calls every frame, so a window being
 * resized is followed as it goes; 16:9 again whenever filling is off.
 */
extern int xbox_D3D8FillShape(unsigned int *width, unsigned int *height);
extern void xbox_D3D8SetTitleAnyAspect(int on);
extern void xbox_D3D8SetInterpRegisters(int projection, int affine_first, int affine_count);
extern void xbox_Nv2aFlipGateSetTitlePaced(int on);
extern void recomp_prompts_icon(uint32_t checksum, const char *control, uint32_t rgb);
extern int xbox_D3D8PictureRect(float *x, float *y, float *w, float *h);
extern void xbox_D3D8MenuSetPalette(unsigned dark, unsigned mid, unsigned ink);

static void fp_aspect_follow(void)
{
    static int wide;                        /* the game booted 16:9 */
    unsigned w = 16, h = 9, a, b, t;

    if (!wide) {
        if (FP_MEM32(0x004C0458u) != 16 || FP_MEM32(0x004C045Cu) != 9)
            return;
        wide = 1;
    }
    /* Online: 16:9 on both machines -- the game lays its world and HUD out
     * by this shape, and two machines must lay them out alike. */
    if (hle_net_local_port() < 0 && xbox_D3D8FillShape(&w, &h) && h) {
        if (w * 3u < h * 4u) {
            w = 4;                          /* narrower than 4:3 stays 4:3 */
            h = 3;
        }
        for (a = w, b = h; b; t = a % b, a = b, b = t)
            ;
        w /= a;
        h /= a;
    } else {
        w = 16;
        h = 9;
    }
    if (FP_MEM32(0x004C0458u) != w || FP_MEM32(0x004C045Cu) != h) {
        FP_MEM32(0x004C0458u) = w;
        FP_MEM32(0x004C045Cu) = h;
        FP_MEMF(0x004C0460u) = (float)(4.0 * h / (3.0 * w));
        fprintf(stderr, "[FP] screen shape %u:%u, 2D squeeze %.4f\n", w, h,
                (double)FP_MEMF(0x004C0460u));
        fflush(stderr);
    }
}

/* -- Future Perfect: the mouse in the menus --------------------------------- */

/*
 * The front end is a table of 20 widgets of 0x23C bytes at 0x006FEBE8
 * (cleared by sub_00074F70, handed out by sub_00078E50). A widget's
 * children start at +0x16C and go on through each child's +0x30; a list
 * keeps its items at +0x178, how many at +0x17C, and the one selected at
 * +0x184 (its record at +0x198). Items are records of 0x10C bytes, linked
 * through +0x4: +0x0 the kind (4, 7 and 8 are the ones a list can select;
 * the main menu's buttons are 7), +0x14 the position, +0x88 the widget,
 * and +0x78..+0x84 the box it is drawn in -- left, top, right, bottom -- in
 * the 640x480 screen the front end lays out, squeezed already for 16:9 or
 * any other shape. Found by RAM snapshots across presses of down in the main
 * menu, and the boxes checked against screenshots in 4:3 and 16:9.
 *
 * Each frame, a screen updates its widgets by calling sub_0007A7B0 with its
 * root (the main menu's is [0x007613E4]); the call walks the children
 * itself. Wrapped below: before the outermost call, the pointer is put
 * through the picture's place in the window (xbox_D3D8PictureRect) into
 * that 640x480 screen and hit against every selectable item under the root.
 * Moving onto one selects it the game's own way, sub_00076EF0(widget, item)
 * -- the highlight, the help line and the sound follow -- and only on a
 * move, so a stationary pointer never fights the keys or the pad. A left
 * click on an item presses A, a right click B, the wheel up and down on the
 * D-pad: what the game reads for the rest is unchanged. A value (a choice or
 * a volume bar, fp_ui_value) is stepped instead: a click left or right of
 * its middle presses left or right, held it stays pressed for the game's
 * own repeat, and the wheel over it turns it. Two screens draw what a
 * click needs without items to hit: the pause screen's OPTIONS / STATUS
 * header (fp_ui_pause_tab) and character select's portraits (fp_cs_*).
 *
 * The pointer is asked for (recomp_mouse_pointer_mode) while a list is up
 * and the player is not in play (the look builder has not run for 100 ms):
 * the mouse is let go for the menu and taken back when play resumes.
 *
 * RECOMP_FP_UI_DUMP=<ms>[,<ms>...] prints the widget table and every item's
 * record at those times on the input script's clock: where to start for a
 * screen whose controls are not plain lists.
 */
extern void sub_0007A7B0_gen(void);
extern void sub_00076EF0(void);

#define FP_UI_TABLE   0x006FEBE8u
#define FP_UI_WIDGETS 20
#define FP_UI_SIZE    0x23Cu

static void fp_ui_dump(void)
{
    static int parsed, count, next;
    static unsigned long at[16];
    int e;

    if (!parsed) {
        const char *s = getenv("RECOMP_FP_UI_DUMP");
        char *end;
        parsed = 1;
        while (s && *s && count < 16) {
            at[count++] = strtoul(s, &end, 10);
            if (end == s)
                break;
            s = *end == ',' ? end + 1 : end;
        }
    }
    if (next >= count || recomp_input_script_ms() < at[next])
        return;
    next++;
    fprintf(stderr, "[FP-UI] root [0x007613E4] %08X, squeeze %.4f\n", FP_MEM32(0x007613E4u),
            FP_MEMF(0x004C0460u));
    for (e = 0; e < FP_UI_WIDGETS; e++) {
        uint32_t w = FP_UI_TABLE + (uint32_t)e * FP_UI_SIZE, it;
        int n = 0, o;
        if (!FP_MEM32(w))
            continue;
        fprintf(stderr, "[FP-UI] widget %d @%08X kind %u parent %08X child %08X next %08X items "
                "%08X count %u selected %d box %.1f %.1f %.1f %.1f\n", e, w, FP_MEM32(w),
                FP_MEM32(w + 0x2Cu), FP_MEM32(w + 0x16Cu), FP_MEM32(w + 0x30u),
                FP_MEM32(w + 0x178u), FP_MEM32(w + 0x17Cu), (int)FP_MEM32(w + 0x184u),
                FP_MEMF(w + 0xA8u), FP_MEMF(w + 0xACu), FP_MEMF(w + 0xB0u), FP_MEMF(w + 0xB4u));
        for (it = FP_MEM32(w + 0x178u); it && n < 64; it = FP_MEM32(it + 4u), n++) {
            char line[1400], text[48];
            uint32_t s = FP_MEM32(it + 0x20u);
            int len, c;
            for (c = 0; s && c < 47; c++) {
                uint8_t ch = FP_MEM8(s + (uint32_t)c);
                if (ch < 0x20 || ch > 0x7E) break;
                text[c] = (char)ch;
            }
            text[s ? c : 0] = '\0';
            len = snprintf(line, sizeof line, "[FP-UI]   item %08X \"%s\":", it, text);
            for (o = 0; o < 0x10C && len < (int)sizeof line - 24; o += 4) {
                uint32_t v = FP_MEM32(it + (uint32_t)o);
                float fv = FP_MEMF(it + (uint32_t)o);
                if (!v)
                    continue;
                if (fv > 0.5f && fv < 5000.0f && (v >> 24) < 0x80)
                    len += snprintf(line + len, sizeof line - len, " +%X=%.1f", o, fv);
                else
                    len += snprintf(line + len, sizeof line - len, " +%X=%X", o, v);
            }
            fprintf(stderr, "%s\n", line);
        }
    }
    fflush(stderr);
}

/* sub_00076EF0(widget, item), cdecl, from here: the arguments and a return
 * address on the guest stack, the volatile registers kept. */
static void fp_ui_select(uint32_t widget, uint32_t item)
{
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx;

    g_esp -= 4u; FP_MEM32(g_esp) = item;
    g_esp -= 4u; FP_MEM32(g_esp) = widget;
    g_esp -= 4u; FP_MEM32(g_esp) = 0u;      /* the return address; it pops it */
    sub_00076EF0();
    g_esp += 8u;
    g_eax = eax;
    g_ecx = ecx;
    g_edx = edx;
}

/* What a list can select: kinds 4, 7 and 8, without bit 4 of +0x28 -- the
 * game's own select (sub_00076EF0) passes over an item with that bit to the
 * next, so a title, a text field or a row of button prompts drawn as a
 * button is never landed on (the name entry screen has all three). */
static int fp_ui_selectable(uint32_t item)
{
    uint32_t kind = FP_MEM32(item);
    return (kind == 4u || kind == 7u || kind == 8u) && !(FP_MEM8(item + 0x28u) & 4u);
}

/* The item under (x, y) among every list under root, and its widget; 0 for
 * none. Also whether any list is up at all. */
static uint32_t fp_ui_hit(uint32_t root, float x, float y, uint32_t *widget, int *lists)
{
    uint32_t stack[32], w;
    int sp = 0, guard = 0;

    *lists = 0;
    *widget = 0;
    stack[sp++] = root;
    while (sp && guard++ < 64) {
        uint32_t it, c;
        int n = 0;

        w = stack[--sp];
        if (w < FP_UI_TABLE || w >= FP_UI_TABLE + FP_UI_WIDGETS * FP_UI_SIZE || !FP_MEM32(w))
            continue;
        for (c = FP_MEM32(w + 0x16Cu); c && sp < 32 && n < 32; c = FP_MEM32(c + 0x30u), n++)
            stack[sp++] = c;
        for (it = FP_MEM32(w + 0x178u), n = 0; it && n < 128; it = FP_MEM32(it + 4u), n++) {
            if (!fp_ui_selectable(it))
                continue;
            *lists = 1;
            if (x >= FP_MEMF(it + 0x78u) && x < FP_MEMF(it + 0x80u) &&
                y >= FP_MEMF(it + 0x7Cu) && y < FP_MEMF(it + 0x84u)) {
                *widget = w;
                return it;
            }
        }
    }
    return 0;
}

/* An item that holds a value rather than doing something: a choice (+0xB4
 * set; +0xC0 the current one of +0xC4, the value itself at [+0xD8]) or a
 * bar (the volumes: +0x28 0xC2000). Left and right on the D-pad change
 * either; the buttons beside them have +0x30 2 and a handler at +0x38. From
 * the Audio/Video Options screen. */
static int fp_ui_value(uint32_t item)
{
    return FP_MEM32(item) == 7u && FP_MEM32(item + 0x30u) != 2u &&
           (FP_MEM32(item + 0xB4u) == 1u || FP_MEM32(item + 0x28u) == 0xC2000u);
}

/* The pause screen's tabs, OPTIONS and STATUS, are no list: left and right
 * on the D-pad turn them, and the header naming them is drawn, not hit.
 * Which is showing is told by what is under root -- OPTIONS has Continue
 * (+0x38 0x2EBB80), STATUS its objectives box (kind 8, +0x50 0x2F5E10).
 * A click on the header beside the tab showing turns to the other: -1 left,
 * 1 right, 0 not on it. The header's place was measured from screenshots in
 * 4:3; across 16:9 the front end squeezes toward the middle by [0x004C0460]. */
static int fp_ui_pause_tab(uint32_t root, float x, float y)
{
    uint32_t stack[32], w;
    int sp = 0, guard = 0, showing = 0;
    float squeeze = FP_MEMF(0x004C0460u), dx;

    if (y < 34.0f || y > 76.0f || !(squeeze > 0.1f && squeeze <= 1.0f))
        return 0;
    stack[sp++] = root;
    while (sp && guard++ < 64 && !showing) {
        uint32_t it, c;
        int n = 0;

        w = stack[--sp];
        if (w < FP_UI_TABLE || w >= FP_UI_TABLE + FP_UI_WIDGETS * FP_UI_SIZE || !FP_MEM32(w))
            continue;
        for (c = FP_MEM32(w + 0x16Cu); c && sp < 32 && n < 32; c = FP_MEM32(c + 0x30u), n++)
            stack[sp++] = c;
        for (it = FP_MEM32(w + 0x178u), n = 0; it && n < 128; it = FP_MEM32(it + 4u), n++) {
            if (FP_MEM32(it) == 7u && FP_MEM32(it + 0x38u) == 0x002EBB80u)
                showing = 1;                /* OPTIONS: its name spans dx -112..22 */
            else if (FP_MEM32(it) == 8u && FP_MEM32(it + 0x50u) == 0x002F5E10u)
                showing = 2;                /* STATUS: -16..102 */
        }
    }
    dx = (x - 320.0f) / squeeze;
    if (!showing || dx < -140.0f || dx > 140.0f)
        return 0;
    if (showing == 1)
        return dx < -112.0f ? -1 : dx > 22.0f ? 1 : 0;
    return dx < -16.0f ? -1 : dx > 102.0f ? 1 : 0;
}

/* Character select is no list: the portraits down the left are drawn by a
 * screen of its own, whose one item covers everything above the button
 * prompts (0,0 to 640,414, +0x28 0xC0000). Its state, found by RAM
 * snapshots across presses of up and down: the character selected at
 * [0x0078A248], the row at the top of the column as a float at [0x0078A28C]
 * (it scrolls smoothly; the selection sits on the fourth row once past it),
 * and [0x0078A294] set only while the screen is up. [0x0078A27C] is the
 * player's state, 1 choosing and 4 ready, and [0x0078A264] a float that
 * starts near 1 when the selection changes and runs down to 0 over about a
 * second as the new character comes in -- A is not taken until it has. How
 * many there are to choose from is at [0x0078AC98], up wrapping from the
 * first to the last.
 * The rows, measured in 4:3 and the same in 16:9: 73.7 apart, the first
 * centred at 68.8, the portraits between x 34 and 110. */
#define FP_CS_SELECTED 0x0078A248u
#define FP_CS_TOP      0x0078A28Cu
#define FP_CS_ACTIVE   0x0078A294u
#define FP_CS_COUNT    0x0078AC98u
#define FP_CS_STATE    0x0078A27Cu
#define FP_CS_FADE     0x0078A264u

static int fp_cs_screen(uint32_t item)
{
    /* The item's box is squeezed toward the middle for 16:9 like any other
     * (80 to 560 then; 639.99994 rather than 640 in 4:3), the portraits are
     * not: they keep to the left edge. */
    float half = 319.0f * FP_MEMF(0x004C0460u);
    return item && FP_MEM32(FP_CS_ACTIVE) && FP_MEM32(item + 0x28u) == 0x000C0000u &&
           FP_MEMF(item + 0x78u) <= 320.0f - half && FP_MEMF(item + 0x80u) >= 320.0f + half;
}

/* The character under (x, y), or -1. */
static int fp_cs_row(float x, float y)
{
    float top = FP_MEMF(FP_CS_TOP), at;
    int count = (int)FP_MEM32(FP_CS_COUNT), i;

    if (x < 30.0f || x > 114.0f || y < 0.0f || y >= 414.0f || !(top >= 0.0f))
        return -1;
    at = top + (y - 68.8f) / 73.7f;
    i = (int)floorf(at + 0.5f);
    return i >= 0 && i < count ? i : -1;
}

/* Walking the selection to a character: one press of up or down at a time,
 * held until the game has moved (a frame can stall while the model before
 * loads) and then let go long enough to be seen let go, and A at the end if
 * it was a click. */
static struct { int target, activate, phase, from; long t, pressed_a; } g_fp_cs = { -1 };

static void fp_cs_follow(unsigned port, long now)
{
    int sel = (int)FP_MEM32(FP_CS_SELECTED);

    if (g_fp_cs.target < 0)
        return;
    if (g_fp_cs.phase == 1) {               /* pressing */
        if (sel == g_fp_cs.from && now - g_fp_cs.t < 500) {
            recomp_bindings_press(port, g_fp_cs.target > g_fp_cs.from ? "dpad_down" : "dpad_up", 50);
            return;
        }
        g_fp_cs.phase = 2;
        g_fp_cs.t = now;
    }
    if (g_fp_cs.phase == 2) {               /* letting go */
        if (now - g_fp_cs.t < 80)
            return;
        g_fp_cs.phase = 0;
    }
    if (sel == g_fp_cs.target) {
        /* A is not taken while the new character comes in, so it waits for
         * that, and goes again until the player reads as ready (a frame can
         * stall over the press). */
        if (!g_fp_cs.activate || FP_MEM32(FP_CS_STATE) == 4u || now - g_fp_cs.t > 4000) {
            g_fp_cs.target = -1;
            return;
        }
        if (FP_MEMF(FP_CS_FADE) <= 0.001f && now - g_fp_cs.pressed_a >= 400) {
            recomp_bindings_press(port, "a", 100);
            g_fp_cs.pressed_a = now;
        }
        return;
    }
    g_fp_cs.from = sel;
    g_fp_cs.t = now;
    g_fp_cs.phase = 1;
    recomp_bindings_press(port, g_fp_cs.target > sel ? "dpad_down" : "dpad_up", 50);
}

/* The pointer on character select: moving onto a portrait walks the
 * selection there, a left click there picks it (A once it is reached), a
 * left click anywhere else picks the one selected; the wheel and a right
 * click stay as they are. 1 when the left click was taken. */
static int fp_cs_mouse(unsigned port, long now, float x, float y, int moved, unsigned clicks,
                       int wheel)
{
    int row = fp_cs_row(x, y);

    if (wheel)
        g_fp_cs.target = -1;                /* the wheel steers instead */
    if (FP_MEM32(FP_CS_STATE) != 1u)
        row = -1;                           /* ready: the portraits are fixed */
    if (hle_net_local_port() > 0)
        row = -1;                           /* online player 2: FP_CS_SELECTED is player 1's */
    if (row >= 0 && (moved || (clicks & RECOMP_MOUSE_LEFT))) {
        if (g_fp_cs.target != row)
            g_fp_cs.activate = 0;
        g_fp_cs.target = row;
        if (clicks & RECOMP_MOUSE_LEFT) {
            g_fp_cs.activate = 1;
            g_fp_cs.t = now;
            g_fp_cs.pressed_a = now - 1000;
        }
    } else if (clicks & RECOMP_MOUSE_LEFT) {
        g_fp_cs.target = -1;
        recomp_bindings_press(port, "a", 80);
    }
    fp_cs_follow(port, now);
    return (clicks & RECOMP_MOUSE_LEFT) != 0;
}

/* RECOMP_FP_POINTER_TEST=<ms>:<x>:<y>:<click>[,...]: the pointer at (x, y)
 * in the front end's 640x480 screen from that time on the input script's
 * clock, as if the real one were there, and a click as the step begins (1
 * left, 2 right, 0 none, 3 left and held down until the next step; 4 and 5
 * the wheel, a notch up or down) -- a run cannot move the desktop's pointer.
 * 1 while a test is configured, with *have whether a step has begun. */
static int fp_pointer_test(float *x, float *y, unsigned *clicks, unsigned *held, int *wheel,
                           int *have)
{
    static int parsed, count, done = -1;
    static struct { unsigned long at; float x, y; unsigned click; } steps[16];
    unsigned long long t;
    int i, cur = -1;

    if (!parsed) {
        const char *s = getenv("RECOMP_FP_POINTER_TEST");
        char *end;
        parsed = 1;
        while (s && *s && count < 16) {
            steps[count].at = strtoul(s, &end, 10);
            if (*end != ':') break;
            steps[count].x = (float)strtod(end + 1, &end);
            if (*end != ':') break;
            steps[count].y = (float)strtod(end + 1, &end);
            if (*end != ':') break;
            steps[count].click = (unsigned)strtoul(end + 1, &end, 10);
            count++;
            s = *end == ',' ? end + 1 : end;
        }
    }
    if (!count)
        return 0;
    t = recomp_input_script_ms();
    for (i = 0; i < count; i++)
        if (t >= steps[i].at)
            cur = i;
    *have = cur >= 0;
    if (cur >= 0) {
        static const char *const what[] = { "", " left click", " right click", " left held",
                                            " wheel up", " wheel down" };
        unsigned click = steps[cur].click;
        *x = steps[cur].x;
        *y = steps[cur].y;
        if (click == 3)
            *held |= RECOMP_MOUSE_LEFT;
        if (cur != done) {
            done = cur;
            if (click == 1 || click == 3) *clicks |= RECOMP_MOUSE_LEFT;
            if (click == 2) *clicks |= RECOMP_MOUSE_RIGHT;
            if (click == 4) *wheel += 1;
            if (click == 5) *wheel -= 1;
            fprintf(stderr, "[FP-UI] pointer test: (%.0f, %.0f)%s\n", *x, *y,
                    click < 6 ? what[click] : "");
            fflush(stderr);
        }
    }
    return 1;
}

/* The pointer's walk in a session (fp_menu_mouse): to an item by presses
 * of up and down, one at a time, each waited out until the selection moves;
 * on arriving, what the click asked for. A list where up and down never
 * reach the item (a grid: the name keyboard) is given up on. */
static struct {
    uint32_t widget, target;
    int      click;             /* a click, not just pointing */
    float    x;                 /* where, for a value's side */
    uint32_t moved_from;        /* the selection when the last press went */
    long     at;
    int      presses;
} g_fp_walk;

static int fp_ui_index(uint32_t widget, uint32_t item);     /* with co-op's helpers, below */

static void fp_walk_to(uint32_t widget, uint32_t target, int click, float x)
{
    if (g_fp_walk.target != target || g_fp_walk.widget != widget)
        g_fp_walk.presses = 0;
    g_fp_walk.widget = widget;
    g_fp_walk.target = target;
    g_fp_walk.click |= click;
    g_fp_walk.x = x;
}

static void fp_walk(unsigned port, long now)
{
    uint32_t w = g_fp_walk.widget, t = g_fp_walk.target, sel;
    int at, to;

    if (!t)
        return;
    if (!FP_MEM32(w) || fp_ui_index(w, t) < 0) {
        memset(&g_fp_walk, 0, sizeof g_fp_walk);    /* the list has gone */
        return;
    }
    sel = FP_MEM32(w + 0x198u);
    if (sel == t) {
        if (g_fp_walk.click) {
            const char *press = "a";
            if (fp_ui_value(t) && !(FP_MEM32(t + 0x38u) && g_fp_walk.x >= FP_MEMF(t + 0x80u) - 32.0f))
                press = g_fp_walk.x < (FP_MEMF(t + 0x78u) + FP_MEMF(t + 0x80u)) * 0.5f
                      ? "dpad_left" : "dpad_right";
            recomp_bindings_press(port, press, 80);
        }
        memset(&g_fp_walk, 0, sizeof g_fp_walk);
        return;
    }
    /* A press goes out, crosses the input delay, and is seen: until the
     * selection moves (or a while passes), no other. */
    if (g_fp_walk.at && sel == g_fp_walk.moved_from && now - g_fp_walk.at < 400)
        return;
    if (g_fp_walk.at && now - g_fp_walk.at < 90)
        return;
    if (++g_fp_walk.presses > 12) {
        memset(&g_fp_walk, 0, sizeof g_fp_walk);
        return;
    }
    at = fp_ui_index(w, sel);
    to = fp_ui_index(w, t);
    recomp_bindings_press(port, at >= 0 && to < at ? "dpad_up" : "dpad_down", 50);
    g_fp_walk.moved_from = sel;
    g_fp_walk.at = now;
}

static void fp_menu_mouse(uint32_t root)
{
    static float last_x = -1.0f, last_y = -1.0f;
    static int wheel_left;
    static uint32_t hold_item;              /* the value a held left button keeps stepping */
    static const char *hold_dir;
    long now = (long)(clock() * 1000 / CLOCKS_PER_SEC);
    int port = recomp_bindings_mouse_port(), lists = 0, moved, wheel = 0, have, value, tab;
    uint32_t item, widget;
    unsigned clicks, held;
    float cx, cy, fx, fy, fw, fh, x = -1.0f, y = -1.0f;

    if (port < 0)
        return;                             /* no mouse bound: nothing to point with */
    if (now - g_fp_look_tick <= 100) {
        recomp_mouse_pointer_mode(0);       /* in play: the mouse aims */
        return;
    }
    item = fp_ui_hit(root, -1.0f, -1.0f, &widget, &lists);
    recomp_mouse_pointer_mode(lists);
    if (!lists)
        return;
    have = recomp_mouse_pointer(&cx, &cy);
    clicks = recomp_mouse_pointer_clicks(&wheel);
    held = recomp_mouse_pointer_held();
    if (have) {
        xbox_D3D8PictureRect(&fx, &fy, &fw, &fh);
        x = (cx - fx) / fw * 640.0f;
        y = (cy - fy) / fh * 480.0f;
    }
    fp_pointer_test(&x, &y, &clicks, &held, &wheel, &have);
    moved = have && (fabsf(x - last_x) >= 1.0f || fabsf(y - last_y) >= 1.0f);
    if (have) {
        last_x = x;
        last_y = y;
    }
    if (fp_cs_screen(fp_ui_hit(root, 320.0f, 200.0f, &widget, &lists))) {
        if (fp_cs_mouse((unsigned)port, now, have ? x : -1.0f, have ? y : -1.0f, moved, clicks,
                        wheel))
            clicks &= ~(unsigned)RECOMP_MOUSE_LEFT;
    } else {
        g_fp_cs.target = -1;
    }
    item = have ? fp_ui_hit(root, x, y, &widget, &lists) : 0;
    /* In a session, selecting by writing the game's menu state would do it on
     * one machine only. Pointing at an item walks the selection there by
     * presses instead -- up and down, as a pad would, which both machines
     * see -- and a click there does what it does once it is reached. */
    if (hle_net_local_port() >= 0) {
        if (item && FP_MEM32(widget + 0x198u) != item) {
            if (moved || (clicks & RECOMP_MOUSE_LEFT))
                fp_walk_to(widget, item, (clicks & RECOMP_MOUSE_LEFT) != 0, x);
            item = 0;
            clicks &= ~(unsigned)RECOMP_MOUSE_LEFT;
        }
        fp_walk((unsigned)port, now);
    } else if (item && (moved || (clicks & RECOMP_MOUSE_LEFT)) &&
               FP_MEM32(widget + 0x198u) != item) {
        fp_ui_select(widget, item);
    }
    value = item && fp_ui_value(item);
    /* A value that A also opens (Weapon Set and Bot Set: their editors) ends
     * in A's prompt; a click there is A. */
    if (value && FP_MEM32(item + 0x38u) && x >= FP_MEMF(item + 0x80u) - 32.0f)
        value = 0;
    if ((clicks & RECOMP_MOUSE_LEFT) && value) {
        /* A value steps the way of the half clicked; held, the D-pad stays
         * down and the game's own repeat carries on. */
        hold_item = item;
        hold_dir = x < (FP_MEMF(item + 0x78u) + FP_MEMF(item + 0x80u)) * 0.5f ? "dpad_left"
                                                                            : "dpad_right";
        recomp_bindings_press((unsigned)port, hold_dir, 60);
    } else if ((clicks & RECOMP_MOUSE_LEFT) && item) {
        recomp_bindings_press((unsigned)port, "a", 80);
    } else if ((clicks & RECOMP_MOUSE_LEFT) && have) {
        tab = fp_ui_pause_tab(root, x, y);
        if (tab)
            recomp_bindings_press((unsigned)port, tab < 0 ? "dpad_left" : "dpad_right", 80);
    }
    if (hold_item && (held & RECOMP_MOUSE_LEFT) && item == hold_item)
        recomp_bindings_press((unsigned)port, hold_dir, 60);
    else if (!(clicks & RECOMP_MOUSE_LEFT))
        hold_item = 0;
    if (clicks & RECOMP_MOUSE_RIGHT)
        recomp_bindings_press((unsigned)port, "b", 80);
    /* The wheel, a notch a press: one at a time, so the menu sees each. Over
     * a value it turns the value (up more), elsewhere it moves the
     * selection. */
    wheel_left += wheel;
    if (wheel_left > 8) wheel_left = 8;
    if (wheel_left < -8) wheel_left = -8;
    {
        static long last_press;
        if (wheel_left && now - last_press >= 120) {
            const char *dir = value ? (wheel_left > 0 ? "dpad_right" : "dpad_left")
                                    : (wheel_left > 0 ? "dpad_up" : "dpad_down");
            recomp_bindings_press((unsigned)port, dir, 60);
            wheel_left += wheel_left > 0 ? -1 : 1;
            last_press = now;
        }
    }
}

/* -- Future Perfect: co-op, straight to the main menu -------------------- */

extern unsigned long hle_lockstep_frame(void);

/*
 * Between the game starting and its main menu come how many players and a
 * profile for each: in co-op on two PCs, always two, and a profile each.
 * Gone through here for the players, as they would: each machine presses for
 * its own player only, through its own controls (recomp_bindings_press on
 * hle_net_local_input_port), so the presses go to the other machine like
 * any others and both games see the same. What to press is read from the
 * game -- the same on both -- by the screen's lists, whose items say what
 * they are (+0x20, the label):
 *
 *   "1 Player" .. "4 Player"      the host: down to 2 Player, A
 *   two profile panels (kind 3), the host's player 1's then player 2's:
 *     "Press START to Join"        Start
 *     "Create New Profile" + profiles
 *                                  the first profile free (not bit 0 or 2
 *                                  of +0x28: taken by the other player, or
 *                                  a title), else Create New Profile; A
 *     "Enter Name"                 the name it offers ("Player 2"): Done, A
 *     "Select Storage Device"      Xbox Hard Disk, A
 *     "Ready"                      nothing: the other player
 *   "Story Co-op"                 the main menu with both in: stop, and
 *                                  leave the rest -- Change Players
 *                                  included -- to the players
 *
 * A press, then a pause for the screen to follow. RECOMP_FP_COOP_AUTO=0
 * leaves all of it to the players.
 */
static int fp_ui_label_is(uint32_t item, const char *want)
{
    uint32_t s = item ? FP_MEM32(item + 0x20u) : 0;
    int c;

    if (!s)
        return !*want;
    for (c = 0; want[c]; c++)
        if (FP_MEM8(s + (uint32_t)c) != (uint8_t)want[c])
            return 0;
    return FP_MEM8(s + (uint32_t)c) == 0;
}

static int fp_ui_label_empty(uint32_t item)
{
    uint32_t s = FP_MEM32(item + 0x20u);
    return !s || !FP_MEM8(s);
}

/* The first item in a widget's list labelled `want`, or 0. */
static uint32_t fp_ui_find(uint32_t widget, const char *want)
{
    uint32_t it;
    int n = 0;

    for (it = FP_MEM32(widget + 0x178u); it && n < 128; it = FP_MEM32(it + 4u), n++)
        if (fp_ui_label_is(it, want))
            return it;
    return 0;
}

static int fp_ui_index(uint32_t widget, uint32_t item)
{
    uint32_t it;
    int n = 0;

    for (it = FP_MEM32(widget + 0x178u); it && n < 128; it = FP_MEM32(it + 4u), n++)
        if (it == item)
            return n;
    return -1;
}

/* Every widget under root with a list, in the order the game keeps them. */
static int fp_ui_lists(uint32_t root, uint32_t *out, int max)
{
    uint32_t queue[32];
    int head = 0, tail = 0, n = 0, guard = 0;

    queue[tail++] = root;
    while (head < tail && guard++ < 64) {
        uint32_t w = queue[head++], c;
        int k = 0;

        if (w < FP_UI_TABLE || w >= FP_UI_TABLE + FP_UI_WIDGETS * FP_UI_SIZE || !FP_MEM32(w))
            continue;
        if (FP_MEM32(w + 0x178u) && n < max)
            out[n++] = w;
        for (c = FP_MEM32(w + 0x16Cu); c && tail < 32 && k < 32; c = FP_MEM32(c + 0x30u), k++)
            queue[tail++] = c;
    }
    return n;
}

/* Toward `target` in a list: A on it, else a step up or down. */
static const char *fp_coop_toward(uint32_t widget, uint32_t target)
{
    uint32_t sel = FP_MEM32(widget + 0x198u);
    int at = fp_ui_index(widget, sel), to = fp_ui_index(widget, target);

    if (sel == target)
        return "a";
    if (at < 0 || to < 0)
        return "dpad_down";
    return to > at ? "dpad_down" : "dpad_up";
}

/* The frame the main menu (both players in) was last up: B and Back there
 * go back to the player select, which co-op has no use for (fp_net_filter). */
static unsigned long g_fp_main_menu_frame;

/* In a session, this machine's controls before they go out: no B or Back
 * on the main menu. Alt+Tab pressed Back there (Tab is Back), and the
 * player select it led to is co-op's undoing -- fp_coop_auto would put it
 * right, but only by going through it again. */
static void fp_net_filter(RecompInputGamepad *g)
{
    unsigned long frame = hle_lockstep_frame();

    if (g_fp_main_menu_frame && frame - g_fp_main_menu_frame <= 2u) {
        g->analog_buttons[1] = 0;           /* B */
        g->buttons &= (uint16_t)~0x0020u;   /* Back */
    }
}

static void fp_coop_auto(uint32_t root)
{
    static int enabled = -1, done;
    static long next;
    static unsigned presses;
    long now = (long)(clock() * 1000 / CLOCKS_PER_SEC);
    uint32_t lists[8], panel = 0, it;
    int n, i, me, panels = 0;
    const char *press = NULL;

    if (enabled < 0) {
        const char *e = getenv("RECOMP_FP_COOP_AUTO");
        enabled = !(e && *e && strcmp(e, "0") == 0);
    }
    if ((me = hle_net_local_port()) < 0)
        return;
    n = fp_ui_lists(root, lists, 8);
    for (i = 0; i < n; i++)
        if (fp_ui_find(lists[i], "Story Co-op")) {
            g_fp_main_menu_frame = hle_lockstep_frame();
            if (enabled && !done) {
                done = 1;
                fprintf(stderr, "[FP] co-op: both players are in, at the main menu (%u presses)\n",
                        presses);
                fflush(stderr);
            }
            return;
        }
    /* Back at the player count -- the attract movie ends there, or the
     * players found a way -- and it is all to do again. */
    for (i = 0; i < n && done; i++)
        if (fp_ui_find(lists[i], "1 Player") && fp_ui_find(lists[i], "2 Player")) {
            done = 0;
            presses = 0;
        }
    if (!enabled || done || now < next)
        return;
    for (i = 0; i < n && !press; i++) {
        uint32_t w = lists[i];

        if (fp_ui_find(w, "1 Player") && fp_ui_find(w, "2 Player")) {
            if (me == 0)
                press = fp_coop_toward(w, fp_ui_find(w, "2 Player"));
            break;
        }
        if (FP_MEM32(w) == 3u && panels++ == me)
            panel = w;
    }
    if (!press && panel) {
        if (fp_ui_find(panel, "Press START to Join")) {
            press = "start";
        } else if ((it = fp_ui_find(panel, "Create New Profile")) != 0) {
            uint32_t create = it, pick = 0, p;
            int k = 0;
            for (p = FP_MEM32(it + 4u); p && k < 64 && !pick; p = FP_MEM32(p + 4u), k++)
                if (FP_MEM32(p) == 7u && !(FP_MEM32(p + 0x28u) & 5u) && !fp_ui_label_empty(p))
                    pick = p;
            press = fp_coop_toward(panel, pick ? pick : create);
        } else if (fp_ui_find(panel, "Enter Name")) {
            press = fp_ui_label_is(FP_MEM32(panel + 0x198u), "Done") ? "a"
                  : (presses & 1u) ? "dpad_left" : "dpad_up";
        } else if ((it = fp_ui_find(panel, "Xbox Hard Disk")) != 0) {
            press = fp_coop_toward(panel, it);
        }
    }
    if (!press)
        return;
    if (++presses > 200) {
        done = 1;                       /* something unforeseen: leave it to the players */
        fprintf(stderr, "[FP] co-op: the screens before the main menu are the players' now\n");
        fflush(stderr);
        return;
    }
    recomp_bindings_press((unsigned)hle_net_local_input_port(), press, 80);
    next = now + (strcmp(press, "a") == 0 || strcmp(press, "start") == 0 ? 700 : 250);
}

/* A screen's widgets, once a frame (above): the mouse first, at the
 * outermost call; the calls it makes for the children come back in here.
 * In co-op, the screens before the main menu are gone through first. */
void sub_0007A7B0(void)
{
    static int depth;

    if (!depth)
        fp_coop_auto(FP_MEM32(g_esp + 4u));
    if (!depth)
        fp_menu_mouse(FP_MEM32(g_esp + 4u));
    depth++;
    sub_0007A7B0_gen();
    depth--;
}

/* -- Future Perfect: "Xbox Live" and "System Link" renamed --------------- */

/*
 * The game's text is two banks, each a table of pointers to its strings and
 * then the strings, at [0x006B7B08] (0x291C0 bytes at [0x006B7B0C], 4149
 * strings: everything the game says) and [0x006B7B10] (size at 0x006B7B14,
 * 3463: the front end's), found by the strings in a RAM snapshot and the
 * globals pointing at their tables. Menus take their labels from these
 * strings by pointer, so a string changed in place is a label changed. Each
 * new name is shorter than the old, so it fits: the rest of the string moves
 * up. A bank is patched when it appears or moves, and again if a patched
 * string reads as it did before (loaded again at the same place).
 */
static const struct { const char *from, *to; } k_fp_names[] = {
    { "on Xbox Live or System Link", "online or over LAN" },   /* the main menu's help */
    { "System Link", "LAN" },
    { "Xbox Live",   "Online" },
};

/* Every occurrence in [p, p + n) changed, string by string; the first
 * change's place in *mark (0 for none) and which name it was in *which.
 * Returns how many. */
static int fp_rename_block(uint32_t va, uint32_t n, uint32_t *mark, int *which)
{
    uint8_t *p = FP_GUEST(va), *end = p + n;
    int count = 0;
    size_t k;

    *mark = 0;
    for (k = 0; k < sizeof k_fp_names / sizeof k_fp_names[0]; k++) {
        size_t fl = strlen(k_fp_names[k].from), tl = strlen(k_fp_names[k].to);
        uint8_t *q;

        for (q = p; q + fl <= end; q++) {
            uint8_t *z;
            if (*q != (uint8_t)k_fp_names[k].from[0] || memcmp(q, k_fp_names[k].from, fl) != 0)
                continue;
            for (z = q + fl; z < end && *z; z++)
                ;                               /* the string's end */
            if (z >= end)
                break;
            memcpy(q, k_fp_names[k].to, tl);
            memmove(q + tl, q + fl, (size_t)(z - (q + fl)) + 1u);
            memset(z + 1 - (fl - tl), 0, fl - tl);
            if (!*mark) {
                *mark = va + (uint32_t)(q - p);
                *which = (int)k;
            }
            count++;
        }
    }
    return count;
}

static void fp_rename(void)
{
    static uint32_t bank[2], size[2], mark[2];
    static int which[2];
    int b;

    for (b = 0; b < 2; b++) {
        uint32_t va = FP_MEM32(0x006B7B08u + 8u * (uint32_t)b);
        uint32_t n = FP_MEM32(0x006B7B0Cu + 8u * (uint32_t)b);

        if (!va || n < 16u || n > 0x100000u)
            continue;
        if (va == bank[b] && n == size[b] &&
            (!mark[b] || memcmp(FP_GUEST(mark[b]), k_fp_names[which[b]].from,
                                strlen(k_fp_names[which[b]].from)) != 0))
            continue;                           /* as left */
        bank[b] = va;
        size[b] = n;
        if (fp_rename_block(va, n, &mark[b], &which[b]) > 0)
            fprintf(stderr, "[FP] text bank %d at %08X: Xbox Live / System Link renamed\n",
                    b, va);
    }
}

/* The screen's width and height in the game's terms, and whether it is
 * wider than 4:3 (the original: exactly 16:9). */
void sub_0003C400(void)
{
    fp_ui_dump();
    fp_rename();
    fp_aspect_follow();
    g_eax = FP_MEM32(0x004C0458u);
    g_esp += 4u;
}

void sub_0003C410(void)
{
    fp_aspect_follow();
    g_eax = FP_MEM32(0x004C045Cu);
    g_esp += 4u;
}

void sub_0003C420(void)
{
    fp_aspect_follow();
    g_eax = FP_MEM32(0x004C0458u) * 3u > FP_MEM32(0x004C045Cu) * 4u ? 1u : 0u;
    g_esp += 4u;
}

/* RECOMP_FP_SCAN=<file>:<from ms>:<ms long>: each look-builder call in the
 * window, on the input script's clock, appends a record -- the clock, the
 * view's yaw and pitch, the look vector, then the player ([0x007B0C7C],
 * 0x1600 bytes), its camera ([player+8], 0x400) and [player+0x14] (0x400) --
 * so the fields that follow the view can be found offline. */
static void fp_scan(uint32_t look_va, int after)
{
    static int parsed;
    static FILE *f;
    static unsigned long from, len;
    unsigned long long t;
    uint32_t pl = FP_MEM32(0x007B0C7Cu), base[3], size[3] = { 0x1600u, 0x400u, 0x400u };
    float hdr[6];
    int i;

    if (!parsed) {
        const char *s = getenv("RECOMP_FP_SCAN");
        char path[512], *c;
        parsed = 1;
        if (!s || !*s)
            return;
        snprintf(path, sizeof path, "%s", s);
        c = strrchr(path, ':');
        if (!c) return;
        len = strtoul(c + 1, NULL, 10);
        *c = 0;
        c = strrchr(path, ':');
        if (!c) return;
        from = strtoul(c + 1, NULL, 10);
        *c = 0;
        f = fopen(path, "wb");
    }
    if (!f || !pl)
        return;
    t = recomp_input_script_ms();
    if (t < from || t >= from + len)
        return;
    base[0] = pl;
    base[1] = FP_MEM32(pl + 8u);
    base[2] = FP_MEM32(pl + 0x14u);
    fp_view_angles(&hdr[0], &hdr[1]);
    hdr[2] = FP_MEMF(look_va);
    hdr[3] = FP_MEMF(look_va + 4u);
    hdr[4] = (float)after;
    hdr[5] = (float)FP_MEM32(pl + 0xF5Cu);
    fwrite(&t, 8, 1, f);
    fwrite(hdr, sizeof hdr, 1, f);
    for (i = 0; i < 3; i++) {
        static uint8_t zero[0x1600];
        fwrite(&base[i], 4, 1, f);
        if (base[i] >= 0x10000u)
            fwrite((const void *)FP_GUEST(base[i]), 1, size[i], f);
        else
            fwrite(zero, 1, size[i], f);
    }
    fflush(f);
}

/* The look builder, once per game tick while the player is in play: the
 * field of view, then the game's own look, then the mouse over it. */
void sub_0025CE80(void)
{
    uint32_t look_va = FP_MEM32(g_esp + 4u);

    fp_poke();
    fp_apply_fov();
    fp_apply_weapon_fov();
    sub_0025CE80_gen();
    g_fp_look_tick = (long)(clock() * 1000 / CLOCKS_PER_SEC);
    fp_mouse_aim(look_va);
    fp_look_test(look_va);
    fp_scan(look_va, 0);
}

/* -- Future Perfect: lockstep ---------------------------------------------- */

/*
 * Two copies of the game in lockstep must step their worlds alike, and the
 * world steps by the vblanks since the last frame: the frame clock
 * (sub_001538C0) reads the vblank count through sub_00022030 -- the count
 * at 0x00563918, raised by the vblank interrupt's handler sub_00022020 --
 * and takes the difference from the count it kept at 0x007A58AC, spinning
 * while it is under one, three at most. Vblanks arrive with real time, so a
 * frame that took long on one machine stepped further there. The one other
 * reader is the loading screen's loop (sub_00156CB0), which draws a frame
 * of it (sub_00156890) for each vblank until the load is done.
 *
 * Under RECOMP_LOCKSTEP_CLOCK the frame clock is answered "one more than
 * last time": every frame steps exactly once, whatever the timing, with
 * Swap still held to the vblank for pacing. The loading loop keeps the real
 * count -- it must, to notice the load end -- and RECOMP_LOCKSTEP_LOADER=skip
 * leaves out the loading screen's drawing, to see whether its frames reach
 * the game's state.
 */
extern int xbox_LockstepClockOn(void);
extern void xbox_LockstepGateThread(uint32_t start_context);
extern void sub_00022030_gen(void);
extern void sub_00156890_gen(void);

void sub_00022030(void)
{
    if (xbox_LockstepClockOn() && FP_MEM32(g_esp) == 0x00153975u) {
        g_eax = FP_MEM32(0x007A58ACu) + 1u;         /* the frame clock: one step */
        g_esp += 4u;
        return;
    }
    sub_00022030_gen();
}

void sub_00156890(void)
{
    static int skip = -1;

    if (skip < 0) {
        const char *e = getenv("RECOMP_LOCKSTEP_LOADER");
        skip = xbox_LockstepClockOn() && e && strcmp(e, "skip") == 0;
    }
    if (skip) {
        g_esp += 4u;                                /* cdecl: the caller pops */
        return;
    }
    sub_00156890_gen();
}

/* -- Future Perfect: one player's view, the whole screen ------------------ */

/*
 * Online co-op runs split-screen co-op on both machines, and each shows only
 * its own player. Split screen is laid out per camera by sub_0025A570(player,
 * rect): it asks how many screens there are (sub_001CDAE0) and gives the
 * camera's rectangle block -- the camera's +0x224, filled by sub_00055D90(rect,
 * x0, y0, x1, y1) with the corners, centre, size and share of the screen --
 * the whole screen, a half, or a quarter. The view, its aspect, its scissor
 * and the HUD all follow that block. Found by watching a write to player 2's
 * (+0x238, the top row: 240) back to sub_00055D90 and its caller.
 *
 * Two machines in lockstep must lay the screen out alike -- giving each
 * player's camera a size of its own made them part within frames (buffers
 * sized by the view, characters animated by whether they are seen) -- so in
 * a network session (hle_net_local_port) every player's camera is given the
 * whole screen, on both machines. Both views are still drawn, one after the
 * other, as split screen draws them; each machine keeps only its own
 * player's draws (fp_keep_draw, xbox_D3D8SetDrawFilter).
 * FP_SOLO_VIEW=<player index> does the same without a session, to test one
 * view.
 */
extern void sub_0025A570_gen(void);
extern void sub_00055D90(void);
extern void xbox_D3D8SetDrawFilter(int (*keep)(void));


static int fp_solo_view(void)
{
    static int fixed = -2;
    if (fixed == -2) {
        const char *e = getenv("FP_SOLO_VIEW");
        fixed = e && *e ? atoi(e) : -1;
    }
    return fixed >= 0 ? fixed : hle_net_local_port();
}

static void fp_set_rect(uint32_t rect, int x0, int y0, int x1, int y1)
{
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx;

    g_esp -= 4u; FP_MEM32(g_esp) = (uint32_t)y1;
    g_esp -= 4u; FP_MEM32(g_esp) = (uint32_t)x1;
    g_esp -= 4u; FP_MEM32(g_esp) = (uint32_t)y0;
    g_esp -= 4u; FP_MEM32(g_esp) = (uint32_t)x0;
    g_esp -= 4u; FP_MEM32(g_esp) = rect;
    g_esp -= 4u; FP_MEM32(g_esp) = 0u;          /* the return address; it pops it */
    sub_00055D90();
    g_esp += 20u;
    g_eax = eax;
    g_ecx = ecx;
    g_edx = edx;
}

void sub_0025A570(void)
{
    uint32_t rect = FP_MEM32(g_esp + 8u);

    sub_0025A570_gen();
    if (fp_solo_view() >= 0 && rect)
        fp_set_rect(rect, 0, 0, (int)FP_MEM32(0x004E8CF0u) - 1, (int)FP_MEM32(0x004E8CF4u) - 1);
}

/* The draw filter. The frame (sub_0003C440) draws the screens in two loops,
 * each setting the current player ([0x007B0C7C]) and camera ([0x007B0CC0])
 * before a call: the world, sub_000C8120 (from 0x0003C794), then each
 * player's HUD, sub_000C84A0 (from 0x0003C842). Those calls are the other
 * player's to leave out -- their clears too, which would wipe this player's
 * view. Everything else is kept: the frame's first clear, cutscenes (one
 * camera, drawn outside the loops), rendered textures, menus. Measured: 392
 * draws for player 1's world, 331 for player 2's, 26 and 15 for the HUDs. */
extern void sub_000C8120_gen(void);
extern void sub_000C84A0_gen(void);

static volatile int g_fp_view = -1;      /* whose screen is being drawn, or -1 */
extern unsigned long hle_lockstep_frame(void);

static void fp_view_pass(void (*draw)(void), uint32_t loop_return)
{
    uint32_t cur = FP_MEM32(0x007B0C7Cu);
    int was = g_fp_view;

    /* Only in play -- the look builder ran within 150 ms; a cutscene stops it
     * -- and only with the player's own camera ([0x007B0CC0] is [player+8]):
     * a cutscene goes through the same loop, everyone's to see. */
    if (fp_solo_view() >= 0 && FP_MEM32(g_esp) == loop_return && cur &&
        FP_MEM32(0x007B0CC0u) == FP_MEM32(cur + 8u) &&
        (long)(clock() * 1000 / CLOCKS_PER_SEC) - g_fp_look_tick <= 150)
        g_fp_view = (int)FP_MEM32(cur + 4u);
    draw();
    g_fp_view = was;
}

void sub_000C8120(void) { fp_view_pass(sub_000C8120_gen, 0x0003C799u); }
void sub_000C84A0(void) { fp_view_pass(sub_000C84A0_gen, 0x0003C847u); }

/* The other player's screen is left out only while this player has one of
 * their own: a level's intro is drawn through player 1's camera alone (player
 * 2's pass draws a handful of things), and player 2 must see it. So each
 * pass's draws are counted, and the last frame's count decides. */
static unsigned long g_fp_pass_draws[4], g_fp_pass_last[4], g_fp_pass_frame;

static int fp_keep_draw(void)
{
    int solo = fp_solo_view();
    unsigned long frame;

    if (solo < 0 || solo > 3 || g_fp_view < 0 || g_fp_view > 3)
        return 1;
    frame = hle_lockstep_frame();
    if (frame != g_fp_pass_frame) {
        memcpy(g_fp_pass_last, g_fp_pass_draws, sizeof g_fp_pass_last);
        memset(g_fp_pass_draws, 0, sizeof g_fp_pass_draws);
        g_fp_pass_frame = frame;
    }
    g_fp_pass_draws[g_fp_view]++;
    return g_fp_view == solo || g_fp_pass_last[solo] < 50;
}

/* -- Future Perfect: what the runtime is told before the game starts ----- */

/* Aim mode, for holding the aim button rather than clicking it
 * (recomp_bindings_toggle_reader): [player+0xF5C], the flag sub_0025D520
 * steers by. -1 outside play, so a held button presses nothing in a menu. */
static int fp_aim_on(unsigned port)
{
    FpLook rd;
    long now = (long)(clock() * 1000 / CLOCKS_PER_SEC);

    (void)port;
    if (now - g_fp_look_tick > 100 || !fp_look_reader(&rd))
        return -1;
    return rd.aim;
}

/* Crouching, for holding the crouch key: bit 0x10 of [player+0xC], beside
 * aim mode's 0x20. Found by RAM snapshots standing, crouched, standing,
 * crouched (RECOMP_RAM_SNAPSHOT over 0x82000000): the one flag that follows,
 * with the eye height at +0x94 halving alongside it. */
static int fp_crouch_on(unsigned port)
{
    uint32_t pl = FP_MEM32(0x007B0C7Cu);
    long now = (long)(clock() * 1000 / CLOCKS_PER_SEC);

    (void)port;
    if (now - g_fp_look_tick > 100 || !pl)
        return -1;
    return (FP_MEM32(pl + 0xCu) & 0x10u) != 0;
}

/* In play, for pausing when the window loses the focus: the look builder
 * runs every tick of play and in no menu, pause screen or cutscene, so
 * Start pressed then pauses and never resumes. Player 1's port. */
static int fp_in_play(unsigned port)
{
    long now = (long)(clock() * 1000 / CLOCKS_PER_SEC);

    return port == 0 && now - g_fp_look_tick <= 100 && FP_MEM32(0x007B0C7Cu) ? 1 : 0;
}

/* Called from main.c before the game's entry point. */
void fp_title_setup(void)
{
    /* The EA Games logo (one per language) and Free Radical's, before the
     * title screen; the attract-mode movie is not one of them. */
    xmv_set_intro_videos("eag_*.xmv;frd.xmv");
    recomp_bindings_toggle_reader("rthumb", fp_aim_on);
    recomp_bindings_toggle_reader("lthumb", fp_crouch_on);
    recomp_bindings_pause_on_focus_loss("start", fp_in_play);
    /* The button icons the front end and the prompts draw: 32x32 DXT3, by
     * the texture cache's checksum (found with RECOMP_HLE_D3D8_TEX_DUMP
     * through the menus and a level), shown as the keys bound to them while
     * the keyboard and mouse are in use. */
    recomp_prompts_icon(0xDDC6741Bu, "a", 0x4CB830u);
    recomp_prompts_icon(0x4D4970F1u, "b", 0xD8302Au);
    recomp_prompts_icon(0x1C76AFDDu, "x", 0x3A6CE0u);
    recomp_prompts_icon(0xCFBE7E42u, "y", 0xE8A020u);
    recomp_fov_support(55.0);               /* the camera's own, every level seen */
    recomp_weapon_fov_support(45.0);        /* the held weapon's, at that view */
    xbox_D3D8SetTitleAnyAspect(1);          /* fp_aspect_follow: 21:9 and the rest */
    /* The F1 panel in Future Perfect's blues, as its launcher is
     * (titles/futureperfect/CMakeLists.txt, PALETTE). */
    xbox_D3D8MenuSetPalette(0x3B5382u, 0x5E8387u, 0xFFFFFFu);
    /* Lockstep: the sound worker (sub_00030160: DirectSoundDoWork and the
     * game's stream service, then a 16 ms sleep) may run one pass a frame
     * (RECOMP_LOCKSTEP_GATE=1; not needed, see docs/technical/lockstep.md),
     * and the game's objects -- the contiguous heap from 0x82000000 -- are
     * what two machines compare to catch a desync. */
    xbox_LockstepGateThread(0x00030160u);
    hle_net_hash_range(0x82000000u, 0x01400000u);
    xbox_D3D8SetDrawFilter(fp_keep_draw);
    hle_net_set_local_filter(fp_net_filter);
    /* Frame interpolation: the projection at c60-c63, then the model-view
     * (c64), the projection times it (c68, which most programs draw with)
     * and a second model-view (c72). Bones are indexed from c96 and are not
     * blended. */
    xbox_D3D8SetInterpRegisters(60, 64, 12);
    /* The frame clock (sub_001538C0) spins on the vblank count, so Swap
     * comes just after a vblank: the gate holds it to the next one while
     * frames are interpolated, or there is no time to draw them in. */
    xbox_Nv2aFlipGateSetTitlePaced(1);
    {
        const char *e = getenv("RECOMP_FP_MOUSE_AIM");
        if (!(e && *e && strcmp(e, "0") == 0))
            recomp_mouse_declare_direct();      /* the menu shows zoomed aim */
    }
}

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    /*
     * Rate-limited per target, reporting at 1, 10, 100 ...
     *
     * An unresolved target inside a loop is the normal case, not the rare
     * one: Mortal Kombat: Deadly Alliance produced 46843 of these for a
     * single address in one 30-second run, seventeen lines each. That buries
     * every other diagnostic in the log and says nothing the first report did
     * not. The progression is the useful part -- one line says a target was
     * never identified, a run of them says the title is stuck on it.
     */
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }

    RECOMP_DIAG_LOCK();
    fprintf(stderr, "[ICALL] unresolved %starget 0x%08X -- %llu time(s) "
                    "(total calls: %llu)\n",
            g_icall_dispatch_form == 1 ? "call " :
            g_icall_dispatch_form == 2 ? "jump " : "",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    if (g_icall_dispatch_form == 2 && hits[i] == 1)
        fprintf(stderr, "  a jump, not a call: if this address is inside a "
                        "function rather than at its start, the guest is doing "
                        "its own control flow (a coroutine, a longjmp, or a "
                        "switch arm) and seeding it as a function will not "
                        "help\n");

    /*
     * Who called it. The lifted caller pushed its return address on the guest
     * stack, so code-range values above esp name the chain -- the same scan
     * the memory watchpoints use. It is a heuristic: stale return addresses
     * from earlier frames show up too, and the first entry is the reliable
     * one. It is still the difference between an address with no context and
     * a function to open, which is what an unresolved target needs.
     */
    {
        const uint8_t *stack = (const uint8_t *)g_xbox_mem_offset
                             + g_icall_saved_esp;
        int shown = 0;
        fprintf(stderr, "  callers:");
        for (i = 0; g_xbox_mem_offset && g_icall_saved_esp
                    && i < 160 && shown < 6; i++) {
            uint32_t v;
            memcpy(&v, stack + (size_t)i * 4, sizeof v);
            if (v >= g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "%s 0x%08X", shown ? " <-" : "", v);
                shown++;
            }
        }
        if (!shown)
            fprintf(stderr, " (none on the stack)");
        fprintf(stderr, "\n");
    }

    /* The recent-target ring buffer, once per address. What ran just before
     * an unresolved call is context for the first report and noise after. */
    if (hits[i] == 1) {
        fprintf(stderr, "  Recent ICALL targets:\n");
        for (i = 0; i < 16; i++) {
            int idx = (g_icall_trace_idx - 16 + i) & 15;
            if (g_icall_trace[idx])
                fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
        }
    }
    RECOMP_DIAG_UNLOCK();
    fflush(stderr);
}
/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va, uint32_t saved_esp)
{
    /* A power of ten, because the rate limiter above only reaches this
     * function body at 1, 10, 100 ... and the verdict has to land on one of
     * those. A hundred thousand skips of one target is unambiguous: no title
     * makes progress through that. */
    enum { SLOTS = 16, SPIN_VERDICT = 100000 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    static int said_it;   /* the verdict below is said once, not once per target */
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    {
        /* The call site, read off the guest stack. Without it the log
         * says a wild pointer was skipped but not by whom, and the
         * caller is the only thing that leads anywhere.
         *
         * It comes from the esp the dispatch macro captured, not from
         * g_esp. The lifted call site pushes its return address onto the
         * *local* esp and only syncs g_esp at certain points, so g_esp is
         * stale here -- it read 0 exactly when a null target most needed
         * explaining, which sent three rounds of Jet Set Radio Future
         * chasing inferences instead of a call site. saved_esp is the value
         * before that push, so the return address is the dword below it.
         *
         * Only for the call form. A tail jump pushes no return address, so
         * that dword is whatever the frame happened to leave there: Tony
         * Hawk's Pro Skater 2X reported "from 0x00000008" and then "from
         * 0x00000004" for the same million skipped calls, and both are
         * plainly not code. Printing a number that cannot be an address as
         * though it were the caller is worse than printing nothing, because
         * it is the one field a reader goes to next.
         *
         * So: name the form, and where the top of the stack cannot answer,
         * scan for code-range values the way the unresolved-target logger
         * above does. Stale return addresses from earlier frames come back
         * too and the first is the reliable one, but a list of real code
         * addresses is a place to start and 0x00000008 is not. */
        uint32_t caller = 0;
        if (g_icall_dispatch_form != 2 && saved_esp >= 4 && g_xbox_mem_offset)
            caller = *(const uint32_t *)((const uint8_t *)g_xbox_mem_offset
                                         + (saved_esp - 4));
        if (caller < g_xbox_code_lo || caller >= g_xbox_code_hi)
            caller = 0;
        RECOMP_DIAG_LOCK();
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped "
                        "%llu time(s) via a %s",
                va, (unsigned long long)hits[i],
                g_icall_dispatch_form == 2 ? "jump" : "call");
        if (caller)
            fprintf(stderr, " from 0x%08X", caller);
        fprintf(stderr, " (null or wild function pointer, at call #%llu)\n",
                (unsigned long long)g_icall_count);

        /* The stack scan, once per address: the same heuristic the
         * unresolved-target logger uses, and the only thing that names a
         * caller when the return address is not where it should be. */
        if (hits[i] == 1) {
            const uint8_t *stack = (const uint8_t *)g_xbox_mem_offset
                                 + saved_esp;
            int shown = 0, k;
            fprintf(stderr, "  callers:");
            for (k = 0; g_xbox_mem_offset && saved_esp
                        && k < 160 && shown < 6; k++) {
                uint32_t v;
                memcpy(&v, stack + (size_t)k * 4, sizeof v);
                if (v >= g_xbox_code_lo && v < g_xbox_code_hi) {
                    fprintf(stderr, "%s 0x%08X", shown ? " <-" : "", v);
                    shown++;
                }
            }
            if (!shown)
                fprintf(stderr, " (none on the stack)");
            fprintf(stderr, "\n");
        }

        /* RECOMP_ICALL_FATAL=1: fault here instead of skipping, so the crash
         * handler prints the guest call stack that reached this call.
         *
         * The caller printed above is read from the top of the guest stack,
         * which is right only when the lifted call site pushed a return
         * address there and nothing has since moved esp. When it reads 0 --
         * exactly the case where a null target most needs explaining -- the
         * line says a wild pointer was skipped and nothing about by whom. A
         * deliberate fault costs the run and buys the backtrace. */
        {
            static int fatal = -1;
            if (fatal < 0) {
                const char *v = getenv("RECOMP_ICALL_FATAL");
                fatal = v && *v && strcmp(v, "0") != 0;
            }
            if (fatal) {
                fprintf(stderr, "[ICALL] RECOMP_ICALL_FATAL: faulting here for "
                                "a backtrace\n");
                fflush(stderr);
                *(volatile int *)0 = 1;
            }
        }
    }

    /* Past a certain count this stops being a warning and becomes a verdict.
     *
     * A skipped call sets eax to 0 and returns. Zero is a fine answer for a
     * null function pointer in most code -- it reads as NULL, false, or
     * nothing -- but it is also S_OK, so in COM-shaped code a loop that
     * repeats while the result is non-negative can never leave. Panzer
     * Dragoon Orta hung exactly there, three billion skips in forty seconds,
     * with no frame ever presented.
     *
     * The value is deliberately left alone: there is no return value that is
     * right for both conventions, and guessing failure would break the
     * callers for which zero is correct. What can be fixed is the silence.
     * Say plainly that the title is hung, once, and say what to do about it.
     */
    if (hits[i] == SPIN_VERDICT && !said_it) {
        said_it = 1;
        fprintf(stderr,
            "[ICALL] ^^ this is a hang, not slow progress. One target has been\n"
            "        skipped %d times. The skip returns eax = 0, which is also\n"
            "        S_OK, so a loop testing for a non-negative result will\n"
            "        never exit. Try `py -3 -m tools.seed_from_log <log>` to\n"
            "        recover the target as a function, and read\n"
            "        docs/technical/memory-watchpoints.md for who wrote the\n"
            "        pointer. Set RECOMP_ICALL_SPIN_FATAL=1 to stop here\n"
            "        instead of spinning.\n", SPIN_VERDICT);
        fflush(stderr);
        if (getenv("RECOMP_ICALL_SPIN_FATAL")) {
            fprintf(stderr, "[ICALL] RECOMP_ICALL_SPIN_FATAL is set; exiting.\n");
            fflush(stderr);
            exit(3);
        }
    }
    RECOMP_DIAG_UNLOCK();
    fflush(stderr);
}

