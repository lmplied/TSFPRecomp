/*
 * hle_netplay.h -- two machines playing one game in lockstep.
 *
 * Each machine runs the whole game. Every frame each sends the other its
 * own player's controls for a frame a few ahead (the input delay), and
 * before a frame starts each waits until it has every player's controls
 * for that frame. The game reads those, the same on both machines, so the
 * two play out the same -- given a game that does the same thing with the
 * same inputs, which hle_lockstep.h is the measurement of (and
 * RECOMP_LOCKSTEP_CLOCK a condition for).
 *
 *   RECOMP_NET=host:<port>               wait for the other player, who is
 *                                        player 2; this machine is player 1
 *   RECOMP_NET=join:<address>:<port>     join a host, as player 2
 *   RECOMP_NET_DELAY=<frames>            input delay, 3 by default
 *
 * UDP. Each packet carries the last 16 frames' controls, so a packet lost
 * costs nothing while the next one arrives. Every 60 frames each side also
 * sends a hash of the game's state (hle_net_hash_range); a difference is
 * logged as a desync, with the frame, and shown (hle_net_notice).
 *
 * A session begins before either game does (hle_net_session_begin): the
 * joiner's build and game files are checked against the host's, the host's
 * saves are sent to the joiner (kept apart from the joiner's own, in an
 * `online` folder in its save folder), the host's settings that change how
 * the game plays out are taken by both, and the input delay is set from the
 * round trip measured (RECOMP_NET_DELAY on the host overrides it). Both games
 * then start from the same saves, settings and first frame.
 */
#ifndef XBOXRECOMP_HLE_NETPLAY_H
#define XBOXRECOMP_HLE_NETPLAY_H

#include <stddef.h>
#include <stdint.h>
#include "input_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* From the title's main, as soon as the XBE is read and before anything
 * else -- the memory layout starts threads that read settings (the lockstep
 * clock among them), and the kernel reads saves: with RECOMP_NET set, the
 * session's setup, in a small window of its own (above). 1 to go on -- also
 * when no session was asked for -- and 0 when the session cannot go ahead or
 * the player cancelled (the reason already shown): the title should quit. */
int hle_net_session_begin(const void *xbe, size_t xbe_size, const char *game_dir);

/* The title starts sessions (it calls hle_net_session_begin): co-op can be
 * offered in its menus. */
int hle_net_supported(void);

/* Co-op from inside the game: both games must start together, so this one
 * starts again -- into a session with `spec` (RECOMP_NET's "host:<port>" or
 * "join:<address>:<port>"), or with NULL out of the one it is in -- from the
 * environment it was started with, and this process ends. 0 when the new
 * one could not be started; otherwise it does not return. */
int hle_net_restart(const char *spec);

/* In a session: the other machine's address, the input delay in frames and
 * the round trip measured as the session began; 0 outside one. */
int hle_net_session_info(char *peer, size_t n, int *delay, unsigned *rtt_ms);

/* This PC's public address, for a host to give out (recomp_net_addr.h). The
 * first call starts asking, on a thread of its own: 1 with the address, 0
 * while still asking, -1 when nobody answered. */
int hle_net_public_addr(char *out, size_t n);

/* The text to the clipboard: an address to send a friend. */
void hle_net_copy_text(const char *text);

/* RECOMP_NET is set. */
int hle_net_active(void);

/* The end of the frame-th frame (hle_lockstep_swap): this machine's
 * controls go out for a frame ahead, and the next frame's controls for both
 * players are waited for and taken. */
void hle_net_frame_end(unsigned long frame);

/* While netplay drives game port `port`: 1, with the controls for the frame
 * running; 0 for a port that is not netplay's. */
int hle_net_port_input(unsigned port, RecompInputGamepad *out);

/* A title that turns its player's view by the mouse itself (1:1 aim) does
 * not write the turn in a session: it hands it here, in its own units, and
 * the turn goes out with this machine's controls; port_aim then gives each
 * player's turn for the frame running, the same on both machines, to apply.
 * port_aim is 1 while netplay carries that port, and gives a frame's turn
 * once: asked again in that frame, the turn is 0. */
void hle_net_add_local_aim(float dx, float dy);
int  hle_net_port_aim(unsigned port, float *dx, float *dy);

/* Which of this machine's input ports feeds its player in a session: 0, or
 * 1 for player 2's copy when both games run on one PC (the host then has
 * the keyboard and mouse, player 2 the first controller). A title's own
 * mouse handling takes the mouse only when it is bound to this port. */
int hle_net_local_input_port(void);

/* This machine's player's game port in a session, or -1 outside one. */
int hle_net_local_port(void);

/* A line for the screen while a session has something to say -- the other
 * player is late, or the games have drifted apart -- or NULL. */
const char *hle_net_notice(void);

/* The game ports netplay reports as plugged in, as bits. */
unsigned hle_net_ports(void);

/* The part of guest memory whose hash is compared between the machines: the
 * game's own state, without what differs between machines and does not
 * matter (a title project knows which). */
void hle_net_hash_range(uint32_t va, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif
