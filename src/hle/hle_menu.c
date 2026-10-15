/*
 * hle_menu.c -- see hle_menu.h.
 *
 * Two threads touch this. The game window's thread hands over F1 and every
 * key pressed while the menu is open (hle_menu_key) and wheel notches
 * (hle_menu_wheel); the thread that presents runs everything else once a
 * frame (hle_menu_frame): it reads the pad, acts on the keys, applies and
 * saves settings, and hands d3d8_menu.c the panel to draw.
 *
 * Settings live where the launcher keeps them: the display settings in the
 * title's settings file (recomp_settings_*), the mouse and the key bindings in
 * the title's binding file (bind_load / bind_save, the launcher's own code).
 * What the runtime can change while the title runs it changes at once
 * through the setter that owns it; the rest is written for the next start.
 */
#include "hle_menu.h"
#include "d3d8_menu.h"
#include "d3d8_xbox.h"          /* xbox_D3D8SetVrr, xbox_D3D8SetAnisotropy */
#include "d3d8_display.h"       /* the running resolution and widescreen */
#include "recomp_config.h"
#include "input_bindings.h"
#include "input_mouse.h"
#include "recomp_pad.h"
#include "launcher_bindings.h"
#include "audio_output.h"          /* mute in the background */
#include "hle_xmv_play.h"          /* skip intro videos */
#include "../kernel/xbox_memory_layout.h"   /* the frame cap */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>              /* the field of view across the screen */

enum { TAB_DISPLAY, TAB_MOUSE, TAB_CONTROLS, TAB_GENERAL, TAB_COUNT };
static const char *const k_tab_names[TAB_COUNT] = { "Display", "Mouse", "Controls", "General" };

enum { D_RES, D_WIDE, D_FOV, D_ANISO, D_BRIGHT, D_CAP, D_FPS, D_FULL, D_VRR, D_INTERP, D_COUNT };

/* Field of view is there only for a title that can set it: the Display
 * page's rows, and which setting each one is. */
static int disp_rows(void)
{
    return recomp_fov_game() > 0.0 ? D_COUNT : D_COUNT - 1;
}

static int disp_kind(int row)
{
    return (recomp_fov_game() > 0.0 || row < D_FOV) ? row : row + 1;
}
/* The Mouse page's rows, in order, made when the menu opens (mouse_rows):
 * zoomed aim only for a title that aims by the mouse itself, a hold-or-toggle
 * row for each control the title reports a toggle for. */
enum { MR_SENS, MR_YSCALE, MR_INVERT, MR_ZOOM, MR_HOLD, MR_RESET };
enum { G_MUTE, G_INTRO, G_COUNT };
enum { C_RESET = BIND_CONTROLS, C_COUNT };          /* then a reset row */

/* The frame cap's stops, in xbox_Nv2aFlipGateSetMode's order, as the
 * settings file spells them. */
static const char *const k_caps[] = { "adaptive", "60", "30", "0" };
static const char *const k_cap_names[] = {
    "Adaptive (as the console)", "60 frames a second", "30 frames a second", "Uncapped"
};
static const int k_aniso[] = { 1, 2, 4, 8, 16 };

static struct {
    volatile LONG open;
    volatile LONG toggle;       /* F1, from the window thread */
    int  tab, sel[TAB_COUNT];

    RecompSettings set;         /* the settings file, as edited */
    int  set_dirty;
    int  run_scale, run_wide, run_interp;   /* what this run started with */
    int  run_skip_intro;
    int  run_known;

    int  mouse_port;            /* -1: no port binds the mouse */
    RecompMouseSettings mouse;
    int  mouse_dirty;
    int  mrow[12], mhold[12];   /* the Mouse page: each row's kind, and a hold
                                 * row's control (bind_control_names) */
    int  nmrow;

    BindConfig bind;            /* the binding file, as edited */

    int  editing, edit_armed;   /* Controls: waiting for a press */
    BindCapture capture;
    int  release_wait;          /* closed: the game waits for the keys to lift */

    CRITICAL_SECTION lock;
    volatile LONG lock_ready;
    unsigned keys[32];
    int  nkeys;
    int  wheel;

    WORD pad_prev;
    int  repeat_dir;
    ULONGLONG repeat_at;
} m;

static void ensure_lock(void)
{
    static volatile LONG once;

    if (InterlockedCompareExchange(&once, 1, 0) == 0) {
        InitializeCriticalSection(&m.lock);
        InterlockedExchange(&m.lock_ready, 1);
    }
    while (!m.lock_ready)
        Sleep(0);
}

int hle_menu_open(void)
{
    return m.open != 0;
}

int hle_menu_key(unsigned vk)
{
    ensure_lock();
    if (vk == VK_F1) {
        InterlockedExchange(&m.toggle, 1);
        return 1;
    }
    if (!m.open)
        return 0;
    EnterCriticalSection(&m.lock);
    if (m.nkeys < (int)(sizeof m.keys / sizeof m.keys[0]))
        m.keys[m.nkeys++] = vk;
    LeaveCriticalSection(&m.lock);
    return 1;
}

void hle_menu_wheel(int delta)
{
    ensure_lock();
    if (!m.open || !delta)
        return;
    EnterCriticalSection(&m.lock);
    m.wheel = delta;
    LeaveCriticalSection(&m.lock);
}

/* ---------------------------------------------------------------- files */

static void settings_load(void)
{
    char path[1024];

    recomp_settings_defaults(&m.set);
    if (recomp_settings_path(recomp_config_title_id(), path, sizeof path))
        recomp_settings_read(path, &m.set);
}

static void settings_save(void)
{
    char path[1024];

    if (!m.set_dirty)
        return;
    if (recomp_settings_path(recomp_config_title_id(), path, sizeof path) &&
        recomp_settings_write(path, &m.set)) {
        fprintf(stderr, "[MENU] settings written to %s\n", path);
        m.set_dirty = 0;
    }
}

static void bindings_load(void)
{
    char path[1024], preset[1024];
    int have = recomp_bindings_read_path(recomp_config_title_id(), path, sizeof path);

    if (!recomp_bindings_preset_path(preset, sizeof preset))
        preset[0] = '\0';
    bind_load_with_labels(have ? path : NULL, preset, &m.bind);
}

/* Write the binding file, the mouse as it is now included, so a rebind and
 * a sensitivity change in the same visit both land. */
static int bindings_save(void)
{
    char path[1024];

    if (m.mouse_port >= 0) {
        BindPort *bp = &m.bind.port[m.mouse_port];
        BindMouse *bm = &bp->mouse;
        int k;

        bm->sensitivity = m.mouse.sensitivity;
        bm->y_scale = m.mouse.y_scale;
        bm->invert_y = m.mouse.invert_y;
        bm->present = 1;
        bp->hold = 0;
        for (k = 0; k < BIND_CONTROLS; k++)
            if (recomp_bindings_get_hold((unsigned)m.mouse_port, bind_control_names[k]))
                bp->hold |= 1u << k;
    }
    if (!recomp_bindings_write_path(recomp_config_title_id(), path, sizeof path) ||
        !bind_save(path, &m.bind))
        return 0;
    fprintf(stderr, "[MENU] controls written to %s\n", path);
    m.mouse_dirty = 0;
    return 1;
}

/* The title's own preset, for the reset rows: beside the executable, or the
 * built-in mapping when it ships none. */
static void preset_load(BindConfig *c)
{
    char path[1024];

    bind_defaults(c);
    if (recomp_bindings_preset_path(path, sizeof path))
        bind_load(path, c);
}

/* ------------------------------------------------------------ open/close */

static void mouse_rows(void)
{
    int k;

    m.nmrow = 0;
    if (m.mouse_port < 0)
        return;
    m.mrow[m.nmrow++] = MR_SENS;
    m.mrow[m.nmrow++] = MR_YSCALE;
    m.mrow[m.nmrow++] = MR_INVERT;
    if (recomp_mouse_aims_directly())
        m.mrow[m.nmrow++] = MR_ZOOM;
    for (k = 0; k < BIND_CONTROLS && m.nmrow < 11; k++)
        if (recomp_bindings_hold_available(bind_control_names[k])) {
            m.mhold[m.nmrow] = k;
            m.mrow[m.nmrow++] = MR_HOLD;
        }
    m.mrow[m.nmrow++] = MR_RESET;
}

static void open_menu(void)
{
    const D3D8DisplayPolicy *p = d3d8_display_policy();
    int i;

    recomp_input_suspend(1);
    settings_load();
    if (!m.run_known) {
        m.run_known = 1;
        m.run_scale = (int)p->scale;
        m.run_wide = p->widescreen;
        m.run_interp = recomp_config_int("RECOMP_FRAME_INTERP", "frame_interp", 1);
        if (m.run_interp < 2)
            m.run_interp = 1;
        m.run_skip_intro = recomp_config_bool("RECOMP_SKIP_INTRO", "skip_intro", 0);
    }
    /* The live rows show what is live, which the environment may have set
     * differently from the file. */
    i = xbox_Nv2aFlipGateModeIndex();
    if (i >= 0)
        snprintf(m.set.frame_cap, sizeof m.set.frame_cap, "%s", k_caps[i]);
    m.set.fps_overlay = hle_d3d8_fps_overlay();
    m.set.fullscreen = hle_d3d8_fullscreen();
    m.set.anisotropy = (int)p->anisotropy;
    m.set.resolution_scale = (int)p->scale;
    m.set.brightness = xbox_D3D8Brightness();
    m.set.fov = recomp_fov();
    m.set.wide_fill = xbox_D3D8WideFill();
    m.set.mute_unfocused = recomp_audio_output_mute_unfocused();
    m.set_dirty = 0;

    bindings_load();
    m.mouse_port = recomp_bindings_mouse_port();
    if (m.mouse_port >= 0)
        recomp_bindings_get_mouse((unsigned)m.mouse_port, &m.mouse);
    m.mouse_dirty = 0;
    mouse_rows();
    m.editing = 0;
    EnterCriticalSection(&m.lock);
    m.nkeys = 0;
    m.wheel = 0;
    LeaveCriticalSection(&m.lock);
    InterlockedExchange(&m.open, 1);
    fprintf(stderr, "[MENU] open\n");
    fflush(stderr);
}

static void close_menu(void)
{
    settings_save();
    if (m.mouse_dirty)
        bindings_save();
    m.editing = 0;
    InterlockedExchange(&m.open, 0);
    m.release_wait = 1;
    xbox_D3D8MenuShow(NULL);
    fprintf(stderr, "[MENU] closed\n");
    fflush(stderr);
}

/* Nothing that opens, closes or answers the menu is still held: the game
 * gets its input back only then, or the Escape that closed the menu would
 * reach it as Start and pause it. */
static int all_released(WORD pad)
{
    static const int keys[] = { VK_ESCAPE, VK_F1, VK_RETURN, VK_SPACE, VK_BACK,
                                VK_LBUTTON, VK_RBUTTON, VK_MBUTTON };
    size_t i;

    for (i = 0; i < sizeof keys / sizeof keys[0]; i++)
        if (GetAsyncKeyState(keys[i]) & 0x8000)
            return 0;
    return (pad & (RECOMP_PAD_A | RECOMP_PAD_B | RECOMP_PAD_START | RECOMP_PAD_BACK)) == 0;
}

/* --------------------------------------------------------------- moving */

static int row_count(int tab)
{
    switch (tab) {
    case TAB_DISPLAY:  return disp_rows();
    case TAB_MOUSE:    return m.mouse_port >= 0 ? m.nmrow : 1;
    case TAB_GENERAL:  return G_COUNT;
    default:           return C_COUNT;
    }
}

static void move(int dy)
{
    int n = row_count(m.tab), s = m.sel[m.tab] + dy;

    if (s < 0) s = n - 1;
    if (s >= n) s = 0;
    m.sel[m.tab] = s;
}

static void next_tab(int d)
{
    m.tab = (m.tab + d + TAB_COUNT) % TAB_COUNT;
    if (m.sel[m.tab] >= row_count(m.tab))
        m.sel[m.tab] = 0;
}

static void set_mouse(void)
{
    if (m.mouse_port < 0)
        return;
    recomp_bindings_set_mouse((unsigned)m.mouse_port, &m.mouse);
    m.mouse_dirty = 1;
}

/* Left and right on a row (dir -1 or 1), or Enter on a switch (dir 1). */
static void change(int dir)
{
    int row = m.sel[m.tab], i;

    if (m.tab == TAB_DISPLAY) {
        switch (disp_kind(row)) {
        case D_RES:
            m.set.resolution_scale += dir;
            if (m.set.resolution_scale < 1) m.set.resolution_scale = 1;
            if (m.set.resolution_scale > 8) m.set.resolution_scale = 8;
            xbox_D3D8RequestScale((UINT)m.set.resolution_scale);   /* at the next present */
            break;
        case D_WIDE:
            if (xbox_D3D8TitleAnyAspect()) {
                /* Off, 16:9, fill the window: the last two are the same
                 * start-up mode, so moving between them is live. */
                int at = !m.set.widescreen ? 0 : (m.set.wide_fill ? 2 : 1);

                at = (at + dir + 3) % 3;
                m.set.widescreen = at > 0;
                m.set.wide_fill = at == 2;
                xbox_D3D8SetWideFill(m.set.wide_fill ? TRUE : FALSE);
            } else {
                m.set.widescreen = !m.set.widescreen;
            }
            break;
        case D_FOV: {
            double game = recomp_fov_game();
            double f = (m.set.fov > 0.0 ? m.set.fov : game) + dir * 5.0;

            if (f < RECOMP_FOV_MIN) f = RECOMP_FOV_MIN;
            if (f > RECOMP_FOV_MAX) f = RECOMP_FOV_MAX;
            m.set.fov = (f > game - 0.5 && f < game + 0.5) ? 0.0 : f;
            recomp_fov_set(m.set.fov);
            break;
        }
        case D_ANISO:
            for (i = 0; i < 5 && k_aniso[i] < m.set.anisotropy; i++)
                ;
            i += dir;
            if (i < 0) i = 0;
            if (i > 4) i = 4;
            m.set.anisotropy = k_aniso[i];
            xbox_D3D8SetAnisotropy((UINT)m.set.anisotropy);
            break;
        case D_BRIGHT:
            m.set.brightness += dir * 0.05;
            if (m.set.brightness < 0.5) m.set.brightness = 0.5;
            if (m.set.brightness > 2.0) m.set.brightness = 2.0;
            xbox_D3D8SetBrightness((float)m.set.brightness);
            break;
        case D_CAP:
            for (i = 0; i < 4 && strcmp(k_caps[i], m.set.frame_cap) != 0; i++)
                ;
            i = (i >= 4 ? 0 : i + dir + 4) % 4;
            snprintf(m.set.frame_cap, sizeof m.set.frame_cap, "%s", k_caps[i]);
            xbox_Nv2aFlipGateSetMode(i);
            break;
        case D_FPS:
            m.set.fps_overlay = !m.set.fps_overlay;
            hle_d3d8_set_fps_overlay(m.set.fps_overlay);
            break;
        case D_FULL:
            m.set.fullscreen = !m.set.fullscreen;
            hle_d3d8_request_fullscreen(m.set.fullscreen);
            break;
        case D_VRR:
            m.set.vrr = !m.set.vrr;
            xbox_D3D8SetVrr(m.set.vrr ? TRUE : FALSE);
            break;
        case D_INTERP:
            /* Off, 2x, 3x, 4x: one for each of 120, 180 and 240 Hz. */
            i = m.set.frame_interp < 1 ? 1 : m.set.frame_interp > 4 ? 4 : m.set.frame_interp;
            m.set.frame_interp = (i - 1 + dir + 4) % 4 + 1;
            break;
        }
        m.set_dirty = 1;
        return;
    }
    if (m.tab == TAB_MOUSE && m.mouse_port >= 0) {
        switch (m.mrow[row]) {
        case MR_SENS: {
            /* Tenths up to 3, halves above, where a tenth is not a
             * difference anyone can feel; 3 itself steps down by a tenth. */
            double s = m.mouse.sensitivity;
            double step = (s > 3.0 + 1e-6 || (dir > 0 && s >= 3.0 - 1e-6)) ? 0.5 : 0.1;
            m.mouse.sensitivity += dir * step;
            if (m.mouse.sensitivity < 0.1) m.mouse.sensitivity = 0.1;
            if (m.mouse.sensitivity > 20.0) m.mouse.sensitivity = 20.0;
            set_mouse();
            break;
        }
        case MR_YSCALE:
            m.mouse.y_scale += dir * 0.1;
            if (m.mouse.y_scale < 0.2) m.mouse.y_scale = 0.2;
            if (m.mouse.y_scale > 5.0) m.mouse.y_scale = 5.0;
            set_mouse();
            break;
        case MR_INVERT:
            m.mouse.invert_y = !m.mouse.invert_y;
            set_mouse();
            break;
        case MR_ZOOM:
            m.mouse.zoom_match = !m.mouse.zoom_match;
            set_mouse();
            break;
        case MR_HOLD: {
            const char *name = bind_control_names[m.mhold[row]];
            unsigned port = (unsigned)m.mouse_port;

            recomp_bindings_set_hold(port, name, !recomp_bindings_get_hold(port, name));
            m.mouse_dirty = 1;
            break;
        }
        }
        return;
    }
    if (m.tab == TAB_GENERAL) {
        switch (row) {
        case G_MUTE:
            m.set.mute_unfocused = !m.set.mute_unfocused;
            recomp_audio_output_set_mute_unfocused(m.set.mute_unfocused);
            break;
        case G_INTRO:
            m.set.skip_intro = !m.set.skip_intro;
            break;
        }
        m.set_dirty = 1;
    }
}

static void accept(void)
{
    int row = m.sel[m.tab];

    if (m.tab == TAB_DISPLAY) {
        int kind = disp_kind(row);

        if (kind == D_WIDE || kind == D_FPS || kind == D_FULL || kind == D_VRR || kind == D_INTERP)
            change(1);
        return;
    }
    if (m.tab == TAB_MOUSE) {
        int kind = m.mouse_port >= 0 ? m.mrow[row] : -1;

        if (kind == MR_INVERT || kind == MR_ZOOM || kind == MR_HOLD) {
            change(1);
        } else if (kind == MR_RESET) {
            BindConfig preset;
            const BindPort *pp;
            int r;

            preset_load(&preset);
            pp = &preset.port[m.mouse_port];
            m.mouse.sensitivity = pp->mouse.sensitivity;
            m.mouse.y_scale = pp->mouse.y_scale;
            m.mouse.invert_y = pp->mouse.invert_y;
            m.mouse.zoom_match = pp->mouse.zoom_match;
            for (r = 0; r < m.nmrow; r++)
                if (m.mrow[r] == MR_HOLD)
                    recomp_bindings_set_hold((unsigned)m.mouse_port,
                                             bind_control_names[m.mhold[r]],
                                             (pp->hold >> m.mhold[r]) & 1u);
            set_mouse();
        }
        return;
    }
    if (m.tab == TAB_GENERAL) {
        change(1);
        return;
    }
    if (row < BIND_CONTROLS) {
        m.editing = 1;
        m.edit_armed = 0;
    } else if (row == C_RESET) {
        BindConfig preset;

        preset_load(&preset);
        m.bind.port[0] = preset.port[0];
        memcpy(m.bind.action, preset.action, sizeof m.bind.action);
        if (bindings_save())
            recomp_bindings_reload();
    }
}

/* Controls: wait for everything to be let go, then take the next press. */
static void capture_tick(int escape)
{
    const char *got;
    int k = m.sel[TAB_CONTROLS];
    BindCapture none;

    if (escape) {
        m.editing = 0;
        return;
    }
    if (!m.edit_armed) {
        memset(&none, 0, sizeof none);
        if (bind_capture_poll(&none, -1) == NULL && !(GetAsyncKeyState(VK_ESCAPE) & 0x8000)) {
            m.edit_armed = 1;
            bind_capture_begin(&m.capture);
        }
        return;
    }
    got = bind_capture_poll(&m.capture, k);
    m.capture.wheel = 0;
    if (!got)
        return;
    snprintf(m.bind.port[0].key_src[k], BIND_SOURCE_LEN, "%s", got);
    bind_drop_hidden_keys(&m.bind.port[0], k);
    m.editing = 0;
    if (bindings_save())
        recomp_bindings_reload();
}

/* ---------------------------------------------------------------- drawing */

static void row(D3D8Menu *menu, const char *label, const char *value, int arrows)
{
    D3D8MenuRow *r;

    if (menu->row_count >= D3D8_MENU_ROWS)
        return;
    r = &menu->rows[menu->row_count++];
    snprintf(r->label, sizeof r->label, "%s", label);
    snprintf(r->value, sizeof r->value, "%s", value);
    r->arrows = arrows;
}

static const char *on_off(int v) { return v ? "On" : "Off"; }

/* A setting the title reads only when it starts: say when it will apply. */
static void next_start(char *out, size_t n, const char *value, int changed)
{
    snprintf(out, n, "%s%s", value, changed ? "   (from the next start)" : "");
}

static void build(D3D8Menu *menu)
{
    char v[96], tmp[64];
    int i, sel;

    memset(menu, 0, sizeof *menu);
    snprintf(menu->title, sizeof menu->title, "Settings");
    menu->tab_count = TAB_COUNT;
    menu->tab = m.tab;
    for (i = 0; i < TAB_COUNT; i++)
        snprintf(menu->tabs[i], sizeof menu->tabs[i], "%s", k_tab_names[i]);
    sel = m.sel[m.tab];
    menu->selected = sel;
    menu->editing = m.editing;

    if (m.tab == TAB_DISPLAY) {
        unsigned gw = 640, gh = 480;
        hle_d3d8_guest_size(&gw, &gh);
        snprintf(v, sizeof v, "%dx  (%ux%u)", m.set.resolution_scale,
                 gw * (unsigned)m.set.resolution_scale, gh * (unsigned)m.set.resolution_scale);
        row(menu, "Resolution", v, 1);
        next_start(v, sizeof v, !m.set.widescreen ? "Off"
                                : !xbox_D3D8TitleAnyAspect() ? "On"
                                : m.set.wide_fill ? "Fill the window" : "16:9",
                   m.set.widescreen != m.run_wide);
        row(menu, "Widescreen", v, 1);
        if (recomp_fov_game() > 0.0) {
            /* Vertical, as the setting is, and across the screen as it is
             * now shaped, which is the number most PC games show. */
            double vfov = m.set.fov > 0.0 ? m.set.fov : recomp_fov_game();
            UINT fw = 0, fh = 0;
            double aspect = xbox_D3D8FillShape(&fw, &fh) && fh ? (double)fw / (double)fh
                          : m.run_wide ? 16.0 / 9.0 : 4.0 / 3.0;
            double across = 2.0 * atan(tan(vfov * 3.14159265358979 / 360.0) * aspect)
                            * 180.0 / 3.14159265358979;

            snprintf(v, sizeof v, "%.0f\xC2\xB0  (%.0f\xC2\xB0 across)%s", vfov, across,
                     m.set.fov > 0.0 ? "" : "   the game's own");
            row(menu, "Field of view", v, 1);
        }
        snprintf(v, sizeof v, m.set.anisotropy > 1 ? "%dx" : "Off", m.set.anisotropy);
        row(menu, "Texture sharpness", v, 1);
        snprintf(v, sizeof v, "%d%%", (int)(m.set.brightness * 100.0 + 0.5));
        row(menu, "Brightness", v, 1);
        for (i = 0; i < 4 && strcmp(k_caps[i], m.set.frame_cap) != 0; i++)
            ;
        row(menu, "Frame rate", i < 4 ? k_cap_names[i] : m.set.frame_cap, 1);
        row(menu, "Show frame rate", on_off(m.set.fps_overlay), 1);
        row(menu, "Fullscreen", on_off(m.set.fullscreen), 1);
        row(menu, "G-SYNC / FreeSync", on_off(m.set.vrr), 1);
        if (m.set.frame_interp >= 2)
            snprintf(tmp, sizeof tmp, "%dx  (%d fps)", m.set.frame_interp,
                     60 * m.set.frame_interp);
        else
            snprintf(tmp, sizeof tmp, "Off");
        next_start(v, sizeof v, tmp,
                   (m.set.frame_interp >= 2 ? m.set.frame_interp : 1) != m.run_interp);
        row(menu, "Frame interpolation", v, 1);
        {
            static const char *const help[D_COUNT] = {
                "Render larger than the console, then filter back down: sharper edges "
                "and detail, at a cost in speed. Changes at once.",
                "The game's own 16:9 mode, where it has one: the view, HUD and menus "
                "widen. Fill the window draws any shape, 21:9 included. On or off is "
                "read when the game starts.",
                "How much of the world the view takes in, top to bottom. Wider shows "
                "more at the edges; the game's own culling follows.",
                "Sharpen textures seen at a glancing angle: floors, walls, roads.",
                "Above 100% lifts the shadows, below deepens them. 100% is the game "
                "exactly as drawn.",
                "Adaptive paces the game the way the console did (nothing to do with "
                "G-SYNC or FreeSync). F10 also changes it.",
                "The frame-rate counter in the corner. F9 also toggles it.",
                "Borderless, on this monitor. Alt+Enter also switches.",
                "For a monitor with G-SYNC or FreeSync, in fullscreen: each frame is shown "
                "the moment it is ready, so 60 is an even 60. Off for other monitors.",
                "Draws extra frames between each two the game makes: 2x for a 120 Hz "
                "display, 3x for 180 Hz, 4x for 240 Hz. Adds a little input delay. Set "
                "at start.",
            };
            snprintf(menu->help, sizeof menu->help, "%s", help[disp_kind(sel)]);
        }
    } else if (m.tab == TAB_MOUSE) {
        if (m.mouse_port < 0) {
            row(menu, "No mouse", "This game's controls do not use the mouse", 0);
            snprintf(menu->help, sizeof menu->help,
                     "Bind a mouse button or movement on the Controls page first.");
        } else {
            /* The title turns its camera by the mouse itself: no stick in
             * between, and the game's own Inverse Look is the pad's. */
            int direct = recomp_mouse_aims_directly();

            for (i = 0; i < m.nmrow; i++) {
                switch (m.mrow[i]) {
                case MR_SENS:
                    snprintf(v, sizeof v, "%.1f", m.mouse.sensitivity);
                    row(menu, "Sensitivity", v, 1);
                    break;
                case MR_YSCALE:
                    snprintf(v, sizeof v, "%.1f x side to side", m.mouse.y_scale);
                    row(menu, "Up and down speed", v, 1);
                    break;
                case MR_INVERT:
                    row(menu, "Invert up and down", on_off(m.mouse.invert_y), 1);
                    break;
                case MR_ZOOM:
                    row(menu, "Zoomed aim", m.mouse.zoom_match ? "Slower, matched to the zoom"
                                                               : "Same as unzoomed", 1);
                    break;
                case MR_HOLD:
                    snprintf(tmp, sizeof tmp, "%s button", bind_control_title(&m.bind, m.mhold[i]));
                    row(menu, tmp, recomp_bindings_get_hold((unsigned)m.mouse_port,
                                                            bind_control_names[m.mhold[i]])
                                       ? "Hold" : "Press to toggle", 1);
                    break;
                default:
                    row(menu, "Back to this game's defaults", "", 0);
                    break;
                }
            }
            switch (m.mrow[sel]) {
            case MR_SENS:
                snprintf(menu->help, sizeof menu->help, "%s", direct
                         ? "How far the view turns for how far the mouse moves: the same angle "
                           "for the same distance, every time. Changes as you set it."
                         : "How far the view turns for how far the mouse moves. Changes as "
                           "you set it.");
                break;
            case MR_YSCALE:
                snprintf(menu->help, sizeof menu->help, "%s", direct
                         ? "Looking up and down against turning. 1.0 is the same speed both ways."
                         : "Looking up and down against turning: console games look up and "
                           "down slower than they turn.");
                break;
            case MR_INVERT:
                snprintf(menu->help, sizeof menu->help, "%s", direct
                         ? "Pushing the mouse away looks down. The game's own Inverse Look is "
                           "for the controller only."
                         : "Pushing the mouse away looks down. If the game's own Inverse Look "
                           "is on, it inverts again.");
                break;
            case MR_ZOOM:
                snprintf(menu->help, sizeof menu->help,
                         "Zoomed in, the same mouse movement moves the same distance across "
                         "the screen as it does unzoomed: the view turns less, by the zoom.");
                break;
            case MR_HOLD:
                snprintf(menu->help, sizeof menu->help,
                         "Hold: %s lasts while its key or mouse button is held, as on PC. "
                         "Press to toggle: one press on, the next off, as on the console. "
                         "A controller always toggles.", bind_control_title(&m.bind, m.mhold[sel]));
                break;
            default:
                snprintf(menu->help, sizeof menu->help,
                         "Every setting on this page as this game ships it.");
                break;
            }
        }
    } else if (m.tab == TAB_GENERAL) {
        row(menu, "Mute in the background", on_off(m.set.mute_unfocused), 1);
        next_start(v, sizeof v, on_off(m.set.skip_intro), m.set.skip_intro != m.run_skip_intro);
        row(menu, "Skip intro videos", v, 1);
        {
            static const char *const help[G_COUNT] = {
                "Silence the game while another window has the focus. F12 saves a "
                "screenshot to your Pictures folder.",
                "Skip the logo videos before the title screen. Read when the game starts.",
            };
            snprintf(menu->help, sizeof menu->help, "%s", help[sel]);
        }
    } else {
        for (i = 0; i < BIND_CONTROLS; i++) {
            int hidden = bind_hidden_count(&m.bind.port[0], i);
            if (hidden)
                snprintf(v, sizeof v, "%s  +%d", bind_source_label(m.bind.port[0].key_src[i]),
                         hidden);
            else
                snprintf(v, sizeof v, "%s", bind_source_label(m.bind.port[0].key_src[i]));
            row(menu, bind_control_title(&m.bind, i), v, 0);
        }
        row(menu, "Reset controls to this game's defaults", "", 0);
        if (m.editing)
            snprintf(menu->help, sizeof menu->help,
                     "Press the key or mouse button for %s, or turn the wheel; for a "
                     "direction, move the mouse that way. Escape cancels.",
                     bind_control_title(&m.bind, sel));
        else if (sel < BIND_CONTROLS)
            snprintf(menu->help, sizeof menu->help,
                     "Enter to choose a new key or mouse button for %s (%s on a "
                     "controller). Saved at once.",
                     bind_control_title(&m.bind, sel), bind_control_labels[sel]);
        else
            snprintf(menu->help, sizeof menu->help,
                     "Every key and mouse binding back to this game's own layout.");
    }
    snprintf(menu->footer, sizeof menu->footer,
             "Arrows choose and change    Enter select    Tab next page    Esc or F1 close"
             "    (pad: D-pad, A, LB/RB, B)");
}

/* ----------------------------------------------------------------- frame */

static void act(unsigned vk)
{
    switch (vk) {
    case VK_UP:     move(-1); break;
    case VK_DOWN:   move(1); break;
    case VK_LEFT:   change(-1); break;
    case VK_RIGHT:  change(1); break;
    case VK_RETURN:
    case VK_SPACE:  accept(); break;
    case VK_TAB:    next_tab((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1); break;
    case VK_PRIOR:  next_tab(-1); break;
    case VK_NEXT:   next_tab(1); break;
    case VK_ESCAPE: close_menu(); break;
    default: break;
    }
}

void hle_menu_frame(void)
{
    RecompPadState pad;
    WORD btn = 0, pressed, dirs;
    unsigned keys[32];
    int nkeys, wheel, i, escape = 0;
    ULONGLONG now = GetTickCount64();

    ensure_lock();
    memset(&pad, 0, sizeof pad);
    if (recomp_pad_read(0, &pad)) {
        btn = pad.buttons;
        /* The left stick steers the menu like the D-pad. */
        if (pad.ly >  20000) btn |= RECOMP_PAD_DPAD_UP;
        if (pad.ly < -20000) btn |= RECOMP_PAD_DPAD_DOWN;
        if (pad.lx < -20000) btn |= RECOMP_PAD_DPAD_LEFT;
        if (pad.lx >  20000) btn |= RECOMP_PAD_DPAD_RIGHT;
    }
    pressed = (WORD)(btn & ~m.pad_prev);

    if (m.release_wait && all_released(btn)) {
        m.release_wait = 0;
        recomp_input_suspend(0);
    }

    /* Back and Start together: the pad's way in, and out. */
    if ((btn & RECOMP_PAD_BACK) && (btn & RECOMP_PAD_START) &&
        (pressed & (RECOMP_PAD_BACK | RECOMP_PAD_START)))
        InterlockedExchange(&m.toggle, 1);
    if (InterlockedExchange(&m.toggle, 0)) {
        if (m.open) {
            if (!m.editing)
                close_menu();
        } else if (!m.release_wait) {
            open_menu();
            pressed = 0;                /* the press that opened it is spent */
        }
    }
    if (!m.open) {
        m.pad_prev = btn;
        return;
    }

    EnterCriticalSection(&m.lock);
    nkeys = m.nkeys;
    memcpy(keys, m.keys, sizeof keys);
    m.nkeys = 0;
    wheel = m.wheel;
    m.wheel = 0;
    LeaveCriticalSection(&m.lock);

    if (m.editing) {
        for (i = 0; i < nkeys; i++)
            if (keys[i] == VK_ESCAPE)
                escape = 1;
        if (pressed & RECOMP_PAD_B)
            escape = 1;
        if (wheel && m.edit_armed)
            bind_capture_wheel(&m.capture, wheel);
        capture_tick(escape);
    } else {
        for (i = 0; i < nkeys && m.open; i++)
            act(keys[i]);
        if (m.open) {
            /* The pad: A, B, the shoulders, and directions that repeat
             * while held. */
            dirs = (WORD)(btn & (RECOMP_PAD_DPAD_UP | RECOMP_PAD_DPAD_DOWN |
                                 RECOMP_PAD_DPAD_LEFT | RECOMP_PAD_DPAD_RIGHT));
            if (pressed & RECOMP_PAD_A)              accept();
            if (pressed & RECOMP_PAD_LEFT_SHOULDER)  next_tab(-1);
            if (pressed & RECOMP_PAD_RIGHT_SHOULDER) next_tab(1);
            if (dirs && ((pressed & dirs) || now >= m.repeat_at)) {
                m.repeat_at = now + ((pressed & dirs) ? 400 : 110);
                if (dirs & RECOMP_PAD_DPAD_UP)    move(-1);
                if (dirs & RECOMP_PAD_DPAD_DOWN)  move(1);
                if (dirs & RECOMP_PAD_DPAD_LEFT)  change(-1);
                if (dirs & RECOMP_PAD_DPAD_RIGHT) change(1);
            }
            if (pressed & (RECOMP_PAD_B | RECOMP_PAD_START))
                close_menu();
        }
    }
    m.pad_prev = btn;

    if (m.open) {
        D3D8Menu menu;
        build(&menu);
        xbox_D3D8MenuShow(&menu);
    }
}
