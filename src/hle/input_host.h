/*
 * input_host.h -- the host controller behind the replaced XAPI input.
 *
 * Adapted from doaxbv-re (https://github.com/NoRain211/doaxbv-re,
 * recomp-runtime/input_host_win32.h), GPL-3.0.
 */
#ifndef XBOXRECOMP_INPUT_HOST_H
#define XBOXRECOMP_INPUT_HOST_H

#include <stdbool.h>
#include "input_model.h"

/* Guest ordering of the analog button bytes. */
enum {
    HOST_ANALOG_A = 0, HOST_ANALOG_B, HOST_ANALOG_X, HOST_ANALOG_Y,
    HOST_ANALOG_BLACK, HOST_ANALOG_WHITE, HOST_ANALOG_LTRIG, HOST_ANALOG_RTRIG,
};

/* One sample of the host device bound to this port, as the Xbox gamepad the
 * title reads. Always succeeds: with nothing connected or pressed it is a
 * neutral pad. RECOMP_FAKE_INPUT and RECOMP_INPUT_SEQ presses are added on
 * port 0 only. Ports are 0-based here and 1-based in the UI and the config
 * file, where they are what is written on the console. */
bool recomp_input_host_sample_port(unsigned port, RecompInputGamepad *gamepad);

/* Port 0, for callers that only ever had one pad. */
bool recomp_input_host_sample(RecompInputGamepad *gamepad);

/* The ports RECOMP_INPUT_SEQ presses on (steps written p2.a and so on), as
 * bits: those read as a pad plugged in, and only the script drives them. */
unsigned recomp_input_seq_ports(void);

/* This machine's player's controls -- `port`'s devices (0, or 1 for a
 * second copy of the game on the same PC: hle_netplay.c) and port 0's
 * script -- as netplay sends them, whichever game port that player plays. */
void recomp_input_host_sample_local(unsigned port, RecompInputGamepad *gamepad);

#endif
