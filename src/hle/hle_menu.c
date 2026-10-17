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
#include "hle_prompts.h"           /* button prompts */
#include "hle_netplay.h"           /* co-op: the Co-op page, and what a session fixes */
#include "recomp_net_addr.h"        /* this PC's addresses, to give out */
#include "recomp_invite.h"          /* invite codes */
#include "../kernel/xbox_memory_layout.h"   /* the frame cap */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>              /* the field of view across the screen */

/* Co-op last, and only for a title that starts sessions (hle_net_supported). */
enum { TAB_DISPLAY, TAB_MOUSE, TAB_CONTROLS, TAB_GENERAL, TAB_COOP, TAB_COUNT };
static const char *const k_tab_names[TAB_COUNT] = {
    "Display", "Mouse", "Controls", "General", "Co-op"
};

static int tabs_shown(void)
{
    return hle_net_supported() ? TAB_COUNT : TAB_COOP;
}

enum { D_RES, D_WIDE, D_FOV, D_WFOV, D_ANISO, D_FXAA, D_BRIGHT, D_CAP, D_FPS, D_FULL, D_VRR,
       D_INTERP, D_COUNT };

/* The fields of view are there only for a title that can set them: the
 * Display page's rows, and which setting each one is. */
static int disp_shown(int kind)
{
    return kind == D_FOV ? recomp_fov_game() > 0.0
         : kind == D_WFOV ? recomp_weapon_fov_game() > 0.0 : 1;
}

static int disp_rows(void)
{
    int k, n = 0;

    for (k = 0; k < D_COUNT; k++)
        n += disp_shown(k);
    return n;
}

static int disp_kind(int row)
{
    int k;

    for (k = 0; k < D_COUNT; k++)
        if (disp_shown(k) && row-- == 0)
            return k;
    return D_COUNT - 1;
}
/* The Mouse page's rows, in order, made when the menu opens (mouse_rows):
 * zoomed aim only for a title that aims by the mouse itself, a hold-or-toggle
 * row for each control the title reports a toggle for. */
enum { MR_SENS, MR_YSCALE, MR_INVERT, MR_ZOOM, MR_HOLD, MR_RESET };
enum { G_VOLUME, G_MUTE, G_PAUSE, G_KBALONE, G_PROMPTS, G_INTRO, G_COUNT };
enum { C_RESET = BIND_CONTROLS, C_COUNT };          /* then a reset row */
/* The Co-op page's rows, by what they are: hosting, joining, or in a
 * session (coop_rows). */
enum { CR_MODE, CR_CODE, CR_PUBLIC, CR_LAN, CR_ADDR, CR_PORT, CR_GO, CR_WITH, CR_DELAY,
       CR_LEAVE };

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
    /* The mouse, from the window thread: moves and clicks over the panel, in
     * the window's pixels, in order. */
    struct { int kind, x, y; } ptr[16];
    int  nptr;

    WORD pad_prev;
    int  repeat_dir;
    ULONGLONG repeat_at;

    /* The Co-op page. The address joined is the settings file's
     * (online_address); the port is typed as text and kept there too. */
    unsigned chars[32];         /* typed, from the window thread */
    int  nchars;
    int  coop_join;             /* 0 host, 1 join */
    char coop_port[8];
    char coop_lan[128];         /* this PC's addresses on its networks */
    char coop_code[24];         /* hosting: the invite code, made as the page first opens */
    int  copied_kind;           /* the row just copied, for a moment */
    ULONGLONG copied_at;
    ULONGLONG confirm_at;       /* Enter once on Host / Join / Leave: once more to restart */
    char coop_error[96];
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

int hle_menu_char(unsigned ch)
{
    ensure_lock();
    if (!m.open || m.tab != TAB_COOP)
        return 0;
    EnterCriticalSection(&m.lock);
    if (m.nchars < (int)(sizeof m.chars / sizeof m.chars[0]))
        m.chars[m.nchars++] = ch;
    LeaveCriticalSection(&m.lock);
    return 1;
}

void hle_menu_wheel(int delta)
{
    ensure_lock();
    if (!m.open || !delta)
        return;
    EnterCriticalSection(&m.lock);
    m.wheel += delta;
    LeaveCriticalSection(&m.lock);
}

void hle_menu_mouse(int kind, int x, int y)
{
    ensure_lock();
    if (!m.open)
        return;
    EnterCriticalSection(&m.lock);
    /* Moves in a row fold into the last; a click is never dropped for one. */
    if (kind == HLE_MENU_MOUSE_MOVE && m.nptr &&
        m.ptr[m.nptr - 1].kind == HLE_MENU_MOUSE_MOVE)
        m.nptr--;
    if (m.nptr < (int)(sizeof m.ptr / sizeof m.ptr[0])) {
        m.ptr[m.nptr].kind = kind;
        m.ptr[m.nptr].x = x;
        m.ptr[m.nptr].y = y;
        m.nptr++;
    }
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
    m.set.fxaa = xbox_D3D8Fxaa() ? 1 : 0;
    m.set.fov = recomp_fov();
    m.set.weapon_fov = recomp_weapon_fov();
    m.set.wide_fill = xbox_D3D8WideFill();
    m.set.mute_unfocused = recomp_audio_output_mute_unfocused();
    m.set.pause_unfocused = recomp_bindings_pause_unfocused();
    m.set.keyboard_alone = recomp_bindings_keyboard_alone();
    m.set.prompts = recomp_prompts_mode();
    m.set.volume = recomp_audio_output_volume();
    m.set_dirty = 0;

    if (hle_net_supported()) {
        snprintf(m.coop_port, sizeof m.coop_port, "%d",
                 m.set.online_port > 0 ? m.set.online_port : 47800);
        recomp_net_local_addrs(m.coop_lan, sizeof m.coop_lan);
        if (!m.coop_code[0]) {
            const char *e = getenv("RECOMP_NET_SIGNAL");     /* 0: no relay, no code */
            if (!(e && strcmp(e, "0") == 0))
                recomp_invite_new(m.coop_code, sizeof m.coop_code);
        }
        m.confirm_at = 0;
        m.coop_error[0] = '\0';
    }
    if (m.tab >= tabs_shown())
        m.tab = 0;

    bindings_load();
    m.mouse_port = recomp_bindings_mouse_port();
    if (m.mouse_port >= 0)
        recomp_bindings_get_mouse((unsigned)m.mouse_port, &m.mouse);
    m.mouse_dirty = 0;
    mouse_rows();
    m.editing = 0;
    EnterCriticalSection(&m.lock);
    m.nkeys = 0;
    m.nchars = 0;
    m.wheel = 0;
    m.nptr = 0;
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

/* --------------------------------------------------------------- co-op */

/* The Co-op page's rows: in a session, whom with and a way out; else host
 * (the addresses to give out) or join (the address to type), the port, and
 * the button that starts the game again into co-op. */
static int coop_rows(int *kind)
{
    int n = 0;

    if (hle_net_active()) {
        kind[n++] = CR_WITH;
        kind[n++] = CR_DELAY;
        kind[n++] = CR_LEAVE;
        return n;
    }
    kind[n++] = CR_MODE;
    if (!m.coop_join) {
        if (m.coop_code[0])
            kind[n++] = CR_CODE;
        kind[n++] = CR_PUBLIC;
        kind[n++] = CR_LAN;
    } else {
        kind[n++] = CR_ADDR;
    }
    kind[n++] = CR_PORT;
    kind[n++] = CR_GO;
    return n;
}

static int coop_kind(int row)
{
    int kind[8], n = coop_rows(kind);

    return row >= 0 && row < n ? kind[row] : -1;
}

static int coop_port(void)
{
    int port = atoi(m.coop_port);

    return port >= 1 && port <= 65535 ? port : 0;
}

/* The first of this PC's addresses on its networks, with the port. */
static void coop_lan_first(char *out, size_t n)
{
    const char *comma = strchr(m.coop_lan, ',');
    size_t len = comma ? (size_t)(comma - m.coop_lan) : strlen(m.coop_lan);

    if (!len) {
        out[0] = '\0';
        return;
    }
    snprintf(out, n, "%.*s:%d", (int)len, m.coop_lan, coop_port() ? coop_port() : 47800);
}

/* Host, join or leave: the game starts again (both must start together).
 * Enter once arms it, again within a few seconds does it -- anything not
 * saved is lost. */
static void coop_go(int kind)
{
    char spec[200];
    ULONGLONG now = GetTickCount64();

    m.coop_error[0] = '\0';
    if (kind == CR_GO && !coop_port()) {
        snprintf(m.coop_error, sizeof m.coop_error, "The port is a number from 1 to 65535.");
        return;
    }
    if (kind == CR_GO && m.coop_join && !m.set.online_address[0]) {
        snprintf(m.coop_error, sizeof m.coop_error,
                 "Type the host's invite code or address first.");
        return;
    }
    if (!m.confirm_at || now - m.confirm_at > 4000) {
        m.confirm_at = now;
        return;
    }
    m.confirm_at = 0;
    if (kind == CR_GO) {
        m.set.online_port = coop_port();
        m.set_dirty = 1;
        char code[24];
        if (m.coop_join && recomp_invite_parse(m.set.online_address, code, sizeof code))
            snprintf(spec, sizeof spec, "invite:%s", code);
        else if (m.coop_join)
            snprintf(spec, sizeof spec, "join:%s:%d", m.set.online_address, coop_port());
        else if (m.coop_code[0])
            snprintf(spec, sizeof spec, "host:%d:%s", coop_port(), m.coop_code);
        else
            snprintf(spec, sizeof spec, "host:%d", coop_port());
    }
    settings_save();
    if (!hle_net_restart(kind == CR_GO ? spec : NULL))
        snprintf(m.coop_error, sizeof m.coop_error, "The game could not be started again.");
}

/* A character typed on the Co-op page: into the address or the port. */
static void coop_type(unsigned ch)
{
    int kind = coop_kind(m.sel[TAB_COOP]);
    char *text = kind == CR_ADDR ? m.set.online_address : kind == CR_PORT ? m.coop_port : NULL;
    size_t cap = kind == CR_ADDR ? sizeof m.set.online_address : sizeof m.coop_port, n;

    if (!text)
        return;
    n = strlen(text);
    if (ch == 8) {
        if (n)
            text[n - 1] = '\0';
    } else if (ch == 22) {                                  /* Ctrl+V */
        if (OpenClipboard(NULL)) {
            HANDLE h = GetClipboardData(CF_TEXT);
            const char *t = h ? (const char *)GlobalLock(h) : NULL;

            for (; t && *t; t++)
                if (*t > ' ' && *t < 0x7F && (kind != CR_PORT || (*t >= '0' && *t <= '9')) &&
                    n + 1 < cap) {
                    text[n++] = *t;
                    text[n] = '\0';
                }
            if (h)
                GlobalUnlock(h);
            CloseClipboard();
        }
    } else if (ch > ' ' && ch < 0x7F && (kind != CR_PORT || (ch >= '0' && ch <= '9')) &&
               n + 1 < cap) {
        text[n] = (char)ch;
        text[n + 1] = '\0';
    } else {
        return;
    }
    m.confirm_at = 0;
    m.coop_error[0] = '\0';
    if (kind == CR_PORT && coop_port())
        m.set.online_port = coop_port();
    m.set_dirty = 1;
}

/* --------------------------------------------------------------- moving */

static int row_count(int tab)
{
    int kind[8];

    switch (tab) {
    case TAB_DISPLAY:  return disp_rows();
    case TAB_MOUSE:    return m.mouse_port >= 0 ? m.nmrow : 1;
    case TAB_GENERAL:  return G_COUNT;
    case TAB_COOP:     return coop_rows(kind);
    default:           return C_COUNT;
    }
}

static void move(int dy)
{
    int n = row_count(m.tab), s = m.sel[m.tab] + dy;

    m.confirm_at = 0;

    if (s < 0) s = n - 1;
    if (s >= n) s = 0;
    m.sel[m.tab] = s;
}

static void next_tab(int d)
{
    m.confirm_at = 0;
    m.tab = (m.tab + d + tabs_shown()) % tabs_shown();
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
        case D_WFOV: {
            double game = recomp_weapon_fov_game();
            double f = (m.set.weapon_fov > 0.0 ? m.set.weapon_fov : game) + dir * 5.0;

            if (f < RECOMP_WEAPON_FOV_MIN) f = RECOMP_WEAPON_FOV_MIN;
            if (f > RECOMP_WEAPON_FOV_MAX) f = RECOMP_WEAPON_FOV_MAX;
            m.set.weapon_fov = (f > game - 0.5 && f < game + 0.5) ? 0.0 : f;
            recomp_weapon_fov_set(m.set.weapon_fov);
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
        case D_FXAA:
            m.set.fxaa = !m.set.fxaa;
            xbox_D3D8SetFxaa(m.set.fxaa ? TRUE : FALSE);
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
        case G_VOLUME:
            m.set.volume += dir * 5;
            if (m.set.volume < 0) m.set.volume = 0;
            if (m.set.volume > 100) m.set.volume = 100;
            recomp_audio_output_set_volume_percent(m.set.volume);
            break;
        case G_MUTE:
            m.set.mute_unfocused = !m.set.mute_unfocused;
            recomp_audio_output_set_mute_unfocused(m.set.mute_unfocused);
            break;
        case G_PAUSE:
            if (!recomp_bindings_pause_available())
                return;
            /* Online, both are fixed for the session (hle_netplay.c,
             * pin_settings): the change is saved for the next game. */
            m.set.pause_unfocused = !m.set.pause_unfocused;
            if (!hle_net_active())
                recomp_bindings_set_pause_unfocused(m.set.pause_unfocused);
            break;
        case G_KBALONE:
            m.set.keyboard_alone = !m.set.keyboard_alone;
            if (!hle_net_active())
                recomp_bindings_set_keyboard_alone(m.set.keyboard_alone);
            break;
        case G_PROMPTS:
            if (!recomp_prompts_available())
                return;
            m.set.prompts = (m.set.prompts + dir + 3) % 3;
            recomp_prompts_set_mode(m.set.prompts);
            break;
        case G_INTRO:
            m.set.skip_intro = !m.set.skip_intro;
            break;
        }
        m.set_dirty = 1;
    }
    if (m.tab == TAB_COOP && coop_kind(row) == CR_MODE) {
        m.coop_join = !m.coop_join;
        m.confirm_at = 0;
        m.coop_error[0] = '\0';
    }
}

static void accept(void)
{
    int row = m.sel[m.tab];

    if (m.tab == TAB_DISPLAY) {
        int kind = disp_kind(row);

        if (kind == D_WIDE || kind == D_FXAA || kind == D_FPS || kind == D_FULL || kind == D_VRR ||
            kind == D_INTERP)
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
    if (m.tab == TAB_COOP) {
        int kind = coop_kind(row);
        char addr[96], pub[64];

        if (kind == CR_MODE) {
            change(1);
        } else if (kind == CR_CODE) {
            hle_net_copy_text(m.coop_code);
            m.copied_kind = kind;
            m.copied_at = GetTickCount64();
        } else if (kind == CR_PUBLIC && hle_net_public_addr(pub, sizeof pub) > 0) {
            snprintf(addr, sizeof addr, "%s:%d", pub, coop_port() ? coop_port() : 47800);
            hle_net_copy_text(addr);
            m.copied_kind = kind;
            m.copied_at = GetTickCount64();
        } else if (kind == CR_LAN) {
            coop_lan_first(addr, sizeof addr);
            if (addr[0]) {
                hle_net_copy_text(addr);
                m.copied_kind = kind;
                m.copied_at = GetTickCount64();
            }
        } else if (kind == CR_GO || kind == CR_LEAVE) {
            coop_go(kind);
        }
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
    menu->tab_count = tabs_shown();
    menu->tab = m.tab;
    for (i = 0; i < tabs_shown(); i++)
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
        if (recomp_weapon_fov_game() > 0.0) {
            snprintf(v, sizeof v, "%.0f\xC2\xB0%s",
                     m.set.weapon_fov > 0.0 ? m.set.weapon_fov : recomp_weapon_fov_game(),
                     m.set.weapon_fov > 0.0 ? "" : "   the game's own");
            row(menu, "Weapon field of view", v, 1);
        }
        snprintf(v, sizeof v, m.set.anisotropy > 1 ? "%dx" : "Off", m.set.anisotropy);
        row(menu, "Texture sharpness", v, 1);
        row(menu, "Edge smoothing (FXAA)", on_off(m.set.fxaa), 1);
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
                "How wide the weapon in your hands is drawn, whatever the view's field "
                "of view: wider shows it smaller and further off. The game's own keeps "
                "it as the game drew it.",
                "Sharpen textures seen at a glancing angle: floors, walls, roads.",
                "Smooth jagged edges, cheaply. For a low resolution: at 4x and above "
                "the supersampling has already smoothed them, and this is skipped.",
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
    } else if (m.tab == TAB_COOP) {
        int kind[8], n = coop_rows(kind), port = coop_port() ? coop_port() : 47800;
        int caret = (GetTickCount64() / 500) % 2 == 0;
        int armed = m.confirm_at && GetTickCount64() - m.confirm_at <= 4000;
        char pub[64], peer[64];
        int found, delay = 0;
        unsigned rtt = 0;

        for (i = 0; i < n; i++) {
            int lit = i == sel, copied = m.copied_kind == kind[i] &&
                                         GetTickCount64() - m.copied_at < 2000;
            switch (kind[i]) {
            case CR_MODE:
                row(menu, "Play", m.coop_join ? "Join a friend's game"
                                              : "Host: wait for player 2 to join", 1);
                break;
            case CR_CODE:
                snprintf(v, sizeof v, "%s%s", m.coop_code, copied ? "   copied" : "");
                row(menu, "Invite code", v, 0);
                break;
            case CR_PUBLIC:
                found = hle_net_public_addr(pub, sizeof pub);
                if (found > 0)
                    snprintf(v, sizeof v, "%s:%d%s", pub, port, copied ? "   copied" : "");
                else
                    snprintf(v, sizeof v, "%s", found < 0 ? "Not found (offline?)" : "Finding...");
                row(menu, "Internet address", v, 0);
                break;
            case CR_LAN:
                coop_lan_first(tmp, sizeof tmp);
                snprintf(v, sizeof v, "%s%s", tmp[0] ? tmp : "Not on a network",
                         copied ? "   copied" : "");
                row(menu, "On this network", v, 0);
                break;
            case CR_ADDR:
                snprintf(v, sizeof v, "%s%s", m.set.online_address,
                         lit && caret ? "_" : m.set.online_address[0] ? "" : lit ? " " : "Type it here");
                row(menu, "Invite code or address", v, 0);
                break;
            case CR_PORT:
                snprintf(v, sizeof v, "%s%s", m.coop_port, lit && caret ? "_" : "");
                row(menu, "Port", v, 0);
                break;
            case CR_GO:
                row(menu, m.coop_join ? "Join co-op" : "Host co-op",
                    armed ? "Press Enter again to restart the game now"
                          : m.coop_join ? "Restart the game and join"
                                        : "Restart the game and wait for player 2", 0);
                break;
            case CR_WITH:
                hle_net_session_info(peer, sizeof peer, &delay, &rtt);
                snprintf(v, sizeof v, hle_net_local_port() == 0 ? "Hosting; player 2 is at %s"
                                                                : "Player 2; the host is at %s",
                         peer);
                row(menu, "Co-op", v, 0);
                break;
            case CR_DELAY:
                hle_net_session_info(NULL, 0, &delay, &rtt);
                snprintf(v, sizeof v, "%d frame%s  (%u ms there and back)", delay,
                         delay == 1 ? "" : "s", rtt);
                row(menu, "Input delay", v, 0);
                break;
            default:
                row(menu, "Leave co-op", armed ? "Press Enter again to restart the game now"
                                               : "Restart the game on its own", 0);
                break;
            }
        }
        switch (kind[sel < n ? sel : 0]) {
        case CR_MODE:
            snprintf(menu->help, sizeof menu->help,
                     "Co-op on two PCs, each player on their own screen. The game starts again "
                     "on both: the host first, then player 2 joins. The host's saves are played.");
            break;
        case CR_CODE:
            snprintf(menu->help, sizeof menu->help,
                     "Send player 2 this code; Enter or a click copies it. They choose Join and "
                     "type it in -- no ports to open, for most connections.");
            break;
        case CR_PUBLIC:
            snprintf(menu->help, sizeof menu->help,
                     "Instead of the code: player 2 can join over the internet with this. "
                     "Hosting asks your router to open the port; if it can't, forward UDP %d "
                     "to this PC, or both join one VPN.", port);
            break;
        case CR_LAN:
            snprintf(menu->help, sizeof menu->help,
                     "On the same network, player 2 joins this; Enter or a click copies it.%s%s",
                     strchr(m.coop_lan, ',') ? " This PC also has: " : "",
                     strchr(m.coop_lan, ',') ? strchr(m.coop_lan, ',') + 2 : "");
            break;
        case CR_ADDR:
            snprintf(menu->help, sizeof menu->help,
                     "The invite code the host sent (FP-XXXX-XXXX-XXXX), or their address, like "
                     "203.0.113.5:%d. Type it; Ctrl+V pastes, Backspace deletes.", port);
            break;
        case CR_PORT:
            snprintf(menu->help, sizeof menu->help,
                     "The UDP port the host waits on; both use the same. 47800 unless "
                     "something else on the host uses it.");
            break;
        case CR_GO:
            snprintf(menu->help, sizeof menu->help, "%s",
                     m.coop_error[0] ? m.coop_error
                     : m.coop_join ? "Starts the game again and joins the host. Anything not "
                                     "saved is lost."
                                   : "Starts the game again and waits for player 2. Anything "
                                     "not saved is lost.");
            break;
        case CR_WITH:
            snprintf(menu->help, sizeof menu->help,
                     "This game is in co-op with another PC, each player on their own screen.");
            break;
        case CR_DELAY:
            snprintf(menu->help, sizeof menu->help,
                     "Frames between a press and the game seeing it, so that both PCs have both "
                     "players' controls in time. Set from the round trip as co-op began.");
            break;
        default:
            snprintf(menu->help, sizeof menu->help, "%s",
                     m.coop_error[0] ? m.coop_error
                     : "Starts the game again on its own. The other player is told you left.");
            break;
        }
    } else if (m.tab == TAB_GENERAL) {
        snprintf(v, sizeof v, "%d%%", m.set.volume);
        row(menu, "Volume", v, 1);
        row(menu, "Mute in the background", on_off(m.set.mute_unfocused), 1);
        row(menu, "Pause in the background",
            recomp_bindings_pause_available() ? on_off(m.set.pause_unfocused)
                                              : "Not for this game", 1);
        row(menu, "Keyboard and mouse",
            m.set.keyboard_alone ? "Player 1 alone, controllers from player 2"
                                 : "Player 1, with the first controller", 1);
        row(menu, "Button prompts",
            !recomp_prompts_available() ? "Not for this game"
            : m.set.prompts == RECOMP_PROMPTS_KEYBOARD ? "Keyboard and mouse"
            : m.set.prompts == RECOMP_PROMPTS_CONTROLLER ? "Controller"
            : "Automatic", 1);
        next_start(v, sizeof v, on_off(m.set.skip_intro), m.set.skip_intro != m.run_skip_intro);
        row(menu, "Skip intro videos", v, 1);
        {
            static const char *const help[G_COUNT] = {
                "Everything the game plays, music, voices and effects together. "
                "Changes at once.",
                "Silence the game while another window has the focus. F12 saves a "
                "screenshot to your Pictures folder.",
                "Alt+Tab during play pauses the game, so it is waiting at the pause "
                "menu when you come back.",
                "Alone: a friend joining through Steam Remote Play or Parsec, or any "
                "controller, plays player 2 and up while you play player 1 on keyboard "
                "and mouse.",
                "The game's button icons as the keys bound to them. Automatic follows "
                "whichever you used last, keyboard and mouse or a controller.",
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
             "Arrows, Enter, Tab, Esc    Mouse: point, click, wheel; right-click closes"
             "    Pad: D-pad, A, LB/RB, B");
}

/* ----------------------------------------------------------------- frame */

/* The mouse over the panel: moving onto a row selects it (only a move, so a
 * pointer left still never fights the keys); a click on a tab turns to it,
 * on a row's arrows changes it, on the rest of a row is Enter; a right click
 * anywhere closes, as Escape does. */
static void mouse_act(int kind, int x, int y)
{
    int index, hit = xbox_D3D8MenuHit(x, y, &index);

    if (kind == HLE_MENU_MOUSE_RIGHT) {
        close_menu();
        return;
    }
    if (hit == D3D8_MENU_HIT_TAB && kind == HLE_MENU_MOUSE_LEFT && index != m.tab &&
        index >= 0 && index < tabs_shown()) {
        m.confirm_at = 0;
        m.tab = index;
        if (m.sel[m.tab] >= row_count(m.tab))
            m.sel[m.tab] = 0;
        return;
    }
    if ((hit != D3D8_MENU_HIT_ROW && hit != D3D8_MENU_HIT_LEFT && hit != D3D8_MENU_HIT_RIGHT) ||
        index < 0 || index >= row_count(m.tab))
        return;
    m.sel[m.tab] = index;
    if (kind != HLE_MENU_MOUSE_LEFT)
        return;
    if (hit == D3D8_MENU_HIT_LEFT)
        change(-1);
    else if (hit == D3D8_MENU_HIT_RIGHT)
        change(1);
    else
        accept();
}

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
    unsigned keys[32], chars[32];
    int nkeys, nchars, wheel, nptr, i, escape = 0;
    struct { int kind, x, y; } ptr[16];
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
    nptr = m.nptr;
    memcpy(ptr, m.ptr, sizeof ptr);
    m.nptr = 0;
    nchars = m.nchars;
    memcpy(chars, m.chars, sizeof chars);
    m.nchars = 0;
    LeaveCriticalSection(&m.lock);
    for (i = 0; i < nchars && m.tab == TAB_COOP; i++)
        coop_type(chars[i]);

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
        /* While a control waits for a new binding (above) the mouse's clicks
         * are the binding's, so the panel takes them only otherwise. */
        for (i = 0; i < nptr && m.open && !m.editing; i++)
            mouse_act(ptr[i].kind, ptr[i].x, ptr[i].y);
        /* The wheel steps through the rows, a notch a row. */
        for (i = wheel / 120; i != 0 && m.open && !m.editing; i += i > 0 ? -1 : 1)
            move(i > 0 ? -1 : 1);
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
