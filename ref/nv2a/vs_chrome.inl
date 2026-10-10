// vs_chrome.vp - the chrome tail of vs_studio (docs/r3d/divergences.md
// #170): R_StudioSetupChrome's texture coordinates on the NV2A's vertex
// unit. Not a program on its own: it is loaded over vs_studio's last two
// slots (and the 26 after them), so the GPU runs vs_studio's first 104
// instructions - position, local lights, lighting and colour, all
// unchanged - and then these 28 instead of vs_studio's texture
// coordinates. It reads what that prefix leaves: R10 = R n, the vertex
// bone's rotation applied to the model-space normal (vs_studio.vp keeps
// it there for this). Compiled with deps/nxdk/tools/vp20compiler, like
// vs_studio.vp; vs_chrome.inl is its unedited output
// (vp20compiler vs_chrome.vp, its one warning line left out).
//
// What it computes, per vertex - R_StudioSetupChrome and
// R_StudioDrawChromeMesh in the reference (ref/gl/gl_studio.c:1120-1153
// and :1696, or R_StudioBuildArrayChromeMesh's :1873-1874 on the default
// r_studio_drawelements path - the same arithmetic; xash3d-fwgs c7768c48):
//   tmp   = normalize( bone origin - chrome_origin )     (:1129-1134)
//   up    = normalize( cross( tmp, RI.vright ))          (:1135-1136)
//   right = normalize( cross( up, tmp ))                 (:1137-1138)
//   s     = ( dot( n, R^T right ) + 1 ) * 32 * ( 1 / width )   (:1147-1148, :1696)
//   t     = ( dot( n, R^T up ) + 1 ) * 32 * ( 1 / height )     (:1151-1152, :1696)
// then the bound texture's coordinate scale, as vs_studio's. The bone is
// the normal's (gl_studio.c:2042 passes *pnormbone); the program has the
// vertex bone's rows, so ref/nv2a draws a chrome mesh here only when the
// two are the same bone at every vertex (nv2a_sgpu_mesh_t.same_bone; every
// retail chrome mesh). R^T is the bone's lighttransform transposed
// (Matrix3x4_VectorIRotate, :1140-1141); dot( n, R^T u ) = dot( R n, u ),
// and the CPU side draws here only when lighttransform equals the
// bonestransform the rows hold. Not bit-exact: RSQ for 1 / sqrt, and the
// dot taken after the rotation instead of before; the host test
// (tools/host-tests/nv2a_studio_gpu_host_test.c) measures the difference.
//
// Constants (c[n] is hardware constant n + 96):
//   c[94]     ( chrome_origin, NV2A_SGPU_CHROME_TINY ) - g_studio.chrome_origin at the draw
//   c[95]     ( RI.vright, 0 )
//   c[12]     the mesh's ( 1/width, 1/height, texture width, height ), as vs_studio
//   c[14]     ( 1, 255, 1/255, 32 ), as vs_studio
//   c[9].zw   ( 0, 1 ): q
// The bone's rows are c[A0.x + 0..2] with A0 from v[1].x, as vs_studio's
// position block; .w of each row is the bone's origin.
// The instruction after the ARL does not read A0, as in vs_studio.
//
// Each squared length is floored at c[94].w (1e-20) before its RSQ. The
// reference's VectorNormalize leaves a zero vector at zero (its 1 / length
// is skipped when the length is 0, xash3d_mathlib.h:131), so a bone at the
// chrome origin gets up = right = 0 and the coordinates ( 32, 32 ) * scale;
// without the floor RSQ( 0 ) is infinite and 0 * inf a NaN. With it the
// zero vector stays zero, and no length above 1e-10 changes.
//
// The first instruction is not part of the tail: vp20compiler refuses a
// program that never writes o[HPOS], so it is here only for the compiler,
// and r_context.c loads the .inl from its second instruction on
// (nv2a_vp_chrome_words + 4). Each instruction encodes alone - registers
// and constants are named, nothing is allocated - so dropping it changes
// no other word.
0x00000000, 0x0020001b, 0x0836106c, 0x2070f800,
0x00000000, 0x01a00200, 0x0836106c, 0x207007f8,
0x00000000, 0x002d201b, 0x0c36106c, 0x20703848,
0x00000000, 0x002c00ff, 0x0c36106c, 0x280007fa,
0x00000000, 0x002c20ff, 0x0c36106c, 0x240007fa,
0x00000000, 0x002c40ff, 0x0c36106c, 0x220007fa,
0x00000000, 0x0077c01b, 0x0436146c, 0x3e0007f8,
0x00000000, 0x00a0001b, 0x0436086c, 0x210007f8,
0x00000000, 0x0157c0ff, 0x05fe186c, 0x210007f8,
0x00000000, 0x0800001b, 0x08361300, 0x101807f8,
0x00000000, 0x0040001b, 0x0400286c, 0x2e0007f8,
0x00000000, 0x0057e087, 0x04c6186c, 0x2e2007f8,
0x00000000, 0x0097e063, 0x050e1c6c, 0x9e2007f8,
0x00000000, 0x00a0001b, 0x2436486c, 0x212007f8,
0x00000000, 0x0157c0ff, 0x25fe186c, 0x212007f8,
0x00000000, 0x0800001b, 0x08361300, 0x901407f8,
0x00000000, 0x0040001b, 0x24aa286c, 0x2e2007f8,
0x00000000, 0x00400087, 0x24c6086c, 0x2e3007f8,
0x00000000, 0x00800063, 0x250e0c6c, 0xde3007f8,
0x00000000, 0x00a0001b, 0x3436686c, 0x213007f8,
0x00000000, 0x0157c0ff, 0x35fe186c, 0x213007f8,
0x00000000, 0x0800001b, 0x08361300, 0xd01207f8,
0x00000000, 0x0040001b, 0x3554286c, 0x2e3007f8,
0x00000000, 0x00a0001b, 0xa436686c, 0x284007f8,
0x00000000, 0x00a0001b, 0xa436486c, 0x244007f8,
0x00000000, 0x006dc01b, 0x44361000, 0x3c4007f8,
0x00000000, 0x004dc01b, 0x45fe186c, 0x2c4007f8,
0x00000000, 0x004d801b, 0x4436186c, 0x2c4007f8,
0x00000000, 0x004d801b, 0x4576186c, 0x2070c849,
