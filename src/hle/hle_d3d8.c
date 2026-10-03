/*
 * hle_d3d8.c -- Direct3D 8 functions replaced by name.
 *
 * The route the DOAXBV port proved: replace D3D8 at its API and translate to
 * the host's graphics API, rather than emulate the NV2A beneath it. Measured
 * for Burnout 2 by tools/hle_audit/device_refs.py: no game code reads the
 * D3D device global itself -- 118 references, all inside the D3D section --
 * but two game functions fill their own push buffers through BeginPush
 * (call sites 0x000FA0D9 and 0x000FBE46), and those bypass any replacement
 * made here.
 *
 * The display-filter setters come first because their correct behaviour on a
 * PC is to do nothing: they tune the TV encoder's flicker filter, which a
 * monitor does not have.
 *
 * Shadow mode, RECOMP_HLE_D3D8=shadow
 *
 * The step toward drawing through the host renderer. Every replacement below
 * runs the title's own lifted body first (HLE_ORIGINAL), so the guest device,
 * the push buffer and every frame the executor draws stay exactly as they
 * were. Then, with the switch set, the same call is repeated on a host D3D11
 * device in a window of its own. Registers and return values are the
 * original's, so the title sees no difference. Without the switch these
 * replacements only run the originals.
 *
 * What reaches the host device so far:
 *   - CreateDevice (size), Clear, Swap;
 *   - vertex shaders: FVF codes as they are, and programs the title created,
 *     replayed from their microcode;
 *   - SetTransform, SetViewport;
 *   - DrawVerticesUP and DrawIndexedVerticesUP, the draws whose vertices the
 *     title hands over directly. In a profiled first minute of Burnout 2
 *     these were 29,087 of 36,275 draws; the rest go through vertex buffers;
 *   - SetTexture, in hle_d3d8_texture.c;
 *   - render and texture stage states, in hle_d3d8_state.c;
 *   - SetStreamSource, DrawVertices and DrawIndexedVertices, the vertex
 *     buffer draws, and the vertex shader constants, in hle_d3d8_vertex.c.
 * Draws under a vertex program use the declaration the XDK parsed into the
 * shader object (shadow_read_declaration), with NORMPACKED3 normals unpacked
 * on the CPU and the XDK's screen-space transform undone on the host (see
 * "viewports"). A program whose declaration the host cannot lay out is
 * counted and skipped, and so are draws under a declaration-only shader.
 *
 * Every call into the host renderer goes through the wrappers in
 * hle_d3d8_record.c (host_*), which is also where a frame capture is
 * recorded (RECOMP_D3D8_CAPTURE); see hle_d3d8_record.h.
 *
 * RECOMP_HLE_D3D8_DUMP=<prefix> writes the host frame to <prefix>NNN.bmp
 * every RECOMP_HLE_D3D8_DUMP_EVERY swaps (default 300), at most 24 files --
 * the same format as the executor's RECOMP_FB_DUMP, to put them side by side.
 * RECOMP_HLE_D3D8_DUMP_KEEP_LAST=<k> makes the last k of those roll, so the
 * files show where a long run ended up and not only how it began.
 *
 * Two host facts shape it:
 *   - Guest threads are real host threads. DXGI's Present sends messages to
 *     the window's thread and waits for them, so the window gets a thread of
 *     its own that does nothing but pump it.
 *   - The host D3D8 device is one process-wide object. The host FMV player
 *     (RECOMP_FMV_HOST) creates it too, so the two switches exclude each
 *     other.
 *
 * Each logs its first call, so a run shows the replacement was reached.
 */
#include "platform/xbox_winnt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "recomp_config.h"

#ifdef _WIN32
#include "d3d8_xbox.h"
#include "d3d8_vsh.h"
#include "d3d8_overlay.h"
#include "d3d8_movie.h"
#include "d3d8_xbox_map.h"
#include "hle_d3d8_record.h"
#endif

static void first_call(int *seen, const char *name, uint32_t arg)
{
    if (!*seen) {
        *seen = 1;
        fprintf(stderr, "[HLE] %s(0x%X) replaced by name\n", name, arg);
        fflush(stderr);
    }
}

/* void D3DDevice_SetFlickerFilter(DWORD Filter) -- TV encoder only. */
HLE_EXPORT(D3DDevice_SetFlickerFilter)
{
    static int seen;
    first_call(&seen, "D3DDevice_SetFlickerFilter", HLE_ARG(0));
    HLE_RETURN(0);
}

/* void D3DDevice_SetSoftDisplayFilter(BOOL Enable) -- TV encoder only. */
HLE_EXPORT(D3DDevice_SetSoftDisplayFilter)
{
    static int seen;
    first_call(&seen, "D3DDevice_SetSoftDisplayFilter", HLE_ARG(0));
    HLE_RETURN(0);
}

/* ------------------------------------------------------------------ shadow */

#ifdef _WIN32

#define SHADOW_WM_DESTROY (WM_APP + 1)

static int                g_shadow_mode = -1;
static int                g_shadow_tried;
static IDirect3DDevice8  *g_shadow;
static UINT               g_shadow_width, g_shadow_height;
/* The render target being drawn into: the back buffer, or an offscreen one the
 * title selected (shadow_set_render_target). Viewports and the programs'
 * screen-space undo are relative to it. */
static UINT               g_target_width, g_target_height;
static unsigned long      g_target_sets, g_target_scratch, g_target_failed;
/* Shadow maps (depth_texture_surface): whether draws now go to one, how many
 * times one was made the target, and how many draws it then received --
 * every draw the title makes, before anything is skipped. */
static int                g_target_shadow_map;
static unsigned long      g_shadow_map_sets, g_shadow_map_draws;
static unsigned long      g_shadow_map_big_sets, g_shadow_map_big_draws;   /* 256 wide and up */
/* D3DDevice_EndPush: pushes seen, draws replayed from them, and pushes left
 * alone (another method, or no vertex size). */
static unsigned long      g_push_count, g_push_draws, g_push_other, g_push_nostride;
static unsigned long      g_frame_draws;    /* draws since the last Swap */
static HWND               g_shadow_hwnd;
static DWORD              g_shadow_create_thread;
static DWORD              g_shadow_swap_thread;
static int                g_shadow_thread_notes;
static unsigned long      g_shadow_clears;
static unsigned long      g_shadow_swaps;
/* The video overlay's state, from EnableOverlay and UpdateOverlay. */
static int                g_overlay_enabled, g_overlay_updated;
/* Where the playing movie's surface keeps its texels (hle_xmv_play.c), and
 * whether a draw that reached the host this frame sampled them. Bound is not
 * enough: XGRA binds its movie texture through SetTexture and then draws
 * through a push buffer of its own, so the bind happens and no draw does. */
static uint32_t           g_movie_phys;
static int                g_movie_sampled;
static int                g_movie_yuy2;       /* the movie surface is YUY2 */
static uint32_t           g_stage_texels[4];

void hle_d3d8_movie_surface(uint32_t data, uint32_t xbox_format)
{
    g_movie_phys = data & 0x0FFFFFFFu;
    g_movie_yuy2 = data && xbox_format == 0x24u;
}

void hle_d3d8_note_stage_texels(uint32_t stage, uint32_t phys)
{
    if (stage < 4u)
        g_stage_texels[stage] = phys;
}

/* RECOMP_XMV_LAYER=1: draw a playing movie over the frame on the host's
 * movie layer even when the title draws it itself -- for a title whose own
 * movie draw comes out wrong, and to tell a decoding fault from a drawing one. */
static int movie_layer_forced(void)
{
    static int forced = -1;
    if (forced < 0) {
        const char *e = getenv("RECOMP_XMV_LAYER");
        forced = e && *e && strcmp(e, "0") != 0;
    }
    return forced;
}

/* After a draw reached the host. */
static void note_draw_sampled_movie(void)
{
    int s;

    if (!g_movie_phys)
        return;
    for (s = 0; s < 4; s++)
        if (g_stage_texels[s] == g_movie_phys)
            g_movie_sampled = 1;
}
static uint32_t           g_shadow_last_color;

/* RECOMP_HLE_D3D8_TRACE_SWAPS=<from>-<to>: one line per render-target set,
 * clear, back-buffer query, copy and swap while the swap count is in that
 * range, so the order of a title's frame can be read rather than inferred
 * from totals. Frame dumps say what a frame looked like; this says what the
 * title asked for to get there. Two or three frames is plenty. */
static int shadow_trace_on(void)
{
    static long from = -1, to = -1;

    if (from < 0) {
        const char *e = getenv("RECOMP_HLE_D3D8_TRACE_SWAPS");
        from = to = 0;
        if (e && *e) {
            char *end;
            from = strtol(e, &end, 10);
            to = (*end == '-') ? strtol(end + 1, NULL, 10) : from;
        }
    }
    return to > 0 && (long)g_shadow_swaps >= from && (long)g_shadow_swaps <= to;
}

/* For the texture layer's SetTexture line in the same trace. */
int hle_d3d8_trace_on(void)
{
    return shadow_trace_on();
}

/* The device's swap effect, from the title's D3DPRESENT_PARAMETERS
 * (D3DSWAPEFFECT_DISCARD 1, FLIP 2, COPY 3, COPY_VSYNC 4). Reported at
 * CreateDevice because it changes what a Swap does inside the XDK: a
 * copy-effect device copies back to front in Swap, or leaves that to the
 * title's swap callback when the title passes D3DSWAP_BYPASSCOPY (0x2).
 * TimeSplitters: Future Perfect is such a title: its callback draws the
 * back buffer through a colour-grading combiner into the front buffer,
 * inside Swap's own body, and follows with a Swap(D3DSWAP_FINISH) that
 * draws nothing. Every Swap still presents here; the second present of a
 * pair shows the same frame again, which is harmless. */
static uint32_t g_swap_effect;
static DWORD              g_shadow_last_report;

static int shadow_requested(void)
{
    if (g_shadow_mode < 0) {
        const char *mode = getenv("RECOMP_HLE_D3D8");
        const char *fmv  = getenv("RECOMP_FMV_HOST");

        /* On by default: it is what draws the picture, and a player running
         * the executable wants the picture. RECOMP_HLE_D3D8=off turns it off
         * for the measurements that need the title alone. */
        g_shadow_mode = mode ? strcmp(mode, "shadow") == 0 : 1;
        if (mode && !g_shadow_mode)
            fprintf(stderr, "[HLE-D3D8] RECOMP_HLE_D3D8=%s: nothing draws the "
                    "picture; the title runs blind\n", mode);
        if (g_shadow_mode && fmv && *fmv && strcmp(fmv, "0") != 0) {
            fprintf(stderr, "[HLE-D3D8] shadow mode off: RECOMP_FMV_HOST creates the "
                    "same host device, and there is only one\n");
            g_shadow_mode = 0;
        }
    }
    return g_shadow_mode;
}

/* kernel_bridge.c: the flushes a title's own exit does, then ExitProcess. */
extern void xbox_HostExit(const char *why);

static void shadow_set_fullscreen(HWND hwnd, int on);
static int  g_shadow_fullscreen;

static LRESULT CALLBACK shadow_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SYSKEYDOWN:
        /* Alt+Enter, the usual Windows key for it: fullscreen and back. */
        if (wp == VK_RETURN && (lp & (1 << 29)) && !(lp & (1 << 30))) {
            shadow_set_fullscreen(hwnd, !g_shadow_fullscreen);
            return 0;
        }
        break;
    case WM_SYSCHAR:
        if (wp == '\r')
            return 0;                   /* no beep for Alt+Enter */
        break;
    case WM_SETCURSOR:
        /* The game has no pointer; over a fullscreen picture it is in the way. */
        if (g_shadow_fullscreen && LOWORD(lp) == HTCLIENT) {
            SetCursor(NULL);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        /* This window is the game's display now, so closing it is the user
         * quitting. (It used to hide, from when it sat beside the title's
         * own window as a comparison.) The title cannot be told; it has no
         * such event on the console either. */
        fprintf(stderr, "[HLE-D3D8] window closed by the user\n");
        fflush(stderr);
        xbox_HostExit("window closed");
        return 0;
    case SHADOW_WM_DESTROY:              /* DestroyWindow only works on this thread */
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* Borderless fullscreen: the window, without its frame, covering the monitor
 * it is on. Not an exclusive mode -- nothing changes the display -- but a
 * window that covers its screen is one the driver can flip straight to,
 * which is what lets a variable-refresh display follow the game (the
 * renderer is told, xbox_D3D8SetFullscreen). Alt+Enter switches; the
 * fullscreen setting (RECOMP_FULLSCREEN) starts in it. Runs on the window's
 * own thread. */
static WINDOWPLACEMENT g_shadow_restore;

static void shadow_set_fullscreen(HWND hwnd, int on)
{
    on = on ? 1 : 0;
    if (on == g_shadow_fullscreen)
        return;
    if (on) {
        MONITORINFOEXA mi;
        DEVMODEA dm;
        int w, h;

        memset(&mi, 0, sizeof mi);
        mi.cbSize = sizeof mi;
        if (!GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST),
                             (MONITORINFO *)&mi))
            return;
        g_shadow_restore.length = sizeof g_shadow_restore;
        GetWindowPlacement(hwnd, &g_shadow_restore);
        w = mi.rcMonitor.right - mi.rcMonitor.left;
        h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        SetWindowLongA(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, w, h,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        memset(&dm, 0, sizeof dm);
        dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
            fprintf(stderr, "[HLE-D3D8] fullscreen: %dx%d at %lu Hz (Alt+Enter for a window)\n",
                    w, h, (unsigned long)dm.dmDisplayFrequency);
        else
            fprintf(stderr, "[HLE-D3D8] fullscreen: %dx%d (Alt+Enter for a window)\n", w, h);
    } else {
        SetWindowLongA(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(hwnd, &g_shadow_restore);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fprintf(stderr, "[HLE-D3D8] windowed\n");
    }
    fflush(stderr);
    g_shadow_fullscreen = on;
    xbox_D3D8SetFullscreen(on ? TRUE : FALSE);
}

typedef struct {
    UINT   width, height;
    HWND   hwnd;
    HANDLE ready;
} shadow_window_request;

/* Owns the shadow window for the life of the process and pumps it, so a
 * Present from any guest thread always has a thread answering its messages. */
static DWORD WINAPI shadow_window_thread(LPVOID param)
{
    shadow_window_request *req = param;
    WNDCLASSA wc;
    RECT r;
    MSG msg;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = shadow_wndproc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursorA(NULL, MAKEINTRESOURCEA(32512));   /* IDC_ARROW */
    wc.lpszClassName = "xboxrecomp_hle_d3d8_shadow";
    RegisterClassA(&wc);

    r.left = 0;
    r.top = 0;
    r.right = (LONG)req->width;
    r.bottom = (LONG)req->height;
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    req->hwnd = CreateWindowA(wc.lpszClassName, "xboxrecomp - D3D8 replacement (shadow)",
                              WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              r.right - r.left, r.bottom - r.top,
                              NULL, NULL, wc.hInstance, NULL);
    /* RECOMP_WINDOW_BACKGROUND=1: open behind the other windows and without
     * the focus, for a run a script drives while someone works at the same
     * desk. The game neither needs nor notices the focus: a script is its
     * only input, and the frame is drawn and dumped the same either way. */
    if (req->hwnd) {
        const char *bg = getenv("RECOMP_WINDOW_BACKGROUND");

        if (bg && *bg && strcmp(bg, "0") != 0) {
            ShowWindow(req->hwnd, SW_SHOWNOACTIVATE);
            SetWindowPos(req->hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                         SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
        } else {
            ShowWindow(req->hwnd, SW_SHOW);
            /* Not for a background run: covering the screen is the opposite
             * of staying out of the way. */
            if (recomp_config_bool("RECOMP_FULLSCREEN", "fullscreen", 0))
                shadow_set_fullscreen(req->hwnd, 1);
        }
    }

    /* Say what is being played, not what is playing it. The name is the
     * title's own, out of its XBE certificate, and it is set through the
     * wide call because a certificate may hold one that no ANSI code page
     * can spell -- CreateWindowA above would render that as mojibake. A
     * title whose headers carried no name keeps the caption it was made
     * with. */
    if (req->hwnd) {
        const char *name = xbox_XbeTitleName();
        WCHAR wide[128];

        if (name && MultiByteToWideChar(CP_UTF8, 0, name, -1, wide,
                                        (int)(sizeof wide / sizeof wide[0])) > 0)
            SetWindowTextW(req->hwnd, wide);
    }
    if (!req->hwnd)
        fprintf(stderr, "[HLE-D3D8] shadow: CreateWindow failed (%lu)\n", GetLastError());
    SetEvent(req->ready);                /* req lives on the waiting caller's stack */
    if (!req->hwnd)
        return 1;

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

static HWND shadow_window(UINT width, UINT height)
{
    shadow_window_request req;
    HANDLE thread;

    memset(&req, 0, sizeof req);
    req.width = width;
    req.height = height;
    req.ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!req.ready)
        return NULL;
    thread = CreateThread(NULL, 0, shadow_window_thread, &req, 0, NULL);
    if (!thread) {
        fprintf(stderr, "[HLE-D3D8] shadow: CreateThread failed (%lu)\n", GetLastError());
        CloseHandle(req.ready);
        return NULL;
    }
    WaitForSingleObject(req.ready, INFINITE);
    CloseHandle(req.ready);
    CloseHandle(thread);                 /* it keeps running; only the handle goes */
    return req.hwnd;
}

/* The surfaces the XDK sets as target and depth inside CreateDevice -- the
 * frame buffer and the automatic depth buffer, 0 until seen -- and the host
 * device's own depth surface (shadow_set_render_target). */
static int      g_in_create_device;
static uint32_t g_backbuffer_va, g_autodepth_va;

/* The frame buffers' data pointers: the surface CreateDevice set as the
 * target, and every surface GetBackBuffer2 has returned. A surface is the
 * screen when its data pointer is one of these, whatever object it hangs
 * off. TimeSplitters: Future Perfect wraps a frame buffer (data 0x00204000)
 * in a texture of its own (0x00563154, via XGSetTextureHeader), takes a
 * surface of that texture each frame and draws its whole frame into it;
 * judged by parent alone that surface was "a render target texture" and
 * every draw went into a host texture nothing ever presented, leaving the
 * screen white. */
#define SWAP_SURFACES 4
static uint32_t g_swap_data[SWAP_SURFACES];
static int      g_nswap;

static void note_swap_surface(uint32_t va)
{
    uint32_t data;
    int i;

    if (!va)
        return;
    data = HLE_MEM32(va + 4);
    if (!data)
        return;
    for (i = 0; i < g_nswap; i++)
        if (g_swap_data[i] == data)
            return;
    if (g_nswap < SWAP_SURFACES)
        g_swap_data[g_nswap++] = data;
}

static int is_swap_data(uint32_t data)
{
    int i;

    for (i = 0; i < g_nswap; i++)
        if (data && g_swap_data[i] == data)
            return 1;
    return 0;
}

/* Where the frame buffer lives, as physical addresses. A title that
 * post-processes its own image does not copy the screen anywhere: it makes
 * a texture whose texels ARE the frame buffer and binds that. On hardware
 * that samples what the GPU just drew; here nothing draws on the guest side,
 * so those texels are zeros and the effect blends black over the picture.
 * Recognising the address is what lets the texture layer fill it from the
 * host's own frame instead. There are two, flipped between. */
static uint32_t g_framebuffer_phys[4];
static int      g_framebuffer_count;

static void note_framebuffer_phys(uint32_t data)
{
    uint32_t phys = data & 0x0FFFFFFFu;
    int i;

    if (!phys)
        return;
    for (i = 0; i < g_framebuffer_count; i++)
        if (g_framebuffer_phys[i] == phys)
            return;
    if (g_framebuffer_count < 4) {
        g_framebuffer_phys[g_framebuffer_count++] = phys;
        fprintf(stderr, "[HLE-D3D8] frame buffer at physical 0x%08X; a texture"
                " whose texels live there is the title reading its own screen\n", phys);
        fflush(stderr);
    }
}

int hle_d3d8_is_framebuffer(uint32_t phys)
{
    int i;
    for (i = 0; i < g_framebuffer_count; i++)
        if (g_framebuffer_phys[i] == phys)
            return 1;
    return 0;
}
static IDirect3DSurface8 *g_device_depth;

/* ------------------------------------------------------------- viewports */

/* Xbox vertex programs end in the XDK's screen-space transform: oPos comes
 * out in render-target pixels, through the viewport scale and offset the XDK
 * keeps in constants 58 and 59 (c-38 and c-37), with Z scaled to the depth
 * buffer's range. The values follow Cxbx-Reloaded
 * (GetXboxViewportOffsetAndScale, GetZScaleForPixelContainer). The host
 * programs undo a whole render target's worth of it
 * (d3d8_vsh_set_screenspace), which leaves the viewport already applied:
 * program draws use a host viewport over the whole target with depth 0..1,
 * fixed-function draws the title's own.
 *
 * Z is scaled for the format of the depth surface bound with the current
 * render target (shadow_set_render_target), the CreateDevice one to begin
 * with.
 *
 * Not handled: X_D3DSCM_NORESERVEDCONSTANTS, which frees 58 and 59 for the
 * title (a title that then sets them wins until its next SetViewport). */
static float        g_z_scale = 1.0f;
static D3DVIEWPORT8 g_title_viewport;
static int          g_title_viewport_set;
static int          g_host_viewport_mode = -1;   /* 0 the title's, 1 whole target */

static float xbox_depth_z_scale(uint32_t format)
{
    switch (format) {
    case 0x2C: case 0x30: return 65535.0f;       /* D16, LIN_D16 */
    case 0x2A: case 0x2E: return 16777215.0f;    /* D24S8, LIN_D24S8 */
    case 0x2D: case 0x31: return 511.9375f;      /* F16, LIN_F16 */
    case 0x2B: case 0x2F: return 1.0e30f;        /* F24S8, LIN_F24S8 */
    default:              return 1.0f;
    }
}

static void shadow_viewport_constants(const D3DVIEWPORT8 *vp)
{
    float half_w = (float)g_target_width / 2.0f;
    float half_h = (float)g_target_height / 2.0f;
    float reserved[8] = {
        (float)vp->Width / 2.0f, -(float)vp->Height / 2.0f,
        (vp->MaxZ - vp->MinZ) * g_z_scale, 1.0f,
        (float)vp->Width / 2.0f + (float)vp->X, (float)vp->Height / 2.0f + (float)vp->Y,
        vp->MinZ * g_z_scale, 0.0f
    };
    float scale[4]  = { half_w, -half_h, g_z_scale, 1.0f };
    float offset[4] = { half_w, half_h, 0.0f, 0.0f };

    host_vsh_set_constant(58, reserved, 2);
    host_vsh_set_screenspace(scale, offset);
}

static void shadow_use_viewport(int whole_target)
{
    D3DVIEWPORT8 vp;

    if (g_host_viewport_mode == whole_target)
        return;
    g_host_viewport_mode = whole_target;
    if (!whole_target && g_title_viewport_set) {
        vp = g_title_viewport;
    } else {
        vp.X = 0;
        vp.Y = 0;
        vp.Width = g_target_width;
        vp.Height = g_target_height;
        vp.MinZ = 0.0f;
        vp.MaxZ = 1.0f;
    }
    host_SetViewport(g_shadow, &vp);
}

/* The guest's D3DPRESENT_PARAMETERS is the Xbox layout in 32-bit guest
 * memory: BackBufferWidth at +0, BackBufferHeight at +4, a guest HWND at +24.
 * The host struct holds a pointer-sized HWND, so only the fields the host
 * device needs are copied, one by one. One attempt per process: a title that
 * recreates its device must not leave a window behind for each failure. */
/* A vertex declaration as the title's own D3DVSD tokens give it: each
 * register's stream, offset and X_D3DVSDT type, and how many registers it
 * declares (0: no tokens). Decoded at CreateVertexShader (decl_from_tokens),
 * because the token buffer, like the microcode, need not outlive the call. */
typedef struct {
    int      regs;
    uint8_t  stream[16];
    uint16_t offset[16];
    uint8_t  format[16];
} shadow_decl_tokens;

/* Vertex shaders the title created before this device existed (further down,
 * with D3DDevice_CreateVertexShader). */
static void shadow_track_vertex_shader(uint32_t guest, int has_function,
                                       uint32_t header, const DWORD *code,
                                       const shadow_decl_tokens *tokens);
static void shadow_keep_early_vertex_shader(uint32_t guest, int has_function,
                                            uint32_t header, const DWORD *code,
                                            const shadow_decl_tokens *tokens);
static void shadow_replay_early_vertex_shaders(void);

static void shadow_create(uint32_t pp_va)
{
    D3DPRESENT_PARAMETERS pp;
    IDirect3D8 *d3d;
    HRESULT hr;
    UINT width = 640, height = 480;
    HWND hwnd;

    g_shadow_tried = 1;
    if (pp_va && HLE_MEM32(pp_va + 0) && HLE_MEM32(pp_va + 4)) {
        width  = HLE_MEM32(pp_va + 0);
        height = HLE_MEM32(pp_va + 4);
    }
    /* EnableAutoDepthStencil at +32, AutoDepthStencilFormat at +36. */
    if (pp_va && HLE_MEM32(pp_va + 32))
        g_z_scale = xbox_depth_z_scale(HLE_MEM32(pp_va + 36));

    hwnd = shadow_window(width, height);
    g_shadow_hwnd = hwnd;
    if (!hwnd)
        return;

    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = width;
    pp.BackBufferHeight = height;
    pp.BackBufferCount = 1;
    pp.hDeviceWindow = hwnd;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE;

    d3d = xbox_Direct3DCreate8(0);
    hr = d3d ? d3d->lpVtbl->CreateDevice(d3d, 0, 1 /* HAL */, hwnd, 0, &pp, &g_shadow)
             : E_FAIL;
    if (FAILED(hr) || !g_shadow) {
        fprintf(stderr, "[HLE-D3D8] shadow: CreateDevice failed (0x%08lX)\n", (unsigned long)hr);
        g_shadow = NULL;
        PostMessageA(hwnd, SHADOW_WM_DESTROY, 0, 0);
        return;
    }
    /* Never wait for vertical blank here. The title paces its loader on the
     * frames it presents itself; a vsync wait on this side device throttled
     * Burnout 2's whole loop to 27 frames a second. */
    xbox_D3D8SetPresentInterval(0);
    /* The size the title's screen-space geometry is measured in, whatever
     * size the host ends up rendering at. */
    xbox_D3D8SetGuestSize(width, height);
    g_shadow_width = width;
    g_shadow_height = height;
    g_target_width = width;
    g_target_height = height;
    g_device_depth = host_DeviceDepthSurface(g_shadow);
    hle_d3d8_interp_init();
    {
        /* Until the title sets one: the whole back buffer. */
        D3DVIEWPORT8 whole = { 0, 0, width, height, 0.0f, 1.0f };
        shadow_viewport_constants(&whole);
    }

    /* Until the first draw applies the title's own states: nothing culled, no
     * texture and no lighting. */
    host_SetRenderState(g_shadow, D3DRS_CULLMODE, D3DCULL_NONE);
    host_SetRenderState(g_shadow, D3DRS_LIGHTING, FALSE);
    host_SetTexture(g_shadow, 0, NULL);

    g_shadow_create_thread = GetCurrentThreadId();
    fprintf(stderr, "[HLE-D3D8] shadow device %ux%u, from the title's own "
            "CreateDevice parameters, on guest thread %lu; vertex program Z "
            "scale %g\n", width, height, (unsigned long)g_shadow_create_thread,
            g_z_scale);
    fflush(stderr);
    shadow_replay_early_vertex_shaders();
}

/* xbox_clear_flags_to_host() is in d3d8_xbox_map.h with the other Xbox-to-host
 * value tables. The first Clear calls log the raw flags below, so the mapping
 * stays checkable against what the title really passes. */

/* ----------------------------------------------------------- vertex shaders */

/* Xbox SetVertexShader takes either an FVF code (bit 0 clear) or the address
 * of the title's X_D3DVertexShader with bit 0 set (Cxbx-Reloaded,
 * XbVertexShader.h, VshHandleIsVertexShader). FVF bits are the PC's, so an
 * FVF code goes to the host as it is. A program is created on the host when
 * the title creates it, and found again here by its guest handle. */
#define SHADOW_MAX_PROGRAMS 128

enum { SHADER_DECLARATION, SHADER_HOST_PROGRAM, SHADER_NOT_REPLAYED };

#define SHADOW_MAX_PACKED 8              /* registers expanded to floats per draw */

struct shadow_program {
    uint32_t guest;
    DWORD    host;
    int      kind;
    int      from_slot;                  /* host belongs to g_slot_host, not this entry */
    /* From the declaration (shadow_read_declaration), host programs only. */
    int      has_declaration;            /* the host has its vertex layout */
    UINT     extent;                     /* bytes of a vertex it reads */
    /* Registers in a format the host cannot read as it is (xbox_vsdt_expanded).
     * Each draw copies its vertices with these unpacked to floats in a prefix
     * of expanded_bytes; the rest of the vertex follows unchanged. */
    int      packed_count;
    UINT     packed_offset[SHADOW_MAX_PACKED];   /* in the guest vertex */
    UINT     packed_out[SHADOW_MAX_PACKED];      /* in the prefix */
    uint32_t packed_format[SHADOW_MAX_PACKED];   /* X_D3DVSDT */
    /* Registers read from a stream other than 0 go through the same prefix:
     * the host binds one stream, so each draw copies them in beside the
     * stream 0 vertex, as they are when the host can read the format
     * (packed_raw, packed_size bytes) and unpacked when it cannot. */
    uint8_t  packed_stream[SHADOW_MAX_PACKED];
    uint8_t  packed_raw[SHADOW_MAX_PACKED];
    UINT     packed_size[SHADOW_MAX_PACKED];
    int      other_streams;              /* any register from stream 1..15 */
    UINT     expanded_bytes;
    /* The declaration as the title's own D3DVSD tokens give it, decoded at
     * CreateVertexShader. Used instead of the copy the XDK parses into the
     * shader object when the two disagree: that copy is read at object + 20,
     * 16 bytes a register, which is its layout from XDK 4627 on but not on
     * XDK 3925 (Halo), where every "format" read there is another register's
     * offset and every program's draws were skipped as "no host layout". The
     * token stream is the same on every XDK. */
    int      use_tokens;
    shadow_decl_tokens tok;
};

/* The first vertex of the buffer draw about to be made, so other streams can
 * be read at the same index; NO_FIRST_VERTEX for a UP draw, which has only
 * the vertices it was handed. Set by hle_d3d8_vertex.c. */
#define NO_FIRST_VERTEX 0xFFFFFFFFu
static uint32_t g_draw_first = NO_FIRST_VERTEX;
void hle_d3d8_shadow_set_first_vertex(uint32_t first) { g_draw_first = first; }
/* From hle_d3d8_vertex.c: vertex `first` of stream `stream`, or NULL. */
const void *hle_d3d8_stream_vertices(uint32_t stream, uint32_t first, uint32_t vertices,
                                     uint32_t *stride);

static struct shadow_program g_programs[SHADOW_MAX_PROGRAMS];
static int      g_program_count;
static uint32_t g_shadow_vs;             /* guest handle last selected */
static int      g_shadow_vs_is_program;  /* bit 0 set: a shader object */
static int      g_shadow_vs_kind;        /* its kind, or -1 if never created */
static int      g_shadow_vs_slot = -1;   /* its g_programs entry, or -1 */
/* The program address SelectVertexShader named with it (SHADOW_PROGRAM_SLOTS
 * for SetVertexShader), so a later LoadVertexShaderProgram into that address
 * can replace the running program as it does on the NV2A. */
static uint32_t g_shadow_vs_address = 0xFFFFFFFFu;

/* The guest object of the vertex program selected now, its declaration at +20;
 * 0 under an FVF. For hle_d3d8_vertex.c's draws from title-pushed arrays. */
uint32_t hle_d3d8_shadow_program_object(void)
{
    return g_shadow_vs_is_program ? (g_shadow_vs & ~1u) : 0u;
}

/* The handle is the address of the title's shader object, so a shader
 * created after another was deleted can reuse its handle. Creation replaces
 * an entry, so each handle has at most one. */
static int shadow_program_find(uint32_t guest)
{
    int i;

    for (i = 0; i < g_program_count; i++)
        if (g_programs[i].guest == guest)
            return i;
    return -1;
}

/* Programs loaded by slot rather than created by handle.
 *
 * The NV2A holds 136 transform-program instruction slots. Two XDK paths fill
 * them: CreateVertexShader keeps the microcode in a shader object and
 * LoadVertexShader copies it in when the object is selected -- the path
 * Burnout 2 (5344) takes, tracked in g_programs by the object's handle --
 * and LoadVertexShaderProgram(pFunction, Address), which copies microcode
 * straight into slot Address with no object at all. TimeSplitters 2 (4721)
 * takes the second: four programs loaded once, then
 * SelectVertexShaderDirect(pVAF, Address) copies the vertex declaration into
 * one static object in the D3D section and selects that object's handle with
 * the slot number. Keyed by handle alone, every one of its draws was
 * "unknown shader". So the slot number is tracked too, and a selected handle
 * with no created program borrows the program loaded at its slot. */
#define SHADOW_PROGRAM_SLOTS 136
static DWORD g_slot_host[SHADOW_PROGRAM_SLOTS];
static int   g_slot_loaded[SHADOW_PROGRAM_SLOTS];
static unsigned long g_slot_reloads;   /* loads answered from the program cache */
static unsigned long g_slot_rebinds;   /* loads that replaced the selected program */

/* Host programs by microcode. A title on this API rotates a few programs
 * through the same slots -- TimeSplitters 2 puts three different programs
 * into slot 0 in turn, ~60k loads in two minutes -- so "the slot already
 * holds this" almost never matches. A load is answered by content instead:
 * a host program, once made, is kept for the run and shared by every slot
 * and every selected entry that names it, and is never deleted while it is
 * in here. Beyond the cache's size loads fall back to create-and-delete. */
#define SLOT_PROGRAM_CACHE 96

typedef struct SlotProgram {
    uint32_t hash;
    int      count;
    DWORD    host;
} SlotProgram;

static SlotProgram g_slot_programs[SLOT_PROGRAM_CACHE];
static int         g_slot_program_count;

static uint32_t microcode_hash(const DWORD *microcode, int count)
{
    const uint8_t *p = (const uint8_t *)microcode;
    size_t n = (size_t)count * 4 * sizeof(DWORD), i;
    uint32_t h = 0x811C9DC5u;

    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

static int slot_program_find(uint32_t hash, int count, const DWORD *microcode)
{
    int i;
    for (i = 0; i < g_slot_program_count; i++)
        if (g_slot_programs[i].hash == hash && g_slot_programs[i].count == count &&
            host_vsh_same_microcode(g_slot_programs[i].host, microcode, count))
            return i;
    return -1;
}

static int slot_program_cached(DWORD host)
{
    int i;
    for (i = 0; i < g_slot_program_count; i++)
        if (g_slot_programs[i].host == host)
            return 1;
    return 0;
}

static void shadow_read_declaration(int slot, uint32_t handle);
/* From hle_d3d8_vertex.c: vertex arrays the title pushed itself stop applying
 * when the XDK writes its own again, which a new declaration makes it do. */
void hle_d3d8_push_arrays_off(void);

static void shadow_select_vertex_shader(uint32_t handle, uint32_t address)
{
    int i;

    hle_d3d8_push_arrays_off();
    g_shadow_vs = handle;
    g_shadow_vs_address = address;
    g_shadow_vs_is_program = (handle & 1) != 0;
    if (!g_shadow_vs_is_program) {
        g_shadow_vs_kind = -1;
        g_shadow_vs_slot = -1;
        host_SetVertexShader(g_shadow, handle);
        return;
    }
    i = shadow_program_find(handle);
    if ((i < 0 || g_programs[i].from_slot) &&
        address < SHADOW_PROGRAM_SLOTS && g_slot_loaded[address]) {
        /* A handle whose program came from a slot: the object holds only the
         * declaration, and SelectVertexShaderDirect rewrites it on every
         * call, so the layout is re-read each time it is selected. */
        if (i < 0 && g_program_count < SHADOW_MAX_PROGRAMS) {
            i = g_program_count++;
            g_programs[i].guest = handle;
        }
        if (i >= 0) {
            static int said;
            g_programs[i].host = g_slot_host[address];
            g_programs[i].kind = SHADER_HOST_PROGRAM;
            g_programs[i].from_slot = 1;
            shadow_read_declaration(i, handle);
            if (said++ < 4)
                fprintf(stderr, "[HLE-D3D8] shadow vertex shader 0x%08X: program from "
                                "slot %u%s\n", handle, address,
                        g_programs[i].has_declaration ? "" : " (no host layout)");
        }
    }
    g_shadow_vs_slot = i;
    g_shadow_vs_kind = i >= 0 ? g_programs[i].kind : -1;
    if (g_shadow_vs_kind == SHADER_HOST_PROGRAM)
        host_SetVertexShader(g_shadow, g_programs[i].host);
}

/* ---------------------------------------------------------------- drawing */

/* The Xbox primitive types (XPT_*) and xbox_primitive_to_host() are in
 * d3d8_xbox_map.h. */

/* FVF vertex size, as the host computes it for its input layout. A draw
 * whose stride disagrees would be read at the wrong offsets, so it is
 * skipped and counted rather than drawn as noise. */
static UINT fvf_stride(DWORD fvf)
{
    UINT size = 0, tex;

    switch (fvf & 0x00E) {
    case 0x002: size = 12; break;        /* XYZ */
    case 0x004: size = 16; break;        /* XYZRHW */
    case 0x006: size = 16; break;        /* XYZB1 */
    case 0x008: size = 20; break;
    case 0x00A: size = 24; break;
    case 0x00C: size = 28; break;
    }
    if (fvf & 0x010) size += 12;         /* NORMAL */
    if (fvf & 0x040) size += 4;          /* DIFFUSE */
    if (fvf & 0x080) size += 4;          /* SPECULAR */
    tex = (fvf >> 8) & 0xF;
    if (tex > 8)                         /* TEX8 is the most; more is garbage */
        tex = 8;
    for (UINT i = 0; i < tex; i++) {
        /* D3DFVF_TEXCOORDSIZEn: 2 bits per set from bit 16; 0 means 2D. */
        static const UINT dims[4] = { 2, 3, 4, 1 };
        size += 4 * dims[(fvf >> (16 + 2 * i)) & 3];
    }
    return size;
}

/* hle_d3d8_state.c */
void hle_d3d8_pixel_shader_selected(uint32_t handle);
void hle_d3d8_shadow_apply_states(IDirect3DDevice8 *dev);

static unsigned long g_draws_up, g_draws_indexed_up, g_draws_vb, g_draws_indexed_vb,
                     g_draws_program, g_draws_declaration, g_draws_unknown_vs,
                     g_draws_stride, g_draws_primitive, g_draws_failed;
/* Dropped by RECOMP_HLE_D3D8_SKIP_FULLSCREEN; see the draw gate. */
static unsigned long g_skipped_fullscreen;
/* The inline immediate-mode vertex path, counted but not implemented; see the
 * replacements for D3DDevice_Begin further down. */
static unsigned long g_inline_begin, g_inline_end, g_inline_vdata;
static unsigned long g_inline_drawn, g_inline_program;   /* drawn at End; under a program */
static unsigned long g_inline_begin_frame, g_inline_vdata_frame;
static unsigned long g_inline_begin_max, g_inline_vdata_max;
/* From hle_d3d8_texture.c: stage 0 holds the title's own frame. */
int hle_d3d8_stage0_is_framebuffer(void);
/* From hle_d3d8_texture.c: re-read each stage's guest palette, as the Xbox
 * does at every draw, and re-bake a P8 texture drawn under a new one. */
void hle_d3d8_sync_palettes(IDirect3DDevice8 *dev);
/* From hle_d3d8_texture.c: at the end of a frame, textures whose texels
 * changed after the frame drew with them. */
void hle_d3d8_texture_frame_end(void);
/* Draws arriving on a guest thread other than the one that swaps. A loader
 * thread drawing to warm caches puts geometry through the host that no
 * presented frame ever contains -- it would count as drawn and never show. */
static unsigned long g_draws_off_thread;

/* The vertex formats draws arrive with, whether or not they are then drawn:
 * a shader handle seen for the first time is logged, up to a limit. */
static void note_draw_format(uint32_t xpt, uint32_t stride)
{
    static uint32_t seen[24];
    static int nseen;
    int i;

    for (i = 0; i < nseen; i++)
        if (seen[i] == g_shadow_vs)
            return;
    if (nseen < (int)(sizeof seen / sizeof seen[0])) {
        seen[nseen++] = g_shadow_vs;
        fprintf(stderr, "[HLE-D3D8] shadow draw: %s 0x%08X, stride %u, primitive %u\n",
                g_shadow_vs_is_program ? "vertex program" : "FVF",
                g_shadow_vs, stride, xpt);
    }
}

/* Fixed function described by a declaration.
 *
 * CreateVertexShader with a declaration and no program is the Xbox's
 * fixed-function pipeline fed by an arbitrary vertex layout: the registers
 * are the fixed-function inputs (D3DVSDE_POSITION 0, NORMAL 2, DIFFUSE 3,
 * SPECULAR 4, TEXCOORD0..3 9..12), in any order, any format and any stream.
 * The host's fixed-function pipeline is driven by an FVF instead, so each
 * draw copies its vertices into the FVF that carries the same inputs, in FVF
 * order, with every format the FVF cannot hold converted to floats.
 *
 * Mortal Kombat: Deadly Alliance draws its arenas this way -- FLOAT3
 * position, NORMPACKED3 normal, FLOAT2 texture coordinates, a second set in
 * stream 1 -- 35,000 draws a minute that were all skipped as "declaration
 * shader", leaving every fight on black. */
#define FF_MAX_INPUTS 8

static struct {
    DWORD    fvf;
    UINT     out_stride;
    int      count;
    uint8_t  stream[FF_MAX_INPUTS];
    UINT     offset[FF_MAX_INPUTS];      /* in the guest vertex of that stream */
    uint32_t format[FF_MAX_INPUTS];      /* X_D3DVSDT */
    UINT     out[FF_MAX_INPUTS];         /* in the FVF vertex */
    int      as_color[FF_MAX_INPUTS];    /* written as a D3DCOLOR, not floats */
    int      floats[FF_MAX_INPUTS];      /* else how many floats are written */
    int      tex_set[FF_MAX_INPUTS];     /* the texture set it fills, or -1 */
} g_ff;
static int g_ff_active;                  /* the current draw goes through g_ff */
static unsigned long g_draws_ff_declaration;

/* An X_D3DVSDT element as floats: how many, or 0 for a format this cannot
 * read. D3DCOLOR comes back as r, g, b, a in 0..1. */
static int ff_read_floats(uint32_t format, const uint8_t *at, float f[4])
{
    int n = (int)(format >> 4), c;

    switch (format & 0x0F) {
    case 0x2:                                    /* FLOAT1..4 */
        if (format == 0x72 || n < 1 || n > 4)
            return 0;                            /* FLOAT2H, or garbage */
        memcpy(f, at, (size_t)n * sizeof *f);
        return n;
    case 0x0:                                    /* D3DCOLOR: B, G, R, A bytes */
        if (format != 0x40)
            return 0;
        f[0] = at[2] / 255.0f; f[1] = at[1] / 255.0f;
        f[2] = at[0] / 255.0f; f[3] = at[3] / 255.0f;
        return 4;
    case 0x1:                                    /* NORMSHORT1..4 */
    case 0x5:                                    /* SHORT1..4 */
        if (n < 1 || n > 4)
            return 0;
        for (c = 0; c < n; c++) {
            int16_t s;
            memcpy(&s, at + 2 * c, sizeof s);
            f[c] = (format & 0x0F) == 0x1 ? s / 32767.0f : (float)s;
        }
        return n;
    case 0x4:                                    /* PBYTE1..4 */
        if (n < 1 || n > 4)
            return 0;
        for (c = 0; c < n; c++)
            f[c] = at[c] / 255.0f;
        return n;
    case 0x6:                                    /* NORMPACKED3 */
        if (format != 0x16)
            return 0;
        {
            uint32_t bits;
            memcpy(&bits, at, sizeof bits);
            f[0] = (float)((int32_t)(bits << 21) >> 21) / 1023.0f;
            f[1] = (float)((int32_t)(bits << 10) >> 21) / 1023.0f;
            f[2] = (float)((int32_t)bits >> 22) / 511.0f;
        }
        return 3;
    default:
        return 0;
    }
}

/* Bytes an X_D3DVSDT element occupies in the guest vertex. */
static UINT ff_format_size(uint32_t format)
{
    UINT n = format >> 4;

    switch (format & 0x0F) {
    case 0x2: return 4 * n;
    case 0x0: return 4;
    case 0x1: case 0x5: return 2 * n;
    case 0x4: return n;
    case 0x6: return 4;
    default:  return 0;
    }
}

/* Build g_ff from the selected declaration-only shader object. 1 when the host
 * can draw it; otherwise 0, with a note the first few times saying why. */
static int ff_from_declaration(uint32_t handle, uint32_t stride)
{
    static int notes;
    uint32_t object = handle & ~1u, i;
    int have_tex[4] = { 0, 0, 0, 0 }, ntex = 0, dropped = 0;
    struct { int reg; uint32_t stream, offset, format; } in[16];
    int n = 0, k;
    const char *why = NULL;
    UINT out = 0;

    memset(&g_ff, 0, sizeof g_ff);
    for (i = 0; i < 16u; i++) {
        uint32_t attr = object + 20u + i * 16u;
        uint32_t format = HLE_MEM32(attr + 8u);

        if (format <= 0x02u)
            continue;                            /* not in the declaration */
        in[n].reg = (int)i;
        in[n].stream = HLE_MEM32(attr);
        in[n].offset = HLE_MEM32(attr + 4u);
        in[n].format = format;
        n++;
    }

    /* FVF order: position, normal, diffuse, specular, texture sets. */
    {
        static const int order[] = { 0, 2, 3, 4, 9, 10, 11, 12 };
        int o;

        for (o = 0; o < (int)(sizeof order / sizeof order[0]) && !why; o++) {
            for (k = 0; k < n; k++)
                if (in[k].reg == order[o])
                    break;
            if (k == n) {
                if (order[o] == 0)
                    why = "no position";
                continue;
            }
            {
                UINT size = ff_format_size(in[k].format);
                float probe[4];
                int floats;

                if (!size || in[k].stream > 15u || in[k].offset > 0xFFFFu) {
                    why = "a format or stream it cannot read";
                    break;
                }
                if (in[k].stream == 0u && in[k].offset + size > stride) {
                    why = "a register past the end of the vertex";
                    break;
                }
                /* the count a format reads to, without reading any vertex */
                floats = ff_read_floats(in[k].format,
                                        (const uint8_t *)"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0",
                                        probe);
                if (!floats) {
                    why = "a format it cannot convert";
                    break;
                }
                if (g_ff.count == FF_MAX_INPUTS) {
                    why = "too many inputs";
                    break;
                }
                g_ff.stream[g_ff.count] = (uint8_t)in[k].stream;
                g_ff.offset[g_ff.count] = in[k].offset;
                g_ff.format[g_ff.count] = in[k].format;
                switch (order[o]) {
                case 0:
                    if (floats == 3)
                        g_ff.fvf |= 0x002;       /* XYZ */
                    else if (floats == 4)
                        g_ff.fvf |= 0x004;       /* XYZRHW */
                    else
                        why = "a position that is not 3 or 4 wide";
                    g_ff.floats[g_ff.count] = floats;
                    break;
                case 2:
                    if (floats != 3)
                        why = "a normal that is not 3 wide";
                    g_ff.fvf |= 0x010;
                    g_ff.floats[g_ff.count] = 3;
                    break;
                case 3:
                case 4:
                    g_ff.fvf |= order[o] == 3 ? 0x040 : 0x080;
                    g_ff.as_color[g_ff.count] = 1;
                    break;
                default:
                    have_tex[order[o] - 9] = floats;
                    g_ff.floats[g_ff.count] = floats;
                    g_ff.tex_set[g_ff.count] = order[o] - 9;
                    break;
                }
                if (order[o] < 9)
                    g_ff.tex_set[g_ff.count] = -1;
                g_ff.count++;
            }
        }
    }
    for (k = 0; k < n; k++)
        if (in[k].reg == 1 || in[k].reg == 5 || in[k].reg == 6 ||
            in[k].reg == 7 || in[k].reg == 8 || in[k].reg > 12)
            dropped++;                           /* weights, fog, point size, back colours */

    if (!why) {
        /* Texture sets are positional in an FVF: up to the highest one used,
         * with any gap below it filled by a 2D set of zeros. */
        for (k = 3; k >= 0 && !have_tex[k]; k--)
            ;
        ntex = k + 1;
        g_ff.fvf |= (DWORD)ntex << 8;
        for (k = 0; k < ntex; k++) {
            static const DWORD code[5] = { 0, 3, 0, 1, 2 };   /* by width */
            int w = have_tex[k] ? have_tex[k] : 2;
            g_ff.fvf |= code[w] << (16 + 2 * k);
        }
        /* Offsets in the FVF vertex. The inputs are already in FVF order; a
         * texture set that is missing below a used one is left as two zero
         * floats (the copy is zero-filled). */
        {
            int next_set = 0;
            for (k = 0; k < g_ff.count; k++) {
                int set = g_ff.tex_set[k];
                for (; set >= 0 && next_set < set; next_set++)
                    out += 8u;
                if (set >= 0)
                    next_set = set + 1;
                g_ff.out[k] = out;
                out += g_ff.as_color[k] ? 4u : (UINT)g_ff.floats[k] * 4u;
            }
        }
        g_ff.out_stride = fvf_stride(g_ff.fvf);
        if (out != g_ff.out_stride)
            why = "a layout that does not add up to its FVF";
    }
    if (why) {
        if (notes++ < 8)
            fprintf(stderr, "[HLE-D3D8] fixed-function declaration 0x%08X: %s; "
                            "its draws are skipped\n", handle, why);
        return 0;
    }
    if (notes < 8 && dropped) {
        notes++;
        fprintf(stderr, "[HLE-D3D8] fixed-function declaration 0x%08X: drawn as FVF "
                        "0x%08lX without %d input(s) an FVF cannot carry (blend "
                        "weights, fog, point size or back-face colours)\n",
                handle, (unsigned long)g_ff.fvf, dropped);
    }
    return 1;
}

/* Common checks for a draw under the current vertex shader. */
static int shadow_can_draw(uint32_t xpt, uint32_t stride)
{
    g_ff_active = 0;
    if (g_target_shadow_map) {
        g_shadow_map_draws++;
        if (g_target_width >= 256) {
            static unsigned long said, last_swap = (unsigned long)-1;

            g_shadow_map_big_draws++;
            /* Which frames draw casters: the first twenty, once each. */
            if (said < 20 && last_swap != g_shadow_swaps) {
                said++;
                last_swap = g_shadow_swaps;
                fprintf(stderr, "[HLE-D3D8] shadow map %ux%u drawn into at swap %lu\n",
                        g_target_width, g_target_height, g_shadow_swaps);
            }
        }
    }
    if (g_shadow_swap_thread && GetCurrentThreadId() != g_shadow_swap_thread)
        g_draws_off_thread++;
    note_draw_format(xpt, stride);
    if (g_shadow_vs_is_program) {
        const struct shadow_program *p;

        if (g_shadow_vs_kind == SHADER_DECLARATION) {
            /* fixed function by declaration: drawn as the equivalent FVF */
            if (!ff_from_declaration(g_shadow_vs, stride)) {
                g_draws_declaration++;
                return 0;
            }
            host_SetVertexShader(g_shadow, g_ff.fvf);
            g_ff_active = 1;
            g_draws_ff_declaration++;
            shadow_use_viewport(0);
            goto states;
        }
        if (g_shadow_vs_kind != SHADER_HOST_PROGRAM || g_shadow_vs_slot < 0) {
            g_draws_unknown_vs++;
            return 0;
        }
        p = &g_programs[g_shadow_vs_slot];
        if (!p->has_declaration) {
            g_draws_program++;           /* no vertex layout for the host */
            return 0;
        }
        if (stride < p->extent) {        /* the layout would read past a vertex */
            g_draws_stride++;
            return 0;
        }
        shadow_use_viewport(1);
    } else {
        if (fvf_stride(g_shadow_vs) != stride) {
            g_draws_stride++;
            return 0;
        }
        shadow_use_viewport(0);
    }
states:
    /* RECOMP_HLE_D3D8_SKIP_FULLSCREEN=1: drop the title's full-screen passes
     * over its own frame -- the draws that sample the frame buffer at stage 0.
     *
     * This is a measurement, and a crude workaround. Those passes remove a
     * fixed share of the light in every frame (0.62 of it on TimeSplitters 2's
     * snow level, 0.367 on another, constant within a level to a standard
     * deviation of 0.002), which is the brightness bug in
     * docs/technical/timesplitters2-open-issues.md. Turning them off says
     * whether they own that loss outright, and gives a bright picture without
     * whatever they were for -- bloom or glow, on the evidence of a fixed
     * one-texel offset repeated three times with descending alpha. */
    {
        static int skip = -1;
        if (skip < 0)
            skip = getenv("RECOMP_HLE_D3D8_SKIP_FULLSCREEN") ? 1 : 0;
        if (skip && hle_d3d8_stage0_is_framebuffer()) {
            g_skipped_fullscreen++;
            return 0;
        }
    }
    /* The title's render and texture stage states as they stand now, read
     * from its own state arrays (hle_d3d8_state.c). */
    hle_d3d8_shadow_apply_states(g_shadow);
    hle_d3d8_sync_palettes(g_shadow);
    g_frame_draws++;
    return 1;
}

/* ------------------------------------------------------------ frame dumps */

/* Set by the key that asks for the frame on screen; see overlay_frame. */
static int g_dump_requested;

static void shadow_dump_next_frame(void)
{
    g_dump_requested = 1;
}

/* RECOMP_HLE_D3D8_BRIGHT=<n>: every n swaps, say how bright the finished frame
 * is, and how many draws made it.
 *
 * Pair it with RECOMP_HLE_D3D8_FB_PROBE at the same interval. That one reads
 * the screen copy the title samples, which is the scene *before* its
 * full-screen passes; this one reads the back buffer at Swap, which is the
 * scene *after* them. Two numbers a few lines apart in the log, against the
 * same swap number, are what those passes did to the picture -- measured in
 * the running title.
 *
 * It has to be measured live. src/replay never performs the screen copy, so
 * replaying these draws samples a black texture and answers a different
 * question; two diagnoses of the TimeSplitters 2 brightness bug died of that.
 * Standing still and then moving with this on is the whole experiment. */
static void shadow_frame_brightness(void)
{
    static int every = -1;
    IDirect3DSurface8 *surf = NULL;
    D3DLOCKED_RECT lr;
    unsigned long long sum = 0;
    unsigned samples = 0;
    UINT x, y;

    if (every < 0) {
        const char *v = getenv("RECOMP_HLE_D3D8_BRIGHT");
        every = (v && atoi(v) > 0) ? atoi(v) : 0;
    }
    if (!every || !g_shadow || (g_shadow_swaps % (unsigned long)every) != 0)
        return;

    if (FAILED(g_shadow->lpVtbl->GetBackBuffer(g_shadow, 0, 0, &surf)) || !surf)
        return;
    if (FAILED(surf->lpVtbl->LockRect(surf, &lr, NULL, D3DLOCK_READONLY))) {
        surf->lpVtbl->Release(surf);
        return;
    }
    /* R8G8B8A8, sparsely sampled: enough for a mean, cheap enough that the
     * readback stall does not change what is being measured. */
    for (y = 0; y < g_shadow_height; y += 16) {
        const uint8_t *row = (const uint8_t *)lr.pBits + (size_t)y * (size_t)lr.Pitch;
        for (x = 0; x < g_shadow_width; x += 16) {
            const uint8_t *p = row + (size_t)x * 4u;
            sum += (unsigned)p[0] + p[1] + p[2];
            samples += 3;
        }
    }
    surf->lpVtbl->UnlockRect(surf);
    surf->lpVtbl->Release(surf);

    fprintf(stderr, "[HLE-D3D8] frame brightness swap %lu: after everything, "
            "mean %.1f/255 over %lu draws\n", g_shadow_swaps,
            samples ? (double)sum / samples : 0.0, g_frame_draws);
    fflush(stderr);
}

/* The back buffer as it is now, to a 24-bit BMP at path. 0 on success. */
static int dump_back_buffer(const char *path)
{
    IDirect3DSurface8 *surf = NULL;
    D3DLOCKED_RECT lr;
    uint8_t hdr[54];
    UINT w, h, y, x, pad;
    uint32_t filesz;
    FILE *f;
    int ok = -1;

    if (!g_shadow)
        return -1;
    if (FAILED(g_shadow->lpVtbl->GetBackBuffer(g_shadow, 0, 0, &surf)) || !surf)
        return -1;
    if (FAILED(surf->lpVtbl->LockRect(surf, &lr, NULL, D3DLOCK_READONLY))) {
        surf->lpVtbl->Release(surf);
        return -1;
    }
    /* R8G8B8A8 (d3d8_device.c), but not necessarily the guest's size: the
     * host renders the scene larger than the guest asked whenever
     * supersampling is on, and GetBackBuffer hands back that scene. Taking
     * the size from the surface keeps the dump whole at any scale --
     * g_shadow_width here wrote the top-left corner and called it a frame. */
    {
        D3DSURFACE_DESC sd;

        if (SUCCEEDED(surf->lpVtbl->GetDesc(surf, &sd)) && sd.Width && sd.Height) {
            w = sd.Width;
            h = sd.Height;
        } else {
            w = g_shadow_width;
            h = g_shadow_height;
        }
    }
    pad = (4 - ((w * 3) & 3)) & 3;
    filesz = 54 + (w * 3 + pad) * h;
    f = fopen(path, "wb");
    if (f) {
        memset(hdr, 0, sizeof hdr);
        hdr[0] = 'B'; hdr[1] = 'M';
        memcpy(hdr + 2, &filesz, 4);
        hdr[10] = 54;
        hdr[14] = 40;
        memcpy(hdr + 18, &w, 4);
        memcpy(hdr + 22, &h, 4);
        hdr[26] = 1;
        hdr[28] = 24;
        fwrite(hdr, 1, sizeof hdr, f);
        for (y = h; y-- > 0; ) {         /* BMP rows run bottom-up */
            const uint8_t *row = (const uint8_t *)lr.pBits + (size_t)y * (size_t)lr.Pitch;
            for (x = 0; x < w; x++) {
                const uint8_t *p = row + x * 4;
                uint8_t bgr[3] = { p[2], p[1], p[0] };
                fwrite(bgr, 1, 3, f);
            }
            fwrite("\0\0\0", 1, pad, f);
        }
        fclose(f);
        ok = 0;
    }
    surf->lpVtbl->UnlockRect(surf);
    surf->lpVtbl->Release(surf);
    return ok;
}

/* For frame interpolation's RECOMP_INTERP_DUMP: an in-between frame, read
 * while it is the back buffer. */
int hle_d3d8_dump_back_buffer(const char *path)
{
    return dump_back_buffer(path);
}

static void shadow_dump_frame(void)
{
    static const char *prefix;
    static int configured, every = 300, written, keep_last;
    static unsigned long min_draws, last_dump, from_swap;
    static char ring[24][512];
    static char asked_prefix[8];
    int asked;
    char path[512];

    if (!configured) {
        const char *e = getenv("RECOMP_HLE_D3D8_DUMP_EVERY");
        configured = 1;
        prefix = getenv("RECOMP_HLE_D3D8_DUMP");
        if (e && atoi(e) > 0)
            every = atoi(e);
        /* RECOMP_HLE_D3D8_DUMP_MINDRAWS=<n>: dump only frames with at least n
         * draws, at least `every` swaps apart. A title's 3D frames can be rare
         * among its 2D ones, and a fixed interval keeps missing them. */
        e = getenv("RECOMP_HLE_D3D8_DUMP_MINDRAWS");
        if (e && atol(e) > 0)
            min_draws = (unsigned long)atol(e);
        /* RECOMP_HLE_D3D8_DUMP_FROM=<swap>: nothing before this swap, so the
         * 24 dumps can bracket a moment late in a run instead of its start. */
        e = getenv("RECOMP_HLE_D3D8_DUMP_FROM");
        if (e && atol(e) > 0)
            from_swap = (unsigned long)atol(e);
        /* RECOMP_HLE_D3D8_DUMP_KEEP_LAST=<k>: the first 24-k dumps are kept
         * as usual and the last k slots roll, always holding the latest
         * frames, as <prefix>_s<swap>.bmp. Without it a fast title fills the
         * 24 files in its first few hundred swaps: Outrun 2's matrix frames
         * all showed the intro logos while it was 90 seconds into a race. */
        e = getenv("RECOMP_HLE_D3D8_DUMP_KEEP_LAST");
        if (e && atoi(e) > 0)
            keep_last = atoi(e) > 24 ? 24 : atoi(e);
    }
    /* Asked for by hand: this frame, wherever the run has got to, whatever
     * the interval and the 24-file cap say, and beside the executable when
     * no prefix was given -- so pressing the key is the whole procedure. */
    asked = g_dump_requested;
    g_dump_requested = 0;
    if (asked && (!prefix || !*prefix)) {
        snprintf(asked_prefix, sizeof asked_prefix, "frame");
        prefix = asked_prefix;
    }
    if (!asked) {
        if (!prefix || !*prefix || g_shadow_swaps < from_swap)
            return;
        if (written >= 24 && !keep_last)
            return;
        if (min_draws) {
            if (g_frame_draws < min_draws ||
                (last_dump && g_shadow_swaps - last_dump < (unsigned long)every))
                return;
            last_dump = g_shadow_swaps;
        } else if ((g_shadow_swaps % (unsigned long)every) != 0) {
            return;
        }
    }

    if (!asked && keep_last && written >= 24 - keep_last) {
        char *slot = ring[(written - (24 - keep_last)) % keep_last];

        if (slot[0])
            remove(slot);
        snprintf(path, sizeof path, "%s_s%08lu.bmp", prefix, g_shadow_swaps);
        snprintf(slot, sizeof ring[0], "%s", path);
        written++;
    } else {
        snprintf(path, sizeof path, "%s%03d.bmp", prefix, written++);
    }
    if (dump_back_buffer(path) == 0)
        fprintf(stderr, "[HLE-D3D8] shadow frame %lu -> %s\n", g_shadow_swaps, path);
}

#endif /* _WIN32 */

HLE_ORIGINAL(Direct3D_CreateDevice);
HLE_ORIGINAL(D3DDevice_Clear);
HLE_ORIGINAL(D3DDevice_Swap);
/* The pre-Swap flip, for XDKs that have only this one. */
HLE_ORIGINAL(D3DDevice_Present);
HLE_ORIGINAL(D3DDevice_CreateVertexShader);
HLE_ORIGINAL(D3DDevice_SetVertexShader);
HLE_ORIGINAL(D3DDevice_SelectVertexShader);
HLE_ORIGINAL(D3DDevice_LoadVertexShaderProgram);
HLE_ORIGINAL(D3DDevice_SetTransform);
HLE_ORIGINAL(D3DDevice_SetMaterial);
HLE_ORIGINAL(D3DDevice_SetLight);
HLE_ORIGINAL(D3DDevice_LightEnable);
HLE_ORIGINAL(D3DDevice_SetViewport);
HLE_ORIGINAL(D3DDevice_SetScissors);
HLE_ORIGINAL(D3DDevice_CopyRects);
HLE_ORIGINAL(D3DDevice_GetBackBuffer2);
/* The dispatch table, for calling a title's callback. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);

HLE_ORIGINAL(D3DDevice_SetRenderTarget);
HLE_ORIGINAL(D3DDevice_SetPixelShader);
HLE_ORIGINAL(D3DDevice_SetVertexDataColor);
HLE_ORIGINAL(D3DDevice_SetVertexData2f);
HLE_ORIGINAL(D3DDevice_DrawVerticesUP);
HLE_ORIGINAL(D3DDevice_DrawIndexedVerticesUP);

/* The GPU "time" fence.
 *
 * D3D8 stamps the push buffer with a rising fence value and keeps the value
 * the GPU has reached in a word of guest memory the device points at; the
 * GPU's interrupt writes it. D3D_BlockOnTime(time) spins until that word
 * catches up, and D3D_KickOffAndWaitForIdle, BlockOnFence, BlockOnResource
 * and Swap all wait through it. Nothing here raises that interrupt, so the
 * word never moves and the first wait is the last thing the title does:
 * TimeSplitters 2 stopped after its first Swap, 270 functions in, with the
 * ISR still ticking.
 *
 * The kernel's fence mirror is the answer -- it copies the device's
 * submitted value onto the completed word every poll, the same
 * acknowledgement DMA_GET = DMA_PUT makes for the FIFO. It needs the two
 * device offsets, and those move between XDK builds (5344: +0x2C submitted,
 * +0x30 pointer to completed; 4721: +0x30 and +0x34). Rather than a table by
 * XDK version, read them from D3D_BlockOnTime's own prologue, which is the
 * one place they are certainly right for this title:
 *
 *     8B 3D <g_pDevice> mov  edi, [D3D_g_pDevice]
 *     8B 47 <A>         mov  eax, [edi + A]     ; pointer to the completed word
 *     8B 08             mov  ecx, [eax]
 *     8B 47 <B>         mov  eax, [edi + B]     ; last submitted value
 *
 * Which register holds the device varies. 5344 and 4721 load it into esi
 * first thing; 4134 (Jet Set Radio Future) puts the `time` argument in esi
 * and the device in edi, so the same three reads are 8B 47 rather than
 * 8B 46. The scan below therefore looks for the load of D3D_g_pDevice
 * wherever it sits in the prologue, takes the register from its ModRM, and
 * reads A and B against that -- which also rejects a prologue reading some
 * other global without a separate check. A build laid out differently still
 * gets a line saying so and no mirror, which is the hang this replaces,
 * not a wrong write.
 */
HLE_IMPORT_VAR(D3D_g_pDevice);
HLE_IMPORT_VAR(D3D_BlockOnTime);
/* For the swap throttle's counter pair, read out of its own code below. */
HLE_IMPORT_VAR(D3DDevice_Present);

/* `mov eax, [reg + disp]` at code[0], with an 8- or 32-bit displacement.
 * Returns the instruction's length and writes the displacement, or 0 when it
 * is not that instruction. Both widths, because the offset a build uses
 * decides the width for it: 0x3F0 on XDK 3925 does not fit in a byte. */
static int mov_reg_from_reg(const uint8_t *code, int dest, int reg,
                            uint32_t *disp)
{
    uint8_t modrm;

    if (code[0] != 0x8B)
        return 0;
    modrm = code[1];
    if (((modrm >> 3) & 7) != (uint8_t)dest)
        return 0;
    if ((modrm & 7) != (uint8_t)reg || (modrm & 7) == 4)  /* no SIB form */
        return 0;
    if ((modrm >> 6) == 1) {
        *disp = code[2];
        return 3;
    }
    if ((modrm >> 6) == 2) {
        uint32_t d;
        memcpy(&d, code + 2, 4);
        *disp = d;
        return 6;
    }
    return 0;
}

static int mov_eax_from_reg(const uint8_t *code, int reg, uint32_t *disp)
{
    return mov_reg_from_reg(code, 0, reg, disp);
}

/*
 * The swap throttle's counter pair, read out of D3DDevice_Present.
 *
 * XDK 3925 throttles frames in Present itself rather than waiting on the GPU
 * time fence:
 *
 *      mov  esi, [D3D_g_pDevice]
 *      ...
 *   L: mov  eax, [esi + 0x2518]      ; frames completed -- the GPU moves this
 *      mov  ecx, [esi + 0x2B60]      ; frames submitted -- the title moves it
 *      sub  ecx, eax
 *      cmp  ecx, 2
 *      jae  L
 *
 * Nothing here is a GPU, so "completed" never moved and Max Payne spun in
 * Present forever -- 88% of its main thread, in a four-instruction delay loop
 * the XDK calls between polls. Mirroring submitted onto completed is the
 * truthful answer for the same reason the DMA_PUT/GET acknowledgement is: the
 * frames really have been drawn, by the host, by the time Present returns.
 *
 * The offsets are read from the title's own code rather than tabulated,
 * because they are a per-build detail and reading them is what makes this
 * work on the next XDK without a new table entry. xbox_Nv2aMirrorCounter
 * copies rather than increments, so it cannot run ahead of the title and the
 * unsigned subtraction above cannot underflow.
 */
static void mirror_swap_throttle(void)
{
    static int done;
    const uint8_t *code;

    if (done)
        return;
    done = 1;
    if (!hle_var_D3D_g_pDevice || !hle_var_D3DDevice_Present)
        return;             /* a build that throttles through the fence */

    code = (const uint8_t *)HLE_PTR(hle_var_D3DDevice_Present);
    {
        enum { SCAN = 160 };
        int at = -1, reg = -1, i;
        uint32_t completed = 0, submitted = 0;

        for (i = 0; i + 6 <= SCAN; i++) {
            uint32_t addr;
            if (code[i] != 0x8B || (code[i + 1] & 0xC7) != 0x05)
                continue;
            memcpy(&addr, code + i + 2, 4);
            if (addr != hle_var_D3D_g_pDevice)
                continue;
            reg = (code[i + 1] >> 3) & 7;
            at = i + 6;
            break;
        }
        if (at < 0)
            return;         /* Present does not hold the device in a register */

        /* `mov eax,[reg+A]; mov ecx,[reg+B]; sub ecx,eax` -- the pair, in the
         * order the throttle reads them. 2B C8 is `sub ecx, eax`. */
        for (i = at; i < at + SCAN; i++) {
            int n1 = mov_eax_from_reg(code + i, reg, &completed);
            int n2;
            if (!n1)
                continue;
            /* the second load is into ecx: same encoding, reg field 1 */
            n2 = mov_reg_from_reg(code + i + n1, 1, reg, &submitted);
            if (!n2)
                continue;
            if (code[i + n1 + n2] != 0x2B || code[i + n1 + n2 + 1] != 0xC8)
                continue;
            if (xbox_Nv2aMirrorCounter(hle_var_D3D_g_pDevice,
                                       submitted, completed) == 0)
                fprintf(stderr, "[HLE-D3D8] swap throttle mirrored: device "
                        "+0x%X (submitted) -> +0x%X (completed), offsets read "
                        "from D3DDevice_Present\n", submitted, completed);
            return;
        }
        fprintf(stderr, "[HLE-D3D8] swap throttle not mirrored: D3DDevice_Present "
                "at 0x%08X loads the device into r%d but no counter pair "
                "follows; if it spins there, this is why\n",
                hle_var_D3DDevice_Present, reg);
    }
}

static void mirror_gpu_time_fence(void)
{
    static int done;
    const uint8_t *code;
    uint32_t get_ptr_off, put_off;

    if (done)
        return;
    done = 1;
    if (!hle_var_D3D_g_pDevice || !hle_var_D3D_BlockOnTime) {
        fprintf(stderr, "[HLE-D3D8] GPU time fence not mirrored: %s not named in "
                        "this XBE\n",
                hle_var_D3D_g_pDevice ? "D3D_BlockOnTime" : "D3D_g_pDevice");
        return;
    }
    code = (const uint8_t *)HLE_PTR(hle_var_D3D_BlockOnTime);
    {
        /* Find `mov <reg>, [D3D_g_pDevice]` rather than assuming which
         * register holds the device or where the load sits. ModRM for
         * `mov r32, [disp32]` is (reg << 3) | 0x05, so the destination is
         * (modrm >> 3) & 7 and the address follows it. Matching on the
         * address means a prologue that reads some other global is rejected
         * for free, which the separate check used to do. */
        enum { SCAN = 24 };
        int at = -1, reg = -1, i;

        for (i = 0; i + 6 <= SCAN; i++) {
            uint32_t addr;
            if (code[i] != 0x8B || (code[i + 1] & 0xC7) != 0x05)
                continue;
            memcpy(&addr, code + i + 2, 4);
            if (addr != hle_var_D3D_g_pDevice)
                continue;
            reg = (code[i + 1] >> 3) & 7;
            at = i + 6;
            break;
        }
        if (at < 0) {
            fprintf(stderr, "[HLE-D3D8] GPU time fence not mirrored: "
                            "D3D_BlockOnTime at 0x%08X does not load the device "
                            "from 0x%08X in its first %d bytes (starts %02X %02X "
                            "%02X %02X)\n",
                    hle_var_D3D_BlockOnTime, hle_var_D3D_g_pDevice, (int)SCAN,
                    code[0], code[1], code[2], code[3]);
            return;
        }
        /* Then, against that register:
         *     mov eax, [reg + A]   ; -> completed word
         *     mov ecx, [eax]       ; 8B 08
         *     mov eax, [reg + B]   ; last submitted value
         *
         * Neither the displacement width nor the position is fixed. XDK 3925
         * reads the device at +0x3F0, which needs a 32-bit displacement where
         * later builds use an 8-bit one, and it puts a conditional jump
         * between the device load and these three instructions. Matching only
         * an 8-bit displacement immediately after the load found neither, and
         * Max Payne's main thread then blocked on a fence nothing advanced. */
        {
            enum { FENCE_SCAN = 64 };
            uint32_t a = 0, b = 0;
            int p, n1, n2, found = 0;

            for (p = at; p < at + FENCE_SCAN; p++) {
                n1 = mov_eax_from_reg(code + p, reg, &a);
                if (!n1)
                    continue;
                if (code[p + n1] != 0x8B || code[p + n1 + 1] != 0x08)
                    continue;
                n2 = mov_eax_from_reg(code + p + n1 + 2, reg, &b);
                if (!n2)
                    continue;
                found = 1;
                break;
            }
            if (!found) {
                fprintf(stderr, "[HLE-D3D8] GPU time fence not mirrored: "
                                "D3D_BlockOnTime at 0x%08X loads the device into "
                                "r%d but no `mov eax,[r%d+A]; mov ecx,[eax]; "
                                "mov eax,[r%d+B]` follows within %d bytes "
                                "(starts %02X %02X %02X %02X)\n",
                        hle_var_D3D_BlockOnTime, reg, reg, reg, (int)FENCE_SCAN,
                        code[at], code[at + 1], code[at + 2], code[at + 3]);
                return;
            }
            get_ptr_off = a;
            put_off = b;
        }
    }
    if (xbox_Nv2aMirrorFence(hle_var_D3D_g_pDevice, put_off, get_ptr_off) == 0)
        fprintf(stderr, "[HLE-D3D8] GPU time fence mirrored: device +0x%02X -> "
                        "*(device +0x%02X), offsets read from D3D_BlockOnTime\n",
                put_off, get_ptr_off);
}

/* tools.recomp keeps the original whenever it replaces one of these names and
 * lifts its body, so a missing one means a build/lift mismatch or a body this
 * lift left out. Returning without running it would turn the call into a
 * silent no-op, so it is reported every time. */
static int original_missing(void (*fn)(void), const char *name)
{
    if (fn)
        return 0;
    fprintf(stderr, "[HLE] %s: original body missing -- regenerate the lift\n", name);
    return 1;
}

/* HRESULT Direct3D_CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType,
 *     HWND hFocusWindow, DWORD BehaviorFlags,
 *     D3DPRESENT_PARAMETERS *pPresentationParameters,
 *     IDirect3DDevice8 **ppReturnedDeviceInterface)                        */
HLE_EXPORT(Direct3D_CreateDevice)
{
    static int seen;
    uint32_t pp_va = HLE_ARG(4);

    first_call(&seen, "Direct3D_CreateDevice", pp_va);
    if (original_missing(hle_original_Direct3D_CreateDevice, "Direct3D_CreateDevice"))
        HLE_RETURN(0x80004005u);                 /* E_FAIL */
#ifdef _WIN32
    g_in_create_device = 1;
#endif
    /* Before the original, not after: some XDKs wait on the fence inside
     * CreateDevice itself. XGRA's (5558) calls D3D_KickOffAndWaitForIdle
     * there and spun in D3D_BlockOnTime before the device was ever returned.
     * The kernel follows the device pointer afresh on every poll and skips
     * it while it is still zero, so the mirror can go in before the device
     * exists. */
    mirror_gpu_time_fence();
    HLE_CALL_ORIGINAL(Direct3D_CreateDevice);
    mirror_swap_throttle();
#ifdef _WIN32
    g_in_create_device = 0;
    if (pp_va) {
        /* D3DPRESENT_PARAMETERS: BackBufferCount +0xC, SwapEffect +0x14,
         * Flags +0x28, FullScreen_PresentationInterval +0x30. The swap effect
         * decides what a Swap means (frame_end_shadow). */
        g_swap_effect = HLE_MEM32(pp_va + 0x14);
        fprintf(stderr, "[HLE-D3D8] CreateDevice: %ux%u format 0x%02X, %u back buffer(s), "
                "swap effect %u (1 discard, 2 flip, 3 copy, 4 copy vsync), flags 0x%08X, "
                "refresh %u Hz, presentation interval 0x%08X -> 0x%08X\n",
                HLE_MEM32(pp_va + 0x0), HLE_MEM32(pp_va + 0x4), HLE_MEM32(pp_va + 0x8),
                HLE_MEM32(pp_va + 0xC), g_swap_effect, HLE_MEM32(pp_va + 0x28),
                HLE_MEM32(pp_va + 0x2C), HLE_MEM32(pp_va + 0x30), g_eax);
    }
    if (!g_backbuffer_va)
        fprintf(stderr, "[HLE-D3D8] CreateDevice set no render target; the back "
                "buffer is taken to be any parentless surface of its size\n");
    /* Only beside a guest device that exists: the original's HRESULT. */
    if (shadow_requested() && !g_shadow_tried && (int32_t)g_eax >= 0)
        shadow_create(pp_va);
#endif
}

/* HRESULT D3DDevice_Clear(DWORD Count, const D3DRECT *pRects, DWORD Flags,
 *     D3DCOLOR Color, float Z, DWORD Stencil)                               */
HLE_EXPORT(D3DDevice_Clear)
{
    static int seen;
#ifdef _WIN32
    uint32_t flags = HLE_ARG(2), color = HLE_ARG(3);
    uint32_t z_bits = HLE_ARG(4), stencil = HLE_ARG(5);
#endif

    first_call(&seen, "D3DDevice_Clear", HLE_ARG(2));
    if (original_missing(hle_original_D3DDevice_Clear, "D3DDevice_Clear"))
        HLE_RETURN(0x80004005u);
    HLE_CALL_ORIGINAL(D3DDevice_Clear);
#ifdef _WIN32
    if (g_shadow) {
        float z;

        memcpy(&z, &z_bits, sizeof z);
        if (g_shadow_clears < 4)
            fprintf(stderr, "[HLE-D3D8] shadow Clear: flags 0x%08X color 0x%08X "
                    "z %g stencil %u\n", flags, color, z, stencil);
        host_Clear(g_shadow, 0, NULL, xbox_clear_flags_to_host(flags), color, z, stencil);
        g_shadow_clears++;
        g_shadow_last_color = color;
        if (shadow_trace_on())
            fprintf(stderr, "[TRACE swap %lu] Clear flags 0x%X color 0x%08X\n",
                    g_shadow_swaps, flags, color);
    }
#endif
}

/* Where a frame's time goes around Swap, for the five-second report. */
static LARGE_INTEGER g_swap_last;
static long long g_swap_gate_ticks, g_swap_body_ticks, g_swap_frame_ticks;
static unsigned long g_swap_timed;
/* Inside frame_end_shadow: running a deferred frame's queue (the frame's
 * drawing, under RECOMP_HLE_D3D8_DEFER) and the host present. The longest
 * of each too, since a stall is one frame, not an average. */
static long long g_swap_flush_ticks, g_swap_present_ticks;
static long long g_swap_flush_max, g_swap_present_max;

static void swap_timing_report(void)
{
    LARGE_INTEGER qpf;
    double ms;

    if (!g_swap_timed)
        return;
    QueryPerformanceFrequency(&qpf);
    ms = 1000.0 / (double)qpf.QuadPart / (double)g_swap_timed;
    fprintf(stderr, "[HLE-D3D8] swap timing over %lu frames: gate wait %.2f ms, "
            "title's Swap %.2f ms, rest of frame %.2f ms (per frame)\n",
            g_swap_timed, (double)g_swap_gate_ticks * ms,
            (double)g_swap_body_ticks * ms, (double)g_swap_frame_ticks * ms);
    fprintf(stderr, "[HLE-D3D8]   of which: deferred draw %.2f ms (worst %.1f), host "
            "present %.2f ms (worst %.1f)\n",
            (double)g_swap_flush_ticks * ms,
            (double)g_swap_flush_max * ms * (double)g_swap_timed,
            (double)g_swap_present_ticks * ms,
            (double)g_swap_present_max * ms * (double)g_swap_timed);
    g_swap_gate_ticks = g_swap_body_ticks = g_swap_frame_ticks = 0;
    g_swap_flush_ticks = g_swap_present_ticks = 0;
    g_swap_flush_max = g_swap_present_max = 0;
    g_swap_timed = 0;
}

/* ------------------------------------------------------------------ overlay
 *
 * A frame-rate counter the player can turn on, and a frame cap they can
 * change without restarting. The renderer draws the line (d3d8_overlay.h);
 * what it says and when it appears is decided here, because this is where
 * the frame rate is already counted and where the flip gate is reachable.
 *
 * F9 shows or hides the counter, F10 steps the cap. Both are read only while
 * the game's window is in front, so they do nothing while the player is in
 * another application, and neither is a key the input bindings offer, so
 * neither can collide with a control. RECOMP_FPS_OVERLAY=1 starts with the
 * counter already on.
 *
 * The rate is measured over half-second windows at the same Swap that
 * RECOMP_FPS counts, so the number on screen and the number in the log are
 * the same measurement.
 */
/* The overlay's line, and whether it is shown: kept here so an in-between
 * frame (hle_d3d8_interp.c) carries the same line as the frames around it. */
static int  g_overlay_shown;
static char g_overlay_line[96];

void hle_d3d8_overlay_redraw(void)
{
    if (g_overlay_shown)
        d3d8_overlay_draw(g_overlay_line);
}

static void overlay_frame(void)
{
    static int configured, f9_was_down, f10_was_down, f11_was_down;
    static LARGE_INTEGER qpf, window_start;
    static unsigned window_frames;
    static unsigned long window_extra;
    int enabled = g_overlay_shown;
    char *line = g_overlay_line;
    const size_t line_size = sizeof g_overlay_line;
    LARGE_INTEGER now;
    int front, f9, f10, f11;

    if (!configured) {
        const char *v = recomp_config_lookup("RECOMP_FPS_OVERLAY", "fps_overlay");

        configured = 1;
        enabled = v && *v && strcmp(v, "0") != 0;
        QueryPerformanceFrequency(&qpf);
        QueryPerformanceCounter(&window_start);
        snprintf(line, line_size, "-- fps   cap %s", xbox_Nv2aFlipGateModeName());
        fprintf(stderr, "[HLE-D3D8] F9 shows the frame rate on screen, F10 steps the "
                "frame cap (now %s)\n", xbox_Nv2aFlipGateModeName());
        fflush(stderr);
    }

    front = g_shadow_hwnd && GetForegroundWindow() == g_shadow_hwnd;
    f9  = front && (GetAsyncKeyState(VK_F9)  & 0x8000) != 0;
    f10 = front && (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    f11 = front && (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    if (f11 && !f11_was_down) {
        /* Both, because they answer different questions: the picture shows
         * what is wrong, the capture lets it be replayed draw by draw with
         * no game running (src/replay). */
        hle_d3d8_capture_next_frame();
        shadow_dump_next_frame();
    }
    f11_was_down = f11;
    if (f9 && !f9_was_down)
        enabled = !enabled;
    if (f10 && !f10_was_down) {
        xbox_Nv2aFlipGateCycle();
        window_start.QuadPart = 0;           /* the old window straddles the change */
    }
    f9_was_down = f9;
    f10_was_down = f10;

    QueryPerformanceCounter(&now);
    if (!window_start.QuadPart) {
        window_start = now;
        window_frames = 0;
        window_extra = hle_d3d8_interp_presents();
    }
    window_frames++;
    if (qpf.QuadPart &&
        now.QuadPart - window_start.QuadPart >= qpf.QuadPart / 2) {
        double secs = (double)(now.QuadPart - window_start.QuadPart) / (double)qpf.QuadPart;
        unsigned long extra = hle_d3d8_interp_presents() - window_extra;

        /* With frame interpolation, what reaches the screen as well. */
        if (extra)
            snprintf(line, line_size, "%.1f fps (%.1f shown)   cap %s",
                     (double)window_frames / secs,
                     (double)(window_frames + extra) / secs, xbox_Nv2aFlipGateModeName());
        else
            snprintf(line, line_size, "%.1f fps   cap %s",
                     (double)window_frames / secs, xbox_Nv2aFlipGateModeName());
        window_start = now;
        window_frames = 0;
        window_extra = hle_d3d8_interp_presents();
    }

    g_overlay_shown = enabled;
    if (enabled)
        d3d8_overlay_draw(line);
}

/* HRESULT D3DDevice_Swap(DWORD Flags)                                       */
#ifdef _WIN32
/* Everything a completed frame needs after the title's own flip has run:
 * the capture boundary, the frame dump, the overlay, the host present and
 * the five-second report. Shared because a title reaches this point
 * through either entry point -- see the Present replacement below. */
/* A movie went over the last frame on the movie layer, which is not one of
 * the frame's own draws: frame interpolation stands aside. */
static int g_movie_drawn;

int hle_d3d8_movie_layer_shown(void)
{
    return g_movie_drawn;
}

static void frame_end_shadow(void)
{
    if (g_shadow) {
        DWORD now = GetTickCount();
        DWORD thread = GetCurrentThreadId();

        /* Which guest threads swap, against the one that created the device.
         * Capped, in case they alternate. */
        if (thread != g_shadow_swap_thread && g_shadow_thread_notes < 8) {
            fprintf(stderr, "[HLE-D3D8] shadow: Swap on guest thread %lu (device "
                    "created on %lu)\n", (unsigned long)thread,
                    (unsigned long)g_shadow_create_thread);
            g_shadow_thread_notes++;
        }
        g_shadow_swap_thread = thread;

        /* Deferred frames (RECOMP_HLE_D3D8_DEFER): the frame's device calls
         * run now, before anything reads the result -- the capture boundary,
         * the dump, the present. */
        {
            LARGE_INTEGER a, b;
            QueryPerformanceCounter(&a);
            hle_d3d8_defer_flush();
            QueryPerformanceCounter(&b);
            g_swap_flush_ticks += b.QuadPart - a.QuadPart;
            if (b.QuadPart - a.QuadPart > g_swap_flush_max)
                g_swap_flush_max = b.QuadPart - a.QuadPart;
        }

        /* Textures this frame drew with, checked again now that the frame
         * is finished (hle_d3d8_texture.c). */
        hle_d3d8_texture_frame_end();

        g_shadow_swaps++;
        /* The frame boundary for capture: closes the frame being recorded, or
         * starts recording if this is the requested swap. */
        hle_d3d8_capture_swap(g_shadow_swaps, g_shadow_width, g_shadow_height);
        /* A movie on the video overlay (UpdateOverlay, below) is a plane the
         * scan-out puts over the frame buffer; here it is drawn over the
         * finished frame, before the dump so captures show it. */
        g_movie_drawn = 0;
        if (g_overlay_enabled && g_overlay_updated) {
            d3d8_movie_draw();
            g_movie_drawn = 1;
        } else if (g_movie_phys &&
                   (!g_movie_sampled || g_movie_yuy2 || movie_layer_forced())) {
            /* A movie is playing and no draw this frame sampled its picture.
             * The title shows it by a way the host cannot see -- XGRA and
             * Breakdown draw through push buffers they fill themselves -- so
             * the picture goes over the frame the way the overlay's does.
             * A title that draws the movie surface itself (Black) samples it
             * and is left alone.
             *
             * A YUY2 movie surface goes on the layer whatever the title
             * draws. YUY2 is the video overlay's format: Future Perfect and
             * Breakdown use it that way, and Otogi, which textures from it,
             * does so through a two-pass draw that comes out black here while
             * the layer shows its promo exactly (RECOMP_XMV_LAYER=1). The
             * cost is that anything drawn over such a movie is covered. */
            d3d8_movie_draw();
            g_movie_drawn = 1;
        }
        g_movie_sampled = 0;
        shadow_dump_frame();             /* before Present discards the buffer */
        shadow_frame_brightness();       /* likewise: Present discards it */
        if (g_inline_begin_frame > g_inline_begin_max)
            g_inline_begin_max = g_inline_begin_frame;
        if (g_inline_vdata_frame > g_inline_vdata_max)
            g_inline_vdata_max = g_inline_vdata_frame;
        g_inline_begin_frame = 0;
        g_inline_vdata_frame = 0;
        g_frame_draws = 0;
        overlay_frame();                 /* after the dump: not in the captures */
        {
            LARGE_INTEGER a, b;
            QueryPerformanceCounter(&a);
            host_Swap(g_shadow, 0);
            QueryPerformanceCounter(&b);
            g_swap_present_ticks += b.QuadPart - a.QuadPart;
            if (b.QuadPart - a.QuadPart > g_swap_present_max)
                g_swap_present_max = b.QuadPart - a.QuadPart;
        }
        /* The frame just shown is the one frame interpolation draws toward
         * next; it starts keeping the one after. */
        hle_d3d8_interp_frame_end();
        if (!g_shadow_last_report) {
            g_shadow_last_report = now;
        } else if (now - g_shadow_last_report >= 5000) {
            fprintf(stderr, "[HLE-D3D8] shadow: %lu swaps, %lu clears, last clear "
                    "color 0x%08X | draws: %lu UP + %lu indexed UP + %lu buffer + "
                    "%lu indexed buffer drawn (%lu of them fixed function by "
                    "declaration); skipped %lu program without layout, %lu "
                    "declaration shader, %lu unknown shader, %lu stride, %lu "
                    "primitive, %lu failed; %lu off the swapping thread\n",
                    g_shadow_swaps, g_shadow_clears, g_shadow_last_color,
                    g_draws_up, g_draws_indexed_up, g_draws_vb, g_draws_indexed_vb,
                    g_draws_ff_declaration,
                    g_draws_program, g_draws_declaration, g_draws_unknown_vs,
                    g_draws_stride, g_draws_primitive, g_draws_failed,
                    g_draws_off_thread);
            if (g_inline_begin || g_inline_vdata)
                fprintf(stderr, "[HLE-D3D8] inline vertex path: %lu Begin, %lu End, "
                        "%lu SetVertexData4f; %lu drawn, %lu under a vertex program "
                        "(not drawn); peak per frame %lu Begin, %lu vertex data\n",
                        g_inline_begin, g_inline_end, g_inline_vdata, g_inline_drawn,
                        g_inline_program, g_inline_begin_max, g_inline_vdata_max);
            if (g_skipped_fullscreen)
                fprintf(stderr, "[HLE-D3D8] shadow: %lu full-screen passes over the "
                        "title's own frame dropped (RECOMP_HLE_D3D8_SKIP_FULLSCREEN)\n",
                        g_skipped_fullscreen);
            swap_timing_report();
            hle_d3d8_interp_report();
            if (g_slot_reloads)
                fprintf(stderr, "[HLE-D3D8] shadow vertex programs: %lu loads answered from "
                        "the %d cached host programs; %lu replaced the selected program\n",
                        g_slot_reloads, g_slot_program_count, g_slot_rebinds);
            if (g_target_sets)
                fprintf(stderr, "[HLE-D3D8] shadow render targets: %lu set, %lu to a "
                        "scratch target, %lu failed\n", g_target_sets,
                        g_target_scratch, g_target_failed);
            if (g_shadow_map_sets)
                fprintf(stderr, "[HLE-D3D8] shadow maps: targeted %lu time(s), %lu draw(s) "
                        "into them; 256 wide and up: %lu, %lu draw(s)\n", g_shadow_map_sets,
                        g_shadow_map_draws, g_shadow_map_big_sets, g_shadow_map_big_draws);
            if (g_push_count)
                fprintf(stderr, "[HLE-D3D8] push buffer: %lu push(es), %lu draw(s) replayed, "
                        "%lu stopped at another method, %lu with no vertex size\n",
                        g_push_count, g_push_draws, g_push_other, g_push_nostride);
            fflush(stderr);
            g_shadow_last_report = now;
        }
    }
}
#endif

HLE_EXPORT(D3DDevice_Swap)
{
    static int seen;

    /* D3DSWAP_FINISH (4) without D3DSWAP_COPY (1) or D3DSWAP_BYPASSCOPY (2)
     * only finishes the frame: the XDK's Swap (Flags 0 means 5) counts a new
     * frame only when one of the low two bits is set. TimeSplitters: Future
     * Perfect's pause and game-over screens call Swap(2) and then Swap(4)
     * with nothing drawn between; presented, gated to a vblank and counted
     * as a frame of its own, that second call put an empty frame between
     * every two of the title's -- the screen behind those menus flickered
     * black at 30 Hz. The title's own body still runs. */
    if (HLE_ARG(0) != 0 && !(HLE_ARG(0) & 3u)) {
        first_call(&seen, "D3DDevice_Swap", HLE_ARG(0));
        if (original_missing(hle_original_D3DDevice_Swap, "D3DDevice_Swap"))
            HLE_RETURN(0x80004005u);
        HLE_CALL_ORIGINAL(D3DDevice_Swap);
        return;
    }
    /* Counted before anything else here runs, so RECOMP_FPS means the same
     * thing whatever is switched on below. */
    xbox_FpsCountSwap();
#ifdef _WIN32
    if (g_backbuffer_va)
        note_framebuffer_phys(HLE_MEM32(g_backbuffer_va + 4));
    if (shadow_trace_on())
        fprintf(stderr, "[TRACE swap %lu] Swap flags 0x%X (back buffer 0x%08X data 0x%08X)\n",
                g_shadow_swaps, HLE_ARG(0), g_backbuffer_va,
                g_backbuffer_va ? HLE_MEM32(g_backbuffer_va + 4) : 0);
#endif
    first_call(&seen, "D3DDevice_Swap", HLE_ARG(0));
    if (original_missing(hle_original_D3DDevice_Swap, "D3DDevice_Swap"))
        HLE_RETURN(0x80004005u);
    /* Console pacing: the flip gate sleeps here until the next vblank
     * (xbox_memory_layout.h), before the title's own Swap runs. The three
     * times are kept for the five-second report below: how long the gate
     * held, how long the title's own Swap took, and the rest of the frame. */
    {
        LARGE_INTEGER t0, t1, t2;
        QueryPerformanceCounter(&t0);
        if (g_swap_last.QuadPart)
            g_swap_frame_ticks += t0.QuadPart - g_swap_last.QuadPart;
        xbox_Nv2aFlipGateArm();
        QueryPerformanceCounter(&t1);
        HLE_CALL_ORIGINAL(D3DDevice_Swap);
        QueryPerformanceCounter(&t2);
        g_swap_gate_ticks += t1.QuadPart - t0.QuadPart;
        g_swap_body_ticks += t2.QuadPart - t1.QuadPart;
        g_swap_last = t2;
        g_swap_timed++;
    }
#ifdef _WIN32
    frame_end_shadow();
#endif
}

/* HRESULT D3DDevice_Present(const RECT *src, const RECT *dst,
 *                           void *dstSurface, void *dirtyRegion)
 *
 * The flip, on an XDK that predates Swap. Max Payne is XDK 3925 and its
 * D3D8 exports Present and no Swap at all, so a shadow renderer that only
 * replaces Swap never sees a frame boundary: it draws, and never presents.
 *
 * Later XDKs export both, and there Present is a wrapper that ends up in
 * Swap. Doing the frame-end work in both would present twice and count
 * every frame twice, so this defers to Swap whenever the title has one and
 * only takes over when it does not.
 */
HLE_EXPORT(D3DDevice_Present)
{
    static int seen;

    first_call(&seen, "D3DDevice_Present", HLE_ARG(0));
    if (original_missing(hle_original_D3DDevice_Present, "D3DDevice_Present"))
        HLE_RETURN(0x80004005u);

    /* The title has a Swap of its own, which this call will reach and which
     * does the frame-end work. Nothing to do here but let it through. */
    if (hle_original_D3DDevice_Swap) {
        HLE_CALL_ORIGINAL(D3DDevice_Present);
        return;
    }

    xbox_FpsCountSwap();
#ifdef _WIN32
    if (g_backbuffer_va)
        note_framebuffer_phys(HLE_MEM32(g_backbuffer_va + 4));
#endif
    {
        LARGE_INTEGER t0, t1, t2;
        QueryPerformanceCounter(&t0);
        if (g_swap_last.QuadPart)
            g_swap_frame_ticks += t0.QuadPart - g_swap_last.QuadPart;
        xbox_Nv2aFlipGateArm();
        QueryPerformanceCounter(&t1);
        HLE_CALL_ORIGINAL(D3DDevice_Present);
        QueryPerformanceCounter(&t2);
        g_swap_gate_ticks += t1.QuadPart - t0.QuadPart;
        g_swap_body_ticks += t2.QuadPart - t1.QuadPart;
        g_swap_last = t2;
        g_swap_timed++;
    }
#ifdef _WIN32
    frame_end_shadow();
#endif
}

#ifdef _WIN32
/* The XDK keeps the declaration it parsed in the shader object itself:
 * X_D3DVertexShader { RefCount, Flags, ProgramSize, ProgramAndConstantsDwords,
 * BYTE Dimensionality[4], X_VERTEXATTRIBUTEFORMAT VertexAttribute } -- 16 slots
 * of { StreamIndex, Offset, Format, 4 bytes }, one per vertex register
 * (Cxbx-Reloaded, XbD3D8Types.h). Logged for the first programs so a run shows
 * which vertex formats a title's programs read before the host is taught
 * them. Format X_D3DVSDT_NONE (0x02) or 0 is an unused register. */
static void note_vertex_attributes(uint32_t handle)
{
    static int notes;
    uint32_t object = handle & ~1u, i;
    char line[640];
    int n;

    if (notes >= 32 || !object)
        return;
    notes++;
    n = snprintf(line, sizeof line, "[HLE-D3D8] shadow declaration 0x%08X flags 0x%X:",
                 handle, HLE_MEM32(object + 4u));
    for (i = 0; i < 16u && n > 0 && n < (int)sizeof line; i++) {
        uint32_t slot = object + 20u + i * 16u;
        uint32_t format = HLE_MEM32(slot + 8u);

        if (format <= 0x02u)
            continue;
        n += snprintf(line + n, sizeof line - (size_t)n, " v%u=s%u+%u:%02X", i,
                      HLE_MEM32(slot), HLE_MEM32(slot + 4u), format);
    }
    fprintf(stderr, "%s\n", line);
}

/* An X_D3DVSDT format the host reads as it is: its DXGI format and size.
 * D3DCOLOR is stored BGRA. Three-component shorts and bytes have no DXGI
 * format, and unnormalised shorts would need an integer shader input, so
 * those return 0. */
static int xbox_vsdt_to_dxgi(uint32_t format, RhiFormat *dxgi, UINT *size)
{
    switch (format) {
    case 0x12: *dxgi = RHI_FORMAT_R32_FLOAT;          *size = 4;  return 1; /* FLOAT1 */
    case 0x22: *dxgi = RHI_FORMAT_R32G32_FLOAT;       *size = 8;  return 1; /* FLOAT2 */
    case 0x32: *dxgi = RHI_FORMAT_R32G32B32_FLOAT;    *size = 12; return 1; /* FLOAT3 */
    case 0x42: *dxgi = RHI_FORMAT_R32G32B32A32_FLOAT; *size = 16; return 1; /* FLOAT4 */
    case 0x40: *dxgi = RHI_FORMAT_B8G8R8A8_UNORM;     *size = 4;  return 1; /* D3DCOLOR */
    case 0x11: *dxgi = RHI_FORMAT_R16_SNORM;          *size = 2;  return 1; /* NORMSHORT1 */
    case 0x21: *dxgi = RHI_FORMAT_R16G16_SNORM;       *size = 4;  return 1; /* NORMSHORT2 */
    case 0x41: *dxgi = RHI_FORMAT_R16G16B16A16_SNORM; *size = 8;  return 1; /* NORMSHORT4 */
    case 0x14: *dxgi = RHI_FORMAT_R8_UNORM;           *size = 1;  return 1; /* PBYTE1 */
    case 0x24: *dxgi = RHI_FORMAT_R8G8_UNORM;         *size = 2;  return 1; /* PBYTE2 */
    case 0x44: *dxgi = RHI_FORMAT_R8G8B8A8_UNORM;     *size = 4;  return 1; /* PBYTE4 */
    default:   return 0;
    }
}

/* An X_D3DVSDT format the host cannot read as it is, expanded to floats per
 * draw (shadow_expand_vertices): how many floats it becomes, with *size the
 * bytes it occupies in the guest vertex, or 0 for a format read directly.
 * NORMPACKED3 has no DXGI format at all. The unnormalised shorts do (R16_SINT
 * and friends) but those need an integer shader input, and the generated
 * programs read floats; TimeSplitters 2's world geometry is SHORT2 texture
 * coordinates and SHORT4 positions, 15% of its in-level draws. */
static int xbox_vsdt_expanded(uint32_t format, UINT *size)
{
    switch (format) {
    case 0x16: *size = 4; return 3;      /* NORMPACKED3, 11:11:10 signed */
    case 0x15: *size = 2; return 1;      /* SHORT1 */
    case 0x25: *size = 4; return 2;      /* SHORT2 */
    case 0x35: *size = 6; return 3;      /* SHORT3 */
    case 0x45: *size = 8; return 4;      /* SHORT4 */
    default:   return 0;
    }
}

static const RhiFormat FLOATN_FORMAT[5] = {
    RHI_FORMAT_UNKNOWN, RHI_FORMAT_R32_FLOAT, RHI_FORMAT_R32G32_FLOAT,
    RHI_FORMAT_R32G32B32_FLOAT, RHI_FORMAT_R32G32B32A32_FLOAT,
};

/* Bytes an X_D3DVSDT type occupies in a vertex (count in the high nibble). */
static UINT vsdt_bytes(uint32_t fmt)
{
    UINT count = fmt >> 4;

    switch (fmt & 0xFu) {
    case 0x0: return 4u;                 /* D3DCOLOR */
    case 0x1: case 0x5: return count * 2u; /* NORMSHORTn, SHORTn */
    case 0x2: return fmt == 0x72u ? 12u  /* FLOAT2H: three floats */
                     : count * 4u;       /* FLOATn; 0x02 is NONE */
    case 0x4: return count;              /* PBYTEn */
    case 0x6: return 4u;                 /* NORMPACKED3 */
    default:  return 0u;
    }
}

/* The declaration from the title's D3DVSD token stream: bits 31..29 are the
 * token type -- 1 STREAM (bit 28 clear: the stream number in the low bits),
 * 2 STREAMDATA (bit 28 clear: REG, the register in bits 4..0 and the type in
 * bits 23..16; bit 28 set: SKIP, a count in bits 26..16 of dwords, or of
 * bytes when bit 27 is set), 3 TESSELLATOR, 4 CONSTMEM (4 * bits 28..25
 * constant dwords follow), 5 EXT (bits 28..24 dwords follow), 0 NOP, and
 * 0xFFFFFFFF ends it. Returns the number of registers it declares. */
static int decl_from_tokens(uint32_t decl, shadow_decl_tokens *t)
{
    uint32_t stream = 0, offset = 0, i;

    memset(t, 0, sizeof *t);
    if (!decl)
        return 0;
    for (i = 0; i < 256u; i++) {
        uint32_t w = HLE_MEM32(decl + 4u * i);
        uint32_t type = w >> 29;

        if (w == 0xFFFFFFFFu)
            break;
        if (type == 1u) {
            if (!(w & 0x10000000u)) {
                stream = w & 0xFu;
                offset = 0;
            }
        } else if (type == 2u) {
            if (w & 0x10000000u) {
                uint32_t n = (w >> 16) & 0x7FFu;
                offset += (w & 0x08000000u) ? n : n * 4u;
            } else {
                uint32_t reg = w & 0x1Fu, fmt = (w >> 16) & 0xFFu;
                if (reg < 16u) {
                    t->stream[reg] = (uint8_t)stream;
                    t->offset[reg] = (uint16_t)offset;
                    t->format[reg] = (uint8_t)fmt;
                    t->regs++;
                }
                offset += vsdt_bytes(fmt);
            }
        } else if (type == 4u) {
            i += 4u * ((w >> 25) & 0xFu);
        } else if (type == 5u) {
            i += (w >> 24) & 0x1Fu;
        }
    }
    return t->regs;
}

/* Register i's stream, offset and X_D3DVSDT type: from the tokens when this
 * program uses them, else from the shader object's parsed copy. */
static void decl_attr(const struct shadow_program *p, uint32_t object, uint32_t i,
                      uint32_t *stream, uint32_t *offset, uint32_t *format)
{
    if (p->use_tokens) {
        *stream = p->tok.stream[i];
        *offset = p->tok.offset[i];
        *format = p->tok.format[i];
    } else {
        uint32_t attr = object + 20u + i * 16u;
        *stream = HLE_MEM32(attr);
        *offset = HLE_MEM32(attr + 4u);
        *format = HLE_MEM32(attr + 8u);
    }
}

/* Whether the object's copy says what the tokens say, register by register.
 * Where it does, nothing changes: the object is read as it always was. */
static int decl_object_agrees(const struct shadow_program *p, uint32_t object)
{
    uint32_t i;

    for (i = 0; i < 16u; i++) {
        uint32_t attr = object + 20u + i * 16u;
        uint32_t f = HLE_MEM32(attr + 8u), t = p->tok.format[i];

        if (t <= 0x02u && f <= 0x02u)
            continue;
        if (f != t || HLE_MEM32(attr + 4u) != p->tok.offset[i] ||
            HLE_MEM32(attr) != p->tok.stream[i])
            return 0;
    }
    return 1;
}

/* The host program's vertex layout, from the same slots: each register at its
 * declared offset in the stream 0 vertex. Registers in a format the host
 * cannot read as it is (xbox_vsdt_expanded) are unpacked to floats per draw:
 * each draw copies the vertex behind a prefix holding those floats
 * (shadow_expand_vertices), so they read from the prefix and every other
 * offset moves up by its size. A declaration the host cannot take -- another
 * stream, or a format with no DXGI equivalent -- leaves has_declaration 0,
 * and its draws are skipped and counted.
 *
 * A stream 0 register at an offset that is not a multiple of four is copied
 * into the prefix as it is, like a register from another stream. D3D11 wants
 * every element on a four-byte boundary, and the prefix is always a multiple
 * of four, so such a register would land misaligned on the host however much
 * it moved up. Dino Crisis 3's skinned meshes are FLOAT3 position and normal,
 * FLOAT2 texture coordinates, SHORT3 bone indices at 32 and FLOAT3 weights at
 * 38 -- straight after a six-byte type, so the weights sat at host offset 50,
 * came back wrong, and every character exploded into screen-sized dark
 * triangles over the scene. */
static void shadow_read_declaration(int slot, uint32_t handle)
{
    static int notes;
    struct shadow_program *p = &g_programs[slot];
    D3D8VshInput in[16];
    uint32_t object = handle & ~1u, i;
    uint32_t bad_reg = 0, bad_stream = 0, bad_format = 0;
    UINT shift = 0, out = 0;
    int n = 0, packed = 0, refused = 0;

    p->has_declaration = 0;
    p->extent = 0;
    p->packed_count = 0;
    p->other_streams = 0;
    p->expanded_bytes = 0;
    if (!object)
        return;
    for (i = 0; i < 16u; i++) {
        uint32_t stream, offset, format;
        RhiFormat dxgi;
        UINT size;
        int floats;

        decl_attr(p, object, i, &stream, &offset, &format);
        if (format <= 0x02u)
            continue;
        if ((floats = xbox_vsdt_expanded(format, &size)) != 0) {
            packed++;
            shift += (UINT)floats * 4u;
        } else if ((stream != 0u || (offset & 3u)) &&
                   xbox_vsdt_to_dxgi(format, &dxgi, &size)) {
            packed++;
            shift += (size + 3u) & ~3u;
        }
    }
    if (packed > SHADOW_MAX_PACKED)
        return;
    packed = 0;

    for (i = 0; i < 16u; i++) {
        uint32_t stream, offset, format;
        RhiFormat dxgi;
        UINT size;
        int floats;

        decl_attr(p, object, i, &stream, &offset, &format);
        if (format <= 0x02u)
            continue;
        if (stream > 15u || offset > 0xFFFFu) {
            refused = 1;
        } else if ((floats = xbox_vsdt_expanded(format, &size)) != 0) {
            p->packed_offset[packed] = offset;
            p->packed_out[packed] = out;
            p->packed_format[packed] = format;
            p->packed_stream[packed] = (uint8_t)stream;
            p->packed_raw[packed] = 0;
            p->packed_size[packed] = size;
            packed++;
            in[n].format = FLOATN_FORMAT[floats];
            in[n].offset = out;
            out += (UINT)floats * 4u;
        } else if (!xbox_vsdt_to_dxgi(format, &dxgi, &size)) {
            refused = 1;
        } else if (stream != 0u || (offset & 3u)) {
            p->packed_offset[packed] = offset;
            p->packed_out[packed] = out;
            p->packed_format[packed] = format;
            p->packed_stream[packed] = (uint8_t)stream;
            p->packed_raw[packed] = 1;
            p->packed_size[packed] = size;
            packed++;
            in[n].format = dxgi;
            in[n].offset = out;
            out += (size + 3u) & ~3u;
        } else {
            in[n].format = dxgi;
            in[n].offset = offset + shift;
        }
        if (refused) {
            bad_reg = i;
            bad_stream = stream;
            bad_format = format;
            break;
        }
        in[n].reg = (int)i;
        if (stream != 0u)
            p->other_streams = 1;
        else if (offset + size > p->extent)
            p->extent = offset + size;
        n++;
    }

    if (!refused && n > 0 && SUCCEEDED(host_vsh_set_declaration(p->host, in, n))) {
        p->has_declaration = 1;
        p->packed_count = packed;
        p->expanded_bytes = shift;
    } else if (refused && notes++ < 16) {
        fprintf(stderr, "[HLE-D3D8] shadow declaration 0x%08X: v%u (stream %u, format "
                "0x%02X) has no host layout; its draws are skipped\n",
                handle, bad_reg, bad_stream, bad_format);
    }
}
#endif

/* TEMPORARY DIAGNOSTIC (RECOMP_DECL_TOKENS=1): the declaration token stream
 * exactly as the title supplies it, before anything here parses it, so the
 * register numbers can be read from the title's own data rather than inferred
 * from the XDK's parsed array. Xbox D3DVSD token form: bits 31..29 select the
 * token type -- 1 STREAM (index in the low bits, bit 28 = tessellator
 * stream), 2 STREAMDATA (bit 28 set = SKIP of (t >> 16) & 0xFFF dwords,
 * otherwise REG with the vertex register in the low 5 bits and the X_D3DVSDT
 * data type in bits 23..16), 0 NOP, 3 TESSELLATOR, 4 CONSTMEM, 5 EXT --
 * and 0xFFFFFFFF ends the stream. */
static void note_declaration_tokens(uint32_t decl)
{
    static int notes, enabled = -1;
    uint32_t stream = 0, offset = 0, i;

    if (enabled < 0) {
        const char *e = getenv("RECOMP_DECL_TOKENS");
        enabled = e && *e && *e != '0';
    }
    if (!enabled || notes >= 48 || !decl)
        return;
    notes++;
    fprintf(stderr, "[DECL] raw declaration at 0x%08X\n", decl);
    for (i = 0; i < 128u; i++) {
        uint32_t t = HLE_MEM32(decl + i * 4u);
        uint32_t type = (t >> 29) & 7u;

        if (t == 0xFFFFFFFFu) {
            fprintf(stderr, "[DECL]  [%2u] 0x%08X  END\n", i, t);
            break;
        }
        if (type == 1u && !(t & 0x10000000u)) {
            stream = t & 0x1FFFFFFFu;
            offset = 0;
            fprintf(stderr, "[DECL]  [%2u] 0x%08X  STREAM %u\n", i, t, stream);
        } else if (type == 2u && (t & 0x10000000u)) {
            uint32_t dwords = (t >> 16) & 0xFFFu;
            fprintf(stderr, "[DECL]  [%2u] 0x%08X  SKIP %u dword(s), "
                    "offset %u -> %u\n", i, t, dwords, offset,
                    offset + dwords * 4u);
            offset += dwords * 4u;
        } else if (type == 2u) {
            uint32_t reg = t & 0x1Fu;
            uint32_t fmt = (t >> 16) & 0xFFu;
            uint32_t count = fmt >> 4, kind = fmt & 0xFu, size;

            switch (kind) {
            case 0x0: size = 4; break;                 /* D3DCOLOR */
            case 0x1: case 0x5: size = count * 2u; break; /* NORMSHORT / SHORT */
            case 0x2: size = count * 4u; break;        /* FLOAT */
            case 0x4: size = count; break;             /* PBYTE */
            case 0x6: size = 4; break;                 /* NORMPACKED3 */
            default:  size = 0; break;                 /* NONE and unknown */
            }
            fprintf(stderr, "[DECL]  [%2u] 0x%08X  REG v%u stream %u offset %u "
                    "type 0x%02X size %u\n", i, t, reg, stream, offset, fmt, size);
            offset += size;
        } else {
            fprintf(stderr, "[DECL]  [%2u] 0x%08X  token type %u\n", i, t, type);
        }
    }
}

/* HRESULT D3DDevice_CreateVertexShader(const DWORD *pDeclaration,
 *     const DWORD *pFunction, DWORD *pHandle, DWORD Usage)
 *
 * pFunction is the XDK's compiled form: a 4-byte X_VSH_SHADER_HEADER
 * (Version, NumInst) and then NumInst instructions of 4 DWORDs of NV2A
 * microcode (Cxbx-Reloaded, XbD3D8Types.h). The host takes the microcode.
 * Only ordinary programs (Version 0x2078, 'x ') are replayed; state and
 * read/write programs set NV2A state rather than transform vertices. */
HLE_EXPORT(D3DDevice_CreateVertexShader)
{
    static int seen;
    uint32_t declaration = HLE_ARG(0);
    uint32_t function = HLE_ARG(1);
#ifdef _WIN32
    uint32_t handle_va = HLE_ARG(2);
#endif

    first_call(&seen, "D3DDevice_CreateVertexShader", function);
    note_declaration_tokens(declaration);
    if (original_missing(hle_original_D3DDevice_CreateVertexShader,
                         "D3DDevice_CreateVertexShader"))
        HLE_RETURN(0x80004005u);
    HLE_CALL_ORIGINAL(D3DDevice_CreateVertexShader);
#ifdef _WIN32
    if ((int32_t)g_eax >= 0 && handle_va) {
        uint32_t guest = HLE_MEM32(handle_va);
        uint32_t header = function ? HLE_MEM32(function) : 0;
        const DWORD *code = function ? (const DWORD *)HLE_PTR(function + 4) : NULL;
        shadow_decl_tokens tokens;

        /* Only a program's declaration is ever read from its tokens. */
        if (function)
            decl_from_tokens(declaration, &tokens);
        else
            memset(&tokens, 0, sizeof tokens);
        if (g_shadow)
            shadow_track_vertex_shader(guest, function != 0, header, code, &tokens);
        else
            shadow_keep_early_vertex_shader(guest, function != 0, header, code, &tokens);
    }
#endif
}

#ifdef _WIN32
/* Shaders created before the device.
 *
 * Mortal Kombat: Deadly Alliance calls CreateVertexShader before
 * Direct3D_CreateDevice, so the shadow device did not exist yet and the
 * program was never recorded; every draw under it was then "unknown
 * shader" -- 3,796 of them in a minute, its HUD among them. The guest's
 * function buffer need not outlive the call, so the microcode is copied
 * here and replayed when the device is made. */
#define SHADOW_EARLY_SHADERS 16

static struct {
    uint32_t guest;
    int      has_function;
    uint32_t header;
    DWORD    code[136 * 4];
    shadow_decl_tokens tokens;
} g_early_vs[SHADOW_EARLY_SHADERS];
static int g_early_vs_count;

static void shadow_keep_early_vertex_shader(uint32_t guest, int has_function,
                                            uint32_t header, const DWORD *code,
                                            const shadow_decl_tokens *tokens)
{
    uint32_t n = header >> 16;

    if (g_early_vs_count >= SHADOW_EARLY_SHADERS) {
        fprintf(stderr, "[HLE-D3D8] vertex shader 0x%08X created before the "
                "device, past the %d kept: it will be unknown\n", guest,
                SHADOW_EARLY_SHADERS);
        return;
    }
    g_early_vs[g_early_vs_count].guest = guest;
    g_early_vs[g_early_vs_count].has_function = has_function;
    g_early_vs[g_early_vs_count].header = header;
    g_early_vs[g_early_vs_count].tokens = *tokens;
    if (has_function && n <= 136)
        memcpy(g_early_vs[g_early_vs_count].code, code, n * 4 * sizeof(DWORD));
    g_early_vs_count++;
}

static void shadow_replay_early_vertex_shaders(void)
{
    int i;

    for (i = 0; i < g_early_vs_count; i++) {
        fprintf(stderr, "[HLE-D3D8] vertex shader 0x%08X was created before the "
                "device; recording it now\n", g_early_vs[i].guest);
        shadow_track_vertex_shader(g_early_vs[i].guest, g_early_vs[i].has_function,
                                   g_early_vs[i].header, g_early_vs[i].code,
                                   &g_early_vs[i].tokens);
    }
    g_early_vs_count = 0;
}

/* Record a created vertex shader against the shadow device. */
static void shadow_track_vertex_shader(uint32_t guest, int has_function,
                                       uint32_t header, const DWORD *code,
                                       const shadow_decl_tokens *tokens)
{
    {
        int slot = shadow_program_find(guest);
        DWORD host = 0;
        HRESULT hr = E_FAIL;
        int kind;

        if (slot >= 0) {                 /* handle reused: drop the old program */
            if (g_programs[slot].kind == SHADER_HOST_PROGRAM && !g_programs[slot].from_slot)
                host_vsh_delete_shader(g_programs[slot].host);
            g_programs[slot].from_slot = 0;
        } else if (g_program_count < SHADOW_MAX_PROGRAMS) {
            slot = g_program_count++;
        }

        /* The host device's vtable has no CreateVertexShader slot; its vertex
         * program layer is called directly, with the header's count. */
        if (has_function && (header & 0xFFFF) == 0x2078 &&
            (header >> 16) != 0 && (header >> 16) <= 136)
            hr = host_vsh_create_shader(code, (int)(header >> 16), &host);
        kind = !has_function ? SHADER_DECLARATION
             : SUCCEEDED(hr) ? SHADER_HOST_PROGRAM : SHADER_NOT_REPLAYED;
        if (slot >= 0) {
            g_programs[slot].guest = guest;
            g_programs[slot].host = kind == SHADER_HOST_PROGRAM ? host : 0;
            g_programs[slot].kind = kind;
            g_programs[slot].has_declaration = 0;
            g_programs[slot].packed_count = 0;
            g_programs[slot].use_tokens = 0;
        } else if (kind == SHADER_HOST_PROGRAM) {
            host_vsh_delete_shader(host);
        }
        fprintf(stderr, "[HLE-D3D8] shadow vertex shader 0x%08X: %s%s\n", guest,
                kind == SHADER_DECLARATION ? "declaration only"
                : kind == SHADER_HOST_PROGRAM ? "host program" : "program not replayed",
                slot < 0 ? " (table full, not tracked)" : "");
        note_vertex_attributes(guest);
        if (slot >= 0 && kind == SHADER_HOST_PROGRAM && tokens && tokens->regs > 0) {
            g_programs[slot].tok = *tokens;
            if (!decl_object_agrees(&g_programs[slot], guest & ~1u)) {
                static int said;
                g_programs[slot].use_tokens = 1;
                if (said++ < 4)
                    fprintf(stderr, "[HLE-D3D8] shadow declaration 0x%08X: the shader "
                            "object's parsed copy is not laid out as XDK 4627+ lays it "
                            "out; reading the title's declaration tokens instead\n", guest);
            }
        }
        if (slot >= 0 && kind == SHADER_HOST_PROGRAM)
            shadow_read_declaration(slot, guest);
        /* This entry may be the selected one, recreated under the same handle. */
        if (slot >= 0 && slot == g_shadow_vs_slot)
            g_shadow_vs_kind = kind;
    }
#endif
}

/* void D3DDevice_LoadVertexShaderProgram(const DWORD *pFunction, DWORD Address)
 * Xbox-only: copy a compiled program (the same X_VSH_SHADER_HEADER form
 * CreateVertexShader takes) into transform-program slot Address, with no
 * shader object. Selected later by SelectVertexShader(handle, Address). */
HLE_EXPORT(D3DDevice_LoadVertexShaderProgram)
{
    static int seen;
    uint32_t function = HLE_ARG(0);
#ifdef _WIN32
    uint32_t address = HLE_ARG(1);
#endif

    first_call(&seen, "D3DDevice_LoadVertexShaderProgram", function);
    if (original_missing(hle_original_D3DDevice_LoadVertexShaderProgram,
                         "D3DDevice_LoadVertexShaderProgram"))
        HLE_RETURN(0u);
    HLE_CALL_ORIGINAL(D3DDevice_LoadVertexShaderProgram);
#ifdef _WIN32
    if (g_shadow && function && address < SHADOW_PROGRAM_SLOTS) {
        static int logged;
        uint32_t header = HLE_MEM32(function);
        int count = (int)(header >> 16);
        int valid = (header & 0xFFFF) == 0x2078 && count != 0 &&
                    (uint32_t)count <= SHADOW_PROGRAM_SLOTS - address;
        const DWORD *microcode = (const DWORD *)HLE_PTR(function + 4);
        DWORD host = 0;
        HRESULT hr = E_FAIL;

        if (valid) {
            uint32_t hash = microcode_hash(microcode, count);
            int i = slot_program_find(hash, count, microcode);

            if (i >= 0) {
                host = g_slot_programs[i].host;
                hr = S_OK;
                g_slot_reloads++;
            } else {
                hr = host_vsh_create_shader(microcode, count, &host);
                if (SUCCEEDED(hr) && g_slot_program_count < SLOT_PROGRAM_CACHE) {
                    SlotProgram *sp = &g_slot_programs[g_slot_program_count++];
                    sp->hash = hash;
                    sp->count = count;
                    sp->host = host;
                }
            }
        }
        /* The slot's previous program goes only if nothing else can name it. */
        if (g_slot_loaded[address] && g_slot_host[address] != host &&
            !slot_program_cached(g_slot_host[address]))
            host_vsh_delete_shader(g_slot_host[address]);
        g_slot_loaded[address] = SUCCEEDED(hr);
        g_slot_host[address] = SUCCEEDED(hr) ? host : 0;
        /* The NV2A runs whatever sits at the address the selection named, so
         * a program loaded there after SelectVertexShader replaces the running
         * one at once, under the declaration already selected. TimeSplitters:
         * Future Perfect selects a vertex format at slot 0 and then loads
         * each mesh's program there, selecting again only when the format
         * changes; bound only at selection, those meshes were drawn with the
         * previous mesh's program -- skinned hands and soldiers with a rigid
         * program (the hands floated in front of the guns), rigid meshes with
         * a skinning program reading bone indices their format lacks. */
        if (SUCCEEDED(hr) && address == g_shadow_vs_address && g_shadow_vs_slot >= 0 &&
            g_programs[g_shadow_vs_slot].from_slot &&
            g_programs[g_shadow_vs_slot].host != host) {
            g_programs[g_shadow_vs_slot].host = host;
            g_programs[g_shadow_vs_slot].kind = SHADER_HOST_PROGRAM;
            g_shadow_vs_kind = SHADER_HOST_PROGRAM;
            shadow_read_declaration(g_shadow_vs_slot, g_shadow_vs);
            host_SetVertexShader(g_shadow, host);
            g_slot_rebinds++;
        }
        if (!valid || !slot_program_cached(host) || g_slot_programs[g_slot_program_count - 1].host == host) {
            /* a new program, or one that could not be made: worth a line */
            if (logged < 64 && !(valid && hr == S_OK && g_slot_reloads && slot_program_cached(host) &&
                                 g_slot_programs[g_slot_program_count - 1].host != host))
                fprintf(stderr, "[HLE-D3D8] shadow vertex program at slot %u: %u instructions, %s%s\n",
                        address, header >> 16,
                        SUCCEEDED(hr) ? "host program" : "not replayed",
                        ++logged == 64 ? " (further loads not logged)" : "");
        }
    }
#endif
}

/* HRESULT D3DDevice_SetVertexShader(DWORD Handle)                            */
HLE_EXPORT(D3DDevice_SetVertexShader)
{
    static int seen;
    uint32_t handle = HLE_ARG(0);

    first_call(&seen, "D3DDevice_SetVertexShader", handle);
    if (original_missing(hle_original_D3DDevice_SetVertexShader, "D3DDevice_SetVertexShader"))
        HLE_RETURN(0x80004005u);
    HLE_CALL_ORIGINAL(D3DDevice_SetVertexShader);
#ifdef _WIN32
    if (g_shadow)
        shadow_select_vertex_shader(handle, SHADOW_PROGRAM_SLOTS);
#endif
}

/* HRESULT D3DDevice_SelectVertexShader(DWORD Handle, DWORD Address)
 * Xbox-only: select a shader, running the program previously loaded at slot
 * Address (Cxbx-Reloaded, CxbxImpl_SelectVertexShader). Handle may be 0,
 * which keeps the current declaration and is not tracked here. */
HLE_EXPORT(D3DDevice_SelectVertexShader)
{
    static int seen;
    uint32_t handle = HLE_ARG(0);

    first_call(&seen, "D3DDevice_SelectVertexShader", handle);
    if (original_missing(hle_original_D3DDevice_SelectVertexShader,
                         "D3DDevice_SelectVertexShader"))
        HLE_RETURN(0x80004005u);
    HLE_CALL_ORIGINAL(D3DDevice_SelectVertexShader);
#ifdef _WIN32
    if (g_shadow && handle)
        shadow_select_vertex_shader(handle, HLE_ARG(1));
#endif
}

/* void D3DDevice_SetPixelShader(DWORD Handle)
 * Handle is a shader object carrying a D3DPIXELSHADERDEF; hle_d3d8_state.c
 * reads the combiner setup out of it, because the deferred render states do
 * not follow what this call selects. Handle 0 goes back to fixed function.
 * RECOMP_HLE_D3D8_PS_PROBE=1 dumps the first few objects, which is how the
 * definition's offset was found. */
HLE_EXPORT(D3DDevice_SetPixelShader)
{
    static int seen;
#ifdef _WIN32
    static int dumped, probe = -1;
    uint32_t handle = HLE_ARG(0);
#endif

    first_call(&seen, "D3DDevice_SetPixelShader", HLE_ARG(0));
    if (original_missing(hle_original_D3DDevice_SetPixelShader,
                         "D3DDevice_SetPixelShader"))
        HLE_RETURN(0x80004005u);
    HLE_CALL_ORIGINAL(D3DDevice_SetPixelShader);
#ifdef _WIN32
    if (g_shadow)
        hle_d3d8_pixel_shader_selected(handle);
    /* Read once: this runs on every SetPixelShader, which Outrun 2 makes
     * before most draws, and the switch was being looked up in the
     * environment each time. */
    if (probe < 0)
        probe = getenv("RECOMP_HLE_D3D8_PS_PROBE") ? 1 : 0;
    if (probe && handle >= 0x10000u && handle < 0x08000000u && !(handle & 3u) &&
        dumped < 6) {
        int i;

        dumped++;
        fprintf(stderr, "[HLE-D3D8] SetPixelShader(0x%08X):", handle);
        for (i = 0; i < 64; i++) {
            if ((i % 8) == 0)
                fprintf(stderr, "\n  +0x%02X:", i * 4);
            fprintf(stderr, " %08X", HLE_MEM32(handle + (uint32_t)i * 4));
        }
        fprintf(stderr, "\n");
        fflush(stderr);
    }
#endif
}

#ifdef _WIN32
/* Immediate mode, drawn (D3DDevice_Begin ... SetVertexData* ... End).
 *
 * Each SetVertexData* between Begin and End sets one input register's current
 * value; writing the position register (0, or D3DVSDE_VERTEX, -1) ends a
 * vertex with every register's current value, as the NV2A does. At End the
 * vertices are laid out for the fixed-function shader's FVF and drawn through
 * the same path as DrawVerticesUP, pre-transformed positions included.
 *
 * XGRA draws its movies this way, one quad a frame, and was black while these
 * were only counted. A vertex program is still only counted: its layout comes
 * from a declaration there is no stream for. */
#define INLINE_MAX_VERTICES 4096u
static int      g_inline_on;
static uint32_t g_inline_xpt;
static float    g_inline_cur[16][4] = {
    { 0, 0, 0, 1 }, { 0, 0, 0, 1 }, { 0, 0, 0, 1 }, { 1, 1, 1, 1 }, { 0, 0, 0, 1 },
    { 0, 0, 0, 1 }, { 0, 0, 0, 1 }, { 1, 1, 1, 1 }, { 0, 0, 0, 1 }, { 0, 0, 0, 1 },
    { 0, 0, 0, 1 }, { 0, 0, 0, 1 }, { 0, 0, 0, 1 }, { 0, 0, 0, 1 }, { 0, 0, 0, 1 },
    { 0, 0, 0, 1 } };
static float  (*g_inline_verts)[16][4];
static uint32_t g_inline_nverts;

void hle_d3d8_shadow_draw(uint32_t xpt, uint32_t count, const void *verts,
                          uint32_t stride, int from_buffer);

/* 1 if this call was part of an immediate-mode vertex and has been taken. */
static int inline_vertex_data(uint32_t reg, const float v[4])
{
    if (!g_inline_on)
        return 0;
    if (reg == 0xFFFFFFFFu)
        reg = 0;                         /* D3DVSDE_VERTEX: the position */
    if (reg >= 16u)
        return 1;
    memcpy(g_inline_cur[reg], v, sizeof g_inline_cur[reg]);
    if (reg == 0u) {
        if (!g_inline_verts)
            g_inline_verts = malloc(INLINE_MAX_VERTICES * sizeof *g_inline_verts);
        if (g_inline_verts && g_inline_nverts < INLINE_MAX_VERTICES)
            memcpy(g_inline_verts[g_inline_nverts++], g_inline_cur, sizeof g_inline_cur);
    }
    return 1;
}

static uint32_t inline_color(const float *c)   /* r, g, b, a -> D3DCOLOR */
{
    uint32_t r = (uint32_t)(c[0] * 255.0f + 0.5f), g = (uint32_t)(c[1] * 255.0f + 0.5f);
    uint32_t b = (uint32_t)(c[2] * 255.0f + 0.5f), a = (uint32_t)(c[3] * 255.0f + 0.5f);
    return ((a > 255 ? 255 : a) << 24) | ((r > 255 ? 255 : r) << 16) |
           ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
}

/* The recorded vertices under a vertex program. Immediate mode feeds a
 * program its sixteen input registers directly, with no stream declaration,
 * and the recording already holds exactly that: sixteen float4s a vertex. So
 * the program gets that layout for this one draw, and its own declaration
 * back afterwards for the stream draws that follow. */
static void inline_draw_program(uint32_t n)
{
    struct shadow_program *p, saved;
    D3D8VshInput in[16];
    int i;
    static int notes;

    if (notes++ < 2) {
        uint32_t k;
        fprintf(stderr, "[HLE-D3D8] inline draw under program 0x%08X: %u vertices, "
                "primitive %u\n", g_shadow_vs, n, g_inline_xpt);
        for (k = 0; k < n && k < 4; k++) {
            float (*v)[4] = g_inline_verts[k];
            fprintf(stderr, "[HLE-D3D8]   vertex %u: v0 (%.1f %.1f %.1f %.1f) v3 (%.2f %.2f "
                    "%.2f %.2f) v9 (%.3f %.3f %.3f %.3f)\n", k,
                    v[0][0], v[0][1], v[0][2], v[0][3], v[3][0], v[3][1], v[3][2], v[3][3],
                    v[9][0], v[9][1], v[9][2], v[9][3]);
        }
    }

    if (g_shadow_vs_kind != SHADER_HOST_PROGRAM || g_shadow_vs_slot < 0) {
        g_inline_program++;
        return;
    }
    p = &g_programs[g_shadow_vs_slot];
    for (i = 0; i < 16; i++) {
        in[i].reg = i;
        in[i].format = RHI_FORMAT_R32G32B32A32_FLOAT;
        in[i].offset = (UINT)i * 16u;
    }
    if (FAILED(host_vsh_set_declaration(p->host, in, 16))) {
        g_inline_program++;
        return;
    }
    saved = *p;
    p->has_declaration = 1;
    p->extent = sizeof g_inline_verts[0];
    p->packed_count = 0;
    p->other_streams = 0;
    p->expanded_bytes = 0;
    hle_d3d8_shadow_draw(g_inline_xpt, n, g_inline_verts, sizeof g_inline_verts[0], 0);
    *p = saved;
    if (saved.has_declaration)
        shadow_read_declaration(g_shadow_vs_slot, g_shadow_vs);
    g_inline_drawn++;
}

/* The recorded vertices in the current FVF's layout, drawn. */
static void inline_draw(void)
{
    uint32_t fvf = g_shadow_vs, n = g_inline_nverts, pos, weights, ntex, i, t;
    uint32_t stride, tsize[8];
    uint8_t *buf;

    if (!g_shadow || !n)
        return;
    if (g_shadow_vs_is_program) {
        inline_draw_program(n);
        return;
    }
    pos = fvf & 0x00Eu;
    weights = pos >= 0x006u ? (pos - 0x004u) / 2u : 0u;   /* XYZB1..XYZB5 */
    stride = pos == 0x004u ? 16u : 12u + weights * 4u;
    if (fvf & 0x010u) stride += 12u;                       /* normal */
    if (fvf & 0x020u) stride += 4u;                        /* point size */
    if (fvf & 0x040u) stride += 4u;                        /* diffuse */
    if (fvf & 0x080u) stride += 4u;                        /* specular */
    ntex = (fvf >> 8) & 0xFu;
    if (ntex > 4u)
        ntex = 4u;
    for (t = 0; t < ntex; t++) {
        static const uint32_t sizes[4] = { 2, 3, 4, 1 };
        tsize[t] = sizes[(fvf >> (16u + 2u * t)) & 3u];
        stride += tsize[t] * 4u;
    }
    buf = malloc((size_t)n * stride);
    if (!buf)
        return;
    for (i = 0; i < n; i++) {
        float (*v)[4] = g_inline_verts[i];
        uint8_t *o = buf + (size_t)i * stride;
        uint32_t c;

        memcpy(o, v[0], pos == 0x004u ? 16u : 12u);
        o += pos == 0x004u ? 16u : 12u;
        if (weights) {
            memcpy(o, v[1], weights * 4u);
            o += weights * 4u;
        }
        if (fvf & 0x010u) { memcpy(o, v[2], 12u); o += 12u; }
        if (fvf & 0x020u) { memcpy(o, &v[6][0], 4u); o += 4u; }
        if (fvf & 0x040u) { c = inline_color(v[3]); memcpy(o, &c, 4u); o += 4u; }
        if (fvf & 0x080u) { c = inline_color(v[4]); memcpy(o, &c, 4u); o += 4u; }
        for (t = 0; t < ntex; t++) {
            memcpy(o, v[9 + t], tsize[t] * 4u);
            o += tsize[t] * 4u;
        }
    }
    hle_d3d8_shadow_draw(g_inline_xpt, n, buf, stride, 0);
    free(buf);
    g_inline_drawn++;
}
#endif

/* void D3DDevice_SetVertexDataColor(INT Register, D3DCOLOR Color)
 * The current value of an input register: what a vertex program reads from a
 * register the vertex does not carry. Burnout 2 sets v3, the diffuse colour,
 * to each object's material colour before drawing it. The body writes
 * NV097_SET_VERTEX_DATA4UB, so it still runs. */
HLE_EXPORT(D3DDevice_SetVertexDataColor)
{
    static int seen;
#ifdef _WIN32
    uint32_t reg = HLE_ARG(0), color = HLE_ARG(1);
#endif

    first_call(&seen, "D3DDevice_SetVertexDataColor", HLE_ARG(0));
    if (original_missing(hle_original_D3DDevice_SetVertexDataColor,
                         "D3DDevice_SetVertexDataColor"))
        return;
    HLE_CALL_ORIGINAL(D3DDevice_SetVertexDataColor);
#ifdef _WIN32
    if (g_shadow) {
        float v[4];

        v[0] = (float)((color >> 16) & 0xFF) / 255.0f;
        v[1] = (float)((color >>  8) & 0xFF) / 255.0f;
        v[2] = (float)( color        & 0xFF) / 255.0f;
        v[3] = (float)((color >> 24) & 0xFF) / 255.0f;
        if (!inline_vertex_data(reg, v))
            host_vsh_set_vertex_data((int)reg, v);
    }
#endif
}

/* void D3DDevice_SetVertexData2f(INT Register, float a, float b)
 * NV097_SET_VERTEX_DATA2F_M: the register becomes (a, b, 0, 1). */
HLE_EXPORT(D3DDevice_SetVertexData2f)
{
    static int seen;
#ifdef _WIN32
    uint32_t reg = HLE_ARG(0), a = HLE_ARG(1), b = HLE_ARG(2);
#endif

    first_call(&seen, "D3DDevice_SetVertexData2f", HLE_ARG(0));
    if (original_missing(hle_original_D3DDevice_SetVertexData2f,
                         "D3DDevice_SetVertexData2f"))
        return;
    HLE_CALL_ORIGINAL(D3DDevice_SetVertexData2f);
#ifdef _WIN32
    if (g_shadow) {
        float v[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

        memcpy(&v[0], &a, 4);
        memcpy(&v[1], &b, 4);
        if (!inline_vertex_data(reg, v))
            host_vsh_set_vertex_data((int)reg, v);
    }
#endif
}

/* The inline immediate-mode vertex path: Begin, then one SetVertexData* per
 * attribute per vertex, then End.
 *
 * The bodies run, so the title's own D3D8 writes its NV097_SET_BEGIN_END and
 * vertex data into the push buffer exactly as before; the vertices are also
 * recorded and drawn on the host at End (see "Immediate mode, drawn" above). The question these counters answer is how much of a
 * title's scene goes this way, which decides whether implementing the path is
 * worth it. Marvel vs Capcom 2 calls Begin from six sites and
 * SetVertexData4f from sixteen, but a static call site says nothing about how
 * often it runs.
 *
 * Reported per frame at the shadow summary, alongside the draws that do
 * arrive, so the two can be compared directly. */
static unsigned long g_inline_begin, g_inline_end, g_inline_vdata;
static unsigned long g_inline_begin_frame, g_inline_vdata_frame;
static unsigned long g_inline_begin_max, g_inline_vdata_max;

HLE_ORIGINAL(D3DDevice_Begin);
HLE_ORIGINAL(D3DDevice_End);
HLE_ORIGINAL(D3DDevice_SetVertexData4f);

/* void D3DDevice_Begin(X_D3DPRIMITIVETYPE PrimitiveType) */
HLE_EXPORT(D3DDevice_Begin)
{
    static int seen;

    first_call(&seen, "D3DDevice_Begin", HLE_ARG(0));
    if (original_missing(hle_original_D3DDevice_Begin, "D3DDevice_Begin"))
        return;
    g_inline_begin++;
    g_inline_begin_frame++;
    HLE_CALL_ORIGINAL(D3DDevice_Begin);
#ifdef _WIN32
    g_inline_on = 1;
    g_inline_xpt = HLE_ARG(0);
    g_inline_nverts = 0;
#endif
}

/* void D3DDevice_End(void) */
HLE_EXPORT(D3DDevice_End)
{
    static int seen;

    first_call(&seen, "D3DDevice_End", 0);
    if (original_missing(hle_original_D3DDevice_End, "D3DDevice_End"))
        return;
    g_inline_end++;
    HLE_CALL_ORIGINAL(D3DDevice_End);
#ifdef _WIN32
    if (g_inline_on) {
        inline_draw();
        g_inline_on = 0;
        g_inline_nverts = 0;
    }
#endif
}

/* ------------------------------------------------------------------------
 * Draws a title writes into the push buffer itself.
 *
 * D3DDevice_BeginPush hands the title a pointer into the push buffer and
 * EndPush takes back where it stopped; between the two the title writes NV2A
 * methods with no D3D call at all. Nothing executes the push buffer here, so
 * whatever is drawn that way never reached the host. OutRun 2 does it at
 * seven sites (0x00084CDF, 0x00086F3B, 0x0009DB08, 0x000D2361, 0x000D43E1,
 * 0x000D4A2F, 0x000D4D9D), each the same shape: SET_BEGIN_END (0x17FC) with a
 * primitive, INLINE_ARRAY (0x1818, non-incrementing) carrying the vertices,
 * SET_BEGIN_END 0. Its 512x512 shadow map was cleared every frame and never
 * drawn into.
 *
 * So EndPush walks what was written since BeginPush and replays each
 * begin/inline/end group as the draw DrawVerticesUP would have made: the
 * NV2A primitive codes are the Xbox D3DPRIMITIVETYPE values, and the vertices
 * are laid out as the current vertex shader's stream 0. The vertex array
 * methods (SET_VERTEX_DATA_ARRAY_OFFSET 0x1720 and _FORMAT 0x1760) are
 * walked past: they draw nothing themselves, and hle_d3d8_vertex.c records
 * them for the DrawVertices and DrawIndexedVertices calls that follow the
 * push (TimeSplitters: Future Perfect points every attribute at an array of
 * its own that way). Any other method stops the walk for that push and is
 * counted, so a title that writes state this way is reported rather than
 * half-drawn. RECOMP_HLE_D3D8_PUSH_DRAWS=0 leaves the draws alone; the
 * arrays are recorded either way.
 * ------------------------------------------------------------------------ */
#define NV2A_PUSH_BEGIN_END    0x17FCu
#define NV2A_PUSH_INLINE_ARRAY 0x1818u
/* SET_VERTEX_DATA_ARRAY_OFFSET (0x1720) and _FORMAT (0x1760), 16 of each. */
#define NV2A_PUSH_ARRAYS_FIRST 0x1720u
#define NV2A_PUSH_ARRAYS_END   0x17A0u

/* hle_d3d8_vertex.c: the vertex arrays the title pushes itself. */
void hle_d3d8_push_arrays_scan(uint32_t start, uint32_t end);


HLE_ORIGINAL(D3DDevice_EndPush);

static int push_draws_on(void)
{
    static int on = -1;
    if (on < 0)
        on = xbox_EnvSwitch("RECOMP_HLE_D3D8_PUSH_DRAWS", 1);
    return on;
}


#ifdef _WIN32
/* Bytes of one vertex under the current vertex shader's stream 0. */
static uint32_t push_vertex_stride(void)
{
    if (g_shadow_vs_is_program) {
        if (g_shadow_vs_kind == SHADER_HOST_PROGRAM && g_shadow_vs_slot >= 0 &&
            g_programs[g_shadow_vs_slot].has_declaration)
            return (g_programs[g_shadow_vs_slot].extent + 3u) & ~3u;
        return 0;
    }
    return fvf_stride(g_shadow_vs);
}

static void push_replay(uint32_t start, uint32_t end)
{
    static uint32_t *data;
    static uint32_t cap;
    uint32_t va = start, prim = 0, n = 0;

    if (!start || end <= start || end - start > 4u * 1024u * 1024u)
        return;
    while (va + 4u <= end) {
        uint32_t h = HLE_MEM32(va), method, count, k;

        va += 4u;
        if (h & 0xA0000003u) {           /* a jump, call or return: not ours */
            g_push_other++;
            return;
        }
        method = h & 0x1FFCu;
        count = (h >> 18) & 0x7FFu;
        if (va + 4u * count > end) {
            g_push_other++;
            return;
        }
        if (method == NV2A_PUSH_BEGIN_END && count == 1) {
            uint32_t v = HLE_MEM32(va);

            if (v) {
                prim = v;
                n = 0;
            } else if (prim && n) {
                uint32_t stride = push_vertex_stride();

                if (!stride || (n * 4u) % stride) {
                    g_push_nostride++;
                } else {
                    hle_d3d8_shadow_draw(prim, n * 4u / stride, data, stride, 0);
                    g_push_draws++;
                }
                prim = 0;
                n = 0;
            }
        } else if (method == NV2A_PUSH_INLINE_ARRAY && prim) {
            if (n + count > cap) {
                uint32_t want = (n + count) * 2u;
                uint32_t *grown = (uint32_t *)realloc(data, want * 4u);

                if (!grown)
                    return;
                data = grown;
                cap = want;
            }
            for (k = 0; k < count; k++)
                data[n + k] = HLE_MEM32(va + 4u * k);
            n += count;
        } else if (method >= NV2A_PUSH_ARRAYS_FIRST && method < NV2A_PUSH_ARRAYS_END) {
            /* The title's vertex arrays: hle_d3d8_push_arrays_scan has them. */
        } else {
            static uint32_t said[8];
            int s;

            for (s = 0; s < 8 && said[s] && said[s] != method; s++)
                ;
            if (s < 8 && !said[s]) {
                said[s] = method;
                fprintf(stderr, "[HLE-D3D8] push buffer: method 0x%04X (count %u) "
                        "written by the title is not replayed; its push is skipped "
                        "from there\n", method, count);
            }
            g_push_other++;
            return;
        }
        va += 4u * count;
    }
}
#endif

/* void D3DDevice_EndPush(DWORD *pPush) -- stdcall.
 *
 * Where the push began is the device's own push pointer: BeginPush returns
 * it without moving it (its tail, the XDK's MakeSpace, only makes room), and
 * EndPush's whole body is storing the end there. So it is read before the
 * original runs, and BeginPush -- which the lifter cannot wrap, it ends in a
 * tail jump -- needs no hook. */
HLE_EXPORT(D3DDevice_EndPush)
{
    static int seen;
    uint32_t end = HLE_ARG(0), start = 0, device;

    first_call(&seen, "D3DDevice_EndPush", end);
    if (original_missing(hle_original_D3DDevice_EndPush, "D3DDevice_EndPush"))
        return;
#ifdef _WIN32
    g_push_count++;
    device = hle_var_D3D_g_pDevice ? HLE_MEM32(hle_var_D3D_g_pDevice) : 0;
    if (device)
        start = HLE_MEM32(device);       /* m_pPush, still where the push began */
    if (g_shadow)
        hle_d3d8_push_arrays_scan(start, end);
    if (push_draws_on() && g_shadow)
        push_replay(start, end);
    if (g_push_count == 1 || (g_push_count % 100000) == 0)
        fprintf(stderr, "[HLE-D3D8] push buffer: %lu push(es), %lu draw(s) replayed, "
                "%lu stopped at another method, %lu with no vertex size\n",
                g_push_count, g_push_draws, g_push_other, g_push_nostride);
#else
    (void)start; (void)device;
#endif
    HLE_CALL_ORIGINAL(D3DDevice_EndPush);
}

/* void D3DDevice_SetVertexData4f(INT Register, float a, float b, float c,
 *     float d) -- one attribute of one inline vertex. */
HLE_EXPORT(D3DDevice_SetVertexData4f)
{
    static int seen;

    first_call(&seen, "D3DDevice_SetVertexData4f", HLE_ARG(0));
    if (original_missing(hle_original_D3DDevice_SetVertexData4f,
                         "D3DDevice_SetVertexData4f"))
        return;
    g_inline_vdata++;
    g_inline_vdata_frame++;
    HLE_CALL_ORIGINAL(D3DDevice_SetVertexData4f);
#ifdef _WIN32
    if (g_shadow) {
        uint32_t w[4] = { HLE_ARG(1), HLE_ARG(2), HLE_ARG(3), HLE_ARG(4) };
        float v[4];

        memcpy(v, w, sizeof v);
        if (!inline_vertex_data(HLE_ARG(0), v))
            host_vsh_set_vertex_data((int)HLE_ARG(0), v);
    }
#endif
}

/* HRESULT D3DDevice_SetTransform(D3DTRANSFORMSTATETYPE State,
 *     const D3DMATRIX *pMatrix)
 * The Xbox packs the states together -- VIEW 0, PROJECTION 1, TEXTURE0-3
 * 2-5, WORLD-WORLD3 6-9 (Cxbx-Reloaded, XbD3D8Types.h) -- where the host has
 * the PC's 2, 3, 16-19, 256-259. D3DMATRIX is 16 floats on both. */
HLE_EXPORT(D3DDevice_SetTransform)
{
    static int seen;
    uint32_t state = HLE_ARG(0);
#ifdef _WIN32
    uint32_t matrix = HLE_ARG(1);
#endif

    first_call(&seen, "D3DDevice_SetTransform", state);
    if (original_missing(hle_original_D3DDevice_SetTransform, "D3DDevice_SetTransform"))
        HLE_RETURN(0x80004005u);
    HLE_CALL_ORIGINAL(D3DDevice_SetTransform);
#ifdef _WIN32
    if (g_shadow && matrix && state < 10) {
        D3DMATRIX m;
        DWORD host_state;

        memcpy(&m, HLE_PTR(matrix), sizeof m);
        if (xbox_transform_state_to_host(state, &host_state))
            host_SetTransform(g_shadow, (D3DTRANSFORMSTATETYPE)host_state, &m);
    }
#endif
}

/* Fixed-function lighting: the material, the lights and which are on.
 *
 * Unforwarded, the host had no lights, so D3DRS_LIGHTING was held off
 * (hle_d3d8_state.c) and lit geometry came out in its unlit colour. Dino
 * Crisis 3's map draws its rooms as position-and-normal meshes with no
 * texture and no vertex colour, lit by the material: every room was a flat
 * white silhouette. D3DMATERIAL8 (17 floats) and D3DLIGHT8 (a type and 25
 * floats) are laid out the same on the Xbox and the PC, so they are copied
 * as they are. XDK 5558: all three stdcall, SetMaterial ret 4, SetLight and
 * LightEnable ret 8. */

/* HRESULT D3DDevice_SetMaterial(const D3DMATERIAL8 *pMaterial) */
HLE_EXPORT(D3DDevice_SetMaterial)
{
    static int seen;
    uint32_t material = HLE_ARG(0);

    first_call(&seen, "D3DDevice_SetMaterial", material);
    if (original_missing(hle_original_D3DDevice_SetMaterial, "D3DDevice_SetMaterial"))
        HLE_RETURN(0x80004005u);
    HLE_CALL_ORIGINAL(D3DDevice_SetMaterial);
#ifdef _WIN32
    if (g_shadow && material) {
        D3DMATERIAL8 m;
        memcpy(&m, HLE_PTR(material), sizeof m);
        host_SetMaterial(g_shadow, &m);
    }
#endif
}

/* HRESULT D3DDevice_SetLight(DWORD Index, const D3DLIGHT8 *pLight) */
HLE_EXPORT(D3DDevice_SetLight)
{
    static int seen;
    uint32_t index = HLE_ARG(0);
#ifdef _WIN32
    uint32_t light = HLE_ARG(1);
#endif

    first_call(&seen, "D3DDevice_SetLight", index);
    if (original_missing(hle_original_D3DDevice_SetLight, "D3DDevice_SetLight"))
        HLE_RETURN(0x80004005u);
    HLE_CALL_ORIGINAL(D3DDevice_SetLight);
#ifdef _WIN32
    if (g_shadow && light) {
        D3DLIGHT8 l;
        memcpy(&l, HLE_PTR(light), sizeof l);
        host_SetLight(g_shadow, index, &l);
    }
#endif
}

/* HRESULT D3DDevice_LightEnable(DWORD Index, BOOL bEnable) */
HLE_EXPORT(D3DDevice_LightEnable)
{
    static int seen;
    uint32_t index = HLE_ARG(0);
#ifdef _WIN32
    uint32_t enable = HLE_ARG(1);
#endif

    first_call(&seen, "D3DDevice_LightEnable", index);
    if (original_missing(hle_original_D3DDevice_LightEnable, "D3DDevice_LightEnable"))
        HLE_RETURN(0x80004005u);
    HLE_CALL_ORIGINAL(D3DDevice_LightEnable);
#ifdef _WIN32
    if (g_shadow)
        host_LightEnable(g_shadow, index, enable ? TRUE : FALSE);
#endif
}

/* HRESULT D3DDevice_SetViewport(const D3DVIEWPORT8 *pViewport)
 * Four DWORDs and two floats on both, so it is copied whole. */
HLE_EXPORT(D3DDevice_SetViewport)
{
    static int seen;
    uint32_t viewport = HLE_ARG(0);

    first_call(&seen, "D3DDevice_SetViewport", viewport);
    if (original_missing(hle_original_D3DDevice_SetViewport, "D3DDevice_SetViewport"))
        HLE_RETURN(0x80004005u);
    HLE_CALL_ORIGINAL(D3DDevice_SetViewport);
#ifdef _WIN32
    if (g_shadow && viewport) {
        D3DVIEWPORT8 vp;

        memcpy(&vp, HLE_PTR(viewport), sizeof vp);
        /* XDK code passes Width and Height of INT_MAX to mean "the whole
         * render target" (Cxbx-Reloaded, CxbxImpl_SetViewport), and D3D11
         * would take that literally, so the viewport is kept inside the
         * current render target. */
        if (vp.X > g_target_width)
            vp.X = g_target_width;
        if (vp.Y > g_target_height)
            vp.Y = g_target_height;
        if (vp.Width > g_target_width - vp.X)
            vp.Width = g_target_width - vp.X;
        if (vp.Height > g_target_height - vp.Y)
            vp.Height = g_target_height - vp.Y;
        g_title_viewport = vp;
        g_title_viewport_set = 1;
        g_host_viewport_mode = -1;       /* the next draw picks which to use */
        shadow_viewport_constants(&vp);
    }
#endif
}

/* void D3DDevice_SetScissors(DWORD Count, BOOL Exclusive, const D3DRECT *pRects)
 * Xbox-only: clip drawing to (or, with Exclusive, outside) up to eight
 * rectangles of the render target; Count 0 turns it off. TimeSplitters 2
 * scrolls its mission briefing inside one, and without this the text ran
 * over the heading and the button bar. */
HLE_EXPORT(D3DDevice_SetScissors)
{
    static int seen;
    uint32_t count = HLE_ARG(0);
    uint32_t exclusive = HLE_ARG(1);
    uint32_t rects = HLE_ARG(2);

    first_call(&seen, "D3DDevice_SetScissors", count);
    if (original_missing(hle_original_D3DDevice_SetScissors, "D3DDevice_SetScissors"))
        HLE_RETURN(0u);
    HLE_CALL_ORIGINAL(D3DDevice_SetScissors);
#ifdef _WIN32
    if (g_shadow) {
        D3DRECT rect[8];
        UINT n = count > 8 ? 8 : count;

        if (n && rects)
            memcpy(rect, HLE_PTR(rects), n * sizeof rect[0]);
        else
            n = 0;
        host_SetScissors(n, exclusive != 0, rect);
    }
#endif
}

#ifdef _WIN32
/* ------------------------------------------------------------ render targets
 *
 * Titles draw into textures -- Burnout 2 renders its world several times a
 * frame into offscreen targets (and cube map faces) before it composes the
 * screen -- and without this every one of those passes, and the clear before
 * each, landed on the shadow back buffer and wiped what came before.
 *
 * An Xbox surface is a pixel container with a Parent at +20: the texture it
 * is a level of, or NULL for a surface of its own (Cxbx-Reloaded,
 * XbD3D8Types.h, X_D3DSurface). The target becomes:
 *   - the host back buffer, for the surface the XDK itself set during
 *     CreateDevice (or, if it set none, a parentless surface of the back
 *     buffer's size);
 *   - level 0 of a host render target texture, for a 2D texture's surface --
 *     the same one SetTexture binds (hle_d3d8_render_texture), so what was
 *     drawn is what the title then samples;
 *   - a face of a host cube texture, for a surface whose parent is a cube
 *     container -- a title's environment map, which it renders each frame and
 *     then samples;
 *   - a scratch target of the surface's size for anything else (other levels,
 *     parentless surfaces of another size), so those passes at least stop
 *     drawing over the screen. Their contents are not used yet.
 * Depth: none when the title passes none. The CreateDevice depth surface with
 * the back buffer is the host device's own; any other request gets a host
 * depth surface of the target's size, since D3D11 accepts depth only at the
 * target's exact size. As on the
 * Xbox (Cxbx-Reloaded, CxbxImpl_SetRenderTarget), a new target resets the
 * viewport to all of it. */
IDirect3DTexture8 *hle_d3d8_render_texture(IDirect3DDevice8 *dev, uint32_t va);
IDirect3DCubeTexture8 *hle_d3d8_render_cube(IDirect3DDevice8 *dev, uint32_t va);
int hle_d3d8_cube_face(uint32_t parent_va, uint32_t surface_va,
                       uint32_t *face, uint32_t *level);

#define SHADOW_SCRATCH 8
#define SURFACE_PARENT 20

static struct { UINT width, height; IDirect3DTexture8 *texture; } g_scratch[SHADOW_SCRATCH];
static struct { UINT width, height; IDirect3DSurface8 *surface; } g_depths[SHADOW_SCRATCH];

static void surface_measure(uint32_t va, UINT *w, UINT *h, uint32_t *fmt)
{
    uint32_t format = HLE_MEM32(va + 12), size = HLE_MEM32(va + 16);

    *fmt = (format >> 8) & 0xFF;
    if (size) {                          /* linear: width-1 and height-1 */
        *w = (size & 0xFFF) + 1;
        *h = ((size >> 12) & 0xFFF) + 1;
    } else {                             /* log2 dimensions */
        *w = 1u << ((format >> 20) & 0xF);
        *h = 1u << ((format >> 24) & 0xF);
    }
}

static int rt_parentless_is_backbuffer(void)
{
    static int on = -1;
    if (on < 0)
        on = xbox_EnvSwitch("RECOMP_RT_PARENTLESS_BACKBUFFER", 0);
    return on;
}

static IDirect3DTexture8 *scratch_target(UINT w, UINT h)
{
    int i;

    for (i = 0; i < SHADOW_SCRATCH; i++) {
        if (g_scratch[i].texture && g_scratch[i].width == w && g_scratch[i].height == h)
            return g_scratch[i].texture;
    }
    for (i = 0; i < SHADOW_SCRATCH; i++) {
        if (g_scratch[i].texture)
            continue;
        if (FAILED(host_CreateTexture(g_shadow, w, h, 1, D3DUSAGE_RENDERTARGET,
                                      D3DFMT_LIN_X8R8G8B8, D3DPOOL_DEFAULT,
                                      &g_scratch[i].texture)))
            return g_scratch[i].texture = NULL;
        g_scratch[i].width = w;
        g_scratch[i].height = h;
        return g_scratch[i].texture;
    }
    return NULL;
}

static IDirect3DSurface8 *depth_surface(UINT w, UINT h)
{
    int i;

    for (i = 0; i < SHADOW_SCRATCH; i++) {
        if (g_depths[i].surface && g_depths[i].width == w && g_depths[i].height == h)
            return g_depths[i].surface;
    }
    for (i = 0; i < SHADOW_SCRATCH; i++) {
        if (g_depths[i].surface)
            continue;
        if (FAILED(host_CreateDepthStencilSurface(g_shadow, w, h, D3DFMT_D24S8,
                                                  &g_depths[i].surface)))
            return g_depths[i].surface = NULL;
        g_depths[i].width = w;
        g_depths[i].height = h;
        return g_depths[i].surface;
    }
    return NULL;
}

static int shadow_maps_on(void)
{
    static int on = -1;
    if (on < 0)
        on = xbox_EnvSwitch("RECOMP_HLE_D3D8_SHADOW_MAPS", 1);
    return on;
}

/* Depth formats a title can render into and then sample: a shadow map. */
static int is_depth_format(uint32_t fmt)
{
    return fmt == 0x2A || fmt == 0x2E ||     /* D24S8, LIN_D24S8 */
           fmt == 0x2C || fmt == 0x30;       /* D16, LIN_D16 */
}

/* The guest texture a surface belongs to, if it is a 2D depth-format
 * texture whose level 0 the surface is; 0 otherwise. */
static uint32_t depth_texture_of(uint32_t surface)
{
    uint32_t parent = surface ? HLE_MEM32(surface + SURFACE_PARENT) : 0;
    uint32_t format;

    if (!parent || HLE_MEM32(parent + 4) != HLE_MEM32(surface + 4))
        return 0;
    format = HLE_MEM32(parent + 12);
    if ((format & 0x4) || ((format >> 4) & 0xF) != 2)
        return 0;
    return is_depth_format((format >> 8) & 0xFF) ? parent : 0;
}

/* The host depth surface for a guest depth texture: level 0 of the same
 * host texture SetTexture binds when the title samples it
 * (hle_d3d8_render_texture), so what a shadow pass writes is what the next
 * pass reads. NULL if the host cannot hold it. */
static IDirect3DSurface8 *depth_texture_surface(uint32_t texture_va)
{
    static struct { IDirect3DTexture8 *texture; IDirect3DSurface8 *surface; } cache[8];
    IDirect3DTexture8 *t = hle_d3d8_render_texture(g_shadow, texture_va);
    int i;

    if (!t)
        return NULL;
    for (i = 0; i < 8; i++)
        if (cache[i].texture == t)
            return cache[i].surface;
    for (i = 0; i < 8 && cache[i].texture; i++)
        ;
    if (i == 8)
        i = 0;                           /* reuse the oldest; keeps its ref */
    if (cache[i].surface)
        cache[i].surface->lpVtbl->Release(cache[i].surface);
    cache[i].texture = t;
    cache[i].surface = NULL;
    if (FAILED(t->lpVtbl->GetSurfaceLevel(t, 0, &cache[i].surface)))
        cache[i].surface = NULL;
    else
        fprintf(stderr, "[HLE-D3D8] shadow map 0x%08X: depth renders into the "
                "texture the title samples\n", texture_va);
    return cache[i].surface;
}

static void shadow_set_render_target(uint32_t rt, uint32_t zs)
{
    static struct { uint32_t va; int kind; } seen[32];
    static int nseen;
    static uint32_t current_rt;
    IDirect3DBaseTexture8 *target = NULL;
    IDirect3DTexture8 *texture = NULL;
    IDirect3DSurface8 *depth = NULL;
    UINT w, h, zw = 0, zh = 0, face = 0, level = 0;
    uint32_t fmt, zfmt = 0, parent;
    int kind, i;                         /* 0 back buffer, 1 texture, 2 scratch, 3 cube */

    /* A NULL target keeps the current one; only the depth surface changes. */
    if (!rt)
        rt = current_rt;
    current_rt = rt;
    g_target_sets++;
    if (!rt) {
        kind = 0;
        w = g_shadow_width;
        h = g_shadow_height;
    } else {
        surface_measure(rt, &w, &h, &fmt);
        parent = HLE_MEM32(rt + SURFACE_PARENT);
        if (parent && (HLE_MEM32(parent + 12) & 0x4)) {  /* a cube face */
            uint32_t f = 0, lv = 0;
            IDirect3DCubeTexture8 *cube =
                hle_d3d8_cube_face(parent, rt, &f, &lv) == 0
                    ? hle_d3d8_render_cube(g_shadow, parent) : NULL;

            if (cube) {
                target = (IDirect3DBaseTexture8 *)cube;
                face = (UINT)f;
                level = (UINT)lv;
                kind = 3;
            } else {
                kind = 2;
            }
        } else if (is_swap_data(HLE_MEM32(rt + 4))) {
            /* The surface's memory is a frame buffer, so this is the screen
             * even when the surface hangs off a texture the title made over
             * that memory (see g_swap_data). Measured on Future Perfect,
             * frame 900 of a capture: all 110 draws of its front end went to
             * a surface of texture 0x00563154, whose data is the frame
             * buffer 0x00204000, and none to either swap surface. */
            kind = 0;
        } else if (depth_texture_of(rt)) {
            /* A colour surface over a depth texture's memory. Xbox titles
             * pair a shadow pass's depth target with a colour target that
             * aliases it, colour writes off, to spend no memory on colour
             * (OutRun 2's 512x512 LIN_D24S8 map). The host cannot render
             * colour into a depth texture, and nothing is meant to land
             * there: a scratch target of the same size, and the depth goes
             * to the texture below. */
            kind = 2;
        } else if (parent && HLE_MEM32(parent + 4) == HLE_MEM32(rt + 4)) {
            texture = hle_d3d8_render_texture(g_shadow, parent);   /* level 0 */
            target = (IDirect3DBaseTexture8 *)texture;
            kind = texture ? 1 : 2;
        } else if (g_backbuffer_va == rt ||
                   (!parent && w == g_shadow_width && h == g_shadow_height
                    && rt_parentless_is_backbuffer())) {
            /* A parentless surface the size of the screen is a back buffer.
             *
             * This used to accept only the one VA CreateDevice named, once it
             * had named one, and send every other parentless screen-sized
             * surface to a scratch target whose contents are thrown away.
             * A double-buffered title has two of them and alternates, so half
             * its frames were being drawn into nothing: TimeSplitters: Future
             * Perfect sets 13,767 render targets in two minutes and 6,883 of
             * them -- almost exactly half -- went to scratch, its two
             * surfaces sitting 0x18 apart at 0x003E5984 and 0x003E599C.
             *
             * Measured on Future Perfect: it takes scratch targets from
             * 6,883 to 0 and the screen from black-with-a-loading-icon to
             * pure white, because 0x003E599C is an offscreen surface the
             * title clears, not a second back buffer. So this is OFF by
             * default and kept only as a switch for the next title whose
             * two screen-sized surfaces really are a swap pair.
             *
             * RECOMP_RT_PARENTLESS_BACKBUFFER=1 enables it. The
             * scratch path exists because an offscreen pass that lands on the
             * screen is worse than one that vanishes -- that is what kept
             * Burnout 2 black -- so this is a switch until the library has
             * been measured with it. */
            kind = 0;
        } else {
            kind = 2;
        }
        if (kind == 2) {
            texture = scratch_target(w, h);
            target = (IDirect3DBaseTexture8 *)texture;
            g_target_scratch++;
        }
        if (shadow_trace_on())
            fprintf(stderr, "[TRACE swap %lu] SetRenderTarget 0x%08X data 0x%08X parent 0x%08X "
                    "%ux%u zs 0x%08X -> %s\n", g_shadow_swaps, rt, HLE_MEM32(rt + 4),
                    parent, w, h, zs,
                    kind == 0 ? "back buffer" : kind == 1 ? "render target texture"
                                  : kind == 3 ? "cube face" : "scratch target");
        for (i = 0; i < nseen && (seen[i].va != rt || seen[i].kind != kind); i++)
            ;
        if (i == nseen && nseen < (int)(sizeof seen / sizeof seen[0])) {
            seen[nseen].va = rt;
            seen[nseen++].kind = kind;
            /* The data pointers matter as much as the parent: a title can
             * wrap its frame buffer in a texture of its own (XGSetTextureHeader
             * over the same memory), and then a surface whose parent is that
             * texture *is* the screen, however it is named. */
            fprintf(stderr, "[HLE-D3D8] shadow render target 0x%08X: %ux%u format 0x%02X, "
                    "data 0x%08X, parent 0x%08X (format 0x%08X, data 0x%08X) -> %s"
                    " [back buffer 0x%08X data 0x%08X]\n", rt, w, h, fmt,
                    HLE_MEM32(rt + 4), parent,
                    parent ? HLE_MEM32(parent + 12) : 0,
                    parent ? HLE_MEM32(parent + 4) : 0,
                    kind == 0 ? "back buffer" : kind == 1 ? "render target texture"
                                  : kind == 3 ? "cube face" : "scratch target",
                    g_backbuffer_va, g_backbuffer_va ? HLE_MEM32(g_backbuffer_va + 4) : 0);
            /* Where in its container this face sits, so a title whose cube
             * this code reads wrongly can be told apart from one it cannot
             * read at all. hle_d3d8_cube_face does the arithmetic; this
             * prints what it was given and what it made of it. */
            if (parent && (HLE_MEM32(parent + 12) & 0x4)) {
                uint32_t pdata = HLE_MEM32(parent + 4), sdata = HLE_MEM32(rt + 4);
                uint32_t f = 0, lv = 0;
                int ok = hle_d3d8_cube_face(parent, rt, &f, &lv);

                fprintf(stderr, "[HLE-D3D8]   cube: container data 0x%08X, surface data "
                        "0x%08X, delta %d -> %s\n", pdata, sdata, (int)(sdata - pdata),
                        ok == 0 ? "face/level below" : "not a face this reads");
                if (ok == 0)
                    fprintf(stderr, "[HLE-D3D8]   cube: face %u level %u\n", f, lv);
            }
        }
        if (kind == 2 && !texture) {
            g_target_failed++;
            kind = 0;
            target = NULL;
            w = g_shadow_width;
            h = g_shadow_height;
        }
    }

    if (zs) {
        int own;

        surface_measure(zs, &zw, &zh, &zfmt);
        if (shadow_trace_on())
            fprintf(stderr, "[TRACE swap %lu]   depth 0x%08X data 0x%08X parent 0x%08X "
                    "%ux%u format 0x%02X\n", g_shadow_swaps, zs, HLE_MEM32(zs + 4),
                    HLE_MEM32(zs + SURFACE_PARENT), zw, zh, zfmt);
        g_z_scale = xbox_depth_z_scale(zfmt);
        own = g_autodepth_va ? zs == g_autodepth_va : (zw == w && zh == h);
        depth = (kind == 0 && own && g_device_depth) ? g_device_depth : NULL;
        /* A depth texture the title will sample: render into its host copy
         * (RECOMP_HLE_D3D8_SHADOW_MAPS=0 for the old scratch depth). */
        if (!depth && zw == w && zh == h && depth_texture_of(zs) && shadow_maps_on()) {
            depth = depth_texture_surface(depth_texture_of(zs));
            if (depth) {
                g_shadow_map_sets++;
                if (w >= 256)
                    g_shadow_map_big_sets++;
            }
        }
        if (!depth)
            depth = depth_surface(w, h);
        g_target_shadow_map = depth && depth_texture_of(zs) && zw == w && zh == h &&
                              shadow_maps_on() && kind != 0;
    } else {
        g_z_scale = 1.0f;
        g_target_shadow_map = 0;
    }

    if (FAILED(host_SetRenderTarget(g_shadow, kind == 0 ? NULL : target, level, face,
                                    depth))) {
        IDirect3DTexture8 *scratch = kind != 0 ? scratch_target(w, h) : NULL;

        /* The host keeps its old targets. An offscreen pass goes to a
         * scratch target of its own size, not the screen: Outrun 2 renders
         * colour into a surface whose parent texture is LIN_D24S8, the host
         * cannot make that a colour target, and its 512x512 shadow pass
         * landed on the back buffer every frame and blacked out the race. */
        g_target_failed++;
        if (scratch && SUCCEEDED(host_SetRenderTarget(g_shadow,
                                     (IDirect3DBaseTexture8 *)scratch, 0, 0, depth))) {
            kind = 2;
        } else {
            /* Go to the back buffer instead, so the sizes below describe
             * what is drawn into. */
            kind = 0;
            w = g_shadow_width;
            h = g_shadow_height;
            depth = zs ? g_device_depth : NULL;
            host_SetRenderTarget(g_shadow, NULL, 0, 0, depth);
        }
    }
    g_target_width = w;
    g_target_height = h;

    /* The Xbox resets the viewport to the whole new target. */
    g_title_viewport.X = 0;
    g_title_viewport.Y = 0;
    g_title_viewport.Width = w;
    g_title_viewport.Height = h;
    g_title_viewport.MinZ = 0.0f;
    g_title_viewport.MaxZ = 1.0f;
    g_title_viewport_set = 1;
    g_host_viewport_mode = -1;
    shadow_viewport_constants(&g_title_viewport);
}
#endif /* _WIN32 */

/* void D3DDevice_InsertCallback(D3DCALLBACKTYPE Type, D3DCALLBACK pCallback,
 *     DWORD Context) -- stdcall; the callback is __cdecl void (DWORD Context).
 *
 * The XDK writes the callback into the push buffer for the GPU to raise when
 * it gets there: READ (0) once the GPU has read everything before it, WRITE
 * (1) once it has finished it. No GPU here runs push buffers, so the callback
 * never came, and Breakdown spun forever on the flag its callback clears
 * (sub_0018EA30, `while (flag) ;` straight after the insert). Everything
 * before the call has already been drawn by the time it returns, so both
 * kinds are due at once, and the callback runs here. The original body still
 * runs first, so the push buffer is what the XDK made. */
HLE_ORIGINAL(D3DDevice_InsertCallback);
HLE_EXPORT(D3DDevice_InsertCallback)
{
    static int seen;
    uint32_t type = HLE_ARG(0), callback = HLE_ARG(1), context = HLE_ARG(2);
    recomp_func_t fn;

    first_call(&seen, "D3DDevice_InsertCallback", callback);
    if (original_missing(hle_original_D3DDevice_InsertCallback, "D3DDevice_InsertCallback"))
        HLE_RETURN(0u);
    HLE_CALL_ORIGINAL(D3DDevice_InsertCallback);
    if (!callback)
        return;
    fn = recomp_lookup(callback);
    if (!fn)
        fn = recomp_lookup_manual(callback);
    if (!fn) {
        static int said;
        if (!said++)
            fprintf(stderr, "[HLE-D3D8] InsertCallback: callback 0x%08X (type %u) is not "
                    "in the dispatch table; it does not run\n", callback, type);
        return;
    }
    {
        /* cdecl: the callee leaves its argument, so the stack is put back. */
        uint32_t saved_esp = g_esp;
        uint32_t saved_eax = g_eax;
        g_esp -= 4; HLE_MEM32(g_esp) = context;
        g_esp -= 4; HLE_MEM32(g_esp) = 0;        /* return address, popped by its ret */
        fn();
        g_esp = saved_esp;
        g_eax = saved_eax;
    }
}

/* The video overlay: the plane XMV movies are shown on by titles that use it
 * (TimeSplitters: Future Perfect calls UpdateOverlay once per decoded frame
 * and then Swap). The picture itself comes from hle_xmv.c, which decodes the
 * movie and hands each frame to d3d8_movie; these only say whether the plane
 * is showing, which is all the host needs from them. */
HLE_ORIGINAL(D3DDevice_EnableOverlay);
HLE_ORIGINAL(D3DDevice_UpdateOverlay);

/* void D3DDevice_EnableOverlay(BOOL Enable)
 *
 * The XDK's own body is NOT run. Turning the overlay off waits for the video
 * scaler to let go of it, polling hardware nothing here emulates: Future
 * Perfect hung there, 82% of its main thread in this function's lifted body,
 * the moment its first movie ended. The plane only exists on the host, so the
 * two flags below are the whole of its state. */
HLE_EXPORT(D3DDevice_EnableOverlay)
{
    static int seen;
    uint32_t enable = HLE_ARG(0);

    first_call(&seen, "D3DDevice_EnableOverlay", enable);
#ifdef _WIN32
    g_overlay_enabled = enable != 0;
    if (!enable) {
        g_overlay_updated = 0;
        d3d8_movie_clear();
    }
#endif
}

/* void D3DDevice_UpdateOverlay(D3DSurface *pSurface, const RECT *SrcRect,
 *     const RECT *DstRect, BOOL EnableColorKey, D3DCOLOR ColorKey)          */
HLE_EXPORT(D3DDevice_UpdateOverlay)
{
    static int seen;

    first_call(&seen, "D3DDevice_UpdateOverlay", HLE_ARG(0));
    if (original_missing(hle_original_D3DDevice_UpdateOverlay, "D3DDevice_UpdateOverlay"))
        HLE_RETURN(0u);
    HLE_CALL_ORIGINAL(D3DDevice_UpdateOverlay);
#ifdef _WIN32
    /* A title that never calls EnableOverlay still means the plane to show
     * when it updates it; the XDK turns it on at the first update. */
    g_overlay_enabled = 1;
    g_overlay_updated = HLE_ARG(0) != 0;
#endif
}

/* void D3DDevice_SetRenderTargetFast(D3DSurface *pRenderTarget,
 *     D3DSurface *pNewZStencil, DWORD Flags) -- stdcall, later XDKs.
 *
 * The same switch without SetRenderTarget's checks, and a title can use both:
 * Outrun 2 (5849) renders its environment cube through SetRenderTarget and
 * goes back to the screen for the world through this one, so without it the
 * whole race was drawn into a 128x128 cube face (Cxbx-Reloaded patches it the
 * same way, onto its SetRenderTarget). */
HLE_ORIGINAL(D3DDevice_SetRenderTargetFast);
HLE_EXPORT(D3DDevice_SetRenderTargetFast)
{
    static int seen;
#ifdef _WIN32
    uint32_t rt = HLE_ARG(0), zs = HLE_ARG(1);
#endif

    first_call(&seen, "D3DDevice_SetRenderTargetFast", HLE_ARG(0));
    if (original_missing(hle_original_D3DDevice_SetRenderTargetFast,
                         "D3DDevice_SetRenderTargetFast"))
        HLE_RETURN(0x80004005u);
    HLE_CALL_ORIGINAL(D3DDevice_SetRenderTargetFast);
#ifdef _WIN32
    if (g_shadow)
        shadow_set_render_target(rt, zs);
#endif
}

/* void D3DDevice_SetRenderTarget(D3DSurface *pRenderTarget,
 *     D3DSurface *pNewZStencil)                                             */
HLE_EXPORT(D3DDevice_SetRenderTarget)
{
    static int seen;
#ifdef _WIN32
    uint32_t rt = HLE_ARG(0), zs = HLE_ARG(1);
#endif

    first_call(&seen, "D3DDevice_SetRenderTarget", HLE_ARG(0));
    if (original_missing(hle_original_D3DDevice_SetRenderTarget, "D3DDevice_SetRenderTarget"))
        HLE_RETURN(0x80004005u);
    HLE_CALL_ORIGINAL(D3DDevice_SetRenderTarget);
#ifdef _WIN32
    if (g_in_create_device && rt) {
        g_backbuffer_va = rt;
        g_autodepth_va = zs;
        note_swap_surface(rt);
        fprintf(stderr, "[HLE-D3D8] CreateDevice set target 0x%08X, depth 0x%08X: "
                "the frame buffer and the device's depth\n", rt, zs);
    }
    if (g_shadow)
        shadow_set_render_target(rt, zs);
#endif
}

/* IDirect3DSurface8 *D3DDevice_GetBackBuffer2(INT BackBuffer)
 * Xbox-only: the back buffer's surface, returned rather than written through
 * a pointer. Worth replacing only to learn which surface that is, so
 * CopyRects below can tell "the title is copying the screen" from "the title
 * is copying something else". */
HLE_EXPORT(D3DDevice_GetBackBuffer2)
{
    static int seen;

    first_call(&seen, "D3DDevice_GetBackBuffer2", HLE_ARG(0));
    if (original_missing(hle_original_D3DDevice_GetBackBuffer2, "D3DDevice_GetBackBuffer2"))
        HLE_RETURN(0u);
    HLE_CALL_ORIGINAL(D3DDevice_GetBackBuffer2);
#ifdef _WIN32
    if (g_shadow && g_eax && (int32_t)HLE_ARG(0) <= 0) {
        g_backbuffer_va = g_eax;
        note_swap_surface(g_eax);
    }
    if (g_shadow && shadow_trace_on())
        fprintf(stderr, "[TRACE swap %lu] GetBackBuffer2(%d) -> 0x%08X data 0x%08X\n",
                g_shadow_swaps, (int32_t)HLE_ARG(0), g_eax,
                g_eax ? HLE_MEM32(g_eax + 4) : 0);
#endif
}

/* HRESULT D3DDevice_CopyRects(IDirect3DSurface8 *src, const RECT *srcRects,
 *                             UINT count, IDirect3DSurface8 *dst,
 *                             const POINT *dstPoints)
 *
 * The case this exists for is a title copying the finished frame into a
 * texture and drawing that texture back over the scene -- a glow or a
 * soften. The title's own copy still runs, but it runs in the guest's world
 * where nothing is rendered, so what it reads is zeros; TimeSplitters 2 then
 * blends those zeros over its own image three times a frame and loses 62% of
 * the picture's brightness. The host repeats the copy from its own back
 * buffer, into the host texture standing in for the destination.
 *
 * Only that case. A copy between two of the title's own surfaces is left to
 * the title, whose result the host never sees anyway, and is counted so a
 * title that needs more than this says so in the log. */
HLE_EXPORT(D3DDevice_CopyRects)
{
    static int seen;
#ifdef _WIN32
    uint32_t src = HLE_ARG(0), dst = HLE_ARG(3);
#endif

    first_call(&seen, "D3DDevice_CopyRects", HLE_ARG(0));
    if (original_missing(hle_original_D3DDevice_CopyRects, "D3DDevice_CopyRects"))
        HLE_RETURN(0x80004005u);
    HLE_CALL_ORIGINAL(D3DDevice_CopyRects);
#ifdef _WIN32
    if (g_shadow && src && dst && shadow_trace_on())
        fprintf(stderr, "[TRACE swap %lu] CopyRects src 0x%08X data 0x%08X -> dst 0x%08X data 0x%08X\n",
                g_shadow_swaps, src, HLE_MEM32(src + 4), dst, HLE_MEM32(dst + 4));
    if (g_shadow && src && dst) {
        static unsigned long from_screen, elsewhere;
        static int said_other;
        uint32_t src_parent = HLE_MEM32(src + SURFACE_PARENT);
        uint32_t dst_parent = HLE_MEM32(dst + SURFACE_PARENT);
        UINT sw, sh, dw, dh;
        uint32_t sfmt, dfmt;

        surface_measure(src, &sw, &sh, &sfmt);
        surface_measure(dst, &dw, &dh, &dfmt);
        /* The source is the screen when it is the surface GetBackBuffer2
         * returned, or -- before that is known -- any parentless surface of
         * the back buffer's size, the same test SetRenderTarget uses. */
        if (dst != g_backbuffer_va &&
            (g_backbuffer_va ? src == g_backbuffer_va
                             : (!src_parent && sw == g_shadow_width && sh == g_shadow_height)) &&
            dst_parent && HLE_MEM32(dst_parent + 4) == HLE_MEM32(dst + 4)) {
            IDirect3DTexture8 *tex = hle_d3d8_render_texture(g_shadow, dst_parent);
            /* CopyRects(src, rects, count, dst, points): with rectangles,
             * only those, each to its point (or, with no points, to where
             * it was). TimeSplitters 2 draws its handheld's map into a
             * corner of the back buffer and copies that corner into the
             * handheld's screen; copying the whole frame showed the frame
             * there instead of the map. */
            uint32_t rects_va = HLE_ARG(1), count = HLE_ARG(2), points_va = HLE_ARG(4);
            static int said_rect;

            if (tex && rects_va && count) {
                uint32_t i, n = count > 16 ? 16 : count;

                for (i = 0; i < n; i++) {
                    RECT r;
                    POINT p;

                    r.left   = (LONG)HLE_MEM32(rects_va + i * 16 + 0);
                    r.top    = (LONG)HLE_MEM32(rects_va + i * 16 + 4);
                    r.right  = (LONG)HLE_MEM32(rects_va + i * 16 + 8);
                    r.bottom = (LONG)HLE_MEM32(rects_va + i * 16 + 12);
                    p.x = points_va ? (LONG)HLE_MEM32(points_va + i * 8 + 0) : r.left;
                    p.y = points_va ? (LONG)HLE_MEM32(points_va + i * 8 + 4) : r.top;
                    if (SUCCEEDED(host_CopyBackBufferRectToTexture(tex, &r, &p)))
                        from_screen++;
                    if (said_rect++ < 4)
                        fprintf(stderr, "[HLE-D3D8] the title copies part of its screen: "
                                "%ld,%ld-%ld,%ld to %ld,%ld in texture 0x%08X (%ux%u)\n",
                                (long)r.left, (long)r.top, (long)r.right, (long)r.bottom,
                                (long)p.x, (long)p.y, dst_parent, dw, dh);
                }
            } else if (tex && dw && dh && (dw < sw || dh < sh)) {
                /* No rectangles and a smaller destination: CopyRects does
                 * not scale, so it copies the destination's size from the
                 * top left. TimeSplitters 2 composes its handheld's map in
                 * the top left 128x128 of the back buffer and copies it into
                 * the handheld's screen this way, every frame; scaling the
                 * whole frame into it showed the frame instead of the map. */
                RECT r;
                POINT p;

                r.left = r.top = 0;
                r.right = (LONG)dw;
                r.bottom = (LONG)dh;
                p.x = p.y = 0;
                if (SUCCEEDED(host_CopyBackBufferRectToTexture(tex, &r, &p)))
                    from_screen++;
                if (said_rect++ < 4)
                    fprintf(stderr, "[HLE-D3D8] the title copies the top left %ux%u of its "
                            "screen into texture 0x%08X\n", dw, dh, dst_parent);
            } else if (tex && SUCCEEDED(host_CopyBackBufferToTexture(tex))) {
                from_screen++;
                if (from_screen == 1)
                    fprintf(stderr, "[HLE-D3D8] the title reads its own screen back: "
                            "copying the host frame into texture 0x%08X (%ux%u)\n",
                            dst_parent, dw, dh);
            }
        } else {
            elsewhere++;
            if (dst == g_backbuffer_va || !dst_parent) {
                note_framebuffer_phys(HLE_MEM32(dst + 4));
                note_framebuffer_phys(HLE_MEM32(src + 4));
            }
            if (said_other++ < 8)
                fprintf(stderr, "[HLE-D3D8] CopyRects not recognised as a screen read: "
                        "src 0x%08X %ux%u parent 0x%08X | dst 0x%08X %ux%u parent 0x%08X "
                        "(dst parent data 0x%08X, dst data 0x%08X; back buffer 0x%08X)\n",
                        src, sw, sh, src_parent, dst, dw, dh, dst_parent,
                        dst_parent ? HLE_MEM32(dst_parent + 4) : 0u,
                        HLE_MEM32(dst + 4), g_backbuffer_va);
        }
    }
#endif
}


#ifdef _WIN32
/* The vertices a program with NORMPACKED3 registers reads: each vertex copied
 * behind its unpacked normals, as shadow_read_declaration laid them out. The
 * bits are x:11, y:11, z:10, signed, divided by 1023, 1023 and 511
 * (Cxbx-Reloaded's vertex buffer conversion). Returns NULL with *failed clear
 * when the current program packs nothing, and NULL with *failed set when the
 * copy cannot be made; otherwise the copy, and *stride grows to match. */
static uint8_t *shadow_expand_vertices(const void *verts, UINT vertices, UINT *stride,
                                       uint32_t first, int *failed)
{
    const struct shadow_program *p;
    const uint8_t *base[SHADOW_MAX_PACKED];
    uint32_t sstride[SHADOW_MAX_PACKED];
    UINT in_stride = *stride, out_stride, shift, v;
    uint8_t *out;
    int k;

    *failed = 0;
    if (g_ff_active) {
        /* Fixed function by declaration (ff_from_declaration): every vertex
         * rebuilt in FVF order from wherever its inputs are. */
        const uint8_t *src[FF_MAX_INPUTS];
        uint32_t sst[FF_MAX_INPUTS];

        for (k = 0; k < g_ff.count; k++) {
            if (!g_ff.stream[k]) {
                src[k] = (const uint8_t *)verts;
                sst[k] = in_stride;
                continue;
            }
            src[k] = first == NO_FIRST_VERTEX ? NULL
                   : (const uint8_t *)hle_d3d8_stream_vertices(g_ff.stream[k], first,
                                                               vertices, &sst[k]);
            if (!src[k] || sst[k] < g_ff.offset[k] + ff_format_size(g_ff.format[k])) {
                *failed = 1;
                return NULL;
            }
        }
        out_stride = g_ff.out_stride;
        out = calloc((size_t)vertices, out_stride);
        if (!out) {
            *failed = 1;
            return NULL;
        }
        for (v = 0; v < vertices; v++) {
            uint8_t *dst = out + (size_t)v * out_stride;
            for (k = 0; k < g_ff.count; k++) {
                const uint8_t *at = src[k] + (size_t)v * sst[k] + g_ff.offset[k];
                float f[4];
                int c = ff_read_floats(g_ff.format[k], at, f);

                if (g_ff.as_color[k]) {
                    if (g_ff.format[k] == 0x40) {
                        memcpy(dst + g_ff.out[k], at, 4);   /* already a D3DCOLOR */
                    } else {
                        uint8_t bgra[4];
                        float r = c > 0 ? f[0] : 0, g = c > 1 ? f[1] : 0;
                        float b = c > 2 ? f[2] : 0, a = c > 3 ? f[3] : 1;
#define FF_U8(x) (uint8_t)((x) <= 0 ? 0 : (x) >= 1 ? 255 : (int)((x) * 255.0f + 0.5f))
                        bgra[0] = FF_U8(b); bgra[1] = FF_U8(g);
                        bgra[2] = FF_U8(r); bgra[3] = FF_U8(a);
#undef FF_U8
                        memcpy(dst + g_ff.out[k], bgra, 4);
                    }
                } else {
                    memcpy(dst + g_ff.out[k], f, (size_t)g_ff.floats[k] * sizeof f[0]);
                }
            }
        }
        *stride = out_stride;
        return out;
    }
    if (!g_shadow_vs_is_program || g_shadow_vs_slot < 0)
        return NULL;
    p = &g_programs[g_shadow_vs_slot];
    if (!p->packed_count)
        return NULL;
    /* Where each register's vertices are: stream 0 is what the draw was
     * handed, any other stream is looked up at the same first vertex. A UP
     * draw has no other streams to read, so it cannot feed this program. */
    for (k = 0; k < p->packed_count; k++) {
        if (!p->packed_stream[k]) {
            base[k] = (const uint8_t *)verts;
            sstride[k] = in_stride;
            continue;
        }
        base[k] = first == NO_FIRST_VERTEX ? NULL
                : (const uint8_t *)hle_d3d8_stream_vertices(p->packed_stream[k], first,
                                                            vertices, &sstride[k]);
        if (!base[k] || sstride[k] < p->packed_offset[k] + p->packed_size[k]) {
            *failed = 1;
            return NULL;
        }
    }
    shift = p->expanded_bytes;
    /* Rounded up, so the host stride is a multiple of four even when the
     * title's is not (Dino Crisis 3's skinned vertex is 50 bytes). */
    out_stride = shift + ((in_stride + 3u) & ~3u);
    out = calloc((size_t)vertices, out_stride);
    if (!out) {
        *failed = 1;
        return NULL;
    }
    for (v = 0; v < vertices; v++) {
        const uint8_t *src = (const uint8_t *)verts + (size_t)v * in_stride;
        uint8_t *dst = out + (size_t)v * out_stride;

        for (k = 0; k < p->packed_count; k++) {
            const uint8_t *at = base[k] + (size_t)v * sstride[k] + p->packed_offset[k];
            float n[4];
            int c, count;

            if (p->packed_raw[k]) {
                memcpy(dst + p->packed_out[k], at, p->packed_size[k]);
                continue;
            }
            if (p->packed_format[k] == 0x16u) {      /* NORMPACKED3 */
                uint32_t bits;
                memcpy(&bits, at, sizeof bits);
                n[0] = (float)((int32_t)(bits << 21) >> 21) / 1023.0f;
                n[1] = (float)((int32_t)(bits << 10) >> 21) / 1023.0f;
                n[2] = (float)((int32_t)bits >> 22) / 511.0f;
                count = 3;
            } else {                                 /* SHORTn: the value itself */
                count = (int)(p->packed_format[k] >> 4);
                for (c = 0; c < count; c++) {
                    int16_t s;
                    memcpy(&s, at + 2 * c, sizeof s);
                    n[c] = (float)s;
                }
            }
            memcpy(dst + p->packed_out[k], n, (size_t)count * sizeof n[0]);
        }
        memcpy(dst + shift, src, in_stride);
    }
    *stride = out_stride;
    return out;
}

/* The host half of every non-indexed draw: the UP draw below and the vertex
 * buffer draws in hle_d3d8_vertex.c. `verts` is already a host pointer to the
 * first vertex; from_buffer only picks the counter the draw lands in. */
void hle_d3d8_shadow_draw(uint32_t xpt, uint32_t count, const void *verts,
                          uint32_t stride, int from_buffer)
{
    D3DPRIMITIVETYPE pt;
    UINT prims, host_stride = stride;
    uint8_t *loop = NULL, *expanded;
    int failed;
    HRESULT hr;

    if (!g_shadow || !verts || !stride || !count)
        return;
    if (!shadow_can_draw(xpt, stride))
        return;
    if (!xbox_primitive_to_host(xpt, count, &pt, &prims)) {
        g_draws_primitive++;
        return;
    }
    expanded = shadow_expand_vertices(verts, count, &host_stride,
                                      from_buffer ? g_draw_first : NO_FIRST_VERTEX,
                                      &failed);
    g_draw_first = NO_FIRST_VERTEX;
    if (failed) {
        g_draws_failed++;
        return;
    }
    if (expanded)
        verts = expanded;
    if (xpt == XPT_LINELOOP) {           /* close the loop: repeat vertex 0 */
        loop = malloc((size_t)(count + 1) * host_stride);
        if (!loop) {
            free(expanded);
            g_draws_failed++;
            return;
        }
        memcpy(loop, verts, (size_t)count * host_stride);
        memcpy(loop + (size_t)count * host_stride, verts, host_stride);
        verts = loop;
    }
    hr = host_DrawPrimitiveUP(g_shadow, pt, prims, verts, host_stride);
    free(loop);
    free(expanded);
    if (FAILED(hr))
        g_draws_failed++;
    else if (from_buffer)
        g_draws_vb++;
    else
        g_draws_up++;
    if (SUCCEEDED(hr))
        note_draw_sampled_movie();
}

/* The host half of every indexed draw. `count` counts indices, always 16-bit
 * on the Xbox, relative to `verts`; the host wants the vertex range too, found
 * from the largest index. The host's indexed UP draw converts no primitive
 * types, so fans, polygons, quad lists and line loops are rewritten into index
 * lists here. */
void hle_d3d8_shadow_draw_indexed(uint32_t xpt, uint32_t count, const uint16_t *idx,
                                  const void *verts, uint32_t stride, int from_buffer)
{
    uint16_t *list = NULL;
    uint8_t *expanded;
    D3DPRIMITIVETYPE pt;
    UINT prims, vertices = 0, i, n = 0, host_stride = stride, min_index = 0;
    int failed;
    HRESULT hr;

    if (!g_shadow || !idx || !verts || !stride || !count)
        return;
    if (!shadow_can_draw(xpt, stride))
        return;
    if (!xbox_primitive_to_host(xpt, count, &pt, &prims)) {
        g_draws_primitive++;
        return;
    }
    /* Only the vertices between the lowest and highest index are handed to
     * the host, with the indices rebased to start at zero. A title that
     * draws a level from one shared vertex buffer indexes tens of thousands
     * of vertices in: TimeSplitters 2's Siberia draws 340 pieces a frame with
     * indices up to 35,000, and copying every vertex below the highest one
     * for each of them moved hundreds of megabytes a frame and ran at 14 fps.
     * The range a draw actually uses is a few kilobytes. */
    {
        UINT lo = 0xFFFFu, hi = 0;
        for (i = 0; i < count; i++) {
            if (idx[i] < lo) lo = idx[i];
            if (idx[i] > hi) hi = idx[i];
        }
        min_index = lo;
        vertices = hi - lo + 1;
        verts = (const uint8_t *)verts + (size_t)lo * stride;
    }

    switch (xpt) {
    case XPT_TRIANGLEFAN:
    case XPT_POLYGON:
        list = malloc((size_t)prims * 3 * sizeof *list);
        for (i = 0; list && i < prims; i++) {
            list[n++] = (uint16_t)(idx[0] - min_index);
            list[n++] = (uint16_t)(idx[i + 1] - min_index);
            list[n++] = (uint16_t)(idx[i + 2] - min_index);
        }
        pt = D3DPT_TRIANGLELIST;
        break;
    case XPT_QUADLIST:
        list = malloc((size_t)prims * 6 * sizeof *list);
        for (i = 0; list && i < prims; i++) {
            const uint16_t *q = idx + i * 4;
            uint16_t a = (uint16_t)(q[0] - min_index), b = (uint16_t)(q[1] - min_index);
            uint16_t c = (uint16_t)(q[2] - min_index), d = (uint16_t)(q[3] - min_index);
            list[n++] = a; list[n++] = b; list[n++] = c;
            list[n++] = a; list[n++] = c; list[n++] = d;
        }
        pt = D3DPT_TRIANGLELIST;
        prims *= 2;
        break;
    case XPT_LINELOOP:
        list = malloc(((size_t)count + 1) * sizeof *list);
        for (i = 0; list && i < count; i++)
            list[i] = (uint16_t)(idx[i] - min_index);
        if (list)
            list[count] = (uint16_t)(idx[0] - min_index);
        break;
    default:
        if (min_index) {
            list = malloc((size_t)count * sizeof *list);
            for (i = 0; list && i < count; i++)
                list[i] = (uint16_t)(idx[i] - min_index);
        }
        break;
    }
    if ((xpt == XPT_TRIANGLEFAN || xpt == XPT_POLYGON || xpt == XPT_QUADLIST ||
         xpt == XPT_LINELOOP || min_index) && !list) {
        g_draws_failed++;
        return;
    }
    expanded = shadow_expand_vertices(verts, vertices, &host_stride,
                                      from_buffer && g_draw_first != NO_FIRST_VERTEX
                                          ? g_draw_first + min_index : NO_FIRST_VERTEX,
                                      &failed);
    g_draw_first = NO_FIRST_VERTEX;
    if (failed) {
        free(list);
        g_draws_failed++;
        return;
    }
    hr = host_DrawIndexedPrimitiveUP(
        g_shadow, pt, 0, vertices, prims, list ? list : idx, D3DFMT_INDEX16,
        expanded ? expanded : verts, host_stride);
    free(list);
    free(expanded);
    if (FAILED(hr))
        g_draws_failed++;
    else if (from_buffer)
        g_draws_indexed_vb++;
    else
        g_draws_indexed_up++;
    if (SUCCEEDED(hr))
        note_draw_sampled_movie();
}
#endif /* _WIN32 */

/* void D3DDevice_DrawVerticesUP(D3DPRIMITIVETYPE PrimitiveType,
 *     UINT VertexCount, const void *pVertexStreamZeroData,
 *     UINT VertexStreamZeroStride)                                          */
HLE_EXPORT(D3DDevice_DrawVerticesUP)
{
    static int seen;
    uint32_t xpt = HLE_ARG(0);
#ifdef _WIN32
    uint32_t count = HLE_ARG(1), data = HLE_ARG(2), stride = HLE_ARG(3);
#endif

    first_call(&seen, "D3DDevice_DrawVerticesUP", xpt);
    if (original_missing(hle_original_D3DDevice_DrawVerticesUP, "D3DDevice_DrawVerticesUP"))
        HLE_RETURN(0x80004005u);
    HLE_CALL_ORIGINAL(D3DDevice_DrawVerticesUP);
#ifdef _WIN32
    if (data)
        hle_d3d8_shadow_draw(xpt, count, HLE_PTR(data), stride, 0);
#endif
}

/* void D3DDevice_DrawIndexedVerticesUP(D3DPRIMITIVETYPE PrimitiveType,
 *     UINT VertexCount, const void *pIndexData,
 *     const void *pVertexStreamZeroData, UINT VertexStreamZeroStride)
 * VertexCount counts indices; no base vertex index applies (Cxbx-Reloaded). */
HLE_EXPORT(D3DDevice_DrawIndexedVerticesUP)
{
    static int seen;
    uint32_t xpt = HLE_ARG(0);
#ifdef _WIN32
    uint32_t count = HLE_ARG(1), index_va = HLE_ARG(2);
    uint32_t data = HLE_ARG(3), stride = HLE_ARG(4);
#endif

    first_call(&seen, "D3DDevice_DrawIndexedVerticesUP", xpt);
    if (original_missing(hle_original_D3DDevice_DrawIndexedVerticesUP,
                         "D3DDevice_DrawIndexedVerticesUP"))
        HLE_RETURN(0x80004005u);
    HLE_CALL_ORIGINAL(D3DDevice_DrawIndexedVerticesUP);
#ifdef _WIN32
    if (index_va && data)
        hle_d3d8_shadow_draw_indexed(xpt, count, (const uint16_t *)HLE_PTR(index_va),
                                     HLE_PTR(data), stride, 0);
#endif
}

/* HRESULT D3DDevice_GetVisibilityTestResult(DWORD Index, UINT *pResult,
 *     ULONGLONG *pTimeStamp)
 *
 * A visibility test's answer is written by the GPU: EndVisibilityTest puts a
 * GET_REPORT in the push buffer, the NV2A writes the pixel count and a time
 * stamp into a report slot, and this function answers D3DERR_TESTINCOMPLETE
 * (0x88760828) until the slot's status word leaves -1. Here nothing executes
 * the push buffer -- the host draws from the replaced calls -- so no report
 * is ever written, and a title that waits for one spins for ever. Halo's main
 * menu does exactly that, with no timeout (XDK 3925, its wait at 0x00063460).
 *
 * The original runs first, so a report that does arrive (RECOMP_PB_EXEC, the
 * software executor) is used, and nothing is changed while that executor is
 * on. Otherwise an incomplete test is answered as complete with no pixels
 * drawn: the conservative count, under which a lens flare or a corona that
 * tests its own visibility draws nothing rather than shining through walls.
 * RECOMP_HLE_D3D8_VISIBLE_PIXELS=<n> answers n instead. Answering from the
 * host -- an occlusion query around the test's draws -- is not done yet.
 *
 * Only a title that waits for the answer gets one. Halo spins on its test;
 * TimeSplitters 2 and Future Perfect poll theirs once a frame and draw on
 * without the result. Answered, a poller starts a new test every frame, and
 * each one waits on the GPU's acknowledgement; with the acknowledge thread
 * idling between passes (RECOMP_NV2A_ACK_IDLE_US, #34) TimeSplitters 2's
 * first level fell to 5 fps. So a test is answered once it has been asked
 * VISIBILITY_SPIN times running with no frame presented in between; one
 * polled a frame at a time keeps seeing "not yet", as it does without
 * this replacement. */
#define VISIBILITY_SPIN 64u
HLE_ORIGINAL(D3DDevice_GetVisibilityTestResult);
HLE_EXPORT(D3DDevice_GetVisibilityTestResult)
{
    static int seen, executor = -1;
    static unsigned long answered, polled;
    static uint32_t pixels, spin_index = 0xFFFFFFFFu, spin_polls;
    static unsigned long spin_swap;
    uint32_t index = HLE_ARG(0), result_va = HLE_ARG(1), stamp_va = HLE_ARG(2);

    first_call(&seen, "D3DDevice_GetVisibilityTestResult", index);
    if (original_missing(hle_original_D3DDevice_GetVisibilityTestResult,
                         "D3DDevice_GetVisibilityTestResult"))
        HLE_RETURN(0x80004005u);
    HLE_CALL_ORIGINAL(D3DDevice_GetVisibilityTestResult);
    if (g_eax != 0x88760828u)                 /* not D3DERR_TESTINCOMPLETE */
        return;
    if (executor < 0) {
        const char *e = getenv("RECOMP_HLE_D3D8_VISIBLE_PIXELS");
        executor = getenv("RECOMP_PB_EXEC") != NULL;
        pixels = (e && *e) ? (uint32_t)strtoul(e, NULL, 0) : 0u;
    }
    if (executor)
        return;                               /* the executor will write it */
    if (index != spin_index || g_shadow_swaps != spin_swap) {
        spin_index = index;                   /* another test, or a new frame */
        spin_swap = g_shadow_swaps;
        spin_polls = 0;
    }
    if (++spin_polls < VISIBILITY_SPIN) {
        if (polled++ == 0) {
            fprintf(stderr, "[HLE-D3D8] visibility test %u: no GPU report; left "
                    "incomplete unless the title asks %u times in one frame "
                    "(it is waiting)\n", index, VISIBILITY_SPIN);
            fflush(stderr);
        }
        return;                               /* D3DERR_TESTINCOMPLETE stands */
    }
    if (result_va)
        HLE_MEM32(result_va) = pixels;
    if (stamp_va) {
        HLE_MEM32(stamp_va) = 0;
        HLE_MEM32(stamp_va + 4) = 0;
    }
    if (answered++ == 0) {
        fprintf(stderr, "[HLE-D3D8] visibility test %u: no GPU report (nothing "
                "executes the push buffer); answered complete, %u pixel(s) "
                "(RECOMP_HLE_D3D8_VISIBLE_PIXELS)\n", index, pixels);
        fflush(stderr);
    }
    HLE_RETURN(0);                            /* S_OK */
}

#ifdef _WIN32
/* For hle_d3d8_texture.c: the shadow device (NULL when shadow mode is off)
 * and the frame count its cache ages entries by. */
IDirect3DDevice8 *hle_d3d8_shadow_device(void)
{
    return g_shadow;
}

unsigned long hle_d3d8_shadow_swaps(void)
{
    return g_shadow_swaps;
}
#endif
