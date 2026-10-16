# The in-game settings menu

*October 2026.* **F1** while a title runs -- or **Back and Start together** on
a pad -- opens a settings panel over the game. Esc, F1 or B closes it.

| Page | Row | When it applies |
|---|---|---|
| Display | Resolution | at once: the scene target and its depth buffer are made again at the new size between two frames (`xbox_D3D8RequestScale`) |
| | Widescreen: Off, 16:9, or Fill the window (a title that can draw any shape) | on or off next start (a title reads the console's video flag once); 16:9 and Fill at once |
| | Field of view (a title that can set it) | at once |
| | Texture sharpness | at once |
| | Edge smoothing (FXAA): in the resolve, at the output pixel's centre on the scene; skipped where the resolve averages several taps (4x and up at 1080 lines), which is supersampling already | at once |
| | Brightness | at once: a gamma in the resolve, 50% to 200%; 100% is the picture exactly as drawn |
| | Frame rate | at once (the same stops as F10) |
| | Show frame rate | at once (the same switch as F9) |
| | Fullscreen | at once (the same switch as Alt+Enter) |
| | G-SYNC / FreeSync (variable refresh, fullscreen) | at once |
| | Frame interpolation: Off, 2x, 3x, 4x (for 120, 180, 240 Hz displays) | next start |
| Mouse | Sensitivity, up and down speed, invert | at once |
| | Zoomed aim (a title that aims by the mouse itself) | at once |
| | Hold or toggle, for each toggle the title reports (Future Perfect: aim, crouch) | at once |
| | Back to this game's defaults | at once |
| Controls | every control's keyboard or mouse binding | at once, saved at once |
| | Reset controls to this game's defaults | at once |
| General | Volume, 0 to 100% in fives: the master voice's gain, squared so steps sound even | at once |
| | Mute in the background | at once |
| | Pause in the background: on losing the focus in play, the title's pause control is pressed for it (`recomp_bindings_pause_on_focus_loss`; FP: Start, while the look builder runs) | at once |
| | Button prompts: Automatic, Keyboard and mouse, Controller (`hle_prompts.h`; input-binding.md) | at once |
| | Skip intro videos | next start (only the title's logo videos, which its project names) |

A row that only applies at the next start says so beside its value.

## With the mouse

The panel takes the pointer while it is open, in fullscreen too: moving onto a
row selects it (only a move, so a pointer left still never fights the keys),
a click on a tab turns to that page, a click on a selected row's `<` or `>`
changes its value, a click elsewhere on a row is Enter (a switch flips, a
control waits for its new binding, a reset resets), the wheel steps through
the rows, and a right click closes the panel as Escape does. While a control
waits for a binding the clicks are the binding's, so a mouse button can still
be bound by clicking it. Closing the panel gives the game back the mouse
capture it had, without another click.

The renderer keeps where it drew each tab, row and arrow
(`xbox_D3D8MenuHit` in `d3d8_menu.h`, in the window's pixels), and the
window procedure hands moves and clicks to `hle_menu_mouse`. Tested by posting
mouse messages to the game window on a timeline (nothing reaches the
desktop's pointer), against copies of the settings and binding files.

## Keys that work any time

F9 the frame counter, F10 the frame cap, F11 a developer capture,
**F12 a screenshot** (`src/hle/hle_screenshot.c`): the next frame as it
reaches the window -- widescreen shape and supersampling in it, the counter
and this menu not -- saved as a PNG in `Pictures\<the game's name>`, with a
line on screen saying where. The readback happens on the presenting thread;
the encoding (SDL's PNG writer) on a thread of its own, so it costs no stutter.
`RECOMP_SCREENSHOT_DIR` puts them elsewhere; `RECOMP_SCREENSHOT_AT=<ms>,...`
takes them in a scripted run.

## Where things go

The menu writes the files the launcher writes, with the launcher's own code,
so the two never disagree:

- display and general settings: the title's settings file
  (`recomp_settings_path`), on closing the menu;
- mouse and key bindings: the title's binding file
  (`recomp_bindings_write_path`, `bind_save` from
  `src/launcher/launcher_bindings.c`, now built into `xbox_input`), on closing
  for the mouse and at once for a rebind, which the runtime then rereads
  (`recomp_bindings_reload`).

`RECOMP_DISPLAY_CONFIG` and `RECOMP_INPUT_CONFIG` move both, which is how a
test drives the menu without touching a player's files.

## How it works

- `src/hle/hle_menu.c` is the menu: the game window's thread hands it F1 and
  the keys pressed while it is open; the presenting thread, once a frame,
  reads the pad, acts, applies and saves, and hands the panel to
- `src/d3d/d3d8_menu.c`, which draws it with GDI into a texture and puts it
  over the window's buffer *after* the resolve, at the window's own size, so
  it is neither stretched by a widescreen present nor softened by
  supersampling. The frame-rate counter (`d3d8_overlay.c`) is drawn into the
  scene instead, which is fine for one line and wrong for a panel.
- While it is open, `recomp_input_suspend` takes every input from the game:
  each port samples as an untouched pad, the keyboard and mouse read idle,
  and the mouse is released. The game keeps running, so pause it first. On
  closing, input returns only once Escape, Enter, F1 and the pad's A, B,
  Back and Start are all let go, or the key that closed the menu would reach
  the game.
- Rebinding takes the next key, mouse button or wheel notch, and for a stick
  direction a push of the mouse that way (`bind_capture_*`, shared with the
  launcher). F1, Escape and F9-F12 are never offered.

## Action names

A title's preset may carry `"labels"` -- `{"rtrigger": "Fire", ...}` -- and the
Controls page and the launcher then show "Fire" rather than "Right trigger".
They describe the game, so they are always taken from the preset beside the
executable, even when the player's own file predates them
(`bind_load_with_labels`). The runtime never reads them.

## Testing it without a person

`PostMessage` reaches the game window without the focus, so a script can open
the menu and walk it while the run sits in the background (`WM_KEYDOWN` for
the keys, `WM_MOUSEWHEEL` for a wheel binding), and
`RECOMP_HLE_D3D8_DUMP_PRESENTED=1` dumps what reached the window, panel
included. A key binding cannot be tested this way: capture reads the real key
state, which posted messages do not change.
