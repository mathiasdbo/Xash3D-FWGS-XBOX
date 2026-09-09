# Xbox bring-up plan

Modelled on [xashPS3](https://github.com/Mayo1970/xash3d-fwgs)'s goal stack,
which is the only ordering for this work validated end to end on real console
hardware — it reached a full campaign at a locked 60 FPS.

Their sequence is not the obvious one, and reading it changed four things
here. Those are marked **[from PS3]**.

Each phase is a branch, with micro-commits, specialist review and a merged PR
before advancing (see `/AGENTS.md`). Divergences go into `divergences.md` the
same day.

## What their ordering teaches

**Get logs off the box before anything else.** Their goal 1 was toolchain
bring-up *and* UDP debug logging, together. A console has no stdout to read.
Without a log channel every later failure is silent, and silent failure on
unfamiliar hardware is the expensive kind.

**Prove video outside the engine.** Their goal 4 built RSX video
infrastructure as a **standalone tool**. A triangle on screen from a 200-line
program separates "does the GPU work" from "does the engine work". Inside the
engine you debug both at once.

**Wire the software renderer to the display before writing the hardware one.**
Their goal 5 put `ref_soft` on screen; the RSX `ref_gl` did not arrive until
goal 11, six goals later. This is the item this plan had wrong. `ref/soft` is
17,422 lines that already exist and are already correct — putting it on the
framebuffer validates the filesystem, BSP loading, PVS, entity handling and
the frame loop **before** a single pushbuffer command is written. The hardware
backend then replaces a known-good picture instead of producing the first one.

**Stub the game module first.** Their goal 6 ran the main loop against a stub
client, decoupling engine bring-up from SDK bring-up. Two hard problems, one
at a time.

## Where Xbox differs from PS3

**Easier:**

- **Endianness is free.** x86 little-endian, same as the host. Their goal 13
  was a whole endianness audit across networking, demos, saves and HPK. We
  skip it.
- **Same architecture as the host**, so much numerical behaviour matches the
  PC build by construction.

**Harder:**

- **The linker.** Their static linking worked because PPC/ELF suits
  `scripts/waifulib/xshlib.py`. Ours does not — though **not for the reason
  this document gave until 2026-09-05**, see `x0-probe-results.md` §4. The
  `-melf_i386` in `xshlib.py:49` is appended only `if '-m32' in LINKFLAGS`,
  and nxdk never passes `-m32`, so `ld -r` would run with *no emulation flag
  at all*, which is a subtler failure than an ELF/COFF mismatch. What
  genuinely breaks: `objcopy` does not exist in nxdk, `llvm-objcopy` rejects
  `-G` on COFF, and `lld -flavor link -r` cannot do partial links. Their goal
  2 was solved; ours is open with three known fallbacks.
- **The GPU.** They wired `ref_gl` to the RSX, a G70 derivative with real
  shaders. The NV2A is fixed-function T&L plus VP20 and register combiners.
  There is no `ref_gl` path for us — X9 is a new backend, not an adaptation.
- **Memory.** The PS3 has 256 MB XDR plus 256 MB GDDR3. The Xbox has **64 MB
  total, shared with the GPU**. They still hit memory pressure hard enough to
  cap listen servers at four players. We have a quarter of their budget.

---

## What nxdk provides, and what it does not

Checked in `deps/nxdk`, because the plan should not assume anything that is
not there.

**There is no OpenGL.** None, at any version, in any wrapper. `pbkit` is raw
NV2A register access — `lib/pbkit/nv_regs.h` and `nv_objects.h` are register
and object definitions, not a graphics API. Upstream's `3rdparty/gl4es`
translates GL to GLES and there is no GLES here either. `ref/gl` has no path
to this hardware, and `ref/nv2a` is written against the hardware directly.

**But nxdk vendors SDL2**, as a first-class submodule alongside `SDL2_image`
and `SDL_ttf`, with working samples for video, `sdl_gamecontroller`, images
and TrueType text. It also provides `hal` (video, audio, Xbox APIs), `usb`,
`net` (lwIP, with an httpd sample), `xaudio`, `libjpeg`, `libpng` and `zlib`.

That matters because **the engine already has a complete SDL2 platform
backend**: `engine/platform/sdl2/` is 2,980 lines across `host_sdl2.c`,
`vid_sdl2.c`, `in_sdl2.c`, `joy_sdl2.c`, `s_sdl2.c`, `sensor_sdl2.c` and
`sys_sdl2.c`. If nxdk's SDL2 port is functional, most of `engine/platform/
xbox/` does not need writing — video surface, events, controller and **audio**
come from code that already exists and is already maintained.

The honest caveats, none of them settled:

- **SDL2 video on Xbox is a software framebuffer, not a GL context**, and X5
  does not use it either — see X5. Coexistence with pbkit is **not** an open
  question: the application owns the mode, arbitration is `PCRTC_START` via
  `pb_show_front_screen()` / `pb_show_debug_screen()`, and it ships as an
  upstream sample. Initialise SDL without `SDL_INIT_VIDEO` and call
  `pb_show_front_screen()` after every `pb_init()`.
- **SDL2 costs memory and binary size** on a 64 MB budget. Measure before
  committing to it as permanent.
- **Its completeness here is unverified.** The submodules are not initialised.
  Verifying it is an early, cheap task with a large payoff either way.

If SDL2 audio works, **X8 may largely disappear** — `s_sdl2.c` already exists,
and the audio abstraction described there becomes a nice-to-have for upstream
rather than a prerequisite for us.

---

## X0 — Toolchain and a log channel

**[from PS3]** The log channel ships with the toolchain, not after it.

- nxdk in the waf build: root `wscript`, toolchain
  `clang -target i386-pc-win32 -march=pentium3 -fuse-ld=nxdk-link`.
- **Answered per subsystem** — `x0-probe-results.md` §1. What remains is an
  enumerated list, not an investigation:
  - Clone `3rdparty/library_suffix` and add an Xbox case to `build.h`.
    **Until this exists nothing in the tree compiles at all**:
    `common/defaults.h:20` includes it and every `XASH_*` macro comes from it.
    It is a third-party submodule, so plan for a fork.
  - ~~Plumb `-DXBOX` into every engine TU; add a `sys/types.h` shim; patch
    the five hint constants.~~ **Done.** `tools/xbox-cc.sh` passes
    `-D__XBOX__=1` (the exact macro `SDL_config.h:32,46` checks);
    `compat/xbox/sys/types.h` is the shim; all five hints are defined,
    guarded, in `compat/xbox/xbox_prelude.h`.
  - ~~Drop `SDL_INIT_VIDEO` from `sys_sdl2.c:130`.~~ **Done, 2026-09-08.**
    `#if XASH_XBOX` now omits it from the `SDL_Init` flags; every other
    `XASH_WIN32` target is unaffected.
  - **`VIDEO_XBOX`/`INPUT_XBOX` in `common/backends.h`/`common/defaults.h` —
    corrected 2026-09-08, found in review: this was never X0 scope, though
    the reason is narrower than a first pass at this correction claimed.**
    A dedicated `VIDEO_XBOX` backend (`vid_xbox.c` against nxdk's HAL
    framebuffer directly) is real rendering work with a real
    per-frame-cost question attached (§3's LUT-blit budget) — it belongs to
    X5, where that work is already scheduled, not to a
    toolchain-and-log-channel phase. **Correction to the correction, found
    by Codex's own review of the branch that first wrote this note: Xbox
    is not currently kept off `vid_sdl2.c`.** `defaults.h`'s generic
    `#if XASH_SDL` arm still selects `VIDEO_SDL` for Xbox today, `vid_sdl2.c`
    genuinely compiles, and nxdk ships a real (if minimal) `SDL_xbvideo.c`
    driver a normal client boot would reach through it — see divergence
    #14 for the full trace, including the 0×0 window/display-mode risk
    that path has today because nothing has called `XVideoSetMode()` yet.
    Not currently reachable only because no Xbox `main()`/link/running
    binary exists in this tree at all - a fact about X1/X2 not having
    happened yet, not a property of the video architecture. `INPUT_SDL`
    needs no Xbox-specific branch at all: it comes through the same generic
    `#if XASH_SDL` arm, and §1's own bottom line only asks to compile
    `in_sdl2.c` "as inert" - not to replace it, and input has no equivalent
    of the 0×0-mode risk. X0's own exit criterion (boots, prints, exits)
    still needs none of this *itself* - X0 has never called `CL_Init`. What
    it does need, before X1/X2 wires up a real entry point: a decision
    about whether that entry point calls `XVideoSetMode()` with a sane
    default before reaching `Host_Main`, or forces Xbox into
    `Host_IsDedicated()`-equivalent behaviour until `VIDEO_XBOX` exists at
    X5. Left undecided here on purpose - deciding it now would guess at
    X1/X2's own design; `vid_xbox.c` itself still belongs on X5's list.
  - Pick a `DEST_OS` that is not `win32`. **Still open** — measured, not
    merely planned: `x0-probe-results.md` §7 shows `DEST_OS = xbox` plus a
    one-token widening of `engine/wscript:181`'s denylist is the cheaper
    fork (three broken Win32 files avoided against two POSIX ones, which
    would need `#ifdef`-ing individually rather than excluding wholesale).
    What that measurement does not cover is wiring `DEST_OS` into waf's own
    `configure()` in the first place — `nswitch`/`psvita` each get there
    through a dedicated `conf.load(<platform>)` toolchain module
    (`scripts/waifulib/`) handling cross-compilation specifics, and nxdk's
    toolchain has no such module yet. Writing one is real, separate,
    open-ended engineering, not a one-line fork like the source-selection
    question it unblocks — worth its own phase-sized branch, not a
    ride-along on this checklist. Every measurement so far (X0's own
    toolchain, X1's static-linking probe) has gone through `tools/xbox-cc.sh`
    and manual `nxdk-link` invocations instead, bypassing waf entirely, so
    nothing currently blocks on this being unresolved.
  - `public/miniz.c` does not compile for nxdk — `<sys/stat.h>`, `errno_t`,
    `localtime_s`, and `_MSC_VER` steering it into the Win32 branch.
  - ~~**Build nxdk on the dev host.**~~ **Done 2026-09-05.** 19 libraries,
    7.1 MB, including libSDL2 (1.5 MB), libpbkit, libpdclib, libnxdk_hal,
    libSDL_ttf and libc++. Recipe in `tools/xbox-env.sh`; single-TU
    compiles go through `tools/xbox-cc.sh`.
  - ~~**Prove the toolchain reaches an XBE.**~~ **Done.** nxdk's `hello`
    sample builds end to end — `nxdk-cc` → `nxdk-link` → `cxbe` →
    `extract-xiso` — producing a 102,400-byte `default.xbe` and an ISO.
- **A working log channel before anything else** — nxdk's debug channel, then
  network logging, so failures on hardware are readable.
- **A ruler in the same channel.** Per-subsystem `rdtsc` counters and a
  frame-time line, here rather than in X11. Numbers are logs. Every claim in
  `optimization-transfer.md` is a hypothesis until a phase that draws prints
  one, and until 2026-09-06 this plan had the instrument scheduled last.
- XBE packaging with `cxbe`.


### Networking is parked, deliberately

Six files — `net_ws.c`, `net_chan.c`, `xrcon.c`, `cl_steam.c`, the two
`http/` files — do not compile, and are being left that way.

Both routes were priced:

**`XASH_NO_NETWORK=1`** selects `engine/platform/stub/net_stub.h`
(`net_ws_private.h:17`), which is upstream's own no-network mode and the
direction X3 takes anyway. It is not self-contained: the stub declares the
socket structures opaquely, while `net_ws.c` uses `sockaddr_in6`,
`sockaddr_storage` and `in6_addr` **by value**. On Linux the real headers
are present regardless of the flag, so upstream never hits this. On a
platform with no BSD headers at all, it does not build.

**Routing through nxdk's lwIP would give the real types, and the port layer
already exists — corrected 2026-09-07, this section previously said
otherwise and was wrong.** nxdk ships both `lwipopts.h` and `arch/cc.h`
(`deps/nxdk/lib/net/nforceif/include/`), `LWIP_SOCKET` and `LWIP_UDP` are on,
and `libnxdk_net.lib` is already built with them — `deps/nxdk/samples/
httpd_bsd` proves BSD sockets work here, and X0's log channel now does too
(`engine/platform/xbox/net_xbox_log.c`, divergence #13). There is no hundred
lines to write.

What is still genuinely unknown, and is a smaller question than "does nxdk
have a port layer": whether `net_ws.c`'s specific field-by-field use of
`sockaddr_in6`/`sockaddr_storage`/`in6_addr` compiles unmodified against
`<lwip/sockets.h>`'s versions of those types, or needs a translation shim.
That is the actual remaining unknown, not the port layer, and it is a
question about one file, answerable by trying to compile it — not yet done.

So: parked, not forgotten. Two things worth doing later, in this order:

1. **Complete upstream's stub** so `XASH_NO_NETWORK` is self-contained.
   That is a small, generic patch that helps any platform without BSD
   headers, and it is the kind of thing FWGS would plausibly take. It also
   makes X3's trim trivial instead of surgical.
2. Only if the console build ever wants real networking, write the lwIP
   port layer.

**Exit:** an XBE that boots in xemu, prints, and exits cleanly.

## X0.5 — Root directory detection

**A blocker that was not in this plan.** `SDL_GetBasePath` returns NULL on
nxdk and `getcwd` does not exist, so `filesystem_engine.c` cannot determine
the root directory and the engine `Sys_Error`s before X2 begins. A small
patch, but a hard gate, and it lived nowhere until now.
See `x0-probe-results.md` §1.5.

### The exit above is NOT sufficient. Found in review, 2026-09-06.

Finding the root directory is half of it. `FS_InitStdio` then mounts
everything through **relative** paths, and nxdk has no current directory to
resolve them against:

- `FS_SetCurrentDirectory` is one of the ten `!XASH_XBOX` widenings in
  divergence #6, so Xbox falls into `filesystem/sys.c:511`, which prints
  "not implemented, ignoring..." and **returns true**. `compat/xbox`'s
  `chdir()` — which fails honestly with `ENOSYS` — is never reached. The one
  place in the ledger where "compat/xbox supplies what that branch needs" is
  false.
- `FS_InitStdio` then uses the literal `"./"`: `FS_ValidateDirectories` at
  `searchpath.c:623`, `FS_AddGameDirectory` at `:649`, `listdirectory` at
  `:677`.
- `FS_AddGameHierarchy` builds every real searchpath relatively too —
  `"valve/"` (`:378`), `_downloaded` (`:374`), `_hd` (`:386`), `_addon`
  (`:392`), `_lv` (`:398`), the language dir (`:404`), `custom` (`:410`).
  `fs_rootdir` is consumed at `dll.c:78,83`, `gameinfo.c:487` and
  `searchpath.c:507,851,952`, and **never to prefix a searchpath**.

Every nxdk path API resolves against `ObDosDevicesDirectory()` with no CWD in
the stack: `CreateFileA` (`fileio.c:32`), `GetFileAttributesA`
(`filemanip.c:26`), `FindFirstFileA` (`findfile.c:74`). So `opendir("./")`
builds `".\*"`, nxdk splits it into dirPath `".\"` + mask `"*"`
(`findfile.c:51-63`) and `NtOpenFile`s `"."` as a DOS device name, which it is
not. `STATUS_OBJECT_NAME_NOT_FOUND`.

The consequence is worse than "no pak0.pak": `FS_ParseGameInfo` never reads
`valve/gameinfo.txt`, `FI.numgames` stays 0, and the engine has no game to
run. Handing it an absolute `rootdir` does not fix it, because the searchpaths
built from that point on are relative regardless.

**Two ways out, and the choice belongs to this phase:**

1. **Resolve a process-wide CWD inside `r3d_native_path`.** One place, no
   upstream edit, and `compat/xbox/xbox_posix_io.c:615-617` currently argues
   against it ("fail rather than pretending to succeed"). That argument was
   written about `chdir` as an API, not about path resolution, and it is worth
   revisiting: an emulated CWD is a lie only if something can observe the
   difference, and on a platform with exactly one process nothing can.
2. **Make `filesystem/` prefix searchpaths with `fs_rootdir`.** Correct
   everywhere, larger diff, and it touches a file upstream changes often.

Route 1 is smaller and reversible; route 2 is the one that could go upstream.
Neither is written yet.

**Exit:** the engine finds its game directory on Xbox **and mounts it** —
`FI.numgames > 0` and `pak0.pak` opens.

## X1 — Static linking proven

### MEASURED, 2026-09-06: ten duplicate symbols

The measurement this phase existed to demand has been run. 157 objects —
every non-networking, non-test translation unit in `engine/`,
`filesystem/`, `public/`, `common/` and `compat/` — linked with the exact
`nxdk-link` flags plus `-errorlimit:0`, **no `ld -r`, no objcopy**:

```
_FI              _FS_LoadFile       __Mem_Alloc
_FS_Close        _FS_LoadGameInfo   __Mem_Free
_FS_FindLibrary  _FS_Open
_FS_LoadDirectFile  _FS_Search
```

**Ten.** All of them one boundary: the filesystem module defines the FS API,
and `engine/common/filesystem_engine.c` re-exports the same names as thin
wrappers. `_FI` is the known struct-versus-pointer collision
(`filesystem/filesystem.c:30` against `filesystem_engine.c:41`). **Corrected
2026-09-08, found while fixing this: the `_Mem_*` pair is not "`zone.c`
against the filesystem's own allocator" as if filesystem had an independent
one — `filesystem/filesystem.c`'s `_Mem_Alloc`/`_Mem_Free` are thin
forwarders to `g_engfuncs._Mem_Alloc`/`._Mem_Free`, the engine's own real
allocator handed over via `fs_interface_t` at init. The collision is in
*name* only, not in behaviour — but the name collision is exactly as real,
and needed the identical fix.**

A first run reported 21. Eleven of those were `_main`, from
`filesystem/tests/` and `public/tests/` — unit-test programs that have no
business in an engine link at all. Worth recording because it is the sort of
number that gets quoted: the real figure is ten, and the difference was a
glob, not a problem.

**This is design-stage option 1 + 4, the cheapest branch.** Ten names is
`llvm-objcopy --redefine-sym` territory — the one symbol option that works
on COFF — or ten `#define`s. `xshlib.py` is bypassed entirely on Xbox: no
`ld -r`, no `objcopy -G`, no GNU binutils in the toolchain, none of the
COMDAT-folding hazard the earlier probe found. The largest risk this project
recorded is retired, and it was retired by measuring rather than by
building the machinery to survive it.

### FIXED, 2026-09-08: the ten duplicates, verified by relinking

Went with the `#define` branch, not `objcopy` — the toolchain reality
above still holds (`objcopy` does not exist anywhere in nxdk, per "What
stands" below). Eight of the ten (`FI`, all six `FS_*` names) are a blanket
`#if XASH_XBOX` `#define` block in `filesystem/filesystem_internal.h`,
scoped so it can only ever affect `filesystem/`'s own translation units
(confirmed by grep: no `engine/` or `ref/` file includes
`filesystem_internal.h`). `_Mem_Alloc`/`_Mem_Free` needed a hand rename
instead of a blanket one: `fs_interface_t` (`fscallback.h`) has struct
*member* fields also literally named `_Mem_Alloc`/`_Mem_Free` — the
engine's own allocator, handed to filesystem at init — and a blind
`#define` rewrote those too, caught immediately by trying to compile
`dll.c`/`filesystem.c` against that version rather than assumed safe.
Divergence #15 has the full account.

**Verified by relinking, not just recompiling.** `deps/nxdk/bin/nxdk-link
-errorlimit:0` over every object file a real Xbox build would actually
select (excluding `filesystem/tests`, `public/tests`, and every
`engine/platform/*` subdirectory other than `sdl2`/`xbox`/`misc` — files
`tools/xbox-sweep.sh` compiles individually regardless of which platform
would really select them, the exact "N of M compile has never meant N of M
would link" gap that script's own header names) reported these same ten
duplicates before the fix, byte-for-byte matching this measurement, and
**zero** duplicate-symbol errors after — confirmed the same session, not
assumed from the fix reading correctly. The first re-link attempt (before
narrowing the object set to what a real build selects) found nine
*additional* apparent duplicates, `SNDDMA_*`/`VoiceCapture_*` between
`platform/sdl1/s_sdl1.c` and `platform/sdl2/s_sdl2.c` — a measurement
artifact of linking both SDL1 and SDL2 sound backends together, which a
real Xbox build never does (only `XASH_SDL=2` is selected), not a real
collision.

### MEASURED, 2026-09-08: the undefined-symbol picture, with the real library list

Linking against every base nxdk library `main.exe`'s own `Makefile`
dependency chain accumulates for an `NXDK_SDL=y` build (`libxboxkrnl`,
`libpdclib`, `libwinapi`, `libnxdk_hal`, `libnxdk`, `libnxdk_automount_d`,
`libpbkit`, `nxdk_usb`, `libxboxrt`, `libzlib`, `libpng`, `libjpeg`,
`libnxdk_net`, `libSDL2`, `libSDL2_image`, `libSDL_ttf`, `libfreetype`,
`winmm` — all 18, already built on this dev host, per `lib/Makefile`'s own
chain of `main.exe: $(NXDK_DIR)/lib/libX.lib` rules) dropped the count from
335 to 107, then to 102 after three small fixes (below). Zero duplicate-
symbol errors throughout — the fix above holds under the real library set,
not just the bare-object measurement.

Three real, small fixes, none speculative:

- **`Q_buildos`/`Q_buildarch`** were never actually missing - their real
  implementation (`3rdparty/library_suffix/src/library_suffix.c`) compiles
  clean for Xbox and was simply never in this project's own sweep/link
  scope. **Corrected 2026-09-08, found in review: a first pass at this
  fix only included the file in the manual measurement, not in the
  repository's own documented sweep command - a future checkout following
  it verbatim would still reproduce the identical gap.** Fixed for real by
  extending the canonical command: `tools/xbox-sweep.sh engine filesystem
  public common compat 3rdparty/library_suffix/src`, now 187 of 221 (was
  186 of 220), same 34 failing.
- **`Q_stricmp`/`Q_strnicmp`** were a genuine, small upstream gap:
  `crtlib.h`'s own `#if HAVE_STRICMP || HAVE_STRCASECMP` already
  anticipates a platform with neither, but nothing anywhere in this tree
  ever defined the `#else` branch's fallback. Not Xbox-specific — any
  platform bypassing waf's configure-time check (as `tools/xbox-cc.sh`
  does) or genuinely lacking both hits the identical gap. Fixed with a
  portable, generic implementation in `public/crtlib.c`, worth sending
  upstream per this project's own "send upstream anything generically
  useful" rule.
- **`COM_CheckLibraryDirectDependency`** is an eleventh landmine of
  divergence #7c's class: its call site (`cl_game.c`) is guarded by
  `XASH_WIN32 && !XASH_64BIT`, reachable on Xbox as written, but its only
  definition (`engine/platform/win32/lib_win.c`) parses a real PE import
  table — Win32 loader machinery nxdk has no equivalent of. Xbox has no
  dynamically-loaded `client.dll` to inspect in the first place (static
  single-image build), so no replacement is needed, same as the other ten;
  widened to `&& !XASH_XBOX`.

**What remains, fully accounted for — 102 of 102, not "the rest":**

- **63: the already-parked networking group** (`NET_*`, `Netchan_*`,
  `HTTP_*`, `XRcon_*`, `SteamBroker_*`, and `net_*`'s internal statics) -
  exactly the six files (`net_ws.c`, `net_chan.c`, `xrcon.c`, `cl_steam.c`,
  the two `http/` files) this section already documents as parked,
  deliberately. Expected; not a new finding.
- **1: `_main`** - no Xbox entry point exists in this tree yet. Expected;
  X1/X2's own job.
- **38, a genuinely new finding: `3rdparty/opus`, `3rdparty/opusfile`,
  `3rdparty/vorbis` and `3rdparty/MultiEmulator` have never been compiled
  or measured for Xbox at all.** 37 symbols (`op_*`/`opus_*`/`ov_*`/
  `vorbis_comment_query`) are audio-codec calls from `engine/client/sound/
  voice.c` (which compiles clean) and `engine/client/soundlib/
  snd_ogg_vorbis.c` (also compiles clean) into these vendored-but-never-
  built third-party libraries; 1 (`GenerateRevEmu2013`, called from
  `cl_main.c`) is `3rdparty/MultiEmulator/src/RevEmu2013.c`, likewise never
  built. Unlike the `library_suffix`/`Q_buildos` case, this is not just a
  sweep-scope gap - nobody has yet checked whether `3rdparty/opus`,
  `opusfile`, `vorbis` or `MultiEmulator` even compile under nxdk's
  toolchain. Real, separate, unscoped work: not attempted here, not folded
  into this measurement's "known and accepted" bucket the way the parked
  networking group is.

**The gate is closed for the duplicate-symbol question. It is not closed
for "a real link succeeds"**: 63 + 1 symbols are accepted, deliberate gaps
with an owning decision already on record; 38 were a real, newly-measured
gap with no owning decision yet, as of this paragraph.

### MEASURED, 2026-09-08: the answer for opus/opusfile/vorbis/MultiEmulator

**Overwhelmingly yes, with three small, well-understood gaps.** Each
submodule's own `wscript` names its real source list (not "everything
under the directory" - each excludes demo/test/CLI-tool files that are
not part of the library); compiled every one of them individually with
`tools/xbox-cc.sh` plus the extra `-I`/`-D` flags that `wscript` declares.
**176 of 179 real library source files compile clean for Xbox (173 portable/generic + 6 x86 SIMD wscript adds unconditionally for any x86 DEST_CPU):**

- **`3rdparty/MultiEmulator`: 10 of 10.** Including `RevEmu2013.c` -
  `GenerateRevEmu2013` is real and compiles as-is, no fix needed.
- **`3rdparty/libogg`: 2 of 2.** `config_types.h` is a `waf 'subst'`
  template (`INCLUDE_INTTYPES_H`/`STDINT_H`/`SYS_TYPES_H`, checked in that
  order against what the host libc has) - nxdk's pdclib has `inttypes.h`,
  so the first branch wins; generated by hand for this measurement, not
  committed (the file is gitignored inside the `libogg` submodule, same as
  a real build would produce it).
- **`3rdparty/vorbis`: 21 of 21** (`libvorbis`'s 20 files plus
  `vorbisfile.c`), against `libogg`'s headers, with no extra defines
  needed at all - not even `USE_MEMORY_H`, which the `wscript` treats as
  optional.
- **`3rdparty/opus`: 140 of 142.** All three of `opus/src`, `opus/celt`,
  `opus/silk`, `opus/silk/float` (`wscript`'s own source glob, demo files
  excluded) plus the six x86 SIMD files `wscript`'s own `DEST_CPU.startswith
  ('x86')` branch adds unconditionally. **Corrected 2026-09-08, found in
  review: an earlier version of this measurement excluded those six by
  reasoning alone (`-march=pentium3` is Pentium III, no SSE2) without
  actually testing them - a real gap between what the prose claimed to
  measure ("`wscript`'s own source glob") and what it tested.** Tested
  properly: five of the six compile clean, including two files named
  `*_sse2.c`; `vq_sse2.c` fails to compile at all, with a real, loud
  clang error (`always_inline function '_mm_set_epi32' requires target
  feature 'sse2', but would be inlined into function ... compiled without
  support for 'sse2'`) - confirming the Pentium III/no-SSE2 reasoning was
  right, but as a compile-time fact for this one file, not a blanket
  exclusion for "anything with sse2 in the name." Whether the five that
  DO compile are safe to actually call on real Xbox hardware is a
  separate, unanswered question - a clean compile is not evidence a
  function tagged for SSE2 target support won't fault if genuinely
  invoked without a runtime feature check, and none of the four fully-
  tested libraries have been run on real hardware or in xemu (same
  caveat this branch's other UNVERIFIED notes already carry). One trap
  along the way, in the portable (non-SIMD) files: all 37 failures on the
  first pass were the identical error - `opus` refuses to compile without
  `VAR_ARRAYS`, `USE_ALLOCA`, or `NONTHREADSAFE_PSEUDOSTACK` defined, and
  `wscript`'s own `configure()` picks this based on `alloca.h`'s
  availability, which nxdk's pdclib does not have. `VAR_ARRAYS` (C99
  variable-length arrays, no libc dependency) fixed all 37 at once. The
  one remaining portable-file failure, `celt/celt.c`: `celt/arch.h:77`'s
  `celt_fatal()` calls `_set_abort_behavior`/`_WRITE_ABORT_MSG` under
  `#if defined(_MSC_VER)` - genuinely true for nxdk's clang target
  (`_MSC_VER 1950`, `x0-probe-results.md`'s own §1 measurement), but
  neither symbol exists in nxdk's pdclib; real MSVC uses them only to
  suppress a debug abort dialog Xbox has no equivalent of. Not fixed here:
  the right form (a `compat/xbox` shim, matching `xbox_win32_ext.c`'s
  established pattern, versus a direct one-line patch to this vendored
  header, matching `library_suffix`'s own precedent) is a real decision,
  not a default - fixed for real when the patch mechanism is decided.
- **`3rdparty/opusfile`: 3 of 4.** `info.c`, `internal.c`, `opusfile.c`
  compile clean. `stream.c`'s `#if defined(_WIN32)` branch (again
  genuinely true - the raw macro, confirmed predefined for nxdk's target
  since X0) calls `_filelengthi64`/`_fileno`/`_get_osfhandle`/
  `GetFileType`/`_wfopen`/`fdopen`, none of which nxdk's pdclib has. The
  `#else` branch is not a free fallback either: it uses `fseeko`/`ftello`,
  which nxdk's pdclib also does not have (grepped the whole `deps/nxdk`
  tree for both names; the only hits are other vendored libraries'
  unrelated build configs, not a real declaration). Neither branch this
  file already has is usable as-is - the real fix is narrower than either,
  and narrower than an earlier version of this note claimed too:
  **corrected 2026-09-08, found in review - "nxdk has no file-length
  query at all" is false. `deps/nxdk/lib/winapi/fileapi.h:38-39` declares
  `GetFileSize`/`GetFileSizeEx`, and `GetFileSizeEx` is a real,
  implemented function (`fileio.c:398-408`, via
  `FILE_NETWORK_OPEN_INFORMATION`), not a stub.** What nxdk actually lacks
  is narrower: `_fileno`/`_get_osfhandle`, the bridge from a stdio
  `FILE *` to the `HANDLE` `GetFileSizeEx` needs (confirmed absent from
  every nxdk header, same grep as the `fseeko`/`ftello` check above).
  `op_fseek`'s `_WIN32` branch already uses `fgetpos`/`fsetpos` (both
  genuinely available) for `SEEK_CUR` and `SEEK_SET`; only `SEEK_END`
  needs that bridge - and `compat/xbox/`'s own `r3d_fds[]` (`net_xbox_log.c`'s
  own comments already name it as this project's existing file-descriptor
  tracking) is a plausible place to build it, not an unimplementable gap.
  Not fixed here - same reasoning as `celt.c` above, the
  vendored-patch-versus-compat-shim question deserves its own decision.

**Answers the question this section asked.** `3rdparty/opus`/`opusfile`/
`vorbis`/`MultiEmulator` are viable for Xbox - 176 of 179 real files
(including the six x86 SIMD files a real build would select, tested
directly rather than reasoned about) already compile, no architectural
blocker found anywhere. What remains is three small, well-isolated gaps
(`celt.c`'s abort-dialog suppression, `stream.c`'s missing `FILE*`-to-`HANDLE` bridge,
`vq_sse2.c`'s genuine, loud compile failure under `-march=pentium3` — a
real reason to exclude that one file specifically, not the whole x86
SIMD set), not an open question about
whether these libraries can work here at all.

### FIXED, 2026-09-08: `celt.c`'s abort-dialog suppression, and a real link with the third-party codecs included

The `celt.c` gap named above is fixed: `_set_abort_behavior`/
`_WRITE_ABORT_MSG` are now declared in `compat/xbox/xbox_prelude.h` and
defined (no-op, matching real MSVC's own no-dialog-here behaviour on this
platform) in `compat/xbox/xbox_win32_ext.c` — divergence #17. `celt.c`
compiles clean; **opus is now 141 of 142**, only `vq_sse2.c` excluded (the
genuine, loud `-march=pentium3`/no-SSE2 compile failure named above, not a
judgment call). The main engine sweep baseline is unaffected by this
tree-wide `-include` addition (`187 ok, 34 failing`, unchanged).

With that fix in place, all 174 fully-working third-party objects (141
opus + 21 vorbis + 2 libogg + 10 MultiEmulator — `opusfile` still excluded,
`stream.c`'s gap above is unfixed) were compiled and linked in alongside
the main engine set and the full 18-library nxdk set, the same way the
duplicate-symbol and undefined-symbol measurements above were done, but
now with real third-party objects present instead of the engine linked
alone: **0 duplicate symbols, 74 undefined** — down from the 102 measured
above, because opus/vorbis/libogg/MultiEmulator supply their own real
definitions where the engine build previously only had symbol
*references*. The 74 are fully accounted for: 63 parked networking symbols
(unchanged from the 102 above), 1 `_main` (no Xbox entry point exists in
this tree yet, see divergence #14's own note on this), and 10
opusfile-specific symbols (`op_*`, `opus_tags_query`) pending `stream.c`'s
own separate fix.

Built as a real, reusable tool rather than another one-off measurement —
`tools/xbox-link-check.sh` — after recognizing this was the exact
"not repository-backed" mistake this project's own review process had
already caught once earlier in the same phase (the duplicate-symbol
sweep-command gap, see the FIXED section above). It composes with
`tools/xbox-sweep.sh` rather than duplicating it (reuses whatever objects
that sweep last left in `$SCRATCH/*.obj`, does not recompile the engine),
compiles the third-party codec objects fresh each run against each
submodule's own documented `wscript` flags, regenerates libogg's
`config_types.h` from an inline template (previously hand-generated per
measurement, now automatic), links with the real 18-library set, and
takes `--expect-undefined N` to gate on regression/improvement the same
way `tools/xbox-sweep.sh --expect-fail` already does. Any duplicate
symbol at all is always a hard failure — there is no accepted-duplicate
case, unlike undefined symbols. Verified against `--expect-undefined 74`:
`0 duplicate, 74 undefined`, matching the hand-checked figures above
exactly; also verified its three failure paths directly (no prior sweep →
fails loudly with the documented instruction; `--expect-undefined` too
low → REGRESSION; too high → IMPROVEMENT), each exiting 1 as designed.

Static linking is **mandatory**, not preferred: nxdk's `LoadLibraryExA`
unconditionally returns NULL (`deps/nxdk/lib/winapi/libloaderapi.c:16-23`) and
`SDL_LOADSO_DISABLED 1`. There is no dynamic path for anything.

~~**Measure before choosing.** Compile `engine/`, `filesystem/` and
`ref/soft/` with the real `nxdk-cc` line, archive `public` with `llvm-lib`,
link with the exact `nxdk-link` flags plus `-errorlimit:0`, **no `ld -r`, no
objcopy**, and count the genuine duplicates. Nobody has done this; every
figure so far came from synthetic modules or from linking one object
twice.~~ **Done, 2026-09-06 through 2026-09-08 — see "MEASURED" and
"FIXED"/"MEASURED" above, not repeated here.** The real count was "a
handful" (ten), not "a large structural list": `#define`-based renaming was
the actual fix (not `llvm-objcopy --redefine-sym`, which was the other
handful-sized fallback considered here and not needed once `#define` proved
sufficient), verified by relinking with zero duplicate-symbol errors under
the real object set. The large-structural-list fallback below (`ld -r`
twice, `objcopy --set-section-flags`, `xshlib.py`'s missing `_` prefix) was
never needed and stays here only as a historical record of the path not
taken — `xshlib.py` is bypassed on Xbox entirely regardless, per "What
stands" below.

- ~~A handful of real conflicts → `llvm-objcopy --redefine-sym`, the one
  symbol option that works on COFF~~ **Not the path taken — see above.**
  `xshlib.py` is bypassed on Xbox entirely.
- A large structural list → GNU `ld -r -m i386pe` run **twice** (one pass
  folds duplicate COMDATs but leaves a dangling undefined that binds silently
  to the engine at final link, exit 0, no diagnostic), then `objcopy` with
  `.data` included in `--set-section-flags`, then the missing `_` prefix at
  `xshlib.py:36`. **Moot — the real count was ten, not a large structural
  list.**

~~One genuine conflict is already known: `fs_globals_t FI;`
(`filesystem/filesystem.c:30`) against `fs_globals_t *FI;`
(`filesystem_engine.c:41`) — struct versus pointer, same name.~~ **Fixed,
see "FIXED" above — this was one of the ten, not the only one.**

Also required here: **`exports.txt` does not exist for `ref_soft`, `menu`,
`server` or `client`** — only `filesystem/` and `ref/gl/` have one, and
`xshlib.py:86` reads it unconditionally, so `--static-linking=ref_soft` fails
today even on Linux. And note `XASH_STATIC_LIBS` implies
`XASH_ALLOW_SAVERESTORE_OFFSETS`, which makes the **save format depend on link
layout**.

Settle here which HL SDK base compiles under nxdk. **[from PS3]** Their goal
10 used `FWGS/hlsdk-portable`, not the `halflife-updated` family pinned in
`deps/`. Evidence, not proof, but it points one way.

**Exit:** engine, filesystem, menu and a stub game module link into one XBE.

## X2 — Filesystem and headless boot

**Bigger than it looked.** The `filesystem/` module rests on POSIX that
nxdk does not have. Measured by compiling it: `fcntl.h` is included 8
times across `filesystem/`, `sys/types.h` 7, `sys/stat.h` 7, `unistd.h` 5.
Of those nxdk supplies only a `stat.h` declaring `stat`/`fstat`; its
`unistd.h` is an empty licence stub. There is no `io.h` and no `_commit`,
both of which `io.c` reaches for under `XASH_WIN32` — which is true for
us, since `XASH_XBOX` is a sub-platform of it.

**Strategy: a compat include directory, not `#ifdef`s.** Put `fcntl.h`,
`sys/types.h`, `sys/stat.h` and `io.h` shims in a directory of ours, added
to the Xbox include path only. Most forward to the flat headers nxdk
already ships under `xboxrt/libc_extensions`; the rest supply `O_*`
constants and a few underscore-prefixed CRT functions. That keeps this on
the cheapest rung of the divergence ladder — new files in our own tree,
zero edits to upstream — instead of dozens of guards through a module
upstream actively maintains.

- `filesystem/mpak.c` as a `searchpath_t` (7 pointers, modelled on `pak.c`),
  loading **per resource** through `pfnLoadFile`'s custom `pfnAlloc`/`pfnFree`.
- Read from the Xbox HDD or a mounted ISO.
- **[from PS3]** Headless mode: boot, mount, load a map's data, run server
  frames, no video at all. Asset-path bugs found here are found without a
  renderer in the picture.

**Exit:** the engine loads a map from `.mpak` and runs the server loop with
`ref/null`.

## X3 — Memory budget

Earlier than the PS3 ordering, because 64 MB is a quarter of their budget and
finding the ceiling late is not survivable.

- `tools/asset-studio` emits a per-archive memory manifest and **fails the
  build** when `global + max(map)` exceeds the declared budget.
- `zone.c` gains a per-pool cap and a high-water mark; attribution exists.
- **One ledger, two allocators**: NV2A textures need physically contiguous
  memory outside the normal heap, so a budget counting only the heap is blind
  to half of it.
- Lightmaps to RGB565 — 8 MB to 4 MB on the worst map, and `MpakHeader.h:94`
  already reserves the field.

**Exit:** a declared budget, enforced at bake time, measured at runtime across
level transitions.

## X4 — Video bring-up, standalone

**[from PS3]** Outside the engine. A separate XBE that initialises pbkit, sets
a video mode, clears, and draws one textured triangle.

This is where the NV2A is learned: pushbuffer submission, DMA fencing in
unified memory, texture swizzling, and `fp20compiler`/`vp20compiler` from
`deps/nxdk/tools/`. Getting it wrong here costs an afternoon; getting it wrong
inside the engine costs a week of not knowing which half is broken.

**Exit:** a triangle, on hardware, from a program small enough to read in one
sitting.

## X5 — `ref/soft` on the framebuffer

**[from PS3]** The change their roadmap forced in this plan.

**Do not route this through SDL's window surface.** Write
`engine/platform/xbox/vid_xbox.c` — roughly 250 lines, modelled on
`vid_fbdev.c` — against `hal/video` at 16 bpp. That deletes the 1.2 MB shadow
surface, the per-frame `SDL_ConvertPixels`, and ~350 KiB of SDL software-2D
code from the image. Add double buffering, `XVideoWaitForVBlank`, and
`VID_Info_f`, which exists only in the `sdl2`/`sdl3` backends.

The render-resolution lever is `R_SaveVideoMode(640,480,320,240,…)`, not
`vid_scale`.

This puts a **complete and correct** image on screen — world, entities,
lightmaps, HUD — validating the filesystem, PVS, BSP loading, entity handling
and frame loop before any hardware renderer exists. Everything after compares
against a known-good picture instead of guessing.

It will be slow. That is fine; it is a reference, not a shipping path.

**Exit:** a recognisable Half-Life map on screen, rendered in software — and
its frame time on `c1a3` and `crossfire` at 640×480 and 320×240, from the X0
counters. This is the baseline X9 has to beat; without it, "the hardware path
is faster" is an opinion.

## X6 — Input and menu

- **Gamepad is free.** SDL's controller layer works on nxdk through its own
  OHCI/XID stack, with a `"default,Original Xbox Controller,…"` mapping
  compiled in for every GUID, so no `gamecontrollerdb.txt` needs shipping.
  Rumble works.
- **Keyboard and mouse do not exist, and are not needed.** `XBOX_PumpEvents`
  is an empty function and `in_sdl2.c` compiles inert. Nothing crashes; it
  simply produces nothing.
- Sequence the menu half **after** X1 picks a mechanism — `3rdparty/mainui` is
  C++ and multi-TU, which is exactly where the `ld -r` hazard bites.
- `3rdparty/mainui` compiled in. **[from PS3]** Their goal 7 had the Half-Life
  menu visible before input worked, which confirms the menu is cheap and makes
  a good early validation checkpoint.
- Gamepad navigation per `ui-architecture.md` §7.

**Exit:** the menu is navigable with a controller.

## X7 — Game code

The real game module statically linked, replacing X1's stub, on whichever SDK
base X1 settled.

`vgui_support` to a null stub: `engine/client/vgui/vgui.c:345` reports the
load failure and continues, and single-player Half-Life does not use VGUI.

**Exit:** a map playable in software rendering, with HUD and game logic.

## X8 — Audio

**Largely resolved.** `s_sdl2.c` works on nxdk's Xbox audio driver
(`XAudioInit(16, 2, …)`, `XAudioProvideSamples`, `XAudioPlay`). Two one-line
fixes and it is done:

- `SOUND_DMA_SPEED` is a compile-time `#define` of 44100
  (`engine/client/sound.h:27,29`) with no cvar escape, and the Xbox driver
  hard-forces 48000. That makes an `SDL_AudioStream` resample
  **unconditional** on a 733 MHz CPU. Set 48000 for this build, or pass
  `SDL_AUDIO_ALLOW_FREQUENCY_CHANGE`.
- Voice capture cannot work — `HasCaptureSupport = 0`. Compile out
  `VoiceCapture_Init`.

The NVAPU/ADPCM backend and the `engine/client/sound/` abstraction come off
the Xbox critical path entirely. Keep them as an upstream nice-to-have.

**Exit:** positional sound and sentences.

## XB — Baker output for the NV2A

Between X5 and X9, and runnable alongside X6–X8: it touches only
`tools/asset-studio` and the PC build. Seven of the nineteen real items in
`xbox-performance-review.md` §3 are baker work, and until this phase existed
none of them had an owner.

**Two censuses first, because they decide the rest.** Both are one-minute
scripts over the retail assets, and both answer questions the plan had been
carrying as opinions:

- **Lightstyles.** How many faces per map carry `styles[1..3] != 255`. The
  baker bakes style 0 only (`BspParser.cpp:514-523`), so "RGB565 pages, no
  runtime conversion" currently means no light in the game switches or
  flickers on Xbox. The number picks between per-style pages combined at
  runtime, a baked *on* state with binary switching, or an accepted loss —
  and the choice is written down here, not arrived at.
- **Bones.** Which retail models reference more bones per mesh than a VP20
  palette holds — 192 constants (`deps/nxdk/tools/vp20compiler/config.h:11`),
  three per bone, roughly 50 after the view-projection and light constants,
  against `MAXSTUDIOBONES 128`. Every model over the line needs partitioning
  by bone subset; the code must exist for the ones that do.

Then the format, decided once:

- **Vertex layout** the NV2A reads natively: FLOAT, normalised SHORT,
  D3DCOLOR. No integer attributes. `base_vertex`-chunked u16 indices — the
  decision the Rust port already made, carried over rather than re-derived.
- **Draw lists per cluster, per material**, so runtime submission is a walk in
  baked order. Per leaf alone explodes the draw count, and on pbkit every
  submission is CPU time writing pushbuffer words into write-combined memory.
- **Lightmap pages in RGB565**, with whatever the census decided.
- **Bone partitioning** for the models the census named.
- **DXT laid out linear by block row.** `swizzleNV2A`
  (`TextureOpt.cpp:110-132`) applies a Morton order to DXT blocks, which is
  wrong for `D3DFMT_DXT1` on this hardware. Verified against a known-good
  surface, not reasoned about.
- **Post-T&L cache ordering** of indices. Free offline, invisible to the
  engine.

**The exit below was wrong as first written. Found in review, 2026-09-07.**
"`ref/gl` renders the baked render-half" cannot mean what it sounds like it
means, because `ref/gl` has no consumer for most of what this phase bakes.
Its default world path is `glpoly2_t` fans drawn per polygon
(`R_RenderBrushPoly`, `gl_rsurf.c:1479`) built from `surf->samples`
(`GL_BuildLightmaps`, `:3898`); its one indexed, batched path is `R_DrawVBO`,
which §1 of `xbox-performance-review.md` says not to port — it is off by
default and not a base to build on. `ref/gl` has no concept of "per-cluster,
per-material draw list" or "post-T&L cache order" at all: it walks its own
texture chain every frame regardless of what order anything was baked in.

So there are two different things to validate, and only one of them is
`ref/gl`'s job:

1. **Data correctness — is what got baked right?** Vertex positions, UVs,
   lightmap texel values, the lightstyle policy's output, texture and bone
   assignment. **Corrected 2026-09-07 — the wrong layer was named.**
   `filesystem/mpak.c` is a `searchpath_t` VFS shim (`divergences.md:66`),
   raw bytes in, raw bytes out, the same job `pak.c` does for `.pak` — it
   does not touch `glpoly2_t` or build anything. The reconstruction is the
   render-half loader rewrite `divergences.md` already anticipates
   (`Mod_LoadVertexes/Edges/SurfEdges/Surfaces/Textures/Lighting`, six
   functions in `mod_bmodel.c`, high merge risk): those six read the bytes
   `mpak.c` hands them and populate the same `msurface_t`/`glpoly2_t`
   structures the loose-folder path does, which `ref/gl` then consumes
   unmodified via `GL_BuildPolygonFromSurface` (`gl_rsurf.c:381`) and
   `samples` (assigned at `mod_bmodel.c:3952`). A pixel-compare against the
   loose-folder path is a real, meaningful test of those six functions, not
   of `mpak.c`.
2. **Draw-order correctness — is the per-cluster, per-material list and the
   index chunking actually right for pushbuffer submission?** `ref/gl` cannot
   exercise this; nothing in this codebase can except a pushbuffer consumer.
   That consumer is `ref/nv2a`, and it does not exist until X9.

**Exit, split honestly:** `ref/gl` on PC renders a map from the baked
render-half — vertex, UV, lightmap and material data — pixel-compared against
the loose-folder path. This validates (1). It does **not** validate the
per-cluster/per-material draw-list order, the `base_vertex` index chunking, or
the post-T&L cache ordering — those are unverified until X9 builds their only
consumer, on hardware, and X9's exit criterion is where that verification
actually happens. The D4 principle (the console path runs first on the machine
with a debugger) holds for the data; it does not hold for the submission
shape, and this document should not have implied it did.

## X9 — `ref/nv2a`

The hardware renderer, replacing X5's software path against a picture already
known to be correct, consuming the format XB decided.

- Per-map atlas instead of texture arrays — the baker already produces it.
- Vertex format with no integer attributes: FLOAT1-4, D3DCOLOR and normalised
  SHORT only.
- Register combiners for `diffuse × lightmap × 2` and native alpha test.
- Matrix-palette skinning in VP20, within the 192-register palette, on meshes
  XB has already partitioned.
- **Memoised bone setup.** The reference is eager every frame
  (`R_StudioSetupBones`, `gl_studio.c:815`); this is new work, and it is the
  half of the skinning cost VP20 does not take.
- **Dynamic lighting — two small items, not a redesign.** The reference has no
  per-pixel lights (`xbox-performance-review.md` §1.3): studio models are
  per-vertex with `MAX_LOCALLIGHTS 4`, so raise to the eight the T&L unit
  gives; world dlights are lightmap-space sub-uploads, so measure linear
  (`LU_IMAGE_R5G6B5`) against swizzled pages and pick by number. The "real
  variance" this plan used to name here lives in XB's lightstyle policy.
- `r_alphasort`'s job is already done by `R_TransEntityCompare`
  (`gl_rmain.c:86`); keep that order when the entity list becomes a baked
  walk.
- ~~Fix `swizzleNV2A` on DXT blocks (`TextureOpt.cpp:110-132`)~~ **Moved to
  XB, 2026-09-07: this line duplicated XB's DXT block-row fix and left the
  plan asserting two owners for the same bug.** `TextureOpt.cpp` is
  `tools/asset-studio`, the PC baker — X9 never touches it, and XB already
  lists this fix. What belongs here instead: verify on hardware that XB's
  fix produced a correct swizzle, since xemu is not validation for this.
- Aspect ratio: implement `R_SetDisplayTransform`'s separate `scale_x`/
  `scale_y` and feed a display aspect rather than pixel dimensions.
  `V_AdjustFov` (`cl_view.c:252`) returns early on 640×480, which is exactly
  wrong for anamorphic widescreen. See `ui-architecture.md` §8.

**Exit:** a map drawn on the NV2A, compared frame by frame against X5's
software output and the PC capture — and a hardware frame time on `c1a3` and
`crossfire` against X5's baseline. This is also, per XB's corrected exit
criterion, **the first time the per-cluster/per-material draw-list order, the
`base_vertex` index chunking and the post-T&L cache ordering are verified at
all** — nothing before this phase has a consumer for that shape. Treat the
first hardware run of this phase as a validation of XB's baked format, not
only of the backend, and be ready for the bug to be in the bake.

## X10 — First playable

Full campaign start to finish on real hardware, with HUD, audio, input and
level transitions.

**Exit:** someone finishes Half-Life on an Xbox.

## X11 — Performance hardening

**[from PS3]** Their goal 14 is where 720p and a locked 60 FPS came from, and
several of its findings were transcription bugs and undersized buffers rather
than algorithmic problems. Expect the same shape.

- The counters come from X0 and the baseline from X5. This phase is what
  remains after X9, not the first time anyone measures.
- Measure on `c1a3` and `crossfire`, not `c0a0` — one of the emptiest maps.
- The Rust port's lessons: no allocation in the frame loop; no unconditional
  logging on a serial-console platform. (Memoised bones moved to X9.)
- The rows of `xbox-performance-review.md` §3 that no earlier phase owns:
  the transient-buffer allocation audit; threaded save and load I/O, which
  transfers because the hitch is I/O-bound, not because there is a second
  core; `R_CullSurface` backface rejection against deliberate under-culling,
  measured both ways rather than ported on the strength of the reference
  doing it; and SSE intrinsics only for a loop a hardware profile has named.
- Memory audit: per-map peak, fragmentation after repeated `changelevel`.

**Exit:** a documented frame budget and peak memory figure on real hardware.

---

## Out of scope

- **VGUI1 widgets** — null stub until multiplayer UI is wanted, then adopt
  `FWGS/freevgui` and write only the Surface backend.
- **VGUI2** — its own epic, after X10. It unlocks Counter-Strike, Day of
  Defeat and Blue Shift, and moves the first playable build no closer.
- **Ironwail's GPU-driven techniques** — Vulkan only, structurally.
- **Multiplayer on console.**

## Validation discipline

Borrowed from their AGENTS.md, and it is why their roadmap worked:

- One goal at a time. Do not implement ahead of the current phase.
- Do not speculate about hardware behaviour; confirm it.
- On failure, isolate to a minimal case rather than rewriting.
- Escalate after two failed attempts with diagnostics, not a new hypothesis.
- **xemu is not validation.** DMA timing, the serial channel and unified
  memory behaviour are not faithfully emulated.
