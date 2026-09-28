# X9-X design: studio models in ref/nv2a

Input for roadmap X9 item 14, step 5 (`docs/r3d/fork-plan.md`, "Studio
models"). From the nv2a-xbox-specialist (hardware feasibility,
2026-09-26), extended by the spec stage's own reading of `ref/gl`,
`ref/soft` and `ref/nv2a` and a read-only census of the retail models,
against base commit `98928861` (branch `x9-x-studio-models`). Nothing here
was built or run when it was written. Line numbers drift: search for the
named function. Items marked unverified must be checked on hardware.

Refereed: no (agentic-loop 20260926-174804)

`AGENTS.md` asks for `reference-renderer-specialist` on any renderer
design. That pass has not run on this note; change the line above when it
has.

## 0. What exists to reuse

- **The reference path is CPU skinning with immediate submission.**
  `ref/gl/gl_studio.c` `R_StudioDrawPoints` (`:1951`) transforms every
  vertex on the CPU into world space (`Matrix3x4_VectorTransform` with
  `g_studio.bonestransform[pvertbone[i]]`), computes per-vertex light
  (`R_StudioLighting` `:1324`, `R_LightLambert` `:1378`) and chrome texture
  coordinates (`R_StudioSetupChrome` `:1120`) on the CPU, and submits each
  `mstudiomesh_t` as GL strips and fans (`R_StudioDrawNormalMesh` `:1599`)
  under the world's own modelview. No GPU skinning exists in the
  reference.
- **ref/soft already did this port once.** `ref/soft/r_studio.c` is
  `gl_studio.c` with every `pglBegin/pglTexCoord2f/pglVertex3fv/pglEnd`
  replaced by the TriAPI (`TriBegin( TRI_TRIANGLE_FAN )`, `TriTexCoord2f`,
  `TriVertex3fv`, `TriEnd`, `:1710-1724`) and the GL state calls commented
  out. ref/nv2a's TriAPI now draws for real in 3D (X9-X step 4, verified
  live with `r_showhull 1`): `Begin`/`TexCoord2f`/`Vertex3f(v)`/`End`/
  `CullFace`/`TriRenderMode`/`Color4ub` in `ref/nv2a/r_context.c`, a
  24-byte vertex (position, texcoord, UB colour) appended to the 256 KiB
  ring and drawn with the world's own `nv2a_map_viewproj` and depth
  buffer. The studio port routes geometry through the same internals.
- **Texel coordinates are already handled.** `Begin` captures the bound
  texture's width/height and `Vertex3f` multiplies (`r_context.c`,
  `nv2a_tri_tex_w/h`), so the reference's `s = 1/width` scaling survives
  unchanged.
- **Paletted input is already handled.** `nv2a_tex_process_image`
  (`ref/nv2a/r_image.c`) runs `Image_Process` on `PF_INDEXED_24/32` and
  honours `TF_FORCE_COLOR`; `GL_LoadTexture( name, buf, size, flags )`
  calls `FS_LoadImage` with the buffer, which is what
  `R_StudioLoadTexture` (`gl_studio.c:3373`) feeds through
  `Image_SetMDLPointer` and the engine's `Image_LoadMDL`
  (`engine/common/imagelib/img_wad.c:196`).
- **Lighting helpers are shared.** `R_LightVec`, `R_EntityDynamicLight`,
  `R_GatherPlayerLight` are in `ref/common/ref_light.c` and already link
  into ref/nv2a. Gamma tables come from the engine
  (`PARM_GET_LIGHTGAMMATABLE_PTR` and siblings, as `ref/soft/r_main.c:1378-1381`
  fetches them), elights from `PARM_GET_ELIGHTS_PTR`, the view entity
  from `PARM_GET_VIEWENT_PTR` (`ref/gl/gl_opengl.c:1270`).
- **Frustum culling exists:** `nv2a_frustum_cull_box` (`r_context.c`) is
  the `R_CullBox` that `R_CullModel` (`ref/gl/gl_cull.c:43`) needs.

## 1. Decision: CPU skinning through the TriAPI ring now; VP20 palette later

Port `gl_studio.c` whole into `ref/nv2a/r_studio.c`, submitting through
the TriAPI internals exactly as ref/soft does. This is the reference's
own algorithm, needs no new shader (the existing `vs_tri`/`ps_2d` pair
already does `texture x vertex colour`), and puts every studio feature
(skins, render modes, chrome, lighting, glow shell, events, attachments)
on screen before any performance work.

**Matrix-palette VP20 skinning stays the performance route and is out of
scope for this slice** (specialist, point 1). The plan's own gate stands
unchanged (`fork-plan.md`, the "Bones" bullet under the XB censuses): 192
constants (`deps/nxdk/tools/vp20compiler/config.h:11`), 3 per bone,
roughly 50 bones after view-projection and light constants - an
estimate, not a compiled layout; the retail worst case per mesh is 35
(`v_grenade.mdl`'s `grenade_hand`); do not write bone-subset
partitioning unless a compiled VP20 shader's real constant layout puts
the palette below 35. NV2A has no vertex texture fetch and no indirect
constant addressing beyond the constant file, so a 128-bone palette
(384 constants) cannot fit; do not relax the gate to "skin all 128".

Memoised bone setup (`fork-plan.md`, "Memoised bone setup") is also out
of scope; the reference is eager (`R_StudioSetupBones`,
`gl_studio.c:815`), and the HL client sets up bones itself anyway.

## 2. View-model depth range

Reference: `R_DrawViewModel` (`gl_studio.c:3322`) calls
`pglDepthRange( gldepthmin, gldepthmin + 0.3f * ( gldepthmax - gldepthmin ))`
before drawing the view model (`:3348`) and restores it after (`:3363`);
`gldepthmin`/`gldepthmax` are 0 and 1 outside the developer overview mode (`R_Clear`, `gl_rmain.c:310-319`), which ref/nv2a does not draw.

The specialist (point 2) found that ref/nv2a writes no depth-range
register today and named `NV097_SET_CLIP_MIN`/`NV097_SET_CLIP_MAX`
(`deps/nxdk/lib/pbkit/nv_regs.h:339-340`) as the hardware's equivalent.
In this renderer the viewport transform is not fixed-function: `vs_tri`
multiplies by one matrix and divides by w, and the screen-space z range is
baked into that matrix by `nv2a_matrix_viewport( ..., z_min, z_max )`
(`r_context.c:936`), which the world builds with `0, 65536.0f`
(`r_context.c:4866`). `glDepthRange` is exactly a remap of that range, so
the view model gets a second matrix, built the same way with
`z_min = 0`, `z_max = 0.3f * 65536.0f`, pushed at constant 96 for the view
model's batches only. The clip registers do not need to change: the new
range is a sub-range of the world's, which already renders on hardware
(item 12's frame times were measured on the console). The batch state
needs a "view-model depth range" flag so a change forces a flush and a
constant re-push.

## 3. Skins

- **The specialist's DXT1 concern (point 3) does not arise.** The asset
  pipeline does not re-encode models: `tools/asset-studio/src/main.cpp`
  (the X2 scope comment near `packModelManifest`) records `.mdl` names in
  a manifest and the disc carries the retail files unchanged
  (`tools/xbox-xiso.sh` copies the game directory). Skins reach
  `Mod_StudioLoadTextures` as 8-bit paletted data inside the model and
  leave `Image_Process` as RGBA32, which the skin upload then packs to
  16 bpp (below). Nothing compressed reaches the studio path, and
  nothing is swizzled; the "never Morton-swizzle DXT1" rule applies the
  day skins are swizzled.
- **Linear, clamp-only, no mips.** Skins are uploaded linear
  (`LU_IMAGE_R5G6B5`, or `LU_IMAGE_A1R5G5B5` for `STUDIO_NF_MASKED`)
  with `TEXTURE_ADDRESS 0x00030303` (clamp). Studio texture
  coordinates are texel integers inside the skin in normal content, but a
  coordinate outside `[0,1]` clamps here where ref/gl repeats - a named
  divergence. No mip chain: distant models alias.
- **Memory (census, section 5):** all 225 retail models that carry
  textures total 27.2 MiB at 32 bits per texel; the largest single model
  is `nihilanth.mdl` at 873 KiB, then `big_momgibs.mdl` 708 KiB and
  `hassault.mdl` 553 KiB.
- **Memory, measured - why skins are not uploaded at precache.** The
  first version did what `ref/gl` does (`gl_studio.c`
  `R_StudioLoadTexture`): every skin of every precached model uploaded
  at load, RGBA32. Half-Life's `W_Precache` precaches every weapon's
  `v_`/`p_`/`w_` models, gibs and more on every map, so that is far more
  than any view shows. Kernel free memory after the map loads
  (`tools/xemu-debug.py memstats`, main vs that version):

  | map | main | eager RGBA32 skins |
  |---|---|---|
  | c1a0 | 21.2 MiB | 6.7 MiB (skins: 14.5 MiB) |
  | c1a4 | 19.1 MiB | 5.0 MiB |
  | c2a5 | 17.2 MiB | 1.6 MiB |
  | c4a3 | 13.1 MiB | fails to load |
  | disposal (MP) | 1.7 MiB | fails to load |

  The bake-time fit check approved all of these: `Nv2aMapFit.h`'s
  `kXboxPrecacheBytes` was measured while `Mod_StudioLoadTextures` was a
  no-op.
- **The design now (divergences.md #126).**
  - *Lazy.* `Mod_StudioLoadTextures` registers a named slot per skin
    with no GPU storage (`nv2a_tex_register_studio_skin`), so
    `mstudiotexture_t::index` is still the live, named texnum that
    `cl_remap.c` resolves by name and by `gl_texturenum`. The 8-bit
    pixels are NOT in RAM afterwards (`XASH_LOW_MEMORY` truncates them,
    `mod_studio.c:1105-1116`; a T.mdl's buffer is freed at `:1175`), so
    the slot records the file (`mod->name`, or its `T.mdl`), the pixel
    offset and the file's length. The first `GL_Bind` re-reads
    `w*h + 768` bytes and expands them through a 256-entry 16 bpp table
    built the way `Image_LoadMDL` builds its palette (`LUMP_TEXGAMMA`
    through the engine's texture gamma table, or `LUMP_MASKED`: raw
    colours, index 255 transparent; `img_wad.c:212-221`,
    `img_utils.c:284-310`). It deliberately does not call `FS_LoadImage`
    mid-game: that allocates an RGBA copy from the engine's image pool,
    whose exhaustion is a `Sys_Error` (NV2A specialist review); the only
    allocation here is one `malloc` of the raw bytes, and a failure just
    retries later. A skin over 1024 texels on a side (none in retail)
    still goes through `FS_LoadImage` for the resampler. A file whose
    length changed is never read.
  - *16 bpp.* `R5G6B5`, or `A1R5G5B5` for masked skins
    (`nv2a_skin_cache.c`, round-to-nearest, host-tested). Each
    texture slot carries its colour format, which the batch key carries
    into `NV097_SET_TEXTURE_FORMAT`; 2D/HUD/sprites stay `A8B8G8R8`.
  - *Bounded, backed on demand.* Uploaded skins live in 512 KiB
    physically contiguous chunks (`NV2A_STUDIO_SKIN_CHUNK_BYTES`), each
    allocated only when the chunks already backed have no room, up to
    `NV2A_STUDIO_SKIN_CACHE_BYTES` (2 MiB); no skin ever spills outside
    them, so `Nv2aMapFit.h` charges that budget
    (`kXboxStudioSkinCacheBytes`). Before every map's GPU upload
    (`nv2a_try_pending_upload`, between frames) the whole cache is
    flushed and its chunks given back, so the world's buffers get the
    memory first. At the budget, the cache evicts the least recently
    used skin not drawn for `NV2A_STUDIO_SKIN_EVICT_MIN_AGE` (2) frames:
    1 is the safety floor (`R_BeginFrame` spins `pb_busy` before opening
    a frame), the second avoids evicting what is on screen but not yet
    reached this frame. "Used" is marked on bind AND on every primitive
    appended to the batch, because a texture can stay bound across
    frames. Nothing is evicted unless evicting every eligible skin could
    cover the request.
  - *Placeholder.* A bind whose skin is not resident draws white (lit,
    so the shape reads) - at most 2 uploads / 256 KiB per frame
    (`NV2A_STUDIO_SKIN_UPLOADS_PER_FRAME` / `_BYTES_PER_FRAME`), so a
    scene change spreads its uploads over a few frames instead of
    hitching. Each upload is a file open, seek and read inside
    `GL_Bind`: a cold HDD seek is ~10-15 ms, a DVD one can pass 100 ms,
    and xemu models neither - measure on the console. A skin evicted
    while still bound rebinds the white placeholder, so a draw with no
    new `GL_Bind` is not dropped. A full cache is reported at most once per 300 frames;
    `texturelist` prints the span's use and eviction count.
  - *Lifetime.* `GL_FreeTexture` (from `Mod_StudioUnloadTextures`)
    detaches the skin, defers its bytes to the next safe point like any
    other D4 free, and frees the source record, so a map change leaves
    nothing pointing into the span.
  - *Not covered:* `TF_KEEP_SOURCE` skins (Paranoia2's deluxe data only)
    and player colour-remap copies (`CL_DuplicateTexture`, whose palette
    exists only during that call) keep the eager RGBA32 path.
  - *Why 2 MiB, on demand - measured, not guessed.* The first version
    reserved 4 MiB at R_Init. Reading the texture table live over the
    debugger on c1a0: EVERY skin the map precaches (458) is 4.11 MiB at
    16 bpp, the opening view draws 48 of them in 0.37 MiB, the largest
    is 132 KiB. A reservation that size sat mostly idle while taking
    memory the world upload needs first - disposal's world upload failed
    with it.
- **Measured after (xemu, `memstats` after the map loads, the same five
  maps).** A like-for-like comparison needs the same module composition:
  the `main` XBE the first measurements used was linked WITHOUT the menu
  module (mainui is linked whenever `$SCRATCH/mainui` holds a clean
  mainui sweep, which nothing in the identity sidecar recorded - it now
  carries `mainui=`). The branch XBE had the menu, and the menu's own
  font atlases and Steam background tiles (4.59 MiB contiguous: five
  `stb_font` atlases 2.88 MiB, the `800_*_loading.tga` tiles 1.70 MiB),
  its heap (~0.8 MiB) and its code (0.47 MB) read as a studio-model
  regression. The first table's "eager RGBA32" column is very likely
  inflated the same way (not re-measured); the 4.11 MiB of 16 bpp skins
  above puts eager RGBA32 at roughly 8-9 MiB on c1a0, not 14.5.

  | map | main, no menu | this, no menu | this, with menu |
  |---|---|---|---|
  | c1a0 | 21.24 MiB | 20.25 MiB | 14.48 MiB |
  | c1a4 | 19.5 MiB | 19.08 MiB | 13.26 MiB |
  | c2a5 | 17.5 MiB | 16.55 MiB | 10.63 MiB |
  | c4a3 | 13.2 MiB | 12.29 MiB | 5.87 MiB |
  | disposal (MP) | 8.1 MiB | 7.18 MiB | 1.34 MiB |

  All ten boots reached the map (`nv2a_map_ready` 1, `cls.state` 4).
  The studio models' whole cost on c1a0, by kernel page type: contiguous
  +0.50 MiB (one skin chunk), virtual memory +0.42 MiB (the 384 KiB
  `studio_draw_state_t`, the skin source records and cache entries),
  image +0.07 MiB (code) = 0.99 MiB. The menu, not studio models, is
  what leaves disposal with 1.34 MiB: its fonts and background stay
  resident during play in that measurement. The X9-X follow-up now
  releases fonts and background on low-memory map loads and restores
  them on demand (divergence #127). The earlier renderer-deadlock
  diagnosis was unproven; a later investigation found an OOM while
  booting a stale disc. Pause restores fonts, which remain resident
  after resume until another map loads. Code and other menu allocations
  remain resident. Current checks: `reports/menu-loading-fix.md`.
- **Unload.** `ref/gl`'s `Mod_ProcessRenderData( create = false )` calls
  `Mod_UnloadTextures` -> `Mod_StudioUnloadTextures` (`gl_context.c:111,127`);
  ref/nv2a's `Mod_ProcessRenderData` must do the same for `mod_studio`, or
  skins leak across map changes into the 1024-slot table and the arena.

## 4. Chrome and lighting stay on the CPU (specialist, point 4)

`R_StudioSetupChrome`, `R_StudioLighting`, `R_LightLambert`,
`R_StudioEntityLight`, `R_StudioSetupLighting` compute vertex colours and
texture coordinates before submission. That is the right fit for NV2A (no
per-pixel lighting stage beyond the register combiners) and is copied as
is, not rebuilt as a combiner effect. `R_StudioEstimateFrame`
(`gl_studio.c:561`) and `R_StudioLerpMovement` (`:498`) are CPU-only math
the engine already calls (`engine/client/cl_frame.c:248,1220`) and are
today stubs returning 0 in `r_context.c`; they copy unchanged.

## 5. Studio state size

`gl_studio.c`'s `studio_draw_state_t` sizes its per-vertex arrays by
`MAXSTUDIOVERTS` (16384, `engine/studio.h:36`); `lightpos` alone is
`16384 x 4 x 16` bytes = 1 MiB. Upstream ref/soft caps it at 1024 under
`XASH_LOW_MEMORY` (`ref/soft/r_studio.c:42-45`, also in the root clone),
with no bounds check before `g_studio.verts[i]` is written.

Census (read-only walk of `studiohdr_t` -> `mstudiobodyparts_t` ->
`mstudiomodel_t` over the 310 files in the build-test `valve/models`, 614
submodels): the largest `max(numverts, numnorms)` is 1198 (`garg.mdl`),
then 1128 (`nihilanth.mdl`), 1038 (`loader.mdl`), 1015
(`hassassin.mdl`); three submodels exceed 1024, none exceeds 2048.
Upstream's 1024 would overflow on three retail models. ref/nv2a uses 2048
(the smallest power of two above the measured maximum) and skips, with a
one-time log line, any submodel whose `numverts` or `numnorms` exceeds
it. The GL-only draw-elements arrays (`arrayverts`, `arraycoord`,
`arrayelems`, `arraycolor`) are dropped - submission goes through the
TriAPI ring - which leaves roughly 0.3 MiB.

ref/soft's state is heap-allocated in `R_Init` on Xbox because it is
linked beside ref/nv2a and its static tables cost `.bss` while ref/nv2a
draws (`divergences.md` #114, `R_AllocStudioStatics`,
`ref/soft/r_studio.c:113-125`). ref/nv2a follows the same pattern and
records the size as a resident term beside the texture arena, the vertex
ring and the texture table in `tools/asset-studio/src/formats/Nv2aMapFit.h`
(`kXboxRefNv2aResidentBytes`), pinned from C with a `STATIC_ASSERT` as
`nv2a_texture.h` pins the texture table.

## 6. Render state the studio path needs from the TriAPI batch state

The reference's state changes, mapped to the TriAPI shadow
(`nv2a_2d_desired` and the flush in `r_context.c`):

- `R_StudioSetupRenderer` (`gl_studio.c:2547`): alpha test off.
  `R_StudioRestoreRenderer` (`:2578`): blend off if the render mode was
  not normal.
- `GL_StudioSetRenderMode` (`:2667`): normal - nothing (inherits);
  `kRenderTransColor` - blend `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA`;
  `kRenderTransAdd` - blend `ONE`/`ONE`, depth mask off; default - blend
  `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA`, depth mask on.
- Per mesh (`R_StudioDrawPoints`, `:2090-2123`): `STUDIO_NF_MASKED` -
  alpha test on, `GREATER 0.5`, depth mask on, restored to `GREATER 0`
  (`DEFAULT_ALPHATEST`) and alpha test off after; `STUDIO_NF_ADDITIVE` on
  an opaque entity - blend `ONE`/`ONE`, depth mask off, restored after;
  on a translucent entity - `SRC_ALPHA`/`ONE`.
- Culling: `GL_Cull( GL_FRONT )` is the TriAPI's `CullFace( TRI_FRONT )`;
  the flipped view model (`R_AllowFlipViewModel`, `cl_righthand > 0`)
  draws with culling off; the flip only runs when culling is on
  (`:2058-2076`, the GoldSrc bug note).

New to the batch state: an alpha reference (the flush pushes 0 today) and
`SFACTOR_V_ONE` (`nv_regs.h:257`, 0x0001), neither pushed by this
renderer before. `GREATER 0.5` is `ALPHA_REF 0x7F`: `a/255 > 0.5` holds
for `a >= 128`, the same derivation `nv2a_set_render_mode` uses for
`GREATER 0.25` -> `0x3F` (`r_context.c`, `kRenderTransAlpha`). Masked
skin alpha is 0 or 255 in practice (`Image_LoadMDL`'s index-255 key), so
any reference in `[0,254]` renders identically.

## 7. Draw order

ref/gl (`R_DrawEntitiesOnList`, `gl_rmain.c:797-935`): solid entities
(brush and studio), sprites, `CL_DrawEFX(false)`, `pfnDrawNormalTriangles`,
translucent entities sorted by `R_TransEntityCompare` (brush, studio,
sprite), `pfnDrawTransparentTriangles`, `CL_DrawEFX(true)`,
`R_DrawViewModel`. `R_RunViewmodelEvents` runs before the scene
(`gl_rmain.c:1103`). ref/nv2a today draws brush entities inside
`nv2a_draw_map_geometry` (`nv2a_draw_brush_entities`, both lists) and only
accepts brush models into its lists (`R_AddEntity`). Studio entities join
the same two lists; the translucent comparator gains the reference's
`model->type != mod_brush ||` condition and the studio branch of
`R_GetEntityRenderMode` (`gl_studio.c:2384`); brush draws push GPU state
directly, so the TriAPI ring must be flushed before each and marked dirty
after.

## 8. Pitfalls

- **Ring wraps stall.** 256 KiB / 24 bytes is about 10,900 vertices;
  strips and fans become triangle lists (about 3 vertices per triangle),
  so an NPC of 1,000 triangles is about 3,000 vertices and a view model
  can be twice that. Each wrap flushes and spins `pb_busy()`. Correct,
  but a frame with several NPCs may wrap several times.
- **Pushbuffer.** Vertex data lives in the ring, not the pushbuffer, so a
  studio draw costs a flush's state words plus one `DRAW_ARRAYS` word per
  256 vertices; overflow is still unchecked in release builds
  (textures-and-2D note, section 4).
- **Sprites are step 6.** The glow shell binds the chrome sprite
  (`TriSpriteTexture( GetDefaultSprite( REF_CHROME_SPRITE ))`), and muzzle
  flashes are sprite temp entities; neither sprite draws until step 6.
  The `EF_MUZZLEFLASH` elight and model-event sounds (`pfnStudioEvent` ->
  `HUD_StudioEvent`) do run in this step.
- **Player colour remap** (`DM_Base`/`remap` skins) needs
  `GL_ProcessTexture`, a stub in ref/nv2a; such skins draw with their
  base palette.

## 9. Unverified until hardware

- The view-model depth range as a viewport-matrix remap (z in
  `[0, 0.3 x 65536]`), with `NV097_SET_CLIP_MIN/MAX` left at their
  defaults: argued from the world's range working on the console, not
  observed there.
- `NV097_SET_ALPHA_REF 0x7F` with `GREATER` for masked meshes, and
  `NV097_SET_BLEND_FUNC_SFACTOR_V_ONE` for additive meshes and
  `kRenderTransAdd`: register values this renderer has never pushed on
  hardware (xemu accepts values the hardware rejects, `lessons.md`).
- Strip/fan winding and the `CullFace( TRI_FRONT )` convention for
  studio meshes: the TriAPI's convention was verified in xemu only
  (`r_showhull 1`).
- Frame time of CPU skinning plus ring-wrap stalls with several NPCs and
  the view model on screen, on item 12's ~30 fps maps.
- Linear (unswizzled) skins' texture-cache cost on the GPU.
- The skin cache's numbers: whether 2 MiB holds a busy view without
  thrashing (watch `texturelist`'s eviction count in combat; the
  unattended c1a0 boot never moved past its opening view), and
  whether two uploads per frame hides the re-read cost from the HDD or
  shows as pop-in. Both constants are first choices, not measurements.
- 16-bit linear skins (`LU_IMAGE_R5G6B5`/`A1R5G5B5`) are register values
  this renderer has not pushed on hardware before.
- The VP20 usable palette (~50 bones) is still an estimate, not a
  compiled constant layout.
