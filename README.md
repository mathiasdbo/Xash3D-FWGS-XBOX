# Resonance3D

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

**Status: early.** The tree is upstream Xash3D-FWGS plus the baker. Nothing is
ported yet. See [`docs/r3d/fork-plan.md`](docs/r3d/fork-plan.md) for the
phased plan and [`docs/r3d/divergences.md`](docs/r3d/divergences.md) for every
deliberate deviation from upstream.

## Targets

| Target | Renderer | Status |
|---|---|---|
| Original Xbox (NV2A) | `ref/nv2a` — pushbuffer, register combiners, VP20 | Not started |
| PC | `ref/vulkan` | Not started |
| PC | `ref/gl` (upstream) | Works — kept as the reference build |

`ref/gl` stays alive until Vulkan matches it. It is the only thing that tells
us what correct looks like.

## Building

Nothing Xbox-specific builds yet. For the PC reference build, follow
upstream's instructions — the build system is unchanged
(`./waf configure -T release && ./waf build`).

The Xbox target will use upstream's existing static-linking support
(`scripts/waifulib/xshlib.py`, `--static-linking`), since nxdk has no dynamic
library loading:

```
./waf configure --xbox --static-linking=filesystem_stdio,ref_nv2a,menu,server,client
```

The asset baker is a separate CMake project in
[`tools/asset-studio/`](tools/asset-studio/) and builds for the host, not the
console.

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
