// vs_warp.vp - the water warp on the NV2A's vertex unit (R5,
// docs/r3d/divergences.md #147): EmitWaterPolys' per-vertex body
// (ref/gl/gl_rsurf.c:961-984 in the root clone) for the static water
// vertices r_warp.c builds at R_NewMap. Compiled with
// deps/nxdk/tools/vp20compiler; vs_warp.inl is its unedited output.
//
// The wave and the texture warp are the reference's quantised ones, not a
// smooth sine: each of the four table indices is (int)( ... ) & 255 with
// C's truncation toward zero, and each table value is warpsin.h's own
// float, read from constants. The arithmetic is the CPU path's
// (nv2a_warp_util.c nv2a_warp_wave_z_frame / nv2a_warp_texcoord_frame),
// operation for operation: this frame's time terms are split on the CPU
// into an integer base and a float fraction (nv2a_warp_frame_init), each
// index is the fraction plus the vertex's terms in float, in the same
// order, floored with EXP's .y (= x - floor(x)), plus one where the whole
// sum base + x is negative and has a fraction (truncation), plus
// base & 255, wrapped into 0..255 with EXP's .y again. Products that
// could round differently fused (MAD) are written as MUL then ADD; every
// MAD left is exact (products by 1 and -1, of 0 and 1, and -128 and -2
// times 0 or 1).
// tools/host-tests/nv2a_warp_util_host_test.c emulates this program and
// checks it against both the CPU path and the reference's double formula.
// Not bit-exact with the reference: its index sums are doubles (cl.time is
// a double), these are floats, as the CPU path's are, and the s/t index is
// reassociated to o * ( 0.125 * TURBSCALE ) + time * TURBSCALE; at an index
// boundary that is one table step (at most 0.1 unit of wave). Nor is
// "identical to the CPU path" established on the console: the emulation
// assumes IEEE single rounding for ADD/MUL and an exact EXP .y, which the
// NV2A does not document (xemu runs host floats, so its pixel-identical
// screenshots do not settle it). Unverified on hardware, with vs_studio's
// (divergences.md #143): negative-A0 reads below c[0], EXP .y as an exact
// frac, and loading program memory between draws with no drain.
//
// 59 instructions. Not resident: all 136 slots hold vs_lit, vs_2d, vs_tri
// and vs_studio (r_context.c nv2a_vp_table), so this program is loaded over
// vs_studio's last 59 slots when the water draws, and vs_studio's tail is
// put back when a studio model draws next (nv2a_select_vertex_program).
//
// Lanes, throughout: x = the first wave index (time * 160 + y + x),
//                    y = the second (x * 5 + time * 171 - y),
//                    z = s's index (( ot * 0.125 + time ) * TURBSCALE),
//                    w = t's index (( os * 0.125 + time ) * TURBSCALE).
//
// Inputs:   v[0] OPOS ( x, y, z ), the face's model-space vertex,
//           v[8] TEX0 (hardware attribute 9) ( os, ot ), its texinfo dot
//                products (R_TextureCoord for SURF_DRAWTURB, gl_rsurf.c:147).
// Constants (c[n] is hardware constant n + 96, vp20compiler main.c:516):
//   c[0..3]  the entity's model-view-projection rows (vs_tri's slot, the
//            same layout vs_studio.vp's c[5..8] uses)
//   c[5]     ( frac160, frac171, fracts, 0.125 * TURBSCALE )
//   c[6]     ( 1, 5, 1, -1 )
//   c[7]     ( -base160, -base171, -basets, -basets )
//   c[8]     ( base160 & 255, base171 & 255, basets & 255, basets & 255 ) / 256
//   c[9]     ( 1/256, 256, 128, -128 )
//   c[10]    ( 8, 0.8, waveHeight, 1/64 )
//   c[11]    ( -2, 1, table address bias, 0 )
//   c[12]    the colour ( r, g, b, a ) / 255
//   table    hardware 0..64: .x = r_turbsin[k], k = 0..64, reached as
//            c[A0.x + 0] with A0 = k + bias (nv2a_warp_util.h). The table
//            is exactly symmetric (r_turbsin[128 - k] = r_turbsin[k],
//            r_turbsin[k + 128] = -r_turbsin[k], checked by the host test),
//            so index j reads entry min( j', 128 - j' ), j' = j mod 128,
//            negated when j >= 128.
// Every ARL operand is an exact integer plus 0.25, so floor and round
// agree. Each ARL is followed by an instruction that does not read A0, as
// in vs_studio.vp.
0x00000000, 0x004cc04b, 0x0836186c, 0x2c2007f8,
0x00000000, 0x004cb204, 0x09fe186c, 0x232007f8,
0x00000000, 0x006ca01b, 0x2436106c, 0x3c1007f8,
0x00000000, 0x006ca01b, 0x243612a8, 0x331007f8,
0x00000000, 0x008cc01b, 0x0976186c, 0x5c1007f8,
0x00000000, 0x0a00001b, 0x08361000, 0x503407f8,
0x00000000, 0x00600000, 0x14361554, 0xd84007f8,
0x00000000, 0x0a00001b, 0x08361100, 0x503407f8,
0x00000000, 0x00600055, 0x14361554, 0xd44007f8,
0x00000000, 0x0a00001b, 0x08361200, 0x503407f8,
0x00000000, 0x006000aa, 0x14361554, 0xd24007f8,
0x00000000, 0x0a00001b, 0x08361300, 0x503407f8,
0x00000000, 0x006000ff, 0x14361554, 0xd14007f8,
0x00000000, 0x0160001b, 0x4436286c, 0x2f5007f8,
0x00000000, 0x016ce01b, 0x4436186c, 0x2f6007f8,
0x00000000, 0x0080001b, 0x5436c86d, 0x1f4007f8,
0x00000000, 0x004d201b, 0x4400186c, 0x2f4007f8,
0x00000000, 0x006d001b, 0x4436106c, 0x3f4007f8,
0x00000000, 0x0a00001b, 0x08361001, 0x103407f8,
0x00000000, 0x004d2055, 0x34aa186c, 0x287007f8,
0x00000000, 0x0a00001b, 0x08361101, 0x103407f8,
0x00000000, 0x004d2055, 0x34aa186c, 0x247007f8,
0x00000000, 0x0a00001b, 0x08361201, 0x103407f8,
0x00000000, 0x004d2055, 0x34aa186c, 0x227007f8,
0x00000000, 0x0a00001b, 0x08361301, 0x103407f8,
0x00000000, 0x004d2055, 0x34aa186c, 0x217007f8,
0x00000000, 0x018d201b, 0x7554186c, 0x2f8007f8,
0x00000000, 0x008d201b, 0x85fe186d, 0xdf7007f8,
0x00000000, 0x006d20aa, 0x0c36146d, 0xdf9007f8,
0x00000000, 0x0120001b, 0x7437286c, 0x2f7007f8,
0x00000000, 0x008d601b, 0x84001954, 0x3f8007f8,
0x00000000, 0x006d601b, 0x743612a8, 0x3f7007f8,
0x00000000, 0x01a00000, 0x7436106c, 0x207007f8,
0x00000000, 0x002d801b, 0x0c36106c, 0x2070f818,
0x00000000, 0x002c0000, 0x0c36106c, 0x28a007fa,
0x00000000, 0x01a00040, 0x7436106c, 0x207007f8,
0x00000000, 0x002d60fd, 0x0c36106c, 0x20703848,
0x00000000, 0x002c0000, 0x0c36106c, 0x24a007fa,
0x00000000, 0x01a00080, 0x7436106c, 0x207007f8,
0x00000000, 0x0040001b, 0xa437086c, 0x2ca007f8,
0x00000000, 0x002c0000, 0x0c36106c, 0x22a007fa,
0x00000000, 0x01a000c0, 0x7436106c, 0x207007f8,
0x00000000, 0x006d401b, 0xa4361000, 0x3cb007f8,
0x00000000, 0x002c0000, 0x0c36106c, 0x21a007fa,
0x00000000, 0x0040001b, 0xa437086c, 0x23a007f8,
0x00000000, 0x004d4055, 0xb4aa186c, 0x24b007f8,
0x00000000, 0x00600055, 0xb4361002, 0xd8b007f8,
0x00000000, 0x004d4000, 0xb554186c, 0x28b007f8,
0x00000000, 0x00600000, 0xb43612a8, 0x220007f8,
0x00000000, 0x004c2055, 0x0836186c, 0x2f1007f8,
0x00000000, 0x008c0000, 0x0836186c, 0x5f1007f8,
0x00000000, 0x008c40aa, 0x0436186c, 0x5f1007f8,
0x00000000, 0x006c601b, 0x1436106c, 0x3f1007f8,
0x00000000, 0x0400001b, 0x08361300, 0x502807f8,
0x00000000, 0x0040001b, 0x1400486c, 0x2070e800,
0x00000000, 0x0020001b, 0x1436106c, 0x20701800,
0x00000000, 0x002000ff, 0x1436106c, 0x20708828,
0x00000000, 0x0060121b, 0xa4361004, 0x23a007f8,
0x00000000, 0x004d40bb, 0xa5fe186c, 0x2070c849,
