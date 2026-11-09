# Xash3D-FWGS-XBOX

*Formerly named Resonance3D. Older changelog entries, the divergence ledger and
the `// Resonance3D (#NN)` markers in the source keep the old name.*

An Original Xbox (2001) port of **Xash3D-FWGS**, a GoldSrc-compatible engine,
built against the open-source **nxdk** homebrew SDK — with a modern Vulkan
renderer kept alongside it on PC.

Two things make this port different from a straight recompile:

- **A 64 MB budget shared with the GPU.** The memory ceiling is enforced
  offline, by the asset baker, not discovered at runtime. A map that doesn't
  fit breaks the build, not the console.
- **An offline asset pipeline.** Maps, textures and sounds are compiled ahead
  of time into `.mpak` archives with per-map texture atlases, DXT compression
  and packed lightmaps, so the engine never parses a WAD on the console.

**Status: Half-Life runs on a real Original Xbox.** The engine, the NV2A
renderer (`ref/nv2a`), the menu, saves, hardware audio on the console's audio
chip and the offline asset pipeline are working, and the first campaign maps
play on hardware. It is still work in progress: frame times are measured, not
yet where we want them (see [`docs/r3d/frametime-recorder.md`](docs/r3d/frametime-recorder.md)),
and Opposing Force and Blue Shift are not packaged yet. The changelog
([`CHANGELOG.md`](CHANGELOG.md)) lists what changed and what was measured. See
[`docs/r3d/fork-plan.md`](docs/r3d/fork-plan.md) for the phased plan and
[`docs/r3d/divergences.md`](docs/r3d/divergences.md) for every deliberate
deviation from upstream.

## Documentation

The guides are a small static site in [`site/`](site/) (open `site/index.html`;
it publishes to GitHub Pages with the *Pages* workflow once Pages is enabled).

| Read | For |
|---|---|
| [Install](site/install.html) | Putting Half-Life on your Xbox with the packager |
| [Console settings](site/settings.html) | 720p, anti-aliasing, anisotropy, overlays, from your PC |
| [Menus](site/menus.html) | The controller-first menus, screens and what is still to do |
| [Progress](site/progress.html) | The 720p, 60 fps target, measured frame times, optimisations |
| [Performance](site/performance.html) | Frame-time reports from the console and the emulator, built from `docs/r3d/reports/` |
| [Building](site/build.html) | The PC build, the Xbox toolchain, the baker |
| [Frame-time recorder](site/recorder.html) | Measuring frames on the console |
| [How it differs](site/differences.html) | What was added to Xash3D FWGS |
| [About](site/about.html) | Credits, licence, AI disclosure |

The engineering record is in [`docs/r3d/`](docs/r3d/README.md): the
[divergence ledger](docs/r3d/divergences.md), the
[fork plan](docs/r3d/fork-plan.md) and the
[frame-time recorder reference](docs/r3d/frametime-recorder.md).

## Name and version

The console build reports itself as `Xash3D FWGS-XBOX 0.21 (xbox-i386 build N)`:
the engine is Xash3D FWGS, the version is upstream's, and the OS is `xbox`
because nxdk targets i386-pc-win32 and would otherwise say `win32`. Read it
with the `host_ver` console command. Other targets keep upstream's name
(divergence #216 in [`docs/r3d/divergences.md`](docs/r3d/divergences.md)).

## Targets

| Target | Renderer | Status |
|---|---|---|
| Original Xbox (NV2A) | `ref/nv2a` - pushbuffer, register combiners, VP20 | Half-Life plays on hardware; performance work ongoing |
| PC | `ref/vulkan` | Not started |
| PC | `ref/gl` (upstream) | Works - kept as the reference build |

`ref/gl` stays alive until Vulkan matches it. It is the only thing that tells
us what correct looks like.

## Putting Half-Life on your Xbox

You need your own copy of Half-Life (Steam). The **Xash3D-FWGS-XBOX Packager**
(`tools/packager/`) combines your game files with the stable Xbox build and
writes either a `Half-Life` folder (copy it to the console by FTP) or an XISO
disc image. It finds your Steam install by itself on Windows (registry) and
asks for the folder otherwise, and checks that the install has what the engine
needs. Windows: run the `.exe` from the latest
[release](../../releases). Anywhere else: unzip the portable package and run
`python3 r3d_packager.py` (Python 3 with Tk). Details in
[`tools/packager/README.md`](tools/packager/README.md). Nothing from the game
is included in the download.

## Building

For the PC reference build, follow upstream's instructions - the build system
is unchanged (`./waf configure -T release && ./waf build`).

The Xbox build uses the toolchain in `deps/nxdk` and the scripts in `tools/`:
`tools/xbox-sweep.sh` compiles the engine, `tools/xbox-game-link-check.sh hl
--include-client --include-freevgui --xbe <path>` links the Half-Life
`default.xbe`, and `tools/xbox-xiso.sh` packs a disc from it. The nxdk has no
dynamic library loading, so everything is linked statically into the XBE.
Conventions and gates are in [`AGENTS.md`](AGENTS.md).

The asset baker is a separate CMake project in
[`tools/asset-studio/`](tools/asset-studio/) and builds for the host, not the
console.

## Related repositories

This repository pulls in forks that carry this port's changes:

| Repository | What it holds |
|---|---|
| [mathiasdbo/halflife-updated-xash3d](https://github.com/mathiasdbo/halflife-updated-xash3d) | Half-Life game DLLs (`deps/hlsdk`), with the Xbox changes |
| [mathiasdbo/halflife-op4-updated](https://github.com/mathiasdbo/halflife-op4-updated) | Opposing Force game DLLs |
| [mathiasdbo/halflife-bs-updated](https://github.com/mathiasdbo/halflife-bs-updated) | Blue Shift game DLLs |
| [mathiasdbo/mainui_cpp](https://github.com/mathiasdbo/mainui_cpp) | The menu, `3rdparty/mainui` |
| [mathiasdbo/nxdk-ex](https://github.com/mathiasdbo/nxdk-ex) | Audio (APU) and other Xbox extensions on top of nxdk |

## Credits

This project is a derivative work. It exists because of:

- **[Xash3D-FWGS](https://github.com/FWGS/xash3d-fwgs)** and its
  contributors — the engine this port is built on.
- **Uncle Mike** — original author of Xash3D, whose copyright headers are
  throughout this tree.
- **Valve** — Half-Life and the original GoldSrc engine and SDK.
- **[XboxDev / nxdk](https://github.com/XboxDev/nxdk)** — the open-source
  homebrew SDK this port is built against, including pbkit, the NV2A
  pushbuffer library.
- **[FWGS/library-suffix](https://github.com/FWGS/library-suffix)** —
  platform detection used by the build system.

### Upstream and reference projects

Read-only references. None of them are responsible for this port.

| Project | Role |
|---|---|
| [FWGS/xash3d-fwgs](https://github.com/FWGS/xash3d-fwgs) | The engine. This tree starts from it |
| [FWGS/mainui_cpp](https://github.com/FWGS/mainui_cpp) | The menu, vendored under `3rdparty/mainui` |
| [XboxDev/nxdk](https://github.com/XboxDev/nxdk) | Xbox toolchain, libc, pbkit |
| [xemu](https://github.com/xemu-project/xemu) | Xbox emulator used for development |
| [Mayo1970/xash3d-fwgs](https://github.com/Mayo1970/xash3d-fwgs) | xashPS3, a sibling console port of the same engine. Its porting log is the best available map of this territory |

Upstream's own README is preserved in the history and can be read with
`git show upstream-master:README.md`.

## AI disclosure

Significant portions of this port — platform bring-up, build-system
integration, renderer/audio/input backends, the asset pipeline, and bug fixes
across the engine — were developed with the assistance of Claude (Anthropic),
used as a coding tool under human direction and review.

## License

Xash3D-FWGS is licensed **GPL-3.0-or-later**, and so is this port. See the
copyright headers throughout the source, e.g. `engine/common/host.c`.

This repository contains **no Valve assets**. Running the game requires your
own legally obtained copy of Half-Life. The asset baker reads that
installation and produces `.mpak` archives locally; those archives are not
redistributable.
