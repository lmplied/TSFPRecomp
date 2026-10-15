/*
 * hle_screenshot.h -- F12: the picture on screen, saved as a PNG.
 *
 * The frame is read back as it reaches the window, so it is the size the
 * player sees and has the widescreen shape and supersampling in it, but not
 * the frame counter or the settings menu. It goes to the player's Pictures
 * folder, under the game's own name; saving runs on a thread of its own, so
 * a 4K PNG costs the game one readback, not a stutter.
 */
#ifndef XBOXRECOMP_HLE_SCREENSHOT_H
#define XBOXRECOMP_HLE_SCREENSHOT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Take the next frame. Returns 1 when one was asked for, so the caller
 * leaves its overlay off that frame. */
int hle_screenshot_request(void);

/* 1 when RECOMP_SCREENSHOT_AT says a scripted run takes one now. */
int hle_screenshot_scripted(void);

/* Once a frame, on the presenting thread: collects a taken frame and starts
 * saving it. Returns a line to show on screen while the result is fresh
 * ("Screenshot saved in Pictures\..."), or NULL. */
const char *hle_screenshot_frame(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_HLE_SCREENSHOT_H */
