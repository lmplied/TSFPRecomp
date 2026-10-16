/*
 * hle_d3d8_texture.c -- D3DDevice_SetTexture, forwarded to the shadow device.
 *
 * Part of shadow mode (hle_d3d8.c, RECOMP_HLE_D3D8=shadow). The title's own
 * SetTexture runs first; then the texture it bound is found or built on the
 * host device and bound to the same stage there.
 *
 * An Xbox texture is a guest X_D3DPixelContainer (Cxbx-Reloaded,
 * XbD3D8Types.h): Common, Data, Lock, Format, Size. Data is a physical
 * address; the runtime keeps physical page P at guest 0x80000000 + P, where
 * MmAllocateContiguousMemory allocations live (xbox_memory_layout.c), so the
 * texels are read there. Format packs the D3DFORMAT (bits 8-15), mip levels
 * (16-19) and, for swizzled and compressed textures, log2 width and height
 * (20-23, 24-27). A linear texture instead has a non-zero Size: width-1,
 * height-1 and pitch/64-1, and one level (CxbxGetPixelContainerMeasures in
 * XbConvert.cpp).
 *
 * The host D3D8 layer takes Xbox format codes itself and keeps a mip chain in
 * the Xbox's own packing -- level after level, each row_pitch * rows -- which
 * it unswizzles and converts at upload. So swizzled and compressed levels are
 * copied as they are. That the guest packs its levels with no padding between
 * them is assumed, not verified.
 *
 * Titles write texels without any call the replacement could see (Lock is
 * inline), so a cached texture is checksummed again the first time it is
 * bound in each frame and uploaded again if it changed.
 *
 * P8 is forwarded: the host expands palettised texels to BGRA at upload
 * through the palette of the stage the texture is bound to, and
 * D3DDevice_SetPalette at the end of this file supplies that palette from the
 * guest's own resource. Because the expansion is baked, the palette a host
 * texture was baked with is remembered, and it is baked again when it is
 * drawn under a different one (sync_palette).
 *
 * Not handled, counted instead: cube and volume textures, surfaces bound as
 * textures, textures outside the contiguous window, and any format
 * d3d8_format_bpp does not know (named in the log when one is refused).
 */
#include "platform/xbox_winnt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hle.h"

#ifdef _WIN32
#include "d3d8_xbox.h"
#include "d3d8_internal.h"
#include "d3d8_swizzle.h"
#include "hle_d3d8_record.h"
#include "hle_texture_replace.h"

/* Reads a RECOMP_* switch; see xbox_memory_layout.h. Declared here rather than
 * including the kernel header, which this file otherwise has no need of. */
int xbox_EnvSwitch(const char *name, int default_on);

/* From hle_d3d8.c: the shadow device, or NULL, and its swap count. */
IDirect3DDevice8 *hle_d3d8_shadow_device(void);
unsigned long hle_d3d8_shadow_swaps(void);
/* Whether these texels are the frame buffer's -- see hle_d3d8.c. */
int hle_d3d8_is_framebuffer(uint32_t phys);
/* From hle_d3d8.c: where the texels of the texture bound to a stage live, so
 * a draw can tell whether it samples the playing movie. */
void hle_d3d8_note_stage_texels(uint32_t stage, uint32_t phys);
int hle_d3d8_trace_on(void);

#define CONTIG_BASE          0x80000000u
#define CONTIG_SIZE          (64u * 1024u * 1024u)   /* kernel.h XBOX_CONTIG_SIZE */
#define COMMON_TYPE_MASK     0x00070000u
#define COMMON_TYPE_TEXTURE  0x00040000u
#define COMMON_TYPE_SURFACE  0x00050000u
#define FORMAT_CUBEMAP       0x00000004u
#define XFMT_P8              0x0B
#define TEXTURE_CACHE        512
#define MAX_STAGES           4

typedef struct {
    uint32_t           va, data, format, size;   /* guest identity */
    IDirect3DTexture8 *host;
    uint32_t           checksum;
    unsigned long      checked_swap, used_swap;
    /* The cheap check (texel_sample) and the frame of the last full one;
     * see texels_changed. */
    uint32_t           quick;
    unsigned long      full_swap;
    /* The title renders into this texture (hle_d3d8_render_texture): the
     * host texture is a render target, and what shadow mode drew into it is
     * the content -- the guest's bytes are not, since nothing on the guest
     * side draws them, so it is never re-uploaded. */
    int                rendered;
    /* The texels are the frame buffer's, so the content comes from the
     * host's finished frame and is refreshed every frame it is bound. */
    int                framebuffer;
    /* P8: the host texture holds the texels expanded through a palette, and
     * this is the checksum of that palette (0: not known, bake again). */
    int                p8;
    uint32_t           pal_sum;
    /* The title's replacer knows these texels (hle_texture_replace.h), and
     * whether the host texture is its image; rep_gen is the replacements'
     * generation it was decided at. */
    int                candidate, replaced;
    long               rep_gen;
} texture_entry;

static texture_entry g_textures[TEXTURE_CACHE];
static int           g_texture_count;
static IDirect3DTexture8 *g_bound[MAX_STAGES];
static texture_entry *g_bound_entry[MAX_STAGES];

/* Each stage's palette: where the guest keeps it (physical, 0 for none) and
 * the checksum of what the host stage palette holds (0: not known). */
static uint32_t g_pal_data[MAX_STAGES];
static uint32_t g_pal_sum[MAX_STAGES];
static unsigned long g_pal_variants, g_pal_switches, g_pal_rebakes, g_pal_first;
static unsigned long g_midframe_changes;

/* Stage 0 currently holds the title's own frame; see SetTexture below and
 * hle_d3d8_stage0_is_framebuffer(). */
static int g_stage0_framebuffer;

static IDirect3DTexture8 *white_texture(IDirect3DDevice8 *dev);

static unsigned long g_bound_count, g_uploads, g_reuploads, g_skip_type,
                     g_skip_cube, g_skip_format, g_skip_range, g_skip_create;

typedef struct {
    uint32_t fmt, width, height, levels, guest_pitch;
    int      linear;                 /* Size != 0: one level, guest pitch */
    uint32_t phys, bytes;            /* where the texels are, and how many */
} texture_layout;

static uint32_t level_dim(uint32_t d, uint32_t level)
{
    d >>= level;
    return d ? d : 1;
}

static uint32_t level_rows(uint32_t fmt, uint32_t h)
{
    return d3d8_format_is_compressed((D3DFORMAT)fmt) ? (h + 3) / 4 : h;
}

/* Reads the guest pixel container. 0 with a skip counter bumped if the
 * texture is one this file does not forward. */
static int read_layout(uint32_t va, texture_layout *t)
{
    uint32_t common = HLE_MEM32(va + 0);
    uint32_t data   = HLE_MEM32(va + 4);
    uint32_t format = HLE_MEM32(va + 12);
    uint32_t size   = HLE_MEM32(va + 16);
    uint32_t l;
    uint64_t bytes = 0;

    /* A D3DSurface is accepted as well as a D3DTexture. On the Xbox both are
     * D3DPixelContainers with the same Format, Size and Data fields, and the
     * hardware reads a texture from those three, so a title can hand
     * SetTexture a surface and it works. TimeSplitters: Future Perfect does:
     * its colour-grading quad binds the back buffer's surface object
     * (0x003E5984, common type 5) and samples the scene through it. Refused
     * here as "not a texture", that bind got the 1x1 white placeholder, and
     * the whole graded frame came out white -- once per frame, for 3,296 of
     * the run's skipped binds. */
    if ((common & COMMON_TYPE_MASK) != COMMON_TYPE_TEXTURE &&
        (common & COMMON_TYPE_MASK) != COMMON_TYPE_SURFACE) {
        g_skip_type++;
        return 0;
    }
    if ((format & FORMAT_CUBEMAP) || ((format >> 4) & 0xF) != 2) {
        /* Say what was refused, once per kind: a skipped bind samples the
         * 1x1 white placeholder, and in a combiner that adds or multiplies
         * that stage the draw comes out white with nothing in the log. */
        static uint32_t seen[16];
        static int nseen;
        int k;
        for (k = 0; k < nseen && seen[k] != format; k++)
            ;
        if (k == nseen && nseen < 16) {
            seen[nseen++] = format;
            fprintf(stderr, "[HLE-D3D8] texture 0x%08X not mirrored: format word "
                    "0x%08X (%s, %u dimension(s), format 0x%02X, %u level(s), "
                    "log2 size %u x %u x %u), Size field 0x%08X; it samples white\n",
                    va, format, (format & FORMAT_CUBEMAP) ? "cube" : "not cube",
                    (format >> 4) & 0xF, (format >> 8) & 0xFF, (format >> 16) & 0xF,
                    (format >> 20) & 0xF, (format >> 24) & 0xF, (format >> 28) & 0xF,
                    size);
        }
        g_skip_cube++;
        return 0;
    }
    memset(t, 0, sizeof *t);
    t->fmt = (format >> 8) & 0xFF;
    /* Palettised textures; RECOMP_HLE_D3D8_P8=0 refuses them as before.
     *
     * The host side expands P8 to BGRA through d3d8_convert_linear_pixels,
     * the device keeps four palettes, and D3DDevice_SetPalette at the end of
     * this file forwards the guest's. Marvel vs Capcom 2 is a sprite fighter
     * that binds almost nothing else.
     *
     * This was off by default because turning it on made MvC2's start-up
     * worse. That start-up was broken by lifter bugs fixed on 24 Sep 2026
     * (sar branches, a memcpy jump-table arm, CF after repe cmpsb). With
     * those fixed, MvC2 runs clean with P8 on and draws its character select
     * fully textured, where with it off the screen stayed white. */
    if (t->fmt == XFMT_P8 && !xbox_EnvSwitch("RECOMP_HLE_D3D8_P8", 1)) {
        g_skip_format++;
        return 0;
    }
    if (d3d8_format_bpp((D3DFORMAT)t->fmt) == 0) {
        /* Say which format, once each. The count alone says a title's textures
         * are being refused without saying what to implement. */
        static uint8_t said[256];
        if (!said[t->fmt]) {
            said[t->fmt] = 1;
            fprintf(stderr, "[HLE-D3D8] texture format 0x%02X refused (no bpp "
                    "known for it); draws using it are untextured\n", t->fmt);
            fflush(stderr);
        }
        g_skip_format++;
        return 0;
    }
    if (size) {
        t->linear = 1;
        t->width  = (size & 0xFFF) + 1;
        t->height = ((size >> 12) & 0xFFF) + 1;
        t->guest_pitch = (((size >> 24) & 0xFF) + 1) * 64;
        t->levels = 1;
        if (t->guest_pitch < d3d8_row_pitch((D3DFORMAT)t->fmt, t->width)) {
            g_skip_format++;
            return 0;
        }
        bytes = (uint64_t)t->guest_pitch * level_rows(t->fmt, t->height);
    } else {
        t->width  = 1u << ((format >> 20) & 0xF);
        t->height = 1u << ((format >> 24) & 0xF);
        t->levels = (format >> 16) & 0xF;
        if (!t->levels)
            t->levels = 1;
        for (l = 0; l < t->levels; l++)
            bytes += (uint64_t)d3d8_row_pitch((D3DFORMAT)t->fmt, level_dim(t->width, l)) *
                     level_rows(t->fmt, level_dim(t->height, l));
    }
    /* Data is physical; tolerate it arriving with the window's high bits. */
    t->phys = data & 0x0FFFFFFF;
    if (!data || (uint64_t)t->phys + bytes > CONTIG_SIZE) {
        g_skip_range++;
        return 0;
    }
    t->bytes = (uint32_t)bytes;
    return 1;
}

/* A checksum of the texture's contents, every byte of every level.
 *
 * This used to be FNV-1a over 4096 evenly spaced bytes of level 0, on the
 * theory that a sample is enough to notice a change. It is not, for a title
 * that updates a texture a piece at a time. Marvel vs Capcom 2 streams its
 * fighters' animation frames into their sprite sheets tile by tile, the way
 * the Dreamcast original wrote VRAM, and most tiles fall between the sampled
 * bytes: 74 re-uploads in 120 s of fighting, and the sprites (and the load
 * screen's portraits) showed blocks of stale frames that stayed there, even
 * with the game paused.
 *
 * Word at a time, so hashing the few megabytes a frame binds costs about a
 * millisecond. RECOMP_HLE_D3D8_TEX_SAMPLED=1 restores the old sample, for
 * comparison. */
static uint32_t level0_checksum(const texture_layout *t)
{
    static int sampled = -1;
    const uint8_t *p = (const uint8_t *)HLE_PTR(CONTIG_BASE + t->phys);

    if (sampled < 0)
        sampled = xbox_EnvSwitch("RECOMP_HLE_D3D8_TEX_SAMPLED", 0);
    if (sampled) {
        uint32_t n = t->linear
            ? t->guest_pitch * level_rows(t->fmt, t->height)
            : d3d8_row_pitch((D3DFORMAT)t->fmt, t->width) * level_rows(t->fmt, t->height);
        uint32_t h = 2166136261u, i, step = n > 4096 ? n / 4096 : 1;

        for (i = 0; i < n; i += step) {
            h ^= p[i];
            h *= 16777619u;
        }
        return h;
    }
    {
        uint64_t h = 0x9E3779B97F4A7C15ull, w;
        uint32_t n = t->bytes, i;

        for (i = 0; i + 8 <= n; i += 8) {
            memcpy(&w, p + i, 8);
            h = (h ^ w) * 0x100000001B3ull;
            h ^= h >> 29;
        }
        for (; i < n; i++)
            h = (h ^ p[i]) * 0x100000001B3ull;
        return (uint32_t)(h ^ (h >> 32));
    }
}

/* A cheap look at level 0: 1,024 eight-byte words spread across it.
 *
 * It was 4,096 single bytes through one FNV chain, each multiply waiting on
 * the last; at ~380 binds a frame in an OutRun 2 race that was a millisecond
 * or two of the main thread. Words cover twice the bytes with a quarter of
 * the steps, and four independent lanes let the multiplies overlap. */
static uint32_t texel_sample(const texture_layout *t)
{
    const uint8_t *p = (const uint8_t *)HLE_PTR(CONTIG_BASE + t->phys);
    uint32_t n = t->linear
        ? t->guest_pitch * level_rows(t->fmt, t->height)
        : d3d8_row_pitch((D3DFORMAT)t->fmt, t->width) * level_rows(t->fmt, t->height);
    uint64_t h[4] = { 0xCBF29CE484222325ull, 0x84222325CBF29CE4ull,
                      0x9E3779B97F4A7C15ull, 0x7F4A7C159E3779B9ull };
    uint64_t w;
    uint32_t i, k, words = n / 8, step;

    if (!words) {
        uint32_t s = 2166136261u;
        for (i = 0; i < n; i++)
            s = (s ^ p[i]) * 16777619u;
        return s;
    }
    step = words > 1024 ? words / 1024 : 1;
    for (i = 0, k = 0; i < words; i += step, k = (k + 1) & 3) {
        memcpy(&w, p + (size_t)i * 8, 8);
        h[k] = (h[k] ^ w) * 0x100000001B3ull;
    }
    w = h[0] ^ (h[1] * 3) ^ (h[2] * 5) ^ (h[3] * 7);
    return (uint32_t)(w ^ (w >> 32));
}

/* Have this texture's texels changed since the host copy was made?
 *
 * Every first bind of a frame used to hash the whole texture, every mip of
 * it: Outrun 2 binds about 380 a frame in a race, many of them 512x512, and
 * the hashing was a quarter of the main thread -- 5 ms of a 20 ms frame
 * that had to fit in 16.7 to make 60.
 *
 * So the check is tiered. A sample of level 0 is taken at every check, and
 * the full hash only when the sample moved or the last full one is
 * RECOMP_HLE_D3D8_TEX_FULL_EVERY frames old (8; 1 = every check, as before).
 * A texture rewritten wholesale -- a streamed sprite frame, a decoded movie,
 * a reused slot -- changes the sample and uploads the same frame. A small
 * edit the sample misses uploads within that many frames instead of the
 * next. Updates e->checksum, e->quick and e->full_swap. */
static int texels_changed(texture_entry *e, const texture_layout *t,
                          unsigned long now)
{
    static int full_every = -1;
    uint32_t quick = texel_sample(t), sum;

    if (full_every < 0) {
        const char *v = getenv("RECOMP_HLE_D3D8_TEX_FULL_EVERY");
        full_every = v && atoi(v) > 0 ? atoi(v) : 8;
    }
    if (quick == e->quick && now - e->full_swap < (unsigned long)full_every)
        return 0;
    e->quick = quick;
    e->full_swap = now;
    sum = level0_checksum(t);
    if (sum == e->checksum)
        return 0;
    e->checksum = sum;
    return 1;
}

static void upload(IDirect3DTexture8 *host, const texture_layout *t)
{
    const uint8_t *src = (const uint8_t *)HLE_PTR(CONTIG_BASE + t->phys);
    uint32_t l, y;

    for (l = 0; l < t->levels; l++) {
        uint32_t w = level_dim(t->width, l), h = level_dim(t->height, l);
        uint32_t pitch = d3d8_row_pitch((D3DFORMAT)t->fmt, w);
        uint32_t rows = level_rows(t->fmt, h);
        D3DLOCKED_RECT lr;

        if (FAILED(host_LockRect(host, l, &lr, NULL, 0)))
            return;
        if (t->linear) {             /* guest rows are padded to 64 bytes */
            for (y = 0; y < rows; y++)
                memcpy((uint8_t *)lr.pBits + (size_t)y * (size_t)lr.Pitch,
                       src + (size_t)y * t->guest_pitch, pitch);
        } else {
            memcpy(lr.pBits, src, (size_t)pitch * rows);
            src += (size_t)pitch * rows;
        }
        host_UnlockRect(host, l);
    }
}

/* ---- a title's own images (hle_texture_replace.h) ---------------------- */

#define XFMT_A8R8G8B8 0x06              /* swizzled: addressed from 0 to 1 */

static xbox_TextureReplacer g_replacer;
static volatile LONG       g_replace_gen;

void xbox_D3D8SetTextureReplacer(xbox_TextureReplacer fn)
{
    g_replacer = fn;
    InterlockedIncrement(&g_replace_gen);
}

void xbox_D3D8TextureReplacementsChanged(void)
{
    InterlockedIncrement(&g_replace_gen);
}

static int power_of_two(uint32_t v)
{
    return v && !(v & (v - 1));
}

/* e->host from t: the replacer's image when it has one -- A8R8G8B8 at the
 * image's own size, swizzled as the host's upload expects of that format --
 * or the guest's own texels. 0 when no host texture could be made. */
static int make_host(IDirect3DDevice8 *dev, texture_entry *e, const texture_layout *t)
{
    XboxTextureImage img;
    int r = g_replacer ? g_replacer(e->checksum, t->fmt, t->width, t->height, &img) : -1;

    e->candidate = r >= 0;
    e->replaced = 0;
    e->rep_gen = g_replace_gen;
    if (r == 1 && img.argb && power_of_two(img.width) && power_of_two(img.height) &&
        SUCCEEDED(host_CreateTexture(dev, img.width, img.height, 1, 0,
                                     (D3DFORMAT)XFMT_A8R8G8B8, D3DPOOL_MANAGED, &e->host)) &&
        e->host) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(host_LockRect(e->host, 0, &lr, NULL, 0))) {
            xbox_swizzle_rect(lr.pBits, img.argb, img.width, img.height, 4);
            host_UnlockRect(e->host, 0);
        }
        e->replaced = 1;
        return 1;
    }
    if (FAILED(host_CreateTexture(dev, t->width, t->height, t->levels, 0,
                                  (D3DFORMAT)t->fmt, D3DPOOL_MANAGED, &e->host)) || !e->host)
        return 0;
    upload(e->host, t);
    return 1;
}

static void note_format(const texture_layout *t)
{
    static uint32_t seen[32];
    static int nseen;
    uint32_t key = t->fmt | (t->levels << 8) | ((uint32_t)t->linear << 12);
    int i;

    for (i = 0; i < nseen; i++)
        if (seen[i] == key)
            return;
    if (nseen < (int)(sizeof seen / sizeof seen[0])) {
        seen[nseen++] = key;
        fprintf(stderr, "[HLE-D3D8] shadow texture: format 0x%02X %ux%u, %u level(s), %s\n",
                t->fmt, t->width, t->height, t->levels,
                t->linear ? "linear" : "swizzled/compressed");
    }
}

/* Frees the least recently bound entry that no stage holds. */
static texture_entry *cache_slot(IDirect3DDevice8 *dev, unsigned long now)
{
    texture_entry *victim = NULL;
    int i, s;

    if (g_texture_count < TEXTURE_CACHE)
        return &g_textures[g_texture_count++];
    for (i = 0; i < TEXTURE_CACHE; i++) {
        texture_entry *e = &g_textures[i];
        int held = 0;
        for (s = 0; s < MAX_STAGES; s++)
            held |= g_bound[s] == e->host;
        if (!held && e->used_swap != now && (!victim || e->used_swap < victim->used_swap))
            victim = e;
    }
    if (!victim)
        return NULL;
    (void)dev;
    if (victim->host)
        host_ReleaseTexture(victim->host);
    memset(victim, 0, sizeof *victim);
    return victim;
}

/* A texture whose texels are the frame buffer: created as a render target so
 * the host can draw the finished frame into it, refreshed once per frame, and
 * never uploaded from guest memory. */
/* RECOMP_HLE_D3D8_FB_PROBE=<n>: every n swaps, read back what the screen copy
 * actually landed in the texture the title samples, and say so.
 *
 * This exists because the question "does the frame buffer texture hold the
 * frame?" could not be answered from a capture. src/replay never performs the
 * copy -- it is done here, in the HLE -- so a replay samples the captured
 * guest texels, which are zeros, and cannot tell a broken fill from an absent
 * one. The answer has to be read out of the running title.
 *
 * TimeSplitters 2's three full-screen quads compute out = t0*a + dst*(1-a)
 * with t0 this texture, so a correct copy makes them a no-op and a black one
 * makes them multiply the picture by (1-a). That is the difference between a
 * motion blur and the brightness bug, and it is one number.
 *
 * It must read through the surface, not the texture. IDirect3DTexture8's
 * LockRect returns tex->sys_mem, the upload shadow that UnlockRect pushes to
 * the GPU; the screen copy writes the GPU texture through a render target
 * view and never touches it, so that buffer reads as zeros whether the copy
 * works or not. The surface's LockRect is the one that copies the D3D11
 * resource into a staging texture and maps it. The first version of this
 * probe used the texture and "proved" the copy was broken. */
static void framebuffer_probe(texture_entry *e, unsigned long now)
{
    static int every = -1;
    static unsigned long last;
    IDirect3DSurface8 *surf = NULL;
    D3DLOCKED_RECT lr;
    unsigned long long sum = 0;
    unsigned samples = 0, nonzero = 0;
    UINT x, y;

    if (every < 0) {
        const char *v = getenv("RECOMP_HLE_D3D8_FB_PROBE");
        every = (v && atoi(v) > 0) ? atoi(v) : 0;
    }
    if (!every || (last && now - last < (unsigned long)every))
        return;
    last = now;

    if (FAILED(e->host->lpVtbl->GetSurfaceLevel(e->host, 0, &surf)) || !surf) {
        fprintf(stderr, "[HLE-D3D8] fb probe: no level 0 surface on texture "
                "0x%08X; the fill cannot be checked this way\n", e->va);
        fflush(stderr);
        every = 0;
        return;
    }
    if (FAILED(surf->lpVtbl->LockRect(surf, &lr, NULL, D3DLOCK_READONLY))) {
        fprintf(stderr, "[HLE-D3D8] fb probe: cannot read the copy back "
                "(texture 0x%08X); the fill cannot be checked this way\n", e->va);
        fflush(stderr);
        surf->lpVtbl->Release(surf);
        every = 0;               /* asking again every frame would say the same */
        return;
    }
    /* A sparse grid: enough to tell black from a picture, cheap enough to
     * leave on. All three colour channels, because this number is meant to be
     * compared against RECOMP_HLE_D3D8_BRIGHT's reading of the finished frame,
     * which averages three -- sampling one channel here made the scene look
     * brighter than the frame by the size of the blue cast alone. */
    for (y = 0; y < 480u; y += 16) {
        const uint8_t *row = (const uint8_t *)lr.pBits + (size_t)y * lr.Pitch;
        for (x = 0; x < 640u; x += 16) {
            const uint8_t *p = row + (size_t)x * 4u;
            sum += (unsigned)p[0] + p[1] + p[2];
            samples += 3;
            if (p[0] || p[1] || p[2])
                nonzero++;
        }
    }
    surf->lpVtbl->UnlockRect(surf);
    surf->lpVtbl->Release(surf);

    /* "Not arriving" means near-enough nothing, not merely dark: a night level
     * legitimately leaves plenty of black pixels, so the threshold is one in
     * twenty rather than a majority. */
    fprintf(stderr, "[HLE-D3D8] fb probe swap %lu: texture 0x%08X holds mean %.1f/255, "
            "%u of %u pixels non-zero -- %s\n", now, e->va,
            samples ? (double)sum / samples : 0.0, nonzero, samples / 3u,
            nonzero * 20u < samples / 3u ? "the copy is not arriving"
                                         : "the copy has content");
    fflush(stderr);
}

static IDirect3DTexture8 *framebuffer_texture(IDirect3DDevice8 *dev, uint32_t va,
                                              const texture_layout *t)
{
    unsigned long now = hle_d3d8_shadow_swaps();
    uint32_t data = HLE_MEM32(va + 4), format = HLE_MEM32(va + 12), size = HLE_MEM32(va + 16);
    texture_entry *e = NULL;
    int i;

    for (i = 0; i < g_texture_count; i++) {
        texture_entry *c = &g_textures[i];
        if (c->host && c->va == va && c->framebuffer) {
            e = c;
            break;
        }
    }
    if (!e) {
        e = cache_slot(dev, now);
        if (!e)
            return NULL;
        if (FAILED(host_CreateTexture(dev, t->width, t->height, 1, D3DUSAGE_RENDERTARGET,
                                      (D3DFORMAT)t->fmt, D3DPOOL_DEFAULT, &e->host)) ||
            !e->host) {
            memset(e, 0, sizeof *e);
            g_skip_create++;
            return NULL;
        }
        e->va = va;
        e->rendered = 1;
        e->framebuffer = 1;
        fprintf(stderr, "[HLE-D3D8] the title binds its own frame as a texture "
                "0x%08X (%ux%u, format 0x%02X, data 0x%08X); filling it from the "
                "host's frame\n", va, t->width, t->height, t->fmt, data);
        fflush(stderr);
    }
    e->data = data;
    e->format = format;
    e->size = size;
    e->used_swap = now;
    /* Once per frame: the picture it should hold is this frame's, so far. */
    if (e->checked_swap != now) {
        e->checked_swap = now;
        host_CopyBackBufferToTexture(e->host);
        framebuffer_probe(e, now);
    }
    return e->host;
}

static IDirect3DTexture8 *host_texture(IDirect3DDevice8 *dev, uint32_t va)
{
    unsigned long now = hle_d3d8_shadow_swaps();
    texture_layout t;
    texture_entry *e = NULL;
    uint32_t data = HLE_MEM32(va + 4), format = HLE_MEM32(va + 12), size = HLE_MEM32(va + 16);
    int i;

    /* Before the ordinary lookup, because these are not identified by
     * their texels -- they have none of their own -- and because they
     * must be refreshed every frame, which a cache hit would skip. */
    if (hle_d3d8_is_framebuffer(data & 0x0FFFFFFFu) && read_layout(va, &t)) {
        IDirect3DTexture8 *fb = framebuffer_texture(dev, va, &t);
        if (fb)
            return fb;
    }

    for (i = 0; i < g_texture_count; i++) {
        texture_entry *c = &g_textures[i];
        if (c->host && c->va == va && c->data == data && c->format == format && c->size == size) {
            e = c;
            break;
        }
    }
    if (e) {
        e->used_swap = now;
        /* RECOMP_HLE_D3D8_TEX_EVERY_BIND=1: check the texels at every bind,
         * not only the first bind of a frame. A title that rewrites one
         * texture between two draws of the same frame -- a sprite fighter
         * streaming each character's animation frame through it -- otherwise
         * draws the second with the first one's texels. Counted either way
         * it is on, as "changed mid-frame". */
        static int every_bind = -1;
        if (every_bind < 0)
            every_bind = xbox_EnvSwitch("RECOMP_HLE_D3D8_TEX_EVERY_BIND", 0);
        if (!e->rendered && (e->checked_swap != now || every_bind ||
                             (e->candidate && e->rep_gen != g_replace_gen))) {
            /* RECOMP_HLE_D3D8_TEX_REFRESH=1: upload every bound texture once
             * a frame regardless of the checksum, to tell a stale cache from
             * a wrong draw. */
            static int refresh = -1;
            int same_frame = e->checked_swap == now;
            if (refresh < 0)
                refresh = getenv("RECOMP_HLE_D3D8_TEX_REFRESH") ? 1 : 0;
            e->checked_swap = now;
            if (read_layout(va, &t)) {
                int changed = texels_changed(e, &t, now);
                XboxTextureImage probe;
                /* The replacer's say: a texture it knows whose answer may have
                 * changed, or texels that are now (or were) one of its own.
                 * The host texture is made again, since a replacement has its
                 * own size and format; anything else uploads in place. */
                if (g_replacer &&
                    ((e->candidate && e->rep_gen != g_replace_gen) ||
                     (changed && (e->replaced ||
                                  g_replacer(e->checksum, t.fmt, t.width, t.height,
                                             &probe) >= 0)))) {
                    host_ReleaseTexture(e->host);
                    e->host = NULL;
                    if (!make_host(dev, e, &t)) {
                        memset(e, 0, sizeof *e);
                        return NULL;
                    }
                    e->pal_sum = 0;
                    g_reuploads++;
                    return e->host;
                }
                if (changed || (refresh && !same_frame)) {
                    if (same_frame && changed)
                        g_midframe_changes++;
                    upload(e->host, &t);
                    e->pal_sum = 0;     /* baked through whichever palette */
                    g_reuploads++;
                }
            }
        }
        return e->host;
    }

    if (!read_layout(va, &t))
        return NULL;
    note_format(&t);
    /* The title's own frame, bound as a texture. Nothing drew those texels on
     * the guest side, so they are zeros; the host's frame goes in instead, and
     * the entry is marked so the zeros never overwrite it. */
    if (hle_d3d8_is_framebuffer(t.phys)) {
        IDirect3DTexture8 *fb = framebuffer_texture(dev, va, &t);
        if (fb)
            return fb;
    }
    e = cache_slot(dev, now);
    if (!e)
        return NULL;
    e->va = va;
    e->data = data;
    e->format = format;
    e->size = size;
    e->checksum = level0_checksum(&t);
    /* RECOMP_HLE_D3D8_TEX_DUMP=<folder>: each small texture's guest bytes
     * (64x64 and under), once, as <checksum>_<format>_<w>x<h>_<levels>.bin --
     * how a title project finds the checksums of textures it replaces
     * (xbox_D3D8SetTextureReplacer): its button icons, say. */
    {
        static const char *dir;
        static int said;
        if (!said) {
            said = 1;
            dir = getenv("RECOMP_HLE_D3D8_TEX_DUMP");
        }
        if (dir && *dir && t.width <= 64 && t.height <= 64) {
            char path[1024];
            FILE *f;
            snprintf(path, sizeof path, "%s/%08X_%02X_%ux%u_%u.bin", dir, e->checksum, t.fmt,
                     t.width, t.height, t.levels);
            if ((f = fopen(path, "rb")) != NULL) {
                fclose(f);
            } else if ((f = fopen(path, "wb")) != NULL) {
                fwrite(HLE_PTR(CONTIG_BASE + t.phys), 1, t.bytes, f);
                fclose(f);
            }
        }
    }
    e->quick = texel_sample(&t);
    /* Spread the full checks: textures made together (a level's load) would
     * otherwise all take their full hash in the same frame, every 8th frame,
     * which is a periodic spike rather than a cost spread across frames.
     * texels_changed keeps each texture's phase from here on. */
    e->full_swap = now - ((va >> 5) ^ (data >> 12)) % 8u;
    e->checked_swap = e->used_swap = now;
    e->p8 = t.fmt == XFMT_P8;
    e->pal_sum = 0;
    if (!make_host(dev, e, &t)) {
        memset(e, 0, sizeof *e);
        g_skip_create++;
        return NULL;
    }
    g_uploads++;
    return e->host;
}

/* Checksum of a guest palette: 256 entries of ARGB8888. Never 0, which means
 * "not known". */
#define GREY_RAMP_SUM 0x9E3779B9u

static uint32_t palette_sum(uint32_t phys)
{
    const uint8_t *p;
    uint32_t h = 2166136261u, i;

    if (!phys)
        return GREY_RAMP_SUM;
    p = (const uint8_t *)HLE_PTR(CONTIG_BASE + phys);
    for (i = 0; i < 1024u; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h ? h : 1u;
}

/* Give the host's stage palette the guest's current one.
 *
 * dev_SetPalette re-expands whatever texture the host has bound to that
 * stage, and here that would be a texture baked for some other palette --
 * overwriting it. So the stage is emptied first. */
static void host_stage_palette(IDirect3DDevice8 *dev, uint32_t stage, uint32_t sum)
{
    if (g_pal_sum[stage] == sum)
        return;
    host_SetTexture(dev, stage, NULL);
    host_SetPalette(dev, stage, g_pal_data[stage]
        ? (const DWORD *)HLE_PTR(CONTIG_BASE + g_pal_data[stage]) : NULL);
    g_pal_sum[stage] = sum;
}

/* Expand a P8 entry's texels through the stage's current palette. */
static void bake(IDirect3DDevice8 *dev, uint32_t stage, texture_entry *e,
                 const texture_layout *t, uint32_t sum)
{
    host_stage_palette(dev, stage, sum);
    d3d8_base_set_palette((IDirect3DBaseTexture8 *)e->host, stage);
    upload(e->host, t);
    e->pal_sum = sum;
}

/* Make the P8 texture bound to a stage show the palette the guest has for
 * that stage now. Called before every draw.
 *
 * The host expands P8 to BGRA when it uploads, and binding a texture does not
 * expand it again. The Xbox has no such step: the hardware reads the palette
 * from memory at every draw. Marvel vs Capcom 2 colours its characters by
 * drawing one sheet under several palettes, so a single host texture per sheet
 * either kept the first palette it was baked with or, re-expanded at every
 * change, spent half the main thread's time in d3d8_convert_linear_pixels.
 *
 * So a P8 sheet gets one host texture per palette it is drawn with -- each a
 * cache entry of its own, found by the palette's checksum and evicted like any
 * other -- and a draw switches the stage to the one that matches. A palette
 * edited in place changes the checksum, and so the texture, on the next draw.
 * The cost per P8 stage per draw is a 1 KB checksum. */
static void sync_palette(IDirect3DDevice8 *dev, uint32_t stage)
{
    texture_entry *e = g_bound_entry[stage], *v = NULL;
    unsigned long now;
    texture_layout t;
    uint32_t sum;
    int i;

    if (!e || !e->p8 || !e->host)
        return;
    sum = palette_sum(g_pal_data[stage]);
    if (e->pal_sum == sum)
        return;
    if (!read_layout(e->va, &t))
        return;
    now = hle_d3d8_shadow_swaps();

    /* Just uploaded, through no palette in particular: this one is the
     * texture for the current palette, not a stale variant of it. */
    if (!e->pal_sum) {
        g_pal_first++;
        bake(dev, stage, e, &t, sum);
        host_SetTexture(dev, stage, (IDirect3DBaseTexture8 *)e->host);
        return;
    }

    for (i = 0; i < g_texture_count; i++) {
        texture_entry *c = &g_textures[i];
        if (c->host && c->p8 && c->pal_sum == sum && c->va == e->va &&
            c->data == e->data && c->format == e->format && c->size == e->size) {
            v = c;
            break;
        }
    }
    if (v) {
        g_pal_switches++;
        /* The texels are shared by every variant, and only the one the bind
         * found was checked this frame. */
        if (v->checksum != e->checksum) {
            v->checksum = e->checksum;
            bake(dev, stage, v, &t, sum);
            g_pal_rebakes++;
        }
    } else {
        v = cache_slot(dev, now);
        if (!v)
            return;
        if (FAILED(host_CreateTexture(dev, t.width, t.height, t.levels, 0,
                                      (D3DFORMAT)t.fmt, D3DPOOL_MANAGED, &v->host)) ||
            !v->host) {
            memset(v, 0, sizeof *v);
            g_skip_create++;
            return;
        }
        v->va = e->va;
        v->data = e->data;
        v->format = e->format;
        v->size = e->size;
        v->checksum = e->checksum;
        v->checked_swap = now;
        v->p8 = 1;
        bake(dev, stage, v, &t, sum);
        g_pal_variants++;
    }
    v->used_swap = now;
    host_stage_palette(dev, stage, sum);
    host_SetTexture(dev, stage, (IDirect3DBaseTexture8 *)v->host);
    g_bound[stage] = v->host;
    g_bound_entry[stage] = v;
}

/* At the end of a frame: which textures this frame drew with have different
 * texels now than when it drew with them?
 *
 * The shadow renderer draws at the call; the NV2A draws when it reaches the
 * command in the push buffer, which can be after the CPU has written a
 * texture it was told to draw with. A title that queues a sprite's draws and
 * only then writes that frame's tiles is correct on the console and one frame
 * behind here. This counts it, so the question has a number. */
static unsigned long g_changed_after_draw, g_frames_changed_after_draw;

void hle_d3d8_texture_frame_end(void)
{
    unsigned long now = hle_d3d8_shadow_swaps();
    texture_layout t;
    int i, any = 0;
    static int on = -1;

    /* RECOMP_HLE_D3D8_TEX_AFTER_DRAW=1. It hashes every texture the frame
     * used a second time, for a count, and was a fifth of Outrun 2's main
     * thread in a race; so it is asked for, not paid for by default. */
    if (on < 0)
        on = xbox_EnvSwitch("RECOMP_HLE_D3D8_TEX_AFTER_DRAW", 0);
    if (!on)
        return;
    for (i = 0; i < g_texture_count; i++) {
        texture_entry *e = &g_textures[i];
        if (!e->host || e->rendered || e->framebuffer || e->used_swap != now)
            continue;
        if (!read_layout(e->va, &t) || level0_checksum(&t) == e->checksum)
            continue;
        g_changed_after_draw++;
        any = 1;
    }
    if (any)
        g_frames_changed_after_draw++;
}

static void op_sync_palettes(const void *arg)
{
    uint32_t s;
    IDirect3DDevice8 *dev = hle_d3d8_shadow_device();

    (void)arg;
    if (!dev)
        return;
    for (s = 0; s < MAX_STAGES; s++)
        sync_palette(dev, s);
}

/* Before a draw. Deferred, it runs where the draw runs, so the palette is
 * read when the draw is executed. */
void hle_d3d8_sync_palettes(IDirect3DDevice8 *dev)
{
    if (!dev)
        return;
    if (hle_d3d8_defer_recording())
        hle_d3d8_defer_op(op_sync_palettes, NULL, 0);
    else
        op_sync_palettes(NULL);
}

/* The palette a stage uses, in order with the draws around it. */
static void op_set_pal_data(const void *arg)
{
    const uint32_t *a = (const uint32_t *)arg;
    g_pal_data[a[0]] = a[1];
}

static void set_pal_data(uint32_t stage, uint32_t data)
{
    uint32_t a[2];

    a[0] = stage;
    a[1] = data;
    if (hle_d3d8_defer_recording())
        hle_d3d8_defer_op(op_set_pal_data, a, sizeof a);
    else
        op_set_pal_data(a);
}

/* Cube textures a title renders into: its environment map. Only rendered
 * cubes are mirrored -- a cube the title uploads from guest memory still
 * binds white -- so the entries carry no contents and are never re-uploaded.
 * Kept apart from the 2D cache above, which is keyed and evicted by texel
 * checksums that mean nothing here. */
#define CUBE_CACHE      8
#define CUBE_FACE_ALIGN 128u        /* nv2a_regs.h NV2A_CUBEMAP_FACE_ALIGNMENT */

static struct {
    uint32_t va, data, format;
    IDirect3DCubeTexture8 *host;    /* NULL with tried set: the host refused it */
    int      tried;
} g_cubes[CUBE_CACHE];
static int g_cube_count;
static unsigned long g_cube_full, g_cube_failed;
static unsigned long g_cube_binds;

/* Which face and level of a cube container a surface is. The Xbox lays the
 * six faces out one after another in the container's data, each holding that
 * face's whole mip chain, and each face starts on a 128-byte boundary
 * (src/nv2a/nv2a_regs.h, NV2A_CUBEMAP_FACE_ALIGNMENT). So the distance
 * between the surface's data pointer and the container's says which face and
 * which level this is.
 *
 * Burnout 2's cube cannot show the padding: one 128x128 R5G6B5 face is 32768
 * bytes, which is already a multiple of 128, so its deltas look the same
 * either way. The alignment is taken from the register header rather than
 * from that measurement. A mipped cube is where the two differ -- 8 levels of
 * 128x128 R5G6B5 is 43690 bytes, padded to 43776.
 *
 * 0 on success with *face and *level filled, -1 for a surface this does not
 * describe. */
int hle_d3d8_cube_face(uint32_t parent_va, uint32_t surface_va,
                       uint32_t *face, uint32_t *level)
{
    uint32_t format = HLE_MEM32(parent_va + 12);
    uint32_t size   = HLE_MEM32(parent_va + 16);
    uint32_t fmt    = (format >> 8) & 0xFF;
    uint32_t levels = (format >> 16) & 0xF;
    uint32_t w      = 1u << ((format >> 20) & 0xF);
    uint32_t h      = 1u << ((format >> 24) & 0xF);
    uint32_t pdata  = HLE_MEM32(parent_va + 4);
    uint32_t sdata  = HLE_MEM32(surface_va + 4);
    uint32_t chain = 0, stride, offset, delta, l;

    /* A linear container keeps its size in Size, not in the format's log2
     * fields; reading those would size every face 1x1. */
    if (size || !levels || sdata < pdata)
        return -1;
    for (l = 0; l < levels; l++)
        chain += d3d8_row_pitch((D3DFORMAT)fmt, level_dim(w, l)) *
                 level_rows(fmt, level_dim(h, l));
    if (!chain)
        return -1;
    stride = (chain + (CUBE_FACE_ALIGN - 1u)) & ~(CUBE_FACE_ALIGN - 1u);
    delta  = sdata - pdata;
    if (delta / stride >= 6u)
        return -1;
    *face  = delta / stride;
    offset = delta % stride;

    /* The remainder names a level: the start of one of the face's mips. */
    for (l = 0, chain = 0; l < levels; l++) {
        if (offset == chain) {
            *level = l;
            return 0;
        }
        chain += d3d8_row_pitch((D3DFORMAT)fmt, level_dim(w, l)) *
                 level_rows(fmt, level_dim(h, l));
    }
    return -1;
}

/* The host cube for a guest cube container, created on first use. NULL if
 * there is no room or the host refuses it. */
IDirect3DCubeTexture8 *hle_d3d8_render_cube(IDirect3DDevice8 *dev, uint32_t va)
{
    uint32_t data = HLE_MEM32(va + 4), format = HLE_MEM32(va + 12);
    uint32_t edge = 1u << ((format >> 20) & 0xF);
    uint32_t levels = (format >> 16) & 0xF;
    uint32_t fmt = (format >> 8) & 0xFF;
    int i;

    for (i = 0; i < g_cube_count; i++)
        if (g_cubes[i].va == va && g_cubes[i].data == data &&
            g_cubes[i].format == format)
            return g_cubes[i].host;      /* NULL if the host refused it before */
    if (!levels || HLE_MEM32(va + 16)) {
        g_skip_cube++;                   /* no levels, or a linear container */
        return NULL;
    }
    if (g_cube_count >= CUBE_CACHE) {
        if (!g_cube_full++)
            fprintf(stderr, "[HLE-D3D8] shadow cubes: the cache holds %d and this "
                    "title wants more; 0x%08X and any after it draw to a scratch "
                    "target and sample white\n", CUBE_CACHE, va);
        return NULL;
    }
    /* The entry is kept whatever happens: a format the host refuses would
     * otherwise be retried on every face of every frame. */
    g_cubes[g_cube_count].va = va;
    g_cubes[g_cube_count].data = data;
    g_cubes[g_cube_count].format = format;
    g_cubes[g_cube_count].tried = 1;
    if (FAILED(host_CreateCubeTexture(dev, edge, levels, D3DUSAGE_RENDERTARGET,
                                      (D3DFORMAT)fmt, D3DPOOL_DEFAULT,
                                      &g_cubes[g_cube_count].host)) ||
        !g_cubes[g_cube_count].host) {
        g_cubes[g_cube_count].host = NULL;
        g_cube_count++;
        g_skip_create++;
        g_cube_failed++;
        return NULL;
    }
    fprintf(stderr, "[HLE-D3D8] shadow render target cube 0x%08X: format 0x%02X "
            "%ux%u, %u level(s)\n", va, fmt, edge, edge, levels);
    return g_cubes[g_cube_count++].host;
}

/* The host cube for a guest container the title is binding, if one was
 * rendered into. NULL means "not a cube this file mirrors". */
static IDirect3DCubeTexture8 *bound_cube(uint32_t va)
{
    uint32_t common = HLE_MEM32(va + 0), data = HLE_MEM32(va + 4);
    uint32_t format = HLE_MEM32(va + 12);
    int i;

    /* The container still has to read as a cube texture: after the guest
     * frees this one, whatever lands at the address could otherwise match on
     * data and format alone and be handed a stale cube. */
    if ((common & COMMON_TYPE_MASK) != COMMON_TYPE_TEXTURE ||
        !(format & FORMAT_CUBEMAP))
        return NULL;
    for (i = 0; i < g_cube_count; i++)
        if (g_cubes[i].host && g_cubes[i].va == va && g_cubes[i].data == data &&
            g_cubes[i].format == format)
            return g_cubes[i].host;
    return NULL;
}

/* Cube textures a title fills from memory rather than rendering into: an
 * environment map loaded with the rest of a level. OutRun 2 binds two static
 * DXT5 ones in a race, and while they were refused (read_layout does not
 * take cubes) the stage read nothing and the road came out pale.
 *
 * The layout is hle_d3d8_cube_face's: six faces one after another, each
 * with its whole mip chain, each starting on a 128-byte boundary. The host
 * cube's LockRect/UnlockRect take one face level at a time in the guest's
 * own format and swizzle, and unswizzle it on unlock as a 2D level would be.
 *
 * Kept apart from the rendered cubes above, which carry no contents and
 * are looked up first: a cube the title renders into is that, whatever it
 * held before. Rechecked by checksum every RECOMP_HLE_D3D8_TEX_FULL_EVERY
 * frames it is bound (8 by default), since static cubes are static. */
#define STATIC_CUBE_CACHE 32

static struct {
    uint32_t va, data, format;
    IDirect3DCubeTexture8 *host;    /* NULL: the host refused it, do not retry */
    uint32_t checksum;
    unsigned long checked_swap;
} g_static_cubes[STATIC_CUBE_CACHE];
static int g_static_cube_count;
static unsigned long g_static_cube_uploads, g_static_cube_reuploads, g_static_cube_full;

/* Bytes in one face's mip chain, unpadded. */
static uint32_t cube_chain_bytes(uint32_t fmt, uint32_t edge, uint32_t levels)
{
    uint32_t l, chain = 0;

    for (l = 0; l < levels; l++)
        chain += d3d8_row_pitch((D3DFORMAT)fmt, level_dim(edge, l)) *
                 level_rows(fmt, level_dim(edge, l));
    return chain;
}

static uint32_t guest_bytes_hash(uint32_t phys, uint32_t n)
{
    const uint8_t *p = (const uint8_t *)HLE_PTR(CONTIG_BASE + phys);
    uint64_t h = 0x9E3779B97F4A7C15ull, w;
    uint32_t i;

    for (i = 0; i + 8 <= n; i += 8) {
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return (uint32_t)(h ^ (h >> 32));
}

static void upload_static_cube(IDirect3DCubeTexture8 *host, uint32_t phys, uint32_t fmt,
                               uint32_t edge, uint32_t levels, uint32_t stride)
{
    uint32_t face, l;

    for (face = 0; face < 6; face++) {
        const uint8_t *src = (const uint8_t *)HLE_PTR(CONTIG_BASE + phys + face * stride);

        for (l = 0; l < levels; l++) {
            uint32_t d = level_dim(edge, l);
            uint32_t pitch = d3d8_row_pitch((D3DFORMAT)fmt, d);
            uint32_t rows = level_rows(fmt, d), y;
            D3DLOCKED_RECT lr;

            if (FAILED(host_CubeLockRect(host, (D3DCUBEMAP_FACES)face, l, &lr)))
                return;
            if ((uint32_t)lr.Pitch == pitch) {
                memcpy(lr.pBits, src, (size_t)pitch * rows);
            } else {
                for (y = 0; y < rows; y++)
                    memcpy((uint8_t *)lr.pBits + (size_t)y * (size_t)lr.Pitch,
                           src + (size_t)y * pitch, pitch);
            }
            host_CubeUnlockRect(host, (D3DCUBEMAP_FACES)face, l);
            src += (size_t)pitch * rows;
        }
    }
}

/* The host copy of a static guest cube, made or refreshed. NULL if the
 * container is not one this can mirror. */
static IDirect3DCubeTexture8 *static_cube(IDirect3DDevice8 *dev, uint32_t va)
{
    static int full_every = -1;
    uint32_t common = HLE_MEM32(va + 0), data = HLE_MEM32(va + 4);
    uint32_t format = HLE_MEM32(va + 12), size = HLE_MEM32(va + 16);
    uint32_t fmt = (format >> 8) & 0xFF, levels = (format >> 16) & 0xF;
    uint32_t edge = 1u << ((format >> 20) & 0xF);
    uint32_t phys = data & 0x0FFFFFFFu, chain, stride, total;
    unsigned long now = hle_d3d8_shadow_swaps();
    int i;

    if (full_every < 0) {
        const char *v = getenv("RECOMP_HLE_D3D8_TEX_FULL_EVERY");
        full_every = v && atoi(v) > 0 ? atoi(v) : 8;
    }
    if ((common & COMMON_TYPE_MASK) != COMMON_TYPE_TEXTURE || !(format & FORMAT_CUBEMAP) ||
        size || !levels || !data || d3d8_format_bpp((D3DFORMAT)fmt) == 0)
        return NULL;
    chain = cube_chain_bytes(fmt, edge, levels);
    stride = (chain + (CUBE_FACE_ALIGN - 1u)) & ~(CUBE_FACE_ALIGN - 1u);
    total = 5u * stride + chain;
    if (!chain || (uint64_t)phys + total > CONTIG_SIZE)
        return NULL;

    for (i = 0; i < g_static_cube_count; i++)
        if (g_static_cubes[i].va == va && g_static_cubes[i].data == data &&
            g_static_cubes[i].format == format)
            break;
    if (i < g_static_cube_count) {
        IDirect3DCubeTexture8 *host = g_static_cubes[i].host;

        if (host && now - g_static_cubes[i].checked_swap >= (unsigned long)full_every) {
            uint32_t sum = guest_bytes_hash(phys, total);

            g_static_cubes[i].checked_swap = now;
            if (sum != g_static_cubes[i].checksum) {
                g_static_cubes[i].checksum = sum;
                upload_static_cube(host, phys, fmt, edge, levels, stride);
                g_static_cube_reuploads++;
            }
        }
        return host;
    }
    if (g_static_cube_count >= STATIC_CUBE_CACHE) {
        if (!g_static_cube_full++)
            fprintf(stderr, "[HLE-D3D8] static cubes: the cache holds %d and this "
                    "title wants more; 0x%08X and any after it sample nothing\n",
                    STATIC_CUBE_CACHE, va);
        return NULL;
    }
    g_static_cubes[i].va = va;
    g_static_cubes[i].data = data;
    g_static_cubes[i].format = format;
    g_static_cubes[i].checked_swap = now;
    g_static_cubes[i].checksum = guest_bytes_hash(phys, total);
    g_static_cube_count++;
    if (FAILED(host_CreateCubeTexture(dev, edge, levels, 0, (D3DFORMAT)fmt,
                                      D3DPOOL_MANAGED, &g_static_cubes[i].host)) ||
        !g_static_cubes[i].host) {
        g_static_cubes[i].host = NULL;
        g_skip_create++;
        return NULL;
    }
    upload_static_cube(g_static_cubes[i].host, phys, fmt, edge, levels, stride);
    g_static_cube_uploads++;
    fprintf(stderr, "[HLE-D3D8] static cube 0x%08X mirrored: format 0x%02X %ux%u, "
            "%u level(s), face stride %u\n", va, fmt, edge, edge, levels, stride);
    return g_static_cubes[i].host;
}

/* The host render target for a 2D guest texture the title renders into, for
 * hle_d3d8.c's SetRenderTarget. The same cache entry SetTexture finds, so a
 * texture drawn into and then bound samples what was drawn. A texture already
 * cached as an ordinary one is recreated as a render target. NULL (counted)
 * if the texture is not one this file can mirror. */
IDirect3DTexture8 *hle_d3d8_render_texture(IDirect3DDevice8 *dev, uint32_t va)
{
    unsigned long now = hle_d3d8_shadow_swaps();
    texture_layout t;
    texture_entry *e = NULL;
    uint32_t data = HLE_MEM32(va + 4), format = HLE_MEM32(va + 12), size = HLE_MEM32(va + 16);
    int i;

    for (i = 0; i < g_texture_count; i++) {
        texture_entry *c = &g_textures[i];
        if (c->host && c->va == va && c->data == data && c->format == format && c->size == size) {
            e = c;
            break;
        }
    }
    if (e && e->rendered) {
        e->used_swap = now;
        return e->host;
    }
    if (!read_layout(va, &t))
        return NULL;
    if (!e) {
        e = cache_slot(dev, now);
        if (!e)
            return NULL;
    } else {
        int s;
        /* The host device keeps a plain pointer to each bound texture, so a
         * stage holding this one is moved off it before it is released. It
         * gets white, as an unmirrored texture does, until the title binds
         * again. */
        for (s = 0; s < MAX_STAGES; s++)
            if (g_bound[s] == e->host) {
                g_bound[s] = NULL;
                host_SetTexture(dev, (DWORD)s, (IDirect3DBaseTexture8 *)white_texture(dev));
            }
        host_ReleaseTexture(e->host);
        memset(e, 0, sizeof *e);
    }
    if (FAILED(host_CreateTexture(dev, t.width, t.height, t.levels, D3DUSAGE_RENDERTARGET,
                                  (D3DFORMAT)t.fmt, D3DPOOL_DEFAULT, &e->host)) ||
        !e->host) {
        memset(e, 0, sizeof *e);
        g_skip_create++;
        return NULL;
    }
    e->va = va;
    e->data = data;
    e->format = format;
    e->size = size;
    e->rendered = 1;
    e->checked_swap = e->used_swap = now;
    fprintf(stderr, "[HLE-D3D8] shadow render target texture 0x%08X: format 0x%02X "
            "%ux%u, %u level(s)\n", va, t.fmt, t.width, t.height, t.levels);
    return e->host;
}

/* 1x1 opaque white, created once, never evicted. */
static IDirect3DTexture8 *white_texture(IDirect3DDevice8 *dev)
{
    static IDirect3DTexture8 *white;
    static int tried;
    D3DLOCKED_RECT lr;

    if (white || tried)
        return white;
    tried = 1;
    if (FAILED(host_CreateTexture(dev, 1, 1, 1, 0, D3DFMT_LIN_A8R8G8B8,
                                  D3DPOOL_MANAGED, &white)) || !white) {
        white = NULL;
        return NULL;
    }
    if (SUCCEEDED(host_LockRect(white, 0, &lr, NULL, 0))) {
        memset(lr.pBits, 0xFF, 4);
        host_UnlockRect(white, 0);
    }
    return white;
}

static void report(void)
{
    static DWORD last;
    DWORD now = GetTickCount();

    if (!last) {
        last = now;
    } else if (now - last >= 5000) {
        fprintf(stderr, "[HLE-D3D8] shadow textures: %lu binds, %d cached, %lu uploads, "
                "%lu re-uploads; skipped %lu not a texture, %lu cube/volume, %lu format, "
                "%lu out of range, %lu create failed\n",
                g_bound_count, g_texture_count, g_uploads, g_reuploads, g_skip_type,
                g_skip_cube, g_skip_format, g_skip_range, g_skip_create);
        fprintf(stderr, "[HLE-D3D8] shadow cubes: %d rendered into, %lu binds, "
                "%lu refused by the host, %lu past the cache\n",
                g_cube_count, g_cube_binds, g_cube_failed, g_cube_full);
        if (g_frames_changed_after_draw)
            fprintf(stderr, "[HLE-D3D8] shadow textures: %lu changed after the frame "
                    "drew with them, in %lu frames\n", g_changed_after_draw,
                    g_frames_changed_after_draw);
        if (g_midframe_changes)
            fprintf(stderr, "[HLE-D3D8] shadow textures: %lu changed mid-frame "
                    "(RECOMP_HLE_D3D8_TEX_EVERY_BIND)\n", g_midframe_changes);
        if (g_pal_variants || g_pal_switches || g_pal_first)
            fprintf(stderr, "[HLE-D3D8] shadow palettes: %lu baked after upload, "
                    "%lu more made for another palette, %lu draws switched to "
                    "one, %lu re-baked for new texels\n",
                    g_pal_first, g_pal_variants, g_pal_switches, g_pal_rebakes);
        last = now;
    }
}
#endif /* _WIN32 */

HLE_ORIGINAL(D3DDevice_SetTexture);

#ifdef _WIN32
static void shadow_set_texture(uint32_t stage, uint32_t texture);
static void op_set_texture(const void *arg)
{
    const uint32_t *a = (const uint32_t *)arg;
    shadow_set_texture(a[0], a[1]);
}
#endif

/* HRESULT D3DDevice_SetTexture(DWORD Stage, IDirect3DBaseTexture8 *pTexture) */
HLE_EXPORT(D3DDevice_SetTexture)
{
    static int seen;
    uint32_t stage = HLE_ARG(0);
#ifdef _WIN32
    uint32_t texture = HLE_ARG(1);
#endif

    if (!seen) {
        seen = 1;
        fprintf(stderr, "[HLE] D3DDevice_SetTexture(0x%X) replaced by name\n", stage);
    }
    if (!hle_original_D3DDevice_SetTexture) {
        fprintf(stderr, "[HLE] D3DDevice_SetTexture: original body missing -- regenerate the lift\n");
        HLE_RETURN(0x80004005u);
    }
    HLE_CALL_ORIGINAL(D3DDevice_SetTexture);
#ifdef _WIN32
    /* Deferred (RECOMP_HLE_D3D8_DEFER), the host side runs when the frame is
     * executed, so the texels are read then -- as the NV2A reads them when it
     * reaches the draw, not when the title issued it. */
    if (hle_d3d8_defer_recording()) {
        uint32_t a[2];
        a[0] = stage;
        a[1] = texture;
        hle_d3d8_defer_op(op_set_texture, a, sizeof a);
    } else {
        shadow_set_texture(stage, texture);
    }
#endif
}

#ifdef _WIN32
static void shadow_set_texture(uint32_t stage, uint32_t texture)
{
    {
        IDirect3DDevice8 *dev = hle_d3d8_shadow_device();
        IDirect3DTexture8 *host = NULL;

        if (!dev || stage >= MAX_STAGES)
            return;
        if (texture) {
            /* A cube the title rendered into is bound as itself, and one it
             * filled from memory as a copy of those bytes (static_cube). */
            static int static_cubes = -1;
            IDirect3DCubeTexture8 *cube = bound_cube(texture);

            if (static_cubes < 0)
                static_cubes = xbox_EnvSwitch("RECOMP_HLE_D3D8_STATIC_CUBES", 1);
            if (!cube && static_cubes)
                cube = static_cube(dev, texture);

            if (cube) {
                g_bound[stage] = NULL;
                g_bound_entry[stage] = NULL;
                g_cube_binds++;
                host_SetTexture(dev, stage, (IDirect3DBaseTexture8 *)cube);
                g_bound_count++;
                report();
                return;
            }
            host = host_texture(dev, texture);
        }
        hle_d3d8_note_stage_texels(stage, texture ? HLE_MEM32(texture + 4) & 0x0FFFFFFFu : 0u);
        g_bound[stage] = host;
        g_bound_entry[stage] = NULL;
        {
            int i;
            for (i = 0; host && i < g_texture_count; i++)
                if (g_textures[i].host == host) {
                    g_bound_entry[stage] = &g_textures[i];
                    break;
                }
        }
        if (stage == 0) {
            /* Whether this draw is one of the title's full-screen passes over
             * its own frame. Nothing else binds the frame buffer as stage 0,
             * so it identifies them without matching a shader or a size. */
            int i;
            g_stage0_framebuffer = 0;
            for (i = 0; host && i < g_texture_count; i++)
                if (g_textures[i].host == host && g_textures[i].framebuffer) {
                    g_stage0_framebuffer = 1;
                    break;
                }
        }
        /* The host pixel shader samples every stage whatever its operation
         * (d3d8_shaders.c), and an unbound D3D11 slot reads as zero, so a
         * stage with no host texture would turn the draw black once the
         * title's own texture operations are forwarded. It gets opaque white
         * instead. That a missing Xbox texture contributes white is assumed,
         * not verified against hardware. */
        host_SetTexture(dev, stage,
                        (IDirect3DBaseTexture8 *)(host ? host : white_texture(dev)));
        g_bound_count++;
        if (hle_d3d8_trace_on()) {
            int fb = 0, i;
            for (i = 0; host && i < g_texture_count; i++)
                if (g_textures[i].host == host && g_textures[i].framebuffer)
                    fb = 1;
            fprintf(stderr, "[TRACE swap %lu] SetTexture stage %u <- 0x%08X common 0x%08X "
                    "data 0x%08X format 0x%08X size 0x%08X -> %s\n",
                    hle_d3d8_shadow_swaps(), stage, texture,
                    texture ? HLE_MEM32(texture) : 0, texture ? HLE_MEM32(texture + 4) : 0,
                    texture ? HLE_MEM32(texture + 12) : 0, texture ? HLE_MEM32(texture + 16) : 0,
                    !texture ? "none" : fb ? "frame buffer copy" : host ? "host texture" : "white");
        }
        report();
    }
}
#endif

/* Does the draw about to be made sample the title's own frame at stage 0?
 *
 * That is what its full-screen passes do and what nothing else does, so it
 * identifies them without having to match a shader hash or a vertex count.
 * hle_d3d8.c uses it for RECOMP_HLE_D3D8_SKIP_FULLSCREEN. */
int hle_d3d8_stage0_is_framebuffer(void)
{
    return g_stage0_framebuffer;
}

HLE_ORIGINAL(D3DDevice_SetPalette);

/* void D3DDevice_SetPalette(DWORD Stage, X_D3DPalette *pPalette)
 *
 * The other half of P8. read_layout() now lets palettised textures through,
 * and the host expands them to BGRA at upload through the palette belonging
 * to the stage they are bound to (d3d8_convert_linear_pixels). That palette
 * has to come from somewhere, and this is it.
 *
 * X_D3DPalette is an X_D3DResource -- Common, Data, Lock -- so the entries are
 * at the physical address in Data, the same way a texture's texels are
 * (Cxbx-Reloaded's CxbxImpl_SetPalette takes GetDataFromXboxResource(pPalette)
 * and nothing else). 256 entries of ARGB8888, which is what the device keeps.
 *
 * Only remembered here. The hardware reads the palette at each draw, so
 * sync_palette reads it there too and picks the host texture baked with it.
 * A null palette means the device's grey ramp, not whatever was there last.
 *
 * The host's palettes go through host_SetPalette (host_stage_palette), so a
 * capture records them (D3D8CAP_PALETTE, format 7) and a replayed frame
 * expands its P8 textures through the palettes the title set.
 */
HLE_EXPORT(D3DDevice_SetPalette)
{
    static int seen;
#ifdef _WIN32
    uint32_t stage = HLE_ARG(0), palette_va = HLE_ARG(1);
#endif

    if (!seen) {
        seen = 1;
        fprintf(stderr, "[HLE] D3DDevice_SetPalette(stage %u) replaced by name\n",
                (unsigned)HLE_ARG(0));
        fflush(stderr);
    }
    if (!hle_original_D3DDevice_SetPalette) {
        fprintf(stderr, "[HLE] D3DDevice_SetPalette: original body missing -- "
                        "regenerate the lift\n");
        return;
    }
    HLE_CALL_ORIGINAL(D3DDevice_SetPalette);
#ifdef _WIN32
    {
        IDirect3DDevice8 *dev = hle_d3d8_shadow_device();
        uint32_t data;

        if (!dev || stage >= 4u)
            return;
        if (!palette_va) {
            set_pal_data(stage, 0);
            return;
        }
        data = HLE_MEM32(palette_va + 4) & 0x0FFFFFFFu;
        /* 256 entries of 4 bytes, and it has to be inside the window the
         * texels live in or the pointer is not a palette. */
        if (!data || (uint64_t)data + 1024u > CONTIG_SIZE) {
            static int said;
            if (!said++) {
                fprintf(stderr, "[HLE-D3D8] SetPalette: stage %u palette data "
                        "0x%08X is outside the contiguous window; ignored\n",
                        (unsigned)stage, data);
                fflush(stderr);
            }
            return;
        }
        set_pal_data(stage, data);
    }
#endif
}
