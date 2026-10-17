/*
 * hle_input.c -- XAPI controller input replaced by name.
 *
 * Adapted from doaxbv-re (https://github.com/NoRain211/doaxbv-re,
 * recomp-runtime/input_adapter.c), GPL-3.0.
 *
 * The game's own XAPI enumerates pads through the USB stack, which needs an
 * emulated OHCI controller that finds a device, runs its SETUP transfers and
 * reports it. Ours starts and never gets that far: Burnout 2 calls
 * XGetDeviceChanges 13,172 times, is never told a pad arrived, and so never
 * calls XInputOpen or XInputGetState -- "Press START" cannot be pressed.
 *
 * doaxbv-re's answer, reused: replace the XAPI input functions and keep a
 * small model with a pad on port 0 (input_model.c), sampled from the host's
 * XInput pad 0 and keyboard (input_host.c).
 *
 * Differences from doaxbv-re:
 *  - keyed by XDK function name, not one title's addresses;
 *  - the gamepad device type comes from the title's XDK symbols by name
 *    (HLE_IMPORT_VAR), where doaxbv-re hard-codes its title's address. A
 *    title passes this pointer to say a call is about pads; memory units
 *    (g_DeviceType_MU) and the rest get "nothing connected";
 *  - XInputSetState also completes the request's status word, which a title
 *    may poll;
 *  - a lock, since nothing guarantees one guest thread;
 *  - four ports rather than one. Which host device stands behind each is
 *    src/input/input_bindings.c reading the config file the input UI writes;
 *    a port with no device is reported empty, and a pad plugged in while the
 *    title runs arrives through XGetDeviceChanges, the way the console would
 *    have reported it.
 */
/* The NT type vocabulary: <windows.h> on Windows, the POSIX primitives
 * (critical sections) elsewhere. XInputOpen and friends below are the XDK
 * functions being replaced, not calls into the host's XInput. */
#include "platform/xbox_winnt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "input_model.h"
#include "input_host.h"
#include "input_bindings.h"
#include "hle_netplay.h"

/* RECOMP_INPUT_LOG=1 -- see the read in XInputGetState below. */
static int input_log_wanted(void)
{
    static int wanted = -1;
    if (wanted < 0) {
        const char *v = getenv("RECOMP_INPUT_LOG");
        wanted = v && *v && strcmp(v, "0") != 0;
    }
    return wanted;
}

/* RECOMP_INPUT_CAPS=zero -- report the pad's controls as absent, the way this
 * file did before September 2026. Kept as a switch because "does this title
 * believe XInputGetCapabilities?" is a question worth being able to ask
 * again without a rebuild. */
static int zeroed_capabilities_wanted(void)
{
    static int wanted = -1;
    if (wanted < 0) {
        const char *v = getenv("RECOMP_INPUT_CAPS");
        wanted = v && strcmp(v, "zero") == 0;
    }
    return wanted;
}

HLE_IMPORT_VAR(g_DeviceType_Gamepad);

/* XINPUT_CAPABILITIES is
 *      BYTE Type; BYTE SubType; WORD Reserved;   -- 4 bytes
 *      union { XINPUT_GAMEPAD Gamepad; ... } In; -- 18 bytes, at offset 4
 *      union { XINPUT_RUMBLE Rumble; }      Out; --  4 bytes, at offset 22
 * so 26 bytes, not 25. In and Out are not a *state*: each field is a mask
 * saying whether that control exists and at what resolution, so 0xFF there
 * means "present, full range" and 0 means "this pad has no such control".
 * Cxbx-Reloaded fills the whole In+Out block with 0xFF for the same reason. */
enum {
    CAPABILITIES_SIZE = 26u,          /* XINPUT_CAPABILITIES */
    CAPABILITIES_IN = 4u,             /* In.Gamepad, then Out.Rumble */
    CAPABILITIES_MASKED = 22u,        /* 18 + 4 bytes of "this control exists" */
    DEVTYPE_GAMEPAD = 1u,             /* XINPUT_DEVTYPE_GAMEPAD */
    DEVSUBTYPE_GC_GAMEPAD = 1u,       /* XINPUT_DEVSUBTYPE_GC_GAMEPAD */
    STATE_SIZE = 22u,                 /* XINPUT_STATE */
    FEEDBACK_LEFT_MOTOR = 0x42u,      /* after the 66-byte feedback header */
    FEEDBACK_RIGHT_MOTOR = 0x44u,
    ERR_SUCCESS = 0u,
    ERR_INVALID_PARAMETER = 0x57u,
    ERR_DEVICE_NOT_CONNECTED = 0x48Fu,
};

static RecompInputModel g_model;
static CRITICAL_SECTION g_lock;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    uint32_t present, port, count = 0u;

    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_lock);
    recomp_bindings_init();               /* logs the config file it read */
    present = recomp_bindings_present_mask() | recomp_input_seq_ports() | hle_net_ports();
    recomp_input_reset(&g_model, present);
    for (port = 0u; port < RECOMP_INPUT_PORT_COUNT; port++)
        if (present & (1u << port))
            count++;
    fprintf(stderr, "[INPUT] XAPI input replaced by name: %u pad(s)%s; "
                    "gamepad type 0x%08X\n", count,
            getenv("RECOMP_FAKE_INPUT") ? ", scripted presses on port 1" : "",
            hle_var_g_DeviceType_Gamepad);
    for (port = 0u; port < RECOMP_INPUT_PORT_COUNT; port++)
        if (present & (1u << port))
            fprintf(stderr, "[INPUT]   port %u <- %s\n", port + 1u,
                    recomp_bindings_device_name(port));
    if (!hle_var_g_DeviceType_Gamepad)
        fprintf(stderr, "[INPUT] g_DeviceType_Gamepad is not named in this "
                        "title's XDK symbols: no pad will be reported\n");
    fflush(stderr);
    return TRUE;
}

static void lock(void)
{
    InitOnceExecuteOnce(&g_once, init, NULL, NULL);
    EnterCriticalSection(&g_lock);
}

static void unlock(void) { LeaveCriticalSection(&g_lock); }

/* Anything plugged in or pulled out since the last look, called under the
 * lock from the two functions a title uses to ask. input_bindings re-checks
 * XInput only a few times a second, so this is cheap to call as often as a
 * title asks -- and Burnout 2 asks 13,172 times. */
static void refresh_connected(void)
{
    uint32_t present = recomp_bindings_present_mask() | recomp_input_seq_ports() | hle_net_ports(),
             port;

    for (port = 0u; port < RECOMP_INPUT_PORT_COUNT; port++) {
        bool now = (present & (1u << port)) != 0u;
        if (now != ((g_model.connected_mask & (1u << port)) != 0u))
            recomp_input_set_connected(&g_model, port, now);
    }
}

static int is_gamepad(uint32_t type)
{
    return hle_var_g_DeviceType_Gamepad != 0u &&
           type == hle_var_g_DeviceType_Gamepad;
}

/* DWORD XGetDevices(PXPP_DEVICE_TYPE type) -- bitmask of connected ports. */
HLE_EXPORT(XGetDevices)
{
    uint32_t type = HLE_ARG(0), mask = 0u;

    lock();
    if (is_gamepad(type)) {
        refresh_connected();
        mask = recomp_input_get_devices(&g_model);
    }
    unlock();
    HLE_RETURN(mask);
}

/* BOOL XGetDeviceChanges(type, DWORD *insertions, DWORD *removals) */
HLE_EXPORT(XGetDeviceChanges)
{
    uint32_t type = HLE_ARG(0), ins_va = HLE_ARG(1), rem_va = HLE_ARG(2);
    uint32_t insertions = 0u, removals = 0u;
    bool changed = false;

    lock();
    if (is_gamepad(type)) {
        refresh_connected();
        changed = recomp_input_get_device_changes(&g_model, &insertions, &removals);
    }
    unlock();
    if (ins_va) HLE_MEM32(ins_va) = insertions;
    if (rem_va) HLE_MEM32(rem_va) = removals;
    HLE_RETURN(changed ? 1u : 0u);
}

/* HANDLE XInputOpen(type, DWORD port, DWORD slot, PXINPUT_POLLING_PARAMETERS) */
HLE_EXPORT(XInputOpen)
{
    uint32_t type = HLE_ARG(0), port = HLE_ARG(1), slot = HLE_ARG(2), handle = 0u;
    static int said;

    lock();
    if (is_gamepad(type) && slot == 0u)
        handle = recomp_input_open(&g_model, port);
    unlock();
    if (handle && !said) {
        said = 1;
        fprintf(stderr, "[INPUT] XInputOpen port %u (%s) -> handle 0x%08X\n",
                port + 1u, recomp_bindings_device_name(port), handle);
        fflush(stderr);
    }
    HLE_RETURN(handle);
}

/* VOID XInputClose(HANDLE) */
HLE_EXPORT(XInputClose)
{
    lock();
    recomp_input_close(&g_model, HLE_ARG(0));
    unlock();
    HLE_RETURN(0);
}

/* DWORD XInputGetCapabilities(HANDLE, PXINPUT_CAPABILITIES) */
HLE_EXPORT(XInputGetCapabilities)
{
    uint32_t handle = HLE_ARG(0), out = HLE_ARG(1), packet;
    RecompInputGamepad pad;
    uint32_t result = ERR_DEVICE_NOT_CONNECTED;
    static int said;

    if (!out)
        HLE_RETURN(ERR_INVALID_PARAMETER);
    lock();
    if (recomp_input_get_state(&g_model, handle, &packet, &pad)) {
        uint8_t *p = (uint8_t *)HLE_PTR(out);

        memset(p, 0, CAPABILITIES_SIZE);
        p[0] = (uint8_t)DEVTYPE_GAMEPAD;
        p[1] = (uint8_t)DEVSUBTYPE_GC_GAMEPAD;
        /* Every control present, at full resolution. Zeroing this block told
         * the title its pad had no sticks, no analog buttons and no motors,
         * which a title is entitled to believe. TimeSplitters 2 asks once,
         * immediately after XInputOpen, and was not visibly harmed by the
         * old answer -- so this is a latent defect found while chasing a
         * different one, not a fix for it. */
        if (!zeroed_capabilities_wanted())
            memset(p + CAPABILITIES_IN, 0xFF, CAPABILITIES_MASKED);
        result = ERR_SUCCESS;
    }
    unlock();
    if (!said) {
        said = 1;
        fprintf(stderr, "[INPUT] XInputGetCapabilities(handle 0x%08X) -> %s\n",
                handle, result == ERR_SUCCESS
                    ? (zeroed_capabilities_wanted()
                       ? "a gamepad with every control reported absent "
                         "(RECOMP_INPUT_CAPS=zero)"
                       : "a gamepad with every control present")
                    : "device not connected");
        fflush(stderr);
    }
    HLE_RETURN(result);
}

/* DWORD XInputGetState(HANDLE, PXINPUT_STATE) */
HLE_EXPORT(XInputGetState)
{
    uint32_t handle = HLE_ARG(0), out = HLE_ARG(1), packet, port;
    RecompInputGamepad sampled, pad;
    uint32_t result = ERR_DEVICE_NOT_CONNECTED;
    uint8_t *p;
    uint32_t i;

    if (!out)
        HLE_RETURN(ERR_INVALID_PARAMETER);
    /* The device bound to this handle's own port, sampled outside the lock:
     * reading four host devices inside one critical section would serialise
     * four guest threads on the slowest of them. */
    if (!recomp_input_port_for_handle(handle, &port))
        HLE_RETURN(ERR_DEVICE_NOT_CONNECTED);
    recomp_input_host_sample_port(port, &sampled);
    lock();
    recomp_input_set_gamepad(&g_model, handle, &sampled);
    if (recomp_input_get_state(&g_model, handle, &packet, &pad)) {
        p = (uint8_t *)HLE_PTR(out);
        memset(p, 0, STATE_SIZE);
        memcpy(p + 0, &packet, 4);                       /* dwPacketNumber */
        memcpy(p + 4, &pad.buttons, 2);                  /* wButtons */
        for (i = 0; i < RECOMP_INPUT_ANALOG_BUTTON_COUNT; i++)
            p[6 + i] = pad.analog_buttons[i];            /* bAnalogButtons */
        memcpy(p + 14, &pad.thumb_lx, 2);
        memcpy(p + 16, &pad.thumb_ly, 2);
        memcpy(p + 18, &pad.thumb_rx, 2);
        memcpy(p + 20, &pad.thumb_ry, 2);
        result = ERR_SUCCESS;
        /* RECOMP_INPUT_LOG=1: what the title is actually reading, twice a
         * second while anything is held. "The game ignores my controller"
         * splits here into a binding that produces nothing, a title that
         * never reads this port, and a title that reads it and does
         * nothing with it -- and only the last one is the title's fault. */
        if (input_log_wanted()) {
            /* The analog buttons too (A B X Y black white L R): on this
             * console those are bytes, not bits in wButtons, and a scripted
             * `a` press that never appears here is what "the menu ignores
             * A" looks like from the title's side. */
            int any = pad.buttons || pad.thumb_lx || pad.thumb_ly ||
                      pad.thumb_rx || pad.thumb_ry;
            for (i = 0; i < RECOMP_INPUT_ANALOG_BUTTON_COUNT; i++)
                any |= pad.analog_buttons[i] != 0;
            if (any) {
                static ULONGLONG next;
                ULONGLONG now = GetTickCount64();
                if (now >= next) {
                    next = now + 500;
                    fprintf(stderr, "[INPUT] port %u reads buttons 0x%04X analog", port + 1u,
                            pad.buttons);
                    for (i = 0; i < RECOMP_INPUT_ANALOG_BUTTON_COUNT; i++)
                        fprintf(stderr, " %02X", pad.analog_buttons[i]);
                    fprintf(stderr, " lx %6d ly %6d rx %6d ry %6d\n", pad.thumb_lx,
                            pad.thumb_ly, pad.thumb_rx, pad.thumb_ry);
                    fflush(stderr);
                }
            }
        }
    }
    unlock();
    HLE_RETURN(result);
}

/* DWORD XInputSetState(HANDLE, PXINPUT_FEEDBACK) -- rumble. Recorded, and the
 * request's status (the header's first word) completed at once: on hardware
 * it reads ERROR_IO_PENDING until the motors take the value. */
HLE_EXPORT(XInputSetState)
{
    uint32_t handle = HLE_ARG(0), fb = HLE_ARG(1), result = ERR_INVALID_PARAMETER;

    if (fb) {
        uint16_t left = *(uint16_t *)HLE_PTR(fb + FEEDBACK_LEFT_MOTOR);
        uint16_t right = *(uint16_t *)HLE_PTR(fb + FEEDBACK_RIGHT_MOTOR);
        uint32_t port;
        lock();
        result = recomp_input_set_feedback(&g_model, handle, left, right)
                 ? ERR_SUCCESS : ERR_DEVICE_NOT_CONNECTED;
        unlock();
        /* And to the pad itself, which recomp_pad drives through SDL3 or
         * XInput -- so a DualSense rumbles as an Xbox pad would. */
        if (result == ERR_SUCCESS && recomp_input_port_for_handle(handle, &port))
            recomp_bindings_rumble(port, left, right);
        HLE_MEM32(fb) = result;
    }
    HLE_RETURN(result);
}
