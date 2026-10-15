"""
The binding config: defaults, validation and what a partial file means.

    py -3 -m unittest tools.input_ui.test_bindings

No device and no display: this is the half of the feature that can be tested
anywhere, and it is the half that decides what a title reads.
"""

import json
import os
import tempfile
import unittest

from tools.input_ui import bindings


class Defaults(unittest.TestCase):
    def test_four_controllers_each_on_its_own_pad(self):
        config = bindings.default_config()
        self.assertEqual(len(config["controllers"]), 4)
        for index, controller in enumerate(config["controllers"]):
            self.assertEqual(controller["port"], index + 1)
            self.assertEqual(controller["device"], "gamepad:%d" % index)

    def test_every_control_is_bound_by_default(self):
        for controller in bindings.default_config()["controllers"]:
            for name in bindings.CONTROL_NAMES:
                self.assertTrue(controller["bindings"][name],
                                "%s is unbound by default" % name)

    def test_only_controller_one_gets_the_keyboard(self):
        config = bindings.default_config()
        first = config["controllers"][0]["bindings"]
        self.assertIn("key:Z", first["a"])
        self.assertIn("key:RETURN", first["start"])
        for controller in config["controllers"][1:]:
            for sources in controller["bindings"].values():
                self.assertFalse([s for s in sources if s.startswith("key:")])

    def test_the_keyboard_defaults_are_the_ones_the_runtime_always_had(self):
        # arrows = D-pad, Enter = START, Backspace = BACK, Z = A, X = B,
        # A = X, S = Y, Q = White, W = Black, E/R = triggers.
        first = bindings.default_config()["controllers"][0]["bindings"]
        expected = {
            "a": "key:Z", "b": "key:X", "x": "key:A", "y": "key:S",
            "white": "key:Q", "black": "key:W", "ltrigger": "key:E",
            "rtrigger": "key:R", "start": "key:RETURN", "back": "key:BACK",
            "dpad_up": "key:UP", "dpad_down": "key:DOWN",
            "dpad_left": "key:LEFT", "dpad_right": "key:RIGHT",
        }
        for control, source in expected.items():
            self.assertIn(source, first[control])

    def test_every_default_source_is_one_the_runtime_accepts(self):
        for controller in bindings.default_config()["controllers"]:
            for sources in controller["bindings"].values():
                for source in sources:
                    self.assertTrue(bindings.is_valid_source(source), source)


class Sources(unittest.TestCase):
    def test_a_named_key_a_letter_and_a_raw_code_are_all_valid(self):
        for source in ("key:RETURN", "key:Z", "key:7", "key:#191"):
            self.assertTrue(bindings.is_valid_source(source), source)

    def test_nonsense_is_not(self):
        for source in ("", "key:", "key:NOPE", "pad:z", "pad:lz+", "Z", None,
                       "key:z", "key:#0", "key:#999"):
            self.assertFalse(bindings.is_valid_source(source), repr(source))

    def test_a_key_code_is_written_by_name_when_it_has_one(self):
        self.assertEqual(bindings.key_source(0x0D), "key:RETURN")
        self.assertEqual(bindings.key_source(ord("Z")), "key:Z")
        self.assertEqual(bindings.key_source(0xBF), "key:SLASH")

    def test_a_key_code_with_no_name_keeps_its_number(self):
        self.assertEqual(bindings.key_source(0xF0), "key:#240")

    def test_every_pad_source_the_ui_offers_is_valid(self):
        for source in bindings.PAD_SOURCES:
            self.assertTrue(bindings.is_valid_source(source), source)


class Mouse(unittest.TestCase):
    def test_every_mouse_source_is_valid_and_has_a_label(self):
        for source in bindings.MOUSE_SOURCES:
            self.assertTrue(bindings.is_valid_source(source), source)
            self.assertNotEqual(bindings.source_label(source), source)

    def test_a_mouse_name_the_runtime_does_not_know_is_not(self):
        for source in ("mouse:", "mouse:button4", "mouse:x", "mouse:wheel"):
            self.assertFalse(bindings.is_valid_source(source), source)

    def test_mouse_bindings_and_settings_survive_normalising(self):
        config = bindings.normalise({"controllers": [{
            "port": 1,
            "mouse_sensitivity": 2.5, "mouse_deadzone": 8000,
            "mouse_invert_y": True, "mouse_window_ms": 24,
            "bindings": {"rstick_right": "mouse:x+", "rtrigger": ["mouse:left"]},
        }]})
        port = config["controllers"][0]
        self.assertEqual(port["bindings"]["rstick_right"], ["mouse:x+"])
        self.assertEqual(port["bindings"]["rtrigger"], ["mouse:left"])
        self.assertEqual(port["mouse_sensitivity"], 2.5)
        self.assertEqual(port["mouse_deadzone"], 8000)
        self.assertIs(port["mouse_invert_y"], True)
        self.assertEqual(port["mouse_window_ms"], 24)

    def test_out_of_range_mouse_settings_are_dropped(self):
        config = bindings.normalise({"controllers": [{
            "port": 1, "mouse_sensitivity": 0, "mouse_deadzone": 40000,
            "mouse_invert_y": "yes", "mouse_window_ms": 1000,
        }]})
        port = config["controllers"][0]
        for key, _default, _low, _high in bindings.MOUSE_SETTINGS:
            self.assertNotIn(key, port)

    def test_action_labels_survive_and_unknown_ones_are_dropped(self):
        config = bindings.normalise({"labels": {"rtrigger": "Fire", "nonsense": "x",
                                                "x": 7}})
        self.assertEqual(config["labels"], {"rtrigger": "Fire"})
        self.assertNotIn("labels", bindings.default_config())

    def test_the_hold_list_survives_and_unknown_controls_are_dropped(self):
        data = {"controllers": [{"port": 1, "hold": ["rthumb", "warp", "lthumb"]}]}
        port = bindings.normalise(data)["controllers"][0]
        self.assertEqual(port["hold"], ["rthumb", "lthumb"])
        self.assertNotIn("hold", bindings.normalise({})["controllers"][0])

    def test_a_config_without_the_mouse_gains_no_mouse_keys(self):
        port = bindings.default_config()["controllers"][0]
        self.assertFalse([k for k in port if k.startswith("mouse_")])


class Normalising(unittest.TestCase):
    def test_a_file_with_one_binding_changes_one_binding(self):
        config = bindings.normalise({
            "version": 1,
            "controllers": [{"port": 1, "bindings": {"a": "key:SPACE"}}],
        })
        first = config["controllers"][0]["bindings"]
        self.assertEqual(first["a"], ["key:SPACE"])
        self.assertIn("key:X", first["b"])          # everything else default
        self.assertEqual(len(config["controllers"]), 4)

    def test_unknown_controls_and_sources_are_dropped(self):
        config = bindings.normalise({
            "controllers": [{"port": 2, "bindings": {
                "a": ["pad:a", "pad:nope"], "jump": "key:J"}}],
        })
        second = config["controllers"][1]["bindings"]
        self.assertEqual(second["a"], ["pad:a"])
        self.assertNotIn("jump", second)

    def test_a_control_can_be_unbound(self):
        config = bindings.normalise({
            "controllers": [{"port": 1, "bindings": {"black": "none",
                                                     "white": []}}],
        })
        first = config["controllers"][0]["bindings"]
        self.assertEqual(first["black"], [])
        self.assertEqual(first["white"], [])

    def test_a_bad_device_falls_back_rather_than_failing(self):
        config = bindings.normalise({
            "controllers": [{"port": 1, "device": "joystick"}],
        })
        self.assertEqual(config["controllers"][0]["device"], "gamepad:0")

    def test_devices_that_are_allowed(self):
        for device in ("keyboard", "none", "gamepad:0", "gamepad:3",
                       "xinput:0", "xinput:3"):
            config = bindings.normalise({
                "controllers": [{"port": 3, "device": device}]})
            self.assertEqual(config["controllers"][2]["device"], device)

    def test_pad_api_survives_and_defaults_to_sdl(self):
        self.assertEqual(bindings.default_config()["pad_api"], "sdl")
        self.assertEqual(bindings.normalise({"pad_api": "xinput"})["pad_api"], "xinput")
        self.assertEqual(bindings.normalise({"pad_api": "joystick"})["pad_api"], "sdl")

    def test_rubbish_gives_the_defaults(self):
        for data in (None, [], {}, {"controllers": "no"},
                     {"controllers": [None, 7]}):
            self.assertEqual(bindings.normalise(data), bindings.default_config())

    def test_a_port_outside_one_to_four_is_ignored(self):
        config = bindings.normalise({
            "controllers": [{"port": 9, "bindings": {"a": "key:SPACE"}}]})
        self.assertEqual(config, bindings.default_config())


class Files(unittest.TestCase):
    def test_a_saved_config_reloads_the_same(self):
        config = bindings.default_config()
        config["controllers"][0]["device"] = "keyboard"
        config["controllers"][0]["bindings"]["a"] = ["key:SPACE"]
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "nested", "input_bindings.json")
            bindings.save(config, path)
            self.assertEqual(bindings.load(path), config)

    def test_a_missing_file_loads_the_defaults(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "absent.json")
            self.assertEqual(bindings.load(path), bindings.default_config())

    def test_a_broken_file_loads_the_defaults(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "broken.json")
            with open(path, "w", encoding="utf-8") as handle:
                handle.write("{not json")
            self.assertEqual(bindings.load(path), bindings.default_config())

    def test_what_is_written_is_json_the_runtime_shape(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "written.json")
            bindings.save(bindings.default_config(), path)
            with open(path, encoding="utf-8") as handle:
                data = json.load(handle)
            self.assertEqual(data["version"], bindings.VERSION)
            self.assertEqual([c["port"] for c in data["controllers"]],
                             [1, 2, 3, 4])

    def test_the_path_follows_the_environment_variable(self):
        old = os.environ.get("RECOMP_INPUT_CONFIG")
        os.environ["RECOMP_INPUT_CONFIG"] = os.path.join("x", "y.json")
        try:
            self.assertEqual(bindings.config_path(), os.path.join("x", "y.json"))
        finally:
            if old is None:
                del os.environ["RECOMP_INPUT_CONFIG"]
            else:
                os.environ["RECOMP_INPUT_CONFIG"] = old


if __name__ == "__main__":
    unittest.main()
