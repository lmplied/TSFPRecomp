"""
The UI and the runtime must name the same things.

    py -3 -m unittest tools.input_ui.test_runtime_vocabulary

src/input/input_bindings.c has its own copies of the control names, the pad
source names, the key names and the default mapping, because it cannot import
Python. Nothing but this test stops the two drifting, and drift is silent in
the worst way: the UI writes a binding, the file saves, the runtime does not
recognise the name and the control simply never fires.

The C tables are read out of the source rather than a generated header, so the
runtime file stays readable on its own.
"""

import os
import re
import unittest

from tools.input_ui import bindings

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
SOURCE = os.path.join(ROOT, "src", "input", "input_bindings.c")
CONFIG_SOURCE = os.path.join(ROOT, "src", "config", "recomp_config.c")


def read_source():
    with open(SOURCE, encoding="utf-8") as handle:
        return handle.read()


def table(text, name):
    """The body of a C initialiser list `... NAME[...] = { ... };`."""
    match = re.search(re.escape(name) + r"\[[^\]]*\]\s*=\s*\{(.*?)\n\};",
                      text, re.S)
    assert match, "%s is not in %s any more" % (name, SOURCE)
    return match.group(1)


class Vocabulary(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = read_source()

    def test_the_control_names_are_the_same_and_in_the_same_order(self):
        names = re.findall(r'\{\s*"([a-z_0-9]+)"', table(self.text, "CONTROLS"))
        self.assertEqual(names, bindings.CONTROL_NAMES)

    def test_the_pad_button_names_are_the_same(self):
        names = re.findall(r'"([a-z_0-9]+)"', table(self.text, "PAD_BUTTONS"))
        self.assertEqual(names, bindings.PAD_BUTTONS)

    def test_the_defaults_are_the_same(self):
        rows = re.findall(r'\{\s*"([a-z_0-9]+)"\s*,\s*"([^"]+)"\s*,\s*'
                          r'(?:"([^"]+)"|NULL)\s*\}',
                          table(self.text, "DEFAULTS"))
        self.assertEqual([(control, pad, key or None)
                          for control, pad, key in rows], bindings.DEFAULTS)

    def test_the_key_names_are_the_same(self):
        rows = re.findall(r'\{\s*"([A-Z0-9_]+)"\s*,\s*(0x[0-9A-Fa-f]+)\s*\}',
                          table(self.text, "KEYS"))
        self.assertEqual({name: int(code, 16) for name, code in rows},
                         bindings.KEY_NAMES)

    def test_the_mouse_source_names_are_the_same_and_in_the_same_order(self):
        names = re.findall(r'"([a-z_0-9+\-]+)"', table(self.text, "MOUSE_SOURCES"))
        self.assertEqual(names, bindings.MOUSE_SOURCE_NAMES)

    def test_the_runtime_reads_every_mouse_setting_the_ui_writes(self):
        for key, _default, _low, _high in bindings.MOUSE_SETTINGS:
            self.assertIn('strcmp(key, "%s")' % key, self.text)

    def test_the_mouse_window_default_is_the_same(self):
        match = re.search(r"#define MOUSE_WINDOW_DEFAULT (\d+)", self.text)
        self.assertTrue(match)
        defaults = {key: default for key, default, _l, _h in bindings.MOUSE_SETTINGS}
        self.assertEqual(int(match.group(1)), defaults["mouse_window_ms"])

    def test_the_deadzone_default_is_the_same(self):
        match = re.search(r"#define DEADZONE_DEFAULT (\d+)", self.text)
        self.assertTrue(match)
        self.assertEqual(int(match.group(1)), bindings.DEADZONE_DEFAULT)

    def test_the_runtime_reads_the_same_environment_variable(self):
        self.assertIn('getenv("RECOMP_INPUT_CONFIG")', self.text)

    def test_the_runtime_looks_in_the_same_per_user_places(self):
        # bindings.config_path() builds %APPDATA%\xboxrecomp\input_bindings.json
        # and, given a title, ...\titles\<ID>.bindings.json; the runtime gets
        # both from recomp_bindings_read_path in src/config, built from the
        # same pieces.
        with open(CONFIG_SOURCE, encoding="utf-8") as handle:
            config = handle.read()
        self.assertIn("recomp_bindings_read_path", self.text)
        self.assertIn('getenv("APPDATA")', config)
        self.assertIn('"\\\\xboxrecomp"', config)
        self.assertIn('"input_bindings.json"', config)
        self.assertIn('"titles" SEP "%08X.bindings.json"', config)
        if not os.environ.get("RECOMP_INPUT_CONFIG"):
            self.assertTrue(bindings.config_path().endswith(
                os.path.join("xboxrecomp", "input_bindings.json")))
            self.assertTrue(bindings.config_path(0x45410066).endswith(
                os.path.join("xboxrecomp", "titles", "45410066.bindings.json")))


if __name__ == "__main__":
    unittest.main()
