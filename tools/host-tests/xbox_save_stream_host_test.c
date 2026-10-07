/* Host test for engine/platform/xbox/xbox_save_stream.c (divergence
   #153): the .sav a deferred save writes a chunk per frame must be the
   bytes upstream's SaveGameSlot writes in one go - the header block, then
   for every <map>.HL? file a zero-filled 260-byte name, its length as an
   int and its bytes (DirectoryCopy, `sv_save.c:639-660` in the reference).

   The expected bytes are built straight from that description; the
   stream is drained with many chunk sizes, alignments and per-fill open
   limits, every file read is checked to land on an aligned offset of the
   chunk, every file to be closed exactly once, and a short read to stop
   the stream as failed. A file that does not open is written with length
   0 and no bytes, as upstream's FS_FileLength( NULL ) and FS_FileCopy.

   gcc -O2 -Wall -std=c99 -Iengine/platform/xbox \
       tools/host-tests/xbox_save_stream_host_test.c \
       engine/platform/xbox/xbox_save_stream.c -o xbox_save_stream_test */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xbox_save_stream.h"

static int failures;
#define CHECK( c ) do { if( !( c )) { printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c ); failures++; } } while( 0 )

#define MAX_FILES 48

typedef struct fake_file_s
{
	char     name[300];
	int32_t  length;
	uint8_t *data;
	int      missing;   /* open returns NULL */
	int      short_at;  /* > 0: reads stop delivering at this offset */
	int      pos;
	int      opened, closed;
} fake_file_t;

typedef struct fake_fs_s
{
	fake_file_t f[MAX_FILES];
	uint32_t    count;
	const uint8_t *chunk;
	uint32_t    align;
	int         misaligned_reads;
	int         order_errors;
	uint32_t    next_open;
} fake_fs_t;

static void *fake_open( void *ctx, uint32_t index, char *name, size_t name_size, int32_t *length )
{
	fake_fs_t *fs = ctx;
	fake_file_t *f = &fs->f[index];

	if( index != fs->next_open )
		fs->order_errors++;
	fs->next_open = index + 1;
	/* Q_strncpy( szName, COM_FileWithoutPath( name ), sizeof( szName )) */
	strncpy( name, f->name, name_size - 1 );
	f->opened++;
	if( f->missing )
		return NULL;
	*length = f->length;
	return f;
}

static int32_t fake_read( void *ctx, void *handle, void *dst, int32_t size )
{
	fake_fs_t *fs = ctx;
	fake_file_t *f = handle;
	int32_t n = size;

	if( (( (const uint8_t *)dst - fs->chunk ) % fs->align ) != 0 )
		fs->misaligned_reads++;
	if( f->pos + n > f->length )
		n = f->length - f->pos;
	if( f->short_at > 0 && f->pos + n > f->short_at )
		n = f->short_at - f->pos;
	if( n < 0 )
		n = 0;
	memcpy( dst, f->data + f->pos, n );
	f->pos += n;
	return n;
}

static void fake_close( void *ctx, void *handle )
{
	( (fake_file_t *)handle )->closed++;
	(void)ctx;
}

static uint32_t rng = 12345u;
static uint32_t next_rand( void )
{
	rng = rng * 1664525u + 1013904223u;
	return rng >> 8;
}

/* the bytes upstream writes */
static size_t expected_bytes( const fake_fs_t *fs, const uint8_t *head, uint32_t head_len, uint8_t *out )
{
	size_t n = 0;

	memcpy( out, head, head_len );
	n += head_len;
	for( uint32_t i = 0; i < fs->count; i++ )
	{
		const fake_file_t *f = &fs->f[i];
		char name[260];
		int32_t length = f->missing ? 0 : f->length;

		memset( name, 0, sizeof( name ));
		strncpy( name, f->name, sizeof( name ) - 1 );
		memcpy( out + n, name, 260 );
		n += 260;
		memcpy( out + n, &length, 4 );
		n += 4;
		if( length > 0 )
		{
			memcpy( out + n, f->data, length );
			n += length;
		}
	}
	return n;
}

static void make_fs( fake_fs_t *fs, uint32_t count, int with_missing, int long_names )
{
	memset( fs, 0, sizeof( *fs ));
	fs->count = count;
	for( uint32_t i = 0; i < count; i++ )
	{
		fake_file_t *f = &fs->f[i];
		uint32_t kind = next_rand() % 4;

		if( long_names && i % 5 == 0 )
		{
			memset( f->name, 'n', 280 ); /* longer than the 260-byte field: cut at 259 */
			f->name[280] = 0;
		}
		else
			snprintf( f->name, sizeof( f->name ), "c1a%u.HL%u", i / 3, i % 3 + 1 );
		/* .HL1 tens of KiB, .HL2 a few, .HL3 often tiny or empty */
		f->length = kind == 0 ? 0 : kind == 1 ? (int32_t)( next_rand() % 300 ) : kind == 2 ? (int32_t)( next_rand() % 9000 ) : (int32_t)( next_rand() % 70000 );
		f->data = malloc( f->length ? f->length : 1 );
		for( int32_t k = 0; k < f->length; k++ )
			f->data[k] = (uint8_t)next_rand();
		f->missing = with_missing && i % 7 == 3;
	}
}

static void free_fs( fake_fs_t *fs )
{
	for( uint32_t i = 0; i < fs->count; i++ )
		free( fs->f[i].data );
}

/* drains the stream as the engine does; returns bytes written, -1 on failure */
static long drain( fake_fs_t *fs, const uint8_t *head, uint32_t head_len, uint32_t cap, uint32_t align, uint32_t max_opens, uint8_t *out, int *fills )
{
	xbox_save_stream_io_t io = { fake_open, fake_read, fake_close, fs };
	xbox_save_stream_t s;
	uint8_t *raw = malloc( cap + align );
	uint8_t *chunk = raw + ( align - (uintptr_t)raw % align ) % align;
	long n = 0;
	int guard = 0;

	fs->chunk = chunk;
	fs->align = align;
	xbox_save_stream_init( &s, &io, head, head_len, fs->count, chunk, cap, align );
	*fills = 0;
	for( ;; )
	{
		uint32_t len = xbox_save_stream_fill( &s, max_opens );

		(*fills)++;
		CHECK( len <= cap );
		CHECK( max_opens == 0 || s.opens <= max_opens );
		memcpy( out + n, chunk, len );
		n += len;
		xbox_save_stream_consumed( &s );
		if( s.failed )
		{
			xbox_save_stream_close( &s );
			n = -1;
			break;
		}
		if( s.done )
			break;
		if( ++guard > 1000000 )
		{
			CHECK( !"stream does not progress" );
			break;
		}
	}
	xbox_save_stream_close( &s );
	free( raw );
	return n;
}

static void test_equivalence( void )
{
	static const uint32_t caps[] = { 4096, 8192, 65536, 131072, 1u << 20 };
	static const uint32_t aligns[] = { 1, 16, 4096 };
	static const uint32_t opens[] = { 0, 1, 3 };
	int cases = 0;

	for( int round = 0; round < 6; round++ )
	{
		fake_fs_t fs;
		uint32_t count = round == 0 ? 0 : round == 1 ? 1 : 3 + next_rand() % ( MAX_FILES - 3 );
		uint32_t head_len = round == 2 ? 0 : 20 + next_rand() % 20000;
		uint8_t *head = malloc( head_len ? head_len : 1 );
		uint8_t *want, *got;
		size_t want_len;

		make_fs( &fs, count, round >= 3, round == 4 );
		for( uint32_t k = 0; k < head_len; k++ )
			head[k] = (uint8_t)next_rand();
		want = malloc( head_len + count * ( 264 + 70000 ) + 1 );
		got = malloc( head_len + count * ( 264 + 70000 ) + 1 );
		want_len = expected_bytes( &fs, head, head_len, want );

		for( size_t c = 0; c < sizeof( caps ) / sizeof( caps[0] ); c++ )
		for( size_t a = 0; a < sizeof( aligns ) / sizeof( aligns[0] ); a++ )
		for( size_t o = 0; o < sizeof( opens ) / sizeof( opens[0] ); o++ )
		{
			int fills;
			long n;

			if( aligns[a] > caps[c] )
				continue;
			for( uint32_t i = 0; i < fs.count; i++ )
				fs.f[i].pos = fs.f[i].opened = fs.f[i].closed = 0;
			fs.misaligned_reads = fs.order_errors = 0;
			fs.next_open = 0;

			n = drain( &fs, head, head_len, caps[c], aligns[a], opens[o], got, &fills );
			CHECK( n == (long)want_len );
			CHECK( n >= 0 && memcmp( got, want, want_len ) == 0 );
			CHECK( fs.misaligned_reads == 0 );
			CHECK( fs.order_errors == 0 );
			for( uint32_t i = 0; i < fs.count; i++ )
			{
				CHECK( fs.f[i].opened == 1 );
				CHECK( fs.f[i].closed == ( fs.f[i].missing ? 0 : 1 ));
			}
			/* a fill that is not the last writes at least a page short of full
			   only when the next read could not start inside it */
			if( opens[o] == 0 )
				CHECK( (size_t)fills <= want_len / ( caps[c] > aligns[a] ? caps[c] - aligns[a] + 1 : 1 ) + 2 );
			cases++;
		}
		free_fs( &fs );
		free( head );
		free( want );
		free( got );
	}
	printf( "equivalence: %d drains\n", cases );
}

static void test_short_read( void )
{
	fake_fs_t fs;
	uint8_t head[64] = { 1, 2, 3 };
	uint8_t *out = malloc( 1 << 22 );
	int fills;
	long n;

	make_fs( &fs, 6, 0, 0 );
	fs.f[2].length = 50000;
	free( fs.f[2].data );
	fs.f[2].data = calloc( 1, 50000 );
	fs.f[2].short_at = 30000;
	n = drain( &fs, head, sizeof( head ), 16384, 4096, 0, out, &fills );
	CHECK( n == -1 );
	CHECK( fs.f[2].closed == 1 ); /* closed by the give-up path */
	CHECK( fs.f[3].opened == 0 ); /* nothing after the failed file */
	free_fs( &fs );
	free( out );
}

static void test_open_limit_progress( void )
{
	/* many empty files and a one-open limit: every fill still progresses */
	fake_fs_t fs;
	uint8_t *out = malloc( 1 << 20 );
	int fills;
	long n;

	memset( &fs, 0, sizeof( fs ));
	fs.count = 40;
	for( int i = 0; i < 40; i++ )
	{
		snprintf( fs.f[i].name, sizeof( fs.f[i].name ), "m%02d.HL3", i );
		fs.f[i].data = malloc( 1 );
	}
	n = drain( &fs, NULL, 0, 4096, 4096, 1, out, &fills );
	CHECK( n == 40 * 264 );
	CHECK( fills >= 40 );
	free_fs( &fs );
	free( out );
}

int main( void )
{
	test_equivalence();
	test_short_read();
	test_open_limit_progress();
	if( failures )
	{
		printf( "xbox_save_stream: %d failure(s)\n", failures );
		return 1;
	}
	printf( "xbox_save_stream: all passed\n" );
	return 0;
}
