/*
xbox_vox_stream_host_test_tu.c - one build of the engine's sound code for xbox_vox_stream_host_test.c
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
tools/host-tests/run.sh compiles this twice: VOX_TEST_XBOX 0 is upstream's
sound code (every name prefixed up_ by vox_stub/common.h), 1 the Xbox
build with divergence #177's stream (xb_). Both are the real files:
s_vox.c, s_mix.c, s_mouth.c, s_load.c, snd_wav.c, snd_main.c, and from
s_main.c (which pulls in the rest of the engine) the functions the
sentences, the channels and save/restore run through, extracted verbatim
by extract_c_functions.py into s_main_part.c (run.sh's list). What
s_main.c defines around them and the test does not exercise - the
spatialisation, the DMA buffer clear, the fade state - is below.
*/

#include "common.h"
#include "sound.h"
#include "client.h"
#include "soundlib.h"
#if !VOX_TEST_XBOX
/* sound.h fixes upstream's mix rate at 44.1 kHz; both builds read the
   test's (the Xbox one through r3d_snd_dma_speed, #173), so a missing
   word's default sound has the same length in both */
#undef SOUND_DMA_SPEED
extern int r3d_snd_dma_speed;
#define SOUND_DMA_SPEED r3d_snd_dma_speed
#endif
#include "platform/xbox/xbox_frametime.h"
#if VOX_TEST_XBOX
#include "platform/xbox/xbox_vox_stream.h"
#include "platform/xbox/xbox_snd_voices.h" /* #182: S_FreeChannel's and S_FreeSound's calls (stubbed, never on) */
#endif

/* ---- what s_main.c defines around the extracted functions ---- */

snd_globals_t snd =
{
	.channels         = (channel_t[MAX_CHANNELS]){},
	.max_channels     = MAX_CHANNELS,
	.raw_channels     = (rawchan_t*[MAX_RAW_CHANNELS]){},
	.max_raw_channels = MAX_RAW_CHANNELS,
};
static struct { int percent; } soundfade;
static cl_entity_t vox_entities[64];

cl_entity_t *CL_GetEntityByIndex( int index )
{
	return index >= 0 && index < 64 ? &vox_entities[index] : NULL;
}

/* SND_Spatialize's listener branch (s_main.c:554-571 in the reference): full volume, then
   the word's own (VOX_SetChanVol) - the same in both builds */
static void SND_Spatialize( channel_t *ch )
{
	ch->leftvol = ch->master_vol;
	ch->rightvol = ch->master_vol;
	VOX_SetChanVol( ch );
}

static void S_ClearBuffer( void )
{
}

/* soundlib's format table (engine/common/soundlib/snd_utils.c) for this
   build: the WAV loader first, as the engine's */
static const loadwavfmt_t vox_load_formats[] =
{
	{ "wav", Sound_LoadWAV },
	{ NULL, NULL },
};
sndlib_t sound = { .loadformats = vox_load_formats };

#include "s_main_part.c"
#include "s_vox.c"
/* s_mix.c's mixing loops are the Xbox's in both builds (#145's integer
   pitch loop, #173's 48 kHz step, #174's s_lerping loop: not bit-equal to
   upstream's at 48 kHz, and checked by s_mix_host_test.c against their
   own claims), so the comparison isolates #177. In the upstream build its
   two #177 lines are upstream's expressions: no channel there holds a
   window (engine_reserved[0] is never set, so S_R3D_ChannelSource reads
   sfx->cache) and the mixer's source is S_LoadSound, below. */
#undef XASH_XBOX
#define XASH_XBOX 1
#include "s_mix.c"
#undef XASH_XBOX
#define XASH_XBOX VOX_TEST_XBOX
#if !VOX_TEST_XBOX
wavdata_t *S_R3D_VoxMixSource( channel_t *ch )
{
	return S_LoadSound( ch->sfx );
}
#endif
#include "s_mouth.c"
#include "s_load.c"
#include "snd_wav.c"
#include "snd_main.c"
#if VOX_TEST_XBOX
#include "xbox_vox_stream.c"
#endif

/* ---- the test's view of this build ---- */

snd_globals_t *VOX_NAME( t_snd )( void )
{
	return &snd;
}

int VOX_NAME( t_timeleft )( const channel_t *ch )
{
	return SND_GetChannelTimeLeft( ch );
}

/* What SND_UpdateSound does before it mixes (s_main.c): respatialise every
   channel (the word's volume), then the frame's stream work (#177). */
void VOX_NAME( t_frame )( void )
{
	int i;

	for( i = NUM_AMBIENTS; i < snd.total_channels; i++ )
	{
		if( snd.channels[i].sfx )
			SND_Spatialize( &snd.channels[i] );
	}
#if VOX_TEST_XBOX
	S_R3D_VoxFrame();
#endif
}

void VOX_NAME( t_init )( void )
{
	memset( vox_entities, 0, sizeof( vox_entities ));
	snd.initialized = true;
	S_StopAllSounds( true );
	S_InitSounds();
	VOX_Init();
#if VOX_TEST_XBOX
	S_R3D_VoxInit();
#endif
}

void VOX_NAME( t_shutdown )( void )
{
	S_StopAllSounds( false );
	S_FreeSounds();
	VOX_Shutdown();
	snd.initialized = false;
}

#if VOX_TEST_XBOX
/* the stream's own state, read by the scenarios */
int xb_t_slots( int state )
{
	int i, n = 0;

	for( i = 0; i < R3D_VOX_SLOTS; i++ )
		n += state < 0 ? vox.slot[i].state != VOX_FREE : vox.slot[i].state == state;
	return n;
}

int xb_t_slot_mem( void )
{
	int i, n = 0;

	for( i = 0; i < R3D_VOX_SLOTS; i++ )
		n += vox.slot[i].mem != NULL;
	return n;
}

int xb_t_headers( void )
{
	int i, n = 0;
	vox_hdr_t *h;

	for( i = 0; i < VOX_HASH; i++ )
		for( h = vox.hash[i]; h; h = h->next )
			n++;
	return n;
}

/* Whether ch's window holds the unit of its position (true for a channel
   that does not stream, or plays a whole fallback): S_R3D_VoxSeek's job
   after S_RestoreSound. */
int xb_t_window_ready( const channel_t *ch )
{
	vox_slot_t *s = Vox_FromToken( ch->engine_reserved[0] );
	uint32_t b0, k;

	if( !s || s->whole || !s->hdr || ch->sample >= HDR_WAV( s->hdr )->samples )
		return 1;
	b0 = s->hdr->data_off + (uint32_t)ch->sample;
	k = b0 / s->unit;
	return k >= s->k0 && k < s->k0 + R3D_VOX_UNITS && s->b[k - s->k0].state == BUF_READY;
}

void xb_t_set_cvar( const char *name, const char *value )
{
	convar_t *v = !Q_strcmp( name, "r3d_vox_stream" ) ? &r3d_vox_stream : !Q_strcmp( name, "r3d_vox_unit" ) ? &r3d_vox_unit : &r3d_vox_dirs;

	Cvar_DirectSet( v, value );
}

/* r3d_vox_crc's sentence check, as the console command runs it */
int xb_t_crc_sentence( const char *name, int *words, int *streamed, int *differ )
{
	vox_crc_stats_t st;
	int same;

	memset( &st, 0, sizeof( st ));
	same = Vox_CrcSentence( name, &st );
	*words = st.words;
	*streamed = st.streamed;
	*differ = st.differ;
	return same;
}

/* the pass r3d_vox_crc runs: 0 upstream's whole loads, 1 the stream, -1
   outside the command (the test's disk fails a load in one pass only) */
int xb_t_crc_mode( void )
{
	return vox.force;
}

/* r3d_vox_crc over a list of sentences, as the command runs `all`: its
   summary line, and whether its verdict is SAME */
int xb_t_crc_command( const char *const *names, int n, char *summary, int size, int *load_fail, int *skipped, int *unloadable )
{
	vox_crc_stats_t st;
	int i;

	memset( &st, 0, sizeof( st ));
	for( i = 0; i < n; i++ )
		Vox_CrcSentence( names[i], &st );
	Vox_CrcSummary( &st, summary, size );
	*load_fail = st.load_fail;
	*skipped = st.skipped;
	*unloadable = st.unloadable;
	return !Q_strcmp( Vox_CrcVerdict( &st ), "SAME" );
}

/* r3d_vox_crc's judgement of two passes given directly: a stream pass that
   built no words cannot be forced on the host (VOX_LoadSound's word list
   does not depend on the mode), so its branch is driven here */
int xb_t_crc_loads( int built0, int built1, int count0, int count1, int stop0, int stop1, char *summary, int size, int *load_fail )
{
	vox_crc_stats_t st;
	vox_crc_pass_t pass[2] = { { built0, count0, stop0 }, { built1, count1, stop1 } };

	memset( &st, 0, sizeof( st ));
	Vox_CrcLoads( "test", pass, &st );
	Vox_CrcSummary( &st, summary, size );
	*load_fail = st.load_fail;
	return !Q_strcmp( Vox_CrcVerdict( &st ), "SAME" );
}
#endif
