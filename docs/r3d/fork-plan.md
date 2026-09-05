# Xbox bring-up plan

Modelled on [xashPS3](https://github.com/Mayo1970/xash3d-fwgs)'s goal stack,
which is the only ordering for this work validated end to end on real console
hardware — it reached a full campaign at a locked 60 FPS.

Their sequence is not the obvious one, and reading it changed four things
here. Those are marked **[from PS3]**.

Each phase is a branch, with micro-commits, specialist review and a merged PR
before advancing (see `/AGENTS.md`). Divergences go into `divergences.md` the
same day.

## What their ordering teaches

**Get logs off the box before anything else.** Their goal 1 was toolchain
bring-up *and* UDP debug logging, together. A console has no stdout to read.
Without a log channel every later failure is silent, and silent failure on
unfamiliar hardware is the expensive kind.

**Prove video outside the engine.** Their goal 4 built RSX video
infrastructure as a **standalone tool**. A triangle on screen from a 200-line
program separates "does the GPU work" from "does the engine work". Inside the
engine you debug both at once.

**Wire the software renderer to the display before writing the hardware one.**
Their goal 5 put `ref_soft` on screen; the RSX `ref_gl` did not arrive until
goal 11, six goals later. This is the item this plan had wrong. `ref/soft` is
17,422 lines that already exist and are already correct — putting it on the
framebuffer validates the filesystem, BSP loading, PVS, entity handling and
the frame loop **before** a single pushbuffer command is written. The hardware
backend then replaces a known-good picture instead of producing the first one.

**Stub the game module first.** Their goal 6 ran the main loop against a stub
client, decoupling engine bring-up from SDK bring-up. Two hard problems, one
at a time.

## Where Xbox differs from PS3

**Easier:**

- **Endianness is free.** x86 little-endian, same as the host. Their goal 13
  was a whole endianness audit across networking, demos, saves and HPK. We
  skip it.
- **Same architecture as the host**, so much numerical behaviour matches the
  PC build by construction.

**Harder:**

- **The linker.** Their static linking worked because PPC/ELF suits
  `scripts/waifulib/xshlib.py`, which merges with `ld -r`, filters with
  `objcopy -G` and sets `-melf_i386`. nxdk emits PE/COFF and links with
  `lld -flavor link`. Their goal 2 was a solved problem; ours is not.
- **The GPU.** They wired `ref_gl` to the RSX, a G70 derivative with real
  shaders. The NV2A is fixed-function T&L plus VP20 and register combiners.
  There is no `ref_gl` path for us — X9 is a new backend, not an adaptation.
- **Memory.** The PS3 has 256 MB XDR plus 256 MB GDDR3. The Xbox has **64 MB
  total, shared with the GPU**. They still hit memory pressure hard enough to
  cap listen servers at four players. We have a quarter of their budget.

---

## X0 — Toolchain and a log channel

**[from PS3]** The log channel ships with the toolchain, not after it.

- nxdk in the waf build: root `wscript`, toolchain
  `clang -target i386-pc-win32 -march=pentium3 -fuse-ld=nxdk-link`.
- `engine/platform/xbox/` from `platform/psvita` as the template.
- **A working log channel before anything else** — nxdk's debug channel, then
  network logging, so failures on hardware are readable.
- XBE packaging with `cxbe`.

**Exit:** an XBE that boots in xemu, prints, and exits cleanly.

## X1 — Static linking proven

**The highest risk in the project. Prove it in week one.**

`xshlib.py` exists to stop symbol collisions between engine, filesystem,
renderer, menu and game code — the `FI` global, duplicated vtables, repeated
helpers. Its mechanism is ELF-specific.

Establish, in order: whether `lld-link` resolves the collisions on its own;
whether a COFF equivalent of `objcopy -G` exists; and failing both, per-module
symbol renaming via `#define`, which is ugly and deterministic.

Settle here which HL SDK base compiles under nxdk. **[from PS3]** Their goal
10 used `FWGS/hlsdk-portable`, not the `halflife-updated` family pinned in
`deps/`. Evidence, not proof, but it points one way.

**Exit:** engine, filesystem, menu and a stub game module link into one XBE.

## X2 — Filesystem and headless boot

- `filesystem/mpak.c` as a `searchpath_t` (7 pointers, modelled on `pak.c`),
  loading **per resource** through `pfnLoadFile`'s custom `pfnAlloc`/`pfnFree`.
- Read from the Xbox HDD or a mounted ISO.
- **[from PS3]** Headless mode: boot, mount, load a map's data, run server
  frames, no video at all. Asset-path bugs found here are found without a
  renderer in the picture.

**Exit:** the engine loads a map from `.mpak` and runs the server loop with
`ref/null`.

## X3 — Memory budget

Earlier than the PS3 ordering, because 64 MB is a quarter of their budget and
finding the ceiling late is not survivable.

- `tools/asset-studio` emits a per-archive memory manifest and **fails the
  build** when `global + max(map)` exceeds the declared budget.
- `zone.c` gains a per-pool cap and a high-water mark; attribution exists.
- **One ledger, two allocators**: NV2A textures need physically contiguous
  memory outside the normal heap, so a budget counting only the heap is blind
  to half of it.
- Lightmaps to RGB565 — 8 MB to 4 MB on the worst map, and `MpakHeader.h:94`
  already reserves the field.

**Exit:** a declared budget, enforced at bake time, measured at runtime across
level transitions.

## X4 — Video bring-up, standalone

**[from PS3]** Outside the engine. A separate XBE that initialises pbkit, sets
a video mode, clears, and draws one textured triangle.

This is where the NV2A is learned: pushbuffer submission, DMA fencing in
unified memory, texture swizzling, and `fp20compiler`/`vp20compiler` from
`deps/nxdk/tools/`. Getting it wrong here costs an afternoon; getting it wrong
inside the engine costs a week of not knowing which half is broken.

**Exit:** a triangle, on hardware, from a program small enough to read in one
sitting.

## X5 — `ref/soft` on the framebuffer

**[from PS3]** The change their roadmap forced in this plan.

Wire the existing software rasteriser to the Xbox framebuffer. No NV2A
knowledge needed beyond X4's mode-set and blit.

This puts a **complete and correct** image on screen — world, entities,
lightmaps, HUD — validating the filesystem, PVS, BSP loading, entity handling
and frame loop before any hardware renderer exists. Everything after compares
against a known-good picture instead of guessing.

It will be slow. That is fine; it is a reference, not a shipping path.

**Exit:** a recognisable Half-Life map on screen, rendered in software.

## X6 — Input and menu

- Gamepad through nxdk's USB stack.
- `3rdparty/mainui` compiled in. **[from PS3]** Their goal 7 had the Half-Life
  menu visible before input worked, which confirms the menu is cheap and makes
  a good early validation checkpoint.
- Gamepad navigation per `ui-architecture.md` §7.

**Exit:** the menu is navigable with a controller.

## X7 — Game code

The real game module statically linked, replacing X1's stub, on whichever SDK
base X1 settled.

`vgui_support` to a null stub: `engine/client/vgui/vgui.c:345` reports the
load failure and continues, and single-player Half-Life does not use VGUI.

**Exit:** a map playable in software rendering, with HUD and game logic.

## X8 — Audio

NVAPU backend with ADPCM. Requires extracting a backend interface from
`engine/client/sound/`'s 6,284 lines — the only major subsystem upstream did
not abstract. **Candidate to send upstream.**

**Exit:** positional sound and sentences.

## X9 — `ref/nv2a`

The hardware renderer, replacing X5's software path against a picture already
known to be correct.

- Per-map atlas instead of texture arrays — the baker already produces it.
- Vertex format with no integer attributes: FLOAT1-4, D3DCOLOR and normalised
  SHORT only.
- Register combiners for `diffuse × lightmap × 2` and native alpha test.
- Matrix-palette skinning in VP20.
- **Dynamic lighting redesign**: N per-pixel lights to 8 per-vertex in
  fixed-function T&L. A visual-design decision made looking at a screen, not a
  port. The real variance in this plan.
- Fix `swizzleNV2A` on DXT blocks (`TextureOpt.cpp:110-132`): Xbox
  `D3DFMT_DXT1` is linear by block rows; the Morton swizzle is for
  uncompressed surfaces.
- Aspect ratio: implement `R_SetDisplayTransform`'s separate `scale_x`/
  `scale_y` and feed a display aspect rather than pixel dimensions.
  `V_AdjustFov` (`cl_view.c:252`) returns early on 640×480, which is exactly
  wrong for anamorphic widescreen. See `ui-architecture.md` §8.

**Exit:** a map drawn on the NV2A, compared frame by frame against X5's
software output and the PC capture.

## X10 — First playable

Full campaign start to finish on real hardware, with HUD, audio, input and
level transitions.

**Exit:** someone finishes Half-Life on an Xbox.

## X11 — Performance hardening

**[from PS3]** Their goal 14 is where 720p and a locked 60 FPS came from, and
several of its findings were transcription bugs and undersized buffers rather
than algorithmic problems. Expect the same shape.

- `rdtsc` counters per subsystem in the platform backend.
- Measure on `c1a3` and `crossfire`, not `c0a0` — one of the emptiest maps.
- The Rust port's lessons: no allocation in the frame loop; lazy memoised pose
  computation rather than an eager per-entity sweep; no unconditional logging
  on a serial-console platform.
- Ironwail's *principles* — less CPU work per frame, batch, sort by state.
  Its *techniques* do not apply: the NV2A has no compute, no indirect draw and
  no bindless textures. See `epics.md` §0.
- Memory audit: per-map peak, fragmentation after repeated `changelevel`.

**Exit:** a documented frame budget and peak memory figure on real hardware.

---

## Out of scope

- **VGUI1 widgets** — null stub until multiplayer UI is wanted, then adopt
  `FWGS/freevgui` and write only the Surface backend.
- **VGUI2** — its own epic, after X10. It unlocks Counter-Strike, Day of
  Defeat and Blue Shift, and moves the first playable build no closer.
- **Ironwail's GPU-driven techniques** — Vulkan only, structurally.
- **Multiplayer on console.**

## Validation discipline

Borrowed from their AGENTS.md, and it is why their roadmap worked:

- One goal at a time. Do not implement ahead of the current phase.
- Do not speculate about hardware behaviour; confirm it.
- On failure, isolate to a minimal case rather than rewriting.
- Escalate after two failed attempts with diagnostics, not a new hypothesis.
- **xemu is not validation.** DMA timing, the serial channel and unified
  memory behaviour are not faithfully emulated.
