/*
music_mp3_decode.h - a retail music track through the engine's own libmpg
Resonance3D (docs/r3d/divergences.md #171)

The baker decodes with engine/client/soundlib/libmpg, compiled into it
from the engine's tree, driven with the calls Stream_OpenMPG and
Stream_ReadMPG make (`snd_mp3.c:306-398` in the reference): create_decoder,
open_mpeg_stream over read/seek callbacks, then read_mpeg_stream until it
stops reporting MP3_OK. What comes out is what the console's mp3 path
decodes, up to the floating-point library (libmpg builds its tables with
sin/cos/pow, `layer3.c`, `tabinit.c`), which the console check
(`r3d_music_crc`) measures.
*/
#ifndef MUSIC_MP3_DECODE_H
#define MUSIC_MP3_DECODE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSIC_MP3_SEEK_PROBES 12

typedef struct music_mp3_s
{
	int rate;
	int channels;
	int16_t *pcm;              /* frames * channels, interleaved; free with Music_Mp3Free */
	uint32_t frames;           /* everything libmpg produced, the short last chunk included */
	uint32_t chunk_bytes;      /* OUTBUF_SIZE, the size every MP3_OK read returned */
	uint32_t ok_chunks;        /* reads that returned MP3_OK */
	uint32_t tail_bytes;       /* what the last, failing read returned */
	uint32_t short_ok_reads;   /* MP3_OK reads shorter than chunk_bytes: must be 0 for the stream model */
	uint32_t tell_mismatches;  /* get_stream_pos after an MP3_OK read but the last != frames handed out: must be 0 */
	int open_tell;             /* get_stream_pos before the first read: must be 0 */
	int last_ok_tell;          /* get_stream_pos after the last MP3_OK read */
	int last_ok_tell_differs;  /* ... and it is not the frames handed out there */
	int done_tell;             /* get_stream_pos after the read that failed: must be `frames` */
	uint32_t tell_first_chunk; /* the first of them: which MP3_OK read (1-based) */
	uint32_t tell_first_value; /* and what get_stream_pos said there */

	/* libmpg's seek (Stream_SetPosMPG) at a spread of positions: is the
	   chunk read after it the decode's own frames from there? */
	uint32_t seek_probes;
	uint32_t seek_exact;
	uint32_t seek_tell_exact;  /* get_stream_pos right after the seek == the target */
	int32_t seek_worst_offset; /* frames between the target and where the data matched, worst probe; 0 if exact */
} music_mp3_t;

/* Returns 1 and fills `out`, or 0 with a message in `err`. */
int Music_Mp3Decode( const char *path, music_mp3_t *out, char *err, size_t err_size );
void Music_Mp3Free( music_mp3_t *m );

/* Upstream's mp3 stream itself, for the tests: Stream_OpenMPG,
   Stream_ReadMPG, Stream_SetPosMPG and Stream_GetPosMPG (`snd_mp3.c:306-430`
   in the reference) copied line for line over stdio, so the baked
   stream's replay can be compared with the real thing request by
   request. */
typedef struct music_mp3_stream_s music_mp3_stream_t;
music_mp3_stream_t *Music_Mp3StreamOpen( const char *path, int *rate, int *channels );
int Music_Mp3StreamRead( music_mp3_stream_t *s, int needBytes, void *buffer );
int Music_Mp3StreamSetPos( music_mp3_stream_t *s, int newpos );
int Music_Mp3StreamGetPos( music_mp3_stream_t *s );
void Music_Mp3StreamClose( music_mp3_stream_t *s );

#ifdef __cplusplus
}
#endif

#endif
