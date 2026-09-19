"""
Tests for tools/xemu-pad.py's own command protocol and validation logic.

Run:  python -m unittest tools/test_xemu_pad.py

Why these exist: Codex review of the first version found that a shared,
append-only command/result file pair let a command that outlived its own
client's timeout have its eventual result claimed by a LATER, unrelated
command - reporting success for an input that never actually happened, or
attributing the wrong command's own result. These tests exercise the
per-command-ID file protocol that replaced it (tools/xemu-pad.py's
_send/cmd_serve), including the exact failure shape the finding named:
a slow, stale response arriving after its own client gave up must never
be read by a different, later command.
"""
import argparse
import importlib.util
import os
import shutil
import tempfile
import threading
import time
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_SPEC = importlib.util.spec_from_file_location("xemu_pad", os.path.join(_HERE, "xemu-pad.py"))
xp = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(xp)


class ButtonAndAxisNames(unittest.TestCase):
    def test_short_aliases_resolve_to_the_real_enum_name(self):
        self.assertEqual(xp.resolve_button_name("down"), "XUSB_GAMEPAD_DPAD_DOWN")
        self.assertEqual(xp.resolve_button_name("a"), "XUSB_GAMEPAD_A")
        self.assertEqual(xp.resolve_button_name("Start"), "XUSB_GAMEPAD_START")

    def test_a_bare_xusb_name_with_or_without_the_prefix_also_resolves(self):
        self.assertEqual(xp.resolve_button_name("XUSB_GAMEPAD_Y"), "XUSB_GAMEPAD_Y")
        self.assertEqual(xp.resolve_button_name("Y"), "XUSB_GAMEPAD_Y")

    def test_an_unknown_button_is_rejected_not_silently_pressed_as_something_else(self):
        with self.assertRaises(ValueError):
            xp.resolve_button_name("banana")

    def test_axis_name_must_be_left_or_right(self):
        self.assertEqual(xp.parse_axis_name("Left"), "left")
        with self.assertRaises(ValueError):
            xp.parse_axis_name("middle")


class _FakeButton:
    def __init__(self, name):
        self.name = name


class _FakeXUSBButton:
    """Stands in for vgamepad's real XUSB_BUTTON enum - only _dispatch's own
    getattr(vg.XUSB_BUTTON, name) needs satisfying, not a real device."""
    XUSB_GAMEPAD_A = _FakeButton("XUSB_GAMEPAD_A")
    XUSB_GAMEPAD_DPAD_DOWN = _FakeButton("XUSB_GAMEPAD_DPAD_DOWN")


class _FakeVg:
    XUSB_BUTTON = _FakeXUSBButton


class _FakeGamepad:
    def __init__(self):
        self.calls = []

    def press_button(self, button):
        self.calls.append(("press", button.name))

    def release_button(self, button):
        self.calls.append(("release", button.name))

    def left_joystick_float(self, x_value_float, y_value_float):
        self.calls.append(("left_axis", x_value_float, y_value_float))

    def right_joystick_float(self, x_value_float, y_value_float):
        self.calls.append(("right_axis", x_value_float, y_value_float))

    def update(self):
        self.calls.append(("update",))


class Dispatch(unittest.TestCase):
    """_dispatch() is the server's own per-command logic, independent of
    the file protocol around it - a fake gamepad stands in for vgamepad's
    real, hardware-backed one."""

    def test_press_holds_for_the_requested_duration_then_releases(self):
        gp = _FakeGamepad()
        t0 = time.monotonic()
        result = xp._dispatch(gp, _FakeVg, "press XUSB_GAMEPAD_A 30")
        elapsed = time.monotonic() - t0
        self.assertEqual(result, "ok pressed XUSB_GAMEPAD_A for 30ms")
        self.assertGreaterEqual(elapsed, 0.03)
        self.assertEqual(gp.calls[0], ("press", "XUSB_GAMEPAD_A"))
        self.assertEqual(gp.calls[-1], ("update",))
        self.assertIn(("release", "XUSB_GAMEPAD_A"), gp.calls)

    def test_axis_sets_the_requested_stick(self):
        gp = _FakeGamepad()
        result = xp._dispatch(gp, _FakeVg, "axis left 0.5 -0.25")
        self.assertEqual(result, "ok axis left 0.5 -0.25")
        self.assertIn(("left_axis", 0.5, -0.25), gp.calls)

    def test_quit_is_acknowledged_without_touching_the_gamepad(self):
        gp = _FakeGamepad()
        self.assertEqual(xp._dispatch(gp, _FakeVg, "quit"), "ok quit")
        self.assertEqual(gp.calls, [])

    def test_an_unknown_command_is_an_error_result_not_an_exception(self):
        gp = _FakeGamepad()
        result = xp._dispatch(gp, _FakeVg, "somethingelse")
        self.assertTrue(result.startswith("error"))

    def test_a_malformed_press_is_an_error_result_not_a_crash(self):
        gp = _FakeGamepad()
        result = xp._dispatch(gp, _FakeVg, "press")
        self.assertTrue(result.startswith("error"))

    def test_empty_command_is_an_error_result(self):
        gp = _FakeGamepad()
        self.assertTrue(xp._dispatch(gp, _FakeVg, "").startswith("error"))

    def test_a_negative_hold_is_rejected_before_the_button_ever_goes_down(self):
        # Codex review round 2: time.sleep() raises ValueError on a
        # negative duration - previously reached only AFTER press_button()
        # had already fired, with nothing to release it. Rejecting first
        # means the button is never touched at all for this input.
        gp = _FakeGamepad()
        result = xp._dispatch(gp, _FakeVg, "press XUSB_GAMEPAD_A -5")
        self.assertTrue(result.startswith("error"))
        self.assertEqual(gp.calls, [], "an invalid duration must never press the button at all")

    def test_an_exception_during_the_hold_still_releases_the_button(self):
        # Codex review round 2's own point: ANY exception between press
        # and release - not just the negative-duration case above - must
        # not leave the button stuck down. Patches xemu-pad's own `time`
        # module (imported at file scope there) so the sleep inside
        # _dispatch's press branch raises, standing in for whatever real
        # failure (a device error, a killed thread) could interrupt it.
        gp = _FakeGamepad()
        real_sleep = xp.time.sleep

        def flaky_sleep(seconds):
            raise RuntimeError("simulated failure during the hold")

        xp.time.sleep = flaky_sleep
        try:
            result = xp._dispatch(gp, _FakeVg, "press XUSB_GAMEPAD_A 50")
        finally:
            xp.time.sleep = real_sleep
        self.assertTrue(result.startswith("error"))
        self.assertIn(("release", "XUSB_GAMEPAD_A"), gp.calls,
                       "the button must be released even though the hold itself failed")


def _run_one_serve_iteration(d, gamepad, vg):
    """One pass of cmd_serve's own inner loop body, without its outer
    'forever' - lets a test drive the exact same dispatch/delete/write-
    result code serve() itself runs, deterministically, one command at a
    time, instead of racing a real background thread against sleeps."""
    cmd_names = sorted(n for n in os.listdir(d) if n.startswith("cmd_") and n.endswith(".txt"))
    for name in cmd_names:
        cmd_id = name[len("cmd_"):-len(".txt")]
        path = os.path.join(d, name)
        with open(path) as f:
            line = f.read().strip()
        os.remove(path)
        result = xp._dispatch(gamepad, vg, line)
        with open(xp._result_path(d, cmd_id), "w") as f:
            f.write(result + "\n")


class SendProtocol(unittest.TestCase):
    """The client side (_send) against a hand-driven fake serve loop - no
    real vgamepad/ViGEmBus needed, since the whole point is the file
    protocol's own correctness, not the device."""

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.gp = _FakeGamepad()

    def _args(self, timeout=2.0):
        return argparse.Namespace(dir=self.dir, timeout=timeout)

    def test_a_command_answered_before_the_client_gives_up_succeeds(self):
        def server():
            deadline = time.monotonic() + 2.0
            while time.monotonic() < deadline:
                if any(n.startswith("cmd_") for n in os.listdir(self.dir)):
                    _run_one_serve_iteration(self.dir, self.gp, _FakeVg)
                    return
                time.sleep(0.01)
        t = threading.Thread(target=server)
        t.start()
        rc = xp._send(self._args(), "press XUSB_GAMEPAD_A 1")
        t.join()
        self.assertEqual(rc, 0)

    def test_a_late_response_after_the_clients_own_timeout_is_not_left_for_a_later_command_to_claim(self):
        # The exact race Codex review found: the server already picked up
        # and deleted A's own command file (a real serve loop does this
        # before dispatching, tools/xemu-pad.py's own comment on why) and
        # is midway through a slow dispatch - here, literally slow, a long
        # --hold-ms - when A's OWN client gives up waiting and times out.
        # The eventual result must land only under A's own unique ID,
        # never picked up by a second, unrelated command B that happens to
        # run afterward and would previously have seen "the result file
        # grew" and wrongly claimed it as its own.
        def slow_server():
            deadline = time.monotonic() + 3.0
            while time.monotonic() < deadline:
                if any(n.startswith("cmd_") for n in os.listdir(self.dir)):
                    _run_one_serve_iteration(self.dir, self.gp, _FakeVg)  # blocks for the press's own hold_ms
                    return
                time.sleep(0.01)

        t = threading.Thread(target=slow_server)
        t.start()
        # hold_ms (300ms) outlasts the client's own timeout (100ms) - the
        # server will have already consumed and deleted the command file
        # by the time this client gives up, exactly the real race. join()
        # BEFORE asserting: an assertion failure must never leave this
        # thread running past the test, into some later test's own
        # (by-then-deleted) tempdir.
        rc_a = xp._send(self._args(timeout=0.1), "press XUSB_GAMEPAD_A 300")
        t.join()
        self.assertEqual(rc_a, 2, "A was already claimed when it timed out - outcome unknown, not a plain failure")

        # A's own result file exists now (the slow server finally answered
        # it) but under A's OWN id - a second, later command B must get
        # its own fresh id and never see A's leftover result.
        self.assertTrue(any(n.startswith("result_") for n in os.listdir(self.dir)),
                         "the slow answer really did arrive, late, under its own id")

        def fast_server():
            deadline = time.monotonic() + 2.0
            while time.monotonic() < deadline:
                if any(n.startswith("cmd_") for n in os.listdir(self.dir)):
                    _run_one_serve_iteration(self.dir, self.gp, _FakeVg)
                    return
                time.sleep(0.01)
        t2 = threading.Thread(target=fast_server)
        t2.start()
        rc_b = xp._send(self._args(timeout=2.0), "press XUSB_GAMEPAD_DPAD_DOWN 1")
        t2.join()
        self.assertEqual(rc_b, 0, "B gets its own real answer, not A's stale leftover")

    def test_no_serve_running_at_all_times_out_with_exit_code_1_never_claimed(self):
        rc = xp._send(self._args(timeout=0.2), "press XUSB_GAMEPAD_A 1")
        self.assertEqual(rc, 1, "never claimed at all - safe to treat as 'nothing happened'")

    def test_a_command_claimed_but_never_answered_times_out_with_exit_code_2_unknown(self):
        # Codex review round 2: a plain, uniform failure here cannot be
        # told apart from "definitely never happened" - a caller could
        # then wrongly retry an input that is still in flight, or already
        # landed. Simulates serve picking the command up (removing its
        # file, exactly what a real dispatch does before it starts) and
        # then hanging forever - never writing a result at all.
        def claims_but_never_answers():
            deadline = time.monotonic() + 2.0
            while time.monotonic() < deadline:
                names = [n for n in os.listdir(self.dir) if n.startswith("cmd_")]
                if names:
                    os.remove(os.path.join(self.dir, names[0]))
                    return  # claimed - then simply never responds
                time.sleep(0.01)
        t = threading.Thread(target=claims_but_never_answers)
        t.start()
        rc = xp._send(self._args(timeout=0.2), "press XUSB_GAMEPAD_A 1")
        t.join()
        self.assertEqual(rc, 2, "claimed but unanswered - outcome unknown, must not be treated as a plain failure")

    def test_two_commands_in_flight_at_once_never_share_a_command_file(self):
        # Two clients writing "at the same time" (here, sequentially but
        # before either is served) must produce two distinct cmd_*.txt
        # files, never one overwriting the other - the second finding
        # Codex review named for the old shared-file design.
        results = {}

        def client(key, line):
            results[key] = xp._send(self._args(timeout=2.0), line)

        def server():
            deadline = time.monotonic() + 2.0
            seen = 0
            while time.monotonic() < deadline and seen < 2:
                names = [n for n in os.listdir(self.dir) if n.startswith("cmd_")]
                if names:
                    _run_one_serve_iteration(self.dir, self.gp, _FakeVg)
                    seen += 1
                time.sleep(0.01)

        t_server = threading.Thread(target=server)
        t_server.start()
        t1 = threading.Thread(target=client, args=("a", "press XUSB_GAMEPAD_A 1"))
        t2 = threading.Thread(target=client, args=("b", "press XUSB_GAMEPAD_DPAD_DOWN 1"))
        t1.start(); t2.start()
        t1.join(); t2.join(); t_server.join()
        self.assertEqual(results, {"a": 0, "b": 0})


if __name__ == "__main__":
    unittest.main()
