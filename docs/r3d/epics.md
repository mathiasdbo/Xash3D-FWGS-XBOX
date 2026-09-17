# Resonance3D — epic plan

A modern GoldSrc engine that performs on retro consoles. This document is the
top level: epics, sequencing, and an honest read of what the scope costs.

Reference base: [xashPS3](https://github.com/Mayo1970/xash3d-fwgs), the PS3
port of this same engine, whose porting log is the only end-to-end map of this
territory that exists.

---

## 0. Critical assessment — read this before the plan

The plan below is worth building. This section is what would sink it.

### The Ironwail requirement is not achievable on the console targets

The brief says Ironwail's optimisations are essential *for all targets*. They
cannot be. Ironwail is fast because it moves culling and lightmap updates to
the GPU using **compute shaders, indirect multi-draw, bindless textures,
persistent buffer mapping and instancing** — a feature set that starts at
OpenGL 4.3 and lands fully at 4.4.

| Target | Compute | Indirect draw | Bindless | Persistent map |
|---|---|---|---|---|
| PC (Vulkan) | yes | yes | yes | yes |
| NV2A — Xbox, 2001 | **no** | **no** | **no** | **no** |
| RSX — PS3, 2006 | **no** | **no** | **no** | **no** |

The NV2A is fixed-function T&L plus VP20 vertex programs and register
combiners. The RSX is a G70 derivative: shaders, but no compute and no
indirect draw. **Those four techniques do not port to either.**

They are not all of Ironwail, and an earlier version of this section said they
were. Most of Ironwail is CPU-side work — sign-bit frustum rejection, radix-
sorted batching, zero-allocation ring buffers, distance-sorted transparency,
PVS caching, threaded I/O — and all of it transfers. Roughly half of what the
four GPU techniques buy is recoverable by moving the same work to **bake
time** instead of to the GPU, which is a destination Ironwail does not have and
we do.

`optimization-transfer.md` is the full analysis. Read it before planning
performance work; this section is only the part that constrains the epic
boundaries.

**Resolution:** Ironwail's four GPU-driven techniques belong to E6 (Vulkan)
alone. Its CPU-side work belongs to every target. Its bake-time equivalents
belong to E2 and E3. The three are tracked separately and never conflated
under one label.

### Path tracing and a 2001 fixed-function console cannot share a draw-level abstraction

The scope spans path tracing on RTX hardware and register combiners on a
733 MHz Pentium III. That is roughly five orders of magnitude of GPU
capability and twenty-five years of architecture.

`ref_interface_s` is the wrong seam for both. It is immediate-mode and
GL-shaped, with `GL_SetupAttributes`, `GL_InitExtensions`,
`GL_BackendStartFrame` and `R_ClearScreen` documented as *"clears color buffer
on GL"*. A path tracer needs a **scene**, not a stream of draw calls; a
pushbuffer backend needs a **command list**, not immediate calls it has to
re-batch.

**Resolution:** E4 defines a scene-level interface — visible geometry,
materials, lights, entity transforms, submitted once per frame — and each
backend decides how to render it. This is the single most important
architectural decision in the project, and it is the reason E4 blocks E5, E6
and E9. Getting it wrong means every backend re-implements a GL state tracker
in order to pretend it is OpenGL.

### The renderer count is the schedule risk

Counting what the brief implies: `ref/gl` (kept), `ref/vulkan`, `ref/dx12`,
`ref/nv2a`, an RSX backend, plus a path-tracing path. That is **five to six
renderers**. `ref/gl` alone is 46,265 lines.

This is more renderer surface than most commercial engines carry. It is
achievable only if they are strictly sequenced and if some are allowed never
to happen. A plan that implies they proceed in parallel is not a plan.

### DX12 should be cut

Vulkan covers Windows and Linux. DX12 is Windows-only, and it buys no console
path: the Original Xbox is D3D8-era fixed function, the PS3 is libgcm on RSX,
and even the Xbox 360 is a D3D9 derivative. Unless a Series X/S target is
actually intended, DX12 is a second modern renderer for zero additional
coverage.

**Recommendation: Vulkan only.** If DX12 stays, it must be justified by a
named target, not by breadth.

### PS3 duplicates finished work

xashPS3 reached its goal 14 — full campaign, 720p, locked 60 FPS on real
hardware. Rebuilding that is spending months to arrive where someone already
is. The valuable relationship there is to read it, cite it, and contribute
back — not to race it.

**Recommendation:** treat PS3 as a *validation target for the abstraction*
(E4), not as a port to build. If our scene interface cannot be implemented on
RSX by someone who already knows RSX, the abstraction is wrong.

### What the brief gets right

- **No repack for PC.** Correct, and cheaper than it sounds — see E2.
- **Map manifests.** The right question, and the answer is yes — see E2.
- **Memory lessons from the Rust port.** The most valuable thing carried
  forward, see E3.

---

## Epics

Dependencies are hard unless stated. Sequence matters more than estimates.

### E0 — Solution design

The stage that precedes implementation. Detailed in `design-stage.md`.

Produces: the scene-interface specification, the memory budget model, the
asset-path decision, and a written answer to every open question in this
document. Nothing in E1+ starts before its own design question is answered,
but E0 does not have to complete before E1 begins.

**Exit:** every epic below has a named owner document and no unresolved
blocking unknown.

### E1 — Build and platform foundation

nxdk in the waf build, `engine/platform/xbox/` from the `psvita` template,
static single-image linking, the game SDK compiled in, XBE packaging.

**Known risk, and the reason this document gave was wrong until 2026-09-05.**
The `-melf_i386` in `xshlib.py:49` is appended only `if '-m32' in LINKFLAGS`,
and nxdk never passes it. What actually breaks: `objcopy` is absent from nxdk,
`llvm-objcopy` rejects `-G` on COFF (`--redefine-sym` is the one that works),
and `lld -flavor link -r` cannot do partial links. Downgraded from "may not
work at all" to "three known fallbacks", but the measurement that closes it
has never been run. See `x0-probe-results.md` §4.

**Exit:** an XBE that boots in xemu with `ref/null` and reaches the engine
console.

### E2 — VFS, assets and manifests

Blocks E3, E5.

**PC uses game folders directly, with no new code.** `filesystem/` already
implements `dir.c` against a `searchpath_t` vtable, and searchpaths are a
*chain*. `valve/`, `gearbox/`, `bshift/` work today. This is not a feature to
build; it is a feature to not break.

**Console adds `filesystem/mpak.c`** as another link in that chain, loading
per resource through `pfnLoadFile`'s custom `pfnAlloc`/`pfnFree` so each asset
lands in the right pool. Per resource, not per archive — that is the fix for
the previous project's 62.9 MB `global.mpak`, of which 99.1 % was one lump
loaded whole.

**Manifests: yes, and generated by running the game, not by scraping.** The
Rust port's baker discovered a map's assets by regex-scraping the BSP entity
lump. That cannot find anything precached by a fixed string in game code — it
silently missed five combat sprites, and the failure mode was that the effects
compiled, tested and shipped while drawing nothing. A manifest produced by
instrumenting a real playthrough records what was *actually* requested. It
also gives the searchpath chain a way to resolve assets that live outside the
archive.

**Because the two paths diverge here, the console path must be exercised on
PC.** An `.mpak` build that only ever runs on the console under-tests the code
that is hardest to debug.

**Exit:** PC boots from loose folders; console boots from `.mpak`; both paths
run in CI on PC.

### E3 — Memory and budget

Depends on E2.

Three layers, one responsibility each: `tools/asset-studio` **decides and
rejects** — a map that does not fit fails the build, not the console;
`zone.c` **measures** — it already has named pools with `__FILE__:__LINE__`
attribution, it needs a cap and a high-water mark; the resource manager
**allocates and frees**.

**One ledger, two allocators.** On Xbox the 64 MB are shared with the GPU and
NV2A textures need physically contiguous memory outside the normal heap. A
budget counting only the heap is blind to half the consumption. This must be
designed in, not retrofitted.

**Exit:** a declared per-target budget, enforced at bake time and verified at
runtime, with peak and fragmentation measured across level transitions.

### E4 — Render abstraction

**Blocks E5, E6, E9. The critical path of the whole project.**

Define a scene-level interface: visible surface set, materials, lights, entity
transforms and skinning, submitted once per frame. Backends consume a scene,
not a call stream.

Keep `ref_interface_s` as an adapter so `ref/gl` and `ref/soft` keep working
throughout — the PC build is the only reference for correct behaviour and must
never break.

**Exit:** `ref/gl` runs through the new interface with no visible change, and
the interface has been reviewed against both a path tracer's needs and a
pushbuffer's.

### E5 — `ref/nv2a`

Depends on E1, E3, E4.

Pushbuffer submission and manual DMA fencing; the per-map atlas the baker
already produces in place of texture arrays; a vertex format with no integer
attributes; register combiners for `diffuse × lightmap × 2` and alpha test;
matrix-palette skinning in VP20. `deps/nxdk/tools/` ships `fp20compiler` and
`vp20compiler`, so combiners and vertex programs are compiled from text, not
hand-assembled.

**The variance is in the baker, not the lights.** An earlier version of this
paragraph called dynamic lighting "the real variance" — N per-pixel lights to
8 per-vertex. The reference has no per-pixel lights: its studio lighting is
already per-vertex and its world dlights are lightmap sub-uploads
(`xbox-performance-review.md` §1.3). The decision that actually changes what
the player sees is the lightstyle policy, and it belongs to `fork-plan.md`
XB — the baker phase between X5 and X9 that owns seven of the nineteen real
performance items and, until 2026-09-06, did not exist.

**Exit:** a map drawn in xemu with lightmaps, animated entities and sprites,
compared frame by frame against the PC capture.

### E6 — `ref/vulkan`

Depends on E4.

The modern PC backend, and the only place Ironwail's GPU-driven techniques
apply: compute-shader culling, indirect multi-draw, bindless or descriptor-
indexed textures, persistent mapping, instancing.

**Exit:** feature parity with `ref/gl` and a measured frame-time win on a
high-entity map.

### E7 — UI and console navigation

Designed in `ui-architecture.md`. Adopt `FWGS/freevgui` for VGUI1, write only
the Surface backend; derive gamepad navigation from widget geometry rather
than declared order; on-screen keyboard as a `mainui` window. Aspect-ratio
handling including the anamorphic-widescreen FOV bug.

VGUI2 is its own epic and is scheduled after Half-Life runs on hardware.

**The `mainui` half has a concrete plan since 2026-09-15: `fork-plan.md`
XM — Console-shaped menus.** X6 showed the stock menu already navigates
with a pad; what is PC-shaped is every screen behind it. XM rewrites the
Video, Controller (with a Duke-diagram mapping screen and presets) and
Audio screens in place, flattens Main, and hides the rest under
`XASH_XBOX` - the same in-place, cvar-linked, macro-guarded approach
`sultim-t/xash-rt` used for its own graphics menu, on an owned `mainui`
fork the way this project already owns its HL SDK fork. **Updated
2026-09-17**: the menu design grew a multiplayer half (System Link,
Internet Games over configurable masters, Host Game, Direct Connect,
Player Profile) plus pause menus and a shared on-screen keyboard - the
keyboard being exactly this epic's own "OSK as a `mainui` window". The
single-player half stays XM.1; the multiplayer half is XM.3, and it is
blocked on a new phase, `fork-plan.md` XN, because the Xbox build
compiles with `XASH_NO_NETWORK` and has no sockets at all today. XN is
optional to the single-player spine - nothing in E1-E5 or X7-X11 waits
on it. **Updated again 2026-09-17**: XM.1 gained item 13, which drops the
retail WON button strip on Xbox and draws every menu row as text in a
shipped font. That is a real capability this epic had not named -
**localization**. The strip's labels are English baked into a bitmap, so
the menu could never have been translated while it drew them; and two
further links in that chain are also broken today, both invisible in
English (roughly twenty of this fork's own strings are dictionary misses
that render their own literal, and the font actually drawing on the disc
is the conchars fallback, which has no accented Latin at all). Item 13
owns all three, and the plan's own text-style mapping against the design
canvas goes with it.

### E8 — Audio

The only major subsystem upstream did not abstract: 6,284 lines in
`engine/client/sound/` coupled to the PC mixer. Extract a backend interface in
the style of the render one; NVAPU with ADPCM on Xbox.

**Candidate to send upstream** — it helps every console port, and if upstream
takes it the divergence cost goes to zero.

### E9 — Modern rendering techniques

Depends on E4, E6. **PC only.**

Forward+ clustered lighting first, since it is the one that most improves the
existing look and needs no special hardware. Path tracing after.

For path tracing, the honest reference is
[xash-rt](https://github.com/sultim-t/xash-rt): a Xash3D-FWGS fork with a
path-traced renderer, built on the separate **RayTracedGL1** library, 2,264
commits and **98 open issues**. Two things to take from it: the integration
shape is a *fork with a whole parallel renderer*, which is exactly the
argument for E4 existing; and a path tracer needs scene data GoldSrc does not
carry — real material properties, emissive surfaces, light sources as
geometry. That data has to be authored or inferred, and that work is larger
than the renderer.

**Do not start E9 before a console target ships.** It is the most attractive
epic and the least load-bearing.

### E10 — Additional console targets

Depends on E4 proving portable.

PS3 as a *validation target* for the abstraction rather than a port to build,
given xashPS3 already reached a locked 60 FPS at 720p. Others as the
abstraction proves itself.

### E11 — Performance

Continuous, per target.

Ironwail's *principles* everywhere, its *techniques* only on E6. The item
list is `xbox-performance-review.md` §3 — nineteen rows. **Corrected
2026-09-07:** seven are present in the tree in some form — five correct and
unqualified, plus a sixth present but disabled and a seventh present but
wrong, additional to the five, not counted inside them. Seven of the
nineteen are baker work (a different cut across the same table: some rows
are both present-in-some-form and baker-owned, such as the disabled batching
row and the wrong DXT row). Each row names its phase. Plus the concrete
lessons from the Rust port: no allocation in the frame loop; no unconditional
logging on a serial-console platform; memoised bone setup, which is new work
rather than a restoration, since the reference is eager.

Measurement is per target and on real hardware. xemu does not emulate DMA
timing or the serial channel faithfully.

---

## Sequencing

```
E0 design ──┬── E1 build ──────────────┬── E5 nv2a ── E10 other consoles
            ├── E2 vfs ── E3 memory ───┤
            └── E4 abstraction ────────┴── E6 vulkan ── E9 modern rendering
                                        └── E7 ui
                                        └── E8 audio
E11 performance: continuous
```

E4 is the bottleneck. E9 is the reward. E1's linker problem is the risk.

## Open questions this plan does not answer

- Does `lld-link` tolerate the symbol collisions that `objcopy -G` exists to
  prevent? Everything in E1 depends on the answer and nobody knows it.
- Which HL SDK base compiles under nxdk: the pinned `halflife-updated` forks,
  or `FWGS/hlsdk-portable`, which every other non-desktop port uses?
- Is DX12 backed by a real target, or is it breadth?
- Is PS3 a port we build or an abstraction we validate?
