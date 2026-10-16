/*
 * d3d8_display.c -- how big the host renders, and the resolve that puts
 * that image on the screen. See d3d8_display.h for why this works at all.
 *
 * The resolve is a box filter: at scale N every output pixel averages the
 * NxN scene pixels it was rasterised from, which is ordered-grid
 * supersampling and antialiases everything the scene contains --
 * geometry edges, alpha-tested cutouts and the title's own 2D layer
 * alike. The tap count is baked into the shader at compile time rather
 * than read from a constant, so the loop unrolls; the scale is fixed for
 * the life of the process, so there is nothing to recompile for.
 */
#include "d3d8_display.h"
#include "d3d8_vsh.h"        /* d3d8_vsh_title_widescreen: Hor+ stands aside */
#include "recomp_config.h"

#if defined(_WIN32)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define D3D8_DISPLAY_MAX_SCALE 8

/* ================================================================
 * Policy
 * ================================================================ */

static D3D8DisplayPolicy g_policy;
static int               g_policy_read;

const D3D8DisplayPolicy *d3d8_display_policy(void)
{
    if (!g_policy_read) {
        const char *v = recomp_config_lookup("RECOMP_RES_SCALE", "resolution_scale");
        long n = 1;


        g_policy_read = 1;
        g_policy.scale = 1;

        if (v && *v) {
            char *endp = NULL;
            n = strtol(v, &endp, 10);
            if (endp == v || (endp && *endp) || n < 1 || n > D3D8_DISPLAY_MAX_SCALE) {
                fprintf(stderr, "D3D8 display: RECOMP_RES_SCALE=%s is not a whole "
                        "number from 1 to %d; rendering at the guest's own size\n",
                        v, D3D8_DISPLAY_MAX_SCALE);
                n = 1;
            }
            g_policy.scale = (UINT)n;
        }

        g_policy.anisotropy = 1;
        v = recomp_config_lookup("RECOMP_ANISO", "anisotropy");
        if (v && *v) {
            long a = strtol(v, NULL, 10);

            if (a < 1 || a > 16) {
                fprintf(stderr, "D3D8 display: RECOMP_ANISO=%s is not a whole number "
                        "from 1 to 16; leaving the title's own filtering\n", v);
                a = 1;
            }
            g_policy.anisotropy = (UINT)a;
        }

        if (g_policy.scale > 1)
            fprintf(stderr, "D3D8 display: supersampling %ux, box filtered at present\n",
                    g_policy.scale);
        g_policy.widescreen = recomp_config_bool("RECOMP_WIDESCREEN", "widescreen", 0);

        v = recomp_config_lookup("RECOMP_WIDESCREEN_2D", "widescreen_2d");
        g_policy.centre_2d = v && (strcmp(v, "centre") == 0 || strcmp(v, "center") == 0);
        g_policy.native_2d = v && strcmp(v, "native") == 0;
        if (g_policy.native_2d)
            d3d8_vsh_title_widescreen(1);
        if (v && *v && !g_policy.centre_2d && !g_policy.native_2d && strcmp(v, "auto") != 0)
            fprintf(stderr, "D3D8 display: RECOMP_WIDESCREEN_2D=%s is none of auto, "
                    "centre and native; using auto\n", v);

        if (g_policy.widescreen)
            fprintf(stderr, "D3D8 display: widescreen, so the title's frame is presented "
                    "at 16:9. A title with no 16:9 mode of its own draws 4:3 and will "
                    "look stretched; leave this off for those.%s\n",
                    g_policy.centre_2d ? " All 2D but whole-screen passes is kept at 4:3."
                                       : "");
        if (g_policy.anisotropy > 1)
            fprintf(stderr, "D3D8 display: anisotropic filtering forced to %ux where the "
                    "title filters linearly\n", g_policy.anisotropy);
    }
    return &g_policy;
}

/* Widescreen by screen. A 4:3-only title that has been widened can have
 * screens that should not be: TimeSplitters 2's front end is laid out
 * for 4:3 and mixes 3D backdrops with 2D panels, so neither stretching
 * it nor squeezing its 2D leaves it right, while its levels are fine at
 * 16:9. The title project knows which screen it is on and says so between
 * frames; the frames it calls 4:3 are shown between bars, their 2D left
 * alone. */
static int      g_title_narrow;
static int      g_title_said, g_last_wide = -1;
static unsigned g_frames;

void xbox_D3D8SetWideFrames(BOOL wide)
{
    g_title_said = 1;
    g_title_narrow = wide ? 0 : 1;
}

/* The device made its targets again at a new scale (xbox_D3D8RequestScale). */
void d3d8_display_set_scale(UINT scale)
{
    d3d8_display_policy();                  /* read the rest first */
    if (scale >= 1 && scale <= D3D8_DISPLAY_MAX_SCALE)
        g_policy.scale = scale;
}

void xbox_D3D8SetAnisotropy(UINT level)
{
    d3d8_display_policy();                  /* read the rest first */
    if (level < 1) level = 1;
    if (level > 16) level = 16;
    g_policy.anisotropy = level;
}

int d3d8_display_wide_now(void)
{
    return d3d8_display_policy()->widescreen && !g_title_narrow;
}

/* Set at CreateDevice from the title's own presentation flags. TimeSplitters:
 * Future Perfect reads the console's widescreen flag (XGetVideoFlags at
 * 0x0037D45F, from its device set-up at 0x00023A91), and when it is set it
 * adds D3DPRESENTFLAG_WIDESCREEN, sets its aspect to 16:9 and stores a 0.75
 * squeeze that its camera, HUD and menus all read. Treated as a 4:3-only
 * title, its full-screen colour grading -- which redraws the whole frame
 * from a copy of it -- was squeezed into the middle three quarters: a
 * narrowed copy of the picture over the ungraded frame, which showed at the
 * sides. A title that says it draws 16:9 is taken at its word. */
static int g_title_native_wide;

void xbox_D3D8SetTitleWidescreen(BOOL native)
{
    g_title_native_wide = native ? 1 : 0;
    d3d8_vsh_title_widescreen(d3d8_display_title_native_wide());
    if (native && d3d8_display_policy()->widescreen)
        fprintf(stderr, "D3D8 display: the title draws its own 16:9 frame "
                "(D3DPRESENTFLAG_WIDESCREEN), so it is only stretched at present: "
                "no 2D is squeezed and Hor+ is not applied\n");
}

int d3d8_display_title_native_wide(void)
{
    return g_title_native_wide || d3d8_display_policy()->native_2d;
}

/* Filling the window, whatever its shape -- 21:9, 32:9, 16:10 -- rather
 * than a 16:9 picture between bars. Only for a title whose project can make
 * the game draw that shape itself (its camera, culling and 2D layout), so it
 * says so (xbox_D3D8SetTitleAnyAspect) and then follows the window's shape
 * (xbox_D3D8FillShape). The player's switch is the settings file's
 * wide_fill (RECOMP_WIDE_FILL), live from the in-game menu. */
static volatile LONG g_title_any_aspect;
static volatile LONG g_wide_fill = -1;

void xbox_D3D8SetTitleAnyAspect(BOOL on)
{
    InterlockedExchange(&g_title_any_aspect, on ? 1 : 0);
}

BOOL xbox_D3D8TitleAnyAspect(void)
{
    return g_title_any_aspect != 0;
}

void xbox_D3D8SetWideFill(BOOL on)
{
    InterlockedExchange(&g_wide_fill, on ? 1 : 0);
}

BOOL xbox_D3D8WideFill(void)
{
    if (g_wide_fill < 0)
        InterlockedExchange(&g_wide_fill,
                            recomp_config_bool("RECOMP_WIDE_FILL", "wide_fill", 0) ? 1 : 0);
    return g_wide_fill != 0;
}

int d3d8_display_fill_now(void)
{
    return g_title_any_aspect && d3d8_display_wide_now() && xbox_D3D8WideFill();
}

void d3d8_display_frame_done(void)
{
    int wide = d3d8_display_wide_now();

    g_frames++;
    if (g_title_said && d3d8_display_policy()->widescreen && wide != g_last_wide) {
        fprintf(stderr, "D3D8 display: frame %u on at %s\n", g_frames,
                wide ? "16:9" : "4:3");
        g_last_wide = wide;
    }
}

void d3d8_display_scene_size(UINT guest_w, UINT guest_h,
                             UINT *scene_w, UINT *scene_h)
{
    const D3D8DisplayPolicy *p = d3d8_display_policy();

    if (scene_w) *scene_w = guest_w * p->scale;
    if (scene_h) *scene_h = guest_h * p->scale;
}

void d3d8_display_output_shape(UINT scene_w, UINT scene_h,
                               UINT *shape_w, UINT *shape_h)
{
    if (d3d8_display_fill_now()) {
        /* The title drew the window's own shape: no bars at all. */
        if (shape_w) *shape_w = 0;
        if (shape_h) *shape_h = 0;
    } else if (d3d8_display_wide_now()) {
        /* The frame is anamorphic: the title squeezed a 16:9 view into
         * whatever buffer it renders to, and the display is meant to
         * stretch it back. Presenting it at its own shape would be the
         * squeeze left in. */
        if (shape_w) *shape_w = 16;
        if (shape_h) *shape_h = 9;
    } else {
        if (shape_w) *shape_w = scene_w;
        if (shape_h) *shape_h = scene_h;
    }
}

D3D8DisplayFit d3d8_display_fit(UINT shape_w, UINT shape_h, UINT bb_w, UINT bb_h)
{
    D3D8DisplayFit f;

    f.x = f.y = 0;
    f.w = bb_w;
    f.h = bb_h;
    if (!shape_w || !shape_h || !bb_w || !bb_h)
        return f;

    /* Compare shapes in integers: shape_w/shape_h against bb_w/bb_h. */
    if ((uint64_t)shape_w * bb_h > (uint64_t)bb_w * shape_h) {
        /* The window is taller than the picture: bars above and below. */
        f.h = (UINT)(((uint64_t)bb_w * shape_h) / shape_w);
        if (!f.h) f.h = 1;
        f.y = (bb_h - f.h) / 2;
    } else if ((uint64_t)shape_w * bb_h < (uint64_t)bb_w * shape_h) {
        /* The window is wider: bars to the left and right. */
        f.w = (UINT)(((uint64_t)bb_h * shape_w) / shape_h);
        if (!f.w) f.w = 1;
        f.x = (bb_w - f.w) / 2;
    }
    return f;
}

/* ================================================================
 * Resolve
 * ================================================================ */

/* origin: where the picture starts inside the back buffer, subtracted
 * because SV_Position counts from the buffer's corner and not the
 * viewport's. ratio: scene texels per output pixel. texel: one over the
 * scene size, for normalised sampling.
 *
 * TAPS x TAPS bilinear samples spread across each output pixel's
 * footprint. Bilinear rather than Load so a ratio that is not a whole
 * number -- which is most window sizes -- still resolves smoothly, and so
 * a window larger than the scene magnifies cleanly instead of blocking
 * up. At a whole-number ratio with taps to match, this is the box filter
 * that makes the supersampling exact.
 *
 * gamma: the player's brightness (xbox_D3D8SetBrightness), as the exponent
 * that applies it -- one over the setting, so a setting above 1 lifts the
 * dark end. Exactly 1 skips the pow, so the picture is bit-for-bit the
 * game's own until someone moves the slider.
 *
 * fxaa: edge smoothing (xbox_D3D8SetFxaa), the compact FXAA -- the luma of a
 * texel's four diagonal neighbours gives the edge's direction, two and four
 * samples along it are blended, and the four-sample blend is kept unless it
 * strays outside the neighbourhood's luma range. On the scene, at the output
 * pixel's centre, so a scene smaller than the window is smoothed and then
 * magnified. Only with one tap: a resolve that averages several is already
 * supersampling. 0 is the plain resolve, bit-for-bit. */
typedef struct {
    float origin_x, origin_y, ratio_x, ratio_y;
    float texel_x, texel_y, inv_taps, gamma;
    float fxaa, pad0, pad1, pad2;
} ResolveConstants;

static const char kSource[] =
    "cbuffer Resolve : register(b7) { float4 p; float4 q; float4 r; };\n"
    "struct VSOut { float4 pos : SV_Position; };\n"
    "VSOut vs_main(uint id : SV_VertexID) {\n"
    "    float2 c = float2((id << 1) & 2, id & 2);\n"   /* one oversized triangle */
    "    VSOut o;\n"
    "    o.pos = float4(c * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    return o;\n"
    "}\n"
    "Texture2D<float4> scene : register(t9);\n"
    "SamplerState smp : register(s9);\n"
    "float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
    "float3 at(float2 uv) { return scene.SampleLevel(smp, uv, 0).rgb; }\n"
    "float4 fxaa(float2 uv, float2 t) {\n"
    "    float4 m = scene.SampleLevel(smp, uv, 0);\n"
    "    float nw = luma(at(uv + float2(-1, -1) * t)), ne = luma(at(uv + float2(1, -1) * t));\n"
    "    float sw = luma(at(uv + float2(-1, 1) * t)), se = luma(at(uv + float2(1, 1) * t));\n"
    "    float lm = luma(m.rgb);\n"
    "    float lo = min(lm, min(min(nw, ne), min(sw, se)));\n"
    "    float hi = max(lm, max(max(nw, ne), max(sw, se)));\n"
    "    float2 dir = float2(-((nw + ne) - (sw + se)), (nw + sw) - (ne + se));\n"
    "    float reduce = max((nw + ne + sw + se) * (0.25 / 8.0), 1.0 / 128.0);\n"
    "    float scale = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);\n"
    "    dir = clamp(dir * scale, -8.0, 8.0) * t;\n"
    "    float3 a = 0.5 * (at(uv + dir * (1.0 / 3.0 - 0.5)) + at(uv + dir * (2.0 / 3.0 - 0.5)));\n"
    "    float3 b = a * 0.5 + 0.25 * (at(uv - dir * 0.5) + at(uv + dir * 0.5));\n"
    "    float lb = luma(b);\n"
    "    return float4((lb < lo || lb > hi) ? a : b, m.a);\n"
    "}\n"
    "float4 ps_main(VSOut i) : SV_Target {\n"
    "    float2 base = (i.pos.xy - p.xy) * p.zw;\n"
    "    float2 step = p.zw / TAPS;\n"
    "    float4 sum = 0;\n"
    "    if (TAPS == 1 && r.x > 0.5) {\n"
    "        sum = fxaa((base + 0.5 * p.zw) * q.xy, q.xy);\n"
    "    } else {\n"
    "        [unroll] for (int y = 0; y < TAPS; y++)\n"
    "            [unroll] for (int x = 0; x < TAPS; x++)\n"
    "                sum += scene.SampleLevel(smp,\n"
    "                           (base + (float2(x, y) + 0.5) * step) * q.xy, 0);\n"
    "        sum *= q.z;\n"
    "    }\n"
    "    if (q.w != 1.0)\n"
    "        sum.rgb = pow(saturate(sum.rgb), q.w);\n"
    "    return sum;\n"
    "}\n";

/* The player's brightness, live (the in-game menu); 0 until the settings
 * have been read. */
static volatile float g_brightness;

void xbox_D3D8SetBrightness(float b)
{
    g_brightness = b < 0.5f ? 0.5f : (b > 2.0f ? 2.0f : b);
}

float xbox_D3D8Brightness(void)
{
    if (g_brightness <= 0.0f) {
        const char *v = recomp_config_lookup("RECOMP_BRIGHTNESS", "brightness");
        xbox_D3D8SetBrightness(v && *v ? (float)atof(v) : 1.0f);
    }
    return g_brightness;
}

/* Edge smoothing, live (the in-game menu); -1 until the settings are read. */
static volatile int g_fxaa = -1;

void xbox_D3D8SetFxaa(BOOL on)
{
    g_fxaa = on ? 1 : 0;
}

BOOL xbox_D3D8Fxaa(void)
{
    if (g_fxaa < 0)
        g_fxaa = recomp_config_bool("RECOMP_FXAA", "fxaa", 0);
    return g_fxaa != 0;
}

static struct {
    RhiShader      *vs;
    RhiShader      *ps;
    RhiBuffer      *cb;
    RhiBlendState  *blend;
    RhiDepthState  *depth;
    RhiRasterState *raster;
    RhiSampler     *sampler;
    UINT            taps;                /* what the shaders were built for */
    int             tried, failed;
} g;

static void fail(const char *what)
{
    g.failed = 1;
    fprintf(stderr, "D3D8 display: %s failed; the scene cannot be "
            "resolved and the window will stay blank\n", what);
    fflush(stderr);
}

static RhiShader *compile_one(uint32_t stage, const char *entry, const char *target,
                              const RhiMacro *macros)
{
    RhiShaderSource src;
    RhiShader *s;
    char err[2048];

    memset(&src, 0, sizeof src);
    src.hlsl = kSource;
    src.len = sizeof kSource - 1;
    src.name = "display_resolve";
    src.macros = macros;
    src.entry = entry;
    src.target = target;
    s = rhi_shader_create(stage, &src, err, sizeof err);
    if (!s) {
        if (err[0])
            fprintf(stderr, "D3D8 display: %s\n", err);
        fail("shader creation");
    }
    return s;
}

static int create(UINT taps)
{
    RhiMacro macros[2];
    char taps_text[16];
    RhiBufferDesc bd;
    RhiBlendDesc bl;
    RhiDepthDesc ds;
    RhiRasterDesc rd;
    RhiSamplerDesc sm;

    if (g.failed) return 0;
    if (g.tried && g.taps == taps) return g.ps != NULL;
    if (!rhi_device_ready()) { g.failed = 1; return 0; }

    /* The tap count follows the window, not the scale: a window of a
     * different shape from the scene gets a different ratio, and one
     * that is resized gets a new one mid-run. Rebuild the two shaders
     * for it and keep everything else. */
    if (g.tried) {
        rhi_shader_destroy(g.vs); g.vs = NULL;
        rhi_shader_destroy(g.ps); g.ps = NULL;
    }
    g.tried = 1;
    g.taps = taps;

    snprintf(taps_text, sizeof taps_text, "%u", taps);
    macros[0].name = "TAPS";
    macros[0].value = taps_text;
    macros[1].name = NULL;
    macros[1].value = NULL;

    if (!(g.vs = compile_one(RHI_STAGE_VERTEX, "vs_main", "vs_4_0", macros))) return 0;
    if (!(g.ps = compile_one(RHI_STAGE_PIXEL, "ps_main", "ps_4_0", macros))) return 0;

    if (g.cb)
        return 1;                       /* rebuild: the rest already exists */

    memset(&bd, 0, sizeof bd);
    bd.size = sizeof(ResolveConstants);
    bd.usage = RHI_USAGE_DYNAMIC;
    bd.bind = RHI_BIND_UNIFORM;
    bd.cpu_access = RHI_CPU_WRITE;
    if (!(g.cb = rhi_buffer_create(&bd, NULL))) { fail("CreateBuffer"); return 0; }

    /* Opaque, depthless, unculled: this pass replaces the target. */
    memset(&bl, 0, sizeof bl);
    bl.write_mask = RHI_WRITE_ALL;
    if (!(g.blend = rhi_blend_state_create(&bl))) { fail("CreateBlendState"); return 0; }

    memset(&ds, 0, sizeof ds);
    if (!(g.depth = rhi_depth_state_create(&ds))) { fail("CreateDepthStencilState"); return 0; }

    memset(&rd, 0, sizeof rd);
    rd.fill = RHI_FILL_SOLID;
    rd.cull = RHI_CULL_NONE;
    rd.depth_clip = 1;
    if (!(g.raster = rhi_raster_state_create(&rd))) { fail("CreateRasterizerState"); return 0; }

    memset(&sm, 0, sizeof sm);
    sm.filter = RHI_FILTER_LINEAR;
    sm.address_u = sm.address_v = sm.address_w = RHI_ADDRESS_CLAMP;
    sm.compare = RHI_CMP_NEVER;
    sm.max_lod = 3.402823466e+38f;          /* D3D11_FLOAT32_MAX */
    if (!(g.sampler = rhi_sampler_create(&sm))) { fail("CreateSamplerState"); return 0; }

    return 1;
}

HRESULT d3d8_display_resolve(RhiView *scene, UINT scene_w, UINT scene_h,
                             RhiView *out, D3D8DisplayFit fit)
{
    RhiOutputState saved;
    RhiViewport vp;
    float blend_factor[4] = { 1, 1, 1, 1 };
    UINT taps;
    void *mapped;

    if (!scene || !out || !fit.w || !fit.h || !scene_h)
        return E_INVALIDARG;

    /* Square taps, from the vertical ratio: the horizontal one is the
     * same whenever the fit preserved the scene's shape, which it does,
     * and averaging a non-square block would soften one axis more than
     * the other. Rounded rather than truncated so a ratio just under a
     * whole number keeps the taps that ratio deserves. */
    taps = (scene_h + fit.h / 2) / fit.h;
    if (taps < 1) taps = 1;
    if (taps > 8) taps = 8;
    if (!create(taps)) return E_FAIL;

    rhi_output_save(&saved);

    if ((mapped = rhi_buffer_map(g.cb, RHI_MAP_WRITE_DISCARD)) != NULL) {
        ResolveConstants c;
        c.origin_x = (float)fit.x;
        c.origin_y = (float)fit.y;
        c.ratio_x = (float)scene_w / (float)fit.w;
        c.ratio_y = (float)scene_h / (float)fit.h;
        c.texel_x = 1.0f / (float)scene_w;
        c.texel_y = 1.0f / (float)scene_h;
        c.inv_taps = 1.0f / (float)(taps * taps);
        c.gamma = 1.0f / xbox_D3D8Brightness();
        c.fxaa = xbox_D3D8Fxaa() ? 1.0f : 0.0f;
        c.pad0 = c.pad1 = c.pad2 = 0.0f;
        memcpy(mapped, &c, sizeof c);
        rhi_buffer_unmap(g.cb);
    }

    vp.x = (float)fit.x;
    vp.y = (float)fit.y;
    vp.width = (float)fit.w;
    vp.height = (float)fit.h;
    vp.min_depth = 0.0f;
    vp.max_depth = 1.0f;
    rhi_set_render_target(out, NULL);
    /* The bars. Cheaper than tracking whether the window changed shape,
     * and it costs one clear of a buffer that is about to be presented. */
    {
        float black[4] = { 0, 0, 0, 1 };
        rhi_clear_color(out, black);
    }
    rhi_set_viewports(1, &vp);
    rhi_set_vertex_layout(NULL);
    rhi_set_topology(RHI_TOPOLOGY_TRIANGLES);
    rhi_set_shader(RHI_STAGE_VERTEX, g.vs);
    rhi_set_shader(RHI_STAGE_PIXEL, g.ps);
    rhi_set_uniform_buffers(RHI_STAGE_PIXEL, 7, 1, &g.cb);
    rhi_set_textures(9, 1, &scene);
    rhi_set_samplers(9, 1, &g.sampler);
    rhi_set_blend_state(g.blend, blend_factor, 0xFFFFFFFF);
    rhi_set_depth_state(g.depth, 0);
    rhi_set_raster_state(g.raster);
    rhi_draw(3, 0);

    /* The scene must not stay bound as a shader input: it is the render
     * target again as soon as the next frame starts. */
    {
        RhiView *none = NULL;
        rhi_set_textures(9, 1, &none);
    }
    rhi_output_restore(&saved);
    return S_OK;
}

void d3d8_display_shutdown(void)
{
    rhi_shader_destroy(g.vs);            g.vs = NULL;
    rhi_shader_destroy(g.ps);            g.ps = NULL;
    rhi_buffer_destroy(g.cb);            g.cb = NULL;
    rhi_blend_state_destroy(g.blend);    g.blend = NULL;
    rhi_depth_state_destroy(g.depth);    g.depth = NULL;
    rhi_raster_state_destroy(g.raster);  g.raster = NULL;
    rhi_sampler_destroy(g.sampler);      g.sampler = NULL;
    g.tried = 0;
    g.failed = 0;
}

#endif /* _WIN32 */
