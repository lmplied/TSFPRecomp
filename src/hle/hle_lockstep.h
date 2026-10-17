/*
 * hle_lockstep.h -- the title's frames, counted, and what its memory holds
 * at them: the measurements lockstep play is built on.
 *
 * Lockstep multiplayer runs the same game on each machine from the same
 * inputs, frame by frame, and needs the two to stay identical. Whether they
 * do is measured here:
 *
 *   RECOMP_STATE_HASH=<file>       every RECOMP_STATE_HASH_EVERY frames
 *                                  (default 60), a line of the 64 KB chunks'
 *                                  hashes over all of guest memory: the low
 *                                  64 MB and the contiguous window at
 *                                  0x80000000. Two runs' files side by side
 *                                  say when they part, and where.
 *   RECOMP_INPUT_SEQ_FRAMES=1      the input script's clock counts frames
 *                                  (each a sixtieth of a second) instead of
 *                                  milliseconds, so a press lands on the same
 *                                  frame in every run (input_host.c).
 */
#ifndef XBOXRECOMP_HLE_LOCKSTEP_H
#define XBOXRECOMP_HLE_LOCKSTEP_H

#ifdef __cplusplus
extern "C" {
#endif

/* D3DDevice_Swap and Present: a frame ended. */
void hle_lockstep_swap(void);

/* Frames ended so far. */
unsigned long hle_lockstep_frame(void);

#ifdef __cplusplus
}
#endif

#endif
