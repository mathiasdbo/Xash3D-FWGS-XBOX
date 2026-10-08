# Perf research: moving CPU work to the idle NV2A, and SSE1/MMX on the Pentium III

Run: `perf-research`, target `xbox`, slug `xbox-gpu-offload-sse`. Base commit
`5f7349d6`. Inputs: four survey lenses (`cpu-p3`, `gpu-nv2a`,
`engine-hotpaths`, `memory-streaming`), the reference-renderer referee's
verdicts, and the planner's own reading of the tree. I opened every
`file:line` cited below for this report. Items that no lens or referee raised
are marked **[planner]**.

This is the companion to `docs/r3d/reports/perf-research/xbox-cpu-bound-o2.md`
(same base commit). That report owns the generic CPU items: P1 replaces
pdclib's -O0 `memcpy`, P2/P6/P7 take synchronous I/O off the frame, and P4
covers client bone setup with `fsincos`. It also owns the measurement table M2.
This report does not repeat those items. It cites them where an offload or
SIMD item depends on one.

## 1. Summary and targets assumed

**The measured problem.** The PR #169 hardware recording (21.4 min, c0a0 to
c1a1a, -O2 engine) shows CPU at 11-26 ms/frame and GPU wait at 0.4-0.6 ms/frame
on every map. The worst maps are c1a0c (27.9 fps, 26.1 ms CPU), c1a1/c1a1a
(~32 fps, ~22 ms) and c1a0e (36.4 fps, 18 ms). The NV2A finishes before the CPU
on every map. GPU wait is not GPU busy time: pbkit has no GPU timestamp
(`docs/r3d/frametime-recorder.md:110-120`). "Mostly idle" is therefore an
inference: the GPU never makes the CPU wait.

**Build levels behind every number here.** Engine, ref/nv2a and the game DLLs
are built at -O2 (`tools/xbox-opt.sh:35`, passed at `tools/xbox-cc.sh:474-476`
as `-target i386-pc-win32 -march=pentium3 "$XBOX_OPT"`). nxdk's prebuilt
libraries (pdclib, pbkit) are -O0 (companion report §1). The -O2 objects I
disassembled come from `.sweep/`, whose manifest's first line reads
`DRIVER tools/xbox-cc.sh … -O2`. The `.sweep-os/` objects the referee used are
-Os (`.sweep-os/sweep-manifest.txt:1`). Their floating-point code class is the
same.

**Targets.**

- **Original Xbox** is the only target in scope. Its CPU is a 733 MHz
  Coppermine Pentium III (MMX and SSE1, no SSE2). Its GPU is an NV2A with
  fixed-function T&L, VP20 vertex programs (136 instructions, 192 constants,
  13 temporaries, A0.x relative addressing:
  `deps/nxdk/tools/vp20compiler/config.h:9-12`, `nvvertparse.c:399-519`),
  4 texture stages and 8 combiner stages. Memory is 64 MiB unified, with
  physically contiguous allocations for anything the GPU reads.
- **Pentium III PC** is not a target of this run. Two things carry over. The
  SDK-side SSE1 item (R4) also runs on a P3 PC that uses the HL client. And
  upstream's own x86 build flag may not run on a P3 at all (§8).

**Main findings.**

1. **The studio per-vertex path is the one large offload, and it lives entirely
   inside ref/nv2a.** Half-Life's client does bone setup itself
   (`deps/hlsdk/cl_dll/StudioModelRenderer.cpp:780`). It then calls back into
   the renderer through `IEngineStudio.StudioDrawPoints`
   (`StudioModelRenderer.cpp:1654`, `:1708`), which is `R_StudioDrawPoints`
   (`ref/nv2a/r_studio.c:3517` → `:2005`). A VP20 path therefore needs **no
   SDK change**. It replaces CPU work that grows with triangle count and
   leaves only work that grows with bone count. Four things gate it: a
   bone-palette census, a fix for the `LightToTexGamma` table
   (`r_studio.c:1644`), a CPU fallback for entities lit by local lights
   (`R_LightLambert`, `:1653-1708`), and a static vertex-buffer budget in the
   bake-time ledger.
2. **[planner] The TriAPI spends CPU per triangle, not per draw.** The
   assembler turns every strip and fan into a triangle list on the CPU
   (`ref/nv2a/nv2a_tri_util.c:65`, `:86`). Each triangle then goes through
   `nv2a_2d_append` (`ref/nv2a/r_context.c:7255-7330`). That function resolves
   a texture key, compares about 16 state fields and the fog block, and copies
   72 bytes (`:7326`). The NV2A could take strips natively. This is the cheap
   step before VP20, and it stays useful for every CPU fallback path.
3. **[planner] The world pass visits every face of the map every frame, and it
   runs the frustum test before the PVS test.** The loop is at
   `r_context.c:3013` and `:3099`, and the order is set at `:3141`
   (`nv2a_frustum_cull_face( face ) || pvs_culled`). Swapping the two operands
   gives the same result and skips a 5-plane box test for every PVS-rejected
   face.
4. **SSE is already on, and float math already runs in SSE registers.** clang
   at `-march=pentium3` compiles `float` arithmetic to scalar SSE. At -O2,
   `.sweep/engine_common_pm_trace.obj` has 385 scalar-SSE instructions and
   1 x87 instruction, and `Matrix3x4_ConcatTransforms` in
   `.sweep/public_matrixlib.obj` is 36 `mulss` and 27 `addss` with no packed
   ops. A binary full of SSE instructions ran for 21 minutes on the console,
   so the kernel has CR4.OSFXSR set. The question "keep PM/physics on x87?" is
   moot: PM has not been on x87 in any Xbox build. What remains for SIMD is
   hand-packed SSE1 where a profile names a loop. The live bone concatenation
   is in the SDK (`deps/hlsdk/pm_shared/pm_math.cpp:396`), not in `public/`.
5. **Three offload candidates are smaller than the brief assumed.**
   - **Water.** Neither tree calls a sine per vertex. Both read the
     `r_turbsin` table (`ref/gl/gl_rsurf.c:960-964`). A VP version is still
     possible, and an exact one is possible too (R5).
   - **Lightstyles.** The CPU rebuild already only runs when a style changes,
     as in the reference, and has a per-frame budget. The only measured cost
     is 128.9k cycles median per rebuild, measured in xemu
     (`tools/xbox-opt.sh:22-26`).
   - **The CPU backface test.** It is already off by default
     (`r_context.c:768`).

## 2. Ranked improvements

The ranking is expected CPU ms removed on the worst maps, divided by cost and
risk. **None of the gains below is a measurement.** No per-subsystem time
exists yet. Each item gives the basis for its estimate and the counter or span
that will turn it into a number. "xemu settles" means the decision depends only
on counts, which are identical in xemu (`frametime-recorder.md:139-148`).
"Hardware-only" means the decision depends on time.

### R0 (prerequisite): the spans and counts the offload decisions need

- **Bound:** none (instrument). **Target:** Xbox.
- **Where:**
  - New work counters go before `XBOX_FT_W_COUNT`
    (`engine/platform/xbox/xbox_frametime_util.h:108-130`), added with
    `NV2A_FT_ADD` (`ref/nv2a/nv2a_ft.h:37`).
  - Companion M2 already defines `studio_points_kcyc` (`r_studio.c:2005`),
    `client_studio_kcyc` (`r_studio.c:3123-3125`) and `lm_kcyc`. Do not
    duplicate them.
- **Add** (offload-specific):

  | Counter / span | Site | Decides |
  |---|---|---|
  | `world_walk_kcyc` | `nv2a_draw_map_geometry` (`r_context.c:2974`) | R3 |
  | `world_faces_walked` (count) | inner loop `:3099` | R3, exact in xemu |
  | `tri_appends` (count) | `nv2a_2d_append` (`:7255`) | R2, exact in xemu |
  | `studio_strip_verts` (count) | `R_StudioSetColorBegin` (`r_studio.c:1711`) | R1/R2: unique strip vertices, against `v_studio` (post-expansion) |
  | `studio_lit_ents` (count) | `R_StudioDrawPoints` when `g_studio.numlocallights > 0` (set at `r_studio.c:1486`, `:1551`) | R1's CPU-fallback share |
  | `water_kcyc` | `R_WarpEmitSurface` loop (`ref/nv2a/r_warp.c:335-376`) | R5 |
  | `particles_kcyc` | `CL_DrawParticles` (`ref/nv2a/r_effects.c:1439`) | R6 |

  `v_studio` cannot show the strip-expansion factor on its own.
  `nv2a_2d_append` adds 3 per triangle (`r_context.c:7329`), so `v_studio`
  divided by `studio_tris` is 3 by construction. `studio_strip_verts` is the
  missing denominator.
- **Offline census (no engine change):** for every retail `.mdl`, count
  `numbones`, the number of distinct bones each mesh references (vertex bones
  plus normal bones), `STUDIO_HAS_BONEWEIGHTS`, and `STUDIO_NF_CHROME` meshes.
  Count tricmd vertices too, for the R1 memory estimate. This settles the
  VP20 palette question, and xemu or the PC can run it.
- **divergences.md:** amend #141 (counters and spans; ref/nv2a is
  fork-owned, form 1). Host-side spans follow companion M2
  (`#if XASH_XBOX`, form 3).

### R1: Studio skinning and lighting in a VP20 program, inside `R_StudioDrawPoints`

- **Bound:** CPU (the work moves to the GPU's vertex stage). **Target:** Xbox.
- **Evidence** (all opened):
  - **Per unique vertex:** a CPU `Matrix3x4_VectorTransform`
    (`r_studio.c:2077`) and `R_LightStrength` (`:2078`, body `:1729-1749`).
    The weighted multi-bone path is at `:2054-2072`.
  - **Per normal:** `R_StudioLighting` (`:2116-2118`, body `:1593-1645`). It
    ends in `LightToTexGamma( illum * 4 ) / 1023.0f` (`:1644`), a table
    lookup.
  - **Per emitted strip vertex (not per unique vertex):**
    `R_StudioSetColorBegin` runs `R_LightLambert` (`:1711-1719`), and then
    `TexCoord2f` and `Vertex3fv` follow (`:1904-1907`).
  - **Chrome** adds two dot products per normal. The per-bone basis is
    already memoised per frame through `chromeage` (`:1375`; reference
    `ref/gl/gl_studio.c:1122`).
  - The reference has the same shape: `gl_studio.c:1951`, `:1997`,
    `:2038-2039` (referee: confirmed).
  - Only three vertex programs exist (`vs_lit`, `vs_2d`, `vs_tri`, included
    at `r_context.c:1245`, `:6678`, `:6722`). Each does one matrix multiply
    and passes the rest through (`ref/nv2a/vs_tri.vs.cg:26-48`).
- **Change** (fork-owned ref/nv2a only):
  1. **Vertex buffer.** Build it per model, from the `.mdl` already in memory,
     on first draw. Do not do it at bake time: the data is there at runtime,
     and doing it lazily avoids a baker dependency. Use an LRU budget like the
     skin cache.
     - Unroll each mesh's tricmds into unique (vert, norm, s, t) tuples.
       Store position, normal, vertex-bone index, normal-bone index and UV.
       Emit an indexed triangle list per mesh with u16 indices, so one draw
       covers a mesh.
     - The vertex and its normal can bind different bones (`pvertbone`
       versus `pnormbone`, `r_studio.c:2043-2044`), so the format needs two
       index attributes.
  2. **Constants per draw.** For each bone, upload the 3x4 `bonestransform`
     (3 registers) and `blightvec[bone]` (1 register). Uploading `blightvec`
     as data rather than re-deriving it in the VP keeps the reference's
     arithmetic exact, including the `VectorNormalize` "in case model may be
     scaled" at `r_studio.c:1579-1581`. That is 4 registers per bone. After
     the view-projection matrix and about 4 lighting constants (ambient,
     shadelight, the `SHADE_LAMBERT` terms), **about 45 bones fit per draw**.
     Split any mesh whose referenced set is larger. The census in R0 decides
     how often that happens.
  3. **Lighting.** The VP computes `illum` the way `R_StudioLighting` does
     (`:1593-1645`), using the DP3, MAD, MAX and MIN instructions. It does
     not apply the gamma table. It outputs `illum*4/1024` as a texture
     coordinate. A spare texture stage samples a 1024x1 point-filtered LUT
     texture that holds `LightToTexGamma(i)/1023`, and the register combiners
     multiply the result by `lightcolor` (a combiner constant) and the
     diffuse texel. This reproduces the table at its own resolution, with no
     polynomial. The remaining difference is ±1 LSB of 8-bit rounding (see
     Risk).
  4. **CPU fallback, unchanged code.** These cases keep today's path:
     - entities with `g_studio.numlocallights > 0`: the `R_LightLambert`
       elight sum with `LinearGammaTable`/`ScreenGammaTable` and a `sqrt`
       (`:1681`, `:1693-1702`);
     - `STUDIO_HAS_BONEWEIGHTS` models;
     - chrome meshes (`:2198`);
     - `kRenderFxGlowShell` (`:2083`);
     - `kRenderTransAdd` meshes, whose light value is a constant
       (`:2102-2109`). This one is cheap to add to the VP later.
  5. **What stays on the CPU.** Bone setup, events and attachments stay in the
     client DLL (`StudioModelRenderer.cpp:780`, `:1187`, `:1478`; companion
     P4). `R_StudioSetupLighting` stays as the per-bone `blightvec` producer.
- **Expected gain:** unmeasured.
  - The basis is a change of scaling. Today the studio cost in ref/nv2a grows
    with triangles. Per triangle it is 3 strip vertices, each with a lighting
    call and three TriAPI calls, then a per-triangle append with a 72-byte
    `memcpy` through pdclib's -O0 loop (companion P1). Per unique vertex and
    normal it is a transform plus lighting. After the change it grows with
    bones and meshes: about 16 constant dwords per bone per draw, plus one
    draw command per mesh. The client's bone setup (companion P4) does not
    change.
  - `studio_points_kcyc` on c1a0c is the ceiling. Fallback entities
    (`studio_lit_ents`) are subtracted from it.
  - The old "1.6 M cycles, 13 % of an empty map" (`optimization-transfer.md`
    §3) is a Rust-port number on an empty map. Do not reuse it.
- **Risk:**
  - **Fidelity.** VP20 arithmetic is not bit-identical to the P3's. Point
    sampling the LUT in the combiner, with 8-bit textures and combiner
    precision, gives about ±1 LSB against the reference's
    `VectorScale(color, 255)` truncation (`:1657`, `:1707`). Compare screenshots at
    two optimisation levels and boot the baseline twice (`lessons.md`).
  - **Fallback visibility.** The fallback split shows: an NPC that steps
    into a muzzle-flash elight switches path. Both paths must produce the
    same pixels when no local light is present. Test that directly with
    `r_studio_builtin_renderer` off.
  - **Stage budget.** The extra stage uses one of the 4 texture stages that
    the studio path does not currently use.
  - **No hardware reference values.** No VP with relative addressing has run
    on hardware in this tree. Take register words from a hardware-proven
    codebase (`lessons.md`, the xemu-is-not-validation rule).
- **Measured by:**
  - xemu (exact): `studio_tris`, `studio_strip_verts`, `pb_dwords`, `draws`
    and `studio_lit_ents` before and after.
  - Hardware-only: `studio_points_kcyc` and CPU ms on c1a0c, c1a1a and c1a0e,
    plus GPU wait. VP throughput is NV2A timing, which xemu does not model.
- **divergences.md:** new row. What: `ref/nv2a/r_studio.c`
  `R_StudioDrawPoints`, and new `ref/nv2a/vs_studio.vs.cg`/`.inl` plus a
  combiner program. Why: per-triangle CPU work moved to VP20. Form 1
  (fork-owned directory). Name the reference functions it diverges from by
  mechanism (`gl_studio.c:1951` `R_StudioDrawPoints`, `R_StudioLighting`) and
  list the fallback cases. The memory budget belongs in `Nv2aMapFit.h` (§5).

### R2 [planner]: Cut the TriAPI's per-triangle overhead; submit strips and fans natively

- **Bound:** CPU. **Target:** Xbox.
- **Evidence:**
  - `nv2a_tri_push` turns a strip into one triangle per vertex
    (`nv2a_tri_util.c:86-110`) and a fan likewise (`:65-84`).
  - Each triangle goes to `nv2a_tri_emit_primitive` (`r_context.c:7623`) and
    then `nv2a_2d_append`. There it costs:
    - `nv2a_2d_resolve_batch_key` (`:6752`), a texture-table lookup;
    - a 16-term state compare plus `memcmp` of the key and the fog block
      (`:7268-7287`);
    - the copy at `:7326`.
  - The file already records the cost once. The water path bypasses the
    assembler because "the assembler's per-vertex staging … dominated the
    cost of a lake's 10,000 vertices a frame" (`r_context.c:7158-7160`).
- **Change** (ref/nv2a):
  1. Resolve the batch key and run the state compare once per `Begin`, not
     per primitive. Between `Begin` and `End`, `Color4ub`/`TexCoord2f` only
     latch per-vertex values (`r_context.c:7730-7748`). Keep the full compare
     on the first append after `Begin`, and assert in debug builds that
     `nv2a_2d_desired` did not change inside the pair. Whether anything
     writes it there is unverified.
  2. Map `TRI_TRIANGLE_STRIP`/`FAN` to the NV2A's native begin ops instead of
     expanding them. That gives one BEGIN/END per strip, 1x the vertices
     instead of about 3x, and a third of the bytes copied.
  3. This extends companion P3 steps 1 and 3 (direct studio emit). If R1
     lands, R2 still serves the R1 fallback paths, beams, sprites and
     2D/HUD.
- **Expected gain:** unmeasured.
  - **Basis:** per-triangle fixed overhead times `tri_appends`. Removing the
    expansion cuts the vertex bytes written to the ring to about
    1/(expansion factor), which equals `studio_strip_verts` /
    (3 × `studio_tris`).
  - **Trade-off:** native strips add a BEGIN/END block per strip (`prims`,
    `pb_dwords`). The block count rises, while the copy volume falls.
- **Risk:** low.
  - The assembler's winding and splitting rules (`nv2a_tri_util.h:8-12`, the
    GL 1.x conventions) must still hold for native strips. The even/odd
    winding swap at `nv2a_tri_util.c:97-104` is what the hardware strip does
    natively; check it against the cull mode on hardware.
  - The 256-vertex `DRAW_ARRAYS` limit (`nv2a_tri_util.h:23-27`) still
    applies to long strips.
- **Measured by:**
  - xemu (exact): `tri_appends`, `prims`, `pb_dwords` and `v_studio`.
  - Hardware-only: `studio_points_kcyc` and CPU ms.
- **divergences.md:** amend the TriAPI row (ref/nv2a, form 1): native
  strip/fan submission and per-`Begin` state resolution. No upstream code is
  touched.

### R3 [planner]: Test PVS before the frustum, then walk only PVS faces

- **Bound:** CPU. **Target:** Xbox.
- **Evidence:**
  - `nv2a_draw_map_geometry` walks every batch (`r_context.c:3013`) and every
    face of each batch (`:3099`). That is every baked face, each frame.
  - Per face it reads `face->sky`, runs `nv2a_map_face_is_world`
    (`:2028-2037`, a second array), and then evaluates
    `nv2a_frustum_cull_face( face ) || pvs_culled` (`:3141`).
  - So a face the PVS already rejected still pays an out-of-line call to
    `nv2a_frustum_cull_box` (`:1379-1406`). That call converts the face's
    bounds to floats, re-reads the `nv2a_r_nocull` cvar, and runs up to
    5 plane tests.
  - The PVS table is rebuilt only when the leaf changes (`nv2a_mark_leaves`,
    `:5377`; the same-leaf early return is in that function, and the memset
    is at `:5547`).
  - ref/gl walks the BSP and never visits faces in non-visible leaves
    (`R_RecursiveWorldNode`, cited at `r_context.c:3127-3130`).
- **Change:**
  1. Swap the two operands at `:3141`. Both are pure predicates, so the
     result is identical.
  2. At PVS recompute (`nv2a_mark_leaves`), also build a compact list of
     visible world-face indices in batch order, with sky and brush-entity
     faces already filtered out. The per-frame walk then iterates that list,
     and run-merging still works on consecutive indices.
  3. Optional, after step 2: SSE1 four faces at a time in the frustum test
     (R4's tooling).
- **Expected gain:** unmeasured. The basis is (all faces − PVS faces) × the
  cost of one frustum call, plus the is-world and sky reads for every face.
  `world_faces_walked` against `world_faces` gives the waste exactly in xemu.
- **Risk:** low for step 1, none visible. Step 2 must keep the "fail open"
  rules. If there is no remap table or the face count mismatches
  (`:5436`, `:5451`), it must fall back to the full walk.
- **Measured by:** xemu (exact) for `world_faces_walked`. Hardware-only for
  `world_walk_kcyc`.
- **divergences.md:** amend #96/#97 (ref/nv2a's frustum/PVS cull, form 1). It
  restores the reference's property of not visiting non-PVS faces.

### R4: Hand-packed SSE1 where the live bone path spends its cycles

- **Bound:** CPU. **Targets:** Xbox, and a P3 PC running the HL client.
- **Evidence:**
  - The default HL world pass hands studio models to the client renderer
    (`r_studio.c:3112-3126`, `r_studio_builtin_renderer` default `"0"` at
    `:160`; installed by `CL_InitStudioAPI`,
    `engine/client/dll_int/cl_game.c:3973-3979`).
  - `IsHardware` returns 1 (`cl_game.c:3946-3949`). So every non-root bone
    pays **two** `ConcatTransforms`
    (`deps/hlsdk/cl_dll/StudioModelRenderer.cpp:995-996`), and a root bone
    pays one plus a `MatrixCopy` (`:978-982`).
  - `ConcatTransforms` is scalar C in the SDK's `pm_shared/pm_math.cpp:396`.
  - ref/nv2a's own `R_StudioSetupBones` (`r_studio.c:1042`) and
    `public/matrixlib.c` are **not** on that path in gameplay (referee).
- **Change:**
  1. **Concat.** Under `#ifdef NXDK`, in the SDK fork, add an SSE1
     `ConcatTransforms`: three row broadcasts times four `__m128` columns,
     with `mulps`/`addps`. Use `movups`, or align `m_pbonetransform`'s
     storage.
  2. **Alignment.** pdclib's dlmalloc aligns to `2*sizeof(void*)` = 8 bytes
     by default (`deps/nxdk/lib/pdclib/functions/_dlmalloc/malloc.c:704`).
     The 16-byte value at `:654` is inside a DARWIN-only block. So `movaps`
     needs explicitly aligned storage. On the P3, an unaligned or split
     `movups` is markedly slower.
  3. **Studio transform.** Do the same, as a fork-owned inline in ref/nv2a,
     for `Matrix3x4_VectorTransform` in `R_StudioDrawPoints`. Do this only
     if R1 is not built, or for its fallback paths.
  4. **Frustum.** Do the same in `nv2a_frustum_cull_box` if `world_walk_kcyc`
     still leads after R3.
- **Not an SSE target:** the transcendentals. `AngleQuaternion` makes six
  `sin`/`cos` calls (`deps/hlsdk/cl_dll/studio_util.cpp:23-42`), and those
  are out-of-line pdclib x87 `fsin`/`fcos`
  (`deps/nxdk/lib/pdclib/platform/xbox/functions/math/sin.c`). SSE1 has no
  transcendental instructions. Companion P4's `fsincos` is the lever there.
- **Expected gain:** `optimization-transfer.md` §4 estimates 1.5-2x on the
  loop itself, because the P3 splits each 128-bit op into two 64-bit µops.
  That estimate is not re-derived here. The frame share is unmeasured, and
  `client_studio_kcyc` (companion M2) bounds it.
- **Risk:**
  - Packed adds associate differently from the scalar C expression order
    (`pm_math.cpp:398-405`), so results can differ in the last bit. Bone
    matrices feed only rendering, not physics, but `pm_math.cpp` is compiled
    into the game DLL too. Guard the function so that only the client's
    copy changes. Unverified: whether the server links the same symbol.
  - There is no SIMD precedent anywhere in this tree.
- **Measured by:** hardware-only (`client_studio_kcyc`, or a one-boot
  `rdtsc` microbenchmark of cycles per concat). xemu does not model P3 µop
  splitting.
- **divergences.md:** new row for the SDK fork (`deps/hlsdk`, owned fork;
  precedent row #44). Form: `#ifdef NXDK` inside `pm_math.cpp` or a new
  `cl_dll` file. Exit: none.

### R5: Water deform in VP20, gated on `water_verts`

- **Bound:** CPU. **Target:** Xbox.
- **Evidence:**
  - Per vertex per frame, `R_WarpEmitSurface` (`r_warp.c:315-379`) does:
    - the wave (`nv2a_warp_wave_z_frame`), which is table lookups, not
      `sinf`; the reference does the same at `gl_rsurf.c:960-964`;
    - two texinfo dot products;
    - a CPU entity transform (`r_warp.c:347`), which ref/gl leaves to GL.
  - Then it appends through `nv2a_tri_append_3d` (`:358`, `:371`).
- **Change:**
  1. Copy each surface's subdivided polys (engine-owned, in the model pool)
     into a static contiguous vertex buffer at `R_NewMap`.
  2. In a VP:
     - reproduce the table **exactly**. `EXP` gives `x − floor(x)` in `.y`,
       which emulates the `(int)(…) & 255` truncation;
     - evaluate the sampled sine with an odd polynomial. `warpsin.h:22-24`
       values match 4·sin(2πi/256);
     - pass `cl.time` and `waveHeight` as constants.
  3. This is an exact port of the quantised wave, not a smooth
     approximation.
- **Expected gain:** unmeasured. It is zero on a map with no water. None of
  c1a0c, c1a1, c1a1a or c1a0e is known to have open water; `water_verts` in
  M1 decides that, exactly in xemu.
- **Risk:** the VP path's precision, and float-to-int truncation of negative
  values, must match `(int)` rounding toward zero. That needs a host-side test
  of the formula and a side-by-side screenshot.
- **Measured by:** xemu for `water_verts`; hardware-only for `water_kcyc`.
- **divergences.md:** amend #166 (ref/nv2a, form 1).

### R6: Particle billboards on the GPU, gated on `particles`

- **Bound:** CPU. **Target:** Xbox.
- **Evidence:**
  - `CL_DrawParticles` (`r_effects.c:1439-1499`) uses one `Begin(TRI_QUADS)`
    for the whole list (`:1453`).
  - Per particle it computes a distance-scaled size and two `VectorScale`s,
    then emits four `Vertex3f` calls (`:1460-1491`), and calls
    `CL_ThinkParticle` (`:1495`). That last call is engine physics and stays
    on the CPU.
  - Point-sprite registers exist (`deps/nxdk/lib/pbkit/nv_regs.h:229-230`,
    `:372`, `:392-400`) and are unused in this tree.
- **Change**, preferred option first:
  - **(a)** Add a `vs_tri` variant that takes the centre, a corner-select
    attribute and size, and expands the corner with `RI.vright`/`vup` as
    constants. This keeps quads, so it keeps today's UVs and depth.
  - **(b)** Use hardware point sprites. Size limits, the screen-space size
    computation and which texture stage gets generated coordinates on the
    NV2A are all unverified.
- **Expected gain:** unmeasured. Only the corner arithmetic and 3 of 4 vertex
  writes go away. The think call and R2's per-append cost remain.
- **Risk:** low with (a); unknown with (b).
- **Measured by:** xemu for `particles` and `tri_appends`; hardware-only for
  `particles_kcyc`.
- **divergences.md:** amend #161 (ref/nv2a, form 1).
- **Measured (2026-10-08, x9-perf-particles, divergences.md #158): not
  built.** xemu, scripted combat in c1a0c, c1a0e, c1a1b and c2a2a: 45-71
  particles a frame (peak 216), the whole `CL_DrawParticles` pass
  0.04-0.06 ms a frame, of it about 0.015 ms `CL_ThinkParticle`. A CPU
  alternative (quads built directly, appended 64 at a time) was
  pixel-identical and gained nothing measurable, so it was withdrawn too.
  The recorder's `part_kc`/`part_think_kc` give the console's split.

### R7: MMX in the software mixer's final stages, gated on `snd_kcyc`

- **Bound:** CPU. **Target:** Xbox.
- **Evidence:** `S_WriteLinearBlastStereo16` clamps 32-bit to 16-bit per
  sample (`engine/client/sound/s_mix.c:497-504`), which is `packssdw`'s job.
  `S_MixBufferWithGain` adds and scales 32-bit pairs (`:477-495`).
  Accumulators are 32-bit, so 16-bit saturating adds (`paddsw`) do **not**
  apply to the accumulation. A -Os object (`.sweep-os/
  engine_client_sound_s_mix.obj`) has no MMX instructions.
- **Change:** MMX (or SSE1 integer-in-MMX-register) versions of the two loops,
  plus `emms`. Interleaving MMX with x87 code costs a mode switch, so keep the
  loops self-contained.
- **Expected gain:** unmeasured and probably small. Mixing volume per frame is
  paint-ahead samples times channels. Companion M2's `snd_kcyc` decides.
- **Risk:** exact integer semantics must match `CLIP16`. A host test against
  the C loop settles that.
- **Measured by:** hardware-only (`snd_kcyc`).
- **divergences.md:** upstream file. Form 2: a new
  `engine/client/sound/s_mix_mmx.c`, selected under `#if XASH_XBOX` at the two
  call sites (form 3, two lines). A candidate to send upstream.

### R8 (demoted, do not build unless `lm_kcyc` leads): per-style lightmap pages combined in the register combiners, or MMX in `nv2a_lm_accumulate_styles`

- **Bound:** CPU. **Target:** Xbox.
- **Evidence:**
  - The rebuild runs only when a style changes, in both trees (`gl_rsurf.c:1413`;
    `r_lightmap.c:685`, `:730`; `nv2a_lightmap_util.c:23-34`).
  - It is capped at 16384 luxels per frame (`r_lightmap.c:110`).
  - A rebuild measured 128.9k cycles median in xemu at -O2
    (`tools/xbox-opt.sh:24`). xemu's TSC is not the P3's clock, so this only
    indicates the order: roughly 0.2 ms if it transferred.
  - The inner loop (`nv2a_lightmap_util.c:60-104`) is u8 × scale into u32.
    MMX `pmaddwd` fits it.
  - The combiner option needs baked per-style pages (`xbox-performance-review.md`
    §2.1). The world pass uses 2 texture stages today, so a face with N
    styles needs N+1 stages, or multipass.
- **Expected gain:** small, bounded by `lm_kcyc`.
- **divergences.md:** amend #134 if built.

### R9 (pointer, owned by companion P4): pose memoisation

The referee confirmed that the reference is eager. In HL gameplay, though, the
eager setup that actually runs is the SDK client's
(`StudioModelRenderer.cpp:780`), not `r_studio.c:1042`. Any memo belongs in the
SDK fork (companion P4 step 3), keyed on every input `StudioCalcRotations`
reads, gait included (`:939`). A memo in ref/nv2a would not touch the default
path.

### R10 (not recommended now): static pushbuffer segments for the world

- The hardware has a DMA subroutine register, and pbkit saves and restores it
  (`NV_PFIFO_CACHE1_DMA_SUBROUTINE`, `deps/nxdk/lib/pbkit/pbkit.c:710`,
  `:743`). pbkit exposes no call/return API.
- The world's geometry is already static. What is rebuilt each frame is the
  visibility walk (R3) and the run list. The pushbuffer dwords are a small
  CPU cost next to the walk.
- Revisit only after R3, and only if `pb_dwords` correlates with CPU time on
  hardware.

### Status items (no work; listed so nobody re-proposes them)

- **Floating-point model.** PM, physics and render math already run as scalar
  SSE single precision (finding 4).
  - Do not add `-mfpmath=387`. Do not "move PM to SSE": it is already there.
  - Compared with a 32-bit GCC PC build, which defaults to x87 on i386 (GCC's
    documented default; I did not check it against a built PC object here),
    float intermediates round differently. A 64-bit upstream build uses SSE,
    as the Xbox does.
  - **divergences.md:** a one-line note on the build-flags row: "float is
    scalar SSE (clang i386 with SSE1), double and the float return ABI are
    x87". It changes no code, and it lets a later physics-parity
    investigation find it.
- **Kernel SSE state.**
  - CR4.OSFXSR is effectively proven: the -O2 XBE executes SSE throughout and
    ran on hardware.
  - The kernel's thread context is the 512-byte FXSAVE layout, with `MXCsr`
    and `XmmRegisterArea[128]` (`deps/nxdk/lib/xboxkrnl/xboxdef.h:52-69`).
    That is strong evidence that XMM state is saved per thread, but I did not
    read the kernel code.
  - OSXMMEXCPT only matters if MXCSR exceptions are unmasked, and nothing
    here unmasks them.
  - Never put SSE in a pbkit DPC/ISR callback: "user callback must be brief
    and preserve fpu state" (`pbkit.c:253`).
- **The world draw is already batched.** It does a lazy per-batch texture
  bind and merges runs (`r_context.c:2974-3276`).
- **The studio skin cache is already budgeted per frame**
  (`nv2a_skin_cache.c`).

## 3. CPU-bound plan (Xbox)

### Measure first

1. **M-offload (xemu, exact counts).** Run the route maps c1a0c, c1a1, c1a1a,
   c1a0e and c1a0b with recorder mode 1. Mode 2's overlay costs 0.87 ms per
   frame in xemu (`frametime-recorder.md:196-198`).
   - Read `studio_tris`, `studio_strip_verts`, `studio_lit_ents`,
     `tri_appends`, `world_faces_walked`, `world_faces`, `water_verts`,
     `particles` and `lm_texels`.
   - Confirm the scene first: map name and entity list (`lessons.md`).
2. **Census (offline).** Run R0's bone and tricmd census over the retail
   `.mdl` set.
3. **M-time (hardware).** Take one v2 route recording with companion M2's
   spans plus R0's `world_walk_kcyc`, `water_kcyc` and `particles_kcyc`.

### Ordered steps

1. **R3 step 1** (swap the operands). It is one line, cannot change the
   picture, and gives a clean before/after on `world_walk_kcyc`.
2. **Companion P1** (fast `memcpy`). It changes the baseline that R2 and R1
   are measured against.
3. **R2** (per-`Begin` state resolution, native strips), if
   `studio_points_kcyc` + `particles_kcyc` is a leading term.
4. **R3 step 2** (visible-face list), if `world_walk_kcyc` after step 1 is
   above about 1 ms on c1a0c.
5. **R1**, if `studio_points_kcyc` on c1a0c is still the largest ref/nv2a
   span after R2, and the census shows that the meshes on c1a0c fit the
   ~45-bone palette or need few splits. R1 is a design-stage item:
   `docs/r3d/design-stage.md`, specialist review, and a pixel diff against
   main.
6. **R4**, if `client_studio_kcyc` leads after companion P4's `fsincos`.
7. **R5, R6, R7, R8**, each only if its own counter or span is non-trivial on
   a worst map.

### Exit criterion

The bar is the same as the companion's, so there is one bar. On real hardware,
same route, -O2, recorder mode 1:

- **c1a0c:** mean CPU at or below **20.0 ms** (now 26.1), and frames over
  33.3 ms at or below **15 %**.
- **Every route map:** mean CPU at or below 20.0 ms.
- **Offload-specific:** if R1 ships, `studio_points_kcyc` on c1a0c falls by at
  least **70 %**, and GPU wait stays below **1.0 ms/frame mean** (the
  report's GPU-bound line).

### If the number is missed

- **Studio span still leads after R1:** check `studio_lit_ents`. If the
  fallback share is large, port the elight sum to the VP. That is up to
  4 lights, with `LinearGammaTable`/`ScreenGammaTable` as two more LUT stages
  or a combiner approximation. It is a fidelity decision, so it needs the
  reference specialist.
- **`client_studio_kcyc` leads:** companion P4 (`fsincos`, skip no-op
  interpolation), then R4, then the SDK memo (R9).
- **World walk leads after R3:** go to baked per-cluster, per-material lists
  (`xbox-performance-review.md` §4.1).
- **None of these leads:** the frame is outside this research (server game
  DLL, delta encoding, prediction). Return to `perf-research` with that span.
  Do not guess.

## 4. GPU-bound plan

The measured frame is not GPU-bound. This section covers what offloading adds
to the GPU, and the limits to watch. All GPU time is hardware-only. xemu's GPU
timing is not the NV2A's, and pbkit has no GPU timestamp.

### Fill

- R1 adds one point-sampled LUT stage per studio pixel. R5 and R6 add none.
- No current fill problem: GPU wait is under 1 ms on every map.
- Watch GPU wait on c1a0c and on any map with large close-up NPCs.
- Exit: GPU wait mean stays below 1.0 ms/frame after R1.

### Vertex

- R1 replaces the trivial `vs_tri` program with a skinning program. Estimated
  length: about 25-40 instructions for the position, normal, hemispheric
  lambert and LUT-coordinate outputs. That is well under the
  136-instruction limit.
- Vertex count per triangle falls, because indexed lists replace expanded
  strips.
- R5 and R6 add short programs.
- Whether VP throughput becomes the limit on a crowded scene is
  hardware-only. Measure GPU wait with a crowd scene, for example c1a0e's
  chamber with the entity list confirmed.

### State/DMA

- On pbkit this is CPU time spent writing pushbuffer dwords
  (`xbox-performance-review.md` §1.4).
- R1 moves vertex data out of the per-frame ring, which today costs 72 bytes
  per triangle (`r_context.c:7326`). It adds per-draw constant uploads: about
  16 dwords per bone through `NV097_SET_TRANSFORM_CONSTANT`, as the world's
  own uploads do (`r_context.c:2521-2522`). It also adds a program and combiner
  switch per studio batch.
- R2 raises the `prims` count when strips go native.
- Track `pb_dwords`, `prims`, `draws` and `tex_binds`, exactly in xemu.
- Exit: `pb_dwords` on c1a0c does not rise by more than the ring bytes the
  change removed.

## 5. Memory and streaming

| Item | Bound | Evidence | Status / action | Measured by |
|---|---|---|---|---|
| R1 static studio vertex buffers | memory | Contiguous memory, outside the heap (AGENTS.md: one ledger, two allocators) | **New ledger line.** Estimate per model from the R0 tricmd census: unique tuples × about 28-36 B plus u16 indices. Hold them in an LRU with a fixed cap, like `NV2A_STUDIO_SKIN_CACHE_BYTES`, and pin the cap in `Nv2aMapFit.h` next to `kXboxStudioStateBytes` (`r_studio.c:151-156`). The cap comes out of the free memory at map start: 22-26 MiB at -O0 (`docs/r3d/fork-plan.md:8875`, `:8887`); the companion gives 15-20 MiB at -O2, which I did not re-verify | xemu counts plus `MmQueryStatistics`; the hardware route confirms |
| R5 static water buffers | memory | Copies of engine-owned polys | Small; size it from `water_verts` | xemu |
| `mem-refresh-headroom-postO2` (lens) | memory | `fork-plan.md:8896-8901` ("re-run the route on hardware before using these numbers") | Keep. Measurement only, and it decides how large R1's cap can be | xemu or hardware (memory, not time) |
| `mem-mpak-fd-session-hold` (lens) | memory / robustness | `fork-plan.md:8905-8917` | Keep, but it is not a frame-time item. Open on demand in `filesystem/mpak.c` (our file, form 2); check load-hitch cost | `MmQueryStatistics`, MAP-chunk load time |
| `mem-mip-chain-full-depth-unmeasured` (lens) | memory | `r_context.c:236-241` | Speculative; census first, no engine work | bake-time census |
| `mem-budget-guard-note-only` (lens) | — | `r_context.c:4203-4221` | Scope note: the runtime guard is a backstop, and `Nv2aMapFit.h` is the ledger. Not new work | — |

## 6. Refuted claims (do not propose these again)

**Refuted by the referee:**

1. **`cpu-p3-lightstyle-already-cached`: "the change-detection cache is NOT
   present in the stock reference".**
   - Refuted by `ref/gl/gl_rsurf.c:1413`: `R_CheckLightMap` compares against
     `cached_light`, refreshed at `:1438`.
   - `nv2a_lightmap_util.c:23` labels its own function "R_CheckLightMap,
     gl_rsurf.c:1410-1414".
   - What nv2a really adds: it refreshes the cache for flicker styles too.
     ref/gl rebuilds those every frame through `dynamic_surfaces`
     (`gl_rsurf.c:1462-1465`). It also adds the luxel budget (#134).
2. **`gpu-nv2a-combiner-lightstyle`: "four style layers combined every
   frame".**
   - Refuted by `gl_rsurf.c:1413`/`:1438` (switchable styles recombine only
     on change) and `r_lightmap.c:685`, `:730` (nv2a rebuilds only when
     `nv2a_lm_styles_changed`).
   - The "-O0, 2-3 ms" estimate it leaned on (`r_lightmap.c:103-107`) is
     stale: the build is -O2 (`tools/xbox-opt.sh:35`).
3. **`mem-style-samples-duplicate`: "ref/gl never bakes an atlas" and "read
   every frame".**
   - Refuted by `ref/gl/gl_rsurf.c:3809-3838`: `GL_CreateSurfaceLightmap`
     builds atlas pages at load, while the raw lighting stays resident
     (`engine/common/mod_bmodel.c:4037`). The reference co-resides both
     copies too.
   - `samples` is read only on a rebuild (`r_lightmap.c:685`, `:730`).

**Refuted in this merge [planner]:**

4. **`engine-hotpaths-drop-cpu-backface-cull`: "gate off the world pass's
   per-face CPU backface test".** It is already off by default:
   `static CVAR_DEFINE_AUTO( nv2a_r_nocullback, "1", …)` (`r_context.c:768`),
   tested at `:3157`. The GPU already does all backface culling. The open
   question about the correctness of turning it **on** (divergence #98) is a
   GPU-index-bandwidth question, and it is moot while the GPU is idle.

**Sub-claims corrected (the candidate survives, retargeted or reworded):**

5. **"Bone-setup memo / SSE bone concat in `ref/nv2a/r_studio.c:1042`/`:1180`".**
   Not on HL's gameplay path. The evidence is `r_studio.c:3112-3126` and
   `:160`, and `cl_game.c:3973-3979`. The live code is
   `deps/hlsdk/cl_dll/StudioModelRenderer.cpp:780` and
   `deps/hlsdk/pm_shared/pm_math.cpp:396` (R4, R9).
6. **"Water warp: replace the CPU sine with a VP polynomial sine".** Neither
   tree calls a sine per vertex. Both read `r_turbsin` (`gl_rsurf.c:960-964`;
   `nv2a_warp_util.c`). The opposite conclusion, "already cheap", does not
   follow either (referee). Per vertex the CPU still does 4 lookups, 2 dot
   products and a transform (`r_warp.c:340-350`).
7. **"The Xbox build does float math on x87; moving PM to SSE is a fidelity
   risk".**
   - Refuted by disassembly of the -O2 objects:
     - `.sweep/engine_common_pm_trace.obj`: 385 scalar-SSE instructions,
       1 x87;
     - `.sweep/public_matrixlib.obj`'s `Matrix3x4_ConcatTransforms`: 36
       `mulss`, 27 `addss`, 48 `movss`, 0 x87.
   - No `-mfpmath` flag is passed (`tools/xbox-cc.sh:474-480`).
8. **"`CL_DrawParticles` lives in `cl_efx.c` and is shared by every backend",
   and "one Begin/End per particle".**
   - Each backend has its own (`ref/gl/gl_rpart.c:48`;
     `ref/nv2a/r_effects.c:1439`).
   - There is a single `Begin` for the whole list (`r_effects.c:1453`).
9. **"`R_LightLambert` is per vertex in `R_StudioDrawPoints`".** It runs per
   *emitted strip vertex*, from `R_StudioSetColorBegin`
   (`r_studio.c:1711-1719`), at emit time.
10. **Citation errors in the lenses.**
    - `optimization-transfer.md:33-50` is the `R_CullBox`/`r_vbo`
      correction. The bone-memo correction is `xbox-performance-review.md:33-50`.
    - `r_studio.c:2922`/`:3059` are the player and non-player entry points,
      not an "RVP pass".
    - The VP includes are at `r_context.c:1245`/`:6678`/`:6722`.
    - The `-march=pentium3` comment is at `tools/xbox-cxx.sh:84-88`.

## 7. Unverified until hardware

- The real per-subsystem split of c1a0c's 26.1 ms (all spans; time is
  hardware-only).
- VP20 throughput for R1's program on a crowded scene, and the GPU wait after
  R1.
- That a VP using A0.x relative constant addressing and the LUT-stage
  combiner setup behave the same on hardware as in xemu. xemu accepts register
  words that hardware rejects (`lessons.md`).
- The ±1 LSB fidelity claim for the LUT path against `R_StudioLighting` and
  the `VectorScale(color, 255)` truncation (screenshot diff on hardware).
- The P3 cycle cost of the TriAPI per-append overhead (R2), and of packed
  SSE1 `ConcatTransforms` against scalar SSE (R4). xemu does not model µop
  splitting or store forwarding.
- Whether NV2A point sprites (R6b) generate texture coordinates on the stage
  the particle path uses, and what point size limit they have.
- Whether the kernel saves XMM state on every thread switch. The struct
  layout says yes (`xboxdef.h:52-69`); the kernel code was not read.
- Free memory at map start on the -O2 build: 15-20 MiB per the companion,
  versus 22-26 MiB at -O0 per `fork-plan.md`. It bounds R1's buffer cap.
- Whether `nv2a_2d_desired` is ever written between `Begin` and `End` (R2's
  precondition). This is a code question, and it is settled with a debug
  assert in xemu.
- Retail bone counts per mesh against the ~45-bone palette. This needs the
  census; it is offline, not hardware.
- Whether any worst-route map has open water (`water_verts`, xemu).
- Whether `pm_math.cpp`'s `ConcatTransforms` is also linked into the server
  game DLL, so that R4 must be client-only. This needs the link map.

## 8. What this research did not check

- The server game DLL (`SV_RunGameFrame`: AI, physics, traces). On a listen
  server it is a plausible leading term on NPC-heavy c1a0c, and no offload or
  SIMD item here touches it.
- Client and server delta encoding, `CL_RedoPrediction`, `CL_EmitEntities`,
  and HUD/VGUI CPU cost beyond the `v_2d` counter. Sky (`ref/nv2a/r_sky.c:168`
  `R_AddSkyBoxSurface`, the same per-face clip as `gl_warp.c`) and decals
  (`decals`, `v_decal`) were not costed. Both have counters, and neither
  looked like an offload candidate.
- Beams (`r_effects.c:392-395`, `:533-567`, `:758-800`) and sprites
  (`ref/nv2a/r_sprite.c:686-693`). The billboard math was read. Beams' noise
  displacement stays on the CPU, so they are not R6 candidates as written.
- Denormal handling. Denormal SSE operands on a P3 take a microcode assist.
  Whether bone or lighting math produces them, and whether setting MXCSR.FTZ
  is acceptable, was not examined. FTZ changes results.
- Whether pdclib's build defines `MALLOC_ALIGNMENT` on the command line,
  overriding the 8-byte default at `malloc.c:704`.
- The audio backend's threading on Xbox: which thread runs `S_PaintChannels`,
  and whether a second thread uses SSE.
- The Pentium III PC target. One flag to check before anyone builds for a P3:
  upstream's x86 build passes `-march=pentium-m` (SSE2) unconditionally for
  gcc/clang (`scripts/waifulib/compiler_optimizations.py:283-289`). Whether
  the compiler then emits SSE2 instructions a P3 cannot run was not tested.
- The XEMEX pushbuffer sniffer was not used. It could confirm R2's prims and
  dword counts against a live capture.
