/*
 * hle_d3d8_record.h -- the host boundary of shadow mode, and frame capture
 * recorded at it.
 *
 * Every call src/hle makes into the host renderer (src/d3d) goes through one
 * of the wrappers below. Each one forwards the call unchanged and, while a
 * frame is being captured, also writes it into the capture
 * (d3d8_capture.h). Because the wrappers sit after all of shadow mode's Xbox
 * conversion, a capture holds exactly what the host was given, and replay
 * (src/replay) needs no Xbox knowledge to reproduce it.
 *
 * The rule that keeps this true: nothing in src/hle calls a device vtable
 * entry or a d3d8_vsh_* / d3d8_combiners_* setter directly. A direct call is
 * a call the capture cannot see, and a replay that silently differs from the
 * run. The one exception is reading the back buffer for frame dumps
 * (hle_d3d8.c, shadow_dump_frame), which changes no state.
 *
 * Cost when not capturing: one test of a static pointer per call.
 *
 *   RECOMP_D3D8_CAPTURE=<path>     where to write; capture is off without it
 *   RECOMP_D3D8_CAPTURE_SWAP=<n>   which swap to record (default 120)
 *   RECOMP_D3D8_CAPTURE_EVERY=<n>  then one more every n swaps (n >= 2), to
 *                                  <path>_<swap>.d3dcap, at most 24 files
 *   RECOMP_D3D8_CAPTURE_MINDRAWS=<n>  keep a frame only if it drew at least n
 *                                  times; otherwise try the next swap. Every
 *                                  frame is recorded (a full snapshot each)
 *                                  and deleted while it looks, so it is slow,
 *                                  but it finds a title's rare 3D frames
 *
 * Threads: guest threads are host threads, and the wrappers take no lock.
 * That is no worse than the host renderer itself, which takes none either;
 * shadow mode's calls arrive from whichever guest thread draws. A title that
 * draws from two threads at once would interleave its chunks in the capture
 * in the order the calls happened to be made.
 *
 * Windows only, like the rest of shadow mode.
 */
#ifndef XBOXRECOMP_HLE_D3D8_RECORD_H
#define XBOXRECOMP_HLE_D3D8_RECORD_H

#include <stdint.h>

#ifdef _WIN32

#include "d3d8_xbox.h"
#include "d3d8_vsh.h"

/* Called once per Swap, before the host Swap, with the swap counter after it
 * was incremented and the host back buffer's size. This is the capture's only
 * frame boundary: recording starts when the counter reaches the requested
 * swap (with a snapshot of the host's state) and the file is closed at the
 * next one. */
void hle_d3d8_capture_swap(unsigned long swaps, uint32_t width, uint32_t height);

/* True while a frame is being recorded. */
int hle_d3d8_capture_active(void);

/* Capture the next frame, whatever swap it turns out to be. This is the
 * answer to "it looks wrong here": a swap number has to be guessed before
 * the run, and nobody knows in advance which swap they will be standing on
 * when they see it. Without RECOMP_D3D8_CAPTURE the file lands beside the
 * executable. */
void hle_d3d8_capture_next_frame(void);

/* ------------------------------------------------------- deferred frames
 *
 * RECOMP_HLE_D3D8_DEFER=1 (or `defer_draws = 1` in the title's settings
 * file): the shadow renderer queues a frame's device calls instead of making
 * them, and runs the queue at the frame's end (hle_d3d8_defer_flush). That is
 * when the NV2A would reach them: the title's own D3D writes commands into a
 * push buffer the GPU consumes later, so a title may write a texture after
 * queueing the draw that samples it and still be correct on the console.
 * Marvel vs Capcom 2 writes each fighter's new animation tiles that way, and
 * drawing at the call showed the previous pose's tiles for one frame.
 *
 * State and draw calls below are queued with copies of what they point at.
 * Creation, locking and vertex-program declarations run at the call. HLE code
 * whose effect depends on guest memory at execution time -- binding a guest
 * texture, choosing its palette -- queues itself with hle_d3d8_defer_op. */
int  hle_d3d8_defer_on(void);
/* On, and not the thread currently running the queue: calls should queue. */
int  hle_d3d8_defer_recording(void);
/* Queue fn(copy of arg). The copy lives until fn has run. */
void hle_d3d8_defer_op(void (*fn)(const void *arg), const void *arg, size_t size);
/* Run everything queued, in order. Safe to call when nothing is. */
void hle_d3d8_defer_flush(void);

/* --------------------------------------------------- frame interpolation
 *
 * RECOMP_FRAME_INTERP=<n> (or `frame_interp = n`): hle_d3d8_interp.c draws
 * n-1 frames between each two the title draws, by drawing the newest one
 * again with its transforms blended toward the one before. The wrappers
 * below feed it: while hle_d3d8_interp_rec is set, each call that changes
 * state or draws is also kept as an op (the deferred queue's format), and
 * a draw is kept with what identifies it from one frame to the next. */
extern int hle_d3d8_interp_rec;

typedef struct {
    uint64_t content;     /* hash of the index and vertex bytes */
    DWORD    vs;          /* program handle or FVF */
    UINT     stride, prims;
    DWORD    prim_type, index_format;
    const void *tex[4];   /* bound textures, as keys only */
    const void *target;   /* render target texture, NULL for the back buffer */
    int      uses_proj;   /* drawn through the projection: 3D */
    int      programmable; /* a vertex program, not fixed function */
} HleInterpDrawKey;

void hle_d3d8_interp_op(void (*fn)(const void *arg), const void *arg, size_t size);
void hle_d3d8_interp_draw(void (*fn)(const void *arg), const void *arg, size_t size,
                          const HleInterpDrawKey *key);
/* Run fn(arg) once the frame being kept is no longer needed: a release or a
 * delete the kept ops may still name. */
void hle_d3d8_interp_retire(void (*fn)(const void *arg), const void *arg, size_t size);
/* Keep, as ops, everything the next frame draws with that was set before it
 * (the frame-start state), and the vertex constants as the title gave them
 * (before Hor+ scaled any). */
void hle_d3d8_interp_snapshot(void);
const float *hle_d3d8_interp_constants(void);   /* NV2A_VS_MAX_CONSTANTS float4s */
/* hle_d3d8.c: the F9 overlay over an in-between frame too; and whether a
 * movie went over the last frame on the movie layer. */
void hle_d3d8_overlay_redraw(void);
int  hle_d3d8_movie_layer_shown(void);
/* hle_d3d8.c: the back buffer now, to a BMP (RECOMP_INTERP_DUMP). */
int  hle_d3d8_dump_back_buffer(const char *path);
/* Called by hle_d3d8.c: once the device exists, and after each frame is
 * presented. */
void hle_d3d8_interp_init(void);
void hle_d3d8_interp_frame_end(void);
/* The five-second report's line, and in-between frames shown so far. */
void hle_d3d8_interp_report(void);
unsigned long hle_d3d8_interp_presents(void);

/* Screen copies, wrapped so frame interpolation draws them again. */
HRESULT host_CopyBackBufferToTexture(IDirect3DTexture8 *dst);
HRESULT host_CopyBackBufferRectToTexture(IDirect3DTexture8 *dst, const RECT *src,
                                         const POINT *at);

/* ----------------------------------------------------------- device calls */

HRESULT host_Clear(IDirect3DDevice8 *dev, DWORD count, const D3DRECT *rects,
                   DWORD flags, D3DCOLOR color, float z, DWORD stencil);
/* Not recorded: Swap is the frame boundary, and replay presents on its own. */
HRESULT host_Swap(IDirect3DDevice8 *dev, DWORD flags);
HRESULT host_SetRenderState(IDirect3DDevice8 *dev, D3DRENDERSTATETYPE state,
                            DWORD value);
HRESULT host_SetTextureStageState(IDirect3DDevice8 *dev, DWORD stage,
                                  D3DTEXTURESTAGESTATETYPE type, DWORD value);
HRESULT host_SetTransform(IDirect3DDevice8 *dev, D3DTRANSFORMSTATETYPE state,
                          const D3DMATRIX *matrix);
HRESULT host_SetViewport(IDirect3DDevice8 *dev, const D3DVIEWPORT8 *viewport);
/* Fixed-function lighting: the material and up to d3d8_GetNumLights() lights. */
HRESULT host_SetMaterial(IDirect3DDevice8 *dev, const D3DMATERIAL8 *material);
HRESULT host_SetLight(IDirect3DDevice8 *dev, DWORD index, const D3DLIGHT8 *light);
HRESULT host_LightEnable(IDirect3DDevice8 *dev, DWORD index, BOOL enable);
/* The Xbox scissor (xbox_D3D8SetScissors); the first rectangle is recorded. */
void    host_SetScissors(UINT count, BOOL exclusive, const D3DRECT *rects);
/* xbox_D3D8SetTwoDPlacement, recorded. A title project calls this rather
 * than the device's own, so a capture replays with the same placements. */
void    host_SetTwoDPlacement(int placement, uint32_t tag);
HRESULT host_SetTexture(IDirect3DDevice8 *dev, DWORD stage,
                        IDirect3DBaseTexture8 *texture);
HRESULT host_SetVertexShader(IDirect3DDevice8 *dev, DWORD handle);
HRESULT host_DrawPrimitiveUP(IDirect3DDevice8 *dev, D3DPRIMITIVETYPE type,
                             UINT prims, const void *vertices, UINT stride);
HRESULT host_DrawIndexedPrimitiveUP(IDirect3DDevice8 *dev, D3DPRIMITIVETYPE type,
                                    UINT min_index, UINT num_vertices, UINT prims,
                                    const void *indices, D3DFORMAT index_format,
                                    const void *vertices, UINT stride);

/* Textures. Creation and locking are not recorded as such: a texture is
 * written whole, from the host's own copy, the first time the capture needs
 * it (bound, or bound at the snapshot). An unlock of a texture the capture
 * already holds re-records that level, and a release retires its id. */
HRESULT host_CreateTexture(IDirect3DDevice8 *dev, UINT width, UINT height,
                           UINT levels, DWORD usage, D3DFORMAT format,
                           D3DPOOL pool, IDirect3DTexture8 **texture);
/* A cube texture: one the title renders into, or one it fills from memory
 * (static_cube). A capture records the creation, and the contents of a cube
 * that is not a render target (host_CubeUnlockRect); what is drawn into a
 * render-target cube's face is drawn again on replay. */
HRESULT host_CreateCubeTexture(IDirect3DDevice8 *dev, UINT edge, UINT levels,
                               DWORD usage, D3DFORMAT format, D3DPOOL pool,
                               IDirect3DCubeTexture8 **texture);
/* One face level of a cube the title fills from memory (hle_d3d8_texture.c,
 * static_cube). The capture records the texels with the cube
 * (D3D8CAP_CUBE_LEVEL), so a frame sampling such a cube replays with them. */
HRESULT host_CubeLockRect(IDirect3DCubeTexture8 *cube, D3DCUBEMAP_FACES face,
                          UINT level, D3DLOCKED_RECT *locked);
HRESULT host_CubeUnlockRect(IDirect3DCubeTexture8 *cube, D3DCUBEMAP_FACES face,
                            UINT level);
HRESULT host_LockRect(IDirect3DTexture8 *texture, UINT level,
                      D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags);
HRESULT host_UnlockRect(IDirect3DTexture8 *texture, UINT level);
/* SetPalette: the palette P8 textures bound to the stage are expanded through
 * when uploaded (d3d8_resources.c). Recorded, so a replayed frame expands its
 * P8 textures the way the live one did. */
HRESULT host_SetPalette(IDirect3DDevice8 *dev, DWORD stage, const DWORD *entries);
ULONG   host_ReleaseTexture(IDirect3DTexture8 *texture);

/* Render targets. texture NULL is the back buffer; otherwise level `level` of
 * a texture created with D3DUSAGE_RENDERTARGET (src/d3d, dev_SetRenderTarget),
 * and `face` names the cube face when that texture is a cube. The surface
 * object for the level or face is taken and released inside.
 *
 * depth NULL is no depth; the device's own depth buffer is
 * host_DeviceDepthSurface. A switch is recorded with the texture and depth
 * surface it names, each written the first time; the depth surface is not
 * recorded at creation. */
HRESULT host_SetRenderTarget(IDirect3DDevice8 *dev, IDirect3DBaseTexture8 *texture,
                             UINT level, UINT face, IDirect3DSurface8 *depth);
HRESULT host_CreateDepthStencilSurface(IDirect3DDevice8 *dev, UINT width, UINT height,
                                       D3DFORMAT format, IDirect3DSurface8 **surface);
/* The device's own depth surface, read once while it is still the one bound
 * (call right after creating the device). A capture names it with
 * D3D8CAP_DEPTH_DEVICE rather than as a depth surface of its own. Not a
 * reference the caller owns. */
IDirect3DSurface8 *host_DeviceDepthSurface(IDirect3DDevice8 *dev);

/* -------------------------------------------- vertex programs, combiners */

HRESULT host_vsh_create_shader(const DWORD *microcode, int insn_count, DWORD *handle);
HRESULT host_vsh_delete_shader(DWORD handle);
/* Whether the host program behind handle holds exactly this microcode. */
BOOL    host_vsh_same_microcode(DWORD handle, const DWORD *microcode, int insn_count);
void    host_vsh_set_constant(int first_reg, const float *data, int count);
HRESULT host_vsh_set_declaration(DWORD handle, const D3D8VshInput *inputs, int count);
void    host_vsh_set_screenspace(const float scale[4], const float offset[4]);
void    host_vsh_set_vertex_data(int reg, const float value[4]);
void    host_combiners_set_pixel_shader(DWORD token);

#endif /* _WIN32 */

#endif /* XBOXRECOMP_HLE_D3D8_RECORD_H */
