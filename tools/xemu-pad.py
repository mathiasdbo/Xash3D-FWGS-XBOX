#!/usr/bin/env python3
"""
xemu-pad.py - synthetic Xbox 360 controller input for a running xemu
instance, via ViGEmBus (a Windows kernel-mode virtual-XInput driver) and
its Python client wrapper, vgamepad.

Owns ViGEmBus/vgamepad exclusively, kept out of tools/xemu-debug.py on
purpose: that tool is stdlib-only by its own stated design ("Stdlib
only... No pip"), and vgamepad is a pip, Windows-only, native-DLL-backed
dependency - bolting it onto the GDB-remote client would break that
property for anyone who only wants to read guest state, not inject input.
This tool talks to xemu through an entirely different transport (an OS-
level virtual USB device, not the GDB stub) and has nothing to do with
GdbRemote.

## Why a serve/client split, not one-shot presses

A ViGEmBus virtual pad only exists while its owning process is alive -
destroying the Python object (or the process exiting) unplugs it. xemu
resolves which SDL joystick GUID `xemu.toml`'s `port1` binding means once,
and a device that disappears and reappears mid-session is not guaranteed
to be picked back up (found live, docs/r3d/divergences.md #77: the
device's own GUID was stable across recreation in the same session, but
losing and regaining the OS-level device while xemu was already running
left it unresponsive until xemu itself was relaunched). A scenario that
presses several buttons over time therefore needs ONE persistent process
holding the pad open for the whole scenario, not one process per press.

`serve` is that persistent process: it creates the pad once and then
watches a directory for one command file per request, writing one result
file per request - deliberately files, not a socket or named pipe, so
this stays stdlib-only-in-spirit even though the pad itself is not (the
same file-polling scheme this fork's own feasibility spike already proved
reliable enough to move a live menu cursor). `press`/`axis`/`quit` are
thin one-shot clients: write a command file, wait for its own matching
result file, print it, exit. Run one `serve` per test session; run
`press`/`axis` as many times as the scenario needs against it.

Each command gets its own unique ID, carried in both the command
filename and the result it produces (Codex review, round 1: an earlier
version shared one command file and one append-only result file, matched
by "did the result file grow since I wrote my command" - a command that
outlived its own client's timeout left an unclaimed result behind that a
LATER, unrelated command would then claim as its own, reporting success
for an input that never happened, or reporting the wrong one's result. A
plain shared command file also let two concurrent clients overwrite each
other's request outright. Per-command files sidestep both: a client only
ever waits for the one result filename it itself asked for, and two
clients writing distinct files can never collide).

## Commands

  serve [--dir D]
                                Create the virtual pad and hold it open,
                                 watching --dir (default .sweep/xemu-pad/)
                                 for cmd_<id>.txt files written by
                                 press/axis/quit, writing one
                                 result_<id>.txt per command it finds
                                 (deleting the command file first, so a
                                 crash mid-dispatch cannot reprocess it).
                                 Runs until a "quit" command arrives or
                                 the process is killed.
  press <button> [--hold-ms N] [--settle-ms N] [--dir D] [--timeout N]
                                Ask an already-running serve process to
                                 press and release one button. <button> is
                                 an XUSB_BUTTON name with or without the
                                 XUSB_GAMEPAD_ prefix (a, b, x, y, up,
                                 down, left, right, start, back,
                                 leftshoulder, rightshoulder, leftthumb,
                                 rightthumb, guide). --hold-ms (default
                                 200) is how long the button stays down;
                                 --settle-ms (default 100) is an extra
                                 wait after release before this returns,
                                 for the guest to actually process the
                                 release edge before the next command.
                                 Neither value is measured against this
                                 fork's own real frame timing yet - both
                                 are conservative placeholders (see
                                 docs/r3d/divergences.md #77's own "NOT
                                 yet confirmed" list) until a real
                                 measurement replaces them.
  axis <left|right> <x> <y> [--settle-ms N] [--dir D] [--timeout N]
                                Set a thumbstick's position, -1.0 to 1.0
                                 on each axis, and leave it there (no
                                 auto-release - call again with 0 0 to
                                 recenter).
  quit [--dir D] [--timeout N]
                                Ask the serve process to unplug the pad
                                 and exit.

Exit codes for press/axis/quit: 0 on success. 1 if the command was never
claimed at all - serve is not running, or timed out before ever picking
it up - safe to treat as "nothing happened" and retry. 2 if serve DID
claim the command (its file is gone) but never answered within the
timeout: the outcome is genuinely unknown - it may still complete, or
may have already, and a script must not assume either and must not
retry it as a fresh command (retrying could double-apply an input that
actually landed).
"""
import argparse
import os
import sys
import time
import uuid

DEFAULT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".sweep", "xemu-pad")

# Accept a button name with or without the XUSB_GAMEPAD_ prefix, and a few
# shorter aliases for the ones this fork's own menus actually use (D-pad,
# face buttons, Start) - typing "down" beats typing "XUSB_GAMEPAD_DPAD_DOWN"
# for every scenario script that will ever call this.
BUTTON_ALIASES = {
    "up": "XUSB_GAMEPAD_DPAD_UP", "down": "XUSB_GAMEPAD_DPAD_DOWN",
    "left": "XUSB_GAMEPAD_DPAD_LEFT", "right": "XUSB_GAMEPAD_DPAD_RIGHT",
    "a": "XUSB_GAMEPAD_A", "b": "XUSB_GAMEPAD_B", "x": "XUSB_GAMEPAD_X", "y": "XUSB_GAMEPAD_Y",
    "start": "XUSB_GAMEPAD_START", "back": "XUSB_GAMEPAD_BACK",
    "leftshoulder": "XUSB_GAMEPAD_LEFT_SHOULDER", "rightshoulder": "XUSB_GAMEPAD_RIGHT_SHOULDER",
    "leftthumb": "XUSB_GAMEPAD_LEFT_THUMB", "rightthumb": "XUSB_GAMEPAD_RIGHT_THUMB",
    "guide": "XUSB_GAMEPAD_GUIDE",
}


def resolve_button_name(name):
    """User-facing button name -> the exact XUSB_BUTTON enum member name.
    Raises on anything unrecognised rather than silently pressing the
    wrong button."""
    key = name.strip().lower()
    if key in BUTTON_ALIASES:
        return BUTTON_ALIASES[key]
    upper = name.strip().upper()
    if not upper.startswith("XUSB_GAMEPAD_"):
        upper = "XUSB_GAMEPAD_" + upper
    valid = {"XUSB_GAMEPAD_DPAD_UP", "XUSB_GAMEPAD_DPAD_DOWN", "XUSB_GAMEPAD_DPAD_LEFT",
             "XUSB_GAMEPAD_DPAD_RIGHT", "XUSB_GAMEPAD_START", "XUSB_GAMEPAD_BACK",
             "XUSB_GAMEPAD_LEFT_THUMB", "XUSB_GAMEPAD_RIGHT_THUMB", "XUSB_GAMEPAD_LEFT_SHOULDER",
             "XUSB_GAMEPAD_RIGHT_SHOULDER", "XUSB_GAMEPAD_GUIDE", "XUSB_GAMEPAD_A",
             "XUSB_GAMEPAD_B", "XUSB_GAMEPAD_X", "XUSB_GAMEPAD_Y"}
    if upper not in valid:
        raise ValueError(f"unknown button {name!r} - known names: "
                          f"{', '.join(sorted(BUTTON_ALIASES))} or a bare XUSB_BUTTON name")
    return upper


def parse_axis_name(name):
    key = name.strip().lower()
    if key not in ("left", "right"):
        raise ValueError(f"unknown stick {name!r} - must be 'left' or 'right'")
    return key


def _cmd_path(d, cmd_id):
    return os.path.join(d, f"cmd_{cmd_id}.txt")


def _result_path(d, cmd_id):
    return os.path.join(d, f"result_{cmd_id}.txt")


def cmd_serve(a):
    import vgamepad as vg  # deferred: only serve actually needs ViGEmBus installed

    d = os.path.abspath(a.dir)
    os.makedirs(d, exist_ok=True)
    # Clear anything left over from a previous, uncleanly-ended session -
    # a stale cmd_*.txt here would otherwise be picked up and executed the
    # moment this serve starts, for a client that is long gone.
    for name in os.listdir(d):
        if name.startswith(("cmd_", "result_")) and name.endswith(".txt"):
            os.remove(os.path.join(d, name))

    gamepad = vg.VX360Gamepad()
    print(f"xemu-pad: virtual pad created, watching {d}", flush=True)

    while True:
        cmd_names = sorted(n for n in os.listdir(d) if n.startswith("cmd_") and n.endswith(".txt"))
        for name in cmd_names:
            cmd_id = name[len("cmd_"):-len(".txt")]
            path = os.path.join(d, name)
            try:
                with open(path) as f:
                    line = f.read().strip()
            except OSError:
                continue  # a client's os.replace() raced us; it will retry
            # Delete before dispatching, not after: a crash mid-dispatch
            # then loses this one command rather than replaying it forever.
            try:
                os.remove(path)
            except OSError:
                pass
            result = _dispatch(gamepad, vg, line)
            with open(_result_path(d, cmd_id), "w") as f:
                f.write(result + "\n")
            print(f"xemu-pad: [{cmd_id}] {result}", flush=True)
            if line == "quit":
                print("xemu-pad: pad unplugged, exiting", flush=True)
                return
        time.sleep(0.02)


def _dispatch(gamepad, vg, line):
    parts = line.split()
    if not parts:
        return "error empty command"
    op = parts[0]
    try:
        if op == "quit":
            return "ok quit"
        if op == "press":
            button_name, hold_ms = parts[1], int(parts[2])
            if hold_ms < 0:
                # Validated before the button ever goes down - Codex review
                # round 2: time.sleep() raises ValueError on a negative
                # duration, which would otherwise leave the button
                # pressed with nothing left to release it.
                return f"error hold_ms must be >= 0, got {hold_ms}"
            button = getattr(vg.XUSB_BUTTON, button_name)
            gamepad.press_button(button=button)
            gamepad.update()
            try:
                time.sleep(hold_ms / 1000.0)
            finally:
                # Guaranteed even if the sleep itself raises (or anything
                # else does, once the enclosing try/except below is also
                # considered) - Codex review round 2: a stuck-down button
                # after any dispatch exception would silently contaminate
                # every later press/read in the same serve session, not
                # just fail the one command that caused it.
                gamepad.release_button(button=button)
                gamepad.update()
            return f"ok pressed {button_name} for {hold_ms}ms"
        if op == "axis":
            stick, x, y = parts[1], float(parts[2]), float(parts[3])
            if stick == "left":
                gamepad.left_joystick_float(x_value_float=x, y_value_float=y)
            else:
                gamepad.right_joystick_float(x_value_float=x, y_value_float=y)
            gamepad.update()
            return f"ok axis {stick} {x} {y}"
        return f"error unknown command {op!r}"
    except Exception as e:  # noqa: BLE001 - reported to the client, not swallowed
        return f"error {e}"


def _send(a, line):
    d = os.path.abspath(a.dir)
    os.makedirs(d, exist_ok=True)
    cmd_id = uuid.uuid4().hex[:12]
    cmd_path = _cmd_path(d, cmd_id)
    result_path = _result_path(d, cmd_id)
    tmp_path = cmd_path + ".tmp"
    # Write-then-rename: os.replace() is atomic on both POSIX and Windows,
    # so serve's own os.listdir() never sees a partially-written command
    # file - the failure mode a shared, in-place-appended file could not
    # avoid at all.
    with open(tmp_path, "w") as f:
        f.write(line)
    os.replace(tmp_path, cmd_path)

    deadline = time.monotonic() + a.timeout
    while time.monotonic() < deadline:
        if os.path.exists(result_path):
            try:
                with open(result_path) as f:
                    result = f.read().strip()
            except OSError:
                time.sleep(0.02)
                continue
            try:
                os.remove(result_path)
            except OSError:
                pass
            print(result)
            return 0 if result.startswith("ok ") else 1
        time.sleep(0.02)

    # Timed out. One last check - the result may have landed in the gap
    # between the loop's last iteration and here.
    if os.path.exists(result_path):
        with open(result_path) as f:
            result = f.read().strip()
        try:
            os.remove(result_path)
        except OSError:
            pass
        print(result)
        return 0 if result.startswith("ok ") else 1

    # Still nothing. Whether this command's own eventual result (if any)
    # can be trusted depends on whether serve had already claimed it -
    # Codex review round 2: reporting a plain, uniform failure here would
    # let a caller assume "definitely did not happen" and retry, when
    # serve may already be mid-dispatch and the input could still land
    # (or double-apply, if retried) after this call returns. Removing the
    # command file, if it is still ours to remove, is what actually
    # decides which case this is: gone already means serve got there
    # first (unknown outcome, exit 2); still present and removable here
    # means serve never saw it at all (nothing happened, exit 1, safe to
    # retry as a fresh command).
    try:
        os.remove(cmd_path)
        claimed = False
    except OSError:
        claimed = True

    if claimed:
        print(f"error: xemu-pad serve accepted this command but never answered within "
              f"{a.timeout}s - outcome UNKNOWN, do not assume it failed or retry it "
              f"(id {cmd_id})", file=sys.stderr)
        return 2
    print(f"error: no response from xemu-pad serve within {a.timeout}s - "
          f"is 'xemu-pad.py serve' running, watching {d}?", file=sys.stderr)
    return 1


def cmd_press(a):
    button = resolve_button_name(a.button)
    return _send(a, f"press {button} {a.hold_ms}")


def cmd_axis(a):
    stick = parse_axis_name(a.stick)
    return _send(a, f"axis {stick} {a.x} {a.y}")


def cmd_quit(a):
    return _send(a, "quit")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    def add_dir_arg(sp):
        sp.add_argument("--dir", default=DEFAULT_DIR)

    s = sub.add_parser("serve")
    add_dir_arg(s)
    s.set_defaults(func=cmd_serve)

    s = sub.add_parser("press")
    s.add_argument("button")
    s.add_argument("--hold-ms", type=int, default=200)
    s.add_argument("--settle-ms", type=int, default=100)
    s.add_argument("--timeout", type=float, default=5.0)
    add_dir_arg(s)
    s.set_defaults(func=cmd_press)

    s = sub.add_parser("axis")
    s.add_argument("stick")
    s.add_argument("x", type=float)
    s.add_argument("y", type=float)
    s.add_argument("--settle-ms", type=int, default=100)
    s.add_argument("--timeout", type=float, default=5.0)
    add_dir_arg(s)
    s.set_defaults(func=cmd_axis)

    s = sub.add_parser("quit")
    s.add_argument("--timeout", type=float, default=5.0)
    add_dir_arg(s)
    s.set_defaults(func=cmd_quit)

    a = p.parse_args()
    if a.cmd == "serve":
        a.func(a)
        return 0
    rc = a.func(a)
    if a.cmd in ("press", "axis") and rc == 0 and a.settle_ms > 0:
        time.sleep(a.settle_ms / 1000.0)
    return rc


if __name__ == "__main__":
    sys.exit(main())
