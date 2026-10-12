/*
xbox_snd_rate_cfg_host_test.c - r3d_snd_rate from the boot configs against the real config-exec order (divergence #173)
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
engine/platform/xbox/xbox_snd_rate_cfg.h's parser, then a model of the
boot the console ran (recorder session 115aa3a1):

- S_Init opens the device (CL_Init, engine/common/host.c:1311), then
  Host_InitCommon queues `userconfigd` (host.c:1401), which appends one
  `exec` per userconfig.d/<name>.cfg in order (cmd.c:1376-1382);
- `exec` inserts the file at the front of the command buffer (cmd.c:1243);
- `wait N` stops the buffer for N frames (Cmd_Wait_f, cmd.c:363-372);
- `map` loads the map after the frame (Host_SetNextState
  STATE_LOAD_LEVEL, host_state.c), and the client is in play some frames
  later;
- each frame SND_UpdateSound runs S_R3D_ApplyRate, which reopens the
  device for a changed r3d_snd_rate until the game first reaches a map.

With 00-c1a0.cfg's waits around its `map`, 96-rate44.cfg's setting runs
after the map is up: the rule alone leaves the device at 48 kHz (the bug,
reproduced), and the boot read (the parser over the same files, at
S_Init) opens it at 44.1 kHz with no reopen at all. The listening-test
doc's single 95-mix48-ab.cfg works both ways.

  gcc -O2 -Wall -std=c99 -Iengine/platform/xbox tools/host-tests/xbox_snd_rate_cfg_host_test.c -o snd_rate_cfg_test && ./snd_rate_cfg_test
*/

#include <stdio.h>
#include <string.h>
#include "xbox_snd_rate_cfg.h"

static int failures, checks;
#define CHECK( cond ) do { checks++; if( !( cond )) { failures++; printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); } } while( 0 )

static int parse( const char *text )
{
	int rate = -1;

	return Xbox_SndRateCfg_Parse( text, &rate ) ? rate : -1;
}

static void test_parser( void )
{
	CHECK( parse( "r3d_snd_rate 44100\n" ) == 44100 );
	CHECK( parse( "r3d_snd_rate 44100" ) == 44100 );                   /* no final newline */
	CHECK( parse( "R3D_SND_RATE 44100\r\n" ) == 44100 );                /* any case, CRLF */
	CHECK( parse( "  \tr3d_snd_rate   \"44100\"\n" ) == 44100 );        /* blanks, a quoted value */
	CHECK( parse( "set r3d_snd_rate 44100\n" ) == 44100 );
	CHECK( parse( "seta r3d_snd_rate 44100\n" ) == 44100 );
	CHECK( parse( "sv_cheats 1; r3d_snd_rate 44100; map c1a0c\n" ) == 44100 );
	CHECK( parse( "r3d_snd_rate 44100\nr3d_snd_rate 48000\n" ) == 48000 ); /* the last one wins */
	CHECK( parse( "r3d_snd_rate 44100 // the debug rate\n" ) == 44100 );
	CHECK( parse( "// r3d_snd_rate 44100\n" ) == -1 );                  /* commented out */
	CHECK( parse( "sv_cheats 1 // r3d_snd_rate 44100\nmap x\n" ) == -1 );
	CHECK( parse( "alias ab44 \"r3d_snd_rate 44100; echo x\"\n" ) == -1 ); /* inside an alias */
	CHECK( parse( "bind DPAD_UP \"r3d_snd_rate 44100\"\n" ) == -1 );
	CHECK( parse( "echo \"a;b\"; r3d_snd_rate 44100\n" ) == 44100 );      /* ';' inside quotes is not a split */
	CHECK( parse( "r3d_snd_rate\n" ) == -1 );                           /* no value */
	CHECK( parse( "r3d_snd_rate_x 44100\nmy_r3d_snd_rate 44100\n" ) == -1 );
	CHECK( parse( "" ) == -1 );
	CHECK( parse( "echo \"unterminated\nr3d_snd_rate 44100\n" ) == 44100 ); /* a quote ends with its line */
}

/* ---- the boot, modelled ---- */

#define MAX_CMDS 256

typedef struct { const char *name, *text; } cfgfile_t;

typedef struct
{
	char cmd[MAX_CMDS][128];
	int n;
	int wait;
	int map_pending, map_active_in, in_map; /* a map queued; frames until in play; in play */
	int cvar, device_rate, reopens, latched, refused;
	const cfgfile_t *files;
	int nfiles;
} boot_t;

static void push_front( boot_t *b, const char *text )
{
	char lines[MAX_CMDS][128];
	int k = 0, i;
	const char *s = text;

	while( *s && k < MAX_CMDS )
	{
		const char *e = s;
		int len;

		while( *e && *e != '\n' && *e != ';' )
			e++;
		len = (int)( e - s );
		if( len > 127 )
			len = 127;
		memcpy( lines[k], s, (size_t)len );
		lines[k][len] = 0;
		if( len )
			k++;
		s = *e ? e + 1 : e;
	}
	memmove( b->cmd[k], b->cmd[0], sizeof( b->cmd[0] ) * (size_t)b->n );
	for( i = 0; i < k; i++ )
		strcpy( b->cmd[i], lines[i] );
	b->n += k;
}

static void push_back( boot_t *b, const char *line )
{
	strcpy( b->cmd[b->n++], line );
}

static const char *find_file( boot_t *b, const char *name )
{
	int i;

	for( i = 0; i < b->nfiles; i++ )
		if( !strcmp( b->files[i].name, name ))
			return b->files[i].text;
	return NULL;
}

/* Cbuf_Execute: until the buffer is empty or a wait */
static void execute( boot_t *b )
{
	while( b->n && !b->wait )
	{
		char line[128];
		int rate;

		strcpy( line, b->cmd[0] );
		memmove( b->cmd[0], b->cmd[1], sizeof( b->cmd[0] ) * (size_t)( b->n - 1 ));
		b->n--;
		if( !strncmp( line, "userconfigd", 11 ))
		{
			int i;
			char e[128];

			for( i = 0; i < b->nfiles; i++ )
			{
				snprintf( e, sizeof( e ), "exec %s", b->files[i].name );
				push_back( b, e );
			}
		}
		else if( !strncmp( line, "exec ", 5 ))
		{
			const char *t = find_file( b, line + 5 );

			if( t )
				push_front( b, t );
		}
		else if( !strncmp( line, "wait", 4 ))
			b->wait = atoi( line + 4 ) > 0 ? atoi( line + 4 ) : 1;
		else if( !strncmp( line, "map ", 4 ))
			b->map_pending = 1;
		else if( Xbox_SndRateCfg_Parse( line, &rate ))
			b->cvar = rate;
	}
}

/* S_R3D_ApplyRate's rule, once a frame */
static void apply_rate( boot_t *b )
{
	if( b->in_map )
		b->latched = 1;
	if( b->cvar == b->device_rate )
		return;
	if( b->latched )
	{
		b->refused = 1;
		return;
	}
	b->device_rate = b->cvar;
	b->reopens++;
}

static void boot( boot_t *b, const cfgfile_t *files, int nfiles, int boot_read, int frames )
{
	int f;

	memset( b, 0, sizeof( *b ));
	b->files = files;
	b->nfiles = nfiles;
	b->cvar = 48000;
	if( boot_read )
	{
		/* S_Init: S_R3D_ReadBootRate over the same files, in order */
		int i, rate;

		for( i = 0; i < nfiles; i++ )
			if( Xbox_SndRateCfg_Parse( files[i].text, &rate ))
				b->cvar = rate;
	}
	b->device_rate = b->cvar; /* SNDDMA_Init */
	push_back( b, "userconfigd" );
	execute( b );              /* Host_InitCommon's Cbuf_Execute */
	for( f = 0; f < frames; f++ )
	{
		if( b->wait )
			b->wait--;
		execute( b );      /* Host_ClientBegin's Cbuf_Execute */
		apply_rate( b );   /* SND_UpdateSound */
		if( b->map_pending )
		{
			b->map_pending = 0; /* loaded after the frame; in play a few frames later */
			b->map_active_in = 3;
		}
		else if( b->map_active_in && !--b->map_active_in )
			b->in_map = 1;
	}
}

static void test_boot_order( void )
{
	/* the console's: 00-c1a0.cfg with waits around its map, then the rate */
	static const cfgfile_t owner[] =
	{
		{ "userconfig.d/00-c1a0.cfg", "wait 200\nsv_cheats 1\nmap c1a0c\nwait 300\nimpulse 101\n" },
		{ "userconfig.d/96-rate44.cfg", "r3d_snd_rate 44100\n" },
	};
	/* no wait after the map: the rule alone reopens right before the load */
	static const cfgfile_t nowait[] =
	{
		{ "userconfig.d/00-c1a0.cfg", "wait 200\nsv_cheats 1\nmap c1a0c\n" },
		{ "userconfig.d/96-rate44.cfg", "r3d_snd_rate 44100\n" },
	};
	/* the listening-test doc's procedure: the menu-only 00-c1a0.cfg and its one cfg */
	static const cfgfile_t doc[] =
	{
		{ "userconfig.d/00-c1a0.cfg", "// boots to the main menu\n" },
		{ "userconfig.d/95-mix48-ab.cfg", "// X12 step 2 listening test\nr3d_snd_rate 44100\n"
			"alias lt1 \"map c0a0; alias lt_next lt2\"\nbind DPAD_RIGHT \"lt_next\"\nlt1\n" },
	};
	/* the doc's other boot: the line commented out */
	static const cfgfile_t doc48[] =
	{
		{ "userconfig.d/00-c1a0.cfg", "// boots to the main menu\n" },
		{ "userconfig.d/95-mix48-ab.cfg", "// r3d_snd_rate 44100\nalias lt1 \"map c0a0\"\nlt1\n" },
	};
	/* a later file sets it back: the last setting wins, as the buffer does */
	static const cfgfile_t two[] =
	{
		{ "userconfig.d/50-a.cfg", "r3d_snd_rate 44100\n" },
		{ "userconfig.d/60-b.cfg", "r3d_snd_rate 48000\n" },
	};
	boot_t b;

	/* the bug: the rule alone, the owner's configs - 48 kHz, the change refused */
	boot( &b, owner, 2, 0, 1000 );
	CHECK( b.device_rate == 48000 && b.cvar == 44100 && b.refused && b.reopens == 0 );
	/* the fix: read at boot - 44.1 kHz from the start, never a reopen or a refusal */
	boot( &b, owner, 2, 1, 1000 );
	CHECK( b.device_rate == 44100 && b.reopens == 0 && !b.refused );

	boot( &b, nowait, 2, 0, 1000 );
	CHECK( b.device_rate == 44100 && b.reopens == 1 ); /* a reopen just before the load */
	boot( &b, nowait, 2, 1, 1000 );
	CHECK( b.device_rate == 44100 && b.reopens == 0 );

	/* the doc's two boots work with or without the boot read; with it, no reopen */
	boot( &b, doc, 2, 0, 1000 );
	CHECK( b.device_rate == 44100 && !b.refused );
	boot( &b, doc, 2, 1, 1000 );
	CHECK( b.device_rate == 44100 && b.reopens == 0 && !b.refused );
	boot( &b, doc48, 2, 1, 1000 );
	CHECK( b.device_rate == 48000 && b.reopens == 0 && !b.refused );

	boot( &b, two, 2, 1, 100 );
	CHECK( b.device_rate == 48000 && b.reopens == 0 );
}

int main( void )
{
	test_parser();
	test_boot_order();
	printf( "snd_rate_cfg: %d checks, %d failures\n", checks, failures );
	return failures != 0;
}
