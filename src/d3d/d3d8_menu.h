/*
 * d3d8_menu.h -- a menu panel drawn over the picture in the window.
 *
 * The renderer's half of the in-game settings menu (src/hle/hle_menu.c owns
 * what it says and what it does). Like d3d8_overlay it knows nothing about
 * its content: it is handed a title, tabs, rows and a line of help, and
 * draws them.
 *
 * Unlike the overlay it draws after the resolve, onto the window's own
 * buffer at the window's own size. The overlay goes into the scene, which is
 * the title's shape at the title's resolution: a menu there would be
 * stretched by a widescreen present and softened by a supersampled one. Here
 * every glyph lands on a screen pixel.
 */
#ifndef XBOXRECOMP_D3D8_MENU_H
#define XBOXRECOMP_D3D8_MENU_H

#ifdef __cplusplus
extern "C" {
#endif

#define D3D8_MENU_TABS 4
#define D3D8_MENU_ROWS 32

typedef struct D3D8MenuRow {
    char label[48];
    char value[64];
    int  arrows;        /* the value is changed with left and right */
    int  heading;       /* a section heading, not a setting */
} D3D8MenuRow;

typedef struct D3D8Menu {
    char        title[48];
    char        tabs[D3D8_MENU_TABS][24];
    int         tab_count, tab;
    D3D8MenuRow rows[D3D8_MENU_ROWS];
    int         row_count;
    int         selected;   /* the row with the focus */
    int         editing;    /* the selected row is waiting for a press */
    char        help[256];  /* about the selected row */
    char        footer[160];
} D3D8Menu;

/* Show this menu from the next present on (it is copied), or hide it with
 * NULL. Cheap to call every frame with the same content: the panel is only
 * drawn again when something in it changed. */
void xbox_D3D8MenuShow(const D3D8Menu *menu);

/* d3d8_device.c, after the resolve: draw the menu, if one is showing, onto
 * the window's buffer of w by h. */
struct RhiView;
void d3d8_menu_draw(struct RhiView *target, unsigned w, unsigned h);

/* Releases everything. Called from the device teardown. */
void d3d8_menu_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
