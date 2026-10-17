/*
xbox_vox_stream.c - dialogue streamed from the loose WAVs instead of loaded whole
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
Resonance3D (docs/r3d/divergences.md #177, X12 item 4 Stage A).

WHY. Upstream loads every sentence word whole, synchronously, when it
starts (VOX_LoadWord, s_vox.c:141-168 in the reference): the first word
while the start message is parsed, every later one inside the paint
(VOX_MixChannelToBuffer, s_mix.c:282-309), and every remaining word of a
sentence whenever another sound picks a channel (SND_GetChannelTimeLeft,
s_main.c:285-330). On the console a load costs 1.51 ms + 0.068 ms per KiB
(route-pr193: 203 word loads, 714 ms, up to 16 ms each for the 194 KiB
scientist lines). Precached speech is loaded whole at the map's start and
stays resident.

WHAT. A sound in the stream class - the 8-bit mono 11/22/44 kHz PCM WAVs
of retail speech, whose samples upstream's loader (Sound_LoadWAV,
snd_wav.c:177-378) copies unchanged except for the unsigned-to-signed
step - plays from a WINDOW: three units of U bytes of the file (U =
r3d_vox_unit, 8 KiB on the hard disk), contiguous, 4096-aligned in the
file and in memory, read with #172's overlapped unbuffered reads
(compat/xbox/xbox_async_read.h). Sample s is file byte data_off + s; each
unit is XORed with 0x80 over the data chunk as it lands, which is the
loader's `(byte)*p - 128`. The window hangs off ch->data (the field
upstream's VOX already sets per channel, so two channels saying one word
each read their own), and S_RetrieveAudioSamples (s_main.c) maps
upstream's own position and count into it: the pointer it hands the mixer
and the lip sync covers the same bytes upstream's whole buffer holds
there. Entering the window's second unit slides it (memmove of two units)
and queues the next unit's read; a unit not landed is waited for; a jump
outside the window (a restore, a loop) reads synchronously. Nothing is
ever played that is not the file's byte.

sfx->cache of a streamed sound holds a HEADER placeholder (size 0, its
facts: samples, rate, flags, loop_start), so every reader that only wants
those facts or a non-NULL cache - SND_GetChannelTimeLeft, the inaudible
test, the save's looping flag, S_FindName - runs unchanged. A placeholder
is refcounted by the slots streaming from it; S_LoadSound on one does
upstream's whole load in its place (a play the stream did not take).

The first word of a sentence opens its file and reads the unit at its
start and the block at its end synchronously (upstream's S_TrimStart and
S_TrimEnd then run on those bytes), then queues the next unit. The word
after it is PREFETCHED from that moment: its file opened, its head and
tail queued into a slot of its own, adopted when VOX_LoadWord reaches it.
SND_GetChannelTimeLeft takes a later word's length from its header (a
header read, cached) instead of loading it. Precached speech in the
sentence directories (r3d_vox_dirs) is registered as a placeholder and
streams on the channel that plays it; a looped one keeps the two units at
its loop start in a buffer of their own, so the jump back is a copy.

WHAT STAYS UPSTREAM'S. Anything outside the class (16-bit, stereo, other
rates, mp3-in-wav, the three CRC-silenced names), data of U bytes or less
(the fixed cost dominates; such a word's first sighting is decoded by
upstream's loader from the bytes the stream's open read, so it is not
read twice), a file in an archive or not found as "sound/<name>.wav", no
free slot, a request larger than the window, a failed read: all take
S_LoadSound, or for a request or a read the window cannot serve, a whole
FS_LoadSound kept by the slot. The client's own sound interface (the HL
SDK sets none) turns the stream off. r3d_vox_stream 0 is upstream.

LIFETIME. Slots come from a pool of R3D_VOX_SLOTS, their memory
allocated lazily from host.soundpool and freed by S_StopAllSounds (a map
change). A channel holds its slot by a token in engine_reserved[0] (the
window) and [1] (the next word's prefetch), fields upstream reserves for
engine use; every frame a slot whose channel no longer holds its token -
the static-channel reuse and S_StopAllSounds both memset channels - is
released. Stopping a sound with a read in flight orphans the slot instead
of waiting; the next frame reclaims it. S_StopAllSounds and S_Shutdown
wait for those reads (the kernel owns the buffer until they land).

Everything runs on the main thread, like #172: the SDL callback only
copies the DMA ring.

The design, its measurements and Stage B (the ordered speech pack, a
later phase that the HEADER facts and the r3d_vox_stream 2 value are
reserved for) are in docs/r3d/fork-plan.md, X12 item 4.
*/

#include "common.h"

#if XASH_XBOX && !XASH_DEDICATED

#include <math.h>
#include "client.h"
#include "sound.h"
#include "soundlib.h"
#include "xbox_frametime.h"
#include "xbox_vox_stream.h"
#include "xbox_async_read.h" /* compat/xbox: the overlapped reads, #172 */

/* Not archived, like r3d_music_baked: an A/B that ended on 0 would keep
   upstream's path through the HDD's config.cfg. 2 is reserved for Stage
   B's speech pack and plays as 1 until it exists. */
static CVAR_DEFINE_AUTO( r3d_vox_stream, "1", 0,
	"stream dialogue from the loose WAVs instead of loading it whole: 0 upstream, 1 stream (2: the speech pack, a later phase; plays as 1) (docs/r3d/divergences.md #177)" );
static CVAR_DEFINE_AUTO( r3d_vox_unit, "8192", 0,
	"bytes per unit of a dialogue stream's window (8192-32768, a multiple of 4096): taken when no stream holds memory (#177)" );
static CVAR_DEFINE_AUTO( r3d_vox_dirs, "vox fvox barney scientist hgrunt gman holo agrunt aslave", 0,
	"sound directories whose precached files stream like sentence words (#177)" );

int s_r3d_load_tag = R3D_VOX_TAG_OTHER;
uint32_t s_r3d_vox_bytes; /* every block this file holds - slots, loop units, header entries, whole fallbacks: the recorder's vox_window_kb */

/* ---- state ---- */

enum { VOX_FREE = 0, VOX_PREFETCH, VOX_ACTIVE, VOX_ORPHAN };
enum { BUF_EMPTY = 0, BUF_PENDING, BUF_READY, BUF_FAILED };

/* read targets: the window's units, the tail block, a loop's two units */
#define T_TAIL      R3D_VOX_UNITS
#define T_LOOP      ( R3D_VOX_UNITS + 1 )
#define T_COUNT     ( R3D_VOX_UNITS + 3 )

#define VOX_HASH        64
#define VOX_FACTS_MAX   256     /* header facts kept for sounds nothing streams (about 130 bytes each; the spec's 64 thrashed on the vox vocabulary) */
#define VOX_PRECACHE_MIN 65536u /* precached speech streams only above this much data: below it the memory saved is small, and upstream plays it from memory with no read at its start */
#define VOX_TRIM_BYTES  256     /* upstream's TRIM_SCAN_MAX (255, s_vox.c:21) and one */

typedef struct vox_buf_s
{
	uint32_t off;       /* the file offset of its first byte */
	uint32_t valid;     /* READY: the bytes that arrived */
	uint8_t state;      /* BUF_* */
	uint8_t conv;       /* the data chunk's bytes in it are signed (XORed) */
} vox_buf_t;

/* A sound's header facts; the wavdata_t placeholder follows it in the
   same allocation (HDR_WAV), so sfx->cache can point at one. */
typedef struct vox_hdr_s
{
	struct vox_hdr_s *next;
	sfx_t *sfx;             /* the key; NULL once S_FreeSound forgot it */
	int refs;               /* slots streaming from it */
	qboolean attached;      /* it is sfx->cache */
	qboolean streamable;    /* in the class, data above U: streams */
	uint32_t lru;
	uint32_t file_size, data_off, data_bytes;
} vox_hdr_t;

#define VOX_HDR_WAV_OFS (( sizeof( vox_hdr_t ) + 15u ) & ~(size_t)15u )
#define HDR_WAV( h ) ((wavdata_t *)((byte *)( h ) + VOX_HDR_WAV_OFS ))
#define WAV_HDR( w ) ((vox_hdr_t *)((byte *)( w ) - VOX_HDR_WAV_OFS ))

typedef struct vox_slot_s
{
	int state;
	uintptr_t token;        /* index + 1, generation above */
	channel_t *owner;
	voxword_t *words;       /* PREFETCH: the owner's words when it was issued */
	int word;               /* PREFETCH: the word it is for */
	sfx_t *sfx;
	vox_hdr_t *hdr;         /* a reference; NULL until the header is parsed */
	uint32_t file_size;
	r3d_async_read_t io;
	qboolean io_open;
	int pending;            /* the target of the read in flight, -1 */
	byte *mem;              /* one allocation: the window's wavdata_t, the units, the tail */
	byte *loop_mem;         /* a looped stream's two units at its loop start (own allocation) */
	wavdata_t *win;
	byte *buf;              /* win->buffer: R3D_VOX_UNITS units */
	byte *tail;
	byte *loop;
	uint32_t unit;          /* U when the memory was allocated */
	uint32_t k0;            /* the window's first unit */
	uint32_t tail_lo, tail_hi; /* PREFETCH: the tail block's bytes [lo, hi) (hi 0: the file's last block; lo UINT32_MAX: none) */
	vox_buf_t b[T_COUNT];
	wavdata_t *whole;       /* the fallback: upstream's whole load, owned */
	qboolean hit;           /* a prefetch adopted with nothing to wait for */
	qboolean dead;          /* the window and the whole load both failed: no samples (a broken disk) */
	qboolean played;        /* the mixer has read from it: a jump from here is a seek */
	byte trim[VOX_TRIM_BYTES];
} vox_slot_t;

static struct
{
	vox_slot_t slot[R3D_VOX_SLOTS];
	vox_hdr_t *hash[VOX_HASH];
	uint32_t lru, gen;
	uint32_t unit;          /* U for slots allocated now */
	int force;              /* r3d_vox_crc: -1, or the mode it runs */
	sfx_t *preload_sfx;     /* S_LoadSound decodes these bytes (a small file the open read) */
	const byte *preload;
	uint32_t preload_size;
} vox = { .unit = R3D_VOX_UNIT_MIN, .force = -1 };

/* ---- counters (xbox_frametime_util.h, XBOX_FT_W_VOX_*) ---- */

static void Vox_Count( int id, uint32_t n )
{
	xbox_ft_work.c[id] += n;
}

static void Vox_Peak( int id, uint32_t v )
{
	if( xbox_ft_work.c[id] < v )
		xbox_ft_work.c[id] = v;
}

/* Why candidates were left to upstream's load, by reason, since the last
   r3d_vox_crc (which prints them). */
static const char *const vox_why_names[] = { "small", "format", "chunks", "nofile", "open", "slot", "io", "big" };
static uint32_t vox_why_count[ARRAYSIZE( vox_why_names )];

static void Vox_Why( const char *why )
{
	size_t i;

	for( i = 0; why && i < ARRAYSIZE( vox_why_names ); i++ )
	{
		if( !Q_strcmp( why, vox_why_names[i] ))
		{
			vox_why_count[i]++;
			return;
		}
	}
}

static void Vox_Note( const char *why, const sfx_t *sfx, const xbox_ft_span_t *span )
{
	Vox_Why( why );
	char name[XBOX_FT_NOTE_NAME_BYTES];

	Q_snprintf( name, sizeof( name ), "%s %s", why, sfx->name );
	Xbox_FT_Note( XBOX_FT_NOTE_VOX, name, xbox_ft_span_kc_since( span ), 0 );
}

/* ---- memory, counted (vox_window_kb) ---- */

static void *Vox_Alloc( size_t size, qboolean zero )
{
	s_r3d_vox_bytes += (uint32_t)size;
	return zero ? Mem_Calloc( host.soundpool, size ) : Mem_Malloc( host.soundpool, size );
}

static void Vox_Free( void *p, size_t size )
{
	s_r3d_vox_bytes -= (uint32_t)size;
	Mem_Free( p );
}

#define VOX_HDR_BYTES ( VOX_HDR_WAV_OFS + sizeof( wavdata_t ))
#define VOX_SLOT_BYTES( u ) ( sizeof( wavdata_t ) + R3D_VOX_ALIGN + R3D_VOX_UNITS * ( u ) + R3D_VOX_TAIL_BYTES )
#define VOX_LOOP_BYTES( u ) ( R3D_VOX_ALIGN + 2 * ( u ))

/* ---- the switch ---- */

static qboolean Vox_On( void )
{
	if( vox.force >= 0 )
		return vox.force != 0;
	/* a client sound interface (sound_api.h) sees channels and sfx->cache:
	   one that mixes, frees, spatialises or is told of channels could read
	   a window or a placeholder as samples; the HL SDK sets none */
	if( clgame.soundFuncs.pfnS_Init || clgame.soundFuncs.pfnS_PaintChannels || clgame.soundFuncs.pfnS_FreeSound
		|| clgame.soundFuncs.pfnS_UpdateChannel || clgame.soundFuncs.pfnS_Spatialize )
		return false;
	return r3d_vox_stream.value >= 1.0f;
}

/* FS_LoadSound's first candidate for `name` (snd_main.c:57-104 in the
   reference): "sound/<name>.wav", the name's own extension stripped when
   it is "wav". false for a name whose extension is another load format
   (upstream would open only that one), or a '*'/'!' name. */
static qboolean Vox_Path( const char *name, char *path, size_t size )
{
	string loadname;
	const char *ext;
	const loadwavfmt_t *format;

	if( !name[0] || name[0] == '*' || name[0] == '!' )
		return false;
	Q_strncpy( loadname, name, sizeof( loadname ));
	ext = COM_FileExtension( name );
	if( !COM_StringEmpty( ext ))
	{
		for( format = sound.loadformats; format && format->ext; format++ )
		{
			if( !Q_stricmp( format->ext, ext ))
			{
				if( Q_stricmp( ext, "wav" ))
					return false;
				COM_StripExtension( loadname );
				break;
			}
		}
	}
	return Q_snprintf( path, size, DEFAULT_SOUNDPATH "%s.wav", loadname ) > 0;
}

/* Sound_LoadWAV silences these three when their CRC matches
   (snd_wav.c:352-375 in the reference): whole loads, always. */
static qboolean Vox_CrcName( const char *path )
{
	return Q_stristr( path, "null.wav" ) || Q_stristr( path, "_period.wav" ) || Q_stristr( path, "_comma.wav" );
}

/* A precached sound in one of r3d_vox_dirs. */
static qboolean Vox_Speech( const char *name )
{
	const char *slash = Q_strchr( name, '/' ), *p = r3d_vox_dirs.string;
	size_t len;

	if( !slash || slash == name )
		return false;
	len = slash - name;
	while( p && *p )
	{
		const char *e;

		while( *p == ' ' )
			p++;
		for( e = p; *e && *e != ' '; e++ );
		if( (size_t)( e - p ) == len && !Q_strnicmp( p, name, len ))
			return true;
		p = e;
	}
	return false;
}

/* ---- header facts and placeholders ---- */

static uint32_t Vox_Hash( const sfx_t *sfx )
{
	return (uint32_t)(((uintptr_t)sfx / sizeof( sfx_t )) % VOX_HASH );
}

static vox_hdr_t *Vox_HdrFind( const sfx_t *sfx )
{
	vox_hdr_t *h;

	for( h = vox.hash[Vox_Hash( sfx )]; h; h = h->next )
	{
		if( h->sfx == sfx )
			return h;
	}
	return NULL;
}

static void Vox_HdrUnlink( vox_hdr_t *h )
{
	vox_hdr_t **p;

	if( !h->sfx )
		return;
	for( p = &vox.hash[Vox_Hash( h->sfx )]; *p; p = &( *p )->next )
	{
		if( *p == h )
		{
			*p = h->next;
			break;
		}
	}
	h->next = NULL;
	h->sfx = NULL;
}

/* Frees an entry nothing holds that is no longer keyed (S_FreeSound forgot
   its sfx). */
static void Vox_HdrTrim( vox_hdr_t *h )
{
	if( h && !h->refs && !h->attached && !h->sfx )
		Vox_Free( h, VOX_HDR_BYTES );
}

/* Keeps at most VOX_FACTS_MAX keyed entries that nothing holds (facts
   kept for a later start; the least recently used go). Run when an entry
   is added - a header read, not a frame's work - so the scan is never per
   sound. It frees only entries nothing references or attaches, before
   the new one exists: no caller holds such an entry by pointer across
   Vox_HdrNew (Vox_Stream takes its `h` again after Vox_Header). */
static void Vox_HdrEvict( void )
{
	for( ;; )
	{
		int i, n = 0;
		vox_hdr_t *e, *oldest = NULL;

		for( i = 0; i < VOX_HASH; i++ )
		{
			for( e = vox.hash[i]; e; e = e->next )
			{
				if( e->refs || e->attached )
					continue;
				n++;
				if( !oldest || e->lru < oldest->lru )
					oldest = e;
			}
		}
		if( n < VOX_FACTS_MAX || !oldest )
			return;
		Vox_HdrUnlink( oldest );
		Vox_Free( oldest, VOX_HDR_BYTES );
	}
}

static vox_hdr_t *Vox_HdrNew( sfx_t *sfx )
{
	vox_hdr_t *h = Vox_HdrFind( sfx );

	if( !h )
	{
		const uint32_t i = Vox_Hash( sfx );

		Vox_HdrEvict();
		h = Vox_Alloc( VOX_HDR_BYTES, true );
		h->sfx = sfx;
		h->next = vox.hash[i];
		vox.hash[i] = h;
	}
	h->lru = ++vox.lru;
	return h;
}

static void Vox_HdrUnref( vox_hdr_t *h )
{
	if( !h )
		return;
	h->refs--;
	Vox_HdrTrim( h );
}

/* sfx->cache becomes the placeholder: where upstream would have loaded
   the sound whole (so S_FindName, FL_VOXWORD_IN_CACHE and the NULL tests
   on sfx->cache see what they see upstream) */
static void Vox_Attach( vox_hdr_t *h )
{
	h->sfx->cache = HDR_WAV( h );
	h->attached = true;
}

qboolean S_R3D_VoxIsHeader( const wavdata_t *sc )
{
	return sc && FBitSet( sc->flags, SOUND_R3D_HEADER );
}

void S_R3D_VoxDetach( sfx_t *sfx )
{
	vox_hdr_t *h;

	if( !S_R3D_VoxIsHeader( sfx->cache ))
		return;
	h = WAV_HDR( sfx->cache );
	sfx->cache = NULL;
	h->attached = false;
	Vox_HdrTrim( h );
}

void S_R3D_VoxForget( sfx_t *sfx )
{
	vox_hdr_t *h;

	S_R3D_VoxDetach( sfx );
	if(( h = Vox_HdrFind( sfx )) != NULL )
	{
		Vox_HdrUnlink( h );
		Vox_HdrTrim( h );
	}
}

/* ---- the header: Sound_LoadWAV's parse, over the bytes read so far ---- */

/* The bytes a parse can see: the window's first unit (file offset 0) and
   the tail block, as read (not yet signed). */
typedef struct vox_src_s
{
	const byte *p[2];
	uint32_t off[2], len[2];
	int64_t end;            /* iff_end: the file's size */
	int64_t last;           /* iff_lastChunk */
	int64_t ptr;            /* iff_dataPtr; -1 for NULL */
	int32_t chunk_len;      /* iff_chunkLen */
	qboolean missing;       /* a byte it needed was not read */
} vox_src_t;

static const byte *Vox_SrcAt( vox_src_t *s, int64_t off, int64_t len )
{
	int i;

	for( i = 0; i < 2; i++ )
	{
		if( s->p[i] && off >= s->off[i] && off + len <= (int64_t)s->off[i] + s->len[i] )
			return s->p[i] + ( off - s->off[i] );
	}
	s->missing = true;
	return NULL;
}

/* GetLittleLong / GetLittleShort / IsFourCC (snd_wav.c:26-63) */
static int Vox_SrcLong( vox_src_t *s, int64_t at )
{
	const byte *b = Vox_SrcAt( s, at, 4 );
	uint val;

	if( !b )
		return 0;
	val = ((uint)b[0] << 0 ) + ((uint)b[1] << 8 ) + ((uint)b[2] << 16 ) + ((uint)b[3] << 24 );
	return (int)val;
}

static short Vox_SrcShort( vox_src_t *s, int64_t at )
{
	const byte *b = Vox_SrcAt( s, at, 2 );
	short val = 0;

	if( !b )
		return 0;
	val += ( b[0] << 0 );
	val += ( b[1] << 8 );
	return val;
}

static qboolean Vox_SrcFourCC( vox_src_t *s, int64_t at, const char *fourcc )
{
	const byte *b = Vox_SrcAt( s, at, 4 );

	return b && !memcmp( b, fourcc, 4 );
}

/* FindNextChunk (snd_wav.c:70-121), in file offsets */
static void Vox_FindNextChunk( vox_src_t *s, const char *name )
{
	for( ;; )
	{
		int64_t remaining = s->end - s->last;

		if( remaining < 8 )
		{
			s->ptr = -1;
			return;
		}
		s->ptr = s->last + 4;
		remaining -= 8;
		s->chunk_len = Vox_SrcLong( s, s->ptr );
		s->ptr += 4;
		if( s->missing )
			return;
		if( s->chunk_len < 0 )
		{
			s->ptr = -1;
			return;
		}
		if( s->chunk_len > remaining )
			s->chunk_len = (int32_t)remaining;
		remaining -= s->chunk_len;
		s->ptr -= 8;
		s->last = s->ptr + 8 + s->chunk_len;
		if(( s->chunk_len & 1 ) && remaining )
			s->last++;
		if( Vox_SrcFourCC( s, s->ptr, name ))
			return;
		if( s->missing )
			return;
	}
}

typedef enum
{
	VOX_PARSE_OK = 0,
	VOX_PARSE_FAIL,     /* Sound_LoadWAV returns false (or the stream cannot reproduce it) */
	VOX_PARSE_MISSING   /* a chunk header lies in bytes not read */
} vox_parse_t;

/* Sound_LoadWAV (snd_wav.c:177-378) up to its copy of the samples, then
   SoundPack's fields (snd_main.c:30-48): `wav` gets the facts, *data_off
   the samples' offset in the file and *data_bytes sound.size. */
static vox_parse_t Vox_Parse( vox_src_t *s, wavdata_t *wav, uint32_t *data_off, uint32_t *data_bytes )
{
	int64_t iff_data, p;
	int fmt, channels, rate, width, samples;
	uint loopstart = 0, nsamples = 0, flags = 0;

	s->last = 0;
	Vox_FindNextChunk( s, "RIFF" );
	if( s->missing ) return VOX_PARSE_MISSING;
	if( s->ptr < 0 || !Vox_SrcFourCC( s, s->ptr + 8, "WAVE" ))
		return s->missing ? VOX_PARSE_MISSING : VOX_PARSE_FAIL;

	iff_data = s->ptr + 12;
	s->last = iff_data;
	Vox_FindNextChunk( s, "fmt " );
	if( s->missing ) return VOX_PARSE_MISSING;
	if( s->ptr < 0 )
		return VOX_PARSE_FAIL;

	p = s->ptr + 8;
	fmt = Vox_SrcShort( s, p ); p += 2;
	if( fmt != 1 )
		return s->missing ? VOX_PARSE_MISSING : VOX_PARSE_FAIL; /* 85, mp3 in a WAV: upstream's own path */
	channels = Vox_SrcShort( s, p ); p += 2;
	if( channels != 1 && channels != 2 )
		return s->missing ? VOX_PARSE_MISSING : VOX_PARSE_FAIL;
	rate = Vox_SrcLong( s, p ); p += 4;
	p += 6;
	width = Vox_SrcShort( s, p ) / 8; p += 2;
	if( s->missing ) return VOX_PARSE_MISSING;
	if( width != 1 && width != 2 )
		return VOX_PARSE_FAIL;

	s->last = iff_data;
	Vox_FindNextChunk( s, "cue " );
	if( s->missing ) return VOX_PARSE_MISSING;
	if( s->ptr >= 0 && s->end - s->ptr >= 36 )
	{
		loopstart = (uint)Vox_SrcLong( s, s->ptr + 32 );
		SetBits( flags, SOUND_LOOPED );
		Vox_FindNextChunk( s, "LIST" ); /* from the cue's iff_lastChunk */
		if( s->missing ) return VOX_PARSE_MISSING;
		if( s->ptr >= 0 && s->end - s->ptr >= 32 )
		{
			if( Vox_SrcFourCC( s, s->ptr + 28, "mark" ))
				nsamples = loopstart + Vox_SrcLong( s, s->ptr + 24 ); /* samples in loop */
		}
		if( s->missing ) return VOX_PARSE_MISSING;
	}

	s->last = iff_data;
	Vox_FindNextChunk( s, "data" );
	if( s->missing ) return VOX_PARSE_MISSING;
	if( s->ptr < 0 )
		return VOX_PARSE_FAIL;
	samples = Vox_SrcLong( s, s->ptr + 4 ) / width;
	if( s->missing ) return VOX_PARSE_MISSING;

	if( nsamples )
	{
		if( samples < nsamples )
			return VOX_PARSE_FAIL;
	}
	else nsamples = samples;
	if( nsamples <= 0 )
		return VOX_PARSE_FAIL;
	nsamples /= channels;

	if( FBitSet( flags, SOUND_LOOPED ) && loopstart >= nsamples )
	{
		ClearBits( flags, SOUND_LOOPED );
		loopstart = 0;
	}

	*data_off = (uint32_t)( s->ptr + 8 );
	*data_bytes = nsamples * width * channels;
	memset( wav, 0, sizeof( *wav ));
	wav->size = 0;
	wav->loop_start = loopstart;
	wav->samples = nsamples;
	wav->type = WF_PCMDATA;
	wav->flags = flags | SOUND_R3D_HEADER;
	wav->rate = rate;
	wav->width = width;
	wav->channels = channels;
	return VOX_PARSE_OK;
}

/* The class the window reproduces byte for byte (the loader's only change
   to these files is the sign), and a size where it is worth it. */
static qboolean Vox_Class( const vox_hdr_t *h )
{
	const wavdata_t *w = HDR_WAV( h );

	if( w->width != 1 || w->channels != 1 )
		return false;
	/* S_LoadSound resamples every other rate (s_load.c:130-135 in the reference) */
	if( w->rate != SOUND_11k && w->rate != SOUND_22k && w->rate != SOUND_44k )
		return false;
	/* the loader's memcpy reads data_bytes from data_off */
	return (uint64_t)h->data_off + h->data_bytes <= h->file_size;
}

/* ---- slots ---- */

static uint32_t Vox_AlignUp( uint32_t v )
{
	return ( v + R3D_VOX_ALIGN - 1 ) & ~( R3D_VOX_ALIGN - 1 );
}

static vox_slot_t *Vox_FromToken( uintptr_t token )
{
	const uintptr_t i = ( token & 0xFF ) - 1;
	vox_slot_t *s;

	if( !token || i >= R3D_VOX_SLOTS )
		return NULL;
	s = &vox.slot[i];
	if( s->token != token || ( s->state != VOX_ACTIVE && s->state != VOX_PREFETCH ))
		return NULL;
	return s;
}

static vox_slot_t *Vox_SlotOf( const wavdata_t *win )
{
	int i;

	for( i = 0; i < R3D_VOX_SLOTS; i++ )
	{
		if( vox.slot[i].win == win && vox.slot[i].state == VOX_ACTIVE )
			return &vox.slot[i];
	}
	return NULL;
}

static uint32_t Vox_InUse( void )
{
	uint32_t i, n = 0;

	for( i = 0; i < R3D_VOX_SLOTS; i++ )
		n += vox.slot[i].state != VOX_FREE;
	return n;
}

static void Vox_FreeMem( vox_slot_t *s )
{
	if( s->mem )
		Vox_Free( s->mem, VOX_SLOT_BYTES( s->unit ));
	if( s->loop_mem )
		Vox_Free( s->loop_mem, VOX_LOOP_BYTES( s->unit ));
	s->mem = s->loop_mem = NULL;
	s->win = NULL;
	s->buf = s->tail = s->loop = NULL;
}

static qboolean Vox_AnyMem( void )
{
	int i;

	for( i = 0; i < R3D_VOX_SLOTS; i++ )
	{
		if( vox.slot[i].mem )
			return true;
	}
	return false;
}

static void Vox_SlotRelease( vox_slot_t *s );

/* A free slot with its memory, or NULL. A prefetch takes one only while
   VOX_PF_RESERVE stay free for sounds starting; a start with none free
   takes a prefetch's (one with no read in flight: it frees at once). */
#define VOX_PF_RESERVE 3
static vox_slot_t *Vox_Acquire( qboolean prefetch )
{
	vox_slot_t *s = NULL;
	int i, nfree = 0;

	for( i = 0; i < R3D_VOX_SLOTS; i++ )
	{
		if( vox.slot[i].state == VOX_FREE )
		{
			if( !s )
				s = &vox.slot[i];
			nfree++;
		}
	}
	if( prefetch && nfree <= VOX_PF_RESERVE )
		return NULL;
	for( i = 0; i < R3D_VOX_SLOTS && !s; i++ )
	{
		vox_slot_t *p = &vox.slot[i];

		if( p->state == VOX_PREFETCH && p->pending < 0 && p->owner )
		{
			if( p->owner->engine_reserved[1] == p->token )
				p->owner->engine_reserved[1] = 0;
			Vox_SlotRelease( p );
			if( p->state == VOX_FREE )
				s = p;
		}
	}
	if( !s )
		return NULL;

	if( !Vox_AnyMem( ))
	{
		/* r3d_vox_unit is taken here, when no window exists to resize */
		const uint32_t u = (uint32_t)r3d_vox_unit.value;

		vox.unit = ( u >= R3D_VOX_UNIT_MIN && u <= R3D_VOX_UNIT_MAX && !( u % R3D_VOX_ALIGN )) ? u : R3D_VOX_UNIT_MIN;
	}
	if( s->mem && s->unit != vox.unit )
		Vox_FreeMem( s );
	if( !s->mem )
	{
		byte *a;

		/* one allocation (docs/r3d/lessons.md: a glue that frees one block
		   cannot miss a piece): the window's wavdata_t first, then the units
		   from the next 4096 boundary, then the tail block. The wavdata_t's
		   own buffer[] is never read: S_RetrieveAudioSamples and the trims
		   take their pointers into the units from this file */
		s->unit = vox.unit;
		s->mem = Vox_Alloc( VOX_SLOT_BYTES( s->unit ), false );
		a = (byte *)((((uintptr_t)s->mem + sizeof( wavdata_t )) + R3D_VOX_ALIGN - 1 ) & ~(uintptr_t)( R3D_VOX_ALIGN - 1 ));
		s->win = (wavdata_t *)s->mem;
		s->buf = a;
		s->tail = a + R3D_VOX_UNITS * s->unit;
	}

	s->token = ((uintptr_t)( ++vox.gen & 0xFFFFFF ) << 8 ) | (uintptr_t)( s - vox.slot + 1 );
	s->owner = NULL;
	s->words = NULL;
	s->word = -1;
	s->sfx = NULL;
	s->hdr = NULL;
	s->file_size = 0;
	s->io_open = false;
	s->pending = -1;
	s->k0 = 0;
	s->tail_lo = UINT32_MAX;
	s->tail_hi = 0;
	memset( s->b, 0, sizeof( s->b ));
	s->whole = NULL;
	s->hit = false;
	s->dead = false;
	s->played = false;
	s->state = VOX_PREFETCH; /* not FREE: the caller sets what it is */
	Vox_Peak( XBOX_FT_W_VOX_SLOTS, Vox_InUse( ));
	return s;
}

static void Vox_CloseIo( vox_slot_t *s )
{
	xbox_ft_span_t span;

	if( !s->io_open )
		return;
	xbox_ft_span_begin( &span );
	r3d_async_close( &s->io );
	xbox_ft_time_add( &span, XBOX_FT_W_VOX_IO_KC );
	s->io_open = false;
	s->pending = -1;
}

static byte *Vox_Dst( vox_slot_t *s, int t )
{
	if( t < R3D_VOX_UNITS )
		return s->buf + t * s->unit;
	if( t == T_TAIL )
		return s->tail;
	return s->loop + ( t - T_LOOP ) * s->unit;
}

/* The data chunk's bytes in buffer t, signed: Sound_LoadWAV's
   `(byte)((int)((byte)*pData) - 128 )` (snd_wav.c:337-350) is an XOR with
   0x80. Nothing outside [data_off, data_off + data_bytes) is touched or
   ever handed out. */
static void Vox_Convert( vox_slot_t *s, int t )
{
	vox_buf_t *b = &s->b[t];
	uint32_t lo, hi, i;
	byte *p;

	if( !s->hdr || b->state != BUF_READY || b->conv )
		return;
	b->conv = true;
	lo = Q_max( b->off, s->hdr->data_off );
	hi = Q_min( b->off + b->valid, s->hdr->data_off + s->hdr->data_bytes );
	p = Vox_Dst( s, t ) - b->off;
	for( i = lo; i < hi; i++ )
		p[i] ^= 0x80;
}

/* Collects the read in flight: true when there is none left (it landed,
   or failed), false when `wait` is not set and it is still running. */
static qboolean Vox_Land( vox_slot_t *s, qboolean wait )
{
	xbox_ft_span_t span;
	int r, waited, t = s->pending;
	vox_buf_t *b;

	if( t < 0 )
		return true;
	xbox_ft_span_begin( &span );
	r = r3d_async_finish( &s->io, wait, &waited );
	xbox_ft_time_add( &span, XBOX_FT_W_VOX_IO_KC );
	if( r == R3D_ASYNC_PENDING )
		return false;
	if( waited )
	{
		Vox_Count( XBOX_FT_W_VOX_WAITS, 1 );
		xbox_ft_time_add( &span, XBOX_FT_W_VOX_WAIT_KC );
		s->hit = false;
	}
	s->pending = -1;
	b = &s->b[t];
	if( r < 0 )
	{
		b->state = BUF_FAILED;
		b->valid = 0;
		return true;
	}
	b->state = BUF_READY;
	b->valid = (uint32_t)r;
	b->conv = false;
	Vox_Convert( s, t );
	return true;
}

/* Queues a read of `bytes` at `off` into buffer t (one at a time). */
static qboolean Vox_Issue( vox_slot_t *s, int t, uint32_t off, uint32_t bytes, qboolean counted_async )
{
	xbox_ft_span_t span;
	vox_buf_t *b = &s->b[t];
	int r;

	if( !Vox_Land( s, true ))
		return false;
	b->off = off;
	b->valid = 0;
	b->conv = false;
	if( !bytes )
	{
		b->state = BUF_READY; /* past the end of the file: nothing to read */
		return true;
	}
	xbox_ft_span_begin( &span );
	r = r3d_async_start( &s->io, off, Vox_Dst( s, t ), bytes );
	xbox_ft_time_add( &span, XBOX_FT_W_VOX_IO_KC );
	if( r < 0 )
	{
		b->state = BUF_FAILED;
		return false;
	}
	b->state = BUF_PENDING;
	s->pending = t;
	Vox_Count( XBOX_FT_W_VOX_READS, 1 );
	Vox_Count( XBOX_FT_W_VOX_READ_KB, ( bytes + 1023u ) / 1024u );
	if( r > 0 && counted_async )
		Vox_Count( XBOX_FT_W_VOX_ASYNC, 1 );
	return true;
}

/* Reads buffer t now: a synchronous read (the first word's head and tail,
   a jump outside the window). */
static qboolean Vox_ReadNow( vox_slot_t *s, int t, uint32_t off, uint32_t bytes )
{
	uint32_t waits, wait_kc;

	Vox_Land( s, true ); /* a queued read the frame waits for: counted */
	waits = xbox_ft_work.c[XBOX_FT_W_VOX_WAITS];
	wait_kc = xbox_ft_work.c[XBOX_FT_W_VOX_WAIT_KC];
	if( !Vox_Issue( s, t, off, bytes, false ))
		return false;
	Vox_Count( XBOX_FT_W_VOX_SYNC, 1 );
	s->hit = false;
	Vox_Land( s, true );
	/* its own wait is the read, not a queued read the frame caught up with */
	xbox_ft_work.c[XBOX_FT_W_VOX_WAITS] = waits;
	xbox_ft_work.c[XBOX_FT_W_VOX_WAIT_KC] = wait_kc;
	return s->b[t].state == BUF_READY;
}

/* Unit k's read: U bytes, or the file's rest rounded up to a sector. */
static uint32_t Vox_UnitBytes( const vox_slot_t *s, uint32_t k )
{
	const uint32_t off = k * s->unit;

	if( off >= s->file_size )
		return 0;
	return Q_min( s->unit, Vox_AlignUp( s->file_size - off ));
}

/* The tail block for bytes [b0, b1) (at most two sectors: the end trim's
   255 samples), or the file's last 8 KiB when b1 is 0. */
static void Vox_TailFor( const vox_slot_t *s, uint32_t b0, uint32_t b1, uint32_t *off, uint32_t *bytes )
{
	uint32_t start;

	if( !b1 )
	{
		start = s->file_size ? ( s->file_size - 1 ) & ~( R3D_VOX_ALIGN - 1 ) : 0;
		if( start >= R3D_VOX_ALIGN )
			start -= R3D_VOX_ALIGN;
		b1 = s->file_size;
	}
	else start = b0 & ~( R3D_VOX_ALIGN - 1 );
	*off = start;
	*bytes = Q_min( Vox_AlignUp( b1 - start ), R3D_VOX_TAIL_BYTES );
	if( start >= s->file_size )
		*bytes = 0;
	else *bytes = Q_min( *bytes, Vox_AlignUp( s->file_size - start ));
}

/* Whether unit k holds data the stream may still need: up to the data's
   end, or for a word with an end cut, a little past its forced end (the
   lookahead; a restore past it is read when asked) - not units it will
   never play, whose reads would only hold the slot and the disk. */
static qboolean Vox_DataEnd( const vox_slot_t *s, uint32_t k )
{
	uint32_t end;

	if( !s->hdr )
		return false;
	end = s->hdr->data_off + s->hdr->data_bytes;
	if( s->owner && s->owner->words && s->owner->forced_end > 0 && s->owner->forced_end + 2 < s->hdr->data_bytes )
		end = s->hdr->data_off + (uint32_t)s->owner->forced_end + 2;
	return k * s->unit < end;
}

/* Keeps the disk busy: the window's next missing unit, then a looped
   stream's loop units; a prefetch's head unit, then its tail. */
static void Vox_Pump( vox_slot_t *s )
{
	int j;

	if( s->pending >= 0 && !Vox_Land( s, false ))
		return;
	if( s->whole || !s->io_open )
		return;

	if( s->state == VOX_PREFETCH )
	{
		if( s->b[0].state == BUF_EMPTY )
			Vox_Issue( s, 0, s->k0 * s->unit, Vox_UnitBytes( s, s->k0 ), true );
		else if( s->tail_lo != UINT32_MAX && s->b[T_TAIL].state == BUF_EMPTY )
		{
			uint32_t off, bytes;

			Vox_TailFor( s, s->tail_lo, s->tail_hi, &off, &bytes );
			Vox_Issue( s, T_TAIL, off, bytes, true );
		}
		return;
	}
	if( s->state != VOX_ACTIVE || !s->hdr )
		return;

	for( j = 0; j < R3D_VOX_UNITS; j++ )
	{
		const uint32_t k = s->k0 + j;

		if( s->b[j].state == BUF_EMPTY && Vox_DataEnd( s, k ))
		{
			Vox_Issue( s, j, k * s->unit, Vox_UnitBytes( s, k ), true );
			return;
		}
	}

	/* a looped sound on a channel that loops: the two units at its loop
	   start, read once, so the jump back is a copy (Vox_Window) */
	if( FBitSet( HDR_WAV( s->hdr )->flags, SOUND_LOOPED ) && s->owner && FBitSet( s->owner->flags, FL_CHAN_USE_LOOP ))
	{
		const uint32_t l = ( s->hdr->data_off + HDR_WAV( s->hdr )->loop_start ) / s->unit;

		if( !s->loop_mem )
		{
			s->loop_mem = Vox_Alloc( VOX_LOOP_BYTES( s->unit ), false );
			s->loop = (byte *)(((uintptr_t)s->loop_mem + R3D_VOX_ALIGN - 1 ) & ~(uintptr_t)( R3D_VOX_ALIGN - 1 ));
		}
		for( j = 0; j < 2; j++ )
		{
			if( s->b[T_LOOP + j].state == BUF_EMPTY && Vox_DataEnd( s, l + j ))
			{
				Vox_Issue( s, T_LOOP + j, ( l + j ) * s->unit, Vox_UnitBytes( s, l + j ), true );
				return;
			}
		}
	}
}

static void Vox_LandUnits( vox_slot_t *s )
{
	if( s->pending >= 0 && s->pending < R3D_VOX_UNITS )
		Vox_Land( s, true );
}

/* Moves the window d units on: the units it keeps move down (memmove), the
   rest empty, and Vox_Pump queues them. */
static void Vox_Slide( vox_slot_t *s, uint32_t d )
{
	uint32_t j;

	Vox_LandUnits( s ); /* the kernel must not write into what moves */
	memmove( s->buf, s->buf + d * s->unit, ( R3D_VOX_UNITS - d ) * s->unit );
	memmove( &s->b[0], &s->b[d], ( R3D_VOX_UNITS - d ) * sizeof( vox_buf_t ));
	for( j = R3D_VOX_UNITS - d; j < R3D_VOX_UNITS; j++ )
		memset( &s->b[j], 0, sizeof( s->b[j] ));
	s->k0 += d;
}

/* A looped stream back at its loop start: the loop units become the
   window's first two. */
static qboolean Vox_LoopJump( vox_slot_t *s, uint32_t ka, uint32_t kb )
{
	uint32_t l;

	if( !s->loop || !s->hdr )
		return false;
	l = ( s->hdr->data_off + HDR_WAV( s->hdr )->loop_start ) / s->unit;
	if( ka != l || kb > l + 1 || s->b[T_LOOP].state != BUF_READY || ( kb > l && s->b[T_LOOP + 1].state != BUF_READY ))
		return false;
	Vox_Land( s, true ); /* nothing in flight while buffers change hands */
	memcpy( s->buf, s->loop, 2 * s->unit );
	s->b[0] = s->b[T_LOOP];
	s->b[1] = s->b[T_LOOP + 1];
	if( s->b[1].state != BUF_READY )
		memset( &s->b[1], 0, sizeof( s->b[1] ));
	memset( &s->b[2], 0, sizeof( s->b[2] ));
	s->k0 = l;
	return true;
}

/* Bytes [b0, b1) of the file, signed, contiguous in the window: NULL when
   they cannot be (more than the window, a failed read). Waits for a
   queued unit, reads a missing one, and queues the next. */
static const byte *Vox_Window( vox_slot_t *s, uint32_t b0, uint32_t b1 )
{
	const uint32_t ka = b0 / s->unit, kb = ( b1 - 1 ) / s->unit;
	uint32_t k;

	if( kb - ka >= R3D_VOX_UNITS )
		return NULL;
	if( ka > s->k0 && ka < s->k0 + R3D_VOX_UNITS )
	{
		/* entering a later unit: the window moves on to start there, so the
		   units after it are queued while it plays */
		Vox_Slide( s, ka - s->k0 );
	}
	else if( ka < s->k0 || kb >= s->k0 + R3D_VOX_UNITS )
	{
		if( !Vox_LoopJump( s, ka, kb ))
		{
			/* a jump: a loop without its units, a seek in play (a restore's is
			   counted by S_R3D_VoxSeek; a start's first position is none) */
			Vox_LandUnits( s );
			memset( s->b, 0, R3D_VOX_UNITS * sizeof( vox_buf_t ));
			s->k0 = ka;
			if( s->played )
				Vox_Count( XBOX_FT_W_VOX_SEEKS, 1 );
		}
	}
	for( k = ka; k <= kb; k++ )
	{
		const int j = k - s->k0;
		vox_buf_t *b = &s->b[j];

		if( b->state == BUF_PENDING )
			Vox_Land( s, true );
		if( b->state == BUF_EMPTY )
			Vox_ReadNow( s, j, k * s->unit, Vox_UnitBytes( s, k ));
		Vox_Convert( s, j );
		if( b->state != BUF_READY || !b->conv )
			return NULL;
		if(( k < kb && b->valid < s->unit ) || ( k == kb && b1 > b->off + b->valid ))
			return NULL;
	}
	Vox_Pump( s );
	return s->buf + ( b0 - s->k0 * s->unit );
}

/* Upstream's whole load, for what the window cannot serve. */
static qboolean Vox_Whole( vox_slot_t *s, const char *why )
{
	xbox_ft_span_t span;

	if( s->whole )
		return true;
	if( s->dead || !s->sfx || !s->hdr )
		return false;
	xbox_ft_span_begin( &span );
	s->whole = FS_LoadSound( s->sfx->name, NULL, 0 );
	if( s->whole )
		s_r3d_vox_bytes += (uint32_t)( sizeof( wavdata_t ) + s->whole->size );
	Vox_Count( XBOX_FT_W_VOX_FALLBACKS, 1 );
	Vox_Note( why, s->sfx, &span );
	if( s->whole && ( s->whole->samples != HDR_WAV( s->hdr )->samples || s->whole->width != 1 || s->whole->channels != 1 ))
	{
		s_r3d_vox_bytes -= (uint32_t)( sizeof( wavdata_t ) + s->whole->size );
		FS_FreeSound( s->whole ); /* not the file the header described: nothing to serve */
		s->whole = NULL;
	}
	if( !s->whole )
		s->dead = true; /* once: a retrieve does not retry a load every paint */
	return s->whole != NULL;
}

/* ---- open, header, start ---- */

/* A slot with the file `path` open, or NULL with *why. */
static vox_slot_t *Vox_Begin( sfx_t *sfx, const char *path, const char **why, qboolean prefetch )
{
	xbox_ft_span_t span;
	const char *disk;
	vox_slot_t *s;
	long long size;
	int r;

	xbox_ft_span_begin( &span );
	/* the file FS_LoadFile would open: the same search, NULL for one in an
	   archive or not found (FS_GetFullDiskPath, filesystem/io.c) - before
	   a slot is taken, so a missing file neither allocates one nor cancels
	   another channel's prefetch */
	if( !( disk = FS_GetDiskPath( path, false )))
	{
		*why = "nofile";
		return NULL;
	}
	if( !( s = Vox_Acquire( prefetch )))
	{
		*why = "slot";
		return NULL;
	}
	s->sfx = sfx;
	r = r3d_async_open( &s->io, disk );
	Vox_Count( XBOX_FT_W_VOX_OPENS, 1 );
	if( r == 0 )
	{
		s->io_open = true;
		size = r3d_async_size( &s->io );
	}
	else size = -1;
	xbox_ft_time_add( &span, XBOX_FT_W_VOX_OPEN_KC );
	xbox_ft_time_add( &span, XBOX_FT_W_VOX_IO_KC );
	if( size <= 0 || size > 0x7FFFFFFF )
	{
		*why = "open";
		Vox_CloseIo( s );
		s->state = VOX_FREE;
		return NULL;
	}
	s->file_size = (uint32_t)size;
	return s;
}

static void Vox_SlotRelease( vox_slot_t *s );

/* The header of the file open in s: parsed from unit 0 and the tail block
   (read now if they are not), kept for its sfx. */
static qboolean Vox_Header( vox_slot_t *s, const char *path, const char **why )
{
	vox_src_t src;
	vox_hdr_t *h;
	wavdata_t wav;
	uint32_t off, bytes, data_off, data_bytes;
	vox_parse_t r;

	if( s->hdr )
		return true;
	if( s->k0 != 0 || s->b[0].state != BUF_READY )
	{
		Vox_LandUnits( s );
		memset( s->b, 0, R3D_VOX_UNITS * sizeof( vox_buf_t ));
		s->k0 = 0;
		if( !Vox_ReadNow( s, 0, 0, Vox_UnitBytes( s, 0 )))
		{
			*why = "io";
			return false;
		}
	}
	Vox_TailFor( s, 0, 0, &off, &bytes );
	if( s->file_size > s->b[0].valid && ( s->b[T_TAIL].state != BUF_READY || s->b[T_TAIL].off != off ))
	{
		if( !Vox_ReadNow( s, T_TAIL, off, bytes ))
		{
			*why = "io";
			return false;
		}
	}

	memset( &src, 0, sizeof( src ));
	src.end = s->file_size;
	src.p[0] = s->buf;
	src.off[0] = 0;
	src.len[0] = s->b[0].valid;
	if( s->b[T_TAIL].state == BUF_READY )
	{
		src.p[1] = s->tail;
		src.off[1] = s->b[T_TAIL].off;
		src.len[1] = s->b[T_TAIL].valid;
	}
	r = Vox_Parse( &src, &wav, &data_off, &data_bytes );
	if( r != VOX_PARSE_OK )
	{
		*why = r == VOX_PARSE_MISSING ? "chunks" : "format";
		return false;
	}

	h = Vox_HdrNew( s->sfx );
	h->file_size = s->file_size;
	h->data_off = data_off;
	h->data_bytes = data_bytes;
	*HDR_WAV( h ) = wav;
	h->streamable = Vox_Class( h ) && !Vox_CrcName( path ) && data_bytes > s->unit;
	s->hdr = h;
	h->refs++;
	return true;
}

/* The decision for a file whose header is known: stream it (true), or
   leave it to S_LoadSound with *why ("small" when only its size keeps it
   out). A small file whose bytes s holds whole is decoded from them by
   upstream's loader (vox.preload), not read again. */
static qboolean Vox_Streams( vox_slot_t *s, const char *path, uint32_t min_bytes, const char **why )
{
	const vox_hdr_t *h = s->hdr;

	if( h->streamable && h->data_bytes > min_bytes )
		return true;
	if( Vox_Class( h ) && !Vox_CrcName( path ))
	{
		*why = "small";
		if( s->k0 == 0 && s->b[0].state == BUF_READY && !s->b[0].conv && s->b[0].valid >= h->file_size )
		{
			vox.preload_sfx = s->sfx;
			vox.preload = s->buf;
			vox.preload_size = h->file_size;
		}
	}
	else *why = "format";
	return false;
}

/* ---- the channel side ---- */

static void Vox_SlotRelease( vox_slot_t *s )
{
	if( s->state == VOX_FREE || s->state == VOX_ORPHAN )
		return;
	s->owner = NULL;
	s->words = NULL;
	s->sfx = NULL;
	if( s->whole )
	{
		s_r3d_vox_bytes -= (uint32_t)( sizeof( wavdata_t ) + s->whole->size );
		FS_FreeSound( s->whole );
		s->whole = NULL;
	}
	if( s->hdr )
	{
		vox_hdr_t *h = s->hdr;

		s->hdr = NULL;
		Vox_HdrUnref( h );
	}
	if( s->pending >= 0 && !Vox_Land( s, false ))
	{
		/* the kernel still writes into this slot's memory: reclaimed when
		   it lands (S_R3D_VoxFrame), never waited for here */
		s->state = VOX_ORPHAN;
		return;
	}
	Vox_CloseIo( s );
	s->state = VOX_FREE;
}

void S_R3D_VoxRelease( channel_t *ch, qboolean all )
{
	vox_slot_t *s;

	if(( s = Vox_FromToken( ch->engine_reserved[0] )) != NULL && s->owner == ch )
		Vox_SlotRelease( s );
	ch->engine_reserved[0] = 0;
	if( all )
	{
		if(( s = Vox_FromToken( ch->engine_reserved[1] )) != NULL && s->owner == ch )
			Vox_SlotRelease( s );
		ch->engine_reserved[1] = 0;
	}
}

/* Whether s's channel still holds it (a memset channel holds nothing). */
static qboolean Vox_Owned( const vox_slot_t *s )
{
	const channel_t *ch = s->owner;

	if( !ch )
		return false;
	if( s->state == VOX_ACTIVE )
		return ch->engine_reserved[0] == s->token && ch->data == s->win;
	return ch->engine_reserved[1] == s->token && ch->words && ch->words == s->words
		&& s->word >= 0 && s->word < CVOXWORDMAX && ch->words[s->word].sfx == s->sfx;
}

/* s becomes ch's window, positioned at sample `pos`. */
static wavdata_t *Vox_Activate( vox_slot_t *s, channel_t *ch, int pos )
{
	const vox_hdr_t *h = s->hdr;
	const uint32_t b0 = h->data_off + (uint32_t)bound( 0, pos, (int)HDR_WAV( h )->samples );
	int j;

	s->state = VOX_ACTIVE;
	s->owner = ch;
	s->words = NULL;
	s->word = -1;
	*s->win = *HDR_WAV( h );
	ClearBits( s->win->flags, SOUND_R3D_HEADER );
	SetBits( s->win->flags, SOUND_R3D_WINDOW );
	s->win->size = R3D_VOX_UNITS * s->unit;
	for( j = 0; j < T_COUNT; j++ )
		Vox_Convert( s, j );
	ch->engine_reserved[0] = s->token;
	ch->data = s->win;
	/* the unit at the start, now: the trim and the first paint read it */
	if( b0 < h->data_off + h->data_bytes && !Vox_Window( s, b0, b0 + 1 ) && !Vox_Whole( s, "io" ))
	{
		ch->engine_reserved[0] = 0;
		ch->data = NULL;
		Vox_SlotRelease( s );
		return NULL;
	}
	Vox_Count( XBOX_FT_W_VOX_STARTS, 1 );
	return s->win;
}

/* The prefetch ch holds for the word it is starting, landed, or NULL (a
   stale one is released). */
static vox_slot_t *Vox_TakePrefetch( channel_t *ch, sfx_t *sfx )
{
	vox_slot_t *s = Vox_FromToken( ch->engine_reserved[1] );

	ch->engine_reserved[1] = 0;
	if( !s )
		return NULL;
	if( s->state != VOX_PREFETCH || s->owner != ch || s->sfx != sfx || s->word != ch->word_index || s->words != ch->words )
	{
		Vox_SlotRelease( s );
		return NULL;
	}
	s->hit = true;
	while( s->pending >= 0 )
		Vox_Land( s, true );
	if( s->b[0].state != BUF_READY || ( s->tail_lo != UINT32_MAX && s->b[T_TAIL].state != BUF_READY ))
		s->hit = false; /* Vox_Header or the trim reads what is missing */
	return s;
}

/* Where word w's trims will read (hints for what to read first; the trims
   are upstream's own, s_vox.c:127-167 in the reference): the file byte at
   its start, and the bytes [e0, e1) S_TrimEnd scans back from its end. */
static void Vox_WordBytes( const vox_hdr_t *h, const voxword_t *w, uint32_t *b0, uint32_t *e0, uint32_t *e1 )
{
	const uint32_t n = HDR_WAV( h )->samples;
	/* Integer arithmetic, never the C library's round(): r3d_vox_crc in
	   xemu found 7 words (vox/to at e70, armory and remaining at e95 -
	   each a product exactly on .5) whose trimmed end upstream's own
	   `round( e * 0.01f * samples )` in VOX_LoadWord put one sample
	   lower when this hint had called round() just before it, with the
	   same data, the same sample count and, measured, the same x87
	   control word and MXCSR around every kernel call. nxdk's round()
	   (deps/nxdk/lib/pdclib/platform/xbox/functions/math/round.c) is
	   frndint under round-to-nearest-even, with an inline asm that
	   writes its 16-bit control-word slot 32 bits wide; how it changed
	   the later call was not isolated. Without it the console check is
	   SAME over every sentence. A hint needs no float. */
	const int start = bound( 0, (int)(((uint64_t)w->start * n + 50u ) / 100u ), (int)n );
	/* VOX_LoadWord: end <= start reads as 0, which S_TrimStartEndTimes
	   turns into samples - channels */
	int end = w->end <= w->start ? (int)n - 1 : (int)(((uint64_t)w->end * n + 50u ) / 100u );

	end = bound( 1, end, (int)n );
	*b0 = h->data_off + (uint32_t)start;
	*e1 = h->data_off + (uint32_t)end;
	*e0 = *e1 > h->data_off + 255 ? *e1 - 255 : h->data_off;
}

/* Opens the next word's file and queues its head and tail. */
static void Vox_Prefetch( channel_t *ch )
{
	const int next = ch->word_index + 1;
	char path[MAX_QPATH + 16];
	const char *why;
	vox_slot_t *s;
	vox_hdr_t *h;
	sfx_t *sfx;

	if(( s = Vox_FromToken( ch->engine_reserved[1] )) != NULL && s->owner == ch )
		Vox_SlotRelease( s );
	ch->engine_reserved[1] = 0;
	if( !Vox_On() || !ch->words || next < 0 || next >= CVOXWORDMAX )
		return;
	sfx = ch->words[next].sfx;
	if( !sfx || ( sfx->cache && !S_R3D_VoxIsHeader( sfx->cache )) || !Vox_Path( sfx->name, path, sizeof( path )))
		return;
	h = Vox_HdrFind( sfx );
	if( h && !h->streamable )
		return; /* known small or outside the class: upstream at its start */
	if( !( s = Vox_Begin( sfx, path, &why, true )))
		return;
	s->owner = ch;
	s->words = ch->words;
	s->word = next;
	if( h && h->file_size == s->file_size )
	{
		uint32_t b0;

		s->hdr = h;
		h->refs++;
		Vox_WordBytes( h, &ch->words[next], &b0, &s->tail_lo, &s->tail_hi );
		s->k0 = b0 / s->unit;
	}
	else if( s->file_size > s->unit )
	{
		s->tail_lo = 0; /* the file's last block, for the chunks after `data` */
		s->tail_hi = 0;
	}
	ch->engine_reserved[1] = s->token;
	Vox_Pump( s );
}

/* A candidate: streaming is on and sfx holds no whole load. */
static qboolean Vox_Candidate( const sfx_t *sfx, char *path, size_t size )
{
	if( !Vox_On( ))
		return false;
	if( sfx->cache && !S_R3D_VoxIsHeader( sfx->cache ))
		return false; /* S_LoadSound returns it */
	return Vox_Path( sfx->name, path, size );
}

/* The stream for sfx on ch from sample `pos` (a word's estimated trimmed
   start), or NULL with *why. `pf` is a landed prefetch to use. */
static wavdata_t *Vox_Stream( channel_t *ch, sfx_t *sfx, const char *path, vox_slot_t *pf, const voxword_t *word, int pos, uint32_t min_bytes, const char **why )
{
	vox_hdr_t *h = Vox_HdrFind( sfx );
	vox_slot_t *s = pf;
	wavdata_t *win;

	if( !s && h && ( !h->streamable || h->data_bytes <= min_bytes ))
	{
		*why = ( Vox_Class( h ) && !Vox_CrcName( path )) ? "small" : "format";
		return NULL;
	}
	if( !s && !( s = Vox_Begin( sfx, path, why, false )))
		return NULL;
	if( s->hdr && s->hdr->file_size != s->file_size )
	{
		Vox_HdrUnref( s->hdr ); /* the file changed under its facts */
		s->hdr = NULL;
	}
	if( !s->hdr && h && h->file_size == s->file_size )
	{
		s->hdr = h;
		h->refs++;
		h->lru = ++vox.lru;
	}
	if( !Vox_Header( s, path, why ) || !Vox_Streams( s, path, min_bytes, why ))
	{
		/* the slot goes, and its bytes with it - unless S_LoadSound is
		   about to decode them (vox.preload): released after */
		if( vox.preload_sfx != sfx )
			Vox_SlotRelease( s );
		else s->state = VOX_ACTIVE, s->owner = NULL; /* held, not owned: the sweep releases it */
		return NULL;
	}
	h = s->hdr;
	if( word )
	{
		uint32_t b0, e0, e1;

		Vox_WordBytes( h, word, &b0, &e0, &e1 );
		pos = (int)( b0 - h->data_off );
		/* the end trim's bytes into the tail block now, unless the tail or
		   the window's first unit holds them: VOX_LoadWord runs S_TrimEnd
		   next */
		if( !( s->b[T_TAIL].state == BUF_READY && s->b[T_TAIL].off <= e0 && e1 <= s->b[T_TAIL].off + s->b[T_TAIL].valid )
			&& !( s->b[0].state == BUF_READY && e0 >= s->k0 * s->unit && e1 <= s->k0 * s->unit + s->b[0].valid ))
		{
			uint32_t off, bytes;

			Vox_TailFor( s, e0, e1, &off, &bytes );
			Vox_ReadNow( s, T_TAIL, off, bytes );
		}
	}
	/* the placeholder: sfx->cache non-NULL, as after upstream's load */
	if( !sfx->cache )
		Vox_Attach( h );
	if( !( win = Vox_Activate( s, ch, pos )))
	{
		*why = "io";
		return NULL;
	}
	return win;
}

/* Releases a slot Vox_Stream held for vox.preload, and the preload. */
static void Vox_PreloadDone( void )
{
	int i;

	vox.preload_sfx = NULL;
	vox.preload = NULL;
	vox.preload_size = 0;
	for( i = 0; i < R3D_VOX_SLOTS; i++ )
	{
		if( vox.slot[i].state == VOX_ACTIVE && !vox.slot[i].owner )
			Vox_SlotRelease( &vox.slot[i] );
	}
}

static wavdata_t *Vox_LoadSound( sfx_t *sfx, int tag )
{
	wavdata_t *sc;

	s_r3d_load_tag = tag;
	sc = S_LoadSound( sfx );
	s_r3d_load_tag = R3D_VOX_TAG_OTHER; /* S_LoadSound resets it only when it loads */
	if( vox.preload_sfx )
		Vox_PreloadDone(); /* not for every channel the mixer passes here */
	return sc;
}

/* S_LoadSound's file read, for the small file a stream's open read whole
   (s_load.c, the Xbox arm before FS_LoadSound): upstream's loader on
   those bytes - FS_LoadSound's buffer mode, which runs the same
   Sound_LoadWAV and SoundPack (snd_main.c:81-83, :119-130). The name only
   reaches Sound_LoadWAV's messages and its CRC-silenced names, which never
   stream. */
wavdata_t *S_R3D_VoxPreloaded( sfx_t *sfx )
{
	char name[MAX_QPATH + 2];

	if( !vox.preload || vox.preload_sfx != sfx )
		return NULL;
	Q_snprintf( name, sizeof( name ), "#%s", sfx->name );
	return FS_LoadSound( name, vox.preload, vox.preload_size );
}

wavdata_t *S_R3D_VoxLoadWord( channel_t *ch, const voxword_t *word )
{
	char path[MAX_QPATH + 16];
	sfx_t *sfx = word->sfx;
	wavdata_t *wav = NULL;
	const char *why = NULL;
	xbox_ft_span_t span;

	/* the channel's previous window (a static channel reused) */
	S_R3D_VoxRelease( ch, false );

	xbox_ft_span_begin( &span );
	if( Vox_Candidate( sfx, path, sizeof( path )))
	{
		vox_slot_t *pf = Vox_TakePrefetch( ch, sfx );

		if(( wav = Vox_Stream( ch, sfx, path, pf, word, 0, 0, &why )) != NULL )
		{
			xbox_ft_time_add( &span, XBOX_FT_W_VOX_FIRST_KC );
			if( pf && pf->hit )
				Vox_Count( XBOX_FT_W_VOX_PF_HITS, 1 );
		}
		else if( why && !Q_strcmp( why, "small" ))
		{
			Vox_Count( XBOX_FT_W_VOX_SMALL, 1 );
			Vox_Why( why );
		}
		else if( why )
		{
			Vox_Count( XBOX_FT_W_VOX_FALLBACKS, 1 );
			Vox_Note( why, sfx, &span );
		}
	}
	if( !wav )
		wav = Vox_LoadSound( sfx, R3D_VOX_TAG_WORD );
	if( wav )
		Vox_Prefetch( ch ); /* the next word, while this one plays */
	return wav;
}

const int8_t *S_R3D_VoxTrimData( const wavdata_t *wav, int sample, int dir )
{
	vox_slot_t *s = Vox_SlotOf( wav );
	const int samples = (int)wav->samples;
	int lo, hi;

	if( !s )
		return NULL;
	if( dir > 0 )
	{
		lo = sample;
		hi = Q_min( sample + 255, samples );
	}
	else
	{
		lo = Q_max( sample - 254, 0 );
		hi = sample + 1;
	}
	memset( s->trim, 0, sizeof( s->trim ));
	if( lo < 0 || lo >= hi || hi > samples )
		return (const int8_t *)s->trim; /* upstream's loop reads nothing here */

	if( s->whole )
		memcpy( s->trim, s->whole->buffer + lo, hi - lo );
	else
	{
		const uint32_t b0 = s->hdr->data_off + (uint32_t)lo, b1 = s->hdr->data_off + (uint32_t)hi;
		const vox_buf_t *t = &s->b[T_TAIL];
		const byte *p = NULL;

		if( t->state == BUF_READY && t->conv && b0 >= t->off && b1 <= t->off + t->valid )
			p = s->tail + ( b0 - t->off );
		else if( dir < 0 && !( b0 >= s->k0 * s->unit && b1 <= s->k0 * s->unit + s->b[0].valid && s->b[0].conv ))
		{
			/* the end of the word: into the tail block, the window stays at
			   the start */
			uint32_t off, bytes;

			Vox_TailFor( s, b0, b1, &off, &bytes );
			if( Vox_ReadNow( s, T_TAIL, off, bytes ))
			{
				Vox_Convert( s, T_TAIL );
				if( b1 <= t->off + t->valid )
					p = s->tail + ( b0 - t->off );
			}
		}
		if( !p )
			p = Vox_Window( s, b0, b1 );
		if( p )
			memcpy( s->trim, p, hi - lo );
		else if( Vox_Whole( s, "io" ))
			memcpy( s->trim, s->whole->buffer + lo, hi - lo );
	}
	return (const int8_t *)s->trim + ( sample - lo );
}

int S_R3D_VoxSamples( const wavdata_t *source, const void **output_buffer, int byte_offset, int num_samples )
{
	vox_slot_t *s;
	const byte *p;

	if( !FBitSet( source->flags, SOUND_R3D_WINDOW ) || !( s = Vox_SlotOf( source )))
	{
		/* a placeholder has no samples: never a normal path */
		Vox_Count( XBOX_FT_W_VOX_FALLBACKS, 1 );
		return 0;
	}
	if( s->dead )
		return 0;
	if( !s->whole )
	{
		const uint32_t b0 = s->hdr->data_off + (uint32_t)byte_offset;

		s->played = true;
		if(( p = Vox_Window( s, b0, b0 + (uint32_t)num_samples )) != NULL )
		{
			*output_buffer = p;
			return num_samples;
		}
		if( !Vox_Whole( s, ( num_samples > (int)( 2 * s->unit )) ? "big" : "io" ))
			return 0;
	}
	*output_buffer = s->whole->buffer + byte_offset;
	return num_samples;
}

wavdata_t *S_R3D_VoxMixSource( channel_t *ch )
{
	vox_slot_t *s;

	if( ch->engine_reserved[0] && !Vox_FromToken( ch->engine_reserved[0] ))
	{
		/* a token no slot answers to (never expected): forget it, play upstream's way */
		ch->engine_reserved[0] = 0;
		ch->data = NULL;
	}
	if( ch->engine_reserved[0] )
	{
		/* upstream's S_LoadSound here reloads a cache another channel's
		   VOX_FreeWord dropped (s_mix.c:354 in the reference): the
		   placeholder goes back the same way */
		if(( s = Vox_FromToken( ch->engine_reserved[0] )) != NULL && !ch->sfx->cache && s->hdr && s->hdr->sfx == ch->sfx )
			Vox_Attach( s->hdr );
		return ch->data;
	}
	return Vox_LoadSound( ch->sfx, R3D_VOX_TAG_MIX );
}

/* The header facts for sfx: known, or read now through a slot (released
   at once). NULL when the file is not one they can describe. A small
   file's bytes are left for S_LoadSound (vox.preload). */
static vox_hdr_t *Vox_Facts( sfx_t *sfx, const char *path, uint32_t min_bytes )
{
	vox_hdr_t *h = Vox_HdrFind( sfx );
	const char *why = NULL;
	vox_slot_t *s;

	if( h )
		return h;
	if( !( s = Vox_Begin( sfx, path, &why, false )))
		return NULL;
	if( !Vox_Header( s, path, &why ))
	{
		Vox_SlotRelease( s );
		return NULL;
	}
	h = s->hdr;
	if( !Vox_Streams( s, path, min_bytes, &why ) && vox.preload_sfx == sfx )
	{
		s->state = VOX_ACTIVE; /* held for S_LoadSound's decode, released after */
		s->owner = NULL;
	}
	else Vox_SlotRelease( s );
	return h;
}

wavdata_t *S_R3D_VoxWordInfo( sfx_t *sfx )
{
	char path[MAX_QPATH + 16];
	vox_hdr_t *h;

	if( sfx->cache )
		return sfx->cache; /* what S_LoadSound returns */
	if( Vox_Candidate( sfx, path, sizeof( path )) && ( h = Vox_Facts( sfx, path, 0 )) != NULL && h->streamable )
	{
		/* upstream's S_LoadSound here leaves the word loaded: its placeholder
		   is attached instead (only `samples` is read) */
		Vox_Attach( h );
		return HDR_WAV( h );
	}
	return Vox_LoadSound( sfx, R3D_VOX_TAG_TIMELEFT );
}

wavdata_t *S_R3D_VoxStartSfx( channel_t *ch, sfx_t *sfx )
{
	char path[MAX_QPATH + 16];
	const char *why = NULL;
	xbox_ft_span_t span;
	vox_slot_t *s = Vox_FromToken( ch->engine_reserved[0] );
	wavdata_t *wav;

	/* S_AmbientSound restarting the same sound on its own channel keeps
	   its window (the channel keeps its position, s_main.c:882-958) */
	if( s && s->owner == ch && s->sfx == sfx && s->state == VOX_ACTIVE && ch->data == s->win )
		return s->win;
	if( s && s->owner == ch && ch->data == s->win )
		ch->data = NULL; /* never read through: upstream may leave a freed cache there */
	S_R3D_VoxRelease( ch, true );

	if( sfx->cache && !S_R3D_VoxIsHeader( sfx->cache ))
		return Vox_LoadSound( sfx, R3D_VOX_TAG_START ); /* held whole: no string work */
	xbox_ft_span_begin( &span );
	if( Vox_Speech( sfx->name ) && Vox_Candidate( sfx, path, sizeof( path )))
	{
		if(( wav = Vox_Stream( ch, sfx, path, NULL, NULL, (int)ch->sample, VOX_PRECACHE_MIN, &why )) != NULL )
		{
			xbox_ft_time_add( &span, XBOX_FT_W_VOX_FIRST_KC );
			return wav;
		}
		if( why && !Q_strcmp( why, "small" ))
			Vox_Count( XBOX_FT_W_VOX_SMALL, 1 );
		else if( why )
		{
			Vox_Count( XBOX_FT_W_VOX_FALLBACKS, 1 );
			Vox_Note( why, sfx, &span );
		}
	}
	return Vox_LoadSound( sfx, R3D_VOX_TAG_START );
}

void S_R3D_VoxSeek( channel_t *ch )
{
	vox_slot_t *s = Vox_FromToken( ch->engine_reserved[0] );
	uint32_t b0, end;

	if( !s || s->owner != ch || !s->hdr || s->whole )
		return;
	end = s->hdr->data_off + s->hdr->data_bytes;
	b0 = s->hdr->data_off + (uint32_t)bound( 0, (int)ch->sample, (int)HDR_WAV( s->hdr )->samples );
	if( b0 < end && ( b0 / s->unit < s->k0 || b0 / s->unit >= s->k0 + R3D_VOX_UNITS ))
		Vox_Count( XBOX_FT_W_VOX_SEEKS, 1 );
	if( b0 < end && !Vox_Window( s, b0, b0 + 1 ))
		Vox_Whole( s, "io" );
}

void S_R3D_VoxRegister( sfx_t *sfx )
{
	char path[MAX_QPATH + 16];
	vox_hdr_t *h;

	if( sfx->cache )
		return; /* S_LoadSound returns at once */
	if( Vox_Speech( sfx->name ) && Vox_Candidate( sfx, path, sizeof( path )) && ( h = Vox_Facts( sfx, path, VOX_PRECACHE_MIN )) != NULL && h->streamable && h->data_bytes > VOX_PRECACHE_MIN )
	{
		Vox_Attach( h );
		return;
	}
	Vox_LoadSound( sfx, R3D_VOX_TAG_REGISTER );
}

/* ---- the frame, and closing ---- */

void S_R3D_VoxFrame( void )
{
	int i;

	for( i = 0; i < R3D_VOX_SLOTS; i++ )
	{
		vox_slot_t *s = &vox.slot[i];

		switch( s->state )
		{
		case VOX_ORPHAN:
			if( Vox_Land( s, false ))
			{
				Vox_CloseIo( s );
				s->state = VOX_FREE;
			}
			break;
		case VOX_ACTIVE:
		case VOX_PREFETCH:
			/* released when its channel no longer holds it: a static
			   channel reused (SND_PickStaticChannel, then S_StartSound's
			   memset) and S_StopAllSounds's memset drop the token */
			if( !Vox_Owned( s ))
			{
				if( s->owner && s->owner->engine_reserved[s->state == VOX_ACTIVE ? 0 : 1] == s->token )
					s->owner->engine_reserved[s->state == VOX_ACTIVE ? 0 : 1] = 0;
				Vox_SlotRelease( s );
			}
			else Vox_Pump( s );
			break;
		}
	}
	Vox_Peak( XBOX_FT_W_VOX_SLOTS, Vox_InUse( ));
}

void S_R3D_VoxCloseAll( void )
{
	int i;

	for( i = 0; i < R3D_VOX_SLOTS; i++ )
	{
		vox_slot_t *s = &vox.slot[i];

		if( s->state == VOX_ACTIVE || s->state == VOX_PREFETCH )
		{
			if( s->owner )
			{
				s->owner->engine_reserved[0] = 0;
				s->owner->engine_reserved[1] = 0;
			}
			Vox_SlotRelease( s );
		}
		if( s->state == VOX_ORPHAN )
		{
			Vox_Land( s, true ); /* the buffer is the kernel's until it lands */
			Vox_CloseIo( s );
			s->state = VOX_FREE;
		}
		Vox_FreeMem( s );
	}
}

/* ---- r3d_vox_ab, r3d_vox_crc ---- */

static void Vox_AB_f( void )
{
	Cvar_DirectSet( &r3d_vox_stream, r3d_vox_stream.value ? "0" : "1" );
	Con_Printf( "r3d_vox_ab: dialogue now %s\n", r3d_vox_stream.value ? "STREAMED (#177)" : "LOADED WHOLE (upstream)" );
}

static file_t *crc_file;

static void Vox_Print( const char *fmt, ... )
{
	char line[512];
	va_list args;

	va_start( args, fmt );
	Q_vsnprintf( line, sizeof( line ), fmt, args );
	va_end( args );
	Con_Printf( "%s", line );
	if( crc_file )
		FS_Write( crc_file, line, Q_strlen( line ));
}

typedef struct vox_crc_word_s
{
	double sample, forced_end;
	uint flags;
	uint32_t crc, crc_from, from;
	sfx_t *sfx;
	uint32_t samples, data_crc, rounded;
	int end_pct, at_end[2]; /* the samples at forced_end - 1 and forced_end */
	qboolean whole;
} vox_crc_word_t;

/* What r3d_vox_crc counted. `sentences` were compared in both modes;
   `skipped` had no words to compare in upstream's own pass (a name that is
   no sentence, a directory or a length VOX_LoadSound refuses), so nothing
   was compared; `load_fail` (also in `differ`) loaded in one mode and not
   the other; `unloadable` stopped at the same word in both (upstream's own
   failure: the sentence ends there either way, the rest is not compared). */
typedef struct vox_crc_stats_s
{
	int sentences, words, streamed, checked, differ;
	int load_fail, skipped, unloadable;
	uint64_t ticks[2];
} vox_crc_stats_t;

/* One mode's pass over a sentence: whether VOX_LoadSound built words, how
   many words played, and the index of a word that exists but did not load
   (the sentence ended there), or -1. */
typedef struct vox_crc_pass_s
{
	qboolean built;
	int count, stop;
} vox_crc_pass_t;

static const char *const vox_crc_mode_names[2] = { "whole (upstream)", "streamed" };

/* The verdict of a whole command: SAME only when something was compared,
   nothing differed or failed to load, and no sentence was skipped. */
static const char *Vox_CrcVerdict( const vox_crc_stats_t *st )
{
	if( st->differ || st->load_fail )
		return "DIFFERS";
	if( st->skipped || !st->sentences )
		return "INCOMPLETE";
	return "SAME";
}

static void Vox_CrcSummary( const vox_crc_stats_t *st, char *out, size_t size )
{
	Q_snprintf( out, size, "r3d_vox_crc: %d sentences compared, %d skipped (no words in upstream's pass), %d words (%d streamed), "
		"%d differ (%d failed to load in one mode), %d stopped at a word neither mode loads - %s; loads ms whole %.1f, streamed %.1f (at 733 MHz)\n",
		st->sentences, st->skipped, st->words, st->streamed, st->differ, st->load_fail, st->unloadable, Vox_CrcVerdict( st ),
		(double)st->ticks[0] / 733333.0, (double)st->ticks[1] / 733333.0 );
}

/* How a sentence's two passes compare before their words do: whether
   both built words and loaded the same words. Returns false (and counts
   it) when they did not; true when the words can be compared. */
static qboolean Vox_CrcLoads( const char *name, const vox_crc_pass_t pass[2], vox_crc_stats_t *st )
{
	int mode;

	if( !pass[0].built )
	{
		Vox_Print( "r3d_vox_crc %s: no words in upstream's pass (not a sentence, or its directory or length refused) - SKIPPED\n", name );
		st->skipped++;
		return false;
	}
	st->sentences++;
	for( mode = 0; mode < 2; mode++ )
	{
		const vox_crc_pass_t *a = &pass[mode], *b = &pass[!mode];

		if( !a->built )
		{
			Vox_Print( "r3d_vox_crc %s: the %s pass built no words - LOAD FAILED, DIFFERS\n", name, vox_crc_mode_names[mode] );
			st->load_fail++;
			st->differ++;
			return false;
		}
		/* a word the other mode loaded (it went on past it) */
		if( a->stop >= 0 && ( b->stop < 0 || a->stop < b->stop ))
		{
			Vox_Print( "r3d_vox_crc %s: word %d did not load in the %s pass (it did in the other) - LOAD FAILED, DIFFERS\n",
				name, a->stop, vox_crc_mode_names[mode] );
			st->load_fail++;
			st->differ++;
			return false;
		}
	}
	if( pass[0].stop >= 0 )
	{
		Vox_Print( "r3d_vox_crc %s: word %d loads in neither mode (the sentence ends there in both; the words after it are not compared)\n",
			name, pass[0].stop );
		st->unloadable++;
	}
	if( pass[0].count != pass[1].count )
	{
		Vox_Print( "r3d_vox_crc %s: %d words whole, %d streamed - DIFFERS\n", name, pass[0].count, pass[1].count );
		st->differ++;
		return false;
	}
	return true;
}

/* The samples the mixer would read from the word's current position to
   its end, in requests of one paint's size (1024 outputs at 48 kHz at the
   word's pitch and timecompress, plus the lookahead), CRC'd. */
static uint32_t Vox_CrcWalk( channel_t *ch, uint32_t from )
{
	const voxword_t *w = &ch->words[ch->word_index];
	const wavdata_t *src = S_R3D_ChannelSource( ch );
	const double pitch = ( w->pitch ? w->pitch : 100 ) * 0.01;
	const double tc = w->timecompress >= 100 ? 1.0 : 1.0 / ( 1.0 - w->timecompress / 100.0 );
	const int step = Q_max( 1, (int)ceil( 1024.0 * pitch * src->rate / 48000.0 * tc ));
	uint32_t crc;
	int pos = (int)from, end = ch->forced_end ? (int)ch->forced_end : (int)src->samples;

	CRC32_Init( &crc );
	while( pos < end )
	{
		const void *p = NULL;
		const int n = S_RetrieveAudioSamples( src, &p, pos, step + 1, false );
		const int take = Q_min( Q_min( n, step ), end - pos );

		if( n <= 0 || !p )
			break;
		CRC32_ProcessBuffer( &crc, p, take );
		pos += take;
	}
	return CRC32_Final( crc );
}

/* One sentence through both paths: upstream's whole loads (mode 0) and
   the stream (mode 1), word by word as VOX_MixChannelToBuffer advances
   (s_mix.c:282-309 in the reference), comparing each word's trimmed start
   and end, its flags and the CRC of its samples from the start and from a
   random restore position. */
static qboolean Vox_CrcSentence( const char *name, vox_crc_stats_t *st )
{
	vox_crc_word_t res[2][CVOXWORDMAX];
	vox_crc_pass_t pass[2];
	int count[2] = { 0, 0 }, mode, j;
	qboolean same = true;

	memset( pass, 0, sizeof( pass ));
	for( mode = 0; mode < 2; mode++ )
	{
		sfx_t *kept[CVOXWORDMAX];
		channel_t ch;
		uint64_t t0;

		pass[mode].stop = -1;
		memset( &ch, 0, sizeof( ch ));
		vox.force = mode;
		t0 = xbox_ft_rdtsc();
		VOX_LoadSound( &ch, name );
		st->ticks[mode] += xbox_ft_rdtsc() - t0;
		if( !ch.words )
		{
			/* nothing to compare in this mode: Vox_CrcLoads says why; with
			   no words in upstream's pass the stream's is not run */
			if( mode == 0 )
				break;
			continue;
		}
		pass[mode].built = true;
		/* a word with a placeholder when the sentence started (a registered
		   one, S_EndRegistration) is FL_VOXWORD_IN_CACHE: the whole-load pass
		   would leave it loaded whole for good, so its placeholder goes back
		   afterwards (Vox_CrcRestore) */
		memset( kept, 0, sizeof( kept ));
		for( j = 0; mode == 0 && j < CVOXWORDMAX && ch.words[j].sfx; j++ )
		{
			if( FBitSet( ch.words[j].flags, FL_VOXWORD_IN_CACHE ))
				kept[j] = ch.words[j].sfx;
		}
		for( j = 0; j < CVOXWORDMAX && !FBitSet( ch.flags, FL_CHAN_SENTENCE_FINISHED ); j++ )
		{
			vox_crc_word_t *r = &res[mode][j];

			r->sfx = ch.sfx;
			{
				/* what the word was trimmed from: its length, its end, the bytes
				   there and, for a whole load, all of its data */
				const wavdata_t *src = S_R3D_ChannelSource( &ch );
				const voxword_t *w = &ch.words[ch.word_index];
				int e;

				r->samples = src->samples;
				r->end_pct = w->end;
				r->rounded = (uint32_t)(int)round( w->end * 0.01f * src->samples );
				r->whole = !FBitSet( src->flags, SOUND_R3D_WINDOW );
				r->data_crc = 0;
				if( r->whole )
				{
					CRC32_Init( &r->data_crc );
					CRC32_ProcessBuffer( &r->data_crc, src->buffer, (int)src->samples );
					r->data_crc = CRC32_Final( r->data_crc );
				}
				for( e = 0; e < 2; e++ )
				{
					const int at = (int)ch.forced_end - 1 + e;
					const void *p = NULL;

					r->at_end[e] = 999;
					if( at >= 0 && S_RetrieveAudioSamples( src, &p, at, 1, false ) == 1 && p )
						r->at_end[e] = *(const int8_t *)p;
				}
			}
			r->sample = ch.sample;
			r->forced_end = ch.forced_end;
			r->flags = ch.flags;
			r->crc = Vox_CrcWalk( &ch, (uint32_t)ch.sample );
			if( mode == 0 )
			{
				const int lo = (int)ch.sample, hi = ch.forced_end ? (int)ch.forced_end : (int)ch.sfx->cache->samples;

				r->from = hi > lo ? (uint32_t)( lo + COM_RandomLong( 0, hi - lo - 1 )) : (uint32_t)lo;
			}
			else
			{
				r->from = j < count[0] ? res[0][j].from : (uint32_t)ch.sample;
				st->streamed += ch.engine_reserved[0] != 0;
				/* a restore: S_RestoreSound writes the position, then seeks */
				ch.sample = r->from;
				S_R3D_VoxSeek( &ch );
			}
			r->crc_from = Vox_CrcWalk( &ch, r->from );
			count[mode]++;

			/* the next word, as the mixer takes it */
			t0 = xbox_ft_rdtsc();
			VOX_FreeWord( &ch );
			ch.word_index++;
			VOX_LoadWord( &ch );
			st->ticks[mode] += xbox_ft_rdtsc() - t0;
			if( !FBitSet( ch.flags, FL_CHAN_SENTENCE_FINISHED ))
				ch.sfx = ch.words[ch.word_index].sfx;
		}
		/* the sentence ended at a word that exists but did not load
		   (VOX_LoadWord: no data), not at its end */
		if( ch.word_index >= 0 && ch.word_index < CVOXWORDMAX && ch.words[ch.word_index].sfx )
			pass[mode].stop = ch.word_index;
		pass[mode].count = count[mode];
		VOX_FreeWord( &ch );
		S_R3D_VoxRelease( &ch, true );
		Mem_Free2( &ch.words );
		/* no frame runs between sentences here: reclaim the slots a read in
		   flight orphaned (S_R3D_VoxFrame's job in play), or the pool runs dry */
		for( j = 0; j < R3D_VOX_SLOTS; j++ )
		{
			if( vox.slot[j].state == VOX_ORPHAN && Vox_Land( &vox.slot[j], true ))
			{
				Vox_CloseIo( &vox.slot[j] );
				vox.slot[j].state = VOX_FREE;
			}
		}
		for( j = 0; j < CVOXWORDMAX; j++ )
		{
			char path[MAX_QPATH + 16];
			vox_hdr_t *h;

			if( !kept[j] || !kept[j]->cache || S_R3D_VoxIsHeader( kept[j]->cache ) || !Vox_Path( kept[j]->name, path, sizeof( path )))
				continue;
			h = Vox_Facts( kept[j], path, 0 ); /* kept, or read again if the facts were trimmed */
			Vox_PreloadDone();
			if( h && h->streamable )
			{
				FS_FreeSound( kept[j]->cache );
				kept[j]->cache = NULL;
				Vox_Attach( h );
			}
		}
	}
	vox.force = -1;

	if( !Vox_CrcLoads( name, pass, st ))
		return false;
	for( j = 0; j < count[0]; j++ )
	{
		const vox_crc_word_t *a = &res[0][j], *b = &res[1][j];

		st->words++;
		st->checked++;
		if( a->sample != b->sample || a->forced_end != b->forced_end || a->flags != b->flags || a->crc != b->crc || a->crc_from != b->crc_from )
		{
			Vox_Print( "r3d_vox_crc %s word %d (%s): start %.0f/%.0f end %.0f/%.0f flags %x/%x crc %08x/%08x from %u crc %08x/%08x - DIFFERS\n",
				name, j, a->sfx ? a->sfx->name : "?", a->sample, b->sample, a->forced_end, b->forced_end, a->flags, b->flags,
				a->crc, b->crc, a->from, a->crc_from, b->crc_from );
			Vox_Print( "  e%d: samples %u/%u, rounded end %u/%u, at the end %d %d / %d %d, %s %08x / %s %08x\n",
				a->end_pct, a->samples, b->samples, a->rounded, b->rounded, a->at_end[0], a->at_end[1], b->at_end[0], b->at_end[1],
				a->whole ? "whole" : "window", a->data_crc, b->whole ? "whole" : "window", b->data_crc );
			st->differ++;
			same = false;
		}
	}
	return same;
}

static void Vox_Crc_f( void )
{
	vox_crc_stats_t st;
	char idx[16], line[512];
	int i, n;

	if( Cmd_Argc() < 2 )
	{
		Con_Printf( S_USAGE "r3d_vox_crc <sentence|all>\n" );
		return;
	}
	if( !snd.initialized )
	{
		Con_Printf( "r3d_vox_crc: no sound\n" );
		return;
	}
	memset( &st, 0, sizeof( st ));
	memset( vox_why_count, 0, sizeof( vox_why_count ));
	{
		const uint32_t ids[] = { XBOX_FT_W_VOX_STARTS, XBOX_FT_W_VOX_PF_HITS, XBOX_FT_W_VOX_OPENS, XBOX_FT_W_VOX_READS, XBOX_FT_W_VOX_SYNC, XBOX_FT_W_VOX_WAITS, XBOX_FT_W_VOX_SLOTS };
		size_t k;

		for( k = 0; k < ARRAYSIZE( ids ); k++ )
			xbox_ft_work.c[ids[k]] = 0; /* one command, one frame: its own totals */
	}
	crc_file = FS_Open( "vox_crc.txt", "a", true );
	if( !Q_stricmp( Cmd_Argv( 1 ), "all" ))
	{
		/* every sentence of sentences.txt, by index (VOX_LookupString
		   takes one); one that cannot be compared is counted as skipped,
		   it does not end the list */
		n = VOX_R3D_SentenceCount();
		for( i = 0; i < n; i++ )
		{
			Q_snprintf( idx, sizeof( idx ), "%d", i );
			Vox_CrcSentence( idx, &st );
		}
	}
	else Vox_CrcSentence( Cmd_Argv( 1 ), &st );
	Vox_CrcSummary( &st, line, sizeof( line ));
	Vox_Print( "%s", line );
	Vox_Print( "r3d_vox_crc: stream starts %u (prefetch hits %u), opens %u, reads %u (at once %u), waits %u, most slots %u; left to upstream:",
		xbox_ft_work.c[XBOX_FT_W_VOX_STARTS], xbox_ft_work.c[XBOX_FT_W_VOX_PF_HITS], xbox_ft_work.c[XBOX_FT_W_VOX_OPENS],
		xbox_ft_work.c[XBOX_FT_W_VOX_READS], xbox_ft_work.c[XBOX_FT_W_VOX_SYNC], xbox_ft_work.c[XBOX_FT_W_VOX_WAITS], xbox_ft_work.c[XBOX_FT_W_VOX_SLOTS] );
	for( i = 0; i < (int)ARRAYSIZE( vox_why_names ); i++ )
		Vox_Print( " %s %u", vox_why_names[i], vox_why_count[i] );
	Vox_Print( "\n" );
	if( crc_file )
	{
		FS_Close( crc_file );
		crc_file = NULL;
	}
}

void S_R3D_VoxInit( void )
{
	static qboolean registered; /* S_Init can run again; the commands stay */

	if( registered )
		return;
	registered = true;
	Cvar_RegisterVariable( &r3d_vox_stream );
	Cvar_RegisterVariable( &r3d_vox_unit );
	Cvar_RegisterVariable( &r3d_vox_dirs );
	Cmd_AddCommand( "r3d_vox_ab", Vox_AB_f, "flip r3d_vox_stream between upstream's whole loads and the dialogue stream (docs/r3d/divergences.md #177)" );
	Cmd_AddCommand( "r3d_vox_crc", Vox_Crc_f, "check every word of a sentence (or all) streamed against upstream's whole load: <sentence|all> (#177)" );
}

#endif /* XASH_XBOX && !XASH_DEDICATED */
