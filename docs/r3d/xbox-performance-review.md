# Xbox performance plan — review against the code

Written 2026-09-06, after X0 merged. This reads `optimization-transfer.md`,
the X-phases in `fork-plan.md` and the seven specialist definitions, and checks
each performance claim against `ref/gl`, `engine/`, `tools/asset-studio` and
`deps/nxdk` rather than against memory of Ironwail.

The short version: **the direction is right and the document underneath it is
about a third wrong.** Three of its claims about what the reference renderer
does are false, one of its bake-time rows silently deletes a visible Half-Life
feature, its biggest CPU win has a hardware limit nobody checked, and the plan
puts the measuring instrument at the end of the schedule. None of that is
fatal. All of it would have cost real time if built as written.

---

## 1. Claims about the reference that the reference contradicts

These matter more than the rest, because each one would have produced work
that either already exists or removes nothing.

### 1.1 "Sign-bit frustum rejection transfers unchanged" — it is already here

`optimization-transfer.md` §1 lists Ironwail's `R_CullBox` with precomputed
plane `signbits` as the first item to port. `ref/gl/gl_frustum.c:24` already
computes `signbits = SignbitsForPlane()` for every plane, and
`GL_FrustumCullBox` (`:102`) switches on `p->signbits` at `:119`. It is the
Quake `BoxOnPlaneSide` idiom and every engine in this lineage has it.

There is nothing to port. The item exists because Ironwail's feature list was
read instead of `ref/gl`.

### 1.2 "Lazy, memoised pose computation, as the reference does" — it does not

§4 says the transcendental cost in bone setup "is removed by not calling it —
lazy, memoised pose computation, as the reference does". The
`console-performance-specialist` prompt repeats it as a named failure to check
for: "eager per-entity pose computation where the reference memoises lazily".

`R_StudioSetupBones` (`ref/gl/gl_studio.c:815`) recomputes every bone of every
entity every frame. The `cached_bonestransform` array at `:72-76`, filled at
`:965-971`, is the **parent-attachment cache** that `R_StudioMergeBones`
(`:778-802`) reads so a weapon model inherits its player's bones. It is not a
per-frame memo and it saves nothing across frames.

So the Rust port's measured "13 % of the frame on an empty map for eager
bones" was not a divergence from the reference — it *was* the reference. The
memoisation is good new work. Calling it a port made it look free, and it put
a false claim into a reviewer's standing instructions.

### 1.3 "Dynamic lighting: N per-pixel lights to 8 per-vertex" — the reference has no per-pixel lights

X9 and E5 both call this "the real variance in this plan" and "a visual-design
decision made looking at a screen". It is neither, because the premise is
wrong.

World dynamic lights in `ref/gl` are **lightmap-space**: `R_AddDynamicLights`
(`gl_rsurf.c:569`) accumulates each of `MAX_DLIGHTS` into the surface's
lightmap block on the CPU, and `R_BuildLightMap( …, dynamic = true )`
re-uploads that block — at `:1191`, `:1220`, and inside the VBO path at
`:2950` and `:2990`. Studio models are already **per-vertex** with
`MAX_LOCALLIGHTS 4` (`gl_studio.c:25`, `:104-108`). `ref/soft` does the same
(`ref/soft/r_surf.c:72`, `:174`).

Both halves map onto the NV2A directly. Per-vertex lights are what its T&L
unit does natively — eight of them, so it is a *raise* from four, not a
redesign. Lightmap sub-uploads are texture updates, which it can also do; the
real question is the cost of writing into a **swizzled** texture versus using
one of the linear formats (`NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5`,
`deps/nxdk/lib/pbkit/nv_regs.h:551`) that make partial updates cheap at the
price of texture-cache efficiency. That is a measurement, not a design
decision, and it is a much smaller item than the plan says.

### 1.4 "Radix-sorted batching is the single most undervalued item" — the reference already batches

`R_GenerateVBO` (`gl_rsurf.c:2130`) builds per-texture, per-lightmap arrays at
map load; `R_DrawVBO` (defined at `gl_rsurf.c:3262` — `:58` is the forward
declaration, corrected here 2026-09-07 to match `optimization-transfer.md`)
draws the world from them; translucent entities are `qsort`ed by
`R_TransEntityCompare` (`gl_rmain.c:522`). Material batching of the world
exists. What Ironwail adds is a global sort key across entity types and a
sort that does not allocate — worthwhile, incremental.

**Qualified 2026-09-06, on a second look:** that VBO path is **off by
default** — `CVAR_DEFINE( r_vbo, "gl_vbo", "0", …, "draw world using VBO
(known to be glitchy)" )` at `gl_opengl.c:34`. The default path is the
texture-chain walk with `pglBegin` per polygon (46 `pglBegin` sites across
`ref/gl`) and a **second pass** for lightmaps; the single-pass multitexture
with `GL_RGB_SCALE_ARB 2` (`:2436`, `:2487`) lives inside the VBO path only.
So "the reference already batches" is true of code upstream itself does not
trust, and the honest count is four items already present plus one present
but disabled. The conclusion for the NV2A is unchanged and slightly stronger:
batching is mandatory, upstream's own attempt is not a base to port, and the
baked per-cluster, per-material lists are the right shape.

The genuinely NV2A-specific cost the document does not name: **on pbkit, every
draw call and every state change is the CPU writing DWORDs into write-combined
memory.** There is no driver batching anything. Fewest submissions wins, which
is an argument for per-material batching across the entire visible set — and
that constrains the baker's per-leaf lists (§2.1), which is where the actual
decision lives.

---

## 2. Things the plan does not decide and has to

### 2.1 Lightstyles — the bake-time table deletes a feature

The §2 row "bake lightmaps in final hardware format, RGB565, no runtime
conversion" is wrong as stated. A GoldSrc face carries up to **four lightstyle
layers** (`MAXLIGHTMAPS 4`, `common/com_model.h:37`), combined every frame with
`g_lightstylevalue` (`gl_rsurf.c:751`, cache check at `:1413`). That is how
every switchable light, every flickering corridor and every
`func_button`-toggled lamp in Half-Life works.

`tools/asset-studio` bakes **style 0 only**, and says so in a comment
(`BspParser.cpp:514-523`). So "no runtime conversion" currently means: on Xbox,
no light in the game ever switches or flickers. That is a visible regression
in the first hour of the campaign.

Three honest options, none chosen:

1. **Bake one page per style layer, combine at runtime.** Keeps the feature;
   reintroduces exactly the per-face combine the row claimed to eliminate, on
   the faces that have extra styles. Cost is proportional to how many faces
   carry `styles[1] != 255` — **unmeasured**. A census over the retail BSPs is
   a one-minute script and it decides this.
2. **Bake the combined result for the *on* state, treat styles as binary.**
   Switchable lights work; flicker and pulse patterns do not. A content
   decision someone has to make on purpose.
3. **Accept the loss.** Also a decision, and it should be written down rather
   than arrived at.

This belongs to the baker stage proposed in §4, and it is the first thing that
stage should measure.

### 2.2 VP20 palette limit — the "biggest CPU win" has a ceiling

Matrix-palette skinning is called "the biggest single CPU win available". Two
constraints were never stated:

- **VP20 has 192 constant registers** (`deps/nxdk/tools/vp20compiler/
  config.h:11`) and 136 instructions (`:9`). A 3×4 bone matrix is three
  registers. After the view-projection matrix and light constants, roughly
  **50 bones fit**. `MAXSTUDIOBONES` is 128 (`engine/studio.h:40`). Any mesh
  whose referenced bones exceed the palette has to be **partitioned by bone
  subset** at bake time — the classic D3D8 problem — and the baker does not do
  it. Whether any retail model crosses the line is a one-minute census; the
  partition code must exist for the ones that do.
- **Only half the measured cost moves.** The 1.6 M cycles were palette build
  plus vertex transform. VP20 takes the transform; the parent-chain
  `Matrix3x4_ConcatTransforms` walk (`gl_studio.c:802`, `:950`) stays on the
  CPU and, per §1.2, is eager. The memoisation and the VP20 move are two
  separate items and only their sum approaches the number quoted.

### 2.3 Fixed GPU memory — X3 counts the heap and not the hardware

`pb_init` allocates outside the heap with `MmAllocateContiguousMemoryEx`: the
pushbuffer (`pbkit.c:2305`, `PBKIT_PUSHBUFFER_SIZE + 8 KB`), the framebuffers
(`:2958`), the depth buffer (`:3030`) and an extra buffer (`:3081`) — and it
**deliberately leaves the HAL's framebuffer allocated too** (`:2859-2864`,
already quoted in `x0-probe-results.md` §2). So when `ref/nv2a` is live there
are two sets of framebuffers resident. X3's "one ledger, two allocators" is the
right shape; its line items should start with these fixed costs, because they
are the part of the budget that never comes back.

### 2.4 The measuring instrument is scheduled last

X11 is where "`rdtsc` counters per subsystem" appear. Every claim in
`optimization-transfer.md` is a hypothesis until measured on hardware, the
plan's own validation section says "xemu is not validation", and `lessons.md`
has four entries about trusting a number that was never measured. Putting the
ruler in the last phase is the same mistake in schedule form.

The PS3 port's lesson was "get logs off the box first". Numbers are logs.
Per-subsystem timers belong in X0's log channel, and a hardware frame time
belongs in the exit criterion of every phase that draws — X5 sets the software
baseline that X9 has to beat, and without it "X9 is faster" is an opinion.

---

## 3. What is actually new — the list that should have existed

Everything in `optimization-transfer.md`, reduced to items the tree does not
already contain, with the phase that owns each:

| Item | Status in the tree | Owner |
|---|---|---|
| Sign-bit frustum culling | **Present** (`gl_frustum.c:119`) | — |
| World batching by material | **Present but off by default** — `r_vbo` defaults to `"0"`, "known to be glitchy" (`gl_opengl.c:34`) | Baker stage; do not port `R_DrawVBO` |
| Translucent entity sort | **Present** (`gl_rmain.c:522`) | — |
| Per-vertex studio lighting | **Present**, 4 lights (`gl_studio.c:25`) | X9 raises to 8 |
| Lightmap-space dlights | **Present**, CPU + sub-upload | X9: linear vs swizzled — measure |
| Memoised bone setup | **Absent** — reference is eager | X9 or earlier; independent of the GPU |
| VP20 skinning | Absent | X9, gated on bone partitioning in the baker |
| Bone partitioning for the 192-register palette | Absent | Baker stage (§4) |
| Per-cluster, per-material baked draw lists | Absent | Baker stage |
| `base_vertex`-chunked u16 indices | Decided in the Rust port, not in this baker | Baker stage |
| RGB565 lightmap pages + a lightstyle policy | Field reserved (`MpakHeader.h:94`); style 0 only | Baker stage, after the census |
| Post-T&L vertex cache ordering | Absent | Baker stage |
| DXT linear-by-block-row fix | **Bug present** (`TextureOpt.cpp:110-132`) | Baker stage |
| Zero-allocation transient buffers (decals etc.) | `MAX_DECAL_SURFS 4096` static; allocation audit not done | X11 |
| Threaded save / load I/O | Absent; nxdk threads work (probe §1.4) | X10/X11 |
| `r_alphasort` for translucent brushes | **Present.** `R_TransEntityCompare` (`gl_rmain.c:86-117`) already sorts translucent entities by squared distance, descending — a `kRenderTransAlpha` brush gets a sentinel distance of `1e9` (`:104`) rather than its real one, so it always sorts to the **front** of the array and is drawn **first** (`gl_rmain.c:872-874` draws index 0 onward), as if it were the single farthest object. **Corrected 2026-09-07: an earlier version of this row said "forced last"; it is forced first.** GoldSrc always had this, checked directly against `ref/gl`. Whether Ironwail added it over QuakeSpasm — neither vendored here — is unverified, not fact | — |
| VP20 water deform (`EmitWaterPolys`, `gl_rsurf.c:926`) | Absent; CPU sine warp | X9, low priority |
| SSE intrinsics | Absent; no SIMD anywhere in the tree | After a hardware profile names a loop |
| Under-culling vs `R_CullSurface` backface | Reference culls per face (`gl_cull.c:83-111`) | X11, measure both |

**Corrected 2026-09-07: the table above has nineteen rows, not eighteen, and
seven are Baker-owned, not nine** — both miscounted in an earlier version of
this document and repeated into five other files before anyone recounted the
table itself. Seven are baker work (rows marked "Baker stage"), and the plan
had no baker phase for any of them. Five were already present and correct in
the tree before this project started (sign-bit culling, translucent entity
sort under both its names, per-vertex studio lighting, lightmap-space
dlights); a sixth (world batching) is present but disabled as glitchy; a
seventh (the DXT swizzle) is present and wrong. That is the cost of reading
Ironwail's feature list instead of `ref/gl` — and of not recounting a table
after editing it, which is the same failure mode one level up.

---

## 4. Proposed changes to the plan

### 4.1 A new phase: XB — Baker output for the NV2A

Between X5 and X9, runnable in parallel with X6–X8 because it touches only
`tools/asset-studio` and the PC build:

- **Lightstyle census** over every retail BSP: how many faces carry
  `styles[1..3] != 255`, per map. Decide §2.1 on the number.
- **Vertex format** for the NV2A: FLOAT, normalised SHORT, D3DCOLOR; no integer
  attributes. `base_vertex`-chunked u16 indices, decided once.
- **Draw lists baked per cluster, per material**, so runtime submission is a
  walk, not a sort.
- **Bone partitioning** against the 192-register palette, with a census of
  which retail models need it.
- **Lightmap pages in RGB565** with whatever §2.1 decided.
- **DXT block-row layout** fixed and verified against a known-good swizzle
  from a shipped title or xemu.
- **Post-T&L cache ordering** of indices.

**Exit:** `ref/gl` on PC renders a map from the baked render-half, pixel-compared
against the loose-folder path. This is the D4 principle — the console path runs
first on the machine with the debugger — and it turns X9 into a renderer job
instead of a renderer-plus-format job.

### 4.2 X0 grows a ruler

Per-subsystem `rdtsc` timers and a frame-time line in the log channel, in X0,
not X11. Cost: a few dozen lines. Every phase after it then has a number.

### 4.3 Exit criteria that carry a number

- X5: software frame time on `c1a3` and `crossfire` at 640×480 and 320×240.
  This is the baseline.
- X9: hardware frame time on the same maps, against X5's number and the PC
  capture.
- X11: what remains after X9, not the first time anyone measures.

### 4.4 X9 loses "the real variance"

Replace the dynamic-lighting paragraph with the two smaller items from §1.3:
raise per-vertex lights to eight, and measure linear versus swizzled lightmap
updates. The phrase "visual-design decision made looking at a screen" should
move to the lightstyle question in XB, where it is actually true.

---

## 5. Specialists

Seven exist: `nv2a-xbox`, `build-toolchain`, `render-architecture`,
`memory-budget`, `asset-pipeline`, `upstream-fidelity`, `console-performance`.

**One is missing, and its absence is the cause of §1.** Three of the four false
claims above are claims about what `ref/gl` does, made from memory of Quake or
Ironwail rather than from `gl_*.c`. Nobody owns that question:
`render-architecture` owns the abstraction, `upstream-fidelity` owns the
divergence *form* and the ledger, and `console-performance` owns cost — and its
prompt currently carries one of the false claims.

**Add `reference-renderer-specialist`.** Its only job: refute or confirm any
statement of the form "the reference does / does not do X" by reading `ref/gl`,
`ref/soft` and `engine/client`, and citing `file:line`. It reviews performance
proposals and plan documents, not code changes. The previous project had this
role split across `xash-reference-specialist` and `graphics-rendering-
specialist`; this fork dropped both, and it shows.

**Fix `console-performance-specialist`.** Its "eager pose computation where the
reference memoises lazily" line is false (§1.2). Replace with: the reference is
eager; memoisation is a proposed improvement, not a restoration.

**Do not add a lighting or content specialist.** The lightstyle question is a
one-off census, not a standing role. If it grows into "which map behaviours are
compiler artefacts", revisit.

---

## 6. What this review did not check

- Actual per-frame cost of anything. Nothing here has been measured on
  hardware; that is §2.4's point, not a gap in the review.
- Ironwail's source. It is not vendored and was not fetched; every claim about
  what Ironwail does comes from the earlier documents. The claims about what
  *this tree* does were checked.
- `ref/soft`'s frame time at 640×480 on a 733 MHz Pentium III. The plan says
  "it will be slow, that is fine". Probably true; unverified.
- Whether `hlsdk-portable`'s studio models differ in bone count from the
  pinned SDK's. The bone census in XB answers it.
