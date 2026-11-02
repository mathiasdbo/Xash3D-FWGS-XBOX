#!/usr/bin/env python3
"""Resonance3D packager: your Half-Life files + the Xbox build -> a folder or XISO.

    python r3d_packager.py                      window
    python r3d_packager.py --cli --title hl --steam <Half-Life folder> \
        --format folder|xiso --out <destination folder>

Your own copy of the game is the only source of the game's content; nothing
of it ships with this tool.
"""

import argparse
import os
import queue
import sys
import threading
import types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from r3dpack import bake, payload, settings_tab, stage, steam, xboxftp  # noqa: E402
from r3dpack.inspect_install import inspect  # noqa: E402
from r3dpack.titles import TITLES  # noqa: E402


def resource(*parts):
    base = getattr(sys, "_MEIPASS", HERE)
    return os.path.join(base, *parts)


def upload(files, title, host, remote_base, user, password, progress, log, confirm=None):
    """confirm(text) -> bool is asked before anything already on the console is replaced.
    Without one, an upload that would replace something stops and changes nothing."""
    x = xboxftp.Xbox(host, user, password)
    x.connect()
    try:
        target = remote_base.rstrip("/") + "/" + TITLES[title]["folder"]
        where = "ftp://%s%s" % (host, target)
        plan = x.survey(files, target)
        log(plan.describe(where))
        if plan.needs_confirmation:
            if confirm is None:
                raise xboxftp.UploadError(
                    "%s\nNothing was changed. Run it again with --overwrite to go ahead." % plan.describe(where))
            if not confirm(plan.describe(where) + "\n\nReplace them?"):
                raise xboxftp.UploadError("Cancelled; nothing on the console was changed.")
        log("Uploading to %s" % where)
        sent, skipped = x.upload(files, target, progress, plan=plan)
        log("Uploaded %d files, %d already there." % (sent, skipped))
        gone = x.remove_stale_bsp(files, target)
        if gone:
            log("Removed %d loose .bsp files left by an earlier upload; their maps are in the baked archives." % len(gone))
        return "ftp://%s%s" % (host, target)
    finally:
        x.close()


def run_build(steam_root, title, fmt, out_dir, payload_dir=None, progress=None, log=print, ftp=None, bake_opts=None):
    """bake_opts: None bakes with the defaults; {"enabled": False} skips the Xbox data (the world
    then draws black on the console); {"baker": path, "cache": dir} override where things are."""
    opts = bake_opts or {}
    baked = []
    share = 0.0
    if opts.get("enabled", True):
        share = 0.3  # the bake is the first 30% of the bar, the export the rest
        baked = bake.bake(steam_root, TITLES[title]["gamedir"], opts.get("baker"), opts.get("cache"),
                          (lambda d, t, n: progress(share * d / max(t, 1), 1.0, n)) if progress else None, log)
    else:
        log("Skipping the Xbox data bake: the world will not draw on the console.")
    if progress:
        inner = progress
        progress = lambda d, t, n: inner(share + (1 - share) * d / max(t, 1), 1.0, n)  # noqa: E731
    files, build = stage.plan(steam_root, title, payload_dir, log, baked)
    log("Build: %s %s%s" % (build.get("branch", "?"), (build.get("commit") or "?")[:10],
                            "" if build.get("clean", True) else " (not a clean tree)"))
    log("%d files, %.0f MiB" % (len(files), sum(s for _, _, s in files) / 1048576.0))
    if fmt == "ftp":
        return upload(files, title, ftp["host"], ftp["dir"], ftp.get("user", xboxftp.DEFAULT_USER),
                      ftp.get("password", xboxftp.DEFAULT_PASS), progress, log, ftp.get("confirm"))
    if fmt == "folder":
        return stage.export_folder(files, out_dir, title, progress, log)
    return stage.export_xiso(files, out_dir, title, progress, log)


def cli(a):
    root = a.steam
    if not root:
        found = steam.find_half_life()
        if not found:
            print("No Half-Life install found; pass --steam <folder>.", file=sys.stderr)
            return 2
        root = found[0]
        print("Using %s" % root)
    last = [-1]

    def prog(done, total, name):
        pct = int(100 * done / max(total, 1))
        if pct != last[0] and pct % 5 == 0:
            last[0] = pct
            print("  %3d%%" % pct)

    ftp = None
    fmt = a.format
    if a.upload:
        fmt = "ftp"
        host = a.upload
        if host == "auto":
            hosts = xboxftp.scan()
            if not hosts:
                print("No Xbox found on the network; pass --upload <ip>.", file=sys.stderr)
                return 2
            host = hosts[0]
            print("Found an Xbox at %s" % host)
        rdir = a.remote_dir
        if not rdir:
            x = xboxftp.Xbox(host, a.ftp_user, a.ftp_password)
            x.connect()
            cands = x.find_game_folders()
            x.close()
            big = [c for c in cands if c[2]]
            pool = big or cands
            if len(pool) != 1:
                print("Pick where the game goes with --remote-dir. The console offers:", file=sys.stderr)
                for path, exists, is_big in cands:
                    print("  %s%s%s" % (path, "" if exists else "  (would be created)",
                                        "  (big drive)" if is_big else ""), file=sys.stderr)
                return 2
            rdir = pool[0][0]
            print("Using %s" % rdir)
        ftp = {"host": host, "dir": rdir, "user": a.ftp_user, "password": a.ftp_password,
               "confirm": (lambda text: True) if a.overwrite else None}
    try:
        run_build(root, a.title, fmt, a.out, a.payload, prog, ftp=ftp,
                  bake_opts={"enabled": not a.no_bake, "baker": a.baker, "cache": a.bake_cache})
    except (stage.BuildError, payload.PayloadError, xboxftp.UploadError, bake.BakeError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    return 0


PALETTE = {
    "bg": "#18181b",        # window
    "header": "#121214",    # banner, log
    "card": "#202024",      # cards
    "field": "#2a2a30",     # entries
    "button": "#3f3f46",
    "button_hover": "#52525b",
    "line": "#333338",
    "text": "#f4f4f5",
    "muted": "#a1a1aa",
    "faint": "#71717a",
    "accent": "#f59e0b",    # amber
    "accent_hover": "#fbbf24",
    "accent_down": "#d97706",
    "ok": "#10b981",
    "bad": "#ef4444",
}

BUILD_LABEL = {"folder": "Build the folder", "xiso": "Build the XISO", "ftp": "Send to the Xbox"}


def gui():
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    C = PALETTE
    win = tk.Tk()
    win.title("Resonance3D Packager")
    win.geometry("800x860")
    win.minsize(740, 720)
    win.configure(bg=C["bg"])
    try:
        ico = resource("assets", "icon-xash.png")
        if os.path.exists(ico):
            win.iconphoto(True, tk.PhotoImage(file=ico))
    except tk.TclError:
        pass

    # ---- theme ----
    st = ttk.Style(win)
    st.theme_use("clam")
    st.configure(".", background=C["card"], foreground=C["text"], font=("Segoe UI", 9))
    st.configure("TFrame", background=C["card"])
    st.configure("Dark.TRadiobutton", background=C["card"], foreground=C["text"], focuscolor=C["card"],
                 indicatorbackground=C["field"], indicatorforeground=C["accent"])
    st.map("Dark.TRadiobutton", background=[("active", C["card"])], foreground=[("active", C["accent"])],
           indicatorbackground=[("selected", C["field"])])
    st.configure("Dark.TCombobox", fieldbackground=C["field"], background=C["button"], foreground=C["text"],
                 arrowcolor=C["text"], bordercolor=C["line"], lightcolor=C["field"], darkcolor=C["field"],
                 selectbackground=C["field"], selectforeground=C["text"], padding=4)
    st.map("Dark.TCombobox", fieldbackground=[("readonly", C["field"]), ("disabled", C["bg"])],
           foreground=[("disabled", C["faint"])], background=[("active", C["button_hover"])],
           bordercolor=[("focus", C["accent"])])
    st.configure("Amber.Horizontal.TProgressbar", troughcolor="#2a2a34", background=C["accent"],
                 bordercolor="#2a2a34", lightcolor=C["accent"], darkcolor=C["accent_down"], thickness=14)
    st.configure("Dark.TNotebook", background=C["bg"], borderwidth=1, bordercolor=C["line"],
                 lightcolor=C["line"], darkcolor=C["line"], tabmargins=(0, 0, 0, 0))
    st.configure("Dark.TNotebook.Tab", background=C["card"], foreground=C["muted"], padding=(16, 7),
                 borderwidth=1, bordercolor=C["line"], lightcolor=C["card"], darkcolor=C["card"])
    st.map("Dark.TNotebook.Tab", background=[("selected", C["bg"])], foreground=[("selected", C["accent"])],
           lightcolor=[("selected", C["bg"])], darkcolor=[("selected", C["bg"])])
    for name, bgc in (("Dark.TCheckbutton", C["card"]), ("Bg.TCheckbutton", C["bg"])):
        st.configure(name, background=bgc, foreground=C["text"], focuscolor=bgc, indicatorbackground=C["field"],
                     indicatorforeground=C["accent"])
        st.map(name, background=[("active", bgc)], foreground=[("active", C["accent"])])
    st.configure("Treeview", background=C["field"], fieldbackground=C["field"], foreground=C["text"],
                 bordercolor=C["line"], lightcolor=C["line"], darkcolor=C["line"], rowheight=22)
    st.map("Treeview", background=[("selected", C["accent"])], foreground=[("selected", C["bg"])])
    st.configure("Treeview.Heading", background=C["button"], foreground=C["text"], relief="flat")
    win.option_add("*TCombobox*Listbox.background", C["field"])
    win.option_add("*TCombobox*Listbox.foreground", C["text"])
    win.option_add("*TCombobox*Listbox.selectBackground", C["accent"])
    win.option_add("*TCombobox*Listbox.selectForeground", C["bg"])

    def label(parent, text="", fg=None, font=("Segoe UI", 9), **kw):
        return tk.Label(parent, text=text, fg=fg or C["text"], bg=parent.cget("bg"), font=font, **kw)

    def entry(parent, var, **kw):
        return tk.Entry(parent, textvariable=var, font=("Segoe UI", 9), bg=C["field"], fg=C["text"],
                        insertbackground=C["text"], disabledbackground=C["bg"], disabledforeground=C["faint"],
                        relief="flat", highlightthickness=1, highlightbackground=C["line"],
                        highlightcolor=C["accent"], **kw)

    def button(parent, text, command=None, accent=False, **kw):
        base = C["accent"] if accent else C["button"]
        over = C["accent_hover"] if accent else C["button_hover"]
        b = tk.Button(parent, text=text, command=command, font=("Segoe UI", 11 if accent else 9, "bold" if accent else "normal"),
                      bg=base, fg=C["bg"] if accent else C["text"], activebackground=C["accent_down"] if accent else over,
                      activeforeground=C["bg"] if accent else C["text"], disabledforeground=C["faint"],
                      relief="flat", bd=0, cursor="hand2", **kw)
        b.bind("<Enter>", lambda e: b.cget("state") == "normal" and b.configure(bg=over))
        b.bind("<Leave>", lambda e: b.configure(bg=base if b.cget("state") == "normal" else C["card"] if not accent else C["button"]))
        return b

    def card(parent, title):
        f = tk.LabelFrame(parent, text=" %s " % title, font=("Segoe UI", 9, "bold"), fg=C["accent"], bg=C["card"],
                          bd=1, relief="solid", highlightthickness=0, padx=14, pady=10)
        f.pack(fill="x", pady=(0, 10))
        return f

    # ---- header ----
    head = tk.Frame(win, bg=C["header"], padx=20, pady=12)
    head.pack(fill="x")
    label(head, "RESONANCE3D  PACKAGER", C["accent"], ("Segoe UI", 15, "bold"), anchor="w").pack(anchor="w")
    label(head, "Half-Life for the original Xbox, built from your own copy of the game", C["muted"], anchor="w").pack(anchor="w")

    nb = ttk.Notebook(win, style="Dark.TNotebook")
    nb.pack(fill="both", expand=True, padx=14, pady=(10, 12))
    main = tk.Frame(nb, bg=C["bg"], padx=10, pady=10)
    stab = tk.Frame(nb, bg=C["bg"], padx=10, pady=10)
    nb.add(main, text="  Package  ")
    nb.add(stab, text="  Console settings  ")

    title_var = tk.StringVar(value="hl")
    root_var = tk.StringVar()
    fmt_var = tk.StringVar(value="folder")
    out_var = tk.StringVar(value=os.path.expanduser("~"))
    host_var = tk.StringVar()
    user_var = tk.StringVar(value=xboxftp.DEFAULT_USER)
    pass_var = tk.StringVar(value=xboxftp.DEFAULT_PASS)
    rdir_var = tk.StringVar()

    # ---- 1. game ----
    c1 = card(main, "1. Game")
    c1.columnconfigure(1, weight=1)
    label(c1, "Game").grid(row=0, column=0, sticky="w", pady=3)
    names = [k for k, v in TITLES.items()]
    box = ttk.Combobox(c1, state="readonly", style="Dark.TCombobox",
                       values=[TITLES[k]["label"] + ("" if TITLES[k]["enabled"] else "  (coming soon)") for k in names])
    box.current(0)
    box.grid(row=0, column=1, columnspan=2, sticky="ew", padx=(10, 0), pady=3)

    def picked(_=None):
        k = names[box.current()]
        if not TITLES[k]["enabled"]:
            box.current(names.index(title_var.get()))
            return
        title_var.set(k)
        autodetect()
    box.bind("<<ComboboxSelected>>", picked)

    label(c1, "Half-Life folder").grid(row=1, column=0, sticky="w", pady=3)
    entry(c1, root_var).grid(row=1, column=1, sticky="ew", padx=10, pady=3, ipady=4)
    button(c1, "Browse...", lambda: browse_root(), padx=14, pady=4).grid(row=1, column=2, pady=3)
    status = label(c1, "", C["muted"], wraplength=640, justify="left", anchor="w")
    status.grid(row=2, column=0, columnspan=3, sticky="w", pady=(4, 0))

    # ---- 2. export ----
    c2 = card(main, "2. Export")
    fm = tk.Frame(c2, bg=C["card"])
    fm.pack(fill="x")
    for text, val in (("Folder (copy to the console by FTP)", "folder"), ("XISO disc image", "xiso"),
                      ("Send to the Xbox over FTP", "ftp")):
        ttk.Radiobutton(fm, text=text, variable=fmt_var, value=val, style="Dark.TRadiobutton").pack(anchor="w", pady=2)
    out_row = tk.Frame(c2, bg=C["card"])
    out_row.pack(fill="x", pady=(8, 0))
    out_row.columnconfigure(1, weight=1)
    out_lbl = label(out_row, "Save in")
    out_lbl.grid(row=0, column=0, sticky="w")
    out_entry = entry(out_row, out_var)
    out_entry.grid(row=0, column=1, sticky="ew", padx=10, ipady=4)
    out_btn = button(out_row, "Browse...", lambda: browse_out(), padx=14, pady=4)
    out_btn.grid(row=0, column=2)

    # ---- 3. xbox ----
    c3 = card(main, "3. Xbox (FTP)")
    c3.columnconfigure(1, weight=1)
    a_lbl = label(c3, "Address")
    a_lbl.grid(row=0, column=0, sticky="w", pady=3)
    host_entry = entry(c3, host_var)
    host_entry.grid(row=0, column=1, sticky="ew", padx=10, pady=3, ipady=4)
    scan_btn = button(c3, "Find my Xbox", padx=12, pady=4)
    scan_btn.grid(row=0, column=2, pady=3)
    l_lbl = label(c3, "Login")
    l_lbl.grid(row=1, column=0, sticky="w", pady=3)
    lf = tk.Frame(c3, bg=C["card"])
    lf.grid(row=1, column=1, sticky="w", padx=10, pady=3)
    user_entry = entry(lf, user_var, width=14)
    user_entry.pack(side="left", ipady=4)
    pass_entry = entry(lf, pass_var, width=14, show="*")
    pass_entry.pack(side="left", padx=8, ipady=4)
    g_lbl = label(c3, "Games folder")
    g_lbl.grid(row=2, column=0, sticky="w", pady=3)
    rdir_box = ttk.Combobox(c3, textvariable=rdir_var, style="Dark.TCombobox")
    rdir_box.grid(row=2, column=1, sticky="ew", padx=10, pady=3)
    detect_btn = button(c3, "Look for it", padx=12, pady=4)
    detect_btn.grid(row=2, column=2, pady=3)

    # ---- build ----
    bar_row = tk.Frame(main, bg=C["bg"])
    bar_row.pack(fill="x", pady=(0, 10))
    build_btn = button(bar_row, BUILD_LABEL["folder"], accent=True, padx=24, pady=8)
    build_btn.pack(side="left")

    # ---- progress and log ----
    c4 = tk.LabelFrame(main, text=" Progress & Log ", font=("Segoe UI", 9, "bold"), fg=C["accent"], bg=C["card"],
                       bd=1, relief="solid", padx=14, pady=8)
    c4.pack(fill="both", expand=True)
    pr = tk.Frame(c4, bg=C["card"])
    pr.pack(fill="x", pady=(0, 4))
    state_lbl = label(pr, "Ready.", C["text"], ("Segoe UI", 9, "bold"), anchor="w")
    state_lbl.pack(side="left", fill="x", expand=True)
    pct_lbl = label(pr, "0%", C["accent"], ("Segoe UI", 9, "bold"))
    pct_lbl.pack(side="right")
    bar = ttk.Progressbar(c4, maximum=100, style="Amber.Horizontal.TProgressbar")
    bar.pack(fill="x", pady=(0, 6))
    lg = tk.Frame(c4, bg=C["header"], bd=1, relief="solid")
    lg.pack(fill="both", expand=True)
    logbox = tk.Text(lg, height=8, state="disabled", wrap="word", font=("Consolas", 9), bg=C["header"], fg=C["muted"],
                     bd=0, padx=8, pady=6, insertbackground=C["text"])
    sb = tk.Scrollbar(lg, command=logbox.yview, bg=C["button"], troughcolor=C["header"], relief="flat", bd=0)
    logbox.configure(yscrollcommand=sb.set)
    sb.pack(side="right", fill="y")
    logbox.pack(side="left", fill="both", expand=True)
    logbox.tag_configure("ok", foreground=C["ok"])
    logbox.tag_configure("bad", foreground=C["bad"])

    q = queue.Queue()
    busy = {"scan": False, "detect": False, "build": False}
    valid = [False]

    def log(msg):
        q.put(("log", msg))

    def show_log(msg):
        tag = "ok" if msg.startswith(("Done", "Found", "Games go")) else "bad" if msg.startswith("Failed") else ""
        logbox.configure(state="normal")
        logbox.insert("end", msg + "\n", tag)
        logbox.see("end")
        logbox.configure(state="disabled")

    def setw(w, on):
        if isinstance(w, ttk.Widget):
            w.state(["!disabled" if on else "disabled"])
        else:
            w.configure(state="normal" if on else "disabled")
            if isinstance(w, tk.Button):
                w.configure(bg=C["button"] if w is not build_btn else C["accent"])

    def apply_mode(*_):
        ftp = fmt_var.get() == "ftp"
        for w in (host_entry, user_entry, pass_entry, rdir_box):
            setw(w, ftp)
        setw(scan_btn, ftp and not busy["scan"])
        setw(detect_btn, ftp and not busy["detect"])
        for w in (a_lbl, l_lbl, g_lbl):
            w.configure(fg=C["text"] if ftp else C["faint"])
        c3.configure(fg=C["accent"] if ftp else C["faint"])
        for w in (out_entry, out_btn):
            setw(w, not ftp)
        out_lbl.configure(fg=C["faint"] if ftp else C["text"])
        build_btn.configure(text=BUILD_LABEL[fmt_var.get()])
        setw(build_btn, valid[0] and not busy["build"])
        if not (valid[0] and not busy["build"]):
            build_btn.configure(bg=C["button"])
    fmt_var.trace_add("write", apply_mode)

    def check(*_):
        info = inspect(root_var.get(), title_var.get()) if root_var.get() else None
        if info is None:
            valid[0] = False
            status.configure(text="Pick your Half-Life install folder (the one that contains 'valve').", fg=C["muted"])
        elif info["ok"]:
            valid[0] = True
            status.configure(text="Found: %d files, %.0f MiB.%s" % (
                info["files"], info["size"] / 1048576.0, "".join("\n" + n for n in info["notes"])), fg=C["ok"])
        else:
            valid[0] = False
            status.configure(text="\n".join(info["problems"]), fg=C["bad"])
        apply_mode()
    root_var.trace_add("write", check)

    def autodetect():
        t = TITLES[title_var.get()]
        found = steam.find_half_life(t["steam_app"], t["steam_dir"])
        if found:
            root_var.set(found[0])
        else:
            check()

    def browse_root():
        d = filedialog.askdirectory(title="Half-Life install folder", mustexist=True)
        if d:
            root_var.set(os.path.normpath(d))

    def browse_out():
        d = filedialog.askdirectory(title="Where to save")
        if d:
            out_var.set(os.path.normpath(d))

    def do_scan():
        busy["scan"] = True
        apply_mode()
        log("Looking for an Xbox on the network...")

        def go():
            hosts = xboxftp.scan(user=user_var.get(), password=pass_var.get())
            q.put(("hosts", hosts))
        threading.Thread(target=go, daemon=True).start()
    scan_btn.configure(command=do_scan)

    def do_detect():
        busy["detect"] = True
        apply_mode()

        def go():
            try:
                x = xboxftp.Xbox(host_var.get(), user_var.get(), pass_var.get())
                x.connect()
                found = x.find_game_folders()
                x.close()
                q.put(("dirs", found))
            except xboxftp.UploadError as e:
                q.put(("dirs_fail", str(e)))
        threading.Thread(target=go, daemon=True).start()
    detect_btn.configure(command=do_detect)

    def ask(text):
        """Called from the worker thread: the dialog runs on the window's thread."""
        resp = queue.Queue()
        q.put(("ask", (text, resp)))
        return resp.get()

    def work(root, title, fmt, out, ftp):
        try:
            res = run_build(root, title, fmt, out, None, lambda d, t, n: q.put(("p", 100.0 * d / max(t, 1))), log, ftp)
            q.put(("done", res))
        except Exception as e:  # shown to the user; the log has the context
            q.put(("fail", str(e)))

    def start():
        ftp = None
        if fmt_var.get() == "ftp":
            if not host_var.get() or not rdir_var.get():
                messagebox.showerror("Resonance3D Packager", "Enter the Xbox address and the Games folder first.")
                return
            ftp = {"host": host_var.get(), "dir": rdir_var.get(), "user": user_var.get(), "password": pass_var.get(),
                   "confirm": ask}
        busy["build"] = True
        apply_mode()
        bar["value"] = 0
        pct_lbl.configure(text="0%")
        state_lbl.configure(text="Working...")
        threading.Thread(target=work, daemon=True,
                         args=(root_var.get(), title_var.get(), fmt_var.get(), out_var.get(), ftp)).start()

    build_btn.configure(command=start)

    def pump():
        try:
            while True:
                kind, val = q.get_nowait()
                if kind == "log":
                    show_log(val)
                elif kind == "p":
                    bar["value"] = val
                    pct_lbl.configure(text="%d%%" % int(val))
                elif kind == "call":
                    val()
                elif kind == "ask":
                    text, resp = val
                    resp.put(messagebox.askyesno("Resonance3D Packager", text, icon="warning", default="no"))
                elif kind == "hosts":
                    busy["scan"] = False
                    apply_mode()
                    if val:
                        host_var.set(val[0])
                        show_log("Found an Xbox at %s" % val[0])
                        do_detect()
                    else:
                        show_log("No Xbox found. Check that it is on, on the same network, with its FTP server running.")
                elif kind == "dirs":
                    busy["detect"] = False
                    apply_mode()
                    rdir_box["values"] = [p for p, _, _ in val]
                    big = [c for c in val if c[2]]
                    pool = big or val
                    if len(pool) == 1:
                        rdir_var.set(pool[0][0])
                        show_log("Games go to %s%s." % (pool[0][0], "" if pool[0][1] else " (created for you)"))
                    elif val:
                        rdir_var.set("")
                        show_log("The console has several drives for games: %s. Pick one in the Games folder box."
                                 % ", ".join("%s%s" % (p, "" if e else " (new)") for p, e, _ in val))
                    else:
                        show_log("No drive for games found on the console; type the folder to use.")
                elif kind == "dirs_fail":
                    busy["detect"] = False
                    apply_mode()
                    show_log(val)
                elif kind == "done":
                    busy["build"] = False
                    bar["value"] = 100
                    pct_lbl.configure(text="100%")
                    state_lbl.configure(text="Done.")
                    show_log("Done: %s" % val)
                    apply_mode()
                    messagebox.showinfo("Resonance3D Packager", "Done:\n%s" % val)
                elif kind == "fail":
                    busy["build"] = False
                    state_lbl.configure(text="Failed.")
                    show_log("Failed: %s" % val)
                    apply_mode()
                    messagebox.showerror("Resonance3D Packager", val)
        except queue.Empty:
            pass
        win.after(100, pump)

    settings_tab.build(stab, types.SimpleNamespace(
        C=C, label=label, entry=entry, button=button, card=card, q=q, host_var=host_var, user_var=user_var,
        pass_var=pass_var, rdir_var=rdir_var, title_var=title_var))
    autodetect()
    check()
    pump()
    win.mainloop()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cli", action="store_true")
    ap.add_argument("--title", default="hl", choices=[k for k, v in TITLES.items() if v["enabled"]])
    ap.add_argument("--steam")
    ap.add_argument("--format", default="folder", choices=["folder", "xiso"])
    ap.add_argument("--out", default=".")
    ap.add_argument("--payload")
    ap.add_argument("--baker", help="the asset baker executable (default: the one next to this tool)")
    ap.add_argument("--bake-cache", help="where baked data is kept between runs")
    ap.add_argument("--no-bake", action="store_true", help="skip the Xbox data bake (the world will not draw)")
    ap.add_argument("--upload", metavar="IP|auto", help="send it to an Xbox over FTP instead of writing a file")
    ap.add_argument("--remote-dir", help="games folder on the console (default: the first 'Games' folder found)")
    ap.add_argument("--overwrite", action="store_true",
                    help="replace what is already on the console without asking (the old default.xbe is kept "
                         "as default.xbe.before-packager; your config.cfg and userconfig.d stay)")
    ap.add_argument("--ftp-user", default=xboxftp.DEFAULT_USER)
    ap.add_argument("--ftp-password", default=xboxftp.DEFAULT_PASS)
    a = ap.parse_args()
    if a.cli:
        sys.exit(cli(a))
    gui()


if __name__ == "__main__":
    main()
