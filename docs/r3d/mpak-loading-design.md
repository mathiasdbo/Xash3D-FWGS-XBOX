# Loading a map from `.mpak`: the consumption contract and the work

Research pass, 2026-09-09. No engine or baker code changed. This is the
design the X2 map-loading PRs build on, written after reading the real
loaders, the real consumers (server, `pm_shared`, `ref/soft`, `ref/gl`,
`ref/null`) and the real baker — every claim cites the line it came from
so it can be checked rather than trusted. Section 10 is the work, broken
into packages with exit criteria; section 9 lists the decisions taken
here by recommendation and open to override.

Short version: **the "structural half first, render half later" split in
the plan does not exist** — the reference pipeline is one ordered chain
where the render-side surface build has to run *before* the structural
loaders that index into it; **`ref/soft` rasterizes from BSP edges at
draw time**, so the baked `Vertex`/index buffers are useless to it; and
**the baked lightmap atlas cannot be the only lighting the engine has**
(it holds lightstyle 0 only, and the server, `ref/soft` and `ref/gl` all
rebuild lighting from per-face samples). The consequence is a much
smaller engine change than the plan anticipated — in fact none for the
X2–X5 path: **the archive becomes a BSP30 file** (a real `dheader_t` at
offset 0 whose lump table points at sector-aligned raw lumps, with the
MPAK descriptor at a fixed offset inside the first sector), so every
pre-load validator that reads the header from disk and the
**unmodified** `Mod_LoadBmodelLumps` all just work — *if* the archive
format grows that "classic core" of raw lumps.

## 1. The contract: who consumes what on `model_t`

`model_t` (`common/com_model.h:327-403`) is what every loader fills and
every consumer reads. The consumers that matter for the phases in front
of us: the dedicated/headless server (X2's exit criterion, `ref/null`),
the software renderer (X5), and the eventual hardware renderer (X9,
modelled on `ref/gl` since `ref/nv2a` does not exist yet — `ls ref/` is
`common gl null soft`).

| Field | Built by | Server / `pm_shared` | `ref/soft` (X5) | `ref/gl`-shaped (X9) |
|---|---|---|---|---|
| `entities` | `Mod_LoadEntities` (`mod_bmodel.c:2298`) | spawn, `*N` origins | — | — |
| `planes` | `Mod_LoadPlanes` (`:2424`) | traces, hulls | backface, clipping | same |
| `submodels` | `Mod_LoadSubmodels` (`:2214`) | hulls, bounds, `visleafs` | bmodel bounds | same |
| `vertexes`, `edges16`, `surfedges` | `Mod_LoadVertexes/Edges/SurfEdges` (`:2456-2530`) | **yes**, indirectly: bevels (`Mod_CreateFaceBevels :1712` via `Mod_GetVertexByNumber :1478`) feed `pm_surface.c:136-158` fence/texture traces | **yes, at draw time**: `r_rast.c:444`, `r_bsp.c:386` walk `surfedges`→`edges16`→vertexes to scan-convert every face | at load: `GL_BuildPolygonFromSurface` (`gl_rsurf.c:381`) → `R_GetEdgePosition :165` |
| `textures[]` (`texture_t`) | `Mod_LoadTextures` (`:3104`) → `Mod_LoadTexture :2927` | **names**: `sv_world.c:1436`, `pm_trace.c:835` (footsteps, `TraceTexture`), `Mod_GetFaceContents :1451` (bevel contents from "SKY"/"!"/"*"/"water"), `pm_surface.c:67` (`'{'` fence test) | names + `gl_texturenum` + `width/height`; `R_TextureAnimation` (`r_surf.c:239`) follows `anim_next` built from `+0…`/`+a…` **names** (`:2967-3091`) | same, plus `R_TextureCoord` divides by `texture->width/height` (`gl_rsurf.c:151-159`) |
| original 8-bit pixels | `TF_KEEP_SOURCE` on `'{'` textures (`:2805`) → `image_t.original` (`ref/soft/r_image.c:334`) | **yes**: `pm_surface.c:75` `R_GetTextureOriginalBuffer` decides whether a trace passes through a fence texel | soft converts RGBA→16-bit 3:3:2+minor at upload (`r_image.c:214-283`); needs an *uncompressed* source | GL decompresses/uploads |
| `texinfo` | `Mod_LoadTexInfo` (`:3247`) | `vecs`/`texture` | same | same |
| `surfaces` (`msurface_t`+`mextrasurf_t`) | `Mod_LoadSurfaces` (`:3291`) | see §2 | see §2 | see §2 |
| `lightdata` → `samples` | `Mod_LoadLighting` (`:3883`, `:3952` sets `samples = lightdata + lightofs/lightmap_samples`) | **`SV_RecursiveLightPoint`** reads `samples + dt*smax + ds` per **style** (`sv_world.c:1546-1571`) | `R_BuildLightMap` sums `samples[map*size]` × `g_lightstylevalue[styles[map]]` then `R_AddDynamicLights` (`r_surf.c:174-236`) | `R_BuildLightMap` (`gl_rsurf.c:725`), per style, plus dlights |
| `marksurfaces` | `Mod_LoadMarkSurfaces` (`:2537`) | leaf→surface lists | same | same |
| `leafs` | `Mod_LoadLeafs` (`:3546`) | PVS/PHS (`Mod_CalcPHS :3651`), contents | vis | vis |
| `nodes` | `Mod_LoadNodes` (`:3438`) | traces, light point | BSP walk | BSP walk |
| `clipnodes`/`hulls[]` | `Mod_LoadClipnodes` (`:3785`) + `Mod_MakeHull0` (`:1876`) + `Mod_SetupHull` (`:1936`) | **all collision** | — | — |
| `visdata` | `Mod_LoadVisibility` (`:3830`) | PVS/PHS | PVS | PVS |

`ref/null` implements `Mod_ProcessRenderData` as `return true` and every
texture load as `return 0` (`ref/null/r_context.c:202-260`), so the
headless X2 target needs only the server column.

## 2. Field by field: `msurface_t`, `mextrasurf_t`, `glpoly2_t`

`common/com_model.h:267-303`, `:223-254`, `:131-142`. "Source" is where
the value has to come from when the model was loaded from an archive;
"raw lump" means the existing loader derives it from the classic BSP
lumps, unchanged, once those lumps are present.

### `msurface_t`

| Field | Written by | Read by | Source in an archive |
|---|---|---|---|
| `visframe` | renderers per frame | renderers | runtime only |
| `plane` | `Mod_LoadSurfaces :3344` (`planes + in->planenum`) | traces, both renderers, `SV_RecursiveLightPoint` | raw `FACES.planenum` (`MpakFaceInfo.planenum` holds the same value) |
| `flags` | `:3343-3374` from `side`, texture **name** prefixes, `texinfo.flags`; `SURF_UNDERWATER` added by `Mod_LoadLeafs :2631` | everything | raw `FACES` + texture names (the baker's `MpakFaceInfo.flags` reproduces `:3354-3374` — `BspParser.cpp:215-233` — but `SURF_UNDERWATER` is per-leaf, runtime) |
| `firstedge`, `numedges` | `:3341-3342` | `Mod_CalcSurfaceBounds/Extents/CreateFaceBevels :1590-1761`; `ref/soft` every frame (`r_rast.c:444`); `ref/gl` load (`gl_rsurf.c:381`) | raw `FACES` + `SURFEDGES` + `EDGES` + `VERTEXES` — **no substitute in v6** |
| `texturemins[2]`, `extents[2]` | `Mod_CalcSurfaceExtents :1646-1647` | `ref/soft` surface cache (`r_surf.c:391-430`), `ref/gl` texcoords | raw lumps (baker computes the same values, `BspParser.cpp:541-551`, and stores them in `MpakFaceInfo.lightmapmins/extents` — usable as a cross-check, not needed as input) |
| `light_s`, `light_t`, `lightmaptexturenum` | `ref/gl` `GL_CreateSurfaceLightmap :3823-3833` | `ref/gl` | runtime (renderer's own block allocation) |
| `polys` | `ref/gl` `GL_BuildPolygonFromSurface :406-412`; **never in `ref/soft`** (grep: only decal code reads it there, and bails when NULL) | `ref/gl` draw; decals | runtime |
| `texturechain` | renderers | renderers | runtime |
| `texinfo` | `:3345` | everything | raw `FACES.texinfo` |
| `dlightframe`, `dlightbits` | renderers (`R_MarkLights`) | `R_BuildLightMap`/`R_AddDynamicLights` | runtime |
| `styles[4]` | `:3347-3348` | `R_BuildLightMap` (both refs), `SV_RecursiveLightPoint :1566` | raw `FACES.styles` (`MpakFaceInfo.styles` holds the same bytes) |
| `cached_light[4]` | `ref/gl` `R_UpdateSurfaceCachedLight` | `ref/gl` | runtime |
| `info` | `:3312` | everything | allocated at load |
| `samples` | `Mod_LoadLighting :3952` (`lightdata + lightofs/lightmap_samples`) | `SV_RecursiveLightPoint :1557`, `R_BuildLightMap` (both refs) | raw `LIGHTING` + `FACES.lightofs` — **no substitute in v6** (the atlas is style-0-only and not stride-`smax`) |
| `pdecals` | decal system | decal system | runtime |

### `mextrasurf_t`

| Field | Written by | Read by | Source |
|---|---|---|---|
| `mins`, `maxs`, `origin` | `Mod_CalcSurfaceBounds :1678-1705` from vertex positions | `ref/gl` culling, `R_AddDynamicLights` via `surf->plane`, decals | raw lumps (positions also exist per vertex in the baked `Vertex[]`, so derivable from the X9 block too) |
| `surf` | `:3313` | everything | allocated |
| `dlight_s`, `dlight_t` | `ref/gl` dynamic-lightmap allocation (`gl_rsurf.c:1186-1220`) | `ref/gl` | runtime |
| `lightmapmins[2]`, `lightextents[2]` | `Mod_CalcSurfaceExtents :1654-1662` (== `texturemins/extents` unless `TEX_WORLD_LUXELS`, which retail HL never sets) | `SV_RecursiveLightPoint :1549-1556`, `R_BuildLightMap`, `R_AddDynamicLights`, `ref/soft` `R_DrawSurface :391-430` | raw lumps |
| `lmvecs[2][4]` | `Mod_LightMatrixFromTexMatrix :1543-1581` (a copy of `texinfo->vecs` for retail maps — early return at `:1563`) | `SV_RecursiveLightPoint :1547`, `R_AddDynamicLights :127` | raw `TEXINFO` |
| `deluxemap`, `shadowmap` | `Mod_LoadLighting :3955-3960` when `ENGINE_LOAD_DELUXEDATA` and the extra lumps exist | mods/`ref/gl` | optional; retail has none |
| `lightmapchain`, `detailchain`, `lumachain` | `ref/gl` draw chains | `ref/gl` | runtime |
| `bevel` | `Mod_CreateFaceBevels :1730` — one plane per edge from consecutive vertex positions, contents from texture **name** (`:1719-1721`) | `pm_surface.c:136-158` (server and client traces) | raw lumps + names |
| `parent`, `mirrortexturenum`, `mirrormatrix`, `grass`, `grasscount` | mods / `ref/gl` extras | same | runtime |
| `numverts`, `firstvertex` | `ref/gl` VBO path (`gl_rsurf.c`, 22 references) | `ref/gl` | runtime |

### `glpoly2_t`

`next`, `chain`, `numverts`, `flags`, `verts[numverts][VERTEXSIZE=7]` =
`xyz, s1 t1, s2 t2` (`com_model.h:36,131-142`). Built only by `ref/gl`
(`GL_BuildPolygonFromSurface :381-420`: positions from `surfedges`,
`s1 t1` = `R_TextureCoord` (texinfo vecs / texture dims), `s2 t2` =
`R_LightmapCoord` from the renderer's *own* `light_s/light_t` block
placement). This is the exact shape of the baked `Vertex` (`x y z u v lu
lv`, `PakManager.h:13-27`) — with one difference that decides X9: the
baked `lu lv` are normalized into the baker's 1024×1024 atlas page
(`BspParser.cpp:646-650`), while `s2 t2` are normalized into the
renderer's own `BLOCK_SIZE` lightmap texture. They are only
interchangeable if the X9 renderer adopts the baked atlas as its lightmap
texture wholesale — which means static lighting, no styles, no dlights
(§3, F3). `ref/soft` never builds or reads polys.

## 3. The reference pipeline is one ordered chain

`Mod_LoadBmodelLumps` (`mod_bmodel.c:4145`) first points `dbspmodel_t`'s
pointer/count pairs into the buffer via `Mod_LoadLump` — **pointers, not
copies** (`:925-928`: `*(byte **)((byte *)bmod + info->dataofs) = data`),
with per-lump `mincount`/`maxcount`/entry-size validation and
`loadstat` accounting (`:819-919`) — then runs:

```
Entities → Planes → Submodels → Vertexes → Edges → SurfEdges → Textures →
Visibility → TexInfo → Surfaces → Lighting → MarkSurfaces → Leafs → Nodes →
Clipnodes → MakeHull0 → SetupSubmodels            (mod_bmodel.c:4229-4247)
```

Each later loader `Mem_Malloc`s its own copy into `mod->mempool`, so the
raw buffer is disposable afterwards — `Mod_LoadModel` frees it
(`model.c:426`) after computing the optional map CRC over it (`:407-425`).

The order is load-bearing:

- `Mod_LoadSurfaces` needs `planes`, `texinfo` (which needs `textures`),
  and — through `Mod_CalcSurfaceBounds`/`Extents`/`CreateFaceBevels`
  (`:3376-3378`) — `vertexes`/`edges`/`surfedges`.
- `Mod_LoadMarkSurfaces` writes `mod->surfaces + in[i]` (`:2552, :2574`)
  and `Host_Error`s against `numsurfaces` — **surfaces must exist first.**
- `Mod_LoadLeafs` writes `mod->marksurfaces + in->firstmarksurface`
  (`:2601`) and marks `SURF_UNDERWATER` on those surfaces (`:2631`);
  `Mod_LoadNodes` needs `mod->leafs` (`:3532`); `Mod_MakeHull0` needs
  `mod->nodes` (`:1887`); `Mod_SetupSubmodels` reads surface flags
  (`:2170-2182`).

So "load the 9 structural lumps now, the 6 render ones later" is not a
valid increment. `dbspmodel_t` and all of these functions are `static` to
`mod_bmodel.c` (grep: nothing else references `dbspmodel_t`).

## 4. What MPAK v6 carries versus what the chain needs

Ground truth for the archive side is `PakManager::save()`
(`tools/asset-studio/src/formats/PakManager.cpp:184-307`) and
`BspParser::exportToPak()` (`BspParser.cpp:267-694`); the offsets are
proven by `filesystem/mpak_layout.c` against real bakes.

| Chain input | In MPAK v6? | Notes |
|---|---|---|
| ENTITIES, PLANES, VISIBILITY, NODES, TEXINFO, CLIPNODES, LEAFS, MARKSURFACES, MODELS | **Yes, byte-identical** (`BspParser.cpp:681-689` copy the source lumps verbatim) | Feed straight in; existing loaders run unchanged. |
| VERTEXES, EDGES, SURFEDGES, FACES | **No** | Replaced by baked `Vertex[]`, `uint16` indices, one `FaceDrawCmd` per face in `LUMP_FACES` order (`BspParser.cpp:672`), and `MpakFaceInfo` (`MpakHeader.h:145-155`). |
| TEXTURES (miptex lump: names, dims, 8-bit pixels) | **No** | Resolved from WADs at bake time and written as DXT (Xbox: one atlas; PC: 64/256/512 arrays). Names and per-miptex dimensions/locations survive only *inside each vertex* (`Vertex.rect/texture_layer/array_index`, `u/v` pre-divided — `BspParser.cpp:633-658`). There is no miptex→location table in the file, so `texinfo.miptex` indexes nothing. |
| LIGHTING (per-face `color24`, up to 4 styles) | **No** | Replaced by one RGBA8 1024×1024 page per layer, **style 0 only** (`BspParser.cpp:514-520`), each face's block shelf-packed with a 1-luxel border (`:93-154`). |

Findings, numbered so the plan can cite them:

- **F1 — Server (X2 exit criterion).** `Mod_LoadSurfaces` cannot run
  without `vertexes/edges/surfedges`; `SV_RecursiveLightPoint` cannot
  run without per-style `samples`; `SV_TraceTexture`/`pm_trace` cannot
  return a texture **name**; `Mod_GetFaceContents` cannot classify bevel
  contents. All four are gameplay paths (monster/player light level,
  footstep sounds, sky/water contents), not renderer niceties.
- **F2 — `ref/soft` (X5).** Rasterizes from `surfedges`/`edges16` *every
  frame* (`r_rast.c:444`, `r_bsp.c:386`), rebuilds lighting from
  `samples` per style and per dlight (`r_surf.c:174-236` — the
  flashlight, muzzle flashes, explosions and every `light` with a
  `style` go through this), uploads textures from RGBA
  (`r_image.c:214-283`). Nothing in the baked set is usable by it.
- **F3 — `ref/gl`-shaped (X9).** The one consumer the baked buffers were
  designed for — but its lightmap coordinates come from its own block
  allocation (§2, `glpoly2_t`), so baked `lu/lv` and the atlas only pay
  off as a *static* lighting path. X9's decision, made against a real
  `nv2a` renderer.
- **F4 — `texture_t.name`, `width`, `height`** are needed by both
  renderers and the server; v6 has none of the three per miptex.
- **F5 — Multiplayer CRC.** `sv_init.c:1028` `CRC32_MapFile`s the bytes
  served as `maps/<name>.bsp`; an `.mpak` never matches a client's
  `.bsp`. Parked with networking; noted so it is not rediscovered.
- **F7 — The map is validated from disk before any model loader runs.**
  Found in review of the first draft of this document. `SV_Map_f`
  (`sv_cmds.c:210-214`) calls `SV_ValidateMap` before `COM_LoadLevel`;
  that reaches `SV_ReadEntityScript` (`sv_game.c:823-853`), which
  `FS_Open`s `maps/<name>.bsp`, reads the first `MAX_TOKEN` bytes and
  calls `Mod_TestBmodelLumps` (`mod_bmodel.c:4323-4398`) — a header
  version switch plus `Mod_LoadLump` over every lump *with the file
  handle*, and `Mod_LumpLooksLikeEntitiesFile` seeking into the file by
  `lumps[LUMP_ENTITIES].fileofs`. `SV_WriteEntityPatch` (`:786`),
  `Mod_CheckLump`/`Mod_ReadLump`/`Mod_SaveLump` (`:4436`, `:4493`,
  `:4570`) and the map-list validation in `con_utils.c:93` read the
  header from disk the same way. An in-memory header synthesised inside
  `Mod_LoadBrushModel` — the first draft's loader — runs too late for
  all six; `+map` would refuse the archive before the loader ever saw
  it. The fix is structural (§5): the header they expect has to be the
  first bytes of the file.
- **F6 — Memory (X3).** `c1a3`, real bytes:

  | | bytes |
  |---|---|
  | `c1a3.bsp`, whole file | 1,519,776 |
  | of which LIGHTING (all styles, `color24`) | 598,923 |
  | of which VERTEXES+EDGES+SURFEDGES+FACES | 334,532 |
  | of which TEXTURES (WAD references only, 183 miptex) | 8,056 |
  | `c1a3.mpak` v6, PC / Xbox | 9,256,960 / 8,767,488 |
  | baked `Vertex[]` (23,962 × 64) + indices | 1,615,216 |
  | lightmap atlas, 1 page RGBA8, style 0 only | 4,194,304 |
  | DXT textures (PC arrays / Xbox atlas) | 2,586,624 / 2,097,152 |
  | `MpakFaceInfo` + `FaceDrawCmd` | 269,412 |

  The atlas is **7× the lighting it replaces and carries less**. On a
  64 MB console that is the wrong trade for the X5 path regardless of
  anything else here.

## 5. Recommendation: a "classic core" loaded by the unmodified pipeline

Add to MPAK a core that is exactly what `Mod_LoadBmodelLumps` already
eats, and demote the baked buffers to an optional renderer-specific block.

**Core (every profile):** the nine raw lumps v6 already has **plus** raw
`VERTEXES`, `EDGES`, `SURFEDGES`, `FACES`, `LIGHTING` (all styles,
`dface_t.lightofs` intact) and a `TEXTURES` lump in the *embedded-miptex*
shape compilers emit for `-nowadtextures`: `dmiptexlump_t` (`bspfile.h:196`)
+ per texture `mip_t` (name, width, height, four mip offsets) + the four
8-bit mips + `short 256` + 768-byte palette (`Mod_CalculateMipTexSize
:664`, `Mod_CalcMipTexUsesCustomPalette :673` already detect exactly this
layout). The baker already holds the decoded WAD lumps
(`WadParser::decodeMiptex`), so this is a re-emit, not new decoding.

**Layout: the archive is a BSP30 file.** `Mod_LoadLump` reads each lump's
extent from `((dheader_t *)in)->lumps[n]` and addresses it as `in +
fileofs` (`:769, :925`), and every pre-load validator (F7) reads that same
`dheader_t` straight from the file. So the first sector of a v7 archive
is:

| Offset | Bytes | Content |
|---|---|---|
| 0 | 124 | real `dheader_t`: `version = HLBSP_VERSION`, `lumps[LUMP_n] = { absolute file offset, length }` for all 15 classic lumps, each at a sector-aligned offset ≥ 2048 |
| 124 | 4 | zero — the `dextrahdr_t` probe at `in + sizeof(dheader_t)` (`:783-786`, `:4336`) reads `id == 0`, not `IDEXTRAHEADER` |
| 128 | `sizeof(MpakHeader6)` | the MPAK descriptor (magic, version 7, every section length) — exactly today's header, moved to `MPAK_DESCRIPTOR_OFFSET` |
| … | to 2048 | zero |

Everything after the first sector is unchanged in kind: the sections
`PakManager::save()` already writes, sector-padded, plus the six classic
lumps appended (§6). The `dheader_t` table and the descriptor's lengths
describe the same bytes twice; `mpak_layout.c` verifies they agree.

What this buys, path by path:

1. `SV_ValidateMap` → `Mod_TestBmodelLumps` (F7) sees a version-30
   header, walks real lumps with real `mincount`/`maxcount`, seeks into a
   real entities lump. Passes unchanged.
2. `Mod_LoadModel` (`model.c:341`) dispatches on `HLBSP_VERSION` into the
   ordinary `Mod_LoadBrushModel` → `Mod_LoadBmodelLumps`. **Zero engine
   changes** for X2–X5. Blue-Shift lump-swap detection (`:4168-4172`)
   sees a real entities lump and stays off; `Mod_SwapBSPLumps` is a
   no-op on little-endian; `Mod_FindBSPX` finds no `BSPX` magic in zero
   padding; the CRC (`model.c:407`, `sv_init.c:1028`) is deterministic
   per archive.
3. `ref.dllFuncs.Mod_ProcessRenderData( mod, true, buf, length )`
   (`model.c:381`) already hands every renderer the whole file buffer:
   an X9 renderer reads the descriptor at offset 128 and its baked block
   from `buf` with no engine plumbing — the same hook `ref/gl`,
   `ref/soft` and the client-DLL `Mod_ProcessUserData` use today.
4. `filesystem/mpak.c` keeps serving the archive under
   `maps/<name>.bsp` and keeps its precedence/manifest pairing; its
   probe changes from "magic at 0" to "`dheader_t.version == 30` at 0
   and `MPAK`/7 at 128" (§6). A plain `.bsp` renamed `.mpak` fails the
   second check and is ignored, as it should be.

The `mod_bmodel.c` divergence anticipated as "rewrite 6 functions under
`#ifdef`" (`divergences.md`) becomes **no divergence at all**; every
validation the `.bsp` path performs runs verbatim on the archive because
the archive *is* one. Fallback if something turns out to depend on the
`.bsp` extension specifically rather than the header (nothing found —
`Mod_LoadBrushModel`, `SV_ReadEntityScript` and the lump utilities all go
through the VFS name `maps/<name>.bsp`, which `filesystem/mpak.c` already
answers to): serve the archive as `.bsp` outright.

**Optional block (X9 profile):** `Vertex[]`, indices, `FaceDrawCmd`,
`MpakFaceInfo`, DXT textures, and — *if* X9 wants a static fast path —
an atlas. The core loader ignores this block; X9's renderer reads it via
`model_t.cache`/extradata when present.

What this means for the monolithic-packaging idea: on the X5 path an
`.mpak` becomes "the BSP with its WAD textures inlined, lumps
sector-aligned, one file per map, plus the manifest". That is still the
I/O win the plan wanted (one sequential file instead of a `.bsp` and N
WAD seeks; no WAD directory scans; manifest-driven precache) — it just
is not a different geometry encoding. The re-encoding only pays off for
the hardware renderer, which is where it stays. `MpakHeader.h`'s claim
that the 9 structural loaders "stay untouched because the .mpak stores
those lumps raw" extends to all fifteen.

## 6. Packing the map data: MPAK v7, defined

Two layout changes and six new lumps.

**First sector** (§5): `PakManager::save()` writes the `dheader_t` at
offset 0 and the `MpakHeader6` descriptor at `MPAK_DESCRIPTOR_OFFSET =
128` (new constant in `MpakHeader.h`, mirrored in `mpak_layout.h`),
padding the rest of the sector with zeros. The `dheader_t` lump table is
filled from the same running cursor the writer already uses to lay
sections out, so it cannot disagree with the descriptor by construction;
`filesystem/mpak_layout.c` gains a check that it *doesn't* — every
`lumps[LUMP_n]` must equal the `{offset, length}` `Mpak_ComputeLayout`
derives from the descriptor, and `version` must be `HLBSP_VERSION`. A
mismatch is a writer/reader drift, rejected the way the header-size
mismatch already is. `UiPacker` (menu archives) keeps writing its
descriptor at offset 0 — it is not a map and `filesystem/mpak.c`'s new
probe correctly stops mounting it as one (today it is mounted as
`maps/menu.bsp`, harmlessly but wrongly).

**Descriptor additions**, appended after `lenTexinfo` per
`MpakHeader.h`'s own rule, in this order (the write order in
`PakManager::save()` follows it):

| Field | Content | Source in the baker |
|---|---|---|
| `lenVertexes` | raw `LUMP_VERTEXES` (`dvertex_t[]`) | `getLumpData(LUMP_VERTICES)` |
| `lenEdges` | raw `LUMP_EDGES` (`dedge_t[]`) | `getLumpData(LUMP_EDGES)` |
| `lenSurfedges` | raw `LUMP_SURFEDGES` (`int32[]`) | `getLumpData(LUMP_SURFEDGES)` |
| `lenFaces` | raw `LUMP_FACES` (`dface_t[]`, 20 bytes each) | `getLumpData(LUMP_FACES)` |
| `lenLighting` | raw `LUMP_LIGHTING` (`color24`, all styles) | `getLumpData(LUMP_LIGHTING)` — the field this name had in v5 was removed as "dead weight" precisely because the atlas replaced it (`MpakHeader.h:50-52`); it comes back because F1/F2 |
| `lenMiptex` | embedded-miptex `TEXTURES` lump (§5) | rebuilt from the WAD lumps `exportToPak` already resolves (`BspParser.cpp:421-480`); the missing-texture placeholder (`:383-401`) becomes a real 16×16 8-bit miptex named `missing` |

Semantics fixed here:

- `flags` bit 1 (`MPAK_PROFILE_NV2A`) marks that the v6 block is
  populated. Core-only profile writes `vCount = iCount = 0`, every
  texture-array/atlas/lightmap-atlas/faceInfo/faceDrawCmds length 0 —
  exactly how `lenAssetDict` is already treated. `mpak_layout.c` needs
  no special case: zero-length lumps already contribute no bytes.
- The nine existing raw lumps are unchanged. `MpakFaceInfo` is
  unchanged (it is X9's). Texture names are written lower-cased, the
  way `Mod_LoadTexture :2953` normalises them anyway.
- `bakerBuildId` bumps; `version` becomes 7; `Mpak_ValidateHeader`'s
  hard version check moves with it (every prior bump was a full rebake,
  `MpakHeader.h:65-69`; same here).
- `mpak_layout.c` gains six `Mpak_PlaceLump` calls at the end of the
  order with record sizes `sizeof(dvertex_t)`, `sizeof(dedge_t)`,
  `sizeof(dsurfedge_t)`, `sizeof(dface_t)`, 0 (lighting is a blob) and
  0 (miptex is self-describing); `mpak_layout_t` gains six members;
  the test gains six independently hand-computed offsets and the
  real-bake check (end offset == file size) re-runs for both profiles.
- `build-test-package.ps1` / `main.cpp` grow a `--profile core|nv2a`
  switch; default `core` until X9 exists.

Size, `c1a3`, core profile: the nine v6 lumps total 578 KB (planes 160 K,
nodes 83 K, leafs 67 K, clipnodes 72 K, visibility 39 K, texinfo 54 K,
marksurfaces 14 K, models 10 K, entities 80 K) + geometry 334 KB +
lighting 599 KB + embedded textures (measure in the PR: 183 resolved WAD
lumps, 8-bit, four mips, palette) — on the order of 2–3 MB versus 8.8 MB
today, with every lightstyle and real names.

## 7. The loading chain for an archive, step by step

| Step | Function (all existing, none modified) | Reads | Produces |
|---|---|---|---|
| −1 | `SV_Map_f` → `SV_ValidateMap` → `SV_ReadEntityScript` → `Mod_TestBmodelLumps` (F7), via `FS_Open("maps/<name>.bsp")` → `filesystem/mpak.c` | the archive's `dheader_t` and lumps, from disk | map accepted; entities lump located for `.ent` patching |
| 0 | `Mod_LoadModel` (`model.c:324`) → `FS_LoadFile("maps/<name>.bsp")` → `filesystem/mpak.c` serves the archive | archive bytes | `buf` |
| 1 | `Mod_LoadModel` switch (`:341`) on `HLBSP_VERSION` → `Mod_LoadBrushModel` | `buf[0..3]` | — |
| 2 | `Mod_LoadBmodelLumps` → `Mod_LoadLump` ×15 | `dheader_t.lumps[]` at `buf[0]` | `dbspmodel_t` pointers into `buf` |
| 3 | `Mod_LoadEntities` | ENTITIES | `mod->entities`, `world.wadlist` (only WADs that exist — `:2279`; none needed with embedded textures) |
| 4 | `Mod_LoadPlanes`, `Mod_LoadSubmodels` | PLANES, MODELS | `planes`, `submodels` |
| 5 | `Mod_LoadVertexes/Edges/SurfEdges` | the three geometry lumps | `vertexes`, `edges16`, `surfedges`; world bounds |
| 6 | `Mod_LoadTextures` → `Mod_LoadTexture` → `Mod_LoadTextureData` | embedded miptex | `texture_t[]` with names/dims; on a client the "internal texture" path (`:2860-2868`) uploads via `ref.dllFuncs.GL_LoadTexture`; on dedicated only names/dims (`:2856`); `Mod_SequenceAllAnimatedTextures` links `+`-frames by name |
| 7 | `Mod_LoadVisibility`, `Mod_LoadTexInfo` | VISIBILITY, TEXINFO | `visdata`, `texinfo` (→ `textures[miptex]`) |
| 8 | `Mod_LoadSurfaces` | FACES + everything above | `msurface_t`/`mextrasurf_t`: flags, extents, bounds, bevels, `lightmap_samples` detection (`:3380-3430`) |
| 9 | `Mod_LoadLighting` | LIGHTING + `FACES.lightofs` | `lightdata`, `surf->samples` |
| 10 | `Mod_LoadMarkSurfaces`, `Mod_LoadLeafs`, `Mod_LoadNodes`, `Mod_LoadClipnodes` | MARKSURFACES, LEAFS, NODES, CLIPNODES | tree, PVS pointers, `clipnodes_out` |
| 11 | `Mod_MakeHull0`, `Mod_SetupSubmodels` | above | `hulls[0..3]`, `*N` submodels, `MODEL_*` flags |
| 12 | `Mod_LoadModel` → `ref.dllFuncs.Mod_ProcessRenderData` | — | `ref/null`: `true`; `ref/soft`: `Mod_ProcessUserData` hook only (`ref/soft/r_context.c:88-120`); `ref/gl`: polys + lightmap blocks at world-load time |
| 13 | `Mod_LoadModel:426` frees `buf` | — | archive bytes gone; everything lives in `mod->mempool` |

Nothing in steps −1–13 is archive-aware. The X9 block, when present, is
the one thing that must be lifted out of `buf` before step 13 (into
`mod->cache` or a renderer-owned pool) — an X9 concern, and step 12 is
exactly where the renderer already holds `buf`.

## 8. Verification that does not need a renderer

"The engine loads a map from `.mpak`" is defined **differentially**
against the `.bsp` loader, in the dedicated build `filesystem/tests`
already uses:

1. Load `maps/c1a3.bsp` and `c1a3.mpak` (served under the same name by
   `filesystem/mpak.c`, from two game directories or two runs).
2. Dump from `model_t`: every array count; every `hulls[i]` clipnode
   array and `firstclipnode/lastclipnode`; `visdata`; per surface —
   `texturemins`, `extents`, `info->lightmapmins/lightextents`,
   `info->lmvecs`, `flags`, `plane - planes`, `texinfo - texinfo`,
   `styles`, `samples - lightdata`, `info->mins/maxs/origin`,
   `info->bevel->numedges` and each bevel plane; per texture — `name`,
   `width`, `height`, `anim_next`/`alternate_anims` linkage;
   `mod->flags`, `world.flags`.
3. Byte-compare. Any difference is a loader (or baker) bug by
   definition — the `.bsp` path is the reference.

Prerequisite found while building for this pass: the dedicated test
binary did not link on this PC configuration
(`xash_tests_dedicated.exe`: unresolved `_Platform_Minimize_f` from
`Host_Main`). **Fixed in W3** (divergence #26): `Host_Main` registered
"minimize" in the dead `else` of `if( Host_IsDedicated( ))` — the
constant `true` under `XASH_DEDICATED` — and MSVC at `/Od` kept the
reference where GCC folds it away; the `else` is now under
`#if !XASH_DEDICATED`.

**Built as W3** (divergence #27): `test_dumpworld <map> <outfile>` in
`engine/common/mod_bmodel.c` (test builds only) loads the map through
`Mod_LoadWorld` and writes the list above, pointers turned into indices,
plus a CRC of each surface's lighting bytes sized the way
`SV_RecursiveLightPoint` reads them (`sv_world.c:1549-1558`) and a CRC of
each leaf's decompressed PVS; `tools/mpak-diff.sh` bakes, dumps from the
retail directory via `-rodir` and from a directory whose only map source
is the archive (`gameinfo.txt` with `basedir == gamedir`, so no `valve/`
hierarchy is mounted underneath — a loose `.bsp` would otherwise win,
divergence #24), byte-compares, and runs a control with the archive
removed that must produce no dump. Results: `c1a3` and `crossfire`
identical from `.bsp` and `.mpak` (5,897,085 / 5,143,563 bytes of dumped
state, every leaf of the lump — the world's visible ones and the
submodels' — included); the control run logs `Could not load model
maps/c1a3.bsp from disk`; a flipped lighting byte, and a flipped ambient
byte on the last world leaf and on the last leaf of the array, are each
caught.

Two corrections to this section's own assumptions, both measured by the
first differential run:

- `world.wadlist` is NOT dumped: `Mod_LoadEntities` keeps only the WADs
  present on disk (`:2263-2279`), so the archive-only directory reports
  0 where retail reports 4, legitimately; nothing but WAD texture lookup
  reads the list.
- "A directory holding only the archive" does not boot. The dedicated
  host `Sys_Error`s without `gfx/conchars` (`host.c:1137`, and on win32
  then waits on its console window forever), takes hulls 1..3 of every
  brush model from `host.player_mins` that only the game library's
  `pfnGetHullBounds` fills (`sv_pmove.c:449` — without `dlls/hl.dll` every
  hull is empty and the dump differs), and once the library is loaded
  `Sys_Error`s without `delta.lst` (`net_encode.c:854`). Grep-confirmed
  those are the only file-dependent `Sys_Error` sites at startup. The
  floor is `gfx.wad` + `dlls/hl.dll` + `delta.lst`; W4's directory holds
  those, the archive and the manifest's loose files.

Not covered by the dedicated differential: texture pixels
(`Mod_LoadTextureData` is client-side, `:2855`) — names and dimensions
are compared; the embedded miptex bytes rest on the baker's `--verify`
until the client run.

Then the X2 exit run — **done as W4** (divergence #28): `tools/mpak-run.sh`
starts a dedicated server with `+map <name>` from a directory holding
`<name>.mpak`, the boot floor above and the manifest's loose files (no
`.bsp` in its search path), waits thirty frames, and runs `test_mapprobe`
(`engine/server/sv_world.c`, test builds): at the `sp_entity` spawn it
calls `SV_PointContents` (hull 0), a player-hull `SV_Move` (hull 1's
clipnodes), `SV_TraceTexture` (the embedded texture names — F5's
consumer), `SV_TraceSurface` and `SV_RecursiveLightPoint` (F1/F2's
per-face samples and lightstyles) straight down, and reports frames
ticked since spawn. `c1a3`: 34 server frames, spawn over `elev2_flr`,
hull stops at z=36, light 33,33,32; `crossfire`: over `lab1_helpad`,
light 198,194,177. The control without the archive gets `map c1a3
doesn't exist`. One finding: the baker's manifest (§9.4's "no texture
manifest" stands, but the *asset* manifest) sees entity keyvalues only —
the game library precaches most models and sounds from code (`c1a3`:
263 models and 282 sounds precached, 91 models absent from the
archive-only directory; the server runs regardless). `test_mapprobe
<outfile>` writes the server's precache tables — a measured but
*partial* inventory: studio external sequence groups are loaded by name
from the model itself (`mod_studio.c:602-609`, `Host_Error` if missing),
companion texture models only on the client, and generic resources
(`sv_init.c:388`) live in other tables. Deployment has to expand
dependencies from it, not ship it as-is.

On Xbox that additionally needs `Host_Main` wired (X1 stopped short of
it on purpose — `engine/platform/xbox/xbox_main.c`); that is W5, its
own package and its own risks.

## 9. Decisions taken by recommendation (override here if wrong)

1. **The RGBA lightmap atlas leaves every profile until X9 exists.**
   Largest thing in the archive, one style, and every current consumer
   rebuilds lighting from samples anyway. X9 re-adds an RGB565 variant
   if it wants one (the `lightmapFormat` field already reserves it).
2. **DXT textures are X9-profile only; the core carries 8-bit miptex.**
   `ref/soft` cannot use DXT without decompressing, and the fence-texture
   trace wants the 8-bit original.
3. **`Vertex[]`/indices/`FaceDrawCmd`/`MpakFaceInfo` are X9-profile
   only.** 1.9 MB/map nothing before X9 reads.
4. **No texture-name manifest.** With textures embedded, no loose WADs
   are needed on the console; `Mod_LoadEntities_splitstr_handler`
   (`:2263`) ignores WADs that do not exist.

## 10. Work packages

| # | Package | Touches | Risk | Exit criterion |
|---|---|---|---|---|
| W1 | **MPAK v7: BSP30 first sector, core lumps, profiles** | `tools/asset-studio`: `MpakHeader.h`, `PakManager.{h,cpp}`, `BspParser.cpp` (re-emit embedded miptex, copy 5 lumps), `main.cpp` (`--profile`); `filesystem/mpak.c` (probe at 0 and 128), `filesystem/mpak_layout.{h,c}` + tests (six lumps, descriptor offset, `dheader_t` cross-check) | Low — append-only past the first sector; layout test and real-bake cross-check already exist | v7 bakes of `c1a3` in both profiles; `test_mpak_layout` and `test_mpak` pass; end offset == file size for both; `dheader_t` table == computed layout; core `c1a3.mpak` ≤ 3 MB |
| W2 | **Engine: nothing** — retained as a checkpoint | no files; the check is that `Mod_TestBmodelLumps` and `Mod_LoadBrushModel` accept a v7 archive as-is | — | `xash` / dedicated on PC: `map c1a3` from a directory holding only `c1a3.mpak` passes `SV_ValidateMap` and reaches `Mod_LoadBrushModel` (log lines), with no engine diff |
| W3 | **Differential verification tool** — **done** | `test_dumpworld` in `engine/common/mod_bmodel.c` (test builds, #27) + `tools/mpak-diff.sh`; the `Platform_Minimize_f` link gap fixed in `host.c` (#26) | Low–medium | **met**: byte-identical dumps for `c1a3` and `crossfire` from `.bsp` and `.mpak`, control and negative control both behave (§8) |
| W4 | **Headless run from `.mpak` + the boot floor** — **done** | `test_mapprobe` in `engine/server/sv_world.c` (test builds, #28) + `tools/mpak-run.sh`; directory = `.mpak` + `gfx.wad`, `dlls/hl.dll`, `delta.lst` + manifest files | Medium | **met** on `c1a3` and `crossfire`: frames tick, the scripted trace exercises `SV_TraceTexture`, `SV_Move`, `SV_PointContents` and `SV_RecursiveLightPoint` (§8); the X2 exit line on PC |
| W5 | **Xbox `Host_Main`** | `engine/platform/xbox/xbox_main.c`, whatever boot measures reveal | High (unmeasured, per X1's own notes) | same as W4, on hardware/xemu |
| X9 | `nv2a` consumes the optional block; decides atlas/`lu/lv` | later | — | — |

W1 → W2 → W3 are strictly ordered (W2 is a measurement of W1's output;
W3's dump gives W2 its proof); W3's tool can be written against `.bsp`
alone in parallel with W1. W4 and W5 follow.

## 11. Corrections to existing docs

- `docs/r3d/divergences.md`, Anticipated row "`mod_bmodel.c`: the render
  half — Rewrite of 6 functions under `#ifdef`": retire it — with a real
  `dheader_t` at offset 0 there is no engine divergence for X2–X5.
- `filesystem/mpak.c`'s header comment and probe: the archive's own
  magic is no longer at offset 0.
- `docs/r3d/fork-plan.md`, X2: the `pfnLoadFile` per-resource bullet is
  stale (already noted in `mpak.c`); the section gains the package list.
- `tools/asset-studio/src/formats/MpakHeader.h`: the "9 structural
  functions stay untouched" comment becomes "all fifteen" with v7; the
  atlas/`lu/lv` comments gain "X9 profile only"; the `lenLighting`
  "dead weight" note (`:50-52`) is reversed with the reason (F1/F2).
