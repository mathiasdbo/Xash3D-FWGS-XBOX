/*
xbox_memreport.c - `r3d_memreport`: where the console's memory is, at each
map start
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
Resonance3D (docs/r3d/divergences.md #195). The frame-time recorder keeps
one number per map, the free pages at its start; 720p left about 6-12 MiB
there and nothing said what held the rest. With `r3d_memreport 1` every
map start appends a block to memreport.txt beside config.cfg
(E:\UDATA\<title id>\valve\valve\):

- the kernel's own split (MmQueryStatistics): the XBE image with its BSS,
  the heap's committed virtual memory, pool, stacks, file cache; what is
  left of the used pages is contiguous memory (the renderer's, pbkit's,
  the APU's) and the kernel itself;
- the largest block MmAllocateContiguousMemoryEx can still give, found by
  halving probes, each freed at once: a texture or lightmap upload needs
  one run, which the free page count does not promise;
- the lowest free memory seen at the end of a load stage since the last
  report (the load's own peak, which the map start no longer shows);
- the C library's heap (pdclib's dlmalloc): what is in use, what is free
  inside it (held, not given back), and what of the in-use part the zone
  does not account for - malloc and C++ new outside it;
- every zone pool of 64 KiB or more, largest first;
- the server's edicts in use.

`r3d_memreport_now [tag]` writes a block on demand. Off (the default) the
cost is one cvar test a frame and one per load stage.
*/

#include "common.h"
#include "client.h"
#include "server.h"
#include "xbox_frametime.h"
#include <xboxkrnl/xboxkrnl.h>

/* pdclib's dlmalloc 2.8.6 (nxdk lib/pdclib/functions/_dlmalloc/malloc.c,
   USE_DL_PREFIX): its statistics, not declared in any header. */
struct mr_mallinfo
{
	size_t arena;    /* non-mmapped space allocated from the system */
	size_t ordblks;  /* number of free chunks */
	size_t smblks;
	size_t hblks;
	size_t hblkhd;   /* space in mmapped regions */
	size_t usmblks;  /* maximum total allocated space */
	size_t fsmblks;
	size_t uordblks; /* total allocated space */
	size_t fordblks; /* total free space */
	size_t keepcost; /* releasable (via malloc_trim) space */
};
struct mr_mallinfo dlmallinfo( void );
size_t dlmalloc_footprint( void );
size_t dlmalloc_max_footprint( void );

#define MR_MIB( b )        ( (double)( b ) / ( 1024.0 * 1024.0 ))
#define MR_PAGE            4096u
#define MR_PROBE_STEP      ( 64u * 1024u )
#define MR_PROBE_MAXRAM    0x03FFAFFF  /* the renderer's own limit for its buffers */
#define MR_POOL_MIN        ( 64u * 1024u )
#define MR_POOLS_MAX       128

static CVAR_DEFINE_AUTO( r3d_memreport, "0", 0,
	"1: append where the memory is to memreport.txt at every map start, 2: and every texture and sound (docs/r3d/divergences.md #195)" );

static qboolean mr_was_active;
static uint32_t mr_low_pages = UINT32_MAX;
static char     mr_low_stage[32];

static uint32_t MR_AvailPages( void )
{
	MM_STATISTICS ms;

	memset( &ms, 0, sizeof( ms ));
	ms.Length = sizeof( ms );
	return NT_SUCCESS( MmQueryStatistics( &ms )) ? ms.AvailablePages : 0;
}

/* The largest contiguous block, to MR_PROBE_STEP: the kernel takes and
   gives back each probe within the call, so nothing stays allocated. */
static size_t MR_LargestContiguous( size_t avail )
{
	size_t lo = 0, hi = avail / MR_PROBE_STEP;

	while( lo < hi )
	{
		size_t mid = ( lo + hi + 1 ) / 2;
		void *p = MmAllocateContiguousMemoryEx( mid * MR_PROBE_STEP, 0, MR_PROBE_MAXRAM, 0, PAGE_READWRITE );

		if( p )
		{
			MmFreeContiguousMemory( p );
			lo = mid;
		}
		else
		{
			hi = mid - 1;
		}
	}
	return lo * MR_PROBE_STEP;
}

/* The renderer's own breakdown (ref/nv2a r3d_gpumem) goes into the file
   through the console's redirect, the one rcon uses (host.rd). */
static file_t *mr_file;

static void MR_RedirectFlush( netadr_t adr, rdtype_t target, char *buffer )
{
	if( mr_file )
		FS_Write( mr_file, buffer, Q_strlen( buffer ));
}

static void MR_GpuMem( void )
{
	host_redirect_t saved = host.rd;
	const qboolean allow_console = host.allow_console;
	char buffer[512];

	/* without -console every Con_Printf returns before the redirect */
	host.allow_console = true;
	buffer[0] = '\0';
	host.rd.target = RD_PACKET;
	host.rd.buffer = buffer;
	host.rd.buffersize = sizeof( buffer );
	host.rd.flush = MR_RedirectFlush;
	host.rd.lines = -1;
	Cmd_ExecuteString( "r3d_gpumem" );
	if( r3d_memreport.value >= 2 )
	{
		Cmd_ExecuteString( "texturelist" );
		Cmd_ExecuteString( "soundlist" ); /* each sound's size and loop flag */
	}
	host.rd = saved;
	host.allow_console = allow_console;
}

typedef struct mr_pool_s
{
	const char *name;
	size_t total, real;
} mr_pool_t;

static int MR_PoolCmp( const void *a, const void *b )
{
	size_t ra = ((const mr_pool_t *)a)->real, rb = ((const mr_pool_t *)b)->real;

	return ra < rb ? 1 : ra > rb ? -1 : 0;
}

static void MR_Write( const char *tag )
{
	static mr_pool_t pools[MR_POOLS_MAX];
	MM_STATISTICS ms;
	file_t *f;
	string line;
	size_t i, n = 0, zone_total = 0, zone_real = 0, used, kernel_split, largest;
	const char *name;
	size_t total, real;

	memset( &ms, 0, sizeof( ms ));
	ms.Length = sizeof( ms );
	if( !NT_SUCCESS( MmQueryStatistics( &ms )))
		return;
	/* first: the probes and the file below allocate */
	largest = MR_LargestContiguous( (size_t)ms.AvailablePages * MR_PAGE );

	for( i = 0; Mem_PoolInfo( i, &name, &total, &real ); i++ )
	{
		zone_total += total;
		zone_real += real;
		if( real >= MR_POOL_MIN && n < MR_POOLS_MAX )
		{
			pools[n].name = name;
			pools[n].total = total;
			pools[n].real = real;
			n++;
		}
	}
	qsort( pools, n, sizeof( pools[0] ), MR_PoolCmp );

	f = FS_Open( "memreport.txt", "a", true );

#define MR_LINE( ... ) do { \
		Q_snprintf( line, sizeof( line ), __VA_ARGS__ ); \
		Con_Printf( "%s", line ); \
		if( f ) FS_Write( f, line, Q_strlen( line )); \
	} while( 0 )

	used = (size_t)( ms.TotalPhysicalPages - ms.AvailablePages ) * MR_PAGE;
	/* The kernel's virtual memory figure holds the XBE image and the
	   heap's pages (measured: image + dlmalloc's footprint); what the
	   split leaves is contiguous memory - the renderer's, pbkit's, the
	   APU's - and the kernel's own. */
	kernel_split = ms.VirtualMemoryBytesCommitted
		+ (size_t)( ms.PoolPagesCommitted + ms.StackPagesCommitted + ms.CachePagesCommitted ) * MR_PAGE;

	MR_LINE( "== memreport %s, map %s, %.1f s\n", tag, clgame.mapname[0] ? clgame.mapname : "-", host.realtime );
	MR_LINE( "free %.2f MiB of %.2f; largest contiguous block %.2f MiB\n",
		MR_MIB( (size_t)ms.AvailablePages * MR_PAGE ), MR_MIB( (size_t)ms.TotalPhysicalPages * MR_PAGE ), MR_MIB( largest ));
	if( mr_low_pages != UINT32_MAX )
		MR_LINE( "lowest free at a load stage since the last report: %.2f MiB (after %s)\n",
			MR_MIB( (size_t)mr_low_pages * MR_PAGE ), mr_low_stage );
	MR_LINE( "used %.2f MiB: image %.2f, heap %.2f, stacks %.2f, pool %.2f, file cache %.2f, contiguous and kernel %.2f\n",
		MR_MIB( used ), MR_MIB( (size_t)ms.ImagePagesCommitted * MR_PAGE ), MR_MIB( dlmalloc_footprint()),
		MR_MIB( (size_t)ms.StackPagesCommitted * MR_PAGE ), MR_MIB( (size_t)ms.PoolPagesCommitted * MR_PAGE ),
		MR_MIB( (size_t)ms.CachePagesCommitted * MR_PAGE ), MR_MIB( used > kernel_split ? used - kernel_split : 0 ));
	{
		struct mr_mallinfo mi = dlmallinfo();

		MR_LINE( "heap (dlmalloc): in use %.2f MiB (of it large blocks mapped alone %.2f), free inside %.2f in %u pieces, peak %.2f; "
			"zone %.2f with headers, malloc outside the zone %.2f\n",
			MR_MIB( mi.uordblks ), MR_MIB( mi.hblkhd ), MR_MIB( mi.fordblks ), (unsigned)mi.ordblks, MR_MIB( dlmalloc_max_footprint()),
			MR_MIB( zone_real ), MR_MIB( mi.uordblks > zone_real ? mi.uordblks - zone_real : 0 ));
	}
	for( i = 0; i < n; i++ )
		MR_LINE( "  %8.1f KiB (%8.1f KiB real)  %s\n", pools[i].total / 1024.0, pools[i].real / 1024.0, pools[i].name );
	if( sv.state != ss_dead && GI )
		MR_LINE( "edicts %d of %d\n", svgame.numEntities, GI->max_edicts );
	mr_file = f;
	MR_GpuMem();
	mr_file = NULL;

#undef MR_LINE

	if( f )
		FS_Close( f );
	mr_low_pages = UINT32_MAX;
	mr_low_stage[0] = '\0';
}

/* The end of a load stage (Xbox_FT_LoadEnd, Xbox_FT_LoadAccum). */
void Xbox_MemReport_Stage( const char *stage )
{
	uint32_t avail;

	if( !r3d_memreport.value )
		return;
	avail = MR_AvailPages();
	if( avail && avail < mr_low_pages )
	{
		mr_low_pages = avail;
		Q_strncpy( mr_low_stage, stage ? stage : "-", sizeof( mr_low_stage ));
	}
	if( r3d_memreport.value >= 2 )
	{
		/* #201: a load that stops the console writes no map-start block;
		   each stage's line is on disk (opened and closed) before the
		   next stage runs, so the last line is where it got to */
		file_t *f = FS_Open( "loadtrace.txt", "a", true );

		if( f )
		{
			FS_Printf( f, "%.1f s %-16s free %.2f MiB\n", host.realtime,
				stage ? stage : "-", avail / 256.0 );
			FS_Close( f );
		}
	}
}

/* Every frame (Xbox_FrameTime_BeginFrame, before the recorder's own
   early return): a block at each map's first frame in play. */
void Xbox_MemReport_Frame( void )
{
	qboolean active;

	if( !r3d_memreport.value )
	{
		mr_was_active = false;
		return;
	}
	active = cls.state == ca_active && cls.signon == SIGNONS;
	if( active && !mr_was_active )
		MR_Write( "map start" );
	mr_was_active = active;
}

static void MemReport_f( void )
{
	MR_Write( Cmd_Argc() > 1 ? Cmd_Argv( 1 ) : "command" );
}

void Xbox_MemReport_Init( void )
{
	Cvar_RegisterVariable( &r3d_memreport );
	Cmd_AddCommand( "r3d_memreport_now", MemReport_f, "append where the memory is to memreport.txt now (docs/r3d/divergences.md #195)" );
}
