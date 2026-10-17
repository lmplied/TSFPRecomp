/*
 * launcher_theme.h -- how the launcher looks, kept behind an interface.
 *
 * Everything drawn goes through here, so the whole of the platform's
 * drawing lives in one file. Today that is GDI into a 32-bit DIB;
 * whenever the launcher needs to run somewhere other than Windows, this
 * is the file that is replaced rather than the launcher.
 *
 * The look is the original Xbox dashboard's: black, a green glow that
 * comes from behind, panels with soft edges, and the selected thing
 * lit rather than outlined -- in green unless the title gives colours of
 * its own (theme_set_palette). None of the console's own artwork is used
 * or reproduced -- no logos, no wordmarks, no fonts, no ripped
 * textures. What is here is drawn from arithmetic at run time.
 */
#ifndef XBOXRECOMP_LAUNCHER_THEME_H
#define XBOXRECOMP_LAUNCHER_THEME_H

#include <windows.h>

typedef struct ThemeRect { int x, y, w, h; } ThemeRect;

/* The colours. The default is the Xbox green; a title can have its own
 * (theme_palette_from: Future Perfect's blues and white, as its menus are). */
typedef struct ThemePalette {
    COLORREF bg_edge, bg_glow;      /* the background, at its edges and in the glow */
    COLORREF panel, panel_lit;      /* a panel's slab, resting and selected */
    COLORREF line;                  /* rims, their glow, the tab underline, arrows */
    COLORREF accent;                /* the selected tab and value */
    COLORREF text, text_dim;
    double   vignette;              /* how dark the corners fall, 0..1 */
} ThemePalette;

void theme_set_palette(const ThemePalette *p);
const ThemePalette *theme_palette(void);

/* A palette from three colours, 0xRRGGBB: the darks, the colour a selected
 * panel takes, and the one lines and text are drawn in. */
void theme_palette_from(ThemePalette *out, unsigned dark, unsigned mid, unsigned ink);

#define THEME_ACCENT     (theme_palette()->accent)
#define THEME_TEXT       (theme_palette()->text)
#define THEME_TEXT_DIM   (theme_palette()->text_dim)

/* A back buffer the size of the window. Everything below draws into it
 * and theme_present puts it on screen in one go, so nothing flickers. */
int  theme_begin(int width, int height);
void theme_present(HDC dc);
void theme_shutdown(void);

/* The background: black, with the glow behind where the content sits.
 * `pulse` runs 0..1 and drifts, so the glow breathes as the dashboard's
 * did rather than sitting still. */
void theme_background(double pulse);

/* A panel. `lit` from 0 (resting) to 1 (selected) takes it from a dark
 * translucent slab to one edged and washed with green. */
void theme_panel(ThemeRect r, double lit);

/* Text, left/centre/right aligned within `r`. `size` is a point size;
 * `weight` is 400 for normal and 700 for bold. */
typedef enum { THEME_LEFT, THEME_CENTRE, THEME_RIGHT } ThemeAlign;
void theme_text(ThemeRect r, const char *text, int size, int weight,
                COLORREF colour, ThemeAlign align);

/* The same, wrapped to the rectangle and top aligned, for the lines
 * of explanation. theme_text is single line: a newline in its text
 * draws as nothing and the line runs off the edge. */
void theme_text_wrapped(ThemeRect r, const char *text, int size,
                        COLORREF colour);

/* The tab strip's underline, which slides rather than jumping: `from`
 * and `to` are tab rectangles and `t` runs 0..1 between them. */
void theme_tab_underline(ThemeRect from, ThemeRect to, double t);

/* A left/right chevron pair beside a value, dimmed when the value is at
 * the end of its range and there is nowhere further to go. */
void theme_arrows(ThemeRect r, int can_left, int can_right, double lit);

#endif /* XBOXRECOMP_LAUNCHER_THEME_H */
