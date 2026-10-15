/*
 * d3d8_display.h -- the size the host renders at, and the pass that gets
 * that image onto the screen.
 *
 * The guest presents at its own size (640x480 for most titles) and every
 * draw path already ends in normalised coordinates: vertex programs have
 * their screen-space transform undone, the fixed-function path is
 * transformed by the host, and pre-transformed vertices divide by the
 * guest's presentation size. So a larger host target re-rasterises the
 * same scene rather than magnifying it, and nothing needs resampling.
 *
 * That is the whole mechanism behind both supersampling and, later,
 * widescreen: render the scene at a size of the host's choosing, then
 * resolve it down onto a swap chain that still matches the window.
 */
#ifndef XBOXRECOMP_D3D8_DISPLAY_H
#define XBOXRECOMP_D3D8_DISPLAY_H

#include "d3d8_internal.h"

#if defined(_WIN32)

/* Read once, from the environment, before the first device is created.
 *
 * RECOMP_RES_SCALE=<1..8> is the supersample factor. 1 renders at the
 * guest's own size and must be indistinguishable from having none of this
 * code at all -- the scene target is skipped entirely in that case.
 */
typedef struct D3D8DisplayPolicy {
    UINT scale;
    /* RECOMP_WIDESCREEN: the console is 16:9, and the title's frame is
     * therefore anamorphic -- authored squeezed, to be stretched back out
     * by the display. Only true of a title with a 16:9 mode of its own,
     * which is why it is off by default; a 4:3-only title told this
     * renders 4:3 content that then gets stretched. The kernel reads the
     * same variable to answer XC_VIDEO. */
    int  widescreen;
    /* RECOMP_WIDESCREEN_2D=centre: in widescreen, every 2D draw is kept at
     * 4:3 in the middle, backdrops included, and only whole-screen passes
     * (flat fades, effects over a copy of the frame) span the picture.
     * The default, auto, lets any 2D draw most of the screen wide span it,
     * which suits a title whose backdrops are separate from its HUD and
     * leaves seams in one that composes its menus from both. */
    int  centre_2d;
    /* RECOMP_WIDESCREEN_2D=native: the title draws its own 16:9 frame, 2D
     * and all, so nothing is squeezed. auto chooses this by itself for a
     * title that says so when it creates its device (see
     * d3d8_display_title_native_wide); this is for one that does not. */
    int  native_2d;
    /* RECOMP_ANISO=<1..16>, the anisotropy to force on textures the title
     * already filters linearly. 1 leaves every sampler as the title asked
     * for it. */
    UINT anisotropy;
} D3D8DisplayPolicy;

const D3D8DisplayPolicy *d3d8_display_policy(void);

/* Whether the frame being drawn is shown at 16:9: the widescreen setting,
 * unless the title has said the screen it is on is 4:3
 * (xbox_D3D8SetWideFrames). Everything that treats a frame as widescreen
 * -- the 2D squeeze, the shape at present -- asks this. */
int  d3d8_display_wide_now(void);
/* Whether the title draws its own 16:9 frame: it created its device with
 * D3DPRESENTFLAG_WIDESCREEN (xbox_D3D8SetTitleWidescreen), or the setting
 * says so (widescreen_2d = native). Such a title's 2D layer is authored
 * squeezed like its 3D, so the 2D squeeze and Hor+ -- both there to widen
 * a title that has no 16:9 mode -- stand aside. */
int  d3d8_display_title_native_wide(void);
/* Whether the picture fills the window, whatever its shape: widescreen,
 * the wide_fill setting, and a title that draws any shape itself
 * (xbox_D3D8SetTitleAnyAspect). The output shape is then the window's. */
int  d3d8_display_fill_now(void);
/* A frame has been presented (it notes each change of shape in the log). */
void d3d8_display_frame_done(void);

/* The size the scene is rendered at, for a guest presenting at guest_w by
 * guest_h. Equal to the guest's own size when nothing is scaled. */
void d3d8_display_scene_size(UINT guest_w, UINT guest_h,
                             UINT *scene_w, UINT *scene_h);
/* The device has made its targets again at this scale while the title runs
 * (xbox_D3D8RequestScale): what the policy reports from now on. */
void d3d8_display_set_scale(UINT scale);

/* Where the scene lands inside a back buffer of bb_w by bb_h: the largest
 * centred rectangle with the scene's own shape. Equal to the whole buffer
 * when the two already agree; otherwise the difference is the bars. */
typedef struct D3D8DisplayFit { UINT x, y, w, h; } D3D8DisplayFit;

D3D8DisplayFit d3d8_display_fit(UINT shape_w, UINT shape_h, UINT bb_w, UINT bb_h);

/* The shape the scene should appear as, which is its own unless the
 * title is drawing anamorphically for a 16:9 display. Not a size: only
 * the ratio of the two is used. 0 by 0 when it fills the window. */
void d3d8_display_output_shape(UINT scene_w, UINT scene_h,
                               UINT *shape_w, UINT *shape_h);

/* Draw `scene` into `fit` inside `out`, box filtered, painting everything
 * outside `fit` black so a window of a different shape gets bars rather
 * than a stretched picture. Binds `out` itself and puts back the render
 * target and viewport it found, which is why the caller must not bind the
 * output first: the state this restores is the scene the next frame draws
 * into. */
HRESULT d3d8_display_resolve(RhiView *scene, UINT scene_w, UINT scene_h,
                             RhiView *out, D3D8DisplayFit fit);

void d3d8_display_shutdown(void);

#endif /* _WIN32 */

#endif /* XBOXRECOMP_D3D8_DISPLAY_H */
