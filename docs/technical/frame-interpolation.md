# Frame interpolation

Frames shown between a title's own, so motion looks smoother than 60 while
the title itself still runs at 60. Written 1 October 2026 for xboxrecomp
developers. The code is `src/hle/hle_d3d8_interp.c`; its header comment is the
reference, and this note is the overview and the measurements.

## Why this, and not a higher frame rate

Games of this era step their simulation once a frame. TimeSplitters 2
uncapped runs its physics and timers fast (`resolution-and-framerate.md`), and
making each title's logic time-based is months of work per title that helps
no other. So the title stays at 60, and the toolkit draws the extra frames:
it keeps each frame's host calls and draws the newest frame again with the
matrices in its vertex constants blended part of the way back toward the
frame before. The title never learns these frames exist.

## Switching it on

| Setting | Values |
| --- | --- |
| `RECOMP_FRAME_INTERP` / `frame_interp` | `1` off (the default), `2`-`4` frames shown for each one the title draws. The launcher's "Frame interpolation" row |
| `RECOMP_INTERP_PROJECTION` / `interp_projection` | the first of the 4 projection registers, or `none`. Default: what the title said, else the Hor+ register |
| `RECOMP_INTERP_AFFINE` / `interp_affine` | `64-75`: a run of 4-register affine matrices (model-view, bones), or `none` |

A title project says which registers are which with
`xbox_D3D8SetInterpRegisters(projection, affine_first, affine_count)` (in
`d3d8_xbox.h`); the settings override it. TimeSplitters 2 calls it with
`(60, 64, 12)` at its first present (`src/recomp_manual.c` in split2-recomp),
and Future Perfect with the same numbers from `fp_title_setup`. Without it
nothing but the projection is blended, which is right for no title's moving
objects: find the registers first (below).

A title that waits for the vblank itself before each frame also calls
`xbox_Nv2aFlipGateSetTitlePaced(1)` (`xbox_memory_layout.h`); see "Titles
that pace themselves" below.

Best with variable refresh (`vrr`, fullscreen), where each frame is shown the
moment it is presented. Windowed, the compositor shows the newest frame at
each refresh of the display, which is still smoother than 60 but uneven.

## How it works

**Where the time comes from.** The flip gate (`xbox_memory_layout.c`) holds a
finished frame until its vblank. TimeSplitters 2 finishes a frame in 2-5 ms
and then waits 11-14. The gate lends that wait to the module
(`xbox_Nv2aFlipGateSetIdle`): frame N is complete before the moment halfway
between N-1 and N, so the in-between frame is drawn from N-1 and N, and N is
still shown at its vblank as it always was. **At 2x this adds no latency.**
Nothing is extrapolated.

**Titles that pace themselves.** Future Perfect does not wait in Swap: its
frame clock (`sub_001538C0`) spins on its own vblank counter before each
frame, so it reaches Swap 1-4 ms after the vblank it waited for. That vblank
has already signalled the gate's (auto-reset) event, so under adaptive the
Swap passes at once, the gate waits 0 ms, and no in-between frame is ever
drawn (measured: 0 shown in every window). A title project that knows this
calls `xbox_Nv2aFlipGateSetTitlePaced(1)`; then, while interpolation has
lent itself the gate, adaptive and off hold each Swap to the next vblank as
the "60" cap does. Making that the rule for every title was not done:
strict 60 costs TimeSplitters 2 dips below 60 (`xbox_memory_layout.c`, "Why
adaptive won"). For a self-paced title the hold does add latency over
adaptive -- the finished frame waits for its vblank instead of going out
1-4 ms after the last one -- which is the time the in-between frames are
drawn in.

**What is kept** (`hle_d3d8_record.c`, while `hle_d3d8_interp_rec` is set):
every host call that changes state or draws, as an op in the deferred queue's
format, from the frame's start. The frame starts with a snapshot of the state
set before it (`hle_d3d8_interp_snapshot`, `capture_snapshot`'s list as ops),
so running the list draws the frame again. The snapshot includes every
program's vertex declaration, and each change of one is kept like any state:
Future Perfect draws one program with up to four vertex layouts in a frame
(272 changes in a gameplay frame), and before declarations were kept, the
redraw read each draw's vertices with its program's last layout -- the world
and the cutscenes' backdrops went missing from the in-between frames, leaving
the sky, drawn first, flickering on its own. Vertex constants are kept as
src/hle gave them, before Hor+ scales its register, because they go through
the same setter again. Each draw is kept with a key (a hash of its vertex and
index bytes, its program, shape, textures and target) and the values of the
registers that are blended. Texture releases and program deletes wait until
no kept list can name them. Screen copies are wrapped
(`host_CopyBackBufferToTexture`) so an in-between frame copies its own screen.

**Drawing it.** `xbox_D3D8InterpBegin` points "the back buffer" at a second
image the size of the scene: the scene target still holds frame N, about to be
shown. The kept list runs through the same `host_*` wrappers with keeping off;
before each draw, its blended registers are set. Then the F9 overlay, the
present (`xbox_D3D8InterpPresent`, which does not count a frame), and
`xbox_D3D8InterpEnd`.

**Matching** frame N's draws to N-1's, in three tiers:

1. the same bytes, program, shape and target; of several copies of one mesh,
   the one nearest in translation;
2. the same program, shape, textures and target (a mesh the CPU rewrites);
3. the same program, stride, primitive type and target (a batch the CPU
   builds, whose count and textures change every frame: particles).

Unmatched draws are drawn as N drew them.

**Blending.** The projection term by term, when both frames have the same kind
(perspective or not). Each affine matrix is split by polar decomposition into
rotation and the rest; the rotation is blended as a quaternion, the rest and
the translation linearly, so a turning object keeps its size. Only what the
draw's program reads is blended (`d3d8_vsh_bound_reads`): a register the
program never reads holds whatever was last put there. Only draws whose
program goes through the projection are blended, so screen-space draws (HUD,
menus) are N's to the pixel and do not shimmer. A matrix that turned more than
45 degrees, or moved by more than half its distance, in one frame is a cut,
and drawn as N has it.

**Premultiplied matrices.** Some titles multiply the projection in
themselves and upload the product: most of Future Perfect's programs draw
with `c68`-`c71` = projection (`c60`) times model-view (`c64`), and never read
`c60` at all. Such a group's last row is not `(0,0,0,1)`. It is taken apart
with the projection register's value at the draw (`unproject`: the inverse
projection times the group, which must come out affine, or the register was
not what the title multiplied in and the group is drawn as N has it), the
affine part is blended as above, and the blended projection is multiplied
back in. A program that reads such a group counts as 3D even though it never
reads the projection itself; before this, those draws were held at N while
the rest moved, and FP's world did not turn in the in-between frames at all
(its camera's rotation lives in the model-view, not the projection).

**Held frames.** A frame is not interpolated, and the display keeps N-1 until
N as at 60, when it has no 3D, when less than 60% of its 3D primitives
matched, when more than half were cut, when a movie went over it, while a
capture is being written, or when the redraw would not finish before the
vblank.

**Pacing.** In-between frame k of n is due k/n of the way from the last frame
shown to the vblank that shows this one. It is started early by what a redraw
has been costing (measured up to its present; a Vulkan present can then block
on the next image), and given up if it would not be done before the vblank.
Its blend is for the moment it is shown.

## Results: TimeSplitters 2, Siberia

Offline first (week 1). `RECOMP_D3D8_CAPTURE_EVERY=1` records consecutive
frames and `d3d8_replay --const-patch` redraws a capture with constants
replaced per draw. From three consecutive gameplay frames N-1, N, N+1 (walking
and turning), N-1 and N+1 blended at 0.5 were compared with the real N:

| | |
| --- | --- |
| 3D primitives matched | 94-100% |
| Blend closer to the real frame than repeating N-1 | 8 of 8 triples: 38.3-40.9 dB against 27.4-27.8 dB |

Live, 2x, D3D11, the same walk (five-second windows):

| | |
| --- | --- |
| Game frames | 60.0 a second, unchanged |
| In-between frames shown | 296-300 for each 300 frames in play |
| 3D primitives matched | 98-100% |
| Redraw | 0.2-0.9 ms average, under 2.7 ms worst |
| Shown from its moment | 0.15-0.37 ms average, about 1 ms worst |
| Blend | 0.51-0.52 average (0.5 is on time) |
| Held | menus with no 3D, the cut from the level's intro to play, empty frames at loads |

Vulkan submits more slowly than D3D11 (its own frames take about 1.5 ms
longer): at 2x the redraw is 1.3-3.4 ms and pacing is as good; at 3x most
slots fit and some are late, and are skipped.

## Results: Future Perfect, 2401 (15 October 2026)

Registers from a capture of 24 consecutive frames while turning
(`RECOMP_D3D8_CAPTURE_EVERY=1`) and the generated programs
(`RECOMP_D3D8_VS_DUMP=1`): `c60`-`c63` the projection (a second one, 45
degrees, for the gun), `c64`-`c67` the model-view, `c68`-`c71` their product
(the world and most objects draw with only this), `c72`-`c75` another
model-view. Skinned programs index their bones from `c96` (`c[a0 + 96]`),
outside the blended run.

Live, 3x, D3D11, 1x resolution, turning and walking, five-second windows:

| | |
| --- | --- |
| Game frames | 278-300 a window (300 is 60 a second); held to each vblank, a frame that runs long waits a whole one |
| In-between frames shown | 551-595 a window, 0-5 late |
| 3D primitives matched | 100% in play; 99% in the cut from the intro |
| Redraw | 0.2-0.6 ms average, about 2 ms worst |
| Held | the front end's frames with no 3D, loads |

In-between frames dumped beside the real frames either side
(`RECOMP_INTERP_DUMP`, `RECOMP_HLE_D3D8_DUMP_EVERY`) during a turn: the
world-only top of the image differs from both neighbours, and lies nearer the
one its blend says (blend 0.6: nearer the newer frame). The 3D backgrounds of
the front end interpolate too, with the 2D over them unchanged.

Those first checks missed the declarations (above): the views they dumped
happened to draw well enough with each program's last layout. The level's
opening cutscene did not, and neither did most of a player's own play. With
declarations kept, at 3x resolution, 16:9 and a 90-degree view: 300 game
frames and 600 in-between frames in every five-second window of play, none
late, redraw 0.55 ms; in-between frames of the cutscene and of play each lie
between the frames either side. The broken redraws had also cost the game
itself frames (30-45 a second): vertices read through the wrong layout make
huge triangles.

## Diagnostics

- The five-second report: `[INTERP] 2x: 300 frames, 300 in-between shown, 0
  late; held ...; matched 100% of 3D primitives; redraw ... ms; shown ... ms
  from its moment; blend ...`. Frames that are neither shown nor held ran past
  their vblank, so there was no wait to use.
- `RECOMP_INTERP_DUMP=<prefix>` with `RECOMP_INTERP_DUMP_FROM=<swap>` writes
  in-between frames as BMPs. Beside `RECOMP_HLE_D3D8_DUMP_EVERY=1` from the same
  swap, they sit between the frames either side.
- `RECOMP_INTERP_DEBUG=1` prints the draws behind a frame held as a cut.
- `RECOMP_INTERP_DRAWS=<n>` draws only the first n draws of each in-between
  frame (state still applied), as `d3d8_replay --draws` does for a capture.
  An in-between frame that is wrong where the real one is right: dump both
  (above), capture the same frame (`RECOMP_D3D8_CAPTURE`), and step n until
  the in-between frame and the replay part. That is how the missing
  declarations were found -- draw 37 of a cutscene frame, the space skybox,
  drew in the replay and not in the in-between frame, with the depth and
  stencil tests both forced off.
- F9's overlay shows the game's frame rate and, with interpolation, what is
  shown.

**Finding a title's registers.** Capture two consecutive frames
(`RECOMP_D3D8_CAPTURE_EVERY=1`) and look at the constants of a few draws:
a projection is `(sx,0,0,0) (0,sy,0,0) (0,0,a,b) (0,0,±1,0)` and set once or
twice a frame; a model-view changes from draw to draw, with `(0,0,0,1)` in its
fourth register.

## Limits and next steps

- **Particles the CPU positions stay at 60** (TS2's snow). Blending their
  vertex positions for tier-2 matches would fix it.
- **Fixed-refresh displays at a rate that is not a multiple of 60** (144 Hz)
  need VRR, or a render thread that shows the state at a fixed small delay
  (the design's "model B", 3-4 weeks).
- **Fixed-function titles** are not blended yet: their WORLD, VIEW and
  PROJECTION transforms are named by `SetTransform` and would need no hints.
- **Push-buffer draws** (Burnout 2 in two places, XGRA, Breakdown) never reach
  the host as draws and cannot be redrawn.
- **Deferred frames** (`RECOMP_HLE_D3D8_DEFER`) are not combined with this.
- A title that does not clear its depth buffer each frame is not handled: the
  in-between frame shares the device depth buffer.
