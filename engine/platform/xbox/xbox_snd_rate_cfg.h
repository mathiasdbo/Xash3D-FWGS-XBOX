/*
xbox_snd_rate_cfg.h - r3d_snd_rate read from the boot configs before the sound device opens
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
Resonance3D (docs/r3d/divergences.md #173). The mixer's rate is chosen at
boot, but the configs that choose it run after the device opens:
S_Init (from CL_Init, engine/common/host.c:1311) opens it, and
Host_InitCommon only then queues config.cfg and `userconfigd`
(host.c:1387, :1401). `userconfigd` appends one `exec` per
userconfig.d/<name>.cfg in FS_Search's order (Cmd_Userconfigd_f,
engine/common/cmd.c:1376-1382), and each `exec` inserts its file at the
front of the command buffer (cmd.c:1243), so a later file runs only after
everything an earlier one queued - its `wait`s included. With a
00-c1a0.cfg of `wait 200`, `map c1a0c` and more after it, a
96-rate44.cfg's `r3d_snd_rate 44100` ran after the map was up, when a
change only applies at the next boot: the console booted at 48 kHz
(recorder session 115aa3a1).

So S_Init reads the setting from the files themselves, before the
device opens, the way the command buffer will later apply it: every
userconfig.d/<name>.cfg in FS_Search's order, each command in each file in
order, the last `r3d_snd_rate <n>` (or `set`/`seta r3d_snd_rate <n>`)
winning. Commands inside quotes (an alias, a bind) are not commands of
the file and are skipped, as are `//` comments. A setting the buffer would
only reach through an alias or a nested `exec` is not seen here; the
latch then still reports that it applies at the next boot.

Pure, so tools/host-tests/xbox_snd_rate_cfg_host_test.c can drive it.
*/
#ifndef XBOX_SND_RATE_CFG_H
#define XBOX_SND_RATE_CFG_H

#include <stdlib.h>

static inline int Xbox_SndRateCfg_Lower( int c )
{
	return c >= 'A' && c <= 'Z' ? c + ( 'a' - 'A' ) : c;
}

/* Copies the next token of the command [*p, end) into tok (quotes kept
   out, a quoted token whole); returns 0 at the command's end. */
static inline int Xbox_SndRateCfg_Token( const char **p, const char *end, char *tok, int size )
{
	const char *s = *p;
	int n = 0;

	while( s < end && ( *s == ' ' || *s == '\t' || *s == '\r' ))
		s++;
	if( s >= end )
		return 0;
	if( *s == '"' )
	{
		s++;
		while( s < end && *s != '"' )
		{
			if( n < size - 1 )
				tok[n++] = *s;
			s++;
		}
		if( s < end )
			s++;
	}
	else
	{
		while( s < end && *s != ' ' && *s != '\t' && *s != '\r' )
		{
			if( n < size - 1 )
				tok[n++] = *s;
			s++;
		}
	}
	tok[n] = 0;
	*p = s;
	return 1;
}

/* tok equals the lower-case `word`, ignoring case (cvar and command names
   are matched without case) */
static inline int Xbox_SndRateCfg_Is( const char *tok, const char *word )
{
	int i;

	for( i = 0; word[i]; i++ )
	{
		if( Xbox_SndRateCfg_Lower( (unsigned char)tok[i] ) != word[i] )
			return 0;
	}
	return tok[i] == 0;
}

/* One command [s, end): copies the value into val when it sets the cvar
   `name` (lower case) */
static inline int Xbox_SndCfg_Command( const char *s, const char *end, const char *name, char *val, int size )
{
	char tok[64];

	if( !Xbox_SndRateCfg_Token( &s, end, tok, sizeof( tok )))
		return 0;
	if(( Xbox_SndRateCfg_Is( tok, "set" ) || Xbox_SndRateCfg_Is( tok, "seta" ))
		&& !Xbox_SndRateCfg_Token( &s, end, tok, sizeof( tok )))
		return 0;
	if( !Xbox_SndRateCfg_Is( tok, name ) || !Xbox_SndRateCfg_Token( &s, end, val, size ))
		return 0;
	return 1;
}

/* A config file's text (NUL-terminated): returns 1 and the last value it
   sets the cvar `name` (lower case) to in val, or 0 when it does not set
   it. Commands end at a newline or a ';' outside quotes, as the command
   buffer splits them (cmd.c, Cbuf_ExecuteCommandsFromBuffer); `//` outside
   quotes starts a comment to the end of the line. r3d_snd_backend
   (#178) is read the same way as r3d_snd_rate. */
static inline int Xbox_SndCfg_Parse( const char *text, const char *name, char *val, int size )
{
	const char *s = text, *start = text;
	int quoted = 0, found = 0;

	for( ;; )
	{
		const char c = *s;

		if( c == '"' )
			quoted = !quoted;
		if( !c || c == '\n' || ( !quoted && c == ';' ) || ( !quoted && c == '/' && s[1] == '/' ))
		{
			found |= Xbox_SndCfg_Command( start, s, name, val, size );
			if( !quoted && c == '/' )
			{
				while( *s && *s != '\n' )
					s++; // the comment, to the end of the line
				if( !*s )
					break;
			}
			if( !*s )
				break;
			if( *s == '\n' )
				quoted = 0; // a quote never spans lines
			start = ++s;
			continue;
		}
		s++;
	}
	return found;
}

/* r3d_snd_rate's last value in a config file's text, as above */
static inline int Xbox_SndRateCfg_Parse( const char *text, int *rate )
{
	char val[64];

	if( !Xbox_SndCfg_Parse( text, "r3d_snd_rate", val, sizeof( val )))
		return 0;
	*rate = atoi( val );
	return 1;
}

#endif /* XBOX_SND_RATE_CFG_H */
