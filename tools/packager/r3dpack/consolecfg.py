"""The console build's settings and the cfg file that carries them.

At boot the engine runs config.cfg (the file the in-game menu writes, kept in
UDATA) and then every userconfig.d/*.cfg of the game folder in name order, so a
later file wins. The settings here are the ones that make sense to choose
from outside the console. They are written to one file of the packager's own,
userconfig.d/50-packager.cfg; no other file is edited, and a setting left
unset is not written, so the in-game menu keeps deciding it.

A file whose name ends .off is not run (FS_Search finds *.cfg only), which is
how the user's own cfgs are switched on and off.
"""

import re

MANAGED_NAME = "50-packager.cfg"
MANAGED_HEADER = ("// Managed by the Xash3D-FWGS-XBOX packager (Console settings). Edit it there;\n"
                  "// a setting the packager does not set is decided by the in-game menu.\n")
LABEL_OK = re.compile(r"^[A-Za-z0-9_-]{0,22}$")  # what host_frametime_label keeps (xbox_ft_label_sanitise)

ON_OFF = [("1", "on"), ("0", "off")]

# Original Xbox pads rest well off centre (the right stick reads -5888..-7424 untouched), and below the dead zone
# the view turns by itself (docs/r3d/divergences.md #104). 8689 is the engine's own Xbox default
# (common/defaults.h). A PC's config.cfg, which an earlier packager copied to the console, sets all four to 0.
DEAD_ZONE_KEYS = ("joy_yaw_deadzone", "joy_pitch_deadzone", "joy_side_deadzone", "joy_forward_deadzone")
DEAD_ZONE_XBOX = "8689"
DRIFT_BELOW = 4096  # a dead zone under the PC default cannot hold an Xbox pad still
DEAD_ZONE_CHOICES = [("0", "0 (none)"), ("2048", "2048"), ("4096", "4096 (the PC's)"), ("6000", "6000"),
                     ("8689", "8689"), ("10000", "10000"), ("12000", "12000"), ("16000", "16000")]

# group -> [setting]. kind: choice | text. "default" is the engine's own, shown as a hint.
SETTINGS = [
    # ---- Display ----
    dict(key="r3d_vid_mode", group="Display", label="Output", kind="choice", default="0",
         choices=[("0", "640x480 (480p where the Dashboard allows)"), ("1", "1280x720 where the Dashboard allows")],
         help="Read at boot. 720p needs the HDTV pack and 720p enabled in the Dashboard."),
    dict(key="r3d_vid_aspect", group="Display", label="Aspect at 640x480", kind="choice", default="0",
         choices=[("0", "as the Dashboard is set"), ("1", "4:3"), ("2", "16:9 (anamorphic)")],
         help="The 640x480 output's aspect."),
    dict(key="nv2a_r_aa", group="Display", label="Anti-aliasing", kind="choice", default="0",
         choices=[("0", "off"), ("1", "2x"), ("2", "Quincunx"), ("3", "4x")],
         help="Costs memory and fill rate, most at 720p."),
    dict(key="nv2a_r_anisotropy", group="Display", label="Anisotropic filtering", kind="choice", default="2",
         choices=[("1", "off (1x)"), ("2", "2x"), ("4", "4x"), ("8", "8x")],
         help="2x is the level proven on hardware; 4x and 8x can be tried."),
    dict(key="nv2a_r_texturenearest", group="Display", label="Nearest-neighbour textures", kind="choice",
         default="0", choices=ON_OFF, help="Point-filters the textures, not the lightmaps."),
    dict(key="nv2a_r_lodbias", group="Display", label="Mip LOD bias", kind="choice", default="0",
         choices=[(v, v) for v in ("-3", "-2", "-1", "0", "1", "2", "3")],
         help="Negative is sharper, and switches to smaller levels later."),
    # ---- Controller ----
    dict(key="joy_yaw_deadzone", group="Controller", label="Look stick, left-right", kind="choice",
         default=DEAD_ZONE_XBOX, choices=DEAD_ZONE_CHOICES,
         help="Below about 8000 an Xbox pad can turn the view by itself. 8689 is the Xbox default."),
    dict(key="joy_pitch_deadzone", group="Controller", label="Look stick, up-down", kind="choice",
         default=DEAD_ZONE_XBOX, choices=DEAD_ZONE_CHOICES, help="The same, for looking up and down."),
    dict(key="joy_side_deadzone", group="Controller", label="Move stick, sideways", kind="choice",
         default=DEAD_ZONE_XBOX, choices=DEAD_ZONE_CHOICES, help="The left stick: strafing."),
    dict(key="joy_forward_deadzone", group="Controller", label="Move stick, forward-back", kind="choice",
         default=DEAD_ZONE_XBOX, choices=DEAD_ZONE_CHOICES, help="The left stick: walking."),
    # ---- Performance and measuring ----
    dict(key="cl_showfps", group="Performance", label="Show FPS", kind="choice", default="0",
         choices=[("0", "off"), ("1", "current"), ("2", "min / current / max")], help="On-screen frames per second."),
    dict(key="host_frametime_analysis", group="Performance", label="Frame-time recorder", kind="choice", default="0",
         choices=[("0", "off"), ("1", "record to the drive"), ("2", "record and draw the overlay")],
         help="Recordings go to frametime/ in the save folder; fetch them with tools/frametime-report.py."),
    dict(key="host_frametime_label", group="Performance", label="Recording label", kind="text", default="",
         help="Names the session in the files, up to 22 of A-Z a-z 0-9 _ -."),
    dict(key="host_frametime_spans", group="Performance", label="Time the CPU spans", kind="choice", default="1",
         choices=ON_OFF, help="With the recorder on: per-subsystem timing (turn off to cut its own cost)."),
    dict(key="r3d_memreport", group="Performance", label="Memory report", kind="choice", default="0",
         choices=[("0", "off"), ("1", "each map start"), ("2", "each map start, in full")],
         help="Where the memory goes, written to memreport.txt."),
    dict(key="r3d_snd_meter", group="Performance", label="Sound level meter", kind="choice", default="0",
         choices=ON_OFF, help="The final mix's level every 2 seconds, to sndmeter.txt."),
    dict(key="net_graph", group="Performance", label="Network graph", kind="choice", default="0",
         choices=[("0", "off"), ("1", "on"), ("2", "on, with more"), ("3", "on, with most")], help=""),
    # ---- Audio ----
    dict(key="r3d_snd_hw", group="Audio", label="Hardware audio (APU)", kind="choice", default="0", choices=ON_OFF,
         help="Read at boot: the mix on the APU, the room effect on the GP, sounds on voices."),
    dict(key="r3d_snd_backend", group="Audio", label="Sound backend", kind="choice", default="sdl",
         choices=[("sdl", "SDL (AC97)"), ("al", "OpenAL (APU)")], help="Read at boot."),
    dict(key="r3d_snd_rate", group="Audio", label="Mixer rate", kind="choice", default="48000",
         choices=[("48000", "48000 Hz"), ("44100", "44100 Hz")], help="Read at boot."),
    dict(key="r3d_snd_adpcm", group="Audio", label="ADPCM sounds", kind="choice", default="0", choices=ON_OFF,
         help="Keeps the map's mono sounds as Xbox ADPCM, about half the memory."),
    dict(key="r3d_speech_volume", group="Audio", label="Speech volume", kind="choice", default="1",
         choices=[(v, v) for v in ("0", "0.25", "0.5", "0.75", "1")], help="On top of the master volume."),
    # ---- Developer ----
    dict(key="r_studio_gpu", group="Developer", label="Models skinned on the GPU", kind="choice", default="1",
         choices=ON_OFF, help="Off falls back to the CPU path.", advanced=True),
    dict(key="r_studio_chrome_gpu", group="Developer", label="Chrome models on the GPU", kind="choice", default="1",
         choices=ON_OFF, help="Off falls back to the CPU path.", advanced=True),
    dict(key="nv2a_r_novis", group="Developer", label="Disable PVS culling", kind="choice", default="0",
         choices=ON_OFF, help="A dev aid.", advanced=True),
    dict(key="nv2a_r_nocull", group="Developer", label="Disable frustum culling", kind="choice", default="0",
         choices=ON_OFF, help="A dev aid.", advanced=True),
    dict(key="nv2a_r_lockpvs", group="Developer", label="Freeze the PVS", kind="choice", default="0",
         choices=ON_OFF, help="A dev aid.", advanced=True),
]

BY_KEY = {s["key"]: s for s in SETTINGS}

# Startup is not a cvar: it is lines of the file. Keys of the dict `startup`.
STARTUP_KEYS = ("map", "cheats", "give_suit")

# Presets set these and clear the rest of the Display/Performance choices they do not name.
PRESETS = {
    "Sharp": {"r3d_vid_mode": "1", "nv2a_r_anisotropy": "8"},
    "Measure": {"host_frametime_analysis": "1", "host_frametime_label": "test", "r3d_memreport": "2"},
    # for a console whose saved config.cfg has the PC's zero dead zones: userconfig.d runs after it
    "Fix stick drift": {k: DEAD_ZONE_XBOX for k in DEAD_ZONE_KEYS},
    "Defaults": {},
}

_LINE = re.compile(r'^\s*(?:seta?\s+)?([A-Za-z0-9_]+)\s+(?:"([^"]*)"|(\S+))')


class ConfigError(Exception):
    pass


def _number(text):
    try:
        return float(text)
    except (TypeError, ValueError):
        return float("inf")  # not a number: not this module's to call drifting


def _strip_comment(line):
    i = line.find("//")
    return line if i < 0 else line[:i]


def parse_assignments(text):
    """[(key, value)] in file order, for the keys this module knows and for `map`.

    `key "value"`, `key value` and `set key value` are all read; bind, wait and the like are not."""
    out = []
    for raw in text.replace("\r", "").split("\n"):
        line = _strip_comment(raw)
        m = _LINE.match(line)
        if not m:
            continue
        key = m.group(1)
        val = m.group(2) if m.group(2) is not None else m.group(3)
        if key in BY_KEY or key in ("map", "sv_cheats"):
            out.append((key, val))
    return out


def validate(values, startup):
    for k, v in values.items():
        s = BY_KEY.get(k)
        if s is None:
            raise ConfigError("unknown setting %s" % k)
        if s["kind"] == "choice" and v not in [c for c, _ in s["choices"]]:
            raise ConfigError("%s: %r is not one of %s" % (k, v, ", ".join(c for c, _ in s["choices"])))
    lab = values.get("host_frametime_label")
    if lab is not None and not LABEL_OK.match(lab):
        raise ConfigError("the recording label may use A-Z a-z 0-9 _ - and up to 22 characters")
    m = startup.get("map", "")
    if m and not re.match(r"^[A-Za-z0-9_]+$", m):
        raise ConfigError("the start map is a map name like c0a0")
    if startup.get("give_suit") and not startup.get("cheats"):
        raise ConfigError("giving the suit needs cheats on")


def render(values, startup=None):
    """The text of 50-packager.cfg for the chosen settings. Unset ones are not written."""
    startup = startup or {}
    validate(values, startup)
    lines = [MANAGED_HEADER.rstrip("\n")]
    for s in SETTINGS:
        if s["key"] in values:
            lines.append('%s "%s"' % (s["key"], values[s["key"]]))
    if startup.get("cheats"):
        lines.append("wait 200")
        lines.append("sv_cheats 1")
    if startup.get("map"):
        lines.append("map %s" % startup["map"])
        if startup.get("give_suit"):
            lines += ["wait 300", "give item_suit", "impulse 101"]
    return "\r\n".join(lines) + "\r\n"


def read_managed(text):
    """(values, startup) from an earlier 50-packager.cfg."""
    values, startup = {}, {}
    for k, v in parse_assignments(text):
        if k == "map":
            startup["map"] = v
        elif k == "sv_cheats":
            startup["cheats"] = v == "1"
        elif k in BY_KEY:
            values[k] = v
    if "give item_suit" in text:
        startup["give_suit"] = True
    return values, startup


def preset(name):
    """(values, startup) for a preset name."""
    if name not in PRESETS:
        raise ConfigError("no preset %s" % name)
    return dict(PRESETS[name]), {}


class Effective:
    """What the console will end up with, and which file said so.

    files: [(name, text)] in the order the engine runs them: config.cfg first, then
    userconfig.d/*.cfg by name. Switched-off files (name ends .off) are left out here."""

    def __init__(self, files):
        self.value = {}      # key -> (value, source)
        self.sets = {}       # key -> [(source, value)] in run order
        self.loads_map = []  # [(source, map)] files that load a map on every start
        for name, text in files:
            for k, v in parse_assignments(text):
                if k == "map":
                    self.loads_map.append((name, v))
                elif k in BY_KEY:
                    self.value[k] = (v, name)
                    self.sets.setdefault(k, []).append((name, v))

    def overridden(self, managed_values):
        """[(key, mine, other_file, other_value)] where a file that runs after ours sets the key again."""
        out = []
        for k, mine in managed_values.items():
            seq = self.sets.get(k, [])
            idx = [i for i, (src, _) in enumerate(seq) if src == MANAGED_NAME]
            if not idx:
                continue
            for src, v in seq[idx[-1] + 1:]:
                out.append((k, mine, src, v))
        return out

    def drift(self, managed_values):
        """[(key, value, source)] of dead zones too small for an Xbox pad that nothing here is about to fix."""
        out = []
        for k in DEAD_ZONE_KEYS:
            if k not in self.value:
                continue
            v, src = self.value[k]
            mine = managed_values.get(k)
            if mine is not None and _number(mine) >= DRIFT_BELOW:
                continue  # the settings on screen replace it
            if _number(v) < DRIFT_BELOW:
                out.append((k, v, src))
        return out

    def notes(self, managed_values, managed_startup):
        """Short warnings for the screen."""
        out = []
        low = self.drift(managed_values)
        if low:
            out.append("%s: too small for an Xbox pad, which rests off centre, so the view can turn by itself "
                       "(a PC's config.cfg sets them to 0). The 'Fix stick drift' preset sets them to %s."
                       % (", ".join("%s is %s (%s)" % x for x in low), DEAD_ZONE_XBOX))
        for k, mine, src, v in self.overridden(managed_values):
            out.append("%s: %s sets it to %s after this tool does, so %s wins." % (k, src, v, v))
        others = [(s, m) for s, m in self.loads_map if s != MANAGED_NAME]
        for src, m in others:
            out.append("%s loads map %s on every start." % (src, m))
        if managed_startup.get("map") and others:
            out.append("Two files load a map; the one that runs last wins.")
        return out


def diff(old_text, new_text):
    """A short human summary of what changes between two managed files."""
    def asg(t):
        return dict(parse_assignments(t or "")) if t else {}
    o, n = asg(old_text), asg(new_text)
    lines = []
    for k in sorted(set(o) | set(n)):
        if k not in o:
            lines.append("+ %s %s" % (k, n[k]))
        elif k not in n:
            lines.append("- %s (was %s)" % (k, o[k]))
        elif o[k] != n[k]:
            lines.append("~ %s %s -> %s" % (k, o[k], n[k]))
    return lines
