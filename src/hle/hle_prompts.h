/*
 * hle_prompts.h -- button prompts for the keyboard and mouse.
 *
 * A console game shows its pad's buttons in its prompts: "(A) Select,
 * (B) Back", a green A and a red B. Played on a keyboard, those should be the
 * keys. A title project names the textures that are its button icons -- by
 * the checksum the texture cache keeps, found with RECOMP_HLE_D3D8_TEX_DUMP --
 * and which pad control each one is, and while the player is on the keyboard
 * and mouse each is drawn instead as a keycap with the first key or mouse
 * button bound to that control ("E", "ESC", "LMB"), its border in the
 * button's colour. Rebinding a key changes the prompt; picking up a pad puts
 * the pad's icons back. The title's layout is untouched: only the picture in
 * the icon changes (hle_texture_replace.h).
 *
 * Which ones show: the "prompts" setting (RECOMP_PROMPTS): auto -- the
 * keyboard's while the keyboard or mouse was used last, the pad's while a pad
 * was -- keyboard, or controller.
 */
#ifndef XBOXRECOMP_HLE_PROMPTS_H
#define XBOXRECOMP_HLE_PROMPTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The texture with this checksum is the pad's icon for control ("a", "b",
 * "start", ... as the bindings file names them); rgb is the button's colour
 * (0x00RRGGBB, 0 for none), for the keycap's border. Before the game starts. */
void recomp_prompts_icon(uint32_t checksum, const char *control, uint32_t rgb);

/* Whether a title named any icons, for the menu. */
int  recomp_prompts_available(void);

enum { RECOMP_PROMPTS_AUTO, RECOMP_PROMPTS_KEYBOARD, RECOMP_PROMPTS_CONTROLLER };
int  recomp_prompts_mode(void);
void recomp_prompts_set_mode(int mode);

/* Once a frame (the present): notices the player changing hands, or a key
 * being rebound, and has the icons drawn again. */
void hle_prompts_frame(void);

#ifdef __cplusplus
}
#endif

#endif
