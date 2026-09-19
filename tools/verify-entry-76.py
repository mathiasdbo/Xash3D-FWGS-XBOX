#!/usr/bin/env python3
"""
verify-entry-76.py - Phase 3 of docs/r3d/divergences.md #77's own plan:
use the synthetic-pad-input harness (tools/xemu-pad.py, tools/xemu-debug.py,
tools/xemu-pad-scenario.py) to actually verify entry #76's own still-open
claim - "Options -> Joystick no longer shows Gyroscope, and Options ->
Controls no longer shows Adv. Controls, both needing a pad to actually
navigate there."

Navigates Main -> Options -> Controls, screenshots it, backs out, then
Options -> Joystick, screenshots that too - the two screens divergence #76
named. Screenshots are the actual evidence a human can look at directly;
the machine-checkable signal this script adds on top is that ACTIVATING
the item that would be Adv. Controls/Gyroscope's own position, if either
were restored, instead closes the screen and returns to the very same
Options window object it was opened from (compared by pointer, not merely
by cursor number - see "Why by pointer" below), at the outer cursor
Options was left on.

## Why by pointer, not just by cursor number (Codex review round 1, MUST FIX)

An earlier draft of this script only checked that the item at the
expected index had SOME cursor number, then declared PASS without ever
activating it. That check cannot tell the difference between the item
that's supposed to be there (OK, Done) and the exact regression this
script exists to catch (Adv. Controls, Gyroscope restored at that same
index) - if either forbidden row came back, the cursor number would be
identical, and the script would still print PASS. Activating the item and
checking where it lands closes that gap: OK/Done/Cancel are terminal
actions that close their screen and return to Options, while Adv.
Controls/Gyroscope open a THIRD window instead - a different
`CMenuBaseWindow*`, not a coincidence a raw cursor number could ever rule
out. `xemu-pad-scenario.py`'s `cursor_and_window()` (new, alongside the
existing `cursor()`) returns that pointer.

## What this still doesn't prove, and why (Codex review round 2, MUST FIX
## OR JUSTIFY)

Confirming index 2 activates to OK/Done proves the forbidden row is not
AT that specific index - it does not, by itself, prove no forbidden row
exists anywhere else on the screen (a hypothetical reordering that kept
OK/Done at index 2 but inserted Adv. Controls/Gyroscope after it would
pass unnoticed). Two responses, not one, because the two screens differ:

- Controls.cpp's own Xbox order continues banner(0), UseDefaults(1),
  OK(2), Cancel(3), then the keysList (a different, non-button item -
  `keysList` is a `CMenuKeyBindingModel`-backed list, not a focusable
  button in this holder's own array the same way OK/Cancel are). Cancel
  is reachable and checkable the exact same way as OK (`Controls.cpp:65-69`:
  `Cancel()` also just calls `Hide()`) - this script now verifies BOTH,
  closing the gap for Controls entirely: nothing can sit between OK and
  Cancel, or after Cancel, without one of these two checks catching it.
- Gamepad.cpp's own Xbox order continues banner(0), Controls(1), Done(2),
  then SIX axis-binding buttons (`axisBind[0..5]`, real focusable
  `AddItem` picture-buttons for Side/Forward/Yaw/Pitch/the triggers -
  visible in the Joystick screenshot) and several slider/checkbox items.
  These do not "close to Options" when activated (they enter an axis-bind
  wait state or toggle a value), so the same activate-and-check-window
  proof this script uses for OK/Done/Cancel does not apply to them, and
  extending live coverage that far would trade a cheap, reliable check for
  an expensive, ambiguous one. Gyroscope's own guard (`Gamepad.cpp:236-241`)
  sits BETWEEN Controls(1) and Done - the only place it has ever existed in
  this tree - so if it were restored, Done would shift to index 3 and THIS
  script's existing index-2 check would already fail (index 2 would be
  Gyroscope itself, and activating it would open `UI_GamePadGyro_Menu`, a
  different window, not close to Options). The residual gap Codex named
  (Gyroscope hypothetically moved to AFTER Done) does not match how the
  guard is written anywhere in this codebase; for that hypothetical, and
  for the axis-bind items in general, the screenshot remains the actual
  closing proof - look at it, not just this script's exit code.

## Three real navigation surprises this script encodes (found live,
## 2026-09-19, replacing an earlier, wrong, source-reading-only guess)

1. Main -> Options is FIVE Down presses from a just-booted Main menu, not
   three. Main.cpp's own cursor sequence from boot is 0,1,3,4,5,7 (then
   wraps) - AdjustCursor skips indices 2 and 6 (always-hidden items
   consuming array slots, e.g. resumeGame/saveGame - present in the item
   array on Xbox but never shown or focusable), so cursor 7 (Options,
   confirmed live by entering it and reading "Configuration" off screen)
   is the SEVENTH item in the cursor sequence, reached by the FIFTH Down
   from cursor 0.
2. Options, Controls AND Gamepad (Joystick) screens all call
   `AddItem(banner)` - a decorative picture, `3rdparty/mainui/menus/
   Configuration.cpp:66`, `Controls.cpp:246`, `Gamepad.cpp:234` - BEFORE
   their first real button. `AddItem` puts it in the exact same array
   `m_iCursor` indexes (`ItemsHolder.cpp:529`), so every one of these
   screens' buttons sits ONE INDEX HIGHER than a naive count of
   `AddButton` calls suggests. The banner isn't skipped the way Main's
   truly-hidden items are (cursor CAN rest on it) - pressing A there just
   does nothing, since a bare picture has no click handler, which is what
   made it look like Main's "hidden slot" pattern at first and initially
   led this script to check the wrong item entirely (a first live test of
   the "activate and check where it lands" logic below found pressing A
   on what this script assumed was Controls.cpp's own "OK" button instead
   popped a "Reset all actions to their default keys?" dialog - the
   REAL `GameUI_UseDefaults` button's own callback, `Controls.cpp:247`,
   proving the check had the wrong index, not that OK does something
   unexpected).
3. Confirmed real per-screen mappings, accounting for the banner offset:
   Options: 0=banner, 1=Controls, 2=Audio, 3=Video, 4=Joystick, 5=Game,
   6=Done. Controls.cpp (Xbox): 0=banner, 1=GameUI_UseDefaults,
   2=GameUI_OK, 3=GameUI_Cancel - Adv. Controls, `#if !XASH_XBOX`, would
   sit at index 2 instead of OK if it were present. Gamepad.cpp (Xbox):
   0=banner, 1=Controls (button, opens Controls.cpp), 2=Done - Gyroscope,
   `#if !XASH_XBOX`, would sit at index 2 instead of Done if present.

Requires, already running: 'xemu-pad.py serve' and xemu launched with -s,
booted to Main with the initial cursor at 0 (New Game) - the same
precondition tools/xemu-pad-scenario.py's own 'demo' assumes.

Usage:
  python tools/verify-entry-76.py --map <file> --exe <pe> --out-dir <dir>
"""
import argparse
import importlib.util
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_SPEC = importlib.util.spec_from_file_location("xemu_pad_scenario", os.path.join(_HERE, "xemu-pad-scenario.py"))
xps = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(xps)

# 100ms hold, 400ms settle between presses throughout - divergences.md
# #78 found 200ms holds let UI auto-repeat fire a second move within one
# press; live testing for THIS script additionally found that even a
# 100ms hold with only the Scenario default 100ms settle between two
# rapid presses could still move 3 steps for 2 presses. 400ms settle
# between presses (not just after the final one) is what this script's
# own live testing actually walked the confirmed-correct path with.
HOLD = 100
SETTLE = 400


def press_n(sc, button, n):
    for _ in range(n):
        sc.press(button, hold_ms=HOLD, settle_ms=SETTLE)


def enter_and_check(sc, out_path, label):
    """Press A, screenshot, and confirm a NEW window actually opened (its
    own cursor starts at 0, the banner - see module docstring point 2) -
    cheap insurance against a silently-ignored A press looking, from
    screenshots alone, like nothing went wrong."""
    sc.press("a", hold_ms=HOLD, settle_ms=SETTLE)
    entered_cursor = sc.cursor()
    if entered_cursor != 0:
        print(f"FAIL: entering {label} - expected its own cursor to start at 0 (the banner), "
              f"got {entered_cursor} - the A press may not have opened a new window at all", file=sys.stderr)
        return False
    sc.screenshot(out_path)
    print(f"verify-entry-76: {label} screenshotted to {out_path} (own cursor confirmed 0 on entry)")
    return True


def confirm_item_closes_to_options(sc, label, options_window, options_cursor_after):
    """Press A on the item at Adv. Controls/Gyroscope's own would-be index
    (already confirmed reachable) and require landing back on the SAME
    Options window object, at the cursor Options was left on. A forbidden
    row restored at this same index would open a third window instead of
    closing this one - see the module docstring's "Why by pointer"
    section."""
    sc.press("a", hold_ms=HOLD, settle_ms=SETTLE)
    back_cursor, back_window = sc.cursor_and_window()
    if back_window != options_window or back_cursor != options_cursor_after:
        print(f"FAIL: activating {label}'s own item did not return to Options at cursor "
              f"{options_cursor_after} on the same window - got cursor {back_cursor}, window "
              f"{back_window:#x} (Options window was {options_window:#x}). If this is a NEW window, "
              f"a forbidden row was likely restored at this index instead of {label}'s own expected item.",
              file=sys.stderr)
        return False
    print(f"verify-entry-76: activating {label}'s own item returned to Options (cursor "
          f"{options_cursor_after}, same window) - not a forbidden row")
    return True


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--map", required=True)
    p.add_argument("--exe", required=True)
    p.add_argument("--out-dir", default=os.path.join(_HERE, "..", ".sweep", "verify-entry-76"))
    a = p.parse_args()
    os.makedirs(a.out_dir, exist_ok=True)

    sc = xps.Scenario(a.map, a.exe)

    print("verify-entry-76: reading the initial cursor...")
    start = sc.cursor()
    if start != 0:
        print(f"FAIL: expected cursor 0 (New Game) at Main, got {start} - "
              "this script assumes a just-booted Main menu", file=sys.stderr)
        return 1
    print(f"verify-entry-76: cursor={start}, as expected (New Game)")

    print("verify-entry-76: Main -> Options (Down x5, A)...")
    press_n(sc, "down", 5)
    options_cursor = sc.cursor()
    if options_cursor != 7:
        print(f"FAIL: expected cursor 7 (Options) after five Downs, got {options_cursor}", file=sys.stderr)
        return 1
    sc.press("a", hold_ms=HOLD, settle_ms=SETTLE)
    # Options' own screen starts on its banner (see module docstring) -
    # one Down reaches the first real row, Controls. Capture Options' own
    # window pointer here too - it stays the same object for the rest of
    # this boot, and confirm_item_closes_to_options() below needs it to
    # prove a return lands on OPTIONS, not merely on "some window
    # reporting the expected cursor number".
    press_n(sc, "down", 1)
    options_cursor_1, options_window = sc.cursor_and_window()
    if options_cursor_1 != 1:
        print(f"FAIL: expected cursor 1 (Controls) on Options after skipping the banner, "
              f"got {options_cursor_1}", file=sys.stderr)
        return 1

    print("verify-entry-76: Options -> Controls...")
    controls_shot = os.path.join(a.out_dir, "controls.png")
    if not enter_and_check(sc, controls_shot, "Controls"):
        return 1
    # Controls.cpp's own Xbox order: banner(0), GameUI_UseDefaults(1),
    # GameUI_OK(2), GameUI_Cancel(3) - Adv. Controls, `#if !XASH_XBOX`,
    # would sit at index 2 INSTEAD of OK if present. The screenshot is a
    # human-checkable record of the whole row list; activating index 2
    # and requiring a return to Options (below) is the machine-checkable
    # proof that it really is OK there, not a restored Adv. Controls row.
    press_n(sc, "down", 2)
    controls_cursor_2 = sc.cursor()
    if controls_cursor_2 != 2:
        print(f"FAIL: Controls's own OK-or-Adv.-Controls slot is cursor {controls_cursor_2}, expected 2",
              file=sys.stderr)
        return 1
    if not confirm_item_closes_to_options(sc, "Controls (OK)", options_window, 1):
        return 1
    # Re-enter and check Cancel too (index 3) - closes the "something
    # inserted between OK and Cancel, or after Cancel" gap Codex review
    # round 2 named; see the module docstring's own section on this.
    # confirm_item_closes_to_options() just left Options at cursor 1
    # (Controls' own row) - press A directly, no Down needed.
    sc.press("a", hold_ms=HOLD, settle_ms=SETTLE)  # re-enter Controls
    # Verify the re-entry actually reset to 0 (the banner) before trusting
    # a fixed Down count to land on Cancel - re-entry cursor state was
    # only ever confirmed for a screen's FIRST entry (enter_and_check()),
    # not a second one, and a first live run of this exact code found a
    # cursor of 2, not 3, after three Downs here - ambiguous between "a
    # Down was dropped" (divergences.md #78/#80's already-known settle
    # flakiness) and "re-entry didn't reset to 0". This check tells them
    # apart instead of guessing.
    controls_reentry_cursor = sc.cursor()
    if controls_reentry_cursor != 0:
        print(f"FAIL: re-entering Controls a second time did not reset its cursor to 0, "
              f"got {controls_reentry_cursor}", file=sys.stderr)
        return 1
    press_n(sc, "down", 3)  # banner(0) -> UseDefaults(1) -> OK(2) -> Cancel(3)
    controls_cursor_3 = sc.cursor()
    if controls_cursor_3 != 3:
        print(f"FAIL: Controls's own Cancel slot is cursor {controls_cursor_3}, expected 3", file=sys.stderr)
        return 1
    if not confirm_item_closes_to_options(sc, "Controls (Cancel)", options_window, 1):
        return 1

    print("verify-entry-76: Options -> Joystick...")
    press_n(sc, "down", 3)  # Controls(1) -> Audio(2) -> Video(3) -> Joystick(4)
    joystick_shot = os.path.join(a.out_dir, "joystick.png")
    if not enter_and_check(sc, joystick_shot, "Joystick"):
        return 1
    # Gamepad.cpp's own Xbox order: banner(0), Controls(1, the button
    # opening Controls.cpp), Done(2) - Gyroscope, `#if !XASH_XBOX`, would
    # sit at index 2 INSTEAD of Done if present. Same proof structure as
    # Controls above: activating index 2 must return to Options, not open
    # a third window.
    press_n(sc, "down", 2)
    joystick_cursor_2 = sc.cursor()
    if joystick_cursor_2 != 2:
        print(f"FAIL: Joystick's own Done-or-Gyroscope slot is cursor {joystick_cursor_2}, expected 2",
              file=sys.stderr)
        return 1
    if not confirm_item_closes_to_options(sc, "Joystick (Done)", options_window, 4):
        return 1

    print("verify-entry-76: PASS (positional) - Controls' OK and Cancel, and Joystick's Done, "
          "each independently confirmed to close back to Options rather than open a forbidden "
          "row's own submenu. This does NOT by itself rule out a forbidden row somewhere among "
          "Joystick's own axis-binding buttons (see the module docstring's own section on why) - "
          f"look at {controls_shot} and {joystick_shot} yourself to actually close divergence #76, "
          "not just this exit code.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
