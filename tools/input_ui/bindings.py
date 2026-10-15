"""
The binding config: what is in it, what the defaults are, where it lives.

This is the Python half of a vocabulary the runtime also speaks --
src/input/input_bindings.c reads the same file with the same control names,
source names and defaults. test_runtime_vocabulary.py reads the C tables out
of that file and fails if the two have drifted, because a name that exists on
only one side is a binding the UI can write and the game will ignore.

The file is JSON:

    {
      "version": 1,
      "pad_api": "sdl",
      "controllers": [
        {"port": 1, "device": "gamepad:0", "deadzone": 7849,
         "bindings": {"a": ["pad:a", "key:Z"], "start": "key:RETURN", ...}},
        ...
      ]
    }

Ports are 1-4, as they are written on the console. A binding is one source or
a list of them, and any of them pressing the control presses it -- that is how
the default port 1 answers to both a pad and the keyboard. A control the file
does not mention keeps its default; "none" or [] unbinds it.

"pad_api" is which API reads the pads, for every port: "sdl" (SDL3, every kind
of controller; the default) or "xinput" (Xbox controllers only). A device is
"gamepad:N", the N-th pad through that API; "xinput:N" is what files written
before SDL3 say, and means the same slot.
"""

import json
import os

VERSION = 1
PORTS = 4
PAD_APIS = ("sdl", "xinput")
DEADZONE_DEFAULT = 7849

# Every Xbox input, as (name in the file, label in the UI, group).
# A stick is four half-axes rather than two axes so that a key -- which has no
# middle -- can drive one, and so the runtime can treat every source as a
# magnitude. Order is the order the UI lists them in.
CONTROLS = [
    ("a", "A", "Buttons"),
    ("b", "B", "Buttons"),
    ("x", "X", "Buttons"),
    ("y", "Y", "Buttons"),
    ("black", "Black", "Buttons"),
    ("white", "White", "Buttons"),
    ("start", "Start", "Buttons"),
    ("back", "Back", "Buttons"),
    ("ltrigger", "Left trigger", "Triggers"),
    ("rtrigger", "Right trigger", "Triggers"),
    ("dpad_up", "D-pad up", "D-pad"),
    ("dpad_down", "D-pad down", "D-pad"),
    ("dpad_left", "D-pad left", "D-pad"),
    ("dpad_right", "D-pad right", "D-pad"),
    ("lstick_up", "Left stick up", "Left stick"),
    ("lstick_down", "Left stick down", "Left stick"),
    ("lstick_left", "Left stick left", "Left stick"),
    ("lstick_right", "Left stick right", "Left stick"),
    ("lthumb", "Left stick click", "Left stick"),
    ("rstick_up", "Right stick up", "Right stick"),
    ("rstick_down", "Right stick down", "Right stick"),
    ("rstick_left", "Right stick left", "Right stick"),
    ("rstick_right", "Right stick right", "Right stick"),
    ("rthumb", "Right stick click", "Right stick"),
]

CONTROL_NAMES = [name for name, _label, _group in CONTROLS]
GROUPS = []
for _name, _label, _group in CONTROLS:
    if _group not in GROUPS:
        GROUPS.append(_group)

# What a "pad:" source may name: a host pad's buttons, its two analog
# triggers, and the four stick axes split into halves.
PAD_BUTTONS = [
    "a", "b", "x", "y", "lshoulder", "rshoulder", "start", "back",
    "lthumb", "rthumb", "dpad_up", "dpad_down", "dpad_left", "dpad_right",
]
PAD_AXES = [a + s for a in ("lx", "ly", "rx", "ry") for s in ("+", "-")]
PAD_SOURCES = (["pad:" + b for b in PAD_BUTTONS]
               + ["pad:lt", "pad:rt"]
               + ["pad:" + a for a in PAD_AXES])

# What a "mouse:" source may name: five buttons, the wheel (each notch a short
# press), and motion as four half-axes the way a stick is bound -- x+ right,
# y+ away from the player. Mirrors MOUSE_SOURCES in input_bindings.c.
MOUSE_SOURCE_NAMES = [
    "left", "right", "middle", "x1", "x2", "wheel_up", "wheel_down",
    "x+", "x-", "y+", "y-",
]
MOUSE_SOURCES = ["mouse:" + m for m in MOUSE_SOURCE_NAMES]

# How mouse motion turns into a stick, per controller, with the range the
# runtime accepts: (key, default, low, high). Mirrors MouseFeel and its
# parsing in input_bindings.c.
MOUSE_SETTINGS = [
    ("mouse_sensitivity", 1.0, 0.01, 100.0),
    ("mouse_deadzone", 0, 0, 32766),
    ("mouse_exponent", 1.0, 0.1, 10.0),
    ("mouse_y_scale", 1.0, 0.1, 10.0),
    ("mouse_invert_y", False, None, None),
    ("mouse_zoom_match", True, None, None),
    ("mouse_window_ms", 16, 1, 250),
]

# Virtual-key names a "key:" source may use, beyond the single letters and
# digits (which are their own code on Windows). Mirrors the KEYS table in
# src/input/input_bindings.c.
KEY_NAMES = {
    "UP": 0x26, "DOWN": 0x28, "LEFT": 0x25, "RIGHT": 0x27,
    "RETURN": 0x0D, "ENTER": 0x0D, "BACK": 0x08, "BACKSPACE": 0x08,
    "SPACE": 0x20, "TAB": 0x09, "ESCAPE": 0x1B,
    "SHIFT": 0x10, "LSHIFT": 0xA0, "RSHIFT": 0xA1,
    "CTRL": 0x11, "LCTRL": 0xA2, "RCTRL": 0xA3,
    "ALT": 0x12, "LALT": 0xA4, "RALT": 0xA5,
    "CAPSLOCK": 0x14, "INSERT": 0x2D, "DELETE": 0x2E,
    "HOME": 0x24, "END": 0x23, "PAGEUP": 0x21, "PAGEDOWN": 0x22,
    "F1": 0x70, "F2": 0x71, "F3": 0x72, "F4": 0x73, "F5": 0x74, "F6": 0x75,
    "F7": 0x76, "F8": 0x77, "F9": 0x78, "F10": 0x79, "F11": 0x7A, "F12": 0x7B,
    "NUMPAD0": 0x60, "NUMPAD1": 0x61, "NUMPAD2": 0x62, "NUMPAD3": 0x63,
    "NUMPAD4": 0x64, "NUMPAD5": 0x65, "NUMPAD6": 0x66, "NUMPAD7": 0x67,
    "NUMPAD8": 0x68, "NUMPAD9": 0x69,
    "MULTIPLY": 0x6A, "ADD": 0x6B, "SUBTRACT": 0x6D, "DECIMAL": 0x6E,
    "DIVIDE": 0x6F,
    "SEMICOLON": 0xBA, "EQUALS": 0xBB, "COMMA": 0xBC, "MINUS": 0xBD,
    "PERIOD": 0xBE, "SLASH": 0xBF, "GRAVE": 0xC0, "LBRACKET": 0xDB,
    "BACKSLASH": 0xDC, "RBRACKET": 0xDD, "QUOTE": 0xDE,
}

# The default mapping: (control, pad source, keyboard source or None).
# The keyboard column is the one the runtime has always used and goes on
# controller 1 only. Mirrors DEFAULTS in src/input/input_bindings.c.
DEFAULTS = [
    ("a", "pad:a", "key:Z"),
    ("b", "pad:b", "key:X"),
    ("x", "pad:x", "key:A"),
    ("y", "pad:y", "key:S"),
    ("black", "pad:lshoulder", "key:W"),
    ("white", "pad:rshoulder", "key:Q"),
    ("ltrigger", "pad:lt", "key:E"),
    ("rtrigger", "pad:rt", "key:R"),
    ("start", "pad:start", "key:RETURN"),
    ("back", "pad:back", "key:BACK"),
    ("dpad_up", "pad:dpad_up", "key:UP"),
    ("dpad_down", "pad:dpad_down", "key:DOWN"),
    ("dpad_left", "pad:dpad_left", "key:LEFT"),
    ("dpad_right", "pad:dpad_right", "key:RIGHT"),
    ("lstick_up", "pad:ly+", None),
    ("lstick_down", "pad:ly-", None),
    ("lstick_left", "pad:lx-", None),
    ("lstick_right", "pad:lx+", None),
    ("lthumb", "pad:lthumb", None),
    ("rstick_up", "pad:ry+", None),
    ("rstick_down", "pad:ry-", None),
    ("rstick_left", "pad:rx-", None),
    ("rstick_right", "pad:rx+", None),
    ("rthumb", "pad:rthumb", None),
]


# ---- sources -------------------------------------------------------------

def is_valid_source(source):
    """Is this a source string the runtime will understand?"""
    if not isinstance(source, str) or not source:
        return False
    if source in PAD_SOURCES or source in MOUSE_SOURCES:
        return True
    if source.startswith("key:"):
        name = source[4:]
        if name.startswith("#"):
            return name[1:].isdigit() and 0 < int(name[1:]) < 256
        if len(name) == 1:
            return name.isalnum() and name.upper() == name
        return name in KEY_NAMES
    return False


def key_source(vk, name=None):
    """A "key:" source for a Windows virtual-key code.

    Named keys are written by name so the file reads; anything else keeps its
    raw code, which the runtime accepts as "key:#NN".
    """
    if name and name in KEY_NAMES:
        return "key:" + name
    if 0x30 <= vk <= 0x39 or 0x41 <= vk <= 0x5A:
        return "key:" + chr(vk)
    for known, code in KEY_NAMES.items():
        if code == vk:
            return "key:" + known
    return "key:#%d" % vk


def source_label(source):
    """A source as the UI shows it: "Z", "Pad A", "Pad left stick up"."""
    if not source:
        return "--"
    if source.startswith("key:"):
        name = source[4:]
        return "Key #" + name[1:] if name.startswith("#") else name
    if source.startswith("pad:"):
        name = source[4:]
        pretty = {
            "lshoulder": "left shoulder", "rshoulder": "right shoulder",
            "lthumb": "left stick click", "rthumb": "right stick click",
            "lt": "left trigger", "rt": "right trigger",
            "lx+": "left stick right", "lx-": "left stick left",
            "ly+": "left stick up", "ly-": "left stick down",
            "rx+": "right stick right", "rx-": "right stick left",
            "ry+": "right stick up", "ry-": "right stick down",
        }.get(name, name.replace("dpad_", "D-pad ").replace("_", " "))
        if len(pretty) == 1:
            pretty = pretty.upper()                     # the face buttons
        return "Pad " + pretty
    if source.startswith("mouse:"):
        return {
            "left": "Left click", "right": "Right click",
            "middle": "Middle click", "x1": "Mouse back", "x2": "Mouse forward",
            "wheel_up": "Wheel up", "wheel_down": "Wheel down",
            "x+": "Mouse right", "x-": "Mouse left",
            "y+": "Mouse forward (up)", "y-": "Mouse back (down)",
        }.get(source[6:], source)
    return source


# ---- devices -------------------------------------------------------------

def device_label(device):
    if device == "keyboard":
        return "Keyboard"
    if device in (None, "", "none"):
        return "Nothing"
    if device.startswith(("gamepad:", "xinput:")):
        return "Gamepad %d" % (int(device.split(":", 1)[1]) + 1)
    return device


def pad_index(device):
    """The pad slot a device names, or None for the keyboard or nothing."""
    if isinstance(device, str) and device.startswith(("gamepad:", "xinput:")):
        return int(device.split(":", 1)[1])
    return None


def is_valid_device(device):
    if device in ("keyboard", "none"):
        return True
    if isinstance(device, str) and device.startswith(("gamepad:", "xinput:")):
        index = device.split(":", 1)[1]
        return index.isdigit() and 0 <= int(index) < PORTS
    return False


# ---- whole configs -------------------------------------------------------

def default_controller(port):
    """Defaults for one 1-based port.

    Port 1 gets the keyboard as well as its pad, so a title is playable with
    nothing plugged in; ports 2-4 are their pad alone.
    """
    bindings = {}
    for control, pad, key in DEFAULTS:
        sources = [pad]
        if port == 1 and key:
            sources.append(key)
        bindings[control] = sources
    return {
        "port": port,
        "device": "gamepad:%d" % (port - 1),
        "deadzone": DEADZONE_DEFAULT,
        "bindings": bindings,
    }


def default_config():
    return {
        "version": VERSION,
        "pad_api": "sdl",
        "controllers": [default_controller(p) for p in range(1, PORTS + 1)],
    }


def normalise(data):
    """Any JSON into a config with four controllers and every control bound.

    Anything missing comes from the defaults and anything unrecognised is
    dropped, so a hand-edited file with one binding in it is a config with one
    binding changed -- which is also how the runtime reads it.
    """
    config = default_config()
    if not isinstance(data, dict):
        return config
    if data.get("pad_api") in PAD_APIS:
        config["pad_api"] = data["pad_api"]
    # What each control does in the title ("Fire"), which a title's preset
    # carries for the launcher and the in-game menu to show. Kept, so a save
    # from here does not lose them; the runtime never reads them.
    labels = data.get("labels")
    if isinstance(labels, dict):
        kept = {control: label[:31] for control, label in labels.items()
                if control in CONTROL_NAMES and isinstance(label, str) and label}
        if kept:
            config["labels"] = kept
    listed = data.get("controllers")
    if not isinstance(listed, list):
        return config
    for index, entry in enumerate(listed):
        if not isinstance(entry, dict):
            continue
        port = entry.get("port", index + 1)
        if not isinstance(port, int) or not 1 <= port <= PORTS:
            continue
        target = config["controllers"][port - 1]
        target["port"] = port
        if is_valid_device(entry.get("device")):
            target["device"] = entry["device"]
        elif port != index + 1:
            target["device"] = "gamepad:%d" % (port - 1)
        deadzone = entry.get("deadzone")
        if isinstance(deadzone, int) and 0 <= deadzone < 32767:
            target["deadzone"] = deadzone
        # The mouse settings are written only when the file had them, so a
        # config with no mouse in it stays exactly as it was.
        for key, default, low, high in MOUSE_SETTINGS:
            value = entry.get(key)
            if isinstance(default, bool):
                if isinstance(value, bool):
                    target[key] = value
            elif (isinstance(value, (int, float)) and not isinstance(value, bool)
                  and low <= value <= high):
                target[key] = type(default)(value)
        # Controls whose keys hold a toggle on rather than flip it (aim in
        # Future Perfect); the runtime's "hold". Kept only when the file had it.
        hold = entry.get("hold")
        if isinstance(hold, list):
            target["hold"] = [c for c in hold if c in CONTROL_NAMES]
        bindings = entry.get("bindings")
        if isinstance(bindings, dict):
            for control, value in bindings.items():
                if control not in CONTROL_NAMES:
                    continue
                if isinstance(value, str):
                    value = [] if value in ("", "none") else [value]
                if not isinstance(value, list):
                    continue
                target["bindings"][control] = [s for s in value
                                               if is_valid_source(s)]
    return config


def config_path(title_id=None):
    """Where the UI and the runtime agree the config lives.

    RECOMP_INPUT_CONFIG wins, as it does in the runtime. Otherwise it is a
    per-user file: with a title id, that title's own
    (<user dir>/titles/<TITLE ID>.bindings.json, which the launcher writes
    and the runtime reads first); without one, the file every title shares.
    The runtime's order is recomp_bindings_read_path in src/config.
    """
    env = os.environ.get("RECOMP_INPUT_CONFIG")
    if env:
        return env
    if os.name == "nt":
        base = os.environ.get("APPDATA") or os.path.expanduser("~")
    else:
        base = (os.environ.get("XDG_CONFIG_HOME")
                or os.path.join(os.path.expanduser("~"), ".config"))
    if title_id:
        return os.path.join(base, "xboxrecomp", "titles",
                            "%08X.bindings.json" % title_id)
    return os.path.join(base, "xboxrecomp", "input_bindings.json")


def load(path=None):
    """The config at `path`, or the defaults if it is absent or unreadable."""
    path = path or config_path()
    try:
        with open(path, "r", encoding="utf-8") as handle:
            return normalise(json.load(handle))
    except (OSError, ValueError):
        return default_config()


def save(config, path=None):
    """Write the config, creating the directory. Returns the path written."""
    path = path or config_path()
    directory = os.path.dirname(path)
    if directory:
        os.makedirs(directory, exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(normalise(config), handle, indent=2)
        handle.write("\n")
    return path
