"""Find a Half-Life install: the Windows registry first, then Steam's library folders."""

import os
import re
import sys


def _vdf_paths(text):
    return [m.replace("\\\\", "\\") for m in re.findall(r'"path"\s+"([^"]+)"', text)]


def _steam_roots():
    roots = []
    if sys.platform == "win32":
        import winreg
        for hive, sub, name in (
            (winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam", "SteamPath"),
            (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath"),
            (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Valve\Steam", "InstallPath"),
        ):
            try:
                with winreg.OpenKey(hive, sub) as k:
                    roots.append(winreg.QueryValueEx(k, name)[0])
            except OSError:
                pass
    else:
        home = os.path.expanduser("~")
        roots += [home + "/.steam/steam", home + "/.local/share/Steam",
                  home + "/.var/app/com.valvesoftware.Steam/.local/share/Steam",
                  home + "/Library/Application Support/Steam"]
    return [os.path.normpath(r) for r in roots if r]


def _uninstall_locations(app_id):
    """Windows: 'Steam App <id>' uninstall entries carry the install folder."""
    found = []
    if sys.platform != "win32":
        return found
    import winreg
    for view in (winreg.KEY_WOW64_32KEY, winreg.KEY_WOW64_64KEY):
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                                r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App %d" % app_id,
                                0, winreg.KEY_READ | view) as k:
                found.append(winreg.QueryValueEx(k, "InstallLocation")[0])
        except OSError:
            pass
    return found


def is_install(path):
    return bool(path) and os.path.isfile(os.path.join(path, "valve", "liblist.gam"))


def find_half_life(app_id=70, folder="Half-Life"):
    """Return every Half-Life install found, best first (no duplicates)."""
    cands = list(_uninstall_locations(app_id))
    for root in _steam_roots():
        libs = [root]
        try:
            with open(os.path.join(root, "steamapps", "libraryfolders.vdf"), encoding="utf-8", errors="replace") as f:
                libs += _vdf_paths(f.read())
        except OSError:
            pass
        for lib in libs:
            cands.append(os.path.join(lib, "steamapps", "common", folder))
    out, seen = [], set()
    for c in cands:
        n = os.path.normcase(os.path.normpath(c))
        if n not in seen and is_install(c):
            seen.add(n)
            out.append(os.path.normpath(c))
    return out
