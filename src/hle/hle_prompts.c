/*
 * hle_prompts.c -- see hle_prompts.h.
 *
 * Each keycap is drawn by GDI four times larger than it is kept -- three
 * white-on-black masks: the cap's outline, its face inside the border, and
 * the label -- composed into colour and alpha, and averaged down to 64x64.
 * GDI writes no alpha of its own, which is why it draws masks and not the
 * picture. 64 pixels is about what the icon covers on a 1080-line screen;
 * the title's 32x32 icon is sampled from 0 to 1, so the larger image is
 * simply sharper.
 *
 * Images are kept by label and colour, so a label seen once is drawn once.
 * Everything here runs on the thread that binds textures and presents.
 */
#include "hle_prompts.h"
#include "hle_texture_replace.h"
#include "input_bindings.h"
#include "recomp_config.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define MAX_ICONS   32
#define MAX_IMAGES  24
#define IMG         64                  /* the image kept, a side */
#define SS          4                   /* drawn this much larger */
#define BIG         (IMG * SS)

static struct {
    uint32_t checksum, rgb;
    char     control[16];
} g_icons[MAX_ICONS];
static int g_nicons;

static volatile int g_mode = -1;        /* RECOMP_PROMPTS_*; -1 until read */
static int      g_shown_keyboard = -1;  /* what the icons were last drawn for */
static unsigned g_shown_bindings;

static struct {
    char     label[24];
    uint32_t rgb;
    uint32_t argb[IMG * IMG];
} g_images[MAX_IMAGES];
static int g_nimages, g_next_image;

int recomp_prompts_available(void)
{
    return g_nicons > 0;
}

int recomp_prompts_mode(void)
{
    if (g_mode < 0) {
        const char *v = recomp_config_lookup("RECOMP_PROMPTS", "prompts");
        g_mode = (v && strcmp(v, "keyboard") == 0)   ? RECOMP_PROMPTS_KEYBOARD
               : (v && strcmp(v, "controller") == 0) ? RECOMP_PROMPTS_CONTROLLER
               : RECOMP_PROMPTS_AUTO;
    }
    return g_mode;
}

void recomp_prompts_set_mode(int mode)
{
    g_mode = mode < RECOMP_PROMPTS_AUTO || mode > RECOMP_PROMPTS_CONTROLLER
           ? RECOMP_PROMPTS_AUTO : mode;
}

static int keyboard_now(void)
{
    int mode = recomp_prompts_mode();

    return mode == RECOMP_PROMPTS_KEYBOARD ||
           (mode == RECOMP_PROMPTS_AUTO && recomp_bindings_last_input_keyboard());
}

/* ---- drawing a keycap --------------------------------------------------- */

/* One mask: white on black, from GDI, read back as 0..255 a pixel. */
typedef void (*mask_fn)(HDC dc, const void *arg);

static int draw_mask(HDC dc, uint32_t *bits, mask_fn fn, const void *arg, uint8_t *out)
{
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RECT all = { 0, 0, BIG, BIG };
    int i;

    FillRect(dc, &all, black);
    fn(dc, arg);
    GdiFlush();
    for (i = 0; i < BIG * BIG; i++)
        out[i] = (uint8_t)((bits[i] >> 8) & 0xFF);          /* green */
    return 1;
}

typedef struct { int inset, radius; } RoundArg;

static void round_mask(HDC dc, const void *arg)
{
    const RoundArg *r = (const RoundArg *)arg;
    HRGN rgn = CreateRoundRectRgn(r->inset, r->inset, BIG - r->inset + 1, BIG - r->inset + 1,
                                  r->radius, r->radius);
    FillRgn(dc, rgn, (HBRUSH)GetStockObject(WHITE_BRUSH));
    DeleteObject(rgn);
}

static void text_mask(HDC dc, const void *arg)
{
    const char *label = (const char *)arg;
    WCHAR wide[32];
    int len = MultiByteToWideChar(CP_UTF8, 0, label, -1, wide, 32) - 1;
    int px = BIG * 80 / 100, room = BIG * 66 / 100, cap;
    HFONT font = NULL;
    SIZE sz;
    TEXTMETRICW tm;

    if (len <= 0)
        return;
    /* As large as fits across the face. */
    for (; px > BIG / 8; px -= BIG / 64) {
        if (font)
            DeleteObject(font);
        font = CreateFontW(-px, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                           DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        SelectObject(dc, font);
        if (GetTextExtentPoint32W(dc, wide, len, &sz) && sz.cx <= room)
            break;
    }
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    /* Centred on the capitals' height, not the line's, which has room for
     * accents above and descenders below and would sit the letter low. */
    cap = GetTextMetricsW(dc, &tm) ? tm.tmAscent - tm.tmInternalLeading : px * 7 / 10;
    if (cap > tm.tmAscent * 3 / 4)
        cap = tm.tmAscent * 3 / 4;
    SetTextAlign(dc, TA_BASELINE | TA_CENTER);
    TextOutW(dc, BIG / 2, BIG / 2 + cap / 2, wide, len);
    SetTextAlign(dc, TA_TOP | TA_LEFT);
    SelectObject(dc, GetStockObject(SYSTEM_FONT));
    if (font)
        DeleteObject(font);
}

/* The cap: a dark face, a border in the button's colour (light grey for
 * none), the label in white; outside the outline, transparent. */
static int render(const char *label, uint32_t rgb, uint32_t *out)
{
    static uint8_t outer[BIG * BIG], inner[BIG * BIG], text[BIG * BIG];
    BITMAPINFO bi;
    void *bits = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bmp;
    RoundArg o = { SS * 2, BIG * 40 / 100 }, in = { SS * 6, BIG * 30 / 100 };
    double br = rgb ? (double)((rgb >> 16) & 0xFF) : 220.0;
    double bg = rgb ? (double)((rgb >> 8) & 0xFF) : 220.0;
    double bb = rgb ? (double)(rgb & 0xFF) : 220.0;
    int x, y;

    if (!dc)
        return 0;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = BIG;
    bi.bmiHeader.biHeight = -BIG;                   /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp || !bits) {
        DeleteDC(dc);
        return 0;
    }
    SelectObject(dc, bmp);
    draw_mask(dc, (uint32_t *)bits, round_mask, &o, outer);
    draw_mask(dc, (uint32_t *)bits, round_mask, &in, inner);
    draw_mask(dc, (uint32_t *)bits, text_mask, label, text);
    DeleteObject(bmp);
    DeleteDC(dc);

    /* Compose each big pixel premultiplied, then average SS x SS of them. */
    for (y = 0; y < IMG; y++)
        for (x = 0; x < IMG; x++) {
            double a = 0, r = 0, g = 0, b = 0;
            int i, j;

            for (j = 0; j < SS; j++)
                for (i = 0; i < SS; i++) {
                    int p = (y * SS + j) * BIG + x * SS + i;
                    double ao = outer[p] / 255.0, ai = inner[p] / 255.0, at = text[p] / 255.0;
                    /* the face, dark and nearly opaque, with the label on it */
                    double fr = 24 + (255 - 24) * at, fg = 26 + (255 - 26) * at,
                           fb = 30 + (255 - 30) * at;
                    double pr = br * (1 - ai) + fr * ai, pg = bg * (1 - ai) + fg * ai,
                           pb = bb * (1 - ai) + fb * ai;
                    double pa = ao * (ai > 0 ? (0.92 + 0.08 * at) : 1.0);
                    a += pa;
                    r += pr * pa;
                    g += pg * pa;
                    b += pb * pa;
                }
            a /= SS * SS;
            if (a > 0) {
                r /= SS * SS * a;
                g /= SS * SS * a;
                b /= SS * SS * a;
            }
            out[y * IMG + x] = ((uint32_t)(a * 255.0 + 0.5) << 24) |
                                ((uint32_t)(r + 0.5) << 16) | ((uint32_t)(g + 0.5) << 8) |
                                (uint32_t)(b + 0.5);
        }
    return 1;
}

static const uint32_t *keycap(const char *label, uint32_t rgb)
{
    int i;

    for (i = 0; i < g_nimages; i++)
        if (g_images[i].rgb == rgb && strcmp(g_images[i].label, label) == 0)
            return g_images[i].argb;
    i = g_nimages < MAX_IMAGES ? g_nimages++ : (g_next_image++ % MAX_IMAGES);
    snprintf(g_images[i].label, sizeof g_images[i].label, "%s", label);
    g_images[i].rgb = rgb;
    if (!render(label, rgb, g_images[i].argb)) {
        g_images[i].label[0] = '\0';
        return NULL;
    }
    return g_images[i].argb;
}

/* ---- the replacer ------------------------------------------------------- */

static int replace(uint32_t checksum, uint32_t format, uint32_t width, uint32_t height,
                   XboxTextureImage *out)
{
    char label[24];
    int i, port;

    (void)format;
    (void)width;
    (void)height;
    for (i = 0; i < g_nicons && g_icons[i].checksum != checksum; i++)
        ;
    if (i == g_nicons)
        return -1;
    if (!keyboard_now())
        return 0;
    port = recomp_bindings_mouse_port();
    if (port < 0)
        port = 0;
    if (!recomp_bindings_prompt_label((unsigned)port, g_icons[i].control, label, sizeof label))
        return 0;                           /* only a pad reaches it: its icon */
    out->argb = keycap(label, g_icons[i].rgb);
    out->width = out->height = IMG;
    return out->argb ? 1 : 0;
}

void recomp_prompts_icon(uint32_t checksum, const char *control, uint32_t rgb)
{
    if (g_nicons >= MAX_ICONS || !control)
        return;
    g_icons[g_nicons].checksum = checksum;
    g_icons[g_nicons].rgb = rgb;
    snprintf(g_icons[g_nicons].control, sizeof g_icons[g_nicons].control, "%s", control);
    if (g_nicons++ == 0)
        xbox_D3D8SetTextureReplacer(replace);
}

void hle_prompts_frame(void)
{
    int keyboard;
    unsigned bindings;

    if (!g_nicons)
        return;
    keyboard = keyboard_now();
    bindings = recomp_bindings_generation();
    if (keyboard != g_shown_keyboard || bindings != g_shown_bindings) {
        if (g_shown_keyboard >= 0)
            fprintf(stderr, "[PROMPTS] %s\n", keyboard ? "showing the keyboard's keys"
                                                       : "showing the pad's buttons");
        g_shown_keyboard = keyboard;
        g_shown_bindings = bindings;
        xbox_D3D8TextureReplacementsChanged();
    }
}
