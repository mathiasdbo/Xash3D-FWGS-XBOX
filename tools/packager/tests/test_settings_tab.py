"""Drives the real Console settings tab (Tk) against a folder, with the dialogs answered."""
import os
import queue
import sys
import tempfile
import time
import types
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

try:
    import tkinter as tk
    root = tk.Tk()
    root.withdraw()
except Exception:  # no display
    tk = None


def ui_for(q):
    names = ("bg", "header", "card", "field", "button", "button_hover", "line", "text", "muted",
             "faint", "accent", "accent_hover", "accent_down", "ok", "bad")
    C = {k: "#202024" for k in names}

    def label(parent, text="", fg=None, font=None, **kw):
        return tk.Label(parent, text=text, **kw)

    def button(parent, text, command=None, accent=False, **kw):
        return tk.Button(parent, text=text, command=command, **kw)

    def card(parent, title):
        f = tk.LabelFrame(parent, text=title)
        f.pack(fill="x")
        return f
    return types.SimpleNamespace(
        C=C, label=label, entry=lambda p, v, **kw: tk.Entry(p, textvariable=v, **kw), button=button, card=card, q=q,
        host_var=tk.StringVar(), user_var=tk.StringVar(value="xbox"), pass_var=tk.StringVar(value="xbox"),
        rdir_var=tk.StringVar(), title_var=tk.StringVar(value="hl"))


@unittest.skipIf(tk is None, "no display")
class Tab(unittest.TestCase):
    def setUp(self):
        from r3dpack import settings_tab
        self.st = settings_tab
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        d = os.path.join(self.tmp.name, "valve", "userconfig.d")
        os.makedirs(d)
        with open(os.path.join(d, "90-route.cfg"), "wb") as f:
            f.write(b"r3d_vid_mode 0\r\nmap c0a0\r\n")
        self.q = queue.Queue()
        self.frame = tk.Frame(root)
        self.api = settings_tab.build(self.frame, ui_for(self.q))
        self.api["where"].set("folder")
        self.api["install"].set(self.tmp.name)

    def tearDown(self):
        self.frame.destroy()

    def pump(self, until, timeout=10):
        end = time.time() + timeout
        while time.time() < end:
            root.update()
            try:
                while True:
                    kind, val = self.q.get_nowait()
                    if kind == "call":
                        val()
            except queue.Empty:
                pass
            if until():
                return
            time.sleep(0.01)
        self.fail("timed out; status: %s" % self.api["status"].get())

    def read(self):
        self.api["status"].set("")
        self.api["read"]()
        self.pump(lambda: self.api["status"].get().startswith("Read."))

    def test_read_shows_the_files_and_the_override(self):
        self.read()
        rows = [r["name"] for r in self.api["state"]["rows"].values()]
        self.assertEqual(rows, ["90-route.cfg"])
        shown = self.api["now"].get("1.0", "end")
        self.assertIn("r3d_vid_mode = 0", shown)
        self.assertIn("loads map c0a0", shown)

    def test_write_a_choice_and_a_toggle(self):
        self.read()
        v, maps = self.api["vars"], self.api["maps"]
        label = next(lab for lab, val in maps["r3d_vid_mode"][0].items() if val == "1")
        v["r3d_vid_mode"].set(label)
        v["host_frametime_label"].set("office-run")
        iid = next(iter(self.api["state"]["rows"]))
        self.api["state"]["rows"][iid]["now"] = False  # switch the route file off
        with mock.patch.object(self.st.messagebox, "askyesno", return_value=True) as ask:
            self.api["write"]()
        self.assertIn("+ r3d_vid_mode 1", ask.call_args[0][1])
        self.assertIn("disable 90-route.cfg", ask.call_args[0][1])
        self.pump(lambda: self.api["status"].get().startswith("Written"))
        d = os.path.join(self.tmp.name, "valve", "userconfig.d")
        self.assertEqual(sorted(os.listdir(d)), ["50-packager.cfg", "90-route.cfg.off"])
        with open(os.path.join(d, "50-packager.cfg"), "rb") as f:
            text = f.read().decode()
        self.assertIn('r3d_vid_mode "1"', text)
        self.assertIn('host_frametime_label "office-run"', text)
        self.assertNotIn("nv2a_r_aa", text)

    def test_declining_the_dialog_changes_nothing(self):
        self.read()
        self.api["preset"]("Sharp")
        with mock.patch.object(self.st.messagebox, "askyesno", return_value=False):
            self.api["write"]()
        root.update()
        d = os.path.join(self.tmp.name, "valve", "userconfig.d")
        self.assertEqual(os.listdir(d), ["90-route.cfg"])

    def test_a_bad_value_is_refused_on_screen(self):
        self.read()
        self.api["vars"]["host_frametime_label"].set("not allowed!")
        with mock.patch.object(self.st.messagebox, "askyesno", return_value=True) as ask:
            self.api["write"]()
        self.assertFalse(ask.called)
        self.assertIn("label", self.api["status"].get())

    def test_presets_fill_and_clear_the_choices(self):
        self.api["preset"]("Sharp")
        self.assertTrue(self.api["vars"]["r3d_vid_mode"].get().startswith("1280x720"))
        self.assertTrue(self.api["vars"]["nv2a_r_anisotropy"].get().startswith("8x"))
        self.api["preset"]("Defaults")
        self.assertEqual(self.api["vars"]["r3d_vid_mode"].get(), self.st.UNSET)

    def test_reading_a_missing_folder_says_so(self):
        self.api["install"].set(os.path.join(self.tmp.name, "nope"))
        self.api["status"].set("")
        self.api["read"]()
        self.pump(lambda: "not a folder" in self.api["status"].get())


try:
    from pyftpdlib.authorizers import DummyAuthorizer
    from pyftpdlib.handlers import FTPHandler
    from pyftpdlib.servers import FTPServer
except ImportError:
    FTPServer = None


@unittest.skipIf(tk is None or FTPServer is None, "needs a display and pyftpdlib")
class Scan(unittest.TestCase):
    """Find my Xbox in the Console settings tab, against a fake console."""

    def setUp(self):
        import threading
        from r3dpack import settings_tab, xboxftp
        self.st, self.xf = settings_tab, xboxftp
        self.root = tempfile.mkdtemp()
        half = os.path.join(self.root, "F", "Games", "Half-Life")
        os.makedirs(os.path.join(half, "valve", "userconfig.d"))
        route = b"r3d_vid_mode 1" + bytes([13, 10]) + b"map c0a0" + bytes([13, 10])
        for rel, data in (("default.xbe", b"XBEH"), ("valve/userconfig.d/90-route.cfg", route)):
            with open(os.path.join(half, *rel.split("/")), "wb") as f:
                f.write(data)
        other = os.path.join(self.root, "F", "Games", "Halo")  # another game: not offered
        os.makedirs(other)
        with open(os.path.join(other, "default.xbe"), "wb") as f:
            f.write(b"XBEH")
        os.makedirs(os.path.join(self.root, "E"))
        a = DummyAuthorizer()
        a.add_user("xbox", "xbox", self.root, perm="elradfmwMT")
        h = FTPHandler
        h.authorizer = a
        self.srv = FTPServer(("127.0.0.1", 0), h)
        self.port = self.srv.address[1]
        threading.Thread(target=self.srv.serve_forever, kwargs={"timeout": 0.1}, daemon=True).start()
        real = xboxftp.Xbox
        self.patches = [mock.patch.object(xboxftp, "Xbox", lambda host, user, password, port=21, timeout=30:
                                          real(host, user, password, port=self.port, timeout=timeout))]
        for p in self.patches:
            p.start()
        self.q = queue.Queue()
        self.frame = tk.Frame(root)
        self.ui = ui_for(self.q)
        self.api = settings_tab.build(self.frame, self.ui)

    def tearDown(self):
        for p in self.patches:
            p.stop()
        self.srv.close_all()
        self.frame.destroy()

    def pump(self, until, timeout=15):
        end = time.time() + timeout
        while time.time() < end:
            root.update()
            try:
                while True:
                    kind, val = self.q.get_nowait()
                    if kind == "call":
                        val()
            except queue.Empty:
                pass
            if until():
                return
            time.sleep(0.01)
        self.fail("timed out; status: %s" % self.api["status"].get())

    def test_one_scan_fills_the_address_finds_the_install_and_reads_it(self):
        with mock.patch.object(self.xf, "scan", return_value=["127.0.0.1"]):
            self.api["scan"]()
            self.pump(lambda: self.api["status"].get().startswith("Read."))
        self.assertEqual(self.ui.host_var.get(), "127.0.0.1")
        self.assertEqual(self.ui.rdir_var.get(), "/F/Games")          # the big drive's Games folder
        self.assertEqual(self.api["install"].get(), "/F/Games/Half-Life")
        self.assertEqual(list(self.api["installs"]["values"]), ["/F/Games/Half-Life"])  # not Halo
        self.assertIn("r3d_vid_mode = 1", self.api["now"].get("1.0", "end"))

    def test_no_console_is_said_plainly(self):
        with mock.patch.object(self.xf, "scan", return_value=[]):
            self.api["scan"]()
            self.pump(lambda: "No Xbox found" in self.api["status"].get())

    def test_find_installs_without_an_address_asks_for_one(self):
        self.api["find"]()
        self.assertIn("Find my Xbox", self.api["status"].get())

    def test_the_address_and_scan_are_off_for_a_folder(self):
        self.api["where"].set("folder")
        root.update()
        self.assertEqual(str(self.api["scan_btn"].cget("state")), "disabled")
        self.api["where"].set("xbox")
        root.update()
        self.assertEqual(str(self.api["scan_btn"].cget("state")), "normal")


if __name__ == "__main__":
    unittest.main()
