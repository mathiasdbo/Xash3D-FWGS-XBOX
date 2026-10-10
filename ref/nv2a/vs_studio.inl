// vs_studio.vp - studio model skinning and lighting on the NV2A's vertex
// unit (docs/r3d/divergences.md #143). Written in NV_vertex_program 1.1
// assembly rather than Cg because the program depends on two things cgc
// does not let a source control: relative constant addressing that reaches
// below c[0] (the bone palette lives in hardware constants 0..95 as well as
// 128..183) and the order of the floating-point operations, kept as close
// to ref/gl's CPU arithmetic as the instruction set allows. Not bit-exact:
// the lambert term multiplies by a precomputed 1/r where gl_studio.c:1359
// divides by r, DP3/DP4 sum in an order the hardware does not document, and
// the gamma table is piecewise linear. Each can move a colour byte by 1
// (tools/host-tests/nv2a_studio_gpu_host_test.c measures how often). Compiled with
// deps/nxdk/tools/vp20compiler (the second stage of the cgc + vp20compiler
// pipeline the other vs_*.inl files went through); vs_studio.inl is its
// unedited output.
//
// 106 instructions: with vs_lit (12), vs_2d (7) and vs_tri (11) it fills
// the NV2A's 136 program slots exactly, all four resident at once
// (r_context.c nv2a_vp_table). Any instruction added here needs one taken
// out, or one of the others made non-resident. vs_studio is first in
// program memory, and its first 104 instructions are also the prefix of
// the chrome program (vs_chrome.vp, divergences.md #170), whose 28-slot
// tail replaces the last two and runs on over vs_lit, vs_2d and the start
// of vs_tri when it is loaded: 104 + 28 = 132 of the 136 slots.
//
// What it computes, per vertex (gl_studio.c R_StudioDrawPoints, :1951):
//   position = bonestransform[vertbone] * v        (Matrix3x4_VectorTransform, :1997)
//              then the same view-projection, divide and fog as vs_tri
//   colour   = R_LightLambert( lightpos, normal, lightcolor * lv ) / 255, alpha a constant
//              (R_StudioDrawNormalMesh -> R_StudioSetColorBegin -> R_LightLambert, :1378-1427):
//              with no local light reaching the vertex, trunc( lightcolor * lv * 255 )
//   lv       = R_StudioLighting( normbone, flags, normal ) (gl_studio.c:1324-1369):
//              illum = ambient + shadelight - shadelight * max( 0, ( min( dot( n, blightvec ), 1 ) + ( r - 1 )) / r )
//              clamped to [0, 255], or ambient + shadelight * 0.8 (FLATSHADE), then
//              LightToTexGamma( (uint)( illum * 4 )) / 1023 through a 32-segment
//              piecewise-linear table in c[16..31], or 1.0 (FULLBRIGHT)
//   texcoord = ( s, t ) * per-mesh scale * bound texture size (TexCoord2f then Vertex3f)
//
// Local lights (R_StudioEntityLight's elights; up to two, more stay on the
// CPU path): R_LightStrength's light vector is L = v - R^T ( o - t ), the
// light's origin in the vertex bone's frame (gl_studio.c:1460-1468, R and t
// the bone's lighttransform, which the CPU side checks equals the
// bonestransform the program has). For an orthonormal R that is
// -L = R^T ( o - W ) with W = R v + t, the world position above, so
//   r     = -dot( n, L ) = dot( R n, o - W )   (+dot for a flipped view model)
//   |L|^2 = |o - W|^2
//   fl   += lc + r * R2 / |L|^3, for each light with r > 0
// and, when any light reached the vertex,
//   colour byte = ScreenGammaTable( (uint)( fl + LinearGammaTable( (uint)( colour * 1023 )))) >> 2
// with the two tables evaluated as the powers they tabulate
// (engine/client/gamma.c:77-78) by LIT: 1023 * colour^gamma without the
// table's index and value truncation, then floor( 255.75 * ( c / 1023 )^( 1 / gamma ))
// for the screen table and the shift. A sum above 1023 gives a byte above
// 255, which the colour output's clamp to [0, 1] (NV_vertex_program; xemu
// glsl/vsh.c clamps oD0) turns into the reference's 255. Each LIT carries
// the previous channel's result into the next one's x, its gate (+tiny,
// so a 0 result keeps the next channel open), and it comes back out in y,
// so three channels need one MOV to pack.
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
//             72 + p or 170 + (p - 24): reached as c[A0.x + 0..2] with A0 =
//             slot - 96 (nv2a_studio_gpu_util.h)
//   c[88..93] the local lights (nv2a_studio_gpu_util.h NV2A_SGPU_C_ELIGHT):
//             c[88], c[90] the two origins (world); c[89], c[91]
//             ( locallightcolor, locallightR2 ); c[92] ( gamma, 1 / gamma,
//             255.75^gamma / 1023, 1023 ); c[93] ( sign 0, sign 1, tiny, 0 ),
//             sign +1, -1 when flipped, 0 for an unused light
// Every ARL operand is an exact integer, so floor and round agree; the
// table pair's also gets +0.25 (it is never negative), so an EXP .y that is
// not exactly frac() on the console still lands on the right register.
// Each ARL is followed by an instruction that does not read A0 (no
// latency is documented for ARL; this costs nothing).
// R n stays in R10 to the end: the chrome tail (vs_chrome.vp, divergences.md
// #170) reads it after the last instruction below that it replaces.
// These two are the last two slots, which vs_chrome.vp's tail replaces
// (divergences.md #170): everything above is the chrome variant's too.
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
0x00000000, 0x00ac041b, 0x0836186c, 0x28a007fa,
0x00000000, 0x00ac241b, 0x0836186c, 0x24a007fa,
0x00000000, 0x00ac441b, 0x0836186c, 0x22a007fa,
0x00000000, 0x0077001b, 0x0c36146c, 0x5e2007f8,
0x00000000, 0x0077401b, 0x0c36146c, 0x5e3007f8,
0x00000000, 0x00a0001b, 0xa436486c, 0x286007f8,
0x00000000, 0x00a0001b, 0xa436686c, 0x246007f8,
0x00000000, 0x00a0001b, 0x2436486c, 0x226007f8,
0x00000000, 0x00a0001b, 0x3436686c, 0x216007f8,
0x00000000, 0x0057a01b, 0x6436186c, 0x2c6007f8,
0x00000000, 0x0157a01b, 0x6554186c, 0x236007f8,
0x00000000, 0x0800001b, 0x08361201, 0x907807f8,
0x00000000, 0x0800001b, 0x08361301, 0x907407f8,
0x00000000, 0x0040001b, 0x7436e86c, 0x2c8007f8,
0x00000000, 0x0040001b, 0x8436e86c, 0x2c8007f8,
0x00000000, 0x0040001b, 0x8436c86c, 0x2c8007f8,
0x00000000, 0x0177a0ff, 0x0c02c86c, 0x237007f8,
0x00000000, 0x00972000, 0x85fe186c, 0x3e9007f8,
0x00000000, 0x0040001b, 0x9554e86c, 0x2e9007f8,
0x00000000, 0x00976055, 0x85fe186c, 0x3e4007f8,
0x00000000, 0x0080001b, 0x45fee86e, 0x5e9007f8,
0x00000000, 0x014000aa, 0x75fee86c, 0x219007f8,
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
0x00000000, 0x004dc01b, 0x74aa186c, 0x2e2007f8,
0x00000000, 0x0a00001b, 0x08361000, 0x900407f8,
0x00000000, 0x0a00001b, 0x08361100, 0x901407f8,
0x00000000, 0x0a00001b, 0x08361200, 0x903407f8,
0x00000000, 0x00600000, 0x24361554, 0x182007f8,
0x00000000, 0x00600055, 0x24361554, 0x542007f8,
0x00000000, 0x006000aa, 0x24361554, 0xd22007f8,
0x00000000, 0x00378000, 0x0c36106c, 0x217007f8,
0x00000000, 0x0e00001b, 0x0836100d, 0xd04f07f8,
0x00000000, 0x0077a0aa, 0x443612a8, 0x387007f8,
0x00000000, 0x0e00001b, 0x0836106d, 0xd05f07f8,
0x00000000, 0x0077a0aa, 0x543612a8, 0x387007f8,
0x00000000, 0x0e00001b, 0x083610ad, 0xd06f07f8,
0x00000000, 0x00200055, 0x5436106c, 0x286007f8,
0x00000000, 0x0097801b, 0x65fe186e, 0x5e6007f8,
0x00000000, 0x0a00001b, 0x08361001, 0x900407f8,
0x00000000, 0x0a00001b, 0x08361101, 0x901407f8,
0x00000000, 0x0a00001b, 0x08361201, 0x903407f8,
0x00000000, 0x00600000, 0x64361554, 0x186007f8,
0x00000000, 0x00600055, 0x64361554, 0x546007f8,
0x00000000, 0x006000aa, 0x64361554, 0xd26007f8,
0x00000000, 0x0057801b, 0x6552186c, 0x2f6007f8,
0x00000000, 0x0e00001b, 0x0836100d, 0x904f07f8,
0x00000000, 0x0077a0aa, 0x443612a8, 0x386007f8,
0x00000000, 0x0e00001b, 0x0836106d, 0x905f07f8,
0x00000000, 0x0077a0aa, 0x543612a8, 0x386007f8,
0x00000000, 0x0e00001b, 0x083610ad, 0x908f07f8,
0x00000000, 0x00200055, 0x5436106c, 0x288007f8,
0x00000000, 0x0a00001b, 0x08361002, 0x100407f8,
0x00000000, 0x0a00001b, 0x08361102, 0x101407f8,
0x00000000, 0x0a00001b, 0x08361202, 0x103407f8,
0x00000000, 0x00600000, 0x84361554, 0x188007f8,
0x00000000, 0x00600055, 0x84361554, 0x548007f8,
0x00000000, 0x006000aa, 0x84361554, 0xd28007f8,
0x00000000, 0x0060001b, 0x8436146c, 0x9e8007f8,
0x00000000, 0x0080001b, 0x85ff286c, 0x9e2007f8,
0x00000000, 0x004dc01b, 0x2554186c, 0x2070e818,
0x00000000, 0x002d201b, 0x0c36106c, 0x20703848,
0x00000000, 0x004d801b, 0xb576186c, 0x2070c849,
