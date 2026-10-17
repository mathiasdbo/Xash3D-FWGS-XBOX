/*
xbox_vox_stream.h - dialogue streamed from the loose WAVs instead of loaded whole
Copyright (C) 2026 Resonance3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

/*
Resonance3D (docs/r3d/divergences.md #177, X12 item 4 Stage A). The
engine side of the sound hooks: every call below sits in one `#if
XASH_XBOX` line (or arm) of upstream's sound code, and the design and the
reasons are in xbox_vox_stream.c's header comment.

What the hooks leave unchanged: the samples. A streamed sound's channel
reads its samples through S_RetrieveAudioSamples (s_main.c), and that
function hands back a pointer to the same bytes, and the same count, that
upstream's whole-file buffer would have given at the same position;
S_TrimStart/S_TrimEnd (s_vox.c) run upstream's own loops on the same
bytes. A byte not yet read is read (or waited for) before the pointer is
returned: an underrun is a wait, never silence.
*/

#ifndef XBOX_VOX_STREAM_H
#define XBOX_VOX_STREAM_H

/* wavdata_t->flags bits of ours, far from upstream's (SOUND_LOOPED,
   SOUND_STREAM and Sound_Process's SOUND_RESAMPLE at bit 12,
   engine/common/common.h). A WINDOW is a channel's view of a streamed
   file (hung off ch->data); a HEADER is the placeholder a streamed sfx
   keeps in sfx->cache: the header's facts, no samples (size 0). */
#define SOUND_R3D_WINDOW BIT( 24 )
#define SOUND_R3D_HEADER BIT( 25 )

/* The stream's shape (xbox_vox_stream.c, "Memory"). */
#define R3D_VOX_SLOTS       12      /* streams at once: 9 words were measured playing together (route-pr193, c1a0e) */
#define R3D_VOX_UNITS       3       /* a window is three units of the file, contiguous */
#define R3D_VOX_ALIGN       4096u   /* an unbuffered read's offset, size and buffer (xbox_async_read.h) */
#define R3D_VOX_TAIL_BYTES  8192u   /* the end trim's bytes and the chunks after `data`: two aligned blocks */
#define R3D_VOX_UNIT_MIN    8192u   /* r3d_vox_unit's range, bytes */
#define R3D_VOX_UNIT_MAX    32768u

/* Why S_LoadSound ran, for its `sound` note in the recorder (the caller
   tag): set just before a call by the hooks below, read and reset by
   S_LoadSound (s_load.c). */
#define R3D_VOX_TAG_OTHER    'o'
#define R3D_VOX_TAG_WORD     'w' /* VOX_LoadWord: a sentence's word starting */
#define R3D_VOX_TAG_TIMELEFT 't' /* SND_GetChannelTimeLeft: a later word's length (upstream loads it) */
#define R3D_VOX_TAG_START    's' /* S_StartSound / S_RestoreSound / S_AmbientSound */
#define R3D_VOX_TAG_MIX      'm' /* the mixer's reload of a freed cache (s_mix.c) */
#define R3D_VOX_TAG_REGISTER 'r' /* S_EndRegistration / S_RegisterSound */
extern int s_r3d_load_tag;

/* s_main.c: S_Init registers the cvars and commands; S_UpdateChannels
   runs the frame's work (reads landing, the next ones queued, slots whose
   channel was reused released) before it mixes; S_StopAllSounds and
   S_Shutdown close every stream (a read in flight is waited for: the
   kernel owns its buffer until it lands). */
void S_R3D_VoxInit( void );

/* The number of sentences sentences.txt defined (s_vox.c's
   cszrawsentences): `r3d_vox_crc all` checks each index below it. */
int VOX_R3D_SentenceCount( void );
void S_R3D_VoxFrame( void );
void S_R3D_VoxCloseAll( void );

/* s_vox.c, VOX_LoadWord: the word's samples - a window (the stream), or
   S_LoadSound's whole cache (upstream's path, for anything the stream
   does not take). */
wavdata_t *S_R3D_VoxLoadWord( channel_t *ch, const voxword_t *word );

/* s_vox.c, S_TrimStart (dir > 0) and S_TrimEnd (dir < 0) on a window:
   a pointer p to sample `sample` such that p[0..254] (dir > 0, up to the
   end of the samples) or p[-254..0] (dir < 0, down to sample 0) are the
   samples upstream's buffer holds there. */
const int8_t *S_R3D_VoxTrimData( const wavdata_t *wav, int sample, int dir );

/* s_vox.c VOX_FreeWord, s_main.c S_FreeChannel: the channel's streams
   (its window, and the next word's prefetch with `all`) are released;
   a read still in flight is left to land (the slot is reclaimed by
   S_R3D_VoxFrame), so stopping a sound never waits. */
void S_R3D_VoxRelease( channel_t *ch, qboolean all );

/* s_mix.c: the samples a channel mixes from - its window when it has
   one, otherwise upstream's sfx->cache. A window is recognised by the
   slot token in engine_reserved[0], never by reading ch->data, which
   upstream can leave pointing at a freed cache (VOX_FreeWord on another
   channel speaking the same word). */
static inline wavdata_t *S_R3D_ChannelSource( const channel_t *ch )
{
	return ch->engine_reserved[0] ? ch->data : ch->sfx->cache;
}

/* s_mix.c, S_MixNormalChannelsToRoombuffer's S_LoadSound( ch->sfx ):
   the window, with sfx->cache put back the way upstream's reload there
   would have refilled it; or S_LoadSound itself. */
wavdata_t *S_R3D_VoxMixSource( channel_t *ch );

/* s_main.c, S_RetrieveAudioSamples, for a WINDOW or HEADER source, after
   upstream's own position and count arithmetic: `byte_offset` is
   upstream's start_position * frame_size, `num_samples` its clamped count
   (> 0). Returns num_samples with *output_buffer at the bytes upstream's
   buffer holds there, or 0 for a HEADER (it has no samples: never a
   normal path). */
int S_R3D_VoxSamples( const wavdata_t *source, const void **output_buffer, int byte_offset, int num_samples );

/* s_main.c, SND_GetChannelTimeLeft's S_LoadSound of every later word:
   a wavdata_t whose `samples` is the one S_LoadSound's would have -
   sfx->cache, a header read, or S_LoadSound itself when the file is not
   one the header can describe. */
wavdata_t *S_R3D_VoxWordInfo( sfx_t *sfx );

/* s_main.c, S_StartSound / S_RestoreSound / S_AmbientSound's S_LoadSound
   for a sound that is not a sentence: precached speech streams on its
   channel (a window on ch->data), anything else is S_LoadSound. */
wavdata_t *S_R3D_VoxStartSfx( channel_t *ch, sfx_t *sfx );

/* s_main.c, S_RestoreSound, after the saved position is written: the
   window is moved there now (a load: a synchronous read is fine). */
void S_R3D_VoxSeek( channel_t *ch );

/* s_load.c: placeholders. A streamed sound's sfx->cache holds one exactly
   when upstream's would hold its whole load (attached where upstream
   loads, dropped where it frees), so S_FindName's "already in the cache"
   and FL_VOXWORD_IN_CACHE, and SND_GetChannelTimeLeft's NULL test, read
   as upstream's do. IsHeader tests one; Forget drops sfx's (its cache
   goes NULL) and the header facts kept for it, before S_FreeSound wipes
   the sfx; Detach drops sfx's placeholder where upstream frees the cache
   (VOX_FreeWord) or S_LoadSound puts a whole load in its place; Register
   takes S_EndRegistration's and S_RegisterSound's S_LoadSound. */
qboolean S_R3D_VoxIsHeader( const wavdata_t *sc );
void S_R3D_VoxForget( sfx_t *sfx );
void S_R3D_VoxDetach( sfx_t *sfx );
void S_R3D_VoxRegister( sfx_t *sfx );

/* s_load.c, S_LoadSound's file read: for a small file whose bytes a
   stream's open already read whole, upstream's loader run on those bytes
   (FS_LoadSound's buffer mode, the same Sound_LoadWAV and SoundPack), so
   the file is not read twice; NULL otherwise. */
wavdata_t *S_R3D_VoxPreloaded( sfx_t *sfx );

#endif /* XBOX_VOX_STREAM_H */
