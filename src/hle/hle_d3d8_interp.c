/*
 * hle_d3d8_interp.c -- frame interpolation: frames shown between the
 * title's own, drawn by the toolkit.
 *
 * A title that steps its logic once a frame cannot run faster than 60
 * without running fast. This leaves the title at 60 and draws n-1 more
 * frames in each period (RECOMP_FRAME_INTERP=n, `frame_interp = n`): the
 * newest frame's draws again, with the matrices in their vertex constants
 * blended part of the way back toward the frame before.
 *
 * Where the time comes from. The flip gate (xbox_memory_layout.h) holds a
 * finished frame until its vblank -- TimeSplitters 2 finishes in about 5 ms
 * and waits about 11 -- and lends that wait here
 * (xbox_Nv2aFlipGateSetIdle). So frame N is complete before the moment
 * halfway between N-1 and N, and the in-between frame can be drawn from
 * N-1 and N with no added latency: N is still shown at its vblank, as it
 * always was. The newest frame is drawn into a second image
 * (xbox_D3D8InterpBegin), because the scene target still holds N.
 *
 * What is kept (hle_d3d8_record.c, hle_d3d8_interp_rec). Every host call
 * that changes state or draws, as an op of the deferred queue's kind, from
 * the frame's start; the frame starts with a snapshot of the state set
 * before it (hle_d3d8_interp_snapshot), so drawing the list again draws the
 * frame again. A draw is kept with its key -- a hash of its vertex and index
 * bytes, its program and shape, its textures and target -- and the values
 * of the registers this blends, as src/hle set them. Releases and program
 * deletes the list may still name wait until the list is done with.
 *
 * Matching (match_frames), frame N's draws to N-1's, in three tiers:
 *   1. the same bytes, program, shape and target. Of several (the same mesh
 *      drawn twice), the one nearest in translation;
 *   2. the same program, shape, textures and target: a mesh the CPU
 *      rewrites;
 *   3. the same program, stride, primitive type and target: a batch the CPU
 *      builds, whose count and texture change every frame (particles).
 * Unmatched draws are drawn as N drew them.
 *
 * Blending (the registers: xbox_D3D8SetInterpRegisters, interp_projection,
 * interp_affine). The projection is blended term by term, if both frames
 * have the same kind. Each 4-register affine matrix is split into rotation,
 * scale and translation; the rotation is blended as a quaternion and the
 * other two linearly, so a turning object keeps its size. A matrix that
 * turned more than 45 degrees or moved by half its distance in one frame
 * is a cut, and drawn as N has it. Only draws whose program goes through
 * the projection are blended: screen-space draws are N's exactly, so the
 * HUD does not shimmer. A group whose last row is not (0,0,0,1) is taken
 * as the projection times an affine matrix, which a title that multiplies
 * them itself uploads (Future Perfect's c68-c71): the affine part is
 * unprojected with the projection register's value, blended, and
 * multiplied by the blended projection. A program that reads such a group
 * is 3D even if it never reads the projection itself.
 *
 * A frame is not interpolated (it is "held": the display keeps showing
 * N-1, as at 60) when it has no 3D, when less than 60% of its 3D
 * primitives matched, when more than half were cut, when a movie went
 * over it, while a capture is being written, or when the redraw would not
 * be done before the vblank.
 *
 * Limits: draws from push buffers the title fills itself never reach the
 * host, so cannot be redrawn. Particles the CPU positions stay at 60. A
 * title that does not clear the depth buffer each frame is not handled.
 * The deferred-frames mode (RECOMP_HLE_D3D8_DEFER) is not combined with
 * this; interpolation stays off there.
 *
 * Windows only, like the rest of shadow mode.
 */
#include "platform/xbox_winnt.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#include "d3d8_xbox.h"
#include "d3d8_internal.h"
#include "d3d8_vsh.h"
#include "hle_d3d8_record.h"
#include "recomp_config.h"
#include "../kernel/xbox_memory_layout.h"

#define INTERP_MAX_MULTIPLE  8
#define INTERP_MAX_AFFINE    64                  /* registers */
#define INTERP_MAX_REGS      (4 + INTERP_MAX_AFFINE)
#define INTERP_LIST_LIMIT    (96u * 1024u * 1024u)
#define INTERP_ALIGN(n)      (((n) + 15u) & ~(size_t)15u)

#define MIN_MATCHED          0.60                /* of the 3D primitives */
#define MAX_CUT              0.50
#define MAX_TURN_COS         0.92387953          /* cos(45 deg / 2) */
#define MAX_MOVE             0.5                 /* of the distance */

typedef struct {
    uint32_t size;                               /* payload bytes */
    int32_t  draw;                               /* index into draws, or -1 */
    void (*fn)(const void *arg);
} op_header;

typedef struct {
    HleInterpDrawKey key;
    int match;                                   /* draw in the frame before, or -1 */
    int tier;
    int cut;                                     /* matched, but a matrix jumped */
    int is3d;                                    /* through the projection, or a group
                                                    that has it multiplied in */
    int reads_proj;                              /* its program reads the projection */
    uint32_t groups;                             /* the affine matrices it reads */
} InterpDraw;

typedef struct {
    uint8_t   *ops;
    size_t     len, cap;
    InterpDraw *draws;
    float     *regs;                             /* g_nregs float4s a draw */
    int        ndraws, cap_draws;
    uint8_t   *retire;                           /* ops run when the list is done */
    size_t     rlen, rcap;
    int        whole;                            /* began at a frame start, all kept */
} InterpList;

/* A matrix ready to blend: rotation (a quaternion each side, the second
 * turned to the same hemisphere), the rest of the 3x3, the translation. */
typedef struct {
    int    ok;
    int    projected;                            /* the projection times this: blend the
                                                    affine part, multiply it back in */
    double qa[4], qb[4];
    double sa[9], sb[9];
    double ta[3], tb[3];
} Prepared;

static int        g_multiple;                    /* frames shown a frame drawn; <2 off */
static int        g_on;
static int        g_proj = -1, g_aff_first = -1, g_aff_count, g_nregs, g_groups;
static InterpList g_list[2];
static int        g_cur;                         /* the list being kept */
static Prepared  *g_prep;                        /* g_groups a draw of the current list */
static int        g_prep_cap;

/* This frame, in the gate. */
static int        g_prepared, g_frame_ok, g_slot;
static LONGLONG   g_last_present, g_qpf, g_redraw_est, g_presented_at;

/* RECOMP_INTERP_DUMP=<prefix>: in-between frames from swap
 * RECOMP_INTERP_DUMP_FROM on, as <prefix>_i<swap>_<k>.bmp, the k-th of the
 * frames shown before that swap's frame; at most 24, as the frame dumps.
 * Beside RECOMP_HLE_D3D8_DUMP_EVERY=1 from the same swap, the frames either
 * side. */
static const char   *g_dump_prefix;
static unsigned long g_dump_from, g_swaps;
static int           g_dumped;
static long          g_max_draws = -2;          /* RECOMP_INTERP_DRAWS, in redraw */

/* For the report. */
static unsigned long g_frames, g_shown, g_late, g_held_2d, g_held_match, g_held_cut,
                     g_held_other, g_presents_total;
static double        g_matched_sum, g_redraw_ms_sum, g_redraw_ms_max;
static double        g_alpha_sum, g_off_ms_sum, g_off_ms_max;
static unsigned long g_redraws, g_matched_frames;

unsigned long hle_d3d8_interp_presents(void)
{
    return g_presents_total;
}

/* ------------------------------------------------------------- the lists */

static int grow(uint8_t **buf, size_t *cap, size_t need)
{
    size_t c = *cap ? *cap : (size_t)1 << 20;
    uint8_t *b;

    if (need <= *cap)
        return 1;
    while (c < need)
        c *= 2;
    b = (uint8_t *)realloc(*buf, c);
    if (!b)
        return 0;
    *buf = b;
    *cap = c;
    return 1;
}

static void push(uint8_t **buf, size_t *len, size_t *cap, void (*fn)(const void *),
                 const void *arg, size_t size, int draw, int *whole)
{
    size_t need = sizeof(op_header) + INTERP_ALIGN(size);
    op_header *h;

    if (*len + need > INTERP_LIST_LIMIT || !grow(buf, cap, *len + need)) {
        if (whole)
            *whole = 0;                          /* not the whole frame: not drawn again */
        return;
    }
    h = (op_header *)(*buf + *len);
    h->size = (uint32_t)size;
    h->draw = draw;
    h->fn = fn;
    if (size)
        memcpy(h + 1, arg, size);
    *len += need;
}

void hle_d3d8_interp_op(void (*fn)(const void *arg), const void *arg, size_t size)
{
    InterpList *l = &g_list[g_cur];

    push(&l->ops, &l->len, &l->cap, fn, arg, size, -1, &l->whole);
}

void hle_d3d8_interp_draw(void (*fn)(const void *arg), const void *arg, size_t size,
                          const HleInterpDrawKey *key)
{
    InterpList *l = &g_list[g_cur];
    const float *c = hle_d3d8_interp_constants();
    float *r;

    if (l->ndraws >= l->cap_draws) {
        int n = l->cap_draws ? l->cap_draws * 2 : 1024;
        InterpDraw *d = (InterpDraw *)realloc(l->draws, (size_t)n * sizeof *d);
        float *g;

        if (!d) {
            l->whole = 0;
            return;
        }
        l->draws = d;
        g = (float *)realloc(l->regs, (size_t)n * (size_t)(g_nregs ? g_nregs : 1) * 4 *
                             sizeof(float));
        if (!g) {
            l->whole = 0;
            return;
        }
        l->regs = g;
        l->cap_draws = n;
    }
    l->draws[l->ndraws].key = *key;
    l->draws[l->ndraws].match = -1;
    l->draws[l->ndraws].tier = 0;
    l->draws[l->ndraws].cut = 0;
    /* Only what the program reads is the draw's: a register it never reads
     * holds whatever was last put there, and may jump about. Asked now,
     * while the draw's program is the one bound.
     *
     * A program that never reads the projection is still 3D when a group it
     * reads has a last row other than (0,0,0,1): the title multiplied the
     * projection in itself (Future Perfect's c68-c71), and blend_projected
     * takes it back out. */
    {
        InterpDraw *d = &l->draws[l->ndraws];
        uint32_t reads = key->programmable && g_aff_count
                         ? d3d8_vsh_bound_reads(g_aff_first, g_aff_count) : 0;
        int projected = 0, g;

        for (g = 0; g < g_aff_count / 4 && g_proj >= 0; g++) {
            const float *w = c + (g_aff_first + g * 4 + 3) * 4;
            if (((reads >> g) & 1u) &&
                (fabsf(w[0]) > 1e-4f || fabsf(w[1]) > 1e-4f || fabsf(w[2]) > 1e-4f ||
                 fabsf(w[3] - 1.0f) > 1e-4f))
                projected = 1;
        }
        d->is3d = key->uses_proj || projected;
        d->reads_proj = key->uses_proj && g_proj >= 0 && (d3d8_vsh_bound_reads(g_proj, 4) & 1u);
        d->groups = d->is3d ? reads : 0;
    }
    r = l->regs + (size_t)l->ndraws * (size_t)g_nregs * 4;
    if (g_proj >= 0) {
        memcpy(r, c + g_proj * 4, 16 * sizeof(float));
        r += 16;
    }
    if (g_aff_count)
        memcpy(r, c + g_aff_first * 4, (size_t)g_aff_count * 4 * sizeof(float));
    push(&l->ops, &l->len, &l->cap, fn, arg, size, l->ndraws, &l->whole);
    l->ndraws++;
}

/* Run what the list put off, with nothing kept while it runs: these are the
 * real releases now. */
static void list_retire(InterpList *l)
{
    int rec = hle_d3d8_interp_rec;
    size_t off;

    hle_d3d8_interp_rec = 0;
    for (off = 0; off < l->rlen; ) {
        op_header *h = (op_header *)(l->retire + off);
        h->fn(h->size ? (const void *)(h + 1) : NULL);
        off += sizeof(op_header) + INTERP_ALIGN(h->size);
    }
    hle_d3d8_interp_rec = rec;
    l->rlen = 0;
    l->len = 0;
    l->ndraws = 0;
    l->whole = 0;
}

void hle_d3d8_interp_retire(void (*fn)(const void *arg), const void *arg, size_t size)
{
    InterpList *l = &g_list[g_cur];
    size_t before = l->rlen;

    push(&l->retire, &l->rlen, &l->rcap, fn, arg, size, -1, NULL);
    if (l->rlen == before) {
        /* No room to put it off: do it now, and let the list go. */
        int rec = hle_d3d8_interp_rec;
        l->whole = 0;
        hle_d3d8_interp_rec = 0;
        fn(arg);
        hle_d3d8_interp_rec = rec;
    }
}

/* ------------------------------------------------------------- matching */

typedef uint64_t (*key_fn)(const HleInterpDrawKey *k);

static uint64_t mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

static uint64_t key_same(const HleInterpDrawKey *k)
{
    uint64_t h = mix(k->content, k->vs);
    h = mix(h, ((uint64_t)k->stride << 32) | k->prims);
    h = mix(h, ((uint64_t)k->prim_type << 32) | k->index_format);
    return mix(h, (uint64_t)(uintptr_t)k->target);
}

static uint64_t key_shape(const HleInterpDrawKey *k)
{
    uint64_t h = mix(0x51ED270B27A3C1D5ull, k->vs);
    int s;

    h = mix(h, ((uint64_t)k->stride << 32) | k->prims);
    h = mix(h, ((uint64_t)k->prim_type << 32) | k->index_format);
    for (s = 0; s < 4; s++)
        h = mix(h, (uint64_t)(uintptr_t)k->tex[s]);
    return mix(h, (uint64_t)(uintptr_t)k->target);
}

static uint64_t key_batch(const HleInterpDrawKey *k)
{
    uint64_t h = mix(0x2545F4914F6CDD1Dull, k->vs);
    h = mix(h, ((uint64_t)k->stride << 32) | k->prim_type);
    return mix(h, (uint64_t)(uintptr_t)k->target);
}

/* The translation of a draw's first affine matrix, to choose between
 * copies of one mesh. */
static const float *draw_translation(const InterpList *l, int i, float out[3])
{
    const float *r;

    if (!g_aff_count)
        return NULL;
    r = l->regs + (size_t)i * (size_t)g_nregs * 4 + (g_proj >= 0 ? 16 : 0);
    out[0] = r[3];
    out[1] = r[7];
    out[2] = r[11];
    return out;
}

static void match_tier(InterpList *cur, const InterpList *prev, key_fn key, int tier,
                       uint8_t *used, uint64_t *keys, int *head, int *next, int size)
{
    int i, j;

    for (i = 0; i < size; i++)
        head[i] = -1;
    /* Chains in draw order: insert from the end. */
    for (j = prev->ndraws - 1; j >= 0; j--) {
        int slot;
        if (used[j])
            continue;
        keys[j] = key(&prev->draws[j].key);
        slot = (int)(keys[j] & (uint64_t)(size - 1));
        next[j] = head[slot];
        head[slot] = j;
    }
    for (i = 0; i < cur->ndraws; i++) {
        InterpDraw *d = &cur->draws[i];
        uint64_t k;
        int best = -1, c;
        float ti[3], tc[3];
        double best_dist = 0.0;
        const float *t;

        if (d->match >= 0)
            continue;
        k = key(&d->key);
        t = tier == 1 ? draw_translation(cur, i, ti) : NULL;
        for (c = head[(int)(k & (uint64_t)(size - 1))]; c >= 0; c = next[c]) {
            double dist;
            if (used[c] || keys[c] != k)
                continue;
            if (!t) {
                best = c;
                break;
            }
            /* Of several copies, the nearest: two identical crates must not
             * trade places halfway. */
            draw_translation(prev, c, tc);
            dist = (tc[0] - t[0]) * (tc[0] - t[0]) + (tc[1] - t[1]) * (tc[1] - t[1]) +
                   (tc[2] - t[2]) * (tc[2] - t[2]);
            if (best < 0 || dist < best_dist) {
                best = c;
                best_dist = dist;
            }
        }
        if (best >= 0) {
            used[best] = 1;
            d->match = best;
            d->tier = tier;
        }
    }
}

static void match_frames(InterpList *cur, const InterpList *prev)
{
    static uint8_t  *used;
    static uint64_t *keys;
    static int      *next, *head, cap, head_cap;
    int size = 1, i;

    while (size < prev->ndraws * 2)
        size <<= 1;
    if (prev->ndraws > cap) {
        cap = prev->ndraws * 2;
        used = (uint8_t *)realloc(used, (size_t)cap);
        keys = (uint64_t *)realloc(keys, (size_t)cap * sizeof *keys);
        next = (int *)realloc(next, (size_t)cap * sizeof *next);
    }
    if (size > head_cap) {
        head_cap = size;
        head = (int *)realloc(head, (size_t)head_cap * sizeof *head);
    }
    for (i = 0; i < cur->ndraws; i++) {
        cur->draws[i].match = -1;
        cur->draws[i].tier = 0;
        cur->draws[i].cut = 0;
    }
    if (!used || !keys || !next || !head || !prev->ndraws)
        return;
    memset(used, 0, (size_t)prev->ndraws);
    match_tier(cur, prev, key_same, 1, used, keys, head, next, size);
    match_tier(cur, prev, key_shape, 2, used, keys, head, next, size);
    match_tier(cur, prev, key_batch, 3, used, keys, head, next, size);
}

/* ------------------------------------------------------------- blending */

static double det3(const double *m)
{
    return m[0] * (m[4] * m[8] - m[5] * m[7]) -
           m[1] * (m[3] * m[8] - m[5] * m[6]) +
           m[2] * (m[3] * m[7] - m[4] * m[6]);
}

/* The rotation nearest m (its polar factor), by Higham's iteration:
 * R <- (R + R^-T) / 2. A few steps for any matrix a title draws with. */
static int polar_rotation(const double *m, double *r)
{
    int k, i;

    memcpy(r, m, 9 * sizeof(double));
    for (k = 0; k < 24; k++) {
        double d = det3(r), cof[9], change = 0.0;

        if (fabs(d) < 1e-12)
            return 0;
        cof[0] =  (r[4] * r[8] - r[5] * r[7]);
        cof[1] = -(r[3] * r[8] - r[5] * r[6]);
        cof[2] =  (r[3] * r[7] - r[4] * r[6]);
        cof[3] = -(r[1] * r[8] - r[2] * r[7]);
        cof[4] =  (r[0] * r[8] - r[2] * r[6]);
        cof[5] = -(r[0] * r[7] - r[1] * r[6]);
        cof[6] =  (r[1] * r[5] - r[2] * r[4]);
        cof[7] = -(r[0] * r[5] - r[2] * r[3]);
        cof[8] =  (r[0] * r[4] - r[1] * r[3]);
        for (i = 0; i < 9; i++) {
            double n = 0.5 * (r[i] + cof[i] / d);  /* R^-T = cof(R) / det(R) */
            change += fabs(n - r[i]);
            r[i] = n;
        }
        if (change < 1e-9)
            break;
    }
    return 1;
}

static void quat_from(const double *r, double *q)
{
    double w = sqrt(fmax(0.0, 1.0 + r[0] + r[4] + r[8])) * 0.5;
    double x = sqrt(fmax(0.0, 1.0 + r[0] - r[4] - r[8])) * 0.5;
    double y = sqrt(fmax(0.0, 1.0 - r[0] + r[4] - r[8])) * 0.5;
    double z = sqrt(fmax(0.0, 1.0 - r[0] - r[4] + r[8])) * 0.5;
    double n;

    x = copysign(x, r[7] - r[5]);
    y = copysign(y, r[2] - r[6]);
    z = copysign(z, r[3] - r[1]);
    n = sqrt(w * w + x * x + y * y + z * z);
    q[0] = w / n;
    q[1] = x / n;
    q[2] = y / n;
    q[3] = z / n;
}

static void quat_to(const double *q, double *r)
{
    double w = q[0], x = q[1], y = q[2], z = q[3];

    r[0] = 1 - 2 * (y * y + z * z); r[1] = 2 * (x * y - z * w);     r[2] = 2 * (x * z + y * w);
    r[3] = 2 * (x * y + z * w);     r[4] = 1 - 2 * (x * x + z * z); r[5] = 2 * (y * z - x * w);
    r[6] = 2 * (x * z - y * w);     r[7] = 2 * (y * z + x * w);     r[8] = 1 - 2 * (x * x + y * y);
}

static int is_affine(const float *g)
{
    return fabsf(g[12]) < 1e-4f && fabsf(g[13]) < 1e-4f && fabsf(g[14]) < 1e-4f &&
           fabsf(g[15] - 1.0f) < 1e-4f;
}

/* One side: rotation as a quaternion, S = R^T M, translation. A mirror is
 * kept in S, so the rotation is a proper one. */
static int split(const float *g, double *q, double *s, double *t, int *mirror)
{
    double m[9], r[9];
    int i, j, k;

    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++)
            m[i * 3 + j] = g[i * 4 + j];
        t[i] = g[i * 4 + 3];
    }
    *mirror = det3(m) < 0.0;
    if (*mirror)
        for (i = 0; i < 9; i++)
            m[i] = -m[i];
    if (!polar_rotation(m, r))
        return 0;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) {
            double v = 0.0;
            for (k = 0; k < 3; k++)
                v += r[k * 3 + i] * m[k * 3 + j];
            s[i * 3 + j] = *mirror ? -v : v;
        }
    quat_from(r, q);
    return 1;
}

/* m^-1 by Gauss-Jordan with partial pivoting; 0 if m is singular. */
static int invert4(const double *m, double *out)
{
    double a[4][8];
    int i, j, k;

    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            a[i][j] = m[i * 4 + j];
            a[i][4 + j] = i == j ? 1.0 : 0.0;
        }
    for (i = 0; i < 4; i++) {
        int p = i;
        double v;
        for (k = i + 1; k < 4; k++)
            if (fabs(a[k][i]) > fabs(a[p][i]))
                p = k;
        if (fabs(a[p][i]) < 1e-12)
            return 0;
        if (p != i)
            for (j = 0; j < 8; j++) {
                double t = a[i][j];
                a[i][j] = a[p][j];
                a[p][j] = t;
            }
        v = a[i][i];
        for (j = 0; j < 8; j++)
            a[i][j] /= v;
        for (k = 0; k < 4; k++)
            if (k != i && a[k][i] != 0.0) {
                double f = a[k][i];
                for (j = 0; j < 8; j++)
                    a[k][j] -= f * a[i][j];
            }
    }
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            out[i * 4 + j] = a[i][4 + j];
    return 1;
}

/* g is proj times an affine matrix: that matrix, proj^-1 g, in out. 0 when
 * it is not one -- proj is not what the title multiplied in (a stale
 * register, another camera's), or g is something else. The last row is
 * allowed what a float product of a far translation gets wrong, then made
 * exact. */
static int unproject(const float *proj, const float *g, float *out)
{
    double p[16], inv[16], m[16], scale = 1.0;
    int i, j, k;

    for (i = 0; i < 16; i++)
        p[i] = proj[i];
    if (!invert4(p, inv))
        return 0;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            double v = 0.0;
            for (k = 0; k < 4; k++)
                v += inv[i * 4 + k] * g[k * 4 + j];
            m[i * 4 + j] = v;
        }
    for (i = 0; i < 3; i++)
        if (fabs(m[i * 4 + 3]) > scale)
            scale = fabs(m[i * 4 + 3]);
    if (fabs(m[12]) > 1e-3 || fabs(m[13]) > 1e-3 || fabs(m[14]) > 1e-3 ||
        fabs(m[15] - 1.0) > 1e-4 * scale)
        return 0;
    for (i = 0; i < 12; i++)
        out[i] = (float)m[i];
    out[12] = out[13] = out[14] = 0.0f;
    out[15] = 1.0f;
    return 1;
}

/* a and b are a draw's group in the frame before and this one; pa and pb
 * the projection each side, or NULL when no register is one. */
static void prepare_group(Prepared *p, const float *a, const float *b, const float *pa,
                          const float *pb, int *cut)
{
    float ua[16], ub[16];
    int ma, mb;
    double dot, da, db, dt;

    p->ok = 0;
    p->projected = 0;
    if (memcmp(a, b, 16 * sizeof(float)) == 0)
        return;                                  /* nothing moved: drawn as it is */
    if (!is_affine(a) || !is_affine(b)) {
        if (!pa || !pb || !unproject(pa, a, ua) || !unproject(pb, b, ub))
            return;
        a = ua;
        b = ub;
        p->projected = 1;
    }
    if (!split(a, p->qa, p->sa, p->ta, &ma) || !split(b, p->qb, p->sb, p->tb, &mb) ||
        ma != mb) {
        *cut = 1;
        return;
    }
    dot = p->qa[0] * p->qb[0] + p->qa[1] * p->qb[1] + p->qa[2] * p->qb[2] +
          p->qa[3] * p->qb[3];
    if (dot < 0.0) {
        int i;
        for (i = 0; i < 4; i++)
            p->qb[i] = -p->qb[i];
        dot = -dot;
    }
    da = sqrt(p->ta[0] * p->ta[0] + p->ta[1] * p->ta[1] + p->ta[2] * p->ta[2]);
    db = sqrt(p->tb[0] * p->tb[0] + p->tb[1] * p->tb[1] + p->tb[2] * p->tb[2]);
    dt = sqrt((p->tb[0] - p->ta[0]) * (p->tb[0] - p->ta[0]) +
              (p->tb[1] - p->ta[1]) * (p->tb[1] - p->ta[1]) +
              (p->tb[2] - p->ta[2]) * (p->tb[2] - p->ta[2]));
    if (dot < MAX_TURN_COS || dt > MAX_MOVE * fmax(da, db) + 1e-3) {
        *cut = 1;                                /* a jump, not motion */
        return;
    }
    p->ok = 1;
}

static void blend_group(const Prepared *p, double a, float *out)
{
    double q[4], r[9], s[9], n;
    int i, j, k;

    for (i = 0; i < 4; i++)
        q[i] = (1.0 - a) * p->qa[i] + a * p->qb[i];
    n = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (i = 0; i < 4; i++)
        q[i] /= n;
    quat_to(q, r);
    for (i = 0; i < 9; i++)
        s[i] = (1.0 - a) * p->sa[i] + a * p->sb[i];
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            double v = 0.0;
            for (k = 0; k < 3; k++)
                v += r[i * 3 + k] * s[k * 3 + j];
            out[i * 4 + j] = (float)v;
        }
        out[i * 4 + 3] = (float)((1.0 - a) * p->ta[i] + a * p->tb[i]);
    }
    out[12] = 0.0f;
    out[13] = 0.0f;
    out[14] = 0.0f;
    out[15] = 1.0f;
}

static const float *regs_of(const InterpList *l, int i)
{
    return l->regs + (size_t)i * (size_t)g_nregs * 4;
}

/* Match frame N (the list kept) to N-1 and get every matched 3D draw's
 * matrices ready; decide whether the frame can be interpolated. */
static int prepare_frame(void)
{
    InterpList *cur = &g_list[g_cur], *prev = &g_list[g_cur ^ 1];
    double prims3d = 0.0, matched = 0.0, cut = 0.0;
    int i, g;

    if (!cur->ndraws) {
        g_held_2d++;                             /* nothing drawn: a load, a fade */
        return 0;
    }
    if (!cur->whole || !prev->whole) {
        if (getenv("RECOMP_INTERP_DEBUG"))
            fprintf(stderr, "[INTERP] swap %lu held: this frame %s, %d draws; the one "
                    "before %s, %d draws\n", g_swaps + 1, cur->whole ? "whole" : "not whole",
                    cur->ndraws, prev->whole ? "whole" : "not whole", prev->ndraws);
        g_held_other++;
        return 0;
    }
    if (cur->ndraws * g_groups > g_prep_cap) {
        int n = cur->ndraws * g_groups * 2;
        Prepared *p = (Prepared *)realloc(g_prep, (size_t)n * sizeof *p);
        if (!p) {
            g_held_other++;
            return 0;
        }
        g_prep = p;
        g_prep_cap = n;
    }
    match_frames(cur, prev);
    for (i = 0; i < cur->ndraws; i++) {
        InterpDraw *d = &cur->draws[i];

        if (!d->is3d)
            continue;
        prims3d += d->key.prims;
        if (d->match < 0)
            continue;
        matched += d->key.prims;
        {
            const float *pa = g_proj >= 0 ? regs_of(prev, d->match) : NULL;
            const float *pb = g_proj >= 0 ? regs_of(cur, i) : NULL;
            const float *a = regs_of(prev, d->match) + (g_proj >= 0 ? 16 : 0);
            const float *b = regs_of(cur, i) + (g_proj >= 0 ? 16 : 0);
            for (g = 0; g < g_groups; g++) {
                if ((d->groups >> g) & 1u)
                    prepare_group(&g_prep[i * g_groups + g], a + g * 16, b + g * 16, pa, pb,
                                  &d->cut);
                else
                    g_prep[i * g_groups + g].ok = 0;
            }
        }
        if (d->cut)
            cut += d->key.prims;
    }
    if (prims3d <= 0.0) {
        g_held_2d++;
        return 0;
    }
    g_matched_sum += matched / prims3d;
    g_matched_frames++;
    if (matched / prims3d < MIN_MATCHED) {
        g_held_match++;
        return 0;
    }
    if (cut / prims3d > MAX_CUT) {
        static int debug = -1, said;
        if (debug < 0)
            debug = getenv("RECOMP_INTERP_DEBUG") != NULL;
        if (debug && said < 40) {
            fprintf(stderr, "[INTERP] swap %lu held as a cut: %.0f of %.0f 3D primitives\n",
                    g_swaps + 1, cut, prims3d);
            for (i = 0; i < cur->ndraws && said < 40; i++) {
                const InterpDraw *d = &cur->draws[i];
                if (d->cut) {
                    const float *a = regs_of(prev, d->match) + (g_proj >= 0 ? 16 : 0);
                    const float *b = regs_of(cur, i) + (g_proj >= 0 ? 16 : 0);
                    fprintf(stderr, "[INTERP]   draw %d tier %d vs %lx prims %u: "
                            "t (%.2f %.2f %.2f) -> (%.2f %.2f %.2f)\n", i, d->tier,
                            (unsigned long)d->key.vs, d->key.prims, a[3], a[7], a[11],
                            b[3], b[7], b[11]);
                    said++;
                }
            }
        }
        g_held_cut++;
        return 0;
    }
    return 1;
}

/* Draw i's registers at a: blended where it was matched and is 3D, N's
 * own otherwise -- set either way, since the draw before may have left
 * blended ones. */
static void set_draw_registers(int i, double a)
{
    const InterpList *cur = &g_list[g_cur], *prev = &g_list[g_cur ^ 1];
    const InterpDraw *d = &cur->draws[i];
    const float *b = regs_of(cur, i);
    float out[INTERP_MAX_REGS * 4];
    int blend = d->is3d && d->match >= 0 && !d->cut;
    int g, k;

    memcpy(out, b, (size_t)g_nregs * 4 * sizeof(float));
    if (blend) {
        const float *pa = regs_of(prev, d->match);
        float *o = out;
        double proj[16] = {0};

        if (g_proj >= 0) {
            /* The same kind of projection both sides (the last row says
             * perspective or not), or it is a change of view, not motion. */
            int same = memcmp(pa + 12, b + 12, 4 * sizeof(float)) == 0;
            for (k = 0; k < 16; k++)
                proj[k] = same ? (1.0 - a) * pa[k] + a * b[k] : b[k];
            if (d->reads_proj && same)
                for (k = 0; k < 16; k++)
                    o[k] = (float)proj[k];
            o += 16;
        }
        for (g = 0; g < g_groups; g++) {
            const Prepared *p = &g_prep[i * g_groups + g];
            if (p->ok) {
                blend_group(p, a, o + g * 16);
                if (p->projected) {
                    /* The projection back in: the blended one times the
                     * blended affine matrix. */
                    float m[16];
                    int r, c2, n;
                    memcpy(m, o + g * 16, sizeof m);
                    for (r = 0; r < 4; r++)
                        for (c2 = 0; c2 < 4; c2++) {
                            double v = 0.0;
                            for (n = 0; n < 4; n++)
                                v += proj[r * 4 + n] * m[n * 4 + c2];
                            o[g * 16 + r * 4 + c2] = (float)v;
                        }
                }
            }
        }
    }
    if (g_proj >= 0) {
        d3d8_vsh_set_constant(g_proj, out, 4);
        if (g_aff_count)
            d3d8_vsh_set_constant(g_aff_first, out + 16, g_aff_count);
    } else if (g_aff_count) {
        d3d8_vsh_set_constant(g_aff_first, out, g_aff_count);
    }
}

/* ---------------------------------------------------------- drawing it */

static void redraw(double a)
{
    InterpList *cur = &g_list[g_cur];
    const float *c = hle_d3d8_interp_constants();
    int rec = hle_d3d8_interp_rec;
    size_t off;

    if (!xbox_D3D8InterpBegin()) {
        static int said;
        if (!said++) {
            fprintf(stderr, "[INTERP] the renderer has no second target; frame "
                    "interpolation off\n");
            fflush(stderr);
        }
        g_on = 0;
        return;
    }
    /* RECOMP_INTERP_DRAWS=<n>: only the first n draws of each in-between
     * frame (state still applied), as d3d8_replay --draws does for a
     * capture -- with RECOMP_INTERP_DUMP, the way to find the draw an
     * in-between frame gets wrong. */
    if (g_max_draws == -2) {
        const char *e = getenv("RECOMP_INTERP_DRAWS");
        g_max_draws = e && *e ? atol(e) : -1;
    }
    hle_d3d8_interp_rec = 0;                     /* what is drawn now is not kept */
    for (off = 0; off < cur->len; ) {
        op_header *h = (op_header *)(cur->ops + off);
        if (h->draw >= 0) {
            if (g_max_draws >= 0 && h->draw >= g_max_draws) {
                off += sizeof(op_header) + INTERP_ALIGN(h->size);
                continue;
            }
            set_draw_registers(h->draw, a);
        }
        h->fn(h->size ? (const void *)(h + 1) : NULL);
        off += sizeof(op_header) + INTERP_ALIGN(h->size);
    }
    if (g_dump_prefix && g_swaps + 1 >= g_dump_from && g_dumped < 24) {
        char path[512];
        snprintf(path, sizeof path, "%s_i%05lu_%d.bmp", g_dump_prefix, g_swaps + 1, g_slot);
        if (hle_d3d8_dump_back_buffer(path) == 0) {
            g_dumped++;
            fprintf(stderr, "[INTERP] in-between frame %d before swap %lu (blend %.3f) "
                    "-> %s\n", g_slot, g_swaps + 1, a, path);
        }
    }
    /* Back to frame N's registers as it left them. */
    if (g_proj >= 0)
        d3d8_vsh_set_constant(g_proj, c + g_proj * 4, 4);
    if (g_aff_count)
        d3d8_vsh_set_constant(g_aff_first, c + g_aff_first * 4, g_aff_count);
    hle_d3d8_overlay_redraw();
    {
        /* The moment it goes to the screen. A present can then block --
         * a Vulkan swap chain waits there for an image to draw the next
         * frame into -- but this one is already on its way. */
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        g_presented_at = t.QuadPart;
    }
    xbox_D3D8InterpPresent();
    xbox_D3D8InterpEnd();
    hle_d3d8_interp_rec = rec;
    g_presents_total++;
    g_shown++;
}

/* The gate's wait, lent (xbox_Nv2aFlipGateSetIdle). In-between frame k of
 * n is due k/n of the way from the last frame shown to the vblank that
 * shows this one; it is started early by what a redraw has been costing,
 * so it lands on time, and given up if it would not be done before the
 * vblank. Its blend is for the moment it is shown. */
static long long gate_idle(long long now, long long release)
{
    LONGLONG span = release - g_last_present;

    if (!g_on || !g_last_present)
        return 0;
    if (!g_prepared) {
        g_prepared = 1;
        g_frame_ok = 0;
        if (hle_d3d8_capture_active() || hle_d3d8_movie_layer_shown())
            g_held_other++;
        else
            g_frame_ok = prepare_frame();
    }
    if (!g_frame_ok || span <= g_qpf / 1000 || span > g_qpf / 10)
        return 0;
    while (g_slot < g_multiple) {
        LONGLONG due = g_last_present + span * g_slot / g_multiple;
        LONGLONG start = due - g_redraw_est;
        LARGE_INTEGER a, b;
        double alpha, ms;

        if (now < start)
            return start;
        if (now + g_redraw_est > release - g_qpf / 2000) {
            g_late += g_multiple - g_slot;       /* too late for the rest */
            g_slot = g_multiple;
            /* Nothing measured, so the estimate cannot correct itself: let
             * it fall back, or one slow redraw would hold every frame. */
            g_redraw_est -= g_redraw_est / 4;
            return 0;
        }
        alpha = (double)((now + g_redraw_est > due ? now + g_redraw_est : due) -
                         g_last_present) / (double)span;
        if (alpha > 1.0)
            alpha = 1.0;
        QueryPerformanceCounter(&a);
        redraw(alpha);
        QueryPerformanceCounter(&b);
        /* What a redraw costs up to its present, followed slowly, and never
         * taken as more than a quarter of the period: a frame that stalled
         * (a pipeline built, a page fault) is not what the next one costs. */
        {
            LONGLONG cost = g_presented_at - a.QuadPart;
            if (cost > span / 4)
                cost = span / 4;
            g_redraw_est = (g_redraw_est * 7 + cost) / 8;
        }
        ms = (double)(g_presented_at - a.QuadPart) * 1000.0 / (double)g_qpf;
        g_redraw_ms_sum += ms;
        g_redraws++;
        if (ms > g_redraw_ms_max)
            g_redraw_ms_max = ms;
        /* How far from its moment it reached the screen. */
        ms = fabs((double)(g_presented_at - due)) * 1000.0 / (double)g_qpf;
        g_off_ms_sum += ms;
        if (ms > g_off_ms_max)
            g_off_ms_max = ms;
        g_alpha_sum += alpha;
        g_slot++;
        if (!g_on)
            return 0;
        now = b.QuadPart;
    }
    return 0;
}

/* ---------------------------------------------------------- frame edges */

/* The registers: what the title or the settings say now. A change drops
 * what was kept, since its draws carry the old ones. */
static void follow_registers(void)
{
    int proj, first, count;

    d3d8_vsh_interp_registers(&proj, &first, &count);
    if (count > INTERP_MAX_AFFINE)
        count = INTERP_MAX_AFFINE;
    if (proj == g_proj && first == g_aff_first && count == g_aff_count && g_nregs)
        return;
    g_proj = proj;
    g_aff_first = first;
    g_aff_count = count;
    g_groups = count / 4;
    g_nregs = (proj >= 0 ? 4 : 0) + count;
    list_retire(&g_list[0]);
    list_retire(&g_list[1]);
    {
        int i;
        for (i = 0; i < 2; i++) {
            free(g_list[i].regs);
            free(g_list[i].draws);
            g_list[i].regs = NULL;
            g_list[i].draws = NULL;
            g_list[i].cap_draws = 0;
        }
    }
    fprintf(stderr, "[INTERP] blending c%d-c%d as the projection, %s%d affine "
            "matrices from c%d\n", proj, proj + 3, proj < 0 ? "(none) " : "",
            g_groups, first);
    fflush(stderr);
}

void hle_d3d8_interp_init(void)
{
    static int done;
    LARGE_INTEGER f;
    int n;

    if (done)
        return;
    done = 1;
    n = recomp_config_int("RECOMP_FRAME_INTERP", "frame_interp", 0);
    if (n < 2)
        return;
    if (n > INTERP_MAX_MULTIPLE)
        n = INTERP_MAX_MULTIPLE;
    if (hle_d3d8_defer_on()) {
        fprintf(stderr, "[INTERP] frame interpolation is not combined with deferred "
                "frames (RECOMP_HLE_D3D8_DEFER); off\n");
        fflush(stderr);
        return;
    }
    QueryPerformanceFrequency(&f);
    g_qpf = f.QuadPart;
    g_redraw_est = g_qpf / 500;                  /* 2 ms until measured */
    g_multiple = n;
    g_on = 1;
    g_dump_prefix = getenv("RECOMP_INTERP_DUMP");
    if (g_dump_prefix && !*g_dump_prefix)
        g_dump_prefix = NULL;
    {
        const char *from = getenv("RECOMP_INTERP_DUMP_FROM");
        g_dump_from = from ? (unsigned long)atol(from) : 0;
    }
    xbox_Nv2aFlipGateSetIdle(gate_idle);
    fprintf(stderr, "[INTERP] frame interpolation: %d frames shown for each the title "
            "draws (RECOMP_FRAME_INTERP)\n", n);
    fflush(stderr);
}

void hle_d3d8_interp_frame_end(void)
{
    LARGE_INTEGER now;

    if (!g_multiple)
        return;
    QueryPerformanceCounter(&now);
    g_last_present = now.QuadPart;
    g_frames++;
    g_swaps++;
    if (!g_on) {
        if (hle_d3d8_interp_rec) {
            hle_d3d8_interp_rec = 0;
            list_retire(&g_list[0]);
            list_retire(&g_list[1]);
        }
        return;
    }
    follow_registers();
    /* The frame just shown becomes the one before; what the one before it
     * put off can happen now, since neither will be drawn again. */
    list_retire(&g_list[g_cur ^ 1]);
    g_cur ^= 1;
    list_retire(&g_list[g_cur]);
    g_list[g_cur].whole = 1;
    hle_d3d8_interp_rec = 1;
    hle_d3d8_interp_snapshot();
    g_prepared = 0;
    g_frame_ok = 0;
    g_slot = 1;
}

void hle_d3d8_interp_report(void)
{
    unsigned long held = g_held_2d + g_held_match + g_held_cut + g_held_other;

    if (!g_multiple)
        return;
    fprintf(stderr, "[INTERP] %dx: %lu frames, %lu in-between shown, %lu late; held "
            "%lu (%lu no 3D, %lu matched too little, %lu cut, %lu other); matched "
            "%.0f%% of 3D primitives; redraw %.2f ms average, %.2f worst; shown "
            "%.2f ms from its moment on average, %.2f worst; blend %.2f average%s\n",
            g_multiple, g_frames, g_shown, g_late, held, g_held_2d, g_held_match,
            g_held_cut, g_held_other,
            g_matched_frames ? g_matched_sum * 100.0 / (double)g_matched_frames : 0.0,
            g_redraws ? g_redraw_ms_sum / (double)g_redraws : 0.0, g_redraw_ms_max,
            g_redraws ? g_off_ms_sum / (double)g_redraws : 0.0, g_off_ms_max,
            g_redraws ? g_alpha_sum / (double)g_redraws : 0.0,
            g_on ? "" : " (off)");
    fflush(stderr);
    g_frames = g_shown = g_late = g_held_2d = g_held_match = g_held_cut = 0;
    g_held_other = 0;
    g_matched_sum = g_redraw_ms_sum = g_redraw_ms_max = 0.0;
    g_alpha_sum = g_off_ms_sum = g_off_ms_max = 0.0;
    g_redraws = g_matched_frames = 0;
}

#endif /* _WIN32 */
