# xboxrecomp fork — working context

Fork of `sp00nznet/xboxrecomp`. Goal: static recompilation of original Xbox titles into
native executables, generalised beyond the single proven target.

This file lives at the repo root; Claude Code reads it automatically each session.

---

## The framing to hold onto

**The performance win comes from HLE at the D3D8 boundary — not from lifting CPU code, and
not from the graphics backend.**

Guest is x86-32 little-endian, host is x86-64 little-endian. No byte swapping, 8 guest GPRs
into 16 host GPRs, guest EFLAGS semantics *are* host EFLAGS semantics. Same-ISA translation is
already near-native, and a 733 MHz Coppermine P3 leaves enormous headroom. Frame time goes into
NV2A work — push-buffer parsing, PGRAPH state decode, texture cache invalidation, write-watch
traps — none of which static CPU recompilation touches.

Consequences:

- Recompiled output will **not** beat xemu on a desktop x86 machine unless D3D8 is HLE'd.
- Naive lifted C can be *slower* than a tuned JIT. There is no deficit to start from, so
  codegen quality has to be good just to break even.
- The genuine wins are: HLE'ing D3D8, ARM hosts (where x86-32 → ARM64 is no longer same-ISA
  and lifting genuinely pays), moddability, and portability once the memory model is fixed.

**Why HLE generalises here but not on the 360:** there is no `d3d8.dll` on the Xbox — D3D8 is
statically linked into the XBE, and it is a known, versioned library. Cxbx-Reloaded has spent
fifteen years building OOVPA signature databases identifying it *per-XDK-build*. So the HLE
boundary is per-XDK-version, not per-game, and there is a manageable number of those.

---

## Corrections to the upstream README

Verify against the repo rather than trusting the README. Known discrepancies as of August 2026:

| README says | Reality (per `docs/technical/gap-analysis.md`) |
|---|---|
| NV2A push-buffer interception is a core feature | Push-buffer parsing is a **stub**, marked "N/A — D3D8 API intercept instead". The project already does D3D8 HLE. |
| "115 of 366 ordinals resolved, 55 bridged" | Do not trust any number written down; the useful question is per-title, not global. Run `py -3 -m tools.kernel_audit.coverage <analysis.json> --list`, which splits what is missing into "needs a bridge wrapper", "is a data export", and "does not exist yet". Note an ordinal with an `xbox_*` implementation but no bridge silently returns 0. |
| Portable C output targeting ARM, RISC-V, WASM | Memory model uses `CreateFileMapping` + fixed-address `MapViewOfFileEx` at guest VAs. **"Win32-only in practice" is out of date (Sep 2026):** `src/platform/win32_compat.c` implements those on POSIX, with `MAP_FIXED_NOREPLACE` and a returned-address check, because `xbox_memory_layout.c` depends on a failed placement failing. What is untested is whether the 28 mirror views and the apertures actually place on Linux — no title has ever linked there. The lifted output is portable too: `templates/runtime/recomp_types.h` guards every x86 intrinsic and keeps the MMX/SSE helpers as lane-wise C. See `docs/technical/vulkan-backend.md` §6.5 for the rest of the Linux/Android gap. |
| Burnout 3 is the proven target | True, and it is a **D3D8LTCG** build on XDK 5849 — so LTCG is not disqualifying. |

**Corrected Sep 2026:** that "17 of 66 formats, mipmap level 0 only, no P8 palette, no
texture coordinate generation" line described the *software rasteriser*, not the texture
layer. `src/d3d/d3d8_resources.c` maps ~120 `D3DFMT_` enumerators, builds full mip chains,
handles cube and volume textures, and bakes P8 through the stage palette;
`src/d3d/d3d8_shaders.c` does texture coordinate generation. `src/d3d` is 12.5k lines and
is the renderer. The generalisation blocker is not the texture layer.

**The real blocker was that `src/hle` could not see `src/d3d`.** It can now (Sep 2026):
`xbox_hle` links `xbox_d3d8` on Windows, and `hle_d3d8.c` replaces 40 XDK functions by
name. Each runs the title's own body first (`HLE_ORIGINAL`), and with
`RECOMP_HLE_D3D8=shadow` repeats the call on a host device in a second window.

**Shadow mode draws the title** (Sep 2026): Burnout 2's logos, menus, loading screens, HUD
and race, with its own vertex programs and register combiners. See
`docs/technical/shadow-mode.md` for what reaches the host and from where, the switches, and
the gaps that remain (cube render targets, `CopyRects`, lighting). A path for titles that
fill push buffers through `BeginPush` is still the next piece of work: those draws reach no
replacement at all.

**Second title (Sep 2026): TimeSplitters 2, XDK 4721.** Boots through XAPI start-up,
DirectSound and its first frames with **no overrides**; see
`docs/technical/second-title-bringup.md` for the six things it proved were wrongly
universal (retail disc check, GPU time fence offsets, DSP doorbell, the APU's physical
view, two template gaps) and what is next. Its draws reach the shadow renderer and are
skipped: 4721 loads vertex programs through `LoadVertexShaderProgram` /
`SelectVertexShaderDirect`, which `src/hle` does not replace yet, so no program is ever
known. Both titles need `RECOMP_VBLANK=1 RECOMP_AC97_READY=1` to pace and to get through
audio init; the DSP doorbell is found from the scratch page table, so `RECOMP_APU_DSP_ACK`
is only an override now.

**Update, 18 Sep 2026 evening:** the 4721 shader paths are replaced (`LoadVertexShaderProgram`,
`SelectVertexShaderDirect`, pointer-form pixel shaders), and with a scripted menu path
(`RECOMP_INPUT_SEQ`, see the bring-up doc) TimeSplitters 2 loads the Siberia level and renders
its opening cutscene, at 85-145 fps uncapped. Not yet playable: the level is dark and ~15% of
in-level draws are skipped as "program without layout". Two of the three blockers on the way
were not the title: the entry profiler at a 100-call interval (now capped at one report a
second) and a switch table the disassembler never measured (`resync_jump_tables()` now runs
before every function rebuild). When a title "hangs" after drawing, suspect the diagnostics
first, and when it spins on an `[ICALL] unknown target` a few bytes past a function end, look
for a `jmp [reg*4 + table]` just before it.

**Update, 19 Sep 2026:** TimeSplitters 2 plays. The dark level was stale blend and depth
state (`SetRenderState_Simple` is now stored), the black story intro was a DirectSound
stream status word (`DSSTREAMSTATUS_PLAYING` is 0x10000), the last unresolved indirect
call was a switch arm the translator lost after a re-sync, and both titles are lifted
without `--trace-all-entries`. The level runs 80 fps uncapped on this machine, so the
**flip gate is on by default in adaptive mode** (`RECOMP_FPS_CAP=0` to switch it off,
`=60`/`=30` for the strict console cadence); the measurements behind that are in
`docs/technical/resolution-and-framerate.md`. The performance plan in
`docs/technical/ts2-performance-plan.md`, items 1-7, took the level from 12.4 ms a frame to
5.0 ms (branch `perf/ts2-items-1-7`). From the user's first pad session: `D3DDevice_SetScissors`
is forwarded (the briefing text was spilling out of its box), and closing the game window
exits the process. **F9 shows the frame rate on screen and F10 steps the frame cap**
(adaptive/60/30/off) while a title runs; `RECOMP_FPS_OVERLAY=1` starts with the counter on.
**The built executable runs the game on its own** (Sep 2026): double-click it, or shortcut it
from anywhere. It looks for the game in a `game` folder beside itself, then at `games/<title>/`
relative to itself, then `RECOMP_GAME_DIR`; and the vblank, the emulated audio hardware and the
shadow renderer are **on unless turned off** (`RECOMP_VBLANK=0`, `RECOMP_AC97_READY=0`,
`RECOMP_HLE_D3D8=off`; `xbox_EnvSwitch` reads them). The `run.bat` files are debugging launchers
now, not a requirement. It is a windowed program, so a double-click gives it no console:
diagnostics go to whatever redirected them, else to the terminal it was started from, else to
`<executable>.log` beside it (`setup_output` in the template). Note the switch a title needs is read in **five** places including the
title's own `main.c` (from `templates/new-game/src/main.c`), so change the template and
regenerate with `scripts/regen_title_main.py`.

**F1 opens the in-game settings menu** (or Back+Start on a pad): display, mouse and key
bindings, live where the runtime can change them, saved to the launcher's files
(`docs/technical/in-game-menu.md`). Game input is suspended while it is open.

**F11 captures the frame on screen** -- a BMP and a replayable capture, beside the executable --
which is how a player reports a rendering bug that only happens somewhere specific.
`RECOMP_INPUT_SEQ` can now hold stick directions (`lstick_up`, `rstick_left`, ...) as well as
buttons, and `RECOMP_INPUT_LOG=1` prints what the title actually reads from the pad.

**Audio de-sync was dropped sound, not a clock (19 Sep 2026).** The host audio queue held four
submissions and a DirectSound stream was feeding it a tick's worth at a time against a 400 ms
lead, so most submissions were refused and the stream stepped over them: TimeSplitters 2's
cutscene lost 42 seconds of music in two minutes. A refused chunk is now retried, the chunks
are whole, the queue is deeper, and a stream's packets complete when the host has actually
played them (`RECOMP_DSOUND_WALLCLOCK=1` for the old wall clock). If a title's sound ever
drifts again, read `[audio-output] dropped=` and the `[DSOUND] ... from the host's play
position` line before suspecting anything else.

**Broken, looping dialogue was a flush nobody heard (15 Oct 2026).** Future Perfect stops a
stream with `Pause` then `CDirectSoundStream_FlushEx` (ASYNC) before every new line or track;
only `Flush` was replaced, so the old sound's packets stayed queued here, played ahead of the
new one, were re-read in its format, and every completion after that matched the title's packet
one later than it meant -- it refilled memory not yet played. `FlushEx` is replaced now. To
look at sound without listening: `RECOMP_AUDIO_DUMP=<folder>` writes every host voice's audio
as WAVs plus a submissions CSV, and `RECOMP_DSOUND_TRACE=1` logs every stream packet (with a
checksum), completion, flush, pause, format and status change, and every buffer Play/Stop, on
the input script's clock. A title whose sound breaks up: trace it and check that no packet's
memory is submitted again while it is still queued.

**Frame interpolation needed three things a title can hide (15 Oct 2026).** First, the redraw
did not keep vertex declarations: Future Perfect draws one program with several vertex layouts
in a frame, and in-between frames read every draw with its program's last layout, so the world
vanished and the sky flickered alone. Declarations are now in the snapshot and kept on every
change -- any host call that changes state must be kept, or the redraw is wrong. Second, FP draws
most of its world with the projection already multiplied into the model-view (`c68`-`c71`), and
never reads the projection register: those draws looked 2D to the interpolator and were not
blended, so the world did not turn in the in-between frames. A group whose last row is not
`(0,0,0,1)` is now unprojected with the projection register, blended, and multiplied back. Third,
FP waits for the vblank in its own loop, so under the adaptive gate Swap never waited and no
in-between frame was drawn: a title that paces itself calls `xbox_Nv2aFlipGateSetTitlePaced(1)`,
and while interpolation is on its Swap is held to the next vblank. To check another title:
`[INTERP]` reports "0 in-between shown" (the pacing) or in-between frames whose world matches the
newer frame exactly (`RECOMP_INTERP_DUMP` beside `RECOMP_HLE_D3D8_DUMP_EVERY`: the registers);
an in-between frame missing what the real one has: `RECOMP_INTERP_DRAWS=<n>` against
`d3d8_replay --draws <n>` on a capture of the same frame finds the draw.
`docs/technical/frame-interpolation.md` has the details.

**A mouse aim that steers by feedback fails with a real mouse (15 Oct 2026).** FP's first 1:1
aim wrote the look vector that would turn the view onto a target, learning the gain from the
view matrix; scripted counts were fine, a real mouse made it oscillate and its wrong-way guard
reverse axes at random. It now adds the counts to the player's own yaw/pitch ([player+0x100],
[player+0x104], degrees), found by correlating a per-frame record of the player struct with the
view (`RECOMP_FP_SCAN`). For another title: find the angles and write them; do not close a loop
over the game's turning.

**Widescreen and internal resolution** are investigated in
`docs/technical/widescreen-and-resolution.md`, with every work item classified toolkit or
game-specific. Two things to know before touching either: `XGetVideoFlags` returns 0 today
because the kernel puts the video flags in the low half-word and XAPI reads the high one, and
Xbox widescreen is anamorphic, so a title that has a 16:9 mode needs no stretching anywhere
while a title that has none (TimeSplitters 2) cannot be given one natively.

**Frame interpolation (Oct 2026):** `RECOMP_FRAME_INTERP=2` (launcher row "Frame
interpolation") shows twice the frames while the title still runs at 60. The newest frame's
host calls are kept and drawn again in the flip gate's wait with their matrix constants
blended toward the frame before (`src/hle/hle_d3d8_interp.c`). A title says which registers
hold its projection and affine matrices with `xbox_D3D8SetInterpRegisters`; without that only
the projection blends. The `[INTERP]` five-second line says what was shown and why a frame was
held. See `docs/technical/frame-interpolation.md`.

**Input is bound, not hard-coded (Sep 2026):** all four ports read
`src/input/input_bindings.c`, which loads a JSON config — `RECOMP_INPUT_CONFIG`, else
`%APPDATA%\xboxrecomp\input_bindings.json`, else one beside the executable — and falls
back to exactly the old behaviour when there is none. `py -3 -m tools.input_ui` is the UI
that writes it. See `docs/technical/input-binding.md`. `RECOMP_FAKE_INPUT` and
`RECOMP_INPUT_SEQ` still apply to controller 1 and ignore the bindings.

**Update, 22 Sep 2026 -- TimeSplitters: Future Perfect (XDK 5849) reaches its front end.**
Five faults sat on top of each other and each needed its own measurement; the
list, with the tool that found each, is in `docs/technical/shadow-mode.md` ("A frame
that never reaches the back buffer"). The short form: the XMV entry that builds the
movie's DirectSound stream is now replaced (`XMVPlaybackCreateAudioStream`), XMV status
1 means *new frame* and 2 *end of file* (the HLE had 1 as finished, and FP waited
forever), a surface over frame-buffer memory is the screen whatever its parent
(`g_swap_data`), a title may bind a D3DSurface as a texture (common type 5), and linear
textures are addressed in **texels**, so both pixel paths scale by 1/size for LIN_
formats. Three new switches for this class of problem: `RECOMP_HLE_D3D8_TRACE_SWAPS=a-b`
(the order of a frame, including what the XDK does inside Swap's own body -- FP's swap
callback draws its colour grading there), `RECOMP_HLE_D3D8_FB_PROBE=1` and
`RECOMP_D3D8_SCREENCOPY_PROBE=n`. Still blocking FP's menu: ~25 s in, the title's arena
fill runs with a NULL base and zeroes guest 0..0x80000 (found with
`RECOMP_WATCH_WRITE=0x0001EDCC`); on hardware that faults, so a runtime divergence
precedes it. **Seeds are applied by the disassembly stage:** `--only lift` and
`--from identify` leave a new seed undetected; use `--from disasm`.

**Mortal Kombat: Deadly Alliance (XDK 4721) reaches its Arcade fights (1 Oct 2026).** Its
engine runs game logic as cooperative tasks that switch stacks by hand and `jmp` into the
middle of functions, which lifted C cannot express; `titles/mkda/src/mk_tasks.c` runs each
task on a host fiber instead (`docs/technical/mkda-coroutine.md`, "Resolution"). Three
toolkit changes came with it: a guest `longjmp` to a buffer armed on another native stack is
detected and handed to a title handler (`recomp_set_foreign_longjmp`), vertex shaders
created before `CreateDevice` are replayed to the shadow device, and fixed-function draws
described by a declaration (no program) are drawn through an equivalent FVF instead of being
skipped. `scripts/run_and_report.py` now counts `[ICALL] unresolved ... target` lines; it
had matched only the old `Failed to resolve VA` wording and reported 0 for every title.

**Process exits are traced (22 Sep 2026):** `src/kernel/exit_trace.c` hooks
ExitProcess/TerminateProcess/ExitThread and the ntdll funnel under them in every loaded
module's import table and prints `[EXIT] <call>(code)` with host and guest stacks.
Built for TimeSplitters 2's unexplained `0xFFFFFFFF` exit (one driven run in two to
three, once 4.7 s in, no fault, no kernel call). `RECOMP_EXIT_TRACE=0` turns it off. A
`timeout`-killed run shows as `ExitProcess(0x8F)` from a remote thread -- that is the
killer, not the title.

Two things that cost days and are worth knowing before touching this code. The title's
**deferred render state arrays do not follow its pixel shader** — `SetPixelShader` selects
an object carrying a `D3DPIXELSHADERDEF`, and the arrays hold whichever shader last went
through them. And an Xbox surface says what it is through its **parent**: without
forwarding `SetRenderTarget`, an offscreen pass's clear wipes the screen, which is what
kept Burnout 2's world black.

**Backend decision (Sep 2026): Vulkan**, for portability — Linux, Steam Deck, Android.
The shader generators stay as they are: `d3d8_combiners.c`, `d3d8_vsh.c` and
`d3d8_shaders.c` emit HLSL, and DXC compiles HLSL to SPIR-V, so ~4k lines of translation
survive the move. Do **not** rewrite them to GLSL. Vulkan's costs here are the Y-flip and
front-face winding inversion (a negative viewport height; note `d3d8_states.c` already
carries one hand-annotated winding fix, so do not stack a second).

**The plan behind that paragraph is `docs/technical/vulkan-backend.md`** (Sep 2026): only
7% of `src/d3d` touches D3D11 at all (814 lines of 11.3k, 58 distinct entry points), so the
seam goes *inside* `src/d3d` as an RHI, not at the COM vtable — `d3d8_gl.c` is the in-tree
proof of what the vtable seam costs, and it should be deleted once Vulkan can replay a
frame. Read §4.4 before touching the winding, and §4.10 before choosing a present mode.

The A/B problem is solved: **frame capture and replay** (`src/hle/d3d8_capture.h`,
`src/replay`) records one frame's host calls and plays them back with no game running, so
one frame can be drawn by two backends and compared. That is the bring-up loop for a Vulkan
backend, and it needs no D3D11On12-style bridge.

---

## Planned changes, in dependency order

The first four get baked into generated code and are expensive to retrofit. **Do these before
any bulk codegen.**

### 1. Memory model
Move from fixed-VA mapping to base+offset. Reserve 4 GB. Keep guest pointers **32-bit** — they
live inside guest structs, so widening them breaks every layout. Implement mirror regions as
multiple mappings of one shared object. This is what makes the project portable off Win32.

### 2. Register model
Replace global `g_eax`-style registers plus the simulated stack in a guest memory array with a
per-function context struct and local temporaries. The globals defeat aliasing analysis and
register allocation, which is why naive lifted output loses to a JIT.

### 3. x87 policy
The Xbox is SSE1-only, so era compilers emit x87 for doubles. `long double` is 80-bit on x86
GCC/Clang, 64-bit on MSVC, 128-bit on AArch64 — bit-exactness is not portable for free.
Decision: `double` by default, with a per-function soft-float80 escape hatch. Settle this
before the lifter emits its first FP instruction.

### 4. Threading
The Xbox is uniprocessor. Guest code raises IRQL as mutual exclusion and spins without
barriers. Keep the cooperative single-thread model, but inject **yield checks at loop
back-edges** so a spinning guest thread cannot deadlock.

### 5. Separate engine from per-title data — **upstream owns this now**
Overrides stay in `recomp_manual.c`. Upstream's `tools/recomp/manual_scan.py` treats that
file as the single source of truth for which `sub_XXXXXXXX` are hand-defined, and the
recompiler reads it to decide what *not* to generate — so a second override file causes
duplicate symbols and unresolved externals. A fork-local split was tried here and reverted.

The mandatory-reason-per-override idea is still right and still unenforced. The place to
land it is upstream, as a convention plus a check over `recomp_manual.c`, not as a
competing file.

The kernel thunk table is already per-title upstream: `xbox_kernel_init()` reads ordinals
from the mapped XBE, keeps `g_thunk_ordinals` only as a no-XBE fallback, and adds
`xbox_kernel_set_ordinal_remap()` on top.

### 6. Then, in rough order of what they unlock
- **Texture layer** — table-drive formats, mip levels, P8 palette, texcoord generation.
  XDK-independent, and the thing most likely to stop title #2 even on the same XDK.
- **Function discovery quality** — better indirect-branch analysis, vtable recovery, seeding.
  Every unresolved ICALL becomes a per-game override that doesn't transfer, so this directly
  attacks per-title cost.
- **XDK signature coverage** — an *identification* problem, not a reimplementation one. The
  D3D8 API surface is stable; what changes per build is where functions sit. Much of this is
  porting Cxbx-Reloaded's OOVPA work. LTCG builds are harder but demonstrably tractable.
- **Kernel ordinal coverage** — driven by what titles call, not by XDK version; accumulates
  naturally. Audit per-title with `tools.kernel_audit.coverage`.
- **LLE fallback** — for titles that hand-roll push buffers or defeat signature matching.
  Shipping working-but-slow, then converting to the fast path, is what makes this a toolkit
  rather than a collection of per-game hacks.
- ~~**Pipeline driver script**~~ — done: `scripts/recompile.py`, defaults to `--game-only`,
  with `--from`/`--only` to resume part-way.

---

## Pipeline

Tools run as Python modules from the repo root (`py -3` on Windows, `python3` on Linux).

```
extract-xiso  ->  tools.xbe_parser  ->  tools.disasm  ->  tools.func_id
                                                              |
                     cmake build  <-  tools.recomp  <---------+
```

All four stages at once (this is the normal path):

```bash
python3 scripts/recompile.py game_files/default.xbe
```

Or stage by stage:

```bash
py -3 -m tools.xbe_parser  game_files/default.xbe --json game_files/g_analysis.json
py -3 -m tools.disasm      game_files/default.xbe --text-only -v
py -3 -m tools.func_id     game_files/default.xbe -v
py -3 -m tools.recomp      game_files/default.xbe --game-only --split 1000
```

The `--json` from step one is **required** by the disassembler — it reads section layout from
it. Keep it beside the XBE.

Use `--game-only` when bringing up a new title. Lifting CRT and XDK code you intend to HLE away
wastes compile time and debugging attention. Switch to `--all` only when needed.

**Two titles at once (this fork):** every stage defaults to one shared `tools/*/output`,
and the recompiler's title guard compares file names only, so two `default.xbe`s overwrite
each other silently. Give each title its own outputs and project:

```bash
py -3 scripts/recompile.py "games/<title>/default.xbe" \
    --work-dir games/_pipeline/<name>/out --project titles/<name>
```

Add `--trace-all-entries` only while bringing a title up: it puts a hook at every lifted
function's entry (for `[TRACE]` lines, `RECOMP_TRACE_ONLY`, the entry profiler and the
"main thread stops entering new functions" diagnosis) and costs a few percent of frame
time. A plain lift keeps `RECOMP_SAMPLE` and the kernel log. Both titles are lifted plain
now; TimeSplitters 2 was re-lifted plain on 19 Sep 2026 with no change in behaviour.

Projects live in `titles/<name>/` (committed: CMakeLists, `main.c`, `recomp_manual.c`),
game data in `games/<title>/` and stage output in `games/_pipeline/<name>/out` (both
ignored). Regenerate a title's `main.c` from the template with
`scripts/regen_title_main.py` rather than editing the copy. On this machine CMake is the
one bundled with VS 2019 Build Tools; there is no VS 2022 and none is needed.

---

## Debug loop

Build with `--game-only` → run → read stderr → classify → fix → rebuild. Most project time
lives here.

**Give every scripted or test run its own saves: `RECOMP_SAVE_DIR=<fresh folder>`.** Saves
live in FATX partition images (`Partition0-5.img`), by default in
`%LOCALAPPDATA%\xboxrecomp` -- shared by every title and by the player's own games. A run
without the switch reads and writes the player's real saves, and inherits whatever the run
before it saved, which makes runs depend on each other (on 30 Sep 2026 that looked like an
input regression). Every run's log says which it got: `[PATH] saves in <dir> (RECOMP_SAVE_DIR)`.
The images are sparse, so a fresh folder per run costs almost nothing.

**Before reading a wild pointer as a memory-model problem, spend three runs on
`docs/technical/memory-watchpoints.md`.** `RECOMP_TRAP_NULL`, `RECOMP_FIND_VALUE`
and `RECOMP_WATCH_WRITE` answer "who holds this" and "what wrote this" in a
plain lift with no trace hooks. Outrun 2's 0xF8604020 cost a day as an aperture
question and turned out to be three overlapping stores near a null pointer,
found in three runs once the tools existed.

| stderr shows | Cause |
|---|---|
| `[ICALL] unknown target 0x... from RVA 0x...` | Target never detected as a function. Re-run discovery with `--seed-functions`, or add a manual override. |
| Access violation at `0xFD......` | GPU MMIO — NV2A hooks not initialised |
| Access violation at `0xFE......` | APU MMIO — audio hooks not initialised |
| `SKIP-READ` | Unmapped access; often a native pointer where a guest VA was expected |
| Infinite loop | Waiting on hardware state — stub the wait or fake the state |
| Stack overflow | Wrong ESP at entry, or runaway recursion |
| **Subtly wrong physics or RNG, no crash** | **x87 precision divergence — suspect this first when behaviour differs from xemu without a fault** |
| A key or pad does nothing, and no `[INPUT] port N first press` line appears | The binding, not the title. `[INPUT] bindings from ...` at start-up names the config that was loaded and the device on each port; `py -3 -m tools.input_ui --print` shows what it holds |
| Exits via `HalReturnToFirmware` after ~15 kernel calls | XAPI's retail disc check failed (certificate `AllowedMedia` is DVD-X2 only). The kernel answers it since Sep 2026; if it recurs, look at the mode-sense reply in `kernel_bridge.c` |
| Main thread stops entering new functions right after the first `Swap`; ISR keeps ticking | `D3D_BlockOnTime` waiting for the GPU time fence. The HLE mirrors it from `D3D_BlockOnTime`'s prologue; a "not mirrored" line in the log means this XDK's prologue differs |
| Stops in DirectSound start-up, watchdog shows `ebx = <block>+0x810` | The DSP doorbell. Found from `GPSADDR`/`EPSADDR` under `RECOMP_AC97_READY`; without that switch DirectSound never gets this far and the title dereferences a half-built sound object instead |
| Draws keep coming but `Swap` slows to one every many seconds, no crash, `[FPS]` near 0 | Look at your own switches first. `--profile` (entry profiler) and a `--trace-all-entries` build printing `[TRACE]` lines both cost more per function entry than the title does; `RECOMP_SAMPLE=500` shows it as `NtWriteFile`/`prof_report`. Rerun with `RECOMP_TRACE_BUDGET=0` and no `--profile` before believing the title is stuck |
| Fault through a pointer that is in no aperture, e.g. `0xF8604020` | Suspect debris before a missing memory window. `RECOMP_FIND_VALUE=<the value>` scans guest RAM at the crash and prints who holds it; a hit at a tiny address means a null pointer plus an offset. `RECOMP_TRAP_NULL=1` then moves the fault to the instruction that made the mistake |
| A structure holds a value it should not, and you need to know who put it there | `RECOMP_WATCH_WRITE=<addr>` faults on every write to it and names the writer, in a plain lift. See `docs/technical/memory-watchpoints.md`. Trust its `callers:` line over its symbol, because the linker folds identical functions |
| `[THROW] the title threw a C++ exception` | This runtime cannot unwind, so the throw **returns** and everything after it runs on a stack nothing cleaned up. Rerun with `RECOMP_THROW_FATAL=1` and treat that as the real stopping point. `docs/technical/cpp-exceptions.md` |
| `[INT3] lifted code reached a debug trap` | The same thing seen one step later, when the throw helper was not identified. MSVC puts a trap after any call it thinks cannot return |
| A hang with `[ICALL] target 0x... is not code` repeating | A skipped indirect call returns eax = 0, which is also S_OK, so a COM-shaped loop never exits. The runtime says so after 100,000 skips; `RECOMP_ICALL_SPIN_FATAL=1` stops there. Try `py -3 -m tools.seed_from_log` on the log |
| A signed quantity is never negative — an axis saturates, a delta only grows, an abs is a no-op | A lifter sign-extension gap. `movsx r32, bp` was lifted as a zero extension until 20 Sep 2026, because `_lift_movsx`'s register list was missing `bp`/`sp` and the unmatched case fell through to the plain (zero-extended) read. Grep the generated C for `= LO16(` or `= LO8(` as a whole right-hand side: a bare narrow read where a `SX16`/`SX8` belongs. `movsx` now emits `/* movsx: unhandled source register */` rather than failing silently |

Overrides live in `recomp_manual.c` and are checked before the auto-generated table, so they
always win. `manual_scan.py` parses that file to decide what not to generate, so keep them
there and nowhere else. **Log every override with its reason at the moment you add it** —
nothing enforces this yet, which is why it gets skipped.

**Bring-up order:** CRT startup → hardware init → asset loading → menus → gameplay. Get to a
black screen with no crashes before caring about rendering.

**Reference oracle:** run the title in xemu with its GDB stub. For the no-crash divergence case,
bisect — break at a function in xemu, dump registers, dump the same context struct at the same
point in the lifted build, find the first disagreement. Add a state-dump hook to lifted output
early so this is a flag rather than a rebuild.

Ghidra is static analysis of the XBE (a second opinion on `tools.func_id`), not a live view of
a running game. Its debugger module can attach to xemu's GDB stub, but expect to fight it over
i386 detection. Not a prerequisite.

---

## Choosing the next target

**LTCG status and engine cannot be determined from wikis** — LTCG is a per-build linker setting
recorded nowhere public. Run the survey script over extracted discs (it exists now, and ranks
candidates; `scripts/make_test_xbe.py` builds synthetic XBEs for testing it without game files):

```bash
python3 scripts/survey_xbe_library.py /path/to/extracted/discs --csv survey.csv
python3 scripts/survey_xbe_library.py /path/to/one/default.xbe --verbose
```

It reads XBE headers directly and reports XDK version, D3D8 vs D3D8LTCG, library list,
RenderWare detection and version, `.text` size, kernel import count, and demand-loaded
sections. Treat the ranking as a starting point — it cannot see hand-rolled push buffers,
threading complexity, or whether you want to play the game.

Good first targets: small `.text`, RenderWare or another documented engine, no XONLINE, no
WMADEC, few demand-loaded sections. Criterion titles (Burnout 1/2) overlap most with the
existing reference implementation. Avoid GTA as a *first* target despite being canonical
RenderWare — large, threaded, streaming-heavy. Avoid 2001–2002 launch titles, which sometimes
use debug-era XDK patterns.

**Strategy:** bring up a second title on XDK 5849 first. Anything that breaks is by definition
something the project wrongly treated as universal. A third title on a different XDK then
isolates what is version-specific. That converts "it only does Burnout 3" into a work queue.

---

## Legal

The toolkit ships engine and tooling code, not game content. Recompiling requires game files
from a copy you own. Do not distribute recompiled binaries containing game code or assets, and
do not redistribute XDK library code recovered during signature work.

---

## Note on xboxlle-probe

`mstan/xboxlle-probe` is an nxdk agent listening on TCP 4380 that answers requests: named
read-only CPU/NV2A probes, guarded RAM/MMIO/flash reads, and — in armed unsafe mode — writes
and uploaded x86 payload execution. It is a **poke-and-read hardware oracle, not an instruction
tracer**, and has no Ghidra integration. Its named probes use a fixed register set tested on
one Xbox v1.1, and `controller-s-hub` is an admitted stub.

Its real value here is as ground truth for x87 precision — upload small test payloads via
`EXEC`, read results back — and for MMIO read side effects on the LLE fallback path. Flash
dumps contain per-console secrets and unique identifiers; never commit them.
