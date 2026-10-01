# Perf research: CPU-bound frame on the Original Xbox at -O2

Run: `perf-research`, scenario `cpu`, target `xbox`, slug `xbox-cpu-bound-o2`.
Base commit `5f7349d6`. Inputs: two survey lenses (memory/streaming and
console-performance/Pentium III), the reference-renderer referee's verdicts,
and the planner's own reading of the tree. Every `file:line` below was opened
for this report. Items the planner added, which no lens or referee saw, are
marked **[planner]**.

## 1. Summary and targets assumed

**The measured problem.** This is the real hardware recording from PR #169,
taken with the -O2 engine (`tools/xbox-opt.sh`, `XBOX_OPT=-O2` on every
`xbox-cc.sh`/`xbox-cxx.sh` compile). It covers 21.4 minutes, c0a0 to c1a1a,
at 41.7 fps on average. GPU wait is 0.4-0.6 ms/frame on every map and CPU
time is 11-26 ms/frame, so the frame is CPU-bound everywhere. The worst map
is c1a0c: 27.9 fps, 26.1 ms CPU, 57% of frames over 33.3 ms. Every map also
has single frames of 350-1000 ms.

**Build levels behind the numbers** (`lessons.md`: write the level next to
the number):

- **Engine, ref/nv2a and the game DLLs: -O2.**
- **nxdk's prebuilt libraries: -O0.** `make NXDK_ONLY=y` passes no `CFLAGS`
  (`docs/r3d/fork-plan.md:10183-10186`). The current link map
  (`.sweep/game-hl/frametime.map`, 2026-10-01) resolves `_memcpy`,
  `_memset`, `_memmove`, `_memcmp`, `_strlen`, `_strcmp`, `_qsort`,
  `_malloc`/`_free`, `_sqrtf`, `_sin` and `_cos` from `libpdclib`.

**Targets.**

- **Console:** the Original Xbox. 733 MHz Pentium III (SSE1, MMX, no SSE2),
  NV2A, 64 MiB unified memory, 15-20 MiB free at map start.
- **Pentium III PC:** out of scope for this run. It uses ref/gl, the host
  libc and a different GPU. The only items that carry over are the SSE1
  math ones (P5, and the separate SIMD research run).

**Main findings.**

1. **[planner] Every `memcpy` and `memset` in the XBE runs a -O0 byte loop.**
   The built `memcpy.obj` executes about 16 instructions per byte and keeps
   `n`, `src` and `dst` on the stack (disassembly of
   `deps/nxdk/lib/pdclib/functions/string/memcpy.obj`; source at
   `functions/string/memcpy.c`). Both compile drivers pass `-ffreestanding
   -fno-builtin` (`tools/xbox-cc.sh:478-480`, `tools/xbox-cxx.sh:202-204`),
   so the compiler cannot inline even a small fixed-size copy. Every
   TriAPI primitive goes through `memcpy`: 72 bytes per studio triangle
   (`ref/nv2a/r_context.c:7326`). This is the cheapest broad CPU item found.
   Its size is unmeasured.
2. **The lenses' bone-setup items point at code Half-Life does not run.** HL's
   client exports `HUD_GetStudioModelInterface`
   (`deps/hlsdk/cl_dll/GameStudioModelRenderer.cpp:102-119`), and
   `R_StudioDrawModelInternal` hands every in-world studio model to it
   (`ref/nv2a/r_studio.c:3119-3126`) unless `r_studio_builtin_renderer`
   (default `"0"`, `r_studio.c:160`) is set. So per-frame bone setup runs in
   `CStudioModelRenderer::StudioSetupBones`
   (`deps/hlsdk/cl_dll/StudioModelRenderer.cpp:780`), not in ref/nv2a's
   `R_StudioSetupBones` (`r_studio.c:1042`). ref/nv2a does own the
   per-vertex work, through `R_StudioDrawPoints` (`r_studio.c:2005`).
3. **The spikes are not attributed yet.** The recorder flags each frame by
   the state it starts in (`engine/platform/xbox/xbox_frametime.c:418-424`).
   The frame that runs a `changelevel` therefore starts active and is
   counted as in-play for the map it leaves. `frametime-recorder.md` already
   puts that frame at 250-530 ms. "One 350-1000 ms spike on every map" fits
   that frame, an autosave frame, or a skin or sound read. Check before
   building anything (plan step M0).
4. **Synchronous I/O on the frame thread exists and is confirmed by the
   referee.** Studio-skin re-reads (`ref/nv2a/r_image.c:912`) and
   late-precache sound loads (`engine/client/sound/s_load.c:123-125`) both
   block the frame. **[planner]** `trigger_autosave` (SDK
   `deps/hlsdk/dlls/triggers.cpp:1983`) and its queued `saveshot`
   (`engine/server/sv_save.c:1791`) do too.
5. **Moving work to the NV2A is right in principle and large in practice.**
   The studio per-vertex path (transform, lighting, TriAPI emit) is the one
   big candidate. Water, particles and lightstyles are gated on counters
   that already exist. See section 4.

## 2. Ranked improvements

Ranking is by expected CPU ms removed on the worst maps, weighted by
confidence and cost. Spike items are ranked by the spike they remove. No
expected gain below is a measurement. Each one gives its basis.

### P1 [planner]: Replace pdclib's -O0 byte-loop `memcpy`/`memset`/`memmove`, then rebuild nxdk at -O2

- **Bound:** CPU. **Target:** Xbox only. A PC build uses the host libc.
- **Evidence:**
  - Disassembly of `memcpy.obj` and `memset.obj` shows a byte loop with
    three stack-slot read-modify-writes per byte.
  - The link map resolves both from `libpdclib`.
  - `-ffreestanding -fno-builtin` in `tools/xbox-cc.sh:478-480` and
    `tools/xbox-cxx.sh:202-204` turn every `memcpy` into a call.
  - Hot callers include `nv2a_2d_append`'s `memcpy`
    (`ref/nv2a/r_context.c:7326`). It runs once per TriAPI primitive:
    24-byte vertices, so 72 bytes per triangle, for studio meshes
    (`R_StudioDrawNormalMesh`, `r_studio.c:1888-1912`), particles, beams,
    sprites and water.
  - The compiler also emits `memcpy` calls for large struct copies, for
    example `*nv2a_studio_ctx.currententity = saveent`
    (`r_studio.c:2993`, in the `weaponmodel` branch at `:2981`).
- **Change:**
  1. Add a fork-owned `compat/xbox/xbox_string_fast.c` (or `.S`) that
     defines `memcpy`, `memset` and `memmove` with dword or `rep movsd`
     bodies plus a byte tail. Because it is linked as an object, lld never
     pulls the archive member.
  2. Separately, rebuild nxdk's libraries with `CFLAGS=-O2`, the next step
     `fork-plan.md:10186` already names. That covers `malloc`/`free`,
     `qsort`, `strlen`, `strcmp` and pbkit's out-of-line `pb_push1`.
  3. Optional and unverified: let small fixed-size copies inline. Whether
     clang honours `-fbuiltin` after `-ffreestanding` must be checked by
     disassembling one object first.
- **Expected gain:** unmeasured.
  - Basis: at an assumed 5-7 cycles/byte for the -O0 loop (P6
    store-forwarding chains on three stack slots; not measured) against
    about 0.5 cycles/byte for a dword copy, each 10,000 TriAPI vertices
    (240 KB) cost about 1.6-2.3 ms of the frame at 733 MHz.
  - Add every other `memcpy`/`memset` in engine, client and server on top.
  - The counters `v_studio + v_tri + v_2d + v_decal` give the vertex volume
    exactly.
- **Risk:**
  - A wrong `memmove` overlap direction corrupts data silently, so
    host-test it against the C library.
  - Rebuilding nxdk at -O2 can expose undefined behaviour in nxdk itself
    (`lessons.md`: diff the picture at a second optimisation level).
  - The tree has a precedent of nxdk functions that do not do what their
    declaration says (`lessons.md`, pdclib `%f`), so read each replaced
    symbol's callers' expectations.
- **Measured by:**
  1. xemu: a build flag that makes the new `memcpy` add its byte count to
     a new `memcpy_bytes` work counter (`xbox_frametime_util.h:106-130`,
     appended before `XBOX_FT_W_COUNT`). Counts transfer from xemu, so xemu
     settles the volume. Run it on c1a0c and c1a0e.
  2. Hardware only: a one-boot `rdtsc` microbenchmark of 64 KiB through
     each implementation, for cycles per byte.
  3. Hardware only: a v2 route recording, comparing CPU ms per map.
- **divergences.md:** new row: "`compat/xbox/xbox_string_fast.c` (new
  file, our directory): replaces pdclib's -O0 `memcpy`/`memset`/`memmove`
  at link. Why: byte loops at -O0 on every copy. Form 1. Merge risk none.
  Exit: nxdk ships optimised string functions." The nxdk rebuild flag is a
  build recipe change (`tools/xbox-env.sh` or the setup script), not an
  upstream-code change. It still needs a `fork-plan.md` note and an XBE
  size and memstats line.

### P2: Take the studio-skin disk read off the frame thread (`memstream-skin-async-read` + `cpu-p3-skin-disk-read`, referee: confirmed)

- **Bound:** I/O (spike). **Target:** Xbox.
- **Evidence:**
  - `nv2a_tex_skin_read_raw` (`ref/nv2a/r_image.c:760-775`) does a
    synchronous `Open`/`FileLength`/`Seek`/`Read`/`Close`.
  - `nv2a_tex_upload_skin` (`:851`) calls it inline at `:912`, inside
    `GL_Bind`'s `nv2a_tex_resolve_skin` (`:960`).
  - The allowance limits how many reads run per frame (2 uploads or
    256 KiB, `nv2a_texture.h:103-104`, enforced in
    `nv2a_skin_cache_acquire`, `nv2a_skin_cache.c:191`). It does not limit
    how long each read blocks.
  - The header gives a cold HDD seek of about 10-15 ms and says "not yet
    measured on the console" (`nv2a_texture.h:94-101`).
  - A white placeholder already covers an absent skin (`r_image.c:960-969`,
    `NV2A_SKIN_FRAME_CAP` handling at `:890-891`).
- **Change:** A small nxdk worker thread takes `(path, pixel_offset,
  length)` requests and fills malloc'd raw buffers. `R_BeginFrame` drains a
  completion queue and runs the existing LUT expand into the cache. The
  placeholder is unchanged. Keep the per-frame cap as a cap on completed
  expansions.
- **Expected gain:** unmeasured. It removes up to two blocking seeks and
  reads from any frame that binds new skins: map start, a new NPC group,
  and re-reads after eviction. At the header's own 10-15 ms per seek that
  is 10-30 ms off those frames. It does nothing for steady-state CPU.
- **Risk:**
  - The placeholder shows for longer.
  - The write into write-combined cache memory must keep the
    `NV2A_STUDIO_SKIN_EVICT_MIN_AGE` no-hazard rule (`nv2a_texture.h`,
    "evictable once ... 2 frames").
  - `fsapi` file handles from a second thread: check that the engine's
    filesystem layer is thread-safe for read-only opens before relying on
    it. Unverified.
- **Measured by:** new counters `skin_reads` (count) and `skin_read_kcyc`
  (an `rdtsc` span around `r_image.c:912`, in kilocycles). Count in xemu;
  time is hardware-only, because xemu does not model disk latency.
  Success: on hardware, no frame carries a non-zero `skin_read_kcyc`.
- **divergences.md:** amend row #126 (ref/nv2a is a fork-owned directory,
  form 1): "skin re-read moved to a worker thread; placeholder until it
  lands".

### P3 [planner, partly from `cpu-p3-no-simd-matrixlib`]: Cut the per-vertex cost of ref/nv2a's studio emit path

- **Bound:** CPU. **Target:** Xbox.
- **Evidence:** per studio vertex, ref/nv2a does all of the following:
  - transforms on the CPU (`Matrix3x4_VectorTransform`,
    `r_studio.c:2075-2079`, or the weighted path at `:2062-2064`);
  - computes `R_LightStrength` (`:2078`);
  - lights each normal with `R_StudioLighting` (`:2116-2118`, the
    function at `:1593`, ending in a `LightToTexGamma` lookup at `:1644`);
  - then, per emitted strip or fan vertex, not per unique vertex, runs
    `R_LightLambert` again through `R_StudioSetColorBegin`
    (`:1711-1720`) before three TriAPI calls (`:1904-1907`).
  - The TriAPI assembler expands strips and fans into triangle lists and
    appends each triangle with a `memcpy` (`r_context.c:7623-7650`,
    `:7326`).
- **Change:**
  1. Have `R_StudioDrawNormalMesh` and its siblings write
     `nv2a_tri_vert_t` triangles straight into the ring, bypassing
     `Begin`/`Color4ub`/`TexCoord2f`/`Vertex3fv`. Same vertices, same
     order, same state.
  2. Cache `R_LightLambert`'s colour per unique `(vertex, normal)` pair
     within a mesh rather than recomputing it per strip vertex.
  3. Later, and measured separately: submit strips as
     `NV097_SET_BEGIN_END_OP_TRIANGLE_STRIP` instead of expanding them, if
     batching allows.
  4. SSE1 for `Matrix3x4_VectorTransform` goes in a fork-owned inline in
     ref/nv2a, not in `public/matrixlib.c:28-33`. The separate SIMD
     research run covers it in depth.
- **Expected gain:** unmeasured. It is bounded by the studio share of CPU,
  which is unknown until the M2 spans exist. The ratio `v_studio /
  studio_tris` (exact in xemu) shows how much the strip expansion and
  per-strip-vertex lighting multiply the work: about 3 means every
  triangle carries three fresh vertices.
- **Risk:** this is a fidelity-sensitive path:
  - Lighting must stay bit-identical. `LightToTexGamma` and the
    `lightvalues[ptricmds[1]]` indexing are reference behaviour
    (`gl_studio.c`, same shape).
  - The chrome, float-UV and glow-shell meshes must keep their own paths.
  - Compare screenshots at two optimisation levels and against main, with
    the baseline booted twice (`lessons.md`).
- **Measured by:**
  - xemu: `v_studio`, `studio_tris` and `pb_dwords` before and after (exact).
  - Hardware only: a new `studio_points_kcyc` span around
    `R_StudioDrawPoints` (`r_studio.c:2005`), then CPU ms on c1a0c and
    c1a0e.
- **divergences.md:** ref/nv2a is fork-owned (form 1). Add a row: "studio
  meshes bypass the TriAPI immediate path, same vertices and order", citing
  `gl_studio.c`'s `R_StudioDrawNormalMesh` for the reference shape.

### P4: Make the client DLL's bone setup cheaper (retargeted `cpu-p3-eager-bone-setup`, referee: confirmed eager)

- **Bound:** CPU. **Target:** Xbox (also any P3 PC running the HL client).
- **Evidence:**
  - `CStudioModelRenderer::StudioSetupBones`
    (`deps/hlsdk/cl_dll/StudioModelRenderer.cpp:780`) runs
    `StudioCalcRotations` up to 9 times per entity (`:831-939`).
  - Each animated bone costs two `AngleQuaternion`s, which are six
    separate `sin`/`cos` calls each (`deps/hlsdk/cl_dll/studio_util.cpp:23-42`),
    plus a `QuaternionSlerp` (`:51`).
  - `ConcatTransforms` is plain scalar C (`deps/hlsdk/pm_shared/pm_math.cpp:396`).
  - `sin` and `cos` are out-of-line pdclib calls wrapping a single x87
    `fsin`/`fcos` (`deps/nxdk/lib/pdclib/platform/xbox/functions/math/sin.c`).
  - The referee confirmed that the reference is eager with no memo
    (`ref/gl/gl_studio.c:815`). An animated bone takes about 16
    transcendental calls (referee, `public/xash3d_mathlib.c:301-320`).
- **Change, in order of cost:**
  1. One `fsincos` per angle instead of separate `sin`/`cos` (inline asm
     helper in the SDK fork).
  2. Skip the `prevframe` interpolation passes when `s` is 0 or 1.
  3. A per-entity pose memo keyed on (sequence, frame, blend, controller,
     gait). This only pays when poses actually repeat, which needs the
     census the lens itself asked for.
- **Expected gain:** unmeasured. Step 1 roughly halves the x87
  transcendental count, because `fsincos` returns both values. The share
  of frame time is unknown until a `client_studio_kcyc` span exists.
- **Risk:**
  - The SDK client's numerics must match bit-for-bit or animation
    visibly drifts. `fsincos` and `fsin`+`fcos` give identical x87
    results, but the double-to-float rounding order must be kept.
  - A stale memo shows as bone twitch.
- **Measured by:**
  - Hardware only: an `rdtsc` span around `pStudioDraw->StudioDrawModel`
    and `StudioDrawPlayer` (`ref/nv2a/r_studio.c:3123-3125`) minus the
    `R_StudioDrawPoints` span, which isolates client-side bone and setup
    time.
  - xemu can count calls (a counter of bones set up per frame), not cycles.
- **divergences.md:** `deps/hlsdk` is the owned fork (precedent: rows #44
  and #116). Add a row naming `studio_util.cpp` `AngleQuaternion` and
  `StudioModelRenderer.cpp` `StudioSetupBones`. Form: change inside
  `#ifdef NXDK` in the SDK fork. Exit: none needed on PC.

### P5: SSE1 for the nv2a vertex transform (`cpu-p3-no-simd-matrixlib`, referee: confirmed scalar)

- **Bound:** CPU. **Targets:** Xbox and a P3 PC (shared math).
- **Evidence:** `public/matrixlib.c:28-33` and `:56-70` are scalar. There
  is no SIMD in `public/`, `engine/`, `ref/` or `common/`, here or in the
  root clone (referee).
- **Change:** defer to the separate SIMD research run. If it lands, do it
  as a fork-owned inline in ref/nv2a's studio path, after P3 has removed
  the call overhead, not by editing upstream `matrixlib.c`.
- **Expected gain:** `optimization-transfer.md` §4 estimates 1.5-2x on the
  transform loop itself, because the P3 splits a 128-bit op into two
  64-bit micro-ops. The share of the frame is unmeasured. Unverified
  (check by disassembling one -O2 object): whether clang at
  `-march=pentium3` already uses scalar SSE for `float` arithmetic.
- **Risk:** low, if it is confined to ref/nv2a.
- **Measured by:** hardware only, the `studio_points_kcyc` span from P3.
- **divergences.md:** none if confined to ref/nv2a. A row if `public/` is
  touched (form 3, `#if XASH_XBOX`).

### P6: Late-precache sound loads off the frame (`cpu-p3-sound-late-precache-disk-read`, referee: confirmed, upstream behaviour)

- **Bound:** I/O (spike). **Target:** Xbox.
- **Evidence:**
  - `S_LoadSound` (`engine/client/sound/s_load.c:102-139`) calls
    `FS_LoadSound` synchronously on a cache miss (`:123`, `:125`) and
    resamples inline (`:130-135`).
  - `s_warn_late_precache` defaults to `"0"` (`s_main.c:57`).
  - The referee found the file byte-identical to the root clone
    `c7768c48`.
- **Change:** count first. Add a `snd_late_loads` counter at `:121` (the
  late-precache branch's condition) and an `rdtsc` span around `:123-125`.
  Only if the counter is non-zero on route maps, move the load to the same
  worker thread as P2, with the channel silent until it lands. That is an
  audible-timing policy decision.
- **Expected gain:** unmeasured. Zero if no route map late-loads a sound.
- **Risk:** sounds start late, and the SDK's sentence and `EMIT_SOUND`
  timing is involved.
- **Measured by:** count in xemu, exact. Time is hardware-only.
- **divergences.md:** upstream file. The counter is one add under
  `#if XASH_XBOX && !XASH_DEDICATED`, the #141 pattern: amend row #141. An
  async load would be a new row, form 3.

### P7: Prefetch the opening view's skins inside the load window (`memstream-skin-prefetch-loadscreen`, not refereed: makes no reference claim)

- **Bound:** I/O. **Target:** Xbox.
- **Evidence:**
  - `nv2a_texture.h:62-70`: c1a0's opening view uses 48 skins in
    0.37 MiB, and the cache is 2 MiB (`:71`).
  - The recorder already excludes each map's first 30 frames
    (`frametime-recorder.md`, "Frame flags").
- **Change:** at the end of `R_NewMap`, issue the P2 path for the skins of
  studio entities already in the client's entity list. Derive this from
  runtime state, not from static map data (`lessons.md`: verify presence
  by observing what the engine requests). Stop at a byte cap well under
  the cache budget.
- **Expected gain:** unmeasured. It moves map-start skin reads into the
  load time.
- **Risk:** load time grows by the reads, and evicting the prefetched set
  wastes them.
- **Measured by:** `skin_reads` (P2) over the first 300 in-play frames, and
  the MAP chunk's load time. Counts in xemu, time hardware-only.
- **divergences.md:** amend #126.

### P8: Size the skin-cache budget from evictions, not by guess (`memstream-skin-budget-census`, not refereed)

- **Bound:** memory. **Target:** Xbox.
- **Evidence:** `NV2A_STUDIO_SKIN_CACHE_BYTES` is 2 MiB (`nv2a_texture.h:71`,
  "tune from texturelist's eviction count, not by guessing").
  `nv2a_tex_skin_report_miss` (`r_image.c:739-754`) only logs.
- **Change:** add `skin_evictions` and `skin_misses` work counters at the
  eviction site in `nv2a_skin_cache_acquire` (`nv2a_skin_cache.c`, the
  `cache->evictions++` in the victim loop) and at `r_image.c:741`. Raise
  the budget only if evictions persist after map start on c1a0c and c1a0e.
- **Expected gain:** none by itself. It decides whether P2's re-reads are
  rare or constant.
- **Risk:** each MiB comes out of the 15-20 MiB free.
- **Measured by:** exact counts in xemu, if the scene is reproduced (same
  route and same NPCs). The hardware route recording confirms it.
- **divergences.md:** amend #126.

### P9: `StudioMergeBones` name-scan table (`cpu-p3-mergebones-string-scan`, referee: confirmed, but demoted)

- **Bound:** CPU. **Target:** Xbox.
- **Evidence:** the live loop is the SDK's: `stricmp` at
  `deps/hlsdk/cl_dll/StudioModelRenderer.cpp:1069`, in `StudioMergeBones`
  (`:1032`). Its callers are `MOVETYPE_FOLLOW` entities (`:1181-1183`)
  and a drawn player's `weaponmodel` (`:1550`). The lens's
  `r_studio.c:982-1034` is the built-in renderer HL does not use (see
  Refuted).
- **Why demoted:** in single player the local player is not drawn in first
  person, so the weapon merge runs only in third person. Route maps rarely
  have `MOVETYPE_FOLLOW` entities. That is unverified, so count before
  building anything.
- **Change:** a per-(model, cached-set) index table, invalidated when
  `StudioSaveBones` refills the cache.
- **Expected gain:** about zero on the route unless the counter disagrees.
- **Measured by:** a merge-call counter in xemu, exact.
- **divergences.md:** SDK fork row, if built.

### P10: Split map load time into disk and CPU (`memstream-maploadtime-breakdown`, not refereed)

- **Bound:** I/O. **Target:** Xbox. Load time only, not frame time.
- **Evidence:** `nv2a_read_map_lumps` (`ref/nv2a/r_context.c:4401`), called
  from `nv2a_load_map_geometry` (`:4471`, read at `:4571`). The MAP chunk
  holds one aggregate load time (`frametime-recorder.md`, "Map changes").
- **Change:** two `rdtsc` spans, the lump reads and the table build, added
  to the MAP chunk.
- **Measured by:** hardware only, since xemu does not model the disk.
- **divergences.md:** ref/nv2a is fork-owned. Amend #141 for the MAP chunk
  fields.

### P11: Fragmentation across transitions (`memstream-fragmentation-across-transitions`, not refereed, demoted)

- **Bound:** memory. **Target:** Xbox.
- **Why demoted:** the proposed source cannot give the number. `zone.c`'s
  `Mem_PrintStats` (`engine/common/zone.c:628`) and `Mem_PrintList`
  (`:649`) sum per-pool totals. The pools sit on `Q_malloc` (`:257`,
  `:277`), so zone.c knows no free runs. A largest-free-block figure has
  to come from pdclib's dlmalloc 2.8.6
  (`deps/nxdk/lib/pdclib/functions/_dlmalloc/malloc.c`; whether
  `mallinfo` is compiled in is unverified) and from the kernel's
  contiguous allocator, which the skin cache and texture arena use
  (`r_image.c:663`, `:1626`).
- **Change:** record `MmQueryStatistics` free pages (already recorded) plus
  the size of one probe `MmAllocateContiguousMemoryEx` that succeeds,
  halving from 8 MiB at each MAP chunk. Free the probe immediately.
- **Measured by:** a repeated-transition run in xemu, which is fine for
  allocator state, then hardware.
- **divergences.md:** amend #141.

## 3. CPU-bound plan (Xbox)

### Measure first

**M0: attribute the spikes from the recording we already have.** No engine
change is needed.

- For each in-play frame over 250 ms, read whether the next frame is
  non-active, meaning it was the frame that ran `changelevel` (frame
  flags, `xbox_frametime.c:418-424`), and whether it is a flush frame.
- Make `tools/frametime-report.py` label "frame followed by a load" and
  exclude it like flush frames, with an `--include-load-trigger` switch.
- If the 350-1000 ms spikes are those frames, they are load cost and
  belong to P10, not to gameplay.
- Spikes on c1a1a, the last map, which has no outgoing changelevel in the
  recording, are the test case.
- Cross-check the console log for `Saving game to` (`sv_save.c:1792`) at
  the spike timestamps.

**M1: work counters in xemu, exact.**

- Route maps c1a0c, c1a1, c1a1a, c1a0e and c1a0b, using the PR #169 v2
  counters: `v_studio`, `studio_tris`, `studio_models`, `v_tri`,
  `particles`, `beams`, `lm_texels`, `water_verts`, `pb_dwords`, `draws`.
- Add P1's `memcpy_bytes`, P2's `skin_reads` and P8's `skin_evictions`.
- Confirm the scene before reading anything (`lessons.md`: confirm which
  map; read the entity list).

**M2: per-subsystem `rdtsc` spans on hardware.**

- Add them as new kilocycle work counters before `XBOX_FT_W_COUNT`
  (`engine/platform/xbox/xbox_frametime_util.h:106-130`). The reader finds
  counters by name, so the report needs no format change.
- Spans:

  | Span | Site |
  |---|---|
  | `sv_game_kcyc` | `SV_RunGameFrame` (`engine/server/sv_main.c:699`): game DLL think, physics, AI |
  | `sv_send_kcyc` | `SV_SendClientMessages` (`:702`): delta encoding |
  | `cl_dll_kcyc` | `clgame.dllFuncs.pfnFrame` (`engine/client/cl_main.c:3908`) |
  | `cl_read_kcyc` | `CL_ReadPackets` (`:3914`) |
  | `cl_pred_kcyc` | `CL_RedoPrediction` (`:3930`) |
  | `cl_emit_kcyc` | `CL_EmitEntities` (`:3936`) |
  | `scr_kcyc` | `SCR_UpdateScreen` (`:3954`), all rendering |
  | `snd_kcyc` | `SND_UpdateSound` (`:3957`), the mixer |
  | `client_studio_kcyc` | `pStudioDraw->StudioDraw*` (`ref/nv2a/r_studio.c:3123-3125`) |
  | `studio_points_kcyc` | `R_StudioDrawPoints` (`:2005`) |
  | `lm_kcyc` | `R_LightmapUpdate` (`ref/nv2a/r_lightmap.c:555`) |
  | `skin_read_kcyc` | `r_image.c:912` |

- Host-side spans go under `#if XASH_XBOX && !XASH_DEDICATED`, amending
  divergence #141. These spans are what make every expected gain in
  section 2 a number.

### Ordered steps

1. **M0, M1, M2.** No optimisation lands before M2 has one hardware route
   recording.
2. **P1** (memcpy/memset), then the nxdk -O2 rebuild. These are broad and
   cheap, and they change the baseline every later item is measured
   against, so they go first. Re-record the route.
3. **P3 step 1-2** (direct studio emit, per-unique-vertex lighting), if
   `studio_points_kcyc` plus the share of `memcpy_bytes` from studio is a
   leading term.
4. **P4 step 1-2** (`fsincos`, skip no-op interpolation), if
   `client_studio_kcyc` is a leading term.
5. Whichever M2 span leads that sections 2 and 4 do not cover. The
   candidates are `sv_game_kcyc` (the server game DLL; nothing in this
   research looked at it), `snd_kcyc` and `cl_read_kcyc`/`sv_send_kcyc`.
   Return to `perf-research` with that span as the focus.

### Exit criterion

On real hardware, same route, -O2, recorder mode 1 (no overlay):

- **c1a0c:** mean CPU at or below **20.0 ms** (now 26.1), and frames over
  33.3 ms at or below **15%** (now 57%).
- **Every route map:** mean CPU at or below 20.0 ms.

The second clause is what holds 30 fps under vsync, with headroom for the
NPC-heavy scenes.

### If the number is missed

- Read which M2 span still dominates c1a0c.
- **If it is the studio path:** the structural item is VP20 skinning and
  lighting (section 4, "NV2A offload"). It is a design-stage project, not
  a patch.
- **If it is the server game DLL:** the lever is `sv_fps`. It is an
  upstream cvar, default `"0.0"`, which couples server physics to every
  client frame (`engine/server/sv_main.c:117`, `:599-621`). Evaluating
  `sv_fps 30` changes simulation stepping, so treat it as a gameplay
  divergence that needs the `xash-reference-specialist`, not as a tuning
  knob. Unverified.
- **If no single span dominates:** the frame is death by a thousand cuts.
  Report the per-span table and stop. Do not guess.

## 4. GPU-bound plan (brief; the measured frame is not GPU-bound)

**Measured state.** GPU wait is 0.4-0.6 ms/frame on every map, below the
report's 1 ms "GPU-bound" line. The NV2A finishes before the CPU on every
map. Nothing in this section is needed to hit the CPU exit. The point of
this section is where CPU work can **move** to the idle GPU.

**Fill.** There is no fill problem today. If P3 or a VP20 path adds overdraw
or multipass, watch `GPU wait` per map. Hardware-only: xemu's GPU timing is
not the NV2A's.

**Vertex.** The vertex units are idle. They are the destination for:

- **Studio skinning and lighting in a vertex program** (owner question).
  - What moves to the GPU: the per-vertex transform (`r_studio.c:2075-2079`),
    the per-normal lighting (`:2116-2118`), the per-strip-vertex
    `R_LightLambert` (`:1711-1720`) and the TriAPI emit with its
    `memcpy`.
  - What stays on the CPU: bone setup, which runs in the client DLL
    (P4); chrome, glow shell and the weighted-bone path until each is
    ported.
  - Constraints already documented: VP20's 192 constants hold about 50
    bones (`optimization-transfer.md` §3; `MAXSTUDIOBONES` 128), so a
    bone-count census of retail models comes first. Static per-model
    vertex buffers cost GPU memory out of the 15-20 MiB free.
  - **[planner]** `LightToTexGamma` (`r_studio.c:1644`) is a table lookup
    with no VP20 equivalent. It needs a fitted polynomial, which changes
    the picture and is a divergence, or a different split.
  - Measure: `v_studio`, `studio_tris` and `studio_points_kcyc`.
- **Water** (owner question). `r_warp.c:340-376` warps every water vertex on
  the CPU (sine per vertex, `nv2a_warp_wave_z_frame`) and emits it through
  TriAPI. A VP20 deform is the `xbox-performance-review.md` §3 row, low
  priority. Gate on `water_verts` in M1. None of the worst maps is known
  to be a water map. Unverified.
- **Particles** (owner question). `r_effects.c:1453-1495` builds four
  vertices per particle on the CPU, then calls `CL_ThinkParticle` (engine
  physics, which stays on the CPU). A VP billboard removes only the corner
  arithmetic. The emit and `memcpy` cost is P1/P3's. Whether pbkit can
  drive NV2A point sprites is unverified. Gate on `particles`.
- **Lightstyles** (owner question). The CPU rebuild is budgeted at 16384
  luxels per frame (`r_lightmap.c:110`, `:376-383`). Combining per-style
  pages in the register combiners would remove the rebuild, but it needs
  a baker change (style pages, `xbox-performance-review.md` §2.1) and one
  draw per style signature. Gate on `lm_texels` and `lm_kcyc`. Not worth
  designing unless `lm_kcyc` is a leading term.

**State/DMA.** On pbkit every state change and draw is the CPU writing
pushbuffer dwords (`xbox-performance-review.md` §1.4), so this is a CPU
cost. `pb_dwords`, `draws` and `tex_binds` count it exactly in xemu.
`pb_push1` is an out-of-line call in -O0 pbkit; P1's nxdk rebuild covers
it.

**Exit (GPU side).** After any offload, GPU wait stays under 1 ms/frame
mean on c1a0c, the report's GPU-bound threshold. Hardware-only.

## 5. Memory and streaming (the spike sources)

| Suspect | Where | Status | Settled by |
|---|---|---|---|
| The changelevel frame, counted in-play for the map it leaves | `xbox_frametime.c:418-424`; `frametime-recorder.md` (250-530 ms) | **[planner]**, likely, unverified | M0, from the existing recording |
| Autosave plus next-frame `saveshot` | `deps/hlsdk/dlls/triggers.cpp:1983` → `engine/server/sv_cmds.c:490-491` → `sv_save.c:1791` | **[planner]**, unverified | M0 log cross-check; an `rdtsc` span on `SV_SaveGame`, hardware-only |
| Studio-skin re-read | `r_image.c:912` | confirmed synchronous (referee); cost unmeasured | P2 counters; hardware-only time |
| Skin-cache chunk growth mid-frame (512 KiB contiguous alloc) | `r_image.c:663` via `nv2a_skin_cache_grow_and_alloc` (`nv2a_skin_cache.c:164`) | **[planner]**, cost unverified | `rdtsc` span on the grow callback, hardware-only |
| Late-precache sound load | `s_load.c:123-125` | confirmed synchronous (referee) | P6 counter in xemu |
| Recorder flush frames | `frametime-recorder.md` | excluded by default; confirm the spike list was not made with `--include-flush` | Re-run `report` |

Memory headroom is 15-20 MiB free at map start. P8 (skin budget) and any
VP20 static vertex buffers draw on it. P11 is the fragmentation check.

**Exit (spikes).** In a route recording, no in-play frame over **100 ms**
that is not a load-trigger, flush or autosave frame. Autosave frames are
labelled, not hidden.

## 6. Refuted claims (do not propose these again)

The referee refuted no candidate outright. These sub-claims were refuted
or corrected:

1. **"ref/gl's `R_StudioLoadTexture` loads skin pixels via `FS_LoadImage`
   from disk at precache."** Refuted as to mechanism.
   `R_StudioLoadTexture` calls `GL_LoadTexture` (`ref/gl/gl_studio.c:3458`),
   which calls `FS_LoadImage` (`ref/gl/gl_image.c:1393`) on the `.mdl`
   already in memory through `Image_SetMDLPointer`
   (`gl_studio.c:3452-3458`; `engine/common/mod_studio.c:1188-1194`).
   ref/gl does no disk read for a built-in skin. Only an optional
   `materials/*.tga` replacement reads disk, at load
   (`gl_studio.c:3441-3446`). The conclusion stands: ref/gl never re-reads
   a skin per frame.
2. **"Six transcendental calls per bone" in `R_StudioSetupBones`.**
   Corrected: six is the minimum for one static-bone `AngleQuaternion`
   (`public/xash3d_mathlib.h:485-500`). An animated bone takes 16,
   including `QuaternionSlerp`'s `acos` and 3 `sin`
   (`public/xash3d_mathlib.c:301-320`), per `CalcRotations` pass. The
   calls live in `R_StudioCalcBones` (`xash3d_mathlib.c:423`), not in
   `SetupBones`' own loop.
3. **`memstream-skin-async-read`'s reference evidence.**
   `nv2a_texture.h:54-58` and divergence #126 are this port's description
   of ref/gl, not ref/gl. The referee re-checked against ref/gl directly
   (item 1). The claim holds, but cite `gl_studio.c`, not the port's
   comment.
4. **[planner] "Per-frame bone setup and bone merge cost lives in
   `ref/nv2a/r_studio.c:1042` / `:982`"** (lenses `cpu-p3-eager-bone-setup`,
   `cpu-p3-mergebones-string-scan`). Refuted for Half-Life gameplay:
   - The HL client exports `HUD_GetStudioModelInterface`
     (`deps/hlsdk/cl_dll/GameStudioModelRenderer.cpp:102-119`; allowed
     export at `tools/xbox-client-allowed-exports.txt:38`).
   - `CL_InitStudioAPI` installs it (`engine/client/dll_int/cl_game.c:3976-3979`).
   - `R_StudioDrawModelInternal` calls `pStudioDraw` for every world
     render (`ref/nv2a/r_studio.c:3119-3126`) unless
     `r_studio_builtin_renderer` is non-zero (default `"0"`,
     `r_studio.c:160`).
   - The live loops are `deps/hlsdk/cl_dll/StudioModelRenderer.cpp:780`
     and `:1032`/`:1069`.
   - Both items are retargeted (P4, P9). Their reference claim, that the
     reference is eager, stands.
5. **[planner] "zone.c's pool walk can report the largest free run."**
   Refuted: `Mem_PrintStats`/`Mem_PrintList` (`engine/common/zone.c:628`,
   `:649`) sum allocated sizes per pool over `Q_malloc` blocks (`:257`,
   `:277`) and know nothing about free space. See P11.
6. **[planner] Stale comment:** `ref/nv2a/r_lightmap.c:76-78` says "The
   Xbox objects are built at clang's default -O0 (tools/xbox-cc.sh passes
   no -O)". False since PR #163 (`tools/xbox-cc.sh:476`, `"$XBOX_OPT"`).
   The inner-loop style it justifies is harmless, but the claim should be
   corrected the next time that file is touched.

## 7. Unverified until hardware

- Cycles per byte of pdclib's -O0 `memcpy` on the P3, and P1's real gain
  (hardware microbenchmark).
- The cost of a cold HDD open, seek and read for a skin (P2) or sound (P6).
  xemu does not model disk latency.
- Whether the 350-1000 ms spikes are load-trigger frames, autosaves or
  reads (M0 settles most of it from the existing file).
- The cost of a mid-frame 512 KiB `MmAllocateContiguousMemoryEx`
  (`r_image.c:663`).
- The real per-subsystem split of c1a0c's 26.1 ms. Every M2 span is
  hardware-only for time; xemu gives counts.
- Whether nxdk built at -O2 behaves identically (UB exposure).
- Whether the `fsapi` file layer is safe to use from a worker thread (P2,
  P6). This is a code question, settled by reading `filesystem/` before
  building.
- Whether pbkit can drive NV2A point sprites (particles).
- Bone counts of retail models against VP20's palette (a census, xemu or
  offline).
- Whether `sv_fps` decoupling preserves HL single-player behaviour.

## 8. What this research did not check

- **One survey lens returned no usable output.** The cached result is a
  junk test stub (`summary: "test"`, one candidate `test-1` with evidence
  `a:1`) and was ignored. It is probably the engine hot-path
  (`render-architecture-specialist`) lens, or the NV2A lens if the run
  did not drop it for `scenario: cpu`. Either way, no lens surveyed
  world rendering, the TriAPI path, pushbuffer submission or the engine
  outside ref/nv2a. The planner covered part of that gap (P1, P3,
  findings 2-3), not all of it.
- **The server game DLL** (`SV_RunGameFrame`: monster AI, `SV_Physics`,
  traces, route finding). On a listen server it runs every client frame
  (`sv_fps` 0) and is a plausible leading term on NPC-heavy c1a0c. No
  candidate exists for it. M2's `sv_game_kcyc` is the first look.
- Client and server delta encode/decode, prediction (`CL_RedoPrediction`),
  the software sound mixer, `CL_EmitEntities`, HUD and VGUI drawing.
- pdclib's `malloc`/`free` (dlmalloc at -O0) and how often the frame calls
  them.
- Whether clang already emits scalar SSE for `float` at `-march=pentium3`.
  The separate SIMD research run owns this.
- The Pentium III PC target. Nothing here was checked against ref/gl's
  frame on a P3, and P1/P2/P3/P7/P8 do not apply to it.
- How many `trigger_autosave` entities the route maps contain (an entity
  lump grep over the baked maps would answer it).
- Xash's `filesystem/` thread safety, `R_NewMap`'s exact hook point for P7,
  and the fidelity of `fsincos` against the SDK's `sin`/`cos` double path.
  Each is named in its item and was not read here.
