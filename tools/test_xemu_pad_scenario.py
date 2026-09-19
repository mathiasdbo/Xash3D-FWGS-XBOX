"""
Tests for tools/xemu-pad-scenario.py.

Run:  python -m unittest tools/test_xemu_pad_scenario.py

Why this exists: Codex review found Scenario.screenshot() always placed
xemu-debug.py's own --host/--port after the "screenshot" subcommand name
- those are top-level argparse args (added before add_subparsers()), so
every call failed, host/port defaults or not, until the argument order
was fixed. This asserts the exact generated argv, independent of a real
python executable or a live xemu.
"""
import importlib.util
import os
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_SPEC = importlib.util.spec_from_file_location("xemu_pad_scenario", os.path.join(_HERE, "xemu-pad-scenario.py"))
xps = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(xps)


class ScreenshotArgv(unittest.TestCase):
    def test_host_and_port_come_before_the_subcommand_name(self):
        argv = xps.screenshot_argv("python", "127.0.0.1", 1234, "out.png", 640, 480, 32)
        self.assertEqual(argv, [
            "python", os.path.join(_HERE, "xemu-debug.py"),
            "--host", "127.0.0.1", "--port", "1234",
            "screenshot", "out.png", "--width", "640", "--height", "480", "--bpp", "32",
        ])
        # The specific ordering bug Codex review found: --host/--port must
        # not appear anywhere after "screenshot" in the argument list.
        i = argv.index("screenshot")
        self.assertNotIn("--host", argv[i:])
        self.assertNotIn("--port", argv[i:])


if __name__ == "__main__":
    unittest.main()
