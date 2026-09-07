# Which optimisations actually transfer to the NV2A

`epics.md` §0 said none of Ironwail's optimisations reach the console targets.
That was wrong as stated. It is true of exactly four of them — the GPU-driven
ones — and false of everything else, which is most of Ironwail.

This document is the correction, done properly.

## The reframe

Ironwail is fast because it moves work **from the CPU to the GPU**. We cannot
use its mechanism, because compute shaders and indirect draw do not exist on
this hardware. But its *principle* is "do the work somewhere cheaper", and on
the Xbox there are **two** cheaper places Ironwail does not use:

1. **Bake time.** The baker is our compute shader. Anything Ironwail computes
   on the GPU every frame from static data, we can compute once, offline,
   with no time limit.
2. **The NV2A itself.** Not for culling, but the vertex units and register
   combiners are programmable and largely idle for 1998-era content.

And there is a third fact that changes the trade-offs the original engine
made: **the Xbox is GPU-rich and CPU-poor.** A 733 MHz Pentium III drives a
233 MHz GPU with four pixel pipelines and two vertex units, rendering a game
built for software rasterisers. Quake-lineage renderers spend CPU to avoid
sending geometry. On this machine that trade is backwards.

---

## 1. Transfers unchanged — pure CPU work

Nothing hardware-specific about any of these.

~~**`R_CullBox` with precomputed plane sign bits.**~~ **CORRECTED 2026-09-06:
already in the tree, nothing to port.** `ref/gl/gl_frustum.c:24` computes
`signbits = SignbitsForPlane()` for every frustum plane and `GL_FrustumCullBox`
(`:102`) switches on `p->signbits` at `:119`. This is Quake-lineage code
(`BoxOnPlaneSide`), not an Ironwail invention, and it was listed here as a port
because the Ironwail feature list was read instead of `ref/gl`. See
`xbox-performance-review.md` §1.

**Radix-sorted draw batching.** Ironwail groups draws with a two-pass LSD
radix sort by type and translucency to minimise state changes. It is
cache-friendly, allocation-free, and a fixed-function pipeline pays more for a
state change than a programmable one does.

**CORRECTED 2026-09-06, then corrected again 2026-09-07 — the first
correction overstated what is live.** `R_GenerateVBO`
(`ref/gl/gl_rsurf.c:2130`) builds per-texture, per-lightmap vertex arrays and
`R_DrawVBO` (defined at `:3262`, not `:58` — that is the forward declaration)
draws the world from them, but only when `R_HasEnabledVBO()` is true
(`gl_rsurf.c:2118`), and the cvar it reads defaults to off:
`CVAR_DEFINE( r_vbo, "gl_vbo", "0", …, "draw world using VBO (known to be
glitchy)" )` (`gl_opengl.c:34`). `R_DrawVBO` itself early-returns at `:3268`
without it. So **"world batching by material is not new work" was false as a
statement about the shipped default** — the default world path is the
`pglBegin`-per-polygon texture-chain walk (`R_RenderBrushPoly`,
`gl_rsurf.c:1479`), not the VBO path. Do not port `R_DrawVBO`; it is not a
base to build on, only a shape to note.

Translucent-entity sorting is a separate mechanism and stands: it is not gated
by `r_vbo` and runs unconditionally — see the `r_alphasort` paragraph below,
which this document previously left as an unqualified "port it" alongside this
same mistake.

The part that is genuinely worth more on the NV2A regardless of any of the
above: **every draw call and every state change is CPU time writing
pushbuffer words into write-combined memory.** There is no driver batching
anything on pbkit. Fewest submissions wins, and that argues for per-material
batching across the whole visible set — new work, done in the baker (§2,
`fork-plan.md` XB), not a port of anything upstream ships enabled.

**Zero-allocation ring buffers.** The previous Rust project already mapped
this one: `ferrum-core/src/decals.rs` was described in its roadmap as *"an
Ironwail-style zero-allocation `DecalRingBuffer` (`MAX_DECALS = 2048`)"*. The
pattern generalises to every transient per-frame collection.

~~**`r_alphasort` — distance-sorted transparent brushes.**~~ **CORRECTED
2026-09-07: already in the tree, and unconditionally, not behind `r_vbo`.**
`R_TransEntityCompare` (`gl_rmain.c:86-117`) already sorts translucent
entities back to front by squared distance to the view origin, with
`kRenderTransAlpha` brush entities forced to the **front** of the sorted
array (a sentinel distance of `1e9` at `:104`, not their real distance) and
drawn **first** — treated as the single farthest object, not the nearest.
**Corrected 2026-09-07: an earlier version of this paragraph said "forced
last"; it is forced first.** Ironwail added this mechanism over QuakeSpasm,
which never had it; GoldSrc always did. Nothing to port.

Its companion `r_oit` (order-independent transparency) does **not** transfer —
it needs per-pixel linked lists and fragment atomics.

**PVS caching**, reduced heap usage, and faster map loading. All CPU and data
structure work.

None of this needs a design decision. It needs porting — after checking, item
by item, that the reference does not already do it. **Three of the five items
originally listed above turned out to be in the tree already**: sign-bit
culling, translucent-entity sorting (`r_alphasort`'s job), and — qualified,
not unqualified — the shape of per-material batching, whose only shipped form
defaults to off and is not a base to build on. See `xbox-performance-review.md`
§3 for the full accounting across all nineteen items this project considered.

### Threaded saving — transfers, for a different reason than it looks

Ironwail moves game saving to a background thread to stop the frame-long
hitch on heavy maps.

The obvious reading is that this needs a second core, and **the Xbox does not
have one** — a single Pentium III at 733 MHz, no second core, no
hyperthreading. So the parallelism argument does not apply.

It transfers anyway, because the hitch is not CPU-bound, it is **I/O-bound**.
A save blocks on writes to a slow IDE hard disc. A background thread doing
that I/O yields while it waits, and the main thread keeps rendering. The win
is overlapping I/O latency with work, not doing two things at once.

And it is worth **more** here than on a PC, because the Xbox's disc is much
slower than anything Ironwail was written for. The same argument extends past
saving to any blocking read: level loading, streaming, asset resolution.

nxdk provides threads. This should be treated as a platform capability from
the start rather than something retrofitted after the first hitch is
measured.

---

## 2. Moves to bake time instead of the GPU

This is where most of Ironwail's value is recoverable, and it is the part I
missed entirely.

| Ironwail does, per frame, on the GPU | We do, once, in the baker | Caveat found 2026-09-06 |
|---|---|---|
| Compute-shader culling of surfaces | Precomputed per-leaf draw lists | Per-leaf lists must be merged per material at draw time or the draw count explodes — see §1 on pushbuffer cost. Bake them **per cluster, per material** |
| Sorting draws to minimise state changes | Bake the sort order; runtime is a copy | **Corrected 2026-09-07.** `R_GenerateVBO` exists but only runs when `r_vbo` is enabled, which defaults to off (`gl_opengl.c:34`) — it is not a live default to inherit savings from. The baked per-cluster, per-material order is new work, done once, at bake time; it is not "a copy" of anything the reference does by default |
| Indirect multi-draw to avoid CPU submission | Bake contiguous index ranges so one draw covers many surfaces | Correct, and it is the `base_vertex`-chunked u16 decision the Rust port already made |
| GPU lightmap updates and format conversion | Bake lightmaps in final hardware format, RGB565, no runtime conversion | **Wrong as stated.** A face carries up to 4 lightstyle layers (`MAXLIGHTMAPS 4`, `common/com_model.h:37`) combined at runtime with `g_lightstylevalue` (`gl_rsurf.c:751`). The baker bakes style 0 only (`BspParser.cpp:514-523`). "No runtime conversion" therefore means **no switchable or flickering lights on Xbox** unless per-style pages are baked and combined at runtime — which is the runtime work this row claims to remove. Undecided; see `xbox-performance-review.md` §2 |
| Bindless textures to avoid rebinds | Bake a per-map atlas so there is nothing to rebind | Correct. Already produced by `PakManager.cpp` |

Every row is the same trade: **static data, computed ahead of time, at the
cost of disc space and bake duration** — both of which we have and neither of
which the console pays for.

The previous project already found this shape without naming it. Its roadmap
records choosing *"base_vertex-chunked u16 draws over widening to u32, for
half the index memory and DVD bandwidth"* — a pure packaging decision that
buys runtime performance. That is the same move.

**The atlas row is already done.** `PakManager.cpp:22-60` packs every map
texture into one atlas with a 1 px border and writes normalised UVs into
`Vertex::rect`. It exists, it is correct, and nothing reads it yet.

---

## 3. What the NV2A's own hardware can take

The GPU has headroom. Half-Life's geometry load was designed for a software
rasteriser and a 1998 CPU; the NV2A is not going to be the bottleneck on it.
So work should be pushed onto the GPU wherever the interface allows, even when
that looks wasteful by 1998 standards.

**Matrix-palette skinning in VP20.** Probably the biggest single CPU win
available, with two constraints this document did not state until 2026-09-06:

- **Only half of the measured cost moves.** The Rust port's 1.6 M cycles per
  frame (13 % of the budget on an empty map) covered *building* the bone
  palette — the parent-chain `Matrix3x4_ConcatTransforms` walk at
  `gl_studio.c:802`/`:950` — and *applying* it to vertices. VP20 takes the
  second half. The first stays on the CPU regardless, and the reference does
  it eagerly every frame for every entity (`R_StudioSetupBones`,
  `gl_studio.c:815`); the `cached_bonestransform` at `:72-76` is the
  parent-attachment cache for `R_StudioMergeBones`, **not** a per-frame memo.
  An earlier version of §4 said the reference memoises lazily. It does not.
- **The palette does not fit.** VP20 has 192 constant registers
  (`deps/nxdk/tools/vp20compiler/config.h:11`) and a 3×4 bone costs three of
  them. After the view-projection matrix and light constants that is roughly
  50 bones per program; `MAXSTUDIOBONES` is 128 (`engine/studio.h:40`). Any
  mesh whose bone set exceeds the palette has to be **partitioned by bone
  subset at bake time** — the classic D3D8 problem — and the baker does not
  do that today. Whether any retail model crosses the limit is a one-minute
  census; the partition code must exist for the ones that do.

**Vertex lighting through fixed-function T&L.** Eight lights per vertex,
computed in hardware, free. Currently a CPU cost in the reference.

**Sprite billboarding in VP20** rather than building quads on the CPU.

**Water and wave deformation in VP20** rather than the CPU sine warp.

**Single-pass multitexturing.** Four texture units and eight general register
combiners mean `diffuse × lightmap × 2` plus detail in one pass. Multipass
costs draw calls, and draw calls cost CPU.

**Deliberately under-cull.** This is the counterintuitive one and it follows
directly from the GPU having headroom. The classic renderer does CPU work to
avoid sending triangles; here, sending extra triangles is close to free and
the CPU work is not. Prefer coarse, cheap rejection over precise, expensive
rejection.

**The tension that has to be measured, not assumed:** an earlier review
recommended porting `R_CullSurface` (`gl_cull.c:66-115`) for per-face backface
rejection, on the grounds that we currently send roughly twice the geometry.
On this hardware that is no longer obviously right — the GPU rejects backfaces
for free. But it is not obviously wrong either, because the saving is not
triangles, it is **index-buffer writes into 64 MB of unified memory shared
with the GPU**, and that bandwidth is genuinely scarce. Measure both. Do not
port it on the strength of the reference doing it.

---

## 4. SSE on the Pentium III

The Xbox CPU has **SSE1**: four-wide single precision. Every vector, plane and
matrix operation in this engine is single precision, so the instruction set
fits the problem.

Two caveats that decide whether it is worth the effort:

- **The P3 executes 128-bit SSE as two 64-bit micro-operations.** The
  four-wide speedup is not four times; expect somewhere between 1.5× and 2× on
  a vector-heavy inner loop. Real, but not transformative.
- **There is no SSE2**, so no double-precision SIMD and no integer SIMD beyond
  MMX.

Ironwail ships an `r_simd` cvar and `USE_SIMD` build paths. **Verify whether
those paths require SSE2 before assuming they compile for this target** — that
is a five-minute check and it decides between porting and rewriting.

Where it would pay, in order: frustum and box-plane tests; matrix
concatenation in bone setup, which is the measured hot spot; and any vertex
transform not moved to VP20.

Where it would not: the transcendental calls inside `angle_quaternion`. Six
`sinf`/`cosf` per bone dominate, and SSE1 has no transcendental instructions.
That cost is removed by *not calling it* — lazy, memoised pose computation —
not by vectorising it. **CORRECTED 2026-09-06:** an earlier version said "as
the reference does". The reference does not memoise (§3); this would be new
work, and it is the right new work.

**Also unstated until now:** there is no SIMD anywhere in this tree — a grep
for `__SSE__`, `xmmintrin`, `USE_SIMD` or `_mm_` over `public/`, `engine/`,
`ref/` and `common/` returns nothing — and Ironwail is not vendored, so the
"five-minute check" above cannot be run from this checkout. `nxdk-cc` already
passes `-march=pentium3` (`deps/nxdk/bin/nxdk-cc:9`), so what the compiler
auto-vectorises is already had; anything beyond that is intrinsics written
by hand, and it should wait for a profile that names the loop.

---

## 5. Packaging optimisations

Beyond the bake-time table in §2, the archive format itself is a lever.

- **Post-transform vertex cache ordering.** Reorder indices at bake time so
  the GPU's small post-T&L cache hits. Free offline, measurable at runtime,
  and entirely invisible to the engine.
- **Final-format textures.** DXT already, but swizzled *correctly* — the
  current `swizzleNV2A` applies a Morton order to DXT blocks, which is wrong;
  Xbox `D3DFMT_DXT1` is linear by block rows.
- **RGB565 lightmaps.** 8 MB to 4 MB on the worst map, and `MpakHeader.h:94`
  already reserves the field.
- **Sector-aligned reads.** Already done — 2048-byte alignment throughout.
- **Drop what the console never reads.** The previous project found
  `menu.mpak` baking as 8.48 MB of raw uncompressed RGBA8, and `Vertex::rect`
  written into every vertex and never read by the PC shader — 16 dead bytes
  per vertex.
- **Fix `find_asset`.** The previous project's roadmap flags it as a linear
  scan repeated per lookup. A baked hash or sorted index costs nothing
  offline.

---

## Revised position

The honest version of the claim in `epics.md` §0:

> Four of Ironwail's techniques — compute-shader culling, indirect
> multi-draw, bindless textures and persistent buffer mapping — require
> hardware the NV2A and RSX do not have, and cannot be ported. Everything
> else in Ironwail can be, and roughly half of what those four techniques buy
> is recoverable by moving the same work to bake time instead of to the GPU.

The four that do not port are real and the distinction still matters. But
"none of it transfers" was a conclusion drawn from the headline feature list
rather than from the work, and it would have cost this project most of the
available performance.

One more thing worth taking from Ironwail that is not an optimisation at all:
it carries deliberate fallback paths for old hardware, down to 2010-era cards
and workarounds for Intel integrated graphics with no driver updates. A
renderer written with the assumption that the target might be weak is a better
starting point to read than one written for the top of the market — and the
same author's reasoning about where to degrade is directly useful when
deciding what the NV2A path drops.

Its own framing also argues our case: the frame-rate gains are described as
largest **on systems with slower CPUs**. That is exactly the machine we are
targeting. The mechanism is unavailable to us; the diagnosis is not.
