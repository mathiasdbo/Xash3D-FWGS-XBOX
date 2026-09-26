# X9-X design: the ref/nv2a texture manager and 2D pass

Input for roadmap X9 item 14, steps 2-3 (`docs/r3d/fork-plan.md`). From
the nv2a-xbox-specialist, 2026-09-25, read against `ref/nv2a/r_context.c`
at the frame-structure change (branch `x9-x-frame-2d`); nothing here was
built or run when it was written. Line numbers drift: search for the
named function. Items marked unverified must be checked on hardware.

## 0. What exists to reuse (ref/nv2a/r_context.c)

- Program upload: `nv2a_init_shader` / `nv2a_init_lit_shader` (the
  vertex-program upload loop and the `#include "*.inl"` pixel block). The
  2D pass gets its own `nv2a_init_2d_shader` of the same shape.
- Allocation: `MmAllocateContiguousMemoryEx(size, 0, NV2A_TEST_MAXRAM, 0,
  PAGE_READWRITE|PAGE_WRITECOMBINE)`.
- Vertex arrays (proven live): `nv2a_set_attrib_pointer`, the "all 16
  array formats = TYPE_F, size 0" reset, `BEGIN_END` +
  `0x40000000|NV097_DRAW_ARRAYS`.
- Linear texture register block on stage 0 (the test triangle's): OFFSET
  = physical address `& 0x03ffffff`; FORMAT = CONTEXT_DMA 2 |
  BORDER_SOURCE_COLOR | DIMENSIONALITY 2 | MIPMAP_LEVELS 1, no BASE_SIZE;
  CONTROL1 = `pitch << 16`; IMAGE_RECT = `(w << 16) | h`; ADDRESS
  `0x00030303` (clamp); CONTROL0 `0x4003ffc0`; stage 1 off with
  `TEXTURE_CONTROL0 + 0x40 = 0`.
- Filters (the convolution field must be non-zero on hardware): nearest
  `0x01012000`, bilinear without mips `0x02022000`.
- Byte order: engine `PF_RGBA_32` (bytes R,G,B,A) is `LU_IMAGE_A8B8G8R8`
  (0x3F) - a plain row copy, no swap.
- White texture for `FillRGBA`: a small private page, like
  `nv2a_unlit_page`.
- The GPU is idle at both frame edges: `R_BeginFrame` waits `pb_busy()`
  after the clear, `R_EndFrame` before `pb_finished()`.

## 1. Texture storage

- **Linear textures only for 2D:** any size, texel coordinates, no mips,
  clamp only (that wrap does not work on linear is from XDK memory,
  unverified). Row pitch rounded up to 64 bytes, base offset aligned to
  128. Swizzled textures (needed for wrap and mips: studio skins, world
  sprites) come later; the X4 swizzle defect is still open.
- The only 2D wrap user is `SCR_TileClear` (backtile, only when
  `scr_viewsize < 120`): split into whole-tile quads on the CPU when a
  non-`TF_CLAMP` texture gets s or t outside [0,1].
- **Depth:** ship `A8B8G8R8` only first. Then, behind a cvar, classify at
  upload: alpha only 0/255 -> `A1R5G5B5` (0x10); all 255 -> `R5G6B5`
  (0x11); `A4R4G4B4` (0x1D) opt-in only (bands on HUD gradients). Measure
  with `PARM_TEX_MEMORY`. Scale: the WAD conchars is 256x128 = 128 KiB at
  32 bit.
- **Input formats:** `PF_INDEXED_24/32` arrive when `TF_KEEP_SOURCE` is set
  without `TF_EXPAND_SOURCE` (the console font, `console.c:606`) - expand
  with `ref/soft/r_image.c`'s palette code (3- vs 4-byte stride).
  `GL_UpdateTexture` takes `PF_RGBA_32`, `PF_BGRA_32`, `PF_RGB_24`,
  `PF_BGR_24`, `PF_LUMINANCE` (as `ref/gl` does). Compressed: log and
  refuse.
- **Allocation: one contiguous arena, allocated once in `R_Init` before any
  map loads** - per-texture allocations round to 4 KiB and, long-lived and
  scattered, fragment the physical memory the next map's multi-MiB
  lightmap/texture/vertex buffers need. Start at 3 MiB (resize from
  measurements), first-fit free list with coalescing, 128-byte granules,
  bookkeeping in ordinary heap (never read write-combined memory). Full
  arena: fall back to a per-texture contiguous allocation and log it.
  Report used/free as `PARM_TEX_MEMORY`.
- **Texture table:** 1024 slots (ref/soft's Xbox `MAX_TEXTURES`), slot 0
  reserved, name hash (`ref/soft/r_image.c`). Per slot: name, flags,
  width/height, srcWidth/srcHeight, format, pitch, arena offset/size,
  `last_used_frame`, optional heap copy for `TF_KEEP_SOURCE` -
  `GL_TextureData` and `R_GetTextureOriginalBuffer` return the heap copy,
  never GPU memory.
- **Semantics to match:** `GL_LoadTexture` returns an existing slot by name
  first (`R_GetBuiltinTexture` is `GL_LoadTexture(name,0,0,0)`);
  `GL_CreateTexture` records `*default`, `*white` and the other built-ins
  (as `ref/soft/r_image.c` does); binding texnum <= 0 or out of range
  binds the default texture (`ref/gl` `GL_Bind`). `RefGetParm` answers
  `PARM_TEX_WIDTH/_HEIGHT/_SRC_WIDTH/_SRC_HEIGHT/_FLAGS/_MEMORY` (menu pics
  and tile clear read them) and forwards everything else to
  `ENGINE_GET_PARM_` as ref/soft's default case does (today it returns 0).
- **Freeing while the GPU may read:** `GL_FreeTexture` frees at once when
  no frame is open (the GPU is idle after `R_EndFrame`, and loading needs
  the memory back before `R_NewMap`); inside a frame it defers to the
  next `R_BeginFrame`, after its `pb_busy()` wait. Never reuse freed bytes
  in the same frame. `GL_UpdateTexture` / `GL_LoadTextureFromBuffer(update)`
  mid-frame: stall on `pb_busy()` first if the texture was used this
  frame; a size change allocates new and defer-frees old.

## 2. The 2D pass

- **Submission:** a contiguous 256 KiB vertex ring (allocated in
  `R_Init`, reset to 0 in `R_BeginFrame`; on overflow mid-frame wait
  `pb_busy()` and wrap) and `DRAW_ARRAYS` with `OP_QUADS` - the proven
  mechanism (immediate `SET_VERTEX_DATA` / `INLINE_ARRAY` are unused in the
  tree, so unverified).
- **Vertex (20 bytes):** `float x, y; float u, v; uint32_t rgba;` packed.
  Slot 0 TYPE_F size 2; slot 3 (diffuse) `TYPE_UB_OGL` size 4 (bytes
  R,G,B,A -> x,y,z,w); slot 9 TYPE_F size 2; stride 20.
- **Batching:** consecutive quads with the same texture and render state;
  flush on change: one `BEGIN_END(QUADS)`, `DRAW_ARRAYS` dwords of at most
  256 vertices (8-bit count field), each `pb_begin`/`pb_end` block at most
  128 dwords.
- **Texture coordinates:** CPU-side `u = s * width`, `v = t * height`
  (linear textures are in texels). Colour per vertex, so `Color4ub`
  changes do not break batches.
- **Programs** `vs_2d.vs.cg` / `ps_2d.ps.cg` (cgc + vp20compiler /
  fp20compiler, like the others): VS `pos = mul(float4(position.xy, 0, 1),
  m_viewport)`, `col0 = color`, `tex0 = float4(texcoord.xy, 0, 1)` (q = 1
  for the `2D_PROJECTIVE` stage); PS `return tex2D(t, tex0.xy) * col0`.
  Check in the compiled output that COLOR0 lands on attribute slot 3 and
  combiner input V0. `FillRGBA` binds the white texture: one program pair.
- **2D matrix** at constant 96, row-vector convention like
  `nv2a_matrix_viewport`: `M[0][0] = nv2a_width / vw`, `M[1][1] =
  nv2a_height / vh`, `M[2][2] = 0`, `M[3][2] = 32768`, `M[3][3] = 1`; no
  y flip (engine 2D is y-down). `vw`/`vh` is the engine's 2D size
  (640x480). Pixel-centre offset (0 or -0.5) unverified: a cvar, checked
  with the `TF_NEAREST` console font on hardware.
- **Entering 2D** (lazily on the first 2D draw of a frame, and on
  `R_Set2DMode(true)`): upload the 2D programs (once per entry - any other
  path that uploads its own program clears a "2D resident" flag); depth
  test 0, depth mask 0, cull 0; alpha test GREATER ref 0 (ref/gl's 2D
  default); stage 1 off; constants, array formats, pointers; optionally
  `NV097_BREAK_VERTEX_BUFFER_CACHE` 0 (the ring is rewritten each frame).
- **`GL_SetRenderMode`** (from `ref/gl/gl_backend.c`), with a shadow copy
  so only changes are pushed, and a batch flush on any change:
  - `kRenderNormal` (default): blend off, alpha test off;
  - `kRenderTransColor`, `kRenderTransTexture`: blend ADD, SRC_ALPHA /
    ONE_MINUS_SRC_ALPHA, alpha test off;
  - `kRenderTransAlpha`: blend off, alpha test GREATER 0;
  - `kRenderGlow`, `kRenderTransAdd`: blend SRC_ALPHA / ONE;
  - `kRenderScreenFadeModulate` (0x1000): blend ZERO / SRC_COLOR.
  Do not reuse `nv2a_set_render_mode` (world; it uses CONSTANT_ALPHA).
- **`FillRGBA`** (as `ref/gl/gl_context.c`): white texture; blend SRC_ALPHA
  / ONE for `kRenderTransAdd`, else SRC_ALPHA / ONE_MINUS_SRC_ALPHA; draw
  with (r,g,b,a); afterwards blend off and the current colour left at
  (r,g,b,a), as `pglColor4ub` persists; alpha test untouched.
- **`Color4ub` / `Color4f`:** one global current colour, shared by TriAPI
  and the font code.

## 3. State around the world draw

Done in the frame-structure change: `nv2a_set_3d_state` (depth test and
mask on, cull on, blend off) at the top of `nv2a_draw_map_geometry` and
`nv2a_draw_test_triangle`. `R_Set2DMode(false)` should push the same.

## 4. Pitfalls

- **Pushbuffer overflow is silent in release builds** (the check is
  DBG-only, `pbkit.c`): 512 KiB = 128K dwords between `pb_reset`s, shared
  with the world's inline index pushes. Track dwords used from
  `R_BeginFrame`; above ~75%, `pb_reset()` mid-frame outside any
  begin/end block (register state persists).
- **Texture cache:** no invalidate method found in nxdk's `nv_regs.h`
  (nouveau's nv20 0x1fd8 is unverified); the deferred-free and
  stall-before-update rules avoid needing it. In-place `GL_UpdateTexture`
  between frames needs one hardware test.
- **xemu hides errors:** it accepts a zero convolution filter, does not
  model DMA timing or unified memory, and may wrap linear textures
  differently. Confirm on the console: filter values, the pixel offset,
  colour byte order (a red `FillRGBA` must be red).
- **`RefGetParm` reports `CONTEXT_TYPE_SOFTWARE`**: check which engine
  branches key off it before the menu work.
- **Sprite frames** are loaded through `GL_LoadTexture` (`cl_sprite.c`)
  before ref/nv2a can draw sprites; they will take arena memory - upload
  them now, or register name and size only as a documented stopgap.
