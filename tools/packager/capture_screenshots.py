#!/usr/bin/env python3
"""Screenshots of the packager's own window, for the documentation site (Windows only).

    python tools/packager/capture_screenshots.py [--out site/media]

Starts the real GUI, puts it in each state below, grabs the window by its real screen bounds (title bar
included, shadow not) and closes it. The window comes up on top of your desktop for a few seconds: leave
the mouse alone. Needs Pillow.

  packager-package.png           the Package tab as it opens (the Steam install it finds, a folder export)
  packager-ftp.png               "Send to the Xbox over FTP" chosen, with an example address
  packager-console-settings.png  the Console settings tab
  packager-drift-warning.png     the Controller group and the stick-drift warning, for a console whose saved
                                 config.cfg has the PC's zero dead zones
  packager-drift-fixed.png       the same after the "Fix stick drift" preset

Two fields would show something personal and are set to examples instead: the folder the export is saved
to (it defaults to your home folder) and the Xbox's address. The two drift captures read a demo install, not a
console: a stand-in target whose saved config.cfg is the PC's four zero dead zones (what PC Half-Life writes).
"""
import argparse
import ctypes
import os
import sys
import tempfile
import time
import tkinter as tk
from ctypes import wintypes
from tkinter import ttk

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

EXAMPLE_FOLDER = r"C:\Users\you\Documents\Xbox"
EXAMPLE_ADDRESS = "192.168.1.50"
EXAMPLE_INSTALL = "/F/Games/Half-Life"
# The dead zones PC Half-Life's config.cfg holds, as the console would have saved them after running it
DEMO_CONFIG = "".join('joy_%s_deadzone "0"\r\n' % axis for axis in ("forward", "pitch", "side", "yaw")).encode()


def frame_rect(win):
    """The window's visible frame in screen pixels (DWM's extended frame bounds: no drop shadow)."""
    hwnd = ctypes.windll.user32.GetAncestor(win.winfo_id(), 2)  # GA_ROOT: tk's toplevel is a child of the frame
    rect = wintypes.RECT()
    ctypes.windll.dwmapi.DwmGetWindowAttribute(hwnd, 9, ctypes.byref(rect), ctypes.sizeof(rect))  # EXTENDED_FRAME_BOUNDS
    return rect.left, rect.top, rect.right, rect.bottom


def walk(w):
    yield w
    for c in w.winfo_children():
        yield from walk(c)


def field_beside(win, label_text):
    """the Entry on the same grid row as the Label with this text"""
    for lab in walk(win):
        if isinstance(lab, tk.Label) and lab.cget("text").strip() == label_text:
            row, parent = lab.grid_info().get("row"), lab.master
            for w in parent.winfo_children():
                if isinstance(w, tk.Entry) and w.grid_info().get("row") == row:
                    return w
    raise SystemExit("no field next to %r: the window's layout changed, update this script" % label_text)


def set_entry(entry, text):
    state = entry.cget("state")
    entry.configure(state="normal")
    entry.delete(0, "end")
    entry.insert(0, text)
    entry.configure(state=state)


def run(out):
    from PIL import ImageGrab

    def capture(win):
        win.geometry("+60+30")
        win.attributes("-topmost", True)

        def shot(name):
            win.update_idletasks()
            win.update()
            time.sleep(0.6)  # let it paint
            path = os.path.join(out, name)
            ImageGrab.grab(bbox=frame_rect(win), all_screens=True).save(path, optimize=True)
            print("wrote %s (%d bytes)" % (path, os.path.getsize(path)))

        win.update()
        save_to = [w for w in walk(win) if isinstance(w, tk.Entry) and w.get() == os.path.expanduser("~")]
        for w in save_to:
            set_entry(w, EXAMPLE_FOLDER)
        shot("packager-package.png")

        ftp = [w for w in walk(win) if isinstance(w, ttk.Radiobutton) and w.cget("text").startswith("Send to")]
        if not ftp:
            raise SystemExit("no FTP radio button: the window's layout changed, update this script")
        ftp[0].invoke()
        set_entry(field_beside(win, "Address"), EXAMPLE_ADDRESS)
        shot("packager-ftp.png")

        nb = [w for w in walk(win) if isinstance(w, ttk.Notebook)][0]
        nb.select(nb.tabs()[1])
        shot("packager-console-settings.png")

        # the stick-drift case: read a demo install whose saved config.cfg has the PC's zero dead zones
        api = holder["api"]
        api["install"].set(EXAMPLE_INSTALL)
        api["status"].set("")
        api["read"]()
        for _ in range(500):
            win.update()
            time.sleep(0.02)
            if api["status"].get().startswith("Read."):
                break
        else:
            raise SystemExit("reading the demo install did not finish: %s" % api["status"].get())
        for c in (w for w in walk(win) if isinstance(w, tk.Canvas)):
            c.yview_moveto(0.22)  # the Controller group
        shot("packager-drift-warning.png")
        api["preset"]("Fix stick drift")
        shot("packager-drift-fixed.png")
        win.destroy()

    from r3dpack import settings_tab, target
    holder = {}
    demo = tempfile.TemporaryDirectory()
    os.makedirs(os.path.join(demo.name, "valve", "userconfig.d"))

    class DemoXbox(target.FolderTarget):
        """stands in for the console: a folder, with a saved config.cfg like the one the old export led to"""

        def read_live_config(self, gamedir):
            return DEMO_CONFIG

    target.FtpTarget = lambda host, install, user=None, password=None, port=21: DemoXbox(demo.name)
    real_build = settings_tab.build

    def build(tab, ui):
        holder["api"] = real_build(tab, ui)
        return holder["api"]
    settings_tab.build = build

    tk.Tk.mainloop = lambda self, n=0: capture(self)
    import r3d_packager
    r3d_packager.gui()
    demo.cleanup()


def main():
    if sys.platform != "win32":
        raise SystemExit("This captures the window with Windows' own frame bounds: Windows only.")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.dirname(HERE)), "site", "media"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    ctypes.windll.shcore.SetProcessDpiAwareness(2)  # tk then reports physical pixels, as ImageGrab uses
    run(a.out)


if __name__ == "__main__":
    main()
