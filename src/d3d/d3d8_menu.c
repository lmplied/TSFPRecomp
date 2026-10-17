/*
 * d3d8_menu.c -- see d3d8_menu.h.
 *
 * The panel is drawn by GDI into a 32-bit DIB the size it will appear in the
 * window, uploaded when its content or the window's size changes, and drawn
 * over the window's buffer as one quad read texel for pixel. Same approach,
 * and same reasons, as d3d8_overlay.c: a real font at a real size, no glyph
 * table, and nothing on the device left changed for the title.
 *
 * The look follows the launcher (src/launcher/launcher_theme.h): near-black
 * panels, one green for structure and focus, light text -- or the title's
 * own colours (xbox_D3D8MenuSetPalette), as its launcher has them.
 */
#include "d3d8_internal.h"
#include "d3d8_menu.h"

#include <stdio.h>
#include <string.h>

static struct {
    COLORREF accent, accent_dim, ink, ink_dim, panel, row, row_lit;
} g_pal = {
    RGB(0x9B, 0xCE, 0x1E), RGB(0x4E, 0x6C, 0x14), RGB(0xE8, 0xF2, 0xD8),
    RGB(0x8A, 0x97, 0x7C), RGB(0x0C, 0x0F, 0x0A), RGB(0x14, 0x19, 0x10),
    RGB(0x1E, 0x2C, 0x0E),
};

#define GREEN      g_pal.accent
#define GREEN_DIM  g_pal.accent_dim
#define INK        g_pal.ink
#define INK_DIM    g_pal.ink_dim
#define PANEL      g_pal.panel
#define ROW        g_pal.row
#define ROW_LIT    g_pal.row_lit

/* a + (b - a) * t, per channel, of two 0xRRGGBB. */
static COLORREF mix(unsigned a, unsigned b, double t)
{
    int ar = (int)(a >> 16) & 0xFF, ag = (int)(a >> 8) & 0xFF, ab = (int)a & 0xFF;
    int br = (int)(b >> 16) & 0xFF, bg = (int)(b >> 8) & 0xFF, bb = (int)b & 0xFF;

    return RGB((int)(ar + (br - ar) * t), (int)(ag + (bg - ag) * t), (int)(ab + (bb - ab) * t));
}

void xbox_D3D8MenuSetPalette(unsigned dark, unsigned mid, unsigned ink)
{
    /* As the launcher derives its own from the same three (theme_palette_from). */
    g_pal.accent     = mix(ink, ink, 0.0);
    g_pal.accent_dim = mix(dark, ink, 0.35);
    g_pal.ink        = g_pal.accent;
    g_pal.ink_dim    = mix(ink, mid, 0.42);
    g_pal.panel      = mix(0, dark, 0.72);
    g_pal.row        = mix(0, dark, 0.92);
    g_pal.row_lit    = mix(dark, mid, 0.85);
}

static struct {
    int             tried, failed;
    RhiBuffer      *cb;
    RhiShader      *vs, *ps;
    RhiBlendState  *blend;
    RhiDepthState  *depth;
    RhiRasterState *raster;
    /* The panel at its current size. */
    RhiImage       *texture;
    RhiView        *srv;
    HDC             dc;
    HBITMAP         bitmap;
    void           *bits;
    unsigned        pw, ph;         /* panel size, in window pixels */
    unsigned        win_w, win_h;   /* the window it was laid out for */
    int             top;            /* first row shown: the list scrolls */
    /* Where the last drawn panel's parts are, for xbox_D3D8MenuHit: the
     * panel's corner in the window, and the rest in panel pixels. */
    int             x0, y0;
    RECT            hit_tab[D3D8_MENU_TABS];
    int             hit_tabs;
    RECT            hit_row[D3D8_MENU_ROWS], hit_left[D3D8_MENU_ROWS], hit_right[D3D8_MENU_ROWS];
    int             hit_shown[D3D8_MENU_ROWS];  /* in view, and not a heading */
    /* What to show, and what was last drawn. */
    CRITICAL_SECTION lock;
    int             lock_ready;
    int             shown;
    D3D8Menu        menu, drawn;
    int             dirty;
} g;

typedef struct {
    float x, y, w, h;               /* clip space */
    float tex_w, tex_h, alpha, pad;
} MenuConstants;

static const char kShaderSource[] =
    "cbuffer Menu : register(b7) {\n"
    "    float4 rect;        // x, y, w, h in clip space\n"
    "    float4 size;        // texture width, height, opacity\n"
    "};\n"
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VSOut vs_main(uint id : SV_VertexID) {\n"
    "    float2 c = float2(id & 1, id >> 1);\n"
    "    VSOut o;\n"
    "    o.pos = float4(rect.x + c.x * rect.z, rect.y - c.y * rect.w, 0, 1);\n"
    "    o.uv  = c * size.xy;\n"
    "    return o;\n"
    "}\n"
    "Texture2D<float4> tex : register(t8);\n"
    "float4 ps_main(VSOut i) : SV_Target {\n"
    /* GDI writes B,G,R,x bytes, which a B8G8R8A8 texture reads as RGB. */
    "    float3 rgb = tex.Load(int3(int2(i.uv), 0)).rgb;\n"
    "    return float4(rgb, size.z);\n"
    "}\n";

static void menu_fail(const char *what)
{
    g.failed = 1;
    fprintf(stderr, "D3D8 menu: %s failed; the in-game menu is off for this run\n", what);
    fflush(stderr);
    d3d8_menu_shutdown();
}

static RhiShader *compile_one(uint32_t stage, const char *entry, const char *target)
{
    RhiShaderSource src;
    RhiShader *s;
    char err[2048];

    memset(&src, 0, sizeof src);
    src.hlsl = kShaderSource;
    src.len = sizeof kShaderSource - 1;
    src.name = "menu";
    src.entry = entry;
    src.target = target;
    s = rhi_shader_create(stage, &src, err, sizeof err);
    if (!s && err[0])
        fprintf(stderr, "D3D8 menu: %s\n", err);
    return s;
}

static int menu_create(void)
{
    RhiBufferDesc bd;
    RhiBlendDesc bl;
    RhiDepthDesc ds;
    RhiRasterDesc rd;

    if (g.failed) return 0;
    if (g.tried) return g.ps != NULL;
    g.tried = 1;
    if (!rhi_device_ready()) { g.failed = 1; return 0; }

    memset(&bd, 0, sizeof bd);
    bd.size = sizeof(MenuConstants);
    bd.usage = RHI_USAGE_DYNAMIC;
    bd.bind = RHI_BIND_UNIFORM;
    bd.cpu_access = RHI_CPU_WRITE;
    if (!(g.cb = rhi_buffer_create(&bd, NULL))) { menu_fail("CreateBuffer"); return 0; }
    if (!(g.vs = compile_one(RHI_STAGE_VERTEX, "vs_main", "vs_4_0"))) {
        menu_fail("the vertex shader");
        return 0;
    }
    if (!(g.ps = compile_one(RHI_STAGE_PIXEL, "ps_main", "ps_4_0"))) {
        menu_fail("the pixel shader");
        return 0;
    }

    memset(&bl, 0, sizeof bl);
    bl.enable = 1;
    bl.src = RHI_BLEND_SRC_ALPHA;
    bl.dst = RHI_BLEND_INV_SRC_ALPHA;
    bl.op = RHI_BLEND_OP_ADD;
    bl.src_alpha = RHI_BLEND_ONE;
    bl.dst_alpha = RHI_BLEND_INV_SRC_ALPHA;
    bl.op_alpha = RHI_BLEND_OP_ADD;
    bl.write_mask = RHI_WRITE_ALL;
    if (!(g.blend = rhi_blend_state_create(&bl))) { menu_fail("CreateBlendState"); return 0; }
    memset(&ds, 0, sizeof ds);
    if (!(g.depth = rhi_depth_state_create(&ds))) { menu_fail("CreateDepthStencilState"); return 0; }
    memset(&rd, 0, sizeof rd);
    rd.fill = RHI_FILL_SOLID;
    rd.cull = RHI_CULL_NONE;
    rd.depth_clip = 1;
    if (!(g.raster = rhi_raster_state_create(&rd))) { menu_fail("CreateRasterizerState"); return 0; }

    g.dc = CreateCompatibleDC(NULL);
    if (!g.dc) { menu_fail("CreateCompatibleDC"); return 0; }
    SetBkMode(g.dc, TRANSPARENT);
    return 1;
}

static void release_panel(void)
{
    rhi_view_destroy(g.srv);       g.srv = NULL;
    rhi_image_destroy(g.texture);  g.texture = NULL;
    if (g.bitmap) { DeleteObject(g.bitmap); g.bitmap = NULL; }
    g.bits = NULL;
    g.pw = g.ph = 0;
}

/* A panel sized for a window of w by h: about 780 by 600 at 1080 lines,
 * larger on a larger screen, never past the window's edges. */
static int layout(unsigned w, unsigned h)
{
    RhiImageDesc td;
    BITMAPINFO bi;
    double s = (double)h / 1080.0 * 1.25;
    unsigned pw, ph;

    if (s < 0.6) s = 0.6;
    pw = (unsigned)(780 * s);
    ph = (unsigned)(600 * s);
    if (pw + 24 > w) pw = w > 24 ? w - 24 : w;
    if (ph + 24 > h) ph = h > 24 ? h - 24 : h;
    if (pw < 64 || ph < 64)
        return 0;
    if (g.texture && pw == g.pw && ph == g.ph) {
        g.win_w = w;
        g.win_h = h;
        return 1;
    }
    release_panel();

    memset(&td, 0, sizeof td);
    td.type = RHI_IMAGE_2D;
    td.width = pw;
    td.height = ph;
    td.depth = 1;
    td.mip_levels = 1;
    td.format = RHI_FORMAT_B8G8R8A8_UNORM;
    td.samples = 1;
    td.usage = RHI_USAGE_DYNAMIC;
    td.bind = RHI_BIND_SAMPLED;
    td.cpu_access = RHI_CPU_WRITE;
    if (!(g.texture = rhi_image_create(&td, NULL))) { menu_fail("CreateTexture2D"); return 0; }
    if (!(g.srv = rhi_view_create(g.texture, RHI_VIEW_SAMPLED, NULL))) {
        menu_fail("CreateShaderResourceView");
        return 0;
    }
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = (LONG)pw;
    bi.bmiHeader.biHeight = -(LONG)ph;          /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g.bitmap = CreateDIBSection(g.dc, &bi, DIB_RGB_COLORS, &g.bits, NULL, 0);
    if (!g.bitmap) { menu_fail("CreateDIBSection"); return 0; }
    SelectObject(g.dc, g.bitmap);
    g.pw = pw;
    g.ph = ph;
    g.win_w = w;
    g.win_h = h;
    g.dirty = 1;
    return 1;
}

static HFONT font(int px, int weight)
{
    return CreateFontA(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_SWISS, "Segoe UI");
}

static void fill(int x, int y, int w, int h, COLORREF c)
{
    RECT r;
    HBRUSH b = CreateSolidBrush(c);

    r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    FillRect(g.dc, &r, b);
    DeleteObject(b);
}

static void set_rect(RECT *r, int x, int y, int w, int h)
{
    r->left = x; r->top = y; r->right = x + w; r->bottom = y + h;
}

static void frame(int x, int y, int w, int h, int t, COLORREF c)
{
    fill(x, y, w, t, c);
    fill(x, y + h - t, w, t, c);
    fill(x, y, t, h, c);
    fill(x + w - t, y, t, h, c);
}

static void text(int x, int y, int w, int h, const char *s, HFONT f, COLORREF c, UINT how)
{
    RECT r;

    WCHAR wide[512];

    r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    SelectObject(g.dc, f);
    SetTextColor(g.dc, c);
    /* UTF-8, like everything else the menu shows (action names come from a
     * JSON file): the system code page would turn a degree sign into two
     * characters. */
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, wide, (int)(sizeof wide / sizeof wide[0])) > 0)
        DrawTextW(g.dc, wide, -1, &r, how | DT_NOPREFIX);
    else
        DrawTextA(g.dc, s, -1, &r, how | DT_NOPREFIX);
}

/* Lay the whole panel out in the DIB and upload it. */
static void render(const D3D8Menu *m)
{
    double s = (double)g.ph / 600.0;
    int pad = (int)(28 * s), rowh = (int)(46 * s), gap = (int)(6 * s);
    int x = pad, w = (int)g.pw - 2 * pad;
    int y, i, list_top, list_bottom, visible;
    HFONT f_title = font((int)(28 * s), FW_BOLD);
    HFONT f_tab   = font((int)(17 * s), FW_SEMIBOLD);
    HFONT f_row   = font((int)(17 * s), FW_SEMIBOLD);
    HFONT f_value = font((int)(17 * s), FW_NORMAL);
    HFONT f_small = font((int)(14 * s), FW_NORMAL);

    fill(0, 0, (int)g.pw, (int)g.ph, PANEL);
    frame(0, 0, (int)g.pw, (int)g.ph, (int)(2 * s) > 0 ? (int)(2 * s) : 1, GREEN_DIM);

    y = pad;
    text(x, y, w, (int)(36 * s), m->title, f_title, INK, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    y += (int)(48 * s);

    /* Tabs, left to right; the current one lit and underlined. */
    if (m->tab_count > 0) {
        int tw = w / m->tab_count;
        if (tw > (int)(170 * s)) tw = (int)(170 * s);
        g.hit_tabs = 0;
        for (i = 0; i < m->tab_count && i < D3D8_MENU_TABS; i++) {
            int lit = i == m->tab;
            text(x + i * tw, y, tw, (int)(28 * s), m->tabs[i], f_tab,
                 lit ? GREEN : INK_DIM, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            if (lit)
                fill(x + i * tw, y + (int)(30 * s), tw - (int)(24 * s), (int)(3 * s) + 1, GREEN);
            set_rect(&g.hit_tab[i], x + i * tw, y - (int)(6 * s), tw - (int)(12 * s), (int)(42 * s));
            g.hit_tabs = i + 1;
        }
        y += (int)(46 * s);
    }

    /* The rows, scrolled to keep the selected one in view. */
    list_top = y;
    list_bottom = (int)g.ph - pad - (int)(84 * s);
    visible = (list_bottom - list_top) / (rowh + gap);
    if (visible < 1) visible = 1;
    if (m->selected < g.top) g.top = m->selected;
    if (m->selected >= g.top + visible) g.top = m->selected - visible + 1;
    if (g.top > m->row_count - visible) g.top = m->row_count - visible;
    if (g.top < 0) g.top = 0;
    memset(g.hit_shown, 0, sizeof g.hit_shown);
    for (i = g.top; i < m->row_count && i < g.top + visible; i++) {
        const D3D8MenuRow *r = &m->rows[i];
        int lit = i == m->selected, ry = y + (i - g.top) * (rowh + gap);
        int lx = x + (int)(18 * s), vx = x + w * 52 / 100;
        /* The arrows: two targets a mouse can hit, at the row's right end. */
        int aw = (int)(34 * s), ax = x + w - 2 * aw - (int)(8 * s);

        if (r->heading) {
            text(x, ry, w, rowh, r->label, f_small, GREEN, DT_LEFT | DT_SINGLELINE | DT_BOTTOM);
            continue;
        }
        set_rect(&g.hit_row[i], x, ry, w, rowh);
        if (r->arrows) {
            set_rect(&g.hit_left[i], ax, ry, aw, rowh);
            set_rect(&g.hit_right[i], ax + aw, ry, aw, rowh);
        } else {
            set_rect(&g.hit_left[i], 0, 0, 0, 0);
            set_rect(&g.hit_right[i], 0, 0, 0, 0);
        }
        g.hit_shown[i] = 1;
        fill(x, ry, w, rowh, lit ? ROW_LIT : ROW);
        frame(x, ry, w, rowh, 1, lit ? GREEN : GREEN_DIM);
        text(lx, ry, vx - lx, rowh, r->label, f_row, lit ? INK : INK_DIM,
             DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        text(vx, ry, (r->arrows ? ax : x + w - (int)(12 * s)) - vx, rowh,
             (lit && m->editing) ? "press a key, click or turn the wheel..." : r->value,
             f_value, lit ? GREEN : INK_DIM,
             DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        if (lit && r->arrows) {
            text(ax, ry, aw, rowh, "<", f_row, GREEN, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            text(ax + aw, ry, aw, rowh, ">", f_row, GREEN, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        }
    }
    /* Where in a longer list. */
    if (m->row_count > visible) {
        int track = visible * (rowh + gap) - gap;
        int bar = track * visible / m->row_count;
        int by = list_top + (track - bar) * g.top / (m->row_count - visible);
        fill(x + w + (int)(8 * s), by, (int)(3 * s) + 1, bar, GREEN_DIM);
    }

    /* Help for the selected row, then the keys. */
    text(x, list_bottom + (int)(10 * s), w, (int)(44 * s), m->help, f_small, INK_DIM,
         DT_LEFT | DT_WORDBREAK);
    text(x, (int)g.ph - pad - (int)(22 * s), w, (int)(22 * s), m->footer, f_small, INK,
         DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    GdiFlush();
    DeleteObject(f_title);
    DeleteObject(f_tab);
    DeleteObject(f_row);
    DeleteObject(f_value);
    DeleteObject(f_small);
    rhi_image_update(g.texture, 0, NULL, g.bits, g.pw * 4, 0);
}

void xbox_D3D8MenuShow(const D3D8Menu *menu)
{
    if (!g.lock_ready) {
        InitializeCriticalSection(&g.lock);
        g.lock_ready = 1;
    }
    EnterCriticalSection(&g.lock);
    if (!menu) {
        g.shown = 0;
    } else {
        if (!g.shown || memcmp(menu, &g.menu, sizeof *menu) != 0) {
            g.menu = *menu;
            g.dirty = 1;
        }
        g.shown = 1;
    }
    LeaveCriticalSection(&g.lock);
}

static int in_rect(const RECT *r, int x, int y)
{
    return x >= r->left && x < r->right && y >= r->top && y < r->bottom;
}

int xbox_D3D8MenuHit(int wx, int wy, int *index)
{
    int x, y, i, hit = D3D8_MENU_HIT_OUTSIDE;

    *index = -1;
    if (!g.lock_ready)
        return D3D8_MENU_HIT_NONE;
    EnterCriticalSection(&g.lock);
    if (!g.shown || !g.pw || !g.ph) {
        LeaveCriticalSection(&g.lock);
        return D3D8_MENU_HIT_NONE;
    }
    x = wx - g.x0;
    y = wy - g.y0;
    if (x >= 0 && y >= 0 && x < (int)g.pw && y < (int)g.ph) {
        hit = D3D8_MENU_HIT_PANEL;
        for (i = 0; i < g.hit_tabs; i++)
            if (in_rect(&g.hit_tab[i], x, y)) {
                hit = D3D8_MENU_HIT_TAB;
                *index = i;
            }
        for (i = 0; i < D3D8_MENU_ROWS && hit == D3D8_MENU_HIT_PANEL; i++) {
            if (!g.hit_shown[i] || !in_rect(&g.hit_row[i], x, y))
                continue;
            *index = i;
            hit = in_rect(&g.hit_left[i], x, y) ? D3D8_MENU_HIT_LEFT
                : in_rect(&g.hit_right[i], x, y) ? D3D8_MENU_HIT_RIGHT
                : D3D8_MENU_HIT_ROW;
        }
    }
    LeaveCriticalSection(&g.lock);
    return hit;
}

void d3d8_menu_draw(RhiView *target, unsigned w, unsigned h)
{
    RhiOutputState saved;
    RhiViewport vp;
    MenuConstants c;
    float blend_factor[4] = { 1, 1, 1, 1 };
    void *mapped;
    int x0, y0;

    if (!g.lock_ready || !target || !w || !h)
        return;
    EnterCriticalSection(&g.lock);
    if (!g.shown) {
        LeaveCriticalSection(&g.lock);
        return;
    }
    if (!menu_create() || !layout(w, h)) {
        LeaveCriticalSection(&g.lock);
        return;
    }
    if (g.dirty || memcmp(&g.menu, &g.drawn, sizeof g.drawn) != 0) {
        render(&g.menu);
        g.drawn = g.menu;
        g.dirty = 0;
    }
    /* Centred, on whole pixels, so each texel is one screen pixel. */
    x0 = (int)(w - g.pw) / 2;
    y0 = (int)(h - g.ph) / 2;
    g.x0 = x0;
    g.y0 = y0;
    LeaveCriticalSection(&g.lock);

    rhi_output_save(&saved);
    if (!(mapped = rhi_buffer_map(g.cb, RHI_MAP_WRITE_DISCARD)))
        goto restore;
    c.x = -1.0f + 2.0f * (float)x0 / (float)w;
    c.y =  1.0f - 2.0f * (float)y0 / (float)h;
    c.w =  2.0f * (float)g.pw / (float)w;
    c.h =  2.0f * (float)g.ph / (float)h;
    c.tex_w = (float)g.pw;
    c.tex_h = (float)g.ph;
    /* Opaque: even at 0.97 a title's bright menu text read through the
     * panel's rows. The game stays visible around it. */
    c.alpha = 1.0f;
    c.pad = 0.0f;
    memcpy(mapped, &c, sizeof c);
    rhi_buffer_unmap(g.cb);

    vp.x = vp.y = 0.0f;
    vp.width = (float)w;
    vp.height = (float)h;
    vp.min_depth = 0.0f;
    vp.max_depth = 1.0f;
    rhi_set_render_target(target, NULL);
    rhi_set_viewports(1, &vp);
    rhi_set_vertex_layout(NULL);
    rhi_set_topology(RHI_TOPOLOGY_TRIANGLE_STRIP);
    rhi_set_shader(RHI_STAGE_VERTEX, g.vs);
    rhi_set_shader(RHI_STAGE_PIXEL, g.ps);
    rhi_set_uniform_buffers(RHI_STAGE_VERTEX, 7, 1, &g.cb);
    rhi_set_uniform_buffers(RHI_STAGE_PIXEL, 7, 1, &g.cb);
    rhi_set_textures(8, 1, &g.srv);
    rhi_set_blend_state(g.blend, blend_factor, 0xFFFFFFFF);
    rhi_set_depth_state(g.depth, 0);
    rhi_set_raster_state(g.raster);
    rhi_draw(4, 0);
    {
        RhiView *none = NULL;
        rhi_set_textures(8, 1, &none);
    }
restore:
    rhi_output_restore(&saved);
}

void d3d8_menu_shutdown(void)
{
    release_panel();
    rhi_buffer_destroy(g.cb);            g.cb = NULL;
    rhi_shader_destroy(g.vs);            g.vs = NULL;
    rhi_shader_destroy(g.ps);            g.ps = NULL;
    rhi_blend_state_destroy(g.blend);    g.blend = NULL;
    rhi_depth_state_destroy(g.depth);    g.depth = NULL;
    rhi_raster_state_destroy(g.raster);  g.raster = NULL;
    if (g.dc) { DeleteDC(g.dc); g.dc = NULL; }
    g.tried = 0;
    g.top = 0;
    memset(&g.drawn, 0, sizeof g.drawn);
}
