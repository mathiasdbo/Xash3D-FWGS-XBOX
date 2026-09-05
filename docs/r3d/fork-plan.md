# Original Xbox port plan

Phases over the Xash3D-FWGS fork. Each is a branch, with micro-commits,
specialist review and a merged PR before advancing (see `/AGENTS.md`).

Any divergence a phase introduces goes into `divergences.md` the same day.

## Assumptions

- **PC remains a first-class product**, not just a development environment.
  `ref/gl` stays alive and the `.mpak` serves two consumers with different
  needs — something the format already anticipates, since `MpakHeader6::flags`
  distinguishes `1 = Xbox, 0 = PC`.
- **Ironwail is out of scope** until the engine boots on hardware. Its
  headline optimisations are GPU-driven (compute-shader culling,
  `glMultiDrawElementsIndirect`, OpenGL 4.3+) and the NV2A has neither compute
  nor indirect draw: it is fixed function plus VP20 and register combiners.
  They serve the PC build, not the console goal. Mixing them in now grows the
  risk without moving the target closer.

---

## F0 — Toolchain and static build

Get the tree compiling with nxdk before changing a line of logic.

- `engine/platform/xbox/`, using `platform/psvita` as the template
  (`sys_*.c`, `in_*.c`, `vid_*.c`).
- Wire nxdk into the waf build: root `wscript`, toolchain
  `clang -target i386-pc-win32 -march=pentium3 -fuse-ld=nxdk-link`.
- Turn on `XASH_STATIC_LIBS` + `--single-binary`. **There is no abstraction
  work here**: the mode already exists and is already handled in
  `ref_common.c`, `filesystem_engine.c`, `cl_gameui.c`, `host.c` and
  `lib_common.c`. The option is defined in `scripts/waifulib/xshlib.py:20`.
- Link the HL SDK statically from `deps/hlsdk` (the previous project already
  solved this with the nxdk toolchain and a path that needs no export table).

  **Open question, decide before writing the build glue.** The pinned SDKs are
  forks of the `halflife-updated` family, carried over from Ferrum56, where
  they were compiled as a Windows DLL and called over FFI. Upstream's README
  points at [`FWGS/hlsdk-portable`](https://github.com/FWGS/hlsdk-portable)
  for every non-desktop target, and it is what the Android, iOS and sibling
  console ports build against — meaning it is the one already known to compile
  as a static library for `XASH_INTERNAL_GAMELIBS`. Establish which base
  actually builds under nxdk before committing to either; the answer decides
  whether the forks carry over or get rebased onto hlsdk-portable.

  **Blue Shift is a known problem.**
  `Documentation/not-supported-mod-list-and-reasons-why.md:7` states plainly
  that Blue Shift does not work with the retail SDK because it uses vgui2,
  which xash3d does not support, and points at `hlsdk-portable`'s `bshift`
  branch for recreated source. Combined with the null-vgui stub below, Blue
  Shift should be treated as out of scope for the first playable build.
- `vgui_support` to a null stub. `engine/client/vgui/vgui.c:345` already
  reports the load failure and continues, so it is not fatal. Single-player
  HL1 does not use it at all, and neither does the command console — see
  `ui-architecture.md`. VGUI1 proper arrives by adopting `FWGS/freevgui`
  once there is something to draw it with.

**Exit:** an XBE that boots in xemu with `ref/null` and reaches the engine
console.

---

## F1 — `.mpak` as a searchpath

- `filesystem/mpak.c` implementing `searchpath_t` (7 pointers:
  `pfnOpenFile`, `pfnLoadFile`, `pfnFindFile`, `pfnFileTime`, `pfnSearch`,
  `pfnPrintInfo`, `pfnClose`), modelled on `pak.c`.
- `pfnLoadFile` accepts custom `pfnAlloc`/`pfnFree`: use them so each resource
  lands in the right `zone.c` pool.
- **Load per resource, not per archive.** This solves at the root the previous
  project's 62.9 MB `global.mpak` problem, which was loaded whole (99.1 % of
  that file is `lenAssetData`).

**Exit:** the engine reads a map from a `.mpak` with `ref/null`, without
touching a WAD.

---

## F2 — The 64 MB ceiling

Three layers, one responsibility each:

- **`tools/asset-studio` decides and rejects.** It emits a per-archive memory
  manifest (resident bytes by category after loading) and **fails the build**
  if `global + max(map)` exceeds the declared budget. On fixed hardware the
  budget is enforced at bake time, not at runtime: a map that does not fit
  breaks the build, not the console. The baker already writes every lump
  length into the header; nothing reads them today.
- **`zone.c` measures.** Add a high-water mark and a per-pool cap. The
  accounting and the `file:line` attribution already exist.
- **One ledger, two allocators.** On Xbox the 64 MB are shared with the GPU,
  and NV2A textures need physically contiguous memory
  (`MmAllocateContiguousMemoryEx`), outside the normal heap. A budget that
  only counts the heap is blind to half the consumption. Design it that way
  from the start or the two numbers never reconcile.
- Lightmaps to RGB565: 8 MB → 4 MB on the worst map. `MpakHeader.h:94`
  already reserves the field. It is the largest gain per line written in the
  whole plan.

**Exit:** a declared budget, verified at bake time and measured at runtime.

---

## F3 — Console trims

Xash3D-FWGS assumes a modern host. Prefer existing build flags over new
`#ifdef`s.

- Out: HTTP downloads, demo recording/playback, voice, dedicated server,
  masterlist.
- Review networking: a single-player port does not need it, but cutting it
  touches a lot of code. Assess the real cost before deciding.
- Review `MAX_MODELS` / `MAX_SOUNDS` (`protocol.h:193-194, 211`) against the
  F2 budget.

**Exit:** measured code and resident-data footprint, with the trim justified
in `divergences.md`.

---

## F4 — Brush model loading from `.mpak`

The big divergence, and the most delicate to maintain. The `.mpak` stores the
structural lumps **raw** (`BspParser.cpp:681-686`, `getLumpData(LUMP_NODES)`
and friends) and pre-digests only the render half. `mod_bmodel.c` splits along
exactly that line:

| Untouched (raw lumps) | To be replaced (render half) |
|---|---|
| `Mod_LoadPlanes:2424`, `Mod_LoadNodes:3438`, `Mod_LoadLeafs:3546`, `Mod_LoadClipnodes:3785`, `Mod_LoadMarkSurfaces:2537`, `Mod_LoadVisibility:3830`, `Mod_LoadSubmodels:2214`, `Mod_LoadEntities:2298`, `Mod_LoadTexInfo:3247` | `Mod_LoadVertexes:2456`, `Mod_LoadEdges:2490`, `Mod_LoadSurfEdges:2525`, `Mod_LoadSurfaces:3291`, `Mod_LoadTextures:3104`, `Mod_LoadLighting:3883` |

Nine functions keep working as they are, including **all** the collision and
PVS ones. Six get replaced by a path that reads the pre-baked data.

Look at `Mod_LoadLump:756` first: it already takes a `loadlump_source_t
source` parameter, so the hook point may already exist and the divergence be
much smaller.

**Exit:** a map loads from `.mpak` and draws with `ref/gl` on PC, without
touching a WAD or an original BSP.

---

## F5 — `ref/nv2a`

The genuinely console-specific work. It fits where `ref/soft` fits;
`ref_interface_s` is not touched.

- Pushbuffer submission via `pb_begin`/`pb_end` and a manual fence against DMA
  in unified memory.
- A single per-map atlas instead of texture arrays (the NV2A has none). **The
  baker already produces it**: `PakManager.cpp:22-60`, 1 px border and
  normalised UVs in `Vertex::rect`.
- Vertex format with no integer attributes: the NV2A only accepts FLOAT1-4,
  D3DCOLOR and normalised SHORT. This touches the baker too.
- Register combiners for `diffuse × lightmap × 2` (native scale bit) and
  native alpha test.
- Matrix-palette skinning in VP20.
- **Dynamic lighting redesign**: from N per-pixel lights to 8 per-vertex in
  fixed-function T&L. This is not a port, it is a visual-design decision that
  has to be made looking at the screen. It is the phase carrying the real
  variance in the whole plan.
- Fix `swizzleNV2A` applied to DXT blocks (`TextureOpt.cpp:110-132`): on Xbox
  `D3DFMT_DXT1` is linear by block rows; the Morton swizzle applies to
  uncompressed surfaces. A latent bug, reachable only through the Xbox path.

**Exit:** a map draws in xemu with lightmaps, animated entities and sprites,
compared frame by frame against the PC capture.

---

## F6 — Audio

The only one of the big pieces upstream did not abstract: 6,284 lines in
`engine/client/sound/` coupled to the PC mixer.

- Extract an audio backend interface, in the style of `ref_interface_s`.
- NVAPU backend with ADPCM.
- **Candidate to send upstream**: the abstraction is useful for every console
  port, not just this one. If upstream takes it, the divergence disappears.

**Exit:** positional sound and sentences in xemu.

---

## F7 — Packaging, measurement and tuning

- XBE packaging with `cxbe` and an ISO image with `extract-xiso`.
- Per-subsystem counters using `rdtsc` in the platform backend.
- Measure on `c1a3` and `crossfire`, not on `c0a0` — it is one of the emptiest
  maps.
- Memory audit against the 64 MB: per-map peak, fragmentation after several
  `changelevel`s.
- **Validate on real hardware**, not only in xemu: DMA timing and the serial
  channel are not faithfully emulated.

**Exit:** a map playable start to finish on a real Xbox, with frame budget and
peak memory documented.

---

## Out of scope for now

- **Ironwail** (see Assumptions). Its own phase after F7, if PC remains a
  product.
- **VGUI1 widgets.** Null stub in F0; adopt `FWGS/freevgui` when multiplayer
  UI is wanted. Only the Surface backend is ours to write — see
  `ui-architecture.md`.
- **VGUI2.** Its own phase, after Half-Life runs on real hardware. It unlocks
  Counter-Strike, Day of Defeat and Blue Shift; it moves the first playable
  build no closer. Upstream's route is closed to us: its own support is
  documented as drawing nothing, and it depends on proprietary 32-bit desktop
  libraries including CEF.
- **Multiplayer on console.**
