# Divergence ledger

Every deliberate deviation from the Xash3D-FWGS tree is recorded here **the
same day it is written**. Without this, the third merge with upstream turns
into archaeology over why a given line differs, and the fork gets abandoned.

Fork base: `c7768c48` (2026-08-08).

## Format

One entry per divergence, with:

- **What**: the file(s) and function(s) touched.
- **Why**: the concrete reason, in one sentence.
- **Form**: `#ifdef` / new file / replacement / removal.
- **Merge risk**: high if it touches code upstream changes often.
- **Exit**: what it would take to remove the divergence (sometimes "none, it
  is permanent").

Preferred forms, from cheapest to most expensive to maintain:

1. **New file in one of our directories** — zero merge cost.
2. **New file inside an upstream directory** (e.g. a `filesystem/mpak.c` next
   to `pak.c`) — near-zero cost, just the `wscript`.
3. **`#ifdef XASH_XBOX` around upstream code** — low cost, but it pollutes
   diffs.
4. **Rewriting an upstream function** — high cost. Requires explicit
   justification here.
5. **Changing behaviour with no `#ifdef`** — forbidden unless it is a genuine
   upstream bug, in which case the patch goes upstream first.

## Active divergences

| # | What | Why | Form | Merge risk | Exit |
|---|------|-----|------|------------|------|
| 7 | `filesystem/sys.c` — one added `#include` | The POSIX branch calls `lseek` and `dup` but never includes `<unistd.h>`; on Linux it arrives transitively. A genuine portability fix, not an Xbox special case | One line in an upstream file | Low | **Send it upstream** — it is correct everywhere, not just here |
| 6 | `filesystem/sys.c` — 10 blocks widened to `#if XASH_WIN32 && !XASH_XBOX` | Those blocks use the MSVC wide-char CRT — `_wopen`, `_wstat`, `_findfirst`/`_findnext`/`_findclose`, `_A_SUBDIR`, `MultiByteToWideChar`, `SetCurrentDirectoryW` — none of which nxdk has. Every one already has a POSIX `#else` branch, and `compat/xbox` supplies what that branch needs | `#ifdef`, 10 single-line edits | Medium — upstream touches this file, though each edit is one token | Would disappear if nxdk grew the wide CRT, which is unlikely. Better exit: propose upstream that these blocks test a capability rather than a platform |
| 5 | `3rdparty/library_suffix` — `include/build.h`, 9 lines | `build.h` has no Xbox case, and nxdk targets `i386-pc-win32` so `_WIN32` is defined and the engine would believe it is running on Windows. Adds `XASH_XBOX` as a sub-platform of `XASH_WIN32`, keyed on nxdk's own `-DNXDK`, mirroring how `XASH_ANDROID` nests inside `XASH_LINUX` | **Patch file** at `patches/library_suffix-xbox-subplatform.patch`, applied to the submodule working tree. Not committed to the submodule, because that would require forking `FWGS/library-suffix` | Low as a diff, but **fragile as a process** — an unapplied patch looks like a compiler bug | **Send it upstream.** Small, generic, and useful to any Xbox port. If FWGS takes it the divergence disappears; until then, fork or keep re-applying |
| 4 | `.gitignore` — three negations added for `.claude/` | Upstream ignores `.claude/` globally (`:369`), which kept the specialist agent definitions out of version control. They are project configuration and belong in the repo | Appended negations | Low — same file and same place as divergence #1 | None, it is permanent |
| 3 | `.gitmodules` — four entries appended, under a new `deps/` directory | The game SDKs and the Xbox toolchain have to be pinned, not assumed present on the machine. `deps/` does not exist upstream, so only the `.gitmodules` file itself is shared | Appended entries, new directory | Low — upstream adds submodules occasionally; the conflict is additive and always resolvable by keeping both sides | None, it is permanent |
| 2 | `README.md` — replaced | The repository has its own name and identity; the root README is the first thing anyone arriving sees. It carries the credits section and the licence notice, both mandatory in a GPL derivative work | Full replacement | Low — guaranteed conflict on every merge, but always trivial: discard upstream's side | None, it is permanent. The original reads with `git show upstream-master:README.md` |
| 1 | `.gitignore` — block appended | Upstream's `.gitignore` globally ignores `AGENTS.md` (:367), `CMakeLists.txt` (:68) and `build-*` (:312), which silently kept three of our files out of version control | Appended at the end of the file, with explicit negations | Low — upstream edits this file occasionally, but the conflict would be trivial and always at the end | None, it is permanent |

## Anticipated divergences

What the plan already knows will diverge, so it comes as no surprise. They
move to the table above once written.

| Area | Expected form | Risk | Note |
|---|---|---|---|
| `filesystem/mpak.c` | New file + entry in `filesystem/wscript` | Low | `searchpath_t` is already a 7-pointer vtable; implement it the way `pak.c` does |
| `mod_bmodel.c`: the render half | Rewrite of 6 functions under `#ifdef` | **High** | `Mod_LoadVertexes`, `Mod_LoadEdges`, `Mod_LoadSurfEdges`, `Mod_LoadSurfaces`, `Mod_LoadTextures`, `Mod_LoadLighting`. The 9 structural functions (`Mod_LoadPlanes/Nodes/Leafs/Clipnodes/MarkSurfaces/Visibility/Submodels/Entities/TexInfo`) stay untouched because the `.mpak` stores those lumps raw |
| `ref/nv2a/` | New backend | None | It fits where `ref/soft` fits. `ref_interface_s` is not touched |
| Memory ceiling over `zone.c` | `#ifdef` + new functions | Medium | `zone.c` already has named pools with `file:line` attribution; the cap is what's missing |
| Polymorphic audio | Refactor of `engine/client/sound/` | **High** | The only one of the big pieces upstream did not abstract. Candidate to send upstream |
| Xbox trims (demos, HTTP, voice, networking) | `#ifdef` / build flags | Low | Prefer existing `wscript` flags over new `#ifdef`s |

## Things that are NOT divergences (and must not be touched)

Verified in the tree. Recorded here so nobody "fixes" what already works:

- **The renderer is already polymorphic.** `engine/ref_api.h`,
  `REF_API_VERSION 17`, `ref_interface_s` with ~59 pointers, and three
  shipping backends: `ref/gl`, `ref/soft` (17,422 lines — the proof that the
  interface tolerates a radically different backend), `ref/null`.
- **Static linking is already a first-class build mode.** `XASH_STATIC_LIBS`
  in `common/defaults.h:102-105` turns on `XASH_LIB LIB_STATIC` and
  `XASH_INTERNAL_GAMELIBS`, which is already handled in `ref_common.c:620`,
  `filesystem_engine.c:218`, `cl_gameui.c:1013`, `host.c:905` and
  `lib_common.c:133,153`. The option itself is defined in
  `scripts/waifulib/xshlib.py:20` and reaches the engine via
  `engine/wscript:155`. There is also `--single-binary` at
  `engine/wscript:53`.
- **The platform layer already exists**, with 16 backends, among them
  `psvita`, `nswitch` and `dos`. An `engine/platform/xbox/` is just one more.
- **The menu does not need rewriting.** `3rdparty/mainui` includes
  `menus/Gamepad.cpp` and `menus/Gyro.cpp`, and it is what the console ports
  use. It reproduces the classic WON menu.
- **The memory pools already exist.** `zone.c` (726 lines):
  `Mem_AllocPool`/`Mem_FreePool`/`Mem_PrintStats`/`Mem_Check`, with
  `__FILE__:__LINE__` attribution and separate pools (`mempool`, `imagepool`,
  `soundpool`).
