/*
 * hle_menu.h -- the in-game settings menu.
 *
 * F1 (or Back and Start together on a pad) opens a panel over the game with
 * three pages: Display, Mouse and Controls. What can change while the game
 * runs changes at once -- frame cap, frame-rate counter, fullscreen,
 * variable refresh, texture sharpness, mouse feel, every key binding -- and
 * what the game only reads at start-up (resolution, widescreen, frame
 * interpolation) is saved for the next start and says so. Everything goes
 * to the same files the launcher writes, so the two always agree.
 *
 * While the menu is open the game gets no input at all (recomp_input_suspend)
 * and keeps running; pause it first. The panel is drawn by d3d8_menu.c.
 */
#ifndef XBOXRECOMP_HLE_MENU_H
#define XBOXRECOMP_HLE_MENU_H

#ifdef __cplusplus
extern "C" {
#endif

/* The game window's thread, for each WM_KEYDOWN: 1 when the menu took the
 * key (F1, or anything while it is open). */
int  hle_menu_key(unsigned vk);

/* The game window's thread, for each WM_CHAR while the menu is open: a
 * character typed, for a row that takes text (the Co-op page's address and
 * port). 1 when the menu took it. */
int  hle_menu_char(unsigned ch);

/* The game window's thread, for each WM_MOUSEWHEEL: a notch, to step
 * through the rows or to bind the wheel. */
void hle_menu_wheel(int delta);

/* The game window's thread, while the menu is open: the pointer moved to,
 * or was clicked at, (x, y) in the window's inside, in pixels. */
enum { HLE_MENU_MOUSE_MOVE, HLE_MENU_MOUSE_LEFT, HLE_MENU_MOUSE_RIGHT };
void hle_menu_mouse(int kind, int x, int y);

/* Once a frame, on the thread that presents: read the pad, act on the keys,
 * and show or hide the panel. */
void hle_menu_frame(void);

/* Whether the menu is open. */
int  hle_menu_open(void);

/* What hle_d3d8.c lends the menu: the window's fullscreen state (changed on
 * the window's own thread), and the frame-rate counter. */
int  hle_d3d8_fullscreen(void);
void hle_d3d8_request_fullscreen(int on);
int  hle_d3d8_fps_overlay(void);
void hle_d3d8_set_fps_overlay(int on);
/* The size the title presents at (640x480 for most), for the resolution row. */
void hle_d3d8_guest_size(unsigned *w, unsigned *h);

#ifdef __cplusplus
}
#endif

#endif
