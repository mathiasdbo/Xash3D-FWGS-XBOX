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

## The overlay (mode 2)

Top left, over the HUD and under the console: the last 120 frames as 40
columns of three frames, each column drawn as the slowest of its three
(so a one-frame spike still shows at full height), 50 ms tall, stacked
bottom up as **CPU** (green), **GPU wait** (orange) and the rest of the
frame (blue-grey: vblank, swap, and the time between host frames). The
cyan line (labelled `60`) is 16.7 ms, the red one (`30`) 33.3 ms. Under the
graph a legend: a square in each bar colour (drawn with `FillRGBA`, so it
keeps its colour - the console font tints its text orange) and its label,
`CPU`, `GPU wait`, `waits/vsync`. Then six lines, refreshed four times a
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
  (the half-second mean).

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
name, the load time (from the start of the frame before the first loading
frame - the frame that ran the `changelevel`/`map` and the server spawn -
to the first active frame; so it includes one ordinary frame), free memory
at map start (`MmQueryStatistics().AvailablePages`, after everything is
loaded), and free memory at the last flush before the load (up to 5 s
earlier). The first map after recording starts has no load time.

**Frame flags** mark frames in play (active, no menu or pause), frames that
wrote the file (their time includes the write and `fsync`), frames with the
overlay, and saturated counters (a frame over 5.8 s at 733 MHz). The report
and the overlay use in-play frames only and skip each map's first 30; the
report also leaves out the frames that wrote the file (`--include-flush`
keeps them).

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

## Cost

- **Off:** one float compare per `Host_Frame`; in ref/nv2a one load and
  branch at each of five wait sites, and one add per work-counter site
  (an add costs what a test of "is the recorder on" would). No memory: the
  ring and buffers are allocated when recording starts and freed when it
  stops.
- **On:** ~130 KiB (ring 1024 x 112 B = 112 KiB, the overlay's histogram
  4 KiB, a 14 KiB encode buffer); a few `rdtsc` per frame; a ~33 KiB
  write and `fsync` every 5 s (those frames are flagged); about 6.6 KiB
  per second of play on disk at 60 fps (23 MiB an hour; the file stops at
  64 MiB, ~2.7 hours).
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
  cost. The console's figure comes from the owner's run.

## File format

Version 2, little-endian: a 128-byte header (magic `R3DFTIME`, version,
header and frame-record sizes, TSC and performance-counter rates and the
pair at open, the mode, a build id with the XBE's link timestamp), then
chunks `{u32 type, u32 length, payload}`: COUNTERS (right after the header:
the counters' names, 16 bytes each), FRAMES (first sequence number, count,
frame records of 32 bytes plus one u32 per counter - 112 bytes with today's
20), MAP, CALIB (TSC, performance counter) and END (written on a clean
stop; a file without it was cut short). The reader finds counters by name,
so a later version can add some without breaking it. Version 1 is the same
file without COUNTERS and with 32-byte records; the report reads both. The authoritative field-by-field
layout is the comment at the top of
`engine/platform/xbox/xbox_frametime_util.h`; `parse()` in
`tools/frametime-report.py` reads it, and a file cut mid-record parses up
to its last complete record.
