# TimeSplitters: Future Perfect — native PC

A native Windows build of **TimeSplitters: Future Perfect** (Xbox, 2005), made
by *static recompilation*: the game's original Xbox code is translated into C
and compiled into a Windows program. There is no emulator; the game runs
directly on your PC. On top of that, this project adds the things a PC version
should have — real mouse aim, keyboard and mouse everywhere, high resolutions,
ultrawide, frame interpolation, an in-game settings panel and more.

> **Work in progress.** The menus and the story's opening level are what most
> testing has covered. Expect rough edges elsewhere, and please report them.

If you like the recompile and want to show support for updates & more, I have a **[ko-fi](https://ko-fi.com/xnbdyz)**

## You need your own copy of the game

**This repository contains no game code and no game files.** To build and play
you need a legally obtained copy of *TimeSplitters: Future Perfect* for the
original Xbox — the **USA (NTSC)** release — as a disc image made from your own
disc.

- The build reads the game's executable, `default.xbe`, from your copy and
  generates the recompiled code from it on your machine. That generated code is
  never part of this repository.
- At run time the game's levels, sounds and movies are read from your copy.
- Only the USA release is supported: the project relies on that exact
  executable. Its SHA-1 is `daf0bcc57b6e93a7ebf5498074c8a9e7dc90381e` (step 4
  shows how to check yours).

TimeSplitters and TimeSplitters: Future Perfect are trademarks of their
respective owners. This project is not affiliated with or endorsed by them.

## What's new compared with the Xbox

**Display**
- Render at 1× to 8× the console's resolution (supersampling), changeable while
  playing.
- Widescreen through the game's own 16:9 mode, or *Fill the window* for any
  shape — 21:9 and 32:9 included, with the game laying out its own camera and
  HUD for it.
- Field of view from 40° to 90°, and a separate weapon field of view, so a
  wider view no longer shrinks the gun in your hands.
- Frame interpolation at 2×, 3× or 4× (120, 180 or 240 frames a second on
  screen) while the game itself keeps its native 60.
- FXAA edge smoothing, texture sharpness (anisotropic filtering), brightness.
- Borderless fullscreen (Alt+Enter), G-SYNC / FreeSync, a choice of frame
  pacing, and a frame-rate counter (F9).

**Mouse and keyboard**
- True 1:1 mouse aim: the mouse turns the view directly, with no stick
  deadzone, curve or acceleration in between. Sensitivity, vertical speed and
  invert are adjustable, and zoomed aiming slows the mouse by the zoom so a
  movement covers the same part of the screen.
- Aim and crouch can be held or toggled.
- Every control can be rebound in game, to keys or mouse buttons.
- The mouse works in the game's menus (point, click, right-click to go back,
  wheel to scroll) and in the settings panel. Values such as volume and match
  options step with a click on either side or the wheel; the pause screen's
  OPTIONS / STATUS header, the name keyboard and character select can be
  clicked too.
- On-screen button prompts show your keys — "E Select", "F Back" — instead of
  the pad's A and B, and switch back when you pick up a controller.

**Multiplayer**
- Co-op on two PCs over a network or the internet: each player plays on their
  own PC, full screen, with their own resolution, frame rate, mouse and
  controls. Set up in the game's settings panel (F1, *Co-op*) or the
  launcher's *Co-op* tab (see [Co-op on two PCs](#co-op-on-two-pcs)).
- Split-screen works with a controller per player; a controller plugged in
  while the game runs joins as the next player.
- *Keyboard and mouse: player 1 alone* moves controllers to player 2 and up,
  so a friend joining through Steam Remote Play Together or Parsec plays their
  own player while you play on keyboard and mouse.
- The *Xbox Live / System Link* menu reads *Online / LAN*. Behind them are
  still the game's own Xbox Live and System Link screens, with no service to
  connect to; co-op on two PCs is set up from F1 or the launcher instead.

**Sound**
- Fixed: lines of dialogue cutting into each other, looping and broken
  cutscene dialogue, and the noise on the System Link screen and at the start
  of the first level.
- Master volume, and an option to mute while the window is in the background.

**Quality of life**
- In-game settings panel (F1, or Back+Start on a pad) with Display, Mouse,
  Controls and General pages, plus a launcher for setting up before the first
  run — both in Future Perfect's own blues.
- The game pauses itself if you Alt+Tab away during play.
- Skip the logo videos at start-up.
- F12 saves a screenshot to `Pictures\TimeSplitters FP`.
- Saves are kept in your user folder (`%LOCALAPPDATA%\xboxrecomp`), not beside
  the game files.

## Building

Windows 10 or 11, 64-bit. Building takes about 15 minutes the first time, most
of it waiting.

### Requirements

- **Visual Studio 2022** (Community is fine) with the *Desktop development
  with C++* workload.
- **CMake** 3.20 or newer, on your PATH.
- **Python** 3.10 or newer (the `py` launcher), then:
  ```
  py -3 -m pip install capstone
  ```
- **Git**.
- **FFmpeg** for the game's movies: the BtbN build
  `ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip` from
  <https://github.com/BtbN/FFmpeg-Builds/releases>. Without it the game still
  runs, but its movies are skipped.

The commands below are for a Developer PowerShell or Command Prompt, run from
the repository's folder.

### 1. Get the code

```
git clone https://github.com/lmplied/TSFPRecomp.git
cd TSFPRecomp
git submodule update --init third_party/SDL third_party/XbSymbolDatabase
```

Only those two submodules are needed. The others belong to the wider toolkit
and are large.

### 2. Add FFmpeg

Unzip the FFmpeg build so that its `bin`, `include` and `lib` folders sit in
`third_party\ffmpeg\` (`third_party\ffmpeg\bin\avcodec-62.dll` should exist).

### 3. Build the symbol finder

The pipeline uses XbSymbolDatabase to find the Xbox library functions inside
the game's executable:

```
cmake -S third_party/XbSymbolDatabase -B third_party/XbSymbolDatabase/build -G "Visual Studio 17 2022" -A x64
cmake --build third_party/XbSymbolDatabase/build --config Release
```

### 4. Put your copy of the game in place

Extract your disc image into `games\Timesplitters - Future Perfect (USA)\`
with the included tool:

```
py -3 -m tools.xiso unpack "path\to\your\TimeSplitters - Future Perfect.iso" -o "games/Timesplitters - Future Perfect (USA)"
```

Then check it is the USA executable:

```
certutil -hashfile "games\Timesplitters - Future Perfect (USA)\default.xbe" SHA1
```

It must print `daf0bcc57b6e93a7ebf5498074c8a9e7dc90381e`.

### 5. Generate the recompiled code

First the library symbols, then the recompiler itself (about 4 minutes):

```
py -3 -m tools.xdk_symbols "games/Timesplitters - Future Perfect (USA)/default.xbe"
py -3 scripts/recompile.py "games/Timesplitters - Future Perfect (USA)/default.xbe" --work-dir games/_pipeline/futureperfect/out --project titles/futureperfect
```

Run the symbols step first, every time. Without it the recompiler still
finishes, but the game hangs at start-up.

### 6. Build

About 5 minutes:

```
cmake -S titles/futureperfect -B titles/futureperfect/build -G "Visual Studio 17 2022" -A x64
cmake --build titles/futureperfect/build --config Release -- -m
```

The result is in `titles\futureperfect\build\Release\`:

- `futureperfect_recomp_launcher.exe` — settings, then **Play**
- `futureperfect_recomp.exe` — the game itself
- `input_bindings.json` — the default controls

Started from there, the game finds your files in `games\` by itself.

### Running it from another folder

To keep the game somewhere else, copy into one folder:

- the two `.exe` files and `input_bindings.json` from `titles\futureperfect\build\Release\`;
- `avcodec-62.dll`, `avutil-60.dll` and `swresample-6.dll` from `third_party\ffmpeg\bin\`;
- your extracted game, as a folder named `game` beside them (so that
  `game\default.xbe` exists).

The environment variable `RECOMP_GAME_DIR` can point at the game files instead.

## Playing

### Default controls

| Action | Keyboard and mouse | Pad |
| --- | --- | --- |
| Move | W A S D | Left stick |
| Look | Mouse | Right stick |
| Fire | Left mouse button | Right trigger |
| Aim (zoom) | Right mouse button (hold) | Right stick click |
| Throw grenade | G, middle mouse button | Left trigger |
| Activate / select | E, Space, Enter | A |
| Melee / back | F, Backspace | B |
| Reload | R | X |
| Menu option | V | Y |
| Crouch | C, Left Ctrl | Left stick click |
| Previous / next weapon | Q, mouse wheel, ← → | D-pad left / right |
| Zoom in / out | ↑ ↓, side mouse buttons | D-pad up / down |
| Pause | Esc | Start |
| Scores | Tab | Back |

Everything can be changed in F1 → Controls. Click in the window to give the
game the mouse; Alt+Tab gives it back.

### Keys that work any time

| Key | |
| --- | --- |
| F1 | Settings panel (also Back+Start on a pad) |
| F9 | Frame-rate counter |
| F10 | Cycle the frame pacing |
| F12 | Screenshot |
| Alt+Enter | Fullscreen / window |

### Co-op on two PCs

For two players, each on their own PC. Both need this same release and the
same (USA) game. Set it up in the game (F1, then the *Co-op* page) or in the
launcher's *Co-op* tab; the two work the same way.

1. The host leaves *Play* on *Host* and sends player 2 the *Invite code*
   (FP-XXXX-XXXX-XXXX). Click it to copy it; in the game, Enter on it does
   too.
2. The host chooses *Host co-op* (in the launcher, **Host**). The game starts
   again and waits for player 2 in a small window, which shows the code again
   with a *Copy code* button. The first time, Windows asks whether to let the
   game through its firewall: allow it.
3. Player 2 sets *Play* to *Join*, pastes the code into *Invite code or
   address* (Ctrl+V), and chooses *Join co-op* (in the launcher, **Join**).
4. Both games start together, from the host's saves, and go straight to the
   main menu with both players in: 2 players, the host's profile for player
   1, and a profile for player 2 (made the first time). Pick *Story Co-op*
   or anything else on the host.

To stop, choose *Leave co-op* on the F1 *Co-op* page: the game starts again on
its own, and the other player is told.

No ports to open, for most connections: with the code, the two games find
each other through a free public relay (ntfy.sh), where everything they say
is encrypted with the code, and then connect to each other directly. A few
networks are too strict for that (some mobile and office ones), and the game
says so. Then the host can give out an address instead, which the *Co-op*
page also shows: their internet address, with UDP port 47800 forwarded on
their router (the game asks the router to do it itself, by UPnP, and says
whether it did), or their address on a VPN both have joined (ZeroTier,
Tailscale, Radmin VPN). In the same house, *On this network* always works.

To try it on one PC, run both copies there: the host plays on the keyboard
and mouse, and player 2 on a controller.

Player 2's own saves are left alone: the host's are copied to
`%LOCALAPPDATA%\xboxrecomp\online` for the session. Progress is kept in the
host's saves.

### Where things are kept

- Settings: `%APPDATA%\xboxrecomp\titles\45410066.conf`
- Controls: `%APPDATA%\xboxrecomp\titles\45410066.bindings.json`
- Saves: `%LOCALAPPDATA%\xboxrecomp`
- Log of the last run: `futureperfect_recomp.log` beside the game's `.exe`.
  Please include it with any bug report.

## Known limitations

- Only the USA release, and only on Windows.
- Co-op on two PCs has so far been tested with two copies on one PC, invite
  codes included, but not yet between two houses. Online versus and the game's own LAN (System Link) mode
  are not working yet. Split-screen works.
- Front-end screens not yet seen with the mouse (Mapmaker, some Challenge and
  Extras pages) may still need the keys or a pad.
- Button prompts cover A, B, X and Y. Other icons still show the pad's.
- Frame interpolation adds roughly 10 ms of input delay, and a few effects the
  game moves itself (some particles) stay at 60 frames a second.
- Not every level has been played through on this build yet.

## How it works, and credits

This is built on **[xboxrecomp](https://github.com/sp00nznet/xboxrecomp)** by
sp00nz, a static recompilation toolkit for original Xbox games, and on
**[nemonicicon/ts3FPrecomp](https://github.com/nemonicicon/ts3FPrecomp)**, the
TimeSplitters: Future Perfect project that brought the game up on it. The
toolkit's own documentation is in
[README.xboxrecomp.md](README.xboxrecomp.md) and
[INSTRUCTIONS.md](INSTRUCTIONS.md). The technical notes behind this project's
changes are in [docs/technical/](docs/technical/) — for example
[in-game-menu.md](docs/technical/in-game-menu.md),
[input-binding.md](docs/technical/input-binding.md) and
[frame-interpolation.md](docs/technical/frame-interpolation.md).

## License

GPL-3.0 — see [LICENSE](LICENSE). The original toolkit's MIT notice is kept in
[LICENSE.upstream-MIT](LICENSE.upstream-MIT), and third-party licenses are in
[LICENSES/](LICENSES/).
