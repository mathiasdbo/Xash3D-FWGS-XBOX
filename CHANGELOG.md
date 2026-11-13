# Changelog

Changes made by this fork. Upstream's own history is intact and reachable with
`git log upstream/master`; this file records only what Resonance3D adds.

## [Unreleased]

### Added - a release guard for saved games (XS.0)

Saves store a function pointer as an offset from `GameDLLInit`, so a build that moves the server code makes saves written by an earlier build load against the wrong functions. `tools/save_layout_check.py` compares an XBE's server export table, the SDK's dispatch table and the first 32 bytes of each of the server's roughly 3,000 functions with `tools/save-layout-baseline.txt`, made from the v0.2.0 build (bytes that differ between the three public builds are masked, since they hold addresses). `tools/packager/make_payload.py` refuses an XBE that differs, before it writes anything, and has no option to name another baseline. Both `release.py` and the packager workflow go through `make_payload.py`, so a release cannot ship such a build. The three public builds pass; a moved export, a moved `GameDLLInit`, a moved dispatch entry, a changed callback and two callbacks trading places each fail. The baseline can only be made from the three public builds (their hashes and the file's own digest are pinned in the tool), so regenerating it to fit a new build fails until those are changed on purpose; the few functions it cannot fingerprint are listed in it. Names in saves and reading the old saves through a table are the next steps (XS.1 to XS.3 in `docs/r3d/fork-plan.md`).

### Added - the legacy table: what an `ofs:` in an old saved game means (XS.1)

`tools/save-legacy-table.txt` lists the 230 server functions game code can store in a saved think, touch, use, blocked or move-done pointer, each with its offset from `GameDLLInit` and the field it goes into. It was found from the compiled objects of the v0.2.0 build, not from the source text: a function can only be assigned if its address is stored, and in an object that is an absolute relocation against its symbol. A function is listed when the store into one of the five fields can be read from the code (222 of them) or, when the address went through a register, when it is a non-virtual member of one of the three shapes those fields hold (8). Exports, dispatch functions, static initialisers and virtual functions are not in it, and an offset that is not in it resolves to nothing. Every one of the 92 offsets in 24 saves from a console resolves, and 229 of the 230 offsets are unique. The one that is not, -210368, is `CBaseMonster::CallMonsterThink` in a think pointer and `CFuncPlat::CallHitBottom` in a move-done pointer, and the table tells them apart by field. `tools/save_legacy_table.py` generates the table (the objects and link map stay outside the repository), checks it, and resolves an offset; the table's own digest is pinned in the tool, since it is frozen. Nothing reads it yet: XS.2 and XS.3 write names and read old saves through it.

### Fixed - Packager: the PC's saved games no longer go to the console

The packager copied the `SAVE` folder of the PC's Half-Life install into the game folder with everything else. Those saves were written by the PC game and are not the console's; it now leaves the folder out and says so in the log. The console's own saves, in `UDATA`, were never touched and still are not.

PC saves are not supported on the console: one may load and come up broken (no inventory, and other faults). The site's Troubleshooting page and the packager's README say so. The packager does not remove PC saves an earlier export already copied.

### Changed - the site on v0.2.0 and sound in emulators

The install page has a Playing in an emulator section (xemu and xbased, v0.2.0 or later for sound), the Troubleshooting page has No sound, or only music, in xemu or xbased, and the Building page names xbased beside xemu.

### Fixed - no sound in xemu and xbased

With Hardware audio off there was no sound in either emulator, and with it on only the music played: no effects, no speech. Both emulators give the AC97 chip no audio output and never run its S/PDIF engine, and an Xbox plays the SDL backend and the audio processor's output through exactly that. The game now finds out at start (it plays two silent buffers and watches the S/PDIF engine) and on an emulator uses OpenAL on the audio processor with the room effect and the sounds in the CPU mix, which both emulators play. A console is unchanged. The audio driver also no longer stalls after two callbacks when S/PDIF never moves, so the frame-time recorder's mixer spans mean something in xemu. `sndbackend.txt` has an `emulator:` line.

### Added - console frame-time reports for the v0.1.3 build

`docs/r3d/reports/play-v013-*.html`: four play-through recordings from a retail Xbox with the v0.1.3 build, about 4.5 hours from We've Got Hostiles through Blast Pit and Power Up to c2a2, as interactive reports (frame rate per map, CPU and GPU time, memory at each load). They are listed on the Performance page. Average 59.1 to 59.7 fps in all four; the dips are in c1a3b, c2a1a and c1a4j, and Blast Pit's c1a4i is the heaviest map on the CPU (11 to 12.6 ms per frame).

### Added - stick dead zones in the Controller Settings menu, and a one-time repair of the PC's zeros

The Controller Settings screen has four sliders, one per stick axis (look left-right and up-down, move sideways and
forward-back), 0 to 16000 with the value shown, for the engine's `joy_*_deadzone` (divergences.md #218). At start the
engine also puts a saved dead zone below 4096 back to the Xbox default (8689) once, then never again (#217): a console
that ran the PC's `config.cfg` copied by the old export saved zero dead zones, and an original Xbox pad's resting offset
turned the view, or walked the player, by itself. Leaving the screen with B now saves, as Done does: before, B dropped every change made there. Needs a new XBE build.

### Added - a Troubleshooting page, and a clearer stick-drift warning

`site/troubleshooting.html`: the view turning by itself (what causes it, the Fix stick drift preset with screenshots, the
by-hand file, and starting the settings again), the Windows warning for the unsigned exe, FTP problems, and a black world.
In the Console settings tab the stick-drift warning now comes first in "What the console runs with" (the box is short and
the values used to push it out of sight), is one grouped line, and updates when the preset is chosen.
`tools/packager/capture_screenshots.py` also captures the two drift states.

### Added - the packager can fix stick drift on an install

The Console settings tab has a Controller group with the four stick dead zones (`joy_yaw_deadzone`, `joy_pitch_deadzone`,
`joy_side_deadzone`, `joy_forward_deadzone`) and a "Fix stick drift" preset that sets them to the Xbox default, 8689. It
writes them to `userconfig.d/50-packager.cfg`, which runs after the console's saved `config.cfg`, so it repairs a console
that saved the PC's zero dead zones. "What the console runs with" warns when a dead zone is under 4096. Pairs with the
export no longer copying the PC's `config.cfg`.

### Fixed - the packager no longer copies the PC's controller settings to the console

The export copied the whole `valve` folder, so PC Half-Life's `config.cfg` (every `joy_*_deadzone` set to 0,
plus its `unbindall`), `autoexec.cfg`, `controller.cfg` and Steam Input's `controller_configs/` reached the console.
On a console with no saved config the engine ran that `config.cfg`, and original Xbox pads rest off centre
(divergences.md #104): the view turned by itself. The packager now leaves those files off and ships Xbox ones in
their place: a `valve.rc`, and `xbox_config.cfg`, `xbox_controller.cfg` (the 8689 dead zone) and `xbox_sound.cfg`,
run before `config.cfg` so the console's own saved settings still win. Consoles that already saved zero dead zones
keep them until the engine resets them or the setting is changed.

### Added - a documentation site

`site/` holds guides as static pages: install with the packager, the console
settings, building, the frame-time recorder, how the port differs from Xash3D
FWGS, and credits. A manual *Pages* workflow publishes it once GitHub Pages is
enabled for the repository. The README now links the guides and `docs/r3d/`.
The packager README's cache path lost a stray control character.

### Changed - the project is named Xash3D-FWGS-XBOX

The project, formerly Resonance3D, is now Xash3D-FWGS-XBOX, and its packager the
Xash3D-FWGS-XBOX Packager (window, exe and zip names, `%LOCALAPPDATA%` bake
cache folder; an existing cache under the old name is moved, not rebuilt). The
recorder header, the link-check XBE titles and the tool banners follow. Left
under the old name: the `// Resonance3D (#NN)` markers and copyright lines in
the source, past changelog entries, the `r3d_` cvars, and the `resonance3d`
branch of the forked submodules.

### Changed - Xbox: the build string says "xbox", not "win32"

The console banner, the log header, the crash text, the `host_ver` cvar and the
master-server query named the operating system `win32`, because the Xbox build
targets i386-pc-win32. They now say `xbox` (divergence #216). The engine's name in those strings, the
console title and the HTTP user agent is "Xash3D FWGS-XBOX" on the console build.

### Added - Xbox: the dashboard shows the game's name and picture

The console's memory screen lists a game from `TitleMeta.xbx` and
`TitleImage.xbx` in its `E:\UDATA\<title id>\` folder, and nxdk builds write
neither, so the game's folder showed with no name. On first start the engine
now writes `TitleMeta.xbx` ("Half-Life") and copies `TitleImage.xbx` from the
`titleimage.xbx` the packager puts beside the XBE. Files already in the
folder are left as they are. Not yet run on the console.

### Added - Packager: a Console settings tab, and uploads that ask before replacing

`tools/packager` has a second tab that reads the settings a console install
runs with (the video output, anti-aliasing, anisotropy, the FPS counter, the
frame-time recorder and its label, the memory report, audio, a start map),
shows which file each value comes from and what overrides it, and writes them
to one file of its own, `userconfig.d/50-packager.cfg`. It switches the user's
own cfgs on and off by renaming them to `.off`. It reads and writes an install
on the console over FTP or an exported folder on the PC.

An upload to an install already on the console now says what it would replace
and asks first: the console's `default.xbe` is kept as
`default.xbe.before-packager`, and `config.cfg`, `rt.cfg`, `custom.hpk`,
`userconfig.d` and `save` are left as they are (the command line needs
`--overwrite`).

### Added - Packager: bakes the Xbox data from the user's own game files

A packaged build started with a black world, models only, and an unfinished
menu: the console draws the world from per-map `.mpak` archives, the menu
background from baked tiles, and needs `r_refdll "nv2a"` and the menu's
control images. `tools/packager` now runs `tools/asset-studio` on the user's
install (maps in parallel, the menu art, the skies as DXT1, the music at both
rates), keeps the
result in a cache, leaves the retail `.bsp` of every baked map off the disc
(a loose `.bsp` is loaded in place of its archive), writes `video.cfg` and
adds the control images from `xash-extras`. `tools/packager/build_baker.py`
builds the baker for the host; `UiPacker.h` gains the `<cstdint>` include GCC
needs. Checked in xemu: the packaged disc boots to the full menu, and with a
map loaded draws the lit world exactly as the console's own install does.

### Changed - Xbox: cheaper bone setup in the client library

The client library takes a bone's sine and cosine from one `fsincos` per angle instead of separate calls, and
remembers a bone's quaternion by its angles, since most calls repeat one already computed (94% on the Office
Complex route). On a console, per bone: 5.35 us before, 4.09 us now. Over the route, bone setup went from 0.754
to 0.699 ms a frame and the client's studio total from 1.26 to 1.18 ms (two runs of the same route, so read
those coarsely). The memo is 128 KiB of static data in the client. The two changes are pull requests #3 and #4
of the SDK fork; both are merged, and `deps/hlsdk` is pinned to the merge (divergences #213, #214).

### Changed - Xbox: faster frames in Office Complex, cheaper hardware audio

Measured over the same 45-minute route at 720p on a console:

- Office Complex holds 60 fps more often: frames over 33 ms fell from 5% to
  about 1-3% on c1a2 and from 4.5% to 1% on c1a2a. The renderer sends the
  GPU its work in larger batches, merges a brush entity's faces into fewer
  draws, and reuses a model's lighting while nothing around it changed.
  Single player's entity snapshots hold 128 entities instead of 64, so the
  server stops sending every entity whole in rooms with more than that
  (5-6 ms a frame in the offices).
- Hardware audio costs the main thread less: about 0.30 ms a frame, from
  about 0.45 ms. The audio library no longer reads every sound source each
  frame, and the mix and its upkeep run every other frame.
- Monsters' route checks are cheaper. Each trace now rejects entities
  outside its box before reading their model, and the world's entity grid
  is finer.
- Sounds can be held compressed in memory (`r3d_snd_adpcm 1`, from the
  next map): the audio chip's ADPCM, a quarter of the 16-bit size.
- Explosions that throw dozens of debris pieces hitch less: each new
  piece's geometry goes to the GPU in the frame it appears, instead of two
  a frame with the rest drawn on the CPU meanwhile.

### Fixed - Xbox: the crash loading c1a2a from Office Complex, models untextured after a load

- The renderer's texture animation table ran two bytes past its block on
  maps with an odd number of texinfos (c1a2a has 2057), and the heap
  aborted the load.
- The level change's save buffer (up to 4 MiB) is freed once it is
  written, instead of being held until the new map has spawned: at 720p
  that ran the same load out of memory.
- Models no longer flash untextured after a level load: the skins that
  were on screen are loaded again during the load, not a couple a frame
  after it.

### Added - Xbox: 720p, widescreen and image quality settings

The Display screen sets 1280x720 where the Dashboard allows it, anamorphic
16:9 at 480 lines (following the Dashboard, or forced 4:3 or 16:9),
anti-aliasing of the 3D view (2x, Quincunx or 4x) and texture filtering
with anisotropy. `r3d_memreport` writes where the memory goes at each map
start.

### Changed - Xbox: hardware audio from the menu, speech on the audio chip, an even machine gun

The Audio menu has a "Hardware audio" switch (from the next start) that
puts the whole mix on the console's audio chip - the room effect, the
sounds, the music and now the spoken lines too - and a "Voices volume"
slider for speech alone. On the chip, a held machine gun now fires evenly:
each shot starts at its frame to within a few milliseconds, where before
some started late with their first crack cut off and some ended early.
`r3d_snd_meter 1` logs the final mix's level every two seconds to
`sndmeter.txt`, to compare the chip's loudness with the CPU mix's.

### Changed - Xbox: dialogue streams from the disk instead of loading whole

Every line a character speaks used to be read from the disk whole, in the
middle of a frame, the moment it started: 5 to 16 ms for a long scientist
or guard line, and every word of an announcement loaded again whenever
another sound needed a channel. Spoken lines now stream: the console reads
a small piece of the file ahead of what is playing, in the background, and
the next word of a sentence is opened while the current one plays; long
precached speech (over 64 KB) streams the same way. A line
starts after a short read instead of a whole one, and long speech no
longer sits in memory for the whole map (about 36 KB per line playing,
and as much for the next word being opened). What you hear is unchanged, sample for sample, including the
mouth movements and where a sentence resumes after loading a save; a host
test checks that against the original loading over every sentence in the
game. Short words, unusual formats and anything the stream cannot serve
still load the original way. `r3d_vox_stream 0` switches back to the old
loading for comparison, `r3d_vox_ab` flips it, and `r3d_vox_crc all`
checks every sentence on the console. The frame-time recorder counts the
streams' reads and waits and says why any sound was still loaded in play.

A second step, a speech pack laid out in the order the game speaks it, is
planned next.

The scripted xemu routes (`tools/xemu-route-vox.sh`, `tools/xemu-route-bmi.sh`)
now fail when the route did not run to its end: they check the recording
itself (`frametime-report.py check`: the maps in order, a clean end, the
dialogue counters, and an end marker the route records last) instead of
reporting success after a fixed wait.

### Added - Xbox: sound output through OpenAL on the console's audio chip (`r3d_snd_backend`)

The console can now send the engine's sound to the speakers two ways,
chosen at boot: through SDL's audio thread as before (`r3d_snd_backend sdl`,
the default), or through OpenAL running on the Xbox's own audio processor
(`r3d_snd_backend al`, set in a `userconfig.d` cfg, or `-sndbackend al`).
The sound itself is the same mix either way; this is the first half of
X12 item 5, built so the two paths can be timed against each other on the
Black Mesa Inbound route. If OpenAL cannot start, the console falls back to
SDL. The frame-time recorder's audio counters mean the same on both.

### Added - Xbox: smoother sound effects, on by default (`s_lerping`)

Half-Life's sound effects and voices are recorded at 11 or 22 kHz, and
since the console mixes at 48 kHz each one is stretched by repeating its
samples unevenly, which adds a faint metallic edge. Upstream's option to
blend neighbouring samples instead (`s_lerping`) now runs on the console
without the slow floating-point conversions it would cost there: the
extra tones drop by 17 to 36 dB on a test tone in an 11 kHz sound (12 to
33 dB in a 22 kHz one) and the sound's own "staircase" by 5 to 27 dB,
with no extra memory. The console's listening test preferred it, and the
tram ride measured it at 0.015 to 0.045 ms a frame, so the Xbox turns it
on by default (upstream keeps it off; `s_lerping 0` restores that).
`r3d_mixbench` times the mixer on the console.

A survey of every map's sounds showed why the alternative, storing the
effects already converted to 48 kHz, does not fit: every sound in the
game is 8-bit, a map holds 2 to 5 MB of them, and converting them all
would add 9 to 30 MB per map where 13 to 18 MB are free.

### Changed - Xbox: sound mixed at the console's own 48 kHz

The console's sound chip only plays 48 kHz, while the engine mixed at
44.1 kHz, so a background thread converted every block of sound before
it could play - work the frame-time recorder could not see. The engine
now mixes at 48 kHz on the console and the blocks go straight to the
chip. Sounds keep their pitch and length, the room echoes keep their
delays, and saved games keep their music position. The music is baked a
second time at 48 kHz (`tools/asset-studio --music-rate 48000`,
`media/<Track>.48k.xwav`) with a high-quality filter, so it is not
roughly converted while it plays; the 44.1 kHz bake stays beside it.

`r3d_snd_rate 44100` in a userconfig.d cfg switches back to the old
mixing for comparison; it is chosen at boot (a change once a map has
loaded applies at the next boot, because switching in play could leave
the console without sound). The engine reads that line from the cfg
files before it opens the sound device, so it takes effect even when
another cfg loads a map first. The output is no longer
identical to upstream's: Half-Life's 11 and 22 kHz sounds are stretched
to 48 kHz unevenly, which adds faint extra tones to them, and a
listening test on the console (`docs/r3d/x12-mix48-listening-test.md`)
decides whether that is audible. The frame-time recorder now measures
the sound thread's work, the buffers it hands over, any dropouts and the
mixing rate of every frame, so the console recording can show the saving.

### Changed - Xbox: baked music reads ahead while the game runs

On the console, the baked music still cost 0.13 ms of every frame, and
half of that was waiting for the hard disk: every fifth of a second the
player stopped for 0.5 to 1 ms while the next 9 KB of the track was read.
The music now keeps two 16 KB pieces of the track in memory and asks the
console to fetch the next one in the background as soon as it starts
playing the current one, so the disk works while the game runs and the
frame that needs the data finds it already there. The music sounds and
behaves exactly as before - the same samples, start, end, looping, pause
and saved position - and if a file cannot be read that way it is read as
before. It uses about 24 KB more memory while a track plays. The
frame-time recorder now counts the music's reads, which ones ran in the
background and which ones the game had to wait for, and the time the
music's decoding and reading take, so the next console recording can
confirm the saving.

### Changed - Xbox: music plays from tracks baked offline

The music was decoded from mp3 on the console as it played, which took
about half a millisecond of every frame while a track was on - as much as
all the other sound together. The baker (`tools/asset-studio --music`)
now decodes each retail track ahead of time with the engine's own mp3
decoder and stores it in the console's ADPCM format, as
`media/<Track>.xwav` beside the mp3; the console plays that instead, with a
table lookup per sample. It is the format the console's sound chip
decodes in hardware, for a later step. The 27 tracks take 109 MB on the
disc (the mp3s, 53 MB, stay as the fallback). In the emulator, decoding a
second of music takes 4.6 ms of processor time instead of 29.5 - about
0.08 ms of each frame instead of 0.49 - and the console's own decoders
give exactly the samples the baker recorded, on all 27 tracks.

The music behaves exactly as before: the same start, pause, volume,
looping and end of each track (the last fraction of a second the mp3
player never played is still not played), and saves keep the track and
position, so a save from either build loads on the other. The baker
checks every track against the real mp3 player request by request, and
refuses a track whose sound would be noticeably noisier than the retail
set; such a track can be shipped as plain 16-bit audio instead. A track
without a baked file, or with one that does not match its mp3, plays from
the mp3 as before. `r3d_music_baked 0` plays the mp3s on the same disc,
and `r3d_music_ab` switches mid-track for comparison.

### Changed - Xbox: chrome surfaces on weapons and characters drawn on the graphics chip

The shiny "chrome" parts of models - the gloved hands and metal of every
first-person weapon, spent shell casings, the guards' badges, a few
items - were the last studio-model surfaces drawn entirely by the
processor, and one of them made the processor redo the whole body part
it sits on. They now go through the graphics chip's vertex unit like the
rest, with the same reflection coordinates. In the emulator, with a
weapon out on c1a0c, studio-model drawing takes 0.32 ms a frame instead
of 0.95, and the whole frame's processor time drops by about 0.55 ms
(7.75 to 7.18); the guard on the c1a0 tram costs 0.27 ms less. Screenshots of 13 scenes are
identical, or differ by at most one colour level on a few pixels.
`r_studio_chrome_gpu 0` gives the old drawing.

### Changed - Xbox: decal textures load faster

Finding a decal's texture by name checked every one of the 3,661 entries
in the game's texture files, for each decal texture a map uses. The
entries are already kept in sorted order, so the lookup now goes straight
to the matching ones and finds the same texture. In the emulator, the
static decals of c1a0c load in 53 ms instead of 95 ms (139 to 82 ms over
the four maps on the test route that have them). The first bullet hole or
blood splat of each kind in play is about 2 ms cheaper too. Screenshots
of decal walls and floors are identical before and after.

### Fixed - Xbox: decals no longer flicker out while moving

Since the depth-precision fix for studio models, blood, scorch marks and
bullet holes could vanish for a frame, whole or in pieces, as the player
moved: in the emulator, the depth offset meant to keep a decal in front of
its wall had no visible effect, and decals had only been drawn because they
tested equal to the wall, which the finer depth broke. Each decal is now
drawn a quarter of a unit off its surface. In the emulator, the same
strafing run that lost the decal in about a third of the frames now
shows it in every frame (`nv2a_r_decallift 0` gives the old drawing).

### Changed - Xbox: level loads about a fifth faster again

Four pieces of work every level load repeated for nothing are gone, with
the same game state at the end of the load:

- Loading a map's world looked up each moving brush's origin by reading
  the whole entity list again, once per brush - several megabytes of text
  on the bigger maps. It is now read once.
- Every file lookup asked the hard disk about two folders that do not
  exist and about game folders the save area never has. The file layer
  now remembers a folder the console said is missing, until a folder is
  created.
- The size of each of the ~350 precached files was found by opening it;
  for files on the disc it is now one query.
- Looking up a decal or texture by name listed the whole game folder on
  the disc, entry by entry, every time. Each disc folder is now listed
  once and replayed.

In the emulator the test route's 15 loads went from 10.7 to 8.7 seconds,
every map faster; on the console, where each of the removed disk calls
costs more, the saving should be larger.

### Fixed - Xbox: level changes no longer stall up to a second on the first update from the new map

The first update the game received after every level change spent a
long time marking old network frames as dropped: since the server
started picking a random starting packet number, that bookkeeping walked
through up to 134 million numbers to update 16 slots. On the console it
was 70-900 ms of each level change, different every time. It now
touches each slot once and leaves exactly the same state.

### Added - Xbox frame-time recorder: level loads broken down by stage

With the recorder on, each level load now records its stages - the old
level saved, the new one spawned, the world loaded, models and sounds
precached, the renderer's new map - with how long each took and how
much of that was spent inside file reads, seeks and opens, so a console
recording can tell disk waiting from CPU work. `frametime-report.py
summary` prints them per load, and `--compare` lists both sessions'
load times side by side.

### Fixed - Xbox: characters' clothes, mouths and hands no longer show through each other; no giant shotgun shell when looking down

On the Xbox renderer the depth buffer could only tell surfaces apart
when they were a whole world unit or more from each other. Characters
are built from layers much closer than that, so the inner one often
won: a security guard's shirt showed through his vest, teeth and the
inside of the mouth through closed lips, hands sank into sleeves. The
depth buffer now has about a thousand steps per unit, the same layers
come out in the right order, and the faces match the software
renderer's picture of the same frame.

Two more depth details the PC renderer has were missing. The weapon
in your hands is drawn with its own depth range so it does not sink
into a wall you are standing against; that adjustment was ported in a
form the Xbox's depth buffer ignores, and now works. And the PC (OpenGL)
renderer draws nothing closer than 4 units to the camera; the Xbox one
drew everything, so a shell that is part of the shotgun's model, about
a unit from the camera, filled the screen when you looked down. It is
gone now: the Xbox drops whatever is drawn closer than 4 units, pixel by
pixel. Unlike the PC renderer it does not cut a triangle that crosses
that distance into a smaller one; such triangles are drawn as before.

`nv2a_r_wscale 0` and `nv2a_r_znear 0` bring back the old behaviour of
each fix, so they can be compared on the console in one session.

### Changed - Xbox: lightning and laser beams cost about a third less CPU (xemu)

The frame-time recorder now breaks the effects pass down into beams,
tracers and particles, and it showed where the time went: in xemu, a
map with electric arcs (c1a0c, the test chamber in c1a0e) spent 2.2-2.4
ms a frame on its 8-15 beams, and most of it in the step that cuts
each beam's triangles at the edges of its texture. That step was
calling a rounding function whose Xbox version changes the processor's
rounding mode twice per call, four calls per triangle; it now rounds
with a plain conversion that gives the same numbers. The cut triangles
are also handed to the GPU's queue in groups instead of one at a time.
The picture is the same (screenshots of arcs, the egon and the gauss in
four maps matched pixel for pixel), and the beams pass went from 2.16 to
1.41 ms a frame in c1a0c and from 2.41 to 1.56 in c1a0e, in xemu. The
console's own numbers are still to be recorded. Tracers and particles
turned out to be small (under 0.07 ms a frame each).

### Added - Xbox: the frame-time recorder times the particle pass

The recorder now measures how long the renderer spends on particles
each frame, and how much of that is the engine moving them
(`part_kc`, `part_think_kc`); the report shows it per map next to the
effects time. It answered whether particles were worth moving to the
GPU: in xemu, during scripted fights with gunfire and grenades, they
cost about 0.05 ms a frame (well under 1 us each), so they were not. A
faster way of handing them to the GPU from the CPU was also tried: the
picture was identical, but it saved nothing measurable, so it was left
out. The console's own numbers are still to be recorded.

### Changed - Xbox: the menu fonts come back after a map load about 60 ms faster (xemu)

At the end of every map load, behind the loading screen, the menu
rebuilds its fonts from the font cache - about 230 ms on the console.
Most of that, in xemu, was turning each cached glyph atlas back into a
full-colour image and then shrinking it again to the 8- or 16-bit
texture the Xbox keeps. The renderer now recognises the cache's exact
file shapes and copies them straight into the texture: the same
texture, byte for byte (a host test compares the two paths on thousands
of atlases, and an emulator build compared every atlas it loaded), with
anything unusual taking the old path. In xemu the rebuild went from
84-88 ms to 23-26 ms per load and no longer needs a 1 MiB scratch
buffer; what is left is mostly reading the cache files. The console
figure is still to be measured. The frame-time recorder counts these
uploads (`tex_fast`).

### Changed - Xbox: frame-time recordings are written as small, labelled files per session

A frame-time recording used to be one file of up to 80 MiB: slow to
copy off the console, stopped after about 33 minutes, and lost entirely
if the game crashed before the end. A recording is now a session written
as a series of small files: a new one at every map start and every 8 MiB
(`host_frametime_segment_mb`). Each file stands on its own, so a crash
only loses the last few seconds of the file being written and the
fetch can skip the files it already has. The files are named after
what the session measures - `host_frametime_label`, e.g. `pr190-clents`
- and a session id, e.g. `ft_pr190-clents_a8646741_002.bin`, within the
console file system's 42-character limit.

`tools/frametime-report.py` fetches by label or session, joins a
session's files back into one timeline (its tables are identical to what
one file of the same run gives: the test suite writes a session both
ways and compares them), reads a single file on its own, and compares
two sessions map by map (`summary --compare A B`). Older recordings
read exactly as before. The recorder costs nothing more per frame; a
session now stops at 512 MiB (about 3.5 hours) instead of 80 MiB.

### Changed - Xbox: the client decodes entity updates and rebuilds its collision lists with less work

Every frame the client decodes the entity updates the server sent and
rebuilds the list of solid entities its movement prediction collides
with. Two parts of that did work they did not need, and both now give
exactly the same result for less:

- An entity update carries a yes/no flag for each of the entity's 56
  fields, and usually only one or two are set. The flags are now read
  many at a time instead of one call each. Every update in a scripted
  session (13,088 entity updates, 1,712 player-data and 4,815 weapon
  updates) was decoded both ways and compared: no difference.
- The solid-entity list was rebuilt three times a frame from the same
  data. It is now rebuilt once per new update; the second call only
  redoes the one choice a rebuild can make differently the second time
  round (which model a slot uses, a quirk of the original code), and the
  third is skipped. A check build rebuilt the list anyway at each of the
  7,772 skips and compared it byte for byte: no difference.

In the emulator on c1a0c the update decoding goes from 0.027 to 0.010
ms a frame and the list building from 0.073-0.080 to 0.032; the
console's figures need a hardware run.

The frame-time recorder breaks the client's entity work down (`ce_*`
counters: entities, updates, fields, visible entities, the skipped
rebuilds, and the time in decoding, frame processing, linking, temporary
entities, events and the camera's pitch traces), and its report prints a
client-entities table. Records grow to 700 bytes, so a recording file
can now reach 80 MiB (about 33 minutes at 60 fps) instead of 64.

### Changed - Xbox: player movement checks only the entities near the player

Moving the player costs about 0.8 ms a frame on the console, split
between the server running the movement and the client predicting it.
The client tests every move against every solid entity the server sent
(42 on c1a0c, against the server's 6 within reach), so it did eight
times the collision work for the same movement. A collision test against
an entity is now skipped when no solid part of that entity can be near
the move, which leaves every result exactly as it was: the player's
position, speed, footing, water level and the prediction were compared
frame by frame over a scripted session (a tram ride, walking, jumps,
ducks and crouch-jumps among scientists, a ladder, wading and swimming)
and came out identical, and a check build compared every skipped test
with the full one (2.7 million, no difference). On c1a0c the client's
collision tests drop from 336 to 19 a frame; in the emulator the
client's prediction goes from 0.28-0.29 to 0.22-0.23 ms a frame and,
where the player is stuck against something (c1a1b in that script),
both sides from 0.66-0.73 to 0.50-0.53 ms. The console's figures need
a hardware run.

The frame-time recorder breaks player movement down (`pm_*` counters:
moves, the entity lists, traces, collision tests and how many were
skipped, and the time in setup, movement, its collision queries, the
game's think functions and the client's list rebuild), and its report
prints a player-movement table. Records grow to 644 bytes.

### Changed - Xbox: an autosave no longer writes its save file in the frame it happens

An autosave still froze the game for one frame of 206-289 ms on the
console. About half of that was writing the save file, which carries a
copy of every map visited in the chapter and grows as the chapter goes
on. The game is still saved in that frame: everything that can change
afterwards is taken then. The save file itself is now written over the
next half second or so, 32 KiB a frame, under a temporary name, and
renamed when it is complete, so the menu, a load or a power cut never
see half a save. Anything that needs the save folder first finishes the
pending save at once: a level change, loading a game, another save, the
load and save menus, deleting a save and quitting. The finished file is
byte-for-byte what the old code wrote. In the emulator the autosave
frame drops from about 140 ms to about 103 ms, and each following frame
spends a few milliseconds writing; the console should gain more, since
its disk writes were about twice as slow. `sv_save_async 0` restores the
old behaviour.

The savegame thumbnail is read from the screen in half as many memory
reads (37-38 ms on the console before), with the same pixels.

The frame-time recorder shows the deferred write as `k_save_pump`, with
`save_pump_kb` and `save_flush_kc`, and its report lists each deferred
write. Divergence #153.

### Changed - Xbox: the first pause after a map load no longer freezes

To leave more memory for each map, the menu gives back its fonts while a
map loads (divergence #127). It used to rebuild them the first time the
pause menu opened, which froze that frame for about a third of a second
on the console. The fonts are now rebuilt at the end of the map load,
while the loading screen is still up, as long as at least 8 MiB of
memory is free; with less they are rebuilt when the menu opens, as
before. Opening the pause menu is now as fast the first time as later.
Each map load takes about 300 ms longer on the console, inside the
loading screen. The fonts are now given back as each load begins, so a
level change no longer carries them through its memory peak.

The font cache on the hard disk is about a quarter of its old size
(1.2 MB instead of 4 MB). It stores glyphs at 8 bits per pixel instead
of 32, so a font rebuild reads less and uses less temporary memory. The
menu text is pixel-identical. Old cache files are never read; they are
deleted the first time the new cache is written. Divergences #127 and
#152.

### Added - Frame-time recorder: the menu, the music, and named one-off events

The owner's 29-minute route recording had four single frames of about
320 ms that were not autosaves. Three of them were the first time the
pause menu opened after a map load: the menu rebuilds its fonts and
background then, because it gives that memory back to each map
(divergence #127). New spans now show the menu's drawing and the files
and pictures it loads, and the music stream's own time. The fourth, in
c1a1b's first fight, was 300 ms spent reading one server packet, and
nothing in the recording could say what was in it. Every frame now also
counts the sounds and textures loaded on first use, and the recording
names, with their times, each sound loaded during play and each server
message that took 5 ms or more to read, so the next console run says
which one it was. Divergence #141 (11).

### Changed - Xbox: autosaves hitch for about a third as long

Every autosave froze the game for one frame of 320-425 ms on the
console, the most visible hitch left in the campaign. Almost none of
that was the game working out what to save: it was the save files being
written. The engine wrote each file a few bytes at a time, about 130
separate write calls per save, and on the Xbox every call is a slow trip
into the system's file code however little it carries. It also set
aside and cleared 1 MiB of memory for every map's state it copied into
the save, and cleared two 4 MiB buffers it never needed cleared. The
system turned out to read and write much faster when the memory involved
starts on a page boundary.

Each save file is now assembled in memory the save already has and
written in one go, page-aligned, with nothing extra allocated and
nothing cleared that is not needed. The files are byte for byte what
they were: saves made the old way and the new way in the same frame
are identical. In xemu, with a save carrying twelve maps, the autosave
frame went from 407-424 ms to 140-179 ms. Divergence #151.

### Added - Frame-time recorder: where a save spends its time

Twelve new spans break a save down: the save buffer's setup, the game's
own serialisation, each file written, the aging of old autosaves, the
.sav itself, and the thumbnail (read back, resized, written). The
report's stall table now names the three parts of each stall that took
the longest. Divergence #141 (10).

### Fixed - Xbox: no sound for a whole session on some builds

Some builds had no sound at all on the console, every boot, while builds
differing only in unrelated code played normally. The cause identified
is in the audio driver that came with nxdk (the Xbox SDK this port
builds on). After the sound hardware runs out of queued audio even once
(an underrun), the driver ignores the next "buffer played" signal. On
hardware that stops at the end of its queue, that signal is the one the
audio thread is waiting for, so it waits forever and sound never comes
back. A single underrun at startup is enough, and whether one happens
depends on when the system first runs the audio thread, which shifts
with every code change.

The engine now carries its own copy of that driver, so nxdk's is no
longer linked. The copy works out which buffers have been played from
the sound hardware's own position registers rather than by counting
signals. It recovers from an underrun however the hardware behaves, in
a host test of both behaviours where nxdk's version deadlocks. A
watchdog also checks every frame: if the driver has been given no audio
for a second while sound is on, it writes the driver's state to the
console and restarts the buffer accounting. The diagnosis comes from
the code and that host test. Owner-verified on the console: the
combination that was silent on every boot before (this fix merged with
the x9-perf-svgame branch) now plays sound, and so does this branch on
its own. Divergence #150.

### Changed - Xbox: moving doors, trains, pendulums and fans no longer test every object in the map

Every frame, each moving brush entity (a door, a lift, a train, a
swinging pendulum, a fan) checked every movable object in the map with a
full collision trace before throwing almost all of them away with a cheap
box test. On c1a1b, with 17 pendulums swinging, that was 734 traces a
frame, and it was most of the server's game logic, which the console
spends 6-7 ms a frame on there. The cheap test now comes first for the
objects it would discard anyway; the trace results the game can read are
kept exactly as before (the last skipped trace is redone when its result
is the one left behind). Traces per frame went from 734 to 38 on c1a1b,
316 to 24 on c1a1c, 78 to 11 on c1a0c and 84 to 7 on c1a1f. A scripted
session over those four maps, run with a fixed random seed, gives the
same state for every entity on every server frame as before, and a save
that differs only where two runs of the old build already differ (stack
addresses the save picks up). In xemu the server's game logic went from 3.60 to 0.98 ms
a frame on c1a1b, 1.78 to 0.54 on c1a1c, 1.12 to 0.69 on c1a0c and 0.93
to 0.43 on c1a1f; the console's figure needs a hardware run.

### Added - frame-time recorder: the server's game logic broken down

`k_sv_game`, the server's game logic, is now split by the kind of entity
(monsters, moving brush entities, falling objects, think-only entities)
and the game DLL's `StartFrame`, with counts of thinks, touches,
collision traces and the like, the slowest single entity of each frame,
and a per-map table of the entity classes that cost the most. The report
tool shows all of it, and names the slowest entity of each of the worst
frames. A recording grows to 420 bytes a frame (24.6 KiB a second) and
~507 KiB of memory while it runs; nothing changes with the recorder off.

### Fixed - frame-time recorder: an 8-byte stack overflow on every flush

Each time the recorder wrote its file it put a 24-byte record into a
16-byte buffer, over the registers it keeps for its caller. It went
unnoticed until a change around it crashed the game in xemu on the first
flush in a map; the data in recordings made so far is not affected.

### Fixed - Xbox: each controller press counted once

On the Xbox the engine and the Half-Life client both opened the controller.
The client turned every button into an extra key, so B (use) also fired
whatever the right-trigger key was bound to. It also added a second look
and move driven by its own cvars, which the menu never sets. The client
(deps/hlsdk) no longer opens the controller on the Xbox, so the engine's
gamepad bindings and the menu's controller settings are the only ones in
effect. Owner-verified on the console. The menu submodule also moves to
its merged `resonance3d` branch, with no change in content.

### Changed - Xbox: water warps on the GPU's vertex unit

The water warp (the waves and the swirling texture of every water, slime
and laser surface) was computed on the CPU for every water vertex every
frame: on the console it cost 2.73 ms a frame on c1a1b, the slowest map.
Each map's water is now copied once, at load, into a vertex buffer, and a
new vertex program on the GPU does the per-vertex work; the CPU only sends
the draws. It is the reference's own stepped wave, not a smoothed one -
the same sine table, the same truncation of negative values - and in
xemu, screenshots at eight fixed spots (still and moving water, seen from
above and below, opaque and translucent) are pixel-identical to the CPU
path. In xemu the water's own time went from 1.31 to 0.04 ms on c1a1b,
0.51 to 0.05 on c2a2a and 0.95 to 0.07 on c1a4g; the console's figure
needs a hardware run. The buffer costs 20 bytes per water vertex (0.95
MiB on disposal, the largest), counted in the map budget, and the bake
now also counts the CPU-side water data that was never budgeted. The CPU
path stays behind `r_warp_gpu 0`, and two new frame-time counters show
how much water the GPU drew and how often the water program was loaded.

### Changed - Xbox: the world pass visits only the faces in view of the player's area, and the 3D scene in the frame-time spans

On the console the world pass cost 1.7-2.3 ms a frame on every map. Eight
new frame-time spans split the 3D scene - the sky, the world's face walk,
its water and decals, the visibility update, the brush entities and their
water, and the translucent water - and a new count says how many faces
the world walk visited. Nearly all of the world's time was the face walk,
which looked at every face of the map every frame (6,120 on c1a1b, to
draw 189) and ran its view-cone test even on faces already known to be
out of sight.

The renderer now keeps, for the area the player is in, the list of world
faces that can be seen from it, rebuilt only when the player moves into
another area, and the walk goes through that list. The view-cone test
picks each plane's box corner once a frame instead of per face, and the
back-face test reads each face's plane from a small table built once per
map. Exactly the same faces are drawn in the same order: a verification
build ran the old walk beside the new one every frame across seven map
loads (the tram ride, doors, firing, save and load, a level change,
visibility toggles) and compared every draw, texture change and counter
- 4,500 frames, 417,226 draw events, no difference - and
screenshots at fixed spots match the previous build wherever two boots of
the previous build agree with each other. In xemu
the walk went from 0.94 to 0.39 ms on c1a0c, 1.33 to 0.30 on c1a1f, 1.52
to 0.21 on c1a1b, 0.43 to 0.08 on c1a1a and 2.24 to 0.31 on c2a2a; the
console's figure needs a hardware run. It costs 5 bytes per map face.

### Changed - Xbox: cheaper sound mixing, and the mixer in the frame-time spans

Sound cost about 1.5 ms of a 15.4 ms CPU frame on the console. Seven new
frame-time spans break the mixer down - per-channel spatialization,
channel mixing, room effects, and the clear/volume/copy-out - once for
the main mixer pass and once for the extra pass after rendering.

Nearly all of it was channel mixing. Half-Life's sounds are 11 or 22 kHz
and the mixer resamples them to 44.1 kHz, converting a floating-point
position to an integer on every output sample - an expensive operation on
the console's Pentium III. The mixer now steps through the source with
integer arithmetic that lands on exactly the same samples, and a sound at
normal pitch has each source sample's volume worked out once for the 2 or
4 output samples it covers. The room-effect stereo delay also drops a
division per sample. What is heard is bit for bit the same: a test builds
the original and the new mixer side by side and compares 366,000 calls,
and verification builds compared every resampled call in xemu (71,000
calls) through speech, movement, firing, doors, save/load and level
changes, with no difference. In xemu (with a stand-in audio clock,
because xemu's audio callback never runs) channel mixing went from about
0.40 to 0.05 ms a frame on c1a0c, from 0.17 to 0.05 ms while firing on
c1a0e and from 0.25 to 0.02 ms with scientists talking on c1a0; the
console's figure needs a hardware run.

### Changed - Xbox: cheaper single-player network messages, and the round trip in the frame-time spans

In single player the server and the client still talk through the
network code every frame; on the console that round trip measured 2.8 ms
of a 14.4 ms CPU frame. Eleven new frame-time spans break it down
(entity packing in the game DLL, the clientdata and weapon deltas,
entity delta encoding, the netchan send, the player's movement on both
sides, the client's entity and clientdata parsing, the command packet),
and the report now lists every span under its parent.

The biggest engine-side part was delta encoding of things that had not
changed: most entities in each update and most of the 64 weapon slots.
Those are now recognised whole and written without comparing ~50 fields
one by one, and the netchan no longer clears a 32 KiB buffer for every
packet. What goes over the (loopback) wire is bit for bit the same: a
verification build compared every such message with the original code
through moving, firing, a save and load and a level change, with no
difference in about 250,000 checks. In xemu the server's send went from
0.60 to 0.28 ms a frame on c1a0c and from 0.59 to 0.28 ms while firing
on c1a0e (emulator timing); the console's figure needs a hardware run.
Movement prediction and the game DLL's own work are untouched.

### Changed - Xbox GPU studio path: resident vertex programs, muzzle flashes on the GPU (R2)

All four vertex programs now stay in the NV2A's program memory at once
and a switch is a single register write, instead of re-sending the
program through the pushbuffer every time the renderer moves between
the world, the HUD, the TriAPI and studio models. Studio constants are
only sent when they change, and each mesh's state goes out in one
pushbuffer block. Models lit by up to two entity lights (muzzle flashes,
and the controller's, gargantua's and nihilanth's lights) are now
skinned and lit on the GPU too; more lights, nihilanth's green light
and a few edge cases keep the CPU path. In xemu the 9mmAR firing scene
went from 73 to 66-67 KiB of pushbuffer a frame (peak 113 to 86-105
over three runs), studio skinning and lighting from 0.98 to 0.78 ms of
CPU (emulator timing), and its CPU-drawn studio vertices peaked at 1.0k
instead of 1.9k; the rest are the view model's chrome, still on the CPU. With a muzzle flash
lighting the view model, about 98% of the lit pixels match the CPU path
exactly and most others by one level; a handful of single pixels on
tiny triangles can differ more where a vertex sits right on the edge of
the light. The console's numbers need a hardware run.

### Added - studio models skinned and lit on the Xbox GPU (R1)

Studio models (NPCs, the view model, items) are now transformed and lit
by a VP20 vertex program on the NV2A instead of on the CPU, which was
the largest CPU cost the frame-time spans found (3.5 ms a frame on
c1a0c on hardware). Each submodel's vertices are built once, on first
draw, into a 1 MiB GPU cache; every frame only the bone matrices and
per-bone light vectors are uploaded, and each mesh is one indexed draw.
The picture matches the CPU path to within one colour level on a few
tenths of a percent of pixels (xemu, c1a0e, with and without the
flashlight). Models lit by a muzzle flash or other entity light, glow
shells, additive models and chrome meshes still draw on the CPU, as do
all models when the gamma or brightness setting is far from the default.
`r_studio_gpu 0` switches the GPU path off for comparison. In xemu
(emulator timing, ratios only) the c1a0e firing scene went from 2.64 to
0.50 ms of studio CPU time and from 7.29 to 5.03 ms of CPU per frame;
the console's numbers need a hardware run.

### Added - frame-time recorder: per-subsystem CPU spans (M2)

The recording now says where each frame's CPU time goes, so the next
Xbox optimisation (studio skinning on the GPU, the HL client's bone
setup, or the server game DLL) can be picked from a measurement. 24
nested spans cover every call `Host_Frame` makes: server (usercmds, game
DLL think/physics, delta encoding), client DLL frame, packet reading,
prediction, entity emit, the whole screen update (3D scene, lightmaps,
world, the client's studio-model draws and our studio skinning/lighting
inside them, skin reads, effects, HUD, `R_EndFrame`) and the sound mixer.
Each span leaves out the GPU waits inside it, so the spans plus an
"unattributed" rest add up to the frame's CPU time. They are stored as
named counters plus a new optional chunk, so the file stays version 2 and
older readers still open it. The overlay gets a line with the six largest
spans, and `tools/frametime-report.py` gets a per-map span table and the
frames with the most game-DLL and studio time. The spans cost two
time-stamp reads each, only while recording. `host_frametime_spans 0`
switches them off to measure that cost. With the recorder off, each site
costs one load and branch.

### Added - `tools/xbox-build-nxdk.sh`: nxdk's libraries at -O2, reproducibly

nxdk's own libraries (its C library, pbkit, SDL2, libc++...) were built
with no optimisation flag at all. The script rebuilds them from a clean
state at a chosen level (-O2 by default) with nxdk's own makefile and
flags plus the level, without editing anything inside `deps/nxdk`, and
writes the level to `deps/nxdk/lib/r3d-nxdk-opt.txt`, which the link
check prints on every link. On top of the faster string functions, in
xemu (emulator timing, ratios only): CPU per frame on the busy c1a0e scene
7.55 to 6.83 ms, the c1a0e load 2.86 to 2.06 s, and the XBE 516 KiB
smaller. `deps/nxdk` is shared by every worktree, so rebuilding it
changes what every branch links.

### Changed - fast memcpy, memmove, memset, memcmp and strlen on Xbox

nxdk's C library ships these as unoptimised byte loops, and every memory
copy in the game went through them - the renderer's 72-byte copy per
triangle, struct copies, buffer copies. The Xbox build now links its own
(`compat/xbox/xbox_string_fast.c`): dword moves for small blocks and the
CPU's string instructions for large ones. Small fixed-size copies in C code
are also inlined again instead of being calls. In xemu (emulator timing:
the ratios are the result, not the milliseconds) a 72-byte copy is about
10 times faster and a 64 KiB copy about 17 times, and the CPU time per
frame on a busy scene (c1a0e, firing) dropped from 12.3 to 7.6 ms with
the same rendered work. A new console command, `r3d_membench`, checks the
functions on the console and times them; its output also goes to
`membench.txt`. Divergence #142.

### Changed - frame-time recordings separate level loads and list stalls

The frame that runs a `changelevel` still starts in play, but the engine
loads the next map before the following frame, so that frame's time is the
load: on the console it showed up as "one 400-1000 ms spike on every map"
and counted against the map being left. The recorder now flags it (`LOAD`,
a previously reserved frame-flag bit, so files stay readable by older and
newer tools alike), keeps it out of the overlay's map figures, and starts
the map's recorded load time at it instead of one frame later.
`tools/frametime-report.py` leaves these frames out of each map's
statistics (`--include-level-change` keeps them), lists them in their own
table, finds them by their shape in recordings made before the flag, and
adds a "stalls" table: every in-play frame still over 250 ms with its CPU
share. On the owner's hardware route recording that is 15 level-change
frames, and 7 stalls of which 6 are 616-752 ms of CPU mid-map that line
up with the maps' autosaves. Divergence #141.

### Added - frame-time recorder and overlay on Xbox (`host_frametime_analysis`)

One cvar, `host_frametime_analysis`, selects the frame analysis: `0` off
(the default, and it costs one compare per frame), `1` record, `2` record and
draw an overlay. Recording times every frame with the CPU's time-stamp
counter and splits it into the engine's CPU work, the time the CPU waited
for the GPU, the vblank/swap waits and the time between frames; it also
records each map change (name, load time, free memory). Frames go into an
in-memory ring written to `frametime/ft_NNN.bin` in the engine's write
directory every 5 seconds, at each map start and on quit. The overlay draws
a stacked CPU/GPU graph of the last 120 frames with 60 and 30 fps lines, and
the current, map-average and 1% low figures, over the HUD.
`tools/frametime-report.py` fetches the files from the console over FTP (or
from xemu's disk image, `tools/xemu_hdd.py`) and writes an HTML report: per
map average/min/max/1% low fps, worst frame, CPU and GPU share, frames over
16.7 and 33.3 ms, load times and free memory, and timelines. The GPU figure
is how long the GPU held the CPU up, not the GPU's busy time: pbkit exposes
no GPU timestamp. How-to and file format: `docs/r3d/frametime-recorder.md`.
Replaces the temporary probe used for the item 12 hardware run; divergence
#141.

Format version 2 adds 20 per-frame work counters from ref/nv2a - draws,
BEGIN/END blocks, pushbuffer dwords, texture binds, vertices (world,
studio, TriAPI, 2D, decals), visible world faces, PVS leaves, studio
models and triangles, lightmap faces and texels rebuilt, water vertices,
decals, particles, beams, sprites. They are counts, so a run in xemu
measures them exactly as the console would. The overlay shows them under a
colour legend for the graph and costs less: 0.87 ms instead of 1.08 ms per
frame in xemu on the same view, with the counters added (the graph draws 40
columns of three frames, the slowest of each, and the text is formatted 4
times a second). The report shows the counters per map (mean / peak), their
correlation with CPU time and GPU wait, the GPU-bound frames against the
rest and the ten frames with the most GPU wait; it reads version-1 files
too. The frames that wrote the file are flagged and left out of the
statistics unless `--include-flush` is given.

### Fixed - decals no longer vanish right after loading a save

Loading a savegame restored its decals and then the engine wiped them all
within the first frames. With the retail `logos/remapped.bmp` present, the
player has a spray-logo customization; `CL_UpdateLogo` and a userinfo update
clear it right after the load and call `R_DecalRemoveAll( nUserData1 )`,
but `nUserData1` is 0 until the logo is actually uploaded, and 0 means
"remove every decal" (`gl_decals.c:1157`). Both call sites
(`engine/common/custom.c`, `engine/client/parse/cl_parse.c`) now skip the
call when no logo texture exists. Same code upstream; divergence #140.

### Added - decals, screenshots and savegame thumbnails on Xbox (ref/nv2a)

Bullet holes, blood, scorch marks and sprayed logos now stay on walls, floors
and doors, as in the reference renderer (`gl_decals.c`): a decal is clipped
to every face it touches, lit by that face's lightmap and drawn over it with
a small depth bias (`gl_polyoffset`). Until now ref/nv2a threw every decal
away.
- Decals on doors, trains and other brush entities are kept in the entity's
  own space and move with it; translucent entities draw theirs with the
  entity's colour and transparency.
- Decals are saved with the game and placed again on load
  (`R_CreateDecalList`). Engine note, unchanged here and the same upstream:
  when the player has a logo (the retail `logos/remapped.bmp`) the engine
  removes every decal just after a load, through `R_DecalRemoveAll( 0 )`
  for a logo that was never sprayed.
- The `screenshot`, `snapshot` and `levelshot` commands now write their
  files, and every save writes its thumbnail (`save/<name>.bmp`), which the
  Load Game menu shows. The picture is read back from the NV2A's back
  buffer after the HUD is drawn, as ref/gl's `glReadPixels`.
- Memory: 128 decals (the Xbox's low-memory limit): a 7.5 KiB pool plus each
  decal's clipped polygon in the renderer's zone (128 bytes for a typical
  4-vertex decal, about 16 KiB for a full pool), and a 56 KiB vertex buffer
  for drawing. Saving takes 300 KiB of zone for a moment for the thumbnail.
  The XBE grows by 16 KiB.
- Frame cost: 65-83 decals on screen after a firefight in c1a0e (260-338
  vertices), estimated well under 0.1 ms of CPU at 733 MHz. Not yet
  measured on the console.
- No map needs a re-bake and the disc needs nothing new.
- New test: the `nv2a_decal_util` host test (the decal clip and its
  lightmap coordinates).
- Divergence #139.

### Added - moving water and underwater fog on Xbox (ref/nv2a)

Water (`!` textures) now moves as in the reference renderer: each water
surface is cut into 64-unit pieces when the map loads, and every frame its
texture swirls with the sin-table warp and, where a func_water sets a wave
height, the surface rises and falls (`GL_SubdivideSurface`,
`EmitWaterPolys`). Until now water was a still, baked surface.
- Translucent func_water keeps its render mode and can be seen from below;
  translucent world water (`sv_wateralpha` below 1) draws after models.
- With the view under water the scene fogs with the water's colour and
  density (`R_CheckFog`/`R_DrawFog`), taken from the water texture's
  palette in the game's WADs; the client's `TriFog` works too. Additive
  effects stay unfogged, as in the reference.
- Memory: the water pieces cost at most 0.62 MiB (disposal) in the map's
  own pool, 0.1 MiB or less on 40 of the 52 retail maps with water.
- Frame cost: up to ~12500 warped vertices a frame across the biggest
  lake (c4a3), estimated at 2-3.5 ms of CPU at 733 MHz; a typical pool is a
  few hundred microseconds. Not yet measured on the console.
- No map needs a re-bake and the disc needs nothing new.
- New test: the `nv2a_warp_util` host test (subdivision, the warp and its
  per-frame form, the fog register math).
- Divergence #138; #119's "no fog" is closed.

### Added - skies on Xbox (ref/nv2a); optional DXT1 sky bake for the disc

Sky surfaces now show the map's skybox (`gfx/env/<skyname>`), drawn around
the view as in the reference renderer: each visible sky face is clipped onto
the six sides of a box around the eye and only the covered part of each side
is drawn (`R_AddSkyBoxSurface`/`R_DrawSkyBox`). Until now sky openings were
flat black.
- Everything in front of the sky covers it: world, doors, models, effects.
  ref/nv2a draws the box before the world with depth off, where ref/gl
  draws it after the world at half its far clip. ref/nv2a's projection has a
  fixed far plane of 8192, which the reference's box does not fit on
  MaxRange 8000-12000 maps (c2a5, Xen) (divergence #137).
- Fixes a leak: the engine loaded the six sides on every map and ref/nv2a
  never freed them, so each change to a different sky stranded the previous
  set (1.5 MiB at RGBA32; measured in xemu, c4a1 then c2a5).
- `tools/asset-studio --bake-sky-dxt1 --game-dir <valve> --output <dir>`
  writes every retail sky as DXT1 `.dds` (19 sets, 3.6 MiB on disc), and
  `tools/xbox-xiso.sh --sky-bake <dir>` puts them next to the retail TGAs.
  The engine picks the `.dds` first, so a sky costs 192 KiB of GPU memory
  instead of 1.5 MiB. Quality: mean PSNR 37.9-53.2 dB per set against the
  TGAs (40.1 dB measured on screen). A disc without them still shows the
  TGA skies. No map archive needs a re-bake.
- New tests: the `nv2a_sky_util` host test and the `sky_art_bake_test`
  ctest.

### Added - animated textures on Xbox (ref/nv2a); maps need a re-bake

World and brush-entity textures now animate as in the reference renderer
(`R_TextureAnimation`):
- `+0..+9` textures cycle over time: computer screens, warning signs,
  blinking panels.
- The `+a..+j` alternate set is shown while the entity's frame is non-zero.
  This covers buttons that change look when pressed, health and HEV
  chargers, and lights that a trigger switches.
- `-0..-9` textures pick a random tile per face, as upstream does, instead
  of every face showing tile 0.

Until now the baker dropped every texture no face draws (X9-U's texture
prune). That removed exactly these frames: they are reached only through
the chains the engine builds by name. The baker now appends them to the map's
texture directory, after the drawn textures and in miptex order, so the
format is unchanged. c1a0 keeps 27 frames (+76 KiB). Across the retail
maps, 117 of 125 gain frames: about 100 KiB on average, at most 572 KiB
(rocket_frenzy).
ref/nv2a finds the frames from the engine's own texture list with the same
rule. An archive baked before this change still loads and looks as it did,
and the console says it needs a re-bake. New tests: the
`anim_texture_frames_test` ctest and the `nv2a_texanim_util` host test.

### Changed - the Xbox build is optimised (-O2); it was -O0

No Xbox compile passed an optimisation flag, so everything - engine, ref/nv2a,
the menu, the game code - was built unoptimised. Every Xbox frame time measured
before this, including the real-hardware 44 fps route, was of an -O0 build.
The build now uses -O2 through one setting, `XBOX_OPT` (`tools/xbox-opt.sh`),
recorded in the sweep manifests so a link cannot mix levels. The game code
gets the SDK's own `-fno-strict-aliasing`.
Measured in xemu against the -O0 build: the XBE shrinks from 6.9 MB to
5.5 MB, the resident image by 1.3 MiB, and the lightmap rebuild costs 0.55x
the CPU cycles. A scene that ran at 25-28 fps in xemu holds its 30 fps cap.
The optimiser exposed two latent bugs, both fixed:
- Menu text: the space between words was measured from an uninitialised
  glyph box, so its width depended on the build (divergence #135, an
  upstream bug).
- `printf("%f")` of very large numbers converted an out-of-range double to
  int; the host tests now build at -O2 with undefined-behaviour traps.
nxdk's own prebuilt libraries (pdclib, pbkit, SDL) are still -O0.

### Added - animated and switchable lights, and dynamic lights on walls, on Xbox (ref/nv2a)

World and brush-entity lighting now changes at run time, as in the
reference renderer. Flickering, pulsing and strobing lights animate. Lights
that a map or a trigger turns on or off change the walls around them. The
flashlight, explosions, rockets and server light effects now light nearby
walls and floors as well as models.
Before this the world showed only the lightmap baked for the first light
style of each face. Lights that are on when a map starts but sit on another
style slot were missing, such as c1a0e's floor spotlights in the test
chamber.
It is a port of the reference's lightmap rebuild (`R_BuildLightMap`,
`R_AddDynamicLights`, `R_CheckLightMap`), writing the changed faces into
the existing lightmap atlas before each frame's world draw, and only for
faces on screen.
The work per frame is bounded to 16384 lightmap texels. It costs 9 bytes
per face plus 60 KiB of fixed buffers; the XBE grows
by 12 KiB. No archive changes, so nothing needs re-baking.
Models were already lit by dynamic lights. See divergence #134.

### Added - beams, particles and tracers on Xbox (ref/nv2a)

ref/nv2a now draws the engine's client effects: every beam type (tested
with the gluon and tau guns and tripmine lasers; the resonance cascade's
lightning, trails and rings go through the same code), particles
(bullet impacts, blood, sparks) and tracers.
It is a port of the reference OpenGL renderer's beam and particle code
(`gl_beams.c`, `gl_rpart.c`), called where the reference calls it.
Before this the effects were never drawn and, because the engine frees
dead effects from that same call, their pools filled up and stayed full.
No new video memory: the effects use textures that were already loaded.
See divergence #133.

### Added - sprites on Xbox (ref/nv2a)

ref/nv2a now draws sprite entities and sprite temp-entities: `env_sprite`
and `env_glow` halos around lamps, explosions, muzzle flashes, sparks and
smoke. It is a port of the reference OpenGL renderer's sprite code
(`gl_sprite.c`) with every orientation type, frame interpolation, the
glow fade with distance and occlusion, and the lit 'blood' sprites. The
sprite frames were already loaded into video memory on every map (about
2.3 MiB on c1a0e) but never drawn, so this adds no memory. The glow shell
around a studio model now uses the real chrome sprite instead of a flat
tint. See divergence #132.

### Changed - the menu takes a third of the GPU memory; the focused item glows (Xbox)

With the main menu up, ref/nv2a's menu textures drop from 9.6 MiB to
2.8 MiB and free memory rises from 25.9 to 32.6 MiB (xemu). Images whose
texels allow it are stored at 8 bits per texel (white glyphs and the title
layers, as alpha only) or 16 (grey images), which samples exactly the
same colours; the menu background is baked to DXT1 tiles
(`asset-studio --bake-menu-art --menu-art-dxt1`) and uploaded compressed.
The focused menu button now has a soft glow behind its text, the same
heavy-blur pass the PC menu uses, so the selection is visible at a
glance on a TV. See divergence #131.

### Changed - loading screen for a map started from the main menu (Xbox)

Starting a map from the main menu (New Game, Load Game, or `map` in the
console) now shows a solid black screen with the engine's own LOADING
plaque from the moment the load begins until the level is in. Before,
the Xbox showed about seven seconds of black with only the version
string, then about seven seconds of plain grey, with no sign that
anything was loading. The grey was the full-screen console that upstream
uses as its loading picture when levelshots are off. The level-to-level
transition (changelevel) keeps its existing behaviour: the last frame
with LOADING drawn over it. See divergence #130.

### Fixed - brush entities popping in and out on Xbox (ref/nv2a)

Doors, cabinet doors, the c0a0/c1a0 tram car, grates and handrails
appeared or vanished depending on where the player stood or looked. This
was a regression from the studio-model work (a48b415e, #152). That change
moved brush entities into the entity lists, where they interleave with
studio models, but each brush entity still assumed the world pass's GPU
state. A visible studio model drawn before it had already switched the GPU
to the TriAPI program and vertex arrays, so the brush entity drew nothing.
A culled studio model left the state alone and the brush entity appeared.
Brush entities now re-enter the world's draw state whenever the TriAPI
has taken the GPU, as ref/gl's R_DrawBrushModel sets up its own state.

### Changed - no bring-up triangle when there is no world to draw (ref/nv2a)

The bring-up test triangle, which R_EndFrame drew over the in-game menu
whenever no world had been drawn, is gone along with its shader, texture
and vertex buffer. A frame with no world is now plain black with the
engine version at the bottom right, in the main menu's own text, colour
and position. It is drawn before the HUD, console and menu, so they stay
on top. See divergence #129.

### Fixed - low-memory menu asset restoration (X9-X follow-up)

Low-memory builds release menu fonts, font-file data and background
textures when a map starts, and restore them when the menu needs them.
The menu module itself stays linked into the executable. Reopening Pause
restores fonts; resuming retains them until the next map load.

The first text measurement after release now restores fonts before
measuring the ellipsis. Background preference changes are processed before
restoring released art, so a failed reload no longer prevents selecting a
different available background.

The earlier claim that both release paths deadlocked inside ref/nv2a was
not established. The subsequent investigation caught an out-of-memory
fatal error, and discovered the emulator was booting an older disc.
Release was re-enabled in mainui 1825481 without a renderer change.
That investigation recorded image-verified c1a0 and disposal loads with
19.68 and 6.55 MiB free respectively. Those are historical measurements,
not measurements of this follow-up build. See divergence #127 and
docs/r3d/reports/menu-loading-fix.md for current verification.

### Added - a reduced agentic loop, with cached builds and one-boot live looks

`.claude/workflows/fast-loop.js` (`docs/r3d/fast-loop.md`) does the
agentic loop's job with about half the agents: 5-6 for a typical green
topic, against v2's 8-10 and a maximum of 23. It also spends less time
on the two things that dominated the real runs, large builds and xemu.
v2 stays until the new loop has a measured run.

- One thinking agent per round: research and implementation in one call.
  Every other stage is a Haiku `loop-runner` that runs one generated
  script and returns its files verbatim. The script computes the verdict,
  and the publish script re-checks it on disk before pushing.
- `tools/fast-gates.sh` computes the gate plan from the branch's own
  `git diff` and runs it through `tools/gate-chain.sh`, in two tiers:
  - cheap: self-tests, host and python tests, a compile of each changed
    engine file, and the ref isolate;
  - full: the engine sweep and the hl link.
- `tools/xbox-cc-cache.sh`: an opt-in, content-addressed object cache in
  front of `tools/xbox-cc.sh` and `tools/xbox-cxx.sh`. Every sweep still
  runs the driver on every file; a compile of an input already compiled
  becomes a copy. The key is the preprocessed unit, the arguments and the
  compiler. Repository paths are mapped out of the key and out of
  `__FILE__`, so a worktree reuses the main checkout's objects. Off
  unless `.sweep/cc-cache-dir` or `XBOX_CC_CACHE_DIR` is set.
- `tools/live-look.sh`: one command for a live look.
  - It builds the overlay, packs the disc and boots xemu.
  - Before capturing, it waits for identity and for the scene.
  - It diffs against origin/main's frame of the same scene, which is
    cached, so a look costs one boot instead of two.
- origin/main's baseline build runs while the implementer works, not
  after it.
- Fixed: the v2 loop's `python-tests` gate command could never pass
  (`discover -t .` on a directory that is not a package).


### Changed - Xbox studio model skins upload on first use, at 16 bpp, under a 2 MiB cache (X9-X step 5)

Studio models in ref/nv2a first uploaded every skin of every precached
model at load, at 32 bits per texel, as `ref/gl` does. Half-Life
precaches every weapon's view, player and world models on every map, so
that measured (xemu, `tools/xemu-debug.py memstats`, free memory after
the map loads, main vs that build): c1a0 21.2 -> 6.7 MiB (14.5 MiB of
skins), c1a4 19.1 -> 5.0, c2a5 17.2 -> 1.6, and c4a3 (13.1 on main) and
disposal (1.7 on main) failed to load. The bake-time fit check approved
them all: its precache term was measured before skins loaded at all.

- A skin is now uploaded the first time it is bound, re-read from the
  model file (the engine does not keep the 8-bit data in memory on the
  Xbox) and expanded through its palette with the same gamma and
  transparency rules as the engine's loader, so it looks as before. The
  expansion needs no engine memory, so a late upload on a full map can
  only fall back to untextured, never stop the game.
- It is uploaded at 16 bits per texel: `R5G6B5`, or `A1R5G5B5` for
  masked skins. HUD, menus and sprites stay 32-bit.
- Skins live in a cache of at most 2 MiB, taken in 512 KiB pieces only
  as skins are actually drawn, and handed back before each map loads so
  the level itself gets the memory first. When it is full, the least
  recently used skin not drawn in the last two frames makes room. A
  skin that is not in yet draws white for a frame or two, and at most
  two skins are uploaded per frame, so a scene change does not hitch.
  `texturelist` reports the cache's use and evictions.
- The bake-time fit check (`Nv2aMapFit.h`) charges the 2 MiB, so a map
  is judged against the real bound.
- The link script's identity sidecar now records whether the menu
  module was linked (`mainui=`): the "main" build behind the numbers
  above had no menu, the branch build did, and the menu's own fonts and
  background (4.6 MiB) plus its code and heap were being counted as the
  cost of studio models. The eager-upload numbers above are likely
  inflated by the same ~5.8 MiB.

Free memory after the map loads (xemu), main and this change both
without the menu: c1a0 21.2 -> 20.3 MiB, c1a4 19.5 -> 19.1, c2a5 17.5 ->
16.6, c4a3 13.2 -> 12.3, disposal 8.1 -> 7.2 - all five load. With the
menu linked: 14.5, 13.3, 10.6, 5.9 and 1.3 MiB, all five load; the
menu's resident fonts and background are what make disposal tight.

### Fixed - the Xbox link scripts work under a SCRATCH other than `.sweep`, and link straight after a sweep

The agentic loop's first run after #150 stopped at setup: its baseline
link (a sweep and a link under a `SCRATCH` in the main checkout, keyed by
the `origin/main` commit) failed before any research. Three causes, each
of which also hits a by-hand link:

- `tools/xbox-game-link-check.sh` puts `/c/msys64/usr/bin` first on PATH,
  and that bash does not forward exported variables (`docs/r3d/lessons.md`).
  The caller's `SCRATCH` never reached `tools/xbox-link-check.sh`, the game
  isolates or the freevgui sweep, which looked under the checkout's own
  `.sweep` instead. They now get it through `env`.
- `tools/xbox-link-check.sh` read the `generated_lib_*_exports.c` sources
  from `$SCRATCH/gen`, but `tools/xbox-cc.sh` always writes them to
  `$REPO/.sweep/gen`. They are now read from there; the objects built from
  them stay under `$SCRATCH/gen`.
- `--client-main` linked the sweep's `launcher.c` object, which the sweep
  compiles without `XBOX_CLIENT_MAIN`, so `_main` was undefined after any
  plain sweep (every fresh worktree) unless someone recompiled it by hand.
  The script now compiles the client-main launcher itself, into its own
  directory, and links that one.

Verified: the loop's baseline build (sweep, then
`xbox-game-link-check.sh hl --include-client --include-freevgui`) under
`SCRATCH=.sweep/agentic-loop-baseline/<commit>` links with 0 undefined
and writes a valid XBE plus its `.identity` sidecar.


### Changed - the agentic loop computes its own build gate, spends review rounds only on code that builds, and looks at the screen

Two real runs of the repo's agentic loop (PRs #148 and #149) showed the
same holes: its QA stage could report "green" on code whose build gates
had never run; the fresh worktree it creates per topic had nxdk's
sources but none of its built libraries, so those gates could not run at
all; and the one defect two review rounds missed was visible only on
screen. An eight-lens adversarial audit of the loop against those runs
drove this rewrite (`docs/r3d/agentic-loop.md`, "What two real runs
taught").

- The workflow derives the gates from the files a change touched, runs
  them through `tools/gate-chain.sh` (a runner that records every exit
  code and cannot report an empty pass; it has its own failure-path
  self-test) and computes the verdict itself from the table, by name;
  the runner's first row checks the plan's file list against the real
  change set, it attests an all-green run with digests of the plan, the
  table and the commit, and the publisher recomputes those digests and
  the XBE's hash against its sidecar before opening a normal PR. A model
  no longer decides, and build infrastructure (`tools/xbox-*.sh`, `patches/`, the
  waf scripts) counts as Xbox code for the plan.
- Every worktree carries the main checkout's nxdk path in
  `.sweep/nxdk-dir`; every `tools/xbox-*.sh` site that resolves nxdk
  (the compiler wrappers, the three link checks, `nxdk-link`, `cxbe`,
  `extract-xiso`) honours `NXDK_DIR`, then that file, then its own
  `deps/nxdk`. Setup proves the environment with a fixed plan - the
  libraries, `cxbe`, one compile whose include search list must show
  nxdk was reached, and with a live configuration the baseline build of
  `origin/main` - before any research is paid. Gate commands are bare
  invocations: `lessons.md` records the MSYS2 bash prefix dropping
  exported variables.
- Codex review runs only on a diff that passes the gates, against the
  fork point's SHA rather than a stale local `main`, and a round counts
  only with its thread id and a conclusive exit code; two inconclusive
  runs block the PR. Round two runs only if the diff changed since round
  one. Every finding is fixed, listed as justified, or blocks the PR as
  unaddressed.
- A live stage boots two discs in xemu, baseline and branch, on a real
  map with the renderer selected (`tools/xbox-xiso.sh --overlay`), waits
  for a defined frame (`tools/xemu-debug.py wait-scene`), diffs them
  (`ppmdiff`) and reads the screenshots against the spec's own expected
  list; it runs after the first green table and again whenever the XBE
  changes, so a screen-found fix is still reviewed. A renderer change
  without a live configuration is refused at setup.
- A prompt naming one fork-plan step is one topic; a design note on disk
  means no specialist consultation, a topic without one gets its domain
  specialist and research writes the note the next run will find.
- Each run writes a tracked record under `docs/r3d/reports/agentic-loop/`
  whose measured sections are rendered by the script; the two earlier
  runs are recorded there by hand. `node tools/test_agentic_loop.mjs`
  checks the script's parse and its decision functions;
  `tools/host-tests/run.sh` runs every host test with its documented
  command.

### Added - the Xbox renderer's TriAPI now draws (X9-X step 4)

The client DLL's triangle drawing API (`pTriAPI`) always existed as a
table of function pointers, but five of them - `TexCoord2f`, `Fog`,
`ScreenToWorld`, `GetMatrix` and `FogParams` - were never filled in, and
the engine never called the two hooks (`pfnDrawNormalTriangles`/
`pfnDrawTransparentTriangles`) that let the client DLL draw triangles at
all. Both gaps were silent: nothing crashed, the client's own particle
code just never ran, and neither did the debug tools (`r_showhull`,
`net_graph`'s fills, `r_showtree`) that already called `Begin`/`Vertex3f`/
`End` and simply drew nothing back.

The renderer now fills all five slots, calls both hooks once per frame,
and draws every primitive shape the API supports (points, lines,
triangles, quads, fans, strips, quad-strips, polygons), in either 2D or
3D space, sharing the same batching pass 2D drawing already uses. 3D
TriAPI draws share the world's own camera and depth buffer, so they are
correctly occluded by and cull against world geometry.

Known gaps, left for later: no fog (the client's fog calls are no-ops),
translucent entities between the two triangle hooks, and the texture-wrap
clamp linear textures already had for 2D.

Verified live in xemu (2026-09-26, map `crossfire`): a real breakpoint on
`GetMatrix` hit every frame through particleman's own frustum-culling call
and read back a genuine zNear=1/zFar=8192 projection matrix, matching what
the same draw path actually renders and clips with; `r_showhull 1` showed
correctly depth-occluded, back-face-culled hull polygons; and a
pixel-diff against a pre-change baseline build, with no debug cvars, came
back 0 differing pixels - the change is fully invisible until something
exercises it. `net_graph 1` could not be exercised (a pre-existing,
unrelated gap: this build's menu module is an empty stub, so the engine's
own `host.allow_console` gate for it never opens). Real hardware
verification is still owner-side, post-PR.

### Added - the Xbox renderer can now draw textures and 2D: console, HUD, crosshair, fades and menus (X9-X)

The Xbox renderer could draw the 3D world but nothing else - every texture
call returned nothing, and every 2D drawing call was empty, so the console,
HUD, crosshair, loading plaque and menus were all invisible. It now has a
real texture manager (upload, update, free, all the way up from a raw file
or buffer to GPU-resident memory) and a real 2D batching pass, so those all
draw.

World (`.mip`) textures still never reach the GPU: this renderer draws a
map's own baked texture directory instead, so an Xbox map's worldspawn `wad`
key is now dropped at bake time, matching what the renderer already does -
the engine no longer decodes a texture from a WAD file only for the
renderer to throw it away. Every archive needs rebaking.

Known gaps, left for a later pass: no mipmaps, no compressed (DXT) 2D
textures, no 16-bit formats, sprites and studio-model skins upload but do
not draw yet, and the 2D viewport's pixel-centre offset is unverified on
real hardware.

### Changed - the Xbox renderer draws the world where the engine asks for it (X9-X)

The Xbox renderer used to draw everything at the very end of each frame,
after the engine had already issued the HUD, console and menu, so there was
no way to draw those on top of the world. It now opens the frame when the
engine starts it, draws the world when the engine renders the view, and only
presents at the end, the same order the PC renderers use. This is the first
step towards the HUD, console and menus on the Xbox renderer. The debug text
over the game is gone. Nothing else looks different.

### Added - an Xbox can host multiplayer on the large maps: the PHS is baked offline (X9-W)

A game hosting multiplayer builds each map's PHS (which parts of the map can
hear each other) when the map loads. That needs up to 10 MiB of scratch memory
on the big deathmatch maps, more than the Xbox has to spare. The baker now
builds the PHS itself and stores it in the map archive (MPAK v13), and the
Xbox engine loads it from there instead. It is proven identical to the
engine's own on every retail map, byte for byte.

- In xemu, 2-player listen servers load `crossfire`, `pool_party` and
  `disposal`, with 6.5 to 21.1 MiB left.
- The baker now reports how many players the Xbox could host on each map:
  3 on `pool_party` and `disposal`, 4 on `contamination`, 8 to 12 on the
  others.
- Known issue: changing level while hosting stalls on the Xbox (the local
  client never rejoins). It happens with or without the baked PHS and is
  tracked in roadmap X9 item 13.
- If an archive has no PHS, the engine builds it as before.
- Every map needs rebaking; v12 archives are refused.

### Fixed — the Xbox game no longer hangs at boot with every map baked (X9-V)

With all 124 baked maps in the game folder, the Xbox build froze on the
boot logo. Each map archive keeps its file open for the whole session, and
the Xbox file layer allowed only 64 open files, so the archives used them
all up before the game could read its config. The limit is now 256 (about
2 KiB of memory), enough for every retail map plus the files the game opens
while it plays.

- Level transitions measured in xemu: a deathmatch `map` cycle, and a
  19-minute session played by hand from Black Mesa Inbound to Unforeseen
  Consequences, show no memory fragmentation and no drift between loads
  (roadmap X9 item 12). The game held 60 fps on every map but `c1a0b`
  (54 fps average). The session report, with the frame-time graph, is
  `docs/r3d/reports/x9-item12-blackmesa-frametimes.html`. On a real Xbox
  the same route averages 44 fps, with four maps near 30
  (`docs/r3d/reports/x9-item12-xbox-frametimes.html`).

### Changed — more memory for every Xbox map; the baker refuses maps that cannot load (X9-U)

`rocket_frenzy` was the one retail map that did not load on the Xbox. The
work to make it fit freed memory for every map - in xemu, about 36.5 MiB is
now free before a map loads, up from 32.8 - but `rocket_frenzy` still does
not fit: it needs 988 entity slots where the game allows 900 (it fails the
same way on PC Xash), and it would still be short of memory for its GPU
buffers. So the baker now checks every map before baking it and refuses the
ones that cannot load, saying why, instead of shipping them.

- The engine computes face bevels on demand (6 MB on `rocket_frenzy`),
  and its per-surface and per-entity structures are smaller on the Xbox.
- The software renderer's 3.3 MiB of tables are only allocated when that
  renderer is used.
- The Xbox renderer builds its tables when the map starts, uses a smaller
  per-face table, and falls back to its test view instead of ending the
  game when memory runs out.
- Xbox lightmaps are packed like the PC renderer's, about half the size,
  with no visible change; archives carry only the textures a face draws.
- Over the 125 retail maps the check refuses `rocket_frenzy` only.

### Changed — MPAK v12: large maps load on the Xbox; the engine no longer holds the renderer's data (X9-T)

The engine loads a map by reading the whole file, and on the largest maps
more than half of an Xbox `.mpak` is data only the Xbox renderer uses
(vertices, lightmap, textures). Measured in xemu, about 32.8 MiB is free
before a map loads, so `pool_party`, `contamination` and `disposal` ran
out of memory. Now:

- The archive puts the classic BSP lumps first. The engine is given only
  that part as `maps/<name>.bsp`; the renderer reads its own data from the
  whole archive, `maps/<name>.mpak`.
- The renderer builds its GPU buffers when the map starts
  (`R_NewMap`), after the engine has let go of its copy, and retries once
  if memory is short.
- The lightmap is one page up to 4096 rows tall instead of a second page
  the renderer cannot use.
- On the Xbox the engine's per-surface extra data shrinks from 324 to 148
  bytes, and Xbox map archives no longer carry 8-bit texture pixels
  nothing on the Xbox draws.

Measured in xemu: `disposal` now loads with 2.7 MiB to spare, `pool_party`
loads, `crossfire` is unchanged, and 17 maps booted in a partial xemu corpus
run, all drawing their real geometry. A load-memory model built from each
archive's lump counts reproduces those measurements (disposal's engine
structures plus prefix to within 0.3 MiB). Run over all 125 retail maps, it
predicts that every map but `rocket_frenzy` loads, with at least 2.7 MiB
free (`disposal`, then `contamination` 4.2, `pool_party` 6.0, and 11.9 or
more for the rest). `rocket_frenzy` bakes (78% of the ledger) but still
does not load: the engine runs out of memory building its world model
(34.4 MiB needed of 32.8), and past that it would be about 20 MiB short for
its GPU buffers. Roadmap X9 item 11. MPAK moves to v12, so every map needs a rebake.

### Changed — MPAK v11: vertex chunks lift the 65,536-vertex bake limit (X9-S)

Four retail maps (`contamination`, `disposal`, `pool_party`,
`rocket_frenzy`) aborted the Xbox bake because they need more than 65,536
vertices, and indices are 16-bit. The baker now splits vertices into
chunks of at most 65,536. Each face stays inside one chunk, and `ref/nv2a`
points its vertex arrays at the right chunk before drawing it. Maps that
fit in one chunk are unchanged apart from the version number, and
crossfire renders identically. MPAK moves to v11, so every map needs a
rebake.

The bake-time budget now counts the per-face table at its real 52 bytes a
face (it assumed 32). All four maps now get past the vertex limit, but none
draws yet:
`pool_party` bakes but runs out of memory loading its lightmap in xemu, and
the other three are over the 24 MiB budget. That makes load-time memory
the next task (roadmap X9 item 10).

### Fixed — every retail map fits the Xbox lightmap atlas and memory budget (X9-R)

11 retail maps needed a second 1024x1024 lightmap page. The Xbox renderer
cannot use a second page, so those maps did not draw on the Xbox at all,
and 7 of them also failed the 24 MiB bake-time budget. The baker packed
lightmap blocks in face order. It now packs them tallest-first and trims
the atlas to the rows it actually uses. Every retail map that bakes now
fits on one page and within budget: `boot_camp` went from 110.2% to 64.2%,
and the median from 58.2% to 34.1%. The lighting looks the same: in xemu
at the crossfire spawn, 45 pixels differ from the previous build, by at
most 1/255. `boot_camp` now draws lit in xemu.

### Changed — the v10 texture-directory rule is now one shared header, checked everywhere (X9 follow-up)

The MPAK v10 texture-directory rule (each texture's DXT1 mip-chain length,
128-byte padding) used to exist only inside `ref/nv2a`'s own map loader, as
a private copy `ref/nv2a` had already proven correct by other means (real-
hardware verification, #105) - the PC tests and the baker's own verify pass
never checked it at all. It is now `filesystem/mpak_texture_dir.h`, a
header-only rule `ref/nv2a`, `filesystem/tests/mpak_layout.c` and
`tools/asset-studio`'s `PakManager::verifyFile` all include and run
identically - header-only because `ref/` cannot link `filesystem/
mpak_layout.c`'s real functions (they are separate, isolated static-lib
units), only include its headers for types and constants. `ref/nv2a` keeps
its prior accept/reject set exactly (it does not check padding bytes'
CONTENT, since NV2A never samples them); the PC tests and the baker's
verify pass do check padding content, closing the gap. `buildDxt1MipChain`
(the baker's own mip-chain writer) moved out of an anonymous namespace with
no behaviour change so a new test, `tools/asset-studio/tests/
dxt1_mip_chain_test.cpp`, can call it directly and cross-check its output
size against the shared rule for a square, a 16:1 wide and a 16:1 tall case
(4x4, 16x16, 64x4, 4x64, 256x16).

### Fixed — `tools/verify-atlas-alpha.py` revived for MPAK v10; `menu_bg.mpak` gets its own version number (X9-Q)

`tools/verify-atlas-alpha.py` had been stale since v9 (its field list and
single shared-atlas addressing were both from the v6-v8 format) and had no
v10 counterpart at all, so the per-texture mip chains X9-N added (MPAK v10)
were never checked against the punch-through-alpha invariant
`divergences.md` #99 relies on. Rewritten against the current 43-field
`MpakHeader6` and v10's `align128`-addressed texture directory, paired with
a new `R3D_DUMP_TEXTURE_RGBA` bake-time hook (successor to the pre-v9
`R3D_DUMP_ATLAS_RGBA` hook) that dumps the exact pre-compression buffer for
every mip level. Threshold moved from source alpha `== 255` to the
encoder's own `>= 128`, since mip levels (unlike the old base-level-only
atlas) have real intermediate alphas. Off by default; byte-identical
archives with the env var unset. Re-ran the full retail `valve/maps`
corpus: 121 of 125 maps bake before the bake-time budget gate below, 114 after it (the other 4 fail for the same unrelated,
already-tracked u16 vertex-index limit) - 0 opaque-source-but-transparent-
decoded mismatches across 11,801 textures, 86,063 mip levels and
164,092,624 texels (see `divergences.md` #99's own dated follow-up).

QA round 1 found two gaps in the first pass: a dump record carried only
texture/level/dimensions, so a stale dump from an earlier or different bake
with matching texture sizes could be paired with today's archive and a real
encoder regression could read as a pass; and every mismatched texel kept its
own object even though only the first 20 are ever printed, risking an
out-of-memory failure on a widely-broken encoder over a corpus this size
instead of the promised summary and exit code. Fixed: dump format bumped to
v2, each record now carries a CRC-32 (ISO 3309/IEEE 802.3, the same
algorithm `zlib.crc32` implements) of the exact compressed bytes it was
measured against, which the script recomputes from the archive and requires
to match before decoding; and the script now tracks an exact mismatch
counter, retaining only the first 20 `Mismatch` objects for the detail
report. Re-verified byte-identity and the punch-through-alpha result on a
real `c1a0` bake with the new dump format, and added a real-data negative
control (a hand-corrupted compressed byte against the original dump now
correctly fails closed with a digest mismatch, exit 2) plus two new unit
tests (a cross-archive digest mismatch, and a mismatch count of 2,048 texels
staying exact while only 20 objects are retained).

QA round 2 found one more gap and one doc inconsistency. `_parse_dump`
checked only the 4-byte magic before unpacking the 4-byte version field, so
a dump truncated to 4-7 bytes raised an uncaught `struct.error` - Python's
default exit code 1, which this tool reserves for "a real mismatch was
found" - instead of the documented exit-2 structural-error path. Fixed with
an explicit length check ahead of that unpack, plus two failure-path unit
tests (a magic-only 4-byte dump, and a dump with a partial version field);
confirmed both would have raised the uncaught `struct.error` before this
fix by running the unguarded code path directly. Separately, `fork-plan.md`
had drifted to claim "0 mismatches across the full 125-map retail corpus"
for this same slice, while this entry and `divergences.md` #99 both
correctly say 121 of 125 (4 maps fail to bake for the pre-existing,
unrelated u16 vertex-index limit: `contamination`, `disposal`,
`pool_party`, `rocket_frenzy`); corrected to match.

`UiPacker::packMenuBackground` (`menu_bg.mpak`) built its header without
ever setting `version`, so it silently inherited the BSP map format's
`version` field - deployed as 6, then drifting to 10 with no decision
behind either number, even though this archive isn't map-shaped at all (no
`dheader_t`, descriptor at offset 0). It now writes its own
`MPAK_UI_BACKGROUND_VERSION` (1), independent of the map format version.
No reader change: the archive kind is still identified by descriptor
position, not by this number, and nothing in the engine parses an offset-0
archive today.
### Added — bake-time mirror of `ref/nv2a`'s 24 MiB per-map budget (X9-Q)

`nv2a_load_map_geometry` only checked its own per-map memory ledger at
load time, on the console: an over-budget map baked cleanly, passed
`verifyFile`, and silently fell back to drawing the bring-up triangle
instead of its real geometry. `tools/asset-studio` now computes the same
ledger (`Nv2aMapBudget.h`) right after writing and verifying an Xbox nv2a
archive, and fails the bake — deleting the archive it just wrote — if the
map would not fit. Measured against the full 125-map retail corpus: 7
maps genuinely exceed the cap (worst, `boot_camp`, at 110.2%), 4 hit an
unrelated, already-tracked vertex-index limit first, and the median of
the remaining 121 is 58.2% of budget. Full table in
`docs/r3d/fork-plan.md`'s X9 section.

### Changed — `ref/nv2a` bring-up triangle state tidy-up (X9-P)

The test triangle shown before a map loads now writes a complete texture
coordinate (q = 1), as the map shader has since X9-L, and switches off the
second texture stage it does not use. Its shader source now lives in
`ref/nv2a/vs.vs.cg` instead of only as a compiled copy from the X4 tool.
No visible change: the checkerboard and the map view render as before.

### Fixed — `nv2a_r_nocull` drew trigger volumes and frozen doors (X9-O)

The `ref/nv2a` debug setting `nv2a_r_nocull 1` fell back to drawing every
baked face at once, which skipped the sky, the world/brush-entity split and
visibility culling. The view filled with orange trigger volumes, and doors
were drawn at their closed positions. It now only turns off frustum and
back-face culling, like ref/gl's `r_nocull`. At the crossfire spawn in xemu,
the frame with culling off is pixel-identical to the frame with it on.

### Added — anisotropic filtering and LOD bias for `ref/nv2a` (X9-N2)

With mipmaps on, a real Original Xbox showed a visible resolution change a
few units ahead on floors. Without anisotropic filtering, a surface seen at
a glancing angle switches to a small mip level almost at once; ref/gl
avoids that with `gl_anisotropy` 8 by default. Two new settings:
- `nv2a_r_anisotropy` (1, 2, 4 or 8, default 2): 2x is the level Perfect
  Dark X runs on real hardware; 4 and 8 can be tried from a config file.
- `nv2a_r_lodbias` (-3 to 3, default 0), like ref/gl's `gl_texture_lodbias`.

Checked in xemu; real-hardware check pending.

### Added — mipmaps for `ref/nv2a` textures (X9-N, MPAK v10)

Every Xbox texture had one level, so tiled textures on distant or oblique
faces (gravel, fences, walls seen at a glancing angle) aliased into
shimmering moire. The baker now stores each texture's full DXT1 mip chain,
from full size down to 1x1. Textures with see-through parts average only
their solid texels, so the transparent colour does not bleed into edges.
`ref/nv2a` samples it with trilinear filtering; the nearest-neighbour
option uses the mipmapped equivalent.

In xemu, pixel-to-pixel noise on distant surfaces at the crossfire spawn
dropped by more than half. The near walls are nearly unchanged.

MPAK moves to v10, so every map needs a rebake. Texture memory grows by
about 35% per map (crossfire: 682 KB to 918 KB), well inside the renderer's
per-map budget.

### Fixed — Xbox gamepad sticks turning the view by themselves

Original Xbox controllers rest well off centre. One measured on real
hardware read -5,888 to -7,424 on the right stick's X axis at rest, with
8-bit precision (every value a multiple of 256). The engine's 4,096 dead
zone let that through as a steady push, so the view turned left on its
own; three different controllers drifted. Xbox builds now default every
stick axis's dead zone to 8,689, XInput's own right-stick dead zone. Other
platforms keep 4,096. Pending a retest with all three controllers.

### Added — `ref/nv2a` draws brush entities (X9-M)

Doors, ladders, fences, rails, chargers, buttons and mounted guns are
brush entities: their faces belong to the map's submodels, not the world.
`ref/nv2a` had no entity pass, so it drew every baked face as static world
geometry. The visibility culling then hid those faces permanently, since
no world leaf lists them. Turning culling off drew them frozen in place,
invisible triggers included. This was the "missing caution stripes" item
on the roadmap. On `crossfire` that is 647 faces across 71 entities.

`ref/nv2a` now keeps ref/gl's per-frame entity lists (`R_AddEntity`,
`R_ClearScene`) and draws each brush entity after the world:
- at its own position and rotation;
- with off-screen and back-face culling;
- in ref/gl's render modes: normal, alpha-tested, blended and additive,
  including pulse and fade effects.

The world pass no longer draws entity faces, and it draws exact triangle
ranges, so entity and world triangles never mix. Checked in xemu and on a
real Original Xbox.

### Fixed — `ref/nv2a` textures on a real Original Xbox (X9-L)

The first run of `ref/nv2a` on a real Original Xbox halted with a GPU
"invalid data" error. Once that was fixed, it showed the same texture
corruption that X9-K had in xemu: smeared ground, misplaced wall patches,
and half a sign stretched over the whole board. The fixes:

- Every texture format now sets `BORDER_SOURCE_COLOR`. Without it the GPU
  sampled every texture at half its coordinates. This was the corruption,
  on xemu and hardware alike.
- Texture filter values the hardware accepts: `0x01012000` for nearest (a
  zero convolution-kernel field halts the GPU) and `0x02022000` for
  bilinear (the old `0x04074000` is a convolution filter that smears).
- The map vertex shader writes each texture coordinate's q component, and
  the map path restates pbkit's own `CONTROL0` value. Both are defensive;
  neither changed what the hardware drew.

The crossfire spawn now renders with readable signs and correctly tiled
concrete and asphalt, both in xemu and on real hardware.

### Changed — `ref/nv2a` diffuse textures: per-map DXT1 atlas replaced with native NV2A hardware WRAP addressing (X9-K, MPAK v9)

Every diffuse texture used to be shelf-packed into one shared 2048x2048
DXT1 atlas per map, with a per-vertex remap (`rect[0] + frac(u)*rect[2]`)
approximating texture repeats at load time. That remap is only exact when
a face's UV range fits one texture repeat - `frac()` collapses every
further repeat onto the same fractional value, so any face bigger than one
tile (most floors and walls) sampled a flat, non-repeating patch instead
of tiling. Subdividing tiled faces at bake time to fix that overflows the
u16 vertex-index limit on large outdoor maps - `crossfire`'s worst face
alone would need ~22,560 tiles.

MPAK bumps to v9: the shared atlas is replaced by one independently
power-of-two-resampled DXT1 image per unique texture, addressed with
NV2A's own native hardware WRAP mode - no atlas, no remap, tiling handled
by the GPU. Faces are now sorted by texture at bake time so the renderer
can bind one texture and issue one draw call per batch (measured on the
two staged retail maps: 129 batches / 164 textures for `c1a0`, 91 / 120
for `crossfire`, both with real headroom against the new fixed-capacity
caps this adds). Total diffuse-texture memory dropped versus the old
fixed-size atlas (~535-537 KiB measured per map versus the atlas's fixed
2 MiB, since a real map's footprint was always well under that atlas's own
worst-case size). The map's PVS-visibility system required one careful,
load-bearing fix to stay correct once faces stopped being stored in
original BSP order: a new `originalToSortedFaceIndex[]` table and one
translation step before `nv2a_mark_leaves()` writes its visibility array,
double bounds-checked and failing open (never a wrong cull) rather than
trusting it.

Live-confirmed on both real staged maps under xemu: `c1a0`'s reception
corridor textured and lit exactly as before (mean 86.3/255 versus X9-J's
90.4/255 at the same debug pose - the small delta is real texture content
shifting under POT resampling, not a regression); `crossfire`'s large
outdoor ground and fence/scaffolding textures now genuinely tile instead
of stretching into a flat patch - the map the old subdivision approach
could never have supported at all (u16 index overflow).

Two more real, pre-existing gaps surfaced only by comparing the live
render directly against `ref_soft`, after every structural/static check
had already passed clean - a reminder that index-buffer alignment and
byte-exact archives prove the new code is internally consistent, not that
the picture is right. First: `ref/nv2a` has never read
`MpakFaceInfo::flags` (`SURF_DRAWSKY`, baked since MPAK v6), so sky faces
drew as ordinary geometry with the WAD's literal "sky" placeholder
texture - invisible under the old flatten-to-one-patch remap, glaringly
obvious as a dense tiling pattern under correct WRAP. Fixed: sky faces are
now excluded from every draw run. Second: this renderer has never had a
mipmap chain (`MIPMAP_LEVELS=1`, confirmed unchanged since before this
session) - also hidden by the old flatten behaviour, now visible as
aliasing when a small texture tiles densely across a distant or oblique
face. Ruled out as a data or addressing bug first, not assumed: every one
of `crossfire`'s 120 baked textures was independently decoded straight
from the archive bytes and confirmed pixel-correct, and WRAP vs CLAMP
addressing was A/B tested live with no visible difference. A real mip
chain is real, substantial, separate work, deliberately not bundled into
this change - tracked as an explicit follow-up.

See `docs/r3d/fork-plan.md`'s X9-K entry and `divergences.md` #101 for the
full account.

### Fixed — `ref/nv2a` maps rendered near-black: two texture-setup bugs

Every real map drawn by the NV2A renderer had come out near-black and
perfectly monochrome since its first live frame (screenshot mean
~1.8/255, pixel values only {0,1,2}), and an earlier photometric check
(`divergences.md` #94) had been filed as inconclusive. Two independent
bugs in `ref/nv2a/r_context.c`, each masking the other, isolated with
per-term debug shaders and live memory reads under xemu: the lightmap
page is bound as a LINEAR texture, which NV2A addresses in unnormalized
texel units, but was fed the baker's 0..1 page coordinates, so every
fragment blended the page's texels (0,0) and (1,0) - the lightmap term
was {0,1}/255 across the whole frame; and the DXT1 diffuse atlas was
bound with the linear-texture pattern and no `BASE_SIZE_U/V`, the only
size a compressed texture has on this GPU, so it was sampled as a 1x1
texture - one flat pale green-white colour. The vertex data, the baked
luxels and the atlas bytes were all verified correct at the exact
coordinates the GPU reads before either fix was made. Fixed by scaling
the lightmap UVs by the page size at map load and by setting the atlas's
log2 dimensions in the texture FORMAT word (validated power-of-two and
<= 4096 per side at load, failing closed with a console diagnostic
otherwise); the map now renders textured and lit at sane exposure
(mean 90.4/255, real colour across the frame). Because that UV scale is
only the correct inverse of the baker's fixed 1024-luxel page divisor,
the load-time check now requires the archive's own lightmap page
dimensions to equal 1024x1024 exactly - not merely be square to each
other, which a codex review round caught as still allowing a
same-but-wrong divisor to silently mis-scale every luxel - failing
closed with a console diagnostic instead of scrambling rows silently.
The fix was booted live (this real corridor and reception-desk view of
`c1a0`, mean 90.4/255, 260,833 px with channel spread > 8) and is
pixel-identical to the already-verified diagnostic build, and `xmex`
disassembly of the final XBE reproduces the same register construction.
The bring-up test triangle's own checkerboard UVs had the same
linear-addressing mistake - it had only ever shown a smooth two-texel
gradient, misread as a checkerboard - and is corrected to texel-unit
UVs as a visual canary for that convention; under both the original
filter constant and a plain-bilinear one it still rendered as a smooth
blend rather than a crisp 8x8 checkerboard, but booted a third time
with a NEAREST filter (bring-up path only - the real map's atlas and
lightmap stages are unchanged), it renders a real, crisp 8x8
checkerboard - the blend was bilinear filtering of a single-texel-period
pattern, not a remaining UV bug, and NEAREST is now the bring-up
triangle's permanent, more sensitive regression indicator for this
convention. Register sequence otherwise unchanged; no baker, format or
shader change. Both addressing conventions are verified against
xemu 0.8.136's source and corroborated by nxdk's mesh sample,
nxdk_pgraph_tests' hardware-captured goldens and Cxbx-Reloaded's
XDK-derived headers, but not measured on a physical Xbox by this
project. See
`docs/r3d/fork-plan.md`'s X9-J entry, `divergences.md` #100 and the new
`lessons.md` entry.

### Added — native alpha test for masked textures in `ref/nv2a` (X9-I)

`ref/nv2a` now enables NV2A's own fixed-function alpha-test stage
(`NV097_SET_ALPHA_TEST_ENABLE`/`_FUNC`/`_REF`) when drawing map
geometry, cutting out masked ('{'-prefixed - fences, grates, ladders,
vents) world textures now that the baker's atlas carries real
per-pixel alpha (see the atlas alpha fix above). Matches `ref/soft`'s
unconditional per-texel masking rather than `ref/gl`'s entity+
rendermode-gated mechanism, since X9 has no entity render list yet -
a real, checked reference-fidelity decision, not an assumption. Two
real review rounds found two real bugs, both fixed: a specialist pass
found comment inaccuracies and a dangling GPU-state gap in a fallback
draw path; `codex-review` then found that the pixel shader doubled
alpha the same as color (for an unrelated lightmap-brightness
convention), which combined with the diffuse atlas's own real bilinear
filtering to silently shift the alpha-test cutoff from the intended
50% opacity to 25% - fixed at the shader source, recompiled through
the real `cgc`/`fp20compiler` toolchain, not by picking a compensating
constant. See `docs/r3d/fork-plan.md`'s X9-I entry and `divergences.md`
#99 for the full account. Statically verified clean end to end
(compile, isolate, link, real hl server+client+freevgui module); ships
disabled by default (`nv2a_r_noalphatest "1"`) pending a live-boot A/B,
matching `nv2a_r_nocullback`'s own precedent.

### Fixed — the Xbox texture atlas never carried usable alpha for masked textures

`tools/asset-studio`'s Xbox atlas compression call has always passed
`punchThroughAlpha=false`, so masked ('{'-prefixed - fences, grates,
vents) textures baked with zero real transparency, silently blocking
the renderer's own planned native-alpha-test work on baker output
nobody had traced to its real cause. The per-pixel alpha data needed
was already correct end to end, so the fix started as a one-line flip
to `true` - but texel-level verification (decoding real baked output
against the pre-compression source, not just counting blocks) caught a
real regression the first pass introduced: the atlas packer placed
textures at unaligned pixel offsets, so 10 genuinely opaque texels
(including 2 inside an unrelated non-masked texture) got compressed as
transparent by bleed across block boundaries. Fixed with two changes:
4-pixel-aligned atlas placement with border replication in
`PakManager.cpp` (eliminates cross-texture bleed entirely), and a
per-block transparency gate in `TextureOpt.cpp::compressToDXT` that
only offers punch-through encoding to a block containing at least one
real transparent texel (eliminates the "block never needed
transparency" sub-case). Six texel mismatches remained on the same test
map (0.00014% of texels checked) after those two fixes - a distinct BC1
least-squares tradeoff where a block that genuinely needs some
transparency could still absorb an adjacent near-black opaque texel
into the transparent selector. Closed with a third, self-verifying
pass: `TextureOpt.cpp::fixOpaqueTexelsPunchedThrough` decodes the block
already produced, reassigns exactly the wrongly-transparent texels to
the nearest legal opaque selector, and re-encodes with rgbcx's
`pForce_selectors` - but only ever adopts the retry after re-decoding
and confirming it actually fixed the block, falling back to the
original result otherwise, so it can only fix or no-op, never regress.
A second asset-pipeline-specialist review confirmed the fix correct by
tracing the actual rgbcx internals, but also pointed out that "0
mismatches on one map" doesn't demonstrate completeness across the
corpus and that this defect class had no standing regression
protection - so a real unit test was added
(`tools/asset-studio/tests/texture_opt_test.cpp`, verified to actually
fail without the fix on a brute-force-found violating block), the ad
hoc verification script was promoted to a reusable tool
(`tools/verify-atlas-alpha.py`), and the full 125-map retail corpus was
re-baked with every successfully-baked map's atlas decoded and compared
texel-by-texel against its source - not just checked for bake
success/failure. Result: **0 opaque-source-but-transparent-decoded
mismatches across all 121 clean-baking maps**, the same 4 pre-existing
unrelated failures as before. See `docs/r3d/fork-plan.md`'s XB section
for the full account, including why the first response to this finding
(a written justification instead of a fix) didn't hold up under a
second review round.

Also fixed by the same codex-review pass: atlas overflow (a texture
that doesn't fit the fixed 2048x2048 atlas) used to only print a
warning and hand back a zero-initialized texture location, which the
caller stored as if packing had succeeded - some faces would silently
alias onto atlas slot 0. `PakManager::save()` now refuses to write when
this happens, matching its existing failed-file-open behavior. A full
125-map retail corpus bake confirmed 2 maps (`disposal`,
`rocket_frenzy`) already overflow this atlas on unmodified `main` -
this is a pre-existing capacity limit, not one this branch introduced,
though block-alignment does make it very slightly worse; the other 123
maps are unaffected.

### Fixed — four `tools/*.sh` scripts silently no-op'd on a GNU BRE/grep-and-sed-build mismatch

`tools/xbox-ref-isolate.sh`'s own self-check (every symbol in an
isolated ref module must carry the module's prefix) has been silently,
deterministically wrong: the `grep` this script's own `PATH` export
resolves to does not support `\?` as an "optional preceding character"
the way the interactive shell's own `grep` does, so the check always
reported the symbol count as leaked regardless of whether the rename
had actually worked (it always had). This made `tools/xbox-link-
check.sh` unusable for verifying real Xbox link output for however long
it went unnoticed. Auditing the rest of `tools/*.sh` for the same
pattern, and a follow-up specialist review, found three more live
instances: `tools/xbox-gen-exports.sh`'s stdcall-decorated symbol
lookup (`\+`) had been silently failing to find every real match;
`tools/xbox-link-check.sh`'s own safety net against leftover raw
`ref/soft`/`ref/common` compile artifacts (`\(...\|...\)`) had been
silently doing nothing; `tools/xbox-dedicated-link-check.sh`'s own
`sed` manifest parser (the identical `\(...\)` capture-group issue, in
`sed` rather than `grep`) had been unconditionally failing, blocking
that script from ever producing a result. Fixed the grep cases by
switching to `grep -E` with the plain, unescaped forms, and the sed
case by dropping the second subprocess entirely in favor of bash's own
`=~`/`BASH_REMATCH`, matching the pattern `tools/xbox-link-check.sh`'s
own mainui-manifest check already used. With the fixes, the full link
chain now runs clean: `0 duplicate, 0 undefined` against the current
tree. See `docs/r3d/lessons.md` and `docs/r3d/divergences.md` #97/#98's
own resolution notes for the full account.

### Added — `ref/nv2a` now backface-culls its draw list

Frustum and PVS culling (the two entries below) both still submitted a
face pointing directly away from the camera as long as it was inside
the view volume and its leaf was potentially visible - a wall you are
standing right in front of, facing it, still had its own back side
(the side facing away from you, into the wall) submitted every frame.
The renderer now also asks, per face, which side of that face's own
plane the camera is on - the same default test the PC and software
renderers already apply to every world surface - and skips the ones
facing away. Ships **disabled by default** (`nv2a_r_nocullback` defaults
to on): this test needs to agree with the NV2A's own separate, always-on
hardware backface cull, and that agreement has not been live-verified
yet - see `docs/r3d/divergences.md` #98 for the full account, including
what live check flips the default back on.

### Added — `ref/nv2a` now PVS-culls its draw list

Frustum culling (the previous entry below) still submitted every face
the camera's field of view could geometrically contain, even the ones
behind a wall a few feet away. The renderer now also asks the engine's
own already-loaded world model which leaves are potentially visible from
the camera's current position - the same PVS data `ref/gl` and `ref/soft`
already use - and skips faces that belong to no visible leaf, on top of
the existing frustum test. No baker or format changes were needed: the
map archive is already a real BSP file the engine loads through its
completely standard path, so the visibility data was already there,
just unread by this renderer until now. A new console variable,
`nv2a_r_novis`, disables it for comparison, the same way `nv2a_r_nocull`
already does for the frustum test. See `docs/r3d/divergences.md` #97
for the full account.

### Added — a repo-native agentic-loop Workflow for driving project phases

A new Claude Code Workflow (`.claude/workflows/agentic-loop.js`) and six
new subagents (`.claude/agents/loop-*.md`) turn one general instruction
into a bounded set of atomic tasks, each researched, implemented, QA'd
(with an automatic, capped retry), and published as its own PR, followed
by a retrospective. The 8 existing specialist agents are unchanged and get
consulted from the workflow itself, not by the new agents delegating
further - no agent in either group can spawn further subagents, so total
fan-out per run is fixed by the script, not decided by a model at runtime.
QA delegates code review to the existing `tools/codex-review.sh` rather
than re-implementing it with more Claude subagents, directly answering a
real, expensive mistake this project already made and reversed once (see
`docs/r3d/agentic-loop.md`, which also has the model/effort choice for
each stage and the reasoning behind it). Not yet run on a real task.

### Added — `ref/nv2a` now frustum-culls its draw list

The hardware renderer used to submit a whole map's entire index buffer
every frame regardless of where the real player camera was pointing.
It now builds a real view frustum from that same camera each frame and
skips faces outside it, merging contiguous visible faces back into as
few draw ranges as possible so the existing batched-draw path needs no
changes. A real test map's own spawn camera now submits about a third
of the index buffer instead of all of it, in a few dozen draw ranges
rather than one - measured against the real baked map data and the
real camera the engine captured live, not estimated, and confirmed by
booting it: the live numbers matched the prediction exactly. A new
console variable, `nv2a_r_nocull`, disables it for comparison - with it
off, a screenshot of the same view is pixel-identical to culling being
on. See `docs/r3d/divergences.md` #96 for the full account.

### Added — `ref/nv2a` now draws from the real, live player camera

Replaced the hardware renderer's fixed, one-time debug camera (aimed at
an arbitrary point in the map, computed once when a level loaded) with
the engine's own real, live, per-frame player camera - the same real
position/facing/field-of-view every other renderer uses. Verified two
ways before trusting it: independently reproducing the exact camera
math in Python from the engine's own real captured values and testing
off-axis points (not just dead ahead, which can't tell a correct
camera from a mirrored one) to confirm they land on the geometrically
correct side of the screen in all four directions, and a live boot
showing real geometry roughly centered in the frame - a much more
normal-looking view than the old fixed camera's own arbitrary framing.
See `docs/r3d/divergences.md` #95 for the full account.

### Added — a fix for the recurring live-verification test-environment blocker

Every X9 live-boot attempt so far had silently failed for the same
reason: this fork's own packaging tools give every XBE the identical
hardcoded certificate TitleID, so every test boot on the shared xemu
test HDD writes into the exact same `E:\UDATA\<TitleID>\valve\`
directory, and a stale `video.cfg` left there by any earlier, unrelated
boot permanently shadows a fresh disc's own copy. A new one-shot tool,
`tools/xbox-e-wipe`, mounts E: and deletes exactly `video.cfg` and its
own interrupted-write staging file - nothing else in that directory
(not the whole TitleID directory another nxdk homebrew project could in
principle also be using, and not this fork's own real config/save data
sitting right alongside it). Running it and rebooting fixed the blocker
for real: for the first time, a live boot reached
`ref/nv2a`'s own `R_Init` and ran it to completion, confirmed by reading
guest memory directly.

That surfaced a second, different problem: the engine appeared to hang
before ever presenting a first real frame, on both renderers alike -
this turned out to be a testing-methodology gap, not a new engine
defect. Every live-boot attempt this session had linked a deliberately
empty stub client module, which the engine correctly, deliberately
halts on rather than running with; the real fix was already built and
proven months of project-time ago (X7.4 step 2) and just needed to
actually be used: `tools/xbox-game-link-check.sh hl --include-client
--include-freevgui`. With it, `ref/nv2a` is now confirmed - live, for
the first time - genuinely drawing real baked map geometry, sustained
over thousands of frames. See `docs/r3d/divergences.md` #92/#93 for the
full account.

### Added — the first four slices of the NV2A hardware renderer, now confirmed rendering live

A new `ref/nv2a` module builds and links into the engine, meant to drive
the Xbox's own GPU through the real renderer-selection path. It's grown
from drawing one fixed textured triangle to parsing a real baked map
file and drawing its whole real geometry with a real camera, its own
real compressed diffuse texture, and its own real baked lightmap - a
real view/visibility system, dynamic lighting, and the rest of the
hardware renderer are still follow-on work. Its build/link/parsing logic
is verified correct by direct memory inspection and by validating the
file-format and shader-compiler output against a real baked map (a full
retail map, not just one face) in isolation, and - for the first time -
a live boot now confirms it actually running: a real map genuinely
parsed and drawn, sustained over thousands of frames, real distinct
per-face texture/lightmap variation visible on screen. Whether that
dark overall look is correct stayed a real, open question after a
deeper look: the map's own baked lighting data at that spot is
genuinely dim, but a rigorous, pixel-level comparison against the
actual rendered screenshot didn't confirm the renderer reproduces it
correctly - the most likely concrete bug that result suggested (a
depth-test misconfiguration) was checked and ruled out, but a real
ground-truth comparison remains undone. A real view/visibility system, dynamic lighting, and the
rest of the hardware renderer are still follow-on work, and the
frame-time comparison against X5 on real hardware (not xemu) remains
the real, open exit criterion - see `docs/r3d/divergences.md` #88-#94
for the full account.

### Added — a Controller screen with real, named control schemes

The Xbox pad's own settings screen, rebuilt from the ground up. Pick
Standard, Southpaw (sticks swapped), or Legacy (today's stock defaults,
under a name) from one row, see what every button does on a picture of
the pad, and apply it immediately - Standard also fixes two real
mix-ups in the gamepad's old stock defaults. From there:

- **Settings**: one look-sensitivity slider (replacing four separate raw
  sliders), invert look, vibration, and the on-screen-keyboard toggle
  carried over from the old gamepad screen.
- **Customize**: reassign or clear any individual button, or reset back
  to the active scheme with one press. The pause-menu button is shown
  but can't be reassigned or cleared - it's the only way back to the
  menu from gameplay.

See `docs/r3d/divergences.md` #82-#86 for the full account, including a
real engine bug (stale analog input after switching which stick drives
movement vs. look) found and fixed along the way.

### Fixed — Options no longer has a stale second door into Customize

The standalone "Controls" row on the Options screen used to open the
same screen the Controller screen's own Customize button does, but
with an outdated "keyboard and mouse" label left over from before that
screen became gamepad-only. Hidden, since Customize is already
reachable properly from Controller. "Video" is also renamed to
"Display", matching its own rebuilt screen. See `docs/r3d/divergences.md`
#87.

### Added — synthetic pad input for live menu verification (dev tooling)

Every screen this fork has shipped since Options was rebuilt has only ever
been checked with a static boot screenshot, never confirmed to actually
respond to a real button press - there was no way to simulate one. Three
new `tools/` scripts close that gap: `xemu-pad.py` drives a virtual Xbox
360 controller (via ViGEmBus/vgamepad) into a running xemu instance,
`xemu-debug.py` gained a read-only `cursor` command that reads the
currently-focused menu item straight out of guest memory, and
`xemu-pad-scenario.py` composes the two into scripted "press, wait,
verify" walks. First real use: `tools/verify-entry-76.py` navigated Main
-> Options -> Controls and Main -> Options -> Joystick with a real
synthetic press and confirmed live what had only been claimed from source
before - neither screen shows its PC-only row (Adv. Controls, Gyroscope).
None of this ships to a disc or changes runtime behaviour on any
platform; see `docs/r3d/divergences.md` #76-#80 for the full detail,
including two live navigation surprises a source read alone would not
have caught.

### Removed — PC-only menu screens that had no place on Xbox

Options no longer offers a Gyroscope or Advanced Controls row (neither
means anything on a pad with no gyro and no mouse), and every other
screen that only ever existed for keyboard/mouse play, touch input, or
PC multiplayer - none of it reachable on this fork today - no longer
lingers reachable by console command either.

### Fixed — Load Game / Save Game could freeze on open

A save whose title stripped to an empty string hit a bug that made the
menu read backwards past the end of the string instead of doing
nothing. Not new to this fork - inherited from upstream - but this is
the first place it's been caught, from a real freeze report.

### Added — a control legend at the bottom of every menu screen

Every Xbox menu screen now shows what the pad actually does on it: a
green A, a red B and so on, each with the action it performs, always
in the same place at the bottom of the screen. Screens with a value you
adjust with the D-pad show that too.

### Added — pressing Start during a game now opens a real pause menu

Starting a map and pressing Start used to reopen the full main menu,
just with a couple of extra rows. It now opens a dedicated pause screen
instead: the game visibly freezes and dims behind it rather than
disappearing, the current chapter title and difficulty are shown, and
Resume, Save, Load, Options and Quit are all one press away. Start
closes the menu the same way Resume does; quitting still asks for
confirmation first.

Pausing and unpausing no longer fight with Back, which pauses the game
on its own outside this menu: opening or closing the pause screen only
changes the pause state when this screen is the one that caused it, so
a game already paused by Back stays paused after visiting this menu and
back out again. This screen only opens in singleplayer - Start in a
multiplayer game still opens the regular in-game menu, unaffected.
Saving or loading from Pause correctly resumes gameplay afterward
instead of leaving it paused.

### Added — a Display screen, showing real hardware video state

Options gains a Display screen on Xbox: brightness and gamma (unchanged
from before), texture filtering, and a new read-only row that reports
the console's actual current resolution, refresh rate and aspect ratio
— queried live from the hardware, not a fixed piece of text, so it
stays correct under every Dashboard setting rather than only the common
one.

Two settings the design asked for turned out not to do what their names
promise once checked against the code that actually reads them: one
only ever produces a legacy letterboxing effect with no benefit on this
console, and the other has no way to take effect at all under the
game's own default configuration without a further change this pass
doesn't make. Both are left out rather than shipped broken or
mislabeled; a few settings that only apply to a rendering path this
console doesn't use are also gone, since the game already disabled them
permanently on its own.

### Added — the Xbox menu picks up the design's amber palette

Row labels and a screen heading now use the colour the design called
for instead of the stock game's neutral grey and yellow, on Xbox only —
carried in a small content file the disc ships, not a code change, so
every other platform is untouched and a build without that file looks
exactly as it already did.

A screen heading needed a colour of its own first: nothing distinguished
it from an ordinary status line before, so giving one a colour would
have coloured the other the same way. Verified live that the palette
file actually takes effect; the heading colour itself still needs a
controller in hand to see, the same limit as the screens it colours.

One colour is deliberately left alone rather than guessed at: it is
currently shared between two things the design draws differently, and
picking one would get the other wrong. That split is tracked as its own
follow-up.

### Fixed — help text under a translated menu lost its accented letters

The same German test that proved menu rows translate correctly showed
their help lines losing accents ("für" became "fr"). Both draw through
the same text renderer, just a different typeface, and that second one
was quietly falling back to a character set with no accented letters at
all for the same reason the menu rows themselves needed fixing earlier —
its intended font was never on the disc. Pointed it at the font that is.

Measured with the project's own memory tooling rather than assumed:
this fix costs under a megabyte end to end. That is the true cost of
this change, not a claim that the menu's full memory budget has been
checked — that check needs screens this fork hasn't built yet and stays
open.

### Added — the menu can be translated, and one language proves it

Menu strings are keyed by their own English text, which is the
convention the menu code already expected, so translating the menu needs
a language file and no code changes at all. A German translation ships
as the working example, and a disc can now be packed in a chosen
language.

Verified by doing it rather than assuming: a German disc boots with
German menu rows whose umlauts render correctly. The help lines beneath
them do not — they are drawn with the engine's console glyphs, which
have no accented characters, so they silently lose them. Giving that
text a proper typeface is therefore a prerequisite for shipping any
language rather than a cosmetic nicety, and no disc selects a
non-English language until it lands.

### Added — the menu ships with a real font instead of console glyphs

The Xbox menu now draws every row as text in a real typeface instead of the retail button artwork. The face is Fira Sans Condensed, chosen over the design's original pick after building all three candidates as real discs and comparing them on screen. Without
it the font system fell back to the engine's console glyphs without
saying so — a character set with no accented Latin at all, so most
European languages simply could not be drawn, and English looked
identical either way. Confirmed on a real boot by reading the guest's
own texture table rather than trusting the screen, since that is the
only way to tell the two paths apart.

Measured on a real boot, the text menu costs the console almost exactly
what the retail button art it replaces cost — a wash to within a few tens
of kilobytes — so this buys reach rather than memory. The two halves ship
together by necessity: keeping both would have doubled the menu's texture
budget.

A disc missing its font, or missing the string tables the text now needs,
is a packaging failure rather than a silent downgrade — without the latter
every label would render as its own internal lookup name.

The design originally called for Barlow Semi Condensed. It was dropped
because it cannot draw a single Cyrillic character and is missing nine
Central European letters, which would have quietly limited how far the
menu could ever be translated — the whole reason for the change. The
condensed Fira keeps the narrower look the design wanted, covers every
character the menu can render, and costs exactly the same memory.

The second face the menu can use is deliberately left off: it is Wine's
Tahoma under the LGPL, not an open font licence, and its own notice
refers to a licence text this tree does not carry — so shipping it would
put incorrect provenance on every disc. It only backs the small help
text, which keeps the console glyphs it already had. Giving that text a
properly licensed typeface is tracked separately.

### Added — the Audio screen is trimmed to what a fixed console needs

`Audio.cpp` keeps its three volume sliders (Sound, Music, HEV suit) and
drops DSP effects, mute-on-focus-loss, and both vibration controls -
DSP has no player-facing meaning on a fixed console, and vibration
moves to the Controller screen once it exists (it still works, at its
default of on, just cannot be toggled off until then). The design's
read-only "Speaker output" row is not built yet: nxdk only exposes it
as an undocumented raw EEPROM value, and guessing its layout is exactly
the kind of thing this project's own rules forbid. Every other platform
keeps the screen exactly as it was.

Review caught a different overlap than the Game screen's: the slider
control draws its own label above the coordinate it is placed at, so
the first slider sat close enough to the heading above it that the two
overlapped. Moved down to the same y stock `Audio.cpp` already uses for
its own first slider, where nothing sits above it to collide with.

### Fixed — the Options → Game row showed the wrong button art

It reused `PC_ADV_OPT`, the picture id `PlayerSetup.cpp` already uses
for its own "Adv. Options" button, on the assumption that its ordinal
sat past the classic WON button strip's populated range and would
always render as plain text. Counted correctly, that ordinal is inside
the populated range, so the row drew the strip's real "Adv. Options"
bitmap instead of the "Game" label. A first fix swapped in an id that
happens to be unpopulated in today's strip - review then pointed out
that "unpopulated today" isn't "unpopulated ever", since how many ids
get a picture depends on the loaded bitmap's own size, not a fixed
count. Dropping the picture id fixed that, but the way it was dropped
skipped a bookkeeping step the menu framework needs to clean itself up
without crashing - fixed by keeping that bookkeeping intact and only
leaving the picture assignment out. The row now uses no picture id at
all: a button built without ever assigning it one, so it always
renders as text no matter how the art changes, and shuts down the same
way every other button on the screen does.

### Added — a Game options screen, and Options gains a row to reach it

`GameOptions.cpp` (previously a PC-only network/performance tuning
screen - FPS cap, download permission, `cmdrate`/`updaterate`/`rate`)
is reshaped under `#if XASH_XBOX` into three gameplay toggles: Auto-aim,
Crosshair, and Fast weapon switch - the last a real client cvar
(`hud_fastswitch`) no menu screen linked before this. `Configuration.cpp`
hides Touch and Update (no touchscreen exists; no internet to check for
updates) and adds a new Game row to reach the reshaped screen. Every
other platform keeps both screens exactly as they were.

Controls and Video stay unrenamed for now - "Controller" and "Display"
belong to whichever change actually rebuilds those screens, and
labelling a button for a screen that does not exist yet would be worse
than leaving it as it is.

Review caught the Done button overlapping the new heading and the
Auto-aim row - a real defect the auto-layout helper produced silently,
now positioned explicitly below all three toggles instead - and then
caught the same class of bug PR #83 already fixed once for Main:
registration order is pad focus order, independent of where a control
actually sits on screen, and Done was registered ahead of the three
toggles it sits below. Stock upstream has this exact mismatch too, and
gets away with it because PC navigation is mouse-first; a console has
no mouse, so this now matters on every screen built for one.

### Added — the Xbox Main menu is flattened to a console-shaped shape

Under `#if XASH_XBOX`, `Main.cpp` now shows Resume Game (in-game only) /
New Game / Training Room / Load Game / Save Game (in-game only) / Options,
and hides Console, Disconnect, Multiplayer, Custom Game, Previews and
Quit - a console title has no desktop to quit to and no server list yet.
The old combined "Save\Load Game" button is split into its own Load Game
and Save Game rows. Verified live on both content profiles (the bitmap-
button WON disc and the text-button HL25 disc) with no regression on
either.

Fixing this also fixed something bigger: every `#if XASH_XBOX` written
into this menu module was silently taking the wrong branch, because the
module builds against its own copy of the engine's platform-detection
header and that copy never learned about the Xbox sub-platform at all -
the macro read as undefined (0), not an error, for every file in the
module, for as long as it has existed. `sdk_includes/public/build.h`
now carries the same Xbox branch the engine's own header already has.

Review also caught that the new rows' registration order didn't match
retail's own on-screen order, which would have made Up/Down jump
around the unchanged PC menu instead of following it row by row - the
registration order now follows the platform too. And it caught two
small white squares visible in the corner of every WON menu screenshot
taken this whole session: retail's desktop minimize/close icons, which
a mouse-only flag doesn't actually hide from a screen with no mouse -
neither exists on Xbox now.

### Fixed — a mixed-content menu disc no longer keeps both backgrounds resident

`CMenuBackgroundBitmap::LoadBackground()` (`3rdparty/mainui/controls/BackgroundBitmap.cpp`)
loaded the Steam-style and WON-style background unconditionally whenever a
disc's content carried both, and only ever decided which one to *draw* -
the one not shown stayed loaded, and resident, for the rest of the
session. It now frees whichever side loses right after deciding, and
moves residency over (loading the new side, freeing the old one) if a
player flips `ui_prefer_won_background` at runtime instead of just
switching which one draws. Verified live on a synthetic disc built to
carry both content sets (no real one does): the correct side is picked,
the loser's handle reads freed, and the winner's data is intact -
confirmed at the memory level, not just from what's on screen, since the
current Steam content's own background art renders solid black on both
the fixed and the pre-fix binary alike (a separate, pre-existing issue,
not this change's doing). No regression on a normal, single-profile disc
of either vintage.

Two rounds of review corrections went into this before it was measured
as actually freeing memory: `UI_Precache()` independently preloads the
WON splash under a different cache key than this file's own loader used,
so the two ended up as two separate resident copies of the same image -
freeing one left the other in place. Loading and freeing under the same
key `UI_Precache()` already uses fixed it; the same synthetic disc's
resident memory measurably dropped once it did (3501 to 3350 pages).
Also fixed: a malformed background layout line could leave a texture
loaded but never tracked, unreachable to the cleanup that runs on a
failed background switch.

### Fixed — the console font now draws its own real glyphs instead of neighbour-glyph slices

`con_oldfont` now defaults to `"1"` under `#if XASH_XBOX` (`engine/client/console.c`).
No Xbox disc ships xash-extras' `fonts.wad`/`fonts/font%i` resources, so
`Con_LoadConsoleFont`'s "modern" branch always failed and fell through to
`Con_LoadFixedWidthFont`'s hardcoded 16x16-pixel glyph grid - which doesn't
match the real, variable-width `charinfo` table in either vintage's
`CONCHARS` lump (confirmed by dumping the WON lump's own qfont header and
table: glyphs from 3px to 9px wide, packed edge to edge, 12px row height,
no 16px grid to slice along). The result was legible but wrong text -
consecutive-alphabet fragments like `"PQ RST"` where a run of 16px cells
each straddled two or three real glyphs. Steam content's own `CONCHARS`
happens to use a real 16px grid (the modern qfont layout computes
`width = font.width * 16`), so this never showed there; the classic WON
lump has no such grid, hence "only" the classic disc showing gibberish.
`con_oldfont "1"` routes through `Con_LoadVariableWidthFont`
(`gfx/conchars.fnt`, resolved through the WAD filesystem's existing
`fnt` -> `TYP_QFONT` mapping) instead, which reads each glyph's real
metrics. Verified live on both the classic WON disc (description text and
the bottom version line now read correctly) and a Steam-content disc (no
regression - same correct text, real 16px-grid font unaffected).

### Fixed — the WON console font (and any other TF_KEEP_SOURCE texture) no longer renders as noise

`GL_UploadTexture` (`ref/soft/r_image.c`), under `#if XASH_XBOX`, now
expands an indexed `pic->buffer` to RGBA before its mip loop and
`GL_ResampleTexture` touch it, strided correctly per type (3 bytes/entry
for `PF_INDEXED_24`'s 768-byte palette, alpha forced 255; 4 bytes/entry
for `PF_INDEXED_32`'s 1024-byte one, real alpha). `GL_ProcessImage`
already computes `IMAGE_FORCE_RGBA` for an indexed `pic->type` but never
applies it - a dead local, upstream's own gap, present on every platform
this software renderer runs on - so `Con_LoadFixedWidthFont`'s
`TF_KEEP_SOURCE` (the console font's own load, kept 8-bit deliberately for
later re-tinting) left `pic->buffer` as raw one-byte-per-pixel indices
while every consumer downstream assumed four-byte RGBA: real (if
meaningless) data for the first quarter of the image, zero for the rest
up to the small allocation's actual end, then unmapped memory beyond it.

Found live, tracing the WON menu's console-font corruption for real
box-retail content: both the button-strip atlas and the console font's
own WAD lump were independently proven correct at the decode stage first
(host-side reproductions of each loader's exact byte layout against the
retail files render perfectly legible), which is what pointed at
`GL_UploadTexture` rather than either loader. Verified live: the on-screen
colour-noise grid and yellow/black band are gone, replaced by real
console-font glyphs - legible, though not yet the intended text: those
are consecutive-alphabet slices from a second, separate bug (the console
font loads through the fixed-width 16x16-grid fallback because nothing on
Xbox sets `con_oldfont`, while the oldstyle `CONCHARS` lump is a
variable-width qfont), analysed and recorded in `docs/r3d/fork-plan.md`,
not fixed here. Guarded to Xbox rather than sent upstream (no path to
submit a FWGS patch this session); Codex review round 1 also caught the
first draft always striding the palette at 4 bytes/entry, wrong and
out-of-bounds for `PF_INDEXED_24` (reachable live via the alias-skin
remap path, `engine/client/cl_remap.c`).

### Fixed — a disc without `delta.lst` can no longer be packaged into an invisible boot loop

`tools/xbox-xiso.sh` now refuses to pack a gamedir that provides no
`delta.lst` - loose (any case) or inside a `.pak`, whose directory is
actually read from the archive's header rather than grepped - unless
`--delta-lst <file>` supplies one, taken from the user's own install of a
later release (the Steam `valve/delta.lst`), never from this repository.
Whichever file wins - supplied, loose, or a pak entry extracted by its
directory offset - is parsed with a tokenizer and state machine mirroring
`Delta_InitFields`/`Delta_ParseField` (`net_encode.c:849-885`, `:660-796`):
everything that is `Sys_Error` in the engine is a refusal here, and the
cases the engine tolerates by silently dropping data - a malformed field,
an unknown flag, an empty required section, a field name absent from the
engine's own table for that struct (read out of `net_encode.c` at run
time, not copied) - are refused too. Which file wins follows
`FS_AddGameDirectory`: loose beats every pak, and the pak sorting last
under `Q_strcmp` is searched first. The supplied file is re-resolved in
the stage after the copy; passing the flag for a gamedir that already
ships one is refused rather than silently shadowing it. (Codex review:
round 1 caught the grep-level check accepting seven section names plus
one brace; round 2 caught empty sections passing and the pak scan
checking the first archive the shell glob yielded instead of the one the
engine loads - all three are negative fixtures now.)

Why: a real box-retail Half-Life 1.0.0.9 install (`hlversion "1009"`,
`liblist.gam` only, no `gameinfo.txt`) has no `delta.lst` at all - the file
only exists from 1.1.0.0 on - so `SV_Init` → `Delta_InitFields` hit
`Sys_Error( "couldn't load file delta.lst" )` (`net_encode.c:854`) before
`CL_Init`/`VID_Init`, with no renderer to show it, and the host restarted
into the same error every ~3 s: the screen kept the nxdk boot splash and
the CPU sat in the kernel's idle wait, indistinguishable at a glance from
a hung syscall. Found by tracing the engine's own init milestones with
`tools/xemu-debug.py` breakpoints after four disc variants (full retail,
no intro videos, zero-padded to 600 MB, an 8 MB menu-only subset) and a
misread `stat("D:/")` theory had all produced the identical signature -
recorded in `docs/r3d/lessons.md`. With the gate satisfied the same
content boots to the real 1998 WON menu ("New game / Hazard course /
Configuration / Load game / Multiplayer / Previews / Quit" over
`gfx/shell/splash.bmp` from `pak0.pak`) - the first pre-Steam boot this
port has done, and the content XM item 0's step 5 was waiting for. Also
corrects the script's own "untested path" warning: `liblist.gam`-only
content parses and boots; `delta.lst` was the actual gap.

### Added — an offline bake for the Xbox menu background (`tools/asset-studio --bake-menu-art`)

New `tools/asset-studio` mode composites a retail gamedir's tiled menu
background, resizes it to 640x480, and re-tiles it into ordinary TGA
files under the same names `mainui` already reads - not an archive, since
this engine's menu loads through `PIC_Load`, not any consumer
`UiPacker`'s existing formats have. `tools/xbox-xiso.sh --menu-bake <dir>`
overlays the result onto a staged disc after the retail copy, so a disc
built without the flag is unaffected. Measured, same binary, same
content: 0.25 MiB saved (2512 vs 2448 `VirtualMemory` pages) - real, but
under this phase's own ~1.0-1.2 MiB estimate, not yet explained. Visual
result confirmed correct (real retail art, not the black tiles this was
originally tested against - the local Steam install changed content
mid-session, see `docs/r3d/fork-plan.md`'s XM item 0 step 5), with pixel-
level continuity checked across the retile seam.

### Fixed — the animated HL25 main-menu title now draws

`tools/xbox-xiso.sh` now copies `animated_title`/`hd_background` from a
staged disc's `liblist.gam` into its `gameinfo.txt` when the latter is
missing them. The real Steam install's `gameinfo.txt` has neither field,
even though `liblist.gam` sets both to `1` and the engine's own
`FS_ParseGameInfo` prefers an up-to-date `gameinfo.txt` over re-deriving
from `liblist.gam` - so a disc packaged by copying both files verbatim
never set `GFL_ANIMATED_TITLE`, and the animated logo never drew.
Confirmed fixed live: `CMenuAnimatedBanner::TryLoad` now fires and
returns true, and the real "HALF-LIFE" title draws over its blurred
parallax backdrop, animating between captures a few seconds apart.
Measured cost, same binary: 1324 `VirtualMemory` pages (5.17 MiB) -
busts the 3 MiB per-profile budget on its own, matching this port's own
~5.2 MiB estimate closely; an offline bake (not done yet) is the planned
next step. Also corrects an earlier, wrong claim (X7.5) that a real
disc's `gameinfo.txt` always carries everything its `liblist.gam` does.

### Fixed — the flat-grey Xbox main menu: retail background/button-strip art now loads

`XASH_LOW_MEMORY` (X3) blocked the retail menu background (and the
pre-HL25 button strip) from loading at all, alongside several unrelated
memory-saving effects. A new, finer, non-read-only cvar
(`ui_xbox_menu_art`, default on for Xbox) lets the background load run
independently, read live and confirmed with a `userconfig.d` override -
measured at a 1.125 MiB cost on the same binary (`memstats`, gate on vs
off). The button-strip half of the gate could not be made
config-toggleable within one build without a real regression (every menu
button snapshots its hit-rectangle size at construction time, before any
config-reactive load could run) - reverted to upstream's own load timing
after Codex review caught it; a genuine A/B needs two builds, and this
session's own disc has no button-strip art to test with regardless.
`GFL_HD_BACKGROUND` is masked on Xbox so a 25th-anniversary disc's
`hd_background 1` does not try a 105-tile, 3840x1600 background layout on
a 640x480 screen. Confirmed live by reading the guest's own memory (not a
screenshot): the retail 800x600 background tiles are genuinely opaque
black by design - not a rendering bug - so the animated title (a
follow-up step) is where this port's actual "does the menu look right"
question gets answered.

### Changed — `mainui` (and its nested `miniutl`) moved to owned forks

XM's "Where the code lives" prerequisite: substantive menu-screen changes
cannot live as uncommitted patches. Forked `FWGS/mainui_cpp` to
`mathiasdbo/mainui_cpp` and `FWGS/miniutl` to `mathiasdbo/MiniUTL`, each on
a `resonance3d` branch created from the exact commit already vendored -
no source content changed by the move itself. The NXDK `winlite.h` fix
that used to be a patch file auto-applied at sweep time is now a real
commit on the `MiniUTL` fork; the patch file is gone. The sweep script now
runs `git submodule sync`/`update` itself, unconditionally, before every
sweep, so an already-initialised checkout's cached submodule urls (which
a plain `git submodule update` would not otherwise repoint) self-heal
with no manual step. Re-verified against the exact same
commits: the mainui sweep still compiles 82/82, the baseline engine+menu
link is still 0 duplicate/0 undefined.

### Added — `tools/xbox-xiso.sh`: one real, packaged XISO per title

Assembles a bootable disc from a real Steam Half-Life install and an
already-built `default.xbe`: copies the title's gamedir and basedir,
drops in the XBE, packs with `extract-xiso`. No retail content is copied
into the repo. `gameinfo.txt`/`video.cfg` need no generation or editing -
checked directly against a real install, both are already what the
Xbox target needs, verbatim. `hl` verified end to end: packed, booted in
xemu with no test-only shortcuts, reached the real WON main menu.
`op4`/`bshift` are wired into the title table for when their XBEs exist
(X7.6) but not yet tested.

### Fixed — dedicated-server link check reported `_lib_ref_null_exports`/`_lib_ref_soft_exports` undefined

`engine/platform/misc/lib_static.c` includes the repo-wide generated
library-dispatch table unconditionally, and since X5 registered both
renderers in it, the dedicated-target link (`tools/xbox-dedicated-link-
check.sh`) has been undefined on those two symbols - a dedicated server
never renders, so it never had a reason to compile them, unlike the
already-real client link. Fixed with the same empty-table stub the script
already used for the menu table it also never reaches. A second, unrelated
bug found while re-verifying the fix (stub objects sitting loose in the
scratch directory made a second run without a re-sweep spuriously fail)
is fixed alongside it. `tools/xbox-dedicated-sweep.sh` then two consecutive
link-checks with no re-sweep between them: both clean, both a valid XBE.

### Fixed — floating-point printf on Xbox: every entity placed with an `"angle"` key faced yaw 0 (player spawning backwards, guard facing sideways in `c0a0`)

nxdk's pdclib parses `%f`/`%e`/`%g` and then prints nothing - and never
consumes the argument, so everything after a float is misaligned as well
(`_PDCLIB_print.c:452-458`). The engine's own `"angle"`→`"angles"` keyvalue
conversion (`sv_game.c:5023`, `Q_snprintf( "%g %g %g" )`) therefore
produced an empty string that the HL SDK parsed as `0 0 0`. New
`compat/xbox/xbox_printf.c` provides the buffer-writing printf family
(`vsnprintf`/`snprintf`/`vsprintf`/`sprintf` and the `_`-prefixed MSVC
spellings) with real floating-point support, verified on the host against
the C library on 77 formats. Confirmed live: with the fix in the build, booting into
`c0a0`, the server writes the spawn `svc_setangle` as `(0, 180, 0)` - the
map's own `"angle" "180"` - and the old build's view looks backward into
the tram car where the fixed build's looks forward down the track. The
guard's facing on screen and the hardware run remain -
`docs/r3d/fork-plan.md` XG.1, `docs/r3d/divergences.md` #54. The host
test that validated the formatter is committed as
`tools/host-tests/printf_host_test.c`.

### Fixed — gamepad gameplay actions (jump, crouch, use, attack, ...) surviving `config.cfg`'s `unbindall`

Found live during X6's own controller test: the WON menu navigated fine,
but no gamepad button did anything once a map loaded. Root cause: retail
Half-Life's own `config.cfg` starts with `unbindall`, wiping the full,
already-correct default gamepad scheme `Key_Init()` sets up
(`engine/client/input/in_keys.c`) - `config.cfg` has no idea gamepad
buttons exist, so it never re-lists them the way it does every keyboard
key. Fixed with a new, Xbox-only, one-shot `Key_SeedGamepadDefaults()`
called from `Host_InitCommon` right after `config.cfg` runs: it fills in
the compiled defaults for any gamepad key still empty, then sets an
archived marker cvar (`joy_binds_seeded`) so it never runs again for that
config - a legacy keyboard-only config gets the defaults once, a config
that already carries gamepad state (including a bind the player cleared
on purpose in the Controls menu) is left exactly as saved, and `unbindall`
/`resetkeys` keep their upstream semantics everywhere. Two Codex rounds
shaped this: the first version changed `unbindall` itself, which would
have altered shared behaviour for PC users and mod scripts, and was
dropped.

Compiles clean; not yet re-verified in a live gameplay session or through
the real Controls menu (would need a full client relink, not just this
file's own compile check) - see `docs/r3d/divergences.md`#53.

### Verified under xemu — the WON menu is navigable with a controller (X6, not yet closed)

No code change: `engine/client/input/in_joy.c`'s joystick support and
`3rdparty/mainui`'s `Gamepad.cpp` were already wired up unconditionally for
any non-dedicated build, the same code the psvita/nswitch ports already
use. Verified this session by reading that path end to end, then by a live
human test with a real host gamepad bound in xemu - the main menu responded
to it and started a new game. **X6 stays open, not closed**: xemu is not
validation per this project's own rule, and real-hardware confirmation of
the same test has not happened yet - flagged in Codex review after an
earlier version of this entry called it "Closed" while saying that in the
same breath.

### Added — `tools/x4-triangle`: standalone NV2A bring-up, one textured triangle (X4)

A separate, minimal (~280-line) XBE outside the engine entirely: initialises
pbkit, sets 640x480, and draws one triangle textured with an 8x8
checkerboard via a Cg-compiled vertex/pixel shader pair, matching
`deps/nxdk/samples/triangle/main.c`'s structure with texture setup read from
`deps/nxdk/samples/mesh/main.c` (attribute slot 9 for texcoord0, working
`NV097_SET_TEXTURE_*` register values for stage 0). Boots and renders at
61-63 fps under xemu; not yet run on real hardware.

Uploads both a linear and a Morton-swizzled copy of the texture
(`tools/asset-studio/src/utils/TextureOpt.cpp`'s `morton2D`/`Part1By1`
algorithm, ported to C) and can bind either
(`X4_TEXTURE_SWIZZLED` in `main.c`). Ships with the linear copy bound by
default - verified correct under xemu (the checkerboard is visibly on
screen). The swizzled copy renders as a flat fill instead. Its
`NV097_SET_TEXTURE_FORMAT` encoding and uploaded bytes were checked for
self-consistency (the pushed dword matches this file's own derivation, the
uploaded bytes match this file's own Morton expectation) but that does not
rule out a program-side defect - corrected in `docs/r3d/fork-plan.md`'s X4
section after Codex review pointed out the original write-up overclaimed
this. Genuinely unresolved whether the cause is an xemu limitation, a field
this program has wrong in a way self-consistency can't catch, or the shared
Morton algorithm not matching real NV2A behaviour; recorded as an open
finding for the first real-hardware session, not treated as closed.

### Added — frame-time measurement: `xemu-debug.py fps`, `tools/xbox-offsetof.sh`, and a 320x240 render target on Xbox (X5, divergence #52)

`fps` reads `host.framecount` twice N seconds apart (wall-clock fps and
ms/frame) plus the engine's own `host.pureframetime` (the last frame's
CPU time without `Host_Autosleep`'s sleep), after the same two identity
checks as `pools`. The field offsets are computed at run time by the new
`tools/xbox-offsetof.sh <header> <type> <field>`, which compiles a probe
through `tools/xbox-cc.sh` and reads `offsetof`/`sizeof` out of clang's
own error text - the Xbox build's exact flags and ABI, no constants in
the tool (`XASH_LOW_MEMORY` had just moved `host_parm_t`'s fields; review
rightly refused a copied number with no drift check).

Found doing it: a read-only debugger connection pauses xemu's VM and a
socket close (or a gdb detach) does not resume it - only a continue
does. Every `xemu-debug.py` command is now a context-managed connection
that resumes the guest through the monitor on every exit, exceptions
included, and reports with a nonzero exit if the stub refused. Two
frame-rate runs had reported the counter frozen at the same value in two
separate boots: the driver's first `regs` pausing the guest at the same
moment each time, not a hang.

Map identity, made real (review, two rounds): section extents are not
identity, so `tools/xbox-game-link-check.sh --xbe` emits the map from the
XBE's own link and places it beside the XBE. The first binding scheme
(the link timestamp lld writes into both the map header and the PE's
COFF header) was itself rejected in round 2 - it is wall-clock SECONDS,
so two links in the same second collide, and with equal section extents
too (an ordinary equal-size substitution) a stale map would still bind.
Replaced with a SHA-256 sidecar (`xbe-build.exe.identity`, written next
to the exe with the exact hash of it and of its own map); `pools`/`fps`
require both hashes to match the files on disk before reading anything -
verified live, tampering included. The same round found `close()`'s
resume-on-exit could not distinguish "resumed" from "the stub gave no
reply at all" (both were an empty string); `monitor()` now raises on
that case specifically, so the warning and nonzero exit are real.

`vid_scale` cannot give a 320x240 render on `ref_soft` (it refuses any
scale transform) and the console has no 320x240 video mode, so the Xbox
video arm honours an integer `vid_scale` itself: `vid_scale 2.0` renders
320x240 into the top-left of the 640x480 surface, unscaled - the
`R_SaveVideoMode( 640, 480, 320, 240 )` lever the plan names, which is
what a frame-time measurement at that resolution has to measure.
Presenting it full-screen is `vid_xbox.c`'s 2x2 blit, the planned final
path.

That 320x240 render is now presented full-screen: `SW_UnlockBuffer`'s
Xbox arm expands it 2x2 (nearest, in place, one expanded row per group
and `memcpy` for the rest) before the driver's own copy. Costs measured
at `c1a3`: 43.0 fps render-only, 36.7 with a divide-per-pixel version,
32.7 with the `memcpy` version kept - slower under xemu, where the
guest's `div` becomes one native instruction and `memcpy` is emulated
step by step, and the right choice for a Pentium III, where a `div` per
output pixel is ~16 ms a frame. Bilinear is the GPU's job
(`vid_xbox.c`). The engine's own `cl_showfps 2` counter draws on the
Xbox screen through `ref_soft` and agrees with the tool.

Measured (xemu, `ref_soft`, spawn view, wall fps / `pureframetime`):
`c0a0` 18.3 fps / 52.5 ms at 640x480, 25.9 / 37.4 at 320x240; `c1a3`
28.5 / 34.7 and 43.0 / 23.5; `crossfire` 22.2 / 44.7 and 29.1 / 34.3.
CPU-bound throughout (wall and pure agree), with a fixed full-window
`SDL_ConvertPixels` per frame that `vid_xbox.c` will delete. X5's exit
line is met under emulation; the console repeat is pending hardware.

### Changed — Xbox builds use upstream's low-memory mode; the first map now fits, and it is on screen (X3, divergence #51)

`tools/xbox-cc.sh` compiles every Xbox translation unit with
`XASH_LOW_MEMORY=1` (upstream's own level-1 mode, MotoMAGX's; the
protocol is unchanged, level 2 would change it). Measured on `c0a0` under
`ref_soft`: image 27.1 → 16.1 MiB (`cl` 3.4 → 2.1 MiB, `sv` 2.4 → 0.7,
`net` 1.6 → 0.3, `mod_known` 1.5 → 0.4, `ref_soft`'s `r_images` 2.4 →
0.3, `g_studio` 1.8 → 0.2), and the map that ran the console out of
memory in `CL_InitEdicts` now spawns with 12.6 MiB free and rides the
tram to `c0a0a` with 11.6 MiB free. Captured off the GPU's scan-out: the
inside of the tram car, lit and textured, at 640x480 in software - X5's
"recognisable map" (its frame-time half is next). Sweep baseline moves to
202 ok / 29 failing: `engine/platform/posix/con_posix.c` is entirely
`#if !XASH_LOW_MEMORY` and now compiles empty (not linked on Xbox anyway).

### Added — `tools/xemu-debug.py pools`: where a map's heap actually goes

Reads the engine's own pool table (`zone.c`'s `poolchain`, located via
the map's `_poolcount`/`_poolchain`) and lists pools largest first.
`memstats` says how much; this says what. Found in review, twice: it
runs the full `verify-image` identity check against the linked PE
(`--exe`) before reading a byte, refuses an empty or unprintable table,
and binds the map to that PE without a sidecar - every output segment's
extent in the map's own section table must equal the PE section's
VirtualSize within one 16-byte alignment (a map from a different link is
off by whole functions). This project's own first dump read a credible
"0 pools" from a stale map. `c0a0` up: `ref_soft zone` 12.1
MiB (surface cache ~2.9 + the map's textures as 16-bit texels with mips
and alpha copies), the BSP 3.3, client edicts 2.7 (`max_edicts 900`),
`SoundLib` 2.6 (PCM), client static 1.8, models ~1.3 - 26 of the 30 MiB
of heap. X3's plan entry ranks the next levers from these numbers.
Unit-tested (`Pools`).

### Added — `ref_soft` on the Xbox, and the first frame on its screen (X5, divergence #50)

Upstream's software rasterizer (`ref/soft` + `ref/common`) now compiles
for Xbox (`tools/xbox-cc.sh` gives `ref/` sources `REF_DLL=1` and their
own include directories first - the 20/20 sweep failure was the engine's
`ref_common.h` shadowing the module's), is isolated by the new
`tools/xbox-ref-isolate.sh` (every defined symbol renamed `soft_*`, the
way the SDK is; raw next to the engine it collides on 20 names), and is
registered as `ref_soft` with `XASH_REF_SOFT_ENABLED=1`, so a retail
`video.cfg` selects it through `R_Init()`'s own loop. Divergence #46's
three video bypasses are gone: nxdk's SDL2 driver only asserted because
no mode was set, so `R_Init_Video` sets 640x480x32 and the common path
runs. Measured on the real scan-out: Half-Life's loading screen, drawn
by `ref_soft`. The first map then runs the 64 MiB console out of memory
(`CL_InitEdicts`, 0.97 MiB free; `ref_soft` costs ~7.6 MiB of statics
and ~8.5 MiB more heap than `ref_null`) - X5's exit is blocked on X3/X8's
memory work, with its first real numbers.

### Fixed — two SDL-assert landmines on Xbox

`VID_GetWindowSizeInPixels` called `SDL_GetRendererOutputSize` with a NULL
renderer in software mode; a release SDL2 shrugs, nxdk's (assertions on)
aborts and the boot rebooted every 2.9 s with no error printed. Guarded
(an upstream latent bug). And `Xbox_Init` now installs an SDL assertion
handler that routes any failed `SDL_assert` into `Sys_Error` with its
condition/file/line - after divergence #49's hang and this abort, the
second and third live sessions spent on a silent SDL assert. Found in
review: every later mode change (`vid_setmode`, a cvar restart) is now
clamped to the one 640x480 framebuffer nxdk's driver has, at the
`R_ChangeDisplaySettings` choke point, instead of reaching the driver's
own size assert.

### Changed — the Xbox link check only links objects the sweep vouches for

`tools/xbox-sweep.sh` now writes `sweep-manifest.txt` (the
`tools/xbox-cc.sh` checksum it compiled with, then `OK`/`FAIL` per file,
deleting a now-failing source's stale object), and
`tools/xbox-link-check.sh` refuses any engine object the manifest does
not list OK, any manifest from a different driver checksum, and any
object older than its source or the driver. Found in review: it used to
link whatever `*.obj` sat in `.sweep/`, so a build could pass "0
duplicate, 0 undefined" on objects predating a source edit or a new `-D`
- exactly what adding `XASH_REF_SOFT_ENABLED` would have hidden.

### Verified — X7.4 step 4 closed: a map loads, the tram changes level, saves land on the HDD

On the fixed build (divergence #49), booting with `map c0a0` runs the
whole step-4 chain in under two seconds - `SV_SpawnServer`,
`Mod_LoadWorld`, `SV_SpawnEntities` (every classname resolved), the game
DLL's `ServerActivate`, `SV_SendServerdata`/`CL_ParseServerData`,
`SV_PutClientInServer`, `CL_SignonReply` - then rides the tram
unattended: `c0a0` → `c0a0a` at 47 s and `c0a0a` → `c0a0b` at 94 s
through `SV_ChangeLevel`/`SaveGameState`, with `Sys_Error`/`Host_Error`
armed and never hit. The emulated HDD holds the real save files
(`E:\UDATA\FFFF0002\valve\save\c0a0.HL1`, 41,644 bytes, and siblings) -
divergence #48's writable rootdir, proven by its first real write.
Memory, from the kernel's own page counters (the `MmQueryStatistics`
fields, read through `MmGlobalData`; `tools/xemu-debug.py memstats`):
32.45 MiB available with the menu up, 17.41 MiB with `c0a0` received -
the first map costs +15.04 MiB, all `VirtualMemory`; the 19.11 MiB image,
3.01 MiB of stacks and 2.00 MiB contiguous do not move. X8's opening
numbers.

### Corrected — PR #58's "verified live" was measured against a stale ISO

xemu's `dvd_path` named a pre-fix copy of the disc image next to the
freshly packed one, so every post-fix run booted the old build: its
"reaches `SV_ExecLoadLevel` immediately" hit an unrelated function at
that address, and its "`SV_SpawnServer` never reached, slow filesystem
lookups" finding was an artifact, not a real characteristic. Divergence
#49, the X7.4 plan entry and `lessons.md` now say so; the real
verification above replaces it.

### Added — `tools/xemu-debug.py verify-image` and `memstats`

`verify-image` compares every file-backed byte of the linked PE's
read-only sections (`.text`, `.rdata`) against guest memory, exit 1 on
any mismatch - the check that would have caught the stale ISO on the
first post-fix run. The Xbox loader's in-place kernel-import thunk patch
(ordinal in the file, kernel address in the guest) is accepted only when
the guest dword is exactly what the running kernel's own export table
gives for that ordinal - a different in-range kernel function in the
same slot is a mismatch. Two earlier versions were rejected in review: a
12-symbol sample (cannot rule out a build differing elsewhere) and an
"any kernel address" thunk rule (cannot rule out a different import).
`--addr` remains as an explicitly-labelled spot check, exit 2 whenever
any requested address was unreadable. `memstats` reads the kernel's own
page counters through the exported `MmGlobalData`, with a self-check
that available + busy is the 64 MiB. Both unit-tested
(`tools/test_xemu_debug.py`: `VerifyImage`, `KernelExportsAndMemstats`). The tool's own launch
notes no longer claim breakpoints do not survive `system_reset`; they
do (`lessons.md`), which is exactly why stale ones are dangerous.

### Fixed — Xbox no longer hangs entering gameplay: unsupported SDL2 relative mouse mode (divergence #49)

X7.4 step 4, a bring-up blocker: the first time this fork's boot ever
reached a `key_menu`→`key_game` transition (the menu closing for a real
map load), the engine hung permanently instead of continuing. Traced
live to `IN_SetRelativeMouseMode()` calling `SDL_SetRelativeMouseMode()`,
which asserts `mouse->WarpMouse` is set inside SDL2's own
`ShouldUseRelativeModeWarp()` - and Xbox's SDL2 video driver never
registers a `WarpMouse` or `SetRelativeMouseMode` callback, since a
console has no OS mouse cursor for either to warp. With no interactive
way to answer SDL's own assertion prompt on a console, the failure
manifests as an unrecoverable hang rather than a clean abort.

`IN_SetRelativeMouseMode()` now skips the SDL relative-mode calls on
Xbox specifically (`#if XASH_SDL >= 2 && !XASH_XBOX`), while still
tracking its own grab state normally - the gamepad is Xbox's real aim
input, not a warped mouse cursor. Verified live: the previously-stuck
boot now reaches `SV_ExecLoadLevel` immediately instead of hanging.

The statically linked Half-Life client DLL has its own, independent
copy of this same unguarded call (`deps/hlsdk/cl_dll/inputw32.cpp`'s
`IN_SetMouseRelative`, reached from every `key_menu`/`key_console` to
`key_game` transition, not only the first) - found in review and
fixed the same way, directly in this fork's owned `deps/hlsdk`
checkout.

### Fixed — a real writable rootdir, split from the read-only disc (divergence #48)

X7.4 step 3, a bring-up blocker: `FS_DetermineRootDirectory` resolved
to `D:\` (the disc, X0.5's own real fix for "can the engine find its
assets at all") - but that value becomes `fs_rootdir`, upstream's own
name for the WRITABLE root, and an XISO is permanently read-only. The
first real level transition's `SaveGameState()` (`.HL1`/`.HL2` under
`<gamedir>/save/`) would have failed on it.

`-rodir` cannot arrive via argv or `getenv()` on this platform (nxdk's
own crt0 hardcodes `argc=0`; `getenv()` is a real, unconditional `NULL`
stub), so both `FS_DetermineRootDirectory` and
`FS_DetermineReadOnlyRootDirectory` gained their own direct `XASH_XBOX`
arms. `E:` (confirmed against `hal/fileio.c`'s own partition table to
be `\Device\Harddisk0\Partition1\`, the same partition every retail
title's own `E:\UDATA\<title ID>\` save path already uses) is mounted
and `E:\UDATA\<title ID>\<gamedir>\` created if missing, then returned
as `fs_rootdir`; `D:\` moves to `fs_rodir`, the read-only overlay. The
extra `<gamedir>` level (found in review): `cxbe`, this project's own
XBE-building tool, hardcodes the same certificate `TitleID` for every
XBE it builds (its own `// TODO`), which a first live check confirmed
would collide across future titles - namespaced under `XASH_GAMEDIR`
instead, the same compile-time per-title name `launcher.c` already
relies on for the identical purpose.

Verified live, after the fix: a fresh boot reads `fs_rootdir` as
`E:/UDATA/FFFF0002/valve` and `fs_rodir` as `D:` directly out of guest
memory - the real global the filesystem module writes through -
`nxMountDrive('E', ...)` confirmed to genuinely run and succeed, and
`Host_Frame` is still reached with no `Sys_Error` anywhere in the path.
A real write landing under this directory remains X7.4 step 4's own
exit test. See divergence #48.

### Verified — X7.4 step 2's boot milestone is closed: all three indicators confirmed live

Following the stack-size fix, the milestone's third named indicator -
`UI_LoadProgs` loading the menu - was confirmed directly instead of
inferred from the frame loop surviving. A breakpoint on its own return
address shows `EAX = 1` on a fresh boot: it genuinely loads
`3rdparty/mainui` (statically linked, `GetMenuAPI` resolves in the real
link) and `GetMenuAPI()` succeeds. Activation confirmed past the
engine's own `UI_SetActiveMenu` dispatcher, not just its entry (Codex
review: an entry breakpoint alone cannot rule out `gameui.hInstance`
being null and the call taking its own early-return path one line
later): a breakpoint on `3rdparty/mainui`'s own, separately-linked
`UI_SetActiveMenu(int)` fired with `fActive=1`, called from the
engine's dispatcher at exactly its `pfnSetActiveMenu(...)` call site,
past the null-guard - real proof the request reached mainui. Codex
review, round 2, correctly pushed further: that request only defers
opening the menu to the next `UI_UpdateMenu()` call
(`3rdparty/mainui/BaseMenu.cpp`'s own comment says so). Checked
directly, not inferred: a breakpoint on mainui's own `UI_Main_Menu()`
(the real, deferred open) fired, and a clean, repeated read of mainui's
own `UI_IsVisible()` - five consecutive samples, each matched to its
real return address - read `EAX=1` every time. (Two earlier attempts at
this specific check produced a false `EAX=0` and were not trusted: a
missing single-step-over-the-breakpoint before re-arming, compounded by
stale breakpoints surviving a `system_reset` from earlier in the same
session, desynced the read - a full xemu process restart plus correct
step hygiene is what actually clears the debugger's own breakpoint
list. Same persistence property `docs/r3d/lessons.md` already
documents (found while root-causing divergence #46's stack overflow),
confirmed to bite the same investigation a second time.) With the menu
confirmed genuinely active, not just requested, a further stability
check found no new failure: 2000 consecutive counted `Host_Frame` calls
in 8.3s.

All three of the milestone's own named indicators are now independently
confirmed with real, live evidence: `CL_Init` passes, `UI_LoadProgs`
loads and activates the real menu, `Host_Frame` runs, sustained. X7.4
step 2 is closed. No code changed - `docs/r3d/fork-plan.md` updated.

### Fixed — the engine's main thread ran on a 64 KiB stack; now 1 MiB (divergence #47)

Root cause of the stack-overflow bugcheck divergence #46 left open,
found live rather than guessed. It was never recursion: the bugcheck's
own ESP sat only ~2 KiB below `Host_Frame`'s entry ESP. Catching nxdk's
own `_xlibc_check_stack` (armed only after the first `Host_Frame`, since
init probes 32 KiB for every console command) named the frame: the very
first frame's `NET_QueuePacket` (`engine/common/net_ws.c`) puts
`byte buf[NET_MAX_FRAGMENT]` - 65535 bytes - on the stack, and the check
raises `EXCEPTION_STACK_OVERFLOW` as `NONCONTINUABLE`, which the kernel
turns into exactly the observed `KeBugCheckEx(0x1E, 0xC00000FD)`.

It could never fit because `nxdk-link` hardcodes `-stack:65536`, and
nxdk's `CreateThread(..., 0, ...)` - including the thread crt0 runs
`main()` on - inherits that XBE header value: the whole engine was on
64 KiB. Upstream assumes Windows' 1 MiB and already raises the stack on
its own small-default ports (DOS 512 KiB, Emscripten 16 MiB). Every
`nxdk-link` call in `tools/xbox-game-link-check.sh`,
`tools/xbox-link-check.sh` and `tools/xbox-dedicated-link-check.sh` now
passes `-stack:1048576` (the dedicated tool was missed at first - found
in review; its flag is verified to propagate, though that target's own
boot could not be re-verified: its link currently fails on a separate,
pre-existing `_lib_ref_null_exports` undefined symbol, identically with
or without the flag). The flag is also guarded, not just set: a new
read-only `tools/xbox-check-stack.sh` reads the real field (PE
`SizeOfStackReserve`, XBE `SizeOfStackCommit` at 0x130) and every link
tool now asserts 1048576 on every PE it links and every XBE it converts,
so a toolchain-ordering or propagation regression fails loudly instead
of silently restoring the overflow (found in review).

Verified in the PE (`SizeOfStackReserve`) and XBE (`SizeOfStackCommit`)
headers, then live: 300 consecutive counted `Host_Frame` calls with no
failure breakpoint firing, and a 75-second free run with nothing armed
ending in the kernel's between-frames idle loop, not the bugcheck.
X7.4 step 2's boot milestone now holds for real - `CL_Init` passes and
`Host_Frame` runs, sustained. `UI_LoadProgs` loading the menu is not
claimed yet: next to verify directly. See divergence #47.

### Fixed — Host_Frame reached for the first time ever (divergences #45, #46)

Continuing live boot debugging from divergence #44's fix, the boot
advanced further and hit two new, real, sequential failures:

`R_Init()`'s renderer selection failed with "Can't initialize any
renderer" - its cross-platform bruteforce list never includes `"null"`
on any platform by design, and this milestone's boot-test disc's
`video.cfg` (copied from real retail Half-Life assets) explicitly sets
`r_refdll ""`, overriding any compiled cvar default. Fixed with a real,
`XASH_XBOX`-gated last-resort attempt to load `ref_null` explicitly
after every other option fails (divergence #45) - `ref_null` is already
confirmed statically linked into this build, so this is a real, working
fallback, not a guess.

That unblocked renderer selection but immediately exposed the next
failure: `R_Init_Video()`'s very first SDL call lazily inits SDL's
video subsystem, reaching nxdk's real `SDL_xbvideo.c` driver and
hitting its own unconditional `assert(0)` in `pixelFormatSelector` -
exactly the `CL_Init` → `VID_Init` → `vid_sdl2.c` → `SDL_CreateWindow`
chain divergence #14 already anticipated, back when no real Xbox binary
existed to reach it, deliberately left undecided between two named
options. Fixed by choosing the one matching this milestone's own
headless-boot goal: mirror `CL_Init`'s own established
`Host_IsDedicated()`-equivalent short-circuit, narrowed to video only,
rather than committing to a real video-mode default that belongs to
X5's own planned `VIDEO_XBOX` backend (divergence #46).

With both fixes together, live-verified: the boot demonstrably reaches
`Host_Frame` (`Host_RunFrame` → `Host_Frame`, confirmed by breakpoint
hit and real caller resolution) - X7.4's own actual boot milestone, not
just its link-only gate. A single-step trace of `Host_Frame`'s first
~400 real instructions shows entirely normal per-frame processing
before hitting a new, reproducible kernel bugcheck (`KeBugCheckEx(0x1E`
`KMODE_EXCEPTION_NOT_HANDLED, 0xC00000FD` `STATUS_STACK_OVERFLOW`) a
short time into `Host_Frame`'s real logic - a genuine stack overflow,
precisely characterized but not yet root-caused, real separate
follow-up work for a future session.

Codex review of the video-init fix found a real, separate invariant
bug in it: the early return skipped clearing `host.renderinfo_changed`
(already forced true by `VID_Init`'s own cvar registration), which
would have let `Host_Frame`'s own per-frame `VID_CheckChanges` reach
the same broken driver a different way. Fixed, with defense in depth
(`VID_SetMode()` itself also guarded, not just the one call site).
Re-tested live: the stack overflow still happens, at the same point,
with the identical signature - ruling this out as the actual cause
rather than leaving it unconfirmed; it remains a genuinely separate,
open problem.

A second Codex review round found two more real gaps: the independent
`vid_setmode` console command bypassed every guard by calling
`R_ChangeDisplaySettings()` directly, and every Xbox arm left
`refState.width`/`height` at zero (real client code reads them
unconditionally once active). Fixed together - every arm, including a
new one added directly in `R_ChangeDisplaySettings()` itself (the
actual single choke point all routes share), now establishes real,
non-zero dimensions via `R_SaveVideoMode()` instead of a bare
`return true`. Re-tested live a third time: the stack overflow still
happens, identically - ruling out this round too. See divergences
#45/#46 and `docs/r3d/fork-plan.md`'s X7.4 step 2 for the full account.

### Fixed — deps/hlsdk: exempt exactly "valve" on NXDK, not a prefix match (divergence #44)

Found via live boot debugging of X7.4 step 2's first real combined
server+client+freevgui build: the boot reliably executed a `quit`
command ~2.5-3.3s into every attempt, restarting the whole init
sequence from scratch each time. Traced the real call chain
(`Cmd_ExecuteString` → `CL_Quit_f` → `Sys_Quit("command")`) to
`cdll_int.cpp`'s `Initialize()`, which quits whenever `CL_InitClient()`
returns false - and it was returning false because
`UTIL_IsValveGameDirectory()`, a real HL SDK safety check meant to stop
a mod's compiled DLL from running inside someone's real retail Valve
install, correctly refused the moment it saw our disc's real retail
Half-Life assets sitting in a directory named `valve` (matching
upstream HL's own canonical gamedir - this fork ports the stock game
itself, not a mod alongside a separate retail install).

Fixed in `deps/hlsdk` (an owned fork, direct commit, not a patch):
`UTIL_IsValveGameDirectory()` gained a new `NXDK` arm, alongside the
existing `_STATIC_ENGINE_LINK` one (Ferrum56's own bypass for the
identical tension, kept untouched and independent - reusing that macro
directly would have pulled in unrelated Rust-FFI code paths). Final
shape, after two real review rounds: exempts exactly `valve` (this
checkout's own title - Blue Shift/Op4 have their own separate SDK
checkouts, `deps/hlsdk-bshift`/`deps/hlsdk-opfor`, and need the
identical fix in their own copy when their own bring-up starts) by
full case-insensitive equality, not a prefix match (an earlier version
would also have silently accepted `valve_custom`, or `gearbox`/`bshift`
in this same checkout, which don't belong here). Verified live: read
the actual compiled machine code at the resolved runtime address in a
running boot, then re-tested the full boot - the `quit`-at-~2.5s is
gone, and the boot now demonstrably reaches `R_Init()`'s real
renderer-selection code before hitting a new, later, separate,
well-understood gap (`video.cfg` needs `r_refdll "null"` for this
milestone's headless renderer). See divergence #44.

### Fixed — tools/xemu-debug.py: a killed continue/step desyncs the next connection

Found during X7.4 step 2's first real combined server+client+freevgui
boot attempt. Killing the tool while it waited on a `c`/`s` stop-reply
(e.g. an external `timeout` wrapper) closes the socket out from under it;
xemu's gdbstub responds to the disconnect by auto-halting the CPU and
queuing a stop-reply for the next connection, which then silently
consumes it instead of its own `qSupported` handshake reply - the real
reply arrives one packet late and surfaces as an unrelated
`<unexpected packet: ...>` later on, making a live target look broken
when the tool's own prior invocation caused the desync.

`GdbRemote.__init__` now runs a real handshake: send `qSupported`, then
keep discarding and acking (an unacked packet can make a real stub
withhold whatever it queues next) anything that isn't actually the
`qSupported` reply - told apart by shape, not guessed from a fixed delay
- until the real reply arrives or a bounded deadline is hit.
`cont()`/`step()` raise a clear error on a local read timeout instead of
an uncaught `socket.timeout` traceback, explaining that the `c`/`s` was
already sent and is not undone. Covered by `tools/test_xemu_debug.py`'s
`HandshakeSynchronization` (a loopback `socketpair()`, no emulator
needed; a stale packet, a real reply arriving after a real delay, and no
reply ever arriving are each exercised directly). See
`docs/r3d/lessons.md` for the full account, including the
real (still open) finding this fix made possible: the boot reaches a
genuinely alive, non-crashed state but does not reach `Host_Frame` within
150+ real seconds - the CPU spends that time in the kernel's own
idle/ready-queue-poll loop, consistent with a thread blocked on an
unsignaled kernel wait somewhere earlier in boot.

### Added — real __RTDynamicCast: combined server+client+freevgui link reaches 0 duplicate, 0 undefined (divergence #43)

The last of the 527 originally-undefined VGUI1 symbols (divergence #42),
`___RTDynamicCast`, was previously left parked, on the assumption that
implementing real MSVC RTTI was "real, separate, low-level ABI work" -
the same reasoning divergence #37 used to avoid it for the server SDK.
Checked directly rather than re-assumed: clang already emits a complete,
correct set of MSVC-ABI RTTI type descriptors for every class in this
build - what nxdk's toolchain actually lacks is only the runtime walker
function, a categorically smaller problem.

`compat/xbox/msvc_rtti_dynamic_cast.cpp` is a real, clean-room
`__RTDynamicCast`, researched from public documentation (not transcribed
from Microsoft's own proprietary CRT source), deliberately scoped to the
non-virtual inheritance this codebase's real call sites actually need
(virtual inheritance and reference-cast throws are honestly left
unimplemented - verified unreachable by anything that exists today).

Verified against real, ground-truth Microsoft Visual C++ (`cl.exe`,
32-bit): a test harness compiled the exact same implementation alongside
test classes matching freevgui's real shapes and compared results
directly against real `dynamic_cast`, including a genuine nonzero-offset
multiple-inheritance cross-cast - all 6 cases byte-identical.

With it, the real combined server+client+freevgui link now reaches
**0 duplicate, 0 undefined** for the first time ever - X7.4 step 2's real
link-only exit gate. See divergence #43.

### Added — real VGUI1 via freevgui: 527 undefined VGUI1 symbols down to 1 (divergence #42)

Divergence #41's "the fix is populating `3rdparty/vgui_support`" was
wrong - checked out directly, found headers-only plus prebuilt binaries,
same as `deps/hlsdk/utils/vgui`. The real fix, vendored here:
`3rdparty/freevgui`, FWGS's real, buildable-from-source, clean-room VGUI1
reimplementation, public-API-compatible with Valve's original headers and
shipping a static-link backend purpose-built for this exact engine.

`tools/xbox-freevgui-sweep.sh`: 34/38 clean on the first real attempt,
39/39 after two missing include paths and one real upstream API-drift bug
(`vguiapi_t::SetPaintOffset`, a field this fork's vendored `engine/
vgui_api.h` predated relative to upstream Xash3D-FWGS - added, with a
correct, documented no-op implementation since no reference renderer this
fork has does real 2D drawing yet).

With `--include-freevgui` added to `tools/xbox-game-link-check.sh`, the
real combined server+client+freevgui link went from 527 undefined to 4
(three missing `vgui::App` overrides, fixed with a new file,
`compat/xbox/freevgui_app.cpp`, that does not touch the vendored
submodule at all), then to 0 duplicate, 1 undefined. The one remaining
symbol, `___RTDynamicCast`, is deliberately left as a tracked, deferred
gap rather than fixed via divergence #37's own RTTI-avoidance technique -
that technique is not safely portable to freevgui's `Panel`/`LayoutInfo`,
which deliberately bit-match Valve's original vtable layout for ABI
compatibility with `cl_dll`'s already-compiled objects. Parked as
`docs/r3d/parked-game-freevgui-undefined-symbols.txt`. See divergence #42.

### Fixed — first real combined server+client link: 0 duplicate, 527 undefined (all VGUI1, now tracked) (divergence #41)

The first real link of the real, isolated `hl` server (107 objects) and
real, isolated `hl` client (85 objects) together, ever, in this
project's history (`tools/xbox-game-link-check.sh hl --include-client`,
new flag added this change) hit a genuine duplicate-symbol collision
that only exists once both real modules are present: the engine's own
`IN_ActivateMouse`/`IN_DeactivateMouse`/`IN_MouseEvent`
(`engine/client/input/input.c`) share a name with the client SDK's own
required, protocol-mandated exports of the same name. Harmless on every
other platform (a separate DLL per module); a real duplicate-symbol
error on Xbox's one static binary.

Fixed by renaming only the engine's own linker symbol
(`asm("Host_IN_ActivateMouse")` etc., `#if XASH_XBOX`-guarded in
`engine/client/input.h`) - the client SDK's identical names stay exactly
as-is, since they are the ones a real `COM_GetProcAddress`-style lookup
resolves by name. Zero call-site changes: none of the 5 real callers
take the function's address or look it up by string. Verified directly
with `llvm-nm` on the compiled objects that the engine's real definition
and every real caller agree on the renamed symbol.

With that fixed, the real combined link now reaches **0 duplicate** for
the first time, surfacing **527 undefined symbols, all VGUI1**
(`vgui::Panel`/`Scheme`/`Color`/`Font`/etc.) - `deps/hlsdk/utils/vgui` is
headers-only plus a prebuilt win32 `.lib`, no buildable source anywhere
in this vendored SDK. The real, portable fix is populating
`3rdparty/vgui_support` (an FWGS submodule already declared in
`.gitmodules`, never checked out) - real, separate future work, tracked
as the next concrete blocker for X7.4 step 2 rather than rushed here.
Parked as a new, real, measured baseline: `docs/r3d/parked-game-
undefined-symbols.txt`. See divergence #41.

### Fixed — real `hl` client SDK: 85/85, zero failures - the real, full `hl` client SDK compiles and isolates completely clean for the first time (divergence #40)

The prior entry's `<memory_resource>` categorisation ("genuinely
separate, larger work") was wrong - found running the real client
isolation for the first time (`tools/xbox-game-isolate.sh hl client`).
`cl_dll/cdll_int.cpp`, the client module's own main entry point
providing every REQUIRED export the engine's `COM_GetProcAddress`
lookups hard-need (`HUD_Init`/`HUD_VidInit`/`HUD_Redraw`/21 more),
transitively but genuinely includes `CMiniMem.h` (it really uses
`IParticleMan`, not an incidental pull-in) - a real isolation attempt
without a working `std::pmr` reported all 22 required exports entirely
missing, a real blocker for X7.4 step 2, not a deferrable want.

Fixed with `compat/xbox/memory_resource` (new file) - not a full,
standards-compliant `std::pmr` (real thread-safety options,
`upstream_resource` chaining, chunked-pool allocation performance remain
genuinely deferred), but a correct, minimal implementation of the exact
three methods (`allocate`/`deallocate`/`release`) `CMiniMem` actually
calls (the ONLY consumer of `std::pmr::` anywhere in the client SDK,
verified by grep), as a tracked list of individually `::operator new`'d,
correctly-aligned blocks.

**Codex review round 1: the remaining `GL/gl.h` failure (`particleman/
CFrustum.cpp`) was NOT safely dismissable as "non-required" either.**
Two other real, compiled files (`IParticleMan_Active.cpp`,
`CBaseParticle.cpp`) genuinely call `CFrustum`'s own methods - a real
link without it would fail on 4 undefined symbols the isolation tool's
own required-exports check does not catch. Reading `CFrustum.cpp` itself
found the "needs real rendering, X5's job" framing was also wrong: its
own header comment says it includes `<GL/gl.h>` only for two symbolic
constants (`GL_PROJECTION_MATRIX`/`GL_MODELVIEW_MATRIX`), passed as
plain `int`s to the SDK's own `GetMatrix()` - no real GL function is
ever called. Fixed with two `#define`s (`compat/xbox/GL/gl.h`, values
cross-checked against this repo's own vendored SDL2 header and the real
`ref/gl` reference renderer's own header, not guessed).

Verified end to end: the real `hl` client sweep now reports **85/85,
zero failures** (was 70/85 at the start of this investigation), zero
regressions in the real server sweep (107/107) or `3rdparty/mainui`
(82/82). A fresh real client isolation now reports "isolated cleanly -
85 object(s), 5657 symbol(s) renamed, 0 leaked" - genuinely, fully
link-ready. X7.4 step 2 (the real client link attempt) is now genuinely
attemptable for the first time.

**Codex review round 2**: an earlier version of `compat/xbox/
memory_resource` tracked blocks in a plain `std::vector`, searching
linearly on every `deallocate()` - Theta(N) per call, so retiring N
particles in one frame (`CMiniMem::Reset`, called on every level
transition) cost Theta(N^2) bookkeeping on top of the real frees,
exactly the "quadratic teardown" class of bug `CMiniMem`'s own design
exists to avoid. Fixed: tracked in a `std::unordered_map` keyed by
pointer instead, making both `allocate()`/`deallocate()` amortised
O(1) - closes the specific algorithmic complexity named, without a real
chunked/free-list pool or real-hardware profiling, both still
genuinely deferred, larger work (see `compat/xbox/memory_resource`'s
own header comment). Re-verified: identical 85/85 sweep and clean
isolation results.

### Fixed — real `hl` client SDK compile failures: 70/85 to 77/85, five real root causes (divergence #39)

Building the real `hl` client SDK for the first time
(`tools/xbox-game-sweep.sh hl client`) hit real compile failures, all
downstream of one fact measured directly: this project's own clang
invocation (`-target i386-pc-win32`) defines `_MSC_VER` (`1950`), which
activates `deps/hlsdk/external/SDL2`'s own `#if defined(_MSC_VER)`
real-MSVC branches that were never reachable before.

Fixed: SDL2's SAL-annotation macros (`_Out_bytecap_`, `_In_bytecount_`,
etc. - `compat/xbox/sal.h` extended with the specific ones real headers
invoke, all no-ops, matching this file's own stated design); a missing
legacy `<memory.h>` (three files want only `memcpy` from it -
`compat/xbox/memory.h`, new, redirects to `<string.h>`); and
`SystemParametersInfo` (`inputw32.cpp`'s real mouse-acceleration
save/restore around raw mouse look) - never declared or implemented on
this platform at all, given an unconditional `FALSE` body, the real,
correct behaviour since Xbox has no OS-level mouse-acceleration setting
to query or set, not a stub for missing functionality (confirmed safe:
the caller already checks this return value and skips the save/restore
on `FALSE`). `SPI_GETMOUSE`/`SPI_SETMOUSE` verified against the real
Windows 10 SDK's `WinUser.h`, not guessed.

Two more - a `uintptr_t` redefinition conflict between SDL2's own
Windows config and pdclib's real one, and a `_m_prefetch` builtin
collision between SDL2's own clang workaround and clang's real builtin -
were first fixed as global compiler flags
(`-D_UINTPTR_T_DEFINED`/`-D__PRFCHWINTRIN_H`), then each corrected in
turn by Codex review (round 1 for `_m_prefetch`, round 2 for `uintptr_t`
once it noticed the first fix hadn't been held to the same standard): a
flag predefining a guard macro affects every translation unit
project-wide, not just the one vendored block it was meant for - wrong
for a guard name clang's own real `<intrin.h>` also uses, and risky in
general even for an SDL2-specific one. Fixed at the source instead, on
`deps/hlsdk`'s `resonance3d` branch: `SDL_config_windows.h`'s inner
`uintptr_t` block and `SDL_endian.h`/`SDL_cpuinfo.h`'s `_m_prefetch`
blocks each gain a narrow `&& !defined(NXDK)` on the specific guarded
block responsible, not the whole surrounding `_MSC_VER` branch -
confirmed correct rather than just convenient for `SDL_cpuinfo.h`
specifically, since its same block also unconditionally `#define`s
`__SSE2__`/`__SSE3__`, which the real Xbox CPU never had (clang's own
`-march=pentium3` already predefines the accurate feature set).

Every fix is now a real `compat/xbox/` or vendored-SDL2 source addition
(divergence #39) - no global compiler flags remain.

Verified end to end: the real `hl` client sweep went from 70/85 to
77/85, zero regressions in the real server sweep (107/107) or
`3rdparty/mainui` (82/82). The remaining 8 failures are individually
confirmed (each compiled standalone) to be exactly two already-tracked,
genuinely separate gaps: `<memory_resource>` (7 files -
`std::pmr::unsynchronized_pool_resource` needs a real implementation
nxdk's `libc++.lib` does not have at all, checked directly via
`llvm-nm`) and `GL/gl.h` (1 file, the pre-existing no-real-GL-on-Xbox
gap).

### Confirmed — X7.4 step 1 complete: the real `hl` server module's boot milestone verified by a real breakpoint hit

The entry below left one thing open: whether the boot actually reaches
`CL_Init`'s expected "missing essential exports" failure, not just
"no longer crashing on the same two bugs." The network log channel -
the tool originally meant to answer this - produced no output at all,
indistinguishable from its own already-documented pre-existing
unreliability.

Two real gaps found by Codex review before this was actually rigorous.
Round 2 (on the `continue`-timeout approach): a timeout with no stop
reply only disproves the guest re-trapping the same bug, not that it is
"healthy." Round 1 (on the fix for that: a bare breakpoint at
`Sys_Error`): hitting it only proves SOME call reached that shared
fatal-error sink, not which one - `Sys_Error` has real callers before
`CL_Init` ever runs. Fixed properly: read `Sys_Error`'s own stack at
that same breakpoint hit (`tools/xemu-debug.py break 0xff180`, cdecl
entry, before any prologue clobbers it) - the return address resolves
(fresh linker map) to `Host_Error + 0x63`, and the format string plus
its two varargs, read and dereferenced directly off guest memory,
assemble to the exact expected text: `Host_ErrorInit: can't initialize
client: missing essential exports`, word for word, not inferred from
the symptom.

A third gap found before this reached Codex review at all: decoding
`CL_Init`'s failure alone doesn't prove `SV_LoadProgs` succeeded first -
its own caller discards that return value and calls `CL_Init`
regardless either way. Closed the same way aimed at the real SDK's own
`GetEntityAPI2` instead of the shared sink: a breakpoint there hit with
its caller resolving to `SV_LoadProgs + 0x384`, and (via a second
breakpoint at that captured return address, needed because
`GetEntityAPI2` calls `memcpy` internally so a fixed single-step count
undershot the real return) its actual return value confirmed `EAX = 1`,
true. `SV_LoadProgs` really did complete successfully against the real
`GetEntityAPI2`, and `CL_Init` really did then reach and fail its
client-module load exactly as expected (the client module is still
`stub_game/client`, X7.4 step 2's own job).

Also shows the network log channel is silent even for a message
definitely, verifiably formed and about to be sent - real, independent
evidence the channel's unreliability isn't specific to any one code
path, documented in `docs/r3d/lessons.md` alongside the corrected
breakpoint-plus-stack-decode technique for reuse on future milestones.

### Added — `tools/xbox-game-link-check.sh --xbe`, the real `hl` server module's first real boot attempt

`tools/xbox-game-link-check.sh` gained a `--xbe <path>` mode, mirroring
`tools/xbox-link-check.sh`'s own `-force:unresolved` + `cxbe` sequence
against this tool's own isolated-server response file, producing a real,
`XBEH`-valid `.xbe` for the actual, real `hl` server module for the first
time - packaged onto a real ISO (`extract-xiso`, the real retail `valve/`
tree so `delta.lst` exists) and booted in xemu.

The first real boot attempt crashed immediately, twice in a row - not
the expected "CL_Init: missing essential exports" message at all:
`assert(nullptr != g_pFileSystemModule)` in `FileSystem_LoadFileSystem`
(`deps/hlsdk/game_shared/filesystem_utils.cpp:197`), decoded straight
from the halted guest's own memory rather than inferred from the
symptom. `Sys_LoadModule`'s stock body is a real `LoadLibrary()` call,
which NXDK has never had. A first fix (route `Sys_GetFactory` to this
same file's own local `CreateInterface`) compiled and linked clean but
was itself wrong - caught by a second real boot, not by inspection:
X7.1b's own isolation renames every self-reference to `CreateInterface`
within the server's own isolated set, so that fix always resolved to the
server's own unrelated local registry, not filesystem_stdio's real one.
Fixed properly on `deps/hlsdk`'s `resonance3d` branch (divergence #38)
by reusing the exact mechanism the engine itself already uses
successfully for this (`COM_LoadLibrary`/`COM_GetProcAddress`, proven
since divergence #31).

Re-verified: full server sweep still 107/107, still `0 duplicate, 0
undefined`. A real xemu boot of the fixed XBE is no longer repeatedly
re-trapping the same two asserts the pre-fix build hit every time within
seconds - confirmed by attaching the debugger and issuing a genuine
`continue`, which produced no stop reply for 90+ seconds. That alone
does not mean the boot is healthy or making real progress (an infinite
loop or a legitimate kernel idle-wait looks identical to "no stop
reply") - it only rules out the specific crash this row fixes recurring,
found and corrected via Codex review round 2 on an earlier, overclaiming
version of this same entry. Also found and documented
(`docs/r3d/lessons.md`): `tools/xemu-debug.py`'s `regs`/`mon` reads show
a fixed debugger-attach checkpoint rather than the guest's true current
state while it is genuinely running.

Not yet confirmed: the boot reaching its full expected milestone (the
"CL_Init: missing essential exports" message arriving on the network log
channel) - the channel produced no output at all this session, matching
the same pre-existing, already-documented unreliability from an earlier
session rather than a new symptom this fix introduced.

### Fixed — real `hl` server SDK now links `0 duplicate, 0 undefined`: the `___RTDynamicCast` gap closed (divergence #37)

The prior entry below tracked one real, understood gap as a baseline
(`--expect-undefined 1`): `___RTDynamicCast`, the MSVC-ABI RTTI runtime
function `dynamic_cast` lowers to, absent from nxdk's entire toolchain
(confirmed via `llvm-nm` against every prebuilt `.lib`). Closed the same
day rather than left open - implementing real MSVC-ABI RTTI from scratch
would have been genuine low-level ABI work, but this SDK already has an
established, in-use idiom for exactly this problem: `CBaseEntity::
MyMonsterPointer()`/`MyTogglePointer()`/`MySquadMonsterPointer()`, a
virtual defaulting to `NULL`, overridden to `return this` only in the
target type - an RTTI-free safe downcast, valid wherever the class
hierarchy is single and non-virtual (confirmed true for both affected
types by checking every `dlls/*.h` class declaration directly).

Extended that pattern with two more virtuals on `CBaseEntity` -
`MyPlayerPointer()` (new, overridden in `CBasePlayer`) and
`GetWeaponPtr()` (already existed on `CBasePlayerItem`, pulled up one
level so it is reachable from a bare `CBaseEntity*`) - and replaced all
6 real `dynamic_cast` call sites in `dlls/client.cpp`/`player.cpp` with
them, preserving each site's original null-safety around
`CBaseEntity::Instance()` explicitly (a raw virtual call needs that
where `dynamic_cast` on a null pointer handled it for free).

**Codex review round 1: the first version was unconditional, changing
`CBaseEntity`'s vtable layout and all 6 call sites' behaviour on every
platform this SDK builds for, not just Xbox - a real finding, since
`deps/hlsdk`'s `resonance3d` branch is not Xbox-exclusive and
`dynamic_cast` works fine everywhere else.** Fixed: every addition and
call-site edit now sits behind `#if defined(NXDK)`, with the `#else` arm
restoring the exact original `dynamic_cast` code. Verified by diffing
against the pre-fix commit: every non-NXDK line is byte-identical to
what was there before, confirming zero behavioural change on any other
platform (this repository's own tooling has no PC build path for
`deps/hlsdk` yet, so this diff-identity check is the real verification
available). Re-verified the NXDK path is unaffected by the guards: full
server sweep still 107/107, `0 duplicate, 0 undefined` unchanged.

Verified end to end: full server sweep still 107/107, and
`tools/xbox-game-link-check.sh hl --expect-undefined 0` now reports
**`0 duplicate, 0 undefined`** - a real, valid executable produced for
the first time for the real `hl` server SDK (`nxdk-link` only ever
produces one once nothing remains unresolved). Both
`docs/r3d/parked-undefined-symbols.txt` and the new
`docs/r3d/parked-server-undefined-symbols.txt` are now genuinely empty -
no tracked, accepted undefined-symbol gap remains anywhere in this link.

### Added — `tools/xbox-game-link-check.sh`: the real `hl` server module linked against the real engine for the first time (X7.4 step 1)

Reuses `tools/xbox-link-check.sh`'s response file, `tools/xbox-game-isolate.sh`'s
fresh isolation, and `tools/gen-library-tables.sh`'s generation mechanism:
verifies the baseline engine link is clean, isolates the `hl` server
module, swaps its two stub-server objects for the isolated set plus a
freshly generated `generated_lib_server_exports.c`, and links directly
with `nxdk-link`.

Getting a trustworthy first result took fixing three real bugs, not a
clean first run:

- `tools/xbox-link-check.sh`'s mainui-manifest `sed` extraction came back
  empty right after 186 third-party compiles, reproducibly - a retry loop
  did not fix it, but the script's own `cat` on the same file, moments
  later in the same failing run, always succeeded. Fixed both `sed` call
  sites (`FAIL=`/`TOTAL=`, and a separate `OK=` further down) by reading
  the manifest once with `cat` and extracting with bash `[[ =~ ]]` regex
  instead.
- `eval "$(deps/nxdk/bin/activate -s)"` measurably replaces `$PATH`
  rather than extending it on this Git-Bash/MSYS2 setup, breaking
  `nxdk-link`'s ability to find `lld`. Removed the call from the new
  tool, matching `xbox-link-check.sh`'s own proven approach of never
  calling it.
- The first real link reported one duplicate symbol, `CreateInterface` -
  a genuine `/EXPORT:` in the server target's own `public/interface.cpp`
  (divergence #35/#36), correctly kept by X7.1b's literal "every real
  `/EXPORT:`" rule, but colliding with `filesystem_stdio`'s own
  `CreateInterface` (divergence #30) once both share a link; nothing else
  in the server target references it. Added `tools/xbox-gen-exports.sh
  --exclude <file>` (mirroring the existing `--allow`/`--require`
  design) and `tools/xbox-server-exclude-exports.txt` (one line,
  `CreateInterface`), wired into `xbox-game-isolate.sh`'s server case.

Final, current result: **0 duplicate symbols, 1 real undefined symbol**
(`___RTDynamicCast`, the MSVC-ABI RTTI runtime function `dynamic_cast`
lowers to). Confirmed real and needed: 6 genuine downcast call sites in
`deps/hlsdk/dlls/client.cpp`/`player.cpp`; confirmed absent from nxdk's
entire toolchain via `llvm-nm` against every prebuilt `.lib`. Deliberately
not attempted here - implementing MSVC-ABI RTTI or replacing the SDK's
`dynamic_cast` call sites are both real, separate investigations.
Tracked honestly as the new baseline: `tools/xbox-game-link-check.sh hl
--expect-undefined 1`.

Also corrected `docs/r3d/parked-undefined-symbols.txt`: the 31 tracked
`Netchan_*`/`SteamBroker_*` names were stale (the real current baseline
for the base engine link is 0 undefined, not 32) - emptied to reflect
the real, current measurement.

**Codex review round 1 found two more real gaps.** (1) `--expect-undefined`
only compared a COUNT, the exact same-count-substitution failure class
`tools/xbox-link-check.sh`'s own `parked-undefined-symbols.txt` identity
check already exists to close, but that check was never carried into
the new tool - fixed with a server-specific
`docs/r3d/parked-server-undefined-symbols.txt` (`___RTDynamicCast`) and
the same exact-set diff. (2) the script printed its output executable's
path unconditionally, but `nxdk-link -errorlimit:0` never produces a
`.exe` while any undefined symbol remains (confirmed: the file genuinely
does not exist after this baseline's accepted run) - fixed to report a
real path only when the file actually exists, and to report a
measurement-only pass otherwise; softened the header's "real XBE" claim
to describe what this tool actually does. Re-verified after both fixes:
same `0 duplicate, 1 undefined` result, exact-set match confirmed, and
the "no .exe produced" message now prints correctly instead of a
fabricated path.

### Fixed — real `hl` SDK compile failures: the server module now compiles 107/107 (X7.4)

Two real root causes, found while running `tools/xbox-game-sweep.sh`
against the whole SDK. `tools/xbox-cxx.sh` never passed `-std=c++17` -
measured directly that this project's exact target/flag combination
defaults to C++14, not the C++17 the HL SDK's own build actually
requires, so class template argument deduction and `std::clamp` were
hard errors. Added `-std=c++17` to `xbox-cxx.sh`'s fixed flags (a
caller's own `-std=` still wins); fixed 4 of 5 server failures and 2 of
18 client ones outright. Re-verified `3rdparty/mainui`'s existing sweep
(82/82 clean) is unaffected.

The remaining server failure, `filesystem_utils.cpp`, was never really
a `_stat64i32` naming mismatch - nxdk provides no working `stat()`/
`fstat()` at all (`deps/nxdk/lib/xboxrt/libc_extensions/stat.c`'s
entire body is `#if 0`'d out). Fixed on `deps/hlsdk`'s `resonance3d`
branch (divergence #36) with the same safe `return 0` fallback the
`_STATIC_ENGINE_LINK` branch already uses.

**Two Codex review rounds, each catching the opposite mistake for the
same underlying limitation - nxdk has no real timestamp source at
all.** Round 1: the inherited fallback's claim was false - returning
`0` from `FileSystem_GetFileTime` for both the `.bsp` and the `.nod`
made `FileSystem_CompareFileTime` see them as EQUAL and report success,
and `CGraph::CheckNODFile` only rebuilds on `iCompare > 0` or a failed
compare, so equal timestamps silently kept a stale `.nod` cache -
exactly backwards from the intended behaviour. Fixed by making
`FileSystem_CompareFileTime` return `false` unconditionally on `NXDK`.
Round 2, on that fix: `false` unconditionally makes `CheckNODFile`
reject EVERY graph, even a genuinely valid, freshly-packaged one -
`world.cpp` never even calls `FLoadGraph()`, forcing a full node/routing
regeneration on every single map load. Resolution: removed the
`FileSystem_CompareFileTime` branch entirely - its original,
unconditional logic already does the right thing once `GetFileTime`
alone is fixed (reads as "equal", proceeds to `FLoadGraph()`), and that
function's own independent checks (missing file, wrong version) are the
real gate. True `.nod`/`.bsp` staleness is not caught at Xbox runtime by
this - deferred to asset-packaging time, a real, deliberately-open gap.
Re-verified after both rounds: still 0 errors, server still 107/107, and
`CheckNODFile` hand-traced against the final code to confirm it
proceeds to `FLoadGraph()` rather than rejecting everything.

Server now compiles 107/107 - X7.4 step 1's real, complete object set
exists for the first time. Client: 15/85 remaining, all the
already-known SDL2-header/clang incompatibility or studio-model
rendering headers X5 replaces anyway - separate, larger work.

Also found again while diagnosing this: `xbox-game-sweep.sh`'s
persistent-fd log fix reduces but does not eliminate occasional marker
loss under this Windows/MSYS setup (a fresh run dropped 1 of 15 FAIL
markers). Worked around for this diagnosis by checking `.obj` existence
directly rather than trusting the log; a fully robust fix (per-source
log files) remains real, open follow-up work.

### Fixed — `tools/xbox-game-isolate.sh`: a partial object set was published indistinguishably from a complete one (Codex review round 2)

A run against a known-partial object set (the server's own tracked
5-compile-failure baseline, accepted via `--expect-fail`) published
`manifest.txt` with only object/symbol counts and said "isolated
cleanly" regardless - nothing in the artifact itself let a future
consumer tell a probe apart from a genuinely complete, link-ready
module. Rather than refuse to publish anything for a partial run,
carries the sweep's real `OK`/`FAIL`/`TOTAL` through to a new
`SWEEP_OK`/`SWEEP_FAIL`/`SWEEP_TOTAL`/`COMPLETE yes|no` block in this
tool's own manifest, and the terminal message now reads differently for
a partial run ("isolated a PARTIAL object set... NOT link-ready").
Verified against the real server baseline: `manifest.txt` now correctly
reads `COMPLETE no` with the real fail/total counts.

### Fixed — `tools/xbox-game-isolate.sh`: a hard-required server export was silently renamed away (Codex review round 1)

`exports.txt`'s `GiveFnptrsToDll _GiveFnptrsToDll@8` line has a real
subtlety the allowlist extraction missed: column one is not a real
symbol in the object at all (only the stdcall-decorated form exists),
so keying the allowlist on it kept a name nothing defines while the
real symbol's bare form matched nothing and was silently renamed away -
breaking the one export the engine hard-requires to load the server
module. The existing leak-check couldn't catch this: it and the rename
decision both trust the same allowlist file, so a bug in building that
file passes both self-consistently. Fixed two ways: the allowlist
extraction now uses the asm-symbol column's bare form when present; and
a genuinely independent check, never touching that file, re-reads the
required-exports source of truth directly and searches the real
post-rename symbol table for either accepted form. Verified end to end:
`_GiveFnptrsToDll@8` now confirmed present, unrenamed, after isolation.

### Added — `tools/xbox-game-isolate.sh`: the real bulk module symbol isolation pipeline (X7.1b)

The bulk implementation of X7.1b's 2-object prototype: runs the target's
sweep, generates `exports.txt`, strips `.drectve` from a copy of every
object, builds a rename map from the module's own defined-symbol set
minus its allowlist, applies it with `llvm-objcopy --redefine-syms`, and
self-checks every isolated object afterward for a leaked (neither
allowlisted nor prefixed) symbol. Server isolates cleanly: 102 objects,
9,349 defined symbols, 240 kept, 9,109 renamed `sv_`, 0 leaked. Client
correctly refuses - not a bug in this tool: the 18 already-known client
compile failures include the file defining most required `HUD_*` entry
points, so `exports.txt`'s own completeness gate rejects it; fixing
those (X7.3/X7.4) unblocks this automatically.

Found and fixed two real bugs while running this against ~100+ real
objects, both the same class already hit once this session in
`tools/xbox-game-sweep.sh`'s log: (1) a file written early and read back
only after ~200 external process calls came back missing, traced to
`mktemp -d`'s default system-temp location being unreliable across that
much intervening I/O on this Windows/MSYS setup - fixed by rooting the
working directory under `$SCRATCH` instead. (2) the same repeated-`>>`-
reopen pattern already found once, this time building the module's
defined-symbol list across ~100 objects, undercounted 9,349 real symbols
as 606 and let one genuinely leak through unrenamed - fixed identically,
one persistent file descriptor per accumulation loop. Both traps
recorded in `docs/r3d/lessons.md` for future `tools/xbox-*.sh` work at
this scale.

### Fixed — `tools/xbox-game-sweep.sh`: a rejected run could still publish a clean-looking manifest.txt (Codex review round 2)

`OK/FAIL/TOTAL` was written right after the zero-attempted check but
before the `--expect-total`/`--expect-fail` gates ran - so a run one of
those gates rejected (a source-set drift, say) still published a
clean-looking `manifest.txt` before exiting nonzero, the same
stale-output shape already fixed once in `tools/xbox-game-manifest.sh`
itself. `manifest.txt` is now written only after every applicable gate
has passed. Verified: a deliberately wrong `--expect-total` against the
real server sweep fails and leaves no `manifest.txt` (a pre-existing one
from an earlier accepted run confirmed gone); the correct baseline
re-run afterward still publishes it correctly.

### Fixed — `tools/xbox-game-sweep.sh`: a bare run could exit 0 with every compile failing (Codex review round 1)

Two real gaps. (1) `--expect-fail` is optional, but a bare invocation
with no flags at all silently exited 0 even if every source failed to
compile - a broken toolchain would look identical, to anything checking
only the exit code, to a clean run. Any failure without an explicit
`--expect-fail` acknowledging it is now fatal by default. (2) the
stale-`.obj` cleanup ignored its own exit status, so a genuine removal
failure could let the script compile ahead as if the output directory
were already clean, with a stale object surviving into the current
run's advertised output. The cleanup's exit status is now checked, and
the directory is re-verified empty of `.obj` files afterward. Both
targets re-verified against their real baselines after the fix.

### Added — `tools/xbox-game-sweep.sh`: the real `hl` SDK compile measurement (X7.4)

The compile step `tools/xbox-game-manifest.sh` exists to feed, same
shape as `tools/xbox-mainui-sweep.sh` (OK/FAIL/TOTAL,
`--expect-fail`/`--expect-total` regression gates, a zero-attempted hard
failure) but reading its TU list/defines/includes from the manifest
instead of a hand-maintained glob. First real result, not a three-file
prototype: server 102/107 compile clean, client 67/85. Failures
categorised and left for X7.4/X7.3 (not this tool's scope): a real
Win32-vs-pdclib struct mismatch in `nodes.cpp`, missing `std::byte`/
`std::clamp` visibility under `-ffreestanding -nostdlib`, and (client)
mostly the same SDL2-header/clang incompatibility the manifest tool's
`INCLUDE` work already flagged, plus a few real-`GL/gl.h`/`<memory.h>`
wants from the studio-model renderer path X5 replaces anyway.

Found and fixed while running this for real: the error log silently
lost roughly half of the real failures' detail (though never the
count) by reopening the same file for append once per compile - over a
hundred open/close cycles on one file in quick succession lost data on
this Windows/MSYS setup, reproducibly. Fixed by opening the log once
before the loop and reusing that file descriptor for every compile.

### Fixed — `tools/xbox-game-manifest.sh`: a failed run could leave a stale manifest in place (Codex review round 2)

Every validation in this script ran before the final output write, and
the previous manifest was never removed - a run that failed midway left
a prior successful run's manifest sitting at the fixed path a downstream
sweep is designed to read from, so a later step could compile stale
inputs while believing regeneration had happened. Fixed: the output
path is resolved and any existing manifest removed immediately, before
any parsing begins; the final write goes to a temp file in the same
directory, renamed into place only once complete. Verified through the
real script: a valid manifest written, the source SDK then mangled to
force a failure, and the manifest from the first run confirmed gone
after the second run correctly fails.

### Fixed — `tools/xbox-game-manifest.sh`: missing transitive INTERFACE include directories (Codex review round 1)

`client` links `vgui` and `SDL2` (`add_library(... IMPORTED)` targets
`cl_dll/CMakeLists.txt` defines itself, each with its own INTERFACE
include directory) - `SDL2`'s (`deps/hlsdk/external`) was never listed
anywhere except via that transitive relationship, and `cdll_int.cpp`
unconditionally includes `<SDL2/SDL_messagebox.h>` from it. Fixed by
also scanning `target_link_libraries()` and pulling in any linked
name's own `target_include_directories(NAME INTERFACE ...)` block.
Fixed two more gaps found in the same pass: the CMAKE_CURRENT_SOURCE_DIR
variable now also resolves when used as a path prefix, not just a bare
whole token; and resolved include paths are normalized before deduping,
since the same real directory could otherwise appear twice under two
different spellings. Verified by compiling `cl_dll/cdll_int.cpp` with
the manifest's exact defines/includes: without the fix, `SDL2/SDL_messagebox.h`
fails `file not found`; with it, the header is found and compilation
proceeds into a separate, real, pre-existing problem (SDL2's vendored
headers use MSVC SAL annotations clang doesn't parse) - flagged, not
fixed here, out of scope for this tool.

### Added — `tools/xbox-game-manifest.sh`: derive include directories too, not just sources and defines (X7.1)

Same derivation discipline the defines fix already established, applied
before a sweep/compile step needs its own hand-typed `-I` list:
`target_include_directories()` read from both the per-target CMakeLists
and the shared `set_common_properties()`, with the two real CMake
variable references it contains (`${CMAKE_CURRENT_SOURCE_DIR}`,
`${CMAKE_BINARY_DIR}`) resolved deterministically rather than guessed -
`${CMAKE_BINARY_DIR}` resolves to nothing after confirming (not
assuming) that nothing in `deps/hlsdk` ever generates a header into a
binary directory any `.cpp` includes. New `INCLUDE` manifest directive.
Verified against the real files (server 6, client 10 include dirs, all
confirmed to exist), plus a new end-to-end negative test for an
unrecognized CMake variable reference.

### Fixed — `tools/xbox-game-manifest.sh`: a resolver failure inside a command substitution didn't actually fail the script (Codex review round 2)

`resolve_define`'s hard-error path (an unrecognized generator expression)
ran inside a command substitution feeding a here-string - under bash
that subshell's own `exit 1` doesn't fail the surrounding loop on its
own, so the failure would silently produce zero resolved defines for
that token and the script would carry on. Fixed by capturing the
resolver's output into a variable and checking its exit status
explicitly before iterating. Re-verified through the actual script
end-to-end (not the function in isolation): a scratch copy of the real
SDK tree with an unrecognized generator expression injected fails with
exit 1 and writes no manifest; the real `deps/hlsdk` re-run afterward
confirms no regression in the source/define counts.

### Added — `tools/xbox-game-manifest.sh`: per-title TU list and defines read from the SDK's own CMakeLists (X7.1)

Reads `target_sources()`/`target_compile_definitions()` straight out of
a title's `dlls/CMakeLists.txt`/`cl_dll/CMakeLists.txt` (comment-stripped,
paren-matched, never approximated with a glob - `cl_dll/*.cpp` is not
the real client TU set) and writes `$SCRATCH/game-<title>/<target>.manifest`.
Only `hl` is supported for now: measured that `deps/hlsdk-opfor` and
`deps/hlsdk-bshift` have no `CMakeLists.txt` at all, correcting an
earlier planning assumption that they would "add their own" - both
titles refuse outright with a clear reason rather than a guessed file
list. Verified against the real, current `deps/hlsdk` CMakeLists: server
107 sources/2 defines, client 85 sources/3 defines, every derived path
confirmed to exist on disk, counts independently cross-checked.

While building this, found and fixed three real problems in `deps/hlsdk`'s
`resonance3d` branch CMakeLists (unedited since the branch was cut from
`master`, X7.0): `_STATIC_ENGINE_LINK` was still unconditionally defined
for the server target (the exact macro this project must never define);
with it removed, `public/interface.cpp` needed restoring to the server's
sources (its stock filesystem-loading path calls symbols only that file
defines, and `master`'s CMakeLists drops it because its own
`_STATIC_ENGINE_LINK` path never needs it); and the client target was
missing `WIN32` in its defines, a fix the server side already had. Also
found, not yet fixed (X7.4 scope): with `_STATIC_ENGINE_LINK` correctly
undefined, `SV_InitServer()`'s stock filesystem load fails on Xbox
(`Sys_LoadModule` is a raw `LoadLibrary()`, and nxdk's has no dynamic
loading), and `GameDLLInit()` quits the game immediately as a result -
a more severe, earlier boot blocker than the previously-known writable-
rootdir gap. Recorded as divergence #35.

### Fixed — `tools/xbox-gen-exports.sh`: completeness gate could mark a missing required name as found (X7.2, Codex review round 2)

Two more real gaps, both in the `--require` completeness gate, found on
the round-2 re-review. (1) In `--allow` mode, the gate accepted a
required name found only in the object's raw, unfiltered export list -
not the actually-written, allow-filtered table - so a required callback
that is a genuine export of the object but missing from the allowlist
would pass the gate while never appearing in the generated file. Fixed:
the gate now checks only the table that actually gets written. (2) The
zero-entries check ran after the gate's stdcall/raw-symbol fallback had
already run, so an `--all` object set whose directive extraction found
nothing could still end up with a small, fallback-only "passing" table
(just the required names) while silently losing every real export.
Fixed: a new pre-fallback check refuses to proceed past zero base
entries (before `--require`'s fallback runs) unless `--allow-empty` is
passed. Both re-verified against real compiled objects with dedicated
negative tests, plus a full re-run of the prior round's tests.

### Fixed — `tools/xbox-gen-exports.sh`: object-inspection failures no longer swallowed, client completeness gate enforced (X7.2, Codex review round 1)

Two real gaps in the first version. (1) Both object-inspection loops
discarded `llvm-readobj`/`llvm-nm`'s own exit status behind
`2>/dev/null | ... || true`, so a crashed or unparseable object was
indistinguishable from "exports nothing" and could silently drop real
entity names from the table while the script still exited 0. Each
object's inspector call is now checked individually before its output is
processed; a failure names the tool and the offending object and exits
nonzero. (2) A generated table with zero entries was only ever a
warning, and the documented client invocation never passed `--require`,
leaving the client's 37 hard-required `cdll_exports` names with no
enforced floor. Zero entries is now a hard error by default (nothing is
written), with a new `--allow-empty` opt-out for a genuinely empty
module; a new `tools/xbox-client-required-exports.txt` (the 37
`cdll_exports` names) is now passed via `--require` on the documented
client invocation. All four behaviors (inspector-failure detection,
zero-entries-without-opt-out, zero-entries-with-opt-out, the existing
require-gate regression) re-verified against the real compiled
crossbow.cpp/h_export.cpp/cbase.cpp objects used throughout X7.1b/X7.2.

### Added — `tools/xbox-gen-exports.sh`: exports.txt generated from compiled objects, not typed by hand (X7.2)

The first real piece of X7's `tools/` implementation. Reads a module's
compiled objects and writes `tools/gen-library-tables.sh`'s input format:
`--all` mode (server) includes every unmangled real `/EXPORT:` name found
(the ~260 `LINK_ENTITY_TO_CLASS` entity names plus API exports, none of
it known in advance - directive #6, never a hand-written classname list);
`--allow <file>` mode (client) includes only names present in a given
allowlist, so the shared weapon sources `cl_dll` also compiles never leak
their entity classnames onto the client's public surface. `--require
<file>` validates a completeness gate (`A|B` lines mean "at least one of
these", matching `engine/server/sv_game.c:5235`'s own
`GetEntityAPI`/`GetEntityAPI2` either-or) and, in `--all` mode, adds a
required name found only via a stdcall-decorated symbol
(`gen-library-tables.sh` gains a backward-compatible optional second
column for exactly this - `name asm_symbol` - binding the generated
`extern` to that exact linker symbol via an asm-label override instead of
assuming cdecl). Two small, fixed input files ship with it,
`tools/xbox-server-required-exports.txt` and
`tools/xbox-client-allowed-exports.txt`, both literal transcriptions of
`sv_game.c`'s and `cl_game.c`'s own lookups (the latter corrects a
miscount from earlier X7 planning: `cdll_new_exports[]` has 11 entries,
not 12).

Verified against real SDK files, not synthetic ones, including the
required negative control: `crossbow.cpp` + `h_export.cpp` alone
(missing `GetEntityAPI`/`GetEntityAPI2`, which live in `cbase.cpp`)
correctly fails and writes nothing; with `cbase.cpp` added, writes
exactly the 4 API names (`GiveFnptrsToDll` with its asm override,
verified via the stdcall fallback) and the 3 real entity classnames, zero
of the many mangled C++ methods in the same objects. `--allow` mode
against the client-compiled `crossbow.cpp` and the real 48-name
allowlist: 0 entries, correctly, with a warning rather than a silent
empty success. Full chain proven end to end: the generated
`server/exports.txt` fed through the extended `gen-library-tables.sh`,
compiled, and linked against the three real SDK objects - `0` duplicate,
every name (including the asm-overridden one) resolved with no
undefined-symbol warnings.

### Measured — X7.1b's server/client symbol-isolation design: PASSED against the real 384-object client link

The prototype gate the X7 plan required before any other per-title work:
compiled `deps/hlsdk/dlls/crossbow.cpp` twice with `tools/xbox-cxx.sh`
(server defines, then `CLIENT_DLL`) and found 150 symbol names shared
between the two compiled objects - virtual methods, vtables/RTTI, and
plain global data (`g_engfuncs`, `gpGlobals`, each defined once per
module). Applied the per-module allowlist (server keeps its entity
classnames, client keeps nothing from this TU) via
`llvm-objcopy --redefine-syms`, then linked against
`tools/xbox-link-check.sh`'s OWN `.sweep/link-check-response.txt` - the
exact 384-object list (engine, all third-party, stub game, generated
tables) the real client XBE was just linked from, not a hand-assembled
approximation: two earlier attempts at "the real engine" were each
corrected by Codex review, the second one for missing 28 third-party
objects a filename-glob approach couldn't reach. Appending the crossbow
objects to a copy of that real response file and linking: `0` duplicate
against the full 384-object set; the same pair unrenamed fails with the
13 duplicates named (`_weapon_crossbow`, `_ammo_crossbow`, 11
`CCrossbow::*` methods) - the required negative control, unchanged
across all three attempts. Also corrected: renaming a symbol does NOT
update the `.drectve` section's `/EXPORT:` string for it, and that is
not harmless - a real, populated export table exists in every XBE this
project already links, and the renamed-but-un-stripped client object
produced export entries pointing at the linker's "unresolved" sentinel
RVA (`0xFFFF0000`) for names that no longer existed. Fixed by reading
each object's `/EXPORT:` directives (for `exports.txt`) BEFORE stripping
the whole `.drectve` section, before the rename - re-verified with
`llvm-readobj --coff-exports` on the TRUE full-object-set image: the 14
legitimate pre-existing exports (7 engine, 7 `MultiEmulator`) and zero
crossbow-related entries, zero sentinel RVAs. Full measurement in
`docs/r3d/fork-plan.md` § X7.1b.

The same review round found a second, unrelated gap in X7.2: one of the
four hard-required server API exports, `GiveFnptrsToDll`, cannot be
found by `/EXPORT:`-directive scanning at all - its defining file
(`h_export.cpp`) redefines `DLLEXPORT` as `__stdcall` on `WIN32` instead
of `__declspec(dllexport)`, so it emits no export directive, and its
real compiled linker symbol is the `__stdcall`-decorated
`_GiveFnptrsToDll@8`, not the plain `_GiveFnptrsToDll` a cdecl `extern`
declaration would look for - both measured directly by compiling the
real file. Fixed and verified end-to-end: the four API names are looked
up in the compiled objects' full symbol table (`llvm-nm`, not just
`--coff-directives`) in both cdecl and stdcall-decorated form, and a
stdcall match is bound via an explicit linker-name override
(`extern void GiveFnptrsToDll(void) asm("_GiveFnptrsToDll@8");`) rather
than guessed - a real link against the compiled `h_export.obj` resolves
it with zero related undefined-symbol warnings.

### Changed — fork-plan X7 rewritten as the per-title XBE plan

Half-Life, Opposing Force and Blue Shift each ship as their own
statically-linked XBE on their own XISO, as the retail Xbox discs were -
the only shape a platform with no dynamic loading allows. The plan settles
the SDK base (the vendored `halflife-updated` family, all three titles
from one author), keeps this engine's SDK adaptations on a `resonance3d`
branch per fork (the existing `master` heads carry the Rust port's
`_STATIC_ENGINE_LINK` work, which this engine must never define), and
specifies: an `XBOX_TITLE` selector through the sweep/link scripts,
export tables GENERATED from each compiled module's `/EXPORT:` directives
(every `LINK_ENTITY_TO_CLASS` classname is an exported symbol - 251 / 323
/ 254 per title - so a hand-written list is exactly the hardcoded
classname table the project forbids; measured on a real Xbox object that
`llvm-readobj --coff-directives` yields them exactly), VGUI stripped from
`cl_dll` on the `hlsdk-portable` `USE_VGUI` model, Half-Life first with
three boot milestones, then the two expansions, one XISO per title from
the Steam install. Reviewed twice by Codex and once by the Xbox port
specialist before merging, and reshaped by it: the decisive collision is
server-vs-client inside one XBE (`cl_dll` compiles the same weapon
sources under `CLIENT_DLL`), solved by renaming each module's non-public
symbols with `llvm-objcopy --redefine-syms` against an allowlist of what
the engine resolves for that module - measured on real Xbox objects,
prototyped on `crossbow.cpp` with a negative control before any other
X7 work; translation-unit sets come from each title's CMake, not a glob;
the title is a required argument, never an environment variable; a
writable rootdir (`-rodir`) is a bring-up blocker for the first
changelevel; and memory is measured at entry, where the engine's own
`.bss` (13.1 MB resident) is already the wall the SDK walks up to. Each
SDK fork now has a `resonance3d` branch, pinned in `.gitmodules`.

### Fixed — the Xbox client boot's silent hard freezes were NULL stub-module callbacks: `pfnGetHullBounds`/`pfnPM_Init` at load, `pfnSys_Error` on the fatal-error path (divergence #34)

The first client boot to get past renderer selection did not hang or lose
its log channel; it called through NULL function pointers the static stub
game module had never filled, and on Xbox each of those is a kernel
bugcheck (`KMODE_EXCEPTION_NOT_HANDLED` / `STATUS_ACCESS_VIOLATION`,
exception address `0x00000000`) followed by `cli; hlt` — a freeze
indistinguishable from a hang. Two of them, in order: `SV_LoadProgs` →
`SV_InitClientMove` calls `pfnGetHullBounds` (and then `pfnPM_Init`),
one call below the function the stub's original audit had read; and once
that is past, `CL_LoadProgs("client")` fails — correctly, no client
module exists at this phase — and `Host_Error` → `Sys_Error` →
`SV_SysError` → `pfnSys_Error()` (guarded only by `svgame.hInstance !=
NULL`) fires *before* `Sys_Print`, so "Host_ErrorInit: can't initialize
client: missing essential exports" was fully formatted on the faulting
thread's stack and never emitted anywhere. The stub now provides all
three (each matching the HL SDK's own behaviour, cited in place), and
`SV_SysError` returns early on a NULL member under `#if XASH_XBOX`, so the
same class of miss on the error path reports a message instead of
freezing the machine.

Diagnosed, not guessed: read straight off the halted CPU with the new
`tools/xemu-debug.py` (below) — each exception record, the trap frame's
pushed return address (`SV_InitClientMove+0x88`, a `call eax` with
`eax = 0`), the formatted message, and the `libs[]` table were all
inspected in place. Every earlier "the crash is in `COM_LoadLibrary`"
bisection step was really measuring whether `svgame.hInstance` was set,
which is what gates the NULL calls.

Verified on xemu, positively and negatively, with the same UDP listener
on the network log channel (divergence #13): the pre-fix builds produced
zero packets and the bugcheck register pair; the fixed build produced
`Delta_InitFields: couldn't load file delta.lst` once per boot cycle -
the first `Sys_Error` this port has ever actually reported - with the
CPU in the kernel's idle-wait, not halted. That message is the honest
next stop: `SV_LoadProgs` → `SV_InitClientMove` is now past, `Delta_Init`
(the very next call) needs `valve/delta.lst`, which the minimal test disc
(`gameinfo.txt`, `gfx.wad`, `video.cfg`) does not carry. With the retail
`valve/` tree on the disc the expected next stop is `CL_Init`'s
client-module load (seen on the pre-fix build's stack, not yet reached
on a fixed build), which cannot pass until a client module exists - per
the per-title plan, the first real Half-Life build (`deps/hlsdk`'s
`dlls/` + `cl_dll/` statically linked), not a generic stub.

### Added — `tools/xemu-debug.py`: live CPU/memory/GPU debugging of Xbox builds under xemu

A stdlib-only Python client for xemu's built-in GDB remote stub, plus a
passthrough to the QEMU monitor, so a build can be inspected without
driving xemu's Monitor window through simulated input and screenshots:
registers, virtual and physical memory dumps, stack, byte-pattern search,
symbol resolution against a linker `.map`, NV2A register dumps and a
framebuffer capture read through `PCRTC_START`. The search reports how
much memory it actually inspected and exits nonzero when it could not
read any of it - a scan that reads zero bytes must never look like a
clean "no matches" (found in Codex review round 1), and the overlap
carried between reads accumulates, so a pattern longer than the read
chunk is still found (round 2 - previously a match spanning three or
more reads was silently missed under a "complete" exit 0). Both failure
paths are covered by `tools/test_xemu_debug.py`
(`python -m unittest tools/test_xemu_debug.py`).
Also published standalone
as `github.com/mathiasdbo/xemu-debug-tools` for other projects and
agents. Documents the facts that cost real time to learn (all read from
the guest's own page tables via `info mem`/`info tlb`, not assumed): a
title's addresses are XBE-virtual (`0x10000`-based, equal to the map's
`Rva+Base`) and readable as such through the stub; the image is mapped
page-by-page into physical RAM (mostly at `+0xB0000` for this build, but
not uniformly - resolve through the page tables, never a constant); and
the kernel's `0x80000000+` alias covers only the kernel itself, so a
"cannot access memory" there is the alias ending, not evidence about the
title.

### Added — `ref/null` registered as a real, loadable Xbox renderer module (X5 bring-up)

`VID_Init()` had nothing to load at all: `R_Init()`'s renderer-selection
loop only auto-tries names gated by a compile-time `XASH_REF_*_ENABLED`
define, none of which exist for Xbox yet (no `ref/soft`/`ref/gl` port
written). Registered upstream's own `ref/null` (a real, complete
`ref_interface_t` of no-op stubs, written for headless client testing)
in the same static `COM_LoadLibrary` table menu/`filesystem_stdio`
already use - a new `ref_null:ref/null/exports.txt` entry threaded
through `tools/gen-library-tables.sh`/`tools/xbox-cc.sh`/
`tools/xbox-link-check.sh`, selected at runtime via `r_refdll "null"` in
a disc's `video.cfg` (no engine code changes needed for selection).
Genuinely useful bring-up on its own: lets the rest of the client boot
path (`SV_Init`, `CL_Init`, menu init, the frame loop) be verified
independently of the harder rasterizer-port problem, and proves the
registration mechanism a real renderer will need.

Verified: compiles for Xbox with zero warnings, `GetRefAPI` is a real
defined symbol, full client link stays 0 duplicate/0 undefined with
`ref_null` in the generated table. **Not verified at runtime this
session** - boot attempts stopped producing any network-log-channel
output at all partway through, for reasons not resolved (see
`docs/r3d/lessons.md`'s new entry); documented honestly rather than
claimed.

### Fixed — X0.5 confirmed on xemu (not yet real-hardware-verified): the client engine finds and mounts `valve/`

The network log channel (divergence #13) finally reached the host this
session - the earlier "Windows Firewall silently dropped it" block
documented in divergence #31 was a tooling bug, not a network one:
`XBOX_ENGINE_TESTS=1`, exported in this dev shell and passed to an
explicit `/c/msys64/usr/bin/bash script.sh` invocation (the form these
scripts' own usage comments document as canonical), never actually
reached the compiler - that specific `bash` binary resets its environment
on start and drops custom exports, silently, with the build reporting a
completely normal "96/96 compile" the whole time. See
`docs/r3d/lessons.md`'s new entry for the full reproduction; the fix is
invoking these scripts bare (`tools/xbox-cc.sh <args>`, relying on their
own shebang and their own internal `PATH` fix-up) instead.

With the log channel genuinely live, a real xemu boot found two more real
bugs in `compat/xbox/xbox_posix_io.c`'s CWD emulation (divergence #31),
fixed the same session: a bare `chdir("D:")` failing `EINVAL` (the `D:`
DOS-device symlink object is a different NT path from `D:\`, its target
directory), and `r3d_native_path` appending a literal `"./"` path
component instead of stripping it (nxdk's `FindFirstFileA` does not
canonicalise `\.\` the way real Win32 does) - found in Codex review, the
first version of this second fix only handled `path` exactly equal to
`"./"`, leaving the real `"./valve"`-shaped paths `filesystem/dir.c`'s own
root-searchpath file opens use still broken one level down; fixed to
strip the prefix for any path. All three verified by real boot logs:
`FI.numgames > 0`, `valve/gameinfo.txt` found and mounted, and a real,
retail `gfx.wad` (86,608 bytes) loading through `"./valve/gfx.wad"` with
no load error at all - X0.5's own exit line, finally observed rather than
inferred, **on xemu**. Per AGENTS.md's own Verification section ("xemu is
not validation... a phase does not close until it runs on a real Xbox"),
X0.5 itself stays open until the same boot is confirmed on real hardware
- a real-hardware-testable XISO is packaged for exactly that (see below).

Reaching that boot at all needed the CLIENT target to call `Host_Main`
for the first time ever (divergence #32): a new `XBOX_CLIENT_MAIN=1`
opt-in swaps `engine/platform/xbox/xbox_main.c`'s placeholder `main()`
(X1, #23 - never actually referenced `Host_Main`) for
`engine/common/launcher.c`'s real one, the same entry point the dedicated
target and PSVita/iOS already use. That surfaced the client's real,
previously-never-linked symbol graph: `engine/common/net_chan.c` needed a
real bzip2 (built from its own `CMakeLists.txt`-named 7-file source list,
not a stub - `BZ2_bzBuffToBuffCompress`/`Decompress` are genuinely called
for file-fragment compression), and `engine/client/cl_steam.c` needed
`getsockopt`/`SOL_SOCKET`/`SO_ERROR` added to
`engine/platform/stub/net_stub.h` (a real gap - every other BSD socket
call was already stubbed there). Result: a complete client link - engine,
filesystem, the real menu module, a stub game module, real SDL2, real
(if `XASH_NO_NETWORK=1`-stubbed) networking - 0 duplicate, 0 undefined, a
real, valid XBE.

### Added — MPAK v8: bake per-style lightmap blocks for every extra lightstyle

The lightstyle census (below) ruled out dropping the 6.69% of lit faces
that carry a second, third, or fourth lightstyle - 66% of retail maps are
affected, including the game's own opening map. This bakes the missing
data: for every face using an extra style, `BspParser.cpp` now also
allocates and writes an atlas block for it, recorded in a new sparse v8
section (`MpakExtraLightBlock` - one entry per face per extra style, not
a fixed 4-slot table, so the ~93% of faces that never need it cost
nothing extra). `filesystem/mpak_layout.{h,c}` mirror the format change
for the engine's own C-side reader, with new unit-test coverage matching
every other lump's offset/divisibility/profile-gating checks.

Verified against a real bake of `crossfire.bsp`: 127 extra-light blocks,
exactly matching the census tool's own independently-computed count for
that map (a completely separate code path agreeing on the same number);
a real negative control (one corrupted byte in the new section) is
caught and named specifically (`extra light blocks: byte 0 of 2540
differs`), not silently accepted.

Version bumped v7 -> v8 - every prior bump in this format's history has
meant a full rebake, this one is no different. Scope: only the DATA side
is done. RGB565 conversion (still RGBA8, matching every other lightmap
page) and the runtime blend across active styles are `ref/nv2a`'s job
(X9), which doesn't exist yet - this phase is baker-only.

Memory-budget risk (the extra blocks grow the same unbounded atlas-packer
style 0 already uses, with bake-time enforcement deferred to X3) is
backed by an exhaustive bake of all 125 retail maps for the Xbox target,
not a sample: 110 land on 1 atlas page, 11 on 2, 0 on more than 2. See
`docs/r3d/fork-plan.md`'s X5 section for the full breakdown.
`inspect_mpak`, the small permanent diagnostic added alongside this
change, now also validates that an archive's declared sections actually
account for its real file size, catching truncation a header-only check
would miss.

### Added — XB's two censuses, run for real against the retail install

`docs/r3d/fork-plan.md`'s XB section named two open questions as "one-minute
scripts over the retail assets... answer questions the plan had been
carrying as opinions" and never ran them. `tools/asset-studio/tools/
census_lightstyles.cpp` and `census_bones.cpp` (new, linked against the
same `AssetStudioFormats` the real baker uses) run them for real against
the Steam "Half-Life" install.

Lightstyles: 41,031 of 613,358 lit faces (6.69%) across all 125 retail
maps carry a second/third/fourth lightstyle the baker currently drops;
82 of 125 maps (66%) have at least one - `rocket_frenzy` alone carries
6,223, then a real drop to `disposal` (2,759) and the game's own opening
map `c0a0c` (1,399). Not a negligible edge case - an accepted loss would
be a visible regression on two-thirds of the game's maps and its own
opening level, ruling that option out; the choice is now between
per-style pages combined at runtime and a baked "on" state with binary
switching.

Bones: measures per-mesh, exactly - walking each mesh's real triangle
command stream for both its own vertex- AND normal-bone indices, not the
whole submodel (a submodel is not one draw) and not an over-approximation
(see docs/r3d/fork-plan.md's XB section for two rounds of review finding
real problems here, the second reversing the conclusion). Across all 598
submodels in 296 real studio models, the exact measurement finds ZERO
over the ~50-bone VP20 palette estimate - the worst case, `v_grenade.mdl`'s
grenade_hand mesh, is 35. Bone-subset partitioning code is not needed for
the retail Half-Life content at all.

Both results are written into `docs/r3d/fork-plan.md`'s XB section,
replacing "the number ... decides the rest" with the actual numbers.

### Removed — `.github/workflows/c-cpp.yml`, upstream's own CI

Inherited wholesale from the Xash3D-FWGS fork base: a 15+ job OS/arch
build matrix (Linux amd64/i386/arm64/armhf/riscv64/ppc64el, Android,
Nintendo Switch, PS Vita, Windows amd64/i386, macOS arm64/amd64, iOS)
plus a Flatpak build and a release job, firing on every `push` and every
`pull_request`. None of it builds anything this fork's own workflow
actually verifies - there is no Xbox/nxdk target in the matrix at all,
and every real verification this project does (`tools/xbox-sweep.sh`,
`tools/xbox-link-check.sh`, `tools/mpak-diff.sh`, `tools/mpak-run.sh`,
xemu boots) already runs locally against the actual target platforms,
not through this workflow. It was exhausting the account's Actions quota
for zero benefit. `.github/FUNDING.yml` and `scripts/gha/*.sh` (the
per-platform build scripts the workflow invoked) are left in place -
this removes only the workflow that ran them on every push, not the
scripts themselves, which stay as reference for how upstream's own
targets build if that's ever needed again.

### Added — X0.5 route 1: an emulated CWD, so Xbox can resolve the relative searchpaths FS_InitStdio builds

`docs/r3d/fork-plan.md`'s X0.5 section, confirmed by a real boot in the
previous entry, named two ways out. Implements route 1, the smaller and
reversible one: `compat/xbox/xbox_posix_io.c` gains a real, emulated
process-wide current directory (`r3d_cwd`, defaulting to `"D:\\"` -
guaranteed by `automount_d_drive`, already linked). `chdir()` sets it
(after checking the target is really a directory, via
`GetFileAttributesA`); `getcwd()` reads it; `r3d_native_path` - the one
function every POSIX call in this file already routes through - resolves
a relative path against it before the existing `/`→`\` conversion, and
leaves an absolute path (drive-letter-colon prefixed) untouched exactly
as before. The doc's own objection to this route was about `chdir()` as
an API pretending to succeed at something real; on a single-process
console nothing can observe an emulated CWD versus a real one, so it
is not that.

`filesystem/sys.c`'s `FS_SetCurrentDirectory` (divergence #6's "one
place where 'compat/xbox supplies what that branch needs' is false")
now routes Xbox through this real `chdir()` - `#elif XASH_POSIX ||
XASH_XBOX`, not a new branch, since this file's own `XASH_POSIX` arm
already has exactly the right contract (0/-1, `errno` set).

Verified: both files compile clean for Xbox standalone; the full
dedicated target (with and without `XBOX_ENGINE_TESTS=1`) rebuilds
96/0 sweep, 0 duplicate/0 undefined link, unchanged XBE size.
**UNVERIFIED at runtime**: a real xemu boot with a minimal synthetic
`valve/gameinfo.txt` packaged into the XISO was attempted to confirm
`FS_AddGameHierarchy` actually finds it now, but a Windows Firewall
prompt silently dropped the UDP diagnostic channel's packets for the
first attempts, and after allowing it the boot still had not produced
a log line by the time this session's context ran out. The fix is
reasoned from the same, now-confirmed failure mode the previous entry
observed directly (`r3d_native_path` is the one place every failing
call in that log - `FS_SetCurrentDirectory`, `opendir("./")` -
ultimately goes through), but has not been watched succeed. Confirming
it live is the natural first step of whatever continues this.

### Fixed — X2 W5 follow-up: filesystem_stdio wired into the static library table; X0.5's relative-searchpath problem confirmed by a real boot

The previous entry's "first real boot attempt" left a genuine mystery -
the boot animation not visibly progressing, consistent with but not
proof of X0.5's own documented relative-path problem. Wiring
`Xbox_NetLogInit()` into the dedicated boot path (before `Platform_Init`/
`FS_Init`, after `Con_Init` has something to send - divergence #13's
UDP log channel, unused until now) and running a real listener against
the next boot resolved it in one step: the animation loop was
`Sys_Error` -> "Issuing host shutdown" -> BIOS reboot -> same failure,
repeating every ~9 seconds, and the log said exactly why each time.

**First real error, a genuine bug, not X0.5 at all**: `FS_LoadProgs:
can't load filesystem library filesystem_stdio`. `engine/platform/misc/
lib_static.c`'s generated dispatch table (`tools/gen-library-tables.sh`,
wired through `tools/xbox-cc.sh`) only ever registered `server`/
`client`/`menu` - never `filesystem_stdio`, the literal string
`engine/common/filesystem_engine.c:219` (`#ifdef XASH_INTERNAL_GAMELIBS`)
passes to `COM_LoadLibrary`. Same fix as menu's own (divergence
history): `filesystem/exports.txt` already named the real entry point
(`GetFSAPI`) - `filesystem/*.c` is already part of this build's own
object set (engine and filesystem link into one binary on Xbox), so
this is a real fix, not a new stub. Registered in both
`tools/xbox-cc.sh` (the table generator invocation) and both link
scripts (`tools/xbox-link-check.sh`, `tools/xbox-dedicated-link-check.sh`).

**Second, once the first was fixed**: `FS_LoadProgs: can't find
CreateInterface entry point in filesystem_stdio` - the same table needs
a second real entry point from the same module.
`filesystem/VFileSystem009.cpp` already defines a real, `extern "C"
CreateInterface`; added to `filesystem/exports.txt` and now compiles
into the same table entry as `GetFSAPI`.

**Fixing this surfaced a real, separate, pre-existing gap**:
`tools/xbox-sweep.sh`'s file-discovery only ever globbed `*.c`, never
`*.cpp` - `filesystem/VFileSystem009.cpp` (and `filesystem/tests/
interface.cpp`) had never been attempted by the broad sweep at all, so
registering `CreateInterface` there produced a real, new "33 undefined"
regression against the tracked 32-symbol baseline, from a missing
object rather than a missing declaration. Fixed at the source: the
sweep now finds `*.cpp` too, and (since no library in this toolchain
provides `__CxxFrameHandler3`/`__std_terminate` - the same finding as
before) compiles every `.cpp` file with `-fno-exceptions -fno-rtti`,
which is what let `VFileSystem009.cpp` compile clean the first time
around. Re-verified end to end: compile 199/30 (up from 197/30 - the
two newly-discovered `.cpp` files), link back to the tracked 0
duplicate / 32 undefined exactly, symbol-identical.

**Third error, once BOTH of the above were fixed - the confirmation
this whole investigation was chasing**:

```
FS_SetCurrentDirectory: not implemented, ignoring...
Error: game directory "valve" not exist
Adding directory: ./
Couldn't find game directory 'valve'
```

This is `docs/r3d/fork-plan.md`'s X0.5 section, empirically OBSERVED for
the first time rather than inferred from reading the code: `FS_InitStdio`/
`FS_AddGameHierarchy` build every real searchpath with relative paths
(`"./"`, `"valve/"`), and nxdk's path APIs have no current-directory
concept to resolve them against - even `"./"` alone fails, exactly as
that section predicted. `FS_DetermineRootDirectory`'s `"D:\\"` fix (the
previous entry) was necessary but, as already documented, not
sufficient. **Neither of X0.5's own two proposed routes is fixed here** -
this entry's job was narrowing "boot animation stuck, cause unknown" down
to "this one, already-understood, already-scoped problem, confirmed",
not solving it.

Dedicated target re-verified after every fix above: 96/0 sweep, 0
duplicate/0 undefined link, same 2,273,280-byte XBE. Booted three more
times in xemu against a live UDP log listener to reach each error in
turn.

### Fixed — first Codex review round on the xbox-filesystem-stdio-static branch

One finding, real: `Xbox_NetLogInit()` was called unconditionally on
every `XASH_XBOX` boot. Its DHCP wait is genuinely blocking - up to
~10s with no cable or DHCP server, `net_xbox_log.c`'s own comment - so a
real, shipped, offline console would pay that delay on every single
boot forever, for a diagnostic feature only a development session
benefits from.

Fixed: gated behind `XASH_ENGINE_TESTS` too (`#if XASH_XBOX &&
XASH_ENGINE_TESTS`), the same "test/debug-only, never in a real build"
boundary already drawn for `test_dumpworld`/`test_mapprobe` elsewhere in
this file, reused rather than inventing a second mechanism for the same
distinction. `tools/xbox-cc.sh` gained a matching `XBOX_ENGINE_TESTS=1`
opt-in (env, independent of `XBOX_DEDICATED`) that defines it. Verified
both shapes: the default dedicated sweep/link (no engine tests) stays
96/0 sweep, 0 duplicate/0 undefined, the identical 2,273,280-byte XBE -
a real deployment now never pays the DHCP wait; with
`XBOX_ENGINE_TESTS=1` exported, the same target still sweeps and links
clean, so the debugging workflow this whole investigation used remains
available on request. Broad target re-verified unaffected (199/30
compile, 0 duplicate/32 undefined, symbol-identical).

### Fixed — second Codex review round on the xbox-filesystem-stdio-static branch

One finding, real: `tools/xbox-cc.sh`'s generated-library-table
preflight only checked whether each `generated_lib_*_exports.c`
existed, never whether it was still current against its own
`exports.txt`. Once generated, editing any `exports.txt` again (adding
a third `filesystem_stdio` entry point, say) would silently keep
compiling and linking the stale array - exactly the class of
runtime-only, clean-measurement-but-wrong failure this same commit had
just fixed for real.

Fixed: each generated file's mtime is now checked against its own
`exports.txt` with `-nt`, in addition to the existing existence checks
(kept first, so a fresh clone with no `.sweep/gen` at all still
regenerates rather than every `-nt` test failing against a nonexistent
left-hand file). Verified with a real control: appended a third name to
`filesystem/exports.txt`, touched it newer, recompiled - the generated
array picked up the new entry. Reverted the test addition; rebuilt both
targets clean (dedicated 96/0 sweep, 0 duplicate/0 undefined, same XBE;
broad 199/30 compile, 0 duplicate/32 undefined, symbol-identical).

### Added — X2 W5: a real, dedicated Xbox target that links 0 duplicate / 0 undefined and calls Host_Main — a genuine XBE

The X2 exit line on Xbox, "same as W4" (server frames tick from a real
boot), needs a target that actually reaches Host_Main. Investigating what
that required turned up three real, tractable gaps - none of them the
speculative video/SDL-ownership risk `docs/r3d/x0-probe-results.md`
(2026-09-05) flagged as the hard part, because that analysis was for the
eventual full CLIENT target. W5 is dedicated-only, matching W4's own PC
shape, and `common/defaults.h`'s VIDEO/INPUT/SOUND selection is already
`#if !XASH_DEDICATED`-gated - none of the SDL video-ownership question
applies here at all.

**Found: the 63 parked networking symbols were parked for the wrong
reason.** They were never a "real Xbox has no BSD sockets" gap so much as
`platform/stub/net_stub.h` (upstream's own `XASH_NO_NETWORK` stub, meant
to make `net_ws.c`/`net_chan.c`/`xrcon.c`/the two `common/http/*.c` files
compile without a real socket layer) being incomplete - written before
those files gained IPv6 support, `getaddrinfo`, and `select()`'s `fd_set`
family. Completed it with real, correctly-shaped types (RFC 3493
`sockaddr_in6`/`in6_addr`/`sockaddr_storage`, a `getaddrinfo` that
honestly reports `EAI_NONAME` since no resolver exists here, `fd_set`/
`FD_ZERO`/`FD_SET`/`FD_ISSET` as an inert one-word bitset) - every
function that would actually touch a socket is still the same
always-fails macro this header already had. Verified by compiling: all
five files now compile CLEAN, unmodified, from real upstream sources.
`net_chan.c` needed nothing from this at all - its whole gap was one
`#include <bzlib.h>` already spelled `#if !XASH_DEDICATED` (upstream's
own "bzip2 compression is not supported for server" branch); Xbox just
never defined that flag. `filesystem/VFileSystem009.cpp` needed
`ALLOCA_H` (nxdk's `alloca` lives in `<malloc.h>`, not `<alloca.h>`,
which does not exist here) and, since nxdk has no C++ exception-handling
runtime at all (`__CxxFrameHandler3`/`__std_terminate` - grep-checked
against every `deps/nxdk/lib/*.lib`), `-fno-exceptions -fno-rtti` (its
only use of either is compiler-generated destructor cleanup code, not an
explicit `try`/`catch`/`throw` in its own source).

**tools/xbox-cc.sh**: `XBOX_DEDICATED=1` (env, opt-in - every other Xbox
tool keeps the existing unconditional define set, matching the eventual
full client+menu target those measurements already track) adds
`-DXASH_DEDICATED=1` and `-DXASH_ENABLE_MAIN=1`. `-DALLOCA_H=<malloc.h>`
is unconditional (harmless everywhere, needed by the one C++ file).

**tools/xbox-dedicated-sweep.sh / tools/xbox-dedicated-link-check.sh**
(new): compile and link the real dedicated source list -
`engine/wscript`'s own "`# basic build: dedicated only`" set
(`common/*.c`, `common/imagelib/*.c`, `common/soundlib/*.c`, `server/*.c`,
`common/http/*.c`), verified against this repo's own
`compile_commands.json` from a real, clean, from-scratch PC `--dedicated`
build (109 files, zero under `engine/client/`) - with `platform/win32/*.c`
swapped for `platform/xbox/*.c`, plus `compat/xbox/*.c` (real POSIX I/O)
and `platform/misc/lib_static.c` (`COM_LoadLibrary`/`GetProcAddress`/
`FreeLibrary`/`FunctionFromName`, mandatory under `XASH_STATIC_LIBS`).
The real entry point is `engine/common/launcher.c`'s own `main()` -
`Host_Main(argc, argv, "valve", 0, Sys_ChangeGame)`, the same
statically-linked path PSVita/iOS already use - not
`engine/platform/xbox/xbox_main.c`'s X1 placeholder (divergence #23,
"deliberately not Host_Main"), which the dedicated file list now
excludes to avoid a duplicate `main`.

Two small, real gaps found only by attempting this exact combination
(`XASH_DEDICATED` + Xbox's unconditional `XASH_SDL=2`, needed for
`TIMER_SDL` since Xbox has no working `platform/win32/sys_win.c`): no
target before this one ever compiled `engine/platform/sdl2/sys_sdl2.c`
under `XASH_DEDICATED` (a real PC dedicated build never selects
`TIMER_SDL` at all, so `sys_sdl2.c` is not even in its file list), so its
unconditional calls to `SDLash_InitCursors/FreeCursors/InitSensors/
ShutdownSensors` and `Platform_GetMouseGrab/SetMouseGrab` (real bodies
only in the client-only `vid_sdl2.c`/sensor files) had never needed a
definition anywhere. `engine/platform/xbox/con_xbox.c` gained honest
no-ops for all six, under `#if XASH_DEDICATED` specifically - not
unconditional, since the non-dedicated Xbox target still compiles
`vid_sdl2.c` and already defines these for real; the first version of
this fix skipped that guard and produced 6 duplicate symbols against the
broad sweep, caught by re-running it.

**Result: a full, clean, from-scratch dedicated build - 96 files, 0
compile failures, 0 duplicate symbols, 0 undefined symbols - linked and
converted to a real XBE (2,273,280 bytes, valid `XBEH` header).** Every
symbol the real dedicated engine needs resolves; nothing is
`-force:unresolved`'d the way the broad sweep's tracked parked set is.

**Measured side effect on the existing broad (full-client) sweep and
link baseline**, re-verified end to end after these changes: compile
197 ok / 30 failing (was 193/34 - `net_ws.c`, `xrcon.c` and the two
`common/http/*.c` files now compile clean there too, unconditionally;
only `net_chan.c`, still gated on `XASH_DEDICATED`, and `cl_steam.c`
remain); link-check 0 duplicate / 32 undefined (was 0/63 - the remaining
32 are exactly `net_chan.c`'s own functions and globals plus
`SteamBroker_*`, client-only and still excluded there).
`docs/r3d/parked-undefined-symbols.txt` updated to the new, real 32-symbol
set. Xbox `.xbe` (broad target) still produces a valid, `XBEH`-verified
binary.

### Fixed — first Codex review round on the xbox-dedicated-target branch

One finding, real and severe: this entry originally claimed
`compat/xbox/xbox_posix_io.c` "already had a real `getcwd`, quietly
resolving" X0.5's `FS_DetermineRootDirectory` blocker. **False** - caught
by seeing the function existed, not by reading its body. The real body is
an honest `errno = ENOSYS; return NULL;` (the comment above it says so:
"the Xbox has no per-process current directory... Tracked as X0.5"). With
`getenv` also always NULL on Xbox (`pdclib`'s own stub) and
`SDL_GetBasePath` also NULL (`SDL_FILESYSTEM_DUMMY`), `Host_Main` would
have reached `FS_Init` and `Sys_Error`'d immediately - the dedicated
target would link clean and never get past its first real call.

Fixed: `FS_DetermineRootDirectory` (`engine/common/filesystem_engine.c`)
gained a real `XASH_XBOX` branch returning `"D:\\"` directly - not
guessed, guaranteed: `tools/xbox-dedicated-link-check.sh` already links
`-include:_automount_d_drive` (`deps/nxdk/lib/nxdk/automount_d.c`), a
CRT-init-array hook that runs BEFORE `main()` and mounts `D:` to the
directory containing the running XBE, the standard convention every
`deps/nxdk/samples/*` program relies on. Re-verified: dedicated sweep
still 96/0, link still 0 duplicate/0 undefined, same 2,273,280-byte XBE.

**This does not complete X0.5, and the docs below no longer claim it
does.** `docs/r3d/fork-plan.md`'s own X0.5 section (written 2026-09-06,
predating this session) already found that finding the root directory is
only half the problem: `FS_InitStdio`/`FS_AddGameHierarchy` build every
real searchpath with RELATIVE paths (`"./"`, `"valve/"`, ...), and
nxdk's path APIs resolve everything against `ObDosDevicesDirectory()`
with no current-directory concept at all - `opendir("./")` fails with
`STATUS_OBJECT_NAME_NOT_FOUND`, not "current directory doesn't exist,
try a real one". That doc's own two proposed routes (emulate a
process-wide CWD in `r3d_native_path`, or rewrite `filesystem/` to
prefix every searchpath with `fs_rootdir`) are both still unwritten.
Fixing `FS_DetermineRootDirectory` moves the crash from immediate
(`Sys_Error` in `FS_Init`) to later (the first relative `opendir`/
`CreateFileA` in `FS_InitStdio`), which is real, measured-as-real
progress against what Codex's review found, but is not "the engine
finds its game directory and mounts it" - X0.5's own exit line, still
unmet.

### Fixed — second Codex review round on the xbox-dedicated-target branch

One finding, real: tools/xbox-dedicated-sweep.sh reused its scratch
directory across runs and never removed anything, so a source file later
renamed, excluded or deleted left its last-successful object sitting
there; tools/xbox-dedicated-link-check.sh's plain `ls *.obj` would still
find and link it, reporting a clean link over a source set that no
longer matched the checkout. Same failure shape tools/xbox-mainui-
sweep.sh already guards against with its own manifest.txt (OK=/FAIL=/
TOTAL=) - matched here: the sweep now clears its scratch directory
before every run and writes the manifest only after a clean, non-empty
pass; the link-check now requires that manifest, requires it clean, and
cross-checks the object count on disk against it before linking.

Verified with the control the review asked for: removed
engine/platform/xbox/net_xbox_log.c after a successful build, re-ran the
sweep without manually clearing anything - the stale object was gone
(the sweep's own clearing step), the manifest correctly said OK=95, and
the link-check failed honestly with "undefined symbol: _Xbox_NetLogPrint"
instead of reporting clean. Restored the file; re-verified the real
target: 96/0 sweep, 0 duplicate/0 undefined link, same 2,273,280-byte
XBE.

### UNVERIFIED beyond compiling and linking

No xemu install and no hardware in this environment (the same caveat
this project's own log-channel work already carries, `net_xbox_log.c`) -
the dedicated XBE has never actually been booted, so the relative-path
failure above is inferred from reading the real code paths, not
observed. Still open, deliberately not attempted here: X0.5's own two
routes, and running any of this.

### Added — X2 W4: the headless run from .mpak (test_mapprobe, tools/mpak-run.sh) — the X2 exit line on PC

A dedicated server started with `+map c1a3` from a directory that holds
the map's baked .mpak, the boot floor W3 measured (gfx.wad, dlls/hl.dll,
delta.lst) and the manifest's loose files - and no maps/c1a3.bsp
anywhere in its search path - spawns, ticks server frames, and answers
the server-side consumers of the loaded world. `test_mapprobe [outfile]`
(engine/server/sv_world.c, test builds only, registered in
SV_InitOperatorCommands, divergence #28) stands at the sp_entity spawn
and runs SV_PointContents, a player-hull SV_Move, SV_TraceTexture,
SV_TraceSurface and SV_RecursiveLightPoint straight down - hull 0,
hull 1's clipnodes, the embedded texture names, msurface_t flags, and
the per-face lighting samples with lightstyles - printing every value
and an OK/FAILED verdict. tools/mpak-run.sh bakes, assembles the
directory (retail reached read-only via -rodir), runs `+map <map> +wait
30 +test_mapprobe <map>.precache.txt +quit`, requires the OK line, and
runs a control without the archive that must fail with "map <name>
doesn't exist" and "no active server".

Measured: c1a3 spawns from the archive, 34 server frames in 27 host
frames, 379 entities, spawn at 24,1517,47 over `elev2_flr` (the
elevator the map starts on), hull fraction 0.0131 stopping at z=36,
light 33,33,32; crossfire: 254 entities, over `lab1_helpad`, light
198,194,177. Both controls behave.

One finding for the manifest design: the baker's <map>.assets.txt can
only see entity keyvalues (12 files for c1a3, 2 for crossfire); the game
library precaches most models and sounds from CODE - c1a3's server
precaches 263 models and 282 sounds, 91 of the models not on disk in the
archive-only directory (barney, headcrab, hgrunt, the gib models...). The
server runs without them (the engine logs "Could not load model" per
reference and carries on; a dedicated server never needed the
geometry). With an outfile, test_mapprobe writes the server's precache
tables - a measured, PARTIAL inventory of what the map needs on disk,
not a deployment manifest: studio external sequence groups are loaded
by name from the model and Host_Error if missing (mod_studio.c:602-609),
companion texture models only on the client, and generic resources
(sv_init.c:388, RES_FATALIFMISSING) sit in other tables. A deployment
step still has to expand dependencies from it; what this run
establishes is that the BSP-derived manifest is far short of even the
server's own tables.

### Fixed — first Codex review round on the headless-mpak-run branch

Two findings, both real:

- test_mapprobe's light check started from {1,1,1} and ignored
  SV_RecursiveLightPoint's result, so a surface with no lighting samples
  (the function returns true without touching the color, sv_world.c:
  1546-1547) or a missed traversal (:1497-1498) still passed as "lit" -
  a baker regression dropping LUMP_LIGHTING would have gone unnoticed.
  The color now starts black and the verdict requires the traversal's
  own true plus a non-black sum. Measured with the control the review
  asked for: an archive whose dheader_t LIGHTING length is zeroed makes
  the probe report lit=1 light=0,0,0 and FAILED, and the runner fails.

- The precache export was described as "the complete answer" to what a
  map needs on disk. It is the server's tables only: studio external
  sequence groups are loaded by name from the model (mod_studio.c:
  602-609, Host_Error if missing), companion texture models only on the
  client, and generic resources / event scripts sit in other tables.
  The generic (files_precache) and event tables are now exported too;
  the model-internal dependencies remain outside, and every claim
  (CHANGELOG, the design doc, the fork plan, the divergence row, the
  script's own header) now says "partial inventory, expand before use".

### Fixed — second Codex review round on the headless-mpak-run branch

Two findings, both real, both false-pass paths in the verification:

- tools/mpak-run.sh silently skipped staging when the manifest was
  missing, and a failed mkdir/cp counted as nothing at all - the verdict
  only looked at the probe, which does not care whether models exist. A
  missing or unreadable manifest after the bake now fails the map; a
  failed mkdir/cp is a staging error and fails the map; files the retail
  install genuinely lacks stay the one measured, non-fatal count.

- test_mapprobe printed "cannot write" and OK in the same breath when
  the export destination could not be opened, ignored every write and
  close result, and announced the list as written regardless. A
  requested export is now part of the verdict: FS_Open, every FS_Printf
  and FS_Close must succeed, the file ends with an "end" marker, and the
  runner checks that marker before advertising the path. Control: an
  export aimed at a directory (dlls) reports "cannot write dlls" and
  FAILED. Not exercised by a control: a mkdir/cp failure and an
  unreadable manifest - neither can be injected without a test hook in
  the script (the work directory is emptied at start and the bake
  writes the manifest itself); the paths are straight-line shell and
  were reviewed, not measured.

### Added — X2 W3: the .bsp-vs-.mpak differential (test_dumpworld, tools/mpak-diff.sh)

"The engine loads a map from .mpak" is now measured, not asserted
(docs/r3d/mpak-loading-design.md, section 8). `test_dumpworld <map>
<outfile>` (engine/common/mod_bmodel.c, test builds only, registered in
Mod_Init) loads the map through the ordinary Mod_LoadWorld path and
writes every structure the loaders built - counts, planes, vertexes,
edges, surfedges, texinfo, textures with their animation links, every
surface with its extents, lightmap matrix, bevel planes and a CRC of the
lighting bytes the server reads for it, marksurfaces, every leaf with a
CRC of its decompressed PVS, nodes, clipnodes, hulls, submodels - with
pointers turned into indices. tools/mpak-diff.sh bakes each map in the
core profile, dumps it from the retail directory (reached read-only via
-rodir) and from a directory whose only map source is the archive
(gameinfo with basedir == gamedir, so no valve/ hierarchy is mounted
underneath), and requires the two dumps to be byte-identical; a control
run per map removes the archive and must produce no dump at all.

Results: c1a3 and crossfire load from .mpak into byte-identical state
(5,897,085 and 5,143,563 bytes of dumped structures, every leaf of the
lump included - see the review round below); the control run
logs "Could not load model maps/c1a3.bsp from disk"; a flipped lighting
byte in the archive is caught at the first surface's lightcrc. W2 - "the
engine needs no change" - is therefore measured too: the archive went
through SV-side validation, Mod_LoadModel's version switch and the
unmodified Mod_LoadBmodelLumps with no engine diff. Divergence #27.

Two things the first differential run taught, both environmental, both
now handled in the tool and recorded for W4:

- A dedicated boot that loads a map needs exactly three files besides
  the archive: gfx.wad (Host_InitCommon Sys_Errors without gfx/conchars,
  host.c:1137 - and on a win32 dedicated build Sys_Error then waits on
  the console window forever), dlls/hl.dll (hulls 1..3 of every brush
  model take their bounds from host.player_mins, which only the game
  library's pfnGetHullBounds fills, sv_pmove.c:449 - without it the
  world loads with three empty hulls), and delta.lst (net_encode.c:854,
  reached once the library is loaded). Grep-confirmed those are the only
  file-dependent Sys_Error sites at startup; everything else missing
  only warns. So "a directory holding only the .mpak" is really "the
  archive plus that floor".
- world.wadlist differs legitimately: Mod_LoadEntities keeps only the
  WADs present on disk (mod_bmodel.c:2263-2279), so it is left out of
  the dump; nothing but WAD texture lookup reads it.

What the dedicated differential does not cover: texture pixels
(Mod_LoadTextureData is client-side, mod_bmodel.c:2855) - names and
dimensions are compared, the embedded miptex bytes rest on the baker's
own --verify until the client run.

### Fixed — first Codex review round on the mpak-diff-tool branch

Two findings, both real, plus what the requested negative control found
on top:

- The leaf loop ran to model_t::numleafs, which after Mod_SetupSubmodels
  is the world's VISIBLE leaf count (mod_bmodel.c:2145, from
  dmodel_t::visleafs), so the last visible leaf was never dumped. Codex's
  fix - 0 through numleafs, as the PHS builder walks it (:3655) - was
  applied and then measured with the control it asked for: flipping the
  last leaf's ambient byte in the archive. That control flipped the last
  leaf of the LUMP (c1a3: index 2383 of 2384) and was NOT detected,
  because the array holds every leaf of the lump - the world's 1241,
  then the submodels' leafs, all reached through node children - and
  numleafs + 1 covers only the world's. The array size is not retained
  in model_t, so the dumper now recovers it from the nodes (the highest
  leaf index any node child points at; the array is contiguous) and
  dumps all of them: 2384 for c1a3, 1856 for crossfire. Both controls
  now detect: leaf 1240 (the last world leaf) and leaf 2383 (the last in
  the array).

- tools/mpak-diff.sh accepted any two non-empty dumps, so two runs dying
  at the same point would have compared equal prefixes and passed. The
  positive runs now require the engine to exit 0 through +quit and the
  dump to end with the dumper's "end" marker; the control run must not
  only produce no dump but log the expected "Could not load model
  maps/<map>.bsp from disk" - an absent dump alone also describes a run
  that died earlier for an unrelated reason.

### Fixed — second Codex review round on the mpak-diff-tool branch

One finding, real: the hull-0 clipnode dump was sized
lastclipnode - firstclipnode + 1, but after Mod_SetupSubmodels
lastclipnode is the COUNT of nodes reachable from the model's headnode
(mod_bmodel.c:2126, one increment per node at :1791), not an index - one
record past Mod_MakeHull0's numnodes allocation (:1911) on a world-only
map, and short of the shared array on any map with inline brush models.
It now dumps numnodes records, matching the allocation. Measured: c1a3
dumps 3447 hull-0 records for numnodes 3447 where the old bound gave
2212; crossfire 2982 for 2982 where the old bound gave 2468. Both maps
still compare identical (5,947,048 / 5,164,230 bytes).

### Fixed — the dedicated debug build never linked on MSVC

`xash_tests_dedicated.exe` (and the dedicated `xash.exe`) failed with
"unresolved external symbol _Platform_Minimize_f referenced in function
_Host_Main" on this tree's `-T debug --dedicated` configuration - the
prerequisite gap section 8 of the design doc recorded. Host_Main
registers "minimize" in the else of `if( Host_IsDedicated( ))`; under
XASH_DEDICATED that is the constant true (common.h:99), the else is dead,
no dedicated win32 build defines Platform_Minimize_f, and MSVC at /Od
keeps the reference where GCC folds it away. The else is now under
`#if !XASH_DEDICATED` (divergence #26, an upstream candidate). With the
binary linking, the stock engine tests run on this configuration for the
first time: `-runtests` reports 1011 passed, 0 failed.

### Changed — X2 W1: MPAK v7 - a BSP30 file with a classic core, in two profiles

The archive format change docs/r3d/mpak-loading-design.md defined (F1-F7,
§5-§6), implemented in tools/asset-studio and filesystem/:

- A map archive is now a BSP30 file: a real dheader_t at offset 0 whose
  lump table points at the sector-aligned raw lumps, the MpakHeader6
  descriptor at MPAK_DESCRIPTOR_OFFSET (128), bytes 124..127 zero so the
  engine's dextrahdr_t probe never sees IDEXTRAHEADER. The lump table is
  filled from the same cursor that lays the sections out
  (PakManager::save is now two-pass), so it cannot disagree with the
  descriptor by construction - and filesystem/mpak_layout.c's new
  Mpak_ValidateBspHeader checks that it doesn't anyway.

- Six appended lumps, the "classic core": raw VERTEXES, EDGES, SURFEDGES,
  FACES, LIGHTING (every lightstyle, dface_t::lightofs intact - the
  field v6 removed as "dead weight" returns, with the reason) and
  TEXTURES rebuilt as embedded miptex from the WADs the baker already
  resolves. Texture names, dimensions and 8-bit pixels reach the engine
  in the one shape it already parses; an unresolved named texture keeps
  its header (name, dimensions, zero offsets) so the engine keeps the
  name and substitutes only the image, exactly as with a real .bsp and
  a missing WAD.

- Two profiles: core (default) writes only the classic lumps; nv2a
  (--profile nv2a, MPAK_FLAG_NV2A_BLOCK) also writes the baked
  Vertex/index/FaceDrawCmd/FaceInfo/DXT/lightmap-atlas block for the X9
  renderer. Real c1a3 numbers: core 2,994,176 bytes (v6 was 8,767,488
  Xbox / 9,256,960 PC) - miptex 1,467,172 (183 textures, all with
  pixels), lighting 598,923, geometry 334,532; nv2a 11,173,888 Xbox /
  11,663,360 PC.

- The gate the design demanded: PakManager::verifyFile re-reads an
  archive and byte-compares every classic lump the file's own dheader_t
  points at against the in-memory source, independently of the writer
  (offsets from the file, bytes from the sources). save() runs it on
  what it just wrote; main.cpp's --verify <archive> bakes the map in
  memory and checks an existing file, which is how corrupted copies are
  proven to be caught. It caught a real bug on the very first core
  bake: the PC texture-array lengths were still set in the core profile
  (bytes correctly never written), so the descriptor claimed 2.3 MB of
  arrays that didn't exist. Negative controls performed: one flipped
  byte each in LIGHTING (lump byte 100), a palette (miptex byte 2141)
  and a pixel index (miptex byte 781), lengths unchanged - --verify
  fails on every one naming the exact byte, and passes on the pristine
  archive.

- filesystem/mpak.c's probe now requires both structs (version 30 at 0
  AND MPAK/7 at 128): a plain .bsp renamed .mpak is ignored, and
  UiPacker's menu archives (descriptor at 0, not maps) are correctly no
  longer mounted as "maps/menu.bsp". filesystem/mpak_layout.{h,c} gain
  the six lumps and the dheader_t cross-check; filesystem/tests/
  mpak_layout.c grows to 18 cases (core/nv2a x pc/xbox layouts, a v7
  lump divisibility check, and three dheader_t rejections: wrong BSP
  version, a lump table one sector off, IDEXTRAHEADER-shaped bytes
  after the header); filesystem/tests/mpak.c adds the no-dheader_t
  rejection. All 6 filesystem test binaries pass. The real bakes check
  out with the layout tool: descriptor, computed layout and dheader_t
  agree and the padded end lands exactly on the file size for all three
  variants.

Engine change: none - W2 is a checkpoint, not code (design doc §10).

### Fixed — first Codex review round on the mpak-v7-core branch

Three findings, all real, all in the baker:

- buildEmbeddedMiptexLump bounded an embedded texture's bytes by the
  NEXT directory entry's dataofs without checking that offset against
  the buffer - a damaged BSP whose second dataofs pointed past the lump
  made the first entry copy up to 770 bytes beyond the input into the
  archive (or crash). Every offset is now validated against the buffer,
  only a valid later offset can bound an entry, and an embedded payload
  the lump is too short to hold is demoted to header-only (name and
  dimensions kept, offsets zero) rather than copied short for the
  engine to read past.

- It resolved a same-named WAD lump BEFORE the BSP's own embedded
  pixels. The reference does the opposite by default: r_wadtextures is
  "0" (model.c:32) and Mod_LoadTextureData only tries a WAD when that is
  set or the miptex has no pixels of its own (mod_bmodel.c:2834-2835),
  so a map with customised embedded art under a WAD texture's name
  would have baked pixels the engine's default never shows - and
  verifyFile could not see it, since the expected bytes already carried
  the substitution. Precedence is now embedded-complete, then WAD, then
  header-only. (The nv2a block's DXT path keeps its documented WAD-first
  choice; retail maps embed no textures - c1a3 is 183 WAD references,
  0 embedded - so the two never differ on retail data, and the block is
  X9's to reconcile.)

- verifyFile checked the classic lumps' bytes but nothing about the
  baked block's counts and lengths, so an nv2a archive with vCount
  rewritten to 67,108,864 - a 4 GiB vertex block the C reader rejects
  (filesystem/mpak_layout.c) - verified clean. It now checks every
  baked-block count/length against this bake, the profile and target
  flags, and re-derives every section's offset from the descriptor in
  write order exactly as filesystem/mpak_layout.c does: the fifteen
  classic lumps must land where the file's dheader_t says, and the walk
  must end exactly at the file's size. Any wrong length anywhere in the
  walk shifts everything after it.

The builder is now a public function with a real unit test
(tools/asset-studio/tests/miptex_lump_test.cpp, `ctest -C Release`,
built from a new AssetStudioFormats library the executable links too):
embedded-beats-WAD, WAD resolves an external reference, unresolved keeps
name/dimensions, absent stays -1, the out-of-range next-offset case,
the truncated-payload case, and the empty lump. Negative controls
re-run after the fixes: the three classic-lump byte flips still fail
naming the exact byte, and two new ones - vCount rewritten to
67,108,864, and the target flag flipped - fail on the descriptor
checks; the pristine core and nv2a archives verify.

### Fixed — second Codex review round on the mpak-v7-core branch

Two findings, both real, both in the baker, plus one a negative control
found while re-verifying the fixes:

- The core miptex lump resolved external references from EVERY WAD the
  VFS could see (the worldspawn-listed ones first, then every other
  .wad on disk, later loads overwriting earlier names). The reference
  looks only in world.wadlist, last listed first
  (Mod_LoadTextureFromWadList, mod_bmodel.c:619-655), so an unrelated
  WAD sharing a texture name could have had its pixels baked into the
  map permanently - and since verifyFile compares against the same
  in-memory bytes, it would have passed. exportToPak now builds a
  separate lookup from the listed WADs only, in list order with later
  overriding earlier (indexWadLumpsForCore), and the core builder
  consumes that; the nv2a block's DXT path keeps its all-WADs lookup
  (X9's documented choice). A unit-test case covers the precedence
  (last listed wins, unique names from earlier WADs survive, an
  external reference resolves through that index). On c1a3 the
  distinction is 3628 listed-WAD lumps out of 3654 registered, and the
  core archive is byte-identical in size (2,994,176).

- The core profile still ran the whole nv2a face loop and only
  suppressed its output, so PakManager::addVertex's 65,536-vertex abort
  (a real limit for the 16-bit baked index buffer) could refuse a core
  bake of a map whose per-face UV expansion exceeds it - a buffer the
  core archive does not contain. The DXT resolution, face loop and
  lightmap atlas now run only under isProfileNv2a(); the raw-lump
  copies and the embedded-miptex construction stay unconditional. Every
  retail valve map was baked in both profiles to check this: 125/125
  bake and verify in core, while four (contamination, disposal,
  pool_party, rocket_frenzy) still abort in nv2a at that limit - the
  exact failure the finding predicted, now confined to the profile
  that actually has the buffer.

- Re-running the negative controls after the fixes showed verifyFile
  passing an nv2a archive with a vertex byte flipped: it checked the
  baked block's counts and lengths but not its bytes. The sections the
  baker still holds after save - vertices, indices, face draw cmds,
  lightmap atlas, face info - are now byte-compared at the offsets the
  descriptor walk derives; the DXT arrays/atlas stay structural-only
  (their bytes are produced inside save() and not retained). Controls
  now: pristine core and nv2a verify; lighting, miptex and faces byte
  flips, vCount 67,108,864, a flipped vertex byte, a flipped target
  flag, and a core archive verified as nv2a all fail.

### Changed — X2: the map-loading plan, after auditing what the engine actually needs

docs/r3d/mpak-loading-design.md (new) audits every consumer of the
runtime BSP structures before any loader is written - the server and
pm_shared, ref/soft, ref/gl, ref/null - and the real baker, each claim
cited by file:line. Three findings reshape the remaining X2 work:

- There is no "structural half first, render half later". The reference
  pipeline (mod_bmodel.c:4229-4247) is one ordered chain: MarkSurfaces
  indexes mod->surfaces, Leafs indexes marksurfaces, Nodes needs leafs,
  MakeHull0 needs nodes - Mod_LoadSurfaces has to run first.

- Every consumer before X9 needs exactly what the v6 bake dropped.
  ref/soft scan-converts from surfedges/edges16 every frame
  (r_rast.c:444, r_bsp.c:386); the server (sv_world.c:1546-1571),
  ref/soft (r_surf.c:174-236) and ref/gl (gl_rsurf.c:725) rebuild
  lighting per lightstyle and per dynamic light from per-face samples;
  pm_trace.c:835, sv_world.c:1436, Mod_GetFaceContents and
  animated-texture sequencing need texture names. The v6 atlas is
  lightstyle-0-only and 7x the lighting lump it replaced (4,194,304 vs
  598,923 bytes on c1a3); v6 stores no texture names or dimensions.

- So the right engine change is not a rewrite of six loaders - it is no
  engine change. Give the archive a "classic core" (raw VERTEXES/EDGES/
  SURFEDGES/FACES/LIGHTING plus embedded-miptex TEXTURES resolved from
  the WADs at bake time) and make it a BSP30 file: a real dheader_t at
  offset 0 whose lump table points at the sector-aligned raw lumps, the
  MPAK descriptor at a fixed offset (128) inside the first sector.
  Mod_LoadLump reads lump extents from that header and stores pointers,
  never copies (:769, :925-928), and - found in Codex review of the
  design's first draft, which had synthesised the header in memory
  inside Mod_LoadBrushModel - six engine paths validate a map from disk
  before any loader runs (SV_ValidateMap -> Mod_TestBmodelLumps,
  sv_game.c:823-853 / mod_bmodel.c:4323-4398; Mod_CheckLump/ReadLump/
  SaveLump; con_utils.c:93), so the header they expect has to be the
  first bytes of the file. With it there, the unmodified
  Mod_LoadBmodelLumps loads the archive and renderers already receive
  the raw buffer via Mod_ProcessRenderData (model.c:381) to find the
  optional baked block. That block (Vertex/index/FaceDrawCmd/
  MpakFaceInfo/DXT/atlas) becomes X9-only.

The design doc carries the field-by-field consumption contract for
msurface_t/mextrasurf_t/glpoly2_t, the MPAK v7 packing spec, the
step-by-step loading chain, a renderer-free differential verification
plan (.bsp vs .mpak model_t dump), real c1a3 size numbers, the
decisions taken by recommendation, and the work packages W1-W5.
fork-plan.md's X2 section and divergences.md's anticipated
mod_bmodel.c row are updated to match. No code changed.

### Added — X2: filesystem/mpak_layout.h/.c, the real MPAK offset math

MpakHeader6 has no offset table - "byte layout order IS the format" -
and, verified directly against the real writer
(tools/asset-studio/src/formats/PakManager.cpp's save(), not assumed
from the header comment alone), that layout is NOT simply "walk the
header's fields in declared order": every section is independently
padded to a 2048-byte sector boundary, the vertex/index geometry block
has no length field of its own at all (size = vCount/iCount times a
fixed per-record size the header never states), and the texture block is
target-dependent (Xbox writes one DXT atlas; PC writes three separate
DXT arrays). Getting any of this wrong produces silent, wrong offsets,
not a compile or link error.

Mpak_ValidateHeader/Mpak_ComputeLayout compute every lump's real
offset/length once, correctly, from a validated header. Real BSP-format
lump sizes (dplane_t/dnode_t/dleaf_t/dclipnode_t/dmodel_t/dtexinfo_t)
come from common/bspfile.h directly, not re-derived or guessed -
confirmed byte-identical to the source .bsp's own lumps by tracing
BspParser.cpp's getLumpData(LUMP_X) calls feeding PakManager's
setXData() verbatim.

Does not parse any lump's contents. dbspmodel_t and the
Mod_LoadPlanes/Mod_LoadNodes/etc. functions that would consume these
offsets are static, file-local to engine/common/mod_bmodel.c (confirmed
by grep - nothing else in the tree references dbspmodel_t), so building
runtime BSP structures from this layout is separate, larger, unwritten
work that has to live inside that file.

Verified two ways: a new filesystem/tests/mpak_layout.c unit test
(hand-computes every expected offset independently - Mpak_RoundUpToSector
is static, unreachable from the test - covering both PC/Xbox-target
layouts plus six rejection cases: bad magic, bad version, a
header-size mismatch simulating C/C++ struct drift, a truncated file, a
non-divisible lump length, a lightmap-dimensions/byte-count mismatch);
and a real bake from the actual retail install, both targets, where the
computed end-of-archive offset landed EXACTLY on the real file size
(9256960 and 8767488 bytes - not merely "close" or "under") and the
computed entities-lump offset resolved to genuinely readable GoldSrc
entity text.

### Fixed — first Codex review round on the mpak-layout branch

fs_offset_t is off_t (common/xash3d_types.h:48), confirmed 32-bit
signed - sizeof(fs_offset_t) == 4 - on both Xbox
(compat/xbox/sys/types.h:18) AND this PC/MSVC build, not Xbox-only.
Mpak_PlaceLump took its length as fs_offset_t, and every call site
passed a uint32_t header field straight through: a corrupted
lenVisibility = 0xFFFFFFFF narrows to -1, which the old `length <= 0`
check then treated as "empty, skip" instead of rejecting - a corrupted
~4 GB lump silently vanishing instead of failing loudly. The geometry
block had the same shape one level up -
`(fs_offset_t)header->vCount * sizeof(mpak_vertex_t)` cast vCount to a
32-bit signed type before multiplying, so a vCount of exactly 67108864
makes the product exactly 2^32, wrapping to precisely 0 - a claimed 67
million vertices reads back as "zero vertices, valid". Same fix
applied to the lightmap width*height*layers*bytesPerPixel cross-check,
same overflow shape.

Fixed by widening every length to uint64_t before Mpak_PlaceLump ever
sees it - the bounds check against fileSize is what decides validity
now, not the parameter's own type; narrowing to fs_offset_t only
happens after that check confirms the value fits safely.

Verified with two real negative controls: reverted only this fix,
confirmed two new regression tests (TestRejectsUint32MaxRawLumpLength,
TestRejectsOverflowingVertexCount) genuinely fail against the old code
and genuinely pass against the fix - both exercise the bug directly,
not hypothetically, since fs_offset_t is confirmed 32-bit on this
build too. Re-verified end to end: 6/6 filesystem tests pass, the real
c1a3.mpak/c1a3_xbox.mpak cross-checks still compute identical
byte-exact offsets (legitimate data was never near these overflow
edges), and a full Xbox sweep + xbox-link-check.sh --xbe run still
reports 0 duplicate/63 undefined unchanged and a real .xbe still
builds.

### Fixed — second Codex review round on the mpak-layout branch

Round 1's own fix still had two more overflow paths one level up:

Mpak_PlaceLump bounds-checked only the UNPADDED length against
fileSize, then advanced *cursor by the separately-computed
sector-rounded extent. A length that fits exactly (fileSize ==
INT32_MAX, length == INT32_MAX - cursor, both individually in range)
can still round up to one byte past what a 32-bit signed fs_offset_t
cursor can hold, overflowing it. Fixed by computing the padded extent
in uint64_t up front and bounds-checking THAT against the remaining
space before *cursor or out->length are ever touched. The now-redundant
Mpak_RoundUpToSector helper (same overflow shape, fs_offset_t-typed)
was removed rather than left as dead, unsafe code beside its own fix.

The lightmap size cross-check's plain uint64_t product
(width * height * layers * bytesPerPixel) can itself overflow: three
arbitrary uint32_t values multiplied together can need up to ~96 bits,
and uint64_t only has 64. width=height=2^31, layers=1, bytesPerPixel=4
makes the naive product exactly 2^64, wrapping to 0 - an "impossible"
multi-gigapixel lightmap would have matched a declared
lenLightmapAtlas of 0 and passed. Fixed with a checked multiplication
helper (Mpak_CheckedMulU32) that rejects as soon as the running
product would exceed UINT32_MAX - the ceiling for anything a real
lenLightmapAtlas field could ever represent.

Verified with two more real negative controls: reverted only this fix,
confirmed two new regression tests (TestRejectsPaddedExtentOverflow,
TestRejectsLightmapDimensionOverflow) genuinely fail against round 1's
code and genuinely pass against this fix - both header-constructed
directly since these are about the header's own internal arithmetic,
not real file content. Re-verified end to end: 6/6 filesystem tests
pass (10/10 within test_mpak_layout itself now), the real
c1a3.mpak/c1a3_xbox.mpak cross-checks still compute identical
byte-exact offsets, and a full Xbox sweep + xbox-link-check.sh --xbe
run still reports 0 duplicate/63 undefined unchanged and a real .xbe
still builds.

### Added — X2: filesystem/mpak.c, a real searchpath_t for .mpak archives

filesystem/mpak.c (new) implements the searchpath_t interface (modelled
on pak.c) for the narrowed .mpak format: one archive holds exactly one
map's baked BSP geometry/textures/lightmaps, so unlike pak.c's
dpackfile_t directory there's nothing to search - the archive just
remembers and answers to its own single virtual name,
"maps/<basename>.bsp" (matching sv_init.c:1025's own worldmodel precache
convention), serving the whole file verbatim. Registered in
searchpath.c's g_archives[] table so FS_AddGameDirectory discovers
*.mpak files the same way it already discovers *.pak/*.wad, as an
ordinary entry - a loose maps/<name>.bsp wins if both are present (see
the second Codex review round below for why that, not the reverse, is
the only currently-safe ordering).

Scope, explicit: this only gets a raw MPAK-magic buffer as far as
FS_LoadFile - it does not teach Mod_LoadModel's format-dispatch switch to
recognize MPAK's magic or parse MpakHeader6's lump layout into runtime
structures. That real brush-model port is separate, larger, unwritten
work; X2's "the engine loads a map from .mpak" exit criterion still needs
it.

Verified on both platforms: a new filesystem/tests/mpak.c unit test
(built and run for real via a PC `waf --dedicated --enable-tests` build)
proves discovery + byte-exact whole-file passthrough + rejection of a
bad-magic/bad-version archive; a full Xbox sweep + xbox-link-check.sh
--xbe run confirms 0 duplicate/63 undefined symbols unchanged from the
existing baseline and a real .xbe still builds with mpak.c linked in.

### Fixed — first Codex review round on the mpak-searchpath branch

Two findings, both real.

.mpak did not actually win over a same-named loose map, contradicting
the change above's own stated intent: FS_AddGameDirectory mounts every
entry in g_archives[] as one group, entirely BEFORE it adds the plain
directory, and FS_AddArchive_Fullpath always prepends onto
fs_searchpaths (whatever mounts LAST is searched first) - an entry's
position within g_archives[] only ever decides precedence among archive
types against each other, never against the plain directory. A loose
maps/c1a3.bsp would always have been found before c1a3.mpak's virtual
name. Fixed with a new mount_after_plain_dir field on fs_archive_t:
FS_AddGameDirectory now mounts the ordinary archive group first
(unchanged), then the plain directory (unchanged), then a second pass
for any mount_after_plain_dir entry - .mpak is the only one set today.
Verified with a real negative control: reverted only this fix, confirmed
the new precedence test genuinely fails without it and genuinely passes
with it restored. The precedence test itself needed a second fix first -
an earlier draft called AddGameDirectory twice on the same already-
mounted "./", which passed even with the bug present (a directory
already mounted is never re-prepended, so a later call's fresh archives
always end up ahead of it regardless of ordering logic) - rewritten to
scan a fresh, never-before-mounted directory exactly once with both
files already in place.

docs/r3d/divergences.md was not updated for the upstream-file edits this
branch makes (filesystem_internal.h, searchpath.c, filesystem/wscript) -
the only mpak entry was still under Anticipated divergences. Moved to
Active divergences (#24) with the files actually touched, the precedence
bug and fix above, and the verification evidence.

### Fixed — second Codex review round on the mpak-searchpath branch

Round 1's own fix was itself wrong, one level up: making .mpak actually
win over a loose map turned out to be actively harmful right now, not
just premature. filesystem/mpak.c serves the whole archive as raw
MPAK-magic bytes, and Mod_LoadModel's format-dispatch switch (model.c)
does not recognize that magic yet - a map that would otherwise load fine
from its loose .bsp would instead hard-fail with "has unknown format"
the instant its baked .mpak sibling was merely present alongside it,
for zero benefit, since nothing can consume the winning bytes anyway.

Reverted mount_after_plain_dir and FS_AddGameDirectory's two-pass split
entirely. .mpak is an ordinary g_archives[] entry again, mounted in the
main group before the plain directory like every other archive type -
this is now the smallest possible diff to FS_AddGameDirectory (none at
all; only a new table entry), and a loose .bsp winning means baking a
map's .mpak can never break that map's existing, working load path.

The precedence test was rewritten to assert the opposite of round 1's
version - that the loose file still wins - and verified with a real
negative control: run unchanged against round 1's mount_after_plain_dir
version, it fails exactly as expected; against the reverted version, it
passes. Re-verified end to end on both platforms after the revert: 5/5
filesystem tests pass on PC, and a full Xbox sweep + xbox-link-check.sh
--xbe run still reports 0 duplicate/63 undefined unchanged and a real
.xbe still builds.

Precedence flips back to favor .mpak once a real Mod_LoadModel/
brush-model MPAK parser exists to make winning safe - documented as the
divergence's own exit condition (docs/r3d/divergences.md #24), not
decided or attempted here.

### Changed — X2: narrow .mpak to geometry/textures/lightmaps, add a per-map manifest

tools/asset-studio's baked .mpak archive is now limited to BSP geometry,
level (WAD-resolved) textures, and lightmaps - the data a monolithic
per-map archive earns its keep for on the Xbox's slow IDE bus. Models,
sprites and sounds are ordinary GoldSrc files upstream's own loaders
already read directly from disk, so main.cpp no longer parses or embeds
them at all; it instead writes their names to a sibling
<mapname>.assets.txt manifest (and a shared global.assets.txt for
engine-wide assets), replacing the old asset-dict embedding.

MpakHeader.h's lenAssetDict/lenAssetData fields are kept (append-only,
never removed) but are always 0 now - documented inline.

Corrected stale pre-pivot documentation found while touching this code:
tools/asset-studio was inherited from this repo's own pre-rename
bootstrap (its first-ever commit is literally "Bootstrap the Ferrum56
fork"), and MpakHeader.h's comments still said "the Rust reader
(filesystem/mpak.c, not written yet)" - the real reader is, and was
always meant to be, C, not Rust.

Analysis: the manifest is a build-time precache list only, with no
memory-management responsibility of its own - cross-level asset
dedup/eviction is already solved correctly by the reference engine's
existing mark-and-sweep (Mod_PurgeStudioCache/Mod_FreeUnused in
engine/common/model.c, S_EndRegistration's generation-counter
equivalent in engine/client/sound/s_load.c). A single combined
CPU-heap + GPU-texture-pool memory budget remains unsolved and stays
X3's job.

Found in review: AssetManifest silently reported success after a
failed open or a mid-write failure (disk-full, permissions) - fixed
with a real ok() check, verified with a repro against a nonexistent
output directory. packSprites wrote a stale "hud/" prefix instead of
the real sprites/ path the engine actually requests
(SPR_GetList("sprites/hud.txt", ...), hud.cpp:447) - fixed and
verified against a scratch manifest. build-test-package.ps1 updated to
track global.assets.txt instead of the now-retired global.mpak.

Found in a second review pass: packSounds/packSprites/packModelManifest
had the same false-clean-success shape as AssetManifest above, one
level up - an explicitly requested --sounds/--sprites/--models manifest
that failed to open only warned, and main() never checked anything.
Fixed the same way: all three now return bool, main() treats false as
fatal, verified with real repros against missing manifest paths.
build-test-package.ps1's per-map skip check also only looked at the
.mpak's freshness, not the now-required sibling .assets.txt - a pre-X2
output directory would report every map "up to date" while producing
no per-map manifests; fixed to require both files. Also corrected
$hudManifest/$menuManifest, which resolved against $RepoRoot (itself
already broken - resolves to tools/, not the repo root, part of this
script's already-flagged-dead compilation half) to point at the real
checked-in tools/asset-studio/manifests/ instead.

filesystem/mpak.c (the C engine-side reader for the narrowed format)
is not written yet - still X2 work.

### Added — X1: a real .xbe exists

engine/platform/xbox/xbox_main.c (new) provides main() - deliberately
not Host_Main, which is genuinely X2+ work. Matches nxdk's own
hello-world sample almost exactly: an infinite loop calling
debugPrint, nothing more, touching no engine subsystem at all.
Resolving _main drops the tracked undefined-symbol count from 64 to
exactly 63 - verified symbol-for-symbol, the 63 parked networking
symbols, _main gone entirely.

A linker cannot emit a loadable binary with unresolved symbols -
found attempting the actual conversion: nxdk-link produces no output
at all with any undefined symbols remaining. lld's -flavor link mode
genuinely supports MSVC link.exe's real /force:unresolved flag
(verified directly against lld's own help text, not guessed), which
does exactly what "parked, not forgotten" already means at link time
- a forced-unresolved import resolves to address 0, so anything that
ever actually called one of these 63 symbols would crash loudly, not
silently succeed.

tools/xbox-link-check.sh gained a new --xbe <path> flag: after a
successful --expect-undefined match, it runs a separate
forced-unresolved link (not a change to the measurement link itself,
since that depends on undefined symbols staying real errors, not
warnings), converts the result with cxbe, and verifies the output
genuinely starts with the XBEH magic header.

Found in review: --expect-undefined only compares a COUNT, not which
symbols they are - a same-count swap (one parked symbol accidentally
defined, an unrelated new one going undefined instead) would have
passed and been silently forced into a real .xbe, hiding a real
regression behind a check that only verified the wrong thing. Fixed
with docs/r3d/parked-undefined-symbols.txt (new) - the real, checked-in
reference of every expected symbol name, not just how many. Verified
both ways: the real 63-symbol set still builds, and a simulated
same-count swap is caught and refused.

Found in a second review pass: the identity check above only ran
inside the --xbe branch, so the script's own ordinary, documented
invocation (--expect-undefined N alone, no --xbe) still only ever
compared a count - the same-count swap only got caught when also
requesting an XBE. Moved to run for every successful
--expect-undefined match, unconditional on --xbe. Re-verified both
ways without --xbe at all: the real set still passes, the same
simulated swap is still caught.

Verified end to end: tools/xbox-link-check.sh --expect-undefined 63
--xbe .sweep/resonance3d.xbe produces a 4,198,400-byte file starting
with the real XBEH magic - a structurally valid Original Xbox
executable, built from the full engine + third-party audio + menu
module + stub game module object set. Also verified the failure path:
a wrong --expect-undefined count correctly refuses to attempt the XBE
build at all.

X1's own exit line, in full: "engine, filesystem, menu and a stub
game module link into one XBE." All five nouns are now literally true
of this tree. Not bootable or playable - main() never calls into the
real engine - but a real .xbe, produced by this project's own real
toolchain, genuinely exists for the first time.

### Added — X1: all four names in the exit line link - a stub game module too

stub_game/ (new) - deliberately not deps/hlsdk (the real, ~160-file
Half-Life SDK already vendored for a later phase). Proves the
engine's static-linking-by-name mechanism (lib_static.c's Lib_Find,
since Xbox has no dynamic loading) resolves a real game module, not
that any gameplay logic works - real game code is X7's job.

server/server_stub.c exports GetEntityAPI2, because sv_game.c
genuinely hard-requires at least one of GetEntityAPI/GetEntityAPI2 to
exist. Returns a validated-but-entirely-zeroed DLL_FUNCTIONS - every
pointer NULL, honest about implementing nothing, safe because nothing
in this tree calls through any of them yet (no Xbox entry point
exists). client/exports.txt is deliberately empty: the client's
per-name lookup only matters during a real client boot never reached,
and a missing name just leaves that one pointer NULL rather than
failing the load - a materially softer contract than the server side.

tools/gen-library-tables.sh extended to generate a real per-library
export table, not just the previously-empty outer one.

Verified by linking: server_stub.c plus both generated export tables
compiled and linked in alongside the full engine + third-party + menu
set: 0 duplicate symbols, 64 undefined - the exact already-known set,
verified symbol-for-symbol identical. Main engine baseline unaffected
(187 ok, 34 failing).

Found in review, two findings. The first version only exported
GetEntityAPI2, missing that sv_game.c performs a second, entirely
separate hard check for GiveFnptrsToDll immediately after -
SV_LoadProgs fails and frees the library before GetEntityAPI2 is ever
called if it's absent, so a stub without it links but is not actually
loadable under the real engine's own load sequence. Fixed with a
matching, equally honest no-op GiveFnptrsToDll. Also: xbox-cc.sh only
checked whether generated_library_tables.h existed before skipping
regeneration - a stale .sweep/gen from an older, zero-library-name
version of that script would look already-generated forever even
though the two files this build now needs were never produced. Fixed
by checking all three expected outputs, not just the first. Both
verified: reproduced the exact stale-cache scenario directly, and
re-confirmed 0 duplicate, 64 undefined, symbol-for-symbol identical.

Found in a second review pass, two more findings. SV_LoadProgs calls
pfnGetGameDescription/pfnGameInit/pfnRegisterEncoders unconditionally
as part of its own successful-load path, not deferred to a later game
frame like every other DLL_FUNCTIONS member - the earlier "safe
because nothing calls through these pointers yet" claim was false for
these three. Fixed with real, honest, minimal bodies for exactly
these three; the other ~37 members stay genuinely NULL.

Also: 82 mainui objects being in the link never meant the menu module
was reachable - lib_static.c's Lib_Find only resolves names present
in the generated table, and menu was never registered there at all.
Fixed by extending the generation to cover mainui's own real
GetMenuAPI/GetExtAPI exports (no new stub code needed) and compiling
that table into the link, with a trivial empty fallback for runs
without mainui swept (the outer table now declares the symbol
unconditionally). Both configurations re-verified: 0 duplicate, 64
undefined, checked against the known categories directly.

All four names in X1's own exit line now genuinely link together into
one binary. Only an actual bootable XBE remains - _main is still among
the 64 undefined symbols, and an XBE cannot exist without one.

### Added — X1: it links - engine, filesystem, third-party audio, and the menu module, together

Linked the menu module's 82 objects in alongside the already-proven
engine and third-party audio sets plus deps/nxdk/lib/libc++.lib: 0
duplicate symbols. Undefined started one over the known baseline -
_atof, a real gap (nxdk's pdclib declares atof in <stdlib.h> but has
never implemented it, own source says so) mainui's CFGScript.cpp,
ConnectionProgress.cpp and ServerInfo.cpp call for ordinary cvar
values. Fixed with compat/xbox/xbox_crt_ext.c (new, a self-contained
parser - pdclib has no strtod either, so this can't just forward to
it). Re-linked: 0 duplicate, 64 undefined - the exact already-known
set, verified symbol-for-symbol identical.

Along the way, adding 82 more objects pushed the raw nxdk-link command
line past the OS argument-length limit. Fixed tools/xbox-link-check.sh
to use a response file (@file, the standard link.exe convention lld
already accepts) instead of a raw command line. Found building that
fix: writing MSYS2-style paths into the response file failed with
"could not open" for objects that genuinely existed - MSYS2 bash only
auto-translates a POSIX path when it's an actual argv entry, not text
a program reads back from a file. Fixed with cygpath -m on every path.

tools/xbox-link-check.sh now includes the menu module automatically
whenever tools/xbox-mainui-sweep.sh has been run (optional, so a
caller measuring just engine+third-party isn't forced to sweep mainui
first) - the real, permanent tool for the whole picture now.

Found in review, two findings. tools/xbox-mainui-sweep.sh only ever
added .obj files, never removed any - a file that compiled once, then
got deleted, renamed, or started failing, left a stale object nothing
could tell apart from a current one. Fixed: the sweep clears its
output before every run and writes a manifest (OK=/FAIL=/TOTAL=)
after every run; tools/xbox-link-check.sh now requires that manifest
be present and clean before trusting mainui objects at all.

Found in a second review pass on that same fix: the manifest write
happened before the sweep's own zero-total rejection, so a broken,
empty sweep still wrote "OK=0 FAIL=0 TOTAL=0" and exited 1 - a
manifest that reads as clean (FAIL=0) to anything checking only that
field, which is what the link-check side originally did. Fixed two
ways: the sweep now rejects TOTAL=0 before writing any manifest at
all, and the link-check side independently requires TOTAL>0 in
whatever manifest it reads, plus checks the manifest's OK count
against the real object count on disk. Verified by reproducing the
exact scenario - hiding every mainui source file and running both
scripts in sequence: the sweep fails loudly with no manifest written,
and the link-check correctly refuses instead of silently proceeding
with zero menu objects.

Also: the first version of atof accumulated its exponent with no
bound, then scaled once per exponent unit - a string like
1e2147483647 would drive a multi-billion-iteration loop (a real
CPU-hang, not theoretical: ServerInfo.cpp passes server-derived
values straight to atof) and signed-overflow before that loop was
even reached. Fixed by clamping exponent accumulation at 1000 - past
double's real dynamic range, so legitimate values are unaffected and
anything beyond correctly saturates to inf/0 without the unbounded
loop. Verified natively: extreme exponents now resolve correctly in
under a microsecond.

Answers three of the four names in X1's own exit line: engine,
filesystem, and the menu module all link together cleanly. Only a
stub game module and an actual bootable XBE remain.

### Added — X1: the menu module - 82 of 82 files compile for Xbox

Closed both gaps the previous entry left open, then swept the whole
module. compat/xbox/new.h (new) matches real MSVC's own <new.h> -
one line, #include <new>. Added a preflight that fails fast with both
required commands spelled out if 3rdparty/mainui's nested miniutl
submodule isn't initialised, instead of silently working around it.

That surfaced one more gap of the identical shape: miniutl/winlite.h
unconditionally includes <winsock2.h> and <dbghelp.h> under a
genuinely-reachable _WIN32 block - both already-known, deliberately
parked gaps (networking is parked, there is no crash-dump API on this
platform at all), not new ones. Patched to skip exactly those two
includes, keeping the rest of the file unchanged.

Built tools/xbox-mainui-sweep.sh rather than leaving this as a one-off
measurement - compiles every file mainui's own wscript declares, with
its own real include roots and defines, gated the same way
xbox-sweep.sh already gates on --expect-fail. Main engine baseline
unaffected (187 ok, 34 failing).

Found in review: the first version of this sweep never passed
MAINUI_USE_CUSTOM_FONT_RENDER, so font/WinAPIFont.cpp - the file that
actually calls real Win32 GDI, which nxdk does not have - compiled
down to an empty translation unit instead of being tested. Fixed by
passing that define (wscript's own configure() sets it
unconditionally) plus MAINUI_USE_STB - not a guess, the only font
backend upstream's own code leaves reachable here, since
FreeTypeFont.cpp explicitly excludes _WIN32 regardless of nxdk having
a real libfreetype.lib. Re-verified with the corrected defines: 82 of
82, the same result, now for real.

Also found in review: the miniutl preflight had been placed in
tools/xbox-cxx.sh, forcing unrelated future C++ work (the HL SDK
client/server) to need mainui's own submodule initialised. Moved into
tools/xbox-mainui-sweep.sh, the only script that needs it -
tools/xbox-cxx.sh is generic again.

Not measured: RTTI/dynamic_cast/exceptions, or whether this whole
module actually links - this only proves it compiles.

Found in review (second pass): the sweep's glob loop silently skipped
unmatched paths, so a broken checkout or source-list drift that
matched nothing would have printed "0 ok, 0 failing" - and
--expect-fail 0 alone would call that a pass. Fixed: zero attempted
files is now always a hard failure, and --expect-total N compares the
real attempted count against a tracked baseline. Verified both ways -
hiding every source file fails loudly, the real checkout reports 82
ok, 0 failing, 82 attempted matching both gates.

### Added — X1: a C++ toolchain, and new/delete link clean

Nothing in this tree had compiled a single C++ translation unit for
Xbox before now - tools/xbox-cc.sh is C-only, and the menu module
(3rdparty/mainui, 76 .cpp files) plus the HL SDK's client/server are
all C++. Added tools/xbox-cxx.sh, the same reordered-includes/prelude/
patch-preflight shape as xbox-cc.sh, calling nxdk-cxx's own flags
reordered so compat/xbox wins.

Verified against two representative test files (virtual dispatch,
inheritance, templates, new/delete): both compile clean. Compiling is
not linking, and this script only ever compiles, same scope as
xbox-cc.sh.

Found in review: an earlier version of this entry claimed
deps/nxdk/lib/libcxx has no prebuilt library and that operator new/
operator delete are undefined anywhere in the tree - false, a search
that missed the library by name (it is deps/nxdk/lib/libc++.lib,
literally "libc++" not "libcxx"). It is a real, prebuilt static
library, already linked into every C++ target by nxdk's own CMake
toolchain, and it genuinely defines operator new/delete as weak
symbols. Verified by an actual link, not just reading the source: a
new/delete test case linked against libc++.lib plus the base library
set resolves both operators with zero errors. No compat/xbox shim is
needed for new/delete - a caller linking real C++ code just needs
libc++.lib in its library list.

Also corrected two stale claims found while investigating this:
3rdparty/mainui/exports.txt already exists (not "only filesystem/ and
ref/gl/ have one" as an earlier entry claimed), and the exports.txt/
xshlib.py mechanism this project's own earlier work already found is
GNU ld -r/objcopy relocatable-object machinery this project bypasses
for Xbox entirely - it cannot block a link that never calls it.

Found in review: synthetic test files alone don't prove the tool works
on the actual module they were built for - only that the flags work.
Verified against a real 3rdparty/mainui/*.cpp file instead
(utflib.cpp, with mainui's own wscript-declared include roots and
STDINT_H define passed as extra args, deliberately not baked into the
generic script): compiles clean. A second, more representative file
surfaced two more real gaps along the way: the nested miniutl
submodule was never initialised (undocumented anywhere), and
miniutl.h includes the non-standard <new.h> MSVC ships but nxdk's
libcxx does not provide. Neither blocks tools/xbox-cxx.sh itself -
both are separate, unfixed gaps in the menu module specifically,
recorded rather than chased further in this pass.

### Added — X1: opusfile linked in too, 74 undefined down to 64

Fixed the last third-party audio gap: `opusfile`'s `stream.c` called
`GetFileType` and `_wfopen`/`_wfreopen` under a genuinely reachable
`_WIN32` branch, neither of which nxdk has any equivalent of at all - no
partial Win32 surface to shim against, unlike every other compat/xbox
gap so far. Patched instead (`patches/opusfile-xbox-stream.patch`,
auto-applied by `tools/xbox-cc.sh`, same mechanism as the existing
library_suffix patch): always-seekable file callbacks (every FILE* this
platform opens is a real FATX file, never a pipe), plain fopen/freopen
(GoldSrc paths are ASCII, no Unicode filename to preserve), and an
honest ENOSYS for op_fdopen (its fdopen() call has no pdclib equivalent
at all, and nothing in this tree calls op_fdopen anyway). The two calls
that do have a real nxdk equivalent - _fileno/_filelengthi64, for
op_fseek's SEEK_END path - are genuine new compat/xbox functions built
on the already-real GetFileSizeEx, not stubs. All 4 opusfile files now
compile clean.

Linked in alongside the previous 174 third-party objects: 0 duplicate
symbols, 64 undefined - down from 74, because the 10 op_*/
opus_tags_query symbols engine/client/soundlib/snd_ogg_opus.c already
referenced are now supplied. The 64 remaining are exactly the 63 parked
networking symbols plus 1 _main, unchanged. Verified with
tools/xbox-link-check.sh --expect-undefined 64: exact match.

Found in review: making op_get_file_callbacks always report seekable
made op_fseek/op_ftell's own _WIN32 body live for the first time, and
that body type-puns an 8-byte opus_int64 as fpos_t - safe on MSVCRT,
not on nxdk's pdclib, whose fpos_t is a real struct at least 12 bytes
wide. The OP_ASSERT meant to catch the size mismatch compiles to
nothing without OP_ENABLE_ASSERTIONS, which this build never defines -
a silent stack overrun on any seek or tell, not a loud one. Fixed by
widening op_fseek/op_ftell too, onto this project's own _fseeki64/
_ftelli64 (already real, already used by vorbis under the identical
condition) instead of assuming anything about pdclib's fpos_t layout.

Also found in review: tools/xbox-link-check.sh's --expect-undefined
gate only ran when the flag was passed, so the exact command its own
usage comment showed - without the flag - exited 0 on a link with 64
real undefined symbols. Fixed: any undefined symbol with no
--expect-undefined given is now itself a hard failure.

Found in review, second pass: making op_get_file_callbacks always
report seekable had a side effect the first fix missed - op_fseek's
SEEK_END call to fileno() now runs on every seekable open (opusfile
seeks to end during open), registering a borrowed slot in compat/xbox's
descriptor table that only close(fd) ever releases, never plain
fclose() - which is what opusfile's default close callback was. Every
seekable open would have permanently consumed one of 61 available
slots for the process's life. Fixed with a new _r3d_release_stream_fd
and an NXDK-specific close callback that calls it before fclose.

### Added — X1: link the third-party audio codecs in, for real

Fixed the one remaining opus gap the previous entry left open: `celt.c`
needs `_set_abort_behavior`/`_WRITE_ABORT_MSG` under a live `_MSC_VER`
branch nxdk's clang target genuinely takes, and neither exists in nxdk's
pdclib. Added both to `compat/xbox` (declaration in `xbox_prelude.h`,
no-op definition in `xbox_win32_ext.c` — real MSVC only uses this pair to
suppress a debug dialog Xbox has no equivalent of, so the no-op is the
complete behaviour, not a stand-in). `celt.c` compiles clean; opus is now
141 of 142, only `vq_sse2.c` excluded. The main engine sweep baseline is
unaffected (187 ok, 34 failing, unchanged).

With that fixed, compiled all 174 fully-working third-party objects (opus,
vorbis, libogg, MultiEmulator — opusfile still excluded, its own
`stream.c` gap is separate and unfixed) and linked them in alongside the
main engine and the full 18-library nxdk set: 0 duplicate symbols, 74
undefined — down from the 102 measured previously, because the codecs now
supply real definitions the engine build only had references to before.
All 74 are accounted for: 63 parked networking symbols (unchanged), 1
`_main` (no Xbox entry point exists yet), 10 opusfile symbols pending its
own fix.

Built `tools/xbox-link-check.sh` rather than leaving this as a one-off
measurement — the exact "not repository-backed" mistake this project's
review process already caught once this same phase (see the duplicate-
symbol entry below). It composes with `tools/xbox-sweep.sh` (reuses its
last output, does not recompile the engine), compiles the third-party
codecs fresh each run against each submodule's own `wscript` flags,
regenerates libogg's `config_types.h` automatically, links with the real
library set, and takes `--expect-undefined N` to gate on regression versus
improvement the same way `xbox-sweep.sh --expect-fail` already does. Any
duplicate symbol is always a hard failure, no exceptions. Verified against
`--expect-undefined 74` (exact match) and its three failure paths
individually (no prior sweep, count too low, count too high) — each fails
loudly and exits 1 as designed.

### Measured — X1: opus, opusfile, vorbis and MultiEmulator, for real

The previous entry flagged this as "genuinely unknown in either
direction" - whether the four vendored, never-before-built third-party
libraries behind 38 of the still-undefined symbols even compile for Xbox
at all. Answered: **overwhelmingly yes.** Compiled every real source file
each submodule's own `wscript` names (not everything under the directory
- each excludes its own demo/test/CLI files), individually, with
`tools/xbox-cc.sh` plus the extra flags `wscript` declares.

**176 of 179 real library source files compile clean** (173 portable/
generic files plus six x86 SIMD files `opus`'s own `wscript` adds
unconditionally for any `x86*` `DEST_CPU`):

- `3rdparty/MultiEmulator`: 10 of 10, including `RevEmu2013.c` -
  `GenerateRevEmu2013` is real and needs no fix.
- `3rdparty/libogg`: 2 of 2 (`config_types.h` hand-generated for this
  measurement from its `waf 'subst'` template, matching what a real build
  would produce - nxdk's pdclib has `inttypes.h`, so that's the branch a
  real configure check would also pick).
- `3rdparty/vorbis`: 21 of 21, no extra defines needed at all.
- `3rdparty/opus`: 140 of 142 (136 portable + 6 x86 SIMD). One trap: opus
  refuses to compile at all without
  `VAR_ARRAYS`/`USE_ALLOCA`/`NONTHREADSAFE_PSEUDOSTACK` defined, and its
  own `wscript` picks based on `alloca.h`'s availability - which nxdk's
  pdclib does not have. `VAR_ARRAYS` (C99 VLAs, no libc dependency) fixed
  all 37 first-pass failures at once - they were all the identical error,
  not 37 different problems. **Corrected, found in review: an earlier
  version of this measurement excluded the six x86 SIMD files by
  reasoning alone (Pentium III has no SSE2) rather than testing them -
  tested properly, five of six compile clean and one
  (`celt/x86/vq_sse2.c`) fails with a real, loud clang error requiring
  SSE2 target support `-march=pentium3` does not enable**, confirming the
  original reasoning but as a fact about that one file, not a blanket
  exclusion. The other true remaining gap: `celt/celt.c`'s fatal-error
  path calls MSVC-only `_set_abort_behavior`/`_WRITE_ABORT_MSG` under
  `#if defined(_MSC_VER)` - genuinely true for nxdk's target, but neither
  symbol exists in its pdclib.
- `3rdparty/opusfile`: 3 of 4. `stream.c`'s `#if defined(_WIN32)` branch
  needs six Win32 file-query functions nxdk doesn't have - and its own
  `#else` POSIX fallback isn't free either, since nxdk's pdclib also
  lacks `fseeko`/`ftello`. Neither of the file's own two branches is
  usable as written. **Corrected, found in review: "nxdk has no
  file-length query at all" was false - `GetFileSize`/`GetFileSizeEx` are
  real, implemented functions in nxdk's `winapi`.** The actual gap is
  narrower: `_fileno`/`_get_osfhandle`, the bridge from a stdio `FILE *`
  to the `HANDLE` `GetFileSizeEx` needs, which nxdk genuinely does lack -
  only `SEEK_END` needs that bridge, and `compat/xbox/`'s existing
  `r3d_fds[]` file-descriptor tracking is a plausible place to build it.

The three remaining gaps deliberately left unfixed here: whether the
right form is a `compat/xbox` shim (matching `xbox_win32_ext.c`'s
established pattern) or a direct patch to the vendored file (matching
`library_suffix`'s own precedent) is a real decision, not a default one
measurement pass should make on its own; `vq_sse2.c` simply stays
excluded from the SIMD set, a fact rather than a decision.

Whether the five x86 SIMD files that DO compile are safe to actually call
on real Xbox hardware is a separate, unanswered question - a clean
compile does not prove a function tagged for SSE2 target support won't
fault without a genuine runtime feature check, and none of this has run
on real hardware or in xemu.

**No architectural blocker anywhere in 179 files.** The question this
branch answers is not "are three things missing" - it's "can these
libraries work on Xbox at all," and the answer is yes.

### Added — X1: the full undefined-symbol picture, with the real library list

Linked against every base nxdk library `main.exe`'s own `Makefile`
dependency chain accumulates for an `NXDK_SDL=y` build (18 libraries,
already built on this dev host) instead of no libraries at all. Dropped
the undefined-symbol count from 335 to 107, then to 102 after three real,
small fixes:

- **`Q_buildos`/`Q_buildarch`** were never actually missing - their real
  implementation (`3rdparty/library_suffix/src/library_suffix.c`) compiles
  clean for Xbox and was simply never in this project's own sweep/link
  scope. **Corrected 2026-09-08, found in review: an earlier version of
  this entry called this "fixed" when only the manual measurement had
  included the file - nothing in the repository's own documented sweep
  command did, so a future checkout following it verbatim would still
  reproduce the exact same gap.** The canonical sweep command now includes
  it: `tools/xbox-sweep.sh engine filesystem public common compat
  3rdparty/library_suffix/src`, giving 187 of 221 (was 186 of 220) with
  the same 34 failing. `--expect-fail 34` still matches.
- **`Q_stricmp`/`Q_strnicmp`** — a genuine, generic upstream gap: `crtlib.h`
  already anticipates a platform with neither `stricmp` nor `strcasecmp`,
  but nothing ever defined the fallback. Fixed with a portable
  implementation in `public/crtlib.c`, divergence #16, worth sending
  upstream.
- **`COM_CheckLibraryDirectDependency`** — an eleventh landmine of
  divergence #7c's class, found for the first time by an actual link
  rather than a source review: its call site is reachable on Xbox as
  written, but its only definition parses a real Win32 PE import table.
  Widened the same way as the other ten; no replacement needed, Xbox has
  no dynamically-loaded `client.dll` to inspect.

**What remains is now fully accounted for, not just "the rest":**

- 63 symbols: the already-parked networking group (`net_ws.c`,
  `net_chan.c`, `xrcon.c`, `cl_steam.c`, the two `http/` files) -
  expected, already documented, not a new finding.
- 1: `_main` - no Xbox entry point exists yet, X1/X2's own job.
- **38, a genuinely new finding: `3rdparty/opus`, `opusfile`, `vorbis` and
  `MultiEmulator` have never been compiled or measured for Xbox at all.**
  37 audio-codec symbols called from `engine/client/sound/voice.c` and
  `engine/client/soundlib/snd_ogg_vorbis.c` (both compile clean) into
  these vendored-but-never-built libraries, plus `GenerateRevEmu2013`
  (`3rdparty/MultiEmulator`). Unlike the `library_suffix` case, nobody has
  checked whether these actually compile under nxdk's toolchain in either
  direction - real, separate, unscoped work, not attempted here.

Full sweep, against the canonical command as extended above: 187 ok, 34
failing (was 186 of 220, before `3rdparty/library_suffix/src` joined the
canonical set earlier in this same entry) - the 34 real failures are
unchanged, only the denominator moved.

### Added — X1: the ten duplicate symbols, fixed and verified by relinking

`fork-plan.md`'s X1 section measured this back on 2026-09-06 (linking
every non-networking, non-test Xbox object file with `nxdk-link
-errorlimit:0` reported exactly ten duplicate symbols: `FI`, six `FS_*`
names, `_Mem_Alloc`, `_Mem_Free`) but never fixed it. Fixed now, verified
by relinking rather than by trusting the fix reads correctly.

Eight of the ten (`FI` and all six `FS_*` names) are a blanket `#if
XASH_XBOX` `#define` block in `filesystem/filesystem_internal.h`, renaming
`filesystem/*.c`'s real implementations away from
`engine/common/filesystem_engine.c`'s same-named thin wrapper functions -
which call through a dynamically-loaded `fs_api_t` vtable, an architecture
that cannot work on Xbox at all regardless of this collision, since Xbox
has no dynamic library loading. Scoped so it can only affect
`filesystem/`'s own translation units: confirmed by grep that
`filesystem_internal.h` is never included from `engine/` or `ref/`.

`_Mem_Alloc`/`_Mem_Free` could not use the same blanket `#define`:
`fs_interface_t` (`fscallback.h`) has struct *member* fields also literally
named `_Mem_Alloc`/`_Mem_Free` (the engine's own allocator, handed to
filesystem at init so it can route through the engine's real pools instead
of an independent one - correcting `fork-plan.md`'s own earlier
description of these as "the filesystem's own allocator"). A blind
`#define` renamed those struct-member accesses too, caught immediately by
compiling `dll.c`/`filesystem.c` against that version, not assumed safe.
Renamed by hand instead, at exactly the two declarations, two definitions,
and the three `Mem_Malloc`/`Mem_Calloc`/`Mem_Free` macros that reference
them, leaving `g_engfuncs._Mem_Alloc`/`engfuncs->_Mem_Alloc` (the struct
members) untouched.

**Verified by relinking, not just recompiling.** `deps/nxdk/bin/nxdk-link
-errorlimit:0` over every object file a real Xbox build would actually
select reported these same ten duplicates before the fix and zero after -
confirmed the same session. The object set needed narrowing first: a naive
relink over everything `tools/xbox-sweep.sh` compiles found nine
*additional* apparent duplicates (`SNDDMA_*`/`VoiceCapture_*` between
`platform/sdl1/s_sdl1.c` and `platform/sdl2/s_sdl2.c`), which turned out to
be this session's own measurement artifact - a real Xbox build only
selects `platform/sdl2`, never `platform/sdl1` alongside it - not a real
collision. Excluding `filesystem/tests`, `public/tests`, and every
`engine/platform/*` subdirectory other than `sdl2`/`xbox`/`misc` brought
the count back to exactly the original ten.

Full sweep unaffected: 186 ok, 34 failing, unchanged - this branch's own
`tools/xbox-sweep.sh --expect-fail 34` confirms it.

**Not yet known, same as before the fix**: the relink now reports 335
undefined-symbol errors instead of duplicates - `_WinMainCRTStartup`,
nxdk's `winapi`/`hal`/`pdclib` surface, libm, `SDL_*` - because no nxdk
libraries, no entry point, and no game module are on the command line yet.
That is the next measurement X1's own exit criterion needs, not yet
attempted: the real `-libpath:` and library list nxdk's sample `Makefile`s
use.

### Fixed — first Codex review round on the SDL-video-init branch

One finding (medium severity, confidence 0.82), and it corrected an
overclaim in this same branch's own first commit:

- **"SDL owns no video on Xbox" was not true as stated.** The commit
  dropped `SDL_INIT_VIDEO` from `sys_sdl2.c`'s early, defensive `SDL_Init`
  call and claimed that settled the question. Codex traced the *normal*
  client path instead - `CL_Init` → `VID_Init` → `vid_sdl2.c` →
  `SDL_CreateWindow` - and found it still reaches nxdk's real, currently-
  compiling `SDL_xbvideo.c` driver, since `defaults.h`'s generic
  `XASH_SDL` arm still selects `VIDEO_SDL` for Xbox and SDL lazily
  auto-inits its video subsystem on the first `SDL_CreateWindow` call
  regardless of what flags the earlier `SDL_Init` call passed. Traced
  further to confirm the mechanism, not just the claim:
  `XBOX_CreateWindow`/`XBOX_VideoInit` read `XVideoGetMode()`, which
  returns a module-static `VIDEO_MODE vmCurrent = {0,0,0,0}` until
  something calls `XVideoSetMode()` - a genuine 0×0 window and
  display-mode registration if that path ever runs before a real mode
  exists. Not fixed with new code: no Xbox `main()`, link, or running
  binary exists in this tree at all yet, so nothing currently drives
  execution as far as `CL_Init` - the risk is architecturally real and
  worth deciding, but deciding it now would guess at design that belongs
  to whoever wires up a real Xbox entry point (X1/X2). Corrected the three
  places that repeated the overclaim (`sys_sdl2.c`'s own comment,
  `divergences.md`'s #14, `fork-plan.md`'s X0-checklist correction) rather
  than just the one Codex pointed at, since all three were written in the
  same commit from the same wrong premise.

### Added — X0: SDL owns no video on Xbox, and two plan corrections

`engine/platform/sdl2/sys_sdl2.c`'s early, defensive `SDL_Init` call in
`SDLash_Init` now omits `SDL_INIT_VIDEO` for Xbox (`#if XASH_XBOX` /
`#else`, every other `XASH_WIN32` target unaffected) — divergence #14.
Measured, not merely planned: `x0-probe-results.md` already named this "an
explicit change, not a hope" back when the log channel branch's own
`Sys_Error` comment cited it as still outstanding. **This one call does
not decide the whole video-ownership question** — corrected below, found
in the first Codex review round on this branch: the *normal* client path
(`CL_Init` → `VID_Init` → `vid_sdl2.c` → `SDL_CreateWindow`) still reaches
nxdk's real, compiling `SDL_xbvideo.c` driver regardless of this flag,
since SDL lazily auto-inits its video subsystem on the first
`SDL_CreateWindow` call and `defaults.h`'s generic `XASH_SDL` arm still
selects `VIDEO_SDL` for Xbox. What this one call actually avoids: the
early call failing or doing pointless work before anything has established
a real video mode for `XVideoGetMode()` to return, and any `SDL_Init`
failure there demoting the whole engine to `HOST_DEDICATED`. Updated the
one comment (`system.c`'s
`Sys_Error`) that cited this as not-yet-made to reflect that it now is,
while keeping its broader point (any `SDL_Init` failure still reaches that
branch) intact. Verified: full sweep still 186 ok, 34 failing, unchanged.

Two corrections to `fork-plan.md`'s X0 checklist, found while scoping this
work:

- **`-DXBOX`/`sys/types.h`/the five SDL hint constants were already done**,
  from the toolchain-bring-up branch — marked done rather than left as an
  open bullet a future reader would re-investigate for nothing.
- **`VIDEO_XBOX`/`INPUT_XBOX` in `common/backends.h`/`common/defaults.h`
  was never actually X0 scope** — though the reason is narrower than
  first written here; corrected again below, same day, once Codex's review
  showed the video path is currently live in the tree, just not yet
  reachable by any running code. `x0-probe-results.md` §2-3 already says
  Xbox's real video backend (`vid_xbox.c`, against nxdk's HAL framebuffer)
  is X5 work, with its own unmeasured per-frame-cost question attached;
  `INPUT_SDL` needs no Xbox-specific branch at all, since `defaults.h`'s
  generic `#if XASH_SDL` arm already selects it the same as any other SDL
  platform, with no equivalent risk. X0's own exit criterion (boots,
  prints, exits) has never itself called `CL_Init`, so it needs none of
  this *directly* — but "X0 doesn't need it" is not the same claim as
  "nothing in the tree can reach it," which turned out to be false; see
  the correction immediately below for the difference. Moved off X0's
  checklist onto X5's, where the actual work was already scheduled.

`DEST_OS` stays open, but is now backed by a measured cost
(`x0-probe-results.md` §7: `DEST_OS = xbox` plus a one-token widening of
`engine/wscript:181`) with an explicit note that the missing piece is
writing waf's own toolchain-integration module — real, separate,
open-ended engineering on the scale of `nswitch`/`psvita`'s own dedicated
modules, not a ride-along on this branch. Nothing currently blocks on it:
every measurement so far, including this one, has gone through
`tools/xbox-cc.sh` and manual `nxdk-link` invocations instead of waf.

### Fixed — second Codex review round on the log channel branch

The re-review after the first round's fixes (one finding, medium severity,
confidence 0.99):

- Divergence #7c's table row in `docs/r3d/divergences.md` was not valid
  Markdown: the tenth round's "considered and rejected" paragraph had been
  appended inside the row's "Why" cell, after a blank line, which breaks a
  GitHub-Flavored-Markdown table row (every cell must be on one physical
  line). The row's actual Form/Merge risk/Exit cells were still present in
  the raw text, but glued onto the end of a free paragraph rather than
  parsed as table cells by anything reading the structure rather than the
  raw text - leaving the row without a valid Form, Merge risk, or Exit as
  far as any tool or reader parsing the table itself could tell. Fixed by
  moving the paragraph out of the table entirely, to free-standing prose
  right after the "Fact one"/"Fact two" paragraphs (the same place this
  document already uses for exactly this kind of longer note), and closing
  the #7c row with its own short pointer to that paragraph instead. Verified
  every row in the table now has the same pipe count as the header (7 pipes,
  6 columns) with a small awk scan, not just the row that was reported.

This closes the cadence this branch's `AGENTS.md` section now specifies:
one review, fix what's actionable, one re-review, then open the PR. Both
rounds found real, distinct issues and both are fixed; the branch is
PR-ready.

### Fixed — first Codex review round on the log channel branch

`tools/codex-review.sh`'s first review against the branch's fully-fixed
state (one finding, high severity, confidence 0.88):

- `tools/xbox-sweep.sh` explicitly documented its own `exit 0` on a nonzero
  `$fail` as deliberate (the tenth round's fix), but "deliberate" was not
  the same as "machine-checkable" - nothing distinguished the tracked X0
  baseline (186 ok, 34 failing) from a real regression by exit status
  alone, which is exactly the "gets believed" failure mode this file's own
  header already names, one level up from the per-file check that comment
  was originally about. Added an optional `--expect-fail N` flag: if
  passed, the sweep exits 1 on either a regression (`$fail` above `N`) or
  an unflagged improvement (`$fail` below `N`, which means the tracked
  baseline itself is stale and needs updating), and 0 only when they
  match. Off by default, so ad-hoc single-directory sweeps are unaffected.
  Reproduce the current tracked baseline with `MSYSTEM=MINGW64
  /c/msys64/usr/bin/bash tools/xbox-sweep.sh --expect-fail 34 engine
  filesystem public common compat`. Verified all three outcomes
  (matching, regression, improvement) against the real sweep before and
  after the flag, plus the unflagged default and the `common`-alone/empty-
  directory edge cases, to confirm nothing else regressed.

This also closes a gap the tenth round's own fix explicitly deferred to
X1 ("what X1's real link-step gate needs is a check against the tracked
baseline") - Codex's review reached the same conclusion independently,
close enough to now-known-stable that building it did not require
guessing at anything X1 hasn't decided yet, unlike the `Wcon_*`/
`Xbox_NetLogInit` items still deferred under "Still open in X0" below.

### Fixed — eleventh review round on the log channel branch

Two confirmed findings. One fixed here; the other is a real, defensible
architectural improvement that is noted under "Still open" below rather
than implemented mid-transition away from the review workflow that found
it (see "Changed" below for why).

- `tools/xbox-sweep.sh`'s "environment fault" fast-exit (added in the tenth
  round) checked `ok -eq 0`, not whether every failure was actually
  unexplained. The header comment's own stated intent is "if NOTHING
  compiled and nothing explained why" - which means every failure being
  unexplained, not merely zero files fully succeeding. A mass real
  regression (every one of 220 files failing with a genuine, diagnosed
  clang error) that happened to also include one unrelated, truly
  undiagnosed hiccup on a single file would satisfy the old check and exit
  before ever reaching the "distinct errors, by frequency" ranking - hiding
  a real, already-diagnosed regression behind a "check your toolchain"
  message. Fixed to check `mute -eq fail` instead, matching the header's
  own stated rule; a genuinely broken toolchain still mutes every file
  identically, so the case the check exists for is unaffected. Re-verified:
  186 ok, 34 failing, unchanged.

### Changed — review moved from `pr-review.js` to Codex

Eleven rounds on this branch, each running `.claude/workflows/pr-review.js`
(6 lenses fanned out in parallel, then 3 adversarial refuters per raised
finding), cost 25-37 Claude subagent calls and 2-2.6M subagent tokens per
round to keep re-confirming a shrinking branch. Replaced with
`tools/codex-review.sh`, a direct call into the `openai-codex` Claude Code
plugin's companion script, moving review
compute onto Codex instead of Claude's own token budget. `pr-review.js` is
removed; `docs/r3d/codex-review-focus.md` carries forward the specific
traps its lens prompts encoded. See `AGENTS.md`'s "Review on every PR" for
the new process, capped at two rounds instead of running until a round
comes back clean.

### Fixed — tenth review round on the log channel branch

One critical finding, pre-existing on `main` and not introduced by this
branch, but touching a tool this branch has hardened twice already; one
medium, a real functional gap in the log channel feature itself.

- `tools/xbox-sweep.sh` has no `exit` statement anywhere after its
  early-out guards, so on the single most common real outcome - some
  files compile and some do not, exactly what "186 ok, 34 failing" is -
  the script falls off the end of the file and always exits 0, regardless
  of how many files failed. Nothing in this tree currently reads this
  exit code, so it was not a live outage, but it is the same "a sweep
  that cannot fail is worse than no sweep, because it gets believed"
  shape this file's own header already names, here at the aggregate
  script level rather than the per-file one. Not changed to a nonzero
  exit on any failure: X0's own baseline is 186 of 220 compiling, by
  design, so `$fail -gt 0` is the expected, tracked outcome right now,
  not a regression - exiting nonzero on that would make every sweep this
  project has run so far report as failed. Made the fall-through an
  explicit, documented `exit 0` instead, so it is a decision instead of
  an accident, and left a note for X1's real link-step gate to check
  against the tracked baseline rather than "zero failures."
- `Xbox_ConPrint` forwarded the raw engine message straight to `DbgPrint`
  and the UDP broadcast tier, unlike every other `XASH_WIN32` target's
  `Sys_Print` path, which either renders `^N` colour codes as real
  console colours or strips them for a plain-text sink. Ordinary boot
  output like host.c's `"^3%s:^7 %s is enabled"` reached both log tiers
  with the literal `^3`/`^7` still in the text - not an edge case, this
  fires on every ordinary boot. Fixed by calling `COM_StripColors` before
  both sinks, truncating to the last `MAX_PRINT_MSG` bytes first the same
  way the `XASH_WIN32` branch already guards against an overlong message
  (`COM_StripColors` never grows its input, so bounding what goes in
  bounds what comes out).

### Fixed — ninth review round on the log channel branch

No high-severity findings this round. Two medium, both accepted as
documentation gaps rather than functional bugs; five low, all the same
underlying line-number typo (`platform.h:385` should read `platform.h:394`)
duplicated across `divergences.md` and this file by three separate review
lenses independently re-deriving the same fact.

- `tools/xbox-check-nxdk-cc-drift.sh` only ever checks that every flag
  `nxdk-cc` currently passes appears somewhere in `xbox-cc.sh` - it never
  checks the reverse, so a flag `nxdk-cc` has since removed or changed
  would leave `xbox-cc.sh` stale forever with a clean pass. A proper fix
  needs to track which of `xbox-cc.sh`'s flags were hand-copied from
  `nxdk-cc` in the first place (it also carries flags of its own that
  never appear in `nxdk-cc` at all, which a naive reverse check would
  false-positive on) - real new bookkeeping, not a small patch to the
  existing loop, and disproportionate to add on the ninth round of a
  branch this close to done. Documented as a known, accepted limitation in
  the script's own header instead of building the symmetric check now.
- `Xbox_ConPrint` calls the kernel `DbgPrint` synchronously and
  unconditionally on every `Sys_Print`, with no cost measurement or
  `UNVERIFIED` caveat - unlike `Xbox_NanoSleep` and `Xbox_NetLogInit`,
  both of which already carry one for the identical reason (this branch
  has no xemu install and no real hardware to test against). With
  `host_developer` raised, `Con_DPrintf`/`Con_Reportf` call sites can fire
  dozens of times per frame; if `DbgPrint` blocks on a debug-kit-speed
  transport with no debugger attached, that cost lands inside the render
  loop. Added the same `UNVERIFIED` caveat `Xbox_NanoSleep` and
  `Xbox_NetLogInit` already carry, rather than changing the function's
  behaviour - there is no measurement to act on yet, only an assumption to
  stop stating as settled.
- The "Fact one" recount from the seventh round cited `platform.h:164,261,
  385` for the three comment-prose lines that mention `XASH_XBOX` without
  being a real guard directive; line 385 is `Platform_SetTimer`'s closing
  brace, unrelated. The real third line is `platform.h:394`. Fixed in both
  `divergences.md` and this file's own seventh-round entry.

### Fixed — eighth review round on the log channel branch

The one high-severity finding this round was the same "measured nothing,
reported success" shape as the previous round's two, this time in the
drift-checker's own PATH-pin defense, which the previous round had just
finished hardening.

- `tools/xbox-check-nxdk-cc-drift.sh`'s PATH pin changes which `sed.exe`/
  `grep.exe` get resolved as subprocesses, but says nothing about which
  `bash.exe` is already running the script itself - and this project's own
  default Bash tool runs a different msys build (`bash --version` reports a
  different version) than the one the script's header documents. Invoking
  MSYS2's `sed`/`grep` from that mismatched parent process corrupts them:
  reproduced live, running the script exactly as `bash
  tools/xbox-check-nxdk-cc-drift.sh` produced sixteen
  `sed: -e expression #1, char 20: unterminated 's' command` lines on
  stderr from the per-flag escaping step, and the script still printed its
  normal clean-pass line and exited 0 - confirmed with a deliberately
  incomplete `xbox-cc.sh` copy that it silently missed a real flag it
  should have flagged missing. No `sed`/`grep` call in the script checked
  its own exit status, and a version of the corruption was also reproduced
  where `sed` exits 0 with no stderr at all while still emitting silently
  wrong output, so an exit-status check alone would not have been enough.
  Fixed by having the script re-exec itself under the literal, exact
  documented interpreter (`/c/msys64/usr/bin/bash.exe`) if it isn't already
  running under it, so the result no longer depends on how the caller
  invoked it. Verified against both the real pass case and the same
  incomplete-`xbox-cc.sh` repro, run under the previously-broken default
  interpreter: the missing flag is now correctly reported both times.
- Three review lenses independently found the same stale citation: the
  crash-handler guard's comment in `platform.h` named `host.c:1401` for
  `Sys_RestoreCrashHandler`'s call site - a blank line - instead of the
  real call at `host.c:1410`, and described it as "gated only by `-noch`",
  which is false for that site (`Sys_SetupCrashHandler`'s call is gated by
  `-noch`; `Sys_RestoreCrashHandler`'s is unconditional, inside
  `Host_ShutdownWithReason`). Reworded to cite both sites correctly and
  describe each one's actual gating.
- A DHCP-retry-loop citation to nxdk's `net.c:117-127` was off by one in
  all four places it appears (`net_xbox_log.c`, `con_xbox.c`,
  `divergences.md`, and this file) - the real
  `netifapi_dhcp_start`/`while`/`KeDelayExecutionThread` block is at
  `net.c:118-128`; line 117 is the `if (ipv4_dhcp)` guard one line above
  it. Corrected in all four places.
- `net_xbox_log.c`'s comment claimed lwIP's `read()` macro "silently"
  overrides `port.h`'s `read -> _read` macro. False, and a real compiler
  warning proved it: with the file's `#undef read` placed after the
  `<lwip/sockets.h>` include, clang emitted a genuine
  `'read' macro redefined [-Wmacro-redefined]` warning at the include line
  itself, because lwIP's own `#define read(...)` collided with the
  still-live prior definition. Fixed by moving the `#undef` above the lwIP
  includes instead of just rewording the comment - the prior macro is now
  gone before lwIP tries to define its own, so the override genuinely is
  silent, and the warning is gone on recompile.
- `con_xbox.c`'s doc comment described a search as scoped to a literal
  `engine/common/*.c` glob; the slash-star sequence inside that glob text
  read as the start of a nested C comment to the compiler, a real
  `'/*' within block comment [-Wcomment]` warning on every Xbox build of
  this file. Fixed by describing the scope in words instead of typing the
  glob - and the first replacement wording still spelled out the same
  slash-star sequence while explaining the bug, reproducing it in the
  explanation; the working fix had to avoid typing the sequence at all,
  confirmed by recompiling and checking the warning is actually gone rather
  than trusting the second attempt on sight.

Also asked to re-judge the deferred `Wcon_*`-stub question from the seventh
round's unverified finding: re-checked `engine/wscript` for any Xbox target
added since - none exists yet, so the reasoning for deferring that change to
X1 still holds.

### Fixed — seventh review round on the log channel branch

Framed as the final verification pass before opening the PR. Two of the
four confirmed findings are the same "measured nothing, reported success"
shape this branch has already fought twice (`tools/xbox-sweep.sh`'s own
zero-file guard, `tools/xbox-check-nxdk-cc-drift.sh`'s comment-stripping and
substring fixes) — it kept recurring because each fix closed one instance
of the shape without closing the shape itself.

- `tools/xbox-check-nxdk-cc-drift.sh` never checked that its `grep`/`sed`
  extraction from `nxdk-cc` actually produced any flags before declaring a
  clean pass. If nxdk-cc is ever reformatted, emptied, or replaced upstream
  in a way that defeats the extraction pattern, the comparison loop runs
  zero times, nothing is flagged missing, and the script prints its normal
  success line having verified nothing. Added an explicit non-empty check
  that fails loudly instead; verified against both the real pass case and a
  forced-empty-extraction case built by rewriting a scratch copy of
  `nxdk-cc` so no line matches the extraction pattern, restored afterward
  with a byte-diff confirming it against the tracked blob.
- `tools/xbox-sweep.sh`'s "zero .c files found" guard, added two rounds ago,
  only ever checked the aggregate `ok+fail` total across every directory on
  the command line at once. The documented invocation passes five
  directories together (`engine filesystem public common compat`); one bad
  directory among them — typo'd, or an uninitialised submodule — would
  contribute 0/0 and be silently absorbed into a large, legitimate-looking
  total from the other four, so the run would still report what looks like
  a full, successful sweep. Moved the check inside the per-directory loop.
  Testing the fix against the real invocation immediately found a second
  bug in the fix itself: `common/` is one of the five canonical directories
  and is genuinely header-only, so a naive "zero .c files = fatal" guard
  broke the real command it was meant to protect. Corrected to distinguish
  "directory has no files at all" (fatal — the actual typo/uninitialised
  case) from "directory has files but no `.c` files" (a note, not a
  failure) — verified against `common` alone, `common` plus a genuinely
  empty directory, and the full five-directory sweep (186 ok, 34 failing,
  unchanged).
- `docs/r3d/divergences.md`'s "Fact one" paragraph counted every line
  containing the literal string `XASH_XBOX` (`grep -c`, 40 lines) as a
  guard, but five of those forty are comment prose explaining a nearby
  `#if` rather than a directive itself (`host.c:1241,1243`;
  `platform.h:164,261,394`). Recounted actual `#if`/`#elif`/`#ifdef`/
  `#ifndef` directive lines containing the macro: 35, not 40. Both numbers
  now stand separately rather than one being used where the other was
  meant.
- `host.c`'s comment on the `Wcon_InitConsoleCommands` guard stated "Xbox is
  never dedicated, so no replacement is needed" as settled fact. The
  sibling guard in `system.c`'s `Sys_Error`, written the same day, already
  retracts the identical claim — `sys_sdl2.c` sets `HOST_DEDICATED` on any
  `SDL_Init` failure, and Xbox still passes `SDL_INIT_VIDEO`
  unconditionally, so the dedicated branch is plausibly reachable before
  that is fixed. Reworded to point at that comment instead of repeating the
  overclaim; the guard itself needs no change either way, since it only
  excludes a console Xbox never has.

One finding could not be fully verified — the review round hit a session
reset partway through voting, and this finding's refutation votes did not
complete. Judged directly instead: whether the six `Wcon_*` call-site
guards (`host.c` ×2, `sys_con.c` ×1, `system.c` ×3) should route through new
stub functions in `con_xbox.c`, the way `Xbox_ConPrint`/`Xbox_Init`/
`Xbox_NanoSleep` do, rather than `#ifdef` widening. Considered and rejected
for now: unlike those three, giving `Wcon_*` real Xbox definitions under the
same names would collide with `con_win.c`'s definitions at link time unless
`con_win.c` is excluded from the Xbox file set at the build-system level —
and no Xbox `wscript` target exists yet to exclude it from (that is X1's
job). Recorded in `divergences.md` as a considered-and-deferred alternative,
revisited when X1 wires up the real build.

### Fixed — sixth review round on the log channel branch

Two more silent-pass bugs in `tools/xbox-check-nxdk-cc-drift.sh` itself,
found the same way the last round's were: by testing the failing case, not
by trusting the passing one.

- It matched each nxdk-cc flag against `xbox-cc.sh`'s **whole file text,
  comments included**. A flag this file's own header comment merely
  *quotes* while explaining its history — `-fuse-ld=nxdk-link`, discussed
  two paragraphs above the real exec line that carries it — would have
  read as "present" even with the real line deleted. Now strips `#`
  comments before comparing; verified by deleting the real line and
  confirming the checker catches it despite the comment mentioning the
  same text.
- `grep -qF` (plain substring match) reported no drift after deleting
  `-I "$NXDK_DIR/lib"`, because its normalised text is a literal prefix of
  `-I "$NXDK_DIR/lib/xboxrt/libc_extensions"` two lines below, which
  survived. Switched to a boundary-checked match (flag text, then
  whitespace or end-of-line); verified the same deletion is now caught, and
  that a clean run still passes.
- The flag-extraction regex only recognised `-x` single-letter-prefixed
  flags, silently invisible to any future GNU-style `--long-option` nxdk-cc
  might add. Widened, no observable behaviour change today (nxdk-cc has
  never used one).

Also: `Xbox_NetLogInit()` had no re-entry guard against a caller retrying
after `nxNetInit()` itself succeeded but a later step (`socket()`,
`setsockopt()`) failed — a second call would re-invoke `nxNetInit()`,
whose own `netifapi_netif_add()` has no protection against adding the same
interface twice, and lwIP's `netif_add()` asserts on exactly that rather
than failing cleanly. Unreachable today (nothing calls this function yet),
closed now with a static "attempted" flag so the constraint the doc comment
already stated ("call once") is enforced in code, not only in prose.

And a documentation count: `docs/r3d/divergences.md`'s "40 mentions" for
the seven `XASH_XBOX`-guarded files undercounted a literal token count by
one (`host.c:1243` names the macro twice on one line) — reworded to state
what was actually counted, matching-line count, which is also the more
useful number given the per-file tallies elsewhere in that document already
use it.

186 of 220 still compile. No regressions.

### Fixed — fifth review round on the log channel branch

A tenth landmine of the exact class divergence #7c already tracks nine of:
`Sys_SetupCrashHandler`/`Sys_RestoreCrashHandler` (`platform.h`) declared
under bare `XASH_WIN32`, called unconditionally from `Host_Main`
(`host.c:1089`, `:1401`), only defined in `engine/platform/win32/
crash_win.c`, which needs `<dbghelp.h>`/`<psapi.h>` - both absent from
nxdk, already named as a gap in `x0-probe-results.md:242`. Widened to
exclude Xbox; the existing empty stub is the right answer, since there is
no minidump target to hand a crash report to yet. `XASH_CRASHHANDLER=0`
(`engine/wscript:83`, also passed by `tools/xbox-cc.sh`) is not an existing
mitigation - nothing checks that macro's value anywhere in the tree.

Two review rounds in a row have now each claimed an exhaustive,
mechanism-based search and each still missed one site of this same class.
Recorded as its own point in `docs/r3d/lessons.md`, not folded into the
existing entry: "checked individually" is a claim about the search that was
run, not a guarantee about what remains.

Also: `tools/xbox-check-nxdk-cc-drift.sh`'s own `PATH` pin, added last
round specifically to make its `grep`/`sed` results deterministic, assumed
MSYS2 is installed at `C:\msys64` without checking - true for this project
(the same assumption `tools/xbox-cc.sh` already makes), but a silent no-op
on a machine where it is not, which would have reintroduced the exact
wrong-binary failure mode the pin exists to close. Now checks the pinned
path exists first and fails loudly, with a clear message, if not - verified
both directions, a normal run and one pointed at a path that does not
exist.

186 of 220 still compile. No regressions.

### Fixed — fourth review round on the log channel branch

Two robustness gaps in the tools this branch's own numbers depend on, both
silent-pass failure modes of the exact shape `docs/r3d/lessons.md` already
has several entries about:

- **`tools/xbox-sweep.sh` printed a clean-looking "compile: 0 ok, 0 failing"
  and exited 0** for a typo'd directory argument or an existing-but-empty
  one (an uninitialised submodule). `ok`/`fail`/`mute` all start at zero, so
  every guard that would normally catch a broken run is itself skipped when
  nothing ran at all. Now checks that at least one `.c` file was examined
  before printing anything, and exits 2 with a clear diagnosis if not.
- **`tools/xbox-cc.sh`'s `library_suffix` auto-patch preflight was silently
  skipped**, with no `XBOX_CC_PREFLIGHT_FAILURE` marker, when `build.h`
  does not exist at all — the exact "submodule never initialised" case the
  marker exists to catch, distinct from "exists but unpatched". Every file
  would instead fail individually on clang's own "file not found", the
  identical diagnosis repeated once per translation unit rather than the
  intended single, fast, clearly-labelled failure.

New tool: **`tools/xbox-check-nxdk-cc-drift.sh`**, checking `xbox-cc.sh`'s
hand-copied flags against `deps/nxdk/bin/nxdk-cc`'s real ones — the comment
that said "keep this in sync if nxdk is ever updated" was advice for a
human to notice, not a check anything ran. Not wired into the sweep (real
overhead, no reason to run on every one of 220 compiles); run by hand after
any `nxdk` submodule bump. Getting *this* script right took two more rounds
of the same lesson: the first version passed clean on the first try because
it had never been shown a real discrepancy, and once one was injected, two
more bugs surfaced — an unpinned `PATH` let the invoking shell's `grep`/`sed`
silently mismatch depending on which binaries it resolved to, and the first
two attempts to inject a test flag via `printf`/`sed` one-liners hit the
identical shell-escaping trap the checker itself exists to survive, silently
injecting nothing and reporting a false pass.

Five precision findings from this same round: two wrong `Sys_DebugBreak`
call-site line citations in `platform.h` (one off-by-one, one caller —
`Sys_Warn` — missing entirely) and a `net_xbox_log.c` comment that implied a
concrete `write`/`close` name collision in that specific file, when the only
thing actually shadowed there is `read` (the file never includes
`compat/xbox/unistd.h`, so there is nothing else to shadow yet — the
`#undef` for `write`/`close` is correct, prospective hygiene, not a fix for
a bug present in this file).

186 of 220 still compile. No regressions.

### Added — X0 part 2: the log channel

`engine/platform/xbox/` exists now — real content, not a stub. Both tiers the
plan named: nxdk's kernel debugger channel, then UDP broadcast. The ruler
(per-subsystem `rdtsc` counters) is not part of this.

- **`engine/platform/xbox/con_xbox.c`** — `Xbox_ConPrint`, over nxdk's
  `DbgPrint` (`xboxkrnl.h`), not `hal/debug.h`'s `debugPrint`. The two get
  confused: `debugPrint` draws onto the framebuffer and needs a video mode,
  which is X4/X5 work; `DbgPrint` is the kernel debugger's own channel,
  live from process start with no video mode required, and xemu surfaces it
  to its own console. Divergence #12.
- **A landmine found while wiring it in.** `Sys_Print`
  (`engine/common/system.c`) reaches `Wcon_WinPrint` on every `XASH_WIN32`
  build, `XASH_XBOX` included, and that function's only definition
  (`engine/platform/win32/con_win.c`) does not compile for Xbox and is
  selected by no `DEST_OS` this target would choose. Guarding it out with no
  replacement would have left `Sys_Print` silent on Xbox —
  `sys_con.c:289`'s "Wcon does the job" comment is why no POSIX-style stdout
  fallback runs when `XASH_WIN32` is true, and there is nothing for Xbox to
  fall back to either. Fixed by routing Xbox to `Xbox_ConPrint` instead.
  Divergence #7c.
- **`engine/platform/xbox/net_xbox_log.c`** — UDP broadcast over nxdk's own
  lwIP. `deps/nxdk/samples/httpd_bsd` already proves BSD sockets work on this
  target; `libnxdk_net.lib` was already built. Broadcasts on port `30865`
  rather than taking a configured destination, since a dev machine's IP on a
  dev network is not a constant worth hardcoding. **Deliberately not wired to
  fire automatically** — `nxNetInit`'s DHCP wait genuinely blocks for up to
  ~10 seconds (`nxdk/net.c:118-128`), and firing that from inside the print
  path used for the very first line of boot output would freeze exactly what
  this phase exists to make visible. `Xbox_NetLogInit()` is exposed for a
  future startup sequence to call once, deliberately. Divergence #13.
- **A second, smaller landmine, found building the first.** `<lwip/sockets.h>`
  needs two `-I` flags `tools/xbox-cc.sh` did not have, and once added it
  collided twice with `compat/xbox/xbox_prelude.h`: `socklen_t` was typedef'd
  as `int` with no `SOCKLEN_T_DEFINED` guard, so lwIP's own `typedef u32_t
  socklen_t` — which explicitly checks for that exact guard macro
  (`lwip/sockets.h:115-118`) — redefined it instead of yielding; and
  `write`/`close` (plain, unprefixed names declared in `compat/xbox/
  unistd.h` and implemented in `xbox_posix_io.c`, with `_write`/`_close`
  as thin wrappers calling *through* to them — **corrected 2026-09-07, found
  in review: this entry previously described `common/port.h` as mapping
  `read`/`write`/`close` to underscore-prefixed names; it only does that
  for `open` and `read` — `write` and `close` have no such mapping at all,
  and the actual hazard is lwIP's macros shadowing the plain
  `compat/xbox/unistd.h` declarations directly**) are silently shadowed by
  lwIP's own POSIX-name macros for the rest of any translation unit that
  includes both, in inclusion order. Fixed the `socklen_t` half with the
  guard macro lwIP already documents for it; defended the shadowing with an
  explicit `#undef` in `net_xbox_log.c`, since this file does not want the
  file-I/O names to mean anything at all rather than risk them quietly
  meaning something else.
- **An earlier version of `xbox_prelude.h`'s `socklen_t` comment was wrong,
  and has been standing since X0 part 1.** It said "nxdk has no sockets
  headers at all" — true of what the engine's own `net_ws.c` needs, not of
  what exists in the tree, which this file's `<lwip/sockets.h>` include now
  demonstrates. Corrected in place rather than left to mislead the next
  reader.
- **186 of 220 `.c` files now compile** (`find engine filesystem public
  common compat -name '*.c' | wc -l`; was 184 of 218). This line was wrong
  twice while this branch was under review, both times by adding a count of
  *files carrying `XASH_XBOX` guards* — which includes `platform.h`, a
  header, not a `.c` file — into this `.c`-only compile count as if the two
  were the same denominator. They are not. See `docs/r3d/divergences.md`'s
  "Shape as of X0" for both facts kept properly separate: seven files carry
  guards (40 lines carrying the macro, up from 22), of which six are `.c` files; those six
  plus the two new ones (`con_xbox.c`, `net_xbox_log.c`) plus 178 untouched
  upstream `.c` files account for all 186 that compile.

### Changed — the plan, after checking it against the code

`docs/r3d/xbox-performance-review.md` read `optimization-transfer.md` and the
X-phases against `ref/gl`, the baker and nxdk. Of its nineteen real items,
four were already in the tree and correct, one is present but disabled, one
is present but wrong (`TextureOpt.cpp`'s DXT swizzle), and seven are baker
work with no owning phase. What changed as a result:

- **XB — Baker output for the NV2A**, a new phase between X5 and X9. Two
  censuses first (lightstyles per face, bones per mesh), then the format:
  vertex layout, per-cluster per-material lists, RGB565 pages with a stated
  lightstyle policy, bone partitioning against the 192-register VP20 palette,
  the DXT block-row fix, post-T&L index order. Exit: PC `ref/gl` renders from
  the baked render-half.
- **X0 gains the ruler.** Per-subsystem `rdtsc` counters and a frame-time line
  ship with the log channel, not in X11.
- **X5 and X9 exit with a number** — software frame time on `c1a3` and
  `crossfire` as the baseline, hardware frame time against it.
- **X9 loses "the real variance."** The reference has no per-pixel lights;
  what remains is raising per-vertex lights from four to eight and measuring
  linear against swizzled lightmap updates. Memoised bone setup joins X9 as
  new work — the reference is eager.
- **D8** in `design-stage.md`: the two censuses, as a design question that
  gates XB.
- **`reference-renderer-specialist`** added, with a matching `reference` lens
  in `pr-review`. Its only job is to check "the reference does X" against
  `ref/gl` and cite the line. Three of the four false claims the review found
  were of that shape, and no existing specialist owned the question.
- `console-performance-specialist` no longer claims the reference memoises
  bone setup. It does not.

### Added — X0 part 1: toolchain bring-up and the POSIX compatibility layer

**Not all of X0.** Its exit criterion is an XBE built from this tree that boots
in xemu and prints. That is not met yet — see "Still open" below.

- **nxdk builds on the dev host.** 19 libraries, 7.1 MB, including `libSDL2`
  (1.5 MB), `libpbkit`, `libpdclib`, `libnxdk_hal`, `libSDL_ttf` and `libc++`.
- **The toolchain reaches an XBE.** nxdk's `hello` sample builds end to end —
  `nxdk-cc` → `nxdk-link` → `cxbe` → `extract-xiso` — producing a
  102,400-byte `default.xbe` and an ISO.
- **`XASH_XBOX`**, as a sub-platform of `XASH_WIN32`, keyed on nxdk's `-DNXDK`
  and mirroring how `XASH_ANDROID` nests inside `XASH_LINUX`. Nine lines,
  carried as a patch in `patches/` because committing it needs a fork of
  `FWGS/library-suffix`. Divergence #5.
- **`compat/xbox/`** — POSIX file I/O over Win32. nxdk provides none: no
  `fcntl.h`, `sys/stat.h`, `sys/types.h`, `dirent.h` or `io.h`, an empty
  `unistd.h`, and a `stat.h` whose declarations sit inside an `#if 0`.
  Implements `open`/`read`/`write`/`lseek`/`close`/`fsync`/`ftruncate`,
  `stat`/`fstat`/`mkdir`/`unlink`/`rmdir`/`access`, and
  `opendir`/`readdir`/`closedir`, over `CreateFileA` and friends.
- **`compat/xbox/xbox_win32_ext.c`** — the Win32 surface nxdk omits but the
  engine calls: `GetModuleFileNameA/W` off `XeImageFileName`, a real UTF-8
  `MultiByteToWideChar`/`WideCharToMultiByte` pair (surrogate pairs, U+FFFD on
  malformed input, `NULL` destination for sizing), `utime`, `GetUserNameA/W`,
  `GetModuleHandleA/ExA/ExW`, and `localtime_s`/`gmtime_s` in MSVC's argument
  order rather than C11's.
- **184 of 218 translation units compile for Xbox.** Reproduce with
  `MSYSTEM=MINGW64 /c/msys64/usr/bin/bash tools/xbox-sweep.sh engine filesystem
  public common compat`.

  The 34 failures, classified by tracing `engine/wscript:173-194` rather than
  by reading directory names:

  | Group | Count | Selected by the build? |
  |---|---|---|
  | Networking — `cl_steam.c`, `net_chan.c`, `net_ws.c`, `xrcon.c`, `common/http/`×2 | 6 | Yes. Parked deliberately, both routes priced |
  | `platform/posix/` — `sys_posix.c`, `con_posix.c` | 2 | Yes, unless `wscript:181`'s denylist is widened. See below |
  | `platform/{android,apple,dos,irix,linux,nswitch,psvita,sdl1,sdl3,win32}` and `misc/{kmalloc,sbrk}.c` | 26 | No, under any configuration this target would choose |

  An earlier revision of this entry said "29 are platform backends for targets
  that are not Xbox" and put `platform/misc/lib_static.c` among them. That was
  wrong, and wrong in the way that matters — see **Fixed**.
- **`tools/xbox-env.sh`**, **`tools/xbox-cc.sh`**, **`tools/xbox-sweep.sh`** and
  **`tools/gen-library-tables.sh`** — the build recipe, a single-file compile
  harness, a whole-directory sweep that judges by the compiler's exit status,
  and the generator for the one build artifact waf would otherwise produce.
- **`.claude/workflows/pr-review.js`** — five review lenses (posix, xbox,
  upstream, build, perf) selected from the diff, each finding then put to three
  adversarial refuters that default to rejecting it. Required on every PR; see
  `AGENTS.md`.
- **`.gitattributes`** extended so our text files normalise to LF.

### Fixed

- **Four undefined symbols the engine needs to load the game at all.**
  `-DXASH_STATIC_LIBS=1` sends `common/defaults.h:102-105` down the
  `LIB_STATIC` branch, which makes `platform/win32/lib_win.c` compile to an
  empty object and leaves `platform/misc/lib_static.c` as the only definition
  of `COM_LoadLibrary`, `COM_FreeLibrary`, `COM_GetProcAddress` and
  `COM_FunctionFromName`. That file could not compile: it includes
  `generated_library_tables.h`, which waf writes at
  `scripts/waifulib/xshlib.py:112` and the Xbox build bypasses waf entirely.
  `tools/gen-library-tables.sh` now generates it into `.sweep/gen/` rather than
  into the tree, so it cannot go stale against the library list — which is
  empty today and gets `client` and `server` in X2/X3.

  The X1 link probe could not have caught this, because `lib_static.o` was not
  in the object set it linked.
- **`stat()` cost five kernel transitions per probe where one does.** It called
  `GetFileAttributesA`, then opened the file with `CreateFileA` +
  `GetFileInformationByHandle` + `CloseHandle` just for size and mtime.
  `GetFileAttributesExA` returns attributes, size and all three timestamps from
  a single `NtQueryFullAttributesFile` (`deps/nxdk/lib/winapi/filemanip.c:37`).
  This is the hot path: `FS_FindFile_DIR` (`filesystem/dir.c:396`) probes every
  plain searchpath on every file lookup.
- **`tools/xbox-sweep.sh` deleted the cause of its own failures.** It kept only
  lines matching `error:`, so a broken toolchain — `set -u` on an unset
  `NXDK_DIR`, or clang off `PATH` — reported `FAIL <file>   0 errors` for every
  file, an empty summary, a zero-byte log and exit 0. Indistinguishable from a
  real sweep. Unexplained failures now go to `.sweep/unexplained.txt` verbatim,
  are counted separately, and a sweep with no successes and no diagnostics
  exits 2. Third time this tool has reported a false result.
- **`.claude/workflows/pr-review.js` reported outages as clean reviews.** A null
  scope step collapsed lens selection to the one lens whose predicate is
  `() => true`, under a log line shaped like a normal run; and `parallel()`
  resolving dead refuters to `null` made "no refuter answered" arithmetically
  identical to "every refuter refuted it". Scope now returns a schema-validated
  path list and throws when empty, skipped lenses are logged by name, findings
  with fewer than two answering refuters come back as `UNVERIFIED` instead of
  being dropped, and a run where nothing could be verified throws rather than
  returning `confirmed: []`.
- `filesystem/sys.c` calls `lseek` and `dup` without including `<unistd.h>`,
  relying on it arriving transitively. Correct everywhere, not just here —
  divergence #7, and a candidate to send upstream.

### Changed

- `filesystem/sys.c`: ten `#if XASH_WIN32` blocks widened to
  `#if XASH_WIN32 && !XASH_XBOX`. They use the MSVC wide-char CRT, which nxdk
  does not have, and each already had a POSIX `#else` branch. Divergence #6.

- `engine/common/system.c`: `Sys_WaitForQuit` pumped a message queue that does
  not exist and would have hung; `Sys_CanRestart` joins iOS in returning false,
  because nxdk has no `exec` and `XReboot()` returns to the dashboard rather
  than relaunching the title. Divergence #7b.
- `engine/client/identification.c`: five blocks widened. The machine
  fingerprint is built from the Windows registry; the file-based path the
  non-Windows targets already use is correct here. Divergence #8.
- `engine/platform/sdl2/vid_sdl2.c`: four blocks widened. Window icons need an
  `HWND`, and nxdk's SDL has no window manager. Divergence #9.

### Measured

- **X1's duplicate-symbol risk, retired at ten symbols.** Linking all 157
  non-networking objects with the real `nxdk-link` flags produces ten
  collisions, every one of them the same boundary: `filesystem/filesystem.c`
  defines the FS API and `engine/common/filesystem_engine.c` re-exports the
  same names as wrappers. Ten names is `llvm-objcopy --redefine-sym` territory,
  and `xshlib.py` is bypassed entirely on Xbox. A first run said 21 — eleven of
  those were `_main`, from unit-test programs that have no business in an
  engine link. Recorded because the larger number is the sort that gets quoted.

### Still open in X0

- **`Xbox_NetLogInit()`'s permanent-failure re-entry guard, found by the
  first real run of `tools/codex-review.sh`.** `s_net_log_attempted`
  conflates two failure domains: it exists to stop a second `nxNetInit()`
  call (whose own `netifapi_netif_add()` asserts on an already-added
  interface), but also permanently blocks retrying just the socket setup
  if `nxNetInit()` succeeds and only `socket()`/`setsockopt()` fails
  afterward - a real path, since nxdk's lwipopts.h caps the whole engine at
  lwIP's default `MEMP_NUM_NETCONN` of 4. Splitting "network stack ready"
  from "socket ready" into separately retry-eligible state would fix it.
  Documented in the function's own doc comment rather than fixed: nothing
  calls this function yet (that's X1/X2 work), and designing retry
  semantics for a caller that doesn't exist risks guessing wrong about what
  it actually needs. Revisit when that caller is written.
- **A cheaper form for divergence #7c's six `Wcon_*` no-op call sites,
  found in the eleventh review round, not yet applied.** `system.c`,
  `sys_con.c` and `host.c` each carry `#ifdef` widening around a
  `Wcon_ShowConsole`/`Wcon_DisableInput`/`Wcon_DestroyConsole`/
  `Wcon_InitConsoleCommands` call with nothing Xbox-specific to do. The
  same diff already proves a cheaper pattern two guard blocks later in
  `platform.h`: `Sys_SetupCrashHandler`/`Sys_RestoreCrashHandler` are
  called *unconditionally*, with zero `#ifdef` at either call site,
  because `platform.h` supplies a `static inline` empty-body fallback for
  platforms that don't support it — `static` linkage, so it cannot collide
  with `con_win.c`'s real definitions the way giving `Wcon_*` real bodies
  in `con_xbox.c` would. Applying the same fallback to the four `Wcon_*`
  functions would remove all six call-site edits from three upstream
  files, containing the entire divergence in `platform.h`. Not done in the
  eleventh round because the review workflow that found it was being
  replaced with Codex in the same session (see "Changed" above); revisit
  before this branch's PR closes.
- `engine/platform/xbox/` now has two files (`con_xbox.c`, `net_xbox_log.c`).
  Whether `engine/platform/sdl2/` can host the rest is still open.
- **The log channel's two tiers both exist**, but nothing calls
  `Xbox_NetLogInit()` yet — there is no Xbox startup sequence to call it from.
  That is genuinely X1/X2 work (a real `main()`, wired to `host.c`'s loop, on
  whichever side of static linking gets resolved first), not this phase's.
- **A structurally different landmine shape, checked exhaustively rather
  than assumed, and mostly a false alarm.** `platform.h` declares six
  functions as plain externs with no inline dispatcher — every platform
  defines them directly in its own `sys_<platform>.c`, unlike `Wcon_*`/
  `Win32_*`, which platform.h dispatches to by name. Checked all six
  individually rather than assuming they share one fate: `Platform_
  DoubleTime`, `Platform_Sleep` and `Platform_MessageBox` are *also*
  defined in `engine/platform/sdl2/sys_sdl2.c` (under `XASH_TIMER ==
  TIMER_SDL` and `XASH_MESSAGEBOX == MSGBOX_SDL`, both of which this Xbox
  build selects, and that file compiles clean) — not landmines at all.
  `Platform_SetStatus` and `Platform_ShellExecute` genuinely had no
  Xbox-reachable definition; fixed with empty bodies in `con_xbox.c`, same
  shape as `Xbox_Init`/`Xbox_Shutdown`. `Platform_DebuggerPresent` turned
  out to be the `Wcon_`/`Win32_NanoSleep` shape after all — dispatched from
  an inline `Sys_DebuggerPresent`, `#if XASH_LINUX || XASH_WIN32` with no
  `!XASH_XBOX` — fixed the same way as those.
- **The ruler** — per-subsystem `rdtsc` counters and a frame-time line in the
  same channel, per the performance-review PR. Not started.
- `DEST_OS`, `VIDEO_XBOX`/`INPUT_XBOX` in `common/backends.h`, the
  `common/defaults.h` branch.
- Root-directory detection: `SDL_GetBasePath` returns `NULL` on nxdk and there
  is no `getcwd`, so the engine has no way to find `valve/` yet.
- Networking — six translation units, parked after pricing both routes.
  Upstream's `XASH_NO_NETWORK` stub is not self-contained (it declares the
  socket structs opaquely, and `net_ws.c` uses them by value). The lwIP route
  does **not** need a new port layer — nxdk already ships `lwipopts.h` and
  `arch/cc.h`, and this phase's own log channel proves BSD sockets work here
  — corrected, `fork-plan.md`'s networking-parked section previously said
  otherwise. What's left is whether `net_ws.c`'s exact struct usage compiles
  against `<lwip/sockets.h>` unmodified, which is untried. Completing
  upstream's stub and sending it back is still the right fix regardless.
- XBE packaging from this tree rather than from an nxdk sample.
