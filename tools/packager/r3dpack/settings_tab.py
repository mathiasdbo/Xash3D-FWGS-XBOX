"""The Console settings tab: read the settings a console install runs with, change them,
write them back (to the console over FTP, or to an exported folder on this PC).

Only the Tk side lives here. What the settings are, how the cfg file is written and where it
goes are in consolecfg.py and target.py, which have the tests.
"""

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from . import consolecfg, target as tg, xboxftp
from .titles import TITLES

UNSET = "(don't set)"


def build(tab, ui):
    """tab: the notebook page. ui: the window's theme and shared values (see r3d_packager.gui)."""
    C = ui.C
    state = {"loaded": None, "rows": {}, "busy": False}
    where_var = tk.StringVar(value="xbox")
    install_var = tk.StringVar()
    status_var = tk.StringVar(value="Pick where the install is, then Read.")
    show_dev = tk.BooleanVar(value=False)
    vars_ = {}   # key -> StringVar holding the chosen label
    maps = {}    # key -> ({label: value}, [labels])

    # ---- where ----
    top = ui.card(tab, "Install")
    top.columnconfigure(1, weight=1)
    r = tk.Frame(top, bg=C["card"])
    r.grid(row=0, column=0, columnspan=3, sticky="w")
    ttk.Radiobutton(r, text="On the Xbox (FTP)", variable=where_var, value="xbox", style="Dark.TRadiobutton").pack(side="left")
    ttk.Radiobutton(r, text="A folder on this PC", variable=where_var, value="folder", style="Dark.TRadiobutton").pack(side="left", padx=14)
    addr_lbl = ui.label(top, "Address")
    addr_lbl.grid(row=1, column=0, sticky="w", pady=(8, 0))
    host_entry = ui.entry(top, ui.host_var)
    host_entry.grid(row=1, column=1, sticky="ew", padx=10, pady=(8, 0), ipady=4)
    scan_btn = ui.button(top, "Find my Xbox", padx=12, pady=4)
    scan_btn.grid(row=1, column=2, pady=(8, 0))
    ui.label(top, "Install").grid(row=2, column=0, sticky="w", pady=(8, 0))
    inst_box = ttk.Combobox(top, textvariable=install_var, style="Dark.TCombobox")
    inst_box.grid(row=2, column=1, sticky="ew", padx=10, pady=(8, 0))
    find_btn = ui.button(top, "Find installs", padx=12, pady=4)
    find_btn.grid(row=2, column=2, pady=(8, 0))
    read_btn = ui.button(top, "Read", padx=18, pady=4)
    read_btn.grid(row=2, column=3, padx=(8, 0), pady=(8, 0))
    ui.label(top, "The address is shared with the Package tab; the login is the one set there.", C["faint"]).grid(
        row=3, column=0, columnspan=4, sticky="w", pady=(4, 0))

    # ---- the settings (scrollable) ----
    mid = tk.Frame(tab, bg=C["bg"])
    mid.pack(fill="both", expand=True, pady=(0, 8))
    canvas = tk.Canvas(mid, bg=C["bg"], highlightthickness=0, height=120)
    sb = tk.Scrollbar(mid, command=canvas.yview, bg=C["button"], troughcolor=C["bg"], relief="flat", bd=0)
    canvas.configure(yscrollcommand=sb.set)
    sb.pack(side="right", fill="y")
    canvas.pack(side="left", fill="both", expand=True)
    inner = tk.Frame(canvas, bg=C["bg"])
    win_id = canvas.create_window((0, 0), window=inner, anchor="nw")
    inner.bind("<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
    canvas.bind("<Configure>", lambda e: canvas.itemconfigure(win_id, width=e.width))

    def wheel(e):
        canvas.yview_scroll(int(-1 * (e.delta / 120)), "units")
    canvas.bind("<Enter>", lambda e: canvas.bind_all("<MouseWheel>", wheel))
    canvas.bind("<Leave>", lambda e: canvas.unbind_all("<MouseWheel>"))

    cards = {}
    for s in consolecfg.SETTINGS:
        g = s["group"]
        if g not in cards:
            cards[g] = ui.card(inner, g)
            cards[g].columnconfigure(2, weight=1)
            cards[g]._row = 0
        f = cards[g]
        row = f._row
        f._row += 1
        ui.label(f, s["label"]).grid(row=row, column=0, sticky="w", pady=2)
        v = tk.StringVar(value=UNSET)
        vars_[s["key"]] = v
        if s["kind"] == "choice":
            labels = [UNSET] + [lab + ("  (default)" if val == s["default"] else "") for val, lab in s["choices"]]
            maps[s["key"]] = ({lab: val for (val, _), lab in zip(s["choices"], labels[1:])}, labels)
            w = ttk.Combobox(f, textvariable=v, values=labels, state="readonly", style="Dark.TCombobox", width=34)
            w.grid(row=row, column=1, sticky="w", padx=10, pady=2)
        else:
            v.set("")
            w = ui.entry(f, v, width=36)
            w.grid(row=row, column=1, sticky="w", padx=10, pady=2, ipady=3)
        ui.label(f, s["help"], C["faint"], wraplength=300, justify="left").grid(row=row, column=2, sticky="w")

    # startup (not cvars: lines of the file)
    sc = ui.card(inner, "Startup")
    cards["Startup"] = sc
    map_var, cheats_var, suit_var = tk.StringVar(), tk.BooleanVar(), tk.BooleanVar()
    ui.label(sc, "Start on map").grid(row=0, column=0, sticky="w", pady=2)
    ui.entry(sc, map_var, width=20).grid(row=0, column=1, sticky="w", padx=10, pady=2, ipady=3)
    ui.label(sc, "A map name such as c0a0; leave empty for the menu.", C["faint"]).grid(row=0, column=2, sticky="w")
    ttk.Checkbutton(sc, text="Cheats on", variable=cheats_var, style="Dark.TCheckbutton").grid(row=1, column=1, sticky="w", padx=10)
    ttk.Checkbutton(sc, text="Give the suit and weapons", variable=suit_var, style="Dark.TCheckbutton").grid(row=2, column=1, sticky="w", padx=10)

    def show_groups(*_):
        dev = cards.get("Developer")
        if dev is None:
            return
        if show_dev.get():
            dev.pack(fill="x", pady=(0, 10))
        else:
            dev.pack_forget()
    show_groups()

    # ---- presets and the developer switch ----
    pr = tk.Frame(tab, bg=C["bg"])
    pr.pack(fill="x", pady=(0, 8))
    lab = ui.label(pr, "Presets", C["muted"])
    lab.configure(bg=C["bg"])
    lab.pack(side="left")
    preset_btns = {}
    for name in consolecfg.PRESETS:
        b = ui.button(pr, name if name != "Defaults" else "Clear all", padx=12, pady=3)
        b.pack(side="left", padx=(8, 0))
        preset_btns[name] = b
    cb = ttk.Checkbutton(pr, text="Developer settings", variable=show_dev, command=show_groups, style="Bg.TCheckbutton")
    cb.pack(side="right")

    # ---- what the console has now ----
    now = ui.card(tab, "What the console runs with")
    now_text = tk.Text(now, height=4, wrap="word", font=("Consolas", 9), bg=C["header"], fg=C["muted"], bd=0,
                       padx=8, pady=6, state="disabled")
    now_text.pack(fill="x")
    now_text.tag_configure("warn", foreground=C["accent"])
    now_text.tag_configure("val", foreground=C["text"])

    # ---- the files ----
    ff = ui.card(tab, "Files in userconfig.d (tick = runs at boot)")
    tree = ttk.Treeview(ff, columns=("on", "file", "size"), show="headings", height=3, selectmode="browse")
    tree.heading("on", text="On")
    tree.heading("file", text="File")
    tree.heading("size", text="Bytes")
    tree.column("on", width=40, anchor="center", stretch=False)
    tree.column("file", width=380)
    tree.column("size", width=70, anchor="e", stretch=False)
    tree.pack(fill="x")
    fb = tk.Frame(ff, bg=C["card"])
    fb.pack(fill="x", pady=(6, 0))
    edit_btn = ui.button(fb, "View / edit file...", padx=12, pady=3)
    edit_btn.pack(side="left")

    # ---- the buttons ----
    bot = tk.Frame(tab, bg=C["bg"])
    bot.pack(fill="x", pady=(4, 0))
    write_btn = ui.button(bot, "Write settings", accent=True, padx=22, pady=7)
    write_btn.pack(side="left")
    revert_btn = ui.button(bot, "Revert", padx=14, pady=7)
    revert_btn.pack(side="left", padx=(10, 0))
    st_lbl = ui.label(bot, "", C["muted"], textvariable=status_var, anchor="w", wraplength=420, justify="left")
    st_lbl.configure(bg=C["bg"])
    st_lbl.pack(side="left", padx=14, fill="x", expand=True)

    # ---- plumbing ----
    def set_busy(b):
        state["busy"] = b
        for w in (read_btn, write_btn, revert_btn, find_btn, edit_btn, scan_btn):
            w.configure(state="disabled" if b else "normal")
        if not b:
            update_find_label()

    def say(msg, bad=False):
        status_var.set(msg)
        st_lbl.configure(fg=C["bad"] if bad else C["muted"])

    def bg(fn, ok, busy_msg):
        set_busy(True)
        say(busy_msg)

        def run():
            try:
                res = fn()
            except Exception as e:  # shown on the screen
                msg = str(e) or e.__class__.__name__
                ui.q.put(("call", lambda: (set_busy(False), say(msg, True))))
                return
            ui.q.put(("call", lambda: (set_busy(False), ok(res))))
        import threading
        threading.Thread(target=run, daemon=True).start()

    def make_target():
        path = install_var.get().strip()
        if not path:
            raise tg.TargetError("Choose the install first.")
        if where_var.get() == "folder":
            return tg.FolderTarget(path)
        if not ui.host_var.get().strip():
            raise tg.TargetError("Enter the Xbox address in the Package tab first (Find my Xbox).")
        return tg.FtpTarget(ui.host_var.get().strip(), path, ui.user_var.get(), ui.pass_var.get())

    def gamedir():
        return TITLES[ui.title_var.get()]["gamedir"]

    # ---- state <-> controls ----
    def collect():
        values = {}
        for s in consolecfg.SETTINGS:
            v = vars_[s["key"]].get()
            if s["kind"] == "choice":
                if v != UNSET and v in maps[s["key"]][0]:
                    values[s["key"]] = maps[s["key"]][0][v]
            elif v.strip():
                values[s["key"]] = v.strip()
        startup = {"map": map_var.get().strip(), "cheats": cheats_var.get(), "give_suit": suit_var.get()}
        return values, startup

    def put(values, startup):
        for s in consolecfg.SETTINGS:
            k = s["key"]
            if k not in values:
                vars_[k].set(UNSET if s["kind"] == "choice" else "")
            elif s["kind"] == "choice":
                lab = next((l for l, val in maps[k][0].items() if val == values[k]), None)
                vars_[k].set(lab or UNSET)
            else:
                vars_[k].set(values[k])
        map_var.set(startup.get("map", ""))
        cheats_var.set(bool(startup.get("cheats")))
        suit_var.set(bool(startup.get("give_suit")))

    def show_now(st):
        eff = st["effective"]
        now_text.configure(state="normal")
        now_text.delete("1.0", "end")
        if not eff.value:
            now_text.insert("end", "No setting from this tool's list is set anywhere on the install.\n")
        for k in sorted(eff.value):
            v, src = eff.value[k]
            now_text.insert("end", "%s = %s" % (k, v), "val")
            now_text.insert("end", "   (%s)\n" % src)
        cur_values, cur_startup = collect()
        for n in eff.notes(cur_values, cur_startup):
            now_text.insert("end", "! " + n + "\n", "warn")
        now_text.configure(state="disabled")

    def show_files(files):
        tree.delete(*tree.get_children())
        state["rows"] = {}
        for name, enabled, size in files:
            managed = name == consolecfg.MANAGED_NAME
            iid = tree.insert("", "end", values=("x" if enabled else "", name + ("  (this tool's)" if managed else ""), size))
            state["rows"][iid] = {"name": name, "was": enabled, "now": enabled, "managed": managed}

    def on_click(e):
        if tree.identify_region(e.x, e.y) != "cell" or tree.identify_column(e.x) != "#1":
            return
        iid = tree.identify_row(e.y)
        row = state["rows"].get(iid)
        if not row or row["managed"]:
            return
        row["now"] = not row["now"]
        tree.set(iid, "on", "x" if row["now"] else "")
    tree.bind("<Button-1>", on_click)

    def loaded(st):
        state["loaded"] = st
        put(st["values"], st["startup"])
        show_files(st["files"])
        show_now(st)
        n = len(st["values"]) + sum(1 for k in ("map",) if st["startup"].get(k))
        say("Read. This tool's file sets %d thing(s); %d file(s) in userconfig.d." % (n, len(st["files"])))

    def do_read():
        try:
            t = make_target()
        except tg.TargetError as e:
            say(str(e), True)
            return
        gd = gamedir()

        def work():
            t.open()
            try:
                return tg.load_state(t, gd)
            finally:
                t.close()
        bg(work, loaded, "Reading...")
    read_btn.configure(command=do_read)

    def do_revert():
        if state["loaded"]:
            loaded(state["loaded"])
    revert_btn.configure(command=do_revert)

    def do_write():
        try:
            values, startup = collect()
            text = consolecfg.render(values, startup)
            t = make_target()
        except (consolecfg.ConfigError, tg.TargetError) as e:
            say(str(e), True)
            return
        old = state["loaded"]["managed_text"] if state["loaded"] else None
        toggles = {r["name"]: r["now"] for r in state["rows"].values() if r["now"] != r["was"] and not r["managed"]}
        lines = consolecfg.diff(old, text)
        for name, on in toggles.items():
            lines.append("%s %s" % ("enable" if on else "disable", name))
        if not lines:
            say("Nothing to change.")
            return
        where = "the console" if where_var.get() == "xbox" else install_var.get()
        if not messagebox.askyesno("Xash3D-FWGS-XBOX Packager", "Write to %s?\n\n%s\n\nOnly %s and the files you ticked "
                                   "change; the file it replaces is kept as .before-packager."
                                   % (where, "\n".join(lines[:20]), consolecfg.MANAGED_NAME)):
            return

        gd = gamedir()

        def work():
            t.open()
            try:
                tg.write_settings(t, gd, values, startup, toggles)
                return tg.load_state(t, gd)
            finally:
                t.close()
        bg(work, lambda st: (loaded(st), say("Written. It applies at the next start of the game.")), "Writing...")
    write_btn.configure(command=do_write)

    def do_preset(name):
        values, startup = consolecfg.preset(name)
        put(values, startup)
        say("%s: filled in; Write settings to apply it." % name)
    for name, b in preset_btns.items():
        b.configure(command=lambda n=name: do_preset(n))

    def do_find():
        if where_var.get() == "folder":
            d = filedialog.askdirectory(title="The exported Half-Life folder", mustexist=True)
            if d:
                install_var.set(d.replace("/", "\\") if d[1:2] == ":" else d)
            return
        host = ui.host_var.get().strip()
        if not host:
            say("Press Find my Xbox first (or type its address).", True)
            return
        look(host, "Looking for installs...")
    find_btn.configure(command=do_find)

    def look(host, busy_msg):
        """Games folders of the console and the installs of this title in them; fill the list."""
        user, password, gd = ui.user_var.get(), ui.pass_var.get(), gamedir()

        def work():
            x = xboxftp.Xbox(host, user, password)
            x.connect()
            try:
                folders = x.find_game_folders()
                installs = []
                for path, exists, _big in folders:
                    if exists:
                        installs += x.find_installs(path, needs=gd)
                return folders, installs
            finally:
                x.close()

        def got(res):
            folders, installs = res
            if not ui.rdir_var.get():
                pool = [f for f in folders if f[1] and f[2]] or [f for f in folders if f[1]]
                if pool:
                    ui.rdir_var.set(pool[0][0])
            inst_box["values"] = installs
            if len(installs) == 1:
                install_var.set(installs[0])
                do_read()
            elif installs:
                say("%d installs found: pick one, then Read." % len(installs))
            else:
                say("No %s install found on the console." % TITLES[ui.title_var.get()]["label"], True)
        bg(work, got, busy_msg)

    def do_scan():
        user, password = ui.user_var.get(), ui.pass_var.get()

        def work():
            hosts = xboxftp.scan(user=user, password=password)
            if not hosts:
                raise tg.TargetError("No Xbox found. Check that it is on, on this network, with its FTP server running.")
            return hosts[0]

        def got(host):
            ui.host_var.set(host)
            look(host, "Found an Xbox at %s. Looking for installs..." % host)
        bg(work, got, "Looking for an Xbox on the network...")
    scan_btn.configure(command=do_scan)

    def update_find_label(*_):
        on_pc = where_var.get() == "folder"
        find_btn.configure(text="Browse..." if on_pc else "Find installs")
        host_entry.configure(state="disabled" if on_pc else "normal")
        scan_btn.configure(state="disabled" if on_pc or state["busy"] else "normal")
        addr_lbl.configure(fg=C["faint"] if on_pc else C["text"])
    where_var.trace_add("write", update_find_label)

    def do_edit():
        sel = tree.selection()
        if not sel:
            say("Select a file in the list first.", True)
            return
        row = state["rows"][sel[0]]
        try:
            t = make_target()
        except tg.TargetError as e:
            say(str(e), True)
            return
        name = row["name"]
        rel = "%s/userconfig.d/%s" % (gamedir(), name if row["was"] else name)

        def work():
            t.open()
            try:
                return (t.read(rel) or b"").decode("latin-1")
            finally:
                t.close()

        def show(text):
            ed = tk.Toplevel(tab)
            ed.title(name)
            ed.configure(bg=C["bg"])
            ed.geometry("560x420")
            body = tk.Text(ed, wrap="none", font=("Consolas", 10), bg=C["header"], fg=C["text"],
                           insertbackground=C["text"], bd=0, padx=8, pady=6, undo=True)
            body.pack(fill="both", expand=True, padx=10, pady=(10, 6))
            body.insert("1.0", text.replace("\r\n", "\n"))
            msg = ui.label(ed, "Saving keeps the old file as %s.before-packager." % name, C["muted"])
            msg.configure(bg=C["bg"])
            msg.pack(anchor="w", padx=10)

            def save():
                data = body.get("1.0", "end-1c").replace("\n", "\r\n").encode("latin-1", "replace")

                t2 = make_target()

                def w2():
                    t2.open()
                    try:
                        t2.save_with_backup(rel, data)
                    finally:
                        t2.close()
                ed.destroy()
                bg(w2, lambda _: do_read(), "Saving...")
            ui.button(ed, "Save", save, accent=True, padx=20, pady=6).pack(pady=8)
        bg(work, show, "Reading the file...")
    edit_btn.configure(command=do_edit)
    update_find_label()

    # the bottom parts keep their size; the scrolling settings take what is left
    for w in (bot, ff, now, pr):
        w.pack_forget()
        w.pack(side="bottom", fill="x", pady=(0, 6))
    mid.pack_forget()
    mid.pack(fill="both", expand=True, pady=(0, 6))
    return {"read": do_read, "write": do_write, "preset": do_preset, "vars": vars_, "maps": maps, "where": where_var,
            "install": install_var, "start_map": map_var, "cheats": cheats_var, "tree": tree, "state": state,
            "status": status_var, "now": now_text, "scan": do_scan, "find": do_find, "host": ui.host_var,
            "installs": inst_box, "scan_btn": scan_btn}
