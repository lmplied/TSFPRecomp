/**
 * D3D8 Compatibility Device Implementation
 *
 * Implements the Xbox D3D8 IDirect3DDevice8 interface on the host GPU
 * through rhi.h. The game's translated code calls D3D8 methods through COM
 * vtables; this layer keeps the D3D8 state, owns the scene target, the
 * device depth buffer and the UP rings, and draws through whichever rhi.h
 * backend is in use (D3D11 on Windows; Vulkan is the portable one).
 *
 * Architecture:
 * - The backend creates the device and swap chain (rhi_device_create)
 * - Render state tracking: D3D8 states mapped to rhi.h state objects
 * - Texture/buffer management: D3D8 resource handles wrap rhi.h objects
 * - Fixed-function pipeline: emulated with generated HLSL shaders (the
 *   Xbox D3D8 pipeline is configurable but not fully programmable)
 */

#include "d3d8_internal.h"
#include "d3d8_display.h"
#include "d3d8_menu.h"         /* the settings menu, drawn after the resolve */
#include "d3d8_fvf.h"         /* d3d8_fvf_transformed: draw_extent */
#include "recomp_config.h"   /* RECOMP_VRR */
#include <stdio.h>
#include <string.h>
/* malloc: without <stdlib.h> its pointer is truncated to int. */
#include <stdlib.h>

/* ================================================================
 * Internal device state
 * ================================================================ */

/* Maximum tracked render states, texture stages, and transforms */
#define MAX_RENDER_STATES    256
#define MAX_TEXTURE_STAGES   4
#define MAX_TSS_STATES       32
#define MAX_TRANSFORMS       512
#define MAX_LIGHTS           8

typedef struct D3D8DeviceState {

    /* The device's own depth buffer and a view of it */
    RhiImage               *depth_image;
    RhiView                *depth_view;

    /* Window */
    HWND                    hwnd;
    /* The size the scene is rendered at. Everything downstream -- the
     * device depth buffer, the default viewport, what clip space maps
     * onto -- follows these, so raising them is the whole of
     * supersampling. */
    UINT                    width;
    UINT                    height;
    /* The swap chain's own size, which is what the guest asked to present
     * at. Equal to width/height while nothing is scaled, and then the
     * scene target is the back buffer itself and no resolve runs. */
    UINT                    present_width;
    UINT                    present_height;
    /* The swap chain's real size, which follows the window. Separate from
     * present_width/height because that is the guest's presentation size
     * and must not move when someone drags a corner: the scene is still
     * rendered for a 640x480 guest, and only where it lands changes. */
    UINT                    swap_width;
    UINT                    swap_height;
    RhiImage                 *rhi_scene_image;  /* what every draw goes to */
    RhiView                  *rhi_scene_srv;    /* the finished frame, to read */
    RhiView                  *rhi_default_rtv;  /* "the back buffer" to a title */
    D3DFORMAT               backbuffer_format;

    /* State tracking */
    DWORD                   render_states[MAX_RENDER_STATES];
    DWORD                   tss[MAX_TEXTURE_STAGES][MAX_TSS_STATES];
    D3DMATRIX               transforms[MAX_TRANSFORMS];
    D3DVIEWPORT8            viewport;
    D3DMATERIAL8            material;
    D3DLIGHT8               lights[MAX_LIGHTS];
    BOOL                    light_enable[MAX_LIGHTS];

    /* Current shader/FVF */
    DWORD                   vertex_shader;
    DWORD                   pixel_shader;

    /* Scene state */
    BOOL                    in_scene;

    /* Reference count */
    LONG                    ref_count;
} D3D8DeviceState;

/* Global device instance (Xbox has a single D3D device) */
static D3D8DeviceState g_device_state;
static IDirect3DDevice8 g_device;
static BOOL g_device_initialized = FALSE;

/* Current resource bindings */
static IDirect3DVertexBuffer8 *g_cur_vb = NULL;
static UINT                    g_cur_vb_stride = 0;
static IDirect3DIndexBuffer8  *g_cur_ib = NULL;
static UINT                    g_cur_ib_base_vertex = 0;
static IDirect3DBaseTexture8  *g_cur_textures[4] = { NULL };

/* Bound offscreen render target / depth stencil (NULL = default) */
static D3D8Surface *g_cur_rt = NULL;

/* Whether draws go to the screen (the device's own back buffer) rather than
 * a texture the title renders into. */
BOOL d3d8_target_is_screen(void)
{
    return g_cur_rt == NULL;
}
static D3D8Surface *g_cur_ds = NULL;
/* The device's own depth buffer as a surface object, so SetRenderTarget can
 * name it: a NULL depth surface means no depth, as in D3D8. */
static D3D8Surface *g_auto_ds = NULL;

/* Palettized texture palettes (256 ARGB entries per stage) */
#define D3D8_PALETTE_ENTRIES 256
static DWORD g_palettes[D3D8_MAX_PALETTES][D3D8_PALETTE_ENTRIES];
static BOOL  g_palettes_initialized = FALSE;

static void d3d8_palettes_init(void)
{
    int p, i;
    if (g_palettes_initialized) return;
    for (p = 0; p < D3D8_MAX_PALETTES; p++)
        for (i = 0; i < D3D8_PALETTE_ENTRIES; i++)
            g_palettes[p][i] = 0xFF000000u | ((DWORD)i * 0x010101u); /* gray ramp */
    g_palettes_initialized = TRUE;
}

void d3d8_SetPalette(DWORD stage, const DWORD *entries)
{
    d3d8_palettes_init();
    if (stage >= D3D8_MAX_PALETTES) return;
    if (entries)
        memcpy(g_palettes[stage], entries, sizeof(g_palettes[stage]));
    else {
        for (int i = 0; i < D3D8_PALETTE_ENTRIES; i++)
            g_palettes[stage][i] = 0xFF000000u | ((DWORD)i * 0x010101u);
    }
}

const DWORD *d3d8_GetPalette(DWORD stage)
{
    d3d8_palettes_init();
    return (stage < D3D8_MAX_PALETTES) ? g_palettes[stage] : g_palettes[0];
}

/* Forward declarations */
static const IDirect3DDevice8Vtbl g_device_vtbl;
static void up_ring_shutdown(void);
static void present_resolve(void);
static void interp_target_destroy(void);
static BOOL g_interp_on;                /* frame interpolation, below */
static void follow_scale(void);         /* xbox_D3D8RequestScale, below */
#include "d3d8_overlay.h"
#include "d3d8_movie.h"

/* ================================================================
 * Public frame pump (called from recompiled game code)
 * ================================================================ */
void d3d8_PresentFrame(void)
{
    /* Pump Windows messages */
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) ExitProcess(0);
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    /* Present the backbuffer (VSync = 1) */
    if (rhi_device_ready()) {
        present_resolve();
        rhi_present(1);
    }
}

/* ================================================================
 * Internal accessors (used by d3d8_resources/shaders/states)
 * ================================================================ */

IDirect3DDevice8    *d3d8_GetDevice(void) { return &g_device; }
UINT d3d8_GetBackBufferWidth(void)  { return g_device_state.width; }
UINT d3d8_GetBackBufferHeight(void) { return g_device_state.height; }
HWND                 d3d8_GetHWND(void) { return g_device_state.hwnd; }
UINT                 d3d8_GetBackbufferWidth(void) { return g_device_state.width; }
UINT                 d3d8_GetBackbufferHeight(void) { return g_device_state.height; }

/* The size the guest believes it presents at, which is what its
 * pre-transformed (XYZRHW) vertices are measured in. The same as the back
 * buffer until a host renders larger than the guest asked; then every
 * screen-space quad must still be divided by the guest's 640x480, or a
 * scaled-up window draws its menus in the top-left quarter. Set by the HLE
 * from the title's own present parameters; unset means the back buffer. */
static UINT g_guest_width, g_guest_height;
void xbox_D3D8SetGuestSize(UINT width, UINT height)
{
    g_guest_width = width;
    g_guest_height = height;
}
/* Falls back to the presentation size rather than the scene size: those
 * were the same thing before the host could render larger, and a caller
 * that never set a guest size wants the smaller of the two -- dividing a
 * screen-space quad by the scene size would shrink it. */
UINT d3d8_GetGuestWidth(void)  { return g_guest_width  ? g_guest_width  : g_device_state.present_width; }

RhiImage *d3d8_GetSceneImage(void)      { return g_device_state.rhi_scene_image; }
RhiView  *d3d8_GetSceneView(void)       { return g_device_state.rhi_scene_srv; }
RhiView  *d3d8_GetDefaultTargetView(void) { return g_device_state.rhi_default_rtv; }

/* Guest render-target pixels to host pixels, for the viewport and the
 * scissor rectangle -- the two pieces of state a title hands us measured
 * in pixels rather than normalised.
 *
 * Only the scene target is scaled today. A title's own offscreen target is
 * still created at the size it asked for, so anything bound there keeps a
 * factor of one; that is what lets render-target scaling be a later step
 * rather than a precondition. */
static float rt_scale_x(void)
{
    if (g_cur_rt || !g_device_state.present_width)
        return 1.0f;
    return (float)g_device_state.width / (float)g_device_state.present_width;
}

static float rt_scale_y(void)
{
    if (g_cur_rt || !g_device_state.present_height)
        return 1.0f;
    return (float)g_device_state.height / (float)g_device_state.present_height;
}

/* xbox_D3D8KeepPresented: the frame as it reaches the swap chain. */
static BOOL     g_keep_presented;
static uint8_t *g_presented;
static UINT     g_presented_w, g_presented_h;

void xbox_D3D8KeepPresented(BOOL on) { g_keep_presented = on; }

const uint8_t *xbox_D3D8Presented(UINT *width, UINT *height)
{
    if (width) *width = g_presented_w;
    if (height) *height = g_presented_h;
    return g_presented;
}

static void keep_presented(void)
{
    uint32_t w = 0, h = 0;
    uint8_t *buf;

    if (!g_keep_presented || rhi_swapchain_readback(NULL, 0, &w, &h) != 0 || !w || !h)
        return;
    buf = (uint8_t *)realloc(g_presented, (size_t)w * h * 4u);
    if (!buf)
        return;
    g_presented = buf;
    if (rhi_swapchain_readback(g_presented, w * 4u, &w, &h) == 0) {
        g_presented_w = w;
        g_presented_h = h;
    }
}

/* xbox_D3D8RequestShot / TakeShot: one frame, for a screenshot. Taken on the
 * presenting thread and handed over by pointer, so the thread that saves it
 * never touches the device. */
static volatile LONG g_shot_wanted;
static uint8_t *volatile g_shot;
static UINT g_shot_w, g_shot_h;

void xbox_D3D8RequestShot(void)
{
    InterlockedExchange(&g_shot_wanted, 1);
}

uint8_t *xbox_D3D8TakeShot(UINT *width, UINT *height)
{
    uint8_t *px = (uint8_t *)InterlockedExchangePointer((void *volatile *)&g_shot, NULL);

    if (px) {
        if (width) *width = g_shot_w;
        if (height) *height = g_shot_h;
    }
    return px;
}

static void take_shot(void)
{
    uint32_t w = 0, h = 0;
    uint8_t *px, *old;

    if (!InterlockedExchange(&g_shot_wanted, 0))
        return;
    if (rhi_swapchain_readback(NULL, 0, &w, &h) != 0 || !w || !h)
        return;
    if (!(px = (uint8_t *)malloc((size_t)w * h * 4u)))
        return;
    if (rhi_swapchain_readback(px, w * 4u, &w, &h) != 0) {
        free(px);
        return;
    }
    /* A shot nobody collected yet makes way for this one. */
    old = (uint8_t *)InterlockedExchangePointer((void *volatile *)&g_shot, NULL);
    free(old);
    g_shot_w = w;
    g_shot_h = h;
    InterlockedExchangePointer((void *volatile *)&g_shot, px);
}

BOOL xbox_D3D8FillShape(UINT *width, UINT *height)
{
    UINT w = g_device_state.swap_width, h = g_device_state.swap_height;

    if (!d3d8_display_fill_now() || !w || !h)
        return FALSE;
    if (width) *width = w;
    if (height) *height = h;
    return TRUE;
}

static void present_scene(void);

/* Every way a frame reaches the screen comes through here: put the scene
 * on it, at the shape d3d8_display_wide_now gives for this frame, then
 * note the frame done. */
static void follow_vrr(void);

static void two_d_tag_frame(void);

static void present_resolve(void)
{
    follow_vrr();
    two_d_tag_frame();
    present_scene();
    d3d8_display_frame_done();
}

/* xbox_D3D8SetFullscreen: set by the window's thread, acted on here, on the
 * thread that presents, because the swap chain is not the window thread's
 * to change. */
static volatile LONG g_fullscreen;

void xbox_D3D8SetFullscreen(BOOL on)
{
    InterlockedExchange(&g_fullscreen, on ? 1 : 0);
}

/* Variable refresh only while the window owns its screen: windowed, the
 * compositor shows frames at the display's fixed rate whatever the swap
 * chain does, and presenting without waiting would only tear. */
static volatile LONG g_vrr_setting = -1;

/* The vrr setting changed while the title runs (the in-game menu). */
void xbox_D3D8SetVrr(BOOL on)
{
    InterlockedExchange(&g_vrr_setting, on ? 1 : 0);
}

static void follow_vrr(void)
{
    static int applied = -1;
    int want;

    if (g_vrr_setting < 0)
        InterlockedExchange(&g_vrr_setting, recomp_config_bool("RECOMP_VRR", "vrr", 1));
    want = g_vrr_setting && InterlockedCompareExchange(&g_fullscreen, 0, 0);
    if (want != applied) {
        applied = want;
        rhi_swapchain_set_vrr(want);
    }
}

/* Set while drawing something positioned in screen coordinates the title
 * worked out itself, rather than through its projection. See
 * d3d8_SetTwoDSqueeze. */
static BOOL g_2d_squeeze;
/* Whether the draw in hand is screen-space at all, widescreen or not: the
 * shader path's verdict before the widescreen gate. For the tag report. */
static BOOL g_2d_screen_space;
/* Where the title says its next screen-space draws belong
 * (xbox_D3D8SetTwoDPlacement), and which of its call sites they come from. */
static int      g_2d_placement = XBOX_D3D8_2D_AUTO;
static uint32_t g_2d_tag;
/* The edge a squeezed draw is anchored to: LEFT, RIGHT or CENTRE. The
 * title's placement for LEFT and RIGHT; chosen per draw for SIDE. */
static int      g_2d_anchor = XBOX_D3D8_2D_CENTRE;

/* The widescreen 2D squeeze (see apply_host_viewport) as a horizontal
 * factor about an anchor, both in host pixels of the scene. FALSE when no
 * squeeze applies to the draw in hand. The anchor is the middle of the
 * picture, or its left or right edge for a draw the title has pinned
 * there: a health bar in a 4:3 corner stays in the 16:9 corner.
 *
 * Everything that positions a 2D draw in pixels has to go through this
 * together. The viewport alone is not enough: a title that clips a menu
 * with a scissor rectangle hands it in the same 4:3 pixels, and a squeezed
 * draw under an unsqueezed scissor comes out cut off at the old edge --
 * TimeSplitters 2's character portraits and difficulty list both were. */
static BOOL two_d_squeeze(float *k_out, float *cx_out)
{
    float k;

    if (!g_2d_squeeze || !g_device_state.height || g_cur_rt)
        return FALSE;
    k = ((float)g_device_state.width * 9.0f) /
        ((float)g_device_state.height * 16.0f);
    if (!(k > 0.0f && k < 1.0f))
        return FALSE;
    *k_out = k;
    if (g_2d_anchor == XBOX_D3D8_2D_LEFT)
        *cx_out = 0.0f;
    else if (g_2d_anchor == XBOX_D3D8_2D_RIGHT)
        *cx_out = (float)g_device_state.width;
    else
        *cx_out = (float)g_device_state.width * 0.5f;
    return TRUE;
}

/* Where the last present put the picture, for xbox_D3D8PictureRect. */
static volatile LONG g_picture_known;
static D3D8DisplayFit g_picture_fit;
static UINT g_picture_out_w, g_picture_out_h;

BOOL xbox_D3D8PictureRect(float *x, float *y, float *w, float *h)
{
    D3D8DisplayFit f = g_picture_fit;
    UINT ow = g_picture_out_w, oh = g_picture_out_h;

    if (!g_picture_known || !ow || !oh || !f.w || !f.h) {
        *x = *y = 0.0f;                 /* the back buffer, stretched to the window */
        *w = *h = 1.0f;
        return FALSE;
    }
    *x = (float)f.x / (float)ow;
    *y = (float)f.y / (float)oh;
    *w = (float)f.w / (float)ow;
    *h = (float)f.h / (float)oh;
    return TRUE;
}

/* Put the scene on the back buffer, immediately before presenting it.
 * Nothing to do while unscaled: the scene target is the back buffer, and
 * this is the one call that has to stay free in that case. */
static void present_scene(void)
{
    D3D8DeviceState *s = &g_device_state;
    D3D8DisplayFit fit;
    RECT rc;

    if (!rhi_swapchain_view() || !s->rhi_scene_srv)
        return;

    /* Follow the window. DXGI stretches the back buffer to the client
     * rectangle whatever shape it is, which is the one way a scene that is
     * right all the way through still reaches the screen distorted. Keep
     * the buffer the size of the window and the stretch is the identity;
     * the fit below then puts bars around the picture instead. */
    if (s->hwnd && GetClientRect(s->hwnd, &rc)) {
        UINT w = (UINT)(rc.right - rc.left), h = (UINT)(rc.bottom - rc.top);

        if (w && h && (w != s->swap_width || h != s->swap_height)) {
            if (rhi_swapchain_resize(w, h) == 0) {
                s->swap_width = w;
                s->swap_height = h;
            }
            if (!rhi_swapchain_view())
                return;                 /* try again on the next frame */
        }
    }

    {
        UINT shape_w, shape_h;

        d3d8_display_output_shape(s->width, s->height, &shape_w, &shape_h);
        fit = d3d8_display_fit(shape_w, shape_h, s->swap_width, s->swap_height);
        g_picture_fit = fit;
        g_picture_out_w = s->swap_width;
        g_picture_out_h = s->swap_height;
        InterlockedExchange(&g_picture_known, 1);
    }
    d3d8_display_resolve(s->rhi_scene_srv, s->width, s->height, rhi_swapchain_view(), fit);
    /* A screenshot is the picture, not the menu over it. */
    take_shot();
    /* The settings menu goes over the finished picture at the window's own
     * size, after the resolve, so it is neither stretched by a widescreen
     * present nor softened by a supersampled one (d3d8_menu.c). */
    d3d8_menu_draw(rhi_swapchain_view(), s->swap_width, s->swap_height);
    keep_presented();
}

/* The scissor rectangle, from the Xbox's D3DDevice_SetScissors. The host
 * keeps one: D3D11 applies one scissor per viewport, and what titles clip
 * with it -- a scrolling text box, a minimap -- is one rectangle. More than
 * one, or an exclusive scissor (draw outside the rectangles), cannot be
 * expressed here and is applied as no scissor, said once. Coordinates are
 * render-target pixels, as on the Xbox; when the host renders larger than
 * the guest they will need scaling, like the viewport. */
static BOOL       g_scissor_enabled;
static RhiRect    g_scissor;
static UINT       g_scissor_count;
static BOOL       g_scissor_exclusive;
static D3DRECT    g_scissor_rect;

void xbox_D3D8SetScissors(UINT count, BOOL exclusive, const D3DRECT *rects)
{
    static int said;

    if (!rects)
        count = 0;
    g_scissor_count = count;
    g_scissor_exclusive = exclusive;
    memset(&g_scissor_rect, 0, sizeof g_scissor_rect);
    if (count)
        g_scissor_rect = rects[0];
    g_scissor_enabled = count >= 1 && !exclusive;
    if (g_scissor_enabled) {
        g_scissor.left   = rects[0].x1;
        g_scissor.top    = rects[0].y1;
        g_scissor.right  = rects[0].x2;
        g_scissor.bottom = rects[0].y2;
    }
    if ((count > 1 || (count && exclusive)) && !said++)
        fprintf(stderr, "D3D8: SetScissors with %u rectangle(s)%s: the host applies at most "
                        "one inclusive rectangle\n", count, exclusive ? ", exclusive" : "");
}

BOOL xbox_D3D8GetScissors(UINT *count, BOOL *exclusive, D3DRECT *rect)
{
    if (count)     *count = g_scissor_count;
    if (exclusive) *exclusive = g_scissor_exclusive;
    if (rect)      *rect = g_scissor_rect;
    return g_scissor_count != 0;
}

BOOL d3d8_GetScissor(RhiRect *out)
{
    if (out) {
        float sx = rt_scale_x(), sy = rt_scale_y();
        float left = g_scissor.left * sx, right = g_scissor.right * sx;
        float k, cx;

        /* Stored as the title gave them, converted here, so the stored
         * rectangle stays comparable with anything else in guest pixels
         * and GetScissors keeps answering in the title's own units. */
        if (two_d_squeeze(&k, &cx)) {
            left  = cx + (left  - cx) * k;
            right = cx + (right - cx) * k;
        }
        out->left   = (int32_t)left;
        out->top    = (int32_t)(g_scissor.top    * sy);
        out->right  = (int32_t)right;
        out->bottom = (int32_t)(g_scissor.bottom * sy);
    }
    return g_scissor_enabled;
}
UINT d3d8_GetGuestHeight(void) { return g_guest_height ? g_guest_height : g_device_state.present_height; }
const DWORD         *d3d8_GetRenderStates(void) { return g_device_state.render_states; }
const DWORD         *d3d8_GetTSS(DWORD stage) { return (stage < MAX_TEXTURE_STAGES) ? g_device_state.tss[stage] : NULL; }
IDirect3DBaseTexture8 *d3d8_GetStageTexture(DWORD stage) { return (stage < 4) ? g_cur_textures[stage] : NULL; }
const D3DMATRIX     *d3d8_GetTransform(D3DTRANSFORMSTATETYPE type) {
    return ((DWORD)type < MAX_TRANSFORMS) ? &g_device_state.transforms[(DWORD)type] : NULL;
}

const D3DLIGHT8     *d3d8_GetLight(DWORD index) {
    return (index < MAX_LIGHTS) ? &g_device_state.lights[index] : NULL;
}

BOOL                 d3d8_GetLightEnable(DWORD index) {
    return (index < MAX_LIGHTS) ? g_device_state.light_enable[index] : FALSE;
}

const D3DMATERIAL8  *d3d8_GetMaterial(void) {
    return &g_device_state.material;
}

DWORD                d3d8_GetCurrentFVF(void) {
    return g_cur_vb ? ((D3D8VertexBuffer *)g_cur_vb)->fvf : 0;
}

UINT                 d3d8_GetNumLights(void) {
    return MAX_LIGHTS;
}

/* ================================================================
 * Device creation (the backend makes the device and swap chain)
 * ================================================================ */

static HRESULT create_device_and_swap_chain(
    D3D8DeviceState *state,
    D3DPRESENT_PARAMETERS *pp)
{
    RhiDeviceDesc dd;

    rhi_select_backend(NULL);
    memset(&dd, 0, sizeof(dd));
    dd.window = pp->hDeviceWindow;
    dd.width = pp->BackBufferWidth ? pp->BackBufferWidth : 640;
    dd.height = pp->BackBufferHeight ? pp->BackBufferHeight : 480;
    dd.buffer_count = pp->BackBufferCount ? pp->BackBufferCount : 1;
    dd.windowed = pp->Windowed ? 1 : 0;
#ifdef _DEBUG
    dd.debug = 1;
#endif
    if (rhi_device_create(&dd) != 0)
        return E_FAIL;

    state->hwnd = pp->hDeviceWindow;

    /* The swap chain matches the window and the guest's presentation size;
     * the scene is whatever the display policy makes of it. */
    state->present_width = dd.width;
    state->present_height = dd.height;
    state->swap_width = dd.width;
    state->swap_height = dd.height;
    d3d8_display_scene_size(state->present_width, state->present_height,
                            &state->width, &state->height);

    return S_OK;
}

static HRESULT create_render_targets(D3D8DeviceState *state)
{
    RhiImageDesc depth_desc;
    HRESULT hr;

    {
        RhiImageDesc sd;

        /* An offscreen colour target the size of the scene, always, even
         * when that is the size the guest asked for. The swap chain
         * belongs to the window and the window can be any shape, so the
         * step that puts one on the other is where the picture is kept
         * from stretching -- and a path that only exists above scale 1 is
         * a path that is only right above scale 1. The cost when nothing
         * is scaled is one full-screen copy of a frame that was about to
         * be presented anyway.
         *
         * SHADER_RESOURCE because both the resolve and a title reading
         * back its own screen sample it. */
        memset(&sd, 0, sizeof sd);
        sd.type = RHI_IMAGE_2D;
        sd.width = state->width;
        sd.height = state->height;
        sd.depth = 1;
        sd.mip_levels = 1;
        sd.format = RHI_FORMAT_R8G8B8A8_UNORM;
        sd.samples = 1;
        sd.usage = RHI_USAGE_DEFAULT;
        sd.bind = RHI_BIND_RENDER_TARGET | RHI_BIND_SAMPLED;

        hr = E_FAIL;
        state->rhi_scene_image = rhi_image_create(&sd, NULL);
        if (state->rhi_scene_image)
            state->rhi_default_rtv = rhi_view_create(state->rhi_scene_image,
                                                     RHI_VIEW_RENDER_TARGET, NULL);
        if (state->rhi_default_rtv)
            state->rhi_scene_srv = rhi_view_create(state->rhi_scene_image,
                                                   RHI_VIEW_SAMPLED, NULL);
        if (state->rhi_scene_srv)
            hr = S_OK;
        if (FAILED(hr))
            fprintf(stderr, "D3D8 display: the %ux%u scene target could not be "
                    "made (0x%08lX); nothing will be drawn\n",
                    state->width, state->height, (unsigned long)hr);
    }
    if (FAILED(hr)) return hr;

    /* Create depth stencil */
    memset(&depth_desc, 0, sizeof(depth_desc));
    depth_desc.type = RHI_IMAGE_2D;
    depth_desc.width = state->width;
    depth_desc.height = state->height;
    depth_desc.depth = 1;
    depth_desc.mip_levels = 1;
    depth_desc.format = RHI_FORMAT_D24_UNORM_S8_UINT;
    depth_desc.samples = 1;
    depth_desc.usage = RHI_USAGE_DEFAULT;
    depth_desc.bind = RHI_BIND_DEPTH;

    state->depth_image = rhi_image_create(&depth_desc, NULL);
    if (!state->depth_image) return E_FAIL;

    state->depth_view = rhi_view_create(state->depth_image, RHI_VIEW_DEPTH, NULL);
    if (!state->depth_view) return E_FAIL;

    /* Bind default render targets */
    rhi_set_render_target(state->rhi_default_rtv, state->depth_view);

    return S_OK;
}

static void d3d8_init_default_states(D3D8DeviceState *state)
{
    /* Set Xbox D3D8 default render states */
    memset(state->render_states, 0, sizeof(state->render_states));
    state->render_states[D3DRS_ZENABLE]           = 1;
    state->render_states[D3DRS_FILLMODE]          = D3DFILL_SOLID;
    state->render_states[D3DRS_SHADEMODE]         = 2; /* D3DSHADE_GOURAUD */
    state->render_states[D3DRS_ZWRITEENABLE]      = TRUE;
    state->render_states[D3DRS_ALPHATESTENABLE]    = FALSE;
    state->render_states[D3DRS_SRCBLEND]          = D3DBLEND_ONE;
    state->render_states[D3DRS_DESTBLEND]         = D3DBLEND_ZERO;
    state->render_states[D3DRS_CULLMODE]          = D3DCULL_CCW;
    state->render_states[D3DRS_ZFUNC]             = D3DCMP_LESSEQUAL;
    state->render_states[D3DRS_ALPHAREF]          = 0;
    state->render_states[D3DRS_ALPHAFUNC]         = D3DCMP_ALWAYS;
    state->render_states[D3DRS_ALPHABLENDENABLE]   = FALSE;
    state->render_states[D3DRS_FOGENABLE]         = FALSE;
    state->render_states[D3DRS_STENCILENABLE]     = FALSE;
    state->render_states[D3DRS_COLORWRITEENABLE]  = 0x0F;

    /* Default viewport. In guest pixels, like every viewport a title sets
     * and reads back: the conversion to host pixels happens in
     * dev_SetViewport, and the initial host viewport is set explicitly at
     * the end of CreateDevice. */
    state->viewport.X = 0;
    state->viewport.Y = 0;
    state->viewport.Width = state->present_width;
    state->viewport.Height = state->present_height;
    state->viewport.MinZ = 0.0f;
    state->viewport.MaxZ = 1.0f;

    /* Default texture stage states, as D3D8 documents them: stage 0
     * modulates texture by diffuse, later stages are off, arguments are
     * TEXTURE then CURRENT, and each stage reads its own texcoord set.
     * They must be real values rather than 0: 0 is D3DTA_DIFFUSE, a valid
     * argument, so the pixel shader setup cannot treat 0 as "unset". */
    memset(state->tss, 0, sizeof(state->tss));
    for (int s = 0; s < MAX_TEXTURE_STAGES; s++) {
        state->tss[s][D3DTSS_COLOROP]       = s == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
        state->tss[s][D3DTSS_COLORARG1]     = D3DTA_TEXTURE;
        state->tss[s][D3DTSS_COLORARG2]     = D3DTA_CURRENT;
        state->tss[s][D3DTSS_ALPHAOP]       = s == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
        state->tss[s][D3DTSS_ALPHAARG1]     = D3DTA_TEXTURE;
        state->tss[s][D3DTSS_ALPHAARG2]     = D3DTA_CURRENT;
        state->tss[s][D3DTSS_TEXCOORDINDEX] = (DWORD)s;
    }

    /* Identity matrices */
    for (int i = 0; i < MAX_TRANSFORMS; i++) {
        memset(&state->transforms[i], 0, sizeof(D3DMATRIX));
        state->transforms[i]._11 = 1.0f;
        state->transforms[i]._22 = 1.0f;
        state->transforms[i]._33 = 1.0f;
        state->transforms[i]._44 = 1.0f;
    }

    state->vertex_shader = 0;
    state->pixel_shader = 0;
    state->in_scene = FALSE;
}

/* ================================================================
 * IDirect3DDevice8 method implementations
 * ================================================================ */

static HRESULT __stdcall dev_QueryInterface(IDirect3DDevice8 *self, const IID *riid, void **ppv)
{
    (void)self; (void)riid; (void)ppv;
    return E_NOINTERFACE;
}

static ULONG __stdcall dev_AddRef(IDirect3DDevice8 *self)
{
    (void)self;
    return InterlockedIncrement(&g_device_state.ref_count);
}

static ULONG __stdcall dev_Release(IDirect3DDevice8 *self)
{
    (void)self;
    LONG ref = InterlockedDecrement(&g_device_state.ref_count);
    if (ref <= 0) {
        /* Cleanup subsystems first */
        up_ring_shutdown();
        d3d8_overlay_shutdown();
        d3d8_menu_shutdown();
        d3d8_movie_shutdown();
        xbox_D3D8ScreenCopyShutdown();
        d3d8_display_shutdown();
        d3d8_vsh_shutdown();
        d3d8_combiners_shutdown();
        d3d8_states_shutdown();
        d3d8_shaders_shutdown();

        /* Cleanup D3D11 resources */
        D3D8DeviceState *s = &g_device_state;
        if (g_cur_rt) { IDirect3DSurface8_Release(&g_cur_rt->iface); g_cur_rt = NULL; }
        if (g_cur_ds) { IDirect3DSurface8_Release(&g_cur_ds->iface); g_cur_ds = NULL; }
        if (g_auto_ds) { IDirect3DSurface8_Release(&g_auto_ds->iface); g_auto_ds = NULL; }
        interp_target_destroy();
        rhi_view_destroy(s->depth_view); s->depth_view = NULL;
        rhi_image_destroy(s->depth_image); s->depth_image = NULL;
        rhi_view_destroy(s->rhi_scene_srv); s->rhi_scene_srv = NULL;
        rhi_view_destroy(s->rhi_default_rtv); s->rhi_default_rtv = NULL;
        rhi_image_destroy(s->rhi_scene_image); s->rhi_scene_image = NULL;
        rhi_device_destroy();
        g_device_initialized = FALSE;
    }
    return (ULONG)ref;
}

static HRESULT __stdcall dev_GetDirect3D(IDirect3DDevice8 *self, IDirect3D8 **ppD3D8)
{
    (void)self; (void)ppD3D8;
    /* TODO: return the factory */
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_GetDeviceCaps(IDirect3DDevice8 *self, void *pCaps)
{
    (void)self; (void)pCaps;
    /* TODO: fill with Xbox NV2A capabilities */
    return S_OK;
}

static HRESULT __stdcall dev_GetDisplayMode(IDirect3DDevice8 *self, void *pMode)
{
    (void)self; (void)pMode;
    return S_OK;
}

static HRESULT __stdcall dev_GetCreationParameters(IDirect3DDevice8 *self, void *pParams)
{
    (void)self; (void)pParams;
    return S_OK;
}

static HRESULT __stdcall dev_Reset(IDirect3DDevice8 *self, D3DPRESENT_PARAMETERS *pPP)
{
    (void)self; (void)pPP;
    /* TODO: resize swap chain */
    return S_OK;
}

static DWORD g_d3d_begin_count = 0;
static DWORD g_d3d_end_count = 0;
static DWORD g_d3d_clear_count = 0;
static DWORD g_d3d_draw_count = 0;
static DWORD g_d3d_settransform_count = 0;
static DWORD g_d3d_setrs_count = 0;
static DWORD g_d3d_settexture_count = 0;

static HRESULT __stdcall dev_Present(IDirect3DDevice8 *self, const RECT *src, const RECT *dst, HWND hWnd, void *pDirty)
{
    static DWORD frame_count = 0;
    static DWORD last_tick = 0;
    (void)self; (void)src; (void)dst; (void)hWnd; (void)pDirty;

    frame_count++;
    DWORD now = GetTickCount();
    if (last_tick == 0) last_tick = now;
    if (now - last_tick >= 2000) {
        fprintf(stderr, "  [D3D] %.1fs: %u present (%.1f fps), %u begin, %u end, "
                "%u clear, %u draw, %u xform, %u rs, %u tex\n",
                (now - last_tick) / 1000.0, frame_count,
                frame_count * 1000.0 / (now - last_tick),
                g_d3d_begin_count, g_d3d_end_count,
                g_d3d_clear_count, g_d3d_draw_count,
                g_d3d_settransform_count, g_d3d_setrs_count,
                g_d3d_settexture_count);
        fflush(stderr);
        frame_count = 0;
        g_d3d_begin_count = g_d3d_end_count = 0;
        g_d3d_clear_count = g_d3d_draw_count = 0;
        g_d3d_settransform_count = g_d3d_setrs_count = 0;
        g_d3d_settexture_count = 0;
        last_tick = now;
    }

    /* Pump Windows messages: the game's internal main loop drives rendering,
     * so our external message pump never runs. Process messages here to keep
     * the window responsive and handle input. */
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            ExitProcess(0);
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    present_resolve();
    {
        HRESULT hr = (HRESULT)rhi_present(1);
        follow_scale();
        return hr;
    }
}

static HRESULT __stdcall dev_GetBackBuffer(IDirect3DDevice8 *self, INT iBackBuffer, DWORD Type, IDirect3DSurface8 **ppSurface)
{
    (void)self; (void)iBackBuffer; (void)Type;

    if (!ppSurface) return E_INVALIDARG;

    /* The scene target, not the swap chain: that is what every draw went
     * to, and its size is the one the rest of the device reports. */
    if (!g_device_state.rhi_scene_image) return E_FAIL;

    *ppSurface = d3d8_surface_create(g_device_state.rhi_scene_image, 0, 0,
                                     g_device_state.width,
                                     g_device_state.height,
                                     D3DFMT_X8R8G8B8,
                                     D3DPOOL_DEFAULT, 0,
                                     D3DMULTISAMPLE_NONE, NULL, 0);
    return *ppSurface ? S_OK : E_OUTOFMEMORY;
}

static HRESULT __stdcall dev_BeginScene(IDirect3DDevice8 *self)
{
    (void)self;
    g_device_state.in_scene = TRUE;
    g_d3d_begin_count++;
    return S_OK;
}

static HRESULT __stdcall dev_EndScene(IDirect3DDevice8 *self)
{
    (void)self;
    g_device_state.in_scene = FALSE;
    g_d3d_end_count++;
    return S_OK;
}

/* ---- Clears one call cannot do ----
 *
 * An Xbox Clear can name rectangles, and the colour channels one at a time
 * (D3DCLEAR_TARGET_R, _G, _B, _A); a host clear does the whole target, all
 * four. So such a clear is drawn instead: a quad over each rectangle, of the
 * clear colour, writing only the channels asked for, and the depth and
 * stencil values with the test always passing. Future Perfect's match
 * settings panel clears the alpha alone in its rectangle and then writes
 * its text there into the alpha, to light the text through it; cleared as
 * a whole target (or not at all), the panel lit up white.
 *
 * The channels come in the flags' bits 8-11 under D3DCLEAR_XBOX_CHANNELS
 * (xbox_clear_flags_to_host); without that bit TARGET means all four. */
#define D3DCLEAR_XBOX_CHANNELS 0x1000u

static struct {
    int             tried, failed;
    RhiShader      *vs, *ps;
    RhiBuffer      *cb;
    RhiBlendState  *blend[16];      /* by the channels written */
    RhiDepthState  *depth[4];       /* bit 0 depth, bit 1 stencil */
    RhiRasterState *raster;
} g_clr;

static const char kClearShader[] =
    "cbuffer Clear : register(b7) { float4 color; float4 zs; };\n"
    "float4 vs_main(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 c = float2(id & 1, id >> 1);\n"
    "    return float4(c.x * 2 - 1, 1 - c.y * 2, zs.x, 1);\n"
    "}\n"
    "float4 ps_main() : SV_Target { return color; }\n";

static RhiShader *clear_shader(uint32_t stage, const char *entry, const char *target)
{
    RhiShaderSource src;
    char err[1024];
    RhiShader *s;

    memset(&src, 0, sizeof src);
    src.hlsl = kClearShader;
    src.len = sizeof kClearShader - 1;
    src.name = "xbox clear";
    src.entry = entry;
    src.target = target;
    s = rhi_shader_create(stage, &src, err, sizeof err);
    if (!s && err[0])
        fprintf(stderr, "D3D8: the rectangle clear's shader: %s\n", err);
    return s;
}

static int clear_ready(void)
{
    RhiBufferDesc bd;
    RhiRasterDesc rd;
    int i;

    if (g_clr.tried)
        return !g_clr.failed;
    g_clr.tried = 1;
    g_clr.failed = 1;
    memset(&bd, 0, sizeof bd);
    bd.size = 32;
    bd.usage = RHI_USAGE_DYNAMIC;
    bd.bind = RHI_BIND_UNIFORM;
    bd.cpu_access = RHI_CPU_WRITE;
    memset(&rd, 0, sizeof rd);
    rd.fill = RHI_FILL_SOLID;
    rd.cull = RHI_CULL_NONE;
    rd.depth_clip = 1;
    if (!(g_clr.cb = rhi_buffer_create(&bd, NULL)) ||
        !(g_clr.vs = clear_shader(RHI_STAGE_VERTEX, "vs_main", "vs_4_0")) ||
        !(g_clr.ps = clear_shader(RHI_STAGE_PIXEL, "ps_main", "ps_4_0")) ||
        !(g_clr.raster = rhi_raster_state_create(&rd)))
        return 0;
    for (i = 0; i < 16; i++) {
        RhiBlendDesc bl;
        memset(&bl, 0, sizeof bl);
        bl.write_mask = (uint32_t)i;
        if (!(g_clr.blend[i] = rhi_blend_state_create(&bl)))
            return 0;
    }
    for (i = 0; i < 4; i++) {
        RhiDepthDesc dd;
        memset(&dd, 0, sizeof dd);
        dd.depth_enable = (i & 1) != 0;
        dd.depth_write = (i & 1) != 0;
        dd.depth_func = RHI_CMP_ALWAYS;
        dd.stencil_enable = (i & 2) != 0;
        dd.stencil_read_mask = dd.stencil_write_mask = 0xFF;
        dd.front.fail = dd.front.depth_fail = RHI_STENCIL_REPLACE;
        dd.front.pass = RHI_STENCIL_REPLACE;
        dd.front.func = RHI_CMP_ALWAYS;
        dd.back = dd.front;
        if (!(g_clr.depth[i] = rhi_depth_state_create(&dd)))
            return 0;
    }
    g_clr.failed = 0;
    return 1;
}

/* The clear drawn: the channels in `channels` (RHI_WRITE_*), depth and
 * stencil by `flags`, over each rectangle (the whole viewport for none). The
 * rest of the device's state is the next draw's to set, as for the overlay;
 * the render target and viewport are put back. */
static void clear_drawn(DWORD count, const D3DRECT *rects, uint32_t channels, DWORD flags,
                        D3DCOLOR color, float z, DWORD stencil)
{
    RhiOutputState saved;
    float c[8], factor[4] = { 1, 1, 1, 1 }, sx = rt_scale_x(), sy = rt_scale_y();
    int depth = ((flags & D3DCLEAR_ZBUFFER) ? 1 : 0) | ((flags & D3DCLEAR_STENCIL) ? 2 : 0);
    D3DRECT whole;
    void *mapped;
    DWORD i;

    if (!clear_ready() || (!channels && !depth))
        return;
    if (!channels && !(g_cur_ds && g_cur_ds->dsv))
        return;                             /* depth alone, and none bound */
    if (!count || !rects) {
        whole.x1 = (LONG)g_device_state.viewport.X;
        whole.y1 = (LONG)g_device_state.viewport.Y;
        whole.x2 = whole.x1 + (LONG)g_device_state.viewport.Width;
        whole.y2 = whole.y1 + (LONG)g_device_state.viewport.Height;
        rects = &whole;
        count = 1;
    }
    c[0] = ((color >> 16) & 0xFF) / 255.0f;
    c[1] = ((color >>  8) & 0xFF) / 255.0f;
    c[2] = ((color >>  0) & 0xFF) / 255.0f;
    c[3] = ((color >> 24) & 0xFF) / 255.0f;
    c[4] = z; c[5] = c[6] = c[7] = 0.0f;
    if (!(mapped = rhi_buffer_map(g_clr.cb, RHI_MAP_WRITE_DISCARD)))
        return;
    memcpy(mapped, c, sizeof c);
    rhi_buffer_unmap(g_clr.cb);

    rhi_output_save(&saved);
    {
        /* The target and depth the device clears (dev_Clear's own). */
        RhiView *rtv = g_cur_rt ? g_cur_rt->rtv : g_device_state.rhi_default_rtv;
        RhiView *dsv = g_cur_ds ? g_cur_ds->dsv : NULL;
        if (!dsv)
            depth = 0;
        rhi_set_render_target(rtv, dsv);
    }
    rhi_set_vertex_layout(NULL);
    rhi_set_topology(RHI_TOPOLOGY_TRIANGLE_STRIP);
    rhi_set_shader(RHI_STAGE_VERTEX, g_clr.vs);
    rhi_set_shader(RHI_STAGE_PIXEL, g_clr.ps);
    rhi_set_uniform_buffers(RHI_STAGE_VERTEX, 7, 1, &g_clr.cb);
    rhi_set_uniform_buffers(RHI_STAGE_PIXEL, 7, 1, &g_clr.cb);
    rhi_set_blend_state(g_clr.blend[channels & 15u], factor, 0xFFFFFFFF);
    rhi_set_depth_state(g_clr.depth[depth], stencil & 0xFF);
    rhi_set_raster_state(g_clr.raster);
    for (i = 0; i < count; i++) {
        RhiViewport vp;

        if (rects[i].x2 <= rects[i].x1 || rects[i].y2 <= rects[i].y1)
            continue;
        vp.x = (float)rects[i].x1 * sx;
        vp.y = (float)rects[i].y1 * sy;
        vp.width = (float)(rects[i].x2 - rects[i].x1) * sx;
        vp.height = (float)(rects[i].y2 - rects[i].y1) * sy;
        vp.min_depth = 0.0f;
        vp.max_depth = 1.0f;
        rhi_set_viewports(1, &vp);
        rhi_draw(4, 0);
    }
    rhi_output_restore(&saved);
}

static HRESULT __stdcall dev_Clear(IDirect3DDevice8 *self, DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil)
{
    uint32_t channels = !(Flags & D3DCLEAR_TARGET) ? 0u
                      : (Flags & D3DCLEAR_XBOX_CHANNELS) ? (Flags >> 8) & 15u : RHI_WRITE_ALL;

    (void)self;
    g_d3d_clear_count++;

    /* Rectangles, or some of the channels: drawn (above). */
    if ((Count && pRects) || (channels && channels != RHI_WRITE_ALL)) {
        clear_drawn(Count, pRects, channels, Flags, Color, Z, Stencil);
        return S_OK;
    }

    /* Clear the currently bound targets. With no depth surface bound there
     * is no depth to clear (SetRenderTarget). */
    RhiView *rtv = g_cur_rt ? g_cur_rt->rtv : g_device_state.rhi_default_rtv;
    RhiView *dsv = g_cur_ds ? g_cur_ds->dsv : NULL;

    if ((Flags & D3DCLEAR_TARGET) && rtv) {
        float clear_color[4] = {
            ((Color >> 16) & 0xFF) / 255.0f,  /* R */
            ((Color >>  8) & 0xFF) / 255.0f,  /* G */
            ((Color >>  0) & 0xFF) / 255.0f,  /* B */
            ((Color >> 24) & 0xFF) / 255.0f,  /* A */
        };
        rhi_clear_color(rtv, clear_color);
    }

    if ((Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) && dsv) {
        uint32_t clear_flags = 0;
        if (Flags & D3DCLEAR_ZBUFFER) clear_flags |= RHI_CLEAR_DEPTH;
        if (Flags & D3DCLEAR_STENCIL) clear_flags |= RHI_CLEAR_STENCIL;

        rhi_clear_depth(dsv, clear_flags, Z, (uint8_t)Stencil);
    }

    return S_OK;
}

static HRESULT __stdcall dev_SetTransform(IDirect3DDevice8 *self, D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix)
{
    (void)self;
    g_d3d_settransform_count++;
    if ((DWORD)State < MAX_TRANSFORMS && pMatrix) {
        g_device_state.transforms[(DWORD)State] = *pMatrix;
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetTransform(IDirect3DDevice8 *self, D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix)
{
    (void)self;
    if ((DWORD)State < MAX_TRANSFORMS && pMatrix) {
        *pMatrix = g_device_state.transforms[(DWORD)State];
    }
    return S_OK;
}

static HRESULT __stdcall dev_SetRenderState(IDirect3DDevice8 *self, D3DRENDERSTATETYPE State, DWORD Value)
{
    (void)self;
    g_d3d_setrs_count++;
    if ((DWORD)State < MAX_RENDER_STATES) {
        g_device_state.render_states[(DWORD)State] = Value;
    }
    /* Mark combiner state dirty if any PS register combiner state changed */
    if ((DWORD)State >= D3DRS_PSFINALCOMBINERCONSTANT0 && (DWORD)State <= D3DRS_PSINPUTTEXTURE) {
        d3d8_combiners_mark_dirty();
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetRenderState(IDirect3DDevice8 *self, D3DRENDERSTATETYPE State, DWORD *pValue)
{
    (void)self;
    if ((DWORD)State < MAX_RENDER_STATES && pValue) {
        *pValue = g_device_state.render_states[(DWORD)State];
    }
    return S_OK;
}

static HRESULT __stdcall dev_SetTextureStageState(IDirect3DDevice8 *self, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
{
    (void)self;
    if (Stage < MAX_TEXTURE_STAGES && (DWORD)Type < MAX_TSS_STATES) {
        g_device_state.tss[Stage][(DWORD)Type] = Value;
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetTextureStageState(IDirect3DDevice8 *self, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue)
{
    (void)self;
    if (Stage < MAX_TEXTURE_STAGES && (DWORD)Type < MAX_TSS_STATES && pValue) {
        *pValue = g_device_state.tss[Stage][(DWORD)Type];
    }
    return S_OK;
}

static HRESULT __stdcall dev_SetTexture(IDirect3DDevice8 *self, DWORD Stage, IDirect3DBaseTexture8 *pTexture)
{
    (void)self;
    g_d3d_settexture_count++;
    if (Stage >= 4) return E_INVALIDARG;
    g_cur_textures[Stage] = pTexture;

    /* Bind SRV to pixel shader */
    if (pTexture) {
        RhiView *srv = d3d8_base_srv(pTexture);
        /* P8 textures bake a palette; record which stage palette they use
         * so SetPalette can re-bake them later. */
        if (d3d8_format_is_palettized(d3d8_base_format(pTexture)))
            d3d8_base_set_palette(pTexture, Stage);
        if (srv)
            rhi_set_textures(Stage, 1, &srv);
        /* Mark texture stage as active */
        if (g_device_state.tss[Stage][D3DTSS_COLOROP] == D3DTOP_DISABLE)
            g_device_state.tss[Stage][D3DTSS_COLOROP] = D3DTOP_MODULATE;
    } else {
        RhiView *null_srv = NULL;
        rhi_set_textures(Stage, 1, &null_srv);
        g_device_state.tss[Stage][D3DTSS_COLOROP] = D3DTOP_DISABLE;
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetTexture(IDirect3DDevice8 *self, DWORD Stage, IDirect3DBaseTexture8 **ppTexture)
{
    (void)self;
    if (!ppTexture || Stage >= MAX_TEXTURE_STAGES) return E_INVALIDARG;
    *ppTexture = g_cur_textures[Stage];
    if (*ppTexture) IDirect3DBaseTexture8_AddRef(*ppTexture);
    return S_OK;
}

static HRESULT __stdcall dev_SetStreamSource(IDirect3DDevice8 *self, UINT StreamNumber, IDirect3DVertexBuffer8 *pStreamData, UINT Stride)
{
    (void)self;
    if (StreamNumber != 0) return S_OK; /* Only stream 0 supported */
    g_cur_vb = pStreamData;
    g_cur_vb_stride = Stride;

    if (pStreamData) {
        D3D8VertexBuffer *vb = (D3D8VertexBuffer *)pStreamData;
        rhi_set_vertex_buffer(0, vb->buffer, Stride, 0);
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetStreamSource(IDirect3DDevice8 *self, UINT StreamNumber, IDirect3DVertexBuffer8 **ppStreamData, UINT *pStride)
{
    (void)self;
    if (!ppStreamData || StreamNumber != 0) return E_INVALIDARG;
    *ppStreamData = g_cur_vb;
    if (*ppStreamData) (*ppStreamData)->lpVtbl->AddRef(*ppStreamData);
    if (pStride) *pStride = g_cur_vb_stride;
    return S_OK;
}

static HRESULT __stdcall dev_SetIndices(IDirect3DDevice8 *self, IDirect3DIndexBuffer8 *pIndexData, UINT BaseVertexIndex)
{
    (void)self;
    g_cur_ib = pIndexData;
    g_cur_ib_base_vertex = BaseVertexIndex;

    if (pIndexData) {
        D3D8IndexBuffer *ib = (D3D8IndexBuffer *)pIndexData;
        rhi_set_index_buffer(ib->buffer, ib->format == D3DFMT_INDEX32 ? 32 : 16, 0);
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetIndices(IDirect3DDevice8 *self, IDirect3DIndexBuffer8 **ppIndexData, UINT *pBaseVertexIndex)
{
    (void)self;
    if (!ppIndexData) return E_INVALIDARG;
    *ppIndexData = g_cur_ib;
    if (*ppIndexData) (*ppIndexData)->lpVtbl->AddRef(*ppIndexData);
    if (pBaseVertexIndex) *pBaseVertexIndex = g_cur_ib_base_vertex;
    return S_OK;
}

static uint32_t map_primitive_type(D3DPRIMITIVETYPE pt, UINT count, UINT *out_count)
{
    switch (pt) {
    case D3DPT_TRIANGLELIST:  *out_count = count * 3; return RHI_TOPOLOGY_TRIANGLES;
    case D3DPT_TRIANGLESTRIP: *out_count = count + 2; return RHI_TOPOLOGY_TRIANGLE_STRIP;
    case D3DPT_TRIANGLEFAN:   *out_count = count * 3; return RHI_TOPOLOGY_TRIANGLES;
    case D3DPT_LINELIST:      *out_count = count * 2; return RHI_TOPOLOGY_LINES;
    case D3DPT_LINESTRIP:     *out_count = count + 1; return RHI_TOPOLOGY_LINE_STRIP;
    case D3DPT_POINTLIST:     *out_count = count;     return RHI_TOPOLOGY_POINTS;
    case D3DPT_QUADLIST:      *out_count = count * 6; return RHI_TOPOLOGY_TRIANGLES;
    default:                  *out_count = 0;          return RHI_TOPOLOGY_UNDEFINED;
    }
}

/* ================================================================
 * Triangle fan / quad list → triangle list conversion
 *
 * D3D11 doesn't support triangle fans or quad lists.
 * Convert vertex data in-place to triangle list.
 * Returns malloc'd buffer (caller must free) or NULL if no conversion needed.
 * ================================================================ */

static void *convert_fan_or_quad(D3DPRIMITIVETYPE pt, const void *src,
                                  UINT prim_count, UINT stride,
                                  UINT *out_vertex_count)
{
    BYTE *dst;
    const BYTE *s = (const BYTE *)src;
    UINT i;

    if (pt == D3DPT_TRIANGLEFAN) {
        /* Fan: vertex 0 is the hub, each triangle is (0, i+1, i+2) */
        UINT tri_verts = prim_count * 3;
        dst = (BYTE *)malloc(tri_verts * stride);
        if (!dst) return NULL;

        for (i = 0; i < prim_count; i++) {
            memcpy(dst + (i * 3 + 0) * stride, s, stride);                      /* v0 (hub) */
            memcpy(dst + (i * 3 + 1) * stride, s + (i + 1) * stride, stride);   /* v[i+1] */
            memcpy(dst + (i * 3 + 2) * stride, s + (i + 2) * stride, stride);   /* v[i+2] */
        }
        *out_vertex_count = tri_verts;
        return dst;
    }

    if (pt == D3DPT_QUADLIST) {
        /* Quad list: each quad (v0,v1,v2,v3) → 2 triangles (v0,v1,v2), (v0,v2,v3) */
        UINT tri_verts = prim_count * 6;
        dst = (BYTE *)malloc(tri_verts * stride);
        if (!dst) return NULL;

        for (i = 0; i < prim_count; i++) {
            const BYTE *q = s + i * 4 * stride;
            memcpy(dst + (i * 6 + 0) * stride, q + 0 * stride, stride);  /* v0 */
            memcpy(dst + (i * 6 + 1) * stride, q + 1 * stride, stride);  /* v1 */
            memcpy(dst + (i * 6 + 2) * stride, q + 2 * stride, stride);  /* v2 */
            memcpy(dst + (i * 6 + 3) * stride, q + 0 * stride, stride);  /* v0 */
            memcpy(dst + (i * 6 + 4) * stride, q + 2 * stride, stride);  /* v2 */
            memcpy(dst + (i * 6 + 5) * stride, q + 3 * stride, stride);  /* v3 */
        }
        *out_vertex_count = tri_verts;
        return dst;
    }

    return NULL; /* no conversion needed */
}

/* ================================================================
 * DrawPrimitiveUP ring buffers
 *
 * Instead of creating and destroying a D3D11 buffer on every UP draw,
 * append into a persistent dynamic buffer with the discard /
 * no-overwrite discipline: NO_OVERWRITE while there is room, DISCARD
 * at the wrap, which renames the buffer underneath draws already
 * submitted so they keep their data. One ring holds vertices, one
 * holds indices; the indexed draw used to create and release two
 * immutable buffers per call, ~700 a frame in TimeSplitters 2's level.
 * ================================================================ */

#define UP_RING_BUFFER_SIZE (4 * 1024 * 1024)  /* 4MB per ring */

typedef struct UpRing {
    RhiBuffer *buffer;
    UINT       offset;
    UINT       bind;      /* RHI_BIND_VERTEX or RHI_BIND_INDEX */
    UINT       wraps;     /* how often the ring started over */
} UpRing;

static UpRing g_up_vertex_ring = { NULL, 0, RHI_BIND_VERTEX, 0 };
static UpRing g_up_index_ring  = { NULL, 0, RHI_BIND_INDEX, 0 };

static HRESULT up_ring_init(UpRing *ring)
{
    RhiBufferDesc bd;
    memset(&bd, 0, sizeof(bd));
    bd.size = UP_RING_BUFFER_SIZE;
    bd.usage = RHI_USAGE_DYNAMIC;
    bd.bind = ring->bind;
    bd.cpu_access = RHI_CPU_WRITE;
    ring->buffer = rhi_buffer_create(&bd, NULL);
    return ring->buffer ? S_OK : E_FAIL;
}

static void up_ring_release(UpRing *ring)
{
    rhi_buffer_destroy(ring->buffer);
    ring->buffer = NULL;
    ring->offset = 0;
}

static void up_ring_shutdown(void)
{
    up_ring_release(&g_up_vertex_ring);
    up_ring_release(&g_up_index_ring);
}

/* Append data to a ring, returns its byte offset. Returns (UINT)-1 on failure. */
static UINT up_ring_upload(UpRing *ring, const void *data, UINT size)
{
    void *mapped;
    uint32_t map_type;
    UINT offset;

    if (!ring->buffer) {
        if (FAILED(up_ring_init(ring))) return (UINT)-1;
    }

    if (size > UP_RING_BUFFER_SIZE) return (UINT)-1;

    /* Wrap around if not enough space */
    if (ring->offset + size > UP_RING_BUFFER_SIZE) {
        ring->offset = 0;
        ring->wraps++;
        map_type = RHI_MAP_WRITE_DISCARD;
    } else {
        map_type = RHI_MAP_WRITE_NO_OVERWRITE;
    }

    mapped = rhi_buffer_map(ring->buffer, map_type);
    if (!mapped) return (UINT)-1;

    offset = ring->offset;
    memcpy((BYTE *)mapped + offset, data, size);

    rhi_buffer_unmap(ring->buffer);

    ring->offset = (offset + size + 15) & ~15;  /* 16-byte align */
    return offset;
}

/* RECOMP_D3D8_STAGE0_PROBE=<n>: the host image bound at texture stage 0
 * for the n-th draw since the device was made, read back and written as
 * stage0_draw<n>.bmp in the working directory. It reads what the GPU will
 * sample, through rhi_image_readback, so the same probe run under two
 * backends says whether they were given the same texels -- the question
 * that separates an upload fault from a sampling or shading one. */
static void stage0_probe(void)
{
    static long want = -2, n;
    RhiImage *img;
    RhiImageDesc d;
    uint8_t *px;
    char path[64];
    FILE *f;

    if (want == -2) {
        const char *v = getenv("RECOMP_D3D8_STAGE0_PROBE");
        want = (v && *v) ? atol(v) : -1;
    }
    if (want < 0 || n++ != want)
        return;
    img = g_cur_textures[0] ? d3d8_base_resource(g_cur_textures[0]) : NULL;
    if (!img) {
        fprintf(stderr, "D3D8 stage 0 probe: draw %ld has no texture at stage 0\n", want);
        return;
    }
    rhi_image_get_desc(img, &d);
    if (d.type != RHI_IMAGE_2D || rhi_format_row_pitch(d.format, d.width) != d.width * 4u) {
        fprintf(stderr, "D3D8 stage 0 probe: draw %ld's texture is format %u; only 32-bit "
                "2D images are written\n", want, (unsigned)d.format);
        return;
    }
    px = (uint8_t *)malloc((size_t)d.width * d.height * 4u);
    if (!px || rhi_image_readback(img, 0, px, d.width * 4u) != 0) {
        free(px);
        return;
    }
    snprintf(path, sizeof path, "stage0_draw%ld.bmp", want);
    if ((f = fopen(path, "wb")) != NULL) {
        uint8_t hdr[54];
        UINT y, x;
        memset(hdr, 0, sizeof hdr);
        hdr[0] = 'B'; hdr[1] = 'M';
        *(uint32_t *)(hdr + 2) = 54u + d.width * d.height * 4u;
        *(uint32_t *)(hdr + 10) = 54;
        *(uint32_t *)(hdr + 14) = 40;
        *(int32_t *)(hdr + 18) = (int32_t)d.width;
        *(int32_t *)(hdr + 22) = (int32_t)d.height;
        *(uint16_t *)(hdr + 26) = 1;
        *(uint16_t *)(hdr + 28) = 32;
        fwrite(hdr, 1, sizeof hdr, f);
        for (y = d.height; y-- > 0; )           /* bottom-up; the bytes as the image holds them */
            for (x = 0; x < d.width; x++)
                fwrite(px + ((size_t)y * d.width + x) * 4u, 1, 4, f);
        fclose(f);
        fprintf(stderr, "D3D8 stage 0 probe: draw %ld, %ux%u format %u, written to %s\n",
                want, d.width, d.height, (unsigned)d.format, path);
    }
    free(px);
}

static HRESULT __stdcall dev_DrawPrimitive(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount)
{
    (void)self;
    g_d3d_draw_count++;
    uint32_t topology;
    UINT vertex_count;

    topology = map_primitive_type(PrimitiveType, PrimitiveCount, &vertex_count);
    if (vertex_count == 0) return E_INVALIDARG;

    /* Prepare pipeline: shaders, input layout, constant buffers, render states */
    d3d8_shaders_prepare_draw(g_device_state.vertex_shader);  /* VS, then the combiner PS or the fixed-function one */
    d3d8_states_apply();

    rhi_set_topology(topology);
    stage0_probe();
    rhi_draw(vertex_count, StartVertex);
    return S_OK;
}

static HRESULT __stdcall dev_DrawIndexedPrimitive(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT StartIndex, UINT PrimitiveCount)
{
    (void)self; (void)MinVertexIndex; (void)NumVertices;
    g_d3d_draw_count++;
    uint32_t topology;
    UINT index_count;

    topology = map_primitive_type(PrimitiveType, PrimitiveCount, &index_count);
    if (index_count == 0) return E_INVALIDARG;

    /* Vertex shader: try programmable VS first, fall back to FVF fixed-function */
    d3d8_shaders_prepare_draw(g_device_state.vertex_shader);  /* VS, then the combiner PS or the fixed-function one */
    d3d8_states_apply();

    rhi_set_topology(topology);
    stage0_probe();
    rhi_draw_indexed(index_count, StartIndex, (int32_t)g_cur_ib_base_vertex);
    return S_OK;
}

UINT d3d8_up_vertices_read(D3DPRIMITIVETYPE type, UINT prims)
{
    UINT n;

    map_primitive_type(type, prims, &n);
    if (!n)
        return 0;
    /* map_primitive_type counts what is drawn; fans and quads are expanded
     * from fewer caller vertices (convert_fan_or_quad). */
    if (type == D3DPT_TRIANGLEFAN)
        return prims + 2;
    if (type == D3DPT_QUADLIST)
        return prims * 4;
    return n;
}

UINT d3d8_up_indices_read(D3DPRIMITIVETYPE type, UINT prims)
{
    UINT n;

    /* The indexed draw converts nothing: it reads what it draws. */
    map_primitive_type(type, prims, &n);
    return n;
}

static HRESULT __stdcall dev_DrawPrimitiveUP(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void *pVertexData, UINT VertexStreamZeroStride)
{
    (void)self;
    g_d3d_draw_count++;
    uint32_t topology;
    UINT vertex_count, vb_size, ring_offset;
    const void *draw_data = pVertexData;
    void *converted = NULL;

    if (!pVertexData || !VertexStreamZeroStride) return E_INVALIDARG;

    topology = map_primitive_type(PrimitiveType, PrimitiveCount, &vertex_count);
    if (vertex_count == 0) return E_INVALIDARG;

    /* Convert triangle fans and quad lists to triangle lists */
    if (PrimitiveType == D3DPT_TRIANGLEFAN || PrimitiveType == D3DPT_QUADLIST) {
        converted = convert_fan_or_quad(PrimitiveType, pVertexData,
                                         PrimitiveCount, VertexStreamZeroStride,
                                         &vertex_count);
        /* Without the converted copy, vertex_count is the triangle-list count
         * and would read past the caller's vertices. */
        if (!converted) return E_OUTOFMEMORY;
        draw_data = converted;
    }

    vb_size = vertex_count * VertexStreamZeroStride;

    /* Upload to ring buffer */
    ring_offset = up_ring_upload(&g_up_vertex_ring, draw_data, vb_size);
    if (converted) free(converted);

    if (ring_offset == (UINT)-1) return E_OUTOFMEMORY;

    /* Bind ring buffer at the right offset */
    rhi_set_vertex_buffer(0, g_up_vertex_ring.buffer, VertexStreamZeroStride, ring_offset);

    /* Vertex shader: try programmable VS first, fall back to FVF fixed-function */
    d3d8_shaders_prepare_draw(g_device_state.vertex_shader);  /* VS, then the combiner PS or the fixed-function one */

    /* Where a screen-space draw goes in widescreen: squeezed to 4:3 with
     * the HUD, or spanning the picture like a backdrop or a fade. After
     * prepare_draw, which is what decides whether this is a screen-space
     * draw at all. */
    d3d8_place_2d_draw(pVertexData, VertexStreamZeroStride, vertex_count);
    d3d8_states_apply();

    rhi_set_topology(topology);
    stage0_probe();
    rhi_draw(vertex_count, 0);

    /* Restore previous VB binding if any */
    if (g_cur_vb) {
        D3D8VertexBuffer *vb = (D3D8VertexBuffer *)g_cur_vb;
        rhi_set_vertex_buffer(0, vb->buffer, g_cur_vb_stride, 0);
    }
    return S_OK;
}

static HRESULT __stdcall dev_DrawIndexedPrimitiveUP(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void *pIndexData, D3DFORMAT IndexDataFormat, const void *pVertexData, UINT VertexStreamZeroStride)
{
    (void)self; (void)MinVertexIndex;
    g_d3d_draw_count++;
    uint32_t topology;
    UINT index_count, vb_size, ib_size;
    UINT vb_offset, ib_offset;
    UINT idx_bytes;

    if (!pVertexData || !pIndexData || !VertexStreamZeroStride) return E_INVALIDARG;

    topology = map_primitive_type(PrimitiveType, PrimitiveCount, &index_count);
    if (index_count == 0) return E_INVALIDARG;


    idx_bytes = (IndexDataFormat == D3DFMT_INDEX32) ? 4 : 2;
    vb_size = NumVertices * VertexStreamZeroStride;
    ib_size = index_count * idx_bytes;

    /* Append vertices and indices to their rings; the draw reads them at
     * the returned offsets. */
    vb_offset = up_ring_upload(&g_up_vertex_ring, pVertexData, vb_size);
    if (vb_offset == (UINT)-1) return E_OUTOFMEMORY;
    ib_offset = up_ring_upload(&g_up_index_ring, pIndexData, ib_size);
    if (ib_offset == (UINT)-1) return E_OUTOFMEMORY;

    /* Bind, prepare, draw */
    rhi_set_vertex_buffer(0, g_up_vertex_ring.buffer, VertexStreamZeroStride, vb_offset);
    rhi_set_index_buffer(g_up_index_ring.buffer, idx_bytes * 8, ib_offset);

    /* Vertex shader: try programmable VS first, fall back to FVF fixed-function */
    d3d8_shaders_prepare_draw(g_device_state.vertex_shader);  /* VS, then the combiner PS or the fixed-function one */

    /* Where a screen-space draw goes in widescreen: squeezed to 4:3 with
     * the HUD, or spanning the picture like a backdrop or a fade. After
     * prepare_draw, which is what decides whether this is a screen-space
     * draw at all. */
    d3d8_place_2d_draw(pVertexData, VertexStreamZeroStride, NumVertices);
    d3d8_states_apply();

    rhi_set_topology(topology);
    stage0_probe();
    rhi_draw_indexed(index_count, 0, 0);

    /* Restore previous bindings */
    if (g_cur_vb) {
        D3D8VertexBuffer *vb = (D3D8VertexBuffer *)g_cur_vb;
        rhi_set_vertex_buffer(0, vb->buffer, g_cur_vb_stride, 0);
    }
    if (g_cur_ib) {
        D3D8IndexBuffer *ib = (D3D8IndexBuffer *)g_cur_ib;
        rhi_set_index_buffer(ib->buffer, ib->format == D3DFMT_INDEX32 ? 32 : 16, 0);
    }
    return S_OK;
}

static HRESULT __stdcall dev_CreateTexture(IDirect3DDevice8 *self, UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture8 **ppTexture)
{
    (void)self; (void)Pool;
    return d3d8_CreateTextureImpl(Width, Height, Levels, Usage, Format, ppTexture);
}

static HRESULT __stdcall dev_CreateImageSurface(IDirect3DDevice8 *self, UINT Width, UINT Height, D3DFORMAT Format, IDirect3DSurface8 **ppSurface)
{
    (void)self;
    return d3d8_CreateImageSurfaceImpl(Width, Height, Format, ppSurface);
}

static HRESULT __stdcall dev_CreateCubeTexture(IDirect3DDevice8 *self, UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture8 **ppCubeTexture)
{
    (void)self; (void)Pool;
    return d3d8_CreateCubeTextureImpl(EdgeLength, Levels, Usage, Format, ppCubeTexture);
}

static HRESULT __stdcall dev_CreateVolumeTexture(IDirect3DDevice8 *self, UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture8 **ppVolumeTexture)
{
    (void)self; (void)Pool;
    return d3d8_CreateVolumeTextureImpl(Width, Height, Depth, Levels, Usage, Format, ppVolumeTexture);
}

static HRESULT __stdcall dev_CreateVertexBuffer(IDirect3DDevice8 *self, UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer8 **ppVertexBuffer)
{
    (void)self; (void)Pool;
    return d3d8_CreateVertexBufferImpl(Length, Usage, FVF, ppVertexBuffer);
}

static HRESULT __stdcall dev_CreateIndexBuffer(IDirect3DDevice8 *self, UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer8 **ppIndexBuffer)
{
    (void)self; (void)Pool;
    return d3d8_CreateIndexBufferImpl(Length, Usage, Format, ppIndexBuffer);
}

static HRESULT __stdcall dev_CreateRenderTarget(IDirect3DDevice8 *self, UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, BOOL Lockable, IDirect3DSurface8 **ppSurface)
{
    (void)self; (void)Lockable;
    RhiImageDesc td;
    RhiImage *img;
    UINT sample_count;

    if (!ppSurface || !Width || !Height) return E_INVALIDARG;

    sample_count = d3d8_msaa_sample_count(MultiSample);
    if (sample_count > 1) {
        RhiFormat fmt = d3d8_to_dxgi_format(Format);
        /* Validate against the host's limits; fall back gracefully. */
        if (!rhi_sample_count_supported(fmt, sample_count)) {
            UINT counts[] = { 9, 8, 4, 2, 1 };
            UINT i;
            for (i = 0; i < 5; i++) {
                if (counts[i] >= sample_count) continue;
                if (rhi_sample_count_supported(fmt, counts[i])) {
                    sample_count = counts[i];
                    break;
                }
            }
            if (sample_count > 1) {
                fprintf(stderr, "D3D8: MSAA %lu unsupported, falling back to %lu samples\n",
                        d3d8_msaa_sample_count(MultiSample) > 1 ? (unsigned long)d3d8_msaa_sample_count(MultiSample) : 0,
                        (unsigned long)sample_count);
            } else {
                sample_count = 1;
            }
        }
    }

    memset(&td, 0, sizeof(td));
    td.type = RHI_IMAGE_2D;
    td.width = Width;
    td.height = Height;
    td.depth = 1;
    td.mip_levels = 1;
    td.format = d3d8_to_dxgi_format(Format);
    td.samples = sample_count;
    td.sample_quality = 0;
    td.usage = RHI_USAGE_DEFAULT;
    td.bind = RHI_BIND_RENDER_TARGET | RHI_BIND_SAMPLED;

    img = rhi_image_create(&td, NULL);
    if (!img) {
        fprintf(stderr, "D3D8: CreateRenderTarget failed (fmt=0x%X %ux%u)\n", Format, Width, Height);
        return E_FAIL;
    }

    *ppSurface = d3d8_surface_create(img, 0, 0, Width, Height, Format,
                                     D3DPOOL_DEFAULT, D3DUSAGE_RENDERTARGET,
                                     MultiSample, NULL, 0);
    rhi_image_destroy(img);
    return *ppSurface ? S_OK : E_OUTOFMEMORY;
}

static HRESULT __stdcall dev_CreateDepthStencilSurface(IDirect3DDevice8 *self, UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, IDirect3DSurface8 **ppSurface)
{
    (void)self;
    RhiImageDesc td;
    RhiImage *img;
    RhiFormat host;
    D3DFORMAT surface_format = Format;
    UINT sample_count;

    if (!ppSurface || !Width || !Height) return E_INVALIDARG;

    host = d3d8_to_dxgi_format(Format);
    if (host == RHI_FORMAT_R16_FLOAT) {
        /* F16 has no host depth equivalent; fall back to D16. */
        fprintf(stderr, "D3D8: F16 depth surface falling back to D16\n");
        host = RHI_FORMAT_D16_UNORM;
        surface_format = D3DFMT_D16;
    }

    sample_count = d3d8_msaa_sample_count(MultiSample);
    if (sample_count > 1) {
        if (!rhi_sample_count_supported(host, sample_count)) {
            UINT counts[] = { 9, 8, 4, 2, 1 };
            UINT i;
            for (i = 0; i < 5; i++) {
                if (counts[i] >= sample_count) continue;
                if (rhi_sample_count_supported(host, counts[i])) {
                    sample_count = counts[i];
                    break;
                }
            }
            if (sample_count > 1)
                fprintf(stderr, "D3D8: depth MSAA %lu unsupported, fallback %lu samples\n",
                        (unsigned long)d3d8_msaa_sample_count(MultiSample),
                        (unsigned long)sample_count);
            else
                sample_count = 1;
        }
    }

    memset(&td, 0, sizeof(td));
    td.type = RHI_IMAGE_2D;
    td.width = Width;
    td.height = Height;
    td.depth = 1;
    td.mip_levels = 1;
    td.format = host;
    td.samples = sample_count;
    td.sample_quality = 0;
    td.usage = RHI_USAGE_DEFAULT;
    td.bind = RHI_BIND_DEPTH;

    img = rhi_image_create(&td, NULL);
    if (!img) {
        fprintf(stderr, "D3D8: CreateDepthStencilSurface failed (fmt=0x%X)\n", Format);
        return E_FAIL;
    }

    *ppSurface = d3d8_surface_create(img, 0, 0, Width, Height, surface_format,
                                     D3DPOOL_DEFAULT, D3DUSAGE_DEPTHSTENCIL,
                                     MultiSample, NULL, 0);
    rhi_image_destroy(img);
    return *ppSurface ? S_OK : E_OUTOFMEMORY;
}

/* D3D11 drops a shader resource view that aliases the render target being
 * bound, and never puts it back. That matters as soon as a title renders into
 * something it also samples -- an environment cube, rendered face by face
 * with the cube itself bound on a stage -- because the stage would read zero
 * for the rest of the frame. Every stage is re-applied around a target
 * switch: the one that aliases the new target is cleared deliberately, and
 * the others are restored.
 *
 * SetTexture is the only other place that binds these, so nothing else
 * reinstates them per draw. */
static void rebind_stage_srvs(RhiImage *target)
{
    DWORD stage;

    for (stage = 0; stage < MAX_TEXTURE_STAGES; stage++) {
        RhiView *srv = NULL;

        if (g_cur_textures[stage] &&
            d3d8_base_resource(g_cur_textures[stage]) != target)
            srv = d3d8_base_srv(g_cur_textures[stage]);
        rhi_set_textures(stage, 1, &srv);
    }
}

static HRESULT __stdcall dev_SetRenderTarget(IDirect3DDevice8 *self, IDirect3DSurface8 *pRenderTarget, IDirect3DSurface8 *pZStencilSurface)
{
    (void)self;
    RhiView *rtv;
    RhiView *dsv = NULL;
    D3D8Surface *rt = (D3D8Surface *)pRenderTarget;
    D3D8Surface *ds = (D3D8Surface *)pZStencilSurface;

    /* Refused before anything changes, so a bad call leaves the old targets
     * bound on both sides. NULL is the back buffer (not D3D8, which refuses
     * it); a NULL depth surface is no depth, as in D3D8 -- the device's own
     * is the surface GetDepthStencilSurface returns at creation. */
    if (rt && !rt->rtv) {
        /* Said with the surface's shape the first few times and then
         * counted: Outrun 2 hits this once a frame for a cube face, and a
         * line a frame on the swapping thread is a cost of its own. */
        static unsigned refused;
        refused++;
        if (refused <= 3 || (refused % 1000) == 0)
            fprintf(stderr, "D3D8: SetRenderTarget on non-renderable surface: "
                    "%ux%u format %d usage 0x%X subresource %u (%u time(s))\n",
                    rt->width, rt->height, (int)rt->format, (unsigned)rt->usage,
                    rt->subresource, refused);
        return E_INVALIDARG;
    }
    if (ds && !ds->dsv) {
        fprintf(stderr, "D3D8: SetRenderTarget with non-depth stencil surface\n");
        return E_INVALIDARG;
    }
    /* D3D11 ignores a binding whose depth is not the target's size, which
     * would leave draws going to the previous target. */
    if (ds && (ds->width  != (rt ? rt->width  : g_device_state.width) ||
               ds->height != (rt ? rt->height : g_device_state.height))) {
        fprintf(stderr, "D3D8: SetRenderTarget with depth %ux%u for a %ux%u target\n",
                ds->width, ds->height,
                rt ? rt->width : g_device_state.width,
                rt ? rt->height : g_device_state.height);
        return E_INVALIDARG;
    }
    if (rt) IDirect3DSurface8_AddRef(pRenderTarget);
    if (ds) IDirect3DSurface8_AddRef(pZStencilSurface);
    if (g_cur_rt) IDirect3DSurface8_Release(&g_cur_rt->iface);
    if (g_cur_ds) IDirect3DSurface8_Release(&g_cur_ds->iface);
    g_cur_rt = rt;
    g_cur_ds = ds;

    rtv = rt ? rt->rtv : g_device_state.rhi_default_rtv;
    dsv = ds ? ds->dsv : NULL;
    rhi_set_render_target(rtv, dsv);
    rebind_stage_srvs(rt ? rt->image : NULL);
    return S_OK;
}

static HRESULT __stdcall dev_GetRenderTarget(IDirect3DDevice8 *self, IDirect3DSurface8 **ppRenderTarget)
{
    (void)self;
    if (!ppRenderTarget) return E_INVALIDARG;
    if (g_cur_rt) {
        *ppRenderTarget = &g_cur_rt->iface;
        IDirect3DSurface8_AddRef(*ppRenderTarget);
    } else {
        *ppRenderTarget = NULL;
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetDepthStencilSurface(IDirect3DDevice8 *self, IDirect3DSurface8 **ppZStencilSurface)
{
    (void)self;
    if (!ppZStencilSurface) return E_INVALIDARG;
    if (g_cur_ds) {
        *ppZStencilSurface = &g_cur_ds->iface;
        IDirect3DSurface8_AddRef(*ppZStencilSurface);
    } else {
        *ppZStencilSurface = NULL;
    }
    return S_OK;
}

static void apply_host_viewport(void)
{
    const D3DVIEWPORT8 *vp = &g_device_state.viewport;
    float sx = rt_scale_x(), sy = rt_scale_y();
    float k, cx;
    RhiViewport hv;

    if (!rhi_device_ready())
        return;

    hv.x         = (FLOAT)vp->X * sx;
    hv.y         = (FLOAT)vp->Y * sy;
    hv.width     = (FLOAT)vp->Width * sx;
    hv.height    = (FLOAT)vp->Height * sy;
    hv.min_depth = vp->MinZ;
    hv.max_depth = vp->MaxZ;

    /* In widescreen the whole scene is stretched horizontally at present,
     * which is right for geometry that went through a widened projection
     * and wrong for everything else. Squeezing the 2D viewport by the same
     * factor, about the middle of the scene, means the stretch puts it
     * back: a HUD laid out for 4:3 keeps its proportions and sits in the
     * centre. Nothing is resampled twice -- the squeeze is a viewport, so
     * the draw is simply rasterised narrower. The scissor follows it, in
     * d3d8_GetScissor. */
    if (two_d_squeeze(&k, &cx)) {
        hv.x = cx + (hv.x - cx) * k;
        hv.width *= k;
    }

    rhi_set_viewports(1, &hv);
}

/* Called once per draw, from the shader path that knows which kind of
 * draw it is. Cheap when nothing changes, which is the common case: a
 * frame is a run of 3D draws and then a run of 2D ones. */
void d3d8_SetTwoDSqueeze(BOOL on)
{
    g_2d_screen_space = on ? TRUE : FALSE;
    if (!d3d8_display_wide_now() || d3d8_display_title_native_wide())
        on = FALSE;
    if (g_2d_squeeze == (on ? TRUE : FALSE))
        return;
    g_2d_squeeze = on ? TRUE : FALSE;
    apply_host_viewport();
}

BOOL d3d8_GetTwoDSqueeze(void) { return g_2d_squeeze; }

/* The horizontal extent a screen-space draw can reach, in the title's own
 * screen pixels: its vertices, clipped by the scissor. FALSE when the
 * positions cannot be found.
 *
 * The vertices are the ones handed to the draw: everything reaches this
 * device as DrawPrimitiveUP, including the draws the title sourced from
 * a vertex buffer -- the HLE reads those itself and passes the bytes.
 * Position sits at the offset the program's own declaration gives for
 * the register it copies oPos from. */
/* Whether the last draw_extent measured a fixed-function draw. */
static BOOL g_extent_fixed_function;

static BOOL draw_extent(const void *vertices, UINT stride, UINT count,
                        float *lo_out, float *hi_out)
{
    const unsigned char *p = (const unsigned char *)vertices;
    UINT offset = 0, i;
    DWORD handle = g_device_state.vertex_shader;
    float lo = 3.4e38f, hi = -3.4e38f;

    g_extent_fixed_function = FALSE;
    if (!p || !stride || !count)
        return FALSE;
    if (d3d8_vsh_is_programmable(handle)) {
        int reg = d3d8_vsh_bound_pos_input();

        if (reg < 0 || !d3d8_vsh_bound_input_offset(reg, &offset))
            return FALSE;
    } else if (d3d8_fvf_transformed(handle)) {
        /* Fixed function: the handle is the FVF, and a pre-transformed
         * vertex starts with its screen position. Asking the program
         * helpers instead answered for whichever program was bound last,
         * and usually not at all -- so a fixed-function backdrop never
         * escaped the squeeze: OutRun 2's menus came out as a 4:3 box with
         * the clear colour at the sides once its user-pointer 2D was
         * squeezed (49da008). */
        offset = 0;
        g_extent_fixed_function = TRUE;
    } else {
        return FALSE;
    }
    if (offset + sizeof(float) > stride)
        return FALSE;

    /* Every vertex, not a sample of them. A backdrop is not always a
     * quad -- this title builds several out of long strips -- and
     * measuring only the first few of those reported an extent that
     * stopped short of the edge, so the draw was squeezed and the seam
     * it was supposed to remove stayed. The bound is a sanity limit, not
     * a sample: screen-space draws are small, and this runs only for
     * them. */
    if (count > 8192)
        count = 8192;

    for (i = 0; i < count; i++) {
        float x;

        memcpy(&x, p + (size_t)i * stride + offset, sizeof x);
        if (x < lo) lo = x;
        if (x > hi) hi = x;
    }

    /* What the draw can reach, not what it is: a quad the size of the
     * screen under a scissor the size of a menu box is that box's
     * background. TimeSplitters 2 fills its difficulty list that way, and
     * measured unclipped it passed for a backdrop, stayed unsqueezed, and
     * showed as a dark panel beside the squeezed list. Both are in the
     * title's own screen pixels. */
    if (g_scissor_enabled && !g_cur_rt) {
        if ((float)g_scissor.left > lo)  lo = (float)g_scissor.left;
        if ((float)g_scissor.right < hi) hi = (float)g_scissor.right;
    }

    *lo_out = lo;
    *hi_out = hi;
    return TRUE;
}

/* Whether the bound textures make a draw a pass over the whole screen
 * rather than a picture placed on it: stage 0 is nothing or a single
 * texel (flat colour), or some stage holds a copy of the frame.
 *
 * Only stage 0 is asked about pictures. A title leaves textures bound on
 * stages its shader never reads, so a later stage holding something
 * large says nothing; TimeSplitters 2's glow passes, for one, have the
 * frame on stage 0 and a 1x1 white texture on stage 1. */
static BOOL draw_is_screen_pass(void)
{
    IDirect3DBaseTexture8 *tex;
    UINT w = 0, h = 0;
    DWORD s;

    for (s = 0; s < 4; s++) {
        tex = d3d8_GetStageTexture(s);
        if (tex && IDirect3DBaseTexture8_GetType(tex) == D3DRTYPE_TEXTURE &&
            ((D3D8Texture *)tex)->screen_copy)
            return TRUE;
    }
    tex = d3d8_GetStageTexture(0);
    if (!tex)
        return TRUE;
    return d3d8_base_size(tex, &w, &h) && w <= 1 && h <= 1;
}

/* Does this draw cover the guest's full width, and so escape the 2D
 * squeeze?
 *
 * A screen-space draw that spans the screen is a backdrop, a fade, a
 * letterbox bar or a video frame, and squeezing it leaves the sides of a
 * widescreen picture showing whatever was behind. One that does not is a
 * HUD element, which belongs in the centred box. The difference is the
 * draw's own extent (draw_extent), so measure it rather than keep a list
 * of elements -- unless the title keeps the list (xbox_D3D8SetTwoDPlacement).
 *
 * When the extent is unavailable the answer is FALSE, which keeps the
 * squeeze. That is the safe way to be wrong: a squeezed backdrop is
 * visibly odd in one place, a stretched HUD is subtly wrong everywhere.
 */
BOOL d3d8_draw_escapes_squeeze(const void *vertices, UINT stride, UINT count)
{
    UINT guest_w = d3d8_GetGuestWidth();
    float lo, hi;

    if (!guest_w || !draw_extent(vertices, stride, count, &lo, &hi))
        return FALSE;

    /* How much of the width the draw covers, in the title's own screen
     * pixels. Coverage rather than "touches both edges": this title
     * builds its backdrop from overlapping strips, and the ones that run
     * off the left edge stop a little short of the right. Measured on a
     * menu the pieces fall into two groups with a gap between them:
     * backdrop from 77% of the width upwards, and everything smaller at
     * 70% and below. The threshold sits in that gap.
     *
     * It was 85% first, which split the backdrop rather than separating
     * it from the HUD: the widest strips passed and the rest did not, so
     * the tiling came apart and left an edge partway across where one
     * stopped and the squeezed next one began. A backdrop only looks
     * right if all of it is treated the same way.
     *
     * 75% is a measurement of one title's menu, not a principle, and the
     * gap it sits in is about six points wide. Another title may not
     * leave one.
     *
     * With centre_2d that measurement is not trusted at all. The same
     * title's menus still came apart at it: a glow drawn over the right
     * third of the screen was squeezed off the hex backdrop it was painted
     * to meet, leaving an edge, and a header that animates across the
     * threshold flickered between the two treatments. So there only a
     * whole-screen pass escapes: one covering the full width that is flat
     * colour -- a fade, a tint, a letterbox bar -- or that draws the frame
     * itself back over the frame, which squeezed would put a shrunken copy
     * of the picture in the middle of it. */
    if (d3d8_display_policy()->centre_2d)
        return ((hi - lo) >= (float)guest_w * 0.98f && draw_is_screen_pass())
               ? TRUE : FALSE;
    /* A fixed-function draw escapes only if it covers the whole width. The
     * 75% gap above was measured on program draws; OutRun 2 batches HUD
     * pieces into fixed-function draws, and one holding part of the score
     * spans more than 75% -- at that threshold it escaped and split the
     * score's digits across the screen. Its menu backdrop covers it all. */
    if (g_extent_fixed_function)
        return ((hi - lo) >= (float)guest_w * 0.98f) ? TRUE : FALSE;
    return ((hi - lo) >= (float)guest_w * 0.75f) ? TRUE : FALSE;
}

/* RECOMP_D3D8_2D_TAGS=1: every 120 frames, each placement tag the title
 * set, with its screen-space draws and the extent they reached (clipped by
 * the scissor, in the title's pixels). This is how a title project builds
 * its placement table: it tags draws with the code that made them, and
 * this says what each tag actually drew. */
typedef struct TwoDTagStat {
    uint32_t      tag;
    int           placement;
    unsigned long draws;
    float         lo, hi;       /* everything the tag drew in the window */
    float         widest;       /* its widest single draw */
} TwoDTagStat;

static TwoDTagStat   g_tag_stats[256];
static int           g_tag_count;
static int           g_tag_log = -1;
static unsigned long g_tag_frames;

static int two_d_tag_log(void)
{
    if (g_tag_log < 0) {
        const char *v = getenv("RECOMP_D3D8_2D_TAGS");
        g_tag_log = v && *v && strcmp(v, "0") != 0;
    }
    return g_tag_log;
}

static void two_d_tag_note(const void *vertices, UINT stride, UINT count)
{
    TwoDTagStat *t = NULL;
    float lo, hi;
    int i;

    for (i = 0; i < g_tag_count; i++)
        if (g_tag_stats[i].tag == g_2d_tag && g_tag_stats[i].placement == g_2d_placement) {
            t = &g_tag_stats[i];
            break;
        }
    if (!t) {
        if (g_tag_count == (int)(sizeof g_tag_stats / sizeof g_tag_stats[0]))
            return;
        t = &g_tag_stats[g_tag_count++];
        t->tag = g_2d_tag;
        t->placement = g_2d_placement;
        t->draws = 0;
        t->lo = 3.4e38f;
        t->hi = -3.4e38f;
        t->widest = 0.0f;
    }
    t->draws++;
    if (draw_extent(vertices, stride, count, &lo, &hi)) {
        if (lo < t->lo) t->lo = lo;
        if (hi > t->hi) t->hi = hi;
        if (hi - lo > t->widest) t->widest = hi - lo;
    }
}

static void two_d_tag_frame(void)
{
    static const char *names[] = { "auto", "stretch", "centre", "left", "right", "side" };
    UINT guest_w = d3d8_GetGuestWidth();
    int i;

    if (g_tag_log <= 0 || ++g_tag_frames % 120 != 0)
        return;
    fprintf(stderr, "[2D-TAGS] frame %lu: %d tag(s)\n", g_tag_frames, g_tag_count);
    for (i = 0; i < g_tag_count; i++) {
        const TwoDTagStat *t = &g_tag_stats[i];
        int pl = t->placement >= 0 && t->placement <= XBOX_D3D8_2D_SIDE ? t->placement : 0;

        fprintf(stderr, "[2D-TAGS]   tag %08X %-7s draws %6lu  x %7.1f..%7.1f (%5.1f%%) "
                "widest %5.1f%%\n", t->tag, names[pl], t->draws, t->lo, t->hi,
                t->hi >= t->lo && guest_w ? (t->hi - t->lo) * 100.0f / (float)guest_w : 0.0f,
                guest_w ? t->widest * 100.0f / (float)guest_w : 0.0f);
    }
    fflush(stderr);
    g_tag_count = 0;
}

void xbox_D3D8SetTwoDPlacement(int placement, uint32_t tag)
{
    int anchor;

    if (placement < XBOX_D3D8_2D_AUTO || placement > XBOX_D3D8_2D_SIDE)
        placement = XBOX_D3D8_2D_AUTO;
    g_2d_tag = tag;
    if (placement == g_2d_placement)
        return;
    g_2d_placement = placement;
    anchor = placement == XBOX_D3D8_2D_LEFT || placement == XBOX_D3D8_2D_RIGHT
             ? placement : XBOX_D3D8_2D_CENTRE;
    if (anchor == g_2d_anchor)
        return;
    g_2d_anchor = anchor;
    /* The anchor moved: a squeezed viewport already in place has to follow.
     * The scissor is recomputed at the next draw's state apply. */
    if (g_2d_squeeze)
        apply_host_viewport();
}

/* SIDE: the edge this draw is nearer, from where it sits in the title's
 * own pixels (clipped by the scissor). Thirds rather than halves, so that
 * a piece spanning the middle -- a message, a centred counter -- stays in
 * the middle. */
static int two_d_side_anchor(const void *vertices, UINT stride, UINT count)
{
    UINT guest_w = d3d8_GetGuestWidth();
    float lo, hi, mid;

    if (!guest_w || !draw_extent(vertices, stride, count, &lo, &hi))
        return XBOX_D3D8_2D_CENTRE;
    mid = (lo + hi) * 0.5f;
    if (mid < (float)guest_w / 3.0f)
        return XBOX_D3D8_2D_LEFT;
    if (mid > (float)guest_w * 2.0f / 3.0f)
        return XBOX_D3D8_2D_RIGHT;
    return XBOX_D3D8_2D_CENTRE;
}

int xbox_D3D8GetTwoDPlacement(uint32_t *tag)
{
    if (tag)
        *tag = g_2d_tag;
    return g_2d_placement;
}

/* Once per screen-space draw, after the shader path has said it is one:
 * where it goes in widescreen. The title's placement when it gave one,
 * otherwise the renderer's judgement from the draw's extent. */
void d3d8_place_2d_draw(const void *vertices, UINT stride, UINT count)
{
    if (!g_2d_screen_space)
        return;
    if (two_d_tag_log())
        two_d_tag_note(vertices, stride, count);
    if (!g_2d_squeeze)
        return;
    switch (g_2d_placement) {
    case XBOX_D3D8_2D_STRETCH:
        d3d8_SetTwoDSqueeze(FALSE);
        break;
    case XBOX_D3D8_2D_CENTRE:
    case XBOX_D3D8_2D_LEFT:
    case XBOX_D3D8_2D_RIGHT:
        break;
    case XBOX_D3D8_2D_SIDE: {
        int anchor = two_d_side_anchor(vertices, stride, count);

        if (anchor != g_2d_anchor) {
            g_2d_anchor = anchor;
            apply_host_viewport();
        }
        break;
    }
    default:
        if (d3d8_draw_escapes_squeeze(vertices, stride, count))
            d3d8_SetTwoDSqueeze(FALSE);
        break;
    }
}

static HRESULT __stdcall dev_SetViewport(IDirect3DDevice8 *self, const D3DVIEWPORT8 *pViewport)
{
    (void)self;
    if (pViewport) {
        g_device_state.viewport = *pViewport;
        apply_host_viewport();
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetViewport(IDirect3DDevice8 *self, D3DVIEWPORT8 *pViewport)
{
    (void)self;
    if (pViewport) *pViewport = g_device_state.viewport;
    return S_OK;
}

static HRESULT __stdcall dev_SetMaterial(IDirect3DDevice8 *self, const D3DMATERIAL8 *pMaterial)
{
    (void)self;
    if (pMaterial) g_device_state.material = *pMaterial;
    return S_OK;
}

static HRESULT __stdcall dev_GetMaterial(IDirect3DDevice8 *self, D3DMATERIAL8 *pMaterial)
{
    (void)self;
    if (pMaterial) *pMaterial = g_device_state.material;
    return S_OK;
}

static HRESULT __stdcall dev_SetLight(IDirect3DDevice8 *self, DWORD Index, const D3DLIGHT8 *pLight)
{
    (void)self;
    if (Index < MAX_LIGHTS && pLight) g_device_state.lights[Index] = *pLight;
    return S_OK;
}

static HRESULT __stdcall dev_GetLight(IDirect3DDevice8 *self, DWORD Index, D3DLIGHT8 *pLight)
{
    (void)self;
    if (Index < MAX_LIGHTS && pLight) *pLight = g_device_state.lights[Index];
    return S_OK;
}

static HRESULT __stdcall dev_LightEnable(IDirect3DDevice8 *self, DWORD Index, BOOL Enable)
{
    (void)self;
    if (Index < MAX_LIGHTS) g_device_state.light_enable[Index] = Enable;
    return S_OK;
}

static HRESULT __stdcall dev_CreateVertexShader(IDirect3DDevice8 *self, const DWORD *pDeclaration, const DWORD *pFunction, DWORD *pHandle, DWORD Usage)
{
    (void)self; (void)pDeclaration; (void)Usage;
    if (!pHandle) return E_INVALIDARG;
    if (!pFunction) return E_INVALIDARG;
    /* Count instructions: each is 4 DWORDs, last has bit 0 of word[3] set (END flag) */
    {
        int i, num_insns = 0;
        for (i = 0; i < 136; i++) {
            num_insns++;
            if (pFunction[i * 4 + 3] & 1) break;  /* END bit in last word */
        }
        return d3d8_vsh_create_shader(pFunction, num_insns, pHandle);
    }
}

static HRESULT __stdcall dev_SetVertexShader(IDirect3DDevice8 *self, DWORD Handle)
{
    (void)self;
    g_device_state.vertex_shader = Handle;
    return S_OK;
}

static HRESULT __stdcall dev_GetVertexShader(IDirect3DDevice8 *self, DWORD *pHandle)
{
    (void)self;
    if (pHandle) *pHandle = g_device_state.vertex_shader;
    return S_OK;
}

static HRESULT __stdcall dev_SetVertexShaderConstant(IDirect3DDevice8 *self, INT Register, const void *pConstantData, DWORD ConstantCount)
{
    (void)self;
    d3d8_vsh_set_constant(Register, pConstantData, ConstantCount);
    return S_OK;
}

static HRESULT __stdcall dev_SetPixelShader(IDirect3DDevice8 *self, DWORD Handle)
{
    (void)self;
    g_device_state.pixel_shader = Handle;
    d3d8_combiners_set_pixel_shader(Handle);
    return S_OK;
}

static HRESULT __stdcall dev_GetPixelShader(IDirect3DDevice8 *self, DWORD *pHandle)
{
    (void)self;
    if (pHandle) *pHandle = g_device_state.pixel_shader;
    return S_OK;
}

static HRESULT __stdcall dev_SetPixelShaderConstant(IDirect3DDevice8 *self, INT Register, const void *pConstantData, DWORD ConstantCount)
{
    (void)self; (void)Register; (void)pConstantData; (void)ConstantCount;
    return S_OK;
}

static void __stdcall dev_SetGammaRamp(IDirect3DDevice8 *self, DWORD Flags, const D3DGAMMARAMP *pRamp)
{
    (void)self; (void)Flags; (void)pRamp;
}

static void __stdcall dev_GetGammaRamp(IDirect3DDevice8 *self, D3DGAMMARAMP *pRamp)
{
    (void)self; (void)pRamp;
}

static HRESULT __stdcall dev_SetPalette(IDirect3DDevice8 *self, DWORD PaletteNumber, const void *pEntries)
{
    (void)self;
    if (PaletteNumber >= D3D8_MAX_PALETTES) return E_INVALIDARG;
    d3d8_SetPalette(PaletteNumber, (const DWORD *)pEntries);

    /* A P8 texture bound to this stage bakes the palette at upload; re-bake
     * the raw indices so animated / colorized palettes take effect. */
    if (PaletteNumber < 4) {
        IDirect3DBaseTexture8 *tex = d3d8_GetStageTexture(PaletteNumber);
        if (tex) d3d8_refresh_palette(tex);
    }
    return S_OK;
}

static HRESULT __stdcall dev_BeginPush(IDirect3DDevice8 *self, DWORD Count, DWORD **ppPush)
{
    (void)self; (void)Count; (void)ppPush;
    /* TODO: Xbox push buffer emulation */
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_EndPush(IDirect3DDevice8 *self, DWORD *pPush)
{
    (void)self; (void)pPush;
    return E_NOTIMPL;
}

/* DXGI sync interval for Swap. 1 waits for vertical blank, which is right
 * when this device is the title's display. A device run beside the title's
 * own D3D8 (hle_d3d8.c shadow mode) must not block: the title paces its
 * loader on frames presented, so a vsync wait here throttled Burnout 2 from
 * about 136 frames a second to 27 and cut how far a run got by two thirds. */
static UINT g_present_interval = 1;

void xbox_D3D8SetPresentInterval(UINT interval)
{
    g_present_interval = interval;
}

/* xbox_D3D8RequestScale: the resolution scale, changed while the title runs.
 * Only the scene target and the device's own depth buffer are scaled (see
 * rt_scale_x), so a new scale is those two made again at the new size,
 * between one frame's present and the next frame's first draw:
 *   - the scene's image and views are replaced in the device state, and
 *     everything that sizes itself from it follows on its own -- viewport
 *     and scissor through rt_scale_x, the screen copy and the resolve per
 *     call, frame interpolation's second image when it next starts;
 *   - the depth surface object the title was given keeps its identity (the
 *     HLE holds it, host_DeviceDepthSurface) and has its image and view
 *     traded with a new surface's, whose release then frees the old ones.
 * A failure to make the new targets leaves the old ones, and says so. */
static volatile LONG g_scale_wanted;

void xbox_D3D8RequestScale(UINT scale)
{
    if (scale >= 1 && scale <= 8)
        InterlockedExchange(&g_scale_wanted, (LONG)scale);
}

static void follow_scale(void)
{
    D3D8DeviceState *s = &g_device_state;
    LONG want = InterlockedCompareExchange(&g_scale_wanted, 0, 0);
    RhiImageDesc sd;
    RhiImage *img = NULL, *dimg = NULL;
    RhiView *rtv = NULL, *srv = NULL, *dview = NULL;
    D3D8Surface *fresh = NULL;
    UINT w, h;

    if (!want || g_interp_on || !rhi_device_ready() || !s->present_width)
        return;
    InterlockedExchange(&g_scale_wanted, 0);
    if ((UINT)want * s->present_width == s->width && (UINT)want * s->present_height == s->height)
        return;
    w = s->present_width * (UINT)want;
    h = s->present_height * (UINT)want;

    memset(&sd, 0, sizeof sd);
    sd.type = RHI_IMAGE_2D;
    sd.width = w;
    sd.height = h;
    sd.depth = 1;
    sd.mip_levels = 1;
    sd.format = RHI_FORMAT_R8G8B8A8_UNORM;
    sd.samples = 1;
    sd.usage = RHI_USAGE_DEFAULT;
    sd.bind = RHI_BIND_RENDER_TARGET | RHI_BIND_SAMPLED;
    img = rhi_image_create(&sd, NULL);
    if (img)
        rtv = rhi_view_create(img, RHI_VIEW_RENDER_TARGET, NULL);
    if (rtv)
        srv = rhi_view_create(img, RHI_VIEW_SAMPLED, NULL);
    sd.format = RHI_FORMAT_D24_UNORM_S8_UINT;
    sd.bind = RHI_BIND_DEPTH;
    if (srv)
        dimg = rhi_image_create(&sd, NULL);
    if (dimg)
        dview = rhi_view_create(dimg, RHI_VIEW_DEPTH, NULL);
    if (dview && g_auto_ds)
        fresh = (D3D8Surface *)d3d8_surface_create(dimg, 0, 0, w, h, D3DFMT_D24S8,
                                                   D3DPOOL_DEFAULT, D3DUSAGE_DEPTHSTENCIL,
                                                   D3DMULTISAMPLE_NONE, NULL, 0);
    if (!dview || (g_auto_ds && (!fresh || !fresh->dsv))) {
        fprintf(stderr, "D3D8 display: the %ux%u targets for %ldx could not be made; "
                "staying at %ux%u\n", w, h, (long)want, s->width, s->height);
        if (fresh) IDirect3DSurface8_Release(&fresh->iface);
        rhi_view_destroy(dview);
        rhi_image_destroy(dimg);
        rhi_view_destroy(srv);
        rhi_view_destroy(rtv);
        rhi_image_destroy(img);
        return;
    }

    rhi_set_render_target(NULL, NULL);
    {
        RhiImage *old_img = s->rhi_scene_image, *old_dimg = s->depth_image;
        RhiView *old_rtv = s->rhi_default_rtv, *old_srv = s->rhi_scene_srv;
        RhiView *old_dview = s->depth_view;

        s->rhi_scene_image = img;
        s->rhi_default_rtv = rtv;
        s->rhi_scene_srv = srv;
        s->depth_image = dimg;
        s->depth_view = dview;
        s->width = w;
        s->height = h;
        if (g_auto_ds) {
            RhiImage *ti = g_auto_ds->image;
            RhiView *tv = g_auto_ds->dsv;

            g_auto_ds->image = fresh->image;
            g_auto_ds->dsv = fresh->dsv;
            g_auto_ds->width = w;
            g_auto_ds->height = h;
            fresh->image = ti;
            fresh->dsv = tv;
            IDirect3DSurface8_Release(&fresh->iface);   /* the old depth goes with it */
        }
        rhi_view_destroy(old_srv);
        rhi_view_destroy(old_rtv);
        rhi_image_destroy(old_img);
        rhi_view_destroy(old_dview);
        rhi_image_destroy(old_dimg);
    }
    d3d8_display_set_scale((UINT)want);
    rhi_set_render_target(g_cur_rt ? g_cur_rt->rtv : s->rhi_default_rtv,
                          g_cur_ds ? g_cur_ds->dsv : NULL);
    apply_host_viewport();
    fprintf(stderr, "D3D8 display: rendering at %ldx now, %ux%u\n", (long)want, w, h);
    fflush(stderr);
}

static HRESULT __stdcall dev_Swap(IDirect3DDevice8 *self, DWORD Flags)
{
    HRESULT hr;
    (void)self; (void)Flags;

    /* Pump Windows messages (same as dev_Present) */
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            ExitProcess(0);
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    present_resolve();
    hr = (HRESULT)rhi_present(g_present_interval);
    follow_scale();
    return hr;
}

/* ================================================================
 * Frame interpolation's target (src/hle/hle_d3d8_interp.c)
 *
 * An in-between frame is drawn while the title's finished frame waits for
 * its vblank, so it cannot go to the scene target: that still holds the
 * frame about to be shown. Between Begin and End, "the back buffer" to
 * everything here -- SetRenderTarget(NULL), a screen copy, the resolve --
 * is a second image of the same size. The device depth buffer is shared:
 * the title's frame is finished with it, and the in-between frame starts
 * with the same clears that frame did.
 * ================================================================ */

static RhiImage *g_interp_image;
static RhiView  *g_interp_rtv, *g_interp_srv;
static UINT      g_interp_w, g_interp_h;
static BOOL      g_interp_on;

static void interp_target_destroy(void)
{
    rhi_view_destroy(g_interp_srv); g_interp_srv = NULL;
    rhi_view_destroy(g_interp_rtv); g_interp_rtv = NULL;
    rhi_image_destroy(g_interp_image); g_interp_image = NULL;
}

/* Trade the scene target for the other image, and rebind it if the back
 * buffer is what is bound. */
static void interp_target_swap(void)
{
    D3D8DeviceState *s = &g_device_state;
    RhiImage *image = s->rhi_scene_image;
    RhiView *rtv = s->rhi_default_rtv, *srv = s->rhi_scene_srv;

    s->rhi_scene_image = g_interp_image;
    s->rhi_default_rtv = g_interp_rtv;
    s->rhi_scene_srv = g_interp_srv;
    g_interp_image = image;
    g_interp_rtv = rtv;
    g_interp_srv = srv;
    if (!g_cur_rt)
        rhi_set_render_target(s->rhi_default_rtv, g_cur_ds ? g_cur_ds->dsv : NULL);
}

BOOL xbox_D3D8InterpBegin(void)
{
    D3D8DeviceState *s = &g_device_state;

    if (g_interp_on || !rhi_device_ready() || !s->rhi_scene_image)
        return FALSE;
    if (g_interp_image && (g_interp_w != s->width || g_interp_h != s->height))
        interp_target_destroy();
    if (!g_interp_image) {
        RhiImageDesc sd;

        memset(&sd, 0, sizeof sd);
        sd.type = RHI_IMAGE_2D;
        sd.width = s->width;
        sd.height = s->height;
        sd.depth = 1;
        sd.mip_levels = 1;
        sd.format = RHI_FORMAT_R8G8B8A8_UNORM;
        sd.samples = 1;
        sd.usage = RHI_USAGE_DEFAULT;
        sd.bind = RHI_BIND_RENDER_TARGET | RHI_BIND_SAMPLED;
        g_interp_image = rhi_image_create(&sd, NULL);
        if (g_interp_image)
            g_interp_rtv = rhi_view_create(g_interp_image, RHI_VIEW_RENDER_TARGET, NULL);
        if (g_interp_rtv)
            g_interp_srv = rhi_view_create(g_interp_image, RHI_VIEW_SAMPLED, NULL);
        if (!g_interp_srv) {
            interp_target_destroy();
            return FALSE;
        }
        g_interp_w = s->width;
        g_interp_h = s->height;
    }
    interp_target_swap();
    g_interp_on = TRUE;
    return TRUE;
}

/* The resolve and the present, without counting a frame: the title drew
 * none. */
void xbox_D3D8InterpPresent(void)
{
    if (!g_interp_on)
        return;
    follow_vrr();
    present_scene();
    rhi_present(g_present_interval);
}

void xbox_D3D8InterpEnd(void)
{
    if (!g_interp_on)
        return;
    interp_target_swap();
    g_interp_on = FALSE;
}

/* ================================================================
 * Vtable
 * ================================================================ */

static const IDirect3DDevice8Vtbl g_device_vtbl = {
    dev_QueryInterface,
    dev_AddRef,
    dev_Release,
    dev_GetDirect3D,
    dev_GetDeviceCaps,
    dev_GetDisplayMode,
    dev_GetCreationParameters,
    dev_Reset,
    dev_Present,
    dev_GetBackBuffer,
    dev_BeginScene,
    dev_EndScene,
    dev_Clear,
    dev_SetTransform,
    dev_GetTransform,
    dev_SetRenderState,
    dev_GetRenderState,
    dev_SetTextureStageState,
    dev_GetTextureStageState,
    dev_SetTexture,
    dev_GetTexture,
    dev_SetStreamSource,
    dev_GetStreamSource,
    dev_SetIndices,
    dev_GetIndices,
    dev_DrawPrimitive,
    dev_DrawIndexedPrimitive,
    dev_DrawPrimitiveUP,
    dev_DrawIndexedPrimitiveUP,
    dev_CreateTexture,
    dev_CreateVertexBuffer,
    dev_CreateIndexBuffer,
    dev_CreateRenderTarget,
    dev_CreateDepthStencilSurface,
    dev_SetRenderTarget,
    dev_GetRenderTarget,
    dev_GetDepthStencilSurface,
    dev_SetViewport,
    dev_GetViewport,
    dev_SetMaterial,
    dev_GetMaterial,
    dev_SetLight,
    dev_GetLight,
    dev_LightEnable,
    dev_SetVertexShader,
    dev_GetVertexShader,
    dev_SetVertexShaderConstant,
    dev_SetPixelShader,
    dev_GetPixelShader,
    dev_SetPixelShaderConstant,
    dev_SetGammaRamp,
    dev_GetGammaRamp,
    dev_SetPalette,
    dev_BeginPush,
    dev_EndPush,
    dev_Swap,
    /* Extensions appended by xboxrecomp (slots after the historical ones) */
    dev_CreateImageSurface,
    dev_CreateCubeTexture,
    dev_CreateVolumeTexture,
};

/* ================================================================
 * Public API
 * ================================================================ */

IDirect3DDevice8 *xbox_GetD3DDevice(void)
{
    return g_device_initialized ? &g_device : NULL;
}

/* ================================================================
 * IDirect3D8 factory implementation
 * ================================================================ */

static IDirect3D8 g_d3d8;
static LONG g_d3d8_ref = 0;

static HRESULT __stdcall d3d8_QueryInterface(IDirect3D8 *self, const IID *riid, void **ppv)
{
    (void)self; (void)riid; (void)ppv;
    return E_NOINTERFACE;
}

static ULONG __stdcall d3d8_AddRef(IDirect3D8 *self)
{
    (void)self;
    return (ULONG)InterlockedIncrement(&g_d3d8_ref);
}

static ULONG __stdcall d3d8_Release(IDirect3D8 *self)
{
    (void)self;
    return (ULONG)InterlockedDecrement(&g_d3d8_ref);
}

static HRESULT __stdcall d3d8_CreateDevice(IDirect3D8 *self, UINT Adapter, DWORD DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPP, IDirect3DDevice8 **ppDevice)
{
    (void)self; (void)Adapter; (void)DeviceType; (void)BehaviorFlags;
    HRESULT hr;

    if (!pPP || !ppDevice) return E_INVALIDARG;

    memset(&g_device_state, 0, sizeof(g_device_state));
    g_device_state.ref_count = 1;

    if (!pPP->hDeviceWindow) pPP->hDeviceWindow = hFocusWindow;

    hr = create_device_and_swap_chain(&g_device_state, pPP);
    if (FAILED(hr)) return hr;

    hr = create_render_targets(&g_device_state);
    if (FAILED(hr)) return hr;

    /* The device's own depth buffer, bound as the current depth surface.
     * Anything a previous device left behind goes first. */
    if (g_cur_rt) { IDirect3DSurface8_Release(&g_cur_rt->iface); g_cur_rt = NULL; }
    if (g_cur_ds) { IDirect3DSurface8_Release(&g_cur_ds->iface); g_cur_ds = NULL; }
    if (g_auto_ds) { IDirect3DSurface8_Release(&g_auto_ds->iface); g_auto_ds = NULL; }
    g_auto_ds = (D3D8Surface *)d3d8_surface_create(g_device_state.depth_image, 0, 0,
                                                   g_device_state.width,
                                                   g_device_state.height,
                                                   D3DFMT_D24S8, D3DPOOL_DEFAULT,
                                                   D3DUSAGE_DEPTHSTENCIL,
                                                   D3DMULTISAMPLE_NONE, NULL, 0);
    if (!g_auto_ds || !g_auto_ds->dsv) {
        fprintf(stderr, "D3D8: device depth surface could not be wrapped\n");
        if (g_auto_ds) { IDirect3DSurface8_Release(&g_auto_ds->iface); g_auto_ds = NULL; }
        return E_FAIL;
    }
    g_cur_ds = g_auto_ds;
    IDirect3DSurface8_AddRef(&g_auto_ds->iface);
    rhi_set_render_target(g_device_state.rhi_default_rtv, g_auto_ds->dsv);

    d3d8_init_default_states(&g_device_state);

    /* Set the initial viewport (no backend guesses one) */
    {
        RhiViewport vp;
        vp.x = 0.0f;
        vp.y = 0.0f;
        vp.width = (float)g_device_state.width;
        vp.height = (float)g_device_state.height;
        vp.min_depth = 0.0f;
        vp.max_depth = 1.0f;
        rhi_set_viewports(1, &vp);
    }

    /* Initialize shader and state subsystems */
    hr = d3d8_shaders_init();
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: Shader init failed: 0x%08lX\n", hr);
        return hr;
    }

    hr = d3d8_states_init();
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: State init failed: 0x%08lX\n", hr);
        return hr;
    }

    hr = d3d8_combiners_init();
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: Combiner init failed: 0x%08lX\n", hr);
        /* Non-fatal: fall back to fixed-function pixel shaders */
    }

    hr = d3d8_vsh_init();
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: VSH init failed: 0x%08lX\n", hr);
        /* Non-fatal: fall back to FVF vertex shaders */
    }

    g_device.lpVtbl = &g_device_vtbl;
    g_device_initialized = TRUE;

    *ppDevice = &g_device;
    fprintf(stderr, "D3D8: Device created (%ux%u)\n", g_device_state.width, g_device_state.height);
    return S_OK;
}

static const IDirect3D8Vtbl g_d3d8_vtbl = {
    d3d8_QueryInterface,
    d3d8_AddRef,
    d3d8_Release,
    d3d8_CreateDevice,
};

IDirect3D8 *xbox_Direct3DCreate8(UINT SDKVersion)
{
    (void)SDKVersion;
    g_d3d8.lpVtbl = &g_d3d8_vtbl;
    g_d3d8_ref = 1;
    return &g_d3d8;
}
