# Migration from Ferrum56 (Rust)

## What was archived, and why

The previous project reimplemented Xash3D-FWGS in Rust from scratch. It
reached ~35,000 lines across seven crates, with 631 unit tests and some 8,800
lines of comments citing `file:line` in the reference. It was on Track C/D of
its fidelity plan, still far from feature parity.

The decision to fork was not about the language. The real choice was
**reimplementing** an engine versus **adapting** one. Starting from the fork
reaches parity on day one, and four of the five capabilities that motivated
the rewrite were already solved upstream:

| What was needed | Already in upstream |
|---|---|
| Polymorphic renderer | `ref_api.h` v17, 3 shipping backends |
| RAM resource manager | `zone.c`, pools with `file:line` attribution |
| VFS extensible for `.mpak` | `searchpath_t`, a 7-pointer vtable |
| Static linking for console | `XASH_STATIC_LIBS` + `--single-binary` |
| Menu with gamepad support | `3rdparty/mainui`, with `menus/Gamepad.cpp` |
| Polymorphic audio | **No.** The one piece that is still real work |

The strongest argument against rewriting was those 631 tests and the
reference citations. It collapses on its own: they exist to prove the Rust
matches Xash3D. If you *are* Xash3D, that is true by construction. That
apparatus was the cost of not being the reference, and forking it takes that
cost to zero.

## What was kept

`tools/asset-studio/` — 8,544 lines of C++/CMake. It is the previous
project's genuinely original contribution: the offline baker that produces
`.mpak` archives with per-map atlases, DXT compression, packed lightmaps and
2048-byte sector alignment.

It is better served here than there: `searchpath_t::pfnLoadFile` works per
resource, not per archive, which solves at the root the 62.9 MB `global.mpak`
problem where the whole file was loaded (99.1 % of it is `lenAssetData`).

The manifests (`manifests/`) and `build-test-package.ps1` came across too.
The `.wad` files and the test `.bsp` that sat loose in the baker's directory
did **not**: they are retail assets, they were untracked in the previous repo,
and they are not ours to redistribute.

## What was not brought over

The seven Rust crates (`ferrum-core`, `ferrum-math`, `ferrum-vfs`,
`ferrum-render`, `ferrum-pal`, `ferrum-engine`, `vgui-rs`). All of it is a
partial reimplementation of code that already exists and works in this tree.

The previous repository is worth keeping read-only for two concrete things it
already solved and that will be needed:

- **Static linking of the HL SDK with the nxdk toolchain**, including an
  entity-spawn path that needs no export table.
- **The NV2A analysis**: pushbuffer encoding, register combiners and VP20,
  with tests.

They are references, not code to port.

## Pending steps

### 1. Submodules

Not initialised yet (hundreds of MB). Before building:

```bash
git submodule update --init --recursive
```

`3rdparty/mainui` is the one that matters for the menu.

### 2. Archive the previous repository

On GitHub: Settings → Archive this repository. It leaves the repo read-only
without deleting anything, and it is reversible.

Leave a pointer to this repo in its `README.md` first — an archived repository
cannot be edited afterwards.

### 3. Licence

Xash3D-FWGS is **GPL-3.0-or-later** (confirmed in the copyright headers of
`engine/common/host.c`). Developing privately creates no obligation: the GPL
triggers on **distributing binaries**, and only towards the recipients. The
moment to plan for is handing out an XBE or an ISO, not before.

## A note on the `upstream-master` branch

It is kept on purpose. It is the cheap way to answer "what did we change?"
with a single `git diff upstream-master main`, and the place upstream's
original `README.md` remains readable after ours replaced it
(`git show upstream-master:README.md`).
