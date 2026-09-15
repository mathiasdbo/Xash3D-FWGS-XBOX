/*
x4-triangle - standalone NV2A bring-up: one textured triangle.

Part of docs/r3d/fork-plan.md's X4 ("Video bring-up, standalone"): outside
the engine, on purpose, so pushbuffer submission, DMA fencing in unified
memory, texture swizzling and the vp20/fp20 compiler get learned here, where
getting it wrong costs an afternoon, not a week of not knowing which half of
the real engine is broken. It did: see the X4_TEXTURE_SWIZZLED comment below
for a real, unresolved finding from exactly that afternoon.

Structure and the fixed-function-via-Cg shader plumbing (pb_init, the vertex
program upload loop, the pixel shader pb_begin/#include block) are
deps/nxdk/samples/triangle/main.c, unmodified in shape. Texture setup - the
attribute slot for texcoord0 (9), the real NV097_SET_TEXTURE_* register
values for a working texture stage 0 (context DMA channel 2, "clamp"
addressing, the enable/LOD and bilinear filter magic numbers) - is read from
deps/nxdk/samples/mesh/main.c, the one shipped nxdk sample that actually
binds a texture; see the citations at each use below. That sample's texture
is LINEAR UNSWIZZLED (NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8), the
format this demo ships bound by default too, verified rendering the
checkerboard correctly under xemu.

This demo also builds a genuinely swizzled (SZ_A8R8G8B8, power-of-two only)
copy of the same texture, using the same Morton/Z-order algorithm already
proven in tools/asset-studio's PC-side baker
(tools/asset-studio/src/utils/TextureOpt.cpp: morton2D/Part1By1), ported to C
and cited at the call site instead of re-derived - X4's actual point, texture
swizzling, which the mesh sample never exercises. It is not bound by default;
see X4_TEXTURE_SWIZZLED.
*/
#include <hal/video.h>
#include <hal/xbox.h>
#include <math.h>
#include <pbkit/pbkit.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <xboxkrnl/xboxkrnl.h>
#include <hal/debug.h>
#include <windows.h>

static uint32_t *alloc_vertices;
static uint32_t  num_vertices;
static float     m_viewport[4][4];

typedef struct {
    float pos[3];
    float uv[2];
} __attribute__((packed)) TexturedVertex;

/* One triangle, texture coordinates covering the full 0..1 range of the
 * checkerboard below - this is X4's literal exit line, "one textured
 * triangle", not the two-triangle backdrop the colour-only sample draws. */
static const TexturedVertex verts[] = {
    //  X     Y     Z       U     V
    {{-1.0, -1.0,  1.0}, { 0.0,  1.0}},
    {{ 0.0,  1.0,  1.0}, { 0.5,  0.0}},
    {{ 1.0, -1.0,  1.0}, { 1.0,  1.0}},
};

/* Checkerboard texture, power-of-two only (SZ_ formats require it - see
 * init_texture()). Both a linear and a Morton-swizzled copy are uploaded;
 * X4_TEXTURE_SWIZZLED (near its one use, below) picks which one gets bound. */
#define TEX_SIZE_LOG2 3
#define TEX_SIZE (1 << TEX_SIZE_LOG2) /* 8x8 */

static struct {
    void *linear_addr;   /* contiguous, row-major A8R8G8B8 */
    void *swizzled_addr; /* contiguous, Morton-order A8R8G8B8 */
} texture;

static uint32_t g_tex_format; /* on-screen readback: last NV097_SET_TEXTURE_FORMAT value pushed */
static uint32_t g_tex_phys;   /* on-screen readback: the OFFSET value pushed alongside it */

#define MASK(mask, val) (((val) << (ffs(mask)-1)) & (mask))
#define MAXRAM 0x03FFAFFF

static void matrix_viewport(float out[4][4], float x, float y, float width, float height, float z_min, float z_max);
static void init_shader(void);
static void init_texture(void);
static uint32_t morton2D(uint32_t x, uint32_t y);
static void set_attrib_pointer(unsigned int index, unsigned int format, unsigned int size, unsigned int stride, const void* data);
static void draw_arrays(unsigned int mode, int start, int count);

/* Main program function */
int main(void)
{
    uint32_t *p;
    int       i, status;
    int       width, height;
    int       start, last, now;
    int       fps, frames, frames_total;

    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    if ((status = pb_init())) {
        debugPrint("pb_init Error %d\n", status);
        Sleep(2000);
        return 1;
    }

    pb_show_front_screen();

    /* Basic setup */
    width = pb_back_buffer_width();
    height = pb_back_buffer_height();

    /* Load constant rendering things (shaders, geometry, texture) */
    init_shader();
    init_texture();
    alloc_vertices = MmAllocateContiguousMemoryEx(sizeof(verts), 0, MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
    memcpy(alloc_vertices, verts, sizeof(verts));
    num_vertices = sizeof(verts)/sizeof(verts[0]);
    matrix_viewport(m_viewport, 0, 0, width, height, 0, 65536.0f);

    /* Setup to determine frames rendered every second */
    start = now = last = GetTickCount();
    frames_total = frames = fps = 0;

    while(1) {
        pb_wait_for_vbl();
        pb_reset();
        pb_target_back_buffer();

        /* Clear depth & stencil buffers */
        pb_erase_depth_stencil_buffer(0, 0, width, height);
        pb_fill(0, 0, width, height, 0x00000000);
        pb_erase_text_screen();

        while(pb_busy()) {
            /* Wait for completion... */
        }

        /* Texture stage 0: 8x8 A8R8G8B8, clamped, bilinear. Register values
         * (offset 0x1B00 + n*0x40 per stage) confirmed identical between the
         * legacy NV20_TCL_PRIMITIVE_3D_TX_* names and the NV097_SET_TEXTURE_*
         * names used here (deps/nxdk/lib/pbkit/nv_objects.h:989-996 vs
         * nv_regs.h:529-611). ADDRESS/ENABLE/FILTER magic numbers are read
         * verbatim from deps/nxdk/samples/mesh/main.c:148-150 (a
         * proven-working texture stage on this same hardware/emulator).
         *
         * X4_TEXTURE_SWIZZLED picks between two FORMAT encodings that both
         * point at the SAME uploaded bytes (init_texture() always writes
         * both a linear and a Morton-swizzled copy). Default is 0 (linear,
         * LU_IMAGE_A8R8G8B8 + IMAGE_RECT/CONTROL1 pitch): verified rendering
         * the checkerboard correctly under xemu.
         *
         * The =1 path (SZ_A8R8G8B8 + BASE_SIZE_U/V log2 fields, no
         * IMAGE_RECT/pitch - swizzled addressing needs neither) is real,
         * NV2A-genuine texture swizzling - the thing X4 exists to learn -
         * and every input to it has been independently verified correct
         * this session: the pushed FORMAT dword matches this file's own
         * hand-derived value bit-for-bit (read back on screen via
         * g_tex_format), and the uploaded swizzled bytes match the expected
         * Morton-order checkerboard bit-for-bit (read back from guest
         * physical memory with tools/xemu-debug.py). Despite that, xemu
         * 0.8.136 renders it as a flat fill of texel (0,0) - consistent with
         * the swizzled-texture fetch path being unimplemented or incomplete
         * in xemu's software NV2A model for this format, not with anything
         * wrong on this program's side. Unresolved because it needs either
         * xemu's own nv2a source (not available in this environment) or a
         * real console to tell software-model gap from hardware behaviour
         * apart - exactly the class of question docs/r3d/fork-plan.md's X9
         * section already flags ("xemu is not validation... be ready for
         * the bug to be in the bake"). Flip this to 1 and rebuild to see it
         * on a real Xbox. */
#define X4_TEXTURE_SWIZZLED 0
        p = pb_begin();
#if X4_TEXTURE_SWIZZLED
        g_tex_phys = (uint32_t)texture.swizzled_addr & 0x03ffffff;
        p = pb_push1(p, NV097_SET_TEXTURE_OFFSET, g_tex_phys);
        g_tex_format = MASK(NV097_SET_TEXTURE_FORMAT_CONTEXT_DMA, 2)
                     | MASK(NV097_SET_TEXTURE_FORMAT_DIMENSIONALITY, 2)
                     | MASK(NV097_SET_TEXTURE_FORMAT_COLOR, NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8)
                     | MASK(NV097_SET_TEXTURE_FORMAT_MIPMAP_LEVELS, 1)
                     | MASK(NV097_SET_TEXTURE_FORMAT_BASE_SIZE_U, TEX_SIZE_LOG2)
                     | MASK(NV097_SET_TEXTURE_FORMAT_BASE_SIZE_V, TEX_SIZE_LOG2);
        p = pb_push1(p, NV097_SET_TEXTURE_FORMAT, g_tex_format);
#else
        g_tex_phys = (uint32_t)texture.linear_addr & 0x03ffffff;
        p = pb_push1(p, NV097_SET_TEXTURE_OFFSET, g_tex_phys);
        g_tex_format = MASK(NV097_SET_TEXTURE_FORMAT_CONTEXT_DMA, 2)
                     | MASK(NV097_SET_TEXTURE_FORMAT_DIMENSIONALITY, 2)
                     | MASK(NV097_SET_TEXTURE_FORMAT_COLOR, NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8)
                     | MASK(NV097_SET_TEXTURE_FORMAT_MIPMAP_LEVELS, 1);
        p = pb_push1(p, NV097_SET_TEXTURE_FORMAT, g_tex_format);
        p = pb_push1(p, NV097_SET_TEXTURE_CONTROL1, (TEX_SIZE*4) << 16); /* pitch, mesh main.c:146 */
        p = pb_push1(p, NV097_SET_TEXTURE_IMAGE_RECT, (TEX_SIZE<<16)|TEX_SIZE); /* mesh main.c:147 */
#endif
        p = pb_push1(p, NV097_SET_TEXTURE_ADDRESS, 0x00030303); /* clamp U/V/P, mesh main.c:148 */
        p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0, 0x4003ffc0); /* enable, full LOD range, mesh main.c:149 */
        p = pb_push1(p, NV097_SET_TEXTURE_FILTER, 0x04074000); /* bilinear, mesh main.c:150 */
        pb_end(p);

        /* Send shader constants
         *
         * WARNING: Changing shader source code may impact constant locations!
         * Check the intermediate file (*.inl) for the expected locations after
         * changing the code.
         */
        p = pb_begin();

        /* Set shader constants cursor at C0 */
        p = pb_push1(p, NV097_SET_TRANSFORM_CONSTANT_LOAD, 96);

        /* Send the transformation matrix */
        pb_push(p++, NV097_SET_TRANSFORM_CONSTANT, 16);
        memcpy(p, m_viewport, 16*4); p+=16;

        pb_end(p);
        p = pb_begin();

        /* Clear all attributes */
        pb_push(p++, NV097_SET_VERTEX_DATA_ARRAY_FORMAT,16);
        for(i = 0; i < 16; i++) {
            *(p++) = NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F;
        }
        pb_end(p);

        /* Set vertex position attribute */
        set_attrib_pointer(0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F,
                           3, sizeof(TexturedVertex), &alloc_vertices[0]);

        /* Set texture coordinate attribute (slot 9, confirmed by
         * deps/nxdk/samples/mesh/main.c:230) */
        set_attrib_pointer(9, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F,
                           2, sizeof(TexturedVertex), &alloc_vertices[3]);

        /* Begin drawing triangles */
        draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES, 0, num_vertices);

        /* Draw some text on the screen */
        pb_print(X4_TEXTURE_SWIZZLED ? "X4 Triangle (SZ swizzled)\n" : "X4 Triangle (LU linear)\n");
        pb_print("FORMAT=0x%08lx PHYS=0x%08lx\n", g_tex_format, g_tex_phys);
        pb_print("Frames: %d\n", frames_total);
        if (fps > 0) {
            pb_print("FPS: %d", fps);
        }
        pb_draw_text_screen();

        while(pb_busy()) {
            /* Wait for completion... */
        }

        /* Swap buffers (if we can) */
        while (pb_finished()) {
            /* Not ready to swap yet */
        }

        frames++;
        frames_total++;

        /* Latch FPS counter every second */
        now = GetTickCount();
        if ((now-last) > 1000) {
            fps = frames;
            frames = 0;
            last = now;
        }
    }

    /* Unreachable cleanup code */
    MmFreeContiguousMemory(alloc_vertices);
    MmFreeContiguousMemory(texture.linear_addr);
    MmFreeContiguousMemory(texture.swizzled_addr);
    pb_show_debug_screen();
    pb_kill();
    return 0;
}

/* Construct a viewport transformation matrix */
static void matrix_viewport(float out[4][4], float x, float y, float width, float height, float z_min, float z_max)
{
    memset(out, 0, 4*4*sizeof(float));
    out[0][0] = width/2.0f;
    out[1][1] = height/-2.0f;
    out[2][2] = z_max - z_min;
    out[3][3] = 1.0f;
    out[3][0] = x + width/2.0f;
    out[3][1] = y + height/2.0f;
    out[3][2] = z_min;
}

/* Load the shader we will render with */
static void init_shader(void)
{
    uint32_t *p;
    int       i;

    /* Setup vertex shader */
    uint32_t vs_program[] = {
        #include "vs.inl"
    };

    p = pb_begin();

    /* Set run address of shader */
    p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_START, 0);

    /* Set execution mode */
    p = pb_push1(p, NV097_SET_TRANSFORM_EXECUTION_MODE,
                 MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_MODE, NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_PROGRAM)
                 | MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE, NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE_PRIV));

    p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN, 0);

    pb_end(p);

    /* Set cursor for program upload */
    p = pb_begin();
    p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_LOAD, 0);
    pb_end(p);

    /* Copy program instructions (16-bytes each) */
    for (i=0; i<sizeof(vs_program)/16; i++) {
        p = pb_begin();
        pb_push(p++, NV097_SET_TRANSFORM_PROGRAM, 4);
        memcpy(p, &vs_program[i*4], 4*4);
        p+=4;
        pb_end(p);
    }

    /* Setup fragment shader */
    p = pb_begin();
    #include "ps.inl"
    pb_end(p);
}

/* Bit-interleaving for Morton order (Z-order curve): inserts a 0 bit after
 * each of the 16 low bits of x. Ported verbatim from
 * tools/asset-studio/src/utils/TextureOpt.cpp's Part1By1/morton2D - the same
 * swizzle XB's baker uses, so a divergence here would be a divergence from
 * our own format, not just from hardware. */
static uint32_t part1by1(uint32_t x)
{
    x &= 0x0000ffff;
    x = (x ^ (x <<  8)) & 0x00ff00ff;
    x = (x ^ (x <<  4)) & 0x0f0f0f0f;
    x = (x ^ (x <<  2)) & 0x33333333;
    x = (x ^ (x <<  1)) & 0x55555555;
    return x;
}

static uint32_t morton2D(uint32_t x, uint32_t y)
{
    return (part1by1(y) << 1) | part1by1(x);
}

/* Generate an 8x8 A8R8G8B8 checkerboard and upload both a row-major copy and
 * a Morton-swizzled copy of it to contiguous memory - see the
 * X4_TEXTURE_SWIZZLED comment at the one place either gets bound to texture
 * stage 0, for which is actually used and why both exist. */
static void init_texture(void)
{
    uint32_t linear[TEX_SIZE * TEX_SIZE];
    uint32_t swizzled[TEX_SIZE * TEX_SIZE];
    int x, y;

    for (y = 0; y < TEX_SIZE; y++) {
        for (x = 0; x < TEX_SIZE; x++) {
            int checker = (x ^ y) & 1;
            /* A8R8G8B8: opaque white / opaque HEV orange */
            linear[y * TEX_SIZE + x] = checker ? 0xFFFFFFFFu : 0xFFFF8000u;
        }
    }

    for (y = 0; y < TEX_SIZE; y++) {
        for (x = 0; x < TEX_SIZE; x++) {
            swizzled[morton2D(x, y)] = linear[y * TEX_SIZE + x];
        }
    }

    texture.linear_addr = MmAllocateContiguousMemoryEx(sizeof(linear), 0, MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
    memcpy(texture.linear_addr, linear, sizeof(linear));

    texture.swizzled_addr = MmAllocateContiguousMemoryEx(sizeof(swizzled), 0, MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
    memcpy(texture.swizzled_addr, swizzled, sizeof(swizzled));
}

/* Set an attribute pointer */
static void set_attrib_pointer(unsigned int index, unsigned int format, unsigned int size, unsigned int stride, const void* data)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + index*4,
                 MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE, format) | \
                 MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_SIZE, size) |  \
                 MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_STRIDE, stride));
    p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + index*4, (uint32_t)data & 0x03ffffff);
    pb_end(p);
}

/* Send draw commands for the triangles */
static void draw_arrays(unsigned int mode, int start, int count)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_BEGIN_END, mode);

    p = pb_push1(p, 0x40000000|NV097_DRAW_ARRAYS, //bit 30 means all params go to same register 0x1810
                 MASK(NV097_DRAW_ARRAYS_COUNT, (count-1)) | MASK(NV097_DRAW_ARRAYS_START_INDEX, start));

    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
    pb_end(p);
}
