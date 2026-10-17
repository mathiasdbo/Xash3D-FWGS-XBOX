/*
xbox_frametime_util.h - the frame-time recorder's pure parts: the frame
ring, the per-map statistics and the file format's byte layout
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
Resonance3D (docs/r3d/divergences.md #141, fork-plan item 14 step 13).
Nothing in here touches the engine, the kernel or the GPU, so the whole
file is host-tested (tools/host-tests/xbox_frametime_util_host_test.c) and
the same encoder writes the fixture tools/frametime-report.py's own tests
parse. The recorder that drives it is xbox_frametime.c.

File format (version 3; every integer little-endian, the Xbox's own byte
order - the encoder below writes bytes explicitly, so a host of either
endianness produces the same file). Version 1 is the same file without
work counters: 32-byte frame records and no COUNTERS chunk. The encoders
still write it when given zero counters (the tests' v1 fixture).
Version 2 is one file per recording (ft_NNN.bin) with the 128-byte
header below; version 3 (2026-10-07) is the same chunks split into
SEGMENTS - one file per map and per XBOX_FT_SEGMENT_MB of recording,
named ft_<label>_<session>_<seq>.bin - each with the 256-byte header
(the v2 fields, then the session's id, the label, the segment's number
and why it began) and its own COUNTERS and SPANS chunks, so every
segment reads on its own and a reader stitches a session back together
by session id and segment number. xbox_ft_encode_header still writes
v1/v2 headers (the tests' older fixtures); xbox_ft_encode_segment_open
writes v3.

  header, XBOX_FT_HEADER_BYTES (128; v3: XBOX_FT_HEADER_V3_BYTES, 256):
    0  char[8] magic "R3DFTIME"
    8  u32 version (3; 2 for a single-file recording, 1 without counters)
    12 u32 header bytes (128; v3 256 - a reader starts the chunks here)
    16 u32 frame record bytes (32 + 4 x the counter count; 32 in v1)
    20 u32 ring frames (the recorder's in-memory ring size)
    24 u64 TSC rate estimate at open, Hz (nominal until calibrated -
           the CALIB chunks are the real calibration)
    32 u64 performance-counter rate, Hz (KeQueryPerformanceFrequency)
    40 u64 TSC at open (v3: when the SESSION opened - the same in every
           segment, so each one calibrates over the whole session so far)
    48 u64 performance counter at open (v3: the session's, likewise)
    56 u32 mode (host_frametime_analysis at open: 1 record, 2 overlay)
    60 u32 reserved (0)
    64 char[64] build id, NUL-padded (the XBE's link timestamp in it)
  v3 only:
    128 u8[16] session id (XBOX_FT_SESSION_ID_BYTES; xbox_ft_session_id:
           the kernel clock, the TSC and the performance counter mixed,
           version/variant bits set like a random UUID); the file name
           carries its first 4 bytes as 8 lowercase hex digits
    144 u32 segment number (0 for the session's first)
    148 u32 why the segment began (XBOX_FT_SEG_*: 0 the session started,
           1 a map started, 2 the previous segment reached its size cap)
    152 u32 the sequence number of the first frame it holds (the next
           frame after the previous segment's last - a reader checks the
           seam)
    156 u32 reserved (0)
    160 u64 TSC when the segment opened
    168 u64 kernel system time when the session opened (KeQuerySystemTime:
           100 ns units since 1601-01-01; the console's clock, so only as
           right as it was set)
    176 char[32] label (host_frametime_label sanitised: [A-Za-z0-9_-],
           at most XBOX_FT_LABEL_MAX characters, "untagged" when empty),
           NUL-padded
    208 48 bytes reserved (0)
  then chunks until end of file, each { u32 type, u32 payload bytes,
  payload }; a reader skips a type it does not know by its length:
    1 FRAMES  u32 first sequence number, u32 count, count frame records
    2 MAP     u64 TSC at map start, u64 load ticks, u32 free pages when
              the load began, u32 free pages at map start, u32 total
              pages, u32 flags (bit 0: no load start seen - the first map
              after recording began; bit 1, v3: the map CONTINUES from
              the previous segment, which the size cap closed - the first
              chunk of such a segment, so it reads on its own; a reader
              stitching the session drops it), char[64] map name
    3 CALIB   u64 TSC, u64 performance counter (one per flush)
    4 END     u64 TSC, u32 frames lost to a full ring, u32 why the file
              ended (v3: XBOX_FT_SEG_* of the segment that follows it, 0
              when the session ended; older files 0) - written when
              recording stops cleanly or a segment closes (a file without
              it was cut by a power-off or a crash). v3: the frames lost
              while that segment was open; older files: in the recording
    5 COUNTERS (v2, right after the header, before any FRAMES chunk)
              u32 count, then count x char[16] names, NUL-padded: the
              names of the work counters at the end of every frame
              record, in order (xbox_ft_work_names below). A reader
              finds counters by name, so a later version may append or
              reorder them without breaking it.
    6 SPANS   (2026-10-01, right after COUNTERS) u32 count, then count x
              { char[16] span, char[16] parent }, NUL-padded: which work
              counters are CPU-time spans (k_*, kilocycles, XBOX_FT_S_*
              below) and the span each one is nested in ("" for a span
              directly inside Host_Frame). A reader without it skips the
              chunk by its length and sees the spans as plain counters,
              so the version stays 2.
    7 CLASSES (2026-10-05) the server game frame per entity classname,
              for the frames since the previous CLASSES chunk: written
              before each MAP chunk (closing the map before it) and when
              recording stops. u32 count, then count x
              XBOX_FT_CLASS_BYTES (60): char[32] classname, NUL-padded,
              u32 movetype (the last one seen), u32 runs (entity-frames
              SV_Physics ran it), u32 thinks, u32 SV_Move calls, u32
              kilocycles in its SV_Physics_Entity (think included), u32
              kilocycles in its Think, u32 kilocycles of its slowest
              single SV_Physics_Entity. Every chunk lists every classname
              seen so far in the recording, in first-seen order, so an
              index (the g_worst_cls counter) means the same name in
              every chunk. Only frames that start in play are counted
              (not menu or pause frames; the warm-up frames are).
    8 NOTES   (2026-10-06) named one-off events since the previous
              flush: u32 count, u32 dropped, then count x
              XBOX_FT_NOTE_BYTES (64): u32 frame sequence number, u32
              kind, u32 kilocycles, u32 bytes, char[48] name (see
              xbox_ft_encode_notes below). Written at a flush that has
              any.
  frame record, XBOX_FT_FRAME_BYTES (32) + 4 x counters:
    0  u32 sequence number (consecutive; a gap is frames lost)
    4  u32 total ticks, Host_Frame start to the next Host_Frame start
    8  u32 host ticks, Host_Frame start to its end
    12 u32 GPU wait ticks (CPU blocked in pb_busy: the GPU's tail)
    16 u32 vblank wait ticks (pb_wait_for_vbl)
    20 u32 swap wait ticks (pb_finished: no free back buffer)
    24 u32 overlay ticks (the overlay's own drawing, inside host)
    28 u16 flags (XBOX_FT_F_*), u16 reserved
    32 u32 work counters[count] (v2): this frame's renderer work, see
       XBOX_FT_W_* - counts of things submitted, so unlike the times
       they are the same in xemu and on the console - and, from
       2026-10-01, the per-subsystem CPU spans (k_*, kilocycles: TSC
       ticks / 1000, rounded), which ARE times (hardware only)
  Every tick count saturates at 0xFFFFFFFF (5.8 s at 733 MHz) and sets
  XBOX_FT_F_SATURATED. CPU = host - gpu - vblank - swap; the rest of
  total (total - host) is the time between host frames: the engine's
  frame limiter (Host_Autosleep) and the platform loop.
*/

#ifndef XBOX_FRAMETIME_UTIL_H
#define XBOX_FRAMETIME_UTIL_H

#include <stddef.h>
#include <stdint.h>

#define XBOX_FT_VERSION        3u   /* segments (2026-10-07) */
#define XBOX_FT_VERSION_SINGLE 2u   /* one file per recording, the 128-byte header */
#define XBOX_FT_VERSION_NO_WORK 1u   /* a file without work counters */
#define XBOX_FT_HEADER_BYTES   128u
#define XBOX_FT_HEADER_V3_BYTES 256u
#define XBOX_FT_FRAME_BYTES    32u   /* the fixed part; v2 appends 4 bytes per counter */
#define XBOX_FT_COUNTER_NAME_BYTES 16u
#define XBOX_FT_CHUNK_BYTES    8u  /* type + payload length */
#define XBOX_FT_MAP_BYTES      96u
#define XBOX_FT_CALIB_BYTES    16u
#define XBOX_FT_END_BYTES      16u
#define XBOX_FT_NAME_BYTES     64u

#define XBOX_FT_CHUNK_FRAMES   1u
#define XBOX_FT_CHUNK_MAP      2u
#define XBOX_FT_CHUNK_CALIB    3u
#define XBOX_FT_CHUNK_END      4u
#define XBOX_FT_CHUNK_COUNTERS 5u
#define XBOX_FT_CHUNK_SPANS    6u
#define XBOX_FT_CHUNK_CLASSES  7u

/* Per-frame work counters (v2). ref/nv2a adds to them where the work is
   submitted (xbox_frametime.h's xbox_ft_work); the recorder zeroes them
   at Host_Frame start and stores them in the frame record. Every one is a
   count, not a time, so it is identical in xemu and on the console.
   New ones go in the enum after the spans (below, before XBOX_FT_W_END)
   so the older indices hold; give them a name in
   xbox_frametime_util.c. */
enum
{
	XBOX_FT_W_DRAWS = 0,     /* draw submissions: a world run, a 2D/TriAPI batch flush, a decal */
	XBOX_FT_W_PRIMS,         /* BEGIN/END blocks pushed (a draw splits at pbkit's 128-dword block) */
	XBOX_FT_W_PB_DWORDS,     /* pushbuffer dwords written, R_BeginFrame's pb_reset to R_EndFrame */
	XBOX_FT_W_TEX_BINDS,     /* texture stages programmed (NV097_SET_TEXTURE_OFFSET writes) */
	XBOX_FT_W_VERTS_WORLD,   /* world and brush-entity indices submitted */
	XBOX_FT_W_VERTS_STUDIO,  /* studio-model vertices (the TriAPI path, inside R_StudioDrawPoints) */
	XBOX_FT_W_VERTS_TRI,     /* other 3D TriAPI vertices: sprites, beams, particles, water, sky */
	XBOX_FT_W_VERTS_2D,      /* 2D vertices: HUD, console, menus, this overlay */
	XBOX_FT_W_VERTS_DECAL,   /* decal vertices */
	XBOX_FT_W_WORLD_FACES,   /* world faces left after PVS, frustum and backface culling */
	XBOX_FT_W_PVS_LEAVES,    /* leaves in the current PVS (a state: the count at its last recompute) */
	XBOX_FT_W_STUDIO_MODELS, /* studio models drawn */
	XBOX_FT_W_STUDIO_TRIS,   /* studio mesh triangles drawn */
	XBOX_FT_W_LM_FACES,      /* lightmap faces rebuilt (divergences.md #162) */
	XBOX_FT_W_LM_TEXELS,     /* lightmap texels (luxels) rebuilt */
	XBOX_FT_W_WATER_VERTS,   /* water vertices drawn, warped on the CPU (#166) or the GPU (R5: water_gpu) */
	XBOX_FT_W_DECALS,        /* decals drawn (#167) */
	XBOX_FT_W_PARTICLES,     /* particles drawn (#161) */
	XBOX_FT_W_BEAMS,         /* beams drawn (#161) */
	XBOX_FT_W_SPRITES,       /* sprite models drawn */
	XBOX_FT_W_SPAN_CALLS,    /* span closures timed this frame (the spans' own cost: two rdtsc each) */
	XBOX_FT_W_SPAN_FIRST     /* then the XBOX_FT_S_COUNT spans below, kilocycles each */
};

/* Per-subsystem CPU spans (M2 of docs/r3d/reports/perf-research/
   xbox-cpu-bound-o2.md). Each is the TSC time spent inside one call
   site, accumulated over the frame, MINUS the GPU/vblank/swap waits that
   happened inside it (xbox_ft_waits) - so a span is CPU work, and the
   spans add up against the frame's CPU, not its host time. Stored in the
   frame record as work counter XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_*, in
   kilocycles. Spans nest: xbox_ft_span_parent says in which one (-1 =
   directly inside Host_Frame); a parent's time includes its children's.
   The top-level spans cover every call Host_Frame makes, so CPU minus
   their sum is what no span saw (HTTP_Run, XRcon_Frame, the recorder).
   Where each is measured: docs/r3d/frametime-recorder.md, "Spans". */
enum
{
	XBOX_FT_S_INPUT = 0,     /* Host_InputFrame */
	XBOX_FT_S_CL_BEGIN,      /* Host_ClientBegin: Cbuf_Execute, CL_SendCommand (client.dll CL_CreateMove, prediction) */
	XBOX_FT_S_SV,            /* Host_ServerFrame */
	XBOX_FT_S_SV_READ,       /*   SV_ReadPackets: client usercmds - player movement, PlayerPreThink/PostThink */
	XBOX_FT_S_SV_GAME,       /*   SV_Physics (SV_RunGameFrame): game DLL think, physics, AI */
	XBOX_FT_S_SV_SEND,       /*   SV_SendClientMessages: entity packing, delta encoding */
	XBOX_FT_S_CL,            /* Host_ClientFrame */
	XBOX_FT_S_CL_DLL,        /*   clgame.dllFuncs.pfnFrame */
	XBOX_FT_S_CL_READ,       /*   CL_ReadPackets */
	XBOX_FT_S_CL_PRED,       /*   CL_RedoPrediction */
	XBOX_FT_S_CL_EMIT,       /*   CL_EmitEntities */
	XBOX_FT_S_SCR,           /*   SCR_UpdateScreen: all rendering */
	XBOX_FT_S_R_SCENE,       /*     GL_RenderFrame (V_RenderView): the 3D scene */
	XBOX_FT_S_LM,            /*       ref/nv2a lightstyle/dlight lightmap rebuild */
	XBOX_FT_S_WORLD,         /*       ref/nv2a sky, world and world decals */
	XBOX_FT_S_CLIENT_STUDIO, /*       R_StudioDrawModelInternal: the client's StudioDraw* incl. bone setup */
	XBOX_FT_S_STUDIO_POINTS, /*         R_StudioDrawPoints: our CPU skinning, lighting, TriAPI emit */
	XBOX_FT_S_SKIN_READ,     /*           a studio skin's lazy disk read and decode */
	XBOX_FT_S_EFX,           /*       CL_DrawEFX, solid and translucent: beams, particles, tracers */
	XBOX_FT_S_TRI_HOOKS,     /*       pfnDrawNormalTriangles + pfnDrawTransparentTriangles (client.dll) */
	XBOX_FT_S_HUD,           /*     CL_DrawHUD( CL_ACTIVE ): the client's HUD_Redraw */
	XBOX_FT_S_SND_EXTRA,     /*     S_ExtraUpdate in V_PostRender: a second mixer pass */
	XBOX_FT_S_R_END,         /*     R_EndFrame: 2D flush, pushbuffer kick, present */
	XBOX_FT_S_SND,           /*   SND_UpdateSound: the mixer */
	/* The local server <-> client round trip, broken down (2026-10-02).
	   Appended rather than placed under their parents above so the older
	   spans keep their indices; the parent table gives the nesting. */
	XBOX_FT_S_SV_RELIABLE,   /*   in k_sv_send: SV_UpdateToReliableMessages */
	XBOX_FT_S_SV_FULLPACK,   /*   in k_sv_send: SV_AddEntitiesToPacket - game DLL SetupVisibility + AddToFullPack per edict */
	XBOX_FT_S_SV_CLIENTDATA, /*   in k_sv_send: SV_WriteClientdataToMessage - game DLL UpdateClientData/GetWeaponData + delta */
	XBOX_FT_S_SV_EMIT_ENTS,  /*   in k_sv_send: SV_EmitPacketEntities - entity delta encoding */
	XBOX_FT_S_SV_XMIT,       /*   in k_sv_send: Netchan_TransmitBits of the client datagram */
	XBOX_FT_S_SV_RUNCMD,     /*   in k_sv_read: SV_RunCmd - player movement (PM_Move), PlayerPreThink/PostThink */
	XBOX_FT_S_CL_PKT_ENTS,   /*   in k_cl_read: CL_ParsePacketEntities - entity delta decoding, CL_ProcessPacket */
	XBOX_FT_S_CL_CLIENTDATA, /*   in k_cl_read: CL_ParseClientData */
	XBOX_FT_S_CL_CMD,        /*   in k_cl_begin: CL_CreateCmd - client.dll CL_CreateMove, then prediction */
	XBOX_FT_S_CL_PREDMOVE,   /*     in k_cl_cmd: CL_PredictMovement - client-side PM_Move */
	XBOX_FT_S_CL_WPACKET,    /*   in k_cl_begin: CL_WritePacket - usercmd delta, Netchan_TransmitBits */
	/* The mixer, broken down (2026-10-02, appended for the same reason).
	   S_PaintChannels runs from both SND_UpdateSound and S_ExtraUpdate,
	   so its three spans come twice, one set per caller. */
	XBOX_FT_S_SND_SPATIAL,   /*   in k_snd: SND_UpdateSound before the mix - fade, ambients, every channel's SND_Spatialize, raw channels */
	XBOX_FT_S_SND_MIX,       /*   in k_snd: S_MixNormalChannelsToRoombuffer + S_MixRawChannels - resampling, volume, VOX, lip sync */
	XBOX_FT_S_SND_DSP,       /*   in k_snd: SX_RoomFX - room modulation, reverb, delay, stereo delay */
	XBOX_FT_S_SND_OUT,       /*   in k_snd: S_ClearBuffers, S_MixBufferWithGain, S_TransferPaintBuffer */
	XBOX_FT_S_SNDX_MIX,      /*   in k_snd_extra: the same as k_snd_mix */
	XBOX_FT_S_SNDX_DSP,      /*   in k_snd_extra: the same as k_snd_dsp */
	XBOX_FT_S_SNDX_OUT,      /*   in k_snd_extra: the same as k_snd_out */
	/* The 3D scene, broken down (2026-10-02, appended for the same
	   reason): k_world's parts, and what k_r_scene spent of its own. */
	XBOX_FT_S_PVS,           /*   in k_r_scene: ref/nv2a nv2a_mark_leaves - the PVS and visible-face list at a leaf change */
	XBOX_FT_S_SKY,           /*   in k_world: nv2a_draw_sky - the sky faces' walk and the skybox */
	XBOX_FT_S_WORLD_WALK,    /*   in k_world: nv2a_draw_map_geometry's batch and face walk - culling and index submission */
	XBOX_FT_S_WARP_WORLD,    /*   in k_world: R_WarpDrawWorld - the world's opaque water, warped on the CPU */
	XBOX_FT_S_WORLD_DECALS,  /*   in k_world: R_DecalDrawWorld */
	XBOX_FT_S_BRUSH,         /*   in k_r_scene: nv2a_draw_brush_entity, every solid and translucent brush entity */
	XBOX_FT_S_WARP_BRUSH,    /*     in k_brush: R_WarpDrawBrushEntity - a brush entity's water (func_water) */
	XBOX_FT_S_WARP_TRANS,    /*   in k_r_scene: R_WarpDrawWaterSurfaces - the world's translucent water */
	/* The server's game frame, broken down (2026-10-05, appended for the
	   same reason): SV_Physics' parts. Each entity's whole
	   SV_Physics_Entity - its movement, its think, the touches and traces
	   it causes - is charged to the span of the movetype it had when it
	   started; k_sv_game's own time is the loop itself, lightstyles and
	   the SV_CheckAllEnts sweep. xbox_frametime_svgame.c. */
	XBOX_FT_S_SV_START,      /*   in k_sv_game: the game DLL's StartFrame */
	XBOX_FT_S_SV_PSTEP,      /*   in k_sv_game: MOVETYPE_STEP/PUSHSTEP - monsters: their AI think, SV_Physics_Step */
	XBOX_FT_S_SV_PPUSH,      /*   in k_sv_game: MOVETYPE_PUSH - brush movers (trains, doors, lifts): SV_PushMove/Rotate, think */
	XBOX_FT_S_SV_PTOSS,      /*   in k_sv_game: TOSS/BOUNCE/FLY/FLYMISSILE/BOUNCEMISSILE - gibs, grenades, falling items */
	XBOX_FT_S_SV_PNONE,      /*   in k_sv_game: MOVETYPE_NONE - think-only entities: triggers, logic, sprites */
	XBOX_FT_S_SV_POTHER,     /*   in k_sv_game: NOCLIP, FOLLOW, COMPOUND and any other movetype */
	/* A savegame and its thumbnail, broken down (2026-10-05, appended for
	   the same reason). A `save`/`autosave` runs from Cbuf_Execute in
	   Host_ClientBegin, and the `saveshot` it queues from the same
	   Cbuf_Execute, taken in that frame's V_PostRender, so an autosave is
	   one long frame. The k_save_* children are sequential parts of
	   SaveGameSlot (engine/server/sv_save.c); the k_shot_* ones are
	   ref/nv2a's VID_ScreenShot. A file write is a synchronous kernel
	   call, so the disk's time is in these spans. */
	XBOX_FT_S_SAVE,          /*   in k_cl_begin: SaveGameSlot - the whole save */
	XBOX_FT_S_SAVE_INIT,     /*     in k_save: SaveInit/SaveFinish - the save buffer's allocation, clearing and release */
	XBOX_FT_S_SAVE_ENTS,     /*     in k_save: SaveGameState's serialisation - header, adjacency, lightstyles, every entity's DLL Save, ETABLE, tokens */
	XBOX_FT_S_SAVE_HL1,      /*     in k_save: writing <map>.HL1 and <map>.HL3 */
	XBOX_FT_S_SAVE_CLIENT,   /*     in k_save: SaveClientState - decals, static entities, sounds, <map>.HL2 */
	XBOX_FT_S_SAVE_GLOBALS,  /*     in k_save: SaveGameSlot's game header and the DLL's SaveGlobalState */
	XBOX_FT_S_SAVE_AGE,      /*     in k_save: AgeSaveList - the aged autosave/quick files' deletes and renames */
	XBOX_FT_S_SAVE_SAV,      /*     in k_save: the .sav file - open, header, tokens, globals, every HL? file copied in (DirectoryCopy), close */
	XBOX_FT_S_SCRSHOT,       /*   in k_scr: SCR_MakeScreenShot - the saveshot thumbnail (or any screenshot) */
	XBOX_FT_S_SHOT_READ,     /*     in k_scrshot: ref/nv2a's back-buffer readback (after the GPU wait) */
	XBOX_FT_S_SHOT_RESAMPLE, /*     in k_scrshot: Image_Process - the resample to the thumbnail's size */
	XBOX_FT_S_SHOT_WRITE,    /*     in k_scrshot: FS_SaveImage - the image's encode and write */
	/* The menu and the music, broken down (2026-10-06, x9-perf-stalls,
	   appended for the same reason). The first pause after a map load
	   cost ~310 ms on the console, all of it in k_scr's own time: the
	   menu DLL's redraw had no span. k_ui is that redraw (UI_UpdateMenu's
	   pfnRedraw, engine/client/dll_int/cl_gameui.c), including what the
	   menu restores on demand after a map load released it
	   (docs/r3d/divergences.md #127: font atlases, font files, background
	   tiles); k_ui_file and k_ui_pic are the two engine services that
	   restore goes through. k_snd_music is S_StreamBackgroundTrack, the
	   part of k_snd's own time that opens, reads and decodes the music. */
	XBOX_FT_S_UI,            /*     in k_scr: UI_UpdateMenu - the menu DLL's redraw, its lazy restores included */
	XBOX_FT_S_UI_FILE,       /*       in k_ui: the menu's COM_LoadFile calls - font cache, font files, layouts */
	XBOX_FT_S_UI_PIC,        /*       in k_ui: the menu's PIC_Load calls - image decode and texture upload */
	XBOX_FT_S_SND_MUSIC,     /*   in k_snd: S_StreamBackgroundTrack - the music stream: open, read, decode */
	/* The deferred savegame write (2026-10-06, x9-perf-saveasync,
	   docs/r3d/divergences.md #153): a save's .sav is copied and written
	   over the frames after the save, a chunk a frame, from Host_Frame
	   after the client frame (SV_SaveAsync_Frame). */
	XBOX_FT_S_SAVE_PUMP,     /* top level: SV_SaveAsync_Frame - one chunk of a pending .sav: .HL? opens and reads, the write, the final rename */
	XBOX_FT_S_COUNT
};

/* Counts added after the spans existed (2026-10-02) go after them, so
   every earlier counter and span keeps its place in the record; the
   SPANS chunk names only the XBOX_FT_S_COUNT counters from
   XBOX_FT_W_SPAN_FIRST. */
enum
{
	XBOX_FT_W_WORLD_WALKED = XBOX_FT_W_SPAN_FIRST + XBOX_FT_S_COUNT, /* faces the world pass's face loop visited (world_faces: what it drew) */
	XBOX_FT_W_WATER_GPU,     /* R5: the water_verts the vertex unit warped (vs_warp) */
	XBOX_FT_W_VP_LOADS,      /* R5: vertex-program loads into slots another program had (vs_warp over vs_studio's tail, and back) */
	/* The server game frame's work (2026-10-05, xbox_frametime_svgame.c):
	   counted only inside SV_Physics (k_sv_game), so player movement in
	   k_sv_runcmd and the send path are not in them. Counts, exact in
	   xemu - except the four marked as times. */
	XBOX_FT_W_G_ENTS,        /* entities SV_Physics ran */
	XBOX_FT_W_G_THINKS,      /* game DLL Think calls (SV_RunThink, SV_Physics_Pusher) */
	XBOX_FT_W_G_TOUCHES,     /* game DLL Touch calls (SV_Impact, both ways; SV_TouchLinks) */
	XBOX_FT_W_G_MOVES,       /* SV_Move calls: every hull trace, the engine's physics and the DLL's alike */
	XBOX_FT_W_G_DLL_TRACES,  /* the DLL's TraceLine/TraceHull/TraceMonsterHull/TraceModel/TraceToss calls */
	XBOX_FT_W_G_CLIP_ENTS,   /* entities SV_ClipToLinks looked at for a trace (the area-node candidates) */
	XBOX_FT_W_G_LINKS,       /* SV_LinkEdict calls */
	XBOX_FT_W_G_PUSH_TESTS,  /* SV_TestEntityPosition calls (SV_PushMove/SV_PushRotate's per-entity tests) */
	XBOX_FT_W_G_WALKMOVES,   /* the DLL's WalkMove/MoveToOrigin/DropToFloor calls */
	XBOX_FT_W_G_FIND_VISITS, /* edicts the DLL's FindEntityByString/FindEntityInSphere scans looked at */
	XBOX_FT_W_G_THINK_KC,    /* time: kilocycles inside the DLL's Think (outermost), with what it calls */
	XBOX_FT_W_G_TOUCH_KC,    /* time: kilocycles inside the DLL's Touch (outermost) */
	XBOX_FT_W_G_MOVE_KC,     /* time: kilocycles inside SV_Move (outermost) */
	XBOX_FT_W_G_WORST_KC,    /* time: the slowest single entity's SV_Physics_Entity, kilocycles */
	XBOX_FT_W_G_WORST_CLS,   /* that entity's classname: 1 + its index in the CLASSES chunk (0: none) */
	XBOX_FT_W_G_WORST_ENT,   /* that entity's edict number */
	XBOX_FT_W_G_PUSH_REPLAYS,/* of g_push_tests, the traces #149 replays at the end of a push loop */
	/* First-use loads (2026-10-06, x9-perf-stalls): what a frame read
	   off the disc on the main thread because something used it for the
	   first time. Counts and sizes are exact in xemu; the time is the
	   console's only on the console. */
	XBOX_FT_W_SND_LOADS,     /* sounds S_LoadSound loaded (not cached: a sentence's word, an unprecached name) */
	XBOX_FT_W_SND_LOAD_KB,   /* their decoded size, KiB (each load rounded up) */
	XBOX_FT_W_SND_LOAD_KC,   /* time: kilocycles in those loads, wherever they ran (k_cl_read, k_snd_mix, k_cl_dll...) */
	XBOX_FT_W_TEX_LOADS,     /* textures ref/nv2a created from an image (GL_LoadTexture, GL_LoadTextureFromBuffer) */
	XBOX_FT_W_TEX_LOAD_KB,   /* their texture storage, KiB (each rounded up) */
	/* time: kilocycles in CL_ParseNetMessage's parse function - every
	   server message of the frame's packets - so k_cl_read minus it is
	   what CL_ReadPackets did around the parser (netchan, fragment
	   reassembly, resource requests). A counter, not a span: the parse
	   spans k_cl_pkt_ents and k_cl_clientdata are older and a span's
	   parent must come before it. A k_cl_read stall with no slow-message
	   note is then many small messages (this is high) or work outside
	   the parser (this is low). */
	XBOX_FT_W_CL_PARSE_KC,
	/* The deferred savegame write (2026-10-06, #153). save_pump_kb is the
	   .sav bytes written in the frame, KiB (each chunk rounded up), by
	   k_save_pump or a flush. save_flush_kc is the time of a flush - a
	   pending save completed at once because something was about to read
	   or change the save folder (a level change, a load, another save,
	   the menu's save list, quitting) - wherever it ran, so a counter. */
	XBOX_FT_W_SAVE_PUMP_KB,
	XBOX_FT_W_SAVE_FLUSH_KC,
	/* Player movement (2026-10-06, x9-perf-pmove, xbox_frametime_pmove.h):
	   the server's SV_RunCmd and the client's prediction both run the
	   game's PM_Move. The engine queries (traces, position tests, point
	   contents, texture traces) are counted only while one of the two
	   PM_Move calls runs, so the client's weapon and event traces are not
	   in them. Each _SV counter has its _CL twin right after it. Counts,
	   exact in xemu - except the _KC times. */
	XBOX_FT_W_PM_SV_CMDS,    /* the server's PM_Move calls (one per usercmd, two for a split one) */
	XBOX_FT_W_PM_CL_CMDS,    /* the client's PM_Move calls from CL_CreateCmd's prediction */
	XBOX_FT_W_PM_CL_REDO,    /* the client's PM_Move calls from CL_RedoPrediction (repredicting) */
	XBOX_FT_W_PM_CL_SOLIDS,  /* CL_SetSolidEntities calls (the client's physent list rebuilt) */
	XBOX_FT_W_PM_SV_PHYSENTS,/* numphysent summed over the server's moves */
	XBOX_FT_W_PM_CL_PHYSENTS,/* the same over the client's moves */
	XBOX_FT_W_PM_SV_VISENTS, /* numvisent summed over the server's moves */
	XBOX_FT_W_PM_CL_VISENTS,
	XBOX_FT_W_PM_SV_TRACES,  /* PM_PlayerTraceExt calls inside the server's PM_Move (a position test's own trace included) */
	XBOX_FT_W_PM_CL_TRACES,
	XBOX_FT_W_PM_SV_HULLS,   /* physents those traces ran a hull check against */
	XBOX_FT_W_PM_CL_HULLS,
	XBOX_FT_W_PM_SV_TESTS,   /* PM_TestPlayerPosition calls */
	XBOX_FT_W_PM_CL_TESTS,
	XBOX_FT_W_PM_SV_PTESTS,  /* physents its own loop point-tested (after its trace) */
	XBOX_FT_W_PM_CL_PTESTS,
	XBOX_FT_W_PM_SV_PCONT,   /* PM_PointContents calls */
	XBOX_FT_W_PM_CL_PCONT,
	XBOX_FT_W_PM_SV_TEX,     /* PM_TraceTexture/PM_TraceSurfacePmove calls */
	XBOX_FT_W_PM_CL_TEX,
	XBOX_FT_W_PM_SV_SETUP_KC,/* time: SV_SetupPMove - the player's state and the physent lists */
	XBOX_FT_W_PM_SV_MOVE_KC, /* time: the game DLL's PM_Move on the server */
	XBOX_FT_W_PM_SV_QUERY_KC,/* time: of that, inside the engine queries above (outermost) */
	XBOX_FT_W_PM_SV_THINK_KC,/* time: CmdStart, PlayerPreThink, SV_PlayerRunThink, PlayerPostThink, CmdEnd */
	XBOX_FT_W_PM_SV_TOUCH_KC,/* time: SV_LinkEdict of the player and its touches after the move */
	XBOX_FT_W_PM_CL_SOLID_KC,/* time: CL_SetSolidEntities, wherever it runs (CL_CreateCmd, the packet parse) */
	XBOX_FT_W_PM_CL_MOVE_KC, /* time: the client DLL's PM_Move (both kinds of call) */
	XBOX_FT_W_PM_CL_QUERY_KC,/* time: of that, inside the engine queries (outermost) */
	XBOX_FT_W_PM_CL_POST_KC, /* time: the client DLL's PostRunCmd (weapon prediction) */
	XBOX_FT_W_PM_SV_CULLS,   /* physents a trace or a position test skipped: no solid leaf near (#154) */
	XBOX_FT_W_PM_CL_CULLS,
	/* Client entities (2026-10-07, x9-perf-clents,
	   xbox_frametime_clents.h): the client's entity decode
	   (k_cl_pkt_ents) and its visible-entity link (k_cl_emit) broken
	   down. Counts, exact in xemu - except the _KC times. */
	XBOX_FT_W_CE_ENTS,       /* entities in the frames CL_ParsePacketEntities built (a valid frame's num_entities) */
	XBOX_FT_W_CE_DELTAS,     /* MSG_ReadDeltaEntity calls that walked the fields (every caller: a frame's deltas, baselines) */
	XBOX_FT_W_CE_NOCHANGE,   /* of those, deltas with no field bit set */
	XBOX_FT_W_CE_FIELDS,     /* fields those deltas decoded (change bit set) */
	XBOX_FT_W_CE_VISENTS,    /* entities CL_LinkPlayers/CL_LinkPacketEntities passed to CL_AddVisibleEntity that it accepted */
	XBOX_FT_W_CE_DELTA_KC,   /* time: MSG_ReadDeltaEntity from CL_DeltaEntity (a frame's deltas) */
	XBOX_FT_W_CE_FIELDS_KC,  /* time: the field loop of every MSG_ReadDeltaEntity (in play: of ce_delta_kc; baselines add to it) */
	XBOX_FT_W_CE_PROCESS_KC, /* time: CL_ProcessPacket - each entity's state shuffle, CL_ProcessEntityUpdate, latched state */
	XBOX_FT_W_CE_LINK_KC,    /* time: in k_cl_emit, CL_LinkPlayers + CL_LinkPacketEntities */
	XBOX_FT_W_CE_ADD_KC,     /* time: of that, CL_AddVisibleEntity - the client's AddEntity, R_AddEntity, entity effects */
	XBOX_FT_W_CE_TENTS_KC,   /* time: in k_cl_emit, the client's CreateEntities and CL_TempEntUpdate */
	XBOX_FT_W_CE_EVENTS_KC,  /* time: in k_cl_emit, CL_FireEvents */
	XBOX_FT_W_CE_PITCH_KC,   /* time: in k_cl_emit, CL_SetIdealPitch - up to six CL_TraceLine calls down ahead of the player */
	XBOX_FT_W_CE_SOLID_SKIPS,/* of pm_cl_solids, the CL_SetSolidEntities calls that found the lists current (#156) */
	XBOX_FT_W_TEX_FAST,      /* of tex_loads, the menu's glyph atlases ref/nv2a uploaded straight from their cached TGA (2026-10-08, #157) */
	/* Particles (2026-10-08, x9-perf-particles, plan item R6): where
	   k_efx's particle pass goes. CL_DrawParticles interleaves its draw
	   with the engine's per-particle physics (CL_ThinkParticle), so the
	   draw is part_kc minus part_think_kc. */
	XBOX_FT_W_PART_KC,       /* time: kilocycles in ref/nv2a's CL_DrawParticles, its CL_ThinkParticle calls included */
	XBOX_FT_W_PART_THINK_KC, /* time: of that, the CL_ThinkParticle calls (one rdtsc pair per particle, summed before rounding) */
	/* Beams and tracers (2026-10-08, x9-perf-efx): the rest of k_efx.
	   CL_DrawBeams draws the server's beam entities (env_beam, env_laser:
	   ET_BEAM) and the temp-entity beams (gauss, egon, tripmine lasers,
	   TE_BEAM* messages). `beams` adds one for every beam entity
	   CL_DrawBeams submits and one more for every beam R_BeamDraw gets
	   through to the end of, as ref/gl's r_stats does (gl_beams.c:1226,
	   :1071), so the beams actually drawn are beams - beam_ents.
	   Per-beam times are summed in ticks and rounded once per pass. */
	XBOX_FT_W_BEAM_KC,       /* time: ref/nv2a's CL_DrawBeams, both passes (beam entities and temp-entity beams) */
	XBOX_FT_W_BEAM_NOISE_KC, /* time: of that, FracNoise/SineNoise in R_BeamDraw (R_DrawRing's refills are in beam_geom_kc) */
	XBOX_FT_W_BEAM_GEOM_KC,  /* time: of that, each beam's Begin..End - R_DrawSegs and its siblings, the TriAPI and its repeat splitter */
	XBOX_FT_W_BEAM_ENTS,     /* server beam entities CL_DrawBeams submitted (R_BeamDrawCustomEntity calls, culled ones included) */
	XBOX_FT_W_BEAM_PRIMS,    /* primitives the TriAPI assembled from beam vertices (a strip's triangles, a follow beam's quads) */
	XBOX_FT_W_BEAM_PIECES,   /* primitives appended for them after the repeat splitter (#133) cut them at texture-cell boundaries */
	XBOX_FT_W_TRACERS,       /* tracers drawn (not culled) */
	XBOX_FT_W_TRACER_KC,     /* time: ref/nv2a's CL_DrawTracers, the tracers' own movement included */
	XBOX_FT_W_TRACER_FAILS,  /* CL_DrawTracers passes that had tracers but could not bind the dot sprite, so drew none */
	/* Chrome studio meshes (2026-10-09, x9-perf-chrome-vp, divergence
	   #170): STUDIO_NF_CHROME meshes of a submodel the GPU studio path
	   (#143) would otherwise draw - its geometry built, the mesh's bones
	   in its palette, its coordinate format the built one. chrome_kc is
	   the CPU time they cost: their draws, plus the submodel's CPU
	   transform and lighting when they are its only CPU meshes. */
	XBOX_FT_W_CHROME_MESHES, /* such chrome meshes the CPU path drew (R_StudioDrawChromeMesh) */
	XBOX_FT_W_CHROME_VERTS,  /* their tricmd vertices */
	XBOX_FT_W_CHROME_KC,     /* time: kilocycles of CPU studio work only those meshes caused (above) */
	XBOX_FT_W_CHROME_GPU,    /* chrome meshes the vertex unit drew (vs_studio's chrome variant) */
	/* The baked music's file reads (2026-10-10, x9-audio-music-refill,
	   divergence #172), counted in engine/platform/xbox/
	   xbox_music_stream.c: k_snd_music is music_read_kc (the stream's
	   reads: decode, chunk copies and these file reads) plus upstream's
	   copy into the raw channel (S_RawEntSamples); music_io_kc is the
	   part spent in the file reads' calls, waits for the disk included. */
	XBOX_FT_W_MUSIC_READS,   /* read-ahead windows read (asynchronous or not) */
	XBOX_FT_W_MUSIC_READ_KB, /* KiB those reads asked for */
	XBOX_FT_W_MUSIC_ASYNC,   /* ... of which the kernel queued and the frame went on without */
	XBOX_FT_W_MUSIC_WAITS,   /* reads the frame blocked on: synchronous ones, and queued ones not landed when needed */
	XBOX_FT_W_MUSIC_IO_KC,   /* time: the music's file-read calls, issue, completion and waits */
	XBOX_FT_W_MUSIC_READ_KC, /* time: the music stream's reads (Stream_ReadXwav), the above included */
	/* The audio output (2026-10-11, x9-audio-mix48, divergence #173),
	   sampled at the frame's close from engine/platform/xbox/
	   xbox_audio_hal.c (XAudioR3D_ThreadStats): what SDL's audio thread
	   did over the frame's whole period. That thread runs at
	   time-critical priority and its time lands in whichever span it
	   interrupts. */
	XBOX_FT_W_SND_THR_KC,    /* time: SDL's audio thread, from SDL_SoundCallback's entry to its buffer's XAudioProvideSamples - the callback's copy and, mixing at 44.1 kHz, SDL's conversion to 48 kHz */
	XBOX_FT_W_SND_THR_RUNS,  /* callbacks whose buffer reached the hardware in the frame (snd_thr_kc's intervals) */
	XBOX_FT_W_SND_BUFS,      /* 1024-frame buffers handed to the AC97 (21.3 ms each at 48 kHz) */
	XBOX_FT_W_SND_UNDERRUNS, /* LVBCI interrupts from the PCM-out engine: it played its last valid buffer out */
	XBOX_FT_W_SND_DMA_KC,    /* time: SNDDMA_BeginPainting + SNDDMA_Submit on the main thread - the SDL device lock (a wait for the audio thread's callback), inside k_snd/k_snd_extra */
	XBOX_FT_W_SND_RATE,      /* the mixer's output rate this frame, Hz (r3d_snd_rate: 48000, or 44100 for upstream's) */
	/* The output that played the frame (2026-10-15, x9-audio-al-output,
	   divergence #178): with r3d_snd_backend al the counters above are
	   OpenAL's output thread (Xbox_Snd_ThreadStats), so a session says
	   which side of an A/B it is, whatever its label claims. */
	XBOX_FT_W_SND_BACKEND,   /* 0: SDL's audio thread, 1: OpenAL on the APU (r3d_snd_backend; 0 too when OpenAL did not open) */
	/* The dialogue streams (2026-10-13, x9-audio-vox-stream, divergence
	   #177), counted in engine/platform/xbox/xbox_vox_stream.c. A start is
	   a sentence word or a precached speech sound that streamed; one the
	   stream left to upstream's whole load is a `small` (by size only) or
	   a fallback (named in a `vox` note). */
	XBOX_FT_W_VOX_STARTS,    /* sounds that started streaming */
	XBOX_FT_W_VOX_SLOTS,     /* the most stream slots in use at once in the frame (a state, not a count) */
	XBOX_FT_W_VOX_OPENS,     /* files the streams opened (each a path search and an overlapped open) */
	XBOX_FT_W_VOX_OPEN_KC,   /* time: those opens, the path search included */
	XBOX_FT_W_VOX_READS,     /* reads issued (unit, tail or loop blocks) */
	XBOX_FT_W_VOX_READ_KB,   /* KiB they asked for */
	XBOX_FT_W_VOX_ASYNC,     /* ... of which the kernel queued and the frame went on without */
	XBOX_FT_W_VOX_SYNC,      /* ... of which were read at once: a first word's head and tail, a jump, a missing unit */
	XBOX_FT_W_VOX_WAITS,     /* queued reads the frame had to block on (not landed when needed) */
	XBOX_FT_W_VOX_WAIT_KC,   /* time: those waits */
	XBOX_FT_W_VOX_SEEKS,     /* windows moved by a jump (a restore, a loop without its units) */
	XBOX_FT_W_VOX_IO_KC,     /* time: every file call of the streams (opens, reads, waits, closes) */
	XBOX_FT_W_VOX_FIRST_KC,  /* time: the starts that streamed, whole - the first word's wait */
	XBOX_FT_W_VOX_PF_HITS,   /* word starts that found their prefetch landed (no wait, no read) */
	XBOX_FT_W_VOX_SMALL,     /* candidates left to S_LoadSound for their size (U bytes of data or less) */
	XBOX_FT_W_VOX_FALLBACKS, /* candidates left to upstream for another reason, and windows that fell back to a whole load */
	XBOX_FT_W_VOX_WINDOW_KB, /* KiB the streams hold at the frame's close - slots, loop units, header entries, whole fallbacks (a state) */
	XBOX_FT_W_END
};

#define XBOX_FT_W_COUNT XBOX_FT_W_END

/* The COUNTERS chunk's names, indexed by XBOX_FT_W_*; each fits in
   XBOX_FT_COUNTER_NAME_BYTES with its NUL. The spans are named k_* (the
   perf report's `sv_game_kcyc` is `k_sv_game`; `client_studio_kcyc`
   would not fit in 15 characters). */
extern const char *const xbox_ft_work_names[XBOX_FT_W_COUNT];

/* Each span's parent span (XBOX_FT_S_*), -1 for one directly inside
   Host_Frame. Every parent comes before its children. */
extern const int8_t xbox_ft_span_parent[XBOX_FT_S_COUNT];

/* Rounds a span's accumulated ticks to the frame record's kilocycles
   (saturating: 2^32 kilocycles is ~1.6 hours at 733 MHz). */
uint32_t xbox_ft_span_kcyc( uint64_t ticks );
/* From span kilocycles indexed by XBOX_FT_S_* (one frame's, or a sum
   over frames): each span's own time into `self` - its kilocycles minus
   its children's, 0 if they add up to more (a child that also ran
   outside its parent, e.g. a skin read outside a studio draw) - and
   returns the sum of the top-level spans. */
uint64_t xbox_ft_span_self( const uint64_t *span, uint64_t *self );

/* Frame flags. */
#define XBOX_FT_F_ACTIVE     0x0001u /* in game: cls.state == ca_active, signon complete */
#define XBOX_FT_F_MENU       0x0002u /* a menu, the console or a pause has the screen */
#define XBOX_FT_F_FLUSH      0x0004u /* this frame wrote the file (its time includes the write) */
#define XBOX_FT_F_SATURATED  0x0008u /* a tick count did not fit in 32 bits */
#define XBOX_FT_F_OVERLAY    0x0010u /* the overlay was drawn in this frame */
/* This frame's Host_Frame queued a host state change - a changelevel, a
   map or savegame load, a new game, a disconnect (GameState->nextstate
   left != STATE_RUNFRAME, engine/common/host_state.c). The state machine
   runs it at the start of the next COM_Frame, before that frame's
   Host_Frame (host_state.c:205-224), so the whole load lands in THIS
   frame's time between host frames (total - host), while the frame still
   started in play. A reserved bit until 2026-10-01: older files leave it
   clear, and tools/frametime-report.py finds those frames by their shape
   instead. Not a format change - readers ignore flags they do not know. */
#define XBOX_FT_F_LOAD       0x0020u

#define XBOX_FT_MAP_F_NO_LOAD_START 0x0001u
/* v3: the map continues from the previous segment (the size cap closed
   it mid-map). Written with NO_LOAD_START: there is no load to time. */
#define XBOX_FT_MAP_F_CONTINUED     0x0002u

/* The overlay's and the report's two guidelines, microseconds. A frame
   counts as "over" a guideline when it is strictly longer (a frame at
   exactly the 60 Hz period made the refresh). */
#define XBOX_FT_GUIDE_60_US  16667u
#define XBOX_FT_GUIDE_30_US  33333u

/* Histogram for the percentile: 0.25 ms bins up to 250 ms; slower frames
   land in the last bin (only its lower edge is known). */
#define XBOX_FT_HIST_BIN_US  250u
#define XBOX_FT_HIST_BINS    1000u

typedef struct xbox_ft_frame_s
{
	uint32_t seq;
	uint32_t total;
	uint32_t host;
	uint32_t gpu;
	uint32_t vblank;
	uint32_t swap;
	uint32_t overlay;
	uint16_t flags;
	uint16_t reserved;
	uint32_t work[XBOX_FT_W_COUNT];
} xbox_ft_frame_t;

/* The in-memory ring. `frames` is caller-owned storage of `capacity`
   entries (the recorder allocates it only while recording). `next_seq`
   numbers pushes; `flushed_seq` is the first sequence number not yet in
   the file. */
typedef struct xbox_ft_ring_s
{
	xbox_ft_frame_t *frames;
	uint32_t capacity;
	uint32_t next_seq;
	uint32_t flushed_seq;
	uint32_t lost;          /* frames overwritten before they were flushed */
} xbox_ft_ring_t;

void xbox_ft_ring_init( xbox_ft_ring_t *ring, xbox_ft_frame_t *storage, uint32_t capacity );
/* Stores `frame` with the next sequence number (written into it). When
   the oldest unflushed frame is overwritten it is counted in `lost` and
   flushed_seq moves past it. */
void xbox_ft_ring_push( xbox_ft_ring_t *ring, xbox_ft_frame_t *frame );
uint32_t xbox_ft_ring_unflushed( const xbox_ft_ring_t *ring );
/* The unflushed frames as at most two contiguous spans of the storage
   (the second is non-empty only when they wrap). Returns the span count. */
uint32_t xbox_ft_ring_unflushed_spans( const xbox_ft_ring_t *ring, const xbox_ft_frame_t **span0, uint32_t *count0, const xbox_ft_frame_t **span1, uint32_t *count1 );
void xbox_ft_ring_mark_flushed( xbox_ft_ring_t *ring );
/* The frame `back` pushes ago (0 = the latest), or NULL if the ring no
   longer holds it. */
const xbox_ft_frame_t *xbox_ft_ring_recent( const xbox_ft_ring_t *ring, uint32_t back );

/* Ticks to microseconds at `tsc_hz`, and a 64-bit tick span to a
   saturating 32-bit field. */
uint32_t xbox_ft_ticks_to_us( uint64_t ticks, uint64_t tsc_hz );
uint32_t xbox_ft_saturate( uint64_t ticks, uint16_t *flags );
/* CPU ticks of a frame: host minus its waits, never negative. */
uint32_t xbox_ft_frame_cpu( const xbox_ft_frame_t *frame );

/* Running statistics over a set of frames (the overlay keeps one per
   map), in microseconds. */
typedef struct xbox_ft_stats_s
{
	uint32_t frames;
	uint64_t total_us;
	uint64_t cpu_us;
	uint64_t gpu_us;
	uint32_t over_60;       /* frames longer than 16.7 ms */
	uint32_t over_30;       /* frames longer than 33.3 ms */
	uint32_t worst_us;
	uint32_t hist[XBOX_FT_HIST_BINS];
} xbox_ft_stats_t;

void xbox_ft_stats_reset( xbox_ft_stats_t *stats );
void xbox_ft_stats_add( xbox_ft_stats_t *stats, uint32_t total_us, uint32_t cpu_us, uint32_t gpu_us );
/* The frame time at or under which `permille`/1000 of the frames fall,
   from the histogram - the upper edge of the bin that crosses the rank
   (at most one bin, 0.25 ms, pessimistic). "1% low fps" is 1e6 over
   xbox_ft_stats_percentile_us( stats, 990 ): the frame rate the slowest
   1% of frames fall below. 0 with no frames. */
uint32_t xbox_ft_stats_percentile_us( const xbox_ft_stats_t *stats, uint32_t permille );

/* Byte encoders. Each writes exactly the bytes documented above into
   `out` (which must have room) and returns the count written.
   `counters` is how many work counters each frame record carries: the
   recorder passes XBOX_FT_W_COUNT; 0 writes a version-1 file (no
   COUNTERS chunk, 32-byte records). The header, the frames chunk heads
   and every frame of one file must be given the same value. */
size_t xbox_ft_frame_bytes( uint32_t counters );
size_t xbox_ft_encode_header( uint8_t *out, uint32_t ring_frames, uint64_t tsc_hz, uint64_t qpc_hz, uint64_t tsc_open, uint64_t qpc_open, uint32_t mode, const char *build, uint32_t counters );
/* The COUNTERS chunk: chunk head, count, then xbox_ft_work_names[0..counters). */
size_t xbox_ft_encode_counters( uint8_t *out, uint32_t counters );
/* The SPANS chunk, for the spans among the first `counters` counters:
   nothing (0 bytes) when `counters` stops before the first span. At most
   XBOX_FT_SPANS_CHUNK_BYTES. */
#define XBOX_FT_SPANS_CHUNK_BYTES ( XBOX_FT_CHUNK_BYTES + 4u + XBOX_FT_S_COUNT * 2u * XBOX_FT_COUNTER_NAME_BYTES )
size_t xbox_ft_encode_spans( uint8_t *out, uint32_t counters );
size_t xbox_ft_encode_frames_chunk_head( uint8_t *out, uint32_t first_seq, uint32_t count, uint32_t counters );
size_t xbox_ft_encode_frame( uint8_t *out, const xbox_ft_frame_t *frame, uint32_t counters );
size_t xbox_ft_encode_map( uint8_t *out, uint64_t tsc, uint64_t load_ticks, uint32_t free_before, uint32_t free_after, uint32_t total_pages, uint32_t flags, const char *name );
size_t xbox_ft_encode_calib( uint8_t *out, uint64_t tsc, uint64_t qpc );
size_t xbox_ft_encode_end( uint8_t *out, uint64_t tsc, uint32_t lost );

/* The CLASSES chunk (2026-10-05): one per-classname row of the server
   game frame, in ticks; the encoder rounds them to kilocycles. */
#define XBOX_FT_CLASS_NAME_BYTES 32u
#define XBOX_FT_CLASS_BYTES      ( XBOX_FT_CLASS_NAME_BYTES + 7u * 4u )
#define XBOX_FT_CLASSES_MAX      192u /* distinct classnames per recording; later ones share the last row */

typedef struct xbox_ft_class_s
{
	char name[XBOX_FT_CLASS_NAME_BYTES]; /* NUL-terminated, truncated */
	uint32_t movetype;
	uint32_t runs;
	uint32_t thinks;
	uint32_t moves;
	uint64_t phys;      /* ticks in SV_Physics_Entity */
	uint64_t think;     /* ticks in the DLL's Think */
	uint64_t worst;     /* the slowest single SV_Physics_Entity, ticks */
} xbox_ft_class_t;

/* Writes the chunk for classes[0..count) - XBOX_FT_CHUNK_BYTES + 4 +
   count x XBOX_FT_CLASS_BYTES bytes - and returns that count. */
size_t xbox_ft_encode_classes( uint8_t *out, const xbox_ft_class_t *classes, uint32_t count );

/* The NOTES chunk (2026-10-06, x9-perf-stalls): named one-off events, so a
   stall that the spans put in k_cl_read or a first-use load says what it
   was. A note carries the sequence number of the frame it happened in
   (the frame in progress: the ring's next_seq), its kind, its time in
   kilocycles (0 when the spans were off), a size in bytes and a name.
   Kinds:
     XBOX_FT_NOTE_SOUND  a sound S_LoadSound loaded in play (cls.state ==
                         ca_active, the client-state test of upstream's
                         s_warn_late_precache warning, s_load.c:119 in
                         the reference): name = the sound, bytes = its
                         decoded size
     XBOX_FT_NOTE_MSG    one server message that took the client at least
                         XBOX_FT_NOTE_MSG_KC to parse: name = its svc_* or
                         user-message name, bytes = its size in the packet
     XBOX_FT_NOTE_MENU   the end of a map load ran the menu's font restore,
                         or left the fonts released for lack of memory
                         (xbox_menu_fonts.c, divergences.md #127): name
                         says which and the free memory before, bytes =
                         what the restore took from free memory
     XBOX_FT_NOTE_LOAD   (2026-10-08, x9-perf-loadtime, divergences.md
                         #162) one stage of a level load (SV_SpawnServer,
                         R_NewMap...), or the total of one kind of
                         resource loaded since the last map start (an
                         "acc_" stage: every studio model, every sound):
                         kilocycles = the stage's whole time, wall clock
                         (disk waits included); bytes = what it read
                         from files; name = xbox_ft_load_note_name below,
                         the stage then its file-layer counters
     XBOX_FT_NOTE_MUSIC  (2026-10-10, x9-audio-music-bake, divergences.md
                         #171) a music track opened (FS_OpenStream): name
                         = "xwav adpcm|pcm <file>" when the baked track
                         plays, "mp3 (<why>) <file>" when a baked one
                         was there but the mp3 plays; kilocycles = the
                         open, bytes = the .xwav's size. A track with no
                         .xwav (main, or not baked) leaves no note.
     XBOX_FT_NOTE_VOX    (2026-10-13, x9-audio-vox-stream, divergences.md
                         #177) a dialogue sound the stream left to
                         upstream's whole load, or a window that fell
                         back to one: name = "<reason> <sound>" (nofile:
                         not a plain file on disk; open, io: the kernel
                         refused; slot: none free; format: outside the
                         class; chunks: a chunk header past what the
                         stream reads; big: a request wider than the
                         window); kilocycles = the decision.
   From the same build a sound note's name is xbox_ft_sound_note_name's
   below (the load's caller and its file calls); earlier ones hold the
   sound's name alone.
   The recorder keeps up to XBOX_FT_NOTES_MAX between flushes and counts
   the rest as dropped. */
#define XBOX_FT_CHUNK_NOTES      8u
#define XBOX_FT_NOTE_SOUND       1u
#define XBOX_FT_NOTE_MSG         2u
#define XBOX_FT_NOTE_MENU        3u
#define XBOX_FT_NOTE_LOAD        4u
#define XBOX_FT_NOTE_MUSIC       5u
#define XBOX_FT_NOTE_VOX         6u
#define XBOX_FT_NOTE_NAME_BYTES  48u
#define XBOX_FT_NOTE_BYTES       ( 16u + XBOX_FT_NOTE_NAME_BYTES )
#define XBOX_FT_NOTES_MAX        64u
#define XBOX_FT_NOTE_MSG_KC      3667u /* 5 ms at 733 MHz */
#define XBOX_FT_NOTES_CHUNK_BYTES( n ) ( XBOX_FT_CHUNK_BYTES + 8u + ( n ) * XBOX_FT_NOTE_BYTES )

typedef struct xbox_ft_note_s
{
	uint32_t seq;
	uint32_t kind;
	uint32_t kcyc;
	uint32_t bytes;
	char name[XBOX_FT_NOTE_NAME_BYTES]; /* NUL-terminated, truncated */
} xbox_ft_note_t;

/* Writes the chunk for notes[0..count) - u32 count, u32 dropped, then
   count x { u32 seq, u32 kind, u32 kilocycles, u32 bytes, char[48]
   name } - XBOX_FT_NOTES_CHUNK_BYTES( count ) bytes, and returns that. */
size_t xbox_ft_encode_notes( uint8_t *out, const xbox_ft_note_t *notes, uint32_t count, uint32_t dropped );

/* A load note's name (XBOX_FT_NOTE_LOAD):
     "<stage> <io>,<opens>,<reads>,<seeks>,<probes>,<writes>,<meta>[,<count>]"
   io = kilocycles inside the file layer's kernel calls (compat/xbox/
   xbox_io_stats.h: open, read, write, lseek, stat/access, close and the
   metadata calls), so the stage's CPU time is its kilocycles minus io;
   opens, reads, seeks and writes count those calls, probes the
   stat/access existence tests, meta the rest (flushes, directory changes
   and scans); ",<count>" only on an accumulated stage (count != 0): how
   many loads it adds up. Positional and unlabelled so a console-sized
   load (7-digit io, 4-digit probes) still leaves 14 characters of the
   48-byte field for the stage (console-performance review). The numbers
   are kept whole: when the name does not fit `size`
   (XBOX_FT_NOTE_NAME_BYTES in a note), the stage is cut first, and the
   counters only when they alone do not fit. Writes out[] NUL-terminated
   (size >= 1) and returns its length. */
size_t xbox_ft_load_note_name( char *out, size_t size, const char *stage, uint32_t io_kc, uint32_t opens,
	uint32_t reads, uint32_t seeks, uint32_t probes, uint32_t writes, uint32_t meta, uint32_t count );

/* A sound note's name (XBOX_FT_NOTE_SOUND, divergence #177):
     "<sound> <tag>:<io>,<opens>,<reads>,<seeks>,<probes>"
   tag = why S_LoadSound ran (engine/platform/xbox/xbox_vox_stream.h,
   R3D_VOX_TAG_*: w a word starting, t SND_GetChannelTimeLeft, s a sound
   started, m the mixer's reload, r a registration, o anything else); the
   rest as a load note's, so the load's open and path search (Stage A's
   floor) is told from its transfer. The numbers are kept whole: the sound's
   name is cut first. Writes out[] NUL-terminated (size >= 1) and returns
   its length. */
size_t xbox_ft_sound_note_name( char *out, size_t size, const char *sound, char tag, uint32_t io_kc, uint32_t opens,
	uint32_t reads, uint32_t seeks, uint32_t probes );

/* The accumulated load stages: every load of one kind between two map
   starts, summed into one note at the second (XBOX_FT_NOTE_LOAD,
   "acc_" + xbox_ft_load_acc_names[i]). */
enum
{
	XBOX_FT_LACC_STUDIO,    /* Mod_LoadModel of a studio model (.mdl) */
	XBOX_FT_LACC_SPRITE,    /* ... of a sprite (.spr) */
	XBOX_FT_LACC_BRUSH,     /* ... of a brush model: the world */
	XBOX_FT_LACC_OTHER,     /* ... of anything else (a failed load returns before it is counted) */
	XBOX_FT_LACC_SOUND,     /* S_LoadSound outside play (the precache) */
	XBOX_FT_LACC_BSPDECAL,  /* one svc_bspdecal message parsed */
	XBOX_FT_LACC_COUNT
};
extern const char *const xbox_ft_load_acc_names[XBOX_FT_LACC_COUNT];

/* Sessions and segments (version 3, 2026-10-07). A SESSION is one run of
   the recorder: from host_frametime_analysis going non-zero to it going
   back to 0 or the game quitting. It is written as SEGMENTS, one file
   each, which the recorder rotates to at each map start (the new map's
   MAP chunk opens the new segment) and when a segment passes the size
   cap at a flush (the new one opens with a MAP chunk flagged
   XBOX_FT_MAP_F_CONTINUED). Each segment has its own header, COUNTERS
   and SPANS chunks, so it parses on its own; a crash loses only the end
   of the segment being written, and a fetch can take just the new ones.

   The file name, at most XBOX_FT_FILE_NAME_MAX characters (FATX's limit
   for one name):
     ft_<label>_<session>_<seq>.bin
       "ft_"       3   kept from the single-file names, so a listing or a
                       fetch filter still finds every recording
       label       1-22 (XBOX_FT_LABEL_MAX) host_frametime_label sanitised
       "_"         1
       session     8   the id's first 4 bytes, lowercase hex
       "_"         1
       seq         3   the segment number, decimal, zero-padded
                       (XBOX_FT_SEGMENT_SEQ_MAX segments per session)
       ".bin"      4
   = 20 + label, so a 22-character label makes exactly 42. The label may
   itself contain '_': a reader splits the name from the right. */
#define XBOX_FT_SESSION_ID_BYTES 16u
#define XBOX_FT_SESSION_HEX      8u    /* hex digits of the id in a file name */
#define XBOX_FT_LABEL_BYTES      32u   /* the header's label field */
#define XBOX_FT_LABEL_MAX        22u   /* label characters that fit the name */
#define XBOX_FT_SEGMENT_SEQ_MAX  999u  /* three digits */
#define XBOX_FT_FILE_NAME_MAX    42u   /* FATX: characters in one file name */
#define XBOX_FT_LABEL_EMPTY      "untagged"

/* Why a segment began (header) - and, in an END chunk, why the file
   ended: the reason the NEXT segment begins, 0 when the session ended. */
#define XBOX_FT_SEG_SESSION      0u    /* the session started (END: it stopped) */
#define XBOX_FT_SEG_MAP          1u    /* a map started */
#define XBOX_FT_SEG_SIZE         2u    /* the previous segment reached the size cap */

typedef struct xbox_ft_segment_s
{
	uint8_t session[XBOX_FT_SESSION_ID_BYTES];
	uint64_t systime;           /* KeQuerySystemTime at the session's start */
	char label[XBOX_FT_LABEL_BYTES]; /* sanitised (xbox_ft_label_sanitise) */
	uint32_t seq;               /* this segment's number in the session */
	uint32_t reason;            /* XBOX_FT_SEG_*: why it began */
	uint32_t first_frame;       /* the sequence number of its first frame */
	uint64_t tsc_open;          /* TSC when it opened */
} xbox_ft_segment_t;

/* Writes `label` (any string, NULL allowed) as a name part into `out`
   (room for XBOX_FT_LABEL_MAX + 1): letters, digits, '-' and '_' kept;
   every run of other characters becomes one '-'; leading and trailing
   '-'/'_' dropped; cut to XBOX_FT_LABEL_MAX (trailing '-'/'_' dropped
   again); XBOX_FT_LABEL_EMPTY when nothing is left. Returns its length. */
size_t xbox_ft_label_sanitise( const char *label, char *out );
/* A session id from `n` values that differ between sessions (the kernel
   clock, the TSC, the performance counter...): two 64-bit mixes of all of
   them, with the version (4) and variant bits a random UUID has. */
void xbox_ft_session_id( const uint64_t *inputs, uint32_t n, uint8_t id[XBOX_FT_SESSION_ID_BYTES] );
/* The segment's file name, without a directory, into `out` (room for
   XBOX_FT_FILE_NAME_MAX + 1): ft_<label>_<8 hex>_<seq>.bin, `label`
   sanitised again (so any string is safe) and `seq` clamped to
   XBOX_FT_SEGMENT_SEQ_MAX. Returns its length, at most
   XBOX_FT_FILE_NAME_MAX. */
size_t xbox_ft_segment_name( char *out, const char *label, const uint8_t id[XBOX_FT_SESSION_ID_BYTES], uint32_t seq );
/* Whether to close the segment being written and open the next, decided
   right after a flush: at a map start when the segment already holds
   frames (XBOX_FT_SEG_MAP - a map that starts in an empty segment, the
   first one after recording began mid-load, stays in it), otherwise when
   it holds `cap_bytes` or more (XBOX_FT_SEG_SIZE; `cap_bytes` 0: no cap).
   0: keep writing it. */
uint32_t xbox_ft_segment_rotation( int map_start, uint32_t seg_frames, uint64_t seg_bytes, uint64_t cap_bytes );

/* A v3 header: the v2 fields (`tsc_open`/`qpc_open` are the SESSION's)
   and the segment's. */
size_t xbox_ft_encode_header_v3( uint8_t *out, uint32_t ring_frames, uint64_t tsc_hz, uint64_t qpc_hz, uint64_t tsc_open, uint64_t qpc_open, uint32_t mode, const char *build, uint32_t counters, const xbox_ft_segment_t *seg );
/* Everything a segment opens with: the v3 header, the COUNTERS chunk and
   the SPANS chunk for `counters` counters - at most
   XBOX_FT_SEGMENT_OPEN_BYTES. */
#define XBOX_FT_SEGMENT_OPEN_BYTES ( XBOX_FT_HEADER_V3_BYTES + XBOX_FT_CHUNK_BYTES + 4u + XBOX_FT_W_COUNT * XBOX_FT_COUNTER_NAME_BYTES + XBOX_FT_SPANS_CHUNK_BYTES )
size_t xbox_ft_encode_segment_open( uint8_t *out, uint32_t ring_frames, uint64_t tsc_hz, uint64_t qpc_hz, uint64_t tsc_open, uint64_t qpc_open, uint32_t mode, const char *build, uint32_t counters, const xbox_ft_segment_t *seg );
/* The END chunk with the reason the file ended (XBOX_FT_SEG_* of the
   segment that follows; XBOX_FT_SEG_SESSION: the session stopped).
   xbox_ft_encode_end is this with XBOX_FT_SEG_SESSION. */
size_t xbox_ft_encode_segment_end( uint8_t *out, uint64_t tsc, uint32_t lost, uint32_t next );

#endif /* XBOX_FRAMETIME_UTIL_H */
