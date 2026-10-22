# Controller bindings

*September 2026.*

Until now a recompiled title got one pad, on port 1, driven by XInput pad 0
and a keyboard map written into `src/hle/input_host.c`. Nothing could be
rebound, a second player could not be plugged in, and the only way to change a
key was to edit C and rebuild every title.

There are now three pieces:

| | |
|---|---|
| `src/input/input_bindings.c` | reads the config, samples the four ports |
| `tools/input_ui/` | the UI that writes the config (`py -3 -m tools.input_ui`) |
| `tools/input_ui/bindings.py` | the schema both sides speak, and its tests |

Every title built from this toolkit picks it up: the binding layer is part of
`xbox_input`, which `xbox_hle` and `xbox_usb` both link, so a title only has
to be rebuilt, not changed.

## The file

JSON, one entry per controller:

```json
{
  "version": 1,
  "pad_api": "sdl",
  "controllers": [
    {
      "port": 1,
      "device": "gamepad:0",
      "deadzone": 7849,
      "bindings": {
        "a": ["pad:a", "key:Z"],
        "start": "key:RETURN",
        "lstick_left": ["pad:lx-", "key:A"],
        "black": "none"
      }
    }
  ]
}
```

Ports are **1-4**, as they are written on the console. A controller the file
does not mention keeps its defaults, and so does a control the file does not
mention -- a file with one binding in it is a config with one binding changed.
That is deliberate: it makes the file hand-editable and a diff readable.

**`device`** is `keyboard`, `gamepad:0` .. `gamepad:3`, or `none`. It says where
`pad:` sources are read from. `key:` sources are always read, whatever the
device is, which is why the default port 1 answers to both a pad and the
keyboard at once. `xinput:0` .. `xinput:3` is what files written before SDL3
say, and names the same slot.

**`pad_api`** (Sep 2026) is which API reads every pad: `"sdl"`, the default, or
`"xinput"`. SDL3 reads Xbox pads and everything else -- DualSense and
DualShock 4, Switch Pro, 8BitDo, the Steam Deck, generic pads through SDL's
mapping database -- with hotplug and rumble; a pad takes the first free slot
when it is connected and keeps it until unplugged. XInput reads Xbox
controllers only, by XInput's user index, and is kept for a pad or machine
where SDL3 misbehaves. `RECOMP_PAD_API=sdl|xinput` overrides the file, and
the launcher's Input tab has it as "Controllers". Both go through
`src/input/recomp_pad.h`, which returns XInput's layout whichever API read
the pad; `recomp_pad_probe [seconds] [xinput]` (built on request, target
`recomp_pad_probe`) prints what it sees, live.

**A binding** is one source string or a list of up to four. Any of them
pressing the control presses it, and the strongest wins for anything analog,
so a trigger bound to both a key and a pad trigger behaves sensibly. `"none"`
or `[]` unbinds.

**Sources**

| form | meaning |
|---|---|
| `key:Z`, `key:7` | a letter or digit |
| `key:RETURN`, `key:LSHIFT`, `key:F8`, `key:SLASH` | a named key (the list is `KEYS[]` in `input_bindings.c`, mirrored in `bindings.py`) |
| `key:#191` | a raw Windows virtual-key code, for anything with no name |
| `pad:a` `pad:b` `pad:x` `pad:y` | face buttons on the host pad |
| `pad:lshoulder` `pad:rshoulder` | shoulder buttons (the Xbox's Black and White by default) |
| `pad:start` `pad:back` `pad:lthumb` `pad:rthumb` | the rest of the digital buttons |
| `pad:dpad_up` .. `pad:dpad_right` | the host D-pad |
| `pad:lt` `pad:rt` | analog triggers, 0-255 |
| `pad:lx+` `pad:lx-` `pad:ly+` .. `pad:ry-` | one half of one stick axis |
| `mouse:left` `mouse:right` `mouse:middle` `mouse:x1` `mouse:x2` | mouse buttons (x1 back, x2 forward) |
| `mouse:wheel_up` `mouse:wheel_down` | a wheel notch, as one short press |
| `mouse:x+` `mouse:x-` `mouse:y+` `mouse:y-` | mouse motion, one half of one axis: x+ right, y+ away from the player (stick up) |

**Controls** are the 24 inputs an Xbox pad has: `a b x y black white start
back ltrigger rtrigger`, `dpad_up/down/left/right`, `lthumb`, `rthumb`, and
each stick as four halves -- `lstick_up/down/left/right` and `rstick_*`.

A stick is four halves rather than two axes because a key has no middle. It
also means every source, whatever it is, reads as one 0-255 magnitude:
a digital control fires above a quarter press, an analog button takes the
magnitude, and a stick axis is the positive half minus the negative one,
scaled to the signed 16 bits the guest reads. Binding a trigger to a key
gives a full pull; binding a stick half to a key gives a full deflection.

### Where it lives

Every side looks in the same places, in this order
(`recomp_bindings_read_path` in `src/config`, which the runtime and the
launcher both call):

1. `RECOMP_INPUT_CONFIG`, if set -- an explicit path, and a run says so if it
   cannot be read rather than silently falling back;
2. `%APPDATA%\xboxrecomp\titles\<TITLE ID>.bindings.json` -- this title's own,
   which the launcher writes (`py -3 -m tools.input_ui --title 45410066`
   edits it too);
3. `%APPDATA%\xboxrecomp\input_bindings.json` (on POSIX,
   `$XDG_CONFIG_HOME/xboxrecomp/...`) -- the per-user file every title shares,
   which `tools.input_ui` writes without `--title`;
4. `input_bindings.json` beside the executable, then in the working directory
   -- the preset a title project ships with its build.

**Per title since October 2026.** Which pad sits on which port belongs to the
person, but a keyboard layout belongs to the game: the key that throws a
grenade in Future Perfect means nothing in a racer, and a shared file made a
keyboard player choose one game's layout for all of them. The shared file is
still read, after the title's own, so nothing set up before this changes. The
launcher always saves to the title's own file, and its "Reset this controller"
goes back to the preset in (4) when there is one.

### The mouse (October 2026)

`src/input/input_mouse.c` reads the mouse as raw input on the game window, so
motion is the counts the mouse reports, not the pointer -- no Windows pointer
acceleration, no stop at the screen edge. The game owns the mouse only while it
is **captured**: a click in the window captures it (that click is not passed
on), Alt+Tab or anything else that takes the focus releases it, and a
fullscreen window captures it as soon as it is active. The pointer is hidden and
held in the window meanwhile. Outside a capture every mouse source reads as
still and unpressed. Nothing captures unless a binding names a `mouse:` source.

Motion drives a stick. The speed over the last `mouse_window_ms` milliseconds
(a ring of 1 ms buckets) becomes a deflection, so every poll in a frame sees the
same stick however often the title reads its pad. Per controller:

| key | default | |
|---|---|---|
| `mouse_sensitivity` | 1.0 | 1.0 reaches a full stick at 6 counts a millisecond (4.5 in/s at 800 dpi) |
| `mouse_deadzone` | 0 | the title's own stick deadzone, in stick units: any motion at all lands just past it |
| `mouse_exponent` | 1.0 | applied to the deflection; below 1 straightens a title that curves its stick upward |
| `mouse_y_scale` | 1.0 | up and down against left and right: console games look up and down slower than they turn |
| `mouse_invert_y` | false | |
| `mouse_window_ms` | 16 | |
| `mouse_zoom_match` | true | for a title that aims by the mouse itself: zoomed in, the view turns less, by the zoom |

A click whose release arrives at once is held for 40 ms, so a title sampling
once a frame still sees it; a wheel notch is a 50 ms press and a 30 ms gap.

**Hold or toggle.** A per-controller `"hold": ["rthumb"]` makes the keys
and mouse buttons bound to those controls hold a toggle on for as long as
they are held, where the console toggles on each press (Future Perfect's aim
mode, a click of the right stick). It needs the title to say how the toggle
stands (`recomp_bindings_toggle_reader`); then, whenever the button and the
game disagree, one 50 ms press is sent and the state is given 150 ms to
follow; a press the game ignores (it cannot aim just now) is tried three
times, then left until the button changes. Pad buttons keep the toggle. The
in-game menu has a Hold / Press-to-toggle row for each such control.

**The keyboard and mouse are read only while the game window has the focus.**
`GetAsyncKeyState` answers for the whole desktop, so before this, typing in a
chat window with the game behind it walked and fired in the game. A run with
no game window (the shadow renderer off) reads the keyboard as before.

### Future Perfect's preset

`titles/futureperfect/input_bindings.json`, copied beside the executable and
the launcher at build time. Controller 1 keeps its pad and adds:

| | Xbox control | keyboard and mouse |
|---|---|---|
| Move, side step | left stick | W A S D |
| Look, turn | right stick | mouse |
| Fire | right trigger | left click |
| Aim | right stick click | right click, held |
| Throw grenade | left trigger | G, middle click |
| Activate / menu select | A | E, Space, Enter |
| Melee / menu back | B | F, Backspace |
| Reload | X | R |
| (menus) | Y | V |
| Crouch | left stick click | C, left Ctrl (toggles; can be set to hold) |
| Change weapon | D-pad left / right | wheel, Q / arrows |
| Zoom in / out | D-pad up / down | mouse forward / back buttons, arrows |
| Pause | Start | Escape |
| Back | Back | Tab |

#### True 1:1 aim

The mouse does not go through the right stick in Future Perfect: it turns
the player's view angles itself (`titles/futureperfect/src/recomp_manual.c`,
wrapped around the look builder `sub_0025CE80`; the comment there has the
detail). A stick only sets how fast the view turns, through the game's
deadzone, curve, speed cap and turn acceleration, so no stick setting makes a
mouse feel right. The player keeps yaw at [player+0x100] and pitch at
[player+0x104], in degrees; the look consumer `sub_0025D520` adds the stick's
turn to them, wraps yaw and holds pitch within its limit. The hook, which
runs just before it, adds the counts that arrived times the sensitivity. The
angle turned is the counts times the sensitivity, the same up and down as
left and right, in the frame the mouse moved, and the stick still works
alongside.

The two angles were found by recording the player and its camera every frame
through scripted stick turns (`RECOMP_FP_SCAN=<file>:<from ms>:<ms long>`)
and correlating every field with the view matrix's yaw and pitch: both follow
it at 1.00.

**The first version steered, and it failed in play.** It read where the view
was from the view matrix, moved a target by the counts, and wrote the look
vector that would turn the view onto it, learning how far a unit of look
turned as it went. Scripted counts never showed a problem. A real mouse made
the loop oscillate (the gun whipping about), and its guard against an axis
that seemed to turn the wrong way read the oscillation as one and reversed
the axis at random (`[FP-AIM] the view turns against the mouse`). Writing the
angles needs nothing measured or learned, so there is nothing to run away.

- `mouse_sensitivity` 1 is 0.04 degrees a count (a full turn in about 29 cm
  at 800 dpi); the preset's 1.5 is 0.06, about what PC shooters ship with.
  `mouse_y_scale` is 1.0, and `mouse_invert_y` is the only invert the mouse
  has: the game's Inverse Look applies to the pad alone.
  `mouse_deadzone`, `mouse_exponent` and `mouse_window_ms` are not used
  while the mouse aims directly.
- Aim mode (right click) zooms: the camera's vertical angle ([player+8]
  +0x384) eases to the zoomed one, and with `mouse_zoom_match` the mouse
  turns the view less by the ratio of the two angles' tangents, so a movement
  crosses the same part of the screen zoomed or not (0.52 at the stock
  zoom).
- Holding right click aims (`"hold": ["rthumb"]`): the reader is the aim
  flag at [player+0xF5C]. Crouch can be held too (left Ctrl, C): bit 0x10 of
  [player+0xC], found by RAM snapshots standing and crouched; the preset
  leaves it a toggle.
- Measured with `RECOMP_FP_MOUSE_TEST` (synthetic counts, logged each frame):
  yaw moves exactly counts x 0.04 x sensitivity a frame and the view with it,
  pitch stops at the game's own 60 degrees, and aiming scales both by the
  zoom.
- Where the look consumer does not turn the view by the look vector (a state
  it handles otherwise, a menu, a cutscene) the mouse falls back
  to the right stick within a quarter of a second, with the stick settings
  below. `RECOMP_FP_MOUSE_AIM=0` keeps it on the stick everywhere.

The stick settings were measured, not guessed. Holding the right stick at a
fixed strength in 2401 (`RECOMP_INPUT_SEQ` with `rstick_right@<percent>`, frames
compared for horizontal shift) turned the view not at all up to 35%, slowly at
40%, and about 5x and 13x faster at 50% and 60%: Future Perfect ignores the
right stick below about 37% and curves it upward above that. So the preset's
`mouse_deadzone` is 12450 (38%) and its `mouse_exponent` 0.7. Up and down
looks at about half the speed it turns, which the stick fallback keeps. In
the game's own Controls page Auto Lookahead, which re-centres the view while
walking, is better off.

#### The mouse in the menus

Future Perfect's lists -- the main menu, the pause menu, Arcade Type and the
like -- can be pointed at: moving onto an option highlights it, a left click
picks it, a right click goes back, and the wheel steps up and down. The keys
and the pad work as before alongside. Beyond plain lists:

- Values (Music ON/OFF, the volume bars, a match's Score limit, Weapon Set):
  a click left or right of the row's middle steps it that way, held it keeps
  going (the game's own repeat), and the wheel over it turns it. Weapon Set
  and Bot Set end in A's prompt, which opens their editors: a click there is
  A.
- The pause screen's OPTIONS / STATUS header: a click beside the tab showing
  turns to the other.
- The profile name keyboard: its letters are items like any other.
- Character select: moving onto a portrait walks the selection there, a click
  picks that character (A goes once the new character has come in -- the
  game ignores it before), a click anywhere else picks the one selected.

The runtime side is general (`input_mouse.h`, "for a title whose menus the
mouse can point at"):

- `recomp_mouse_pointer_mode(on)`, every frame: while on, a capture is let
  go (the pointer free and shown -- in fullscreen too, where the window
  otherwise hides it over the picture: `recomp_mouse_pointing`), a click
  captures nothing, and presses and
  the wheel go to `recomp_mouse_pointer_clicks` rather than the bindings. Off,
  or a quarter of a second without a call, and the game takes the mouse back
  without a click, if the window is in front and the pointer over it -- play
  resumes ready to aim. `[INPUT] a menu has the pointer` / `the menu's pointer
  is put away` in the log.
- `recomp_mouse_pointer(&x, &y)`: where the pointer is, as fractions of the
  window's inside; `xbox_D3D8PictureRect` (`d3d8_xbox.h`) says where the
  picture is in the window, bars left out, to map it into the title's screen.
- `recomp_bindings_press(port, "a", ms)` holds a control as if a key bound to
  it were held: how a click becomes the button the menu reads.

The title side (`titles/futureperfect/src/recomp_manual.c`, "the mouse in the
menus") is what FP's front end looked like when read: a table of 20 widgets
at 0x006FEBE8, lists with linked items, and in each item the box it is drawn
in, in the front end's 640x480 screen and already squeezed for widescreen.
The screen's widget update, `sub_0007A7B0`, is wrapped; before its outermost
call the pointer is hit against every selectable item, and a move onto one
selects it through the game's own `sub_00076EF0`, so the highlight, help line
and sound are the game's. Only a move selects: a pointer left still never
fights the keys. An item with bit 4 of its +0x28 set is passed over, as the
game's select passes over it: titles, text fields and rows of prompts drawn
as buttons. `recomp_mouse_pointer_held` says which buttons are still down,
for a held click on a value.

The two screens without items to hit were read differently. The pause
header's place was measured from screenshots (squeezed toward the middle in
16:9 like the rest of the front end); which tab is showing is told by an item
only that tab has. Character select keeps its state at 0x0078A248 on (the
selection, the top row as a float, the player's state, a float that runs down
as a new character comes in), found by RAM snapshots across presses; its
portraits keep to the left edge in 16:9, not squeezed.

`RECOMP_FP_POINTER_TEST=<ms>:<x>:<y>:<click>,...` puts a pointer at (x, y) in
that 640x480 screen on the input script's clock -- click 1 left, 2 right, 3
left held until the next step, 4 and 5 a wheel notch up and down -- since a
run must not move the desktop's pointer; with `RECOMP_INPUT_SEQ_PAD=1` the
presses it makes reach the game beside the script.
`RECOMP_FP_UI_DUMP=<ms>[,<ms>...]` prints the widget table and every item.

#### Button prompts

Future Perfect's prompts ("(A) Select  (B) Back") show the pad's buttons.
While the keyboard and mouse are in use they show the keys bound to those
buttons instead -- an `E` keycap with a green border where the green A was,
`F` in red for B, and so on -- and the pad's icons come back when a pad is
used (`prompts` setting: auto, keyboard, controller; F1, General).

- The icons are separate 32x32 DXT3 textures, found by drawing the main menu's
  prompts in a capture (`d3d8_replay --list-draws`: one small texture, then
  the text) and identified by checksum: `RECOMP_HLE_D3D8_TEX_DUMP=<folder>`
  writes every texture of 64x64 and under as `<checksum>_<format>_<size>`,
  decoded to a contact sheet offline. A `DDC6741B`, B `4D4970F1`, X
  `1C76AFDD`, Y `CFBE7E42` (`fp_title_setup`, `recomp_prompts_icon`).
- `hle_texture_replace.h` is the general part: a title's replacer is asked
  about each texture as it is made and whenever its texels change, and
  answers with an image of any power-of-two size; the cache makes that an
  A8R8G8B8 texture (swizzled, so addressed from 0 to 1 like the original),
  and every draw samples it. `xbox_D3D8TextureReplacementsChanged` has the
  textures it knows asked again: the prompts call it when the player changes
  hands or a key is rebound.
- `hle_prompts.c` draws each keycap with GDI at four times its 64x64 size,
  as masks (outline, face, label) composed into colour and alpha and averaged
  down; the label is the first key or mouse button bound to the control
  (`recomp_bindings_prompt_label`: `E`, `ENTER`, `LMB`). Which hand was used
  last is the bindings sampler's: a key or click on one side, a pad button,
  trigger or a stick pushed past 60% on the other.

### Defaults, when there is no file

Exactly what the runtime did before this existed, so nothing changes for a run
that has not been configured:

- controller 1: XInput pad 0 **and** the keyboard -- arrows = D-pad,
  Enter = START, Backspace = BACK, Z = A, X = B, A = X, S = Y, Q = White,
  W = Black, E = left trigger, R = right trigger;
- controllers 2-4: XInput pad 1, 2, 3, pad only.

The sticks have no keyboard default. Every comfortable key around WASD is
already a button in that map, and guessing would have silently changed what
existing runs do.

### Players joining while the game runs

A port a file does not name keeps its default, pad N for controller N, so a
second pad plugged in mid-game is player 2 without any set-up: the pad list is
read again four times a second (`recomp_pad.c`), and the title hears of the
new pad through `XGetDeviceChanges` as the console would have told it
(`hle_input.c`).

`keyboard_alone` (settings file, `RECOMP_KEYBOARD_ALONE`, F1 General and the
launcher's General page, off by default) is for a friend joining through
Steam Remote Play Together or Parsec while you play on keyboard and mouse:
their virtual pad is this machine's first, which is player 1's beside the
keyboard, so the two of you would steer one player. On, player 1 reads no pad
(when it has keys or the mouse bound) and every later player reads the pad
before the one it names -- controllers start at player 2. Changed live, the
title is told a pad arrived or left. `pad_of` in `input_bindings.c`;
`recomp_pad_test_set` stands in for a pad in tests.

## The UI

```
py -3 -m tools.input_ui                  # the per-user config
py -3 -m tools.input_ui path\to.json     # somewhere else
py -3 -m tools.input_ui --print          # what the runtime would load
py -3 -m tools.input_ui --write-defaults # a default file, no display needed
```

Four controllers down the left as dashboard blades; the selected one's device
across the top -- the keyboard, each XInput slot with whether a pad is
answering right now, or nothing; every control below it. Clicking a binding
(or `Bind`) opens a capture: press a key or a button and it is taken, `+` adds
a second source to the same control, Esc cancels, and the dialog has a button
to clear. `Reset this controller`, `Reset all four`, `Save`.

It is tkinter with every colour set by hand -- black field, `#0D0D0D` panels,
`#107C10` for structure and `#9BE04B` for anything with focus or a value. ttk
on Windows will not let a widget be black, and `tk.Scrollbar` keeps the
system's white trough whatever it is told, so the scrollbar is drawn here too.

Device detection is XInput through `ctypes` (`tools/input_ui/devices.py`): it
tries `xinput1_4`, `xinput1_3`, then `xinput9_1_0`, and asks each of the four
slots. DirectInput and raw HID are not read -- an Xbox-layout pad is what
XInput reports, and anything else would need a translation layer the toolkit
does not have. A pad that is not detected can still be bound and configured;
the port simply reports empty until it is plugged in.

Capture compares against the pad state taken when the dialog opened, so a
stick resting off centre or a trigger resting high does not bind itself.

## Keys Windows keeps

With Alt held, Tab, Enter, Esc, Space and F4 read as up: Alt+Tab, Alt+Enter
(fullscreen), Alt+Esc, Alt+Space and Alt+F4 are Windows' own. The key goes
down while the game's window still has the focus, and Future Perfect binds
Tab to Back -- so Alt+Tab on its main menu pressed Back and went to Change
Players.

## What the runtime does with it

`recomp_bindings_init()` runs once, from whichever input path is reached
first, and logs what it read:

```
[INPUT] bindings from C:\Users\...\AppData\Roaming\xboxrecomp\input_bindings.json
[INPUT]   controller 1: keyboard
[INPUT]   controller 2: nothing
[INPUT] XAPI input replaced by name: 1 pad(s); gamepad type 0x0028C054
[INPUT]   port 1 <- keyboard
[INPUT] XInputOpen port 1 (keyboard) -> handle 0x58490001
[INPUT] port 1 first press: start (keyboard)
```

That last line is named once per port, the first time anything is pressed on
it. A wrong binding is otherwise invisible -- the title just does not respond,
and there is no way to tell a mis-bound key from a title that is not reading
its pads at all.

`src/hle/hle_input.c` now reports **up to four ports**. A port is present when
its device is the keyboard, or when its XInput pad answers, or when it has
keyboard bindings as well (which is why the default port 1 is always there).
`XGetDevices` and `XGetDeviceChanges` re-check, so a pad plugged in while the
title runs arrives as an insertion the way the console reported one, and
`XInputGetState` samples the device bound to that handle's own port.

Sources are resolved to key codes and pad indices once, at load, so a poll
does no string work; a poll reads each distinct key once and its pad once. An
empty XInput slot is re-checked only once a second, because `XInputGetState`
on one costs about a millisecond and a title polls several times a frame --
that was true before this change too, and silently cost more than the game on
a machine with no pad.

`RECOMP_FAKE_INPUT` and `RECOMP_INPUT_SEQ` are unchanged and apply to
controller 1 after its bindings. They stand in for a person at the keyboard,
so what that person bound should not change them.

`src/usb/usb_gamepad.c` reports port 1 through the same bindings, so a title
that finds its pad through the emulated USB stack rather than the XAPI
replacement behaves the same.

## Tests

```
py -3 -m unittest discover -s tools -p "test_*.py" -t .
```

`tools/input_ui/test_bindings.py` covers the half that needs no device:
defaults, what a partial or broken file means, which sources are valid, and
that a saved config reloads identical.

`tools/input_ui/test_runtime_vocabulary.py` reads `CONTROLS`, `PAD_BUTTONS`,
`KEYS` and `DEFAULTS` out of `src/input/input_bindings.c` and fails if the
Python copies have drifted. Drift is the dangerous failure here: the UI would
write a name the runtime does not recognise, the file would save, and the
control would simply never fire.

## Verified

Burnout 2, built from this branch:

- with no config file: boots as before, one pad on port 1, the log naming the
  built-in defaults;
- with a config naming `keyboard` on port 1, `none` on port 2 and START bound
  to `key:F8`: the log named the file and the devices, and pressing F8 came
  through as `port 1 first press: start`.

## Not done

- **Hot reload.** The config is read once, at start-up. Re-reading it when the
  file changes would be a few lines, but a binding that changes mid-frame
  wants thinking about.
- **DirectInput and raw HID**, so a PlayStation or generic pad appears without
  a third-party translation layer.
- **Mouse aim straight into the camera.** Motion goes through the title's
  stick handling, deadzone, curve and turn-speed cap included, which is as
  close as a toolkit can get without knowing the game. A title project can do
  better by adding the mouse to the camera's own angles.
- **A POSIX backend.** The parser is portable and builds anywhere; the
  sampling is XInput and `GetAsyncKeyState`, so on POSIX every port reads
  neutral until an SDL2 backend exists (the same gap `src/hle`'s audio and
  input backends already have).
- **Memory units and other XPP devices.** Only gamepads are bound; the model
  still answers "nothing connected" for everything else.
