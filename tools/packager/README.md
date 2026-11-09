# Xash3D-FWGS-XBOX Packager

Packs **your own** Half-Life files together with the Xash3D-FWGS-XBOX Xbox build
into something the console can run: a `Half-Life` folder, or an XISO disc
image. Nothing of the game ships with this tool.

## Using it

Windows: run `Xash3D-FWGS-XBOX-Packager-<version>-windows.exe` from the
[release](../../../../releases).

Anywhere else: unzip `Xash3D-FWGS-XBOX-Packager-<version>-portable.zip` and run

    python3 r3d_packager.py

(Python 3 with Tk; on Debian or Ubuntu `sudo apt install python3-tk`.)

1. Pick the game. Half-Life is available; Opposing Force and Blue Shift are
   listed and will follow.
2. The Half-Life folder is found automatically on Windows (the Steam
   registry entries, then every Steam library folder). If it is not, browse
   for the folder that contains `valve`. The window says what it found, or
   what is missing: the engine needs `delta.lst` (Half-Life 1.1.0.0 or later)
   and `resource/gameui_english.txt`.
3. Choose **Folder** (copy it to the console by FTP, into the games folder)
   or **XISO**, and where to save.
4. Build.

Command line, no window:

    python3 r3d_packager.py --cli --steam "<Half-Life folder>" --format folder|xiso --out <folder>

## What goes in the output

Your `valve` folder as it is, plus the Xbox build and what it expects beside
it: `default.xbe` (titled "Half-Life"), the menu font, the menu colours and
button art, and the menu translations. `gameinfo.txt` gets the animated title
and HD background flags that `liblist.gam` sets and the Steam copy omits.
These are the same steps as `tools/xbox-xiso.sh`; the packager's own XISO
writer needs no nxdk (checked against `extract-xiso`, byte for byte).

## Console settings

The second tab reads and changes the settings an install runs with: the video
output (480p or 720p), aspect, anti-aliasing, anisotropic filtering, the FPS
counter, the frame-time recorder and its label, the memory report, the sound
meter, the audio options, and a map to start on. On the console, Find my Xbox
fills in the address, looks through its games folders for this game's installs
and reads the one it finds (several: pick one, then Read). For an exported
folder on this PC, choose it instead. Change what you want, and Write.

- It writes one file of its own, `userconfig.d/50-packager.cfg`. The engine
  runs `config.cfg` (the in-game menu's) and then every `userconfig.d/*.cfg` in
  name order, so this file overrides the menu for what it sets. A setting left
  on "(don't set)" is not written and the menu keeps deciding it.
- "What the console runs with" shows each setting's value and the file it comes
  from, and warns when a file that runs later overrides yours or loads a map on
  every start (a leftover route config does both).
- The list of `userconfig.d` files has a tick for each: it renames the file to
  or from `.off`, which is how the engine skips it. "View / edit file" opens a
  file; saving keeps the old one as `.before-packager`.
- Presets fill the choices: Sharp (720p, 8x anisotropy), Measure (recorder on,
  label, memory report) and Clear all. Developer settings (GPU skinning,
  culling switches) are behind a tick.
- The video output, sound backend, rate and hardware audio are read at boot, so
  a change shows at the next start of the game.

## The Xbox data it bakes from your files

The console draws the world, the menu background and the music from data made
from your own game files, so the packager runs the repository's asset baker
(`tools/asset-studio`) on your install and puts the result on the disc:

- one `.mpak` per map (textures, lightmaps and geometry for the Xbox
  renderer). The retail `.bsp` of every baked map is left off the disc: a loose
  `.bsp` beside its archive is loaded instead and the world then draws black,
  with only the models showing;
- the menu background tiles and their layout file;
- the 19 skies as DXT1 `.dds` (about 192 KiB of GPU memory each against 1.5 MiB
  for the retail TGAs, which stay on the disc as the fallback);
- the music as ADPCM `.xwav` files, at the source rate and at 48 kHz.

It also writes `video.cfg` (the console needs `r_refdll "nv2a"`; the retail
file leaves it empty) and adds the menu's control images (checkbox, slider,
arrows) from `xash-extras`, which the options screens need.

The first export takes about two minutes on a modern machine (the maps are
baked in parallel). The result is kept in your cache folder
(`%LOCALAPPDATA%\Xash3D-FWGS-XBOX Packager\bake` on Windows, `~/.cache/xash3d-fwgs-xbox-packager/bake`
elsewhere) and reused until the baker or your game files change.
`--no-bake` skips it (for tests only: the world will not draw). `--baker` and
`--bake-cache` say where the baker and the cache are.

The Windows exe carries the baker. The portable zip carries the Windows
one; on Linux build it first from a checkout of the repository, with cmake and
g++ installed:

    python3 tools/packager/build_baker.py

The Xbox build inside a release is the stable one: a clean build of `main`.
The release notes name its commit.

## For maintainers

    python tools/packager/build_baker.py                              # builds bin/AssetStudio
    python tools/packager/make_payload.py --xbe <clean main build>   # builds payload/
    python tools/packager/release.py --xbe <build> --version v0.21   # builds, does not publish
    python tools/packager/release.py --xbe <build> --version v0.21 --publish

`release.py` needs PyInstaller (`pip install pyinstaller`). The Windows exe is
built on Windows; a Linux binary can only be built on Linux, so Linux users
get the portable zip. Tests: `python -m unittest discover -s tools/packager/tests`.
