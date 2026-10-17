# Lockstep: online co-op on two PCs

*October 2026.* Online co-op for Future Perfect is lockstep. Each machine runs
the whole game in split-screen co-op. Every frame, the two swap their
players' controls, and each shows its own player full screen. The game needs
no netcode of its own, but both copies must stay identical, frame for frame,
given the same inputs. This page covers how that is measured, what had to
change for it to hold, how a session is set up, and what is still open.

Why lockstep and not streaming one machine's picture to the other: streaming
is what Steam Remote Play and Parsec already do (and *Keyboard and mouse:
player 1 alone* is for them). Lockstep sends a few hundred bytes a frame
instead of video, and each player gets their own resolution, frame rate,
field of view and sound, with no compression in the picture. The price is
that the game must be deterministic, which is what most of this page is
about.

## The tools

| Variable | What it does |
|---|---|
| `RECOMP_INPUT_SEQ_FRAMES=1` | the input script's times count the title's frames (a sixtieth of a second each) instead of milliseconds |
| `p2.a`, `p3.start`, ... in `RECOMP_INPUT_SEQ` | a step for another player; a port a script presses on reads as a pad plugged in |
| `RECOMP_LOCKSTEP_CLOCK=1` | everything below that makes the game the same on every machine (a session turns it on itself) |
| `RECOMP_STATE_HASH=<file>` | every `RECOMP_STATE_HASH_EVERY` frames (60), the hash of every 64 KB of guest memory |
| `RECOMP_STATE_HASH_RANGE=<va>:<size>,...` | only those ranges, cheap enough for every frame |
| `RECOMP_STATE_DUMP_AT=<frame>,...` | all guest memory at a frame's end, to `<file>.<frame>.bin` |
| `RECOMP_LOCKSTEP_JITTER=<ms>` (`_FRAMES=<from>-<to>`) | a random pause after each frame: a slower machine in miniature; what parts two runs under it parts two machines |
| `RECOMP_NET=host:<port>` / `join:<addr>:<port>` | a session (`hle_netplay.h`); the launcher's Co-op tab or the F1 Co-op page sets it |
| `RECOMP_NET_DELAY=<frames>` | on the host, the input delay, instead of one from the measured round trip |
| `FP_SOLO_VIEW=<player>` | one player's view full screen without a session, to look at it alone |
| `RECOMP_FP_MOUSE_TEST=<ms>:<dx>:<dy>:<ms long>,...` | mouse counts fed to the aim; in a session they go through the network like a real mouse's |

Scratchpad scripts compare hash files chunk by chunk (the first frame each
chunk differs) and dumps word by word, with the known noise left out.

## What makes two runs differ, and what was done

In the order they were found, each by a clean run against a jittered one:

1. **The world's step.** The frame clock (`sub_001538C0`) steps the world
   by the vblanks since the last frame, and vblanks come with real time.
   `RECOMP_LOCKSTEP_CLOCK` delivers one vblank per frame ended, and the frame
   clock's read of the count (`sub_00022030`, wrapped) is answered "one more
   than last time", so free vblanks during loading cannot reach it.
2. **What counts as a frame.** A loading screen draws on a thread of its own
   (`sub_00156CB0`) while the main thread loads, and swaps as often as time
   allows. Counting its swaps moved every later frame number, and with it
   every input and hash pinned to one. Frames are now the main thread's
   swaps only.
3. **Clocks.** The performance counter, the CPU timestamp counter, system
   time and KeTickCount are all read from the frame count under the lockstep
   clock (`kernel_hal.c`), a microsecond added per read on each thread so a
   wait on the clock still ends.
4. **Sound.** DirectSound's playback position, its stream packet
   completions and "has this stopped" all follow the frames under the lockstep
   clock (`hle_dsound*.c`), never the host's playback. The frames only: the
   vblanks a loading screen is given while the main thread loads (the clock
   runs free after three periods without a frame, item 1) are as many as the
   load took on this machine. Counted into the sound's time, and into
   KeTickCount, they set every sound after a load apart by that much, and the
   game followed its level music a second into the first level: the
   positions of things parted at frame 7,110, after a load 13 vblanks longer
   on one run than the other. Both clocks now stand still while a level
   loads. Two copies on one PC load in near enough the same time that most
   runs got away with it; two PCs would not have.
5. **The sound worker** (`sub_00030160`: DirectSoundDoWork and the game's
   stream service, then a 16 ms sleep) runs whenever the host schedules it.
   With 1 to 4 in place it no longer matters: a clean run and one jittered by
   up to 12 ms a frame are identical but for four words of known noise at
   frame 9400. The gate that would run it once a frame
   (`RECOMP_LOCKSTEP_GATE=1`) stalls the game, because the main thread waits
   on it within a frame, so it stays off.
6. **Files.** A save's dates and the drive's free space come from each
   host's own disk. Under the lockstep clock every file is dated 1 January
   2005, the clock's own start, and the drive reports a fresh console's free
   space (`kernel_file.c`).

7. **The CPU.** Not seen with two copies on one PC, and fixed ahead of two
   PCs: the generated code computes the x87's sines, cosines and powers with
   the C runtime (`sin` at 133 places, `cos` at 107, `exp2` at 9). On x64
   that takes an FMA3 path on a CPU that has one, which Microsoft documents
   may round differently. A session turns it off (`_set_FMA3_enable(0)`) on
   both machines. The GPU never reaches the game's state: a visibility test
   is answered "not yet" or zero pixels, never from the host's GPU, so each
   player's resolution is their own business.

8. **Movies.** A minute idle in the menus, Future Perfect plays its attract
   movie (`attract.xmv`), and the movie player (`hle_xmv_play.c`) showed a
   picture when its time came by the wall clock and ended when the host's
   sound had played out. Under the lockstep clock a picture comes due by the
   title's frames, and the movie ends when its last picture and the whole of
   its sound's length are due; the decoder is asked to be bit-exact. A clean
   run and a jittered one now play the whole movie (3,055 pictures), go back
   to the menu and start it again identical, through 17,880 frames.

Harmless differences, left out of comparisons: the stack cookie
(`0x0054DB04`, seeded from time at start), D3D's push buffer positions and
fence counts (they follow the host GPU), the frame-time profiling history
(`0x007A58C0`), thread stacks.

The game's own random numbers (`0x007497CC`) stayed identical through all of
this: nothing random depends on time.

## Each player's own view

Both machines run split-screen co-op, but each shows one player full
screen:

- **The layout is the same on both.** `sub_0025A570` lays out each camera's
  screen rectangle (`[camera+0x224]`, through `sub_00055D90`). In a session
  it is wrapped to give *every* player the full screen, on both machines.
  Giving each machine only its own player's camera full screen was tried
  first. It desynced within a level: the heap shifts by 0x30000 and the
  animation differs, because the game sizes things by the rectangle.
- **Each machine draws only its own player.** The frame (`sub_0003C440`)
  draws each screen's world (`sub_000C8120`, from `0x0003C799`) and then
  each HUD (`sub_000C84A0`, from `0x0003C847`), with the current player at
  `[0x007B0C7C]` and its camera at `[0x007B0CC0]`. Both are wrapped to mark
  whose pass is running. A draw filter in the D3D layer
  (`xbox_D3D8SetDrawFilter`) drops the other player's draws and clears.
  Cutscenes draw from player 1's pass alone, so when the local pass drew
  almost nothing the frame before (fewer than 50 draws), the other pass is
  kept.
- **Mouse aim goes over the network.** The 1:1 mouse writes the view angles
  directly, which would turn one machine's player only. In a session the
  turn is sent with the controls (`hle_net_add_local_aim`), and each frame
  both machines apply both players' turns (`hle_net_port_aim`).

## What a session fixes

What would let one machine's settings change how the game plays out on it
alone:

| | In a session |
|---|---|
| Field of view, weapon field of view | the game's own on both (`fp_apply_fov`) |
| Screen shape | 16:9 whatever the window's shape (`fp_aspect_follow`) |
| Pointing at a menu item | does not select it (a click still presses A, which is input and goes over the network) |
| Character select with the mouse | a click picks; walking the portraits is off for player 2, whose selection is not the one the walk reads |
| Pause in the background | off: a pause pressed for one is a pause for both |
| Keyboard and mouse alone | off: this machine's keyboard, mouse and first pad all play its player |
| Skip intro, widescreen | the host's, on both |
| Lockstep clock | on |
| Frame rate | 60 a second whatever the cap says (`lockstep_pace`): a frame is a sixtieth of a second of the game's time, so uncapped at 120 the game ran twice as fast |
| Both games on one PC | the host plays on the keyboard and mouse, player 2 on the first controller (`peer_on_this_pc`; player 2's copy reads its second port) |

**Straight to the main menu.** Between the game starting and its main menu
come how many players, and a profile for each. In co-op both machines go
through them by themselves (`fp_coop_auto`, recomp_manual.c). Each presses
for its own player only, through its own controls, and decides what to press
from the screen's lists, which are the same on both. The host chooses
2 Player and its first profile. Player 2 presses Start, takes the first
profile the host has left free, or creates "Player 2" (name, then storage
device). It stops at the main menu, where *Change Players* is the players'
own again. `RECOMP_FP_COOP_AUTO=0` leaves the screens to the players.

## A session

`hle_net_session_begin`, from the title's `main` between the memory layout
(which tells the settings which title this is) and the kernel (which reads
settings and saves):

1. **Join.** The joiner sends its protocol, build (link time and size of the
   executable) and a hash of `default.xbe`. A host refuses another build or
   other game files with a reason the joiner shows.
2. **Saves.** The host sends its `UDATA\45410066` and `TDATA\45410066`. A
   missing chunk of 1 KB is asked for again, and a hash checks the whole.
   The joiner writes them to `online\` in its own save folder and plays from
   there, so its own saves are never touched. Each path is checked: nothing
   may climb out of that folder. The host plays on its own saves, and its
   progress is kept.
3. **Round trip.** Five pings. The input delay is set from the middle one,
   two frames on a LAN and three to five across a country.
   `RECOMP_NET_DELAY` on the host overrides it.
4. **Go.** Both pin the settings above and start the game. Each waits for the
   other's controls before every frame.

**Starting one.** From the launcher's Co-op tab, which starts the game with
`RECOMP_NET` set, or from inside the game: the F1 panel's Co-op page calls
`hle_net_restart`, which starts the game again with `RECOMP_NET` and ends
this run. The new run gets the environment this one started with
(`env_snapshot`, taken before a session pins anything), so *Leave co-op*
also leaves the session's settings and the joiner's save folder behind.
Starting again is the price of both games having to begin together.

**Invites.** A forwarded port is often impossible -- a provider's shared
address (CGNAT), a router nobody can log in to. An invite code
(`recomp_invite.h`) needs none: the host's game makes one (the Co-op page
and the launcher show it before hosting) and player 2 types it in. Each game
learns where it can be reached: its addresses on its networks, and what a
STUN server sees of the very socket it plays on (`recomp_net_stun_on`).
These cross on a public relay, ntfy.sh -- the topic a hash of the code, each
message AES-256-GCM sealed with another, so the relay holds an unguessable
name and ciphertext. The host posts its addresses and listens; the joiner
reads them (asking again for a minute and a half, as the host may not have
started), posts its own, and sends its JOIN to each of the host's addresses
at once, while the host sends `PKT_PUNCH` to each of the joiner's. That
opens each router to the other, and the first address a packet comes back
from is the way (`invite_adopt`). If nothing gets through in 30 seconds the
joiner says the networks are too strict, and an address is the way left.
`RECOMP_NET_SIGNAL` names another ntfy server, or 0 for none;
`RECOMP_NET_INVITE_PUBLIC_ONLY=1` offers only the STUN address, to test the
way through a router from one PC (which only works where the router sends a
packet back to its own public address: many don't).

While the two meet, a small window says what is happening and has a Cancel
button. The host's shows the invite code with a *Copy code* button, and the
addresses player 2 can join on instead, each with the port: the public one (asked of a STUN server, Google's or Cloudflare's:
`recomp_net_addr.c`, one small UDP packet each way; `RECOMP_NET_PUBLIC_IP=0`
never asks) and this PC's on its networks. A *Copy address* button puts the
likelier one on the clipboard. If the router's own public address, from
UPnP, is a private or carrier-shared one, or not the one STUN sees, the
provider's NAT (CGNAT) is in the way and the window says to use a VPN.

**The router.** A host behind a home router can only be reached from the
internet on a port the router forwards. While the host waits, the game asks
the router to forward the port by UPnP (Windows' own client, `natupnp.h`, on
a thread of its own because finding the router can take seconds), and learns
the public address on the way. The forward is removed when the game closes.
One this PC left behind earlier is taken back first; another PC's is never
touched. `RECOMP_NET_UPNP=0` leaves the router alone (the test scripts set
it).

During play, if a frame waits more than a second and a half, that window
opens over the game (without taking the focus). It says whether the other
game is still there (it sends a heartbeat every quarter second, so a level
loading reads as loading), silent, or gone: a game being closed tells the
other one (`PKT_BYE`, from `xbox_HostExit`). It has a Quit button. A
difference in the state hashes, compared every 60 frames, is shown on
screen.

## Where it stands

Measured with two copies on one PC, from boot through the menus, character
select, a level's opening cutscene and play:

- With each machine showing its own player, scripted movement, scripted
  mouse aim on both sides and the joiner starting with no saves of its own,
  the game objects (`0x82000000`, 20 MB) are identical in every 64 KB chunk
  through 19,440 frames.
- With the joiner stalling for seconds at a time, the host waits and both
  carry on identical.
- With the two players' own settings different (one at 1x, the other at 2x
  with frame interpolation, an uncapped frame rate and a field of view of
  90), identical through 19,230 frames.
- Started the way a player starts it, from two launchers (one hosting, one
  joining 127.0.0.1): both games run, and the joiner plays on the host's
  saves. A game closed mid-session tells the other, whose window says so.
- Started from inside the game, by invite code: one copy hosts from its F1
  Co-op page, the other types the code into its own, finds the host through
  ntfy.sh and connects; both go straight to the main menu (5 presses each).
  With the host then starting Story Co-op and both moving in the level,
  identical through 16,590 frames (after the load fix in item 4).
- Idle in the menus into the attract movie and back: identical.
- Frames paced to 60 a second with the frame rate uncapped.

Still to do: a session between two houses. The way through two routers
(hole punching) cannot be tried from one network, and this one's router does
not send packets back to its own public address, so the public-only test
ends in the "too strict" message, as it should. The UPnP forward has not been
tried against a router either. Where both fail, a VPN (ZeroTier, Tailscale,
Radmin) always works; a relay of our own would be the next step. Online
versus would use the same session.
