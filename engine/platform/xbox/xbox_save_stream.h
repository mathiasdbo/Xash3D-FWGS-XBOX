/*
xbox_save_stream.h - the bytes of a savegame's .sav, produced a chunk at a time
Resonance3D (docs/r3d/divergences.md #153)

Upstream's SaveGameSlot (`sv_save.c:1692-1762` in the reference) writes
the whole .sav in the frame the game is saved: the header, the token
table and the game globals, then DirectoryCopy (`:639-660`) appends
every <map>.HL? file of the chapter - a 260-byte name (MAX_OSPATH,
zero-filled), the file's length as a 4-byte int, the file's bytes. The
.HL? files are most of it, and the copy grows with the chapter.

This is that byte sequence as a producer the engine drains over several
frames: each fill puts the next bytes into a caller-provided chunk, and
the engine writes the chunk out (sv_save.c, SaveAsync_*). It has no
engine dependency - the source files are reached through callbacks - so
the host test (tools/host-tests/xbox_save_stream_host_test.c) checks the
bytes against a straight concatenation.

File bytes are read on an `align` boundary of the chunk and moved down
into place, as #151's SaveCopy does: the Xbox kernel reads about three
times faster into a page-aligned buffer (xemu). The chunk itself must
start on such a boundary.
*/
#ifndef XBOX_SAVE_STREAM_H
#define XBOX_SAVE_STREAM_H

#include <stddef.h>
#include <stdint.h>

/* DirectoryCopy's per-file record: szName[MAX_OSPATH] then int fileSize. */
#define XBOX_SAVE_STREAM_NAME_BYTES   260u
#define XBOX_SAVE_STREAM_RECORD_BYTES ( XBOX_SAVE_STREAM_NAME_BYTES + 4u )

typedef struct xbox_save_stream_io_s
{
	/* Opens source `index` (0 .. count-1, in order, each once). Writes
	   its name into `name` (zero-filled by the caller, `name_size`
	   bytes; the last byte is forced to zero after the call) and its
	   length into `*length`, and returns its handle - NULL for a file
	   that would not open, whose length is then 0 (upstream's
	   FS_FileLength( NULL ), io.c). */
	void *( *open )( void *ctx, uint32_t index, char *name, size_t name_size, int32_t *length );
	/* Reads up to `size` bytes; returns how many were read. */
	int32_t ( *read )( void *ctx, void *handle, void *dst, int32_t size );
	/* Closes a handle open returned (never called with NULL). */
	void ( *close )( void *ctx, void *handle );
	void *ctx;
} xbox_save_stream_io_t;

typedef struct xbox_save_stream_s
{
	xbox_save_stream_io_t io;

	const uint8_t *head;  /* the bytes before the files (header, tokens, globals) */
	uint32_t head_len;
	uint32_t head_pos;

	uint32_t count;       /* source files */
	uint32_t next;        /* the next one to open */

	uint8_t  record[XBOX_SAVE_STREAM_RECORD_BYTES]; /* the open file's name and length */
	uint32_t record_pos;  /* XBOX_SAVE_STREAM_RECORD_BYTES: drained (or none yet) */

	void    *in;          /* the open source; NULL with in_open set: it did not open */
	int      in_open;
	int32_t  in_left;     /* its bytes not yet read */

	uint8_t *chunk;       /* starts on an `align` boundary */
	uint32_t cap;         /* chunk bytes */
	uint32_t len;         /* bytes in the chunk, to be written */
	uint32_t align;       /* power of two, >= 1 */

	uint32_t opens;       /* sources opened by the last fill */
	int      failed;      /* a source read fell short: the .sav would be corrupt */
	int      done;        /* every byte produced (the chunk may still hold some) */
} xbox_save_stream_t;

void xbox_save_stream_init( xbox_save_stream_t *s, const xbox_save_stream_io_t *io,
	const void *head, uint32_t head_len, uint32_t count,
	void *chunk, uint32_t cap, uint32_t align );

/* Adds the next bytes to the chunk and returns how many it holds. Stops
   when the chunk is full (or the next file read would not start on an
   aligned offset inside it), after `max_opens` sources were opened by
   this call (0: no limit), at the end (done), or on a short read
   (failed). The caller writes chunk[0 .. len) and calls
   xbox_save_stream_consumed. */
uint32_t xbox_save_stream_fill( xbox_save_stream_t *s, uint32_t max_opens );

/* The chunk was written: the next fill starts at its beginning. */
void xbox_save_stream_consumed( xbox_save_stream_t *s );

/* Closes the open source, if any (the end of the stream, or giving up). */
void xbox_save_stream_close( xbox_save_stream_t *s );

#endif /* XBOX_SAVE_STREAM_H */
