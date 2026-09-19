#!/usr/bin/env python3
"""
xemu-pad-scenario.py - compose tools/xemu-pad.py (input) and
tools/xemu-debug.py (state read-back) into a "press, wait, verify" test,
against a live xemu instance.

Deliberately thin: a short ordered list of steps and one assertion per
step that wants one, not a framework. Building this out further (a DSL,
retries, parallel scenarios) is not this file's job until a second real
scenario actually needs it - the one concrete scenario this repo has
today (closing docs/r3d/divergences.md #76's still-open claim) is its
own, separate script built on top of these functions, not inside them.

Imports tools/xemu-debug.py and tools/xemu-pad.py the same way
tools/test_xemu_debug.py already does (importlib, since these are
hyphenated filenames, not importable packages).

## CLI

  python tools/xemu-pad-scenario.py demo --map <file> --exe <pe>
    A canned smoke test proving the whole chain works end to end: read
    the cursor, press Down, read it again, assert it moved, press Down
    twice more, read once more. Requires 'xemu-pad.py serve' already
    running (see that file's own docstring) and xemu launched with -s.
"""
import argparse
import importlib.util
import os
import subprocess
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))


def screenshot_argv(python, host, port, out_path, width, height, bpp):
    """The argv for shelling out to xemu-debug.py's own screenshot command -
    a pure function so the argument ORDER (Codex review, round 1: --host/
    --port are top-level args, tools/xemu-debug.py:1484-1486, added before
    add_subparsers() - argparse rejects them if they come after the
    subcommand name, which this call always did until this fix) has a test
    that does not need a live xemu or even a real python executable."""
    return [python, os.path.join(_HERE, "xemu-debug.py"),
            "--host", host, "--port", str(port),
            "screenshot", out_path, "--width", str(width), "--height", str(height), "--bpp", str(bpp)]


def _load(name, filename):
    spec = importlib.util.spec_from_file_location(name, os.path.join(_HERE, filename))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod

xd = _load("xemu_debug", "xemu-debug.py")
xp = _load("xemu_pad", "xemu-pad.py")


class ScenarioError(RuntimeError):
    pass


class ScenarioStateUnknownError(ScenarioError):
    """serve accepted a press/axis command but never answered within the
    timeout (tools/xemu-pad.py's own exit code 2) - the input may or may
    not have reached the guest. Deliberately a different exception than a
    plain ScenarioError: a caller must not treat this the way it would
    treat an ordinary failed step (fix the assertion, keep going) - the
    scenario's own state is now unknown, and Codex review round 2's own
    point stands regardless of what catches this: the right response is
    to stop, not to retry the same command or press on with further
    assertions as if nothing happened."""


class Scenario:
    """One xemu-pad 'serve' process (already running, elsewhere) plus one
    map/exe pair to read state against. Each method is a step; each
    raises ScenarioError on failure rather than returning False, so a
    caller's script reads top-to-bottom as the scenario itself, with
    failures stopping it exactly where they happened."""

    def __init__(self, map_path, exe_path, host="127.0.0.1", port=1234,
                 pad_dir=None, pad_timeout=5.0):
        self.map_path = map_path
        self.exe_path = exe_path
        self.host = host
        self.port = port
        self._pad_args = argparse.Namespace(
            dir=pad_dir or xp.DEFAULT_DIR,
            timeout=pad_timeout,
        )

    def press(self, button, hold_ms=200, settle_ms=100):
        args = argparse.Namespace(button=button, hold_ms=hold_ms, **vars(self._pad_args))
        rc = xp.cmd_press(args)
        self._check_pad_rc(rc, f"press({button!r})")
        if settle_ms > 0:
            time.sleep(settle_ms / 1000.0)

    def axis(self, stick, x, y, settle_ms=100):
        args = argparse.Namespace(stick=stick, x=x, y=y, **vars(self._pad_args))
        rc = xp.cmd_axis(args)
        self._check_pad_rc(rc, f"axis({stick!r}, {x}, {y})")
        if settle_ms > 0:
            time.sleep(settle_ms / 1000.0)

    @staticmethod
    def _check_pad_rc(rc, what):
        # tools/xemu-pad.py's own three exit codes for press/axis: 0 ok,
        # 1 never claimed (safe - nothing happened), 2 claimed but never
        # answered (outcome unknown - never treat this as an ordinary
        # failure, see ScenarioStateUnknownError's own docstring).
        if rc == 2:
            raise ScenarioStateUnknownError(
                f"{what} was accepted by xemu-pad serve but never confirmed - "
                "the input may or may not have reached the guest; stop, do not retry")
        if rc != 0:
            raise ScenarioError(f"{what} failed - is 'xemu-pad.py serve' running?")

    def wait(self, ms):
        time.sleep(ms / 1000.0)

    def cursor(self):
        """The currently-focused item's index (CMenuItemsHolder::m_iCursor).
        Raises ScenarioError - wrapping whatever xemu-debug.py's own cmd_cursor
        raised - if the guest isn't verifiably running the given exe, or the
        window-stack node it resolved fails its own validity check."""
        syms = xd.load_map_symbols(self.map_path)
        uistatic_sym = next((k for k in syms if k.endswith("uiStatic@@3UuiStatic_t@@A")), None)
        if uistatic_sym is None:
            raise ScenarioError(f"{self.map_path} has no uiStatic symbol")
        uistatic_addr = syms[uistatic_sym]
        with open(self.exe_path, "rb") as f:
            data = f.read()
        off = _cursor_offsets()
        try:
            with xd.GdbRemote(self.host, self.port) as g:
                a = argparse.Namespace(map=self.map_path, exe=self.exe_path)
                xd.check_identity(g, a, data, "scenario")
                cursor, _window_ptr = xd.resolve_cursor(g.read_mem, uistatic_addr, off)
        except RuntimeError as e:
            raise ScenarioError(str(e)) from e
        return cursor

    def screenshot(self, out_path, width=640, height=480, bpp=32):
        """Shells out to xemu-debug.py's own screenshot command rather than
        duplicating cmd_fbdump's framebuffer-read/PNG-encode logic here -
        that command is a monolithic CLI handler, not a separable function,
        and screenshots are for a human (or a later, separate step) to look
        at, not part of this class's own pass/fail logic."""
        argv = screenshot_argv(sys.executable, self.host, self.port, out_path, width, height, bpp)
        r = subprocess.run(argv, capture_output=True, text=True)
        if r.returncode != 0:
            raise ScenarioError(f"screenshot failed: {(r.stderr or r.stdout).strip()[:300]}")

    def assert_cursor(self, expected):
        """expected is either an int (exact match) or a callable(int) -> bool."""
        got = self.cursor()
        ok = expected(got) if callable(expected) else got == expected
        if not ok:
            raise ScenarioError(f"cursor assertion failed: got {got}, expected {expected}")
        return got


_OFFSETS_CACHE = None


def _cursor_offsets():
    global _OFFSETS_CACHE
    if _OFFSETS_CACHE is None:
        _OFFSETS_CACHE = {
            "uistatic_menu": xd.struct_offset("BaseMenu.h", "uiStatic_t", "menu", 44,
                                               cxx=True, extra_args=xd.MAINUI_INCLUDES),
            "windowstack_active": xd.struct_offset("WindowSystem.h", "CWindowStack", "active", 4,
                                                    cxx=True, extra_args=xd.MAINUI_INCLUDES),
            "windowstack_stack": xd.struct_offset("WindowSystem.h", "CWindowStack", "stack", 40,
                                                   cxx=True, extra_args=xd.MAINUI_INCLUDES),
            "linkedlist_memory": xd.struct_offset(
                "WindowSystem.h", "CUtlLinkedList<CMenuBaseWindow*>", "m_Memory", 16,
                cxx=True, extra_args=xd.MAINUI_INCLUDES),
            "linkedlist_total": xd.struct_offset(
                "WindowSystem.h", "CUtlLinkedList<CMenuBaseWindow*>", "m_TotalElements", 4,
                cxx=True, extra_args=xd.MAINUI_INCLUDES),
            "memorybase_pmemory": xd.struct_offset(
                "WindowSystem.h", f"CUtlMemory<{xd.LIST_ELEM_T}>", "m_pMemory", 4,
                cxx=True, extra_args=xd.MAINUI_INCLUDES),
            "listelem_element": xd.struct_offset("WindowSystem.h", xd.LIST_ELEM_T, "m_Element", 4,
                                                  cxx=True, extra_args=xd.MAINUI_INCLUDES),
            "listelem_previous": xd.struct_offset("WindowSystem.h", xd.LIST_ELEM_T, "m_Previous", 4,
                                                   cxx=True, extra_args=xd.MAINUI_INCLUDES),
            "listelem_next": xd.struct_offset("WindowSystem.h", xd.LIST_ELEM_T, "m_Next", 4,
                                               cxx=True, extra_args=xd.MAINUI_INCLUDES),
            "itemsholder_cursor": xd.struct_offset("ItemsHolder.h", "CMenuItemsHolder", "m_iCursor", 4,
                                                    cxx=True, extra_args=xd.MAINUI_INCLUDES),
            "listelem_size": xd.type_sizeof("WindowSystem.h", xd.LIST_ELEM_T, expect_size=12,
                                            extra_args=xd.MAINUI_INCLUDES),
        }
    return _OFFSETS_CACHE


def cmd_demo(a):
    sc = Scenario(a.map, a.exe)
    print("demo: reading initial cursor...")
    start = sc.cursor()
    print(f"demo: cursor={start}")
    print("demo: pressing Down...")
    sc.press("down")
    after_one = sc.assert_cursor(lambda c: c != start)
    print(f"demo: cursor moved {start} -> {after_one}, as expected")
    print("demo: pressing Down twice more...")
    sc.press("down")
    sc.press("down")
    final = sc.cursor()
    print(f"demo: final cursor={final}")
    print("demo: PASS")
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("demo")
    s.add_argument("--map", required=True)
    s.add_argument("--exe", required=True)
    s.set_defaults(func=cmd_demo)
    a = p.parse_args()
    try:
        return a.func(a)
    except ScenarioStateUnknownError as e:
        print(f"UNKNOWN: {e}", file=sys.stderr)
        return 2
    except ScenarioError as e:
        print(f"FAIL: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
