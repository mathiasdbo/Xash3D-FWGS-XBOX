/*
nv2a_texanim_util.h - X9-X item 14 step 9: animated brush textures for
ref/nv2a, the pure half (no pbkit, no engine headers), host-tested in
tools/host-tests/nv2a_texanim_util_host_test.c - the same split
nv2a_lightmap_util.h and nv2a_sprite_util.h use.

What it ports:

- R_TextureAnimation (ref/gl/gl_rsurf.c:518, root clone) and its
  chain-level twin R_TextureAnim (gl_rsurf.c:467): which frame of a
  texture's animation chain a surface draws - the alternate set (+a..+j)
  when the entity's frame is non-zero, the +0..+9 frames by time, and the
  '-' random tiles by the surface's texturemins through rtable.
  The walk runs on engine texture INDICES (mod->textures[]) instead of
  texture_t pointers: nv2a_texanim_t copies each texture_t's anim_total/
  anim_min/anim_max and turns anim_next/alternate_anims into indices, so
  the renderer can map the result to its own texture directory with one
  array read.
- The selection rule of the frames no face references, which the baker
  appends to the texture directory (tools/asset-studio/src/formats/
  AnimTextureFrames.h, unreferencedAnimationFrames). ref/nv2a must find
  them by position, so this mirrors that function exactly, including its
  use of Mod_NameImpliesTextureIsAnimated (engine/common/mod_bmodel.c:703)
  and the name + 2 group key of Mod_SequenceAnimatedTexture
  (mod_bmodel.c:3043).

Where it differs from R_TextureAnimation:
- speed is always 20 ticks per second. ref/gl uses 10 for a texture whose
  gl texture has TF_QUAKEPAL (gl_rsurf.c:544), which only a Quake-palette
  miptex gets (IMAGE_QUAKEPAL, imagelib img_wad.c:493/566); the MPAK baker
  bakes BSP30 maps only, whose miptexes carry their own palette.
*/
#ifndef NV2A_TEXANIM_UTIL_H
#define NV2A_TEXANIM_UTIL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NV2A_TEXANIM_NONE       0xFFFFu /* no texture / not in the directory */
#define NV2A_TEXANIM_MOD_FRAMES 20      /* MOD_FRAMES, common/com_model.h:577 */
#define NV2A_TEXANIM_SPEED      20      /* gl_rsurf.c:546, non-Quake-palette textures */

/* One engine texture (mod->textures[i]) as the animation walk needs it. */
typedef struct
{
	int16_t  anim_total;  /* texture_t::anim_total: frames * ANIM_CYCLE (<= 20), 0 = not animated */
	int16_t  anim_min;    /* texture_t::anim_min */
	int16_t  anim_max;    /* texture_t::anim_max */
	uint16_t anim_next;   /* index of texture_t::anim_next, NV2A_TEXANIM_NONE if NULL */
	uint16_t alternate;   /* index of texture_t::alternate_anims, NV2A_TEXANIM_NONE if NULL */
	uint16_t width;       /* texture_t::width, the miptex's own (random tiling) */
	uint16_t height;      /* texture_t::height */
	uint8_t  random;      /* name[0] == '-' (gl_rsurf.c:531) */
	uint8_t  pad;
} nv2a_texanim_t;

/* (int)( time * speed ), gl_rsurf.c:549 - once per frame. */
int nv2a_texanim_ticks( double time );

/* Mod_NameImpliesTextureIsAnimated (mod_bmodel.c:703), without its error
   print: '+' or '-', then '0'..'9' or 'a'..'j'. */
int nv2a_texanim_name_animated( const char *name );

/* The texture a surface draws, as an engine texture index -
   R_TextureAnimation (gl_rsurf.c:518) when has_surface is non-zero,
   R_TextureAnim (gl_rsurf.c:467, "do not process random tiling": a '-'
   chain returns base) when it is zero. base is the surface's own
   texinfo->texture; alternate is "the entity's curstate.frame is
   non-zero". Returns base when the walk fails, as the reference does. */
uint32_t nv2a_texanim_resolve( const nv2a_texanim_t *tab, uint32_t count, uint32_t base, int alternate, int ticks,
	int has_surface, int texturemins_s, int texturemins_t, const uint16_t rtable[NV2A_TEXANIM_MOD_FRAMES][NV2A_TEXANIM_MOD_FRAMES] );

/* Whether the texture base resolves differently per surface - the random
   tiling branch of R_TextureAnimation (gl_rsurf.c:531) - so a caller that
   resolves once per texture must resolve this one per surface instead. */
int nv2a_texanim_per_surface( const nv2a_texanim_t *tab, uint32_t count, uint32_t base, int alternate );

/* The engine textures, ascending, that the baker appended to the texture
   directory after the drawn ones: unused (used[i] == 0), animated, and in
   a group with a used animated texture. names[i] is texture_t::name (the
   engine's own lower-cased, 15-character copy) or NULL for no texture.
   Writes at most out_cap indices; returns how many there are in all. */
uint32_t nv2a_texanim_extra_frames( const char *const *names, const uint8_t *used, uint32_t count, uint16_t *out, uint32_t out_cap );

#ifdef __cplusplus
}
#endif

#endif /* NV2A_TEXANIM_UTIL_H */
