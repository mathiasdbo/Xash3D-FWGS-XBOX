// vs_studio.vp - studio model skinning and lighting on the NV2A's vertex
// unit (docs/r3d/divergences.md #143). Written in NV_vertex_program 1.1
// assembly rather than Cg because the program depends on two things cgc
// does not let a source control: relative constant addressing that reaches
// below c[0] (the bone palette lives in hardware constants 0..95 as well as
// 128..191) and the order of the floating-point operations, kept as close
// to ref/gl's CPU arithmetic as the instruction set allows. Not bit-exact:
// the lambert term multiplies by a precomputed 1/r where gl_studio.c:1359
// divides by r, DP3/DP4 sum in an order the hardware does not document, and
// the gamma table is piecewise linear. Each can move a colour byte by 1
// (tools/host-tests/nv2a_studio_gpu_host_test.c measures how often). Compiled with
// deps/nxdk/tools/vp20compiler (the second stage of the cgc + vp20compiler
// pipeline the other vs_*.inl files went through); vs_studio.inl is its
// unedited output.
//
// What it computes, per vertex (gl_studio.c R_StudioDrawPoints, :1951):
//   position = bonestransform[vertbone] * v        (Matrix3x4_VectorTransform, :1997)
//              then the same view-projection, divide and fog as vs_tri
//   colour   = trunc( lightcolor * lv * 255 ) / 255, alpha a constant
//              (R_StudioDrawNormalMesh -> R_StudioSetColorBegin -> R_LightLambert
//               with no local lights: VectorScale( color, 255, out ), gl_studio.c:1382)
//   lv       = R_StudioLighting( normbone, flags, normal ) (gl_studio.c:1324-1369):
//              illum = ambient + shadelight - shadelight * max( 0, ( min( dot( n, blightvec ), 1 ) + ( r - 1 )) / r )
//              clamped to [0, 255], or ambient + shadelight * 0.8 (FLATSHADE), then
//              LightToTexGamma( (uint)( illum * 4 )) / 1023 through a 32-segment
//              piecewise-linear table in c[16..31], or 1.0 (FULLBRIGHT)
//   texcoord = ( s, t ) * per-mesh scale * bound texture size (TexCoord2f then Vertex3f)
//
// Inputs:   v[0] OPOS model-space position, v[2] NRML model-space normal,
//           v[1] WGHT .x = A0 address of the vertex bone's 3 matrix rows,
//                     .y = A0 address of the normal bone's light vector,
//           v[8] TEX0 (hardware attribute 9) raw ( s, t ).
// Constants (c[n] is hardware constant n + 96, vp20compiler main.c:516;
//           c[0..4] are vs_tri/vs_lit's and are never written by this path):
//   c[5..8]   view-projection rows (the world's, or the view model's z range)
//   c[9]      ( ambient + shadelight, shadelight, 0, 1 )
//   c[10]     ( lambert selector, flat-shade illum or 0, 1 - fullbright, fullbright )
//   c[11]     ( lightcolor.rgb, alpha byte / 255 )
//   c[12]     ( 1/width or 1 per mesh, likewise for t, bound texture width, height )
//   c[13]     ( SHADE_LAMBERT - 1, 1 / SHADE_LAMBERT, 4, 1/64 )
//   c[14]     ( 1, 255, 1/255, 32 )
//   c[15]     ( 64, 0.25, 0, 0 )
//   c[16..31] the gamma table, two 32-entry segments per register:
//             ( base0, slope0, base1, slope1 ) in lv units
//   palette   rows at hardware 3p (p < 24) or 128 + 3(p - 24), light vector at
//             72 + p or 176 + (p - 24): reached as c[A0.x + 0..2] with A0 =
//             slot - 96 (nv2a_studio_gpu_util.h)
// Every ARL operand is an exact integer, so floor and round agree; the
// table pair's also gets +0.25 (it is never negative), so an EXP .y that is
// not exactly frac() on the console still lands on the right register.
// Each ARL is followed by an instruction that does not read A0 (no
// latency is documented for ARL; this costs nothing).
0x00000000, 0x01a00200, 0x0836106c, 0x207007f8,
0x00000000, 0x004d921b, 0x0836186c, 0x2cb007f8,
0x00000000, 0x00ec001b, 0x0836186c, 0x281007fa,
0x00000000, 0x00ec201b, 0x0836186c, 0x241007fa,
0x00000000, 0x00ec401b, 0x0836186c, 0x221007fa,
0x00000000, 0x004cc055, 0x1436186c, 0x2f0007f8,
0x00000000, 0x008ca000, 0x1436186c, 0x1f0007f8,
0x00000000, 0x008ce0aa, 0x1436186c, 0x1f0007f8,
0x00000000, 0x006d001b, 0x0436106c, 0x3f0007f8,
0x00000000, 0x0400001b, 0x08361300, 0x102807f8,
0x00000000, 0x0040001b, 0x0400486c, 0x2070e800,
0x00000000, 0x0020001b, 0x0436106c, 0x20701800,
0x00000000, 0x002000ff, 0x0436106c, 0x20708828,
0x00000000, 0x01a00240, 0x0836106c, 0x207007f8,
0x00000000, 0x002d60ff, 0x0c36106c, 0x20701818,
0x00000000, 0x00ac041b, 0x0836186c, 0x283007fa,
0x00000000, 0x012dc000, 0x3400186c, 0x283007f8,
0x00000000, 0x006da000, 0x34361000, 0x383007f8,
0x00000000, 0x004da000, 0x34aa186c, 0x283007f8,
0x00000000, 0x014d2000, 0x3554186c, 0x283007f8,
0x00000000, 0x004d2000, 0x34aa186c, 0x283007f8,
0x00000000, 0x006d2100, 0x34361000, 0x383007f8,
0x00000000, 0x014d2000, 0x3554186c, 0x283007f8,
0x00000000, 0x004d4000, 0x3400186c, 0x283007f8,
0x00000000, 0x006d4000, 0x34361154, 0x383007f8,
0x00000000, 0x012dc000, 0x34aa186c, 0x283007f8,
0x00000000, 0x004da000, 0x3554186c, 0x283007f8,
0x00000000, 0x0a00001b, 0x08361000, 0xd04407f8,
0x00000000, 0x00600000, 0x34361555, 0x183007f8,
0x00000000, 0x004da000, 0x35fe186c, 0x285007f8,
0x00000000, 0x0a00001b, 0x08361001, 0x505407f8,
0x00000000, 0x00600000, 0x54361555, 0x585007f8,
0x00000000, 0x006de000, 0x54361154, 0x385007f8,
0x00000000, 0x01a00000, 0x5436106c, 0x207007f8,
0x00000000, 0x004de055, 0x5400186c, 0x245007f8,
0x00000000, 0x018dc055, 0x55fe186c, 0x225007f8,
0x00000000, 0x008dc0aa, 0x57fe1955, 0x545007f8,
0x00000000, 0x006dc1aa, 0x54361000, 0x315007f8,
0x00000000, 0x004e001b, 0x0df4a86c, 0x2f6007fa,
0x00000000, 0x00600010, 0x643612e9, 0x9c6007f8,
0x00000000, 0x00400055, 0x64aaa86c, 0x246007f8,
0x00000000, 0x00600000, 0x64361155, 0x986007f8,
0x00000000, 0x004d4000, 0x6554186c, 0x286007f8,
0x00000000, 0x006d4000, 0x643613fc, 0x386007f8,
0x00000000, 0x004d601b, 0x0c00c86c, 0x2e7007f8,
0x00000000, 0x004dc01b, 0x74aa186c, 0x2e7007f8,
0x00000000, 0x0a00001b, 0x08361001, 0xd08407f8,
0x00000000, 0x0a00001b, 0x08361101, 0xd09407f8,
0x00000000, 0x0a00001b, 0x08361201, 0xd0a407f8,
0x00000000, 0x00600000, 0x74361556, 0x187007f8,
0x00000000, 0x00600055, 0x74361556, 0x547007f8,
0x00000000, 0x006000aa, 0x74361556, 0x927007f8,
0x00000000, 0x004dc01b, 0x7554186c, 0x2070e818,
0x00000000, 0x004d801b, 0xb576186c, 0x2070c848,
0x00000000, 0x002d201b, 0x0c36106c, 0x20703849,
