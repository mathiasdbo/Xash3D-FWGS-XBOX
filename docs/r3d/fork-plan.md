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
the instant any real gameplay function were invoked (and, found
2026-09-11 once a client boot actually reached it: also the instant the
ENGINE's own fatal-error path invoked `pfnSys_Error` — see X5's
"RESOLVED" note and divergence #34), and the 63 parked
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

**First real measurement and first lever, 2026-09-14 (divergence #51).**
Where a map's memory actually goes on the console, read from the kernel's
own page counters (`tools/xemu-debug.py memstats`) and the engine's own
pool table (`tools/xemu-debug.py pools`, `zone.c`'s `poolchain`), on a
`verify-image`-confirmed guest, `c0a0` under `ref_soft` at 640x480:

- **The lever that made the map fit at all**: `XASH_LOW_MEMORY=1`
  (upstream's own, MotoMAGX's level; protocol-compatible). Image 27.1 →
  16.1 MiB; the PC-sized statics collapse (`cl` 3.4 → 2.1 MiB, `sv` 2.4
  → 0.7, `net` 1.6 → 0.3, `mod_known` 1.5 → 0.4, `ref_soft`'s `r_images`
  2.4 → 0.3, `g_studio` 1.8 → 0.2). Without it the map died in
  `CL_InitEdicts` with 0.97 MiB free; with it, 12.57 MiB free with `c0a0`
  up and 11.63 after the tram to `c0a0a`.
- **The 64 MiB, `c0a0` up**: Image 16.1 (code 6.3, statics 10.5 - of
  which `vid`'s rasterizer tables 2.5, `cl` 2.1, `sv` 0.8, two `gpMove`s
  0.6, `s_knownSfx` 0.5), heap (VirtualMemory) 30.2, stacks 3.0,
  contiguous 2.0 (the 1.2 MiB framebuffer among it), kernel and page
  tables ~0.3, free 12.6.
- **The heap, by pool (26.0 of the 30.2 MiB)**: `ref_soft zone` 12.1
  (~2.9 surface cache at 640x480 by `r_surf.c:778`'s formula; the rest
  the map's textures as 16-bit texels x 4 mips + alpha copies, ~2.7x the
  BSP's 8-bit miptex), `maps/c0a0.bsp` 3.3, `Client Edicts Zone` 2.7
  (`max_edicts 900` x ~2.9 KB `cl_entity_t`, mostly its 64-entry position
  history), `SoundLib Pool` 2.6 (precached sounds as PCM), `Client Static
  Pool` 1.8, `Server Edicts Zone` 0.7, `Zone Engine` 0.6, studio models
  ~1.3. Outside the pools, ~4 MiB: SDL's 1.2 MiB window surface, the CRT
  heap, freevgui.

**Where the next MiB are, in order of size over risk** (analysis, not
done): (1) `ref_soft`'s texture store - 16-bit texels are the
rasterizer's design, but mip 3 (1/64) and the alpha copies are choices,
and a 320x240 render target halves the surface cache; the honest number
needs a per-texture walk of `r_images`, which the pool table does not
give. (2) The two allocators the plan already names: 1.2 MiB of SDL
shadow surface plus a per-frame `SDL_ConvertPixels` that `vid_xbox.c`
deletes outright. (3) `vid`'s 2.5 MiB of fixed tables - `colormap` 512
KiB, `alphamap` 1.5 MiB - are `ref_soft`'s lighting/blending model; only
a different blend path shrinks them. (4) Stacks: 3.0 MiB is three 1 MiB
threads because divergence #47 raised the DEFAULT; a per-thread size for
the secondary threads is the ~2 MiB it deferred. (5) `SoundLib Pool`:
2.6 MiB of PCM for a map with no audio output yet (X8) - real cost
later, but the format (8-bit vs 16-bit, resample-on-load) is X8's
choice. (6) `max_edicts` is per title, not per map: 900 is retail's
`liblist.gam`, and later maps need it, so the edict zone is a fixed
cost. (7) `XASH_LOW_MEMORY=2` would take `cl` down further
(`MULTIPLAYER_BACKUP` 64 → 4) at the cost of the protocol; not needed
yet. (8) The asset-side items above (RGB565 lightmaps, `.mpak`) shrink
the 3.3 MiB BSP pool and the 8-bit texture source, not the renderer's
copies. The frame-time half of X5's exit is measured next, before any of
these is spent.

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
generated static table. **NOT verified at runtime that session**: boot
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

**RESOLVED, 2026-09-11 (divergence #34): it was a real, different
problem, and the log channel was never at fault.** Read straight off the
halted CPU with `tools/xemu-debug.py` (xemu's built-in GDB stub, no
screenshots, no guessing): the boot DID get past renderer selection -
`ref/null` loaded, `SV_Init` ran into `SV_LoadProgs` - and there the stub
game module (divergence #22) called through NULL: `SV_InitClientMove`
(`sv_game.c:5333` → `sv_pmove.c:449`) asks the module for
`pfnGetHullBounds` (then `pfnPM_Init`), members the stub's own "only these
three are called synchronously" audit had missed by reading `SV_LoadProgs`
and stopping one call short (the trap frame's pushed return address,
`SV_InitClientMove+0x88`, is a `call eax` with `eax = 0`). With those
provided, the boot got further still: `CL_Init` ran, and
`CL_LoadProgs("client")` failed, correctly, because no client module
exists at this phase (`lib_client_exports` is an empty table in `.bss`;
X7's job, exactly as the plan says for the server side). `Host_Error`
during init routes to `Sys_Error`, and `Sys_Error` calls
`SV_SysError` → `svgame.dllFuncs.pfnSys_Error()` BEFORE it ever calls
`Sys_Print` - the third NULL member - so the first fatal error of the boot
called address 0 too: kernel bugcheck (`KMODE_EXCEPTION_NOT_HANDLED`,
`STATUS_ACCESS_VIOLATION`, `ExceptionAddress = 0x00000000`, all read from
the faulting thread's stack), then `cli; hlt`. The message
"Host_ErrorInit: can't initialize client: missing essential exports" was
sitting fully formatted on that stack; it never reached any log tier
because the NULL call came first. Fixed at the source (the stub now
provides all three, each matching the HL SDK's own behaviour, cited in
`server_stub.c`) and guarded in `SV_SysError` under `#if XASH_XBOX` so
the same class of miss on the error path can never again turn a one-line
error into a silent freeze. **Verified on the fixed build, with the
network log channel as the independent signal (divergence #13, the very
channel the earlier "went silent" note distrusted):** the first
`Sys_Error` this port has ever reported arrived - `Delta_InitFields:
couldn't load file delta.lst`, once per boot cycle - with the CPU in the
kernel's idle-wait, not halted; the pre-fix builds under the same
listener gave nothing and the bugcheck registers. **What this means for
X5's status: the client-main boot now fails loudly and honestly, and the
current stop is a content gap, not code** - `Delta_Init` (the call right
after `SV_InitClientMove` in `SV_LoadProgs`) needs `valve/delta.lst`,
which the minimal bring-up disc (`gameinfo.txt`, `gfx.wad`, `video.cfg`)
does not carry. With the retail `valve/` tree the next stop is `CL_Init`'s
client-module load (its message was read off the pre-fix build's stack,
but a fixed build has not reached it yet), which cannot pass until a
client module exists. Per the per-title direction set on 2026-09-11
(each title its own statically-linked XBE/XISO, as the retail discs
were), that is the first real Half-Life build - `deps/hlsdk`'s `dlls/` +
`cl_dll/` statically linked - not a generic client stub; see X7. The
earlier claim that `ref/null` "lets the rest of the client boot path
(`SV_Init`, `CL_Init`, menu init, the frame loop) be verified" was too
strong: `SV_Init` yes, `CL_Init` only up to its client-module load.

**The decided route, built and measured, 2026-09-14 (divergence #50):
the first frame this fork has ever put on the Xbox's own scan-out is
Half-Life's loading screen, drawn by `ref_soft` at 640x480.** In order:
`ref/soft` + `ref/common` compile clean for Xbox once `tools/xbox-cc.sh`
gives `ref/` sources `REF_DLL=1` and their own include directories ahead
of the engine's (the 20/20 failure under the plain sweep was the
engine's `engine/client/ref_common.h` shadowing `ref/common/ref_common.h`,
not a platform gap); linked raw they collide with the engine and
`ref_null` on 20 symbols, so `tools/xbox-ref-isolate.sh` renames every
defined symbol `soft_*` the way the SDK is isolated, `ref/soft/
exports.txt` names `_soft_GetRefAPI` for the table's `GetRefAPI` row,
and the build is 0 duplicate / 0 undefined at 6.76 MB. Divergence #46's
three bypasses are gone: nxdk's driver only asserted because no video
mode had been set, so `R_Init_Video`'s Xbox arm calls `XVideoSetMode(
640, 480, 32 )` and the common path runs unchanged - one window, forced
to the framebuffer's size, `SW_CreateBuffer` handed a 640x480 RGB888
surface, `SDL_UpdateWindowSurface` copying into nxdk's real framebuffer.
Two real things stood between that and a picture, both found live:
`VID_GetWindowSizeInPixels` calls `SDL_GetRendererOutputSize( NULL )` in
software mode (an SDL_assert under nxdk's `SDL_ASSERT_LEVEL 2`, whose
promptless fallback aborts - the boot rebooted every 2.9 s with no
`Sys_Error`; guarded, a genuine upstream latent bug worth sending up),
and, because that is the second silent SDL-assert failure this port has
paid a session for, `Xbox_Init` now installs an assertion handler that
turns any of them into a `Sys_Error` naming condition/file/line. The
frame was captured while halted on the return of the second
`SDL_XBOX_UpdateWindowFramebuffer`, read physically from PCRTC START
(`.sweep/x5-shots/halted-frame2.png`, kept out of the repo: it is
Valve's art). **Then `c0a0` runs the console out of memory** -
`_Mem_Alloc` in `CL_InitEdicts` (`cl_game.c:1002`) at 8.6 s with 249
pages (0.97 MiB) free: VirtualMemory 30.8 MiB and climbing, Image 27.1
MiB (19.1 with `ref_null`: `ref_soft`'s `vid` tables 2.5 MiB,
`r_images` 2.4, `g_studio` 1.8, `r_leafkeys` 0.5, `gDecalPool` 0.2),
Stack 3.0, Contiguous 2.0. X7.4 step 4 had 17.4 MiB left after the same
map with `ref_null`; `ref_soft` needs ~7.6 MiB more image and ~8.5 MiB
more heap for it - over by a few MiB. **So X5's exit line ("a
recognisable map, in software", `c1a3`/`crossfire` frame times) is
blocked on X3/X8's memory work, and this is that work's first real
brief**: the engine-side PC-sized statics X8 already names (`cl` 3.4
MiB, `sv` 2.4, `net` 1.6, `mod_known` 1.5, `frame_ents` 0.7,
`s_knownSfx` 0.6) plus `ref_soft`'s own, against a load that peaks
above 31 MiB of heap. Every number above is from the kernel's own page
counters (`tools/xemu-debug.py memstats`), on a guest `verify-image`
confirmed byte-for-byte first. Two review findings closed on the way
(divergence #50): later mode changes are clamped to the driver's one
640x480 framebuffer at `R_ChangeDisplaySettings`, and the link check no
longer links any engine object the sweep's new manifest does not vouch
for - the class of stale-build false pass that adding
`XASH_REF_SOFT_ENABLED` would otherwise have invited.

**The map, on screen, 2026-09-14 (divergence #51).** With
`XASH_LOW_MEMORY=1` the OOM above is gone: `c0a0` spawns under `ref_soft`,
the client receives the world with 12.57 MiB free, and the tram changes
level to `c0a0a` with 11.63 MiB free. Captured off the GPU's scan-out
while halted, ~400 frames in: the inside of the Black Mesa tram car -
lit, textured, seats, rail, the intro credits overlaid - at 640x480 in
software (`.sweep/x5-shots/halted-map-c0a0-tram.png`, Valve's art, not
committed). **A recognisable Half-Life map, rendered in software: the
first half of this milestone's exit line.** The second half - frame
time on `c1a3` and `crossfire` at 640x480 and 320x240 from the X0
counters, X9's baseline - is the next item. X3's entry above has where
the memory went and where the next MiB are.

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

## X7 — Game code: one XBE per title

**Direction set 2026-09-11.** Half-Life, Opposing Force and Blue Shift each
ship as their own statically-linked XBE on their own XISO, exactly as the
retail Xbox discs were. This is not a preference, it falls out of the
platform: Xbox has no dynamic loading (`deps/nxdk/lib/winapi/libloaderapi.c`,
`LoadLibraryExA` returns `NULL`; `engine/platform/misc/lib_static.c`'s
`Lib_Find` is a compile-time table), so "one engine XBE that swaps game
DLLs" cannot exist. Each title = engine + that title's `server` + `client`
modules + its own generated export tables, linked once, plus its own
`gameinfo.txt`/assets on disc. It also decides the open question left by
divergence #34: the `CL_Init` stop ("can't initialize client: missing
essential exports") is resolved by the FIRST REAL TITLE BUILD, not by a
generic `client_stub` - a stub client would prove nothing a real one does
not, and every hour spent on it is an hour not spent on Half-Life.

### What is already true (measured, not assumed)

- All three SDKs are vendored: `deps/hlsdk` (this project's own
  `halflife-updated-xash3d` fork of SamVanheer's `halflife-updated`),
  `deps/hlsdk-opfor` (`halflife-op4-updated`), `deps/hlsdk-bshift`
  (`halflife-bs-updated`) - one author, one code shape, so what is learned
  on Half-Life transfers. Sizes: `dlls/` 103 / 135 / 103 `.cpp`, `cl_dll/`
  49 / 53 / 49, `pm_shared/` 3 each. The SDK-base question the X1 section
  left open ("`hlsdk-portable` compiles under nxdk, evidence not proof")
  is settled by what is vendored and already partly adapted; `hlsdk-portable`
  stays a reference for one thing only, its `USE_VGUI` switch (below).
- `deps/hlsdk`'s extra commits over upstream are Ferrum56 (Rust engine)
  work, ALL gated behind `_STATIC_ENGINE_LINK`: `STRING`/`MAKE_STRING`
  rerouted through `pfnSzFromIndex`/`ALLOC_STRING`, `ALERT`/`CLIENT_COMMAND`
  bridged through non-variadic `Ferrum_*` hooks, FileSystem bootstrap
  neutralised. **None of that applies here.** This engine is real Xash3D:
  `SV_AllocStringPool` sets `pStringBase` (`sv_game.c:3133`, 32-bit path),
  `pfnAlertMessage` is a real variadic C function, `pfnSzFromIndex` exists.
  With `_STATIC_ENGINE_LINK` UNDEFINED the fork is stock upstream plus one
  harmless `extern "C"` on the `pm_shared` entry points. Harmless, and
  also unnecessary - corrected by the Xbox port specialist's review of
  this plan: the engine never looks `PM_Init`/`PM_Move` up by name, it
  reaches them through `svgame.dllFuncs` (`sv_pmove.c:491`, `:956`), and
  the `op4`/`bshift` forks have no `extern "C"` there at all and need
  none. It stays only because removing it would be a gratuitous
  difference between the three `resonance3d` branches.
  The two non-gated changes are real fixes (`multiplay_gamerules.cpp` cast,
  CMake `TARGET_PDB_FILE` guard) and stay.
- The C++ toolchain works: `tools/xbox-cxx.sh` compiles real C++ for Xbox
  (X1 measured), `deps/nxdk/lib/libc++.lib` defines `new`/`delete`, and the
  menu module (76 `.cpp`) has linked clean before. cmake (VS 18) and
  `deps/nxdk/share/toolchain-nxdk.cmake` both exist; the fork's CMake is
  already "NXDK-proof" (`-march=pentium3`, explicit `WIN32`). Two viable
  compile routes, chosen below.
- Engine-vs-SDK export names do not collide, checked by grep: the SDK's
  `extern "C"` names are the four server API exports (`GetEntityAPI`,
  `GetEntityAPI2`, `GetNewDLLFunctions`, `GiveFnptrsToDll`), the 37 client
  exports `cl_game.c`'s `cdll_exports[]` requires (`Initialize`, `HUD_*`,
  `IN_*`, `V_CalcRefdef`, `CL_IsThirdPerson`, `CAM_Think`, `KB_Find`,
  `Demo_ReadBuffer`...), `pm_shared`'s `PM_*`, and every entity classname;
  the engine defines none of the first three in C, and its own `PM_*`
  (`pm_trace.c`) is disjoint from `pm_shared`'s. **That check is
  necessary and nowhere near sufficient - found in Codex review of this
  plan, 2026-09-11. The collision that actually matters is SERVER versus
  CLIENT inside one XBE.** `cl_dll`'s target compiles the SAME `dlls/*.cpp`
  weapon sources the server does (`deps/hlsdk/cl_dll/CMakeLists.txt:129-143`
  pulls `../dlls/crossbow.cpp` and its siblings), under `CLIENT_DLL`, with
  genuinely different bodies (`deps/hlsdk/dlls/crossbow.cpp:293-300` vs
  `:342-349`). Two separate DLLs never see each other's
  `CCrossbow::PrimaryAttack`; one static link sees both, as duplicate
  definitions - and, worse, for anything COMDAT (inline methods, vtables,
  RTTI) lld silently keeps ONE copy, so a class whose layout differs under
  `CLIENT_DLL` would link clean and misbehave. The specialist review
  named the sharpest case: `cl_dll/hl/hl_baseentity.cpp` defines 258
  `CBaseEntity::*`/`CBaseMonster::*`/`CBasePlayerWeapon::*` methods as
  client-side STUBS of the real server implementations in `dlls/`, same
  mangled names, and `GetClassPtr<T>` instantiations are COMDAT - the
  exact shape where lld would silently hand the server a client stub.
  This is precisely the problem upstream's `xshlib.py:33-43` `ld -r` +
  `objcopy -G lib_X_exports` step solves for `--static-linking`, and the
  X1 section's "bypassed entirely" note is where it was lost. It gets its
  own step, X7.1b, prototyped before any other X7 work is accepted.
- **Entity classnames are exported symbols.** `LINK_ENTITY_TO_CLASS`
  (`deps/hlsdk/dlls/util.h:100`) expands to
  `extern "C" DLLEXPORT void <classname>(entvars_t*)`, and the engine spawns
  by `COM_GetProcAddress(svgame.hInstance, classname)` (`sv_game.c`, see
  memory directive #6). Half-Life has 251 such exports, Opposing Force 323,
  Blue Shift 254. So a title's `server/exports.txt` is a ~260-line table
  that MUST be generated from the compiled module, never typed - a
  hand-written list is a hardcoded classname list, the thing directive #6
  forbids, and it would silently `SV_FreeEdict` every entity it missed.
- Assets for all three titles exist on this machine: the Steam install at
  `D:\SteamLibrary\steamapps\common\Half-Life` carries `valve/`, `gearbox/`,
  `bshift/` (and their `_hd` variants). `valve/delta.lst` - the file the
  fixed build stops on today - is there. The baked `ferrum56-build-test/`
  folder has only `valve/` (11 entries, for the Rust port's `.mpak` flow)
  and is not what a disc needs.
- `cl_dll` pulls in VGUI1: 9 `vgui_*.cpp` files, 27 files including its
  headers, linked against Valve's PREBUILT `utils/vgui/lib/win32_vc16/vgui.lib`
  - a proprietary x86 Windows binary that cannot be linked into an XBE. This
  SDK family has no `USE_VGUI` switch (0 `#ifdef USE_VGUI` in `cl_dll/`);
  `hlsdk-portable` does, and is the model. Engine side is already fine:
  `vgui_support` is not compiled for Xbox at all (nothing in
  `tools/xbox-link-check.sh` references it) and `engine/client/vgui/vgui.c:345`
  reports the load failure and continues. Single-player Half-Life does not
  need it; the scoreboard/MOTD/class menus it draws are multiplayer UI.

### X7.0 — A `resonance3d` branch per SDK (decided 2026-09-11)

Each SDK fork gets a `resonance3d` branch that carries ONLY this engine's
adaptations, and the submodules pin to it. `deps/hlsdk`'s current head
stays the Ferrum56/Rust branch (`master`, `_STATIC_ENGINE_LINK` work);
`resonance3d` branches from the last upstream-plus-fixes point (or from
`master` with the gate left undefined - it costs nothing either way since
the gate is off) and never defines `_STATIC_ENGINE_LINK`. `hlsdk-opfor`
and `hlsdk-bshift` branch `resonance3d` from their current stock heads.
`.gitmodules` gains `branch = resonance3d` for all three so `git submodule
update --remote` tracks the right line. What goes on those branches, and
only there: the `USE_VGUI` switch (X7.3), any `#if XASH_XBOX`/nxdk compile
fixes the sweep finds, and nothing that changes gameplay - reference
fidelity rules apply to the SDK exactly as to the engine (`AGENTS.md`),
with `deps/hlsdk`'s upstream as the reference for `deps/hlsdk`, etc.
Divergence ledger: one entry per branch, form 1 (our own forks).

### X7.1 — Build mechanics: the title is a required argument, and the TU set comes from CMake

**The title is a positional argument, never an environment variable.**
Every script in the chain - a new `tools/xbox-game-sweep.sh <title>`,
`tools/xbox-link-check.sh --title <title>`, `tools/xbox-xiso.sh <title>` -
takes `hl|op4|bshift` explicitly and refuses to run without it. Found in
Codex review: an `XBOX_TITLE=` selector with a silent `hl` default is the
exact failure lessons.md already records (`XBOX_*` exports vanish through
the documented MSYS bash invocation, and the result was a clean, bootable
artifact of the wrong configuration). So: no default, the title is written
into the sweep's `manifest.txt`, and the link and XISO steps each re-read
that manifest and compare title, SDK path, scratch dir, gamedir and output
name against what they were asked for before producing anything. A
mismatch is a hard error, not a warning.

**The translation-unit set comes from each title's own CMake, not from a
glob.** Found in Codex review: `cl_dll/*.cpp` is not the client. The real
client target (`deps/hlsdk/cl_dll/CMakeLists.txt:116-159`) is `cl_dll/`
plus `cl_dll/hl/` plus `cl_dll/particleman/` plus files from `common/`,
`dlls/` (the shared weapon sources, under `CLIENT_DLL`), `game_shared/`
and `public/` - and a hard-required export, `HUD_PostRunCmd`, is defined
in `cl_dll/hl/hl_weapons.cpp:892-901`, so a `cl_dll/*.cpp` sweep can never
link. Per title and per target, `tools/xbox-game-manifest.sh <title>
<server|client>` derives the expected TU list and the target's own
defines from that title's `dlls/CMakeLists.txt` / `cl_dll/CMakeLists.txt`
(`target_sources` + `target_compile_definitions`, read, not retyped),
writes it to `$SCRATCH/game-<title>/<target>.manifest`, and the sweep
compiles exactly that list with exactly those defines per TU (`CLIENT_DLL`
on the client side, never on the server side, `_STATIC_ENGINE_LINK` on
neither - see below). The sweep's `manifest.txt` records OK/FAIL/TOTAL
against the EXPECTED count and refuses to report clean on any mismatch in
either direction - a missing TU and an unexpected extra TU are both
failures, same guard shape as `xbox-link-check.sh:361-376` already
enforces for mainui. VGUI sources are excluded by the X7.3 `USE_VGUI` rule
inside that derivation, not by a hand-edited list - concretely, this means
`xbox-game-manifest.sh` needs zero VGUI-specific code at all: it reads
`cl_dll/CMakeLists.txt`'s `target_sources()` verbatim, and once X7.3 edits
that file to drop the 9 `vgui_*.cpp` entries, the tool stops emitting them
with no change to the tool itself. Until X7.3 lands, the client manifest
still includes them, accurately reflecting the file's current state.

**Correction, measured 2026-09-11: `op4` and `bshift` do NOT "add their
own" `CMakeLists.txt`.** The sentence above assumed they would; actually
checking (`find deps/hlsdk-opfor deps/hlsdk-bshift -iname CMakeLists.txt`)
found none in either tree - both ship only Visual Studio `.vcxproj`/`.sln`
and Linux Makefile project files, nothing `target_sources`-shaped to read.
`xbox-game-manifest.sh` therefore supports only `hl` for now and refuses
`op4`/`bshift` outright with a clear error rather than guessing a file
list for them (directive #6) - their own derivation mechanism is X7.6
work, not invented here.

**Built and verified end-to-end, 2026-09-11.** Ran against the real,
current `deps/hlsdk` CMakeLists on both targets: server - 107 `.cpp`
sources; client - 85 `.cpp` sources (VGUI still included, X7.3 not yet
done). Every derived source path checked to actually exist on disk (a
hard error otherwise); an independent comment-stripped recount from the
raw file, run separately rather than trusting the tool's own arithmetic,
matched both counts exactly. Negative tests: `op4` and `bshift` both
correctly refuse with the no-CMakeLists explanation; an unrecognized
token in a `target_sources()` block (neither `.cpp` nor `.h`) is a hard
parse error naming the token, not a silent drop - verified against a
deliberately mangled copy of the real file.

**Codex review round 1 found the defines were incomplete: both targets
also call `set_common_properties(TARGET_NAME)` (`deps/hlsdk/CMakeLists.txt`),
whose own `target_compile_definitions()` this script's first version
never read.** That block unconditionally adds `_CRT_SECURE_NO_WARNINGS`
and `CLIENT_WEAPONS` - not cosmetic: `CLIENT_WEAPONS` changes a weapon
save-field's type (`dlls/weapons.cpp:378-386`) and gates client-side
weapon prediction (`cl_dll/hl/hl_weapons.cpp:910-915`), so a manifest
missing it would compile and link a functioning-looking but materially
wrong build - exactly the "clean sweep of the wrong configuration"
failure mode this project has been burned by before. The block also
contains real CMake generator expressions (`$<$<CONFIG:DEBUG>:_DEBUG>`,
two `$<$<PLATFORM_ID:...>:...>` gates, and a `$<CONFIG>` value
substitution inside `UNIFIED_SDK_CONFIG="$<CONFIG>"`) - resolved against
a single, fixed, explicitly-chosen configuration (`RESOLVED_CONFIG=Release`,
since this project's Xbox compile route has no real CMake configure step
to inherit a Debug/Release choice from, and a shipped title would be
Release; `RESOLVED_PLATFORM=Xbox`, deliberately neither `Linux` nor
`Darwin`, the only two values this SDK's genexps ever test) rather than
by actually running CMake. Every genex SHAPE the script resolves is a
real one measured in the file; anything else is a hard parse error, never
a guess. A second, real parsing bug found and fixed in the same pass,
before ever running against real Codex feedback on it: a naive
whitespace tokenizer would tear apart `$<$<PLATFORM_ID:Linux,
Darwin>:POSIX _POSIX LINUX _LINUX GNUC>` (one CMake list item despite its
internal spaces - legal CMake syntax, whitespace inside a balanced
`$<...>` does not end an unquoted argument) into garbage tokens; fixed
with an angle-bracket-depth-aware splitter used for every list this
script tokenizes. Re-verified end to end against the real files: server -
107 sources, 5 defines (`VALVE_DLL`, `WIN32`, `_CRT_SECURE_NO_WARNINGS`,
`CLIENT_WEAPONS`, `UNIFIED_SDK_CONFIG="Release"`); client - 85 sources, 6
defines (adds `CLIENT_DLL`, `HL_DLL` to the same common five). Both
`$<$<PLATFORM_ID:...>:...>` gates and the `$<$<CONFIG:DEBUG>:_DEBUG>`
gate correctly resolve to nothing on this configuration, confirmed by
the exact count (not just "it didn't crash"). Unit-tested `resolve_define`
directly against all five real shapes plus one deliberately unknown genex
(`$<$<COMPILE_LANGUAGE:CXX>:...>`, never present in this file, to prove
the hard-error path fires rather than silently passing an unrecognized
shape through).

**Codex review round 2 (the cap) found the hard-error path above didn't
actually reach the caller: `resolve_define` runs inside a command
substitution feeding a here-string, and under bash that subshell's own
`exit 1` does not fail the surrounding `while`/`<<<` on its own - a
resolver failure would silently produce zero resolved lines for that
token and the script would carry on, exactly the false-clean measurement
this tool exists to prevent.** Fixed by capturing the resolver's output
into a variable first and checking its exit status explicitly
(`resolved_output="$(resolve_define "$tok")" || exit 1`) before ever
iterating over it. Re-verified through the ACTUAL script end-to-end, not
`resolve_define` in isolation (Codex's own stated bar): a scratch copy of
the real SDK tree with a deliberately unrecognized generator expression
injected into its root `CMakeLists.txt`, run through an unmodified copy
of this script with only `SDK_DIR` repointed at the scratch copy - fails
with exit 1, names the exact offending token, and writes no manifest
file. The real, unmodified `deps/hlsdk` re-run afterward to confirm no
regression: identical 107/5 and 85/6 source/define counts as before this
fix. Not fixed here, out of this script's scope (a
project-wide compiler-flag concern, not a per-title one): `set_common_properties`
also sets real `target_compile_options` (`-fpermissive
-fno-strict-aliasing -Wno-invalid-offsetof`, its own comment already
noting nxdk-cc needs them too) that `tools/xbox-cxx.sh` does not
currently pass - flagged for whoever builds the sweep/compile step to
either bake into `xbox-cxx.sh` directly (these are project-wide, not
per-title) or extend the manifest format for.

**Include directories added to the manifest format (`INCLUDE`), 2026-09-11
- same derivation discipline as defines, applied before writing the
compile/sweep step that would otherwise need its own hand-typed `-I`
list.** `target_include_directories()` read from both the per-target
CMakeLists.txt and `set_common_properties()`, same two-source pattern as
defines. Two real CMake variable references appear, resolved
deterministically rather than guessed: `${CMAKE_CURRENT_SOURCE_DIR}` (each
target's own directory - one fixed value per CMakeLists.txt, not a
generator expression) resolves to `$SDK_DIR/$SUBDIR`; `${CMAKE_BINARY_DIR}`
(from `set_common_properties`) resolves to nothing - checked, not assumed:
`grep configure_file` across every `deps/hlsdk` CMakeLists.txt found only
an install script and `cl_dll`'s Linux-only vgui/SDL2 `.so` copies, no
header any `.cpp` `#include`s ever gets generated into a binary
directory, so adding a scratch path here would invent a value nothing
needs rather than derive a real one. Any other `${...}` reference is a
hard parse error, same posture as an unrecognized generator expression.
Two new end-to-end negative tests through the real script (same
scratch-SDK-copy technique as the generator-expression one above): an
unrecognized `${...}` variable injected into the common include block
fails with exit 1 and writes no manifest; the pre-existing
generator-expression negative test re-run to confirm this change didn't
regress it.

**Codex review round 1 found the include set was still incomplete: neither
`target_include_directories()` block sees a directory a linked library's
own INTERFACE requirement adds.** `client` links `vgui` and `SDL2`
(`cl_dll/CMakeLists.txt:59-63`, both `add_library(... IMPORTED)` targets
the same file defines, each with its own
`target_include_directories(NAME INTERFACE ...)` - `../utils/vgui/include`
and `../external` respectively). `vgui`'s happens to also be listed
directly in `client`'s own include block (redundant, not a bug), but
`../external` is NEVER listed anywhere except via this transitive
INTERFACE relationship, and `cdll_int.cpp:21` unconditionally
`#include <SDL2/SDL_messagebox.h>`s from it - a real compile failure the
old derivation would have produced. Fixed: `target_link_libraries()` (now
also read from both the per-target and common CMakeLists - itself
optional, `dlls/CMakeLists.txt` has none at all) is scanned for each
linked name, and any name with a matching
`target_include_directories(NAME INTERFACE ...)` block anywhere
contributes its resolved directories too; a name with no such block
(`${CMAKE_DL_LIBS}`, or a Windows-only import library like `delayimp`/
`wsock32`, both genex-gated and skipped without evaluating the genex,
since neither has such a block regardless) contributes nothing, which is
correct, not an omission. A second, real parsing gap found and fixed in
the same pass: `vgui`/`SDL2`'s own declarations use
`${CMAKE_CURRENT_SOURCE_DIR}` as a PATH PREFIX
(`${CMAKE_CURRENT_SOURCE_DIR}/../utils/vgui/include`), a shape the first
version's variable resolution never handled (it only matched the bare,
whole-token form) - fixed with a prefix-match case, guarded so a future
match inside the ROOT CMakeLists.txt (where the same variable would mean
something different) fails loudly instead of resolving silently wrong,
since no real case hits that today. A third finding, self-caught while
verifying the fix, not from Codex: the two derivations for `vgui`'s
include now produce the SAME real directory spelled two different ways
(`utils/vgui/include` vs `cl_dll/../utils/vgui/include`) - a plain string
dedup does not catch that, so every resolved path is now normalized with
`realpath -m` before deduping.

Re-verified against the real files: server unaffected (`dlls/CMakeLists.txt`
has no `target_link_libraries()` at all) - still 6 include dirs; client -
11 (adds `external` to the prior 10, `vgui`'s duplicate spelling
collapsed by the new normalization), every one confirmed to exist on
disk. Verified Codex's own recommended check for real, not just at the
derivation level: compiled `cl_dll/cdll_int.cpp` with `tools/xbox-cxx.sh`
using exactly the manifest-derived defines and includes - WITHOUT
`external` on the include path, `SDL2/SDL_messagebox.h` fails outright
(`file not found`, the exact bug reported); WITH it (the fixed manifest's
real output), that header is found and compilation proceeds much
further, into a separate, real, PRE-EXISTING problem this fix does not
claim to solve: `deps/hlsdk/external/SDL2/SDL_stdinc.h` uses MSVC SAL
annotations (`SDL_INOUT_Z_CAP`, `_Scanf_format_string_impl_`) that don't
parse under nxdk-cc's clang - a genuinely new, separate finding, not
this task's scope, flagged here for whoever next attempts a full client
compile (X7.3/X7.4 territory: `SDL2` is only linked for `vgui` and
`inputw32.cpp`'s gamepad support, both already slated for X7.3-adjacent
rework, not core gameplay).

**Codex review round 2 (the cap) found a stale-manifest hole: every
validation in this script runs before the output write at the very end,
so a run that FAILS midway (any of the checks above) left a PRIOR
successful run's manifest untouched at the fixed path this project's own
design has a downstream sweep read from.** A "no manifest" guarantee the
header comment and every negative test above already claim, silently
broken for the one case that matters most in practice: a real edit to
`deps/hlsdk` that introduces a new parse failure would leave the LAST
GOOD manifest in place, and a sweep reading it would compile stale
inputs while believing regeneration had happened - the exact false-clean
failure mode this project keeps finding and fixing. Fixed: the output
path is resolved and any existing manifest removed immediately, before
any CMakeLists parsing begins, and the final write goes to a temp file
in the same directory, renamed into place only once every line is
written - so between "removed" and "renamed" there is genuinely no
manifest at the fixed path, not a stale one and not a truncated one.
Verified exactly the scenario Codex asked for, through the real script:
a valid manifest written from a scratch SDK copy; the same copy then
mangled with an unrecognized generator expression; a second run
correctly fails and the manifest that step 1 wrote is gone, not left in
place. Full regression suite re-run clean against the real, unmodified
`deps/hlsdk` afterward (unchanged 6/11 include counts).

**`deps/hlsdk`'s `resonance3d` branch CMakeLists fixed to actually match
what this project needs, 2026-09-11 (recorded as divergence #35) - found
while building the manifest tool above, not assumed going in.** The
branch was cut from `master`
(X7.0) and had not yet diverged from it, so it still carried three real
problems specific to `master`'s own Ferrum56/Rust-port setup:

- `dlls/CMakeLists.txt` unconditionally defined `_STATIC_ENGINE_LINK` in
  `target_compile_definitions(hl PRIVATE ...)` - the exact macro this
  project's own rules forbid ever defining (X7.1b already established
  that leaving it undefined yields stock upstream behaviour on the .cpp
  files themselves; the CMakeLists is where it would actually have been
  turned on for a real build). Removed.
- With it removed, `game_shared/filesystem_utils.cpp`'s stock (non-
  `_STATIC_ENGINE_LINK`) `FileSystem_LoadFileSystem()` path calls
  `Sys_LoadModule`/`Sys_GetFactory` (`public/interface.h`) at runtime -
  but `master`'s `dlls/CMakeLists.txt` drops `public/interface.cpp` from
  `target_sources()` entirely, correct for `master` (whose
  `_STATIC_ENGINE_LINK` path never reaches those calls) and wrong for
  `resonance3d` (an undefined-symbol link failure otherwise). Restored;
  verified compiling clean with `tools/xbox-cxx.sh` (`-DWIN32`, warnings
  only) and defining the exact mangled symbols
  (`?Sys_LoadModule@@...`/`?Sys_GetFactory@@...`/`?Sys_UnloadModule@@...`/
  `_CreateInterface`) `filesystem_utils.cpp` calls.
- `cl_dll/CMakeLists.txt`'s client target never defined `WIN32` at all -
  the server side already had this exact fix (nxdk-cc never defines it,
  `Platform.h`'s `#ifdef WIN32` branch is needed on both sides), the
  client side was simply missed. Added.

**New, more severe X7.4 boot blocker found in the same pass, not yet
fixed: `SV_InitServer()` (`dlls/game.cpp:461-476`) hard-fails at
`GameDLLInit()` before any gameplay code runs.** With
`_STATIC_ENGINE_LINK` correctly undefined, `FileSystem_LoadFileSystem()`
takes its stock path: `Sys_LoadModule("filesystem_stdio.dll")` is a raw
`LoadLibrary()` call (`public/interface.cpp:130-133`), and nxdk's
`LoadLibraryExA` always returns `NULL` (no dynamic loading on Xbox -
`deps/nxdk/lib/winapi/libloaderapi.c`) - so `FileSystem_LoadFileSystem()`
returns `false`, and `dlls/game.cpp:490-496`'s `GameDLLInit()` responds
with `SERVER_COMMAND("quit\n")` and returns immediately. This links
cleanly (the fix above) but fails at the very first opportunity at
runtime - ahead of the previously-known writable-rootdir gap, which only
matters once a level transition/save is attempted. Not fixed here (real
SDK source-behaviour work, X7.4 scope, not X7.1 tooling); the fix shape
is already known and cheap to verify once X7.4 starts: route
`FileSystem_LoadFileSystem`/`FileSystem_LoadFileIntoBuffer`/etc through
`g_engfuncs.pfnLoadFileForMe`/`pfnFreeFile` (`engine/eiface.h:201-202` -
real Xash3D-FWGS engine callbacks, present in this project's own engine,
not a Rust-specific concept) behind a Resonance3D-specific guard, the
same shape `_STATIC_ENGINE_LINK`'s own `FileSystem_LoadFileIntoBuffer`
branch already takes for a different engine.

Compile route: this project's own `tools/xbox-cxx.sh` per TU, not the
SDK's CMake through `toolchain-nxdk.cmake`. Every other module in the XBE
goes through the sweep scripts and their `compat/xbox` include ordering
and prelude, the manifest/staleness checks are written for that shape,
and a CMake-built `.lib` mixed into a sweep-built link would sit outside
the tooling meant to catch exactly the mistakes that cost time so far.
CMake stays the fallback if a per-TU flag proves impossible to express.
Per-title inputs: SDK dir (`deps/hlsdk` / `deps/hlsdk-opfor` /
`deps/hlsdk-bshift`), scratch dir `$SCRATCH/game-<title>/`, output
XBE/XISO name, disc gamedir (`valve` / `gearbox` / `bshift`). `stub_game/`
is retired the moment `hl` links: it exists to prove the mechanism, and
the mechanism is proven.

### X7.1b — Module symbol isolation, prototyped before anything else

Each module's object set is renamed so its internals cannot meet the
other module's in the link. Order matters and is fixed, not incidental
(the second and third steps were found missing/wrong in Codex review -
see the measurement below): (1) compile; (2) X7.2's generator reads each
object's `/EXPORT:` directives (`llvm-readobj --coff-directives`) to
build `exports.txt` - the LAST point these directives are ever read; (3)
`llvm-objcopy --remove-section=.drectve` strips them, because this
project's static-linking mechanism never consumes a PE export table and
a stale `/EXPORT:` on a symbol the next step renames away corrupts the
final image's export table instead of erroring (measured below); (4)
`llvm-nm --defined-only --extern-only` over the stripped objects produces
the set of symbols the module DEFINES; every one of them NOT on that
module's public allowlist is written to a rename map `old new` with a
`sv_`/`cl_` prefix, and `llvm-objcopy --redefine-syms <map>` (present:
`/c/msys64/mingw64/bin/llvm-objcopy.exe`, COFF-capable) is applied to
every object of that module. Renaming is consistent across definitions
and references within the module because the map is derived from the
module's own definitions, and references to symbols the module does NOT
define (CRT, libc++, nothing from the engine - the SDK reaches the engine
only through `gEngfuncs`) are untouched because they are not in the map.

**The allowlist is the set of names the ENGINE resolves for THAT module,
and nothing else** - corrected in Codex review round 2 of this plan. A
first draft excluded "the module's own export list" from renaming, i.e.
everything its objects mark `/EXPORT:`. That preserves the collision:
the client compiles `crossbow.cpp` too, where `LINK_ENTITY_TO_CLASS`
stays active (`deps/hlsdk/dlls/crossbow.cpp:224`, `:516`, macro
`dlls/util.h:100-102`), so `weapon_crossbow`/`ammo_crossbow` are
`/EXPORT:` C symbols on BOTH sides, and excluding them on both sides
leaves them duplicated. The engine never asks the client for an entity
classname, and never asks either module for `PM_*` by name (`sv_pmove.c:491`,
`:956` go through `svgame.dllFuncs`). So:

- server allowlist = the four API exports (`GetEntityAPI`, `GetEntityAPI2`,
  `GetNewDLLFunctions`, `GiveFnptrsToDll`) + every entity classname, i.e.
  exactly the server objects' `/EXPORT:` set;
- client allowlist = `cl_game.c`'s `cdll_exports[]` (37) + `cdll_new_exports[]`
  (11 - corrected 2026-09-11, X7.2: an earlier grep miscounted this as 12)
  names, and NOTHING from the client objects' `/EXPORT:` set beyond
  those - client-side entity symbols are renamed like any other internal;
- `PM_*` is on neither list and is renamed on both sides.

This is `xshlib.py`'s `objcopy -G` localisation done by renaming instead
of by a relocatable pre-link, since `lld-link` has no COFF `-r`
(measured: `lld-link /?` lists only `/incremental`, no relocatable or
partial-link option). Applied to BOTH modules, not just one: it also
removes the COMDAT silent-merge hazard (a renamed vtable cannot be
merged with the other side's) and any libc-helper collision with the
engine.

**Prototype gate, before X7.1/X7.2 tooling is even written:** compile
`deps/hlsdk/dlls/crossbow.cpp` twice with `tools/xbox-cxx.sh` (server
defines, then `CLIENT_DLL`), plus the minimum each needs to resolve.
Apply the rename to BOTH sides with the allowlists above, then check the
concrete symbols before any link: `llvm-nm` of the renamed client object
must show `cl__weapon_crossbow`, `cl__ammo_crossbow` and a prefixed
`?PrimaryAttack@CCrossbow@@...`, and of the renamed server object an
unprefixed `_weapon_crossbow` with a prefixed `sv_?PrimaryAttack@...`.
Only then link both with the engine object set through
`tools/xbox-link-check.sh` and require `0 duplicate`; then the negative
control - the same link with NO rename must report the duplicates, by
name (`?PrimaryAttack@CCrossbow@@...`, `_weapon_crossbow`), or the gate
has measured nothing. `gen-library-tables.sh` needs no change: the names
it declares `extern void` are exactly the allowlisted, unrenamed ones.
Record the resulting build divergence in the ledger; it lives in
`tools/`, form 1, touching no SDK source at all.

**Measured 2026-09-11 on real `tools/xbox-cxx.sh` objects, before this
was written down as the design:** two TUs, one defining a C++ function
and two `extern "C" DLLEXPORT` functions, the other calling the C++ one.
A rename map derived exactly as above (`llvm-nm --defined-only
--extern-only` of the defining object, minus the two export names, each
line `old cl_old`) applied with `llvm-objcopy --redefine-syms` to both
objects gave: defining TU `T cl_?also_not_exported@@YAHXZ`, referencing
TU `U cl_?also_not_exported@@YAHXZ` - definition and cross-TU reference
renamed consistently - with the `/EXPORT:_monster_headcrab
/EXPORT:_GetEntityAPI2` directives untouched. Two things the experiment
taught that the design must respect: the map has to use the symbol
names exactly as `llvm-nm` prints them (the MSVC-mangled
`?name@@YAHXZ`, not a C-style guess - a first attempt with `_name`
matched nothing and silently renamed nothing, which is why the negative
control below is mandatory), and a prefix in front of a mangled name
yields a non-standard symbol (`cl_?...`) that `lld-link` treats as an
opaque identifier - fine for linking, but never feed those objects to a
demangler and expect sense.

**Prototype gate: PASSED, measured 2026-09-11, corrected 2026-09-11
after Codex review round 1 caught two real gaps in the first pass (both
below).** Compiled `deps/hlsdk/dlls/crossbow.cpp` twice with
`tools/xbox-cxx.sh` - server side (`-DVALVE_DLL -DWIN32`,
`dlls`/`common`/`engine`/`game_shared`/`pm_shared`/`public` on the
include path) and client side (`-DCLIENT_DLL -DHL_DLL -DWIN32`,
`cl_dll`/`cl_dll/hl`/`cl_dll/particleman`/`dlls`/`common`/`game_shared`/
`public`/`pm_shared`/`engine`) - both compile clean (warnings only).
`llvm-nm --defined-only --extern-only` found 267 defined externs
server-side, 220 client-side, **150 shared names between them** before
any rename: constructors/destructors/vtables/RTTI for
`CCrossbow`/`CCrossbowAmmo`/`CBaseEntity`/..., every virtual method body,
AND plain global data - `?g_engfuncs@@3Uenginefuncs_s@@A` and
`?gpGlobals@@3PAUglobalvars_t@@A` are each defined once per module, not
looked up from the engine, so the allowlist and the rename apply to data
symbols exactly as to functions, not "functions" as a first draft might
assume.

**Gap 1, found in review, twice: the first pass linked only the two
crossbow objects with `-force:unresolved`, never the real engine; the
second pass's own "real engine" was still incomplete.** Round 1 of
review: two isolated objects prove they don't collide with EACH OTHER,
nothing about the 384 objects this XBE actually links. First correction
used `ls .sweep/*.obj .sweep/gen/*.obj` plus `tools/xbox-link-check.sh`'s
own filename filters - 178 objects, `0` duplicate on its own - and was
STILL wrong: third-party objects live in `.sweep/3p/`, one directory
level this glob never reached (`tools/xbox-link-check.sh:232-305` builds
`THIRDPARTY_OBJS` there separately), so 206 real linked objects -
including seven real `MultiEmulator` exports
(`3rdparty/MultiEmulator/include/multi_emulator.h:25-31`) - were absent
from what was "tested". Corrected properly this time: ran
`tools/xbox-link-check.sh --client-main --xbe` for real (one clean
invocation, not concurrent with anything else - lessons.md already
documents what concurrent invocations of this script do to `.sweep/gen/`)
and read back its own `.sweep/link-check-response.txt` - the EXACT
384-object list (engine + all third-party + stub game + generated
tables) the actual client XBE was just linked from, `0 duplicate, 0
undefined` on its own. Appended the crossbow objects to a copy of that
same response file rather than reconstructing any object list by hand,
and ran the full link three ways:

- **Clean baseline** (the real 384-object response file, unmodified, no
  crossbow at all): exit 0, `0` duplicate.
- **Positive control** (the same response file + the two RENAMED
  crossbow objects appended): exit 0, `0` duplicate, a real 3.89 MB
  linked image.
- **Negative control** (the same response file + the two UNRENAMED
  crossbow objects appended): exit 1, 13 duplicate-symbol errors, named:
  `_weapon_crossbow`, `_ammo_crossbow`, and 11 `CCrossbow::*` virtual
  methods (`Spawn`, `Precache`, `GetItemInfo`, `Deploy`, `Holster`,
  `PrimaryAttack`, `FireSniperBolt`, `FireBolt`, `SecondaryAttack`,
  `Reload`, `WeaponIdle`) - the SAME 13, exactly, as both earlier
  (insufficient) tests, confirming those weren't wrong about the
  collision itself, only about what they had actually linked it against.

**Gap 2, found in review: `/EXPORT:` directives for a renamed-away
symbol are NOT harmless.** The first pass claimed `lld-link` only
consumes `/EXPORT:` for a `/DLL` link and is otherwise a no-op - false,
measured directly: `llvm-readobj --coff-exports` on `.sweep/xbe-build.exe`
(a real, already-built client XBE, no `/DLL` anywhere in `nxdk-link`,
`deps/nxdk/bin/nxdk-link:7-13`) shows a real, populated export table
(`CreateInterface`, `GetRefAPI`, `GetFSAPI`, ...) - **every EXE this
project links already carries one.** Worse, linking the renamed CLIENT
object as-is (rename applied, directives untouched) produced export
entries for the renamed-away names (`?BoltTouch@CCrossbowBolt@@...`,
`?BubbleThink@...`, `?SUB_CallUseToggle@CBaseEntity@@...`) with RVA
`0xFFFF0000` - a linker sentinel for "could not resolve this", silently
baked into the image as if it were real data, because the symbol the
stale directive named no longer existed to resolve to.

Root cause and fix: our static-linking mechanism (`lib_static.c`'s
`Lib_Find`, populated by `gen-library-tables.sh`'s `extern void name` +
address-of) never calls `GetProcAddress` on ourselves - it resolves
allowlisted names by ordinary compile-time linkage, not through a PE
export table at runtime. **The `/EXPORT:` directives are read exactly
once, by X7.2's generator, before any rename - after that this project
has no use for them at all**, so the fix is to strip the whole
`.drectve` section (`llvm-objcopy --remove-section=.drectve`, confirmed
to leave the defined-symbol set byte-identical) from EVERY module object
immediately after the export list is read and before the rename step -
not to try to selectively edit `/EXPORT:` lines out of the directive
string. Re-ran the full positive control with this order (compile → read
directives for X7.2 → strip `.drectve` → rename → link): `0` duplicate,
same as before, and `llvm-readobj --coff-exports` on the TRUE
full-object-set image shows exactly the 14 legitimate pre-existing
exports - 7 engine (`CreateInterface`, `GetFSAPI`, `GetRefAPI`,
`Host_Main`, `Host_Shutdown`, `pfnSPR_Frames`, `pfnSPR_Load`) and the 7
`MultiEmulator` ones Gap 1 found missing the first time
(`GenerateAVSMP`, `GenerateOldRevEmu`, `GenerateRevEmu`,
`GenerateRevEmu2013`, `GenerateSC2009`, `GenerateSetti`,
`GenerateSteamEmu`) - and zero crossbow-related entries, zero
`0xFFFF0000` sentinels. `/INCLUDE:` directives (used
elsewhere in this SDK to keep an otherwise-unreferenced static data
member from being discarded, e.g. `?AmmoInfoArray@CBasePlayerItem@@...`
on `crossbow.cpp` itself) are lost by a whole-section strip too; nothing
in this project currently relies on one surviving past this point
(`nxdk-link` does not dead-strip unreferenced globals by default), but a
real implementation that ever needs to keep one should filter the
section's text for `/EXPORT:` lines specifically rather than removing it
outright - noted, not yet needed.

Applied the allowlists from the corrected design above: server map
excludes `_weapon_crossbow`/`_ammo_crossbow`/`_crossbow_bolt` (264 other
symbols renamed `sv_*`); client map excludes nothing from this TU
(`crossbow.cpp` exports no `cdll_exports` name - it is a weapon internal,
not client-API surface), so all 220 defined symbols renamed `cl_*`,
entity classnames included.

The generated export tables (X7.2) need no change beyond the ordering
above (read directives before stripping): each module's `exports.txt` is
exactly its allowlist, and the allowlisted names are never renamed, so
`gen-library-tables.sh`'s `extern void name` declarations still bind.
Record the resulting build divergence in the ledger; it lives in
`tools/`, form 1, touching no SDK source at all.

**`tools/xbox-game-isolate.sh` built and run for real against the whole
compiled `hl` object set, 2026-09-12 - the bulk implementation of the
2-object prototype above, not a rewrite of its design.** Runs the target's
sweep itself first (fresh, never trusting stale objects, same discipline
as the sweep tool's own manifest regeneration), generates `exports.txt`
(X7.2), strips `.drectve` from a copy of every object, builds a rename
map from the module's own defined-symbol set minus the allowlist (server:
the just-generated `exports.txt` itself, since the server allowlist IS
"the objects' own `/EXPORT:` set"; client: the FIXED
`tools/xbox-client-allowed-exports.txt`, not the `--allow`-filtered
generated one - per the design above, what matters is which names the
engine will ever look up by that string, not which of them this
particular object set happens to export), applies it with
`llvm-objcopy --redefine-syms`, and self-checks by re-scanning every
isolated object afterward - every defined symbol must be either
allowlisted or `${prefix}_`-prefixed, or the run is a hard failure, not
a warning.

**Server: isolated cleanly.** 102 objects (the sweep's own real, current
baseline - 5 known compile failures, tracked via `--expect-fail`), 9,349
defined symbols total, 240 kept (the API exports plus every real
`LINK_ENTITY_TO_CLASS` classname), 9,109 renamed `sv_`, 0 leaked.
**Client: correctly refuses, not a bug in this tool.** `xbox-gen-exports.sh`'s
own completeness gate (X7.2) refuses to write `exports.txt` - the 67
objects that DID compile are missing `Initialize`/most `HUD_*` entry
points, because the file that defines them (`cdll_int.cpp`) is one of
the 18 real, already-known client compile failures (the SDL2-header/clang
incompatibility `tools/xbox-game-sweep.sh`'s own X7.4 entry already
flagged). Client isolation is blocked on the same pre-existing problem,
not a new one - fixing that (X7.3/X7.4 scope) unblocks this automatically,
no change to this tool needed.

**Two real bugs found and fixed while running this for real, both the
SAME class already found once this session (`tools/xbox-game-sweep.sh`'s
log), re-appearing because this tool didn't yet carry the same fix.**
(1) A file written early (a small allowlist extraction) and read back
only after ~200 external process invocations (a `cp` plus an
`llvm-objcopy` call per object) had run in between came back "No such
file or directory" - traced to the file living under `mktemp -d`'s
default system-temp location, which this session had ALREADY measured
directly to be unreliable for this kind of gap (a plain
`: > /tmp/x; ls /tmp/x` round trip failed outright, run standalone,
earlier the same session). Fixed by rooting the working directory under
`$SCRATCH` instead - the same real project disk every other tool in this
session's own `$SCRATCH` usage has been reliable on. (2) Even after that
fix, one real symbol (`CFlockingFlyer::Save`, mangled) measurably leaked
through unrenamed - traced to the exact repeated-`>>`-reopen pattern
already fixed once in `xbox-game-sweep.sh`'s log (there: ~100 reopens of
one file across a compile loop; here: the same shape building the
module's defined-symbol list across ~100 objects). Fixed identically:
one persistent file descriptor per accumulation loop, opened once,
closed once. Re-verified after both fixes: the defined-symbol count went
from an undercounted 606 to the real 9,349 (over 15x), the previously
leaked symbol confirmed correctly renamed (`sv_?Save@CFlockingFlyer@@...`),
and the full self-check reports 0 leaked.

**Codex review round 1 found a third, more serious real bug: the
self-check couldn't have caught its own allowlist being wrong, and it
was - `GiveFnptrsToDll`, a hard-required server export, was silently
renamed away.** `exports.txt`'s `GiveFnptrsToDll _GiveFnptrsToDll@8` line
(the asm-symbol column X7.2's stdcall fallback writes) has a real subtlety
the allowlist extraction missed: for THIS row, column one
(`GiveFnptrsToDll`) is not a real symbol in the object at all - no plain
cdecl form, no `/EXPORT:` directive, only the stdcall-decorated
`_GiveFnptrsToDll@8` exists (X7.2's own finding). Extracting only column
one kept a name nothing defines, while the real symbol's bare form
(`GiveFnptrsToDll@8`) matched nothing and was silently renamed to
`sv__GiveFnptrsToDll@8` - breaking the one export
`engine/server/sv_game.c` hard-requires to load the module at all. The
existing leak-check couldn't catch this because it (correctly, by
design) trusts the same `$ALLOW_FILE` the rename decision itself uses -
a bug in building that file passes both self-consistently. Fixed two
ways: (1) the allowlist extraction now uses column two's bare form when
present, column one otherwise; (2) a genuinely INDEPENDENT check added
after the leak-check, never touching `$ALLOW_FILE` - re-reads
`tools/xbox-{server,client}-required-exports.txt` directly (the same
source of truth `xbox-gen-exports.sh` itself uses) and searches the real
post-rename symbol table for either form it would accept (plain cdecl or
stdcall-decorated). Unit-tested the independent check against the exact
prior symptom first (a synthetic post-rename symbol list containing only
`sv__GiveFnptrsToDll@8`, no plain or stdcall-decorated form) - correctly
reports it missing. Re-verified end to end against the real objects:
`_GiveFnptrsToDll@8` now confirmed present, unrenamed, in the real
isolated `h_export.obj` (kept count 239→240, renamed count 9,110→9,109,
exactly ±1 as expected); `GetEntityAPI`/`GetEntityAPI2`/`weapon_crossbow`
spot-checked still correctly kept; client re-run confirms an identical,
unaffected refusal (this bug and fix are server-only - the client
allowlist file has no two-column entries).

**Codex review round 2 (the cap) found a real design gap: a PARTIAL
object set (the server's own documented 5-known-failure baseline) was
published as "isolated cleanly" with nothing in the artifact itself
distinguishing it from a genuinely complete, link-ready module.** The
plan's own prose already said "102 objects (5 known compile failures)",
but `manifest.txt` carried only object/symbol counts, and the terminal
message said "isolated cleanly" regardless - a future automated
consumer reading only the artifact, not this document, would have no
way to tell a probe from a real module. Rather than refuse to publish
anything for a partial run (Codex's primary suggestion, which would
remove real value: proving this tool's mechanism against a known,
tracked-partial baseline is exactly what the server run above already
did, and X7.4's own bring-up work does not need every compile failure
fixed first to keep exercising this tool), took Codex's own offered
alternative: the sweep's real `OK`/`FAIL`/`TOTAL` (already written by
`tools/xbox-game-sweep.sh` to its own manifest) is read back and carried
through to a new `SWEEP_OK`/`SWEEP_FAIL`/`SWEEP_TOTAL`/`COMPLETE
yes|no` block in this tool's own `manifest.txt`, and the terminal
message itself now reads differently for a partial run ("isolated a
PARTIAL object set... NOT link-ready") rather than the same "isolated
cleanly" either way. Re-verified against the real server baseline:
`manifest.txt` now reads `SWEEP_OK 102`/`SWEEP_FAIL 5`/`SWEEP_TOTAL 107`/
`COMPLETE no`, and the terminal message correctly reflects the partial
state - a future consumer checking `COMPLETE` before trusting this
directory for a real link now has something real to check.

### X7.2 — Generated export tables, from the objects, not from a list

**Built and verified end-to-end, 2026-09-11 (`tools/xbox-gen-exports.sh`,
`tools/gen-library-tables.sh` extended with a backward-compatible optional
second column).** Real SDK files, not synthetic ones: compiled
`crossbow.cpp` (server side), `h_export.cpp`, and `cbase.cpp` (the file
`GetEntityAPI2`/`GetEntityAPI`/`GetNewDLLFunctions` actually live in) with
`tools/xbox-cxx.sh`, ran `xbox-gen-exports.sh --all --require
<GetEntityAPI|GetEntityAPI2 + GiveFnptrsToDll>` against the three
objects. Negative control first: run against `crossbow.cpp` + `h_export.cpp`
alone (no `cbase.cpp`) - correctly failed, naming
`GetEntityAPI|GetEntityAPI2` as missing, nothing written. With
`cbase.cpp` added: wrote exactly `GetEntityAPI`, `GetEntityAPI2`,
`GetNewDLLFunctions`, `GiveFnptrsToDll _GiveFnptrsToDll@8` (the asm
override, found via the stdcall fallback - the X7.2 gap Codex review
round 2 caught, now exercised for real) and the three real entity
classnames (`ammo_crossbow`, `crossbow_bolt`, `weapon_crossbow`) - zero
mangled C++ methods leaked through, despite the same objects containing
plenty (`CCrossbow::PrimaryAttack` and the rest, from the X7.1b
prototype). `--allow` mode (the client case) tested separately against
the client-compiled `crossbow.cpp`: a 3-name fake `cdll_exports`
allowlist, none of them actually exported by this file, correctly wrote
zero entries with a warning, not a silent empty success. Full chain
proven, not just the generator in isolation: fed the generated
`server/exports.txt` through the now-extended `gen-library-tables.sh`,
compiled the result, and linked it against the three real compiled SDK
objects - `0` duplicate, every one of the seven names (including the
asm-overridden `GiveFnptrsToDll`) resolved with no undefined-symbol
warnings naming it.

**Codex review round 1 found two real gaps in that first version, both
fixed and re-verified, 2026-09-11:** (1) both object-inspection loops
piped `llvm-readobj`/`llvm-nm` through `2>/dev/null | ... || true`,
which reports only the last pipeline stage's exit code - a crashed or
unreadable object was indistinguishable from "genuinely exports
nothing," so a bad object could silently drop its names from the table
while the script still exited 0. Fixed: each object's inspector
invocation is now checked individually, with stderr captured and the
offending object named, before any grep/awk touches its output; a
corrupt-object negative test (`llvm-readobj: ... not recognized as a
valid object file`) now fails fast and names the object, verified for
real. (2) the "zero entries" case above was only ever a warning, and the
plan's own documented client invocation never passed `--require`,
leaving no enforced floor under the client's 37 hard-required
`cdll_exports` names (`cl_game.c:4059-4084` treats any one of them
missing as fatal at load time). Fixed: zero entries is now a hard error
by default (the output file is not written), with a new explicit
`--allow-empty` opt-out for a module that is genuinely, deliberately
empty; and a new checked-in `tools/xbox-client-required-exports.txt`
(the 37 `cdll_exports` names, a literal subset of the 48-name allow-list)
is now passed via `--require` on the documented client invocation below.
Both fixes re-verified end-to-end against the same real compiled
objects: the zero-entries gate (error without `--allow-empty`, success
with it), and the corrupt-object gate, each with a positive and negative
case.

**Codex review round 2 (the cap for this branch) found two more real
gaps, both in the completeness gate itself, both fixed and re-verified,
2026-09-11:** (1) the gate's "already satisfied" check looked at
`directive_names.txt` (the object's full, UNFILTERED raw export list)
as well as the actually-written `out_lines.txt` - correct for `--all`
(where they start identical) but wrong for `--allow`: a required name
that is a real export of the compiled object yet absent from the
allowlist would satisfy the gate via the raw-directive check while never
being written to the generated table at all. Verified with the real
client-compiled `crossbow.cpp` object: allow-list containing only
`ammo_crossbow`, `--require weapon_crossbow` (a real export of that same
object, deliberately left off the allow-list) - before the fix this
incorrectly passed; after, it correctly reports `weapon_crossbow` missing
and writes nothing. Fixed by checking only `out_lines.txt`, the one
thing that actually reaches `$OUT`. (2) the zero-entries check (above)
ran on the FINAL table, after the completeness gate's stdcall/raw-symbol
fallback had already run - so an `--all` object set whose `/EXPORT:`
extraction found genuinely nothing (wrong objects, or a parser that
stopped matching) could still end with a nonzero, "passing" table
containing only the fallback-recovered required names, silently losing
every real entity classname while exiting 0. Verified with `h_export.obj`
alone (which the X7.1b/X7.2 work already established has zero
`/EXPORT:` directives - `GiveFnptrsToDll` only resolves via the stdcall
raw-symbol fallback) and `--require GiveFnptrsToDll`: before the fix
this wrote a 1-line "clean" table; after, it correctly fails before the
fallback ever runs, naming zero directives as the cause, with
`--allow-empty` still available for a module that is genuinely meant to
be that empty. Full regression suite (all six prior positive/negative
cases, including the real 3-object end-to-end server build) re-run
clean after both fixes.

`tools/gen-library-tables.sh` already took `name:exports.txt`; what
changed is where `exports.txt` comes from and, for the one case that
needs it, what each line carries: a new `tools/xbox-gen-exports.sh`
reads the COMPILED objects and writes the file, using
`gen-library-tables.sh`'s new optional second column
(`name asm_symbol`) only for names whose real linker symbol is not the
plain cdecl form - `GiveFnptrsToDll` today, nothing else measured so
far. Every exports.txt written before this existed (`stub_game/`,
`3rdparty/mainui`, `filesystem/`, `ref/null`) has zero lines with a
space, so the extension is strictly backward-compatible, verified by
regenerating `stub_game/server/exports.txt`'s table before and after the
`gen-library-tables.sh` change and diffing byte-for-byte identical.
Per-title, per-target invocation for X7.4: `tools/xbox-gen-exports.sh
--all --require tools/xbox-server-required-exports.txt
server/exports.txt <every compiled dlls/+pm_shared/ object>` and
`tools/xbox-gen-exports.sh --allow tools/xbox-client-allowed-exports.txt
--require tools/xbox-client-required-exports.txt client/exports.txt
<every compiled cl_dll/ object>`. Three small, fixed input files are
written and checked in: `tools/xbox-server-required-exports.txt`
(the `GetEntityAPI|GetEntityAPI2` + `GiveFnptrsToDll` requirement),
`tools/xbox-client-allowed-exports.txt` (`cdll_exports`'s 37 names +
`cdll_new_exports`'s 11, 48 total - re-read directly from
`engine/client/dll_int/cl_game.c:57-111` while writing this, not reused
from an earlier count in this same document that turned out to have one
wrong: `12` for `cdll_new_exports` was a stray grep artifact, the real
array has 11 entries), and `tools/xbox-client-required-exports.txt` (the
first 37 of those - `cdll_exports` only, the legacy callbacks
`CL_LoadProgs` treats as fatal if any is missing - added on Codex review
round 1, see above). All three are literal transcriptions of
`engine/server/sv_game.c`'s and `cl_game.c`'s own lookups - not new
decisions, and not the ~260-entry entity/HUD lists themselves, which
stay fully generated, never hand-written. Neither invocation passes
`--allow-empty`: a title module producing zero real exports is never a
legitimate outcome for either side, so it must fail loudly, not warn and
continue.

Source of truth: `DLLEXPORT` is `__declspec(dllexport)`, which clang
records in each COFF object's `.drectve` section as `/EXPORT:name`
directives - `llvm-readobj --coff-directives` (present:
`/c/msys64/mingw64/bin/llvm-readobj.exe`) prints them verbatim. **Measured
2026-09-11 on a real `tools/xbox-cxx.sh` object**: a TU with two
`extern "C" DLLEXPORT` functions and two plain ones yields exactly
`Directive(s): /EXPORT:_monster_headcrab /EXPORT:_GetEntityAPI2` - the
non-exported functions are absent, and the i386 `_` prefix is present and
must be stripped (the same convention `gen-library-tables.sh` already
assumes for its `extern void name` declarations). That is the
exact set the real DLL would export, derived from the SDK's own source,
regenerated on every build, and it includes every `LINK_ENTITY_TO_CLASS`
name by construction (~260 server entries). For the CLIENT the rule is
the mirror of X7.1b's allowlist (Codex review round 2): `client/exports.txt`
is `cl_game.c`'s `cdll_exports[]` + `cdll_new_exports[]` names FILTERED
to those the client objects actually export - never the client's raw
`/EXPORT:` set, which also carries the shared weapon sources' entity
classnames (`weapon_crossbow`...) that the engine never looks up on the
client and that X7.1b renames away. The
generator fails nonzero when it finds fewer than the four server API
exports or fewer than the 37 required client exports (`cl_game.c`'s
`cdll_exports[]`), so a broken sweep cannot produce a plausible-looking
half table.

The specialist review flagged mangled `/EXPORT:?...` entries as a
correctness risk (a `DLLEXPORT` without `extern "C"`, invisible to any
by-name lookup) and an earlier draft of this plan over-corrected that
into "the generator rejects any mangled export present" - wrong, and
disproven by this plan's own X7.1b measurement: `crossbow.cpp`, real SDK
source, exports several (`?BoltTouch@CCrossbowBolt@@...`,
`?SUB_CallUseToggle@CBaseEntity@@...`) as ordinary, EXPECTED output of
`EXPORT`-marked C++ methods (`common/Platform.h:66`'s
`DLLEXPORT = __declspec(dllexport)`) - not a mistake, just not something
the engine ever looks up by name. A generator that failed the build on
their mere presence could never generate `client/exports.txt` at all.
Corrected: the generator FILTERS every mangled `/EXPORT:` entry out of
the written `exports.txt` silently (X7.1b already renames the underlying
symbol away regardless, so it was never going to be reachable by that
name past the rename step either) and validates the required-count
thresholds - four server API exports, 37 client exports - against the
UNMANGLED entries only. `dlls/util.cpp:1780-1782`'s `extern "C" _rotr`,
compiled only `#ifndef WIN32`, is the reason `WIN32` must be passed to
every SDK compile regardless (the fork's "NXDK-proof" CMake already does;
`nxdk-cxx` does not on its own) - an unrelated C-linkage trap from the
same review, not a mangled-export case. `xbox-cc.sh`'s staleness check
gains these two files as inputs.
Verification: the generated `server/exports.txt` for `hl` has 251 entity
names + 4 API names, checked against `grep -c LINK_ENTITY_TO_CLASS` of the
same tree - two independent counts that must agree.

**Found in Codex review round 2 (the cap for this PR; verified directly
rather than spent on a third automated round), and real: one of the four
required server API names cannot be produced by `/EXPORT:`-directive
scanning at all.** `h_export.cpp` - the file `GiveFnptrsToDll` is
actually defined in - locally `#undef DLLEXPORT` and redefines it as
`__stdcall` on `WIN32` (`h_export.cpp:28-35`), not
`__declspec(dllexport)`; the real shared-DLL build exports it instead
through a separate `hl.def` module-definition file
(`GiveFnptrsToDll @1`). **Measured 2026-09-11, compiling the real file
with `tools/xbox-cxx.sh`:** `GiveFnptrsToDll` produces NO `/EXPORT:`
directive at all (the object's only two directives are unrelated mangled
C++ methods from the same TU), and its actual linker symbol is
`_GiveFnptrsToDll@8` - `__stdcall` name-decorated (`@` + byte-count of
its two pointer arguments), not the plain `_GiveFnptrsToDll` a naive
cdecl `extern void GiveFnptrsToDll(void);` (what `gen-library-tables.sh`
currently emits) would look for. Directive-scanning alone would silently
produce a `server/exports.txt` missing this hard-required name entirely
(`SV_LoadProgs` fails immediately - `sv_game.c:5260-5266`,
`"missing GiveFnptrsToDll export"`) even though the real compiled symbol
is sitting right there under a different linker name.

Fix, verified end-to-end 2026-09-11: the four API names are a FIXED,
small set the ENGINE itself hard-requires by these exact strings
(`sv_game.c`'s own `COM_GetProcAddress` calls) - not something to derive
from SDK export directives at all, unlike the ~260 entity classnames
which genuinely have no other source. The generator looks each of the
four up directly in `llvm-nm --defined-only --extern-only` (not
`--coff-directives`) against both its cdecl form (`_Name`) and its
stdcall form (`_Name@N`, `N` = total argument bytes - `GiveFnptrsToDll`
takes two 4-byte pointers, `@8`), and for a stdcall match emits the
`extern` declaration with an explicit linker-name override rather than
guessing a calling convention: `extern void GiveFnptrsToDll(void) asm(
"_GiveFnptrsToDll@8");` - GNU/clang inline-asm symbol-renaming, binding
the C name to the EXACT decorated symbol regardless of what convention
produced it. Verified with a real link: a TU declaring `GiveFnptrsToDll`
this way and taking its address, linked against the real compiled
`h_export.obj` - `0` undefined-symbol warnings naming
`_GiveFnptrsToDll@8` (the only warnings present are unrelated CRT/`memcpy`
symbols missing from that minimal two-object test link, already proven
present in the real engine link by every successful build so far). The
other three API names (`GetEntityAPI`, `GetEntityAPI2`,
`GetNewDLLFunctions`) ARE plain `extern "C" DLLEXPORT` in this SDK
(`client.cpp`) and need no such override - the generator only reaches
for `asm()` when a name's cdecl form is absent but a stdcall-decorated
form of it exists.

### X7.3 — VGUI out of `cl_dll`, on the `resonance3d` branches

**Superseded, done differently - see divergence #42.** This section
originally planned porting `hlsdk-portable`'s own `USE_VGUI` discipline
(excluding the 9 `vgui_*.cpp` from the sweep, `#ifdef USE_VGUI` the 27
files that include VGUI headers) - an unverified claim: `hlsdk-portable`'s
real, current source has no `USE_VGUI` switch at all (checked directly
against a fresh clone, 0 hits). What it actually ships, and what this
project vendored instead, is `3rdparty/freevgui` - a real, buildable,
clean-room VGUI1 reimplementation, giving `cl_dll`'s existing, unmodified
`vgui_*.cpp` files (single-player AND multiplayer UI both) a real library
to link against, rather than excluding VGUI-dependent code from the build
at all. No SDK-side changes to `deps/hlsdk` were needed for this -
`VGUI_*.h` stayed exactly as upstream ships them.

One engine-side prerequisite the client module has that the stub never
exercised, from the specialist review: `cl_dll/inputw32.cpp:31-33,148`
drives the gamepad through `SDL_GameController`, so the engine's Xbox
`SDL_Init` (`engine/platform/sdl2/sys_sdl2.c:151-152`, currently
`SDL_INIT_TIMER | SDL_INIT_EVENTS` only) must add
`SDL_INIT_GAMECONTROLLER` before `IN_Init` runs. X6's "gamepad is free"
line is only true once that flag is there; without it the controller
subsystem is simply absent and `IN_Init` finds no device, silently.

### X7.4 — Half-Life first: server, then client, each a boot milestone

0. **X7.1b's collision prototype: PASSED, 2026-09-11, against the real
   384-object client response file (see X7.1b for the full
   measurement).** `0` duplicate with the renamed `crossbow.cpp` pair
   added to `tools/xbox-link-check.sh`'s own `.sweep/link-check-response.txt`,
   13 named duplicates with the same pair unrenamed, export table clean
   (no corrupted entries from the stale-directive issue found and fixed
   in the same pass). **X7.2's `GiveFnptrsToDll` gap, found in the same
   review round and fixed and verified separately (X7.2): directive
   scanning alone cannot produce this required export** (it's
   `__stdcall`-only in `h_export.cpp`, no `/EXPORT:` directive at all,
   real symbol `_GiveFnptrsToDll@8`) - the `asm()` linker-name-override
   fix is verified against the real compiled `h_export.obj`, not yet
   against a full server link (that is `hl_dll`/`dlls` -wide, X7.4 step 1
   itself). The real `tools/` implementation (X7.1's TU manifests, the
   allowlist generator with the API-name fallback, the strip-then-rename
   step) is what X7.4 step 1 now needs; the design is de-risked on every
   point two rounds of review found.

   **`tools/xbox-game-manifest.sh` (X7.1) and a new `tools/xbox-game-sweep.sh`
   built and run for real against the whole `hl` SDK, 2026-09-11 - the
   first real compile measurement of this SDK for Xbox, not a
   three-file prototype.** `xbox-game-sweep.sh` is the compile step the
   manifest tool exists to feed, same shape as `tools/xbox-mainui-sweep.sh`
   (OK/FAIL/TOTAL, `--expect-fail`/`--expect-total` regression gates, a
   zero-attempted hard failure) but reading its TU list/defines/includes
   from the manifest instead of a hand-maintained glob. Server: **102/107
   compile clean.** Client: **67/85 compile clean.** Real, categorised
   failures, not yet fixed (X7.4/X7.3 scope, not this tool's):
   - Server (5): `nodes.cpp`'s `_stat64i32`-based `FileSystem_GetFileTime`
     path (a real Win32-vs-pdclib struct-naming mismatch, same file
     X7.4's writable-rootdir risk already flags); `std::byte`/`std::clamp`
     missing across several files (`<cstddef>`/`<algorithm>` not visible
     under `-ffreestanding -nostdlib`'s reduced standard-library surface);
     `player.cpp`'s `CallOnDestroy` template usage; `client.cpp`.
   - Client (18): the SAME SDL2-header/clang incompatibility found and
     flagged while building the manifest tool's `INCLUDE` derivation
     (`SDL_INOUT_Z_CAP`/`SDL_SCANF_FORMAT_STRING` MSVC SAL macros
     `SDL_stdinc.h` uses) accounts for most of them - `cdll_int.cpp`,
     `input.cpp`, `inputw32.cpp`, `in_camera.cpp`, `tri.cpp`, others;
     `GameStudioModelRenderer.cpp`/`StudioModelRenderer.cpp`/`studio_util.cpp`
     want `<memory.h>`/real `GL/gl.h` (no GL on Xbox - `ref/null`/a real
     Xbox renderer replaces this path anyway, X5); `particleman/*.cpp`
     wants `<memory_resource>`.

   **Found and fixed while running this for real: `xbox-game-sweep.sh`'s
   own error log silently lost data on this Windows/MSYS setup.**
   Reopening the same log file for append (`>> "$LOG"`) once per compile -
   over a hundred separate open/close cycles on one file in quick
   succession - lost roughly half of the real failures' markers and
   compiler output, reproducibly, across two full runs (the fail COUNT was
   always correct, since it comes from each compile's own exit status, not
   the file; only the human-readable detail silently vanished). A
   `--expect-fail`/`--expect-total` gate reading only the count would never
   have caught this - a maintainer reading the log to fix a real failure
   would have found 2 of 5, not 5 of 5. Fixed by opening the log file
   ONCE (`exec 3>> "$LOG"`) before the loop and reusing that file
   descriptor for every compile, closed once after - no repeated
   open/close cycle left for Windows to race. Re-verified: two more full
   107-source server runs, all 5 real failures' markers and full compiler
   output present both times. `--expect-fail 5 --expect-total 107`
   (server) and `--expect-fail 18 --expect-total 85` (client) both pass
   against this measurement; an intentionally wrong `--expect-fail 0`
   correctly reports a regression rather than passing.

   **The log-loss mitigation above reduces but does not eliminate the
   problem - hit again, 2026-09-12, diagnosing these same failures for
   real.** A fresh client sweep's `errors.txt` was missing the
   `=== FAIL: cdll_int.cpp ===` marker (14 shown, 15 real failures) despite
   the persistent-fd fix already being in place - the undercount is rarer
   now, not gone. Worked around it for this session's own diagnosis by
   never trusting the log for "which files failed": for each manifest
   `SOURCE` line, checked directly whether its corresponding `.obj`
   exists in the sweep's output directory (a failed compile never
   produces one), which sidesteps the log entirely and is exactly as
   reliable as the sweep's own OK/FAIL/TOTAL count. `xbox-game-sweep.sh`
   itself is not changed further here; a fully robust fix (per-source log
   files instead of any shared accumulator, matching
   `docs/r3d/lessons.md`'s own recorded advice) is real, still-open
   follow-up work, not blocking - the count this tool's gates rely on was
   never wrong, only the optional detail.

   **Two real root causes found and fixed, 2026-09-12, closing most of
   both counts.** (1) `tools/xbox-cxx.sh` never passed `-std=c++17` -
   measured directly (`clang++ ... -dM -E -x c++ /dev/null`, `__cplusplus`
   prints `201402L`) that this exact target/flag combination's real
   default is C++14, not the C++17 the HL SDK's own build
   (`set_common_properties`'s `cxx_std_17`) requires - class template
   argument deduction (`player.cpp`'s `CallOnDestroy resetIsSpawning{...}`)
   and `std::clamp` are unavailable under C++14, both hard errors. Added
   `-std=c++17` to `xbox-cxx.sh`'s fixed flags (a caller's own `-std=` in
   `"$@"` still wins, same override convention as `-o`) - fixed 4 of the
   5 server failures outright (`player.cpp`, `game.cpp`, `nodes.cpp`,
   `client.cpp`) and 2 of the 18 client ones (`input.cpp`,
   `text_message.cpp`). Re-verified `3rdparty/mainui`'s existing sweep
   (82/82 clean) is unaffected - C++17 is a compatible superset of C++14
   for this project's real code, not just in theory. (2) The remaining
   server failure, `game_shared/filesystem_utils.cpp`, was never really
   about `_stat64i32` vs `struct stat` naming (an earlier pass through
   this file mis-attributed it to `nodes.cpp` and called it a naming
   mismatch) - read `deps/nxdk/lib/xboxrt/libc_extensions/stat.c`
   directly and found its ENTIRE body, `struct stat` included, wrapped in
   `#if 0`: nxdk provides no working `stat()`/`fstat()` at all, on either
   the WIN32 or the POSIX branch this function's stock path could take.
   Fixed on `deps/hlsdk`'s `resonance3d` branch (divergence #36) with an
   `#if defined(NXDK)` branch returning `0`, the exact same safe fallback
   the `_STATIC_ENGINE_LINK` branch immediately above it already uses.

   **Codex review round 1 found that fallback's own claim was false:
   returning `0` from `FileSystem_GetFileTime` does NOT make
   `CGraph::CheckNODFile` rebuild the `.nod` cache - it does the exact
   opposite.** Traced the real call chain rather than trusting the
   inherited comment: with both the `.bsp` and the `.nod` timestamps
   returning `0`, `FileSystem_CompareFileTime` sees them as EQUAL and
   returns `true, iCompare == 0` - `CheckNODFile` (`dlls/nodes.cpp:2621-2642`)
   only rebuilds when `iCompare > 0` or the compare call itself fails;
   equal timestamps read as "the cache is current", silently reusing
   stale AI navigation data - a real bug, not a style nit, and one this
   project's own `_STATIC_ENGINE_LINK`-branch-derived comment had simply
   never traced through. Fixed properly: `FileSystem_CompareFileTime`
   itself now returns `false` unconditionally on `NXDK`, taking
   `CheckNODFile`'s own explicit "comparison failed → rebuild" fallback
   directly, rather than depending on a sentinel timestamp value
   surviving a comparison correctly.

   **Codex review round 2 (the cap) found that "fix" was itself a
   different real bug: rejecting the timestamp comparison outright makes
   `CheckNODFile` reject EVERY `.nod`, even a genuinely valid one shipped
   with the disc.** Read `dlls/world.cpp:641-650` directly: when
   `CheckNODFile` returns `false`, `FLoadGraph()` is never even called -
   the map loader jumps straight to `AllocNodes()`, a full node/routing
   regeneration, unconditionally, on every single map load, real CPU
   cost this hardware can least afford. Round 1 and round 2 are the same
   underlying fact from two directions: nxdk has no real timestamp
   source, so neither "always trust the cache" nor "always reject the
   cache" is actually correct. Resolution, matching Codex's own
   recommendation: removed the `FileSystem_CompareFileTime` `NXDK`
   branch entirely - its original, unconditional logic already does the
   right thing once `GetFileTime` alone is fixed (both times report `0`,
   read as equal, `CheckNODFile` proceeds to `FLoadGraph()`), and
   `FLoadGraph()`'s OWN independent checks (missing file, wrong
   `GRAPH_VERSION`) are the real, already-correct gate against a
   genuinely bad graph. True staleness (valid format, but the `.bsp`
   changed since) is not caught at Xbox runtime by this - explicitly
   deferred to asset-packaging time, where real timestamps or hashes
   exist to enforce it; a stale-but-format-valid graph degrading AI
   navigation, not crashing anything, is the accepted, documented
   tradeoff. Re-verified after both rounds: `filesystem_utils.cpp` still
   compiles 0 errors, the full server sweep still reports 107/107, and
   `CheckNODFile`'s logic was hand-traced against the final code to
   confirm it now proceeds to `FLoadGraph()` rather than rejecting
   everything.

   **Result: server compiles 107/107 - X7.4 step 1's real, complete
   object set now exists for the first time.** Client was 15/85 at this
   point (the SDL2-header/clang SAL-annotation incompatibility, plus a
   `GL/gl.h`/`<memory.h>`/`<memory_resource>` want from the studio-model
   rendering path) - **fixed down to 77/85, 2026-09-12, five real root
   causes closed (divergence #39)**, all downstream of one fact measured
   directly: this project's own clang invocation defines `_MSC_VER`
   (`1950`), which activates real-MSVC-only branches in
   `deps/hlsdk/external/SDL2` that were never reachable before. Fixed:
   the SAL-annotation macros (`compat/xbox/sal.h`); a missing legacy
   `<memory.h>` (`compat/xbox/memory.h`); `SystemParametersInfo`
   (`compat/xbox/xbox_prelude.h`/`xbox_win32_ext.c`, a real, correct
   unconditional `FALSE` - Xbox has no OS-level mouse-acceleration
   setting to query or set at all); and a `uintptr_t` redefinition
   conflict plus a `_m_prefetch` builtin collision, BOTH fixed at the
   vendored SDL2 source itself (`SDL_config_windows.h`/`SDL_endian.h`/
   `SDL_cpuinfo.h`, each gaining a narrow `&& !defined(NXDK)` on the
   specific guarded block responsible) after Codex review corrected two
   successive drafts that instead used global compiler flags
   (`-D_UINTPTR_T_DEFINED`/`-D__PRFCHWINTRIN_H`) - a flag predefining a
   guard macro affects every translation unit project-wide, not just the
   one vendored block it was meant for; see `tools/xbox-cxx.sh`'s own
   header comment for the full story. Zero regressions (server 107/107,
   `3rdparty/mainui` 82/82). The remaining 8 were then confirmed (each
   compiled standalone) to be exactly two categories: `<memory_resource>`
   (7 files) and `GL/gl.h` (1 file) - **the `<memory_resource>`
   categorisation as "genuinely separate, larger work" was WRONG,
   corrected the same day running the real client isolation for the
   first time.** `cl_dll/cdll_int.cpp` - the client module's own main
   entry point, providing every REQUIRED export the engine's
   `COM_GetProcAddress` lookups hard-need (`HUD_Init`/`HUD_VidInit`/
   `HUD_Redraw`/21 more) - transitively but genuinely, load-bearingly
   includes `CMiniMem.h` (it really uses `IParticleMan`, not an
   incidental pull-in): a real isolation attempt without a working
   `std::pmr` reported all 22 required exports entirely missing, a real
   blocker for X7.4 step 2, not a deferrable want. Fixed with
   `compat/xbox/memory_resource` (divergence #40) - not a full,
   standards-compliant `std::pmr` (real thread-safety options,
   `upstream_resource` chaining, chunked-pool allocation performance
   remain genuinely deferred), but a correct, minimal implementation of
   the exact three methods (`allocate`/`deallocate`/`release`) `CMiniMem`
   actually calls (the ONLY consumer of `std::pmr::` anywhere in the
   client SDK, verified by grep), as a tracked list of individually
   aligned `::operator new` blocks. **Result at this point: the real
   `hl` client sweep went from 77/85 to 84/85**, zero regressions
   (server 107/107, `3rdparty/mainui` 82/82).

   **Found in Codex review, round 1: the one remaining failure
   (`GL/gl.h`, `particleman/CFrustum.cpp`) was NOT safely dismissable as
   "non-required" either.** Two OTHER real, compiled files
   (`IParticleMan_Active.cpp`, `CBaseParticle.cpp`, confirmed by grep)
   genuinely call `CFrustum`'s own methods - a real link without it
   would fail on 4 undefined symbols the isolation tool's own
   required-exports check does not catch (an internal cross-module
   dependency, not an engine-facing export). Reading `CFrustum.cpp`
   itself found the earlier "needs real rendering, X5's job" framing was
   ALSO wrong: its own header comment says exactly why it includes
   `<GL/gl.h>` ("the triangle API uses OpenGL constants for certain
   functions") - it calls no real GL function at all, only passes
   `GL_PROJECTION_MATRIX`/`GL_MODELVIEW_MATRIX` as plain `int`s to the
   SDK's own `gEngfuncs.pTriAPI->GetMatrix()`. Fixed with two `#define`s
   (`compat/xbox/GL/gl.h`, divergence #40, values cross-checked against
   this repo's own vendored SDL2 header AND the real `ref/gl` reference
   renderer's own header, not guessed).

   **Result: the real `hl` client sweep now reports 85/85, zero
   failures** (was 70/85 at the start of this whole investigation), zero
   regressions (server 107/107, `3rdparty/mainui` 82/82). **A fresh real
   client isolation now reports "isolated cleanly - 85 object(s), 5657
   symbol(s) renamed, 0 leaked"** - genuinely, fully link-ready, not
   partial. X7.4 step 2 (the real client link attempt) is now genuinely
   attemptable for the first time - not yet attempted.

   **`tools/xbox-game-link-check.sh` built and run for real, 2026-09-12 -
   X7.4 step 1's actual real-link attempt, the first time this project has
   linked the real `hl` server SDK against the real engine (not a stub, not
   a 2-object prototype).** Reuses three already-built pieces rather than
   re-deriving them: `tools/xbox-link-check.sh`'s own response file (the
   real, currently-clean engine+third-party+menu+`filesystem_stdio`+`ref_null`
   set, plus the still-stubbed client half), `tools/xbox-game-isolate.sh`'s
   fresh server isolation, and `tools/gen-library-tables.sh` fed the
   isolated `exports.txt` instead of the stub's. Verifies the baseline link
   is clean first (refusing to build on an unverified foundation), then
   swaps the stub server's two objects for the isolated set plus a freshly
   generated `generated_lib_server_exports.c`, and links.

   Getting a trustworthy first result took real debugging, not a clean
   first try - three genuine bugs, none of them in the SDK:
   - `tools/xbox-link-check.sh`'s own mainui-manifest check (`sed -n
     '...FAIL=...' "$MANIFEST"`) came back EMPTY when run right after
     compiling 186 third-party objects, reproducibly, 5 times in a row -
     a retry loop around the same `sed` call did not fix it, but the same
     script's own `cat "$MANIFEST"` a few lines later, in the SAME failing
     run, read the file correctly every time. Fixed by reading the file
     once with `cat` into a variable and extracting both fields (plus a
     SECOND, separate `sed` call further down, `OK=`, found only once the
     first fix let execution reach it) with bash's own `[[ =~ ]]` regex,
     never depending on `sed` for this again.
   - `eval "$(deps/nxdk/bin/activate -s)"` (documented elsewhere as needed
     before `nxdk-link`) measurably REPLACES `$PATH` on this Git-Bash/MSYS2
     setup rather than extending it - `lld` resolved correctly immediately
     before that call and "command not found" immediately after it, same
     shell. `tools/xbox-link-check.sh` itself never calls `activate` at
     all; removed the call, relying on the same plain `MSYS2_BIN` `PATH`
     export every `tools/xbox-*.sh` script already uses.
   - Once linking actually ran: `1 duplicate symbol, 0 undefined` -
     `CreateInterface`. A genuine `/EXPORT:` directive in the `hl` server
     target (`public/interface.cpp`, restored for
     `Sys_LoadModule`/`Sys_GetFactory`'s sake, divergence #35/#36), correctly
     kept unrenamed by X7.1b's own literal "the objects' own `/EXPORT:` set"
     rule - but nothing else in the server target references it, and it
     collides with `filesystem_stdio`'s own real `CreateInterface`
     (divergence #30) once both share a link. The engine never resolves
     "CreateInterface" for the "server" role specifically, only for
     "filesystem_stdio" - keeping it was the generator being literal about
     "every real `/EXPORT:`", not X7.1b's own stated principle ("the
     allowlist is what the ENGINE resolves for THIS module"). Fixed with a
     new, narrow `tools/xbox-gen-exports.sh --exclude <file>` (mirroring
     the existing `--allow`/`--require` file-based design) and a new
     checked-in `tools/xbox-server-exclude-exports.txt` (one line,
     `CreateInterface`) - a measured, specific exception, not an invented
     heuristic.

   **Result after all three fixes: `0 duplicate, 1 undefined`.** The one
   real remaining undefined symbol was `___RTDynamicCast` - the MSVC-ABI
   RTTI runtime function clang emits for `dynamic_cast`. Confirmed real,
   not decorative: `deps/hlsdk/dlls/client.cpp` (5 call sites) and
   `player.cpp` (1) use `dynamic_cast<CBasePlayer*>(CBaseEntity::Instance(pev))`-
   shaped downcasts from a genuine base-class pointer, real runtime type
   verification, not a redundant self-cast that could simply be deleted.
   Checked nxdk's own tree directly (`llvm-nm` against every prebuilt
   `.lib`, source grep across `libcxx`) - no implementation of
   `__RTDynamicCast` exists anywhere in this toolchain; nothing built on
   nxdk before this has ever needed real C++ RTTI. Tracked at the time as
   the baseline: `tools/xbox-game-link-check.sh hl --expect-undefined 1`.
   `docs/r3d/parked-undefined-symbols.txt` (the engine-level baseline,
   `--client-main --xbe` with the stub game module) was found genuinely
   empty in the same pass - the 32 previously-tracked networking symbols
   (`Netchan_*`/`SteamBroker_*`) are ALL resolved in the current tree; that
   file had simply gone unmeasured for a while under the `sed` bug above,
   not silently regressed.

   **`___RTDynamicCast` closed the same day (divergence #37) - real MSVC-ABI
   RTTI support was never actually needed.** Implementing it from scratch
   (matching clang's own `__RTTICompleteObjectLocator`/`TypeDescriptor`
   layout) would have been real, error-prone low-level ABI work - but
   this SDK already has an established, in-use idiom for exactly this
   problem: `CBaseEntity::MyMonsterPointer()`/`MyTogglePointer()`/
   `MySquadMonsterPointer()`, a virtual defaulting to `NULL`, overridden
   to `return this` only in the target type, giving an RTTI-free safe
   downcast wherever the class hierarchy is single and non-virtual (which
   it is confirmed to be, `grep`-checked across every `dlls/*.h`, for both
   `CBasePlayer` and `CBasePlayerWeapon`'s full inheritance chains).
   Extended that exact pattern with two more virtuals -
   `MyPlayerPointer()` (new on `CBaseEntity`, overridden in `CBasePlayer`)
   and `GetWeaponPtr()` (already existed on `CBasePlayerItem`, pulled up
   one level to `CBaseEntity` so it is reachable from a bare
   `CBaseEntity*`) - and replaced all 6 real `dynamic_cast` call sites
   with them; each site's original null-safety around `CBaseEntity::
   Instance()` possibly returning `nullptr` (which `dynamic_cast` handles
   for free but a raw virtual call does not) was preserved explicitly.

   **Codex review round 1 found the first version unconditional - it
   changed `CBaseEntity`'s vtable and all 6 call sites' behaviour on
   every platform this SDK builds for, not just Xbox, a real ABI/
   behaviour divergence mislabelled as the cheapest form.** `deps/hlsdk`'s
   `resonance3d` branch is not Xbox-exclusive, and `dynamic_cast` works
   correctly everywhere else - fixed by putting every addition and
   call-site edit behind `#if defined(NXDK)`, `#else` restoring the exact
   original `dynamic_cast` code. Verified by diffing against the
   pre-fix commit: every non-NXDK line is now byte-identical to what was
   there before this divergence (this repo's own tooling has no PC build
   path for `deps/hlsdk` to compile-verify against directly, so the diff
   check is the real, available verification). Re-verified the NXDK path
   is unaffected by the guards.

   Verified end to end, not just compiled: the full server sweep is still
   107/107, and `tools/xbox-game-link-check.sh hl --expect-undefined 0`
   now reports **`0 duplicate, 0 undefined`** - a real, valid `.exe`
   produced (`nxdk-link` only ever produces one once nothing remains
   unresolved), the first time this has happened for the real `hl` server
   SDK. `docs/r3d/parked-server-undefined-symbols.txt` is now empty, same
   as the engine-level baseline. X7.4 step 1's own exit gate - "0
   duplicate symbols, and the undefined symbol set exactly matches the
   checked-in baseline" - is met with the checked-in baseline being
   truly, fully empty, not a tracked, accepted gap.

   **Codex review round 1 found two more real gaps, both fixed and
   re-verified before round 2.** (1) `--expect-fail` is optional (an
   exploratory run needs no baseline), but a BARE invocation with no
   flags at all silently exited 0 even with every source failing - a
   broken toolchain producing 107/107 failures would look identical, to
   anything checking only the exit code, to a clean run. Fixed: any
   failure without an explicit `--expect-fail` acknowledging it is now
   fatal by default; verified a bare run against the real 5-failure
   server baseline now correctly refuses to exit 0, naming
   `--expect-fail 5` as the way to accept it, while the same run WITH
   the correct `--expect-fail`/`--expect-total` still passes clean. (2)
   the stale-`.obj` cleanup (`rm -f`) ignored its own exit status, so a
   genuine removal failure (a locked file, plausible on the documented
   Windows/MSYS setup) would let the script compile straight ahead as if
   the directory were already clean, with a stale object from an earlier
   run able to survive into "this run's output". Fixed: the cleanup's
   exit status is checked, and the directory is re-verified empty of
   `.obj` files afterward rather than trusting `rm`'s exit code alone -
   re-verified both targets still pass against their real baselines
   after this change.

   **Codex review round 2 (the cap) found the same stale-manifest class
   of bug the manifest tool itself had already been fixed for, this time
   in the sweep's own `manifest.txt`.** `OK=$OK FAIL=$FAIL TOTAL=$TOTAL`
   was written right after the `TOTAL==0` check but BEFORE
   `--expect-total`/`--expect-fail` ran - so a run one of those gates
   REJECTED (a source-set drift to, say, 108 files, all compiling clean)
   still published a clean-looking `manifest.txt` (`FAIL=0 TOTAL=108`)
   before exiting nonzero, exactly the shape a downstream consumer
   modelled on `tools/xbox-link-check.sh`'s own manifest-trust pattern
   would accept despite the gate's rejection. Per the project's 2-round
   cadence cap, fixed and independently re-verified: `manifest.txt` is
   now written only after every applicable gate has passed without
   exiting (it was already removed at the very top of the script, with
   the round-1 fix confirming that removal actually happened, so a
   rejected run now leaves NO manifest.txt at all). Verified exactly the
   scenario Codex named: a deliberately wrong `--expect-total 999`
   against the real 107-file server sweep correctly fails and leaves no
   `manifest.txt` behind (a pre-existing one from an earlier accepted run
   confirmed gone, not just "never created"); the correct
   `--expect-fail 5 --expect-total 107` re-run afterward still publishes
   `OK=102 FAIL=5 TOTAL=107` as before.
1. **`hl` server module links, stub retired.** Sweep `dlls/` + `pm_shared/`;
   generate `server/exports.txt`; link with `--client-main`. Exit gate is
   the same three lines as always: `0 duplicate, 0 undefined`, a real XBE,
   the manifest clean.

   **The link gate is fully real and measured now, INCLUDING the actual
   XBE - `0 duplicate, 0 undefined`, `tools/xbox-game-link-check.sh hl
   --expect-undefined 0 --xbe <path>` (the tool gained a `--xbe` mode
   this same pass, mirroring `tools/xbox-link-check.sh`'s own
   `-force:unresolved` + `cxbe` sequence against THIS tool's own
   isolated-server response file) produces a real, `XBEH`-valid `.xbe`,
   packaged onto a real ISO (`extract-xiso`, the real retail `valve/`
   tree so `delta.lst` exists) and booted for real in xemu.**

   **The first real boot attempt crashed immediately, twice in a row -
   both real, both found and fixed, both documented as divergence #38.**
   Not the expected "CL_Init: missing essential exports" message at all:
   `assert(nullptr != g_pFileSystemModule)` in `FileSystem_LoadFileSystem`
   (`deps/hlsdk/game_shared/filesystem_utils.cpp:197`), decoded straight
   from the halted guest's own memory (`tools/xemu-debug.py`, the real
   expression/function/file/line strings, not inferred from the symptom)
   - `Sys_LoadModule`'s stock body is a real `LoadLibrary()` call, which
   NXDK has never had. A first fix (route `Sys_GetFactory` to this same
   file's own local `CreateInterface`) compiled and linked clean but was
   itself wrong - caught by a SECOND real boot, not by inspection: X7.1b's
   own isolation renames every self-reference to `CreateInterface` within
   the server's own isolated set, so that fix always resolved to the
   server's own unrelated local registry, not filesystem_stdio's real
   one. Fixed properly by reusing the exact mechanism the ENGINE itself
   already uses successfully for this (`COM_LoadLibrary`/
   `COM_GetProcAddress`, proven since divergence #31) - see divergence
   #38 for the full story. Re-verified: full server sweep still 107/107,
   still `0 duplicate, 0 undefined`. **What the debugger CAN confirm about
   the boot: it is no longer repeatedly re-trapping the same two asserts
   the pre-fix build hit every time within seconds** - attaching and
   issuing a genuine `continue` produced no stop reply for 90+ seconds,
   where the pre-fix build always re-hit its crash almost immediately.
   **Found in Codex review, round 2: that observation does NOT, on its
   own, mean the boot is "healthy" or making real progress** - an
   infinite loop, a deadlock, or a legitimate kernel idle-wait would look
   identical to this exact signal (no stop reply, ever), so it only rules
   out "still crashing on the same bug as before," nothing more (see
   `docs/r3d/lessons.md`'s corrected entry).

   **CONFIRMED, 2026-09-12, by a real breakpoint hit with the actual
   caller and message decoded from the guest's own stack - not inferred
   from a `continue` timeout's absence-of-evidence, and not just "some
   call reached `Sys_Error`" either.** Two real gaps in a row here, both
   found by Codex review, both fixed the same rigorous way:

   *Round 2 (on an earlier draft): a `continue` timeout alone does not
   mean the boot is healthy* - see the corrected entry in
   `docs/r3d/lessons.md`; the fix was a real breakpoint at `Sys_Error`'s
   resolved address instead of inferring anything from silence.

   *Round 1 (on the breakpoint version itself): hitting `Sys_Error`
   alone does not prove WHICH caller or WHICH error* - `Sys_Error`
   (`engine/common/system.c:388`) is a shared fatal-error sink with
   multiple real callers before `CL_Init` ever runs (filesystem load
   failures, a missing-gfx check in `Host_Init`), so a bare "the
   breakpoint hit" is exactly the same class of unsupported inference
   the round 2 finding already corrected once. Fixed properly: at the
   SAME breakpoint hit (`tools/xemu-debug.py break 0xff180`, cdecl entry,
   before any prologue clobbers the arguments), read `[ESP+0]` (the
   return address - the actual CALLER) and `[ESP+4..+12]` (`Sys_Error`'s
   own `const char *error, ...` varargs) directly off the guest's stack.
   Return address `0xb3c73` resolves (fresh `-map:`) to `_Host_Error +
   0x63` - not any of the earlier, filesystem-load-time callers, exactly
   `CL_Init`'s own real failure path (divergence #34's traced chain).
   The format string was `"%sInit: %s"` with the two varargs reading
   `"Host_Error"` and `"can't initialize client: missing essential
   exports\n"` - assembling to the EXACT message divergence #34 already
   established as this milestone's real text,
   **`Host_ErrorInit: can't initialize client: missing essential
   exports`**, word for word, decoded directly from guest memory, not
   assumed from the symptom.

   *A further real gap, found before this reached Codex review at all
   (matching round 1's own critique): decoding `CL_Init`'s real failure
   does not, on its own, prove `SV_LoadProgs` itself succeeded first* -
   `SV_InitGame`'s own return value is discarded (`engine/server/
   sv_main.c`), and `Host_Init` calls `CL_Init` regardless, so a FAILED
   server load followed by this exact same client failure would produce
   identical evidence. Closed with the same technique, aimed at the real
   HL SDK's own `GetEntityAPI2` (`deps/hlsdk/dlls/cbase.cpp`, linked in
   as `_GetEntityAPI2`, XBE-virtual `0x2b9fb0`) instead of the ambiguous
   generic sink: a breakpoint there hit, with its caller (read the same
   way, off the stack at entry) resolving to `_SV_LoadProgs + 0x384` -
   the real SDK function, called from the real place. Its actual return
   value needed one more step: `GetEntityAPI2`'s own body calls `memcpy`
   internally (filling the big `DLL_FUNCTIONS` struct), so a fixed
   single-step count landed mid-`memcpy`, not past the real return - the
   robust fix was a SECOND breakpoint at `SV_LoadProgs`'s own resume
   point (`0x1208f4`, the return address already captured from the first
   breakpoint's stack read - the exact instruction after `call
   GetEntityAPI2`, regardless of what the callee does internally). That
   hit too, `EAX = 1` - **`GetEntityAPI2` genuinely returned `TRUE`.**
   `SV_LoadProgs` really did complete successfully against the real
   `GetEntityAPI2`, not merely reach a code region that also runs on
   failure, and `CL_Init` really did then reach and fail its
   client-module load exactly as expected, for the real reason expected
   (the client module is still `stub_game/client`, X7.4 step 2's own
   job). The network log channel
   still produced no output at all across every attempt (the SAME
   pre-existing, already-documented unreliability, not a new symptom -
   `SV_SysError` calls `Log_Printf` before anything else, so this
   channel is now confirmed silent even for a message definitely,
   verifiably sent and fully decoded). **X7.4 step 1 is complete**: link
   gate met (`0 duplicate, 0 undefined`, a real XBE), and the boot
   milestone independently confirmed by direct execution evidence - the
   exact caller, the exact message - not the log channel this bullet
   originally named; that channel's own unreliability remains real,
   separately open, unrelated-to-and-not-blocking this milestone.
   Expected boot (headless,
   `ref/null`, full `valve/` on the disc so `delta.lst` exists):
   `SV_LoadProgs` completes against the REAL `GetEntityAPI2` (every
   `DLL_FUNCTIONS` member real, the stub's six-of-forty gone), then
   `CL_Init` stops at "can't initialize client: missing essential exports"
   - now confirmed by direct execution evidence rather than the log
   channel, since the channel itself is confirmed unreliable even for a
   message provably sent. That confirmation IS the milestone: it proves
   100+ real server translation units link and initialise on the
   console. **And the first memory number is
   taken HERE, at entry, not after a map** (specialist review): the wall
   is already built by the engine's own statics, not by the SDK.
   `.sweep/xbe-buildfix.map` for the current client XBE: `.text` 3.37 MB,
   `.bss` 13.1 MB - and lessons.md's `info mem` shows every XBE section
   resident (`0x10000-0x103d000`), so that is 16.5 MB gone before
   `main()`. Owners: `cl` 3.48 MB, `sv` 2.44 MB, `net` 1.64 MB,
   `mod_known` 1.6 MB, `SV_WriteEntitiesToClient.frame_ents` 697 KB,
   `s_knownSfx` 655 KB, two `gpMove` statics 325 KB each. Retail `hl.dll`
   is ~648 KB text + 118 KB data, `client.dll` ~312 + 272 KB, `opfor.dll`
   ~1.0 MB text; libc++ adds tens of KB for `<string>`/`<vector>`, not
   MB. Measure with `MmQueryStatistics` (`deps/nxdk/lib/xboxkrnl/
   xboxkrnl.h:3333`) at `Host_InitCommon` entry and again after
   `SV_LoadProgs`; those two numbers are X8's opening brief. Don't route
   this through the network log channel and trust its silence either
   way - step 1's own boot confirmation (above) found it produces no
   output even for a message definitely, verifiably sent (`SV_SysError`'s
   `Log_Printf`, confirmed reached via a real breakpoint hit). Read the
   value directly with the debugger instead (a breakpoint at the right
   call site, then `mem`/`regs` on the return value), the same real,
   conclusive technique that closed step 1's own verification gap.
2. **`hl` client module links.** Sweep `cl_dll/` (no VGUI), generate
   `client/exports.txt`, link. Expected boot: `CL_Init` passes, `UI_LoadProgs`
   loads the menu (X6's `mainui`, already linking; its manifest must be
   clean or the link refuses), `Host_Frame` runs. Verify with
   `tools/xemu-debug.py`: `regs` in the kernel idle-wait between frames,
   `mon "info registers"` `HLT=0`, and the log channel carrying `Con_Printf`
   traffic from the frame loop - three independent signals, not one.

   **First real combined link (`tools/xbox-game-link-check.sh hl
   --include-client`, divergence #41): 0 duplicate, 527 undefined.** The
   real, isolated server (107 objects) and real, isolated client (85
   objects) linked together for the first time in this project's history
   hit one genuine collision first - the engine's own `IN_ActivateMouse`/
   `IN_DeactivateMouse`/`IN_MouseEvent` (real, externally-linked dispatcher
   functions, `engine/client/input/input.c`) share a name with the
   client's own required, protocol-mandated exports of the same name
   (`tools/xbox-client-required-exports.txt`), harmless on every other
   platform (a separate DLL per module) but a real duplicate-symbol error
   on Xbox's one static binary. Fixed by renaming only the engine's linker
   symbol (`asm("Host_IN_ActivateMouse")` etc., `#if XASH_XBOX`-guarded in
   `engine/client/input.h`), zero call-site changes. With that fixed, the
   real remaining gap is `deps/hlsdk/utils/vgui` - headers plus a prebuilt
   win32 `.lib`, no buildable source - all 527 undefined symbols are VGUI1
   (`vgui::Panel`/`Scheme`/`Color`/`Font`/etc.). **VGUI1 is the next
   concrete blocker for this step. Correction, same day: the sentence
   originally here ("the real, portable, buildable fix is populating
   `3rdparty/vgui_support`") was wrong, and disproven directly rather than
   left standing** - checking that submodule out (and its own nested
   `vgui-dev` submodule) found BOTH are headers-only plus prebuilt binaries
   too (win32/linux/macos this time, still none buildable, still none
   Xbox), because Valve never publicly released VGUI1's actual
   implementation source at all; `vgui_support` itself is engine-side
   glue that links against a real VGUI1 build, not a VGUI1 replacement.
   **A second correction, same investigation: X7.3's own text below
   (`hlsdk-portable`'s `USE_VGUI` discipline) was ALSO an unverified,
   outdated claim** - a fresh clone of `hlsdk-portable`'s real, current
   source has zero `USE_VGUI` occurrences anywhere. Parked as
   `docs/r3d/parked-game-undefined-symbols.txt` (the real, measured,
   reviewed 527-symbol set) so this stayed a tracked, reproducible
   baseline rather than a silently-accepted gap while the real fix below
   was found and built.

   **The real fix (divergence #42): `3rdparty/freevgui`, FWGS's real,
   buildable-from-source, clean-room VGUI1 reimplementation** - what
   `hlsdk-portable` actually ships today, in place of the `USE_VGUI`
   switch this document originally (wrongly) described. Public-API-
   compatible with Valve's original headers (`deps/hlsdk/utils/vgui/
   include/VGUI_*.h` stay unmodified - only the library backing those
   real, unrenamed symbols changes), with a `platform/xash3d-fwgs`
   static-link backend already purpose-built for this exact engine,
   whose `InitVGUISupportAPI()` entry point matches the real, existing,
   unmodified by-name lookup `engine/client/vgui/vgui_draw.c`'s own
   `VGui_LoadProgs` already performs on the client module. `tools/xbox-
   freevgui-sweep.sh`: 34/38 clean on the first real attempt, 39/39 after
   two missing include paths and one genuine upstream API-drift bug
   (`vguiapi_t::SetPaintOffset`, a real field this fork's vendored
   `engine/vgui_api.h` predated - added correctly, with a documented
   no-op `VGUI_SetPaintOffset` since no reference renderer this fork has
   does real 2D drawing yet for a paint offset to matter to). With
   `--include-freevgui` added to `tools/xbox-game-link-check.sh`, the
   real combined link went from **527 undefined to 4** on the first
   attempt (three missing `vgui::App` overrides, fixed with a new,
   submodule-untouched `compat/xbox/freevgui_app.cpp`), then to **0
   duplicate, 1 undefined** once fixed. The one remaining symbol,
   `___RTDynamicCast`, was first left as a tracked, deferred gap -
   divergence #37's own RTTI-avoidance technique (a safe-downcast virtual
   on the base class) is NOT safely portable to freevgui's `Panel`/
   `LayoutInfo`, which deliberately bit-match Valve's original vtable
   slot order so that `cl_dll`'s already-compiled, isolated objects
   (compiled separately, against Valve's real header) call through the
   correct vtable offsets; adding a new virtual there would silently
   shift every later slot, a memory-corruption-class risk `panel.h`'s own
   `CHECK_STRUCT_SIZE` guard does not catch (it checks per-instance size,
   not vtable slot count).

   **Closed for real, same investigation, divergence #43: `___RTDynamicCast`
   is not actually MSVC RTTI in full - clang already emits every real
   type descriptor, only the runtime walker was missing.** A genuine,
   clean-room `__RTDynamicCast` (`compat/xbox/msvc_rtti_dynamic_cast.cpp`),
   scoped to the non-virtual inheritance this codebase's real call sites
   actually need, verified against real MSVC 32-bit `dynamic_cast` itself
   (a ground-truth test harness built with real `cl.exe`, 6 real cases
   including a genuine nonzero-offset multiple-inheritance cross-cast,
   all byte-identical to the real compiler's own results) - not verified
   by "it links" alone. With it, **the real combined link now reaches 0
   duplicate, 0 undefined** and produces a real linked output for the
   first time ever with server, client, and freevgui all real together.
   X7.4 step 2's real link-only exit gate is met. See divergence #43 for
   the full account.

   **A real `--xbe` build and real boot attempt followed, on real retail
   `valve/` assets packaged into a real XISO and launched under xemu -
   first time ever with server, client, and freevgui all real
   together.** Verified the same rigorous way step 1's own boot was
   (`tools/xemu-debug.py`, real breakpoint/stack evidence, not a
   `continue`-timeout or log-channel silence). Along the way, a real bug
   in the tool itself produced several false "the breakpoint never
   fires" readings - see `docs/r3d/lessons.md`'s entry on
   `_handshake` for the full account - fixed before trusting any
   further result from it. With the fixed tool: the guest is confirmed
   genuinely alive (never `HLT`, real varied EIPs inside genuine engine
   code, e.g. `_memset`/`__Mem_Alloc` activity), the breakpoint
   mechanism itself is confirmed working (an immediate, exact hit with
   correct single-step progression at a known-hot address), but a
   breakpoint at the real, map-resolved `Host_Frame` address never fired
   across 150+ real, uninterrupted seconds of continue time on that
   attempt. Every async pause landed at the exact same instruction - the
   Xbox kernel's own idle/ready-queue-poll loop (`sti; nop; nop; cli;
   cmp ebp,[ebp+0]; je ...`). **Corrected, found by following the trail
   further rather than stopping at that observation: this was NOT a
   thread genuinely stuck on an unsignaled kernel wait, as an earlier
   version of this section concluded.** A `usbh_pooling_hubs()`
   micro-benchmark (single-stepped, 20 calls in 0.07s) that looked
   healthy turned out to be misleading in the other direction too - the
   real bottleneck was self-inflicted: several throwaway diagnostic
   scripts across this investigation each set a software breakpoint via
   the raw GDB `Z0` packet and then exited without clearing it (`z0`),
   and those breakpoints persist on the QEMU target across BOTH
   reconnects and `system_reset` (a real, useful, now-documented xemu
   gdbstub property, not a bug in it) - so later scripts kept re-hitting
   stale, forgotten breakpoints from earlier ones and misreading the
   result as "stuck here." Once every address this investigation had
   ever armed was swept and explicitly cleared, `SDL_XBOX_JoystickInit`
   (nxdk's own real, intentional ~500ms USB-controller-enumeration-
   settle loop, `deps/nxdk/lib/sdl/SDL2/src/joystick/xbox/
   SDL_xboxjoystick.c:195-199`) was confirmed to return normally in
   ~2.2s, exactly as designed - never the blocker at all.

   **The real blocker, found by tracing the actual `quit` command's own
   call chain** (`Cmd_ExecuteString`→`CL_Quit_f`→`Sys_Quit("command")`,
   firing reliably ~2.5-3.3s into every attempt, restarting the whole
   `Host_InitCommon` sequence from scratch each time - not a crash,
   `Sys_Main`'s own C code never loops, so something outside it, likely
   nxdk's own exit handling, was relaunching the XBE): `cdll_int.cpp`'s
   `Initialize()` calls `Sys_Quit` whenever `CL_InitClient()` returns
   false, and `CL_InitClient()` was returning false because
   `UTIL_IsValveGameDirectory()` - a real, deliberate HL SDK safety
   check meant to stop a mod's compiled DLL from running inside a real
   Valve game's own retail install - correctly, as designed, refused to
   proceed the moment it saw our disc's real retail Half-Life assets
   sitting in a directory named `valve`. **Fixed in `deps/hlsdk` (this
   fork's own owned submodule, direct commit, not a patch) - see
   divergence #44** for the full account, including live verification of
   the actual compiled machine code and the re-tested boot result: the
   `quit`-at-~2.5s is gone, and the boot now demonstrably advances
   further, reaching `R_Init()`'s real renderer-selection code before
   erroring with "Can't initialize any renderer" - a new, later, well-
   understood, separate gap (this milestone's headless `ref_null` needs
   `r_refdll "null"` set in `video.cfg`; the real retail `video.cfg` this
   boot-test disc carries never needed to set it, since real Half-Life
   always has a real GPU renderer to fall back to).

   **Both fixed, and `Host_Frame` reached for the first time ever in
   this fork's history - see divergences #45 and #46 for the full
   account.** `R_Init()` gained a real, Xbox-only last-resort attempt to
   load `ref_null` explicitly once every other option has failed
   (divergence #45), which unblocked renderer selection but immediately
   exposed the next real failure: `R_Init_Video()`'s own very first SDL
   call lazily auto-init's SDL's video subsystem, reaching nxdk's real,
   currently-compiling `SDL_xbvideo.c` driver and hitting its own
   unconditional `assert(0)` - exactly the call chain (`CL_Init` →
   `VID_Init` → `vid_sdl2.c` → `SDL_CreateWindow`) divergence #14
   already anticipated back when no real Xbox binary existed to reach
   it, deliberately left undecided between its two named options.
   X7.4, with a real running boot, is where that decision actually
   needed making: mirrored `CL_Init`'s own established
   `Host_IsDedicated()`-equivalent short-circuit, narrowed to video
   only, matching this milestone's own headless-boot goal rather than
   committing to a real video-mode default that belongs to X5's own
   planned `VIDEO_XBOX` backend (divergence #46).

   With both fixes together, live-verified, not guessed: the boot
   demonstrably reaches `Host_Frame` (`Host_RunFrame` → `Host_Frame`,
   confirmed by breakpoint hit and by the real caller resolving
   correctly) - **this milestone's own actual boot exit criterion, not
   just the link-only gate.** A single-step trace of `Host_Frame`'s own
   first ~400 real instructions shows entirely normal per-frame
   processing (`Platform_DoubleTime` → `Con_DPrintf` →
   `Host_InputFrame` → `IN_CheckMouseState`) before hitting the NEW
   blocker this same live boot then found, reproducibly: a real kernel
   bugcheck (`KeBugCheckEx(0x1E` `KMODE_EXCEPTION_NOT_HANDLED,
   0xC00000FD` `STATUS_STACK_OVERFLOW, ...)`, decoded directly off the
   kernel's own bugcheck stack frame) a short time after entering
   `Host_Frame`'s real logic - a genuine stack overflow, not yet
   root-caused. **X7.4 step 2 is therefore still NOT closed - the boot
   does not yet SUSTAIN running past its first real frame - but this is
   the furthest this fork has ever reached, by a wide margin**: the
   link-only exit gate is met, `CL_Init` passes, and `Host_Frame` itself
   is confirmed reached and running real logic for the first time ever.
   The next concrete blocker (root-causing the stack overflow - what
   specifically overflows the stack, and why, somewhere in `Host_Frame`'s
   real per-frame call graph past input processing) is precisely
   characterized with real, decoded evidence rather than guessed at, and
   is real, separate, dedicated follow-up work for a future session.

   **One real hypothesis for the stack overflow was checked live and
   ruled out, not left open on a guess.** Codex review of the video-init
   fix (divergence #46) found a real, separate invariant bug in it: the
   early return skipped clearing `host.renderinfo_changed`, which
   `VID_Init`'s own cvar registration already forces true - left
   standing, `Host_Frame`'s own real per-frame `VID_CheckChanges` call
   would see it and try to create a real window on literally the next
   frame, reaching the same broken driver a different way. Fixed (both
   the one call site and, as defense in depth, `VID_SetMode()` itself).
   Re-tested live after the fix: the stack overflow still happens, at
   the same point, with the identical bugcheck signature - ruling this
   out as the actual cause, not merely leaving it unconfirmed. The
   overflow is confirmed to be a genuinely separate, still-unresolved
   problem.

   **Root-caused and fixed - see divergence #47.** Not recursion at
   all: the bugcheck's own ESP was only ~2 KiB below `Host_Frame`'s
   entry ESP, which pointed at a single frame that could never fit.
   Caught live at nxdk's own `_xlibc_check_stack`, armed only after the
   first `Host_Frame` (during init it probes 32 KiB for every console
   command, hundreds of times): the first frame's `NET_QueuePacket`
   (`engine/common/net_ws.c`) asks for a 0x10100-byte frame - its
   `byte buf[NET_MAX_FRAGMENT]`, 65535 bytes - and the check raises
   `EXCEPTION_STACK_OVERFLOW` as `NONCONTINUABLE`, which is exactly the
   `KeBugCheckEx(0x1E, 0xC00000FD)` observed. It could never fit
   because the whole engine was running on a 64 KiB stack: `nxdk-link`
   hardcodes `-stack:65536`, and nxdk's `CreateThread(..., 0, ...)` -
   including the thread crt0 runs `main()` on - inherits that XBE
   header value. Upstream assumes Windows' 1 MiB and already raises the
   stack explicitly on its own small-default ports (DOS 512 KiB,
   Emscripten 16 MiB). Fixed by passing `-stack:1048576` at every
   `nxdk-link` call in this fork's own link tools; verified in the PE
   and XBE headers, then live: **300 consecutive counted `Host_Frame`
   calls with no failure breakpoint firing, and a 75-second free run
   with nothing armed ending in the kernel's between-frames idle loop,
   not the bugcheck.** X7.4 step 2's boot milestone now holds for real:
   `CL_Init` passes and `Host_Frame` runs, sustained.

   **`UI_LoadProgs` itself confirmed too, not inferred - the milestone's
   third named indicator, checked directly.** A breakpoint on
   `UI_LoadProgs`'s own return address (read off the stack at entry,
   the same technique used throughout this investigation) shows `EAX =
   1` on a fresh boot: it genuinely loads `3rdparty/mainui` (statically
   linked - `GetMenuAPI` resolves from `3rdparty_mainui_udll_int.obj`
   in the real link, confirmed in the map) and its own `GetMenuAPI()`
   call inside succeeds. **Activation confirmed past the engine's own
   `UI_SetActiveMenu`, not just its entry (Codex review, round 1: an
   entry breakpoint alone does not prove activation - `gameui.hInstance`
   could be null and the call would still reach that same entry point,
   just take its own early-return path one line later without ever
   calling into mainui at all).** `3rdparty/mainui` has its own,
   separately-linked `UI_SetActiveMenu(int)` (`BaseMenu.cpp`, a distinct
   C++-mangled symbol in the real map, not the engine's C one) - the
   engine's own version calls into it via
   `gameui.dllFuncs.pfnSetActiveMenu( fActive )` only after its own
   `gameui.hInstance` null-guard. A breakpoint on mainui's OWN function
   fired with `fActive=1`, called from `_UI_SetActiveMenu + 0x3b` -
   exactly that call site, past the guard, real proof the request
   reached mainui. **Codex review, round 2, correctly pushed further**:
   that request only sets `uiStatic.nextFrameActive = true` -
   `BaseMenu.cpp`'s own comment says opening the main menu is
   deliberately deferred to the next `UI_UpdateMenu()` call, so the
   entry hit alone did not yet prove the menu became active. Two more
   things checked directly, not inferred: a breakpoint on mainui's own
   `UI_Main_Menu()` (the real, deferred "open the menu" function,
   `menu_main->Show()`) fired - the deferral genuinely resolves: and a
   clean, repeated read of mainui's own `UI_IsVisible()` return value
   (five consecutive samples, each matched to its real return address)
   read `EAX=1` every time. (The first two attempts at this specific
   check produced a false `EAX=0` reading and were not trusted or
   reported as real - a missing single-step-over-the-breakpoint before
   re-arming, compounded by stale breakpoints surviving a `system_reset`
   from earlier in the same session, desynced the read; a full xemu
   process restart, not just a guest reset, plus correct step hygiene,
   is what actually clears the debugger's own breakpoint list and gives
   a trustworthy result - the exact persistence property
   `docs/r3d/lessons.md` already documents (found while root-causing
   divergence #46's stack overflow), now confirmed to bite the SAME
   investigation a second time.) With the menu confirmed genuinely
   active, not just requested, a further stability check found no new
   failure: 2000
   consecutive counted `Host_Frame` calls in 8.3s. **All three of the
   milestone's own named indicators are now independently confirmed
   with real, live evidence: `CL_Init` passes, `UI_LoadProgs` loads and
   activates the real menu, `Host_Frame` runs, sustained. X7.4 step 2 is
   closed.**
3. **A writable rootdir exists before the first map loads - CLOSED.**
   (specialist review - a bring-up blocker, not X8 polish). Was:
   `FS_DetermineRootDirectory` (`engine/common/filesystem_engine.c`)
   resolved to nxdk's guaranteed-mounted `D:\` (X0.5's own real fix -
   the disc), which becomes `fs_rootdir`, upstream's own name for the
   WRITABLE root - and an XISO is permanently read-only. The very first
   level transition - `c0a0` to `c0a0a`, the tram, minutes into step 4 -
   goes through `SV_ChangeLevel` → `SaveGameState( true )`
   (`engine/server/sv_save.c:2065`), which writes `.HL1`/`.HL2` under
   `<gamedir>/save/` and would have failed on a read-only root.

   **Fixed and verified live - see divergence #48 for the full account.**
   `-rodir` genuinely cannot arrive via argv or `getenv()` on this
   platform (checked directly, not assumed: nxdk's own crt0 hardcodes
   `argc=0` into `main()`, and `getenv()` is a real, unconditional
   `NULL` stub), so both `FS_DetermineRootDirectory` and
   `FS_DetermineReadOnlyRootDirectory` needed their own direct
   `XASH_XBOX` arms rather than threading a synthetic command line
   through. `E:` (confirmed against `hal/fileio.c`'s own partition
   table to be the real `\Device\Harddisk0\Partition1\`, the same
   partition every retail title's own `E:\UDATA\<title ID>\` save path
   already uses - not invented for this fork) is mounted and
   `E:\UDATA\<title ID>\<gamedir>\` created if missing, then returned as
   `fs_rootdir`; `D:\` moves to `fs_rodir`, the read-only overlay it was
   always meant to be. **Codex review, round 1, caught a real gap in
   the title ID itself**: `cxbe` (this project's own XBE-building tool,
   from `deps/nxdk`) hardcodes the certificate's `TitleID` to the same
   `0xFFFF0002` for every XBE it builds (its own `// TODO: generate in
   the form CX-9999`) - confirmed live, the first boot's own
   `fs_rootdir` read back with no per-title distinction at all, meaning
   every future title this fork ships would have collided on the
   identical writable root. Fixed by namespacing one level further
   under `XASH_GAMEDIR`, the same compile-time per-title name
   `launcher.c` already relies on for the identical purpose (same
   `"valve"` default - X7.6's own real per-title build wiring does not
   exist for any translation unit yet). Verified live, after the fix: a
   fresh boot reads `fs_rootdir` as `E:/UDATA/FFFF0002/valve` and
   `fs_rodir` as `D:` directly out of guest memory - the real global the
   entire filesystem module writes through, not inferred -
   `nxMountDrive('E', ...)` confirmed to genuinely run and return
   success (alongside the pre-existing `nxMountDrive('D',
   "\Device\CdRom0\")` automount hook also confirmed still succeeding
   on this real disc-boot XISO), and `Host_Frame` is still reached with
   no `Sys_Error` anywhere in the path. A real, unique certificate
   `TitleID` per title (cxbe's own real fix) would let this drop the
   extra directory level later - X7.6's job, once a second title
   actually exists to test isolation against, not invented ahead of
   that. A real write actually landing under this directory remains
   step 4's own exit test - `gameinfo.txt`/`video.cfg` writes are log
   noise either way and stay that way; the save path was the one that
   would have stopped the game.
4. **A map loads.** `map c0a0` via `+map` in the disc's `video.cfg`/`config`
   path (upstream's own startup-command mechanism, no engine change).
   Exit: `SV_SpawnServer` completes, `pfnServerActivate` runs, entities
   spawn (every classname resolves - the generated table's real test), the
   client receives the world, and it does so with `ref/null` (no picture,
   headless). Then the tram: `c0a0` → `c0a0a` through `SV_ChangeLevel`,
   which is the real test of step 3. Memory again, via the same
   `MmQueryStatistics` print, before and after the load. **The specialist
   review's own stack-size warning here is resolved, not still open**:
   this row originally flagged the main thread's 64 KB stack
   (`deps/nxdk/bin/nxdk-link:11`'s own `-stack:65536` default) as a
   silent-failure risk against the SDK's real, large stack frames
   (`SV_PutClientInServer.msg_buf`, 197 KB, already made static to
   survive it; `nodes.cpp`'s node graph build; `CSave`/`CRestore`) -
   confirmed real and root-caused earlier, not at a map load but at the
   very first `Host_Frame` (`NET_QueuePacket`'s own 65535-byte stack
   buffer), fixed by measuring and raising the link stack to 1 MiB
   (divergence #47, `tools/xbox-check-stack.sh` asserts it on every
   linked PE/XBE now). `nodes.cpp`/`CSave`/`CRestore` may still be worth
   watching at a real map load - 1 MiB is not infinite - but the
   specific "raise `-stack:`, cheap fix, must be measured" action item
   is done.
   **CLOSED, 2026-09-14 - see the end of this entry for the real,
   verified chain and the tram. First, the real, reproducible,
   root-caused boot blocker on this exact path, found and fixed live the
   same day.**
   Injecting `map c0a0` at boot (`.sweep/boot-attempt/disc/valve/
   userconfig.cfg` - test-only, gitignored, matching the file's own
   documented purpose, no engine change needed) reliably reached
   `SV_Map_f`→`SV_ValidateMap`→`COM_LoadLevel` at ~4.2s every attempt,
   which genuinely queued `GameState->nextstate = STATE_LOAD_LEVEL`
   (confirmed by a direct read of `host.game.nextstate`, live, immediately
   after `COM_LoadLevel` returns) - but the engine then never processed
   it: `SV_ExecLoadLevel` never fired even after 450+ real seconds of
   patient waiting, and `Host_RunFrame` itself never returned to its own
   caller in `COM_Frame`'s state-machine loop (`engine/common/
   host_state.c`), confirmed with a single breakpoint on the real return
   address read off the stack, with proper single-step-past-breakpoint
   hygiene, on a freshly relaunched emulator (no stale-breakpoint
   ambiguity). Single-stepping forward from `Host_RunFrame`'s entry
   (thousands of real instruction steps, no tight loop ever detected)
   traced the live call chain all the way down through
   `Host_RunFrame`→`Host_Frame`→`XRcon_Frame` (a fast, correctly-disabled
   no-op)→`SCR_BeginLoadingPlaque`→`UI_SetActiveMenu(false)`→
   `UI_CloseMenu`→`IN_CheckMouseState`→`IN_SetRelativeMouseMode(true)`→
   `SDL_SetRelativeMouseMode`, and resolved the addresses it kept
   executing at to real SDL2 internals: `SDL_PromptAssertion`,
   `SDL_AtomicCAS`, `SDL_AtomicLock` - SDL's own internal-assertion
   machinery, not application code. Root cause, read from SDL's own
   vendored source (`deps/nxdk/lib/sdl/SDL2/src/events/SDL_mouse.c:765-774`,
   `ShouldUseRelativeModeWarp()`): `SDL_assert(mouse->WarpMouse)` fires
   unconditionally whenever `mouse->SetRelativeMouseMode` is unset, and
   the real Xbox SDL2 video driver (`deps/nxdk/lib/sdl/SDL2/src/video/
   xbox/SDL_xb{video,events,framebuffer}.c` - confirmed by grep, not
   assumed) never registers either callback; the console has no OS mouse
   cursor for either to warp in the first place. With no interactive way
   to answer SDL's own assertion prompt on a console, the failure is not
   a clean abort - it manifests as exactly what was observed, an
   unrecoverable hang. This is the first time in this fork's history any
   live boot has reached `key_menu`→`key_game` transition code at all
   (every earlier X7.4 milestone stopped at or before the menu staying
   up), so the gap was invisible until this exact step. `IN_CheckMouseState`
   (`engine/client/input/input.c`) reaches this call because `XASH_XBOX`
   implies `XASH_WIN32` (divergence #7), taking `IN_CheckMouseState`'s
   Win32 branch (`use_raw_input = (m_rawinput.value &&
   clgame.client_dll_uses_sdl) || clgame.dllFuncs.pfnLookEvent != NULL`)
   rather than the `#else` branch's "always true" comment, which does not
   describe Xbox. **Caught and corrected in Codex review, round 1**: an
   earlier version of this entry additionally claimed the left operand
   (`m_rawinput`'s own `"1"` default) was what made that expression true -
   false, since `clgame.client_dll_uses_sdl` is unconditionally excluded
   on Xbox (`engine/client/dll_int/cl_game.c:4013-4019`, `!XASH_XBOX` in
   its own compile guard) and stays at its zero-initialised `false`,
   making the left operand `1 && 0 = 0` regardless of the cvar. Whether
   the right operand (`pfnLookEvent != NULL`) is what actually makes the
   expression true was not independently verified live - no code in this
   tree (`deps/hlsdk`, `compat/`, `3rdparty/`, checked by grep) defines
   `IN_ClientLookEvent`, the export this field resolves - so the precise
   reason this specific boolean evaluates true on Xbox is left an open
   question rather than asserted without evidence; only the call
   actually happening is claimed here, and that IS independently
   confirmed by direct address resolution in the live single-step trace
   above, not by this expression's analysis. Fixed the same way as every other "Xbox is the first target to hit
   this SDL/vid_sdl2 gap" case already in this ledger
   (`Platform_GetMouseGrab`/`SetMouseGrab`, `SDLash_InitCursors`, divergence
   #46's `R_Init_Video`): a local, Xbox-only guard at the real call site,
   not a change to shared PC-facing logic, and not a patch to the
   vendored `deps/nxdk` SDL2 source (out of this project's ownership,
   unlike `engine/`). `IN_SetRelativeMouseMode` now skips the
   `SDL_SetRelativeMouseMode`/`SDL_GetRelativeMouseState` calls under
   `#if XASH_SDL >= 2 && !XASH_XBOX`, while still tracking `s_bRawInput`
   state normally either way - Xbox's primary aim input is the gamepad,
   not a warped OS mouse cursor. **Codex review, round 2, found a real
   second call site to the identical broken function**: the statically
   linked client DLL's own `IN_SetMouseRelative()`
   (`deps/hlsdk/cl_dll/inputw32.cpp`), reachable on every
   `key_menu`/`key_console`→`key_game` transition once the client module
   is loaded, not only the very first one this session traced - fixed
   the same way, directly in this fork's owned `deps/hlsdk` checkout
   (same precedent as divergence #44), committed and pushed to that
   submodule's own remote before bumping this repo's pointer. See
   divergence #49 for the full account, including what remains
   unverified live (the client DLL's own call path specifically, since
   step 4 has not yet progressed far enough to test a menu/console
   reopen after a map is active).

   **What PR #58 called "verified live" was measured against the wrong
   binary, and is retracted here.** xemu's `xemu.toml` `dvd_path` named
   `resonance3d-x74-boot.iso`, a pre-fix copy next to the freshly packed
   `.sweep/boot-attempt/iso`; every post-fix run booted the old build.
   Its "reaches `SV_ExecLoadLevel` immediately" was the old build's
   unrelated function at that address, and the "`SV_SpawnServer` never
   reached, slow `FS_FixFileCase` lookups" finding was an artifact of
   tracing that wrong image - never real. Caught by a milestone two
   instructions past the "hit" refusing to fire, then reading the guest's
   own bytes at the address and finding them not the file's
   (`lessons.md`). `tools/xemu-debug.py verify-image` now exists and is
   the first thing a live session runs after packing an image.

   **The real result, on the guest confirmed byte-for-byte to be running
   the fixed PE (`verify-image`: all 6,405,867 file-backed bytes of
   `.text` and `.rdata` match the linked `xbe-build.exe`, the only
   differences being the 99 dword kernel-import thunks the loader
   patches from ordinal to kernel address, which the tool recognises as
   such - a first, sampled version of the check was rejected in Codex
   review as unable to rule out a build differing where it was not
   sampled, and rewritten to cover every byte):
   every named exit of this step, in order, with `Sys_Error`/`Host_Error`/
   `SV_SysError` armed throughout and never hit.** From `map c0a0` at
   ~4.2 s: `SV_SpawnServer('c0a0')` 4.31 s → `Mod_LoadWorld` 4.36 →
   `SV_SpawnEntities` 4.46 (every classname resolved - the generated
   export table's real test passed, no `Sys_Error`) → `SV_ActivateServer`
   and the game DLL's own `ServerActivate` 4.94 → `SV_New_f` /
   `SV_SendServerdata` / `CL_ParseServerData` 5.08-5.10 (the client
   receives the world, headless, `ref_null`) → `SV_PutClientInServer` and
   `CL_SignonReply` 5.77, second `CL_SignonReply` 8.65. **Then the tram,
   unattended: `COM_ChangeLevel('c0a0a')` at 47.08 s → `SV_ExecChangeLevel`
   → `SV_ChangeLevel` → `SaveGameState` → `SV_SpawnServer('c0a0a')` 47.19 s,
   and again `c0a0a` → `c0a0b` at 94.14 s** - step 3's real test, passed:
   the emulated HDD image (`xbox_hdd.qcow2`, read from the host afterwards)
   holds FATX directory entries `UDATA` → `FFFF0002` → `valve` → `save` →
   `c0a0.HL1` (41,644 bytes), `c0a0.HL2` (4,153), `c0a0.HL3` (8),
   `c0a0a.HL1` (41,951), `c0a0a.HL2` (4,153), `c0a0a.HL3` (4) - real
   writes, real sizes, under exactly the writable root divergence #48
   built. **Memory, the number this step asked for - the kernel's own
   page counters, the very fields `MmQueryStatistics` reports, read
   through the exported `MmGlobalData` (`tools/xemu-debug.py memstats`,
   added for this after Codex review rejected a mapped-address-space
   proxy as not the stated criterion), self-checked to sum to exactly
   16384 pages = 64 MiB at both points**: with the menu up before the
   map, 8,308 pages available (32.45 MiB) against 8,076 busy (31.55 MiB:
   Image 4,891 = 19.11 MiB, VirtualMemory 1,869 = 7.30 MiB, Stack 771 =
   3.01 MiB, Contiguous 513 = 2.00 MiB, page tables/pool/FsCache 32);
   once `c0a0` is received, 4,458 available (17.41 MiB) against 11,926
   busy (46.59 MiB) - **the first map costs +3,850 pages = +15.04 MiB,
   all of it `VirtualMemory` (+3,846) plus 4 page-table pages; Image,
   Stack and Contiguous do not move.** The mapped-address view agrees
   (title address space 26.41 → 41.43 MiB: the engine's main arena at
   `0x1330000` grows 1.25 → 9.19 MiB and ~7.3 MiB of new heap arenas
   appear at `0x7f0b0000-0x7f977000`), but it is the diagnostic, not the
   gate. 17.4 MiB free after one map, on a 19.1 MiB image, is X8's
   opening brief - alongside step 1's 16.5 MiB-at-entry estimate, now
   measured at 19.1 MiB with the real client and freevgui linked in.
   `nodes.cpp`/`CSave`/`CRestore` did not overflow the 1 MiB stack across
   three maps and two saves. The client DLL's own `IN_ActivateMouse`
   (the caller of divergence #49's second guarded site) is hit at 4.31 s
   on the menu-close transition and the chain continues through it, so
   both guards are exercised on this binary. Not done here, deliberately:
   watching a menu/console REOPEN after the map is active needs input,
   which this headless boot has none of - it is X5/X7.3 territory, the
   first time a gamepad exists to press.
5. **On screen** is X5 (`vid_xbox.c` + `ref_soft`), which this step
   unblocks: X5's exit line ("a recognisable map, in software") needs a
   game module to have something to draw.

### X7.5 — Discs: one XISO per title

`tools/xbox-xiso.sh <title>` (replacing the hand-rolled `extract-xiso`
invocation used through X5 bring-up) builds `default.xbe` + the title's
gamedir + `valve/` (the basedir all three fall back to) from the Steam
install path given on the command line - never copied into the repo, none
of it is ours to redistribute. `gameinfo.txt` per title as upstream
generates it from `liblist.gam` (`gamedir`, `basedir`, `title`, `startmap`
`c0a0`/`of0a0`/`ba_tram1`), `video.cfg` with `r_refdll` for the renderer
in use. Disc-size and load-time work (`.mpak`, X8) is deliberately NOT
here; a raw `valve/` tree is fine for bring-up and honest about what it
costs.

### X7.6 — Opposing Force and Blue Shift

Same pipeline, `XBOX_TITLE=op4` / `bshift`, on their `resonance3d`
branches: replay X7.3, sweep, generate tables (323 / 254 entities), link,
disc. What is genuinely different and must be checked rather than
assumed: Opposing Force's extra `dlls/` (135 vs 103 - new monsters,
weapons, the `CTF` gamerules) and any `cl_dll` additions (53 vs 49);
Blue Shift's `bshift` gamedir replaces `valve` content by name (its own
`maps/`, `models/`) with `basedir valve` for the rest, and its Steam
release uses standard BSP 30 (the old retail "BS BSP" variant is not a
concern). Both expansions ship their own `pm_shared` copies - three
`PM_Move`s in the tree, one per XBE, never in the same link. Exit per
title: the same three boot milestones as X7.4, on its own disc, from its
own `startmap`.

### Risks carried into the ledger, not hidden

- **Save format depends on link layout** (`XASH_STATIC_LIBS` implies
  `XASH_ALLOW_SAVERESTORE_OFFSETS`, X1 note): `COM_OffsetNameForFunction`
  (`engine/common/lib_common.c:78`) writes function pointers as `ofs:N`
  relative to `pfnGameInit` and `COM_FunctionFromName_SR` (`:41`) reverses
  it - intra-image relative, so (specialist review) it is adequate for
  bring-up and linker non-determinism only matters ACROSS XBEs: a save is
  valid for the exact XBE that wrote it and no other. lld's default ICF
  (no `-opt:` from `nxdk-link`) folds identical `Think` bodies to one
  address, which is harmless under offset saves. A name-based scheme is
  its own item before any release.
- **Three `pm_shared`s, three sets of engine expectations**: the engine's
  `SV_InitClientMove` hull loop and `pmove` function table are per-link;
  no cross-title sharing, and nothing to get subtly wrong as long as a
  title never links another title's objects (the sweep's per-title scratch
  dir is the guard).
- **Memory is the engine's problem before it is the SDK's**: see X7.4
  step 1 - 16.5 MB of XBE sections resident at entry today, `.bss` alone
  13.1 MB, against ~1-1.3 MB the SDK will add. The SDK does not create
  the wall; it walks up to one that is already there. X8 owns the fix
  (`cl`/`sv`/`net`/`mod_known` static arrays sized for a PC), X7 owns
  producing the two honest numbers that brief it. SDL2 is, separately,
  the single largest `.text` contributor of the current XBE (764 KB) -
  the X5 `vid_xbox.c` decision already exists for that.
- **Linker non-determinism** (lessons.md): two links of identical source
  differed in layout. Combined with the save-format point above, the disc
  pipeline must record which link produced which XBE (the `.map` beside
  it), and never mix.

**Exit for X7 as a whole:** each of the three titles boots from its own
XISO to its own menu, loads its own first map headless with every entity
classname resolving, with the run verified through `tools/xemu-debug.py`
and the log channel, and with the `.map` + manifest of the link that
produced each XBE checked in alongside the build log. "Playable on screen"
is X5 + X7 together, and is measured there.

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
