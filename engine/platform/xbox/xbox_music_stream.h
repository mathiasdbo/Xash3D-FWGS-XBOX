/*
xbox_music_stream.h - baked music tracks (.xwav) as an engine stream
Resonance3D (docs/r3d/divergences.md #171)

FS_OpenStream (engine/client/soundlib/snd_main.c) asks
Xbox_MusicStream_Open first, under #if XASH_XBOX; NULL sends it on to
upstream's own formats unchanged.
*/
#ifndef XBOX_MUSIC_STREAM_H
#define XBOX_MUSIC_STREAM_H

/* Registers r3d_music_baked and the r3d_music_crc / r3d_music_ab
   commands. Called from Xbox_FrameTime_Init, before the configs run. */
void Xbox_Music_Init( void );

/* The hook: a stream over <filename without its stream extension>.xwav,
   or NULL. snd_main.c declares it at its one call site. */
struct stream_s *Xbox_MusicStream_Open( const char *filename );

/* The baked stream behind `stream` when it hands its blocks to the music
   voice (#183) instead of decoding them, else NULL */
struct xbox_music_stream_s *Xbox_MusicStream_Sunk( struct stream_s *stream );

#endif /* XBOX_MUSIC_STREAM_H */
