# Frame-time recorder and overlay (`host_frametime_analysis`)

Fork-plan item 14 step 13; divergence #141. It records the time of every
frame, split into CPU work and waiting, and the renderer's work in that
frame (draws, vertices, pushbuffer, texture binds, lightmap texels...), to a
file on the Xbox's hard drive, and can draw a frame-time graph and the work
counters over the HUD. `tools/frametime-report.py`
turns the file into an HTML report. It replaces the temporary probe that
was patched into `ref/nv2a/r_context.c` for item 12's hardware session
(`D:\probe_ft.bin`, never committed; it no longer applies to the current
code). `cl_showfps` is unchanged.

## How to use it (owner how-to)

1. **Turn it on.** Put one line in a file in `valve/userconfig.d/` on the
   game folder (the engine runs every `.cfg` there at startup), e.g.
   `E:\Games\R3D-X9K\valve\userconfig.d\50-frametime.cfg`:

       host_frametime_analysis 2

   `0` off (the default), `1` record only, `2` record and draw the overlay.
   The cvar is not saved to `config.cfg` on purpose: delete the line and
   the next boot records nothing. It can also be changed from the console
   while playing; each change from `0` starts a new file.
2. **Play.** Every frame goes into a 1024-frame ring in memory, written to
   the drive every 5 seconds and at each map start, and closed on quit. A
   power-off loses at most the last 5 seconds.
3. **Fetch the file** (the game must be closed and the dashboard's FTP
   server running; the tool only lists and downloads):

       python tools/frametime-report.py fetch --host <console IP> --out recordings

   `--host` is required: the console gets its address from DHCP, so it
   changes; the dashboard shows the current one.

   It looks in `/HDD0-E/UDATA/<title id>/valve/valve/frametime/` (the
   engine's write directory, not the game folder) and downloads every
   `ft_NNN.bin`. Each session writes the next free number, so old files are
   never overwritten; delete them over FTP when done. From xemu instead,
   with xemu stopped: `python tools/frametime-report.py fetch-xemu --out recordings`.
4. **Open the report:**

       python tools/frametime-report.py report recordings/ft_000.bin -o ft_000.html

   It prints the per-map table and writes the HTML page (per-map table,
   fps timeline with 60/30 fps lines and level loads, CPU/GPU breakdown
   timeline, and for a version-2 file the work counters: per map mean /
   peak, a work-over-time chart, each counter's correlation with CPU time
   and GPU wait, GPU-bound frames against the rest, and the ten frames
   with the most GPU wait). `summary` prints only the tables. Pass
   `--source xemu` for a recording made in the emulator: the page then
   says its timings are the PC's, not a Pentium III's - while the work
   counters are the console's own.

   The frames that wrote the file (every 5 s and at each map start; their
   time includes the write and `fsync`) are flagged, counted per map with
   their mean and worst time, and **left out of the statistics**:
   `--include-flush` keeps them.

   **Level-change frames** are left out too (`--include-level-change`
   keeps them) and listed in their own table: the frame whose `Host_Frame`
   queued a `changelevel` (or a map or savegame load) still starts in
   play, but the engine runs the load before the next `Host_Frame`
   (`engine/common/host_state.c:205-224`), so that frame's time between
   host frames *is* the load - 0.4 to 1 s on the console. Recordings from
   2026-10-01 flag it (`LOAD`); for older files the report recognises it
   by its shape: one of the last two in-play frames of a map that a load
   follows, with at least 100 ms between host frames. Then a **stalls**
   table lists every in-play frame still over 250 ms with its CPU share:
   a stall that is nearly all CPU happened inside `Host_Frame` (an
   autosave, a synchronous read).

## The overlay (mode 2)

Top left, over the HUD and under the console: the last 120 frames as 40
columns of three frames, each column drawn as the slowest of its three
(so a one-frame spike still shows at full height), 50 ms tall, stacked
bottom up as **CPU** (green), **GPU wait** (orange) and the rest of the
frame (blue-grey: vblank, swap, and the time between host frames). The
cyan line (labelled `60`) is 16.7 ms, the red one (`30`) 33.3 ms. Under the
graph a legend: a square in each bar colour (drawn with `FillRGBA`, so it
keeps its colour - the console font tints its text orange) and its label,
`CPU`, `GPU wait`, `waits/vsync`. Then seven lines, refreshed four times a
second:

- `now` - the average of the last 30 frames: fps, ms, CPU, GPU wait, the
  rest, and the last frame's time;
- `map` - since the current map started (its first 30 frames left out, as
  in the report): average fps, 1% low, CPU and GPU ms, the share of frames
  over 16.7 and 33.3 ms, the worst frame;
- `draws`, `prims`, `pb` (pushbuffer dwords), `tex` (texture binds),
  `faces` (visible world faces), `leaves` (PVS leaves);
- `verts` by source: `world`, `studio`, `tri` (other TriAPI: sprites,
  beams, particles, water, sky), `2d` (HUD, console, menus, this overlay),
  `decal`;
- `mdl` studio models / their triangles, `lm` lightmap faces / texels
  rebuilt, `water` vertices warped, `dec` decals, `part` particles,
  `beam`, `spr` sprites;
- the map, the file being written, and the overlay's own drawing time
  (the half-second mean);
- `self ms` - the six CPU spans (see "CPU spans" below) with the most time
  of their own (a span minus the spans inside it), largest first, then
  `none`: CPU that no span saw. The shown values, the other spans' own
  times and `none` add up to the `now` line's `cpu`. Short labels, since
  each glyph is a quad: `game` (sv_game), `pts` (studio_points), `cstud`
  (client_studio), `scene` (r_scene), `world`, `lm`, `skin` (skin_read),
  `efx`, `tri` (tri_hooks), `hud`, `rend` (r_end), `snd`, `sndx`
  (snd_extra), `svread`, `send`, `cldll`, `clread`, `pred`, `emit`,
  `input`, `clbeg`, the round trip's `rel` (sv_reliable), `fpack`
  (sv_fullpack), `scdat` (sv_clientdata), `sdelta` (sv_emit_ents),
  `sxmit` (sv_xmit), `runcmd`, `cdelta` (cl_pkt_ents), `ccdat`
  (cl_clientdata), `cmd`, `pmove` (cl_predmove), `wpkt` (cl_wpacket),
  and the parents `sv`, `cl`, `scr`. With `host_frametime_spans 0` it
  reads `spans off`.

The counters are the same half-second (30-frame) means as the `now` line.

The overlay's statistics use a 0.25 ms histogram, so its 1% low can differ
from the report's (an exact sort) by up to 0.25 ms of frame time.

## What is measured, and the limits

Every time is the Pentium III's time-stamp counter (`rdtsc`), calibrated
against the kernel's performance counter (`KeQueryPerformanceCounter`) at
every flush; the file keeps every calibration pair.

| field | from | to |
|---|---|---|
| total | `Host_Frame` start (after `Host_FilterTime`) | the next `Host_Frame` start |
| host | `Host_Frame` start | its end |
| GPU wait | ref/nv2a spinning on `pb_busy()`: end of frame, after the clears, a full 2D vertex ring, a texture upload mid-frame | |
| vblank | `pb_wait_for_vbl()` in `R_BeginFrame` | |
| swap | `pb_finished()` returning "all back buffers queued" in `R_EndFrame` | |
| CPU | host minus GPU wait, vblank and swap | |
| between frames | total minus host: the frame limiter (`fps_max`, `Host_Autosleep`) and the platform loop | |

**GPU time is not the GPU's busy time.** pbkit starts the GPU on the push
buffer at every `pb_end()` (`pbkit.c:1824-1850`), so the NV2A works in
parallel with the CPU for the whole frame. The NV2A has no GPU-side
timestamp this code can read: `NV097_GET_REPORT` and the back-end
semaphore need a report/semaphore context DMA, and pbkit binds none to the
kelvin object (`pbkit.c:2643-2659`, `2728-2734`); `NV_PTIMER` is a clock the
CPU reads, like the TSC. What is measured is the time the CPU had to wait
for the GPU: the GPU's work beyond the CPU's. Near zero means the GPU
finished first (the frame was CPU-bound); a large value means the frame
waited for the GPU. The report's "GPU-bound" column counts frames with more
than 1 ms of it.

**Map changes.** A load is the run of frames in which the client is
connecting or connected but not yet active (or the loading plaque is up).
The recorder writes a MAP chunk at the first active frame with the map
name, the load time (from the start of the frame that queued the load -
flagged `LOAD`, see below - to the first active frame, so it includes that
frame's own `Host_Frame`; without such a frame in the two before the first
loading frame, from the start of the frame before it), free memory
at map start (`MmQueryStatistics().AvailablePages`, after everything is
loaded), and free memory at the last flush before the load (up to 5 s
earlier). The first map after recording starts has no load time.

**Frame flags** mark frames in play (active, no menu or pause), frames that
wrote the file (their time includes the write and `fsync`), frames with the
overlay, saturated counters (a frame over 5.8 s at 733 MHz) and, from
2026-10-01, `LOAD` (0x20): the frame's `Host_Frame` left
`GameState->nextstate` other than `STATE_RUNFRAME` - it queued a
changelevel, a load, a new game or a disconnect, which runs before the
next `Host_Frame` and so lands in this frame's time. The in-play flags
describe the state a frame *starts* in, so that frame still reads as in
play; the flag is what says it is not. It used a reserved bit, so the
format version did not change. The report and the overlay use in-play
frames only, skip each map's first 30 and leave out `LOAD` frames; the
report also leaves out the frames that wrote the file (`--include-flush`
keeps them).

Before the flag existed, recordings showed this as "one 350-1000 ms spike
on every map": in the owner's -O2 route (`ft_002.bin`, PR #169's v1 file)
15 of the 22 in-play frames over 250 ms are these frames (CPU 0-12 ms,
390-980 ms between host frames, the second-to-last in-play frame of
each map), 6 are 616-752 ms of CPU inside `Host_Frame` that line up with the maps'
`trigger_autosave`s, and the last is the session's final in-play frame.

## Work counters (format version 2)

Counts, not times: they are the same in xemu as on the console, so a run in
the emulator measures them exactly and they can drive an optimisation
decision there (the times and their correlation with the counters still
need the console). ref/nv2a adds to `xbox_ft_work` - a plain global the
engine defines next to `xbox_ft_waits`, the same pattern and the same
reason (divergence #141) - at the place it submits the work; the recorder
zeroes it at `Host_Frame` start and copies it into the frame record. Each
site is one add; nothing tests whether the recorder is on.

| counter | counted at |
|---|---|
| `draws` | each draw submission: `nv2a_draw_index_range` / `nv2a_draw_exact_indices` call (a world or brush-entity run), each 2D/TriAPI batch flush (`nv2a_2d_flush`), each decal (`nv2a_decal_gpu_draw`) |
| `prims` | each BEGIN/END block pushed (a draw splits at pbkit's 128-dword block) |
| `pb_dwords` | pushbuffer write pointer at `R_EndFrame` minus its value right after `R_BeginFrame`'s `pb_reset()` (pbkit's `pb_begin()` returns the pointer and opens nothing with `DBG` off, `pbkit.c:14`, `:1786`) |
| `tex_binds` | each `NV097_SET_TEXTURE_OFFSET` write: map textures, lightmap stage, flat colour, lit-state entry, decals, 2D/TriAPI batches |
| `v_world` | world and brush-entity indices submitted |
| `v_studio` | TriAPI vertices inside `R_StudioDrawPoints`' mesh loop |
| `v_tri` | other 3D TriAPI vertices (sprites, beams, particles, water, sky) |
| `v_2d` | 2D vertices (HUD, console, menus, the overlay itself) |
| `v_decal` | decal vertices |
| `world_faces` | world faces left after PVS, frustum and backface culling (`nv2a_cull_faces_visible`), 0 in a frame without a world pass |
| `pvs_leaves` | leaves in the PVS at its last recompute (`nv2a_mark_leaves`), a state |
| `studio_models` | the change of `c_studio_models_drawn` over the frame - the game's client increments it itself through `pfnGetModelCounters` |
| `studio_tris` | studio mesh triangles drawn |
| `lm_faces`, `lm_texels` | lightmap faces and texels rebuilt on the CPU (#162) |
| `water_verts` | water vertices warped on the CPU (#166) |
| `decals` | decals drawn (#167) |
| `particles`, `beams` | particles and beams drawn (#161) |
| `sprites` | sprite models drawn |

The report adds `verts`, the sum of the five `v_*` counters.

A counter covers the same span as the frame's `total`: from its
`Host_Frame` start to the next one. So the frame that runs a `changelevel`
counts two frames of work - its own and the loading screen, which the
engine draws from outside `Host_Frame` in that span - and the next frame
counts none (the screen is disabled). In a recording this is the one
frame per map whose counters are double, next to its 250-530 ms time.

## CPU spans (M2, 2026-10-01)

The perf report's step M2 (`docs/r3d/reports/perf-research/xbox-cpu-bound-o2.md`,
"M2: per-subsystem `rdtsc` spans on hardware") is implemented here. It asks
where a frame's CPU goes, by subsystem, so the next optimisation can be
chosen from a measurement. A **span** is the TSC time inside one call,
added up over the frame, minus the GPU, vblank and swap waits that happened
inside it. So a span is CPU work, and the spans add up against the frame's
`cpu`, not its `host` time. The recorder stores each as a work counter in
kilocycles (TSC ticks / 1000, rounded per frame; 1.36 us at 733 MHz),
named `k_*`. The report's `sv_game_kcyc` is `k_sv_game`: a counter name
has at most 15 characters, and `client_studio_kcyc` does not fit.

Spans **nest**. A parent's time includes its children's, and the file's
SPANS chunk names each span's parent. The top level wraps every call
`Host_Frame` makes except `Host_GetCommands` (a dedicated server's input),
`HTTP_Run` and `XRcon_Frame`. So `cpu - (sum of the top-level spans)` is
**unattributed**: those three, the recorder itself and the frame
boundary. The report shows it per map, and the overlay shows it as `none`.

| span | parent | measured around | site |
|---|---|---|---|
| `k_input` | - | `Host_InputFrame` | `engine/common/host.c:670` |
| `k_cl_begin` | - | `Host_ClientBegin`: `Cbuf_Execute`, `CL_SendCommand` (client.dll `CL_CreateMove`, client prediction) | `host.c:671` |
| `k_sv` | - | `Host_ServerFrame` | `host.c:673` |
| `k_sv_read` | `k_sv` | `SV_ReadPackets`: the client's usercmds, i.e. the player's movement and `PlayerPreThink`/`PostThink` | `engine/server/sv_main.c:692` |
| `k_sv_game` | `k_sv` | `SV_Physics` (both calls in `SV_RunGameFrame`): game DLL think, physics, AI | `sv_main.c:613`, `:624` |
| `k_sv_send` | `k_sv` | `SV_SendClientMessages`: entity packing, delta encoding | `sv_main.c:707` |
| `k_cl` | - | `Host_ClientFrame` | `host.c:674` |
| `k_cl_dll` | `k_cl` | `clgame.dllFuncs.pfnFrame` | `engine/client/cl_main.c:3913` |
| `k_cl_read` | `k_cl` | `CL_ReadPackets` | `cl_main.c:3919` |
| `k_cl_pred` | `k_cl` | `CL_RedoPrediction` | `cl_main.c:3935` |
| `k_cl_emit` | `k_cl` | `CL_EmitEntities` | `cl_main.c:3941` |
| `k_scr` | `k_cl` | `SCR_UpdateScreen`: all rendering | `cl_main.c:3959` |
| `k_r_scene` | `k_scr` | `GL_RenderFrame` in `V_RenderView`: the 3D scene | `engine/client/cl_view.c:422` |
| `k_lm` | `k_r_scene` | `nv2a_update_dynamic_lightmaps` (`R_LightmapUpdate` and its brush-entity gather) | `ref/nv2a/r_context.c:6414` |
| `k_world` | `k_r_scene` | `nv2a_draw_sky`, `nv2a_draw_map_geometry`, `R_DecalDrawWorld` | `r_context.c:6420-6427` |
| `k_client_studio` | `k_r_scene` | `R_StudioDrawModelInternal`: `pStudioDraw->StudioDrawModel/Player`, i.e. HL's `StudioModelRenderer` with its bone setup (or the built-in renderer when it is selected) | `ref/nv2a/r_studio.c:3121-3144` |
| `k_studio_points` | `k_client_studio` | `R_StudioDrawPoints`: our CPU skinning, lighting and TriAPI emit. A wrapper around the function, not a call site: the client calls it through `api->StudioDrawPoints` | `r_studio.c:2224` |
| `k_skin_read` | `k_studio_points` | `nv2a_tex_upload_skin` in `nv2a_tex_resolve_skin`: a skin's lazy disk read and decode | `ref/nv2a/r_image.c:978` |
| `k_efx` | `k_r_scene` | both `CL_DrawEFX` calls: beams, particles, tracers | `r_context.c:6469`, `:6482` |
| `k_tri_hooks` | `k_r_scene` | `pfnDrawNormalTriangles`, `pfnDrawTransparentTriangles` (client.dll) | `r_context.c:6471`, `:6473` |
| `k_hud` | `k_scr` | `CL_DrawHUD( CL_ACTIVE )`: the client's `HUD_Redraw` | `cl_view.c:540` |
| `k_snd_extra` | `k_scr` | `S_ExtraUpdate` in `V_PostRender`: a second mixer pass | `cl_view.c:580` |
| `k_r_end` | `k_scr` | `R_EndFrame` in `V_PostRender`: 2D flush, pushbuffer kick, present | `cl_view.c:586` |
| `k_snd` | `k_cl` | `SND_UpdateSound`: the mixer | `cl_main.c:3962` |
| `k_sv_reliable` | `k_sv_send` | `SV_UpdateToReliableMessages` | `engine/server/sv_frame.c:824` |
| `k_sv_fullpack` | `k_sv_send` | `SV_AddEntitiesToPacket`: the game DLL's `SetupVisibility`, then `AddToFullPack` for every edict (with the engine's `pfnCheckVisibility` leaf test inside it) | `sv_frame.c:627` |
| `k_sv_clientdata` | `k_sv_send` | `SV_WriteClientdataToMessage`: the game DLL's `UpdateClientData` and `GetWeaponData`, the clientdata delta and 64 weapon-slot deltas | `sv_frame.c:693` |
| `k_sv_emit_ents` | `k_sv_send` | `SV_EmitPacketEntities`: entity delta encoding (incl. the game DLL's `Entity_Encode`/`Player_Encode` callbacks) | `sv_frame.c:664` |
| `k_sv_xmit` | `k_sv_send` | `Netchan_TransmitBits` of the client datagram: header, copy, loopback send | `sv_frame.c:723` |
| `k_sv_runcmd` | `k_sv_read` | every `SV_RunCmd` in `SV_ParseClientMove`: `PM_Move` and the game DLL's `CmdStart`/`PlayerPreThink`/`PlayerPostThink`/`CmdEnd` | `engine/server/sv_client.c:3330`, `:3337`, `:3344` |
| `k_cl_pkt_ents` | `k_cl_read` | `CL_ParsePacketEntities` (both message types): entity delta decoding, `CL_ProcessPacket` | `engine/client/parse/cl_parse.c:2667`, `:2672` |
| `k_cl_clientdata` | `k_cl_read` | `CL_ParseClientData` | `cl_parse.c:2637` |
| `k_cl_cmd` | `k_cl_begin` | `CL_CreateCmd`: client.dll `CL_CreateMove`, then prediction | `engine/client/cl_main.c:1060` |
| `k_cl_predmove` | `k_cl_cmd` | `CL_PredictMovement( false )`: the client's `PM_Move` over the unacknowledged commands | `cl_main.c:850` |
| `k_cl_wpacket` | `k_cl_begin` | `CL_WritePacket`: usercmd deltas and `Netchan_TransmitBits` | `cl_main.c:1063` |

The last eleven (2026-10-02) break the local server/client round trip
down; they come after `k_snd` in the file so the older spans keep their
indices, and the SPANS chunk gives their parents. Each closes about once
a frame (`k_sv_runcmd` once per usercmd).

Spans not listed under a parent stay in that parent's **own** time.
Examples: the overlay's drawing (inside `k_scr`; it also has its own field)
and `nv2a_draw_solid_entities`' brush entities (inside `k_r_scene`). A
child that also runs outside its parent would push the parent's own time
below zero. The report and the overlay clamp it at 0. One case is a skin
bound outside a studio draw (`R_StudioSetupSkin` is also in the studio
API the client can call). As with the counters, the frame that runs a
`changelevel` also carries the loading screen's spans, which run outside
`Host_Frame`. The report leaves those frames out anyway.

Engine-side, the spans are one macro per call site, `XBOX_FT_SPAN( id,
call )` (`engine/platform/xbox/xbox_frametime.h`). Each engine file
includes the header under `#if XASH_XBOX && !XASH_DEDICATED` (client
files: `#if XASH_XBOX`) and otherwise defines the macro as the bare call,
so a non-Xbox build compiles the upstream line unchanged. ref/nv2a writes
into the shared `xbox_ft_spans` global the same way it writes the
counters, so the ref API does not change.

**In xemu** the spans are the emulator's times on a PC. A run there shows
that they are recorded and add up, not what the Pentium III spends. The
numbers for the decision come from the owner's hardware route.

## Cost

- **Off:** one float compare per `Host_Frame`; in ref/nv2a one load and
  branch at each of five wait sites, and one add per work-counter site
  (an add costs what a test of "is the recorder on" would). No memory: the
  ring and buffers are allocated when recording starts and freed when it
  stops.
- **On:** ~292 KiB (ring 1024 x 256 B = 256 KiB, the overlay's histogram
  4 KiB, a 32 KiB encode buffer); a few `rdtsc` per frame plus two per
  span closed (below); a ~75 KiB write and `fsync` every 5 s (those
  frames are flagged); about 15 KiB per second of play on disk at 60 fps
  (53 MiB an hour; the file stops at 64 MiB, ~1.2 hours). Before the
  round-trip spans (2026-10-02) it was ~243 KiB and 12.4 KiB/s; before
  any spans, ~130 KiB and 6.6 KiB/s.
- **Spans, off** (recorder off, or `host_frametime_spans 0`): one load
  and branch of `xbox_ft_spans.on` per span site, plus a branch on a
  stack local at its end.
- **Spans, on:** per closed span, two `rdtsc`, three 64-bit wait loads
  at each end and two adds. The file counts the closures per frame
  (`span_calls`). The fixed sites close ~20 a frame; `k_client_studio`
  closes once per studio draw, `k_studio_points` once per body part
  drawn, `k_skin_read` once per skin read. In xemu (spans build 6ABEBB7E,
  nxdk at -O2): 20-47 closures a frame on c1a0c, c1a0e and c1a0d. With
  `host_frametime_spans` toggled mid-session on one c1a0c view, mean CPU
  was 10.47 ms with spans on, 10.53 off and 10.29 on again (709, 1495 and
  1494 frames): no cost visible above the scene's own variation.
  Against the build before the spans (PR #173's -O2 XBE 6ABEA057) on the
  static c1a0d view in mode 2, CPU was 4.07 vs 3.93 ms. 0.10 ms of that
  is the overlay's extra line, which leaves ~0.04 ms for the spans and
  the larger records. These are emulator times, and the Pentium III's
  `rdtsc` cost was not measured here. The owner's
  `host_frametime_spans 0`/`1` A/B on one view on the console is the
  number to quote. In every xemu recording with spans on, the top-level spans
  plus `unattributed` matched `cpu`: 0.009 ms unattributed on average,
  0.18 ms at worst in a frame, never negative.
- **Overlay:** at most 120 `FillRGBA` quads for the graph (40 columns x 3
  segments, empty ones skipped), a few for the panel, lines and legend,
  and one quad per glyph of text (~300; spaces are free). Its drawing
  time is measured every frame, shown on its last line and stored in the
  file. Measured in xemu on the same static c1a0d view (`map c1a0d`, no
  input), 1130 in-play frames each: **1.080 ms mean (median 1.045, p90
  1.237) before, 0.867 ms (median 0.849, p90 0.928) after** - 20% less
  while drawing three more lines (the counters) and the legend. The graph
  went from up to 360 quads to up to 120 and the text is formatted 4 times
  a second instead of every frame; the ~300 glyphs are now most of the
  cost. The console's figure comes from the owner's run. The spans line
  (2026-10-01, ~70 glyphs) took it from **0.519 ms (median 0.515, p90
  0.632) to 0.615 ms (median 0.623, p90 0.735)**, +0.10 ms. Same static
  c1a0d view in xemu, 1157 overlay frames each: PR #173's -O2 XBE
  against the spans build.

## File format

Version 2, little-endian: a 128-byte header (magic `R3DFTIME`, version,
header and frame-record sizes, TSC and performance-counter rates and the
pair at open, the mode, a build id with the XBE's link timestamp), then
chunks `{u32 type, u32 length, payload}`: COUNTERS (right after the header:
the counters' names, 16 bytes each), FRAMES (first sequence number, count,
frame records of 32 bytes plus one u32 per counter - 256 bytes with today's
56: the 20 counts, `span_calls` and the 35 spans; 212 with the first 24
spans, 112 before the spans),
SPANS (from 2026-10-01, right after COUNTERS: each span's name and its
parent's, 16 bytes each, "" for the top level), MAP, CALIB (TSC,
performance counter) and END (written on a clean stop; a file without it
was cut short). The reader finds counters by name, so a later version can
add some without breaking it. The spans used that: they are counters plus
an optional chunk, which a reader that does not know it skips by its
length, so the version stayed 2. Version 1 is the same
file without COUNTERS and with 32-byte records; the report reads both. The authoritative field-by-field
layout is the comment at the top of
`engine/platform/xbox/xbox_frametime_util.h`; `parse()` in
`tools/frametime-report.py` reads it, and a file cut mid-record parses up
to its last complete record.
