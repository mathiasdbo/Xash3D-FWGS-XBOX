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

**Partial progress, W5 (divergence #29):** `FS_DetermineRootDirectory`
now has a real `XASH_XBOX` branch returning `"D:\\"` (`automount_d_drive`,
already linked, mounts `D:` to the running XBE's own directory before
`main()`) instead of falling through to `compat/xbox`'s honest `getcwd`
`ENOSYS` and `Sys_Error`ing. That is the "finding the root directory"
half only.

**CONFIRMED by a real boot, W5 follow-up (divergence #30):** after fixing
two unrelated static-linking bugs that were hiding it (`filesystem_stdio`
never registered in `lib_static.c`'s table; see #30), a real xemu boot
with the network log channel wired in reached this section's own
predicted failure directly: `FS_SetCurrentDirectory: not implemented,
ignoring...` then `Error: game directory "valve" not exist` /
`Adding directory: ./` / `Couldn't find game directory 'valve'` - even
`"./"` alone fails to resolve, exactly as "NOT sufficient" above
predicted. This is no longer an inference from reading the code; it is
what actually happens. Neither route below is written yet. The exit
line above is still unmet.

**Route 1 written (divergence #31).** `compat/xbox/xbox_posix_io.c` now
tracks a real, emulated process-wide `r3d_cwd` (default `"D:\\"`, the same
`automount_d_drive` guarantee #29 already relies on); `chdir()` sets it for
real (checked against `GetFileAttributesA`, not just accepted), `getcwd()`
reads it back, and `r3d_native_path` — the one function every POSIX call in
this file already routes through — resolves a relative path against it
before the existing `/`→`\` conversion. `filesystem/sys.c`'s
`FS_SetCurrentDirectory` now calls this real `chdir()` on Xbox instead of
printing "not implemented, ignoring..." and returning true.

**CONFIRMED by a real xemu boot, 2026-09-11 (divergence #32) - not yet
real-hardware-verified, see below.** The network log
channel finally reached the guest - the earlier "Windows Firewall silently
dropped it" block was a tooling bug, not a network one (see
`docs/r3d/lessons.md`'s MSYS2 entry: `XBOX_ENGINE_TESTS=1` was never
actually reaching the compiler in the attempts that produced that block,
so the build genuinely never tried to open the socket). Fixed, then two
real bugs surfaced in sequence by the boot log itself, not by reading code:

1. `chdir("D:")` (the literal string `FS_SetCurrentDirectory` receives,
   `engine/common/filesystem_engine.c`'s own `COM_StripDirectorySlash`
   stripping `r3d_cwd`'s default trailing `\` first) failed `EINVAL`. A
   bare `"D:"` names the `D:` device SYMLINK OBJECT in the NT object
   manager namespace `GetFileAttributesA` resolves against; only `"D:\"`
   resolves THROUGH the symlink to the directory FATX mounts there -
   POSIX has no such distinction, which is exactly why this belongs in
   `r3d_native_path`, not the shared caller. Fixed: a bare drive-letter
   path now gets its trailing `\` appended before resolution.
2. With that fixed, boot progressed to `listdirectory("./")` still
   failing to find `valve` at all (`FI.numgames` stayed 0). `r3d_native_path`
   was appending `"./"` onto `r3d_cwd` as a literal path component
   (`"D:\.\ "`) rather than collapsing it - correct on real Win32 (kernel32
   canonicalises `\.\` before calling `NtCreateFile`), but nxdk's own
   `FindFirstFileA` hands that string to the object manager unresolved,
   where `.` is not a real name under `\??\D:\`. **Found in Codex review**:
   a first version of this fix only collapsed `path` exactly equal to `"."`
   or `"./"`, leaving `"./valve"` (the shape `filesystem/dir.c:468-474`'s
   own root-searchpath file opens actually use, not just the bare
   `opendir("./")` case) still producing `"D:\.\valve"` - the same
   unresolvable string one level down. Fixed for real: any leading `"./"`
   (or a bare trailing `"."`) is now stripped from `path` before the
   `r3d_cwd` join, for every caller, not just the exact-string case.

**All three fixes verified by real boot logs, not separately claimed**:
after fix 1 alone, the boot log read `Couldn't find game directory 'valve'`
(progress - EINVAL gone, but `FI.numgames` still 0); after fix 2's first,
narrower version, on a disc whose `valve/` holds only a synthetic
`gameinfo.txt`, the engine found it and progressed past game-directory
resolution entirely into `Host_InitCommon: couldn't load gfx.wad` - at the
time, read as "expected" (no real assets on that disc) but, per the Codex
finding, not actually ruled out as the SAME path bug one level deeper.
After the corrected fix, a disc whose `valve/` holds `gameinfo.txt` plus a
real, retail `gfx.wad` (86,608 bytes) booted with **no `couldn't load
gfx.wad` error at all** - the file genuinely opens now through the exact
`"./valve/gfx.wad"`-shaped root-searchpath path Codex named. `FI.numgames
> 0`, the game directory mounts, and a real asset opens through it - the
X0.5 exit line, this time correctly closed. On a real full retail `valve/`
disc (392 MB, ~4,200 files), boot was still progressing (real, non-zero,
climbing CPU time; no crash) when this entry was written, apparently
throttled by xemu's own DVD-read pacing for a disc this size rather than
anything filesystem-layer - `Host_InitCommon` onward is X2's own scope to
pick up from here, not X0.5's.

**Exit met on xemu - not yet closed.** The engine finds its game directory
and mounts it (`FI.numgames > 0`, proceeding into `Host_InitCommon`
looking for real game assets - the exact boundary this phase's own exit
line named), but **AGENTS.md's own Verification section is explicit:
"xemu is not validation... A phase does not close until it runs on a real
Xbox."** DMA timing, the serial channel and real unified-memory behaviour
are not faithfully emulated, and this phase's own fix (NT object-manager
path resolution against a real FATX volume) is exactly the kind of thing
an emulator's filesystem shim can paper over differently than real
hardware would. **Found in Codex review, round 2** - do not read the
paragraph above as X0.5 being done; a real-Xbox boot of the same fix,
observing the same `FI.numgames > 0` and a real asset opening through
`"./valve/..."`, is still the open step that actually closes this phase.
A real-hardware-testable XISO is packaged per AGENTS.md's own convention
(`../XISOS/`) specifically so that boot can happen.

**What made this observable at all, this session:** the client (non-dedicated)
target had never actually called `Host_Main` before now -
`engine/platform/xbox/xbox_main.c`'s placeholder `main()` (X1, divergence
#23) linked clean but never referenced it, so X1's own "63 undefined, 0
duplicate" result never proved the client's real boot path links, only
that the dead scaffolding around it does. A new `XBOX_CLIENT_MAIN=1`
opt-in (`tools/xbox-cc.sh`, mirroring `XBOX_DEDICATED`'s existing shape)
makes `engine/common/launcher.c`'s real `main()` (`Host_Main`, the same
entry point PSVita/iOS and this project's own dedicated target already
use) provide `_main` instead, and `tools/xbox-link-check.sh --client-main`
selects it over `xbox_main.obj`. Reaching a real, complete client link (0
duplicate, 0 undefined, real XBE) from there needed three more real fixes,
each verified by compiling and linking, not assumed: `engine/common/net_chan.c`
needed `bzlib.h` on its include path (bzip2 is a real, uncompiled submodule -
now built from its own `CMakeLists.txt`-named 7-file source list, `BZ_NO_STDIO`
on `bzlib.c` only since nxdk's pdclib has no `setmode`/`fdopen`, and a small
generated `BZ2_bz__AssertH__fail` - upstream's own definition sits inside
that same `BZ_NO_STDIO` guard, so defining it drops the one function
`bzlib_private.h`'s `AssertH()` macro calls unconditionally); `engine/client/cl_steam.c`
needed `getsockopt`/`SOL_SOCKET`/`SO_ERROR` added to
`engine/platform/stub/net_stub.h` (every OTHER BSD socket call was already
stubbed there - a real gap, not a deliberate omission, since `XASH_NO_NETWORK=1`
is unconditional and every stub already just fails). None of this is
speculative scaffolding: it is the real upstream client boot path, now
provably reachable on Xbox for the first time.

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
  **Fixed, 2026-09-08 - see the FIXED section below.** The
  vendored-patch-versus-compat-shim question resolved as both, not
  either/or: `GetFileType`/`_wfopen`/`_wfreopen` have no nxdk equivalent
  at all, so `stream.c` is patched (matching `library_suffix`'s own
  precedent, divergence #5) to skip them entirely rather than shim
  something that does not exist; `_fileno`/`_get_osfhandle`/
  `_filelengthi64` DO have a real nxdk equivalent (`GetFileSizeEx`), so
  those are genuine new `compat/xbox` functions, not a patch. `fdopen()`
  itself (not just its `_WIN32` caller) is also missing from pdclib -
  `op_fdopen` fails honestly instead, since nothing in this tree calls
  it. `3rdparty/opusfile` is now 4 of 4.

**Answers the question this section asked.** `3rdparty/opus`/`opusfile`/
`vorbis`/`MultiEmulator` are viable for Xbox - 180 of 183 real files
(176 of 179 named above, plus opusfile's 4 of 4, fixed 2026-09-08) now
compile, no architectural blocker found anywhere. What remains is one
well-isolated gap (`vq_sse2.c`'s genuine, loud compile failure under
`-march=pentium3` — a real reason to exclude that one file specifically,
not the whole x86 SIMD set), not an open question about whether these
libraries can work here at all.

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

### FIXED, 2026-09-08: `opusfile` linked in too, 74 undefined down to 64

The `stream.c` gap named above is fixed — not by shimming, since two of
its three missing calls (`GetFileType`, `_wfopen`/`_wfreopen`) have no
nxdk equivalent to shim against at all. `op_get_file_callbacks`,
`op_fopen`, `op_freopen` and `op_fdopen` are patched under
`#if defined(NXDK)` instead (`patches/opusfile-xbox-stream.patch`,
auto-applied by `tools/xbox-cc.sh` the same way divergence #5's
`library_suffix` patch already is — divergence #18): always-seekable
(every `FILE*` reaching this platform's `op_fopen`/`op_fdopen` is a real
FATX file, never a pipe), plain `fopen`/`freopen` (GoldSrc paths are
ASCII, no Unicode filename to preserve), and an honest `ENOSYS` for
`op_fdopen` (its `fdopen()` call has no pdclib equivalent on any
platform, and nothing in this tree calls `op_fdopen` at all). The two
calls that DO have a real nxdk equivalent — `_fileno`/`_filelengthi64`
for `op_fseek`'s `SEEK_END` path — are genuine new functions in
`compat/xbox/xbox_posix_io.c` (declared in `compat/xbox/io.h`):
`_get_osfhandle` looks up the real `HANDLE` this file's own `fileno()`
already registers in `r3d_fds`, `_filelengthi64` calls the genuinely
real `GetFileSizeEx` on it. `info.c`/`internal.c`/`opusfile.c` needed no
changes. All 4 of 4 compile clean.

**Corrected 2026-09-08, found in review:** the first version of this
patch left `op_fseek`/`op_ftell`'s own `_WIN32` body untouched while
`op_get_file_callbacks` was widened to always report seekable — meaning
that body was now live, not merely compiling. It type-puns an 8-byte
`opus_int64` as `fpos_t` for `fgetpos`/`fsetpos`, safe only if `fpos_t`
is an opaque 8-byte blob (true of MSVCRT, false of nxdk's pdclib, whose
`fpos_t` is a real ≥12-byte struct). The `OP_ASSERT` meant to catch this
compiles to nothing without `OP_ENABLE_ASSERTIONS`, which nothing in
this build defines — a silent stack overrun on any seek or tell, not a
loud one. Fixed by widening `op_fseek`/`op_ftell` too, using this
project's own `_fseeki64`/`_ftelli64` (already real, already used by
`3rdparty/vorbis` under the identical condition) instead of making any
new assumption about pdclib's `fpos_t` layout.

Linked in alongside the 174 objects from the FIXED section above (178
total): **0 duplicate symbols, 64 undefined** — down from 74, because
the 10 `op_*`/`opus_tags_query` symbols `engine/client/soundlib/
snd_ogg_opus.c` already referenced (part of the 74, previously
unresolved because `opusfile` was not yet compiled) are now supplied.
The 64 remaining are exactly the 63 parked networking symbols plus 1
`_main`, unchanged. Re-verified with `tools/xbox-link-check.sh
--expect-undefined 64`: exact match.

Also found in review: `tools/xbox-link-check.sh`'s own `--expect-undefined`
gate only ran when the flag was actually passed, so the exact command
this script's own usage comment showed at the top — without the flag —
exited 0 on a link with 64 real undefined symbols, indistinguishable
from clean in the exit code. Fixed: any undefined symbol with no
`--expect-undefined` given is now itself a hard failure.

**Found in review, second pass:** making `op_get_file_callbacks` always
report seekable had a side effect the first fix missed — `op_fseek`'s
`SEEK_END` call to `fileno()` now runs on every seekable open (opusfile
itself seeks to end during open, `opusfile.c:1399`), registering a
borrowed slot in `compat/xbox`'s descriptor table. That slot is only
ever released by this project's own `close(fd)`, never by plain
`fclose()` — which is exactly what opusfile's default close callback
was. Every seekable opusfile open would have permanently consumed one
of `R3D_MAX_FDS - 3 = 61` slots for the process's whole life — latent,
since nothing calls opusfile from the engine yet, but a real leak
regardless of when it fires. Fixed with a new `_r3d_release_stream_fd`
(releases a borrowed slot for a `FILE*` without touching the OS
handle) and an NXDK-specific close callback (`op_fclose_nxdk`) that
calls it before `fclose` — divergence #18. Re-verified: `stream.c`
still compiles clean, and the full sweep plus link-check both still
report their unchanged baselines (`187 ok, 34 failing`;
`0 duplicate, 64 undefined`).

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

~~Also required here: **`exports.txt` does not exist for `ref_soft`, `menu`,
`server` or `client`** — only `filesystem/` and `ref/gl/` have one, and
`xshlib.py:86` reads it unconditionally, so `--static-linking=ref_soft` fails
today even on Linux.~~ **Corrected 2026-09-09, found while investigating
this: two claims here were wrong. `3rdparty/mainui/exports.txt` (the menu
module) already exists (`GetMenuAPI`, `GetExtAPI`) — checked directly, not
just "only filesystem/ and ref/gl/" as this row claimed. More importantly,
`scripts/waifulib/xshlib.py`'s `apply_static()` (the code that actually
reads `exports.txt`) is GNU `ld -r`/`objcopy -G` relocatable-object
machinery — the exact "large structural list" fallback this section's own
earlier row already retired as moot for Xbox, since there is no GNU
binutils in this toolchain at all (`tools/xbox-cc.sh`/`tools/xbox-link-
check.sh` bypass waf and `xshlib.py` entirely, doing every compile and
link by hand). `exports.txt` existing or not existing for `server`/
`client`/`ref_soft` cannot block an Xbox link that never calls
`apply_static` in the first place. This is only a real requirement for the
waf toolchain module (`DEST_OS` integration, X0's own open item) if that
work ever chooses to route Xbox through `--static-linking` instead of
continuing this project's own manual scripts — undecided, not yet needed.**
And note `XASH_STATIC_LIBS` implies `XASH_ALLOW_SAVERESTORE_OFFSETS`, which
makes the **save format depend on link layout**.

Settle here which HL SDK base compiles under nxdk. **[from PS3]** Their goal
10 used `FWGS/hlsdk-portable`, not the `halflife-updated` family pinned in
`deps/`. Evidence, not proof, but it points one way.

### MEASURED, 2026-09-09: the C++ toolchain compiles, and `new`/`delete` link clean

The menu module (`3rdparty/mainui`, 76 `.cpp` files) and the HL SDK's
`client`/`server` are all C++, and nothing in this tree had compiled a
single C++ translation unit for Xbox before now — `tools/xbox-cc.sh` is
C-only (`clang`, not `clang++`). Built `tools/xbox-cxx.sh`, the same
reordered-includes/prelude/patch-preflight shape as `tools/xbox-cc.sh`
(duplicated rather than shared — no sourcing convention exists yet between
`tools/xbox-*.sh` scripts, not worth inventing for two copies), calling
`nxdk-cxx`'s own flags (`deps/nxdk/bin/nxdk-cxx`) reordered so
`compat/xbox` wins, same as the C script already does for `nxdk-cc`.

Verified against two representative test files (virtual dispatch,
inheritance, templates, `<cstdint>`, `new`/`delete`): both compile clean.
**Compiling is not linking, and this script only ever compiles (`-c`),
same scope as `tools/xbox-cc.sh`.**

~~`deps/nxdk/lib/libcxx` vendors HEADERS only — grepped the whole
`deps/nxdk` tree, there is no prebuilt `libcxx.lib` anywhere... genuinely
undefined anywhere in this nxdk checkout... any C++ code using
`new`/`delete` needs a `compat/xbox` shim for these two operators before
it can link at all.~~ **False, corrected 2026-09-09, found in review: the
search behind this claim missed the library by name alone.** The real
file is `deps/nxdk/lib/libc++.lib` (literally "libc++", not "libcxx") —
a genuine, prebuilt static library, not headers-only. `deps/nxdk/share/
toolchain-nxdk.cmake:37` already links every C++ target against it, and
`deps/nxdk/lib/libcxx/src/new.cpp` defines `operator new`/`operator
delete` as weak symbols under `defined(NXDK)` — real, not aspirational.
Verified by an actual `nxdk-link`, not just reading the source: the
`new`/`delete` test case linked against `libc++.lib` plus the base
library set (`libxboxkrnl`, `libpdclib`, `libwinapi`, `libnxdk_hal`,
`libnxdk`, `libxboxrt`) resolves both operators with **zero errors** — a
version with a real `main()` link with only a benign `lld` `.edata`-merge
warning, nothing else. **No `compat/xbox` shim is needed for `new`/
`delete` at all — a caller linking real C++ code just needs to add
`deps/nxdk/lib/libc++.lib` to its library list**, the same way the
existing 18-library set already gets built for C. Whatever else real C++
code might need at link time (RTTI, `dynamic_cast`, exceptions — the
`-fno-exceptions` flag both `nxdk-cxx` and `tools/xbox-cxx.sh` pass is
still real and still means throwing/catching will not work) remains
unmeasured, but the allocator question this row previously flagged as
"a real decision, fixed once decided" was never actually open — `libc++`
already decided it, correctly, for this exact platform.

**Corrected 2026-09-09, found in review: synthetic test files alone don't
prove the tool works on the actual module named above — they proved the
toolchain flags work, not that mainui's own include/define surface is
reachable.** Verified against a real `3rdparty/mainui/*.cpp` file
instead, with mainui's own `wscript`-declared include roots and
`STDINT_H=<cstdint>` define passed as extra args (deliberately not baked
into `tools/xbox-cxx.sh` itself — that script's own docstring calls it a
generic "one C++ translation unit" compiler, the same posture
`tools/xbox-cc.sh` takes for the engine specifically but a menu-specific
include list does not belong in a tool meant to also serve the HL SDK's
`client`/`server` later). `3rdparty/mainui/utflib.cpp` — genuinely
self-contained, no font/GL dependency — compiles clean with:

```
tools/xbox-cxx.sh 3rdparty/mainui/utflib.cpp \
  -I3rdparty/mainui -I3rdparty/mainui/sdk_includes/common \
  -I3rdparty/mainui/sdk_includes/engine -I3rdparty/mainui/sdk_includes/public \
  -I3rdparty/mainui/sdk_includes/pm_shared -DSTDINT_H='<cstdint>'
```

A second, more representative file (`EventSystem.cpp`) surfaced two more
real, concrete gaps along the way, worth recording rather than chasing
further in this same pass:

- **`3rdparty/mainui/miniutl` (a nested submodule, `FWGS/miniutl`) was
  never initialised** — `git submodule status` reported it uninitialised
  before this session, and nothing in `AGENTS.md`/`README.md` documents
  running `git submodule update --init --recursive` (only a plain
  non-recursive init, which does not descend into a submodule's own
  `.gitmodules`). Fixed locally (`git -C 3rdparty/mainui submodule update
  --init miniutl`) to continue investigating, but the actual fix belongs
  in whatever setup instructions a fresh clone follows, not silently
  worked around here.
- **`3rdparty/mainui/miniutl/miniutl.h` includes `<new.h>`** (the
  non-standard, pre-standard placement-new header MSVC has always
  shipped, not the real `<new>` `tools/xbox-cxx.sh`'s test files already
  used) — nxdk's `libcxx` does not provide it. Whether this needs a
  one-line `compat/xbox` shim (`<new.h>` including `<new>` and nothing
  else, matching how real MSVC's own `<new.h>` is defined) or whether
  `miniutl` has a configuration knob to avoid it is unmeasured — not
  fixed here.

Neither of these blocks `tools/xbox-cxx.sh` itself, which does exactly
what it claims (compiles one C++ translation unit given the right
flags) — both are real, separate gaps in getting the *menu module
specifically* to compile, exactly the kind of thing this measurement
pass exists to surface before guessing at scope.

### FIXED, 2026-09-09: the menu module — 82 of 82 files compile for Xbox

Both gaps above are closed, and the full picture is measured, not just
one or two representative files.

**`<new.h>`**: `compat/xbox/new.h` (new, divergence #19) — one line of
substance, `#include <new>`, matching exactly what real MSVC's own
`<new.h>` does. **`3rdparty/mainui/miniutl` initialisation**: the missing
step is now a fast, explicit `tools/xbox-cxx.sh` preflight failure with
both required commands spelled out (`git submodule update --init
3rdparty/mainui` then `git -C 3rdparty/mainui submodule update --init
miniutl`), the same shape as the `library_suffix`/`opusfile` preflights
already use, rather than a silent local workaround.

With `<new.h>` fixed, a fuller sweep surfaced one more real gap of the
identical shape: `miniutl/winlite.h`'s `#ifdef _WIN32` block
unconditionally includes `<winsock2.h>` and `<dbghelp.h>` — both
genuinely reachable (same `_MSC_VER`/`_WIN32`-is-real-but-the-assumption-
isn't class as divergences #17/#19), and both **already-known,
deliberately-parked gaps** — real Winsock2 (this project's own networking
is parked, see #7c/#13) and a debug-symbol API (no equivalent exists on
this platform at all, #7c). Patched to skip exactly those two includes
under `#ifndef NXDK`, keeping the rest of the file (`windows.h` itself,
every `#undef` guarding a Win32/class-member name collision) unchanged —
divergence #20, `patches/miniutl-xbox-winlite.patch`, same auto-apply
mechanism as `library_suffix`/`opusfile`.

Built `tools/xbox-mainui-sweep.sh` rather than leaving the measurement as
a one-off shell loop — the exact "not repository-backed" mistake this
project's review process has already caught more than once (the
duplicate-symbol sweep-command gap, the pre-`xbox-link-check.sh` audio
codec measurement). Compiles every file `3rdparty/mainui/wscript`'s own
`build()` declares (`*.cpp` plus six subdirectories — not "everything
under the directory," `tests/`/`vs2022/`/`dictgen.py` are not part of the
library), with `wscript`'s own real include roots, its unconditional
`STDINT_H=<cstdint>` define, and takes `--expect-fail N` the same way
`tools/xbox-sweep.sh` already does.

**Corrected 2026-09-09, found in review: the first version of this sweep
never passed `-DMAINUI_USE_CUSTOM_FONT_RENDER`, so `font/WinAPIFont.cpp` —
gated entirely on `defined(MAINUI_USE_CUSTOM_FONT_RENDER) &&
!defined(MAINUI_USE_STB)`, and the file that actually calls real Win32
GDI (`CreateCompatibleDC`, `EnumFontFamiliesExA`) — compiled down to an
empty translation unit instead of being tested at all. The 82/82 result
was vacuous for that one file, not real.** Fixed by passing the define
`wscript`'s own `configure()` sets unconditionally, plus
`-DMAINUI_USE_STB`: not a guess, the only font backend upstream's own
code leaves reachable on this platform. `font/FreeTypeFont.cpp`'s own
guard is `!defined(_WIN32) && defined(MAINUI_USE_FREETYPE)` — real
Windows never gets FreeType in upstream's own code, and `_WIN32` is
genuinely true here (same as every other `_MSC_VER`/`_WIN32`-is-real
trap this phase has already named), so FreeType was never a live option
regardless of nxdk having a real `libfreetype.lib`. `WinAPIFont.cpp`
needs real GDI, which does not exist. `StbFont.cpp` is fully
self-contained (`stb_truetype.h`, no OS font API at all) — the same
bucket `wscript`'s own `configure()` already puts every other
console/embedded target in (`android`, `darwin`, `nswitch`, `psvita`,
`emscripten` all get `MAINUI_USE_STB` unconditionally; Xbox has no
`DEST_OS` assigned yet to trigger that list, but belongs in it for the
identical reason — no window manager, divergence #9). Re-verified with
the corrected defines: **82 of 82**, the same result, now for real —
`tools/xbox-mainui-sweep.sh --expect-fail 0 --expect-total 82` passes
exactly. The main engine sweep baseline is unaffected (`187 ok, 34
failing`, unchanged).

**Corrected 2026-09-09, found in review (second pass): the sweep's own
glob loop silently skipped an unmatched path (`[ -f "$f" ] || continue`),
so a broken checkout, a moved source file, or a future `wscript`
source-list drift that matched nothing would have printed "0 ok, 0
failing" — and `--expect-fail 0` alone would call that a pass, the exact
false-clean class this project's review process keeps catching.** Fixed:
zero attempted files is now always a hard failure, unconditionally, and
`--expect-total N` compares the real attempted count against a tracked
baseline the same way `--expect-fail` already does. Verified both ways —
hiding every mainui source file (renamed away, not deleted) makes the
sweep fail loudly with "0 files matched any glob," and the real checkout
still reports `82 ok, 0 failing, 82 attempted` matching both gates.

**Also found in review: the `miniutl` patch preflight had been placed in
`tools/xbox-cxx.sh`, the generic C++ compiler script, forcing every
caller — including unrelated future work like the HL SDK `client`/
`server` that script's own docstring says it exists to serve — to have
`3rdparty/mainui/miniutl` initialised even to compile something that has
nothing to do with the menu module.** Moved into
`tools/xbox-mainui-sweep.sh`, the only script that actually needs it.
`tools/xbox-cxx.sh` is generic again, verified by compiling an unrelated
synthetic file with `3rdparty/mainui/miniutl` never touched.

**Not measured**: RTTI/`dynamic_cast`/exceptions (mainui's own `wscript`
passes `-fno-rtti`, matching `tools/xbox-cxx.sh`'s scope, but nothing has
tested whether `libc++.lib` alone is sufficient for whatever else 82
files' worth of real code needs at link time beyond `new`/`delete`).

### FIXED, 2026-09-09: it links — engine, filesystem, third-party audio, and the menu module, together

**Not "whether this whole module actually LINKS at all" any more.**
Linked the menu module's 82 objects in alongside the engine and
third-party audio sets already proven (the FIXED sections above) plus
`deps/nxdk/lib/libc++.lib`: **0 duplicate symbols.** Undefined started at
65 — one more than the already-known 64 — and that one new symbol was
`_atof`. Real gap, not a mainui-specific one: nxdk's pdclib **declares**
`atof` in `<stdlib.h>` but has never implemented it (`deps/nxdk/lib/
pdclib/include/stdlib.h:29`'s own comment: `TODO: atof(), strtof(),
strtod(), strtold()`). `3rdparty/mainui/CFGScript.cpp`,
`ConnectionProgress.cpp` and `ServerInfo.cpp` all call it for ordinary
config/cvar values, not scientific-notation-heavy work. Fixed with
`compat/xbox/xbox_crt_ext.c` (new — a self-contained C99 7.20.1.2 parser,
not `return strtod(...)`, since pdclib has no `strtod` either) —
divergence #21. **Re-linked with the fix: 0 duplicate, 64 undefined —
exactly the already-known set (63 parked networking, 1 `_main`), verified
symbol-for-symbol identical, not just count-identical.**

Along the way, adding 82 more objects to an already-large link pushed the
raw `nxdk-link` command line past the OS argument-length limit
("Argument list too long"). Fixed `tools/xbox-link-check.sh` to build a
response file (`@file`, the standard MS `link.exe` convention `lld
-flavor link` — what `nxdk-link` wraps — already accepts) instead of a
raw command line, which sidesteps the limit regardless of how large a
future sweep gets. One more real trap found building this: writing
MSYS2-style paths (`/g/...`) into that file failed with "could not open"
for objects that genuinely existed — MSYS2 bash only auto-translates a
POSIX path to a real Windows one when it is an actual `argv` entry passed
directly to a native `exec`, not when it is text a program reads back
from a file itself. Fixed with `cygpath -m` on every path written to the
response file.

`tools/xbox-link-check.sh` now includes the menu module automatically
when `tools/xbox-mainui-sweep.sh` has been run (optional — a caller
measuring just engine+third-party is not forced to also sweep mainui
first), so this is the real, permanent, repository-backed measurement
tool for the whole picture, not a one-off experiment.

**Corrected 2026-09-09, found in review, two findings:**

**Stale objects.** `tools/xbox-mainui-sweep.sh` only ever added `.obj`
files to its output directory, never removed any — a file that compiled
successfully once and later got deleted, renamed, or started failing
would leave its stale object sitting there untouched, indistinguishable
from a genuinely current one to anything reading the directory. Fixed:
the sweep now clears every `.obj` (and its own manifest) before
compiling, and writes `manifest.txt` (`OK=/FAIL=/TOTAL=`) after every
run, pass or fail. `tools/xbox-link-check.sh` now requires that manifest
before trusting mainui objects at all — refusing (not silently
degrading) if it is missing (predates the fix, or the sweep never
finished) or reports `FAIL>0` (the last sweep was not clean).

**Corrected 2026-09-09, found in a second review pass on this same fix:
the manifest write happened BEFORE the sweep's own zero-total rejection,
so a broken, empty sweep would still write `OK=0 FAIL=0 TOTAL=0` and
exit 1 — leaving a manifest that reads as clean (`FAIL=0`) to anything
checking only that one field, which is exactly what
`tools/xbox-link-check.sh`'s own manifest check originally did. Fixed
two ways: the sweep now rejects `TOTAL=0` before ever writing a manifest
at all (so that failure mode leaves no manifest, caught by the existing
"missing manifest" refusal), and `tools/xbox-link-check.sh` independently
requires `TOTAL>0` in whatever manifest it reads, as defense in depth
against a hand-edited or older-script-written one. Also added a third
check while fixing this: the manifest's own `OK` count against the real
`.obj` count on disk right now, since the manifest could be stale
relative to `$OUT_DIR` even when it was genuinely clean the moment it
was written (an object deleted by hand afterward). Verified by
reproducing the exact failure Codex described — hiding every mainui
source file and running the sweep, then the link-check, in sequence:
the sweep fails loudly and writes no manifest, and the link-check
correctly refuses rather than silently proceeding with zero menu
objects.** Also verified the disk/manifest mismatch case directly
(deleting one `.obj` after a clean sweep) and the original three states
(no manifest, failing manifest, genuinely clean manifest) — all five
states now produce the correct accept/refuse decision.

**Unbounded `atof` exponent.** The first version of `compat/xbox/
xbox_crt_ext.c`'s `atof` accumulated its exponent digit-by-digit with no
bound, then scaled by `10.0` once per exponent unit — a string like
`1e2147483647` would have driven a multi-billion-iteration loop (a real
CPU-hang on a 733 MHz CPU, not theoretical: `ServerInfo.cpp` passes
server-derived `Info_ValueForKey` output straight to `atof`, once
networking is unparked) and signed-overflowed the accumulator before
that loop was even reached. Fixed by clamping exponent accumulation at
1000 — past double's real ~±308/-324 dynamic range, so every legitimate
value is unaffected and anything beyond it correctly saturates to
inf/0 without the unbounded loop or the overflow. Verified natively, not
just re-read: compiled and ran the same algorithm on the host — extreme
exponents now resolve to the correct inf/-inf in under a microsecond,
ordinary cases unaffected.

**Answers three of the four names in this phase's own Exit line.**
Engine, filesystem, and the menu module all link together, cleanly, with
zero duplicate symbols and only the already-known, deliberately-parked
gaps left.

### FIXED, 2026-09-09: the fourth name, too — a stub game module links in

`stub_game/` (new, divergence #22) — deliberately not `deps/hlsdk` (the
real, ~160-file Half-Life SDK already vendored for a later phase). A
"stub" proves the engine's static-linking-by-name mechanism
(`engine/platform/misc/lib_static.c`'s `Lib_Find`, populated by
`tools/gen-library-tables.sh`, since Xbox has no dynamic loading and
bypasses `xshlib.py`'s GNU-binutils relocatable-object machinery
entirely) resolves a real game module, not that any gameplay logic
works — real game code is X7's job.

`server/server_stub.c` exports `GetEntityAPI2` because
`engine/server/sv_game.c:5235` genuinely hard-requires at least one of
`GetEntityAPI`/`GetEntityAPI2` — it fails loudly ("missing GetEntityAPI
and GetEntityAPI2 exports") otherwise. Returns a validated-but-
entirely-zeroed `DLL_FUNCTIONS`: every function pointer `NULL`, honest
about implementing nothing rather than a fake success — safe because
nothing in this tree calls through any of those pointers yet (no Xbox
`main()`/entry point exists, divergence #14). `client/exports.txt` is
deliberately **empty**: `cl_game.c`'s per-name `COM_GetProcAddress`
lookups only matter inside `CL_LoadProgs`, which only runs during a
real client boot never reached, and a name missing from a static table
just leaves that one function pointer `NULL` rather than failing the
load — a materially different, softer contract than the server side's
hard check, so there is nothing dishonest about exporting zero names
where zero are actually implemented.

`tools/gen-library-tables.sh` extended to generate a real per-library
export table (`lib_<name>_exports[]`, mirroring `xshlib.py:83-89`'s
`write_export_list()` exactly — the same extern-void-then-take-its-
address idiom for whatever the function's real signature is, not a new
invention), not just the previously-empty outer table.

**Verified by linking, not just compiling**: `server_stub.c` plus both
generated export tables compiled and linked in alongside the full
engine + third-party + menu set: **0 duplicate symbols, 64 undefined —
the exact already-known set, verified symbol-for-symbol identical.**
The main engine sweep baseline is unaffected (`187 ok, 34 failing`).

**Corrected 2026-09-09, found in review, two findings:**

**Missing second hard requirement.** The first version of `server_stub.c`
only exported `GetEntityAPI2`. `engine/server/sv_game.c:5245-5255`
performs an entirely separate, equally hard check for `GiveFnptrsToDll`
immediately after the `GetEntityAPI`/`GetEntityAPI2` check —
`SV_LoadProgs` fails ("missing GiveFnptrsToDll export") and frees the
library before `GetEntityAPI2` is ever called if this second export is
absent. A stub without it links but is not actually loadable under the
real engine's own load sequence — a genuine gap between "the symbol
table resolves" and "this satisfies the real contract," exactly the
kind of thing that matters given this section's whole point is proving
the load *mechanism*, not just producing a clean link count. Fixed: a
matching, equally honest no-op `GiveFnptrsToDll( enginefuncs_t *,
globalvars_t * )` — its own typedef (`GIVEFNPTRSTODLL`) is local to
`sv_game.c`, not a shared header, matched here by parameter type
instead. Re-verified: still `0 duplicate, 64 undefined`, the identical
set — confirmed symbol-for-symbol, not just by count.

**Stale generation cache.** `tools/xbox-cc.sh` only checked whether
`generated_library_tables.h` existed before skipping regeneration — a
`.sweep/gen` left behind by an OLDER version of this script (from before
`generated_lib_*_exports.c` existed at all, when the table only ever had
zero library names) would look "already generated" forever, even though
the two files this build now also needs were never produced. Fixed: all
three expected outputs are checked, not just the first one written.
Verified by reproducing the exact scenario — hand-writing a stale,
pre-this-branch-shaped `generated_library_tables.h` with no matching
exports files, then confirming `tools/xbox-cc.sh` correctly detects the
gap and regenerates all three files.

**Corrected 2026-09-09, found in a second review pass, two more
findings:**

**The stub still could not actually be loaded.** `SV_LoadProgs` calls
`svgame.dllFuncs.pfnGetGameDescription()`, `.pfnGameInit()` and
`.pfnRegisterEncoders()` unconditionally as part of its own successful-
load path (`sv_game.c:5327,5330,5338`) — not deferred to a later game
frame the way every other `DLL_FUNCTIONS` member genuinely is. An
entirely-zeroed table would NULL-pointer-call all three the instant
`SV_LoadProgs` succeeded, so the earlier "safe because nothing calls
through these pointers yet" claim was false for these three
specifically. Fixed with real, honest, minimal bodies for exactly
these three (a static description string, and two no-ops) — the other
~37 `DLL_FUNCTIONS` members stay genuinely `NULL`, since a full read of
`SV_LoadProgs`'s own synchronous load path confirms nothing else in it
touches them.

**Menu was never actually registered.** 82 mainui objects being present
in the link never meant the menu module was reachable —
`lib_static.c`'s `Lib_Find` only resolves names present in
`generated_library_tables.h`, and this section's first version never
added `"menu"` there at all (`engine/common/lib_common.c:220`'s own
default, `COM_GenerateClientLibraryPath("menu", ...)`, confirms `"menu"`
— not `"gameui"` or anything else — is the real name to register; under
`XASH_INTERNAL_GAMELIBS`, already set here, that function just copies
the name through unchanged). Fixed by extending `tools/gen-library-
tables.sh`'s invocation to also cover `3rdparty/mainui/exports.txt` —
`GetMenuAPI`/`GetExtAPI`, real, already-existing functions in
`3rdparty/mainui/udll_int.cpp`, no new stub code needed for menu at all
— and compiling/linking the resulting export table in
`tools/xbox-link-check.sh`. Because the outer table now declares
`lib_menu_exports` unconditionally (generated once, before this script
knows whether a given run will include mainui), a run *without* mainui
swept needs a real, if trivial, definition too — a genuinely empty,
terminated `table_t` array, so `COM_LoadLibrary("menu")` would still
resolve to something (just export nothing) rather than leaving an
undefined symbol. Both configurations re-verified directly: the "no
mainui" measurement and the full measurement each report `0 duplicate,
64 undefined`, and every one of the 64 was checked against the known
networking/`_main` categories directly, not assumed from a count match.

**All four names in this phase's own Exit line now genuinely link
together into one binary.** Only an actual bootable XBE remains — this
section still only produces a test `.exe` via `nxdk-link`'s raw
`-out:`, not a real `nxdk-cxbe` conversion, and `_main` is still among
the 64 undefined symbols (no Xbox entry point exists in this tree at
all yet) — an XBE cannot exist without one.

### FIXED, 2026-09-09: a real .xbe exists

`engine/platform/xbox/xbox_main.c` (new, divergence #23) provides
`main()` — deliberately not `Host_Main`. Calling into the real engine
boot sequence is genuinely X2+ work (`filesystem/`'s own POSIX layer is
already flagged "bigger than it looked" below; nothing about a real
boot — `Sys_Error`'s `SDL_INIT_VIDEO`-dropped path, `zone.c`'s
allocator under real Xbox memory constraints, whether the log channel
reaches a debugger — has been measured yet). Matches `deps/nxdk/
samples/hello/main.c` almost exactly on purpose: an infinite loop
calling `debugPrint`, nothing more, touching no engine subsystem at
all. Resolving `_main` drops the tracked undefined-symbol count from
**64 to exactly 63** — verified symbol-for-symbol, not just by count:
the 63 remaining are exactly the parked networking group, `_main` gone
entirely.

**A linker cannot emit a loadable binary with unresolved symbols** —
found attempting the actual `.xbe` conversion: `nxdk-link` with
`-errorlimit:0` reports every undefined symbol but produces no `.exe`
output at all when any remain, `-errorlimit:0` only means "don't stop
after N errors," not "produce output anyway." The 63 parked networking
symbols are real, deliberate, tracked debt — not something this phase
should implement to get an `.exe` out. `lld`'s `-flavor link` mode
(what `nxdk-link` wraps) genuinely supports MSVC `link.exe`'s real
`/force:unresolved` flag (verified directly: `lld -flavor link /?`
lists it, not guessed from documentation), which does exactly what
"parked, not forgotten" already means at link time — a forced-
unresolved import resolves to address `0`, so anything that ever
actually called one of these 63 symbols would crash loudly, not
silently succeed, matching this project's own "fail honestly" posture
elsewhere (`compat/xbox`'s `chdir`/`getcwd` stubs, `stub_game`'s own
honesty about implementing nothing).

`tools/xbox-link-check.sh` gained a new `--xbe <path>` flag: after a
successful `--expect-undefined` match (never on a mismatch or an
unexplained failure), it runs a **separate** forced-unresolved link —
deliberately not a change to the measurement link's own flags, since
that link's error-vs-warning accounting depends on undefined symbols
staying real link *errors*, and `/force:unresolved` turns them into
warnings instead — then converts the result with `deps/nxdk/tools/
cxbe/cxbe`, and verifies the output genuinely starts with the `XBEH`
magic header rather than trusting `cxbe`'s exit code alone.

**Corrected 2026-09-09, found in review: `--expect-undefined` only
compares a COUNT, not which symbols they are.** A same-count swap —
one parked networking symbol accidentally getting defined somewhere
while an entirely different, genuinely new symbol goes undefined for
an unrelated reason — would have passed that count check and then been
silently forced into a real `.xbe`, hiding a real regression (a new
address-0 landmine outside the deliberately-parked group) behind a
"matches --expect-undefined" message that only ever checked the wrong
thing. Fixed with `docs/r3d/parked-undefined-symbols.txt` (new) — the
real, checked-in reference: every undefined symbol name, not just how
many. Verified both ways: the real, correct 63-symbol set still
builds; and a deliberately simulated same-count swap (one real symbol
name replaced with a fake one in a scratch copy of the reference file)
is caught and refused, with no `.xbe` produced.

**Corrected 2026-09-09, found in a second review pass: the identity
check above only ran inside the `--xbe` branch.** The script's own
documented, ordinary invocation — `--expect-undefined N` alone, no
`--xbe`, exactly the form its own usage comment shows — still only
ever compared a COUNT, the exact false-clean class this check exists
to close; the same-count swap only got caught when also requesting an
XBE. Moved the identity check to run for every `UNDEF > 0` result,
unconditional on `--xbe` — every successful `--expect-undefined` match
is now symbol-checked, not just the ones that also happen to build an
XBE. Re-verified both ways, this time without `--xbe` at all: the real
63-symbol set still passes plainly, and the same simulated swap is
caught and refused with no `--xbe` flag present.

**Verified end to end, not just by exit code**: `tools/xbox-link-check.sh
--expect-undefined 63 --xbe .sweep/resonance3d.xbe` produces a
4,198,400-byte file starting with `5842 4548` (`XBEH`) — a real,
structurally valid Original Xbox executable, built from the full
engine + third-party audio + menu module + stub game module object
set this entire phase has been proving compiles and links. Also
verified the failure path: a deliberately wrong `--expect-undefined`
count correctly refuses to attempt the XBE build at all, rather than
silently producing one over an unexpected object set.

**X1's own exit line, in full: "engine, filesystem, menu and a stub
game module link into one XBE."** All five nouns are now literally
true of this tree. Not a bootable, playable game — `main()` never
calls into the real engine, `stub_game`'s server module would crash
the instant any real gameplay function were invoked, and the 63 parked
networking symbols are load-time landmines for the day networking
stops being deliberately deferred — but a real `.xbe`, produced by
this project's own real toolchain, from this project's own real
source tree, genuinely exists for the first time.

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

- `filesystem/mpak.c` as a `searchpath_t` (7 pointers, modelled on `pak.c`).
  Done (#17). The original "loading per resource through `pfnLoadFile`"
  wording predates the scope narrowing below and no longer applies — one
  archive is one map, served whole.
- Read from the Xbox HDD or a mounted ISO.
- **[from PS3]** Headless mode: boot, mount, load a map's data, run server
  frames, no video at all. Asset-path bugs found here are found without a
  renderer in the picture.

**`.mpak` scope, narrowed.** `.mpak` is limited to BSP geometry, level
(WAD-resolved) textures, and lightmaps — the genuinely sequential-I/O-shaped
data a monolithic per-map archive earns its keep for on the Xbox's slow IDE
bus. Models, sprites and sounds are ordinary, already-self-contained GoldSrc
files upstream's own loaders already read directly from disk; `tools/asset-studio`
no longer embeds them, and instead writes their names to a sibling
`<mapname>.assets.txt` manifest (plus a shared `global.assets.txt` for
engine-wide assets like the menu/HUD sprites). The manifest is a build-time
precache list only — it carries no memory-management responsibility of its
own. Cross-level dedup/eviction is already solved by the reference engine's
existing mark-and-sweep (`Mod_PurgeStudioCache` / `Mod_FreeUnused` in
`engine/common/model.c`, `S_EndRegistration`'s generation-counter equivalent
in `engine/client/sound/s_load.c`) and does not need to be reinvented here.
What the manifest+mpak split does NOT yet solve — a single combined budget
across the CPU heap and the physically separate, contiguous GPU texture
pool — remains X3's job (see "One ledger, two allocators" below).
`filesystem/mpak.c` now exists as a real `searchpath_t` (modelled on
`pak.c`, registered in `searchpath.c`'s `g_archives[]` table): one archive
= one map, served under the single virtual name `"maps/<basename>.bsp"`
its own precache name always uses (`sv_init.c:1025`). Verified both on PC
(a real `filesystem/tests/mpak.c` unit test, built and run via `waf
--dedicated --enable-tests`) and on Xbox (`xbox-sweep.sh` +
`xbox-link-check.sh --xbe`, 0 duplicate/63 undefined unchanged, a real
`.xbe` still builds). This is filesystem plumbing only - it proves an
`.mpak` archive is discovered and its bytes resolve through the VFS, not
that the engine can parse them. Teaching `Mod_LoadModel`'s format-dispatch
switch to recognize MPAK's magic and parse `MpakHeader6`'s lump layout
into runtime `mnode_t`/`mleaf_t`/etc. structures - the actual brush-model
port - remains unwritten and is real, separate, larger work.

`filesystem/mpak_layout.h`/`.c` (new) now solve the hard, easy-to-get-wrong
part of that remaining work: computing every lump's real byte offset and
length. `MpakHeader6` has no offset table, and - verified directly against
the real writer, `tools/asset-studio/src/formats/PakManager.cpp`'s
`save()`, not assumed from the header comment alone - the actual layout
isn't simply "walk the header's fields in declared order": every section
is independently padded to a 2048-byte sector boundary, the vertex/index
geometry block has no length field of its own at all, and the texture
block is target-dependent (Xbox writes one DXT atlas; PC writes three
separate DXT arrays). Getting any of that wrong produces silent, wrong
offsets, not a compile or link error - exactly the failure class this
project's practices exist to catch before it ships. Verified two ways: a
real unit test (`filesystem/tests/mpak_layout.c`, hand-computes every
expected offset independently rather than reusing the code under test,
covering both target layouts plus six real corruption/rejection cases);
and a real bake from the actual retail install, both targets, where the
computed end-of-archive offset landed EXACTLY on the real file size
(9256960 and 8767488 bytes) and the computed entities-lump offset
resolved to genuinely readable GoldSrc entity text.

Does not parse any lump's contents. `dbspmodel_t` and the
`Mod_LoadPlanes`/`Mod_LoadNodes`/etc. functions that would actually
consume these offsets are `static`, file-local to
`engine/common/mod_bmodel.c` (confirmed by grep - nothing else in the
tree references `dbspmodel_t`), so building runtime BSP structures from
this layout has to live inside that file, not `filesystem/`. That's the
next real step here, and it can now build on layout math already proven
correct against real data instead of re-deriving it inline.

**Loading the map: what the audit found.** `docs/r3d/mpak-loading-design.md`
(2026-09-09) read the real loaders and every real consumer before any
loader was written, and three things change the shape of the remaining
work:

1. There is no "structural half first, render half later". The reference
   pipeline (`mod_bmodel.c:4229-4247`) is one ordered chain:
   `Mod_LoadMarkSurfaces` indexes `mod->surfaces`, `Mod_LoadLeafs` indexes
   marksurfaces, `Mod_LoadNodes` needs leafs, `Mod_MakeHull0` needs nodes —
   `Mod_LoadSurfaces` has to run first, and it needs vertexes, edges,
   surfedges, textures and texinfo to do so.
2. Every consumer before X9 needs exactly what the v6 bake threw away.
   `ref/soft` scan-converts from `surfedges`/`edges16` **every frame**
   (`r_rast.c:444`, `r_bsp.c:386`); the server (`sv_world.c:1546-1571`),
   `ref/soft` (`r_surf.c:174-236`) and `ref/gl` (`gl_rsurf.c:725`) all
   rebuild lighting per lightstyle and per dynamic light from per-face
   `samples`; `pm_trace.c:835`, `sv_world.c:1436`, `Mod_GetFaceContents`
   and animated-texture sequencing need texture **names**. The v6 atlas is
   lightstyle-0-only and 7× the lighting lump it replaced (4.19 MB vs
   599 KB on `c1a3`); v6 stores no names or texture dimensions at all.
3. Therefore the right engine change is not a rewrite of six loaders — it
   is no engine change at all. Give the archive a **classic core** — raw
   `VERTEXES`/`EDGES`/`SURFEDGES`/`FACES`/`LIGHTING` plus an embedded-
   miptex `TEXTURES` lump resolved from the WADs at bake time — and make
   the archive **a BSP30 file**: a real `dheader_t` at offset 0 whose
   lump table points at the sector-aligned raw lumps, the MPAK descriptor
   at a fixed offset inside the first sector. `Mod_LoadLump` only reads
   lump extents from that header and stores pointers, never copies
   (`:769`, `:925-928`); the six engine paths that validate a map from
   disk before any loader runs (`SV_ValidateMap` → `Mod_TestBmodelLumps`,
   `sv_game.c:823-853`, `mod_bmodel.c:4323-4398`; the lump utilities;
   the map-list check — found in review of the design's first draft,
   which had synthesised the header in memory and would have been refused
   by `+map` before the loader ever ran) read exactly that header. So the
   **unmodified** `Mod_LoadBmodelLumps` loads it, and renderers already
   receive the raw buffer through `Mod_ProcessRenderData` (`model.c:381`)
   to find the optional baked block. The baked `Vertex`/index/
   `FaceDrawCmd`/`MpakFaceInfo`/DXT/atlas set becomes that X9-only block.
   Honest consequence: on the X5 path an `.mpak` is "the BSP with its WAD
   textures inlined, sector-aligned, one file per map, plus the manifest"
   — still the I/O and precache win this phase wanted, just not a new
   geometry encoding.

**Work packages** (details, exit criteria and the field-by-field
consumption contract for `msurface_t`/`mextrasurf_t`/`glpoly2_t` are in
the design doc, §2 and §10):

- **W1 — MPAK v7: BSP30 first sector, core lumps, profiles** — **done**
  (`tools/asset-studio`, `filesystem/mpak.c` probe, `filesystem/mpak_layout.*`
  + tests). A real `dheader_t` at offset 0, the descriptor at 128, six
  appended lumps, a `core|nv2a` profile flag, embedded-miptex textures
  with real names; `c1a3` core bake is 2,994,176 bytes (was 8.8 MB);
  `--verify` byte-compares every classic lump against its source and was
  proven against three single-byte negative controls; layout test at 18
  cases including the `dheader_t` cross-check; real-bake end-offset
  check exact for all three variants. See CHANGELOG.
- **W2 — Engine: nothing**, kept as a checkpoint — **measured, by W3**:
  `c1a3.mpak` from a directory whose only map source is the archive goes
  through `Mod_LoadModel`'s version switch and the unmodified
  `Mod_LoadBmodelLumps` into the same runtime state as the `.bsp`, with no
  engine diff on the load path.
- **W3 — Differential verification tool** — **done**: `test_dumpworld`
  (`engine/common/mod_bmodel.c`, test builds, divergence #27) dumps the
  runtime `model_t` — every array, hull, per-leaf PVS CRC, per-surface
  extents/bevels/lighting CRC, per-texture name/dims/animation links —
  and `tools/mpak-diff.sh` compares the `.bsp` and `.mpak` dumps
  byte-for-byte with a no-archive control. `c1a3` and `crossfire`
  identical (5,897,085 / 5,143,563 bytes of state, every leaf of the
  lump); a flipped lighting byte and flipped leaf bytes in the archive
  are caught. The `xash_tests_dedicated` link gap
  (`_Platform_Minimize_f`) is fixed (divergence #26); the stock engine
  tests run on this configuration for the first time (1011 pass). See
  CHANGELOG.
- **W4 — Headless run** — **done, the X2 exit line on PC**: `tools/mpak-run.sh`
  starts a dedicated server with `+map <name>` from a directory holding
  the `.mpak`, the boot floor W3 measured (`gfx.wad`, `dlls/hl.dll`,
  `delta.lst`) and the manifest's loose files, no `.bsp` anywhere in its
  search path; `test_mapprobe` (`engine/server/sv_world.c`, test builds,
  divergence #28) then exercises `SV_PointContents`, a player-hull
  `SV_Move`, `SV_TraceTexture`, `SV_TraceSurface` and
  `SV_RecursiveLightPoint` at the spawn point. `c1a3`: 34 server frames,
  spawn over `elev2_flr`, light 33,33,32; `crossfire`: over
  `lab1_helpad`, light 198,194,177; without the archive `map` refuses.
  Finding: the baker's manifest sees only entity keyvalues (12 files for
  `c1a3`); the game library precaches 91 more models from code (263
  models, 282 sounds in all). The server runs without them; the probe
  writes the server's precache tables (`<map>.precache.txt`) — a
  measured but partial inventory (external sequence groups, client-only
  texture models and generic resources live elsewhere), so deployment
  still has to expand dependencies from it. See CHANGELOG.
- **W5 — Xbox `Host_Main`** — **linked, not yet run**: a real, dedicated
  Xbox target (`tools/xbox-dedicated-sweep.sh`/`-link-check.sh`, divergence
  #29) compiles the actual `engine/wscript` "dedicated only" file list
  (verified against this repo's own `compile_commands.json` from a real
  PC `--dedicated` build) with `platform/xbox/*.c` in place of
  `platform/win32/*.c`, and links 0 duplicate / 0 undefined through
  `engine/common/launcher.c`'s real `main()` → `Host_Main("valve", ...)` —
  the same static-link path PSVita/iOS use. Produces a real, `XBEH`-valid
  XBE (2,273,280 bytes). The 63 parked networking symbols turned out to be
  an incomplete `platform/stub/net_stub.h` (predates `net_ws.c`/`xrcon.c`
  gaining IPv6/`getaddrinfo`), not a real BSD-sockets gap — completing it
  let `net_ws.c`/`xrcon.c`/both HTTP files compile unmodified, and also
  improved the existing broad-sweep baseline (197/30 compile, 0/32 link,
  both down from 34/63). **Codex review, round 1, found a real one: this
  row first claimed `compat/xbox`'s `getcwd` already resolved X0.5 -
  false, its real body is an honest `errno = ENOSYS`.** `FS_Determine
  RootDirectory` now has a real `XASH_XBOX` branch returning `"D:\\"`
  (guaranteed by the already-linked `automount_d_drive` CRT hook, not
  guessed), which fixes the specific `Sys_Error` Codex found - but **does
  not close X0.5 below**, whose own "not sufficient" finding (relative
  searchpaths, no nxdk current-directory concept) this fix does not touch.
  **Still open**: X0.5's own two routes, and running any of this - no
  xemu/hardware in this environment. See CHANGELOG.

W1 → W2 → W3 are ordered; W3's tool can be written against `.bsp` alone
in parallel with W2. Decisions taken by recommendation in the design
doc's §9 (atlas out of every profile until X9; DXT and the baked
geometry X9-only; no texture manifest) stand unless overridden.

**Exit:** the engine loads a map from `.mpak` and runs the server loop with
`ref/null` — **met on PC (dedicated) by W4**; on Xbox it waits on W5.

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

**Route decided, 2026-09-11 — reuses nxdk's SDL2 port, not `vid_xbox.c`.**
X5's own plan below (written earlier) says "do not route this through
SDL's window surface" for the real shipping path's sake - nxdk's SDL2
software-2D layer costs a 1.2 MB shadow surface and a per-frame
`SDL_ConvertPixels`. That is still the right call for the FINAL path, but
the user chose the faster route for FIRST getting something on screen at
all: link against nxdk's existing, already-working SDL2 video driver
(`XASH_SDL=2`, already unconditional in `tools/xbox-cc.sh` for other
reasons) rather than writing `vid_xbox.c` first. `vid_xbox.c` remains the
real, planned final path below - this is a deliberately temporary
detour, not a replacement of the plan.

**Prerequisite found and fixed: `VID_Init()` had no renderer to load at
all.** `R_Init()` (`engine/client/dll_int/ref_common.c`) only auto-tries
renderer names gated by a compile-time `XASH_REF_*_ENABLED` define - none
exist for Xbox (no `ref/soft` or `ref/gl` port written yet) - and with
none available, `Sys_Error`s out before the client can even reach its
main loop. Wired up `ref/null` instead of jumping straight to porting
`ref/soft`: upstream's own real, complete `ref_interface_t` of no-op
stubs (`ref/null/r_context.c`'s own comment: "made to be able to run
full client in hazardous^Wheadless environments... might be a starting
point for new renderers as well"), registered in the same static
`COM_LoadLibrary` table menu/`filesystem_stdio` already use (a new
`ref_null:ref/null/exports.txt` argument to `tools/gen-library-tables.sh`,
threaded through `tools/xbox-cc.sh`/`tools/xbox-link-check.sh`), selected
at runtime via `r_refdll "null"` in a disc's `video.cfg` - upstream's own
designated renderer-choice mechanism, executed by `R_Init()` itself
before renderer selection runs, so no engine code changes were needed
for selection at all. This is real, useful groundwork on its own merits,
not a throwaway stub: it lets the REST of the client boot path (`SV_Init`,
`CL_Init`, menu init, the frame loop) be verified independently of the
harder rasterizer-porting problem, and the exact registration mechanism
a real `ref_soft`/`ref_sdl2` module will need later is now proven.

**Verified: compiles and links clean** - `ref/null/r_context.c` compiles
for Xbox with zero warnings (genuinely portable code, only
`xash3d_types.h`/`const.h`/`cvardef.h`/`xash3d_mathlib.h`/`ref_api.h`),
`GetRefAPI` is a real, defined symbol, and the full client link stays at
0 duplicate/0 undefined with `ref_null` correctly appearing in the
generated static table. **NOT verified at runtime this session**: boot
attempts against this build stopped producing ANY network-log-channel
output at all (not even messages from code paths well before renderer
selection, that had printed successfully in earlier boots this same
session) - self-tested the listener mechanism directly (a manual UDP
packet was received correctly), ruling out the host-side tooling, but
left genuinely unresolved whether this is the network log channel's own
documented DHCP-negotiation unreliability (`net_xbox_log.c`'s own
comment: up to ~10s, not guaranteed to succeed) recurring nondeterministically,
or a real, different problem. Documented honestly rather than claimed -
see `docs/r3d/lessons.md` for the specific reproduction attempted.

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
carrying as opinions.

**MEASURED, 2026-09-10** (`tools/asset-studio/tools/census_lightstyles.cpp`,
`census_bones.cpp` — new, linked against the same `AssetStudioFormats` the
real baker uses, run against the real Steam "Half-Life" install):

- **Lightstyles.** How many faces per map carry `styles[1..3] != 255`. The
  baker bakes style 0 only (`BspParser.cpp:514-523`), so "RGB565 pages, no
  runtime conversion" currently means no light in the game switches or
  flickers on Xbox. **Not a negligible edge case**: across all 125 retail
  maps, 643,971 faces total, 613,358 lit — **41,031 of those lit faces
  (6.69%) carry at least one extra lightstyle, and 82 of the 125 maps (66%)
  have at least one such face**. The distribution is heavily skewed, not
  uniform: `rocket_frenzy` alone carries 6,223 of the 41,031 (a Half-Life
  add-on map, almost entirely dynamic lighting by design), then a real
  drop to `disposal` (2,759), `contamination` (2,194), `c0a0c` (1,399,
  the game's own opening map — a player would notice this in the first
  five minutes) and a long tail down to zero. **Corrected in review,
  round 1: the first version of this row named `c1a2c`/`c1a2b`/`c0a0e`
  (46/43/21 faces) as "the worst maps" — those are real rows from the
  census output, but nowhere near the top of a table the census itself
  sorts descending; a `tail -60` on 125 sorted rows only ever showed the
  bottom of it. `tools/asset-studio/tools/census_lightstyles.cpp` itself
  needed no fix — the tool's own output was correct throughout; this was
  a review-of-the-output mistake, worth recording as its own lesson.**
  An accepted loss would still be a visible regression on two-thirds of
  the game's maps and its own opening level, not a rare corner case —
  that conclusion holds unchanged. Per-style pages combined at runtime,
  or a baked *on* state with binary switching, are both live options; the
  census does not pick between them, only closes off the third.
- **Bones.** Which retail models reference more bones per DRAW than a VP20
  palette holds — 192 constants (`deps/nxdk/tools/vp20compiler/
  config.h:11`), three per bone, roughly 50 after the view-projection and
  light constants (an ESTIMATE, not yet a verified exact cutoff — see
  below), against `MAXSTUDIOBONES 128`. GoldSrc rigs each vertex to
  exactly one bone via `vertinfoindex` (`deps/hlsdk/engine/studio.h`), not
  a weighted multi-bone skin. **Two real mistakes, both found in Codex
  review, both fixed — the second reverses the conclusion entirely.**
  Round 1: the first version measured distinct bones over a whole
  submodel (`mstudiomodel_t`), calling that the "real per-draw bone set"
  — wrong, the reference (`ref/gl/gl_studio.c`'s `R_StudioDrawPoints`,
  `:2078-2112`) submits each `mstudiomesh_t` separately, and a mesh's own
  triangle command stream only touches a subset of the submodel's
  vertices. Fixed to walk each mesh's real triangle command stream (int16
  run-length headers, four-int16 `{vertindex, normindex, s, t}` entries,
  confirmed against `R_StudioBuildArrayNormalMesh`, `gl_studio.c:
  1749-1777`) for exact per-mesh vertex-bones — but round 1's fix still
  added every normal-bone in the WHOLE submodel to every mesh as a
  declared, deliberate over-approximation, unable to confirm from reading
  `gl_studio.c` alone whether a triangle entry's own `normindex` was
  mesh-local or already submodel-global. Round 2 resolved that for real:
  `R_StudioSetColorArray` (`gl_studio.c:1431-1436`) reads
  `pstudionorms[ptricmds[1]]` and `g_studio.lightvalues[ptricmds[1]]`, the
  latter populated by a running index accumulated across every mesh in
  submodel order — a compiled MDL's own per-entry `normindex` already
  addresses the submodel's normal array directly, exactly parallel to
  `vertindex`. Fixed to attribute normal-bones per mesh exactly too, no
  over-approximation. **The corrected, exact measurement finds ZERO
  submodels over the provisional budget** — the previous "3 models, all
  bosses" was entirely an artifact of adding an unrelated mesh's
  normal-bones into the count; the worst real case across all 598
  submodels in 296 studio models is `v_grenade.mdl`'s `grenade_hand` mesh
  at 35 bones, 15 under the ~50 estimate. **This clears the retail
  content against that estimate, but does NOT close the decision** —
  found in a second review pass: the ~50-bone number is itself still an
  unverified estimate (`config.h`'s 192 constants minus a guessed
  view-projection-and-light reservation), not a measured hardware
  palette, so 35 being under 50 is headroom against a guess, not proof
  against the real budget. **Gate, matching the format-decision list
  below: do not implement bone-subset partitioning code unless a
  compiled VP20 shader's real constant layout puts the usable palette
  below 35** — re-check for free with `census_bones --budget N` the
  moment that real number exists. Until then this is "no partitioning
  work is scheduled," not "no partitioning work will ever be needed."

Then the format, decided once:

- **Vertex layout** the NV2A reads natively: FLOAT, normalised SHORT,
  D3DCOLOR. No integer attributes. `base_vertex`-chunked u16 indices — the
  decision the Rust port already made, carried over rather than re-derived.
- **Draw lists per cluster, per material**, so runtime submission is a walk in
  baked order. Per leaf alone explodes the draw count, and on pbkit every
  submission is CPU time writing pushbuffer words into write-combined memory.
- **Lightmap pages in RGB565**, with whatever the census decided. **Data
  side done, 2026-09-10 (MPAK v8).** The census ruled out dropping extra
  lightstyles (66% of maps affected) and the user picked the
  reference-matching option: bake every style a face uses, not just
  style 0, and blend them at runtime the way `R_BuildLightMap` does.
  `BspParser.cpp` now bakes a block per extra style, `MpakExtraLightBlock`
  (a new sparse v8 section) records where each one landed - verified
  against a real bake of `crossfire.bsp`: 127 blocks, matching the
  census's own independently-computed count for that map exactly, plus a
  real negative control (a corrupted byte in the new section is caught
  and named specifically). **Not done**: the RGB565 conversion itself
  (still RGBA8 in the atlas, matching every other lightmap page) and the
  runtime blend - that consumer is `ref/nv2a`, which doesn't exist until
  X9; this phase's own scope is baker-only (see this section's own
  opening line).

  **Xbox memory budget: not enforced here, found in review (twice),
  deliberately deferred to X3.** Extra-style blocks grow the SAME
  atlas-packer (`mPages.emplace_back()`) style 0 always has, with no
  bake-time budget check - but that check does not exist for ANY baked
  data yet (not geometry, not textures, not the pre-existing lightmap
  atlas either); X3's own exit line is exactly this ("a declared budget,
  enforced at bake time"), and implementing it here, for lightmaps
  alone, ahead of X3's real design, would be scope creep this PR does
  not need.

  Round 1's review accepted a 2-map sample (`crossfire.bsp`, `c0a0c.bsp`)
  as evidence. Round 2 correctly rejected that as insufficient: atlas
  consumption depends on block *size*, not just extra-style *count*, so
  a lower-ranked map with larger blocks could in principle still land on
  more pages than a higher-ranked one. The fix is exhaustive measurement,
  not a bigger sample: every one of the 125 retail maps was baked for
  the Xbox target and its atlas page count recorded. Result -
  **110 maps land on 1 page (4 MB), 11 on 2 pages (8 MB), 0 on more than
  2** (`boot_camp`, `c2a2a`, `c2a2e`, `c2a4c`, `c2a4e`, `c3a2a`, `c5a1`,
  `rustmill`, `t0a0`, `t0a0a`, `t0a0b`). The remaining 4 maps
  (`contamination`, `disposal`, `pool_party`, `rocket_frenzy` - including
  both maps round 1 flagged as highest-risk by census count) do not
  reach this code at all: all four already fail bake with `FATAL: vertex
  count exceeded 65536`, a separate, pre-existing gap (the "Vertex
  layout" bullet above, `base_vertex`-chunked indices, not yet
  implemented) that blocks them regardless of lightstyles. This is the
  entire bakeable retail map population, not a sample of it: today, no
  map that can bake for Xbox exceeds 8 MB of atlas, and this change
  cannot be the reason an Xbox archive exceeds budget.
- **Bone partitioning** for the models the census named. **Provisionally
  not needed under the ~50-bone estimate, measured 2026-09-10 — NOT yet a
  closed decision.** The census (above) found a worst-case peak of 35
  bones across every submodel in the entire retail install, comfortably
  under the ~50 estimate. But that estimate is explicitly unverified (see
  above: "an ESTIMATE pending a real VP20 shader constant layout, not a
  verified exact cutoff") — **found in review**: closing this line
  outright would have removed partitioning from the format before the
  thing the 50-bone number depends on exists, and the real shader could
  plausibly reserve enough beyond the naive view-projection-and-light
  guess to bring the usable palette under 35. **Gate: do not implement
  bone-subset partitioning code unless a compiled VP20 shader's real
  constant layout puts the usable palette below 35** (re-run
  `census_bones --budget N` with that real number the moment it exists —
  it is a CLI argument specifically so this recheck costs nothing). Until
  then, treat this as "no partitioning work is scheduled," not "no
  partitioning work will ever be needed."
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
