# SPDX-License-Identifier: MIT
import importlib.machinery
import json
from pathlib import Path
import unittest

launcher = importlib.machinery.SourceFileLoader("launcher", str(Path(__file__).parents[1] / "tools/aurora-capture-ns")).load_module()


class ConfigTest(unittest.TestCase):
    def node(self, **props):
        return [{"type": "PipeWire:Interface:Node", "info": {"props": {
            "node.name": "mic", "media.class": "Audio/Source", "object.serial": 123, **props}}}]

    def test_explicit_source(self):
        self.assertEqual(launcher.source_serial(self.node(), "mic"), "123")
        for props in ({"media.class": "Audio/Sink"}, {"media.class": "Audio/Source/Virtual"},
                      {"object.serial": None}, {"object.serial": True}):
            with self.assertRaises(ValueError):
                launcher.source_serial(self.node(**props), "mic")
        with self.assertRaises(ValueError):
            launcher.source_serial(self.node() * 2, "mic")

    def test_only_capture_graph(self):
        for model in ("rnnoise", "df3"):
            c = json.loads(launcher.config("123", model, '/tmp/a"b.so'))
            args = c["context.modules"][-1]["args"]
            capture, source = args["capture.props"], args["playback.props"]
            self.assertEqual(capture["target.object"], "123")
            self.assertTrue(capture["node.dont-fallback"])
            self.assertTrue(capture["node.dont-reconnect"])
            self.assertFalse(capture["stream.capture.sink"])
            self.assertEqual(source["media.class"], "Audio/Source")
            self.assertFalse(source["node.autoconnect"])
            self.assertEqual(args["filter.graph"]["outputs"], ["ns:Output"])
            self.assertEqual(len(args["filter.graph"]["nodes"]), 1)

    def test_no_config_injection(self):
        with self.assertRaises(ValueError):
            launcher.config('1" node.name = sink', "df3", "a")
        with self.assertRaises(ValueError):
            launcher.config("1", "sink", "a")
