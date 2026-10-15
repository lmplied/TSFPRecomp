# Output resolution and frame-rate cap

Two things every recompiled title should offer and none offers today: render at
more than the console's resolution, and run at the console's frame rate instead
of as fast as the host allows. Both are toolkit features, not per-game ones.
This document says where the numbers come from now, what a scale factor breaks,
where the cap belongs, and in what order to build it.

Everything below was read out of this tree, and the resolution half was
prototyped and replayed. The pictures it produced are described in
"What was measured"; the experiment itself is the commit after this one on
`design/resolution-fps-cap` and is **not** meant to be merged as it stands.

---

## 1. Resolution

### Where 640x480 comes from

It is not a constant. It is the title's own `D3DPRESENT_PARAMETERS`:

```text
src/hle/hle_d3d8.c:312   UINT width = 640, height = 480;      /* only a fallback */
src/hle/hle_d3d8.c:316       width  = HLE_MEM32(pp_va + 0);   /* BackBufferWidth  */
src/hle/hle_d3d8.c:317       height = HLE_MEM32(pp_va + 4);   /* BackBufferHeight */
```

and the run log says so: `shadow device 640x480, from the title's own
CreateDevice parameters`. Burnout 2 asks for 640x480. A title that asks for
720x480 or 1280x720 already gets it. The chain from there:

| Step | Where |
| --- | --- |
| host window sized from it | `hle_d3d8.c:201` `shadow_window` |
| `pp.BackBufferWidth/Height` | `hle_d3d8.c:329` |
| DXGI swap chain | `d3d8_device.c:204` `scd.BufferDesc.Width/Height` |
| `state->width/height`, the backend's idea of the screen | `d3d8_device.c:247` |
| default RTV and the device depth buffer | `d3d8_device.c:265`, `:273` |
| initial D3D11 viewport | `d3d8_device.c:1635` |
| `d3d8_GetBackbufferWidth/Height()` | `d3d8_device.c:156` |
| `g_shadow_width/height`, `g_target_width/height` in the HLE | `hle_d3d8.c:109`, `:113` |

`g_target_*` is the **current render target's** size, not the screen's: it
follows `SetRenderTarget` (`hle_d3d8.c:1532`) and drives two things — the host
viewport clamp (`hle_d3d8.c:1309-1316`, the "INT_MAX means the whole target"
rule) and the Xbox screen-space undo constants
(`shadow_viewport_constants`, `hle_d3d8.c:265-280`).

### What is already resolution-independent

**Vertex programs.** An Xbox vertex program ends in the XDK's screen-space
transform: `oPos` leaves the program in render-target pixels. The generated
host program undoes it:

```text
src/d3d/d3d8_vsh.c:697   /* Undo the Xbox screen-space transform */
                         oPos -= xboxScreenspaceOffset;
                         oPos /= xboxScreenspaceScale;
```

The scale and offset are half the target's width and height (`hle_d3d8.c:267`).
The result is clip space — normalised, with no pixel units left in it. **So the
program path scales for free**, as long as the undo keeps using guest-sized
numbers and only the host viewport grows. That is the single fact the whole
design rests on, and the replay images confirm it.

**Fixed-function 3D.** `SetTransform` is forwarded (`hle_d3d8.c:1280`) and the
host does the transform; the output is clip space for the same reason.

**Textures.** Coordinates are normalised, so nothing changes. There is no
half-texel correction anywhere in the shader generators (a grep for `0.5f` in
`d3d8_shaders.c` finds only spotlight cones) and none is wanted: D3D11 uses the
D3D10+ pixel-centre convention, which is the one the generated code assumes.

**Clears.** `host_Clear(g_shadow, 0, NULL, ...)` — count 0, no rects
(`hle_d3d8.c:828`). Whole-target clears carry no pixel coordinates.

### What breaks, precisely

**1. Pre-transformed (`D3DFVF_XYZRHW`) draws — the 2D and HUD path.** The
generated fixed-function vertex shader maps screen pixels to clip space by
dividing by a constant:

```text
src/d3d/d3d8_shaders.c:157   o.pos.x = (input.pos.x / ScreenSize.x) * 2.0 - 1.0;
src/d3d/d3d8_shaders.c:158   o.pos.y = 1.0 - (input.pos.y / ScreenSize.y) * 2.0;
```

and `ScreenSize` is filled from the **host back buffer**:

```text
src/d3d/d3d8_shaders.c:941   cb->screen_w = (float)d3d8_GetBackbufferWidth();
src/d3d/d3d8_shaders.c:973   cb->screen_h = (float)d3d8_GetBackbufferHeight();
```

The guest's vertices are in *its* pixels. Scale the back buffer without
touching this and every 2D element lands in the top-left quadrant at 1:1 size
(picture below). The fix is one line in each place: divide by the **guest**
presentation size.

This is *already* a bug at scale 1. A title drawing pre-transformed geometry
into an offscreen target of a different size divides by the back buffer's size
today, which is the wrong number. Fixing it for the scale fixes that too — and
that is the one change here that alters 1x behaviour, so it needs its own A/B.

**2. Offscreen render targets and their depth surfaces.** Created at the guest
surface's size:

```text
src/hle/hle_d3d8.c:1379   scratch_target(w, h)       -- host_CreateTexture, RENDERTARGET
src/hle/hle_d3d8.c:1401   depth_surface(w, h)        -- host_CreateDepthStencilSurface
src/hle/hle_d3d8_texture.c:436  hle_d3d8_render_texture  -- the title's render-to-texture
```

Leave them alone and the screen is sharp while every reflection, cube face and
composed pass stays at 640x480 and is magnified on the way back. Scale them and
they must scale *together* with their depth: D3D11 requires the depth surface
to match the target exactly, and the backend already checks this
(`d3d8_device.c:1254`).

**3. Render-target textures share the texture cache with uploaded ones.**
`hle_d3d8_texture.c` keeps one entry per guest texture with a `rendered` flag
(`:61-65`) meaning "the host copy's contents came from drawing, not from guest
bytes, so never re-upload". A texture that is *both* rendered into and refilled
from guest memory would have a scaled host copy and a guest-sized upload. The
prototype hit exactly this: the 867-draw capture replayed with **1 malformed
chunk at 2x and 0 at 1x** — a texture level refill into a target that is no
longer the size the capture recorded. The proper fix is for the cache to know a
scaled entry is a render target and re-create it at guest size if guest bytes
ever arrive.

**4. Anything that reads pixels back.** The frame dump reads the host back
buffer and writes `g_shadow_width * g_shadow_height` (`hle_d3d8.c:635`) — it
must use the scaled size or write a truncated BMP. The capture header records
"the host device's back buffer" (`d3d8_capture.h:143`); that should stay the
**guest** size so a capture is scale-independent and can be replayed at any
scale, which is what makes the A/B tool work.

**5. Things that do not exist yet but will.** Nothing in this tree sets a
scissor rectangle (`d3d8_states.c:222` hard-codes `ScissorEnable = FALSE` and
no `SetScissors` is replaced) and `CopyRects` is not handled at all
(`shadow-mode.md`, "Gaps"). Both carry pixel rectangles in guest coordinates
and both will need the same multiply on the day they land. A lockable render
target the title reads back is the one case that needs a *down*sample rather
than a multiply; xemu does exactly that for surface downloads.

### Recommended mechanism: scale in the backend, not the HLE

Give `src/d3d` a scale factor. `CreateDevice` remembers the guest presentation
size and multiplies the swap chain by the factor; `SetViewport` multiplies the
rectangle it is handed; render targets, render-target textures and depth
surfaces are created multiplied; the pre-transformed shader divides by the
remembered guest size. **`src/hle` needs no change at all** — it goes on
speaking in the guest's pixels, which is what keeps its arithmetic (the
screen-space undo, the `INT_MAX` viewport clamp, surface measurement)
comparable with Cxbx-Reloaded's, and what keeps guest-sized numbers out of the
scaled world.

Three reasons this is the right layer:

- one knob, one module, and every call site that turns a guest number into host
  pixels is already inside it;
- the Vulkan backend inherits the design point rather than the code, and it has
  to touch the viewport anyway (negative height for the Y-flip — and note
  `d3d8_states.c` already carries one hand-annotated winding fix, so do not
  stack a second);
- **the capture/replay tool gets `--scale` for free**, which is the A/B rig for
  the whole feature. Proved: see below.

This is xemu's shape at a different layer. xemu scales NV2A *surfaces*:
`pg->surface_scale_factor` multiplies colour and zeta surface allocations and
the GL viewport, leaves guest-side vertex data alone, and forces a downsample
when a surface is read back into guest memory. We are one level up — at the
D3D8 API rather than at the surface — so we scale render targets and the
viewport and get the same result. Cxbx-Reloaded also scales at its host device
and has to patch pre-transformed vertices, which is precisely the one fix found
here; that agreement is worth something.

### Interface

Now: **`RECOMP_RES_SCALE=<1..8>`**, an integer multiplier of whatever the title
asked for. 640x480 x2 = 1280x960, x3 = 1920x1440. Integer keeps the aspect
exact, keeps the pixel grid aligned, and avoids every half-pixel argument.
Unset means 1, which must be bit-identical to today.

Later: `RECOMP_RES=<WxH>` for an arbitrary target, with the guest's aspect
preserved by letterboxing inside it — a title's 4:3 output stretched to 16:9
is wrong, and the title is the only thing that knows whether it has a widescreen
mode. Titles that do have one already get it, because the shadow device is
created from their own parameters.

Per-title configuration: there is no runtime config reader today — every switch
in the runtime is a `getenv`. `config/` holds per-title *pipeline* seeds, read
by the tools, not by the runtime. The cheap path is to keep the environment
variables as the mechanism and later add a small loader that reads
`titles/<name>/recomp.ini` and sets the same names before anything reads them;
that way no call site changes and both routes stay live. Do not invent a second
configuration mechanism for one feature.

---

## 2. Frame rate

### How a title paces itself here today

Three clocks matter.

**The vblank pump**, `kernel_vblank_tick()` in `src/kernel/kernel_bridge.c`
(`:2230`), a `QueryPerformanceCounter` clock at `RECOMP_VBLANK_HZ` (default 60),
on a high-resolution waitable timer. It delivers 60.00 Hz and costs almost
nothing. This is solved, and it is the only piece of the three that is.

**The title's own wait-for-vblank loop.** Burnout 2's `sub_000CBD90` spins
until a word written by its vblank callback changes — *or* until it has
something else to do, which during a load it always does, so the loop returns at
once and the title presents again (`performance-60fps.md`).

**The GPU time fence, which is what actually fails to throttle.** `Swap` waits
through `D3D_BlockOnTime`, which spins on a word of guest memory the GPU is
supposed to write. Nothing raises that interrupt, so the kernel mirrors it:
`fence_mirrors_tick()` (`src/kernel/xbox_memory_layout.c:524`) copies the
submitted value (`PUT`) onto the completed word, and it is called from
`nv2a_ack_thread`, a `Sleep(0)` loop (`:663`). So the flip completes *the
instant it is submitted* and `Swap` never blocks.

Measured again today, freshly, with the capture recorder on (which itself costs
a lot): Burnout 2 in its front end, **236-380 fps, vblank 60.0 Hz**. The
performance doc has 1750 fps for the same screens without capture and 3400 with
host drawing off. TimeSplitters 2 pacing goes through the same fence — the
mirror is set up from `D3D_BlockOnTime`'s own prologue per title
(`hle_d3d8.c:734`), 4721's offsets being `+0x30/+0x34` against 5344's
`+0x2C/+0x30` — so it has the same gap, and the reason its front end looks
different is that its draws are skipped by the shadow renderer, not that it
paces differently.

### The fix: a flip gate on the fence

*Implemented 19 Sep 2026, with one change from the plan below.* Holding the
fence mirror did not pace anything: a title's Swap waits for the *previous*
frame's fence, which the mirror had completed at the last vblank, so two
Swaps got through per vblank and TimeSplitters 2's menus measured 120-140
fps. The gate that shipped sleeps in the HLE `Swap` itself
(`xbox_Nv2aFlipGateArm`, on an event the vblank tick sets), before the
title's own Swap runs, so the fence is not involved and the guest thread
sleeps instead of spinning. `RECOMP_FPS_CAP=60` (or 30) is the strict
console cadence described below. `RECOMP_FPS_CAP=adaptive` lets a frame that
already missed a vblank present at once and holds only a frame that finished
inside the period, meant to keep a 20 ms frame at ~50 fps rather than 30.
Burnout 2's front end went from 1300 fps to a steady 60.0 with 1 ms of work
per frame under either; `[HLE-D3D8] swap timing` on the five-second report
shows the split.

**Adaptive is the default** (decided 19 Sep 2026). `RECOMP_FPS_CAP=0`
switches the gate off; `=60` or `=30` is strict. The earlier comparison
(adaptive 33 fps against 44 uncapped in Siberia) was taken while another
build shared the machine and is not evidence; it was repeated on a quiet
machine, plain lift, three scripted runs per mode, 130 s each, reading the
level phase (t = 75-125 s) of `[FPS]` and `[HLE-D3D8] swap timing`:

| Mode | Level fps, five-second windows | Gate wait / frame | Rest of frame |
| --- | --- | --- | --- |
| uncapped | 79-89, all three runs | 0.00 ms | 12.4 ms |
| adaptive | 59.9-60.2 in every window, all three runs | 3.6-3.8 ms | 12.8-13.1 ms |
| strict 60 | 59-60 with dips to 55-58 and one to 45, at the same points in all three runs | 3.8-4.0 ms | 12.8-12.9 ms |

Uncapped, the title is not paced at all: 80 presents a second from a title
built for 60. Adaptive spends the headroom on holding exactly 60. Strict's
dips are its own rule: a frame that misses its vblank by a little waits out
a whole extra period, which is what a console does and looks like a stutter
here. Adaptive is therefore the default; strict stays for the title that
needs the console's exact cadence. A machine that cannot make 16.7 ms sees
no difference between adaptive and uncapped.

On hardware the flip completes at the next vblank. Reproduce that:

- `xbox_memory_layout.c` gets a flag and two calls,
  `xbox_Nv2aFlipGateArm()` / `..Release()`. While armed, `fence_mirrors_tick()`
  leaves the completed word alone.
- The HLE `Swap` arms it, right where `xbox_FpsCountSwap()` already sits
  (`hle_d3d8.c:842`), **before** `HLE_CALL_ORIGINAL`, so the title's own wait
  inside the XDK's `Swap` is the thing that blocks.
- `kernel_vblank_tick()` releases it, at the top — **before** the
  `xbox_GetConnectedInterrupt(NV2A_VECTOR)` early return at `:2256`, so a title
  that never connects an ISR is still paced.

At most one `Swap` per vblank; with `RECOMP_VBLANK_HZ=60` that is the console's
60 fps, and it costs about fifty lines, as `performance-60fps.md` estimated.

Two consequences to accept. Any *non-flip* wait on the same fence issued between
a `Swap` and the next vblank also blocks — which is honest, since on hardware
the GPU is busy until the flip. And the guest thread spins in the title's own
loop for the rest of the frame instead of sleeping; the honest fix for that is
to replace `D3D_BlockOnTime` by name and wait on an event the vblank sets, which
is a later, larger change and also lets `nv2a_ack_thread` stop burning a core
(`performance-60fps.md`, recommendation 4).

**The three crashes.** The performance doc lists three failures that appeared
only at unthrottled speed: a null indirect call that quit through
`HalReturnToFirmware` at 17 s; an integer divide by zero in
`sub_000D9370+0x4B5`, the shape of a frame-time delta that came out zero; and a
read of `0x8C00002E` in `sub_00109B40+0x25D`, a contiguous-window pointer walked
off its end during a sequential file read, at 3400 fps. Two of the three are
literally "no time passed between frames" and "the title got somewhere in one
second that the console needs thirty for". The gate removes the condition that
produced them. It does not *prove* them fixed, and the test is to re-run those
same three configurations for 120 s each with the gate on and see them not
recur; that is cheap and should be part of the stage.

### Caps other than 60, and what they mean

- **`RECOMP_VBLANK_HZ` is not a frame-rate cap.** It changes what the console
  *is*. A title that steps its logic per vblank runs slow or fast in wall-clock
  time when you change it. It is the NTSC/PAL knob (50 for PAL) and should keep
  being described that way.
- **A cap is "release the gate every Nth vblank."** `RECOMP_FPS_CAP=30` with a
  60 Hz vblank means the gate opens on every second vblank. That is exactly
  what a title experiences on hardware when it misses a frame, so a title with a
  fixed 30 Hz mode is fine and a title that steps physics per *presented* frame
  runs at half speed. It is a legitimate switch and it is not universally safe;
  say so where it is documented. `RECOMP_FPS_CAP=0` is today's uncapped
  behaviour, worth keeping for measurement. Default 1 (every vblank).
- **Arbitrary caps are not meaningful.** The guest's only clock edge is the
  vblank; a cap that is not a divisor of the vblank rate delivers an uneven
  cadence and buys nothing. Someone who wants 144 Hz smoothness wants
  `RECOMP_VBLANK_HZ=144` and the knowledge that the title's logic speeds up with
  it. Offer it; do not pretend it is a cap.
- **"Match display refresh"** should *not* drive the guest vblank. Reading the
  output's refresh (`IDXGIOutput::GetDisplayModeList`, or `EnumDisplaySettings`)
  and adopting, say, 144 Hz as the console's rate speeds the game up. Adopt it
  only when it is 50 or 60; otherwise leave the guest at 60 and let the host
  swap chain smooth the presentation.
- **Host vsync** is deliberately off: `xbox_D3D8SetPresentInterval(0)` at
  `hle_d3d8.c:348`, with a comment recording that a vsync wait on the shadow
  device throttled Burnout 2's whole loop to 27 fps — because `Present` runs on
  the guest thread. With the flip gate pacing the guest at 60 anyway, host vsync
  becomes a tearing preference rather than a throttle. Add
  `RECOMP_HLE_D3D8_VSYNC=1` for it, keep it off by default, and keep that
  comment.

---

### Future Perfect paces itself, so "uncapped" stays at 60 (October 2026)

With the frame cap off, Future Perfect's menus run at thousands of frames a
second but its levels stay at 60, because the game waits for the vertical
blank itself. Its frame clock, `sub_001538C0`, called at the top of every
frame, spins on the count its vblank callback keeps (`sub_00022020`,
`0x00563918`) until it moves, and takes the blanks that passed, 1 to 3, as the
frame's step: whole at `0x004E7940`, as a float at `0x004E7948`, again at
`0x004E7944`/`0x004E794C` for the game world (0 while paused), added to the
game-time total at `0x007497B0`. A sampling profile of a level shows 72% of
the game thread in that spin.

Letting the spin through and giving each frame a fractional step was tried
(the code is kept in the session notes, not the tree): 240 frames a second
with the game's clock at exactly 60 ticks a second, and the world and player
update still ran 58-60 times a second -- FP updates once per whole tick, so the
extra frames were the same picture again. The world tick is read hundreds of
times, some of them dividing by it or multiplying by it, so a frame with no
tick in it cannot run the update, and a frame forced to one runs every timer
fast. Smoother motion for FP is frame interpolation's job
(`frame-interpolation.md`): its registers are named, and it runs at 2x-4x
(120-240 frames shown a second) with the game at 60.

## 3. Dependencies, risk, and order

**XDK-version independence.** Both halves are version-independent. The scale
lives entirely in `src/d3d`, which never sees an XDK symbol. The flip gate lives
in the kernel and hangs off the fence mirror, whose device offsets are already
read per title out of `D3D_BlockOnTime`'s prologue rather than tabulated by XDK
build. Nothing new becomes per-version.

**What could regress Burnout 2.**

- The scale, with `RECOMP_RES_SCALE` unset, is a no-op everywhere except the
  `ScreenSize` change — which is a real behaviour change at 1x (for the better)
  and the one thing that needs an A/B at scale 1 before anything else lands.
- The flip gate changes pacing for *every* title and every screen. Burnout 2's
  front end goes from ~1750 fps to 60. That is the point, but it also changes
  how fast its loader advances and how long its attract cycle takes, and a title
  waiting on the fence for a *resource* now waits up to one vblank. Watch load
  times, the attract sequence, and the audio path, which is the subsystem most
  likely to notice a different arrival rate.
- Scaling render-target textures is the riskiest single item, because of the
  shared texture cache (section 1, point 3). It is also the one that can be
  deferred: scaling only the back buffer is correct, just less sharp in
  offscreen passes.

**Where capture/replay is the verification.** All of the resolution work.
`src/replay` replays one frame's host calls with no game running, and the
capture records viewports, render-target sizes and depth sizes in guest units,
so the same capture can be drawn at 1x and 2x and compared. That was prototyped
here and it works (below). The flip gate cannot be verified this way — no guest
runs in a replay — and needs live runs of the three crash configurations.

**Stages, in order, with sizes.**

1. **Backend scale skeleton** (~40 lines, plus the `ScreenSize` fix and
   `--scale` in the replay tool). `RECOMP_RES_SCALE`, swap chain, viewport, host
   render target / depth creation, the pre-transformed divisor, the window, the
   frame-dump size. Prototyped on this branch. Gate it behind an A/B at 1x
   first, for the `ScreenSize` change alone.
2. **Render-target textures done properly** (~30 lines). The texture cache
   learns that a scaled entry is a render target and re-creates it at guest size
   if guest bytes are ever uploaded into it. Fixes the one malformed chunk seen
   at 2x. Landing this is what makes reflections and composed passes sharp.
3. **Flip gate and `RECOMP_FPS_CAP`** (~50 lines). Then re-run the three
   unthrottled-speed crashes for 120 s each and record what happened.
4. **Present-side tidy-up** (medium). Optional host vsync switch; make
   `nv2a_ack_thread` sleep instead of `Sleep(0)` now that the gate makes its
   poll rate irrelevant; consider replacing `D3D_BlockOnTime` by name so the
   guest thread waits on an event rather than spinning.
5. **Arbitrary resolutions** (medium). `RECOMP_RES=<WxH>`, letterboxing,
   and a decision about titles with their own widescreen modes.
6. **The deferred multiplies** (blocked). Scissor rectangles, `CopyRects`, and
   downsampling a render target the title reads back. Each is a few lines *on
   the day the underlying feature exists*; none of them should be written
   speculatively now.

---

## What was measured

One 70-second Burnout 2 run (the binary the main checkout had already built),
`RECOMP_VBLANK=1 RECOMP_AC97_READY=1 RECOMP_HLE_D3D8=shadow`, with the frame
recorder on, produced eight `.d3dcap` frames of 749-867 draws each and the
frame-rate lines quoted above. The prototype scale was then built into
`d3d8_replay` and the same captures replayed at each scale:

| Frame | 1x | 2x |
| --- | --- | --- |
| Load/Save menu (18 pre-transformed draws, one 3D backdrop) | correct, 640x480 | **correct, 1280x960** — identical layout, crisper text, the photographic backdrop simply magnified |
| Same frame, with the `ScreenSize` fix reverted | correct | **every element crushed into the top-left 640x480 quadrant**, the rest black |
| Race frame, first 300 draws of 749 | correct | **correct, 1280x960** — same composition, same split viewport, visibly more geometric detail |

That is the whole argument for the mechanism, in three pictures: the program
path and the fixed-function 3D path scale with nothing but a bigger viewport,
and the pre-transformed path is the one thing that has to be told what the guest
thinks the screen is.

Cost, on an Intel integrated GPU, 20 replays of the same 867-draw frame:
**4.63 s at 1x, 4.13 s at 2x, 4.85 s at 3x** — indistinguishable, because that
frame is bound by the per-draw CPU work of the replay, not by fill. Do not
generalise it: a fill-heavy race frame with real overdraw will cost something,
and this says nothing about what. It does say the scale is not *inherently*
expensive at this draw count.

Not measured: anything about the flip gate, which needs a title build.

## Reproducing the replay A/B

```bat
rem record a few frames from a running title
set RECOMP_VBLANK=1
set RECOMP_AC97_READY=1
set RECOMP_HLE_D3D8=shadow
set RECOMP_D3D8_CAPTURE=C:\tmp\c.d3dcap
set RECOMP_D3D8_CAPTURE_SWAP=600
set RECOMP_D3D8_CAPTURE_EVERY=400
set RECOMP_D3D8_CAPTURE_MINDRAWS=20
py -3 scripts\run_and_report.py titles\burnout2\build\Release\burnout2_recomp.exe --seconds 70 --out-dir runs --tag cap

rem draw one of them twice and compare
set RECOMP_RES_SCALE=1
d3d8_replay C:\tmp\c_20006.d3dcap --out one_
set RECOMP_RES_SCALE=2
d3d8_replay C:\tmp\c_20006.d3dcap --out two_
```

A capture holds the title's own textures and vertices, so captures are game
data: keep them out of the repository.
