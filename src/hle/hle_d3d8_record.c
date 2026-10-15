/*
 * hle_d3d8_record.c -- shadow mode's host boundary, and frame capture at it.
 *
 * Part of shadow mode (hle_d3d8.c, RECOMP_HLE_D3D8=shadow). What the
 * wrappers are for, and the rule that every host call goes through them, is
 * in hle_d3d8_record.h; the capture format is in d3d8_capture.h.
 *
 * The default capture swap is 120 because the first frames of a title are
 * its loader: nothing is bound, most state is still at the device's defaults,
 * and the capture would be of an empty screen. 120 swaps is far enough in to
 * have textures and a real draw list, and still inside the first minute.
 *
 * RECOMP_D3D8_CAPTURE_EVERY=<n> (n >= 1) keeps capturing: after the frame
 * at the requested swap, one more every n swaps, each to <path>_<swap> with
 * the capture extension, at most CAPTURE_MAX_FILES files. A title never
 * reaches the same moment at the same swap twice, so one long run captured
 * throughout is how to get the frame wanted; the files are picked afterwards.
 *
 * Frame boundaries: recording starts when the swap counter reaches the
 * requested swap and stops at the next one, so the file holds the host calls
 * between one Swap and the next. Most of the state those calls draw with was
 * set in earlier frames, so the capture opens with a snapshot read back from
 * the host renderer itself (capture_snapshot). Reading the host rather than
 * asking src/hle to re-emit what it holds is what makes the capture
 * independent of src/hle's change caches: whatever they believe, the snapshot
 * is what the device actually had.
 *
 * Not handled:
 *   - a stage bound to a volume texture is recorded as nothing bound, and
 *     counted. src/hle does not create them, so this does not happen today.
 *   - more than CAPTURE_MAX_TEXTURES distinct textures in one frame: the rest
 *     are recorded as nothing bound, and counted.
 *   - calls made while the capture file is being closed on another thread;
 *     see "Threads" in hle_d3d8_record.h.
 */
#include "platform/xbox_winnt.h"
#include <stddef.h>   /* offsetof: a kept declaration's size */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#include "d3d8_xbox.h"
#include "d3d8_internal.h"
#include "d3d8_vsh.h"
#include "d3d8_combiners.h"
#include "d3d8_capture.h"
#include "hle_d3d8_record.h"
#include "recomp_config.h"   /* RECOMP_HLE_D3D8_DEFER / defer_draws */

/* hle_d3d8.c: the shadow device, or NULL. */
IDirect3DDevice8 *hle_d3d8_shadow_device(void);

#define CAPTURE_DEFAULT_SWAP    120
#define CAPTURE_MAX_TEXTURES    1024
#define CAPTURE_MAX_FILES       24   /* as RECOMP_HLE_D3D8_DUMP */
#define CAPTURE_MAX_LEVELS      16
#define CAPTURE_STAGES          4    /* d3d8_device.c MAX_TEXTURE_STAGES */
#define CAPTURE_RENDER_STATES   256  /* d3d8_device.c MAX_RENDER_STATES */
#define CAPTURE_STAGE_STATES    32   /* d3d8_device.c MAX_TSS_STATES */

static D3D8CapWriter *g_cap;
static int            g_configured;
static const char    *g_path;
static unsigned long  g_target_swap;
static int            g_capture_asap;   /* the next swap, whichever it is */
static unsigned long  g_frame;
static unsigned long  g_every;       /* RECOMP_D3D8_CAPTURE_EVERY, 0 = once */
static unsigned long  g_min_draws;   /* RECOMP_D3D8_CAPTURE_MINDRAWS, 0 = any */
static unsigned       g_files;
static char           g_file[1024];  /* the file being written */

/* Host texture objects whose contents are in this capture, and their ids. */
static struct {
    IDirect3DBaseTexture8 *object;
    uint32_t               id;
} g_textures[CAPTURE_MAX_TEXTURES];
static int      g_texture_count;
static uint32_t g_next_texture_id;

/* Host depth surfaces a SET_RENDER_TARGET in this capture has named. Shadow
 * mode keeps its depth surfaces for the life of the process (hle_d3d8.c,
 * depth_surface), so a pointer never comes back as a different surface. */
#define CAPTURE_MAX_DEPTHS 16
static IDirect3DSurface8 *g_depth_objects[CAPTURE_MAX_DEPTHS];
static int                g_depth_count;
static IDirect3DSurface8 *g_device_depth;   /* host_DeviceDepthSurface */

/* The render target the wrappers last set, for the snapshot. Kept here
 * because the host's surface for a texture level does not lead back to the
 * texture; host_SetRenderTarget is the only place the target changes, so
 * this is the host's own. g_target_lost: the snapshot cannot name the
 * target -- the bound texture was released (the host still draws into it
 * through its surface), or the host refused the last switch. */
static IDirect3DBaseTexture8 *g_target_texture;
static UINT               g_target_level, g_target_face;
static IDirect3DSurface8 *g_target_depth;
static int                g_target_lost;

static unsigned long g_draws, g_texture_writes, g_level_writes, g_unrecorded_binds;
static unsigned long g_unrecorded_targets;
static uint64_t      g_texture_bytes;

int hle_d3d8_capture_active(void)
{
    return g_cap != NULL;
}

static void chunk(uint32_t type, const void *p0, size_t n0, const void *p1, size_t n1,
                  const void *p2, size_t n2)
{
    d3d8cap_chunk(g_cap, type, p0, n0, p1, n1, p2, n2);
}

/* ---------------------------------------------------------------- textures */

static int texture_find(IDirect3DBaseTexture8 *object)
{
    int i;

    for (i = 0; i < g_texture_count; i++)
        if (g_textures[i].object == object)
            return i;
    return -1;
}

/* One face and level of a cube's texels, as the host holds them. */
static void cube_level_write(IDirect3DBaseTexture8 *object, uint32_t id,
                             UINT face, UINT level)
{
    D3D8CapCubeLevel c;
    const BYTE *bits;
    UINT pitch, rows;

    if (!d3d8_cube_level(object, face, level, &bits, &pitch, &rows))
        return;
    c.id    = id;
    c.face  = face;
    c.level = level;
    c.pitch = pitch;
    c.rows  = rows;
    c.bytes = pitch * rows;
    chunk(D3D8CAP_CUBE_LEVEL, &c, sizeof c, bits, c.bytes, NULL, 0);
    g_texture_bytes += c.bytes;
}

/* A cube texture's creation, then its texels unless the title renders into
 * it: a cube filled from memory (hle_d3d8_texture.c, static_cube) would
 * otherwise replay empty, and every surface lit through it -- a
 * normalisation cube's dot product, an environment map -- with it.
 * d3d8_cube_info is also the type test: it answers only for a cube. */
static int cube_write(IDirect3DBaseTexture8 *object, uint32_t id)
{
    D3D8CubeInfo info;
    D3D8CapCubeTexture head;

    if (!d3d8_cube_info(object, &info))
        return 0;
    head.id     = id;
    head.format = (uint32_t)info.format;
    head.edge   = info.edge;
    head.levels = info.levels;
    head.usage  = info.usage;
    chunk(D3D8CAP_CUBE_TEXTURE, &head, sizeof head, NULL, 0, NULL, 0);
    g_texture_writes++;
    if (!(info.usage & D3DUSAGE_RENDERTARGET)) {
        UINT f, l;

        for (f = 0; f < 6; f++)
            for (l = 0; l < info.levels; l++)
                cube_level_write(object, id, f, l);
    }
    return 1;
}

/* The whole texture as the host holds it. The host keeps its levels back to
 * back in one allocation (d3d8_internal.h, D3D8Texture.sys_mem), so they go
 * out as one run of bytes; that layout is checked rather than assumed. */
static int texture_write(IDirect3DBaseTexture8 *object, uint32_t id)
{
    D3D8TextureInfo info;
    D3D8CapTexture head;
    D3D8CapLevel levels[CAPTURE_MAX_LEVELS];
    const BYTE *first = NULL;
    size_t total = 0;
    UINT l;

    if (!d3d8_texture_info(object, &info) || !info.levels ||
        info.levels > CAPTURE_MAX_LEVELS)
        return 0;
    for (l = 0; l < info.levels; l++) {
        const BYTE *bits;
        UINT pitch, rows;

        if (!d3d8_texture_level(object, l, &bits, &pitch, &rows))
            return 0;
        if (!l)
            first = bits;
        else if (bits != first + total)
            return 0;
        levels[l].pitch = pitch;
        levels[l].rows  = rows;
        levels[l].bytes = pitch * rows;
        total += levels[l].bytes;
    }
    head.id     = id;
    head.format = (uint32_t)info.format;
    head.width  = info.width;
    head.height = info.height;
    head.levels = info.levels;
    head.usage  = info.usage;
    chunk(D3D8CAP_TEXTURE, &head, sizeof head,
          levels, (size_t)info.levels * sizeof levels[0], first, total);
    g_texture_writes++;
    g_texture_bytes += total;
    return 1;
}

/* The capture id for a texture about to be bound, writing it first if the
 * capture does not hold it yet. 0 for NULL, and for a texture that cannot be
 * recorded (counted). */
static uint32_t texture_id(IDirect3DBaseTexture8 *object)
{
    int i;

    if (!object)
        return 0;
    i = texture_find(object);
    if (i >= 0)
        return g_textures[i].id;
    if (g_texture_count >= CAPTURE_MAX_TEXTURES ||
        (!texture_write(object, g_next_texture_id) &&
         !cube_write(object, g_next_texture_id))) {
        g_unrecorded_binds++;
        return 0;
    }
    g_textures[g_texture_count].object = object;
    g_textures[g_texture_count].id     = g_next_texture_id;
    g_texture_count++;
    return g_next_texture_id++;
}

/* ------------------------------------------------ record, shared with the
 * snapshot so a state reaches the file the same way whichever wrote it */

static void rec_render_state(DWORD state, DWORD value)
{
    D3D8CapRenderState c;

    c.state = state;
    c.value = value;
    chunk(D3D8CAP_RENDER_STATE, &c, sizeof c, NULL, 0, NULL, 0);
}

static void rec_stage_state(DWORD stage, DWORD type, DWORD value)
{
    D3D8CapStageState c;

    c.stage = stage;
    c.type  = type;
    c.value = value;
    chunk(D3D8CAP_TEXTURE_STAGE_STATE, &c, sizeof c, NULL, 0, NULL, 0);
}

static void rec_transform(DWORD state, const D3DMATRIX *m)
{
    D3D8CapTransform c;

    c.state = state;
    memcpy(c.m, m, sizeof c.m);
    chunk(D3D8CAP_TRANSFORM, &c, sizeof c, NULL, 0, NULL, 0);
}

static void rec_material(const D3DMATERIAL8 *m)
{
    D3D8CapMaterial c;

    memcpy(c.words, m, sizeof c.words);
    chunk(D3D8CAP_MATERIAL, &c, sizeof c, NULL, 0, NULL, 0);
}

static void rec_light(DWORD index, const D3DLIGHT8 *l)
{
    D3D8CapLight c;

    c.index = index;
    memcpy(c.words, l, sizeof c.words);
    chunk(D3D8CAP_LIGHT, &c, sizeof c, NULL, 0, NULL, 0);
}

static void rec_light_enable(DWORD index, BOOL enable)
{
    D3D8CapLightEnable c;

    c.index = index;
    c.enable = enable ? 1u : 0u;
    chunk(D3D8CAP_LIGHT_ENABLE, &c, sizeof c, NULL, 0, NULL, 0);
}

static void rec_scissors(UINT count, BOOL exclusive, const D3DRECT *rect)
{
    D3D8CapScissors c;

    c.count = count;
    c.exclusive = exclusive ? 1 : 0;
    c.rect.x1 = rect ? rect->x1 : 0;
    c.rect.y1 = rect ? rect->y1 : 0;
    c.rect.x2 = rect ? rect->x2 : 0;
    c.rect.y2 = rect ? rect->y2 : 0;
    chunk(D3D8CAP_SCISSORS, &c, sizeof c, NULL, 0, NULL, 0);
}

static void rec_two_d_placement(int placement, uint32_t tag)
{
    D3D8CapTwoDPlacement c;

    c.placement = (uint32_t)placement;
    c.tag = tag;
    chunk(D3D8CAP_TWOD_PLACEMENT, &c, sizeof c, NULL, 0, NULL, 0);
}

static void rec_viewport(const D3DVIEWPORT8 *vp)
{
    D3D8CapViewport c;

    c.x      = vp->X;
    c.y      = vp->Y;
    c.width  = vp->Width;
    c.height = vp->Height;
    c.min_z  = vp->MinZ;
    c.max_z  = vp->MaxZ;
    chunk(D3D8CAP_VIEWPORT, &c, sizeof c, NULL, 0, NULL, 0);
}

static void rec_set_texture(DWORD stage, IDirect3DBaseTexture8 *texture)
{
    D3D8CapSetTexture c;

    c.stage      = stage;
    c.texture_id = texture_id(texture);
    chunk(D3D8CAP_SET_TEXTURE, &c, sizeof c, NULL, 0, NULL, 0);
}

static void rec_palette(DWORD stage, const DWORD *entries)
{
    D3D8CapPalette c;

    c.stage = stage;
    memcpy(c.entries, entries, sizeof c.entries);
    chunk(D3D8CAP_PALETTE, &c, sizeof c, NULL, 0, NULL, 0);
}

static void rec_set_vertex_shader(DWORD handle)
{
    D3D8CapSetVertexShader c;

    c.handle = handle;
    chunk(D3D8CAP_SET_VERTEX_SHADER, &c, sizeof c, NULL, 0, NULL, 0);
}

static void rec_vs_create(DWORD handle, const DWORD *microcode, int insn_count)
{
    D3D8CapVsCreate c;

    c.handle     = handle;
    c.insn_count = (uint32_t)insn_count;
    /* DWORD is 32 bits on Windows, so the microcode goes out as it is. */
    chunk(D3D8CAP_VS_CREATE, &c, sizeof c,
          microcode, (size_t)insn_count * 4u * sizeof(DWORD), NULL, 0);
}

static void rec_vs_declaration(DWORD handle, const D3D8VshInput *inputs, int count)
{
    D3D8CapVsDeclaration c;
    D3D8CapVsInput out[NV2A_VS_MAX_INPUTS];
    int i;

    if (count < 0 || count > NV2A_VS_MAX_INPUTS || (count && !inputs))
        return;                          /* the host refuses it too */
    c.handle = handle;
    c.count  = (uint32_t)count;
    for (i = 0; i < count; i++) {
        out[i].reg         = inputs[i].reg;
        out[i].dxgi_format = (uint32_t)inputs[i].format;
        out[i].offset      = inputs[i].offset;
    }
    chunk(D3D8CAP_VS_DECLARATION, &c, sizeof c,
          out, (size_t)count * sizeof out[0], NULL, 0);
}

static void rec_vs_constants(int first_reg, const float *data, int count)
{
    D3D8CapVsConstants c;

    /* The host clamps to its 192 registers (d3d8_vsh_set_constant); the
     * capture keeps what it actually read. */
    if (!data || first_reg < 0 || first_reg >= NV2A_VS_MAX_CONSTANTS || count <= 0)
        return;
    if (count > NV2A_VS_MAX_CONSTANTS - first_reg)
        count = NV2A_VS_MAX_CONSTANTS - first_reg;
    c.first_reg = (uint32_t)first_reg;
    c.count     = (uint32_t)count;
    chunk(D3D8CAP_VS_CONSTANTS, &c, sizeof c,
          data, (size_t)count * 4u * sizeof(float), NULL, 0);
}

static void rec_vs_screenspace(int enabled, const float scale[4], const float offset[4])
{
    D3D8CapVsScreenspace c;

    c.enabled = enabled != 0;
    memcpy(c.scale, scale, sizeof c.scale);
    memcpy(c.offset, offset, sizeof c.offset);
    chunk(D3D8CAP_VS_SCREENSPACE, &c, sizeof c, NULL, 0, NULL, 0);
}

static void rec_vs_vertex_data(int reg, const float value[4])
{
    D3D8CapVsVertexData c;

    if (reg < 0 || reg >= NV2A_VS_MAX_INPUTS)
        return;                          /* the host ignores it too */
    c.reg = (uint32_t)reg;
    memcpy(c.value, value, sizeof c.value);
    chunk(D3D8CAP_VS_VERTEX_DATA, &c, sizeof c, NULL, 0, NULL, 0);
}

static void rec_ps_token(DWORD token)
{
    D3D8CapPsToken c;

    c.token = token;
    chunk(D3D8CAP_PS_TOKEN, &c, sizeof c, NULL, 0, NULL, 0);
}

/* The capture id for a depth surface, writing it first if needed. 0 for
 * NULL, and for one that cannot be recorded (the caller counts it). */
static uint32_t depth_id(IDirect3DSurface8 *object)
{
    D3DSURFACE_DESC desc;
    D3D8CapDepthSurface c;
    int i;

    if (!object)
        return 0;
    if (object == g_device_depth)
        return D3D8CAP_DEPTH_DEVICE;
    for (i = 0; i < g_depth_count; i++)
        if (g_depth_objects[i] == object)
            return (uint32_t)i + 1;
    if (g_depth_count >= CAPTURE_MAX_DEPTHS ||
        FAILED(object->lpVtbl->GetDesc(object, &desc)))
        return 0;
    g_depth_objects[g_depth_count++] = object;
    c.id     = (uint32_t)g_depth_count;
    c.width  = desc.Width;
    c.height = desc.Height;
    c.format = (uint32_t)desc.Format;
    chunk(D3D8CAP_DEPTH_SURFACE, &c, sizeof c, NULL, 0, NULL, 0);
    return c.id;
}

/* A target the capture cannot name is recorded as the back buffer, which is
 * where replay would draw without it anyway, and counted. */
static void rec_set_render_target(IDirect3DBaseTexture8 *texture, UINT level,
                                  UINT face, IDirect3DSurface8 *depth, int lost)
{
    D3D8CapSetRenderTarget c;
    unsigned long binds = g_unrecorded_binds;

    /* texture_id counts a failure as a texture bind; this one is counted
     * below, as a target. */
    c.texture_id = texture_id(texture);
    g_unrecorded_binds = binds;
    c.level      = c.texture_id ? level : 0;
    c.face       = c.texture_id ? face : 0;
    c.depth_id   = depth_id(depth);
    if (lost || (texture && !c.texture_id) || (depth && !c.depth_id))
        g_unrecorded_targets++;
    chunk(D3D8CAP_SET_RENDER_TARGET, &c, sizeof c, NULL, 0, NULL, 0);
}

/* ---------------------------------------------------------------- snapshot */

/* Everything the frame draws with that was set before it began, read from the
 * host. The order is the one d3d8_capture.h documents: SetTexture rewrites
 * COLOROP, so the stage states come after the textures. */
static void capture_snapshot(IDirect3DDevice8 *dev)
{
    static const DWORD transforms[] = {
        D3DTS_VIEW, D3DTS_PROJECTION,
        D3DTS_TEXTURE0, D3DTS_TEXTURE0 + 1, D3DTS_TEXTURE0 + 2, D3DTS_TEXTURE0 + 3,
        D3DTS_WORLD, D3DTS_WORLD + 1, D3DTS_WORLD + 2, D3DTS_WORLD + 3
    };
    const DWORD *rs = d3d8_GetRenderStates();
    const float *constants = d3d8_vsh_constants();
    float scale[4], offset[4];
    int enabled, slot, i;
    DWORD vs = 0, s, t;
    D3DVIEWPORT8 vp;

    for (slot = 0; slot < NV2A_VS_MAX_SLOTS; slot++) {
        const DWORD *microcode;
        const D3D8VshInput *decl;
        int length, decl_count;
        DWORD handle;

        if (!d3d8_vsh_get_slot(slot, &handle, &microcode, &length, &decl, &decl_count))
            continue;
        rec_vs_create(handle, microcode, length);
        if (decl_count)
            rec_vs_declaration(handle, decl, decl_count);
    }
    if (constants)
        rec_vs_constants(0, constants, NV2A_VS_MAX_CONSTANTS);
    d3d8_vsh_get_screenspace(scale, offset, &enabled);
    rec_vs_screenspace(enabled, scale, offset);
    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        float value[4];

        d3d8_vsh_get_vertex_data(i, value);
        rec_vs_vertex_data(i, value);
    }
    rec_ps_token(d3d8_combiners_get_pixel_shader());

    dev->lpVtbl->GetVertexShader(dev, &vs);
    rec_set_vertex_shader(vs);
    /* The palettes before the textures: a P8 texture is expanded through one
     * when replay uploads it. */
    for (s = 0; s < CAPTURE_STAGES; s++)
        rec_palette(s, d3d8_GetPalette(s));
    for (s = 0; s < CAPTURE_STAGES; s++)
        rec_set_texture(s, d3d8_GetStageTexture(s));
    rec_set_render_target(g_target_texture, g_target_level, g_target_face,
                          g_target_depth, g_target_lost);

    for (i = 0; i < (int)(sizeof transforms / sizeof transforms[0]); i++) {
        const D3DMATRIX *m = d3d8_GetTransform((D3DTRANSFORMSTATETYPE)transforms[i]);
        if (m)
            rec_transform(transforms[i], m);
    }
    /* Lights and the material: a title sets them once for a scene, long
     * before the frame a capture starts on. */
    rec_material(d3d8_GetMaterial());
    for (i = 0; i < (int)d3d8_GetNumLights(); i++) {
        const D3DLIGHT8 *l = d3d8_GetLight((DWORD)i);
        if (l)
            rec_light((DWORD)i, l);
        rec_light_enable((DWORD)i, d3d8_GetLightEnable((DWORD)i));
    }
    dev->lpVtbl->GetViewport(dev, &vp);
    rec_viewport(&vp);
    {
        UINT count; BOOL exclusive; D3DRECT rect;
        xbox_D3D8GetScissors(&count, &exclusive, &rect);
        rec_scissors(count, exclusive, &rect);
    }
    {
        uint32_t tag;
        int placement = xbox_D3D8GetTwoDPlacement(&tag);
        rec_two_d_placement(placement, tag);
    }

    for (s = 0; rs && s < CAPTURE_RENDER_STATES; s++)
        rec_render_state(s, rs[s]);
    for (s = 0; s < CAPTURE_STAGES; s++) {
        const DWORD *tss = d3d8_GetTSS(s);
        for (t = 0; tss && t < CAPTURE_STAGE_STATES; t++)
            rec_stage_state(s, t, tss[t]);
    }

    chunk(D3D8CAP_FRAME_START, NULL, 0, NULL, 0, NULL, 0);
}

/* ------------------------------------------------------------ frame boundary */

static void capture_configure(void)
{
    const char *swap, *every, *min_draws;

    g_configured = 1;
    g_path = getenv("RECOMP_D3D8_CAPTURE");
    g_target_swap = CAPTURE_DEFAULT_SWAP;
    swap = getenv("RECOMP_D3D8_CAPTURE_SWAP");
    if (swap && atol(swap) > 0)
        g_target_swap = (unsigned long)atol(swap);
    every = getenv("RECOMP_D3D8_CAPTURE_EVERY");
    if (every && atol(every) >= 1)
        g_every = (unsigned long)atol(every);
    min_draws = getenv("RECOMP_D3D8_CAPTURE_MINDRAWS");
    if (min_draws && atol(min_draws) > 0)
        g_min_draws = (unsigned long)atol(min_draws);
    if (g_path && *g_path)
        fprintf(stderr, "[HLE-D3D8] capture armed: swap %lu%s%s -> %s (host calls, "
                "format v%u)\n", g_target_swap,
                g_min_draws ? " or the first frame after it with enough draws" : "",
                g_every ? ", then every RECOMP_D3D8_CAPTURE_EVERY swaps" : "",
                g_path, D3D8CAP_VERSION);
}

void hle_d3d8_capture_next_frame(void)
{
    static char fallback[512];
    static int asked;

    if (!g_configured)
        capture_configure();
    if (!g_path || !*g_path) {
        /* Nowhere was asked for, so put it where the player can find it --
         * numbered, like the dumps, because somebody pressing the key at
         * several places wants all of them, not the last one. */
        snprintf(fallback, sizeof fallback, "frame%03d%s", asked++, D3D8CAP_EXTENSION);
        g_path = fallback;
        g_every = 0;
        g_min_draws = 0;
    }
    g_capture_asap = 1;                  /* whichever swap comes next */
    fprintf(stderr, "[HLE-D3D8] capturing the next frame to %s\n", g_path);
    fflush(stderr);
}

void hle_d3d8_capture_swap(unsigned long swaps, uint32_t width, uint32_t height)
{
    IDirect3DDevice8 *dev;

    if (!g_configured)
        capture_configure();
    if (!g_path || !*g_path)
        return;

    if (g_cap) {
        D3D8CapWriter *w = g_cap;
        uint32_t chunks = d3d8cap_chunk_count(w);
        int ok;

        g_cap = NULL;                    /* stop recording before closing */
        ok = d3d8cap_close(w) == 0;
        if (g_min_draws && g_draws < g_min_draws) {
            /* Too few draws: dropped, and this swap starts the next try. */
            remove(g_file);
            g_unrecorded_targets = g_unrecorded_binds = 0;
            g_target_swap = swaps;
            goto start;
        }
        fprintf(stderr, "[HLE-D3D8] capture: frame %lu written to %s -- %u chunks, "
                "%lu draws, %lu textures (%llu bytes), %lu level refills%s\n",
                g_frame, g_file, chunks, g_draws, g_texture_writes,
                (unsigned long long)g_texture_bytes, g_level_writes,
                ok ? "" : " (INCOMPLETE: write failed)");
        if (g_unrecorded_targets)
            fprintf(stderr, "[HLE-D3D8] capture: %lu render target switches recorded "
                    "as the back buffer (target released or refused, or table full)\n",
                    g_unrecorded_targets);
        if (g_unrecorded_binds)
            fprintf(stderr, "[HLE-D3D8] capture: %lu texture binds recorded as "
                    "nothing bound (not a 2D texture, or table full)\n",
                    g_unrecorded_binds);
        fflush(stderr);
        g_files++;
        if (g_every && g_files < CAPTURE_MAX_FILES)
            g_target_swap = g_frame + g_every;
        else
            g_path = NULL;               /* done: one frame, or the file cap */
        /* EVERY=1: the next frame starts at this very swap. Consecutive
         * frames are what frame interpolation is measured on -- two frames
         * and the one between them. */
        if (g_path && g_target_swap == swaps)
            goto start;
        return;
    }
start:
    /* Asked for by hand: this swap is the target, since the number it would
     * otherwise have to match cannot be known before the run. */
    if (g_capture_asap) {
        g_capture_asap = 0;
        g_target_swap = swaps;
    }
    if (swaps != g_target_swap)
        return;
    dev = hle_d3d8_shadow_device();
    if (!dev)
        return;

    g_frame = swaps;
    g_texture_count = 0;
    g_next_texture_id = 1;
    g_depth_count = 0;
    g_draws = g_texture_writes = g_level_writes = g_unrecorded_binds = 0;
    g_unrecorded_targets = 0;
    g_texture_bytes = 0;
    if (g_every) {
        /* <path>_<swap><extension>, with the extension moved to the end. */
        size_t n = strlen(g_path), e = strlen(D3D8CAP_EXTENSION);

        if (n >= e && strcmp(g_path + n - e, D3D8CAP_EXTENSION) == 0)
            n -= e;
        snprintf(g_file, sizeof g_file, "%.*s_%05lu%s", (int)n, g_path, swaps,
                 D3D8CAP_EXTENSION);
    } else {
        snprintf(g_file, sizeof g_file, "%s", g_path);
    }
    g_cap = d3d8cap_create(g_file, (uint32_t)swaps, width, height);
    if (!g_cap) {
        fprintf(stderr, "[HLE-D3D8] capture: cannot write %s; capture off\n", g_file);
        g_path = NULL;                   /* one attempt, not one per swap */
        return;
    }
    capture_snapshot(dev);
}

/* --------------------------------------------------------- deferred frames */

/* The queue: records of [header][payload], each header naming the function
 * that runs the payload. One per process, guarded by one lock, because the
 * title's draws can come from more than one guest thread and the order they
 * arrive in is the order the push buffer would have held them. */
typedef struct {
    uint32_t size;                       /* payload bytes */
    uint32_t pad;
    void (*fn)(const void *arg);
} defer_header;

#define DEFER_ALIGN(n)  (((n) + 15u) & ~(size_t)15u)
#define DEFER_LIMIT     (64u * 1024u * 1024u)   /* run early past this */

static uint8_t          *g_dq;
static size_t            g_dq_len, g_dq_cap;
static CRITICAL_SECTION  g_dq_cs;
static INIT_ONCE         g_dq_once = INIT_ONCE_STATIC_INIT;
static volatile DWORD    g_dq_runner;    /* thread running the queue, or 0 */
static int               g_defer = -1;
static unsigned long     g_dq_flushes, g_dq_early, g_dq_ops_run;

static BOOL CALLBACK dq_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_dq_cs);
    return TRUE;
}

int hle_d3d8_defer_on(void)
{
    if (g_defer < 0) {
        InitOnceExecuteOnce(&g_dq_once, dq_init, NULL, NULL);
        g_defer = recomp_config_bool("RECOMP_HLE_D3D8_DEFER", "defer_draws", 0);
        if (g_defer) {
            fprintf(stderr, "[HLE-D3D8] deferred frames: device calls run at the "
                    "frame's end (RECOMP_HLE_D3D8_DEFER)\n");
            fflush(stderr);
        }
    }
    return g_defer;
}

int hle_d3d8_defer_recording(void)
{
    return hle_d3d8_defer_on() && g_dq_runner != GetCurrentThreadId();
}

void hle_d3d8_defer_op(void (*fn)(const void *arg), const void *arg, size_t size)
{
    size_t need = sizeof(defer_header) + DEFER_ALIGN(size);
    defer_header *h;

    EnterCriticalSection(&g_dq_cs);
    if (g_dq_len + need > g_dq_cap) {
        size_t cap = g_dq_cap ? g_dq_cap * 2 : (size_t)1 << 20;
        uint8_t *grown;
        while (cap < g_dq_len + need)
            cap *= 2;
        grown = (uint8_t *)realloc(g_dq, cap);
        if (!grown) {
            /* No room: run what is queued and then this, in order. */
            LeaveCriticalSection(&g_dq_cs);
            hle_d3d8_defer_flush();
            g_dq_runner = GetCurrentThreadId();
            fn(arg);
            g_dq_runner = 0;
            return;
        }
        g_dq = grown;
        g_dq_cap = cap;
    }
    h = (defer_header *)(g_dq + g_dq_len);
    h->size = (uint32_t)size;
    h->pad = 0;
    h->fn = fn;
    if (size)
        memcpy(h + 1, arg, size);
    g_dq_len += need;
    LeaveCriticalSection(&g_dq_cs);

    /* A title that goes a long way between swaps (a loading screen that
     * draws without presenting) must not grow the queue without bound. */
    if (g_dq_len > DEFER_LIMIT) {
        g_dq_early++;
        hle_d3d8_defer_flush();
    }
}

void hle_d3d8_defer_flush(void)
{
    size_t off;

    if (g_defer <= 0)
        return;
    EnterCriticalSection(&g_dq_cs);
    g_dq_runner = GetCurrentThreadId();
    for (off = 0; off < g_dq_len; ) {
        defer_header *h = (defer_header *)(g_dq + off);
        h->fn(h->size ? (const void *)(h + 1) : NULL);
        off += sizeof(defer_header) + DEFER_ALIGN(h->size);
        g_dq_ops_run++;
    }
    g_dq_len = 0;
    g_dq_runner = 0;
    g_dq_flushes++;
    LeaveCriticalSection(&g_dq_cs);
    if ((g_dq_flushes & 4095u) == 1) {
        fprintf(stderr, "[HLE-D3D8] deferred frames: %lu flushes (%lu early), "
                "%lu calls run\n", g_dq_flushes, g_dq_early, g_dq_ops_run);
        fflush(stderr);
    }
}

/* Payload helpers for the wrappers below: a fixed part, then up to two
 * variable-length copies of what the call points at. */
static void defer_call(void (*fn)(const void *), const void *fixed, size_t fixed_size,
                       const void *a, size_t a_size, const void *b, size_t b_size)
{
    uint8_t stack[256], *p = stack;
    size_t total = DEFER_ALIGN(fixed_size) + DEFER_ALIGN(a_size) + b_size;

    if (total > sizeof stack) {
        p = (uint8_t *)malloc(total);
        if (!p)
            return;
    }
    memcpy(p, fixed, fixed_size);
    if (a_size)
        memcpy(p + DEFER_ALIGN(fixed_size), a, a_size);
    if (b_size)
        memcpy(p + DEFER_ALIGN(fixed_size) + DEFER_ALIGN(a_size), b, b_size);
    hle_d3d8_defer_op(fn, p, total);
    if (p != stack)
        free(p);
}

#define DEFER_PART_A(arg, fixed_type) \
    ((const uint8_t *)(arg) + DEFER_ALIGN(sizeof(fixed_type)))
#define DEFER_PART_B(arg, fixed_type, a_size) \
    (DEFER_PART_A(arg, fixed_type) + DEFER_ALIGN(a_size))

/* ------------------------------------------------- frame interpolation
 *
 * The same ops, kept for hle_d3d8_interp.c while the call also runs (see
 * hle_d3d8_record.h). retain_call lays a payload out as defer_call does. */
int hle_d3d8_interp_rec;

/* The vertex constants as src/hle gave them, which is before Hor+ scales
 * its register: an op that sets them again goes through the same scaling,
 * so it has to start from the same values. And what a draw is drawn with,
 * for its key. */
static float                  g_constants[NV2A_VS_MAX_CONSTANTS * 4];
static DWORD                  g_bound_vs;
static IDirect3DBaseTexture8 *g_bound_tex[CAPTURE_STAGES];

const float *hle_d3d8_interp_constants(void)
{
    return g_constants;
}

static void retain_call(void (*fn)(const void *), const void *fixed, size_t fixed_size,
                        const void *a, size_t a_size, const void *b, size_t b_size,
                        const HleInterpDrawKey *key)
{
    uint8_t stack[256], *p = stack;
    size_t total = DEFER_ALIGN(fixed_size) + DEFER_ALIGN(a_size) + b_size;

    if (total > sizeof stack) {
        p = (uint8_t *)malloc(total);
        if (!p)
            return;
    }
    memcpy(p, fixed, fixed_size);
    if (a_size)
        memcpy(p + DEFER_ALIGN(fixed_size), a, a_size);
    if (b_size)
        memcpy(p + DEFER_ALIGN(fixed_size) + DEFER_ALIGN(a_size), b, b_size);
    if (key)
        hle_d3d8_interp_draw(fn, p, total, key);
    else
        hle_d3d8_interp_op(fn, p, total);
    if (p != stack)
        free(p);
}

/* A draw's identity across frames: the bytes it draws, hashed. Eight bytes
 * a step, so a frame's megabyte of vertices costs a fraction of a
 * millisecond. */
static uint64_t hash_bytes(uint64_t h, const void *data, size_t n)
{
    const uint8_t *b = (const uint8_t *)data;

    while (n >= 8) {
        uint64_t v;
        memcpy(&v, b, 8);
        h = (h ^ v) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
        b += 8;
        n -= 8;
    }
    while (n--)
        h = (h ^ *b++) * 0x100000001B3ull;
    return h;
}

static void draw_key(HleInterpDrawKey *k, DWORD type, UINT prims, UINT stride,
                     DWORD index_format)
{
    int s;

    k->vs = g_bound_vs;
    k->stride = stride;
    k->prims = prims;
    k->prim_type = type;
    k->index_format = index_format;
    for (s = 0; s < CAPTURE_STAGES; s++)
        k->tex[s] = g_bound_tex[s];
    k->target = g_target_lost ? NULL : g_target_texture;
    /* Read after the draw: the program's verdict is made when it is bound
     * for it. A fixed-function draw is not blended. */
    k->programmable = d3d8_vsh_is_programmable(g_bound_vs);
    k->uses_proj = k->programmable && d3d8_vsh_bound_uses_projection();
}

/* ------------------------------------------------------------ device calls */

typedef struct { DWORD count, flags; D3DCOLOR color; float z; DWORD stencil; } dq_clear;

static void op_clear(const void *arg)
{
    const dq_clear *c = (const dq_clear *)arg;
    host_Clear(hle_d3d8_shadow_device(), c->count,
               c->count ? (const D3DRECT *)DEFER_PART_A(arg, dq_clear) : NULL,
               c->flags, c->color, c->z, c->stencil);
}

HRESULT host_Clear(IDirect3DDevice8 *dev, DWORD count, const D3DRECT *rects,
                   DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
    if (hle_d3d8_defer_recording()) {
        dq_clear c;
        c.count = rects ? count : 0;
        c.flags = flags;
        c.color = color;
        c.z = z;
        c.stencil = stencil;
        defer_call(op_clear, &c, sizeof c, rects, (size_t)c.count * sizeof *rects, NULL, 0);
        return S_OK;
    }
    if (hle_d3d8_interp_rec) {
        dq_clear c;
        c.count = rects ? count : 0;
        c.flags = flags;
        c.color = color;
        c.z = z;
        c.stencil = stencil;
        retain_call(op_clear, &c, sizeof c, rects, (size_t)c.count * sizeof *rects,
                    NULL, 0, NULL);
    }
    if (g_cap) {
        D3D8CapClear c;
        D3D8CapRect r[16];
        DWORD i, n = rects ? count : 0;

        if (n > 16)                      /* src/hle passes none; more is a bug */
            n = 16;
        for (i = 0; i < n; i++) {
            r[i].x1 = rects[i].x1;
            r[i].y1 = rects[i].y1;
            r[i].x2 = rects[i].x2;
            r[i].y2 = rects[i].y2;
        }
        c.rect_count = n;
        c.flags      = flags;
        c.color      = color;
        c.z          = z;
        c.stencil    = stencil;
        chunk(D3D8CAP_CLEAR, &c, sizeof c, r, (size_t)n * sizeof r[0], NULL, 0);
    }
    return dev->lpVtbl->Clear(dev, count, rects, flags, color, z, stencil);
}

HRESULT host_Swap(IDirect3DDevice8 *dev, DWORD flags)
{
    hle_d3d8_defer_flush();              /* the frame is drawn before it shows */
    return dev->lpVtbl->Swap(dev, flags);
}

typedef struct { DWORD a, b, c; } dq_3;

static void op_render_state(const void *arg)
{
    const dq_3 *p = (const dq_3 *)arg;
    host_SetRenderState(hle_d3d8_shadow_device(), (D3DRENDERSTATETYPE)p->a, p->b);
}

HRESULT host_SetRenderState(IDirect3DDevice8 *dev, D3DRENDERSTATETYPE state, DWORD value)
{
    if (hle_d3d8_defer_recording()) {
        dq_3 p = { (DWORD)state, value, 0 };
        hle_d3d8_defer_op(op_render_state, &p, sizeof p);
        return S_OK;
    }
    if (hle_d3d8_interp_rec) {
        dq_3 p = { (DWORD)state, value, 0 };
        hle_d3d8_interp_op(op_render_state, &p, sizeof p);
    }
    if (g_cap)
        rec_render_state((DWORD)state, value);
    return dev->lpVtbl->SetRenderState(dev, state, value);
}

static void op_stage_state(const void *arg)
{
    const dq_3 *p = (const dq_3 *)arg;
    host_SetTextureStageState(hle_d3d8_shadow_device(), p->a,
                              (D3DTEXTURESTAGESTATETYPE)p->b, p->c);
}

HRESULT host_SetTextureStageState(IDirect3DDevice8 *dev, DWORD stage,
                                  D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
    if (hle_d3d8_defer_recording()) {
        dq_3 p = { stage, (DWORD)type, value };
        hle_d3d8_defer_op(op_stage_state, &p, sizeof p);
        return S_OK;
    }
    if (hle_d3d8_interp_rec) {
        dq_3 p = { stage, (DWORD)type, value };
        hle_d3d8_interp_op(op_stage_state, &p, sizeof p);
    }
    if (g_cap)
        rec_stage_state(stage, (DWORD)type, value);
    return dev->lpVtbl->SetTextureStageState(dev, stage, type, value);
}

typedef struct { DWORD state; D3DMATRIX m; } dq_transform;

static void op_transform(const void *arg)
{
    const dq_transform *p = (const dq_transform *)arg;
    host_SetTransform(hle_d3d8_shadow_device(), (D3DTRANSFORMSTATETYPE)p->state, &p->m);
}

HRESULT host_SetTransform(IDirect3DDevice8 *dev, D3DTRANSFORMSTATETYPE state,
                          const D3DMATRIX *matrix)
{
    if (hle_d3d8_defer_recording() && matrix) {
        dq_transform p;
        p.state = (DWORD)state;
        p.m = *matrix;
        hle_d3d8_defer_op(op_transform, &p, sizeof p);
        return S_OK;
    }
    if (hle_d3d8_interp_rec && matrix) {
        dq_transform p;
        p.state = (DWORD)state;
        p.m = *matrix;
        hle_d3d8_interp_op(op_transform, &p, sizeof p);
    }
    if (g_cap && matrix)
        rec_transform((DWORD)state, matrix);
    return dev->lpVtbl->SetTransform(dev, state, matrix);
}

static void op_material(const void *arg)
{
    host_SetMaterial(hle_d3d8_shadow_device(), (const D3DMATERIAL8 *)arg);
}

HRESULT host_SetMaterial(IDirect3DDevice8 *dev, const D3DMATERIAL8 *material)
{
    if (!material)
        return E_INVALIDARG;
    if (hle_d3d8_defer_recording()) {
        hle_d3d8_defer_op(op_material, material, sizeof *material);
        return S_OK;
    }
    if (hle_d3d8_interp_rec)
        hle_d3d8_interp_op(op_material, material, sizeof *material);
    if (g_cap)
        rec_material(material);
    return dev->lpVtbl->SetMaterial(dev, material);
}

typedef struct { DWORD index; D3DLIGHT8 light; } dq_light;

static void op_light(const void *arg)
{
    const dq_light *p = (const dq_light *)arg;
    host_SetLight(hle_d3d8_shadow_device(), p->index, &p->light);
}

HRESULT host_SetLight(IDirect3DDevice8 *dev, DWORD index, const D3DLIGHT8 *light)
{
    if (!light)
        return E_INVALIDARG;
    if (hle_d3d8_defer_recording()) {
        dq_light p;
        p.index = index;
        p.light = *light;
        hle_d3d8_defer_op(op_light, &p, sizeof p);
        return S_OK;
    }
    if (hle_d3d8_interp_rec) {
        dq_light p;
        p.index = index;
        p.light = *light;
        hle_d3d8_interp_op(op_light, &p, sizeof p);
    }
    if (g_cap)
        rec_light(index, light);
    return dev->lpVtbl->SetLight(dev, index, light);
}

typedef struct { DWORD index; BOOL enable; } dq_light_enable;

static void op_light_enable(const void *arg)
{
    const dq_light_enable *p = (const dq_light_enable *)arg;
    host_LightEnable(hle_d3d8_shadow_device(), p->index, p->enable);
}

HRESULT host_LightEnable(IDirect3DDevice8 *dev, DWORD index, BOOL enable)
{
    if (hle_d3d8_defer_recording()) {
        dq_light_enable p;
        p.index = index;
        p.enable = enable;
        hle_d3d8_defer_op(op_light_enable, &p, sizeof p);
        return S_OK;
    }
    if (hle_d3d8_interp_rec) {
        dq_light_enable p;
        p.index = index;
        p.enable = enable;
        hle_d3d8_interp_op(op_light_enable, &p, sizeof p);
    }
    if (g_cap)
        rec_light_enable(index, enable);
    return dev->lpVtbl->LightEnable(dev, index, enable);
}

typedef struct { UINT count; BOOL exclusive; } dq_scissors;

static void op_scissors(const void *arg)
{
    const dq_scissors *p = (const dq_scissors *)arg;
    host_SetScissors(p->count, p->exclusive,
                     p->count ? (const D3DRECT *)DEFER_PART_A(arg, dq_scissors) : NULL);
}

void host_SetScissors(UINT count, BOOL exclusive, const D3DRECT *rects)
{
    if (hle_d3d8_defer_recording()) {
        dq_scissors p;
        p.count = rects ? count : 0;
        p.exclusive = exclusive;
        defer_call(op_scissors, &p, sizeof p, rects, (size_t)p.count * sizeof *rects,
                   NULL, 0);
        return;
    }
    if (hle_d3d8_interp_rec) {
        dq_scissors p;
        p.count = rects ? count : 0;
        p.exclusive = exclusive;
        retain_call(op_scissors, &p, sizeof p, rects, (size_t)p.count * sizeof *rects,
                    NULL, 0, NULL);
    }
    if (g_cap)
        rec_scissors(count, exclusive, count && rects ? &rects[0] : NULL);
    xbox_D3D8SetScissors(count, exclusive, rects);
}

typedef struct { int placement; uint32_t tag; } dq_two_d_placement;

static void op_two_d_placement(const void *arg)
{
    const dq_two_d_placement *p = (const dq_two_d_placement *)arg;
    host_SetTwoDPlacement(p->placement, p->tag);
}

void host_SetTwoDPlacement(int placement, uint32_t tag)
{
    if (hle_d3d8_defer_recording()) {
        dq_two_d_placement p;
        p.placement = placement;
        p.tag = tag;
        hle_d3d8_defer_op(op_two_d_placement, &p, sizeof p);
        return;
    }
    if (hle_d3d8_interp_rec) {
        dq_two_d_placement p;
        p.placement = placement;
        p.tag = tag;
        hle_d3d8_interp_op(op_two_d_placement, &p, sizeof p);
    }
    if (g_cap)
        rec_two_d_placement(placement, tag);
    xbox_D3D8SetTwoDPlacement(placement, tag);
}

static void op_viewport(const void *arg)
{
    host_SetViewport(hle_d3d8_shadow_device(), (const D3DVIEWPORT8 *)arg);
}

HRESULT host_SetViewport(IDirect3DDevice8 *dev, const D3DVIEWPORT8 *viewport)
{
    if (hle_d3d8_defer_recording() && viewport) {
        hle_d3d8_defer_op(op_viewport, viewport, sizeof *viewport);
        return S_OK;
    }
    if (hle_d3d8_interp_rec && viewport)
        hle_d3d8_interp_op(op_viewport, viewport, sizeof *viewport);
    if (g_cap && viewport)
        rec_viewport(viewport);
    return dev->lpVtbl->SetViewport(dev, viewport);
}

typedef struct { DWORD stage; IDirect3DBaseTexture8 *texture; } dq_set_texture;

static void op_host_set_texture(const void *arg)
{
    const dq_set_texture *p = (const dq_set_texture *)arg;
    host_SetTexture(hle_d3d8_shadow_device(), p->stage, p->texture);
}

HRESULT host_SetTexture(IDirect3DDevice8 *dev, DWORD stage, IDirect3DBaseTexture8 *texture)
{
    if (hle_d3d8_defer_recording()) {
        dq_set_texture p;
        p.stage = stage;
        p.texture = texture;
        hle_d3d8_defer_op(op_host_set_texture, &p, sizeof p);
        return S_OK;
    }
    if (hle_d3d8_interp_rec) {
        dq_set_texture p;
        p.stage = stage;
        p.texture = texture;
        hle_d3d8_interp_op(op_host_set_texture, &p, sizeof p);
    }
    if (stage < CAPTURE_STAGES)
        g_bound_tex[stage] = texture;
    if (g_cap)
        rec_set_texture(stage, texture);
    return dev->lpVtbl->SetTexture(dev, stage, texture);
}

static void op_vertex_shader(const void *arg)
{
    host_SetVertexShader(hle_d3d8_shadow_device(), *(const DWORD *)arg);
}

HRESULT host_SetVertexShader(IDirect3DDevice8 *dev, DWORD handle)
{
    if (hle_d3d8_defer_recording()) {
        hle_d3d8_defer_op(op_vertex_shader, &handle, sizeof handle);
        return S_OK;
    }
    if (hle_d3d8_interp_rec)
        hle_d3d8_interp_op(op_vertex_shader, &handle, sizeof handle);
    g_bound_vs = handle;
    if (g_cap)
        rec_set_vertex_shader(handle);
    return dev->lpVtbl->SetVertexShader(dev, handle);
}

typedef struct { DWORD type; UINT prims, stride; } dq_draw_up;

static void op_draw_up(const void *arg)
{
    const dq_draw_up *p = (const dq_draw_up *)arg;
    host_DrawPrimitiveUP(hle_d3d8_shadow_device(), (D3DPRIMITIVETYPE)p->type, p->prims,
                         DEFER_PART_A(arg, dq_draw_up), p->stride);
}

typedef struct {
    DWORD type;
    UINT min_index, num_vertices, prims, stride;
    DWORD index_format;
    uint32_t index_bytes;
} dq_draw_indexed_up;

static void op_draw_indexed_up(const void *arg)
{
    const dq_draw_indexed_up *p = (const dq_draw_indexed_up *)arg;
    host_DrawIndexedPrimitiveUP(hle_d3d8_shadow_device(), (D3DPRIMITIVETYPE)p->type,
                                p->min_index, p->num_vertices, p->prims,
                                DEFER_PART_A(arg, dq_draw_indexed_up),
                                (D3DFORMAT)p->index_format,
                                DEFER_PART_B(arg, dq_draw_indexed_up, p->index_bytes),
                                p->stride);
}

HRESULT host_DrawPrimitiveUP(IDirect3DDevice8 *dev, D3DPRIMITIVETYPE type,
                             UINT prims, const void *vertices, UINT stride)
{
    if (hle_d3d8_defer_recording() && vertices && stride) {
        dq_draw_up p;
        uint64_t bytes = (uint64_t)d3d8_up_vertices_read(type, prims) * stride;
        if (bytes > DEFER_LIMIT)
            return E_FAIL;
        p.type = (DWORD)type;
        p.prims = prims;
        p.stride = stride;
        defer_call(op_draw_up, &p, sizeof p, vertices, (size_t)bytes, NULL, 0);
        return S_OK;
    }
    if (g_cap && vertices && stride) {
        D3D8CapDrawUp c;
        uint64_t bytes = (uint64_t)d3d8_up_vertices_read(type, prims) * stride;

        if (bytes <= 0xFFFFFFFFu) {
            c.prim_type    = (uint32_t)type;
            c.prim_count   = prims;
            c.stride       = stride;
            c.vertex_bytes = (uint32_t)bytes;
            chunk(D3D8CAP_DRAW_UP, &c, sizeof c, vertices, (size_t)bytes, NULL, 0);
            g_draws++;
        }
    }
    {
        HRESULT hr = dev->lpVtbl->DrawPrimitiveUP(dev, type, prims, vertices, stride);
        uint64_t bytes = (uint64_t)d3d8_up_vertices_read(type, prims) * stride;

        if (hle_d3d8_interp_rec && vertices && stride && bytes <= DEFER_LIMIT) {
            dq_draw_up p;
            HleInterpDrawKey k;

            draw_key(&k, (DWORD)type, prims, stride, 0);
            k.content = hash_bytes(0xCBF29CE484222325ull, vertices, (size_t)bytes);
            p.type = (DWORD)type;
            p.prims = prims;
            p.stride = stride;
            retain_call(op_draw_up, &p, sizeof p, vertices, (size_t)bytes, NULL, 0, &k);
        }
        return hr;
    }
}

HRESULT host_DrawIndexedPrimitiveUP(IDirect3DDevice8 *dev, D3DPRIMITIVETYPE type,
                                    UINT min_index, UINT num_vertices, UINT prims,
                                    const void *indices, D3DFORMAT index_format,
                                    const void *vertices, UINT stride)
{
    if (hle_d3d8_defer_recording() && indices && vertices && stride) {
        dq_draw_indexed_up p;
        UINT index_size = index_format == D3DFMT_INDEX32 ? 4u : 2u;
        uint64_t ibytes = (uint64_t)d3d8_up_indices_read(type, prims) * index_size;
        /* From the base the indices are relative to, through the last vertex. */
        uint64_t vbytes = (uint64_t)(min_index + num_vertices) * stride;
        if (ibytes > DEFER_LIMIT || vbytes > DEFER_LIMIT)
            return E_FAIL;
        p.type = (DWORD)type;
        p.min_index = min_index;
        p.num_vertices = num_vertices;
        p.prims = prims;
        p.stride = stride;
        p.index_format = (DWORD)index_format;
        p.index_bytes = (uint32_t)ibytes;
        defer_call(op_draw_indexed_up, &p, sizeof p, indices, (size_t)ibytes,
                   vertices, (size_t)vbytes);
        return S_OK;
    }
    if (g_cap && indices && vertices && stride) {
        D3D8CapDrawIndexedUp c;
        UINT index_size = index_format == D3DFMT_INDEX32 ? 4u : 2u;
        uint64_t ibytes = (uint64_t)d3d8_up_indices_read(type, prims) * index_size;
        uint64_t vbytes = (uint64_t)num_vertices * stride;

        if (ibytes <= 0xFFFFFFFFu && vbytes <= 0xFFFFFFFFu) {
            c.prim_type    = (uint32_t)type;
            c.min_index    = min_index;
            c.num_vertices = num_vertices;
            c.prim_count   = prims;
            c.index_format = (uint32_t)index_format;
            c.index_bytes  = (uint32_t)ibytes;
            c.stride       = stride;
            c.vertex_bytes = (uint32_t)vbytes;
            chunk(D3D8CAP_DRAW_INDEXED_UP, &c, sizeof c,
                  indices, (size_t)ibytes, vertices, (size_t)vbytes);
            g_draws++;
        }
    }
    {
        HRESULT hr = dev->lpVtbl->DrawIndexedPrimitiveUP(dev, type, min_index, num_vertices,
                                                         prims, indices, index_format,
                                                         vertices, stride);
        UINT index_size = index_format == D3DFMT_INDEX32 ? 4u : 2u;
        uint64_t ibytes = (uint64_t)d3d8_up_indices_read(type, prims) * index_size;
        uint64_t vbytes = (uint64_t)(min_index + num_vertices) * stride;

        if (hle_d3d8_interp_rec && indices && vertices && stride &&
            ibytes <= DEFER_LIMIT && vbytes <= DEFER_LIMIT) {
            dq_draw_indexed_up p;
            HleInterpDrawKey k;

            draw_key(&k, (DWORD)type, prims, stride, (DWORD)index_format);
            k.content = hash_bytes(hash_bytes(0xCBF29CE484222325ull, indices, (size_t)ibytes),
                                   vertices, (size_t)vbytes);
            p.type = (DWORD)type;
            p.min_index = min_index;
            p.num_vertices = num_vertices;
            p.prims = prims;
            p.stride = stride;
            p.index_format = (DWORD)index_format;
            p.index_bytes = (uint32_t)ibytes;
            retain_call(op_draw_indexed_up, &p, sizeof p, indices, (size_t)ibytes,
                        vertices, (size_t)vbytes, &k);
        }
        return hr;
    }
}

HRESULT host_CreateTexture(IDirect3DDevice8 *dev, UINT width, UINT height,
                           UINT levels, DWORD usage, D3DFORMAT format,
                           D3DPOOL pool, IDirect3DTexture8 **texture)
{
    return dev->lpVtbl->CreateTexture(dev, width, height, levels, usage, format,
                                      pool, texture);
}

HRESULT host_CreateCubeTexture(IDirect3DDevice8 *dev, UINT edge, UINT levels,
                               DWORD usage, D3DFORMAT format, D3DPOOL pool,
                               IDirect3DCubeTexture8 **texture)
{
    return dev->lpVtbl->CreateCubeTexture(dev, edge, levels, usage, format,
                                          pool, texture);
}

HRESULT host_CubeLockRect(IDirect3DCubeTexture8 *cube, D3DCUBEMAP_FACES face,
                          UINT level, D3DLOCKED_RECT *locked)
{
    return cube->lpVtbl->LockRect(cube, face, level, locked, NULL, 0);
}

HRESULT host_CubeUnlockRect(IDirect3DCubeTexture8 *cube, D3DCUBEMAP_FACES face,
                            UINT level)
{
    HRESULT hr = cube->lpVtbl->UnlockRect(cube, face, level);
    IDirect3DBaseTexture8 *object = (IDirect3DBaseTexture8 *)cube;
    int i;

    /* As host_UnlockRect: a cube the capture holds already gets the level
     * again; one it does not hold is written whole when first bound. */
    if (g_cap && SUCCEEDED(hr) && (i = texture_find(object)) >= 0) {
        cube_level_write(object, g_textures[i].id, (UINT)face, level);
        g_level_writes++;
    }
    return hr;
}

typedef struct { DWORD stage; int null_entries; DWORD entries[256]; } dq_set_palette;

static void op_set_palette(const void *arg)
{
    const dq_set_palette *p = (const dq_set_palette *)arg;
    host_SetPalette(hle_d3d8_shadow_device(), p->stage,
                    p->null_entries ? NULL : p->entries);
}

HRESULT host_SetPalette(IDirect3DDevice8 *dev, DWORD stage, const DWORD *entries)
{
    HRESULT hr;

    if (hle_d3d8_defer_recording()) {
        dq_set_palette p;

        p.stage = stage;
        p.null_entries = entries == NULL;
        if (entries)
            memcpy(p.entries, entries, sizeof p.entries);
        else
            memset(p.entries, 0, sizeof p.entries);
        hle_d3d8_defer_op(op_set_palette, &p, sizeof p);
        return S_OK;
    }
    hr = dev->lpVtbl->SetPalette(dev, stage, entries);
    /* What the device holds now -- its grey ramp for NULL -- rather than the
     * argument, so replay needs no knowledge of the default. */
    if (g_cap && SUCCEEDED(hr) && stage < CAPTURE_STAGES)
        rec_palette(stage, d3d8_GetPalette(stage));
    return hr;
}

HRESULT host_LockRect(IDirect3DTexture8 *texture, UINT level,
                      D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags)
{
    return texture->lpVtbl->LockRect(texture, level, locked, rect, flags);
}

HRESULT host_UnlockRect(IDirect3DTexture8 *texture, UINT level)
{
    HRESULT hr = texture->lpVtbl->UnlockRect(texture, level);
    IDirect3DBaseTexture8 *object = (IDirect3DBaseTexture8 *)texture;
    int i;

    /* A texture the capture does not hold yet is written whole when it is
     * first bound, with whatever this unlock left in it. */
    if (g_cap && SUCCEEDED(hr) && (i = texture_find(object)) >= 0) {
        D3D8CapTextureLevel c;
        const BYTE *bits;
        UINT pitch, rows;

        if (d3d8_texture_level(object, level, &bits, &pitch, &rows)) {
            c.id    = g_textures[i].id;
            c.level = level;
            c.pitch = pitch;
            c.rows  = rows;
            c.bytes = pitch * rows;
            chunk(D3D8CAP_TEXTURE_LEVEL, &c, sizeof c, bits, c.bytes, NULL, 0);
            g_level_writes++;
            g_texture_bytes += c.bytes;
        }
    }
    return hr;
}

static void op_release_texture(const void *arg)
{
    host_ReleaseTexture(*(IDirect3DTexture8 *const *)arg);
}

ULONG host_ReleaseTexture(IDirect3DTexture8 *texture)
{
    IDirect3DBaseTexture8 *object = (IDirect3DBaseTexture8 *)texture;

    /* Queued draws may still name it: released where the queue reaches. */
    if (hle_d3d8_defer_recording()) {
        hle_d3d8_defer_op(op_release_texture, &texture, sizeof texture);
        return 0;
    }
    /* Likewise the frame frame interpolation keeps: released once it will
     * not be drawn again. */
    if (hle_d3d8_interp_rec) {
        hle_d3d8_interp_retire(op_release_texture, &texture, sizeof texture);
        return 0;
    }
    ULONG left = texture->lpVtbl->Release(texture);
    int i;

    /* Only the pointer is used from here on, as a key: the object may be gone. */
    if (left == 0 && (IDirect3DBaseTexture8 *)texture == g_target_texture) {
        g_target_texture = NULL;
        g_target_lost = 1;
    }
    if (g_cap && left == 0 && (i = texture_find(object)) >= 0) {
        D3D8CapTextureId c;

        c.id = g_textures[i].id;
        chunk(D3D8CAP_TEXTURE_RELEASE, &c, sizeof c, NULL, 0, NULL, 0);
        g_textures[i] = g_textures[--g_texture_count];
    }
    return left;
}

/* ------------------------------------------------------- render targets */

typedef struct {
    IDirect3DBaseTexture8 *texture;
    UINT level, face;
    IDirect3DSurface8 *depth;
} dq_render_target;

static void op_render_target(const void *arg)
{
    const dq_render_target *p = (const dq_render_target *)arg;

    if (FAILED(host_SetRenderTarget(hle_d3d8_shadow_device(), p->texture, p->level,
                                    p->face, p->depth))) {
        /* Queued, the caller was told it worked and cannot take its
         * fallback (a scratch target, then the back buffer). Say so. */
        static int said;
        if (!said++) {
            fprintf(stderr, "[HLE-D3D8] deferred frames: the host refused a render "
                    "target; the draws aimed at it go where the last target was\n");
            fflush(stderr);
        }
    }
}

HRESULT host_SetRenderTarget(IDirect3DDevice8 *dev, IDirect3DBaseTexture8 *texture,
                             UINT level, UINT face, IDirect3DSurface8 *depth)
{
    IDirect3DSurface8 *surface = NULL;
    D3D8CubeInfo cube;
    HRESULT hr;

    if (hle_d3d8_defer_recording()) {
        dq_render_target p;
        p.texture = texture;
        p.level = level;
        p.face = face;
        p.depth = depth;
        hle_d3d8_defer_op(op_render_target, &p, sizeof p);
        return S_OK;
    }

    if (texture && d3d8_cube_info(texture, &cube)) {
        IDirect3DCubeTexture8 *c = (IDirect3DCubeTexture8 *)texture;

        hr = c->lpVtbl->GetCubeMapSurface(c, (D3DCUBEMAP_FACES)face, level, &surface);
        if (FAILED(hr) || !surface)
            return FAILED(hr) ? hr : E_FAIL;
    } else if (texture) {
        IDirect3DTexture8 *t = (IDirect3DTexture8 *)texture;

        hr = t->lpVtbl->GetSurfaceLevel(t, level, &surface);
        if (FAILED(hr) || !surface)
            return FAILED(hr) ? hr : E_FAIL;
    }
    hr = dev->lpVtbl->SetRenderTarget(dev, surface, depth);
    if (surface)
        surface->lpVtbl->Release(surface);   /* the device holds its own reference */
    /* The call is recorded as made either way: replay repeats it, and a
     * refusal with it. */
    g_target_texture = texture;
    g_target_level   = level;
    g_target_face    = face;
    g_target_depth   = depth;
    g_target_lost    = FAILED(hr);
    if (hle_d3d8_interp_rec) {
        dq_render_target p;
        p.texture = texture;
        p.level = level;
        p.face = face;
        p.depth = depth;
        hle_d3d8_interp_op(op_render_target, &p, sizeof p);
    }
    if (g_cap)
        rec_set_render_target(texture, level, face, depth, 0);
    return hr;
}

IDirect3DSurface8 *host_DeviceDepthSurface(IDirect3DDevice8 *dev)
{
    IDirect3DSurface8 *s = NULL;

    if (!g_device_depth && SUCCEEDED(dev->lpVtbl->GetDepthStencilSurface(dev, &s)) && s) {
        g_device_depth = s;
        g_target_depth = s;              /* what the device starts with */
        s->lpVtbl->Release(s);           /* the device keeps it alive */
    }
    return g_device_depth;
}

HRESULT host_CreateDepthStencilSurface(IDirect3DDevice8 *dev, UINT width, UINT height,
                                       D3DFORMAT format, IDirect3DSurface8 **surface)
{
    return dev->lpVtbl->CreateDepthStencilSurface(dev, width, height, format,
                                                  D3DMULTISAMPLE_NONE, surface);
}

/* -------------------------------------------- vertex programs, combiners */

HRESULT host_vsh_create_shader(const DWORD *microcode, int insn_count, DWORD *handle)
{
    HRESULT hr = d3d8_vsh_create_shader(microcode, insn_count, handle);

    /* Only a program that exists can be selected, so only those are kept.
     * The host truncates past NV2A_VS_MAX_INSTRUCTIONS; so does the record. */
    if (g_cap && SUCCEEDED(hr)) {
        if (insn_count > NV2A_VS_MAX_INSTRUCTIONS)
            insn_count = NV2A_VS_MAX_INSTRUCTIONS;
        rec_vs_create(*handle, microcode, insn_count);
    }
    return hr;
}

BOOL host_vsh_same_microcode(DWORD handle, const DWORD *microcode, int insn_count)
{
    const DWORD *have;
    int length;

    if (insn_count > NV2A_VS_MAX_INSTRUCTIONS)
        insn_count = NV2A_VS_MAX_INSTRUCTIONS;
    if (!d3d8_vsh_get_slot((int)(handle - 0x10000), NULL, &have, &length, NULL, NULL))
        return FALSE;
    return length == insn_count &&
           memcmp(have, microcode, (size_t)insn_count * 4 * sizeof(DWORD)) == 0;
}

static void op_vsh_delete(const void *arg)
{
    host_vsh_delete_shader(*(const DWORD *)arg);
}

HRESULT host_vsh_delete_shader(DWORD handle)
{
    if (hle_d3d8_defer_recording()) {
        hle_d3d8_defer_op(op_vsh_delete, &handle, sizeof handle);
        return S_OK;
    }
    if (hle_d3d8_interp_rec) {
        hle_d3d8_interp_retire(op_vsh_delete, &handle, sizeof handle);
        return S_OK;
    }
    if (g_cap) {
        D3D8CapVsHandle c;

        c.handle = handle;
        chunk(D3D8CAP_VS_DELETE, &c, sizeof c, NULL, 0, NULL, 0);
    }
    return d3d8_vsh_delete_shader(handle);
}

typedef struct { int first, count; } dq_constants;

static void op_vsh_constants(const void *arg)
{
    const dq_constants *p = (const dq_constants *)arg;
    host_vsh_set_constant(p->first, (const float *)DEFER_PART_A(arg, dq_constants),
                          p->count);
}

typedef struct { int enabled; float scale[4], offset[4]; } dq_screenspace;

static void op_vsh_screenspace(const void *arg)
{
    const dq_screenspace *p = (const dq_screenspace *)arg;
    host_vsh_set_screenspace(p->enabled ? p->scale : NULL, p->enabled ? p->offset : NULL);
}

typedef struct { int reg; float v[4]; } dq_vertex_data;

static void op_vsh_vertex_data(const void *arg)
{
    const dq_vertex_data *p = (const dq_vertex_data *)arg;
    host_vsh_set_vertex_data(p->reg, p->v);
}

static void op_ps_token(const void *arg)
{
    host_combiners_set_pixel_shader(*(const DWORD *)arg);
}

void host_vsh_set_constant(int first_reg, const float *data, int count)
{
    if (hle_d3d8_defer_recording() && data && count > 0) {
        dq_constants p;
        p.first = first_reg;
        p.count = count;
        defer_call(op_vsh_constants, &p, sizeof p, data,
                   (size_t)count * 4 * sizeof(float), NULL, 0);
        return;
    }
    if (data && first_reg >= 0 && first_reg < NV2A_VS_MAX_CONSTANTS && count > 0) {
        int n = count > NV2A_VS_MAX_CONSTANTS - first_reg
                    ? NV2A_VS_MAX_CONSTANTS - first_reg : count;

        memcpy(&g_constants[first_reg * 4], data, (size_t)n * 4 * sizeof(float));
        if (hle_d3d8_interp_rec) {
            dq_constants p;
            p.first = first_reg;
            p.count = n;
            retain_call(op_vsh_constants, &p, sizeof p, data,
                        (size_t)n * 4 * sizeof(float), NULL, 0, NULL);
        }
    }
    if (g_cap)
        rec_vs_constants(first_reg, data, count);
    d3d8_vsh_set_constant(first_reg, data, count);
}

/* A program's vertex declaration -- where in the vertex each input register
 * reads -- is state like any other, and kept for frame interpolation:
 * Future Perfect draws one program with up to four layouts in a frame (272
 * changes in a gameplay frame), so a frame drawn again with each program's
 * last layout read most of its world through the wrong bytes, and the world
 * went missing from the in-between frames while the sky, drawn first,
 * stayed. */
typedef struct {
    DWORD        handle;
    int          count;
    D3D8VshInput inputs[NV2A_VS_MAX_INPUTS];
} dq_declaration;

static void op_vsh_declaration(const void *arg)
{
    const dq_declaration *p = (const dq_declaration *)arg;
    host_vsh_set_declaration(p->handle, p->count ? p->inputs : NULL, p->count);
}

static void keep_declaration(DWORD handle, const D3D8VshInput *inputs, int count)
{
    dq_declaration p;

    if (count < 0 || count > NV2A_VS_MAX_INPUTS || (count && !inputs))
        return;
    p.handle = handle;
    p.count = count;
    if (count)
        memcpy(p.inputs, inputs, (size_t)count * sizeof *inputs);
    hle_d3d8_interp_op(op_vsh_declaration, &p,
                       offsetof(dq_declaration, inputs) + (size_t)count * sizeof *inputs);
}

HRESULT host_vsh_set_declaration(DWORD handle, const D3D8VshInput *inputs, int count)
{
    if (hle_d3d8_interp_rec)
        keep_declaration(handle, inputs, count);
    if (g_cap)
        rec_vs_declaration(handle, inputs, count);
    return d3d8_vsh_set_declaration(handle, inputs, count);
}

void host_vsh_set_screenspace(const float scale[4], const float offset[4])
{
    if (hle_d3d8_defer_recording()) {
        dq_screenspace p;
        memset(&p, 0, sizeof p);
        p.enabled = scale && offset;
        if (p.enabled) {
            memcpy(p.scale, scale, sizeof p.scale);
            memcpy(p.offset, offset, sizeof p.offset);
        }
        hle_d3d8_defer_op(op_vsh_screenspace, &p, sizeof p);
        return;
    }
    if (hle_d3d8_interp_rec) {
        dq_screenspace p;
        memset(&p, 0, sizeof p);
        p.enabled = scale && offset;
        if (p.enabled) {
            memcpy(p.scale, scale, sizeof p.scale);
            memcpy(p.offset, offset, sizeof p.offset);
        }
        hle_d3d8_interp_op(op_vsh_screenspace, &p, sizeof p);
    }
    if (g_cap && scale && offset)
        rec_vs_screenspace(1, scale, offset);
    d3d8_vsh_set_screenspace(scale, offset);
}

void host_vsh_set_vertex_data(int reg, const float value[4])
{
    if (hle_d3d8_defer_recording() && value) {
        dq_vertex_data p;
        p.reg = reg;
        memcpy(p.v, value, sizeof p.v);
        hle_d3d8_defer_op(op_vsh_vertex_data, &p, sizeof p);
        return;
    }
    if (hle_d3d8_interp_rec && value) {
        dq_vertex_data p;
        p.reg = reg;
        memcpy(p.v, value, sizeof p.v);
        hle_d3d8_interp_op(op_vsh_vertex_data, &p, sizeof p);
    }
    if (g_cap && value)
        rec_vs_vertex_data(reg, value);
    d3d8_vsh_set_vertex_data(reg, value);
}

void host_combiners_set_pixel_shader(DWORD token)
{
    if (hle_d3d8_defer_recording()) {
        hle_d3d8_defer_op(op_ps_token, &token, sizeof token);
        return;
    }
    if (hle_d3d8_interp_rec)
        hle_d3d8_interp_op(op_ps_token, &token, sizeof token);
    if (g_cap)
        rec_ps_token(token);
    d3d8_combiners_set_pixel_shader(token);
}

/* ----------------------------------------------------------- screen copies */

typedef struct { IDirect3DTexture8 *dst; int has_rect; RECT src; POINT at; } dq_screen_copy;

static void op_screen_copy(const void *arg)
{
    const dq_screen_copy *p = (const dq_screen_copy *)arg;

    if (p->has_rect)
        host_CopyBackBufferRectToTexture(p->dst, &p->src, &p->at);
    else
        host_CopyBackBufferToTexture(p->dst);
}

HRESULT host_CopyBackBufferToTexture(IDirect3DTexture8 *dst)
{
    if (hle_d3d8_interp_rec) {
        dq_screen_copy p;
        memset(&p, 0, sizeof p);
        p.dst = dst;
        hle_d3d8_interp_op(op_screen_copy, &p, sizeof p);
    }
    return xbox_D3D8CopyBackBufferToTexture(dst);
}

HRESULT host_CopyBackBufferRectToTexture(IDirect3DTexture8 *dst, const RECT *src,
                                         const POINT *at)
{
    if (hle_d3d8_interp_rec && src) {
        dq_screen_copy p;
        memset(&p, 0, sizeof p);
        p.dst = dst;
        p.has_rect = 1;
        p.src = *src;
        if (at) {
            p.at = *at;
        } else {
            p.at.x = 0;
            p.at.y = 0;
        }
        hle_d3d8_interp_op(op_screen_copy, &p, sizeof p);
    }
    return xbox_D3D8CopyBackBufferRectToTexture(dst, src, at);
}

/* --------------------------------------------- the frame-start state, kept
 *
 * capture_snapshot's list, as ops, in the same order and for the same
 * reason (SetTexture rewrites COLOROP, so the stage states come after the
 * textures). The constants are the ones src/hle gave, not the host's
 * scaled copy: these go through the setter again. */
void hle_d3d8_interp_snapshot(void)
{
    static const DWORD transforms[] = {
        D3DTS_VIEW, D3DTS_PROJECTION,
        D3DTS_TEXTURE0, D3DTS_TEXTURE0 + 1, D3DTS_TEXTURE0 + 2, D3DTS_TEXTURE0 + 3,
        D3DTS_WORLD, D3DTS_WORLD + 1, D3DTS_WORLD + 2, D3DTS_WORLD + 3
    };
    IDirect3DDevice8 *dev = hle_d3d8_shadow_device();
    const DWORD *rs = d3d8_GetRenderStates();
    D3DVIEWPORT8 vp;
    DWORD s, t;
    int i;

    if (!dev || !hle_d3d8_interp_rec)
        return;
    for (i = 0; i < NV2A_VS_MAX_SLOTS; i++) {
        const DWORD *microcode;
        const D3D8VshInput *decl;
        int length, decl_count;
        DWORD handle;

        if (d3d8_vsh_get_slot(i, &handle, &microcode, &length, &decl, &decl_count))
            keep_declaration(handle, decl, decl_count);
    }
    {
        dq_constants p;
        p.first = 0;
        p.count = NV2A_VS_MAX_CONSTANTS;
        retain_call(op_vsh_constants, &p, sizeof p, g_constants, sizeof g_constants,
                    NULL, 0, NULL);
    }
    {
        dq_screenspace p;
        memset(&p, 0, sizeof p);
        d3d8_vsh_get_screenspace(p.scale, p.offset, &p.enabled);
        hle_d3d8_interp_op(op_vsh_screenspace, &p, sizeof p);
    }
    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        dq_vertex_data p;
        p.reg = i;
        d3d8_vsh_get_vertex_data(i, p.v);
        hle_d3d8_interp_op(op_vsh_vertex_data, &p, sizeof p);
    }
    {
        DWORD token = d3d8_combiners_get_pixel_shader();
        hle_d3d8_interp_op(op_ps_token, &token, sizeof token);
    }
    {
        DWORD vs = 0;
        dev->lpVtbl->GetVertexShader(dev, &vs);
        hle_d3d8_interp_op(op_vertex_shader, &vs, sizeof vs);
    }
    for (s = 0; s < CAPTURE_STAGES; s++) {
        dq_set_texture p;
        p.stage = s;
        p.texture = d3d8_GetStageTexture(s);
        hle_d3d8_interp_op(op_host_set_texture, &p, sizeof p);
    }
    {
        dq_render_target p;
        p.texture = g_target_lost ? NULL : g_target_texture;
        p.level = g_target_lost ? 0 : g_target_level;
        p.face = g_target_lost ? 0 : g_target_face;
        p.depth = g_target_depth;
        hle_d3d8_interp_op(op_render_target, &p, sizeof p);
    }
    for (i = 0; i < (int)(sizeof transforms / sizeof transforms[0]); i++) {
        const D3DMATRIX *m = d3d8_GetTransform((D3DTRANSFORMSTATETYPE)transforms[i]);
        if (m) {
            dq_transform p;
            p.state = transforms[i];
            p.m = *m;
            hle_d3d8_interp_op(op_transform, &p, sizeof p);
        }
    }
    hle_d3d8_interp_op(op_material, d3d8_GetMaterial(), sizeof(D3DMATERIAL8));
    for (i = 0; i < (int)d3d8_GetNumLights(); i++) {
        const D3DLIGHT8 *l = d3d8_GetLight((DWORD)i);
        dq_light_enable e;
        if (l) {
            dq_light p;
            p.index = (DWORD)i;
            p.light = *l;
            hle_d3d8_interp_op(op_light, &p, sizeof p);
        }
        e.index = (DWORD)i;
        e.enable = d3d8_GetLightEnable((DWORD)i);
        hle_d3d8_interp_op(op_light_enable, &e, sizeof e);
    }
    dev->lpVtbl->GetViewport(dev, &vp);
    hle_d3d8_interp_op(op_viewport, &vp, sizeof vp);
    {
        UINT count; BOOL exclusive; D3DRECT rect;
        dq_scissors p;
        xbox_D3D8GetScissors(&count, &exclusive, &rect);
        /* The host keeps one rectangle; more than one it applies as none,
         * which is what no rectangle says. */
        p.count = count == 1 ? 1 : 0;
        p.exclusive = exclusive;
        retain_call(op_scissors, &p, sizeof p, &rect, p.count * sizeof rect,
                    NULL, 0, NULL);
    }
    {
        dq_two_d_placement p;
        p.placement = xbox_D3D8GetTwoDPlacement(&p.tag);
        hle_d3d8_interp_op(op_two_d_placement, &p, sizeof p);
    }
    for (s = 0; rs && s < CAPTURE_RENDER_STATES; s++) {
        dq_3 p = { s, rs[s], 0 };
        hle_d3d8_interp_op(op_render_state, &p, sizeof p);
    }
    for (s = 0; s < CAPTURE_STAGES; s++) {
        const DWORD *tss = d3d8_GetTSS(s);
        for (t = 0; tss && t < CAPTURE_STAGE_STATES; t++) {
            dq_3 p = { s, t, tss[t] };
            hle_d3d8_interp_op(op_stage_state, &p, sizeof p);
        }
    }
}

#endif /* _WIN32 */
