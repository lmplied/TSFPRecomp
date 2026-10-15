# Widescreen and internal resolution

Two display features, investigated together because they turn out to be the same
knob: both ask the host to render at a size that is not the size the title asked
for. Neither is implemented. This document says what was verified in the tree and
in the two titles' own code, what each feature costs, and — for every work item —
whether fixing it fixes every title or only this one.

**The bar for widescreen is native 16:9.** Nothing may be scaled non-uniformly:
not the world, not the HUD, not menus, fonts, sprites, full-screen quads, fades or
FMV. "The image is stretched" is a failure, not a trade-off. Every option below is
judged against that.

Everything here was read out of the tree and out of the two titles' disassembly.
Nothing was run: the machine was building and running TimeSplitters 2 for another
session throughout (eight `MSBuild.exe`, one `timesplitters2_recomp.exe`), so no
measurement would have been worth anything. Claims that need a run are marked as
such in the last section.

---

## 1. What the titles actually do

This is the part that decides most of the classification, and it is verifiable
without running anything.

### Burnout 2 has a real 16:9 mode, driven by the console flag

Burnout 2's statically-linked XAPI contains `XGetVideoFlags` at `0x000E00AF`
(`games/_pipeline/burnout2/out/disasm/asm/text.asm:294503`):

```text
0x000E00C0  push 8                    ; XC_VIDEO
0x000E00C2  call 0xe72b4              ; -> ExQueryNonVolatileSetting thunk
0x000E00CB  mov  eax, [ebp - 4]
0x000E00CE  shr  eax, 0x10            ; <-- the flags live in the HIGH half-word
0x000E00D1  and  eax, 0x5f
```

The game calls it exactly once, at `sub_00018390`
(`text.asm:12683`), and keeps bit 0:

```text
sub_00018390:
  call  0xe00af                       ; XGetVideoFlags
  movzx eax, al
  and   eax, 1                        ; XC_VIDEO_FLAGS_WIDESCREEN
  mov   [0x004d03f8], eax
```

`0x004D03F8` is read in about thirty places. The one that matters is
`sub_000B8700` (`text.asm:230194`), a setter that writes an aspect ratio into two
camera objects 0x660 bytes apart (the split-screen pair):

| widescreen | extra condition | float written | value |
| --- | --- | --- | --- |
| 0 | — | `0x3FAAAAAB` | 1.333333 (4:3) |
| 1 | `[ecx+0x6ac7c] != 2` | `0x3FE38E39` | 1.777778 (16:9) |
| 1 | `[ecx+0x6ac7c] == 2` | `0x3F638E39` | 0.888889 (16:9 halved — split screen) |

Other readers branch to entirely separate layout functions —
`sub_0001D0C6` picks `sub_0001BB40` (4:3) or `sub_0001C090` (16:9), and again
`sub_0001C610` / `sub_0001CAE0` a few instructions later. This is a genuine,
deep 16:9 mode: camera aspect plus alternative HUD and menu layout code.

There is **no in-game option**. The flag is read once during start-up and cached,
so it must be right before the title initialises and cannot be toggled at runtime.

### TimeSplitters: Future Perfect has one too, and says so (October 2026)

Future Perfect's XAPI `XGetVideoFlags` is `sub_0037D45F` (`push 8`, then
`shr eax, 0x10; and eax, 0x5f`), and its device set-up `sub_00023930` calls it
at `0x00023A91`. With bit 0 set it ORs `D3DPRESENTFLAG_WIDESCREEN` (0x10) into
the presentation flags it passes to `CreateDevice` (0x110 instead of 0x100),
writes 16 and 9 over the 4 and 3 at `0x004C0458`/`0x004C045C`, and stores 0.75
at `0x004C0460`, a squeeze its camera, HUD and menus read in over three hundred
places. Its front end's 3D room, its levels and its HUD all widen.

So a title that has a 16:9 mode can announce it, and the toolkit now listens:
the HLE reads the flag at `CreateDevice` (`xbox_D3D8SetTitleWidescreen`), and a
title that set it is only stretched at present. The 2D squeeze and Hor+ -- both
there to widen a title that has no 16:9 mode -- stand aside, because this
title's 2D is authored squeezed like its 3D. Before that, Future Perfect's
colour grading, a full-screen pass that redraws the whole frame from a copy of
it, was squeezed into the middle three quarters like a HUD element: a narrowed
copy of the picture over the ungraded frame, which showed at both sides as a
ghost. `widescreen_2d = native` asks for the same treatment for a title that
draws 16:9 without setting the flag. `RECOMP_HLE_D3D8_DUMP_PRESENTED=1` makes
the frame dumps show what reached the window, where this kind of fault is
visible, instead of the title's own-sized scene, where it is not.

#### Any shape: 21:9 and the rest (October 2026)

Those three numbers are all Future Perfect draws its shape from, so it can
draw any shape. The camera builds its horizontal angle from the two integers
every frame (tan(h/2) = tan(v/2) x 16/9, at camera `+0x380`), the HUD and
menus place themselves by the squeeze, and 27 places ask `sub_0003C420`
whether the screen is 16:9 -- comparing for exactly 16 and 9. With
`wide_fill = 1` (the menu's Widescreen: "Fill the window"), the renderer
presents the scene over the whole window, whatever its shape
(`d3d8_display_fill_now`, output shape 0 by 0, so no bars), and the title
project keeps the three numbers at the window's shape
(`xbox_D3D8FillShape`; `fp_aspect_follow` in `recomp_manual.c`, run from
the hand-written accessors `sub_0003C400`/`410`/`420`, the last answering
"wider than 4:3"). A 1680x720 window gives 7:3 and a squeeze of 0.5714, and
the game then draws 21:9 itself: its own camera and culling, so nothing is
missing at the edges; its HUD in the corners at the right proportions; its
menus centred over a wider room. The window can be resized while playing.
The scene is still the guest's 640x480 grid times the resolution scale,
stretched across: at 21:9 a scale of 4 or more keeps it sharp.

#### Changing the resolution while playing

Only the scene target and the device's own depth buffer are scaled; a
title's own render targets keep the size it asked for. So a new
`resolution_scale` (the in-game menu, `xbox_D3D8RequestScale`) is those two
made again at the new size right after a present, before the next frame's
first draw (`follow_scale` in `d3d8_device.c`). Everything else sizes itself
from the scene as it goes -- viewport and scissor through `rt_scale_x`, the
resolve and the screen copy per call, frame interpolation's second image when
it next starts. The depth surface object the title holds keeps its identity
and has its image and view traded for the new ones. Verified in Future
Perfect's 2401 stepping 1x to 4x and back mid-level: six rebuilds, the
picture and its colour grading intact, edges visibly smoother at 4x.

#### Field of view

The camera (`[player+0x8]`) keeps its angles in vertical degrees: now at
`+0x384`, the one it eases toward at `+0x388`, the zoomed one at `+0x38C`.
The game writes `+0x388` only when the view changes mode -- the zoomed angle
going into aim mode, its own 55 coming back out -- so the `fov` setting
(40 to 90, live from the menu) is put there whenever the player is not
aiming, and the game derives the rest, culling included (`fp_apply_fov`).
The weapon in hand is drawn with the same projection and changes with it.
`recomp_fov_support` is how a title project offers the setting; the menu
shows the row only then.

### TimeSplitters 2 has no 16:9 mode at all

The `ExQueryNonVolatileSetting` import thunk in TimeSplitters 2 is
`sub_001D2B06` (`games/_pipeline/timesplitters2/out/disasm/asm/text.asm:637272`).
Its complete caller list, from the disassembler's own cross-references, is six
functions, and every index they pass was read:

| Caller | Index pushed | What it is |
| --- | --- | --- |
| `sub_001CF2CC` | 7 | `XC_LANGUAGE` |
| `sub_001CF2F1` | 9 | `XC_AUDIO` (then checks `HalBootSMCVideoMode` against 3 and 6) |
| `sub_001CF335` | 0x0A | `XC_P_CONTROL_GAMES` |
| `sub_001CF2AA` | pass-through | called with 0xFF (`text.asm:627053`) and 0x11 (`:637156`) |
| `sub_0020FD1E`, `sub_00211642` | — | XNET, network settings |

**Index 8 is never passed.** TimeSplitters 2 never asks whether the console is
widescreen. Its options menu strings (`strings.json`, 0x00245294 onwards) are
"Screen Adjust", "Sound volume", "Music volume" — there is no widescreen entry,
and the XBE contains no occurrence of "widescreen", "16:9", "letterbox" or
"aspect" in any of the four languages it ships. The in-game display option the
brief asked about does not exist in this build; "Screen Adjust" is a safe-area
position nudge.

So: **Burnout 2 needs nothing but the flag. TimeSplitters 2 cannot be made
natively widescreen by the toolkit at all.** That single contrast is the whole
classification, and it is not an XDK difference — it is a title difference.

### The kernel currently tells both titles the wrong thing

`src/kernel/kernel_xbox.c:203-210` answers `XC_VIDEO` with

```c
*(PULONG)Value = XC_VIDEO_FLAGS_WIDESCREEN | XC_VIDEO_FLAGS_HDTV;   /* 0x03 */
```

and `src/kernel/kernel.h:1087-1090` defines those as `0x01` and `0x02`. Those are
the values XAPI returns *after* `shr eax, 0x10`. Written into the raw EEPROM
dword they are in the wrong half-word, so Burnout 2's `XGetVideoFlags()` computes
`0x03 >> 16 & 0x5F` = **0**. Today the comment says "widescreen and HDTV support
enabled" and the title receives no widescreen, no HDTV and not even the 60 Hz bit.

The correct raw values, read straight off the mask XAPI applies (bit *n* after the
shift is bit *n+16* before it, and `0x5F` keeps bits 0-4 and 6):

| Raw `XC_VIDEO` bit | XAPI bit | Meaning |
| --- | --- | --- |
| `0x00010000` | 0x01 | widescreen |
| `0x00020000` | 0x02 | 720p |
| `0x00040000` | 0x04 | 1080i |
| `0x00080000` | 0x08 | 480p |
| `0x00100000` | 0x10 | letterbox |
| `0x00400000` | 0x40 | 60 Hz |

The second, independent route agrees already: `xbox_AvSendTVEncoderOption`
(`src/kernel/kernel_hal.c:464-469`, `:476-481`) returns `AV_FLAGS_WIDESCREEN`
(0x10, correct in that encoding) for both capability queries. So the tree says
"widescreen" on the AV-pack path and "not widescreen" on the EEPROM path, and the
EEPROM path is the one titles use. `RECOMP_XBOX_REGION` does not reach `XC_VIDEO`;
there is no widescreen env var; `ExSaveNonVolatileSetting` discards writes
(`kernel_xbox.c:340-353`), so a title cannot change it either.

`D3DPRESENT_PARAMETERS.Flags` at +40 — where a title announces
`D3DPRESENTFLAG_WIDESCREEN` — is never read: `shadow_create` copies +0, +4, +32
and +36 only (`src/hle/hle_d3d8.c:325-334`). No `D3DPRESENTFLAG_*` constant is
defined anywhere in the tree.

### Xbox widescreen is anamorphic, and that is what makes "native" cheap

Burnout 2 in 16:9 still presents a 640x480 frame buffer. It does not widen the
buffer; it widens the *camera aspect* to 1.7778 and lets the TV unsqueeze 640x480
across a 16:9 screen. The image in the buffer is horizontally compressed on
purpose, and the HUD is authored compressed to match.

This is the key to the no-stretch bar. The host does not have to resample that
buffer. Both host draw paths end in normalised coordinates:

- **vertex programs** — the title's program leaves `oPos` in render-target pixels
  and the generated host program undoes it (`src/d3d/d3d8_vsh.c:698-700`), giving
  clip space;
- **fixed-function 3D** — `SetTransform` is forwarded and the host does the
  transform, also giving clip space;
- **pre-transformed (XYZRHW)** — the generated shader divides by the *guest*
  presentation size (`src/d3d/d3d8_shaders.c:157-158`, constants at `:944-945`),
  mapping the guest's 640x480 onto the whole viewport.

Rendering those into a 16:9 host target with square pixels **re-rasterises** the
anamorphic content at the right shape. Nothing is stretched because nothing is
resampled: the compression the title applied and the unsqueeze the viewport
applies cancel exactly, at whatever resolution you like. The 2D layer un-squeezes
correctly for the same reason, and it needs no change, because the title authored
it in the same anamorphic space.

For a title *without* a 16:9 mode the same arithmetic works against you: its 4:3
projection and its 4:3-authored HUD both map onto a 16:9 viewport and both come
out stretched. That is the case that has no toolkit answer.

---

## 2. The earlier design document, checked against today's tree

`docs/technical/resolution-and-framerate.md` designed the resolution-scale
feature. Re-read against the code:

**Still true.**

- 640x480 is not a constant anywhere; it is the title's own present parameters,
  read at `hle_d3d8.c:325-331`, with 640x480 only as a fallback and again as the
  swap-chain default at `d3d8_device.c:268-269`.
- `src/hle` speaks guest pixels throughout and should keep doing so.
  `g_target_width/height` is the *current guest render target's* size, set from
  `surface_measure` at `hle_d3d8.c:1741-1742`, and it drives both the `INT_MAX`
  viewport clamp (`:1488-1495`) and the screen-space undo constants
  (`shadow_viewport_constants`, `:274-288`, halves of the guest target). Grow only
  the host viewport and the program path scales for free. That claim holds.
- Textures need no half-texel correction; clears carry no coordinates
  (`hle_d3d8.c:903` passes count 0 and no rects).
- Offscreen targets and their depth surfaces are created at guest size
  (`scratch_target` / `depth_surface`, `hle_d3d8.c:1590-1628`) and must scale
  together — D3D11 requires the depth surface to match the target exactly.
- The render-target texture cache hazard is unchanged: `hle_d3d8_texture.c:61-65`
  keeps a `rendered` flag meaning "never re-upload", and a texture that is both
  rendered into and refilled from guest memory would have a scaled host copy and a
  guest-sized upload.
- The frame dump still writes `g_shadow_width * g_shadow_height`
  (`hle_d3d8.c:709-710`) and would truncate at any scale.
- Scaling in `src/d3d` rather than `src/hle` is still the right layer, for the
  reasons given there.

**Stale.**

- *"The fix is one line in each place: divide by the guest presentation size"* —
  **already done.** `xbox_D3D8SetGuestSize` exists (`d3d8_device.c:166-169`), the
  HLE calls it from `shadow_create` (`hle_d3d8.c:360`), and both constant-buffer
  fills use `d3d8_GetGuestWidth/Height()` (`d3d8_shaders.c:944-945`, `:976-977`).
  The "every 2D element crushed into the top-left quadrant at 2x" failure the
  document photographs cannot happen now, and the A/B at 1x it asks for as a gate
  is no longer needed — that behaviour change has already shipped.
  One caveat: `d3d8_GetGuestWidth()` falls back to `g_device_state.width` when
  unset (`d3d8_device.c:171`), so any path that scales the back buffer without
  calling `xbox_D3D8SetGuestSize` — the replay tool, `d3d8_gl.c`, a future
  non-shadow mode — silently gets the old broken behaviour back. Make the guest
  size explicit rather than defaulted.
- *"Nothing in this tree sets a scissor rectangle... both will need the same
  multiply on the day they land"* — **that day has arrived.**
  `D3DDevice_SetScissors` is replaced (`hle_d3d8.c:1509-1531`),
  `xbox_D3D8SetScissors` stores render-target pixels (`d3d8_device.c:186-207`,
  and its own comment says they will need scaling), and `d3d8_states.c:410-424`
  applies them raw. TimeSplitters 2 uses one for its mission-briefing text, so
  this is a live blocker, not a deferred one.
- *"Anything that reads pixels back"* — **not a problem in shadow mode.** The
  complete list of replaced functions is 52 (`grep -c '^HLE_EXPORT(' src/hle/*.c`)
  and contains no `LockRect`, `GetRenderTarget`, `GetBackBuffer` or `CopyRects`.
  The title's own D3D8 body still runs and still reads its own frame buffer out of
  guest memory, which the host never writes. The only host readback is the frame
  dump. This becomes real only when D3D8 is HLE'd outright and no title body runs.
- *"`RECOMP_RES_SCALE=<1..8>`, an integer multiplier... Integer keeps the aspect
  exact"* — **the wrong representation once widescreen exists.** An integer
  multiple of 640x480 is always 4:3, and 16:9 needs a host target whose shape
  differs from the guest's. See R6.
- The document's suggestion of `titles/<name>/recomp.ini` predates
  `src/input/input_bindings.c`, which now does exactly the "small loader, three
  places to look, no new mechanism" job it describes. Reuse that instead.

The framerate half of that document is unaffected by anything here.

---

## 3. Work items

### Resolution scaling

**R1. Scale in the backend.** `src/d3d` learns a host target size derived from the
guest size. Touches: the swap chain (`d3d8_device.c:266-269`), the default RTV and
device depth (`:313-353`), the initial viewport (`:1696-1703`), `dev_SetViewport`
(`:1367-1383`), render-target and depth-surface creation, and the shadow window
(`shadow_window`, `hle_d3d8.c:209-229`). `src/hle` changes not at all. Unset must
be byte-identical to today.

**R2. Multiply scissor rectangles.** `xbox_D3D8SetScissors` converts guest
render-target pixels to host pixels. Four lines, and it is a *correctness* blocker
at any scale above 1, not a polish item.

**R3. Scale offscreen targets with their depth.** `scratch_target`,
`depth_surface`, `hle_d3d8_render_texture` and the cube path. Must move together;
the backend already rejects a mismatch. Then teach the texture cache that a scaled
entry is a render target and re-create it at guest size if guest bytes ever arrive
(the one malformed chunk the earlier prototype saw at 2x).

Whether a title's own render-to-texture passes should scale: **yes, by default**,
because leaving them at guest size makes every reflection and composed pass a
magnified 640x480 patch inside a sharp frame, which looks worse than not scaling
at all. The exception is a target the title later reads back — none in shadow
mode, see R5.

**R4. Size-aware frame dump and capture.** The dump must use the host size; the
capture header (`d3d8_capture.h:143`) must record the **guest** size so a capture
stays scale-independent and can be replayed at any scale. That is what makes
capture/replay the A/B rig for this whole feature.

**R5. Downsample on readback.** Nothing needs it today. Write it on the day a
lockable host render target exists, not before.

**R6. Representation.** Recommend an explicit host target computed from three
inputs — guest size, aspect mode, and a scale — rather than a bare integer
multiplier:

```text
host_h = guest_h * scale
host_w = round(host_h * output_aspect)      /* 4/3, or 16/9 in widescreen */
```

`RECOMP_RES_SCALE=<n>` stays as sugar for `scale`, which keeps the simple case
simple and the pixel grid aligned in 4:3. But the multiplier alone cannot express
widescreen, and the two features must not end up with two size mechanisms.

**R7. Where the setting lives.** Beside input. `src/input/input_bindings.c:568-602`
already searches `RECOMP_INPUT_CONFIG`, then `%APPDATA%\xboxrecomp\`, then beside
the executable, and already parses JSON. A `display` section in the same file (or
a sibling module reusing the same loader) costs almost nothing and honours the
earlier document's rule against inventing a second configuration mechanism. Keep
environment variables as the override layer — the runtime is `getenv`-driven
throughout, and the widescreen flag in particular is read by the kernel before any
D3D exists.

### Widescreen

**W1. Fix the `XC_VIDEO` encoding.** Shift the flags into the high half-word so
XAPI's `>>16 & 0x5F` recovers them. Without this nothing else in this section can
work, and today the tree is silently lying to every title.

**W2. Make widescreen a setting.** The flag is read once during title start-up, so
it must be resolved before the guest's first kernel call and cannot be changed at
runtime. Default should be off (4:3): a title told "widescreen" while the host
presents 4:3 looks worse than one told the truth.

**W3. A 16:9 host target, guest size unchanged.** Exactly R1's mechanism with
`output_aspect = 16/9`. `xbox_D3D8SetGuestSize` keeps saying 640x480 so the
pre-transformed divide keeps un-squeezing the title's anamorphic 2D layer. For
Burnout 2 with W1+W2+W3 this is native, unstretched 16:9 and nothing else is
needed.

**W4. Pillarbox titles with no 16:9 mode.** Two halves that classify differently:

- *the mechanism* — a centred 4:3 host viewport inside a 16:9 target, plus
  **rect-limited clears**. `dev_Clear` (`d3d8_device.c:590-623`) ignores `Count`
  and `pRects` entirely and clears the whole view, so the title's own clear would
  paint over the pillar bars every frame. Needs `ClearView` with a rect, or a
  scissored full-screen quad.
- *knowing which titles need it* — see the table.

**W5. Hor+ projection hack.** Widen horizontal field of view and keep vertical, so
more of the scene is visible rather than the same scene distorted.

For a **fixed-function** title this is mechanical: scale `[0][0]` of
`D3DTS_PROJECTION` by `(4/3)/(16/9)` = 0.75 inside `dev_SetTransform`
(`d3d8_device.c:625-633`). For a **vertex-program** title — which is what both
titles in this tree are — there is no hook. The projection lives in constant
registers the title chooses, usually pre-multiplied into a composite
world-view-projection matrix, uploaded through
`SetVertexShaderConstant{1,4,NotInline,NotInlineFast}`. Finding it means knowing
the title's own register convention.

The one generic lever that *looks* like it might serve is the screen-space undo
(`d3d8_vsh.c:698-700`, constants from `shadow_viewport_constants`). It cannot:
that runs after the title's program has already projected, so scaling x there
shrinks or stretches the finished image. **Post-projection scaling cannot add
field of view.** It can only produce a squeeze — which is the stretched-image
failure, wearing a different sign.

And even where W5 works, it widens the world and leaves the pre-transformed layer
at 4:3, so the HUD stretches. It does not meet the bar on its own.

**W6. Split the viewport by draw class.** The pieces already exist:
`d3d8_fvf_transformed(fvf)` classifies draws (`d3d8_shaders.c:934`) and the HLE
already switches host viewport per class — whole target for program draws, the
title's own viewport for fixed-function and pre-transformed
(`shadow_can_draw`, `hle_d3d8.c:644` and `:650`). So "3D across the full 16:9
viewport, pre-transformed inside a centred 4:3 sub-viewport" is cheap to build.

It gives an unstretched world *and* an unstretched HUD for a 4:3-only title. What
it cannot do is tell a HUD element from a full-screen element. Fades, letterbox
bars, loading backdrops and FMV are all pre-transformed too, and confining them to
the centre 4:3 leaves the sides of the screen unfaded, unbarred, or showing the
previous frame. Deciding which is which is per-title knowledge.

**W7. Lock the window aspect, or letterbox at present.** The swap chain is
`DXGI_SWAP_EFFECT_DISCARD` (`d3d8_device.c:279`) and DXGI stretches the back
buffer to the client rectangle, so a user-resized window at the wrong aspect
stretches non-uniformly — a failure of the bar caused by the window manager rather
than by any title. Either constrain the window (`WM_SIZING`) or present into a
centred, aspect-correct sub-rectangle.

**W8. The guest-framebuffer window.** `src/video/video_pump.c` is the other
display path (`RECOMP_FB_WINDOW`); it has the same aspect question and none of the
above applies to it.

**W9. Make the AV-pack capability queries agree.** `kernel_hal.c:464-469` and
`:476-481` advertise widescreen unconditionally. They should follow W2, or a title
that queries both routes sees a console that contradicts itself.

**W10. Read `D3DPRESENT_PARAMETERS.Flags` (+40).** A title that sets
`D3DPRESENTFLAG_WIDESCREEN` is announcing its own intent. Reading it turns "does
this title have a 16:9 mode" from a hand-maintained table into something the
runtime observes for itself, at least for the titles that set it. *Inference: I
have not verified that either title in this tree sets it, and there is no
`D3DPRESENTFLAG_*` constant in the tree to compare against.*

---

## 4. Classification

| # | Work item | Toolkit or game-specific | Why |
| --- | --- | --- | --- |
| R1 | Scale the host target in `src/d3d` | **Toolkit** | Every guest-pixel-to-host-pixel conversion already lives in one module, and it never sees an XDK symbol. |
| R2 | Multiply scissor rectangles | **Toolkit** | `SetScissors` carries render-target pixels for every title; the conversion is arithmetic. |
| R3 | Scale offscreen targets, RT textures, cube faces with depth | **Toolkit** | NV2A surface rules, identical for all titles; the depth-must-match constraint is D3D11's. |
| R4 | Size-aware frame dump; guest-sized capture header | **Toolkit** | Diagnostics in engine code; no title is involved. |
| R5 | Downsample a host target read back at guest coordinates | **Toolkit, deferred** | No replaced function reads host pixels today; it becomes real only under full D3D8 HLE. |
| R6 | Explicit host target size, not an integer multiplier | **Toolkit** | A single size policy shared by both features; an integer multiple of 640x480 is always 4:3. |
| R7 | Display settings beside the input config | **Toolkit** | One loader, one file, one precedence order for all titles. |
| W1 | `XC_VIDEO` flags in the high half-word | **Toolkit** | XAPI's `shr 16 / and 0x5F` is library code every title links; the value is wrong for all of them equally. |
| W2 | Widescreen as a setting, resolved before title start-up | **Toolkit** | One kernel value, read identically by every title, once. |
| W3 | 16:9 host target with the guest size preserved | **Toolkit** | Both 3D paths reach clip space and the 2D path divides by the guest size, so re-rasterising at 16:9 is title-independent and resamples nothing. |
| W4a | Pillarbox mechanism: centred viewport + rect-limited clear | **Toolkit** | Viewport and clear are engine state; `dev_Clear` ignoring `pRects` is an engine bug. |
| W4b | Knowing a title has *no* 16:9 mode | **Game-specific** | Not derivable in general. TS2 proves it here by never reading `XC_VIDEO`, but absence of a read is evidence, not proof, and a title could gate 16:9 on something else. |
| W5 | Hor+ projection hack, fixed-function titles | **Toolkit (narrow)** | `SetTransform(D3DTS_PROJECTION)` is a named, intercepted call; scaling `[0][0]` by 0.75 is mechanical. Applies only to titles that use the fixed-function transform. |
| W5b | Hor+ projection hack, vertex-program titles | **Game-specific** | The projection is in title-chosen constant registers, usually as a composite WVP; the only generic hook is post-projection, and post-projection scaling cannot add field of view. Both titles here are in this class. |
| W6a | Per-draw-class viewport split (3D wide, 2D centred 4:3) | **Toolkit** | The engine already classifies pre-transformed draws and already switches viewport per class. |
| W6b | Deciding which pre-transformed draws are HUD and which are full-screen | **Game-specific** | A fade, a letterbox bar and a health meter are the same kind of draw; only the title knows which must span the screen. |
| W7 | Window aspect lock / letterboxed present | **Toolkit** | DXGI stretches to the client rect regardless of which title is running. |
| W8 | `video_pump` framebuffer window aspect | **Toolkit** | A second display path with the same property. |
| W9 | AV-pack capability queries follow the widescreen setting | **Toolkit** | One kernel answer; consistency between two routes into the same setting. |
| W10 | Read `D3DPRESENT_PARAMETERS.Flags` | **Toolkit** | Turns part of W4b into something the runtime observes rather than a table — for the titles that set it. |
| — | Burnout 2: nothing | **Game-specific, and already done by the title** | W1+W2+W3 is the whole job; its camera aspect, HUD layout and split-screen halving are all its own code. |
| — | TimeSplitters 2: native 16:9 | **Game-specific, and not achievable** | No 16:9 mode, no console-flag read, vertex-program projection. Correct answer is pillarboxed 4:3 (W4). |

Three sentences, since the brief asked for them plainly:

- **Titles that need nothing but the flag:** those with a real console-flag 16:9
  mode. Burnout 2 is one. W1+W2+W3 and they are natively widescreen.
- **Titles that need the projection trick:** those with no 16:9 mode but a
  fixed-function transform. Neither title here qualifies; for a vertex-program
  title the trick is per-title work, and even then it leaves the 2D layer wrong.
- **Titles that cannot be done natively at all:** 4:3-only vertex-program titles
  with a 4:3-authored 2D layer. TimeSplitters 2 is one. They get pillarboxed 4:3,
  which is correct and never stretched, and is not widescreen.

---

## 5. Recommended order

1. **W1** — fix the `XC_VIDEO` encoding. Smallest change here, and it is a bug
   whether or not either feature is built. Do it alone, with a note that
   `XGetVideoFlags` returned 0 before it. Burnout 2's behaviour will change the
   moment it lands, so land it by itself.
2. **R6 + R1** — the size policy and the backend scale, together, since R6 is the
   shape of R1's interface. `RECOMP_RES_SCALE=1` and 4:3 must be identical to
   today. Verify with capture/replay: one recorded frame drawn at 1x and 2x.
3. **R2** — scissor multiply. Small, and R1 is wrong without it wherever a title
   uses one. TimeSplitters 2's briefing screen is the test.
4. **R4** — dump and capture sizes, so the verification rig itself is honest at
   scale.
5. **W2 + W3** — the widescreen setting and the 16:9 target. This is where
   Burnout 2 becomes natively widescreen, and it is the first point at which
   anything is worth looking at.
6. **R3** — offscreen and render-target-texture scaling, with the cache fix. This
   is what makes reflections and composed passes sharp rather than magnified.
7. **W7 + W4a** — window aspect and rect-limited clears, so a 4:3-only title in a
   16:9 window is pillarboxed rather than stretched. This is the default every
   title falls back to, so it should exist before the feature is offered.
8. **W9, W10, W8** — consistency and the second display path.
9. **W5 / W6** — only if a specific title is worth it, and only as a per-title
   option that is off by default. Do not put either on the generic path.
10. **R5** — when something reads host pixels back.

---

## 6. Risks

- **W1 changes Burnout 2 the day it lands.** It switches the title into a code
  path — camera aspect, menu and HUD layout — that has never run in this project.
  Expect new unresolved indirect calls in `sub_0001C090` / `sub_0001CAE0` and the
  other 16:9 branches, since function discovery only ever saw the 4:3 side. That
  is a feature, not a problem: it is exactly the kind of coverage a second code
  path buys. But it means W1 and W2 must ship together, with widescreen defaulting
  off, so the change is opt-in.
- **The flag is latched at start-up.** No runtime toggle is possible for Burnout 2
  without restarting. Any UI must say so.
- **Scaling render-target textures is still the riskiest single item**, for the
  cache reason in R3. It is also the one that can be deferred: scaling only the
  back buffer is correct, merely less sharp offscreen.
- **`d3d8_GetGuestWidth()`'s fallback** (`d3d8_device.c:171`) hides a scaled
  back buffer with no guest size behind old behaviour instead of failing. Any
  path that scales must set the guest size explicitly.
- **Presentation code is moving.** `src/d3d/d3d8_device.c`, a new
  `src/d3d/d3d8_overlay.*` and `src/hle/hle_dsound_stream.c` are being edited
  concurrently in the main checkout. R1, W3 and W7 all touch device creation and
  `Present`; sequence them after that work settles.
- **The Vulkan move.** R1 and W3 are design points rather than code the Vulkan
  backend inherits, but W7's letterbox and Vulkan's negative-viewport Y-flip touch
  the same state. `d3d8_states.c` already carries one hand-annotated winding fix;
  do not stack a second.
- **Fill cost is unmeasured.** The earlier prototype's "4.63 s at 1x, 4.13 s at
  2x" was a CPU-bound 867-draw replay frame and says nothing about a fill-heavy
  race frame. TimeSplitters 2 currently holds 60 fps with roughly 13 ms of work
  per frame; 2x is four times the pixels and the headroom is not obviously there.

---

## 7. What could not be determined without running

- **Whether Burnout 2 renders correctly in its 16:9 path.** Everything above is
  static: the flag is read, the aspect is 1.7778, thirty call sites branch. Nobody
  has executed those branches in this project.
- **Whether either title sets `D3DPRESENTFLAG_WIDESCREEN`,** and whether Burnout 2
  changes its requested back-buffer size in widescreen. The present-parameters
  block is filled at runtime; the disassembly would answer it but I did not trace
  the fill, and the `Direct3D_CreateDevice` symbol address in
  `default_xdk_symbols.json` did not resolve to a function start. The run log line
  "shadow device 640x480, from the title's own CreateDevice parameters" is the
  cheap check, with widescreen on.
- **The real cost of 2x and 3x** in a fill-heavy frame, for either title.
- **Whether the pre-transformed viewport treatment is right at all.** Pre-transformed
  draws currently take the title's viewport (`hle_d3d8.c:650`) *and* divide by the
  guest presentation size, so a title that sets a half-screen viewport and draws a
  full-width 2D quad gets it squeezed into that half rather than clipped by it.
  Both titles look right today, so either neither does this or it cancels; W6 would
  build directly on this code and should not be built until it is understood.
- **Whether TimeSplitters 2's "Screen Adjust" interacts with a pillarboxed
  viewport.** It nudges a safe-area offset; whether that offset is applied in guest
  pixels before or after the viewport was not traced.
