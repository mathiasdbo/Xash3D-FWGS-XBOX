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
   table lists every in-play frame still over 250 ms with its CPU share
   and its three largest spans by own time: a stall that is nearly all
   CPU happened inside `Host_Frame` (an autosave - `save_sav`,
   `save_ents`... - or a synchronous read). A frame's flags are the state
   it starts in, so the frame in which the player opens the pause menu
   (or the console) still counts as play: the table marks it
   `opens-menu` when the next frame starts in the menu. On the
   2026-10-06 route recording three of the four ~320 ms stalls that were
   not autosaves were that frame - the first pause after each map load,
   when the menu rebuilds the fonts and background it released for the
   map (divergence #127).

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
  `faces` (world faces drawn / faces the world pass walked), `leaves`
  (PVS leaves);
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
  the mixer's `sspat` (snd_spatial), `smix`, `sdsp`, `sout` (snd_mix,
  snd_dsp, snd_out) and `xmix`, `xdsp`, `xout` (sndx_mix, sndx_dsp,
  sndx_out), the scene's `pvs`, `sky`, `walk` (world_walk), `wwarp`
  (warp_world), `wdec` (world_decals), `brush`, `bwarp` (warp_brush),
  `twarp` (warp_trans), and the parents `sv`, `cl`, `scr`. With `host_frametime_spans 0` it
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
| `world_walked` | world faces `nv2a_draw_map_geometry`'s face loop visited (2026-10-02, after the spans in the record): every face of every batch before R3, the PVS's world faces after it. `world_faces` is how many of them it drew |
| `snd_loads`, `snd_load_kb`, `snd_load_kc` | (2026-10-06, at the end of the record) sounds `S_LoadSound` loaded because they were not in its cache - a sentence's word, a name nobody precached - with their decoded size and the time spent loading them, in kilocycles (a time, the console's only on the console). A load runs under whichever span asked for the sound - the first word of a sentence in `k_cl_read` (`S_StartSound` from `svc_sound`), the next words in `k_snd_mix` - so its time is a counter, not a span with one parent. `engine/client/sound/s_load.c`, `S_LoadSound` |
| `tex_loads`, `tex_load_kb` | (2026-10-06) textures ref/nv2a created from an image (`GL_LoadTexture`, `GL_LoadTextureFromBuffer`, updates included) and their storage. In play, a non-zero value is a first-use load: a menu restore, a decal, a sprite. `ref/nv2a/r_image.c` |
| `cl_parse_kc` | (2026-10-06) time, kilocycles: `CL_ParseNetMessage`'s parse call, every server message of the frame's packets (`engine/client/cl_main.c`). Inside `k_cl_read`; a counter rather than a span because `k_cl_pkt_ents` and `k_cl_clientdata`, which it contains, are older spans and a span's parent must come before it in the file |

The report adds `verts`, the sum of the five `v_*` counters. Its stalls
table lists each stall's first-use loads (`snd_*`, `tex_*`) next to its
largest spans.

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
| `k_snd_spatial` | `k_snd` | `SND_UpdateSound` before the mix: `S_UpdateSoundFade`, `S_FreeIdleRawChannels`, `S_UpdateAmbientSounds`, `SND_Spatialize` for every channel with a sound, `S_SpatializeRawChannels` | `engine/client/sound/s_main.c`, `SND_UpdateSound` |
| `k_snd_mix` | `k_snd` | `S_MixNormalChannelsToRoombuffer` (per channel: resampling and volume, `VOX` words, lip sync) and `S_MixRawChannels` | `engine/client/sound/s_mix.c`, `S_PaintChannels` |
| `k_snd_dsp` | `k_snd` | `SX_RoomFX`: room modulation, reverb, delay, stereo delay | `s_mix.c`, `S_PaintChannels` |
| `k_snd_out` | `k_snd` | `S_ClearBuffers`, `S_MixBufferWithGain`, `S_TransferPaintBuffer` (clip to 16 bits into the DMA buffer) | `s_mix.c`, `S_PaintChannels` |
| `k_sndx_mix`, `k_sndx_dsp`, `k_sndx_out` | `k_snd_extra` | the same three, when `S_ExtraUpdate` mixes | `s_mix.c`, `S_PaintChannels` |
| `k_pvs` | `k_r_scene` | `nv2a_mark_leaves`: the view leaf every frame; at a leaf change, the PVS row, the per-face visibility table and the visible-face list | `ref/nv2a/r_context.c`, `GL_RenderFrame` |
| `k_sky` | `k_world` | `nv2a_draw_sky`: the sky batches' faces and the skybox | `r_context.c`, `GL_RenderFrame` |
| `k_world_walk` | `k_world` | `nv2a_draw_map_geometry`'s batch and face walk: culling, texture binds and index submission, up to the water | `r_context.c`, `nv2a_draw_map_geometry` |
| `k_warp_world` | `k_world` | `R_WarpDrawWorld` and its flush: the world's opaque water, warped on the CPU (#166) | `r_context.c`, `nv2a_draw_map_geometry` |
| `k_world_decals` | `k_world` | `R_DecalDrawWorld` | `r_context.c`, `GL_RenderFrame` |
| `k_brush` | `k_r_scene` | every `nv2a_draw_brush_entity` call of the solid and translucent passes: doors, trains, `func_wall`, `func_water` | `r_context.c`, `nv2a_draw_solid_entities` / `nv2a_draw_trans_entities` |
| `k_warp_brush` | `k_brush` | `R_WarpDrawBrushEntity`: a brush entity's water | `r_context.c`, `nv2a_draw_brush_entity` |
| `k_warp_trans` | `k_r_scene` | `R_WarpDrawWaterSurfaces`: the world's translucent water (`sv_wateralpha` below 1) | `r_context.c`, `GL_RenderFrame` |
| `k_sv_start` | `k_sv_game` | the game DLL's `StartFrame` | `engine/server/sv_phys.c`, `SV_Physics` (`:1895` in this tree) |
| `k_sv_pstep` | `k_sv_game` | every `SV_Physics_Entity` of an entity that starts it as `MOVETYPE_STEP`/`PUSHSTEP`: monsters (their AI think included), `func_pushable` | `sv_phys.c`, `SV_Physics` (`:1908`), `XBOX_FT_SV_ENTITY` |
| `k_sv_ppush` | `k_sv_game` | the same for `MOVETYPE_PUSH`: brush movers - doors, trains, lifts, `func_rotating`, `func_pendulum` (`SV_PushMove`/`SV_PushRotate`, their Blocked and Think) | same |
| `k_sv_ptoss` | `k_sv_game` | the same for `TOSS`, `BOUNCE`, `FLY`, `FLYMISSILE`, `BOUNCEMISSILE`: gibs, grenades, items falling | same |
| `k_sv_pnone` | `k_sv_game` | the same for `MOVETYPE_NONE`: think-only entities - triggers, logic, sprites, beams | same |
| `k_sv_pother` | `k_sv_game` | the same for `NOCLIP`, `FOLLOW`, `COMPOUND` and anything else | same |
| `k_save` | `k_cl_begin` | `SaveGameSlot`: a whole `save`/`autosave` | `engine/server/sv_save.c`, `SV_SaveGame` |
| `k_save_init` | `k_save` | `SaveInit`/`SaveFinish`: the save buffer's allocation, clearing and release (the one in `SaveGameState` and the swap between the two buffers in `SaveGameSlot`) | `sv_save.c`, `SaveGameState`, `SaveGameSlot` |
| `k_save_ents` | `k_save` | `SaveGameState`'s serialisation: entity table, `ParmsChangeLevel`, header, adjacency, lightstyles, every entity's `pfnSave` (the game DLL), the ETABLE, the token table | `sv_save.c`, `SaveGameState` |
| `k_save_hl1` | `k_save` | writing `<map>.HL1` and `EntityPatchWrite`'s `<map>.HL3` | `sv_save.c`, `SaveGameState` |
| `k_save_client` | `k_save` | `SaveClientState`: the renderer's decal list, static entities, sounds, `<map>.HL2` | `sv_save.c`, `SaveGameState` |
| `k_save_globals` | `k_save` | the `.sav`'s game header (with `DirectoryCount`) and the DLL's `SaveGlobalState` | `sv_save.c`, `SaveGameSlot` |
| `k_save_age` | `k_save` | `AgeSaveList`: the aged `autosave`/`quick` files' deletes and renames | `sv_save.c`, `SaveGameSlot` |
| `k_save_sav` | `k_save` | the `.sav` itself: open, header, tokens, globals, `DirectoryCopy` of every `.HL?` file in `save/`, close | `sv_save.c`, `SaveGameSlot` |
| `k_scrshot` | `k_scr` | `SCR_MakeScreenShot` in `V_PostRender`: the `saveshot` thumbnail (or any screenshot) | `engine/client/cl_view.c`, `V_PostRender` |
| `k_shot_read` | `k_scrshot` | ref/nv2a's back-buffer readback in `VID_ScreenShot` (the GPU wait before it is not charged) | `ref/nv2a/r_context.c`, `VID_ScreenShot` |
| `k_shot_resample` | `k_scrshot` | `Image_Process`: the resample to the thumbnail's 267x200 | same |
| `k_shot_write` | `k_scrshot` | `FS_SaveImage`: the BMP's encode and write | same |
| `k_ui` | `k_scr` | the menu DLL's `pfnRedraw` in `UI_UpdateMenu`: the menu's drawing, and what it restores on demand after a map load released it (#127) | `engine/client/dll_int/cl_gameui.c`, `UI_UpdateMenu` |
| `k_ui_file` | `k_ui` | the menu's `COM_LoadFile` calls (`Xbox_FT_UI_LoadFile`, a timed `COM_LoadFileForMe`, in the menu's engine-function table): font cache files, font files, layouts. The menu also loads files and pictures outside its redraw (init, key events); those are counted with no `k_ui` open, which the report clamps like a skin read | `engine/platform/xbox/xbox_frametime_client.c` |
| `k_ui_pic` | `k_ui` | the menu's `PIC_Load` calls: the image decode and ref/nv2a's texture upload | `cl_gameui.c`, `pfnPIC_Load` |
| `k_snd_music` | `k_snd` | `S_StreamBackgroundTrack`: the music stream - opening a track, reading and decoding it | `engine/client/sound/s_main.c`, `SND_UpdateSound` |

The eleven from `k_sv_reliable` to `k_cl_wpacket` (2026-10-02) break
the local server/client round trip down; they come after `k_snd` in the
file so the older spans keep their indices, and the SPANS chunk gives
their parents. Each closes about once a frame (`k_sv_runcmd` once per
usercmd).

The last seven (2026-10-02, appended the same way) break the mixer down.
`S_PaintChannels` runs from both `SND_UpdateSound` and `S_ExtraUpdate`,
and a span has one parent, so its three spans come in two sets: `k_snd_*`
under `k_snd` and `k_sndx_*` under `k_snd_extra`. `S_ExtraUpdate` sets
`xbox_ft_snd_extra` while it mixes, and that picks the set. The time
`S_UpdateChannels` spends in `SNDDMA_BeginPainting` (the SDL audio
device lock, held by the audio thread while it copies a buffer out)
stays in the parents' own time; `S_StreamBackgroundTrack` has had its
own span, `k_snd_music`, since 2026-10-06. A renderer
calling `S_ExtraUpdate` through `CL_ExtraUpdate` would charge `k_sndx_*`
outside `k_snd_extra`; ref/nv2a does not call it. The mixer paints only
when the audio thread has consumed a block (1024 samples, 23 ms at
44.1 kHz), so these spans close on roughly two frames in three and their
per-frame average is the useful number. The SDL audio thread itself -
the callback's copy, and SDL's conversion from the engine's 44.1 kHz to
the Xbox's fixed 48 kHz - runs outside every span and lands in whichever
span it interrupts. In xemu that thread's callback never runs (counted:
0 calls over a whole recording, 2026-10-02), so the mixer paints its
0.12 s mix-ahead once and then nothing, and the mixer spans read about
zero there. To compare mixer changes in xemu, a never-committed harness
build advanced `snd.samplepos` by whole 1024-sample blocks of wall-clock
time in `SNDDMA_BeginPainting` (as the callback does on the console);
the console's own numbers need a hardware run.

The last eight (2026-10-02, appended the same way) break the scene
down for R3 (`docs/r3d/reports/perf-research/xbox-gpu-offload-sse.md`):
`k_world` into the sky, the face walk, the opaque water and the decals,
and three pieces of `k_r_scene`'s own time - the PVS, the brush
entities (with their water) and the translucent world water. Each closes
about once a frame; `k_brush` and `k_warp_brush` once per brush entity
drawn. The face walk's count, `world_walked`, comes after the spans in
the record for the same reason.

The last six (2026-10-05, appended the same way) break `k_sv_game` down
by movetype; the next section has the rest of the server game frame's
breakdown. Each entity's whole `SV_Physics_Entity` - its movement, its
think, and the touches and traces both cause - goes to the span of the
movetype it had when it started, so `k_sv_pstep` is the monsters' AI as
well as their physics. `k_sv_game`'s own time is the loop over the
edicts itself, the lightstyles and `SV_CheckAllEnts`. These close once
per entity SV_Physics runs (300-370 a frame on the maps measured), which
is most of the span closures of a frame now.

The last twelve (2026-10-05, appended the same way) break a savegame
down. A `save` or `autosave` (the game's `trigger_autosave` queues one
with `SERVER_COMMAND`) runs from `Cbuf_Execute` in `Host_ClientBegin`,
and `SaveGameSlot` queues `saveshot` into the same command buffer
(upstream `sv_save.c:1740`), so with nothing else queued the thumbnail
is taken in the same frame's `V_PostRender`: an autosave is one long
frame, `k_save` plus `k_scrshot`. A save typed at the console runs from
`Host_InputFrame` instead, outside `k_cl_begin`. The `k_save_*` children
are the sequential parts of `SaveGameSlot`; `SaveGameState` is also
called by a `changelevel`, between frames, where nothing records it.
Every file write is a synchronous kernel call on the Xbox, so the disk's
time is inside these spans - on the console the IDE drive, in xemu the
host's file cache. The report's stalls table names each stall's three
largest spans by own time.

The last four (2026-10-06, x9-perf-stalls, appended the same way) name
two kinds of stall that showed up as a parent's own time. The first
pause after every map load cost about 310 ms on the console, all of it in
`k_scr`'s own time: the menu DLL's redraw had no span, and it is where
the menu rebuilds what divergence #127 releases for a map. `k_ui` is
that redraw, and `k_ui_file` and `k_ui_pic` are the two engine services
the rebuild reads and uploads through, so a menu stall splits into disc,
upload and the menu's own work. `k_snd_music` is the music stream, the
part of `k_snd`'s own time that reads the disc: 120-165 ms `k_snd`
frames on the console's route recording (2026-10-06) had nothing else
in `k_snd` to explain them.

Spans not listed under a parent stay in that parent's **own** time.
Examples: the overlay's drawing (inside `k_scr`; it also has its own field)
and the view setup and entity sorting of `GL_RenderFrame` (inside
`k_r_scene`). A
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

## Notes: named one-off events (2026-10-06)

A span says where a stall's time went; it cannot say which sound or which
server message it was. The NOTES chunk does, for three kinds of event,
each with the sequence number of the frame it happened in:

| kind | written when | name | bytes |
|---|---|---|---|
| `sound` | `S_LoadSound` loads a sound while `cls.state == ca_active` (the client-state test of upstream's `s_warn_late_precache` warning, `s_load.c:119` in the reference, without its cvar or its `*default` exclusion): a sentence's word, a name nobody precached. Map-load registrations happen before the client is active and are left out | the sound's name | decoded size |
| `message` | one server message took the client 5 ms or more to parse (`CL_ParseServerMessage`, `engine/client/parse/cl_parse.c`, through `Xbox_FT_CL_CloseMessage` in `engine/platform/xbox/xbox_frametime_client.c`: the time from one message's start to the next, minus the renderer's waits; the current protocol only - the GoldSrc and legacy parsers are not timed) | its `svc_*` name, or the user message's name (`clgame.msg`) | its size in the packet |
| `menu` | the end of a map load ran the menu's font restore, or left the fonts released because less than 8 MiB was free (`Xbox_MenuFonts_MapLoaded`, `engine/platform/xbox/xbox_menu_fonts.c`, divergence #127; from 2026-10-06, x9-perf-menufonts) | `menu_restorefonts ran, <KiB> KiB free before` or `fonts left released, <KiB> KiB free` | what the restore took from free memory (`MmQueryStatistics`; atlases placed in ref/nv2a's preallocated arena do not show) |

The time is in kilocycles and needs `host_frametime_spans` on (the
default); a note taken with it off has time 0. The recorder keeps up to
64 notes between flushes; past that it counts each new note as dropped
and keeps it in place of the fastest kept one if it is slower, so a
burst of small sentence-word loads cannot crowd out the slow message
the notes are for. It writes the chunk at a flush that has any.
`cl_parse_kc` (a per-frame time counter, below) says how much of
`k_cl_read` was the message parser at all: a `k_cl_read` stall with no
message note is many small messages when it is high, and work around the
parser - netchan, fragment reassembly, resource requests - when it is
low. The report lists each stall's notes next
to its spans and loads, and the slowest twelve of the session.

## The server game frame (2026-10-05)

`k_sv_game` (`SV_Physics`) had become the largest single CPU cost on the
console (6-7 ms a frame on c1a1b) with nothing under it. Besides the
movetype spans above, `engine/platform/xbox/xbox_frametime_svgame.{c,h}`
add three things. The engine/server sites are macros from that header,
the plain upstream statement outside an Xbox build (divergence #141).

**Counts** (`g_*`, after `vp_loads` in the record), counted only while
`SV_Physics` runs and the recorder is recording (with it off each site
is one load and branch) - player movement (`k_sv_runcmd`) and the send
path are not in them. Exact in xemu.

| counter | counted at (this tree) |
|---|---|
| `g_ents` | each entity `SV_Physics` runs (`sv_phys.c:1908`) |
| `g_thinks` | each game DLL `Think`: `SV_TryThink` (`:228`), `SV_Physics_Pusher` (`:1291`) |
| `g_touches` | each game DLL `Touch`: `SV_Impact`, both ways (`:304`, `:310`), `SV_TouchLinks` (`sv_world.c:563`) |
| `g_moves` | each `SV_Move` (`sv_world.c:1305`): every hull trace, the engine's physics and the DLL's |
| `g_dll_traces` | the DLL's `TraceLine`, `TraceToss`, `TraceHull`, `TraceMonsterHull`, `TraceModel` (`sv_game.c:2125-2208`) |
| `g_clip_ents` | each entity `SV_ClipToLinks` takes from an area node's solid list for a trace, before `SV_ClipToEntity`'s filters: the list walk, not the clip tests (`sv_world.c:1204`) |
| `g_links` | each `SV_LinkEdict` (`sv_world.c:632`) |
| `g_push_tests` | each `SV_TestEntityPosition` (`sv_phys.c:192`): `SV_PushMove`/`SV_PushRotate`'s per-entity stuck test, inside-the-pusher test and after-push test, and #149's replays |
| `g_push_replays` | of those, the traces #149 replays at the end of a push loop (`SV_PushReplaySkipped`), at most one per moving pusher |
| `g_walkmoves` | the DLL's `WalkMove`, `MoveToOrigin`, `DropToFloor` (`sv_game.c:1443`, `:1872`, `:1905`) |
| `g_find_visits` | each edict `FindEntityByString`/`FindEntityInSphere` look at (`sv_game.c:1515`, `:1584`) |

**Times**, in kilocycles like the spans but cross-cutting, so not spans:
`g_think_kc` (inside the DLL's Think), `g_touch_kc` (its Touch) and
`g_move_kc` (inside `SV_Move`). Each times only the outermost call of
its kind, but they overlap one another and the movetype spans: a think
that traces counts in `g_think_kc` and `g_move_kc`. `g_worst_kc` is the
slowest single `SV_Physics_Entity` of the frame, `g_worst_cls` its
classname (1 + its row in the CLASSES chunk, 0 none) and `g_worst_ent`
its edict number; the report names it next to each of the top
`k_sv_game` frames.

**The CLASSES chunk** (type 7): per classname, the entity-frames
`SV_Physics` ran it, its thinks, its `SV_Move` calls, its time in
`SV_Physics_Entity` and in Think, and its slowest single run, over the
frames that start in play (warm-up frames included, menu and pause
frames not). The recorder writes one when a map closes - before the next
MAP chunk, and at the END - so each covers the map before it; every
chunk lists every classname seen in the recording, in first-seen order
(192 at most; later ones share an `(other classes)` row), so the
`g_worst_cls` rows mean the same in all of them. A classname is looked
up by its string pointer, one hash probe per entity, in a table
allocated when recording starts. The report prints each map's top
classnames by time.

First use (xemu, 2026-10-05, PR #181's main plus this breakdown): on
every map measured, `k_sv_ppush` was most of `k_sv_game` and `g_push_tests`
most of `g_moves` - the movers' per-entity position tests
(divergence #149). With #149, `k_sv_game` went from 3.60 to 0.98 ms on
c1a1b in xemu, and its largest parts are now the movers' own walk over
the edicts (`k_sv_ppush` 0.61) and the monsters (`k_sv_pstep` 0.23).

## Cost

- **Off:** one float compare per `Host_Frame`; in ref/nv2a one load and
  branch at each of five wait sites, and one add per work-counter site
  (an add costs what a test of "is the recorder on" would). No memory: the
  ring and buffers are allocated when recording starts and freed when it
  stops.
- **On:** ~610 KiB (ring 1024 x 508 B = 508 KiB, the overlay's histogram
  4 KiB, a 63.5 KiB encode buffer, 4 KiB of notes, the server's classname
  table ~30 KiB);
  a few `rdtsc` per frame plus two per span closed and two per server
  message (below); a ~149 KiB write and `fsync` every 5 s (those frames
  are flagged); about 29.8 KiB per second of play on disk at 60 fps, plus
  at most 4 KiB of notes per flush (105 MiB an hour; the file stops at 64
  MiB, ~36 minutes at 60 fps, ~30 at the 72 fps cap - the owner's 29-minute
  route of 2026-10-06 was 47 MB at 468-byte records, ~51 MB at 508).
  Before the menu, music and first-use load
  counters (2026-10-06) it was ~561 KiB and 27.4 KiB/s. Before the
  savegame spans (2026-10-05) it was
  ~507 KiB and 24.6 KiB/s. Before the server game frame's breakdown
  (2026-10-05) it was ~364 KiB and 18.8 KiB/s. Before the
  scene spans (2026-10-02) it was ~324 KiB and 16.6 KiB/s; before the
  mixer spans, ~292 KiB and 15 KiB/s; before the
  round-trip spans, ~243 KiB and 12.4 KiB/s; before any spans, ~130 KiB
  and 6.6 KiB/s.
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
frame records of 32 bytes plus one u32 per counter - 508 bytes with today's
119: the 20 counts, `span_calls`, the 72 spans, `world_walked`, `water_gpu`,
`vp_loads`, the 17 `g_*`, the five first-use load counters and
`cl_parse_kc`; 468 with
68 spans and no load counters, 420 with 56 spans, 328 with 50 spans and three counts after them, 284
with 42 spans, 256 with 35, 212 with the first 24, 112 before the spans),
SPANS (from 2026-10-01, right after COUNTERS: each span's name and its
parent's, 16 bytes each, "" for the top level), MAP, CALIB (TSC,
performance counter), CLASSES (from 2026-10-05: per-classname server
game frame totals, one per map, see above), NOTES (from 2026-10-06: named
one-off events, see "Notes" above) and END (written on a clean
stop; a file without it was cut short). The reader finds counters by name, so a later version can
add some without breaking it. The spans used that: they are counters plus
an optional chunk, which a reader that does not know it skips by its
length, so the version stayed 2. Version 1 is the same
file without COUNTERS and with 32-byte records; the report reads both. The authoritative field-by-field
layout is the comment at the top of
`engine/platform/xbox/xbox_frametime_util.h`; `parse()` in
`tools/frametime-report.py` reads it, and a file cut mid-record parses up
to its last complete record.
