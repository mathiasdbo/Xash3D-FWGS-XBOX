/*
xbox_music_adpcm.h - baked music tracks (.xwav): the Xbox ADPCM block
decoder, the container header, and the stream that replays upstream's
mp3 reader on top of them
Resonance3D (docs/r3d/divergences.md #171)

Upstream plays music by decoding the retail mp3 with libmpg every frame
(Stream_ReadMPG, `snd_mp3.c:361-398` in the reference), which costs
0.44-0.48 ms a frame on the console (k_snd_music, route-pr193).
tools/asset-studio decodes each track offline with the engine's own
libmpg and stores the result as Xbox ADPCM (or 16-bit PCM) in a RIFF
file next to the mp3, `media/<Track>.xwav`; xbox_music_stream.c opens it
in place of the mp3.

This file has no engine dependency: the host test
(tools/host-tests/xbox_music_adpcm_host_test.c) covers the tables, the
block decode, the header parser and the whole stream state machine over
an in-memory file, and the baker compiles it to encode with this
decoder in the loop.

THE BLOCK FORMAT (WAVE_FORMAT_XBOX_ADPCM, 0x0069), per channel 36 bytes:
a 4-byte header - int16 sample, u8 step index 0..88, u8 zero - then 32
bytes of 4-bit IMA codes, low nibble first. Stereo interleaves the 32
bytes in 4-byte groups, channel 0 first. A block is 64 sample frames:
the header sample is frame 0 and the first 63 codes are frames 1..63;
the 64th code is not played. That is what the console's APU does as far
as public sources show: xemu's VP decodes the block with ADPCM-XQ's
`adpcm_decode_block` (header first, `hw/xbox/mcpx/apu/vp/adpcm.h:85-86`)
and advances 64 frames a block (`ADPCM_SAMPLES_PER_BLOCK 64`,
`apu_regs.h:336`, used at `vp/vp.c:1015-1016`), and vgmstream's
`decode_xbox_ima` ("unlike MS-IMA must skip the last nibble",
`src/coding/ima_decoder.c:740-745`) reads retail game audio the same
way. A later APU path can therefore play these files as they are.
*/
#ifndef XBOX_MUSIC_ADPCM_H
#define XBOX_MUSIC_ADPCM_H

#include <stddef.h>
#include <stdint.h>

#define XBOX_ADPCM_BLOCK_FRAMES        64u  /* sample frames per block */
#define XBOX_ADPCM_CHANNEL_BYTES       36u  /* bytes per channel per block */
#define XBOX_ADPCM_MAX_INDEX           88

#define XBOX_XWAV_TAG_PCM              0x0001u
#define XBOX_XWAV_TAG_ADPCM            0x0069u
#define XBOX_XWAV_R3DM_VERSION         1u
#define XBOX_XWAV_R3DM_BYTES           48u
/* How much of the file the parser needs: the RIFF header, `fmt `, `r3dm`
   and the `data` chunk's header, as the baker writes them (well under
   this). */
#define XBOX_XWAV_HEADER_READ          512u

/* The 89-entry IMA step table (the same in xemu's adpcm.h and in the
   IMA ADPCM recommendation). */
extern const uint16_t xbox_adpcm_step_table[89];
/* Index adjustment by the code's three magnitude bits. */
extern const int8_t xbox_adpcm_index_table[8];

/* Builds the per-(index, code) tables the decoder reads. Idempotent;
   called by every entry point below that decodes. */
void Xbox_MusicAdpcm_InitTables( void );

/* One code through the reference shift-add reconstruction (the form
   xemu's adpcm_decode_block uses): updates *sample and *index. The table
   decoder must agree with it for every (index, code) pair. */
void Xbox_MusicAdpcm_StepRef( int32_t *sample, int32_t *index, unsigned code );

/* The same through the tables (what DecodeBlock uses). */
void Xbox_MusicAdpcm_Step( int32_t *sample, int32_t *index, unsigned code );

/* Decodes one block of `channels` (1 or 2) channels, 36 * channels bytes,
   into 64 interleaved frames. Returns 1, or 0 for a header the APU would
   refuse (index above 88 or a non-zero fourth byte - xemu's
   adpcm_decode_block returns 0 for both), in which case the 64 frames are
   written as silence. */
int Xbox_MusicAdpcm_DecodeBlock( const uint8_t *in, int channels, int16_t *out );

/* CRC-32 (the zlib/PNG polynomial, 0xEDB88320, reflected, ~0 in and
   out); `crc` is the running value, 0 to start. */
uint32_t Xbox_MusicCrc32( uint32_t crc, const void *data, size_t len );

/* The container. */
typedef struct xbox_xwav_info_s
{
	uint32_t tag;           /* XBOX_XWAV_TAG_ADPCM or _PCM */
	uint32_t channels;      /* 1 or 2 */
	uint32_t rate;          /* this file's sample rate */
	uint32_t block_bytes;   /* bytes per 64-frame block: 36*ch (ADPCM) or 128*ch (PCM) */
	uint32_t data_offset;   /* file offset of the first block */
	uint32_t data_bytes;

	/* r3dm, version 1 */
	uint32_t frames;        /* sample frames at `rate`: everything libmpg produced */
	uint32_t source_rate;   /* the mp3's own rate: positions are in these frames */
	uint32_t source_frames;
	uint32_t crc_source;    /* CRC-32 of libmpg's int16 output */
	uint32_t crc_decoded;   /* CRC-32 of this file's decode, `frames` frames */
	uint32_t chunk_bytes;   /* libmpg's output chunk, read_mpeg_stream's OUTBUF_SIZE */
	uint32_t source_bytes;  /* size of the file it was baked from */
	char source_ext[8];     /* that file's extension, NUL-terminated ("mp3") */
	uint32_t encoder_id;
	uint32_t resampler_id;
	/* mpg123_tell after the last whole chunk of a fresh open, when it is
	   not the frames handed out (its gapless end adjustment, measured by
	   the baker on the track); 0 when it is */
	uint32_t tell_last_chunk;
} xbox_xwav_info_t;

typedef enum
{
	XBOX_XWAV_OK = 0,
	XBOX_XWAV_ERR_SHORT,      /* truncated before a required chunk */
	XBOX_XWAV_ERR_MAGIC,      /* not RIFF/WAVE */
	XBOX_XWAV_ERR_FMT,        /* missing or malformed `fmt ` */
	XBOX_XWAV_ERR_CODEC,      /* a tag, channel count, rate or block layout this reader does not play */
	XBOX_XWAV_ERR_R3DM,       /* missing `r3dm`, or a version or field it does not accept */
	XBOX_XWAV_ERR_DATA,       /* missing `data`, or a length that disagrees with `frames` */
	XBOX_XWAV_ERR_COUNT
} xbox_xwav_err_t;

/* Parses the first `len` bytes of a file `file_len` bytes long. Every
   chunk the reader needs must start within `len`. A file whose rate is not
   its source's is refused for now: the stream replays libmpg's chunking
   only at the source rate, and no baker writes another (a 48 kHz rebake
   arrives with X12 step 2, with the replay worked out for it). */
xbox_xwav_err_t Xbox_MusicXwav_ParseHeader( const uint8_t *buf, size_t len, uint32_t file_len, xbox_xwav_info_t *out );
const char *Xbox_MusicXwav_ErrorString( xbox_xwav_err_t err );

/* Which file a stream request names, the way FS_OpenStream builds its
   candidates (`snd_main.c:166-181` in the reference): when the name's
   extension is one of `formats` (case-insensitive) it is stripped and that
   format is the only one tried; any other dot is part of the name. Writes
   the base name and returns the requested format's index, or -1 for "any
   format". */
int Xbox_MusicXwav_SplitName( const char *filename, const char *const *formats, int nformats, char *base, size_t base_size );

/* Answers whether a file exists and its size: 1 and *size, or 0. */
typedef int ( *xbox_music_probe_fn )( void *ctx, const char *path, int64_t *size );

typedef enum
{
	XBOX_XWAV_SOURCE_OK = 0,
	XBOX_XWAV_SOURCE_NONE,      /* upstream would find no file for the name */
	XBOX_XWAV_SOURCE_FORMAT,    /* upstream would open another format than the bake's source */
	XBOX_XWAV_SOURCE_SIZE       /* the file upstream would open is not the one baked (size) */
} xbox_xwav_source_t;

/* A .xwav is played only in place of the file upstream would open for the
   same request, and only if that file is the one it was baked from: the
   requested format when the name had one, else the first of `formats`
   that exists (upstream tries them in order, `snd_main.c:184-198`); its
   extension must be r3dm's source_ext and its size source_bytes. */
xbox_xwav_source_t Xbox_MusicXwav_CheckSource( const xbox_xwav_info_t *info, const char *base, int requested,
	const char *const *formats, int nformats, xbox_music_probe_fn probe, void *ctx );

/* Positions are in the source's frames (what Stream_GetPosMPG reports for
   the mp3), so a save written by either build restores on the other and
   a later bake at another rate keeps old saves. ToSourceFrames rounds
   down, ToFileFrames up, so source -> file -> source is exact whenever
   the file's rate is at least the source's. */
uint32_t Xbox_MusicXwav_ToSourceFrames( const xbox_xwav_info_t *info, uint32_t file_frames );
uint32_t Xbox_MusicXwav_ToFileFrames( const xbox_xwav_info_t *info, uint32_t source_frames );

/* THE STREAM. Upstream's Stream_ReadMPG asks libmpg's read_mpeg_stream
   for OUTBUF_SIZE-byte chunks; mpg123_read fills each one completely and
   reports MPG123_OK (`mpg123.c:607-680` in the reference), except the
   last, which is short and reports DONE - and on anything but OK
   Stream_ReadMPG returns 0 for the whole request (`snd_mp3.c:374-375`,
   `:397`). So the tail of each track, the short chunk plus whatever the
   failing request had already copied, is never played. The owner chose
   to keep that (2026-10-10), so this stream runs Stream_ReadMPG's own
   loop over a virtual decoder that hands out the baked frames in the
   same chunks: the bytes returned, the request that returns 0, and the
   position (mpg123_tell: frames handed out of the decoder, the chunk
   held in `temp` included) all match the mp3 path for the same PCM. */
typedef struct xbox_music_io_s
{
	/* Reads up to `bytes` at the current offset; returns the count read. */
	int ( *read )( void *ctx, void *dst, int bytes );
	/* Moves to absolute offset `offset`; returns 0 on success. */
	int ( *seek )( void *ctx, uint32_t offset );
	void *ctx;
} xbox_music_io_t;

/* The compressed read-ahead: 128 stereo ADPCM blocks, 186 ms at 44.1 kHz,
   refilled with one read. */
#define XBOX_MUSIC_READAHEAD_BYTES     9216u

typedef struct xbox_music_stream_s
{
	xbox_xwav_info_t info;
	xbox_music_io_t io;

	/* Stream_ReadMPG's state, by upstream's names: `chunk` is stream->temp
	   (chunk_bytes long, the caller's), `pos` the bytes left in it,
	   `buffsize` the offset of the next one. */
	uint8_t *chunk;
	int pos;
	int buffsize;

	uint32_t next_frame;    /* the virtual decoder's position: mpg123_tell, in file frames */
	int seeked;             /* SetPos was called: tell_last_chunk is a fresh open's only */
	int ended;              /* a chunk came back short: the decoder is at its end */
	uint32_t frame_bytes;   /* 2 * channels */
	uint32_t blocks;        /* blocks in `data` */

	/* The read-ahead: blocks [ra_first, ra_first + ra_count). */
	uint8_t *ra;            /* XBOX_MUSIC_READAHEAD_BYTES, the caller's */
	uint32_t ra_first;
	uint32_t ra_count;
	uint32_t ra_capacity;   /* blocks that fit */

	int16_t carry[XBOX_ADPCM_BLOCK_FRAMES * 2];  /* one decoded block, for a chunk that starts or ends inside it */

	uint32_t io_reads;      /* refills, for the recorder and the tests */
	uint32_t bad_blocks;    /* blocks DecodeBlock refused */
	int io_failed;          /* a read came back short: the track ends there */
} xbox_music_stream_t;

/* Sets up a stream over a parsed file. `chunk` is chunk_bytes long (the
   engine passes stream->temp), `readahead` XBOX_MUSIC_READAHEAD_BYTES.
   Returns 0 when chunk_bytes is not a positive multiple of the frame
   size, or exceeds `chunk_capacity`. */
int Xbox_MusicStream_Init( xbox_music_stream_t *s, const xbox_xwav_info_t *info, const xbox_music_io_t *io,
	uint8_t *chunk, uint32_t chunk_capacity, uint8_t *readahead );

/* Stream_ReadMPG's contract: `need` bytes into `buffer`, or 0. */
int Xbox_MusicStream_Read( xbox_music_stream_t *s, int need, void *buffer );

/* Stream_SetPosMPG: `newpos` in source frames; returns 1. */
int Xbox_MusicStream_SetPos( xbox_music_stream_t *s, int newpos );

/* Stream_GetPosMPG: in source frames. */
int Xbox_MusicStream_GetPos( const xbox_music_stream_t *s );

/* Decodes frames [first, first + count) of the file into `out`
   (interleaved int16), through the read-ahead. Returns the frames
   written: fewer when the data runs out or a read fails. The baker and
   the console check call it for whole-track CRCs. */
uint32_t Xbox_MusicStream_DecodeFrames( xbox_music_stream_t *s, uint32_t first, uint32_t count, int16_t *out );

#endif /* XBOX_MUSIC_ADPCM_H */
