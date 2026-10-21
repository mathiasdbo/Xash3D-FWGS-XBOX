/*
xbox_vox_stream_host_test.c - dialogue streamed (divergence #177) against upstream's whole loads, bit for bit
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
Divergence #177 streams sentence words and precached speech from the
loose WAVs and claims the mix comes out bit for bit as upstream's whole
loads. xbox_vox_stream_host_test_tu.c is the engine's real sound code
built twice in this binary - up_* is upstream's (XASH_XBOX 0), xb_* the
Xbox build with the stream - and this drives both with the same calls,
in the same order, and compares after every paint:
- the mixed output (the DMA ring S_TransferPaintBuffer writes),
- every channel's sfx, position, forced end, word index, flags and
  volumes, and SND_GetChannelTimeLeft,
- every entity's lip-sync state (mouth_t),
- a save's soundlist (S_GetCurrentDynamicSounds).

Runs:
- every sentence of the retail sentences.txt (every word file it names,
  at every s/e/t/p/v it uses), one after another with overlaps, at 48 and
  44.1 kHz, with s_lerping 0 and 1 (1 is the Xbox default since #206),
  with reads that land at once, after a few polls, or slowly;
- save and restore in the middle of sentences, at the saved position and
  at a random position and word;
- scenarios: two channels speaking the same word; a precached word in a
  sentence; precached speech played on its own; a cue-looped speech file
  (and one whose LIST mark truncates it); a missing word; a stop with a
  read in flight; the static-channel reuse that memsets a playing channel;
  more sentences at once than the stream has slots; r3d_vox_unit 32768;
  r3d_vox_stream 0; r3d_vox_crc's own check of every sentence.
Divergence #184's speech feed is never on here (every span mixes), but
each span S_MixChannelToBuffer would hand it is recorded per build: the
spans are the same in both (position, length, bytes), each covers every
sample the CPU mix reads from it, and a word's spans without timecompress
follow on from each other exactly (Xbox_SndFeed_SpanFrames).
The disk is scripted (vox_stub/xbox_async_read.h): a queued read's buffer
holds garbage until it lands, so a read used early shows in the output;
the test's allocator fails a free of a buffer a read is still writing,
and an unaligned unbuffered read. The Xbox build's counters prove the
stream ran: a comparison of two whole-load runs would also be identical.

  tools/host-tests/run.sh builds and runs it:
  xbox_vox_stream_host_test <retail valve dir> <scratch dir> [quick]
  (the retail dir holds sound/sentences.txt and the sound/<dir>/ WAVs)
*/

#define VOX_TEST_MAIN 1
#include "common.h"
#include "sound_api.h"
#include "client.h"
#include "const.h"
#include "platform/xbox/xbox_frametime.h"
#include "xbox_async_read.h"
#include "platform/xbox/xbox_snd_feed.h"
#include <io.h> /* mkdir */

/* ---- the two builds ---- */

#define VOX_BOTH( ret, name, args ) ret up_##name args; ret xb_##name args;
VOX_BOTH( void, S_StartSound, ( const vec3_t pos, int ent, int chan, sound_t handle, float fvol, float attn, int pitch, int flags ))
VOX_BOTH( void, S_RestoreSound, ( const vec3_t pos, int ent, int chan, sound_t handle, float fvol, float attn, int pitch, int flags, double sample, double end, uint wordIndex ))
VOX_BOTH( void, S_StopAllSounds, ( qboolean ambient ))
VOX_BOTH( sound_t, S_RegisterSound, ( const char *name ))
VOX_BOTH( void, S_BeginRegistration, ( void ))
VOX_BOTH( void, S_EndRegistration, ( void ))
VOX_BOTH( void, S_PaintChannels, ( int endtime ))
VOX_BOTH( void, S_FreeChannel, ( channel_t *ch ))
VOX_BOTH( int, S_GetCurrentDynamicSounds, ( soundlist_t *pout, int size ))
VOX_BOTH( snd_globals_t *, t_snd, ( void ))
VOX_BOTH( int, t_timeleft, ( const channel_t *ch ))
VOX_BOTH( void, t_frame, ( void ))
VOX_BOTH( void, t_init, ( void ))
VOX_BOTH( void, t_shutdown, ( void ))
VOX_BOTH( cl_entity_t *, CL_GetEntityByIndex, ( int index ))
VOX_BOTH( sfx_t *, S_FindName, ( const char *name, qboolean *pfInCache ))
int xb_t_slots( int state );
int xb_t_slot_mem( void );
int xb_t_headers( void );
int xb_t_window_ready( const channel_t *ch );
void xb_t_set_cvar( const char *name, const char *value );
int xb_t_crc_sentence( const char *name, int *words, int *streamed, int *differ );
int xb_t_crc_mode( void );
int xb_t_crc_command( const char *const *names, int n, char *summary, int size, int *load_fail, int *skipped, int *unloadable );
int xb_t_crc_loads( int built0, int built1, int count0, int count1, int stop0, int stop1, char *summary, int size, int *load_fail );

/* ---- checks ---- */

static int failures, checks;
static const char *where = "";

#define CHECK( cond ) do { checks++; if( !( cond )) { if( failures++ < 40 ) printf( "FAIL %s:%d [%s]: %s\n", __FILE__, __LINE__, where, #cond ); } } while( 0 )

/* ---- what both builds link against ---- */

host_parm_t host;
static gameinfo_t gameinfo;
gameinfo_t *GI = &gameinfo;
client_static_t cls;
client_t cl;
clgame_static_t clgame;
ref_state_t refState;
convar_t sys_timescale = { (char *)"sys_timescale", (char *)"1", 0, 1.0f };
convar_t s_lerping = { (char *)"s_lerping", (char *)"1", 0, 1.0f };
convar_t s_warn_late_precache = { (char *)"s_warn_late_precache", (char *)"0", 0, 0.0f };
poolhandle_t sndpool;
int r3d_snd_dma_speed = 48000;

xbox_ft_waits_t xbox_ft_waits;
xbox_ft_work_t xbox_ft_work;
xbox_ft_spans_t xbox_ft_spans;
int xbox_ft_snd_extra;
/* divergence #180's room effect on the GP: never on here, so SX_RoomFX's branch runs */
qboolean sx_r3d_gp;
void SX_R3D_GpParams( int *params, qboolean active, int gain ) { (void)params; (void)active; (void)gain; }
void Xbox_SndGpRoom_Paint( const portable_samplepair_t *room, int start, int end, const int *params ) { (void)room; (void)start; (void)end; (void)params; }
/* divergence #182's sounds on APU voices: never on here, every channel mixes */
qboolean Xbox_SndVoices_Channel( channel_t *ch, const wavdata_t *sc, double pitch ) { (void)ch; (void)sc; (void)pitch; return false; }
void Xbox_SndVoices_BeginPaint( int start ) { (void)start; }
void Xbox_SndVoices_EndPaint( void ) { }
/* divergence #183's music on a voice: never on here */
void Xbox_SndMusicVoice_Paint( qboolean paused ) { (void)paused; }
void Xbox_SndVoices_Free( channel_t *ch ) { (void)ch; }
void Xbox_SndVoices_FreeSound( const sfx_t *sfx ) { (void)sfx; }
void Xbox_SndFeed_BeginPaint( int start ) { (void)start; }
void Xbox_SndFeed_EndPaint( void ) { }
qboolean Xbox_SndMeter_SDL( void ) { return false; } /* #189: no meter on the host */
void Xbox_SndMeter_Block( const short *samples, int frames ) { (void)samples; (void)frames; }
void Xbox_SndFeed_Free( channel_t *ch ) { (void)ch; }
uint64_t xbox_ft_snd_dma_ticks;

static int notes[8];
static char last_vox_note[64];
static int whole_notes; /* windows that fell back to a whole load ("io " and "big " vox notes) */
void Xbox_FT_Note( uint32_t kind, const char *name, uint32_t kcyc, uint32_t bytes )
{
	if( kind < 8 )
		notes[kind]++;
	if( kind == XBOX_FT_NOTE_VOX && ( !strncmp( name, "io ", 3 ) || !strncmp( name, "big ", 4 )))
		whole_notes++;
	if( kind == XBOX_FT_NOTE_VOX )
		snprintf( last_vox_note, sizeof( last_vox_note ), "%s", name );
}
void Xbox_FT_LoadBegin( xbox_ft_load_t *l ) { memset( l, 0, sizeof( *l )); }
void Xbox_FT_LoadAccum( const xbox_ft_load_t *l, int acc ) { }
static int sound_notes_tagged[128];
void Xbox_FT_SoundNote( const xbox_ft_load_t *l, const char *sound, int tag, uint32_t kcyc, uint32_t bytes )
{
	sound_notes_tagged[tag & 127]++;
}

qboolean CL_GetEntitySpatialization( channel_t *ch ) { return true; }
qboolean CL_IsInGame( void ) { return true; }
qboolean CL_IsPlayerIndex( int idx ) { return false; }
qboolean Host_IsSinglePlayerGame( void ) { return true; }
void SX_RoomFX( portable_samplepair_t *paint, int num_samples ) { }
void SX_ClearState( void ) { }
void SNDDMA_BeginPainting( void ) { }
void SNDDMA_Submit( void ) { }
float S_GetMasterVolume( void ) { return 1.0f; }
qboolean Sound_Process( wavdata_t **wav, int rate, int width, int channels, uint flags ) { return false; }
struct stream_s *Xbox_MusicStream_Open( const char *filename ) { return NULL; }
qboolean Sound_LoadMPG( const char *name, const byte *buffer, fs_offset_t filesize ) { return false; }

static int verbose;
void Con_Printf( const char *fmt, ... ) { if( verbose ) { va_list a; va_start( a, fmt ); vprintf( fmt, a ); va_end( a ); } }
void Con_DPrintf( const char *fmt, ... ) { }
void Con_Reportf( const char *fmt, ... ) { }

void Cvar_RegisterVariable( convar_t *var ) { var->value = (float)atof( var->string ); }
void Cvar_DirectSet( convar_t *var, const char *value ) { var->string = (char *)value; var->value = (float)atof( value ); }
void Cvar_DirectSetValue( convar_t *var, float value ) { var->value = value; }
int Cmd_AddCommand( const char *cmd_name, xcommand_t function, const char *cmd_desc ) { return 0; }
int Cmd_Argc( void ) { return 0; }
const char *Cmd_Argv( int arg ) { return ""; }

/* deterministic, so a failure reproduces */
static uint32_t rng = 0x12345678u;
static uint32_t rnd( void )
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}
static int rnd_range( int lo, int hi ) /* inclusive */
{
	return hi <= lo ? lo : lo + (int)( rnd() % (uint32_t)( hi - lo + 1 ));
}
int COM_RandomLong( int lMin, int lMax ) { return rnd_range( lMin, lMax ); }

/* ---- memory: the blocks a queued read writes into must not be freed ---- */

typedef struct block_s { struct block_s *next; size_t size; uint32_t magic; } block_t;
#define BLOCK_MAGIC 0xB10CB10Cu
static block_t *blocks;
static long live_blocks;
static int bad_frees;

/* the reads in flight (the fake disk's), checked against every free */
#define MAX_FLIGHT 64
static r3d_async_read_t *flight[MAX_FLIGHT];

void *vox_test_alloc( size_t size, int zero )
{
	block_t *b = malloc( sizeof( block_t ) + size + 16 );

	b->size = size;
	b->magic = BLOCK_MAGIC;
	b->next = blocks;
	blocks = b;
	live_blocks++;
	if( zero )
		memset( b + 1, 0, size );
	else memset( b + 1, 0xA5, size ); /* never zero by luck */
	return b + 1;
}

void vox_test_free( void *p )
{
	block_t **pp, *b;
	int i;

	if( !p )
		return;
	b = (block_t *)p - 1;
	if( b->magic != BLOCK_MAGIC )
	{
		bad_frees++; /* not a block this allocator handed out (Mem_Free of an interior pointer) */
		return;
	}
	b->magic = 0;
	for( i = 0; i < MAX_FLIGHT; i++ )
	{
		const byte *d = flight[i] ? flight[i]->dst : NULL;

		if( d && d >= (byte *)p && d < (byte *)p + b->size )
			bad_frees++;
	}
	for( pp = &blocks; *pp; pp = &( *pp )->next )
	{
		if( *pp == b )
		{
			*pp = b->next;
			break;
		}
	}
	live_blocks--;
	memset( p, 0xDD, b->size );
	free( b );
}

void *vox_test_realloc( void *p, size_t size )
{
	void *n = vox_test_alloc( size, 1 );

	if( p )
	{
		block_t *b = (block_t *)p - 1;

		memcpy( n, p, b->size < size ? b->size : size );
		vox_test_free( p );
	}
	return n;
}

/* ---- files: the retail tree, then the test's synthetic one ---- */

static const char *roots[2];
static char archived[64]; /* a name FS_GetDiskPath answers as inside an archive */
static char fs_fail_in_pass[64]; /* a name whose whole load fails in r3d_vox_crc's pass fs_fail_pass */
static int fs_fail_pass = -1;
static long fs_whole_bytes;

static FILE *open_in_roots( const char *path, char *full, size_t size )
{
	int i;

	for( i = 0; i < 2; i++ )
	{
		FILE *f;

		if( !roots[i] )
			continue;
		snprintf( full, size, "%s/%s", roots[i], path );
		if(( f = fopen( full, "rb" )) != NULL )
			return f;
	}
	return NULL;
}

byte *FS_LoadFile( const char *path, fs_offset_t *filesizeptr, qboolean gamedironly )
{
	char full[1024];
	FILE *f = open_in_roots( path, full, sizeof( full ));
	long n;
	byte *buf;

	if( filesizeptr )
		*filesizeptr = 0;
	if( f && fs_fail_in_pass[0] && strstr( path, fs_fail_in_pass ) && ( fs_fail_pass < 0 || xb_t_crc_mode() == fs_fail_pass ))
	{
		fclose( f );
		f = NULL;
	}
	if( !f )
		return NULL;
	fseek( f, 0, SEEK_END );
	n = ftell( f );
	fseek( f, 0, SEEK_SET );
	buf = vox_test_alloc( n + 1, 0 );
	if( fread( buf, 1, n, f ) != (size_t)n )
		n = 0;
	buf[n] = 0;
	fclose( f );
	if( filesizeptr )
		*filesizeptr = n;
	if( strstr( path, ".wav" ))
		fs_whole_bytes += n;
	return buf;
}

const char *FS_GetDiskPath( const char *name, qboolean gamedironly )
{
	static char full[1024];
	FILE *f;

	if( archived[0] && strstr( name, archived ))
		return NULL;
	if( !( f = open_in_roots( name, full, sizeof( full ))))
		return NULL;
	fclose( f );
	return full;
}

file_t *FS_Open( const char *filepath, const char *mode, qboolean gamedironly ) { return NULL; }
fs_offset_t FS_Read( file_t *file, void *buffer, size_t buffersize ) { return 0; }
fs_offset_t FS_Write( file_t *file, const void *data, size_t datasize ) { return 0; }
int FS_Seek( file_t *file, fs_offset_t offset, int whence ) { return -1; }
fs_offset_t FS_Tell( file_t *file ) { return 0; }
qboolean FS_Eof( file_t *file ) { return true; }
int FS_Close( file_t *file ) { return 0; }
qboolean FS_FileExists( const char *filename, int gamedironly ) { return false; }
search_t *FS_Search( const char *pattern, int caseinsensitive, int gamedironly ) { return NULL; }

/* ---- the disk: compat/xbox/xbox_posix_io.c's r3d_async_*, scripted ---- */

enum { DISK_NOW = 0, DISK_POLLS, DISK_SLOW };
static int disk_mode;
static char disk_fail_name[64]; /* a read of this file at disk_fail_offset fails */
static uint32_t disk_fail_offset;
static int disk_failed;
static long disk_reads, disk_bytes, disk_unaligned, disk_opens, disk_closes;
static int next_handle;

int r3d_async_open( r3d_async_read_t *a, const char *path )
{
	memset( a, 0, sizeof( *a ));
	if( !( a->f = fopen( path, "rb" )))
		return -1;
	snprintf( a->path, sizeof( a->path ), "%s", path );
	a->id = ++next_handle;
	disk_opens++;
	return 0;
}

long long r3d_async_size( r3d_async_read_t *a )
{
	long n;

	if( !a->f )
		return -1;
	fseek( a->f, 0, SEEK_END );
	n = ftell( a->f );
	return n;
}

static int disk_land( r3d_async_read_t *a )
{
	int i, got = 0;

	if( disk_fail_name[0] && a->offset == disk_fail_offset && strstr( a->path, disk_fail_name ))
	{
		got = -1;
		disk_failed++;
	}
	else
	{
		fseek( a->f, a->offset, SEEK_SET );
		got = (int)fread( a->dst, 1, a->asked, a->f );
		disk_bytes += got;
	}
	for( i = 0; i < MAX_FLIGHT; i++ )
	{
		if( flight[i] == a )
			flight[i] = NULL;
	}
	a->state = R3D_ASYNC_IDLE;
	return got;
}

int r3d_async_start( r3d_async_read_t *a, uint32_t offset, void *dst, uint32_t bytes )
{
	int i;

	if( !a->f || a->state != R3D_ASYNC_IDLE )
		return -1;
	if(( offset & 4095 ) || ( bytes & 4095 ) || ((uintptr_t)dst & 4095 ))
		disk_unaligned++;
	disk_reads++;
	a->offset = offset;
	a->asked = bytes;
	a->dst = dst;
	memset( dst, 0xCD, bytes ); /* what an early reader would see */
	if( disk_mode == DISK_NOW && ( rnd() & 7 ))
	{
		a->done_bytes = disk_land( a );
		a->state = R3D_ASYNC_DONE;
		return 0;
	}
	a->state = R3D_ASYNC_RUNNING;
	a->polls = disk_mode == DISK_NOW ? 0 : disk_mode == DISK_POLLS ? rnd_range( 1, 4 ) : rnd_range( 8, 40 );
	for( i = 0; i < MAX_FLIGHT; i++ )
	{
		if( !flight[i] )
		{
			flight[i] = a;
			break;
		}
	}
	return 1;
}

int r3d_async_finish( r3d_async_read_t *a, int wait, int *waited )
{
	*waited = 0;
	if( a->state == R3D_ASYNC_DONE )
	{
		a->state = R3D_ASYNC_IDLE;
		return a->done_bytes;
	}
	if( a->state != R3D_ASYNC_RUNNING )
		return -1;
	if( a->polls > 0 && !wait )
	{
		a->polls--;
		return R3D_ASYNC_PENDING;
	}
	if( a->polls > 0 )
		*waited = 1;
	return disk_land( a );
}

void r3d_async_close( r3d_async_read_t *a )
{
	int waited;

	if( a->state == R3D_ASYNC_RUNNING )
		r3d_async_finish( a, 1, &waited );
	if( a->f )
	{
		fclose( a->f );
		disk_closes++;
	}
	a->f = NULL;
	a->state = R3D_ASYNC_IDLE;
}

/* ---- the comparisons ---- */

#define RING_FRAMES 8192

static void compare( qboolean timeleft )
{
	const snd_globals_t *a = up_t_snd(), *b = xb_t_snd();
	int i, n = a->total_channels > b->total_channels ? a->total_channels : b->total_channels;

	CHECK( a->paintedtime == b->paintedtime );
	CHECK( a->total_channels == b->total_channels );
	CHECK( !memcmp( a->buffer, b->buffer, RING_FRAMES * 4 ));
	if( memcmp( a->buffer, b->buffer, RING_FRAMES * 4 ) && failures < 4 )
	{
		const short *x = (const short *)a->buffer, *y = (const short *)b->buffer;

		for( i = 0; i < RING_FRAMES * 2 && x[i] == y[i]; i++ );
		printf( "  first difference at ring short %d (painted %d): %d / %d\n", i, a->paintedtime, x[i], y[i] );
		for( i = 0; i < n; i++ )
		{
			if( a->channels[i].sfx )
				printf( "  up ch %d %s sample %.3f end %.0f word %d flags %x | xb %s sample %.3f end %.0f res %llx\n", i,
					a->channels[i].sfx->name, a->channels[i].sample, a->channels[i].forced_end, a->channels[i].word_index, a->channels[i].flags,
					b->channels[i].sfx ? b->channels[i].sfx->name : "-", b->channels[i].sample, b->channels[i].forced_end,
					(unsigned long long)b->channels[i].engine_reserved[0] );
		}
	}
	for( i = 0; i < n; i++ )
	{
		const channel_t *x = &a->channels[i], *y = &b->channels[i];

		CHECK( !x->sfx == !y->sfx );
		if( !x->sfx || !y->sfx )
			continue;
		CHECK( !strcmp( x->sfx->name, y->sfx->name ));
		CHECK( !strcmp( x->name, y->name ));
		CHECK( x->sample == y->sample );
		CHECK( x->forced_end == y->forced_end );
		CHECK( x->word_index == y->word_index );
		CHECK( x->flags == y->flags );
		CHECK( x->leftvol == y->leftvol && x->rightvol == y->rightvol );
		CHECK( !x->words == !y->words );
		/* each word's FL_VOXWORD_IN_CACHE: whether upstream held the word
		   loaded when the sentence started (a placeholder stands for that
		   cache, #177), which decides whether VOX_FreeWord frees it */
		if( x->words && y->words )
		{
			int w;

			for( w = 0; w < 64 && ( w <= x->word_index || x->words[w].sfx ); w++ )
				CHECK( x->words[w].flags == y->words[w].flags );
		}
		if( timeleft )
		{
			const int tu = up_t_timeleft( x ), tx = xb_t_timeleft( y );

			CHECK( tu == tx );
			if( tu != tx && failures < 6 )
			{
				int w;

				printf( "  timeleft ch %d: up %d xb %d; word %d sample %.2f end %.0f flags %x; up cache %p xb cache %p\n", i, tu, tx,
					x->word_index, x->sample, x->forced_end, x->flags, (void *)x->sfx->cache, (void *)y->sfx->cache );
				for( w = 0; x->words && w < 64 && ( x->words[w].sfx || w <= x->word_index ); w++ )
				{
					const sfx_t *u = x->words[w].sfx, *v = y->words ? y->words[w].sfx : NULL;

					printf( "    word %d %s e%d flags %d/%d: up %s %d, xb %s %d\n", w, u ? u->name : "(freed)", x->words[w].end, x->words[w].flags,
						y->words ? y->words[w].flags : -1,
						u && u->cache ? "cache" : "NULL", u && u->cache ? (int)u->cache->samples : -1,
						v && v->cache ? (( v->cache->flags & ( 1u << 25 )) ? "header" : "cache" ) : "NULL", v && v->cache ? (int)v->cache->samples : -1 );
				}
			}
		}
	}
	for( i = 0; i < 64; i++ )
	{
		const mouth_t *x = &up_CL_GetEntityByIndex( i )->mouth, *y = &xb_CL_GetEntityByIndex( i )->mouth;

		CHECK( x->mouthopen == y->mouthopen && x->sndcount == y->sndcount && x->sndavg == y->sndavg );
	}
}

static int busy( void )
{
	const snd_globals_t *a = up_t_snd();
	int i;

	for( i = 0; i < a->total_channels; i++ )
	{
		if( a->channels[i].sfx )
			return 1;
	}
	return 0;
}

/* ---- #184's speech feed: the spans the walk would hand it ---- */
#define MAX_SPANS 2048
typedef struct { int chan, start, n; uint32_t crc; byte head[64]; int vol[2]; } span_t;
static span_t spans[2][MAX_SPANS];
static int nspans[2], feed_side = -1;
static struct { const sfx_t *sfx; int word, end; } feed_last[2][MAX_CHANNELS];
static long feed_spans, feed_tiled, feed_frames;

static void feed_forget( void )
{
	memset( feed_last, 0, sizeof( feed_last ));
}

/* Records the span instead of taking it: every channel still mixes */
qboolean Xbox_SndFeed_Take( channel_t *ch, const wavdata_t *wav, const void *audio, int available, double rate, double pitch, int out_count, int offset )
{
	const int side = feed_side;
	snd_globals_t *g;
	int idx, start, n, last, k, tc;
	uint32_t crc = 2166136261u;
	const byte *b = audio;

	(void)pitch;
	(void)offset;
	if( side < 0 )
		return false;
	g = side ? xb_t_snd() : up_t_snd();
	idx = (int)( ch - g->channels );
	start = (int)ch->sample;
	n = Xbox_SndFeed_SpanFrames( ch->sample, rate, out_count, available );
	/* S_MixAudio reads floor( frac + i * rate ) for i < out_count (and one
	   more with s_lerping, the interpolation's, which the APU does not
	   need): within the span, or (upsampled) its end, the next span's first */
	last = (int)floor(( ch->sample - floor( ch->sample )) + ( out_count - 1 ) * rate );
	CHECK( last <= n );
	/* upstream's 8-bit samples are signed, the Xbox build's unsigned (#181:
	   what the APU plays, AL_FORMAT_MONO8): compared as the Xbox's */
	for( k = 0; k < n * wav->width * wav->channels; k++ )
		crc = ( crc ^ (byte)( b[k] ^ ( side == 0 && wav->width == 1 ? 0x80 : 0 ))) * 16777619u;
	tc = ch->words ? ch->words[ch->word_index].timecompress : 0;
	if( idx >= 0 && idx < MAX_CHANNELS )
	{
		const qboolean loops = FBitSet( ch->flags, FL_CHAN_USE_LOOP ) && FBitSet( wav->flags, SOUND_LOOPED );

		/* a looping sound's wrap goes back to its loop start: not checked */
		if( feed_last[side][idx].sfx == ch->sfx && feed_last[side][idx].word == ch->word_index
			&& tc == 0 && !( loops && start < feed_last[side][idx].end ))
		{
			CHECK( start == feed_last[side][idx].end ); /* no gap, no overlap */
			feed_tiled++;
		}
		feed_last[side][idx].sfx = ch->sfx;
		feed_last[side][idx].word = ch->word_index;
		feed_last[side][idx].end = start + n;
	}
	if( nspans[side] < MAX_SPANS )
	{
		span_t *s = &spans[side][nspans[side]++];

		s->chan = idx;
		s->start = start;
		s->n = n;
		s->crc = crc;
		memcpy( s->head, b, n * wav->width < 64 ? (size_t)( n * wav->width ) : 64u );
		s->vol[0] = ch->leftvol;
		s->vol[1] = ch->rightvol;
	}
	feed_spans++;
	feed_frames += n;
	return false;
}

/* The two builds handed the same spans this paint */
static void feed_compare( void )
{
	int i;

	CHECK( nspans[0] == nspans[1] );
	for( i = 0; i < nspans[0] && i < nspans[1]; i++ )
	{
		const span_t *a = &spans[0][i], *b = &spans[1][i];

		if( a->chan != b->chan || a->start != b->start || a->n != b->n || a->crc != b->crc )
		{
			if( verbose || failures < 20 )
				printf( "  span %d: up chan %d at %d, %d frames, crc %08x; xb chan %d at %d, %d frames, crc %08x\n",
					i, a->chan, a->start, a->n, a->crc, b->chan, b->start, b->n, b->crc );
			if( verbose || failures < 20 )
			{
				int k;
				printf( "    vol up %d/%d xb %d/%d; first bytes up/xb:", a->vol[0], a->vol[1], b->vol[0], b->vol[1] );
				for( k = 0; k < 12; k++ )
					printf( " %02x/%02x", a->head[k], b->head[k] );
				printf( "\n" );
			}
			CHECK( !"a span differs between the builds" );
			break;
		}
	}
	nspans[0] = nspans[1] = 0;
}

/* one frame: what SND_UpdateSound does, then a paint of a random length */
static int paints;
static void frame( int max_paint )
{
	const int n = rnd_range( 1, max_paint );
	const int end = up_t_snd()->paintedtime + n;

	up_t_frame();
	xb_t_frame();
	feed_side = 0;
	up_S_PaintChannels( end );
	feed_side = 1;
	xb_S_PaintChannels( end );
	feed_side = -1;
	feed_compare();
	paints++;
	compare(( paints % 5 ) == 0 );
}

static void start( const char *name, int ent, int chan, int pitch, int flags )
{
	const sound_t hu = up_S_RegisterSound( name ), hx = xb_S_RegisterSound( name );
	const vec3_t pos = { 0, 0, 0 };

	CHECK( strlen( name ) < MAX_QPATH ); /* S_FindName drops a longer name: nothing would play */
	feed_forget(); /* a channel may be taken over */
	CHECK( hu == hx );
	up_S_StartSound( pos, ent, chan, hu, 1.0f, ATTN_NORM, pitch, flags );
	xb_S_StartSound( pos, ent, chan, hx, 1.0f, ATTN_NORM, pitch, flags );
	compare( true );
}

static void play_out( int max_frames, int max_paint )
{
	int i;

	for( i = 0; i < max_frames && busy(); i++ )
		frame( max_paint );
}

/* A save of every channel, then each one restored - at the saved place, or
   (`jump`) at a random word and position, the same in both builds. */
static void save_restore( qboolean jump )
{
	soundlist_t la[64], lb[64];
	const int na = up_S_GetCurrentDynamicSounds( la, 64 ), nb = xb_S_GetCurrentDynamicSounds( lb, 64 );
	int i;

	CHECK( na == nb );
	for( i = 0; i < na && i < nb; i++ )
	{
		CHECK( !strcmp( la[i].name, lb[i].name ));
		CHECK( la[i].wordIndex == lb[i].wordIndex && la[i].samplePos == lb[i].samplePos && la[i].forcedEnd == lb[i].forcedEnd && la[i].looping == lb[i].looping );
	}
	up_S_StopAllSounds( true );
	xb_S_StopAllSounds( true );
	feed_forget();
	for( i = 0; i < na && i < nb; i++ )
	{
		soundlist_t *e = &la[i];
		const sound_t hu = up_S_RegisterSound( e->name ), hx = xb_S_RegisterSound( e->name );
		double sample = e->samplePos, end = e->forcedEnd;
		uint word = e->wordIndex;

		if( jump )
		{
			word = (uint)rnd_range( 0, word + 1 );
			sample = rnd_range( 0, 60000 );
			end = rnd() & 1 ? 0.0 : sample + rnd_range( 100, 30000 );
		}
		up_S_RestoreSound( e->origin, e->entnum, e->channel, hu, e->volume, e->attenuation, e->pitch, e->looping ? 0 : SND_STOP_LOOPING, sample, end, word );
		xb_S_RestoreSound( e->origin, e->entnum, e->channel, hx, e->volume, e->attenuation, e->pitch, e->looping ? 0 : SND_STOP_LOOPING, sample, end, word );
	}
	/* the restore read the window at the saved place (S_R3D_VoxSeek), at
	   load time, not in the first paint */
	for( i = 0; i < xb_t_snd()->total_channels; i++ )
	{
		if( xb_t_snd()->channels[i].sfx )
			CHECK( xb_t_window_ready( &xb_t_snd()->channels[i] ));
	}
	compare( true );
}

/* ---- synthetic WAVs ---- */

static void put32( FILE *f, uint32_t v ) { fputc( v & 255, f ); fputc(( v >> 8 ) & 255, f ); fputc(( v >> 16 ) & 255, f ); fputc( v >> 24, f ); }
static void put16( FILE *f, uint32_t v ) { fputc( v & 255, f ); fputc(( v >> 8 ) & 255, f ); }

/* 8-bit mono PCM; a cue chunk when loop >= 0 (after the data, as some
   editors write it), a LIST chunk with a `mark` of `mark` samples when
   mark > 0 */
static void write_wav( const char *dir, const char *name, int rate, int n, int loop, int mark )
{
	char path[1024];
	FILE *f;
	int i, cue = loop >= 0 ? 8 + 28 : 0, list = mark > 0 ? 8 + 36 : 0;
	const int data = n + ( n & 1 );

	snprintf( path, sizeof( path ), "%s/sound/test/%s", dir, name );
	f = fopen( path, "wb" );
	fwrite( "RIFF", 1, 4, f );
	put32( f, 4 + 8 + 16 + 8 + data + cue + list );
	fwrite( "WAVE", 1, 4, f );
	fwrite( "fmt ", 1, 4, f );
	put32( f, 16 );
	put16( f, 1 ); put16( f, 1 ); put32( f, rate ); put32( f, rate ); put16( f, 1 ); put16( f, 8 );
	fwrite( "data", 1, 4, f );
	put32( f, n );
	for( i = 0; i < n; i++ )
		fputc( 128 + (int)( 100.0 * sin( i * 0.05 ) * ( 0.3 + 0.7 * (( i / 977 ) & 1 ))) + (int)( rnd() % 7 ) - 3, f );
	if( n & 1 )
		fputc( 0, f );
	if( loop >= 0 )
	{
		/* one cue point: count, then id, position, fccChunk, chunkStart,
		   blockStart, sampleOffset - which Sound_LoadWAV reads at the chunk's
		   start + 32 (snd_wav.c:245-248) */
		fwrite( "cue ", 1, 4, f );
		put32( f, 28 );
		put32( f, 1 ); put32( f, 1 ); put32( f, 0 ); fwrite( "data", 1, 4, f ); put32( f, 0 ); put32( f, 0 );
		put32( f, (uint32_t)loop );
	}
	if( mark > 0 )
	{
		/* an adtl list with one ltxt: cue id, sample length (Sound_LoadWAV
		   reads it at the chunk's start + 24), purpose "mark" (+ 28) */
		fwrite( "LIST", 1, 4, f );
		put32( f, 36 );
		fwrite( "adtl", 1, 4, f ); fwrite( "ltxt", 1, 4, f ); put32( f, 20 ); put32( f, 1 );
		put32( f, (uint32_t)mark );
		fwrite( "mark", 1, 4, f );
		put32( f, 0 ); put32( f, 0 ); put32( f, 0 );
	}
	fclose( f );
}

/* ---- the runs ---- */

static int count_sentences( void )
{
	fs_offset_t n;
	byte *buf = FS_LoadFile( "sound/sentences.txt", &n, false );
	const char *p, *end;
	int count = 0;

	if( !buf )
		return 0;
	for( p = (const char *)buf, end = p + n; p < end; )
	{
		while( p < end && ( *p == '\n' || *p == '\r' || *p == '\t' || *p == ' ' ))
			p++;
		if( p < end && *p != '/' )
			count++;
		while( p < end && *p != '\n' && *p != '\r' )
			p++;
	}
	vox_test_free( buf );
	return count;
}

static void reset_both( void )
{
	feed_forget();
	up_t_shutdown();
	xb_t_shutdown();
	up_t_init();
	xb_t_init();
	compare( false );
}

static void stream_counters( long *starts, long *pf )
{
	*starts += xbox_ft_work.c[XBOX_FT_W_VOX_STARTS];
	*pf += xbox_ft_work.c[XBOX_FT_W_VOX_PF_HITS];
}

/* Every sentence, with overlaps, saves and restores. */
static void run_sentences( int count, int stride, const char *label, qboolean streams )
{
	char name[32];
	long starts = 0, pf = 0;
	int i;

	for( i = 0; i < count; i += stride )
	{
		const int pitch = ( i % 9 == 4 ) ? 90 : ( i % 9 == 7 ) ? 115 : 100;
		int restore = 1;

		memset( &xbox_ft_work, 0, sizeof( xbox_ft_work ));
		snprintf( name, sizeof( name ), "!%d", i );
		start( name, 1 + i % 8, CHAN_VOICE, pitch, 0 );
		if( i % 7 == 3 && i + 1 < count )
		{
			/* a second speaker over the first; at times the same sentence */
			snprintf( name, sizeof( name ), "!%d", i % 21 == 3 ? i : i + 1 );
			start( name, 9 + i % 4, CHAN_VOICE, 100, 0 );
		}
		if( !( i % 11 == 5 || i % 13 == 7 ))
			restore = 0;
		else
		{
			int k, at = rnd_range( 1, 40 );

			for( k = 0; k < at && busy(); k++ )
				frame( 1500 );
			if( busy( ))
				save_restore( i % 13 == 7 );
		}
		play_out( 100000, 2048 );
		stream_counters( &starts, &pf );
		if( !restore )
		{
			/* straight play: the window only slides, and the units after the
			   one playing are queued ahead - a file is read at once only at
			   its start (its head, its tail, at most a unit the trim reaches) */
			CHECK( xbox_ft_work.c[XBOX_FT_W_VOX_SEEKS] == 0 );
			CHECK( xbox_ft_work.c[XBOX_FT_W_VOX_SYNC] <= 3 * xbox_ft_work.c[XBOX_FT_W_VOX_OPENS] );
		}
		/* frames with nothing playing: the orphans reclaimed, no slot left */
		{
			int k;

			for( k = 0; k < 100 && xb_t_slots( -1 ); k++ )
				frame( 64 );
		}
		CHECK( xb_t_slots( -1 ) == 0 );
		CHECK( whole_notes == 0 ); /* no window fell back to a whole load */
	}
	printf( "  %s: %d sentences, stream starts %ld (prefetch hits %ld), paints %d\n", label, ( count + stride - 1 ) / stride, starts, pf, paints );
	CHECK( streams ? starts > 0 : starts == 0 ); /* which side ran (docs/r3d/lessons.md, the A/B switch entry) */
}

static int crc_all; /* every sentence through r3d_vox_crc's check (the full pass) */

static void scenarios( const char *synth )
{
	long before;
	int i, words, streamed, differ;
	char name[48];

	/* a sentence takes its directory from its last '/' (VOX_GetDirectory,
	   s_vox.c:223-252): every word of one is in the same directory. Each
	   scenario checks the stream ran in it (vox_starts). */
	where = "two channels, one word";
	before = xbox_ft_work.c[XBOX_FT_W_VOX_STARTS];
	start( "!#scientist/c1a0_sci_dis1a", 1, CHAN_VOICE, 100, 0 );
	frame( 300 );
	start( "!#scientist/c1a0_sci_dis1a", 2, CHAN_VOICE, 100, 0 );
	CHECK( xbox_ft_work.c[XBOX_FT_W_VOX_STARTS] == before + 2 );
	play_out( 100000, 2048 );

	where = "precached word in a sentence";
	up_S_BeginRegistration(); xb_S_BeginRegistration();
	/* registered: a placeholder for a word above VOX_PRECACHE_MIN (one
	   below it is held whole from here, as upstream holds every precache) */
	up_S_RegisterSound( "scientist/c1a0_sci_dis1a" ); xb_S_RegisterSound( "scientist/c1a0_sci_dis1a" );
	up_S_EndRegistration(); xb_S_EndRegistration();
	before = xbox_ft_work.c[XBOX_FT_W_VOX_STARTS];
	start( "!#scientist/c1a0_sci_dis1a(s20 e90) headcrab c1a0_sci_dis1a", 3, CHAN_VOICE, 100, 0 );
	play_out( 100000, 2048 );
	CHECK( xbox_ft_work.c[XBOX_FT_W_VOX_STARTS] >= before + 2 ); /* the registered word twice (headcrab streams unless the registration held it whole) */

	where = "precached speech played";
	up_S_BeginRegistration(); xb_S_BeginRegistration();
	up_S_RegisterSound( "scientist/c1a1_sci_4scan.wav" ); xb_S_RegisterSound( "scientist/c1a1_sci_4scan.wav" );
	up_S_EndRegistration(); xb_S_EndRegistration();
	before = xbox_ft_work.c[XBOX_FT_W_VOX_STARTS];
	start( "scientist/c1a1_sci_4scan.wav", 4, CHAN_VOICE, 100, 0 );
	CHECK( xbox_ft_work.c[XBOX_FT_W_VOX_STARTS] == before + 1 );
	for( i = 0; i < 30; i++ )
		frame( 2048 );
	save_restore( false );
	play_out( 100000, 2048 );

	where = "word params at the edges";
	before = xbox_ft_work.c[XBOX_FT_W_VOX_STARTS];
	start( "!#scientist/c1a0_sci_dis1a(s50 e51) c1a0_sci_dis1a(s0 e1)", 5, CHAN_VOICE, 100, 0 );
	play_out( 100000, 2048 );
	start( "!#scientist/c1a0_sci_dis1a(e99) c1a0_sci_dis1a(p50 t50)", 5, CHAN_VOICE, 100, 0 );
	play_out( 100000, 2048 );
	start( "!#scientist/c1a1_sci_4scan(p160 t30 v40)", 5, CHAN_VOICE, 100, 0 );
	play_out( 100000, 2048 );
	CHECK( xbox_ft_work.c[XBOX_FT_W_VOX_STARTS] == before + 5 );

	where = "missing word";
	start( "!#scientist/zzz_not_there headcrab zzz_nor_this", 6, CHAN_VOICE, 100, 0 );
	play_out( 100000, 2048 );

	where = "a word in an archive";
	strcpy( archived, "c1a0_ba_desk" );
	start( "!#barney/c1a0_ba_desk", 6, CHAN_VOICE, 100, 0 );
	play_out( 100000, 2048 );
	archived[0] = 0;

	where = "stop with a read in flight";
	disk_mode = DISK_SLOW;
	before = xbox_ft_work.c[XBOX_FT_W_VOX_STARTS];
	start( "!#scientist/c1a0_sci_dis1a c1a1_sci_4scan", 7, CHAN_VOICE, 100, 0 );
	CHECK( xbox_ft_work.c[XBOX_FT_W_VOX_STARTS] == before + 1 );
	for( i = 0; i < 3; i++ )
		frame( 600 );
	up_S_FreeChannel( &up_t_snd()->channels[NUM_AMBIENTS] ); /* the first dynamic channel: the speaker */
	xb_S_FreeChannel( &xb_t_snd()->channels[NUM_AMBIENTS] );
	compare( false );
	for( i = 0; i < 60; i++ )
		frame( 64 );
	CHECK( xb_t_slots( -1 ) == 0 );
	disk_mode = DISK_POLLS;

	/* SND_PickStaticChannel reuses a channel playing the same sfx at the
	   same place without S_FreeChannel, and S_StartSound memsets it: only a
	   sound that is not a sentence (a sentence's channel holds its word's
	   sfx, not the sentence's) */
	where = "static channel reused while playing";
	start( "scientist/c1a1_sci_4scan.wav", 0, CHAN_STATIC, 100, 0 );
	frame( 500 );
	start( "scientist/c1a1_sci_4scan.wav", 0, CHAN_STATIC, 100, 0 ); /* the same place and sfx: no S_FreeChannel, a memset */
	CHECK( xb_t_slots( 2 ) == 2 ); /* the memset dropped the old window's token, not its slot */
	frame( 500 );
	frame( 500 );
	CHECK( xb_t_slots( 2 ) == 1 ); /* the old window released by the sweep: only the new one plays */
	play_out( 100000, 2048 );

	where = "more speakers than slots";
	for( i = 0; i < 16; i++ )
	{
		snprintf( name, sizeof( name ), "!#scientist/c1a0_sci_dis1a(s%d)", i * 3 );
		start( name, 20 + i, CHAN_VOICE, 100, 0 );
	}
	CHECK( xbox_ft_work.c[XBOX_FT_W_VOX_FALLBACKS] > 0 );
	play_out( 100000, 2048 );

	where = "a failed read";
	CHECK( whole_notes == 0 ); /* nothing so far fell back to a whole load */
	/* a file nothing else plays (a retail word may be held whole by an
	   earlier scenario): its second unit, queued while the first plays */
	write_wav( synth, "fail1.wav", 11025, 40000, -1, 0 );
	snprintf( disk_fail_name, sizeof( disk_fail_name ), "fail1.wav" );
	disk_fail_offset = 8192;
	disk_failed = 0;
	start( "!#test/fail1", 7, CHAN_VOICE, 100, 0 );
	play_out( 100000, 2048 );
	disk_fail_name[0] = 0;
	CHECK( disk_failed == 1 );
	CHECK( whole_notes == 1 ); /* the failed read's window, and only it */
	whole_notes = 0;

	where = "a looped speech file";
	xb_t_set_cvar( "r3d_vox_dirs", "vox fvox barney scientist hgrunt gman holo agrunt aslave test" );
	/* above VOX_PRECACHE_MIN, the data precached speech needs to stream */
	write_wav( synth, "loop1.wav", 11025, 90000, 23456, 0 );
	write_wav( synth, "loop2.wav", 22050, 140000, 100, 100000 ); /* the mark cuts it at 100100 */
	write_wav( synth, "loop3.wav", 11025, 70000, 69990, 0 ); /* a loop of 10 samples */
	up_S_BeginRegistration(); xb_S_BeginRegistration();
	up_S_RegisterSound( "test/loop1.wav" ); xb_S_RegisterSound( "test/loop1.wav" );
	up_S_RegisterSound( "test/loop2.wav" ); xb_S_RegisterSound( "test/loop2.wav" );
	up_S_RegisterSound( "test/loop3.wav" ); xb_S_RegisterSound( "test/loop3.wav" );
	up_S_EndRegistration(); xb_S_EndRegistration();
	before = xbox_ft_work.c[XBOX_FT_W_VOX_STARTS];
	start( "test/loop1.wav", 0, CHAN_STATIC, 100, 0 );
	start( "test/loop2.wav", 30, CHAN_VOICE, 100, 0 );
	start( "test/loop3.wav", 31, CHAN_VOICE, 100, 0 );
	CHECK( xbox_ft_work.c[XBOX_FT_W_VOX_STARTS] == before + 3 );
	/* the fixture is what it claims: upstream's loader saw the cue and the mark */
	{
		const sfx_t *q = up_S_FindName( "test/loop2.wav", NULL );

		CHECK( q && q->cache && q->cache->samples == 100100 && q->cache->loop_start == 100 && ( q->cache->flags & SOUND_LOOPED ));
	}
	for( i = 0; i < 1500; i++ )
		frame( 2048 );
	save_restore( false );
	for( i = 0; i < 300; i++ )
		frame( 2048 );
	up_S_StopAllSounds( true );
	xb_S_StopAllSounds( true );

	where = "r3d_vox_crc";
	before = 0;
	disk_mode = DISK_SLOW; /* reads still in flight when a sentence ends: orphans */
	for( i = 0; i < ( crc_all ? 1065 : 80 ); i++ )
	{
		snprintf( name, sizeof( name ), "%d", crc_all ? i : i * 13 );
		CHECK( xb_t_crc_sentence( name, &words, &streamed, &differ ));
		CHECK( differ == 0 );
		if( differ )
			printf( "  r3d_vox_crc sentence %s: %d of %d words differ\n", name, differ, words );
		before += streamed;
	}
	/* the command runs with no frame between sentences: its slots must
	   not run out (orphans reclaimed), so the stream keeps being checked */
	printf( "  r3d_vox_crc: %ld words streamed over 80 sentences\n", before );
	CHECK( before >= 60 );
	disk_mode = DISK_POLLS;
	verbose = 1;
	CHECK( xb_t_crc_sentence( "#scientist/noidea(s30 e70) cascade(p130 t40)", &words, &streamed, &differ ));
	verbose = 0;
	CHECK( words == 2 && streamed == 2 && differ == 0 );
	if( !( words == 2 && streamed == 2 && differ == 0 ))
		printf( "  r3d_vox_crc: words %d streamed %d differ %d\n", words, streamed, differ );

	/* the verdict: SAME only when every sentence was compared in both
	   passes. A word only the stream's pass fails to load (kept from the
	   stream by an archive, its whole load failing in that pass only):
	   S_LoadSound puts its default sound in place of a failed load
	   (s_load.c), so the word plays a second of silence there - a
	   difference in its samples */
	where = "r3d_vox_crc verdict";
	{
		const char *one[] = { "#test/crcfail" }, *two[] = { "#test/crcfail", "no_such_sentence_xyz" };
		char line[512];
		int load_fail, skipped, unloadable;

		write_wav( synth, "crcfail.wav", 11025, 20000, -1, 0 );
		strcpy( archived, "crcfail" );
		CHECK( xb_t_crc_command( one, 1, line, sizeof( line ), &load_fail, &skipped, &unloadable ));
		CHECK( strstr( line, "- SAME;" ) && load_fail == 0 && skipped == 0 ); /* the control: it loads in both */
		strcpy( fs_fail_in_pass, "crcfail" );
		fs_fail_pass = 1;
		CHECK( !xb_t_crc_command( one, 1, line, sizeof( line ), &load_fail, &skipped, &unloadable ));
		CHECK( !strstr( line, "SAME" ) && strstr( line, "DIFFERS" ));
		if( strstr( line, "SAME" ))
			printf( "  r3d_vox_crc, the stream's load failing: %s", line );
		/* the same load failing in both passes: upstream's own failure
		   (its default sound in both), compared */
		fs_fail_pass = -1;
		CHECK( xb_t_crc_command( one, 1, line, sizeof( line ), &load_fail, &skipped, &unloadable ));
		CHECK( load_fail == 0 && skipped == 0 );
		fs_fail_in_pass[0] = 0;
		archived[0] = 0;
		/* a sentence that cannot be compared at all is skipped, not SAME */
		CHECK( !xb_t_crc_command( two, 2, line, sizeof( line ), &load_fail, &skipped, &unloadable ));
		CHECK( !strstr( line, "SAME" ) && strstr( line, "INCOMPLETE" ) && skipped == 1 && load_fail == 0 );
		if( strstr( line, "SAME" ) || skipped != 1 )
			printf( "  r3d_vox_crc, a skipped sentence: %s", line );
		/* the stream's pass building no words while upstream's did (the
		   branch that once returned SAME) */
		CHECK( !xb_t_crc_loads( 1, 0, 3, 0, -1, -1, line, sizeof( line ), &load_fail ));
		CHECK( !strstr( line, "SAME" ) && load_fail == 1 );
		if( strstr( line, "SAME" ) || load_fail != 1 )
			printf( "  r3d_vox_crc, the stream's pass building no words: %s", line );
		CHECK( !xb_t_crc_loads( 0, 0, 0, 0, -1, -1, line, sizeof( line ), &load_fail ));
		CHECK( xb_t_crc_loads( 1, 1, 2, 2, -1, -1, line, sizeof( line ), &load_fail ) && strstr( line, "- SAME;" ));
		/* a word that exists but did not load (VOX_LoadWord with no data)
		   in one pass only: a load failure, in either direction */
		CHECK( !xb_t_crc_loads( 1, 1, 2, 1, -1, 1, line, sizeof( line ), &load_fail ) && load_fail == 1 );
		CHECK( !xb_t_crc_loads( 1, 1, 1, 2, 1, -1, line, sizeof( line ), &load_fail ) && load_fail == 1 );
		/* the same word in both: compared up to it, and reported */
		CHECK( xb_t_crc_loads( 1, 1, 1, 1, 1, 1, line, sizeof( line ), &load_fail ) && load_fail == 0 && strstr( line, "1 stopped at a word" ));
	}

	where = "scenarios end";
	CHECK( xb_t_slots( -1 ) == 0 );
}

#ifdef _WIN32
/* An undefined-behaviour trap (run.sh's -fsanitize-undefined-trap-on-error)
   is an illegal instruction with no message: say where it was, for
   addr2line -e <the test> <address>. */
static LONG WINAPI trap_where( EXCEPTION_POINTERS *e )
{
	fprintf( stderr, "xbox_vox_stream: exception %08lx at 0x%llx (addr2line -e <test> that address); last check at [%s]\n",
		(unsigned long)e->ExceptionRecord->ExceptionCode,
		(unsigned long long)((uintptr_t)e->ExceptionRecord->ExceptionAddress - (uintptr_t)GetModuleHandleA( NULL ) + 0x140000000ull ), where );
	return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int main( int argc, char **argv )
{
#ifdef _WIN32
	SetUnhandledExceptionFilter( trap_where );
#endif
	setvbuf( stdout, NULL, _IONBF, 0 );
	static short ring_up[RING_FRAMES * 2], ring_xb[RING_FRAMES * 2];
	const qboolean quick = argc > 3 && !strcmp( argv[3], "quick" );
	char synth[1024];
	int nsent, config;
	struct { int rate, lerp, disk, unit; const char *label; } configs[] =
	{
		{ 48000, 1, DISK_POLLS, 8192,  "48 kHz, s_lerping 1, reads land after polls" },
		{ 48000, 0, DISK_NOW,   8192,  "48 kHz, s_lerping 0, reads land at once" },
		{ 44100, 1, DISK_SLOW,  8192,  "44.1 kHz, s_lerping 1, slow reads" },
		{ 44100, 0, DISK_POLLS, 32768, "44.1 kHz, s_lerping 0, r3d_vox_unit 32768" },
	};

	if( argc < 3 )
	{
		printf( "usage: %s <retail valve dir> <scratch dir> [quick]\n", argv[0] );
		return 2;
	}
	roots[0] = argv[1];
	snprintf( synth, sizeof( synth ), "%s", argv[2] );
	roots[1] = synth;
	{
		char d[1100];

		snprintf( d, sizeof( d ), "%s/sound", synth );
		mkdir( d );
		snprintf( d, sizeof( d ), "%s/sound/test", synth );
		mkdir( d );
	}

	cls.state = ca_active;
	cls.key_dest = key_game;
	cl.servercount = 1;
	host.realtime = 1.0;
	up_t_snd()->buffer = (byte *)ring_up;
	xb_t_snd()->buffer = (byte *)ring_xb;
	up_t_snd()->samples = xb_t_snd()->samples = RING_FRAMES * 2;
	up_t_snd()->format.speed = xb_t_snd()->format.speed = 48000;

	up_t_init();
	xb_t_init();
	nsent = count_sentences();
	printf( "xbox_vox_stream: %d sentences in %s/sound/sentences.txt\n", nsent, argv[1] );
	if( nsent < 1000 && !quick )
	{
		printf( "FAIL: the retail sentences.txt was not found\n" );
		return 1;
	}

	for( config = 0; config < (int)( sizeof( configs ) / sizeof( configs[0] )); config++ )
	{
		char unit[16];

		where = configs[config].label;
		r3d_snd_dma_speed = configs[config].rate;
		up_t_snd()->format.speed = xb_t_snd()->format.speed = configs[config].rate;
		s_lerping.value = (float)configs[config].lerp;
		disk_mode = configs[config].disk;
		snprintf( unit, sizeof( unit ), "%d", configs[config].unit );
		xb_t_set_cvar( "r3d_vox_unit", unit );
		reset_both();
		crc_all = !quick;
		run_sentences( nsent, quick ? 37 : 1, configs[config].label, true );
		if( config == 0 )
			scenarios( synth );
	}

	/* r3d_vox_stream 0: the Xbox build is upstream's, and starts nothing */
	where = "r3d_vox_stream 0";
	xb_t_set_cvar( "r3d_vox_stream", "0" );
	reset_both();
	memset( &xbox_ft_work, 0, sizeof( xbox_ft_work ));
	run_sentences( nsent < 50 ? nsent : 50, 1, "r3d_vox_stream 0", false );
	xb_t_set_cvar( "r3d_vox_stream", "1" );

	up_t_shutdown();
	xb_t_shutdown();
	where = "the end";
	CHECK( xb_t_slots( -1 ) == 0 );
	CHECK( xb_t_slot_mem() == 0 );
	CHECK( xb_t_headers() == 0 );
	CHECK( bad_frees == 0 );
	CHECK( disk_unaligned == 0 );
	CHECK( disk_opens == disk_closes );
	CHECK( live_blocks == 0 ); /* every allocation of both builds, the stream's whole fallbacks included */
	printf( "xbox_vox_stream: disk reads %ld (%ld KiB), opens %ld; whole-file loads %ld KiB; notes: sound %d, vox %d (last: %s); "
		"live blocks at exit %ld\n", disk_reads, disk_bytes / 1024, disk_opens, fs_whole_bytes / 1024, notes[XBOX_FT_NOTE_SOUND], notes[XBOX_FT_NOTE_VOX],
		last_vox_note, live_blocks );
	printf( "xbox_vox_stream: #184's spans: %ld handed (%ld frames), %ld checked to follow on from the one before\n", feed_spans, feed_frames, feed_tiled );
	CHECK( feed_tiled > ( quick ? 100 : 10000 ));
	printf( "xbox_vox_stream: %d checks, %d failures\n", checks, failures );
	return failures ? 1 : 0;
}
