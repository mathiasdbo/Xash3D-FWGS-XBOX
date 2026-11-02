"""The titles the packager knows. Same table as tools/xbox-xiso.sh's title_row."""

TITLES = {
    "hl": {"label": "Half-Life", "folder": "Half-Life", "gamedir": "valve", "basedir": "valve",
           "steam_app": 70, "steam_dir": "Half-Life", "enabled": True},
    "op4": {"label": "Half-Life: Opposing Force", "folder": "Opposing Force", "gamedir": "gearbox",
            "basedir": "valve", "steam_app": 50, "steam_dir": "Half-Life", "enabled": False},
    "bshift": {"label": "Half-Life: Blue Shift", "folder": "Blue Shift", "gamedir": "bshift",
               "basedir": "valve", "steam_app": 130, "steam_dir": "Half-Life", "enabled": False},
}
