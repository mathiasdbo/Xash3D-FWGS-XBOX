# Lessons

Rules earned by getting something wrong. Every entry exists because a mistake
was made, found, and generalised.

**This file is read at the start of every session.** It is the only mechanism
that stops the same mistake being made twice, so it is worth keeping short and
sharp. See `/AGENTS.md`, "Learning from mistakes", for when and how to add to
it.

## Meta-rules for writing entries here

Follow these or the file degrades into notes nobody reads.

1. **Lead with the rule, in the imperative.** ALWAYS or NEVER where the rule
   really is absolute. The reasoning comes after, in one sentence.
2. **Generalise past the specific bug.** "Check the header field order" is a
   note. "NEVER trust a format's field order from a comment — read the struct
   that writes it" is a rule.
3. **Keep the evidence.** One `file:line` or one measurement, so a future
   reader can tell whether the rule still applies.
4. **One rule per entry.** If it needs "and", it is two.
5. **Delete rules that stop being true.** A stale rule costs more than a
   missing one, because it gets followed.
6. **Do not add a rule for a one-off slip.** Only for a mistake with a shape
   that will recur.

---

## Rules
### NEVER call a host-tested module tested while its engine glue allocates and frees on its own

x9-audio-music-bake (#171) host-tested every line of the baked-music
decoder and stream, 25 mutants deep, and shipped a leak in the 150-line
glue that wraps it: `Stream_FreeXwav` freed the read-ahead and the
`stream_t` but not the private state between them, about 400 bytes of
`host.soundpool` on every track change, loop and A/B flip. Two reviews
found it; no test could have, because none ran the glue. **When the pure
core is tested and the glue is not, keep the glue to one allocation per
object (carve the parts out of it) so its free cannot miss a piece, or
give the glue's open/free pair a test of its own.**

### ALWAYS make a negative test assert the failure it expects, not only a non-zero exit

The same branch's harness for `xbox-xiso.sh`'s music validator reported
seven of eight failure cases passing on its first run - because the
function under test had not been extracted at all and every call failed
with "command not found" (exit 127). A later fixture then failed a case
for an unrelated reason (a CRLF manifest) and still counted as a pass.
**Match each failure case's own message (or exit code) and check the
function exists before the first case; a test that any breakage
satisfies tests nothing.**

### NEVER census a studio model's texture flags without its T.mdl

x9-perf-chrome-vp (#170) first counted the retail chrome meshes with a
script that read each .mdl's texture table and skipped models with none,
and concluded that scientists and guards have no chrome. Those models
keep their textures in a separate `<name>T.mdl` (`numtextures` 0 in the
main file); read with it, `barney.mdl`'s body has a 58-vertex chrome mesh
on a 382-vertex submodel, `scientist.mdl` a chrome needle hand and
`hgrunt.mdl` a chrome shotgun head - and the guard on the c1a0 tram was
the whole of that map's chrome cost (0.22 ms a frame in xemu). **When a
census reads per-texture data, load the companion T.mdl for every model
whose own texture count is 0, and count the models skipped; a census
that silently skips a class answers a different question.**

### NEVER port a depth-state call by its arithmetic before reading which value the target's depth test compares

ref/nv2a ported `R_DrawViewModel`'s `glDepthRange(0, 0.3)`
(`gl_studio.c:3348`) as a viewport matrix with z in [0, 0.3 x 65536] -
exact as z arithmetic, and it did nothing: the NV2A draws with
W-buffering, where the depth it tests is the interpolated w (xemu
`pgraph/glsl/psh.c`). The same unread fact hid a one-unit depth step
that let a guard's shirt through his vest, and that the projection's
zNear, whatever its value, clips nothing (xemu draws with
`GL_DEPTH_CLAMP`; the only near cut is `CLIP_MIN` on w), so a shotgun
shell part a unit from the eye filled the screen (x9-fix-studio-visuals,
divergence #160). **Before porting a
depth range, near plane, polygon offset or depth format, find the line
in the hardware model (xemu's shader generator) that computes the
compared depth and the cull, and check the ported value reaches it.**

### ALWAYS check that a reference shot shows something only the reference renderer can produce

x9-fix-studio-visuals took two ref/soft "reference" runs in xemu by
setting `r_refdll "soft"` in the disc's `video.cfg`; both were ref/nv2a
(0 pixels different from an nv2a frame), because the HDD's saved
`video.cfg` shadows the disc's, and one had been compared as if it were
the reference. A real ref/soft frame is unmistakable (untextured world,
checkerboards). **Before comparing against a reference picture, name
the feature of it that proves which renderer drew it - or diff it
against the renderer under test and expect a difference.**

### NEVER derive one count from another without reading every early return between their increments

x9-perf-efx (2026-10-08) added `beam_ents` so the report could split
`beams` into temp and entity beams, on the claim that ref/gl's
`r_stats` counts an entity beam twice (`gl_beams.c:1226` after
`R_BeamDrawCustomEntity`, `:1071` at the end of `R_BeamDraw`). The
reference review found the second increment sits behind seven early
returns (`R_BeamCull` among them), and beam entities reach the list
without a frustum test, so an off-screen `env_beam` is counted once and
the report's "temp" column, `beams - 2 * beam_ents`, could go negative.
**Before a formula subtracts or multiplies one counter by another,
trace each increment's path and list the returns that skip one but not
the other; if any exist, count the quantity you want directly.**

### NEVER call a rebuild idempotent before checking that it reads nothing it writes

x9-perf-clents (2026-10-07) skipped `CL_SetSolidEntities` calls whose
inputs had not changed since the last rebuild, reasoning that the same
inputs give the same lists. The verification build's side-by-side check
found 3 of 7,772 skips wrong: `CL_CopyEntityToPhysEnt` picks
`pe->model` or `pe->studiomodel` from `pe->solid` (`cl_pmove.c:341` in
the reference) before it writes `pe->solid = state->solid` (`:357`), so
the first rebuild after a slot held another entity differs from the
second. A function that fills a buffer can read the buffer's old
contents; the key covered every input except that one. **Before
treating "same inputs" as "same output", read the function for any read
of its destination before the write, and check the claim with a run that
does the work anyway and compares.**

### NEVER call a scripted equivalence run a proof before counting what it exercised

x9-perf-pmove's first movement-log comparison (2026-10-06) scripted a
ladder, water and a c1a1b walk by teleporting the player with
`ent_fire 1 set origin` and `map c1a1b`. Two of the three spots left the
player stuck (51-110 position tests a frame, origin never changing), so
"identical" there meant two builds standing still, and the ladder
segment had 0 frames on a ladder. A coverage pass over the log (frames
with movetype FLY, water level 1 and 3, airborne, ducked, standing on an
entity, origin changed) found it; spots chosen from the map's own hull
data (a throwaway script walking the archive's clip nodes) and a turn toward the ladder fixed it.
**Before reporting an A/B log as equivalence, tabulate per segment the
states it was meant to exercise and the frames it actually moved.**

### ALWAYS read the frame after a stall before calling the stall gameplay

A recorded frame's flags are the state it started in, so the frame in
which the player opened the pause menu is an in-play frame. The
x9-perf-stalls route recording (2026-10-06) listed four ~320 ms
non-autosave stalls as play; three were the first pause after a map
load (the next frame starts with `MENU`), the menu rebuilding what
divergence #127 releases for each map, and earlier recordings' 320-430
ms stalls had all been filed as autosaves without that check. **Before
attributing a stall, list what the frames on both sides of it say - the
next frame's flags, the save spans, the map markers - and match it
against what the player did (a pause, a manual save, a load, a death)
before looking for an engine cause.** `tools/frametime-report.py` now
marks such a stall `opens-menu`.

### ALWAYS count the kernel I/O calls and check the buffer's alignment before blaming the bytes of a slow Xbox file write

The autosave stall looked like a big-file problem (a 939 KB .sav). The
x9-perf-autosave spans showed otherwise: in xemu, 72 writes of 260 or 4
bytes cost 64 ms and 36 file-sized writes 123 ms - about 1 ms per
`FS_Write` whatever it carried (each is `write` + `lseek`, synchronous
kernel calls) - and the same 36 reads took 81.5 ms into a buffer not on
a page boundary against 28.9 ms into one that was. One page-aligned write
per file took the save from ~370 to ~100 ms with the same bytes on disk.
**Before optimising how much a console path reads or writes, count its
`NtReadFile`/`NtWriteFile` calls and check where its buffers start;
reduce calls and align buffers first.**

### NEVER size a scratch buffer from one of the things written into it - size it from the largest, or give each its own

`FT_Flush` (`engine/platform/xbox/xbox_frametime.c`, since #169 on
2026-10-01) wrote both a FRAMES chunk head (16 bytes) and a whole CALIB
chunk (24 bytes) through one `uint8_t head[16]`, so every flush wrote 8
bytes past it. For four days nothing showed: the overflow landed in
alignment padding or in saved registers no caller happened to rely on.
The sv_game breakdown (2026-10-05) added a call after `FT_Flush` that kept
a pointer in ESI across it, and the first in-game flush became an access
violation in an unrelated function (`Xbox_FT_SvGame_EncodeClasses`
writing through ESI = 1) - found only by reading the bugcheck's
exception record off the stack with `tools/xemu-debug.py`. A stack
overflow into callee-saved registers is invisible until some caller's
register allocation changes, so "it has always worked" says nothing.
**Declare a shared buffer as the largest thing that goes into it, by its
size constants (`XBOX_FT_CHUNK_BYTES + XBOX_FT_CALIB_BYTES`), not by the
first use; when a new crash points into code that cannot fail, look for
a write past a buffer in the functions it returned from.**

### ALWAYS copy a reference file:line from the authoritative clone while writing the comment, not from the in-repo tree or from memory

R3 (x9-perf-world, #146) committed new ref/nv2a comments whose
reference citations the reference-renderer review then found wrong in
five places. `mod_bmodel.c:3403-3424` was this tree's own engine file (upstream
has the same lines at `:3323-3344`), `gl_cull.c:95`,
`gl_frustum.c:104`, `gl_frustum.c:110-127` and `gl_rsurf.c:3797-3807` were plausible-looking ranges never opened, and
the comment's summary of ref/gl ("never reaches a face outside the PVS")
was stronger than `R_RecursiveWorldNode` + `R_CullSurface` actually do.
A wrong line number makes a later divergence look intentional, which is
what citations exist to prevent. **Open the cited line in
`G:\Proyectos\Ferrum56\xash3d-fwgs` and paste the number you see; cite
an engine file in our tree only when saying so ("in this tree").**

### NEVER script anything after a command the engine follows up through the command buffer

X9-X step 12: a test config ran `save dectest`, then `wait`s, `load dectest`
and `menu_loadgame`. The thumbnail came out black apart from the HUD and the
menu showed none. `SaveGameSlot` queues the thumbnail with
`Cbuf_AddTextf( "saveshot ..." )` (sv_save.c:1791), which appends it behind
everything still in the buffer, so it ran after `menu_loadgame`, over the
open menu, where `V_RenderView` draws no world. A normal save, with an empty
buffer, shoots the next frame.

**When a scripted command makes the engine queue a follow-up (`saveshot`
after `save`, a `levelshot`, an `exec`), end the script there and do the
next step in a new boot or from input.** Otherwise the follow-up runs in
whatever state the rest of the script left.


### NEVER report a CPU or frame-time number without reading the compile line that produced the binary

Every Xbox frame time taken before 2026-09-29 - the real-hardware
route at 44 fps with four maps near 30, and PR #162's per-luxel cycle
estimates - came from an -O0 build: `tools/xbox-cc.sh` and
`tools/xbox-cxx.sh` passed no `-O` flag at all, and nobody had looked.
Nothing chose -O0; it was an omission that every measurement inherited.
At -O2 the same lightmap workload costs 0.54x the cycles and the XBE
is 20% smaller. **Before a performance number goes into a report, read
the exact flags of the compile (and of the prebuilt libraries it links)
and write the level next to the number.** nxdk's libraries are their own
level: a plain `make NXDK_ONLY=y` is -O0; `tools/xbox-build-nxdk.sh`
builds them at a chosen level and stamps it in
`deps/nxdk/lib/r3d-nxdk-opt.txt`, and `tools/xbox-link-check.sh` prints
the stamp on every link - copy that line next to the number. `deps/nxdk`
is shared by every worktree, so its level can change under a branch that
never touched it.

### ALWAYS diff the picture from a second optimisation level before trusting "no visual change"

An uninitialised read is deterministic inside one build, so
screenshots of that build agree with each other and with its own
baseline forever. `CStbFont::GetCharABCWidthsNoCache` computed the
space's advance from a glyph box `stbtt_GetGlyphBox` never wrote
(divergence #135): stable at -O0, stable at -O2, and one to two pixels
apart between them - visible only because every two-word menu label
differed while every one-word label was pixel-identical. **When a
change could expose undefined behaviour, compare the same view built
at two optimisation levels, and read a difference that follows a
pattern (all labels with a space) as a bug, not noise.**

### NEVER leave a renderer callback unimplemented without reading what the engine does around the call

ref/nv2a skipped `CL_DrawEFX` for nine steps as "effects, not drawn
yet". The engine's `CL_DrawEFX` (`cl_efx.c:2079`) is also where dead
beams, particles and tracers are freed, so on main the pools filled and
never emptied: c1a0e read live with 128 dead beams, 3360 particles and
303 tracers, and new effects could no longer be allocated at all. An
unimplemented hook is not only a missing picture; **read the engine
side of every `gEngfuncs`/`ref.dllFuncs` pair you stub, and list the
housekeeping the stub skips.**

### ALWAYS confirm the thing under test is in the renderer's own lists before reading a live look

X9-X step 6's first three xemu looks at sprites (c0a0, c1a0, c1a0b) showed
no sprite on either build, and a draw counter of 0 read as "the new path
draws nothing". Reading `nv2a_solid/trans_entities` over the debugger
showed why: the c0a0 tram had already carried the player 6000 units past
its glows, and the c1a0/c1a0b spawns had no sprite entity in the frame's
lists at all. Teleported into c1a0e's chamber (server edict origin
written over gdb), the same build drew 231 sprite quads in seconds. **A
live look proves a draw path only when the object is known to be sent,
in view and unoccluded: read the frame's entity list, or set position
and angles yourself, before a screenshot of nothing is taken as a
failure (or a pass).**

### NEVER take a "before" measurement from a build a working-tree edit can reach

This session's first baseline XBE for the menu-compression branch was
linked while the renderer edits were uncommitted but not yet compiled,
reasoning that only the sweeps compile source. The link itself re-isolates
ref/nv2a from the working tree (`tools/xbox-link-check.sh:763`), so that
"main" already carried the new texture classifier: it read 31.66 MiB free
against the real main's 25.88, and only a texture table showing A8Y8 rows
on a build that predates A8Y8 exposed it. **A baseline is built from a
clean checkout of the base commit, or with every edit stashed across the
whole build, link included; confirm it with one observable only the
change can produce.** The same holds for the disc: a staged file the new
build needs (here a `BackgroundLayout.txt` naming `.dds` tiles) makes the
old build fail differently, not look like the old build.

### ALWAYS check what state a reused upstream helper reads when calling it earlier than any upstream caller does

Divergence #130 drew the loading plaque with upstream's own
`CL_DrawHUD( CL_LOADING )`, reasoning "the same call that draws it over a
levelshot". The first boot test showed it everywhere except the first
load after boot: that frame runs before the client DLL's HUD init, the
only thing that fills `clgame.scrInfo` (`CL_GetScreenInfo`,
`cl_game.c:1648`), so the plaque was centred on a 0x0 screen and landed
off-screen. Every upstream caller runs after a game has initialised the
HUD, so upstream never hit it. **When a divergence calls an existing
function from a new point in the lifecycle, list the globals it reads and
confirm each is set by then; "upstream calls it too" only covers
upstream's call sites.** Test the first occurrence after boot separately
from a later repeat, since the repeat inherits state the first one lacks.

### NEVER move a draw call that pushes GPU state directly without making it re-enter that state itself

a48b415e moved ref/nv2a's brush entities from right after the world into
the entity lists, interleaved with studio models drawn through the TriAPI
flush, which swaps the program, stage 1 and vertex arrays. The brush draw
only pushed its own per-entity deltas, so it drew correctly exactly when
nothing visible ran before it: doors and the tram vanished whenever a
visible studio model preceded them, and came back when that model was
culled (r_context.c nv2a_draw_brush_entity, fixed 37646d16). A draw that
depends on state it never sets looks like a culling bug, not a state bug,
because whether it breaks depends on what else is on screen.

### ALWAYS bind a boot test to the disc actually selected by the emulator

PR #153's investigation repacked `hl.iso` while xemu's configuration still
selected `agentic-x9-x-menu-texture-unload-fix4.iso`; its later fatal-error
trace found only nine free pages, not a demonstrated texture-free deadlock
(`reports/menu-loading-fix.md`). Pass the disc path explicitly and verify
the running image before using its symbols or attributing a regression.

### NEVER diagnose a boot "hang" from the halted CPU alone - find the last event the engine itself emitted

A disc that sat on the nxdk splash showed `EIP` in the kernel's `sti; nop;
nop; cli; cmp [ebx+0x2c],0; jz` idle wait, with a title-thread stack that
happened to end in `stat()` → `GetFileAttributesExA` → the kernel. Hours went
into `stat("D:/")`, disc size (padded to 600 MB), file count (an 8 MB
subset), intro videos, and a `gameinfo.c` change that "fixed" it - none of
which was the cause, because an idle CPU looks identical whether a thread
is blocked in a syscall, sleeping, or gone. A breakpoint sequence on the
engine's own milestones (`Host_Main` → ... → `SV_Init` → `Sys_Error`) found
the truth in one pass: `Delta_InitFields: couldn't load file delta.lst`
(`engine/common/net_encode.c:854`), fired before `VID_Init`, so no renderer
existed to draw it, and the host restarted into the same error every ~3 s.
`fork-plan.md` had already recorded that exact signature from an earlier
phase. **Start from what the program reports or would report - `Sys_Error`,
`Host_Error`, the log channel, the last milestone reached - before reading
the CPU's wait loop as the fault.** A saved stack is where a thread was
last *scheduled*, not what it is waiting for.

### ALWAYS single-step off a software breakpoint before continuing on xemu's gdb stub

`tools/xemu-debug.py`'s `cont()` after a `Z0` hit re-triggers the same
breakpoint without executing the instruction: a tracer that left its
breakpoints armed reported `WinMainCRTStartup` (and later `main`) hit
every ~4 ms from an identical `esp` - a phantom "relaunch loop" that was
pure round-trip latency - and 40,000 `CloseHandle` hits with zero returns.
The tracer that removed the breakpoint before continuing, and the `break`
subcommand (which single-steps after every hit), were the only ones that
measured anything. **After a stop at a `Z0` address, send `s` (or remove
that breakpoint) before `c`; an identical register set across "consecutive
hits" means the guest never moved.**

### ALWAYS run a tool's tests through the shell the tool is documented to run in

`tools/xbox-xiso.sh` is documented for MSYS2 bash. Its `delta.lst` validator
passed every fixture in an isolation harness run from Git Bash, then
refused the real Steam `delta.lst` from the live packaging run: under
MSYS2, an `awk -v` value of 11,686 bytes arrived as 8,184 - measured with
`length()` inside awk, cut mid-table right after `m_fInZoom` - so every
field after the cut was "unknown". Same gawk 5.0.0 on both sides; the exec
path differed. (Earlier the same day, `tools/codex-review.sh` failed under
MSYS2 because `$HOME` is not the Windows profile there.) **A pass under a
different shell than the one the tool ships for is not a pass; put the
documented shell in the test command.**

### NEVER trust a platform libc function you have not read or tested - a declaration is not an implementation

nxdk's pdclib parses `%f`/`%e`/`%g` and then does nothing
(`deps/nxdk/lib/pdclib/functions/_PDCLIB/_PDCLIB_print.c:452-458`: the cases
`break` with no output and without consuming the argument), so every
`snprintf( "%g" )` on the Xbox produced an empty string and misaligned what
followed - silently, with no error at compile, link or run time. It took out
the engine's own `"angle"`→`"angles"` keyvalue conversion (`sv_game.c:5023`)
and with it the facing of every entity placed with an `"angle"` key
(fork-plan XG.1), and was found only because a human saw the player spawn
backwards. The same library had already shipped a declared-but-absent
`atof` (X1).

**Before relying on a pdclib/xboxrt function that ordinary nxdk samples
never exercise - floating-point formatting or parsing, `strto*`, locale,
wide characters - read its implementation or run it on the host against
the real C library.** The header will happily declare what the library
never wrote, and nothing downstream will say so. (`strtod`/`strtof`/
`strtold` are `assert(0); return 0.0` at `xboxrt/libc_extensions/
stdlib_ext_.c:89-105`, waiting for their turn.)

### ALWAYS boot the baseline twice before calling a branch-vs-main pixel difference a regression

X9-X step 11 (water): at the same teleported pose in c3a1, the rock walls
far from any water read (44.9, 28.0, 29.1) on the branch and (41.1, 24.6,
24.8) on main - stable within each boot over five seconds, so it looked
like the change brightened the world. A second boot of main read (49.0,
29.8, 30.8): the level's own state differs per boot, not per build.

**A difference outside what the change touches is only evidence once the
baseline has been booted at least twice and agrees with itself.** One boot
per build measures the boot, not the build.

### NEVER read a game-derived renderer input after a debugger teleport without reading what the game derived

X9-X step 11: underwater fog never came on in c2a4's func_water after
writing the player's origin into the pool. The renderer was fine: the
teleported player was not being simulated (it hung at z=0 with no gravity),
so the server's `pev->waterlevel` stayed at its spawn value 1
(`entvars_t` offset 448, read over the debugger) and `R_CheckFog`'s first
gate (`PARM_WATER_LEVEL < 3`) returned, as ref/gl's would.

**When a teleport or poke feeds a renderer path through game state -
water level, contents, ground entity - read that state off the guest
before judging the renderer.** Set it explicitly (and say so) when the
simulation will not.

### ALWAYS confirm which map a live measurement is in before comparing it with map data

XG.1's first probes compared a spawn angle read off the wire, `(0, 0, 0)`,
with `c0a0.bsp`'s `"angle" "180"` and concluded the value was lost in the
engine. The disc was not running `c0a0`: its test-only `userconfig.cfg`
does `map c1a3`, whose player start has no angle, so the reading was right
and the comparison was wrong. Caught by reading the spawn spot the SDK had
actually chosen (origin `24 1517 47`, a `c1a3` coordinate). `verify-image`
guards the binary; nothing guards the scene.

**Read something scene-specific off the guest - an origin, a map name, an
entity count - and match it to the data you are about to compare
against, before the comparison.** The `verify-image` rule further down
(a stale disc image made every breakpoint address lie) has a wrong-map
twin, and it costs the same round.

### NEVER conclude an asset is present because the pipeline should have included it

The previous project's baker discovered map assets by regex-scraping the BSP
entity lump. Anything precached by a fixed string in game code was invisible
to it: five combat sprites were silently missing, and the feature compiled,
passed its unit tests, booted, and drew nothing. A code review found it, not
the tests.

**Verify presence by observing what the engine requests at runtime**, not by
reasoning about what the pipeline should have found.

### NEVER derive display aspect ratio from pixel dimensions

`V_AdjustFov` (`engine/client/cl_view.c:252`) returns early when
`width * 3 == 4 * height`. On anamorphic Xbox widescreen the framebuffer is
640x480, so the test is true and no correction is applied — precisely the case
that needs it. Pixel aspect ratio and display aspect ratio are independent
quantities on any console.

### NEVER assume an optimisation transfers because the engine it came from is similar

Ironwail is a Quake source port and its techniques look portable. They are
compute shaders, indirect multi-draw, bindless textures and persistent
mapping, none of which exist on the NV2A or the RSX. Check the *hardware
feature*, not the *engine lineage*, before planning work around a technique.

### ALWAYS look for the project's own environment script before hand-rolling one

Four hypotheses, six failed builds and roughly an hour went into diagnosing why
nxdk would not compile here — MSYS path conversion, libcxx header shadowing,
empty `-I` arguments, Windows SDK auto-detection. All four were wrong.

`deps/nxdk/bin/activate` had existed the whole time. It emits exactly the right
`PATH` and `NXDK_DIR`, and the real problem was simply the wrong shell: nxdk
wants MSYS2 MinGW-64, whose `/mingw64/bin` carries the clang it expects, and Git
Bash has no such directory.

**Before deriving an environment, look for one.** `activate`, `env.sh`,
`*-config`, a CI workflow file, a sibling project that already builds the same
thing. The cost of looking is a minute; the cost of not looking was every wrong
hypothesis above, each of which was individually plausible and locally
consistent with the evidence.

**Corollary, and the sharper half:** every one of those hypotheses was
*supported* by real command output. Plausible mechanisms that fit the symptom
are cheap to generate and expensive to test. Prefer the check that would
*eliminate a whole class* — "does an official recipe exist?" — over the check
that confirms one mechanism.

### NEVER run a mutating agent against a tree you are working in

A workflow spawned to explore build recipes ran `make clean` in `deps/nxdk` and
deleted nineteen freshly built libraries, while the main session was using
them. No conflict was reported, because nothing detects it.

Agents that only read are safe to run alongside anything. Agents that build,
clean or edit need either an isolated worktree or exclusive use of the tree.
Decide which before launching, not after.

### NEVER call something the top risk without reading the conditional around it

`xshlib.py:47-50` sets `-melf_i386`, so this project recorded "the static
linking machinery is ELF-specific and cannot work on PE/COFF" as its single
highest risk, in two planning documents, for a week.

The flag is appended only `if '-m32' in self.env.LINKFLAGS`, and an nxdk build
never passes `-m32`. The line was read; the two lines above it were not.

The real problems turned out to be different and smaller: `objcopy` is absent
from nxdk, `llvm-objcopy` rejects `-G` on COFF while `--redefine-sym` works,
and `lld -flavor link -r` cannot do partial links. Three known fallbacks
exist. The risk was real but it was mis-diagnosed, and a mis-diagnosed risk
sends work in the wrong direction with full confidence.

**Read the enclosing conditional before quoting a line as evidence.** The
higher the stakes of a claim, the more of the surrounding code has to be read
before making it — and a claim that reorders a project's plan is the highest
stakes there is.

### NEVER trust a duplicate-symbol count from a synthetic reproduction

An investigation reported "115 duplicate symbols from `public/miniz.c`". The
number came from linking the *same object twice*. In the real build,
`public/wscript:147` produces a static library and lld resolves archive
members lazily, so three modules pulling the same code produce zero
duplicates.

A synthetic reproduction proves a mechanism can happen. It says nothing about
whether it does happen, or how often. **Measure on the real build before
sizing a problem** — and if the real build does not compile yet, the honest
answer is "unmeasured", not a number from a stand-in.

### NEVER assess a body of work from its headline feature list

Ironwail's README leads with compute shaders, indirect multi-draw, bindless
textures and persistent mapping. None of those exist on the NV2A, and the
conclusion drawn was that none of Ironwail transfers to the console targets.

That was wrong, and it would have cost most of the available performance.
Those four are the *marketing* of the project, not the bulk of it. The bulk is
sign-bit frustum rejection, radix-sorted batching, zero-allocation ring
buffers, distance-sorted transparency, PVS caching and threaded I/O — all of
which port unchanged.

**Read the work, not the summary of the work.** A feature list is written to
impress; it is a bad basis for a portability judgement. And when a mechanism
does not transfer, ask what *destination* it was moving work to, and whether
another destination is available — here, bake time was, and nobody had looked.

### ALWAYS re-check a target spec against the current compiler before trusting it

The previous project's Xbox target JSON accumulated six fields that modern
rustc rejects, over seven months, because nothing exercised the build. A
configuration nothing runs is a configuration that is already broken.

**Corollary:** add a target to CI the moment it compiles, even while the
binary does nothing useful.

### A comment asserting a code path is unreachable is a CLAIM — verify it

`compat/xbox/xbox_posix_io.c` stubbed `dup()` to return `ENOSYS`, with a
comment explaining that its only caller sat behind `XASH_REDUCE_FD`, which
this build does not enable.

The comment inverted the guard. `filesystem/sys.c:71-73` defines `HAVE_DUP`
for every platform except PSVita and NSwitch, so Xbox gets it, and it is the
`XASH_REDUCE_FD` branch that *avoids* `dup()` — and that is set only for
DOS4GW and PSP (`common/defaults.h:44,49`).

`FS_OpenHandle` calls `dup()` on every open of a file inside a PAK, and
retail Half-Life keeps all of its content in `pak0.pak`. Nothing in the game
would have loaded, and it would have presented as an asset problem rather
than a one-line stub. Three independent reviewers found it; the compiler
could not.

**The comment is what made it dangerous.** An unexplained stub invites
scrutiny. A confidently justified one closes the question — for the reader
and, worse, for the author revisiting it later.

So: before writing "the only caller is X", "this cannot happen", or "this
build does not enable that", **go and read the guard**. Cite it by
`file:line` so the next reader can check it in one jump. A claim in a
comment carries the same weight as code and gets none of the verification.

**Corollary:** stubs that fail are not automatically safe. `chdir` and
`getcwd` in that same file also fail — those were correct, because the
platform genuinely lacks the concept. `dup` was wrong because the platform
*does* have the primitive (`NtDuplicateObject`) and the caller *is* live.
"Fails honestly" is only honest when the failure is truly unavoidable.

**Written once, then repeated two commits later, in the same file.**
`fileno()` returned pdclib's raw Win32 `HANDLE` as if it were a descriptor,
under a comment stating that `sys_con.c` "passes it straight to isatty-style
checks and never to `read()` or `close()`". Both halves were false: there is
no `isatty` anywhere in `engine/`, and `sys_con.c:126` stores the value and
then `:251` and `:257` hand it to `write()`. It was latent only because
nxdk's crt0 passes `argc=0`, so the console path never opens.

That the lesson was already written and still did not fire is the part worth
recording. Writing the rule down is not the control; the control is a
mechanical one — **when a comment names a caller, grep for that caller before
the comment is committed.** The review workflow caught it, which is the
argument for running `pr-review` on every PR rather than only on the ones
that feel risky.

### NEVER classify build inputs by directory name

Thirty-five files failed the Xbox compile sweep. I bucketed them by reading
paths: six networking, twenty-nine "platform backends for targets that are not
Xbox", and published "every file this target claims compiles, except
networking" in a PR.

One of the twenty-nine was `engine/platform/misc/lib_static.c`, and this build
selects it. `engine/wscript:194` adds it whenever `XASH_STATIC_LIBS` is
defined, which `tools/xbox-cc.sh:89` defines itself, and
`common/defaults.h:102-105` then sets `XASH_LIB = LIB_STATIC` — which turns
`platform/win32/lib_win.c` into an empty object and leaves `lib_static.c` as
the sole definition of `COM_LoadLibrary`, `COM_FreeLibrary`,
`COM_GetProcAddress` and `COM_FunctionFromName`. It could not compile. Four
undefined symbols, in the path that loads the game at all.

The X1 link probe could not have found it either: `lib_static.o` was not in
the object set, precisely because the sweep had classified it as somebody
else's platform.

**A directory name is not a build description.** `engine/wscript` is, and
until this target has one of its own, every claim about what it "compiles"
has to be traced through that file by hand — `ant_glob` by `DEST_OS`, the
`not in ['win32', 'dos']` denylist, and the two `get_define` branches. Three
of those four selectors point somewhere other than the directory being
judged.

**Corollary, and the reason this rates its own entry:** the false claim was
already published. Measurements get quoted, and a wrong one propagates faster
than the correction. Before a number goes in a PR, state the command that
reproduces it — and then check that the command's scope is the same scope the
sentence claims.

### A measuring tool that cannot report its own failure will eventually lie

`tools/xbox-sweep.sh` has now reported a false result three times:

1. It judged success by `grep -c 'error:'` and reported "182 ok, 0 failing"
   for a directory whose first file emits twenty errors.
2. Its shebang picked Git Bash, where `/mingw64/bin` does not resolve, and it
   reported "216 failing, 0 errors each".
3. Fixed for both of those, it still **discarded the compiler's output** for
   any failure with no `error:` line. A broken toolchain — `set -u` on an
   unset `NXDK_DIR`, clang off `PATH` — printed `FAIL <file>   0 errors` for
   every file, an empty frequency table, a zero-byte log, and exit 0.

Each fix was correct and each left the same hole: **the tool could still fail
in a way that looked like a result.** That is the property to design for, not
the specific bug. Ask of every measuring tool: *if this were broken right now,
would its output differ from a real one?* If the answer is no, the tool is not
finished no matter how many bugs it has stopped having.

The same defect was live in `.claude/workflows/pr-review.js` at the same time,
in two places — a null scope step silently narrowing five lenses to the one
whose predicate is `() => true`, and dead refuters making "nobody answered"
arithmetically identical to "everybody refuted it". The review workflow found
it in itself. That is the argument for pointing the reviewer at the reviewer.

### NEVER list an optimisation as a port without grepping the reference for it

`optimization-transfer.md` listed nineteen things to bring over from
Ironwail. Four were already in the tree — sign-bit frustum culling
(`gl_frustum.c:119`), distance-sorted translucent entities (`gl_rmain.c:86`),
per-vertex studio lighting (`gl_studio.c:25`), lightmap pages — and a fifth,
world batching, is present but disabled as glitchy (`gl_opengl.c:34`). One
claim ran the other way: "memoised bone setup, as the reference does" — it
does not, `R_StudioSetupBones` is eager, and the false claim had already been
copied into a specialist's standing prompt.

Every one of those was a claim about what `ref/gl` does, made from memory of
Quake or Ironwail. **Before writing "port X from Y", grep this tree for X.**
Cite the line where it is, or the function where it is absent. A reviewer
whose only job is that check now exists (`reference-renderer-specialist`); it
exists because five of nineteen got through without one — and a sixth
miscounted claim, about the table's own row and baker counts, got through
after the specialist existed, because nobody had run it on the document yet.

**A seventh got through the specialist's own document, in the paragraph
correcting the fourth.** Writing up `r_alphasort` as present, I read
`R_TransEntityCompare`'s `dist1 > dist2 → return -1` and its sentinel
`1000000000` for `kRenderTransAlpha` brushes, and wrote "forced last" —
plausible, unchecked, and backwards: the sentinel is the *largest* distance,
the comparator sorts descending, and the draw loop walks from index 0, so
those entities are forced to the **front** and drawn **first**. Citing the
right lines (`gl_rmain.c:86-117`, `:872-874`) is not the same as tracing what
they do; I had the citations right and the conclusion inverted. Caught by
`pr-review`'s `upstream` and `reference` lenses together, in the first round
of this branch's review where both actually ran — a caching defect in named
`Workflow` resolution had silently reused a stale script missing the
`reference` lens for several earlier invocations; worked around by invoking
via `scriptPath` instead of `name`, filed separately as tool feedback.

**Corollary:** a sort comparator's return-value convention and the loop's
iteration order are two separate facts, and getting either one backwards
inverts the conclusion. When a claim depends on both, trace both, in the
code, in the same sitting — not one now and the other from memory of how
sorts "usually" work.

### ALWAYS edit existing files as bytes, never through text mode

Files created on this host land as **CRLF**. Python's text mode reads
universal-newlines and writes `os.linesep`, so any read-modify-write in text
mode silently flips a file's line endings — in either direction, depending on
which side started as which.

The cost is not cosmetic. A 14-line addition to `.gitignore` became a 766-line
diff; a 24-line addition to `fork-plan.md` became 390. On a fork that hides the
real change, poisons every future merge, and makes review useless.

Passing `newline=""` on read or `newline="\n"` on write does not save you.
Both were tried; the second is what caused the 390-line diff, by forcing LF
onto a CRLF file. The same trap bites the *search* side: a pattern written with
LF will not match a CRLF file, which fails silently as "anchor not found".

**Open with `"rb"`, replace on `bytes`, write with `"wb"`.** Then check with
`git diff --stat` before committing: if the changed-line count is anywhere near
the file's length, the edit is wrong no matter how correct the content looks.

This rule existed after the first occurrence and was violated twice more,
because text mode is what comes to hand. It is a reflex problem, not a
knowledge problem — which is why the check afterwards matters more than the
intention beforehand.

### ALWAYS state which question an answer is answering when the scope has moved

"Rewrite the engine in C++" and "start from the complete upstream engine
instead of a partial reimplementation" are different questions with opposite
answers. An answer carried over from the earlier framing is wrong without
being incorrect, which is the hardest kind to catch.

### NEVER call something "the one" of anything without grepping for siblings

Fixing `Sys_Print`'s call to `Wcon_WinPrint` — an undefined-symbol landmine,
`con_win.c` does not compile for Xbox — I wrote it up as "the one call site
X0's log channel routes through" and "the one function". Both false. A
`grep -rn "Wcon_" engine/common/*.c` I did not run at the time turns up five
more: two in `Sys_Error`, one in `Sys_DestroyConsole`, two in `Host_Main`
(the last two behind the raw `_WIN32` macro, not `XASH_WIN32`, so the fix I
already had would not even have widened them). All five are the identical
class of bug, in files this same commit range touched or sat next to.

Found by `pr-review`, on the branch that introduced the claim — not by me
rereading my own work, which is the point. **"The one" is a completeness
claim, and completeness claims need the search that would falsify them, not
confidence that the search would come back empty.**

**The very fix for this was itself incomplete, one round later.** When I
finally ran the grep above, I scoped it to `engine/common/*.c` — reasonable,
since that is where all five siblings turned out to live, but not actually
exhaustive. A sixth site, `Wcon_Input` via `Platform_Input`, sat in
`engine/platform/platform.h` the whole time, outside that glob, and survived
one full review-and-fix commit before a second review round caught it. The
lesson does not just apply to the original mistake; it applies to the search
written to fix it. A grep scoped to "where I expect the answer to be" is the
same failure shape as a claim of "the one", just one level removed — the
fix is `grep -rn "Wcon_" engine/` (the whole tree), not a directory chosen
because it is where the pattern happened to cluster last time.

The five-minute cost of the wider grep is smaller than one wrong sentence
propagating into a changelog and a divergence-ledger row, which is what
happened here twice before anyone checked with the right scope.

**A third round, and the widened grep still missed one — because it was
scoped by name, not by mechanism.** `grep -rn "Wcon_" engine/` finds every
`Wcon_*` call, correctly this time. It cannot find `Win32_NanoSleep`, which
is the identical bug — a symbol declared in `platform.h`, defined only in
`sys_win.c`, which does not compile for Xbox — wearing a different prefix.
The pattern was never really "calls starting with `Wcon_`"; it was "any
declared-but-Win32-only-defined symbol reachable under a guard that admits
Xbox." A name-prefix grep is a proxy for that, and a proxy is exactly the
kind of search that comes back clean without having asked the real
question. The question that would have found all eight sites in one pass:
which functions does `engine/platform/win32/*.c` define that `platform.h`
declares, and which of those are called under `XASH_WIN32` or bare
`_WIN32` with no `!XASH_XBOX`? That is a slower search to write and a
faster one to trust.

### NEVER trust a new tool's exit code without testing its failure path too

Writing `tools/xbox-check-nxdk-cc-drift.sh` - a checker to close exactly the
kind of gap this file's own entries keep finding - the first version ran
clean (`every nxdk-cc flag is present`, exit 0) on the first try. That was
not a passed test. It was a script that had never been shown a real
discrepancy, reporting the only thing it had ever been asked about.

Injecting one and re-running found two failures stacked on top of each
other, both invisible to a clean-looking exit code:

1. **PATH, not the pattern, first.** The exact `grep`/`sed` pipeline gave
   silently different results depending on which binaries the invoking
   shell's ambient `PATH` resolved to before this script pinned it -
   `\s` matching nothing, `\\$` failing to strip a trailing backslash in
   either sed mode, with no error either way. `xbox-cc.sh` already pins
   `PATH` for exactly this reason; this new script did not, until tested
   against an actual discrepancy rather than the no-discrepancy case.
2. **Even a purpose-built test injection can lie.** The first two attempts
   to inject a test flag used `printf`/`sed` one-liners that silently
   produced literal `\n` text instead of a real newline - the identical
   class of shell-interpretation trap the checker itself exists to survive
   - and both attempts reported the checker as passing, because nothing
   had actually been injected. Only editing the file directly with a real
   text edit, then independently confirming the injection with `od`/`xxd`
   before trusting the checker's response to it, surfaced either bug.

**A tool that has only ever seen the passing case has not been tested,
regardless of what its exit code says.** Manufacture the failing case,
verify by an independent method that the failing case is actually present
before running the tool against it, and only then trust a clean result on
the real one.

### "Checked individually" is a claim about the search, not a guarantee about what remains

Two review rounds in a row on this branch each ran the mechanism-based
search the previous entry describes, each reported finding every remaining
site, and each was wrong. Round three found `Win32_NanoSleep` and
`Platform_DebuggerPresent` and said "found all eight sites in one pass."
Round four found `Sys_SetupCrashHandler`/`Sys_RestoreCrashHandler` - the
identical shape, declared in `platform.h`, defined only in a Win32-only
file, called under an unwidened guard - which the round-three search should
have caught and did not.

Nothing was wrong with the search *method* either time. Both times it was
the right question, asked correctly, and it still returned an incomplete
answer, because a search over a large, unfamiliar surface (every function
`engine/platform/win32/*.c` defines, cross-referenced against every call
site) is itself a place to make an ordinary mistake - missing one grep
target, stopping one file short, misreading one guard - independent of
whether the underlying method is sound.

**The fix is not "search more carefully next time."** It is: state what was
checked as a fact about the search ("grepped these files for this
pattern"), not as a fact about the codebase ("this is complete"). A
completeness claim should be phrased so that finding a tenth site next
round is a correction to the claim, not evidence the method failed - and
should expect that correction, rather than being surprised by it. This
branch is on its fifth review round; the count of sites found this way
across three of those rounds is 2, 2, then 1 - a declining but nonzero
trend, not a proof of zero remaining.

### NEVER trust a hand-typed backslash-then-newline in a generated shell script without re-reading the raw bytes

Twice in one editing session, a shell one-liner or a Python string literal
meant to insert a real line-continuation (`\` followed by an actual newline
byte) instead produced the two-character sequence backslash-then-`n` -
visually almost identical in a diff or a `sed -n` listing, syntactically
valid enough that `bash -n` did not catch it either time (a literal `\n`
mid-line just becomes part of the argument to the preceding command), and
both times it silently broke the string the line was building
(`tools/xbox-sweep.sh`'s own error message ended up containing a literal
`\n` instead of wrapping). Caught only by `od -c` on the specific bytes,
not by re-reading the file with a normal tool.

**After any programmatic edit that constructs a line continuation, or
after two failed attempts to get one right the same way, dump the exact
bytes with `od -c` (or equivalent) before trusting the file - do not
re-attempt the same escaping approach a third time and hope.** This is the
same failure this file already has an entry for
(`printf`/`sed` test-harness escaping), recurring in a different tool
(Python `bytes` literals instead of a bash heredoc) - the shape is "an
editing tool's own escaping, not the target file's content, produced the
bug," and it generalises across whichever tool is doing the constructing.

### NEVER explain a syntax trap by typing the exact sequence that trips it

Fixing a `-Wcomment` warning caused by a literal `/*` inside a doc comment's
prose (`engine/common/*.c`, where the `/` before `*.c` reads as a nested
comment's opening delimiter), the first replacement wording explained the
bug by describing it as "the original wording here spelled that as a
literal `engine/common/*.c` glob, and the `/*` inside it read as..." -
which itself contains the identical `/*` sequence, still inside the same
block comment, and reproduced the exact warning it was written to explain.
Caught only by recompiling immediately after the edit rather than trusting
the fix on sight.

**When writing prose that explains a syntax trap, describe the trap in
words rather than typing the triggering sequence, and recompile (or
otherwise re-run the check that found it) after the fix, even when the fix
looks obviously right.** The failure mode is specific to explaining a bug
inside the same syntactic context the bug lives in - a comment about a
comment-breaking sequence, a string about a string-escaping bug, a shell
line about a shell-quoting bug - and "obviously right" is exactly the
state of mind the original bug was written in too.

### NEVER read a truncated view of sorted or ranked output and report it as the extreme it isn't

XB's lightstyle census (`tools/asset-studio/tools/census_lightstyles.cpp`)
prints 125 maps sorted DESCENDING by affected-face count, then a totals
line. The tool's own output was correct throughout. Checking it with
`... | tail -60` on a 131-line result showed only the BOTTOM 60 rows - the
smallest values in a descending sort - and those were written into
`fork-plan.md` as "the worst maps" (46/43/21 faces), a claim confidently
wrong by two orders of magnitude (the real worst map, `rocket_frenzy`,
carries 6,223). Found in Codex review: the published per-map numbers
couldn't add up to the published total across 125 rows, which is exactly
the kind of check a human skimming the same truncated tail would not
think to run.

**Before reporting an extreme (worst/best/max/first) from a large tool
output, either view the whole thing or grep/head for the actual extreme
directly - never assume a `tail`/`head` truncation you chose for
readability happens to land on the interesting end.** A sort direction is
easy to get backwards in your own head even when the tool got it right;
the fix is not "concentrate harder," it's "look at the row you're
citing," or better, let the totals do a sanity check the truncated view
can't (a printed max/sum a human can cross-check against what got quoted).

Second data point, 2026-09-26, the other end of the same mistake: a site
inventory, not an extreme. `grep -n 'deps/nxdk\|NXDK_DIR\|cxbe' tools/*.sh
| head -40` was read as the complete list of places the tools hardcode
the nxdk path, five were changed, and the change was committed as
"every site". The audit that followed found the `cxbe` assignment at
`tools/xbox-link-check.sh:1003` and five `nxdk-link` invocations - all
below the fortieth line of that grep. A `head` on an inventory is a
claim that the inventory is short; count it (`| wc -l`) before believing
it, and never `head` a list you are about to call complete.

### NEVER chain a commit after an edit batch that can stop partway - check `git diff --stat` against the claim first

An edit script that asserts each anchor and aborts on the first miss
writes nothing for the files after the miss, while the `&&`-chained test
and commit that follow still run on whatever did land. Twice in one
session (2026-09-26) a commit message described a change its commit did
not contain: `docs/r3d/agentic-loop.md` edits lost to a wrapped anchor,
then the `wait-scene` fix in `tools/xemu-debug.py` (commit `956b38d0`
claims it; `2d7532c7` carries it). The tests passed both times, because
they tested the files that were written.

Before committing a batch, compare `git diff --stat` with the list of
files the message names; a file missing from the stat is a claim the
commit does not support. Better, do not chain the commit at all: edit,
look, then commit.

### `export FOO=1` in this dev shell does not survive an explicit `/c/msys64/usr/bin/bash` invocation, or any invocation whose PATH puts msys64 ahead of this shell's own `bash`

`tools/xbox-cc.sh`'s `XBOX_DEDICATED`/`XBOX_ENGINE_TESTS`/`XBOX_CLIENT_MAIN`
opt-ins (and by extension anything built on top of them, like
`tools/xbox-dedicated-sweep.sh`'s `export XBOX_DEDICATED=1`) all read
`${VAR:-0}` from the environment. Exporting the var in this dev shell and
then invoking the target script via `MSYSTEM=MINGW64 /c/msys64/usr/bin/bash
script.sh` - the exact form these scripts' own header comments document as
canonical - silently drops the export: `${VAR:-0}` reads back `0` inside
the script every time, with no error, no warning, and (since every one of
these flags is a silent, additive opt-in) a build that looks completely
normal. Confirmed directly: `/c/msys64/usr/bin/bash -c 'export FOO=1; env'
| grep FOO` prints nothing, even though `echo $FOO` inside that same `-c`
string prints `1` - the variable is a live shell variable, visibly set,
that this specific `bash` binary just does not forward into any
subprocess's real environment block. The same drop happens indirectly:
running any script via a `bash` that PATH happens to resolve to
`/c/msys64/usr/bin/bash` (for instance after `export
PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"`, which several of
these scripts' own usage comments also show) hits the identical failure,
even with no explicit absolute path anywhere in the command.

Real cost paid for this, this session: a `tools/xbox-dedicated-sweep.sh`
build run with `export XBOX_ENGINE_TESTS=1` reported a clean 96/96
compile and produced a real, bootable XBE - and the resulting
`engine_common_host.obj` contains no reference to `Xbox_NetLogInit` at
all, meaning `XASH_ENGINE_TESTS` was never actually defined and the
network log channel this whole exercise existed to enable was compiled
out the entire time. A full session of xemu port-forwarding, Windows
Firewall rules and UDP listener debugging chased a build that never
attempted to open a socket, and nothing about the build's own "clean"
report gave any reason to suspect it.

**Every one of `tools/xbox-cc.sh`'s scripts finds `clang`/`nxdk-link`/etc.
itself, via its own internal `export PATH=".../mingw64/bin:.../usr/bin:$PATH"`
line** - the `MSYSTEM=MINGW64 /c/msys64/usr/bin/bash` prefix these scripts'
own comments document is not actually load-bearing for that. Invoke them
as `tools/xbox-cc.sh <args>` (bare, relying on their own `#!/usr/bin/env
bash` shebang, with PATH left alone beforehand) instead, and any
`XBOX_*=1` export made in this same dev shell survives correctly - checked
directly by adding a temporary `env | grep XBOX` probe at the top of the
script and confirming the flag was visible with the bare form and absent
with the `/c/msys64/usr/bin/bash` form, holding every other part of the
command identical. **After setting any of these flags, confirm the object
file actually changed shape** (`llvm-nm the.obj | grep TheNewSymbol`) -
"the script exited 0" was exactly the signal that looked fine here while
being wrong.

### The Xbox network log channel went silent for a whole session, with no root cause found - do not treat its absence of output as proof of anything

Earlier the same session, the network log channel (divergence #13) was
confirmed genuinely working: real, correct boot messages came through
reliably across several rebuild-reboot cycles (`chdir("D:") failed:
EINVAL`, `Couldn't find game directory 'valve'`, and - by the absence of
a specific error - a real `gfx.wad` loading successfully). Then, starting
with the very first added diagnostic `Con_Printf` in `engine/client/cl_main.c`
(two lines, bracketing a `VID_Init()` call already known to run), every
subsequent boot - eight or more attempts, several different builds,
including ones that reverted back toward the last known-working shape -
produced **zero** network output, including from `Con_Printf` calls
positioned well before the new code, in functions that HAD printed
successfully minutes earlier in the same session.

Ruled out directly, not assumed: the listener itself (a manual UDP packet
sent to `127.0.0.1:30865` was received correctly while a boot was
producing nothing); a stale/cached build (`strings` on the actual linked
`.obj` confirmed every new diagnostic string was really present); and a
crash (xemu's own process stayed alive and responding, CPU time climbing
throughout every silent attempt, no watchdog reboot loop). What was NOT
resolved: whether `Xbox_NetLogInit()`'s own documented DHCP-negotiation
retry (`net_xbox_log.c`'s own comment - up to ~10 real seconds, not
guaranteed to succeed) was nondeterministically failing across these
specific attempts, or something else entirely. `net_xbox_log.c`'s
`s_net_log_attempted` guard means a single failed attempt disables the
network tier for the rest of that boot, permanently and silently - if
that guard is what triggered, every symptom above (real messages earlier,
total silence later, no crash, no host-side explanation) is exactly what
it would look like.

**Do not read total silence from this channel as "the code never got
there" without a second, independent signal** - a screenshot, a real
crash/hang check via CPU time, or (better, not yet built) a channel that
does not depend on a background DHCP negotiation that can silently fail
once and never retry. A run that logged real messages five minutes ago
proves the mechanism CAN work, not that it currently IS working.

### The "silent" Xbox boot was a NULL callback on the fatal-error path, and every bisection step was measuring a side effect, not the fault

Resolved 2026-09-11, see divergence #34. The whole picture, because each
piece of it was a wrong turn that cost hours:

**What actually happened - two NULL callbacks, one behind the other.**
First: `SV_LoadProgs` calls `SV_InitClientMove` (`sv_game.c:5333`), which
calls `svgame.dllFuncs.pfnGetHullBounds()` (`sv_pmove.c:449`, then
`pfnPM_Init` at `:491`) - members the stub game module never filled. The
trap frame's pushed return address was `SV_InitClientMove+0x88`: `call
eax`, `eax = 0`. Second, once those existed: `CL_LoadProgs("client")`
failed (correctly: no client module exists at this phase). `Host_Error`
during init routes to `Sys_Error`; `Sys_Error` (`engine/common/system.c:411`)
calls `SV_SysError` BEFORE `Sys_Print`; `SV_SysError` calls
`svgame.dllFuncs.pfnSys_Error()` guarded only by `svgame.hInstance != NULL`
- unfilled too, so the very first fatal error of the boot called address
0. On this kernel each is a bugcheck (`KMODE_EXCEPTION_NOT_HANDLED`,
`STATUS_ACCESS_VIOLATION`, `ExceptionAddress 0x00000000`) followed by
`cli; hlt` - a total, silent freeze, with the fully formatted message
"Host_ErrorInit: can't initialize client: missing essential exports"
sitting on the faulting thread's stack, never emitted to any log tier.
The network log channel was never broken.

**The audit that missed them was one call too shallow.** The stub's
header said "these three members, and only these three, are called
synchronously - verified by reading the whole function". It had read
`SV_LoadProgs`. `pfnGetHullBounds`/`pfnPM_Init` are called by a function
`SV_LoadProgs` calls; `pfnSys_Error` by a path nothing in `SV_LoadProgs`
mentions at all. **When a table of callbacks is deliberately left partly
NULL, the audit has to start from the callback sites (`grep` every
`dllFuncs.pfn` use) and walk UP to what reaches them before any frame
runs - not start from one function and read down.**

**Why the bisection pointed at the wrong line for hours.** Commenting
calls out one at a time and rebooting "narrowed the crash" to the
`COM_LoadLibrary("server")` call inside `SV_LoadProgs` - it reproduced
with a hardcoded literal, `Mem_AllocPool`/`Mem_FreePool` around it were
individually safe, the `libs[]` table was intact. It was never the fault
site. It was the last statement whose SIDE EFFECT (`svgame.hInstance`
becoming non-NULL) enabled the NULL call far downstream. Two things made
that invisible: (1) every "crash" had the identical halt EIP, because
every fatal path ends in the kernel's one `cli; hlt` loop - so "same EIP"
carried zero information about WHICH fatal path was taken; (2) every
"healthy" result (`HLT=0`, the kernel's idle-wait EIP, which the
Counter-Strike retail XBE shows at the very same address) was really the
engine erroring out CLEANLY to `Sys_Quit` - not running. **A bisection
that compares "halted vs not halted" bisects the gate on the crash, not
the crash. Read the exception record off the stack first; it names the
fault in one line.**

**Things that were confidently wrong along the way, so they are not
repeated.** (a) Symbolising the halt EIP and the "return addresses"
found near ESP against our own `.map` gave plausible-looking client
functions (`CL_DemoStartPlayback`, `R_BeamRing`); they were kernel
addresses. The title's own addresses are XBE-virtual, `0x10000`-based,
equal to the map's `Rva+Base`, and readable as such (`info mem` shows the
image at virtual `0x10000-0x103d000`); the kernel's `0x80000000+X` alias
is physical X but covers only the kernel (`0x80010000-0x8003d000`), not
RAM at large. Nearest-public-symbol lookup happily answers for any
address, including ones outside the module. (b) "Cannot access memory"
from the GDB stub's virtual reads at `0x80000000+`-alias addresses was
taken as evidence of corrupted/unloaded code. It was the alias ending -
`info mem`/`info tlb` show exactly what is mapped where, and the image is
mapped page-by-page into physical RAM (mostly at `+0xB0000` in this build,
found by searching physical RAM for `COM_LoadLibrary`'s own unrelocated
bytes from the `.obj` and confirmed byte-for-byte including its relocated
`lea`/`call` operands - but NOT uniformly, so never assume a constant
delta; read through the page tables, or physically with `xp`). (c) `xemu
-S` does not hold the machine paused, and
neither software nor hardware breakpoints survive `system_reset` (the
XBE is reloaded over them) - live catching of a sub-second boot path was
never going to work; post-mortem reading is what does. (d) The one
diagnostic that would have ended this in minutes - a working
`Sys_Print` on the error path - was exactly the thing the bug disabled.
When the error channel itself is a suspect, read memory, do not add
prints.

**Two build-tooling traps hit while shipping the fix, both self-inflicted.**
(1) `tools/xbox-link-check.sh` compiles `stub_game/server/server_stub.c`
itself AND sweeps every `.sweep/*.obj` into the engine object set
(`:89`). Hand-compiling that file into `.sweep/` "just to check the
symbols" therefore double-links it: `2 duplicate` (`_GetEntityAPI2`,
`_GiveFnptrsToDll`) on the next link. Check symbols with `llvm-nm` on an
object written ANYWHERE ELSE, or just read the link log's "stub game module
included" line. (2) Two `tools/xbox-link-check.sh` runs in flight at once
race on `.sweep/gen/` and the response file; the loser reports "STUB GAME
MODULE FILE THAT WAS SUPPOSED TO COMPILE DID NOT" plus unrelated shell
errors from `deps/nxdk/bin/activate`. One link at a time, and never trust
an XBE from a run whose log says anything but `0 duplicate, 0 undefined`.

**The tool that ended it.** `tools/xemu-debug.py` (and the standalone
`github.com/mathiasdbo/xemu-debug-tools`): xemu's real GDB stub over TCP,
`regs`/`mem`/`xp`/`stack`/`search`/`sym`/`mon`/`gpu`/`fbdump`. Start
`gdbserver` in the Monitor after the freeze, `stack 1024`, look for
`0xC0000005` - the exception record and the formatted message were both
right there on the first dump, once the dump was read as an exception
frame instead of as a call chain.

**Two more Windows/MSYS file-I/O traps, both self-inflicted, both hit
building the X7 `tools/` chain (manifest/sweep/isolate) on real ~100+
object counts - neither shows up on small inputs, which is exactly why
each shipped once before being caught.** (1) Reopening the SAME file for
append (`>> file`) once per iteration of a 100+-iteration loop silently
loses data on this setup, reproducibly, not randomly: `tools/xbox-game-sweep.sh`
lost 3 of 5 real compile failures' log detail this way (the fail COUNT,
which never went through the file, stayed correct - only the written
detail vanished), and `tools/xbox-game-isolate.sh` independently hit the
identical shape building a module's defined-symbol list across ~100
objects, losing enough real symbols (undercounting 9,349 as 606) that a
genuinely leaked, unrenamed symbol slipped past its own self-check. Fix,
proven twice: open the file ONCE with a persistent file descriptor
(`exec 3>> file`, redirect each iteration's output to `>&3`), close it
once after the loop - never let a loop's iteration count become a
same-file reopen count. (2) `mktemp -d`'s default (the system temp
directory, `/tmp` under this MSYS setup) is not reliably usable for a
file written early and read back only after substantial intervening I/O
- measured directly, standalone, outside any script: a bare
`: > /tmp/x; ls /tmp/x` round trip in one command failed outright.
`tools/xbox-game-isolate.sh` hit this for real: a small allowlist file
written before ~200 external process calls (a `cp` plus an
`llvm-objcopy` per object) came back "No such file or directory" when
read afterward. Fix: root a script's working temp directory under
`$SCRATCH` (`mktemp -d -p "$SCRATCH/tmp"`) instead of accepting `mktemp`'s
system-temp default - every `$SCRATCH`-rooted path in this project's own
tooling has been reliable; the system temp directory, measured twice
now, has not. Neither trap is specific to the tool that first hit it -
any future `tools/xbox-*.sh` doing bulk work across 100+ files should
default to both fixes from the start, not rediscover them.

**Three more real traps hit building and running `tools/xbox-game-link-check.sh`
(X7.4 step 1's real link attempt) for the first time - none of them the
SDK's fault.** (1) `tools/xbox-link-check.sh`'s own mainui-manifest check
called `sed -n '...FAIL=...' "$MANIFEST"` right after compiling 186
third-party objects (heavy I/O) and got EMPTY back, reproducibly, 5 times
in a row - a retry loop around the same `sed` call did NOT fix it (ruling
out a simple sub-second race, the shape that fixed the two traps above),
but the SAME script's own `cat "$MANIFEST"` a few lines later, in the
exact same failing run, read the file correctly every time. Whatever is
wrong is specific to `sed`'s own read path here, not the file or a
generic timing race - avoid depending on it for this kind of extraction
at all rather than chasing why: read the file once with `cat` (proven
reliable in this exact spot) into a variable, then extract fields with
bash's own `[[ =~ ]]` regex against that string, no second subprocess.
**Later identified, likely: the real cause of "whatever is wrong" here
was almost certainly the same `sed`/`grep` GNU-BRE-extension-vs-build
mismatch the "a script's own self-check can be silently, permanently
wrong" entry further down this file root-causes directly (`\( \)`
capture groups silently matching nothing on this project's own pinned
`sed`) - not a timing race after all, confirming this entry's own
correct call to rule timing out, just not what to blame instead.
There were TWO separate `sed` calls against the same manifest a few lines
apart (`FAIL=`/`TOTAL=` together, then a separate `OK=` further down) -
fixing the first let execution reach the second, which then failed the
same way on its own; when a class of bug like this is found, grep the
whole file for the same pattern rather than assuming one fix covers it.
(2) `eval "$(deps/nxdk/bin/activate -s)"` (documented elsewhere in this
project as needed before calling `nxdk-link`) actively REPLACES `$PATH`
on this Git-Bash/MSYS2 setup rather than extending it, substituting a
bare `/mingw64/bin` for the real `/c/msys64/mingw64/bin` - `command -v
lld` resolved correctly immediately before that call and "command not
found" immediately after it, same shell, nothing else changed.
`tools/xbox-link-check.sh` itself never calls `activate` at all and its
own `nxdk-link` calls work fine on the plain `MSYS2_BIN` `PATH` export
every `tools/xbox-*.sh` script already uses - that alone is sufficient;
don't add `activate` on top of it. (3) Excluding stub-module boilerplate,
a module that reuses a genuinely SHARED SDK utility file (here,
`public/interface.cpp`, restored to the `hl` server target for
`Sys_LoadModule`/`Sys_GetFactory`'s sake, divergence #35/#36) can pull in
a function with a real `/EXPORT:` directive that has NOTHING to do with
that module's own role - `CreateInterface` is also compiled into
`filesystem_stdio`/`ref_null`/`menu`, each for their OWN reason, and
X7.1b's "keep the objects' own `/EXPORT:` set" design correctly, by its
own literal rule, kept it unrenamed in `hl`'s server target too, causing
a real duplicate-symbol collision the first time all of them shared one
link. The fix is not "shrink the rule", it's applying X7.1b's own STATED
principle more precisely: the allowlist is what the ENGINE resolves for
THAT module, and nothing else resolves "CreateInterface" for the
"server" role specifically - `tools/xbox-gen-exports.sh` gained a
narrow, measured `--exclude <file>` for exactly this shape (a real
`/EXPORT:` name that must NOT be treated as this module's public
surface), not a guess or an invented heuristic.

### `tools/xemu-debug.py`'s `regs`/`mon "info registers"` show a fixed debugger-attach checkpoint, not the guest's true current state, while it is actually running

Found diagnosing the real `hl` server boot's first two crashes (divergence
#38). A GENUINE crash (the `__xbox_assert` `cli; hlt; jmp 1b` loop from
`deps/nxdk/lib/pdclib/platform/xbox/functions/assert/assert.c`) shows up
exactly as expected: every `regs` read returns the identical `EIP`, and
that's real, because the guest genuinely is halted there. But after
fixing that crash, a FRESH attach to a build that should have been
running freely kept reading `EIP=0x8001b02f` (kernel space, `HLT=0`) on
every single connection - regardless of whether the wait before
attaching was 5 seconds or 120 - with the stack/registers otherwise
identical too. That is not a real guest freeze: confirmed by sending a
genuine GDB `continue` (`GdbRemote.cont()`, not `mon "..."`, which goes
through `qRcmd` and requires the target already stopped to answer at
all) over that same connection and it ran for the full 90-second timeout
with no further stop reply at all - a truly stuck guest would have
re-hit whatever trapped it almost immediately, the way the real assert
loop always did. **`mon` commands and a bare `g` (registers) read both
require the target already stopped to answer, so xemu's gdbstub pauses
ALL vCPUs at its own fixed, deterministic re-entry point the moment a
client asks for state - that point is a debugger-implementation detail,
not "wherever the guest currently is."** Do not read a `regs`/`mon`
snapshot's `EIP` as "the guest is stuck here" without first issuing a
real `continue` and confirming it does NOT immediately re-stop at the
same address.

**Found in Codex review, round 2, on an earlier version of this same
entry: a `continue` that runs past a timeout with no stop reply only
disproves ONE specific thing - that the guest is repeatedly re-trapping
at the same instruction, the way a real crash-halt loop does.** It does
NOT prove the guest is "healthy," making real forward progress, or past
any particular point - an infinite loop, a deadlock, or a legitimate
kernel idle-wait would produce the exact same observation (no stop
reply, ever) and are not distinguishable from each other by this signal
alone. The corrected, narrower claim: this technique rules OUT "still
crashing on the SAME bug as before," nothing more. Calling a boot
"confirmed working" or "verified end to end" still needs an independent
forward-progress signal - a breakpoint set at the actual next expected
milestone (`break <addr> --steps N` and watching it actually advance),
repeated PC sampling that shows genuinely different addresses over time
(not just "no stop reply"), or a working log channel - not merely the
absence of a repeated trap. A screenshot (`tools/xemu-debug.py
screenshot`) is a cheap fourth signal but not conclusive on its own for
a headless `ref_null` build: it showed the plain Xbox boot-splash logo,
unrun and unchanged from BIOS init, which is exactly what an unwritten
framebuffer looks like under a renderer that (correctly, by design at
this milestone) never calls `VID_SetMode` at all - consistent with
either a healthy headless boot or an earlier hang, not distinguishing
evidence either way on its own.

### A real breakpoint at a resolved milestone address is the reliable "did the boot get here" signal - a `continue` timeout alone is not, and neither is "some call reached a shared function"

Follow-up to the entry above, same investigation, corrected TWICE by
Codex review before it was actually rigorous. X7.4 step 1's real exit
criterion (`CL_Init` reaching and failing its client-module load,
calling `Sys_Error` with a specific message) needed a genuine yes/no
answer, and the network log channel - the tool the plan text originally
named for this - produced no output at all, across every attempt,
indistinguishable from its own already-documented pre-existing
unreliability.

**First attempt (corrected in round 1): a bare breakpoint hit on
`Sys_Error` is NOT enough.** Resolved `Sys_Error`'s real address from a
fresh `-map:` linker map and set a breakpoint there
(`tools/xemu-debug.py break <addr> --steps N`) - it hit, with a genuine
moving single-step trace afterward. That is real evidence the function
was ENTERED, but `Sys_Error` is a shared fatal-error sink with several
real callers before `CL_Init` ever runs (a filesystem load failure, a
missing-gfx check) - "the breakpoint hit" alone cannot tell WHICH caller
or WHICH error fired, and claiming the specific milestone from that
alone is exactly the same class of unsupported leap as inferring health
from a `continue` timeout.

**The actual fix: read the function's own arguments off the stack at
the SAME breakpoint hit, before its prologue can clobber them.** cdecl
means, right at entry, `[ESP+0]` is the return address (the real
CALLER) and `[ESP+4...]` are the function's own parameters -
`GdbRemote.read_mem()` reads them directly. The return address resolved
(via the same map) to the SPECIFIC expected caller
(`Host_Error + 0x63`, not any other `Sys_Error` call site), and
`Sys_Error`'s own `(const char *error, ...)` format string plus its two
varargs, read the same way and dereferenced as C strings, assembled to
the EXACT expected message text, word for word - not inferred from the
symptom, decoded directly from guest memory. **When a milestone is "does
the guest reach function X for reason Y", a bare breakpoint on X only
proves X was entered by SOMETHING - read the caller (return address) and
X's own real arguments off the stack at that same stop to prove it was
Y specifically, especially when X is a shared/generic sink (error
handlers, logging funnels, common allocators) with more than one real
caller.** Confirmed separately useful here too: this also showed the
network log channel is silent even for a message DEFINITELY, VERIFIABLY
formed and about to be sent - stronger evidence than any prior session
had that the channel's unreliability is real and independent of
whatever code path is trying to use it, not something a future fix
should assume away.

**A third gap in the same investigation, found before Codex review even
saw it this time: proving the LATER step in a chain fired correctly
does not prove the EARLIER step it depends on actually succeeded.**
Decoding `Sys_Error`'s caller and message proved `CL_Init` reached its
real failure path - but `SV_LoadProgs`'s own success or failure right
before that was never independently checked, and its return value gets
silently discarded by its own caller (`Host_Init` runs `CL_Init`
regardless). A failed server load and a successful one would have
produced the EXACT SAME `CL_Init` evidence. Fixed the same way, aimed
one step earlier: a breakpoint on the SDK's own real target function
(not a shared sink), reading its caller off the stack the same way to
confirm the call came from the SPECIFIC expected place. Its return
value needed one more refinement: single-stepping a fixed count to
"exit the function" is unreliable if the function calls something else
internally (here, `GetEntityAPI2` fills its output struct via `memcpy`,
so a step count sized for a small, simple function landed mid-`memcpy`,
not back at the caller) - the robust fix is a SECOND breakpoint at the
call site's own captured return address (already known from the first
breakpoint's stack read, before ever single-stepping anything), which
fires exactly when the callee ACTUALLY returns regardless of what it
did internally, with `EAX` holding its real return value at that exact
instant. **A milestone chain ("A succeeded, which let B reach its real
failure") needs positive evidence for EVERY step whose result is used
downstream, not just the last one that happens to be easiest to
observe - especially when an intermediate return value is silently
discarded by its own caller, which is itself worth grepping for before
trusting that a later symptom implies an earlier success.**

**A real bug in `tools/xemu-debug.py` itself produced a false "the
breakpoint never fires" signal during the first real combined
server+client+freevgui boot attempt (X7.4 step 2): killing the script
via an external `timeout N` wrapper while it was blocked waiting for a
`c` (continue) stop-reply closes the TCP connection out from under it,
and xemu's gdbstub responds to that abrupt disconnect by auto-halting
the CPU and queuing a synthetic `T02` (SIGINT-style) stop-reply for
whoever connects next. The NEXT invocation's own `qSupported` handshake
then silently consumes that stale packet instead of its own reply, and
the real `qSupported` reply arrives one packet late, surfacing later as
an `<unexpected packet: ...>` in an unrelated `monitor()` call - so
"the debugger looks broken" and "the breakpoint never fires" were, for
several attempts, actually "my own tooling killed the connection and
desynced the next one," not a fact about the guest. **Fixed properly**
(not worked around, and not with a guessed timing window either - two
Codex review rounds on the fix itself found real gaps in both a first,
raw-byte drain and a second, fixed-200ms-window version):
`GdbRemote.__init__` now runs a real handshake (`_handshake`) that sends
its own `qSupported` and keeps discarding - acking each one, since an
unacked packet can make a real stub withhold whatever it queues next -
anything that isn't actually the `qSupported` reply (told apart by
shape: a stop-reply/console/error packet always starts with one of
`S`/`T`/`W`/`X`/`O`/`E`, or is exactly `OK`; a real `qSupported` reply
never does) until the real reply arrives or a bounded deadline is hit,
instead of assuming a fixed window of silence proves the connection
starts clean (`tools/test_xemu_debug.py`'s `HandshakeSynchronization`
covers a stale packet, a real reply arriving after a real delay, and no
reply ever arriving, over a loopback `socketpair()` - no emulator
needed). `cont()`/`step()` now also catch a local read timeout and raise
a clear `RuntimeError` explaining that the `c`/`s` was already sent and
NOT undone - use the script's own `--timeout`, not an external
process-killer, and prefer `mon "cont"` / `mon "info registers"` (the
QEMU monitor passthrough) when a raw GDB stop-reply proves unreliable
for a given target. **Never let a debugger's own retry/timeout wrapper
close the transport out from under a request that is still genuinely in
flight on the target side - the target does not know the client gave
up, and the next session inherits
the mess.**

**With that fixed, the real result for X7.4 step 2's first boot
attempt (server+client+freevgui, real retail `valve/` assets, real
XISO, real xemu launch) is a genuine, well-evidenced finding, not an
inconclusive one: the guest is confirmed alive (never `HLT`, varied
real EIPs seen inside genuine engine code, e.g. `_memset`/`__Mem_Alloc`
activity), the breakpoint mechanism itself is confirmed working (an
immediate, exact hit with correct single-step progression at a
known-hot address, `_memset`), but a breakpoint at the real,
map-resolved `Host_Frame` address never fires across 150+ real,
uninterrupted seconds of continue time. Every async pause lands at the
exact same instruction, `sti; nop; nop; cli; cmp ebp,[ebp+0]; je ...`
at a fixed kernel address - the Xbox kernel's own idle/ready-queue-poll
loop, identified by disassembling the bytes at the repeatedly-observed
EIP and recognizing the self-referential list-empty check
(`cmp ebp,[ebp+0]`), with the same `EBX`/`EBP` values in every reading.
Landing on the EXACT SAME instruction on independent async
disconnect-triggered halts, over many real seconds, is strong evidence
the CPU is spending the overwhelming majority of its time there, not
that it was merely sampled there once. **This conclusion turned out to
be wrong in an instructive way - corrected below, not deleted, because
the wrong turn itself is the lesson.**

**Software breakpoints set via the raw GDB `Z0` packet persist on the
QEMU/xemu target across BOTH a client reconnect AND a full
`system_reset` from the monitor - a real, useful xemu gdbstub property,
not a bug, but one that quietly wrecked the very next round of this
same investigation.** Chasing the "genuinely blocked" hypothesis above
meant setting and clearing many breakpoints across many short-lived
throwaway scripts (one Python process per probe, each opening a fresh
`GdbRemote` connection) - and more than one of those scripts exited
after its own probe without ever removing the breakpoint it had just
set (`z0`). Because those breakpoints stayed armed - through later
reconnects, and through more than one `system_reset` in between -
LATER, unrelated probes kept re-triggering them and misreported real
forward progress as "stuck here again," including one at the exact
return address of a function (`usbh_pooling_hubs`) that had already
been directly proven to return quickly. Once every address the whole
investigation had EVER armed was enumerated and explicitly swept clean
(`z0` for each, checking the reply rather than assuming it worked), the
picture changed completely: nxdk's own SDL2 Xbox joystick driver's
real, intentional ~500ms USB-enumeration-settle loop
(`SDL_xboxjoystick.c:195-199`) - the thing the "genuinely blocked"
theory above had settled on as the likely culprit - was confirmed to
return normally in ~2.2s, never the blocker at all. **A breakpoint
debugging session that spans multiple short-lived client processes
against the SAME long-running target needs its own bookkeeping of
every address it has ever armed, checked and cleared explicitly before
trusting a "stuck here" result - the target does not forget a
breakpoint just because the client that set it exited, or because the
guest itself was reset.**

**The real blocker, found only by following the ACTUAL "it quit" signal
to its real source instead of stopping at "it's stuck": a real
`Sys_Quit("command")` was firing reliably ~2.5-3.3s into every attempt,
restarting the whole init sequence from scratch each time - easy to
mistake for a fixed-point hang if you only ever sample CPU state, not
control flow.** Tracing its actual call chain (`Cmd_ExecuteString` →
`CL_Quit_f` → `Sys_Quit`, each hop confirmed by reading the real return
address and arguments off the stack at a breakpoint, the same
caller-and-arguments technique documented earlier in this file) led
straight to a real, deliberate HL SDK safety check
(`UTIL_IsValveGameDirectory()`) refusing to run because this fork's own
real retail Half-Life assets sit in a directory literally named
`valve` - see divergence #44. **A command that quits the process
cleanly, on a fixed schedule, driven by real application logic, can
look identical from the outside to a genuine hang if the only evidence
gathered is "where is the CPU right now" - reading WHAT commands
actually get executed (breaking on `Cmd_ExecuteString`/`Cbuf_AddText*`
and decoding their real string arguments) is what actually distinguishes
the two, and is worth reaching for before concluding "stuck" from
register samples alone, however consistent those samples look.**

**A `STATUS_STACK_OVERFLOW` bugcheck whose ESP is still near the top of
the stack is not recursion - it is one frame that can never fit, and
the stack size itself is the suspect.** The X7.4 step 2 overflow
(divergence #47) had every outward sign of runaway recursion, and three
plausible hypotheses about the video-init path were each checked live
and ruled out before the actual clue was read properly: the bugcheck
frame's ESP (`0xd005d24c`) sat only ~2 KiB below `Host_Frame`'s own
entry ESP (`0xd005d994`). Nothing recursive had happened yet. That
arithmetic alone says "a single allocation bigger than the whole
remaining stack" - and reading nxdk's `_xlibc_check_stack` confirmed
exactly that contract: `__chkstk` calls it with the requested frame
size, it compares against the thread's `StackLimit`, and raises
`EXCEPTION_STACK_OVERFLOW` as `NONCONTINUABLE` on the spot. From there
the fix was one question ("how big is the stack, and who set it?") and
the answer was `nxdk-link`'s hardcoded `-stack:65536`, inherited by
every `CreateThread(..., 0, ...)` including the one `main()` runs on.
**Two techniques made the catch cheap and unambiguous, both worth
reusing: (1) when the probe/check function is hot during init, arm its
breakpoint only after the milestone breakpoint has already fired - here
`Cmd_TokenizeString` probes a 32 KiB frame for every console command,
so an unfiltered catch burned a 400-hit budget on config execution
alone and never reached the frame loop; (2) record every probe's
requested size and requester (the requester's return address is at
`stack_ptr - 4`, since `__chkstk` hands the check function the caller's
pre-call ESP) and take the last one before `RaiseException` fires - no
stack walk needed, and no guessing about which of several plausible
big-buffer functions was actually the one.** The corollary for a port:
before attributing an overflow to engine code, check what stack size
the platform's runtime actually hands the main thread - upstream's own
small-default ports (DOS, Emscripten) had already had to raise it
explicitly, and that precedent was sitting in `scripts/waifulib/` the
whole time.

**The Z0-breakpoint-persistence gotcha above bit the SAME investigation
a second time, days later, once careful address-tracking stopped being
enough on its own.** Verifying `UI_IsVisible()`'s real return value
(confirming the menu genuinely activates, not just that a request
reached it) produced a clean, confident-looking `EAX=0` (false) twice
in a row - real breakpoints, real matched return addresses, no obvious
red flag. It was wrong both times: a missing single-step-over-the-
just-hit-breakpoint before re-arming and continuing (the CPU, still
sitting exactly at the breakpoint's own address, re-triggers instantly
with zero real execution - `esp` unchanged before and after `cont()` is
the tell), compounded by unrelated stale breakpoints from an earlier
script in the same session surviving a `system_reset` in between. A
`system_reset` clears guest CPU/memory state but NOT the debugger's own
Z0 breakpoint list, which is emulator-process state, not guest state -
so once a live investigation has run more than a handful of throwaway
scripts, each setting breakpoints at dynamically-read return addresses
that are impractical to track and remove exhaustively, **the reliable
fix stops being "remember to clean up" and becomes "kill and relaunch
the whole emulator process before the next verification that actually
matters"** - a genuinely fresh gdbstub has an empty breakpoint list by
construction, which no amount of careful bookkeeping in the client
script can guarantee once the session has been running for a while.
The real, correct answer (`EAX=1`, confirmed on the first read after a
full process restart) was the OPPOSITE of both contaminated reads -
worth remembering next to the original lesson: a clean-looking result
from a live debugging session is not the same as a result from a clean
session.

**A "verified live" result taken against the wrong binary looks exactly
like a real one - every breakpoint still hits, on the wrong function -
and the only defence is to check the guest's bytes against the file
before trusting a single mapped address.** After divergence #49's fix
was built, packed into `.sweep/boot-attempt/iso` and "verified" (the
previously-hung boot "now reached `SV_ExecLoadLevel` immediately"), the
next milestone - `SV_SpawnServer`, called two instructions later in the
same function - never fired in 900 seconds, and a single-step trace
from the "`SV_ExecLoadLevel`" hit showed a seven-instruction function
returning into the middle of `COM_LoadLevel`. Not the source's
`SV_ExecLoadLevel` at all. The linked PE (`xbe-build.exe`) and the
mapped one were byte-identical in `.text`, so the map was right; the
GUEST was wrong: reading the 20 bytes at that address out of guest
memory gave `cmp [global],0 ; setne al ; ret`, not the file's `call
SV_SetStringArrayMode`. xemu's `xemu.toml` `dvd_path` named
`resonance3d-x74-boot.iso` - a copy from before the fix, sitting next to
the freshly packed `iso` - so every post-fix run had booted the old
build, and PR #58's "verified live" claim was made against it (corrected
in divergence #49). The tell was available from the very first post-fix
run: the old build's own function at the new build's `COM_LoadLevel`
address happened to also be reached at ~4.2s, so nothing looked off
until a milestone two instructions away refused to fire. **Rule: the
first thing a live session does after packing a new image is
`tools/xemu-debug.py verify-image <linked .exe>` (added for this; it
compares EVERY file-backed byte of the read-only sections, exit 1 on
any mismatch), and a breakpoint result is not evidence until that has
passed on the same running guest.** Its first version sampled 12
symbols and was rightly rejected in Codex review: a stale build that
differs only where it was not sampled passes a sample, which is the
same blind spot in a smaller coat. The full comparison found one real
subtlety worth knowing: the last 25 KiB of `.rdata` legitimately differ
- libxboxkrnl's `__imp_*` kernel thunk table holds ordinals
(`0x800000NN`) in the file and the resolved `0x8001xxxx-0x8003xxxx`
kernel addresses in the guest, patched by the loader - so the tool
recognises exactly that dword shape and nothing else.

**An `SDL_assert` on nxdk is a landmine with no bang: SDL's own
handler wants to ASK someone, and a console has nobody to ask.** nxdk
builds SDL2 with `SDL_ASSERT_LEVEL 2`, so every `SDL_assert` in it is
live - unlike the release SDL2 a PC build links, where the same checks
compile to nothing. When one fails, `SDL_PromptAssertion` tries a
message box (none on Xbox), then a stdin prompt (none either) and gives
up with `SDL_ASSERTION_ABORT` -> `abort()` -> reboot; or, depending on
what the failing path was doing, simply never returns. Divergence #49
was the never-returns case (a hang that took a session to name); X5's
first picture attempt was the abort case (a boot restarting every 2.9 s
with NO `Sys_Error`, nothing on any log tier - `SW_CreateBuffer` was
simply never reached and the milestones before it kept repeating). Both
were engine code calling an SDL API in a way a release SDL tolerates
(`SDL_GetRendererOutputSize( NULL )` returns -1 quietly in release; it
asserts in debug). **Two rules.** (1) A boot that restarts on a fixed
short period with no error printed is an `abort()`: arm
`SDL_PromptAssertion` (and `abort`/`exit`/`Sys_Quit`) after the last
milestone that does fire, and read the `SDL_AssertData` off the stack
at the prompt - condition, file, line, function are all right there
(`tools/xemu-debug.py`'s `stack`/`mem` do it; the halted-frame script in
this session read them in one shot). (2) Do not let it happen a third
time: `Xbox_Init` installs an SDL assertion handler that routes the
assert into `Sys_Error` with those same fields, so the NEXT one is a
one-line error on the log channel, still fatal, no session spent.

**A framebuffer capture has to be taken while the guest is halted at the
moment you mean, or it shows whatever was on screen fifty seconds
later.** `tools/xemu-debug.py screenshot` reads 1.2 MB over the stub -
~50 s per frame here - so a time-based "shoot at t=3 s / t=12 s"
schedule captured the nxdk boot splash three times and looked, for a
whole round, like "the title never draws" (it did; the mode was set at
3.8 s and the process was gone again by 8.6 s). PCRTC START still
pointing at the splash's `0x3d040` buffer was the tell - a title that
has set a mode points it at a fresh contiguous allocation. What worked:
break on the RETURN of `SDL_XBOX_UpdateWindowFramebuffer` (entry, read
`[esp]`, break there, continue), then read PCRTC START and the frame
physically over the halted stub, however long that takes. Also from
this round: a `mon system_reset` issued by a connect-and-disconnect
client leaves xemu stopped at the reset vector (`EIP=0xfff0`); send the
reset and a `c` on one connection before disconnecting.

**"Out of memory" has two questions, and the kernel answers only the
first.** `memstats` (the `MmQueryStatistics` counters) says HOW MUCH is
gone - `VirtualMemory` 30 MiB, 0.97 MiB free - and stops there; every
lever after that would have been a guess. The engine already keeps the
answer to WHAT: `zone.c`'s `poolchain` is an array of named pools with
running `totalsize`/`realsize`, one per subsystem and one per loaded
model, readable straight out of guest memory at the map's `_poolcount`/
`_poolchain` (now `tools/xemu-debug.py pools`). One dump with the map
up turned "the map needs 30 MiB" into "`ref_soft zone` 12 MiB, the BSP 3,
edicts 3, sounds 3, ..." - which is a work list, not a mystery - and
turned `XASH_LOW_MEMORY=1` from a hopeful flag into a measured 11 MiB of
image (statics, from the linker map's symbol deltas) that made the map
fit. Take both readings at the same halted breakpoint, on a
`verify-image`-confirmed guest, and re-derive the pool addresses from the
CURRENT map: the first dump read 0 pools because the addresses came from
the map of the build before.

**A read-only debugger connection freezes the guest, and xemu does not
thaw it when the client leaves.** Measuring frame rate as "read
`host.framecount`, wait 30 s, read it again" reported "frame counter did
not advance" twice, in two separate boots, with the SAME count (631)
both times - which looked like a deterministic hang 45 s into the map
and was nothing of the kind. QEMU's gdbstub stops the VM the moment a
client connects; a bare socket close does not resume it, and neither
does a proper gdb detach (`D` - tried, EIP and ESP identical across two
`regs` 3 s apart). Only a continue does: the `c` packet, or the
monitor's own `cont`. So every earlier "connect, read, disconnect"
(`regs`, `mem`, `screenshot`) had been leaving the guest paused, and 631
was simply the frame the driver script's first `regs` landed on each
time. `GdbRemote.close()` now sends `mon cont` on the way out (a
synchronous reply, so no outstanding `c` becomes a stale stop-reply for
the next connection), and the guest runs between two CLI invocations as
if no debugger were attached. **The tell was the same number twice**: a
hang lands where the code takes it; a pause lands where the clock takes
it, and a fixed script schedule makes the clock repeatable.

**Struct offsets for live reads come from the compiler, not from
counting fields.** `host.framecount`'s offset (596 in this build) was
needed to read the counter, and `host_parm_t` starts with a `game_status_t`
whose two `MAX_QPATH` arrays had just changed size with
`XASH_LOW_MEMORY=1` - the hand-counted offset X7.4 used for
`nextstate` would have been wrong by 32 bytes on this build.
`tools/xbox-offsetof.sh <header> <type> <field>` compiles a one-line
probe through `tools/xbox-cc.sh` (so every real `-D` applies) that
fails on purpose with `char (*)[N]` in the error text, and prints N.
Zero guesswork, one second, and it is what `xemu-debug.py fps` is
calibrated with. Two smaller lessons
from the same day, both real: (1) a client-side `cont()` timeout does
not stop the guest, and a stop-reply that arrives after the client gave
up is delivered to the NEXT connection's handshake - `GdbRemote` drains
and discards it (with a warning) rather than misreading it as a reply
to the new command, so "discarded 1 stale packet" on connect means the
previous probe's breakpoint DID eventually fire, unseen; (2) with the
right image booting, X7.4 step 4's whole chain ran in under two seconds
and the tram changelevel followed in 43 - the "900-second slowness"
being explained as first-time directory scanning was another artifact
of tracing the wrong binary, never a real finding.

### ALWAYS confirm which module composition produced the XBE under test before diagnosing engine behavior from it

Four separate live-boot attempts (X9-A through the divergences.md #92
investigation) all built with `tools/xbox-game-link-check.sh hl --xbe`
(the still-stubbed client, correct for server-only testing) and then
read a "black screen, frozen `memstats`, no forward progress" as a
mysterious engine hang affecting both `ref_soft` and `ref_nv2a` alike.
It was neither: `CL_LoadProgs()` (`engine/client/dll_int/cl_game.c`)
correctly, deliberately hard-fails against a stub client missing its
required exports, and `Host_Error`→`Sys_Error` correctly halts the
console - exactly the observed symptom, with zero renderer involvement.
The real, already-proven-working combination
(`--include-client --include-freevgui`, closed in X7.4 step 2,
divergences #41-48) was sitting in this same file's own git history the
entire time. **Before spending a debugging session on "why does this
boot behave strangely," re-read the exact build/link command that
produced the binary being tested and compare it against what a working
milestone for that same boot shape actually used - a link summary line
(`hl server - ...` vs `hl server+client+freevgui - ...`) is one grep
away and settles in seconds what hours of live instrumentation would
otherwise "discover" as a false engine defect.** See divergences.md #93.

It recurred for a memory comparison (X9-X step 5, 2026-09-26): a "main"
XBE linked without the menu module and a branch XBE linked with it -
both identity sidecars said `modules=server+client+freevgui`, because
mainui joins the link whenever `$SCRATCH/mainui` holds a clean sweep
and no flag records it - and the menu's fonts and background (4.6 MiB
contiguous) plus its heap and code were chased as a 10 MiB studio-model
regression. The check that settled it was one grep: count `mainui`
symbols in both linker maps (0 vs 3292). **For a before/after
measurement, compare the two linker maps' object lists, not the labels;
the sidecar now carries `mainui=` for this reason.**

### A script's own self-check can be silently, permanently wrong - when a "flaky" tool fails identically every time, stop calling it flaky and diff its check against ground truth

`tools/xbox-ref-isolate.sh`'s leak-check (`0 leaked` required before a
renamed module is trusted) reported real failures - "112", "114",
"585" symbols leaked - on every single run, for both `ref/soft` and
`ref/nv2a`, across an entire session, with `tools/xbox-link-check.sh`
itself stalling or reporting the same false failures further up the
same chain. The printed "leaked" symbols were themselves already
correctly prefixed (`_soft_GetRefAPI` reported as failing a check for
"starts with `soft_`") - the first, cheap, honest signal that the
*check* was wrong, not the renamed objects, and that this was never
about I/O timing at all. Two plausible-sounding, individually-testable
hypotheses were tried and both failed to fix it (a stale re-read of a
just-written text file; a stale re-read of the just-`objcopy`'d `.obj`
files, "fixed" with `sync`) before a byte-level, side-by-side test
found the real cause: `grep`'s `\?` (the GNU BRE "optional preceding
character" extension) is silently treated as a LITERAL
backslash-question-mark by `/c/msys64/usr/bin/grep.exe` specifically -
the exact binary this script's own `PATH` export resolves `grep` to -
even though it self-reports the identical `GNU grep 3.0` version string
as the grep every interactive shell on this same host uses, where `\?`
works. `echo _soft_GetRefAPI | grep "^_\?soft_"` (msys64's own
grep.exe) matches nothing; the same test through `grep -E "^_?soft_"`
matches correctly on both grep builds. This project's own lessons.md
already had an entry for this class of bug in a *different* script
(`tools/xbox-check-nxdk-cc-drift.sh`, "PATH, not the pattern, first")
and it recurred anyway, undetected, in a script every X9 slice's own
review process had been trusting for weeks.

**When a measuring tool fails "flakily" but the SAME way every time
(not intermittently, not with varying numbers), it is not flaky - a
truly random environment fault does not reproduce byte-identical
output across independent runs.** Deterministic-looking failure is
itself evidence the bug is in the tool, not the environment. Before
accepting "the environment is unhealthy" as the explanation and moving
on (documenting it as an open gap is not wrong, but stopping the
investigation there was too early here), check whether the tool's own
report is internally self-contradictory the way this one was - a
"failing" line that already shows the expected content is the tell.
And once a script pins its own `PATH` for `clang`/`nm`/`objcopy` (for
good reason - reproducibility), remember that pin applies to `grep`/
`sed`/every other tool the script calls too, not just the ones it was
written to pin for.

**Auditing the rest of `tools/*.sh` for the same class of bug found two
more real, live instances, not hypothetical ones** - confirming this
was worth a real search, not a one-off. `tools/xbox-gen-exports.sh`'s
own `find_symbol_for()` used `grep -x "_${name}@[0-9]\+"` (unescaped-vs-
backslash-escaped `+`) to find stdcall-decorated export symbols - broken
the identical way, meaning it had been silently failing to find every
real stdcall match since it was written. `tools/xbox-link-check.sh`
used `grep -v '/ref_\(soft\|common\)_[^/]*\.obj$'` (backslash-escaped
grouping+alternation) as a safety net against a real, previously-
measured failure mode (X5: leftover raw `ref/soft`/`ref/common` compile
artifacts sitting next to the isolated, renamed set, recreating the
duplicate symbols isolation exists to remove) - broken the same way, so
that net had been silently doing nothing since it was written; it never
surfaced as a real duplicate-symbol failure only because this project's
current sweep scope happens to never compile `ref/` at all, not because
the guard worked. **Interesting, narrower finding worth recording
precisely**: plain `\|` alone (no grouping) was tested directly and DID
work correctly on the same buggy grep build - only `\?`, `\+`, and
`\(...\)` grouping (bare or combined with `\|`) were confirmed broken.
Do not over-generalize "all GNU BRE extensions are broken here" from
one build's behavior on a subset of them - test each one you rely on.
Prefer `grep -E` with the plain, unescaped forms (`?`, `+`, `(...)`,
`|`) everywhere going forward regardless, since it sidesteps the whole
question rather than relying on which specific escapes happen to work
on which grep build.

**A `build-toolchain-specialist` review of the fix (independently
re-testing every claim rather than trusting the commit) found a THIRD
live instance, in `sed` rather than `grep`:**
`tools/xbox-dedicated-link-check.sh` used
`sed -n 's/OK=\([0-9]*\).*/\1/p'` - the same backslash-escaped BRE
capture-group grouping, broken the same way on this project's own
pinned `sed`, unconditionally rather than intermittently. This left
the script permanently unable to run at all (its own `-z` guard turned
the empty extraction into an immediate loud failure, not a silent bad
pass - the one mercy in this instance). Fixed by matching the pattern
`tools/xbox-link-check.sh`'s own mainui-manifest check already
uses for this identical file shape: read the manifest once with `cat`,
extract every field with bash's own `=~`/`BASH_REMATCH`, no second
`sed` subprocess at all. **That sibling script's own fix predates this
investigation and was attributed to a different cause at the time**
("sed right after heavy I/O returns empty" - the file-consistency
theory this file's own earlier entry on that topic describes). With
what this investigation now knows, that original diagnosis was very
likely ALSO the same BRE incompatibility, not an I/O race - the fix
(stop depending on this project's own `sed`'s `\( \)` support at all)
happens to be correct either way, but the recorded ROOT CAUSE for that
older entry may itself be wrong. Left as-is rather than rewritten,
since the fix it produced was right regardless of which theory
explained it - but a reader relying on that older entry's own causal
story specifically (not just its fix) should treat it as suspect.

**Codex review then disputed this specific finding, with its own
directly-run counter-test claiming `sed -n 's/OK=\([0-9]*\).*/\1/p'`
against `OK=96 FAIL=0 TOTAL=96` outputs `96` correctly on this exact
pinned `sed` - i.e., that the capture group works fine and the fix
above was unnecessary. Re-tested independently, a third time, with the
MOST faithful possible reproduction of the real script's own execution
shape (a real script FILE - not an inline `-c` string - with the exact
`export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"` line
every `tools/xbox-*.sh` script uses, invoked as `MSYSTEM=MINGW64 bash
script.sh` against a real file in the real working directory, never
`/tmp`):**
```
sed resolves to: /c/msys64/usr/bin/sed
/usr/bin/sed (GNU sed) 4.9
RESULT=[]
```
**Empty - confirming the original finding, not Codex's refutation.**
The discrepancy traces to invocation context, not the sed binary
itself: `/c/msys64/usr/bin/sed.exe` (absolute path) and `/usr/bin/sed`
(this project's own interactive Git-Bash session's own resolution) are
BYTE-IDENTICAL (`md5sum` confirmed, same hash) yet gave different
results for the identical command depending on which path string
invoked them and which shell/environment was live at the time - the
same "PATH, not the pattern, first" shape this whole family of bugs
has now shown three times over, one level deeper than expected (not
just "which binary", but "which invocation context for the SAME
binary"). Per `AGENTS.md`'s own fix-or-justify policy, this is the
written justification for NOT reverting the fix Codex's own finding
asked for: the counter-test that finding relied on did not reproduce
the real script's own actual invocation shape, and a test that does
reproduce it - three times now, independently - shows the original
bug is real. The `BASH_REMATCH` fix stays; it is also strictly
correct regardless of which way this dispute resolves, since it drops
the dependency on this ambiguity entirely rather than resting on it.

**Codex review round 2 (final, per `AGENTS.md`'s two-round cap) found one
more real, separate gap while reviewing this same block**: the
post-rewrite `llvm-nm` scan that feeds the leak-check silenced its own
stderr and never checked its exit status, unlike the earlier, already-
hardened compile loop a few lines above it in the same script - a
failing `nm` on one object could have silently dropped that object's
symbols from the count, letting a real leak in that specific object go
undetected while the script still reported `LEAKED=0`. Fixed to match
the already-established, already-correct pattern used elsewhere in this
same file: capture stdout/stderr separately, abort loudly on any
nonzero exit, and reject an empty post-rewrite symbol set outright.

See `docs/r3d/divergences.md` #97/#98's own resolution notes for the
full account, including the real `0 duplicate, 0 undefined` link
result this fix finally produced.

### ALWAYS isolate ONE variable per boot attempt, and check the .mpak archive-discovery scan directory specifically - a real, confirmed bug blocked every prior live X9 boot attempt this session, silently, and a second suspected "bug" was never actually isolated

Setting up a real xemu boot of a real, baked `crossfire.mpak` to finally
close X9-G/H/I's own "not yet live-boot confirmed" gap took three
attempts, two of which changed more than one thing between boots - a
real methodology mistake, caught by `codex-review` on the first draft
of this entry, not by this session's own review of its own work. Keep
both the real confirmed bug AND the correction on record, since the
correction is itself the more generalizable lesson.

**Confirmed: an `.mpak` answering to `maps/<name>.bsp` must sit at the
GAME DIRECTORY'S OWN ROOT, not inside `maps/`.** `filesystem/mpak.c`'s
own header comment is clear that a mounted `.mpak` archive "remembers
which single virtual name its one archive answers to" (derived from
its own basename: `crossfire.mpak` -> `maps/crossfire.bsp`) - true, but
says nothing about WHERE the archive itself must physically live to be
discovered and mounted in the first place. That's `filesystem/
searchpath.c`'s `FS_AddGameDirectory()`: `listdirectory( &list, dir,
false )` - the trailing `false` is NOT recursive, so archive discovery
(`.pak`/`.wad`/`.pk3`/`.mpak` alike) only ever scans the TOP LEVEL of a
game directory (`valve/`), exactly where retail `.wad`/`.pak` files
already live. A `.mpak` placed at `valve/maps/crossfire.mpak` (a
reasonable-looking mirror of where the loose `.bsp` it replaces would
sit) is never even seen by this scan - it isn't rejected, isn't logged,
just never enumerated, so `maps/crossfire.bsp` resolves to nothing and
"map crossfire" fails exactly as it would if no file existed. This is
the ONE variable that actually changed between the second (failed) and
third (succeeded) boot attempts, so it is the one real, isolated,
reproducible finding here: moving the file to `valve/crossfire.mpak`
(no `maps/` prefix) made the map load on the very first poll of the
very next boot, after two prior attempts each ran for 90+ real seconds
with zero state change.

**NOT confirmed, and corrected here (codex-review, real finding,
0.99 confidence): a flat `valve/userconfig.cfg` was never proven not
to work.** The first attempt used a flat `userconfig.cfg` with "map
crossfire" in it and failed; the second attempt switched to
`userconfig.d/00-crossfire.cfg` (reasoning that `Host_InitCommon`'s own
direct `Cbuf_AddText( "userconfigd\n" )`, `engine/common/host.c:1377-
1379`, is unconditional, while a flat file only runs if something
execs it) and ALSO failed, identically - proving the userconfig.d
switch itself changed nothing observable, since the real bug (the
`.mpak` location, above) was still present in both. The real disc's
own retail `config.cfg` (copied verbatim from the Steam install, not
authored by this session) contains `exec userconfig.cfg` at line 150 -
written there by `Host_WriteConfig` (`engine/common/con_utils.c:1424`)
during some earlier PC session - so a flat `userconfig.cfg` on this
exact disc would very likely have worked too, indirectly, once the
real bug was fixed. This was never actually tested in isolation.
`userconfig.d` is still the more ROBUST choice for a scripted test
recipe going forward (guaranteed by `Host_InitCommon` itself, not
contingent on whichever `config.cfg` happens to be staged), but "flat
userconfig.cfg doesn't work on this engine" was an overclaim from
correlation, not a tested causal claim - struck from this entry's own
first version, corrected here rather than left standing.

**Generalizes past this one boot**: when two candidate fixes are bundled
into the same next attempt, a success proves at most one of them
matters and says nothing about the other - change one thing per boot,
or be explicit that a claim about an untested variable is a hypothesis,
not a finding. A test setup that "should" produce a state change but
silently doesn't is exactly as diagnosable by reading the consuming
code's own real file-discovery/exec logic (here: `filesystem/
searchpath.c`) as a genuine engine bug is - don't spend more polling
cycles hoping a slow boot will eventually change what a wrong file path
never could - but do isolate which specific change actually mattered
before writing the lesson down.

See `docs/r3d/divergences.md` #98/#99's own resolution notes for the
real live confirmation this recipe finally produced, and the real
screenshots taken.

### NEVER debug a wrong picture as one bug - isolate each term with a shader that outputs ONLY that term, and verify the data at the exact coordinates the GPU reads before blaming the GPU

Every real map drew near-black and monochrome under `ref/nv2a` in every
session since X9-B (2026-09-20) through X9-I (screenshot mean ~1.8/255,
pixel values only {0,1,2}), and every one of those sessions brightness-boosted its
screenshots, called the polygon silhouettes "encouraging", and moved
on to culling or alpha test. `divergences.md` #94 even measured it -
a 9.7% match against a predicted `diffuse × lightmap × 2` - and filed
the result as a limitation of the check. It was two independent bugs,
each of which made the other invisible: the lightmap term was {0,1}/255
for every fragment, and the diffuse term was one flat colour, and the
product of a wrong brightness and a wrong colour looks exactly like
"a dark frame", which gives no hint that either factor is wrong on its
own. No amount of staring at, boosting, or statistically comparing the
product could have separated them.

What worked, in order: (1) a temporary debug pixel shader per term -
lightmap-only produced exactly two values in the whole frame (0 on
254,979 px, 1 on 50,263 px), diffuse-only produced one colour - and
each of those is a texture-ADDRESSING statement, not a lighting one;
(2) reading the data at the exact coordinates the GPU consumes, before
touching a register: the live vertex buffer's `uv1` (`nv2a_map_vb`,
`tools/xemu-debug.py` memory reads) was correct, the baked luxels at
those exact texels were lit (mean 105/255 over 400 vertices), and the
atlas's GPU copy was byte-identical to the `.mpak` - so "the data is
wrong" was off the table and only "the addressing is wrong" remained;
(3) then, and only then, the register-level facts. Those facts, so
they are never re-derived: NV2A addresses LINEAR textures (`LU_IMAGE_*`,
`deps/nxdk/lib/pbkit/nv_regs.h:577`) in unnormalized texel units and
sizes them from `IMAGE_RECT`/`CONTROL1` pitch, while it addresses
compressed textures (`L_DXT1_*`, `nv_regs.h:547`) in 0..1 and sizes
them ONLY from the FORMAT word's `BASE_SIZE_U/V` log2 fields
(`nv_regs.h:581-583`) - xemu `pgraph/texture.c`
`pgraph_get_texture_shape()`, `if (f.linear) width = rect_width; else
width = 1 << log_width, pitch = 0`, and `glsl/psh.c` dividing only
linear stages' coordinates by `textureSize()`; `deps/nxdk/samples/mesh/
verts.h` feeds integer UVs like `{44,143}` to its 256x256 linear texture
for the same reason. (The same sources size swizzled `SZ_*` textures by
`BASE_SIZE` too, but X4's swizzled-path defect is still open, so that
half is not established here.) Both conventions are verified against
xemu 0.8.136's source and corroborated by nxdk's mesh sample,
`nxdk_pgraph_tests`' hardware-captured goldens and Cxbx-Reloaded's
XDK-derived headers, but not measured on a physical Xbox by this
project - xemu is not validation. The lightmap (linear) got 0..1
coordinates; the atlas (DXT1) got no `BASE_SIZE` and was a 1x1 texture.

The tell had been in every screenshot since X4: a triangle textured
with an 8x8 checkerboard rendered as a smooth orange-to-white gradient,
and `tools/x4-triangle/main.c:150-152` and X9-A both recorded that as
"verified rendering the checkerboard correctly". A smooth gradient
where a checkerboard was expected is a two-texel bilinear blend - it is
the symptom of exactly this addressing bug, and reading it as success
put a green checkmark on a baseline that every later slice then copied.
Generalizes: when the picture is wrong, never compare the product to an
expectation - factor it, render each factor alone, and check each
factor's INPUT bytes at the coordinates the hardware actually reads;
and when a test image was designed to have structure (a checkerboard,
a gradient, a grid) and comes back smooth, that is a failed test, not a
passed one, no matter how plausible the colours look. See
`docs/r3d/divergences.md` #100.

### A structural pass (index alignment, byte-exact archives, gap/overlap checks) proves the NEW code is internally consistent - it says nothing about whether the picture is right. Only looking at the render does that

X9-K's own Workflow was genuinely thorough - five specialist reviewers,
one of whom independently re-baked both real test maps and verified the
new index-buffer batching with a from-scratch Python model (0 misaligned
boundaries, 0 gaps, `FaceDrawCmd` sums matching `iCount` exactly) - and
still reported the static gate green. The map still looked visibly wrong
on the very next live boot: `crossfire`'s sky rendered as a dense tiling
pattern instead of flat sky colour, compared directly against `ref_soft`.
The root cause (`ref/nv2a` has never read `MpakFaceInfo::flags` -
`SURF_DRAWSKY` - so sky faces always drew as ordinary geometry) predates
this session entirely; it was invisible as a defect only because the OLD,
buggy atlas+remap design flattened any repeating texture into one
stretched, non-repeating patch, which accidentally looked like "a flat
sky-ish colour" by coincidence. Fixing the tiling bug removed that
accidental camouflage and exposed a real, separate, pre-existing gap.
Chasing that same live comparison further surfaced a second one the same
way: no NV2A texture in this file has ever had a mip chain
(`MIPMAP_LEVELS=1` since before this session), also hidden by the same
flatten-to-one-patch behaviour, now visible as aliasing on any small
texture tiling densely across a distant face.

Generalizes: every kind of structural/statistical verification this
project uses - index-buffer alignment, byte-exact re-bakes, pixel-mean/
spread thresholds, even a Python model built independently from the
implementation - checks that the new code is *consistent*, not that the
*picture* is right. None of them would have caught either defect here;
only a live screenshot next to a reference renderer did, and only because
someone actually looked at it rather than trusting the gate. A correctness
fix can also work exactly as intended and still make the RESULT look worse
than before, when the bug it fixed was accidentally hiding an unrelated,
older problem - that is a real finding to root-cause and document
(`divergences.md` #101), not evidence the fix itself is wrong. See
`docs/r3d/divergences.md` #101.

Second data point, 2026-09-26: the agentic loop's first run (PR #148)
passed two codex-review rounds and a QA pass and still shipped a 2D
batch flushed under the NEXT draw's render mode - additive HUD sprites
drew opaque on black boxes. One xemu screenshot found it. The loop now
has a live stage that boots the build and reads the PNGs against the
spec's own verification section, for exactly this class.

### xemu is not validation for NV2A register values - and "it's an xemu bug" needs the hardware to disagree first

X9-K's textures looked wrong in xemu: smeared ground, misplaced patches,
half a sign stretched across a whole board. An offline audit proved the
baked textures, UVs, face remap, and the GPU's texture offset and format
registers were all correct. A pixel fit then showed the sampler reading the
wrong bytes, and that result was reported as "probably an xemu bug". It was
not. The first real-hardware boot halted instead, on
`NV097_SET_TEXTURE_FILTER = 0x01010000`: real NV2A rejects a zero
convolution-kernel field (bits 13-15), and xemu accepts it. Once that was
fixed, the hardware showed exactly the same corruption as xemu. The cause
was ours: no texture FORMAT word set `BORDER_SOURCE_COLOR`, so every
texture sampled at (u/2, v/2) on both.

What finally settled it was a controlled experiment, not more fitting.
Every diffuse texture was replaced with a coordinate texture (swizzled
A8R8G8B8, R = x, G = y) and the lightmap with flat grey. Constant UVs were
then written straight into the live vertex buffer through the xemu GDB
stub (`M` packet), with one screenshot per value. UV 0.3 read back as 0.15,
1.3 as 0.65, and 5.7 as 0.85, every time - an unambiguous frac(uv/2). The
earlier statistical fit had been muddled by a guessed brightness factor
and by u and the lightmap coordinate moving together within each face;
the controlled pokes had neither problem.

Rules:
- **Verify on hardware before calling anything an emulator bug.** xemu and
  the hardware agreeing is the strong signal; only a disagreement points at
  the emulator. The Xbox here runs PrometheOS FTP (no XBDM), so deploy
  `default.xbe` over FTP, and get real screenshots by having the XBE
  `WriteFile` `pb_back_buffer()` to `D:\` after N frames, then pulling the
  raw BGRX file back over FTP. FTP is unavailable while the game runs.
- **xemu accepts register values real hardware rejects or renders
  differently.** Take register values from a hardware-proven codebase, not
  from nxdk samples: Perfect Dark X's `port/fast3d/gfx_nv2a.cpp` is one.
  The nxdk mesh sample's `0x04074000` is a convolution filter that smears
  on hardware, and its filter words were the root of the halt.
- **When a fit is ambiguous, control the input.** Poking known values into
  live GPU-visible memory and reading back a coordinate texture isolates one
  stage of the pipeline in a handful of screenshots, with no rebuild. A
  runtime flag (`volatile int`, poked through the stub) lets one build serve
  both the probe and normal rendering, which saves a 15-minute rebuild per
  hypothesis.
- **Diff every format word against a known-good one bit by bit.** The
  missing bit was visible in PD-X's `0x0001122a` all along; comparing the
  whole word rather than the fields believed to matter would have found it
  on day one. See `divergences.md` #102.

### NEVER treat a fresh git worktree as a build environment - it has a submodule's sources, never its built output

Both runs of the agentic loop (PRs #148 and #149) created a worktree with
`git worktree add` and `git submodule update --init --recursive`, which
gave it `deps/nxdk`'s sources and none of what `make NXDK_ONLY=y` builds
there: `lib/*.lib`, `tools/cxbe/cxbe`, `extract-xiso`
(`tools/xbox-env.sh:7-9`; all untracked). Run 1's QA never completed the
isolate or the link and reported green anyway; run 2 spent two docs
commits escalating "this environment cannot produce a bootable Xbox
target" while the main checkout, at the same pinned nxdk commit, had
every built file (commit `dd057133`). Nothing failed loudly in either
run: the missing output looked like a broken submodule, then like an
unbuilt SDK, never like "this checkout was never meant to build".

Point a worktree at the main checkout's built SDK (`NXDK_DIR`, which
nxdk's own `activate` keeps when pre-set and every `tools/xbox-*.sh` site
now honours) and **prove it with one compile before anything else runs**.
A tree you did not build in is a tree to test, not to trust.

### NEVER let the agent that ran a check decide whether it passed - compute the verdict from exit codes it must return by name

The agentic loop's first QA stage returned `green: true` on PR #148 with
`tools/xbox-ref-isolate.sh nv2a`, `tools/xbox-link-check.sh` and
`tools/xbox-game-link-check.sh` all unrun ("did not complete in this
worktree"), on code missing two includes that a ten-second isolate would
have rejected (fixed by hand in `4150bd14`). The schema had a boolean the
model filled in and free-text lists of what it ran; nothing tied the
verdict to the checks the change actually required.

The shape that works is the one `tools/xbox-sweep.sh`'s own history
already taught for measuring tools, applied to the judge: the list of
required checks is a function of what changed, a runner records each
check's exit code without interpreting it (`tools/gate-chain.sh`), and
the verdict is computed by matching that table against the list by name.
A model can still misreport an exit code; it can no longer skip a check,
narrow the list, or say "green". Ask of any judging stage what it would
return if the checks had never run - if the answer can be "pass", it is
not a gate.

### NEVER design around "the engine keeps this data in memory" without reading the XASH_LOW_MEMORY branch of its loader

The lazy studio skin design (X9-X step 5) was specified as "the 8-bit
indices and palette stay in the model's own data, which the engine keeps
resident". True of a PC build, false of this one: `tools/xbox-cc.sh`
builds with `XASH_LOW_MEMORY=1`, and `Mod_MaybeTruncateStudioTextureData`
(`engine/common/mod_studio.c:1105-1116`) reallocs every studio model down
to `texturedataindex` right after `Mod_StudioLoadTextures`; a T.mdl's
pixels were never in `cache.data` at all (freed at `:1175`). Code built
on the premise would have read freed or truncated memory on first draw.
Upstream's low-memory modes discard exactly the kind of data a
memory-saving design wants to keep, so the console build's `#if
XASH_LOW_MEMORY` branches are the ones to read first, not the default
path a desktop reading of the loader shows.

### NEVER ship a gate command you have not run once - a test of the plan's shape says nothing about whether its commands work

The v2 loop's `python-tests` gate was `python -m unittest discover -s
tools -p "test_*.py" -t .`, from its first commit until 2026-09-28. With
`-t .` and no `tools/__init__.py`, unittest raises "Start directory is not
importable" every time, so any change to a `tools/*.py` file could never
go green. `tools/test_agentic_loop.mjs` checked that the plan named that
gate for a `.py` change, and passed. The defect surfaced only when
`tools/fast-gates.sh` ran the same command on its first real invocation.
A gate is code: run each command once on a tree where it should pass,
and once where it should fail, before any plan lists it.

### NEVER let a test's stub return the verdict the test is checking - drive the stub from what the code under test sent it

`tools/test_fast_loop.mjs`'s first version mocked the publish stage with
a fixed status. A scenario asserting RED passed while a mutant that
dropped the XBE-hash condition from the live-look verdict computed
GREEN: the mock's own "RED" was being asserted, not the workflow's. With
the mock echoing the `STATUS=` line the workflow wrote into the publish
script, the same mutant fails the test. In any flow test, ask which value
the assertion actually reads, and whether the code under test produced
it.

### NEVER normalise something out of a cache key without removing it from the cached artifact too

`tools/xbox-cc-cache.sh`'s first version rewrote the repository path to a
token in the key only, reasoning that the path "only" reached the object
through `__FILE__`. Against real clang, a second checkout at another path
got a cache HIT and received an object carrying the first checkout's
absolute `__FILE__` strings: a wrong hit, in a cache whose whole claim is
"a hit is the object a fresh compile would give". The fix applies the
same normalisation to the compile (`-fmacro-prefix-map` for every path
form the key rewrites). Anything a key treats as equal must be made
equal in the output, or it must stay in the key. Test a cache with the
two inputs it claims are the same, not only with one input twice.

Second data point, the same day, same script: the fix normalised the
repository path in the WHOLE preprocessed text, while the prefix map it
relied on only covers `__FILE__`. A review reproduced a cross-checkout
wrong hit through a `-D` string carrying the path. The normalisation now
touches only line markers. The rule held; the scope of "the same
normalisation" had been read wider than the mechanism behind it.

Third data point, a runtime cache (x9-perf-loadtime2, divergence #164):
the table of missing directories first stripped trailing separators
from the directory it recorded after a failed lookup, so a lookup of
`E:\custom\\` (an empty component the kernel fails) recorded `E:\custom`,
a directory that existed. The host test's model of the kernel caught it
in the first run. Record exactly the string the kernel answered for; if
a spelling must be refused, refuse it, never rewrite it into another.

### NEVER build a test fixture from what a tool is supposed to output - capture it from the tool, or test the tool's own mode that the caller runs

`tools/test_fast_loop.mjs` mocked a docs-only verify as an empty plan
with `gate_exit=0`, which is what `tools/fast-gates.sh` was documented to
produce. The real tool wrote a one-newline `plan.tsv` (`printf '%s\n'`
with no arguments), which gate-chain rejects with exit 2. Every docs-only
topic would have gone RED after two fix rounds. `fast-gates.sh`'s own
self-test missed it too: it only exercised `plan --files`, never the run
mode the loop calls. Both were found by a stand-in review on PR #155. A
fixture is a claim about another component's output. Take it from a real
run, or give that component a test of the exact mode its caller uses.


### NEVER force-kill xemu before reading back files the game wrote to its hard disk

An A/B run for the GPU studio path (x9-perf-vp-resident) read back 18
screenshots that were all zeros or failed their PNG CRCs, after the game
had written them and quit minutes earlier. The boot script stopped xemu
with `Stop-Process -Force`; qcow2 keeps newly allocated clusters' table
updates in xemu's memory until a flush, so data in clusters allocated
during the run is lost with the process and `tools/xemu_hdd.py` reads
unallocated (zero) clusters. Frame-time recordings had survived the same
kill only because they reused clusters an earlier file had allocated.
Closing the window (`CloseMainWindow`, then waiting for exit) made every
file read back intact. **Close xemu gracefully and wait for it to exit
before reading anything off its disk; a file that reads back once is no
proof the method is sound.**

### NEVER read an emulator's near-zero span as "cheap" before checking the work ran there

The previous phase's xemu baseline put `k_snd` at 0.07 ms against 0.66 ms
on the console, and nobody asked why. On this build xemu never runs the
SDL audio callback (0 calls in a whole recording, x9-perf-sound,
2026-10-02), so the mixer paints its mix-ahead once and then has nothing
to do: every mixer span read about zero in xemu while the console spent
1.5 ms there. **When a span is an order of magnitude smaller in the
emulator than on hardware, first prove the workload runs there (count its
calls or its samples), and only then compare times.**

### NEVER trust a scripted A/B switch without a per-frame counter that proves which side ran

R5 (x9-perf-water): the "CPU path" recording set `r_warp_gpu 0` at the top
of the config, before the first `map`. ref/nv2a registers that cvar on
its first map load, so the line was an unknown command, the cvar came up
at its default 1, and the "before" run measured the GPU path: the two
recordings agreed to the hundredth of a millisecond, which read like "no
gain" until `water_gpu` equalled `water_verts` in both. **Give every
switch a counter that only one side increments, and read it on both
recordings before comparing their times; set a cvar after the code that
registers it has run.** The screenshot A/B of the same change was valid
only because it was checked the same way, frame by frame.

### STOP bisecting once both halves pass and their sum fails - look for the latch that turns one bad moment into a permanent state

fix/xbox-audio-stall (#150): x9-perf-svgame's XBE had no sound for the
whole session on the console, every boot. Bisecting by halves found
build A (the branch minus its pusher commit) with sound, build B (main
plus only that commit) with sound, and A+B silent; a diagnostic build
(A+B plus 50 lines counting audio callbacks) had sound too, because any
added code is just another layout. No diff was to blame. The mechanism
identified (by reading the code and a host simulation; console
confirmation pending) is in nxdk's audio ISR, the `analogDrained` guard
at `deps/nxdk/lib/hal/audio.c:84-91`: one underrun - most likely the
audio thread first running more than one 21 ms buffer after
`XAudioPlay` - becomes a permanent deadlock, and when the scheduler
first runs that thread depends on the code layout. **When a failure is
deterministic for a given binary, total once it starts, and comes and
goes with changes that cannot touch it, stop bisecting and look in the
failing subsystem for the state that latches: a flag, a lost wakeup, a
counter that never recovers.**
