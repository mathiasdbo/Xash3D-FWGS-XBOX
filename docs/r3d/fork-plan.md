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

**Under xemu, 2026-09-15.** `tools/x4-triangle/` (~280-line `main.c`, two
`.cg` shaders) boots on Xbox, sets 640x480, uploads a vertex buffer and one
Cg-compiled vertex/pixel shader pair through `pb_begin`/`pb_end`, and draws
one triangle textured with an 8x8 checkerboard - all via pbkit, matching
`deps/nxdk/samples/triangle/main.c`'s shape (unmodified) plus texture setup
read from `deps/nxdk/samples/mesh/main.c`, the one shipped nxdk sample that
actually binds a texture (attribute slot 9 for texcoord0, the real
`NV097_SET_TEXTURE_*` register values for a working stage 0). Runs at a
steady 61-63 fps under emulation. The exit line's "on hardware" is not yet
met - this has only run under xemu, and per this project's own verification
rule that is not a substitute for a real console.

**A real, unresolved finding, exactly the "getting it wrong here costs an
afternoon" this section warned about.** The demo uploads BOTH a linear
(row-major) and a Morton-swizzled copy of the same checkerboard and can bind
either (`X4_TEXTURE_SWIZZLED` in `main.c`). The linear copy
(`LU_IMAGE_A8R8G8B8` + `IMAGE_RECT`/pitch) renders correctly. The swizzled
copy (`SZ_A8R8G8B8` + log2 `BASE_SIZE_U`/`V`, the actual NV2A hardware
swizzle path and X4's real point) renders as a flat fill of texel (0,0)
instead. **What was actually checked, stated narrowly (correction, Codex
review, 2026-09-15 - the first version of this paragraph overclaimed):** the
on-screen `g_tex_format`/`g_tex_phys` readout is this program printing back
the dword it just computed, not a GPU-register readback, so "matches this
file's own derivation bit for bit" only shows the arithmetic is
self-consistent - it cannot catch a wrong field position or a consistently
wrong encoder that happens to agree with itself. Likewise, the
guest-memory comparison via `tools/xemu-debug.py xp` only shows the uploaded
bytes match THIS implementation's own idea of Morton order, not that NV2A
expects that exact layout. **Neither check rules out anything, including a
program-side defect - a second round of the same review caught this
paragraph still narrowing the diagnosis ("not an obviously-wrong field")
after saying the checks couldn't establish that.** They rule out nothing at
all beyond "the arithmetic this program performs is internally consistent."
The cause could equally be a genuine xemu swizzled-fetch gap, a field this
program has wrong in a way self-consistency can't catch, or the Morton
algorithm itself (shared with
`tools/asset-studio/src/utils/TextureOpt.cpp`'s `morton2D`/`Part1By1`, XB's
own baker swizzle) not matching what NV2A expects - genuinely, fully open,
not narrowed toward any of the three. **X9's own exit criterion already
names this precise risk** ("verify on hardware that XB's fix produced a
correct swizzle, since xemu is not validation for this") - this is that
check's first data point, one phase early. Next real Xbox session: flip
`X4_TEXTURE_SWIZZLED` to 1, rebuild, and see what actually happens.

**This is not X4 closed, and this commit does not claim it is.** X4's own
exit line needs "on hardware," which has not happened, and its main point -
texture swizzling - does not work under the only environment available this
session. Deferred rather than fixed here because there is no real Xbox in
this environment to test against, and guessing at a further register fix
with no way to observe its effect is exactly what this project's own rules
forbid ("Confirm every assumption by reading the code or observing the
hardware. Never from memory, never by analogy," "After two failed attempts,
stop and escalate with data. Not with a fresh hypothesis") - two independent
attempts (this session's own investigation, and the nv2a-xbox-specialist
review) already found no additional register or field missing by static
analysis. What this commit is: a working linear-textured bring-up milestone,
committed with its one real defect left enabled-but-unbound and fully
documented, matching how this project's own history ships incremental,
honestly-flagged progress rather than holding work until a phase fully
closes (compare the several "not yet fixed," "first real crash fixed," "step
N complete" entries already in `CHANGELOG.md`). The next Xbox session is
what closes X4, not this one.

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
first half of this milestone's exit line.** X3's entry above has where
the memory went and where the next MiB are.

**The second half, measured 2026-09-14 (divergence #52): frame time on
`c1a3` and `crossfire` at 640x480 and 320x240.** Read by the debugger
from the engine's own counters (`host.framecount` over 30 s of wall
clock, and `host.pureframetime`, the last frame's CPU time without
`Host_Autosleep`'s sleep; `tools/xemu-debug.py fps`, offsets from
`tools/xbox-offsetof.sh`), on a `verify-image`-confirmed guest, `ref_soft`,
`XASH_LOW_MEMORY=1`, the map's own spawn view with no input:

| map | render | wall fps | ms/frame (wall) | `pureframetime` | free after load |
|---|---|---|---|---|---|
| `c0a0` (tram, pre-changelevel) | 640x480 | 18.3 | 54.5 | 52.5 ms | 11.8 MiB |
| `c1a3` | 640x480 | 28.5 | 35.1 | 34.7 ms | 7.7 MiB |
| `crossfire` | 640x480 | 22.2 | 45.1 | 44.7 ms | 8.7 MiB |
| `c0a0` | 320x240 | 25.9 | 38.6 | 37.4 ms | 13.7 MiB |
| `c1a3` | 320x240 | 43.0 | 23.3 | 23.5 ms | 9.4 MiB |
| `crossfire` | 320x240 | 29.1 | 34.4 | 34.3 ms | 10.6 MiB |

The 320x240 rows above are the render alone (the quadrant, unscaled).
Presented full-screen by the Xbox arm's 2x2 nearest expansion in
`SW_UnlockBuffer` (in place, before the driver's own copy), measured
twice because the first version taught something:

| map | present step | wall fps | ms/frame |
|---|---|---|---|
| `c0a0` | a division per output pixel | 24.1 | 41.6 |
| `c1a3` | " | 36.7 | 27.2 |
| `crossfire` | " | 27.2 | 36.8 |
| `c0a0` | one expanded row per group + `memcpy` | 22.5 | 44.5 |
| `c1a3` | " | 32.7 | 30.6 |
| `crossfire` | " | 23.9 | 41.8 |

The division-free version is the one kept, and it is the SLOWER one
under xemu: TCG turns the guest's `div` into one native instruction,
while nxdk's `memcpy` runs emulated instruction by instruction - the
emulator ranks this micro-optimisation backwards. On the target a `div`
per output pixel is ~12 M cycles a frame (~16 ms at 733 MHz) and
`rep movsd` streams the 1.2 MB in ~2 ms, so the choice is made on the
hardware's known costs, not the emulator's verdict - and it is one
more reason the absolute numbers here await a console. The engine's own
on-screen counter (`cl_showfps 2`, drawn by `ref_soft`) reads the same
values the tool does (27 / 24 fps on `crossfire` for the two versions,
captured), so a frame rate can be read off the TV without a debugger.

Read it right: (1) wall and pure agree to the millisecond at every
point - the frame is CPU-bound, nothing is sleeping, so these are the
rasterizer's real costs plus a fixed per-frame tax that does not shrink
with the render size: the 640x480 `SDL_ConvertPixels` of the whole
window surface into the framebuffer (`SDL_XBOX_UpdateWindowFramebuffer`),
which `vid_xbox.c` deletes - the 320x240 numbers therefore UNDERSTATE
the rasterizer's own gain. (2) These are xemu's numbers, not the
console's: the CPU is emulated (TCG), not cycle-accurate to a 733 MHz
Pentium III; the ratios between rows are the evidence, the absolute
values await a real Xbox. (3) The "X0 counters" this line was written
against - the per-subsystem `rdtsc` ruler and a frame-time log line -
were never built; the counters read here are the engine's own, through
the debugger, the same instrument every other number in this plan came
from. (4) `c1a3` leaves 7.7 MiB free at 640x480 - the memory ceiling is
now closer than the frame-time floor, and X3's ranked levers are the
next spend. **X5's exit line is met under emulation**; the hardware
repeat of this table is the first thing to do when a console is on the
desk, and X9's `ref/nv2a` has its baseline.

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

**Met under emulation, not yet closed, 2026-09-15.** No code change was
needed - the plumbing this section already named was correct: the exit
criterion is only met under xemu, not on hardware, and per this project's
own rule that means X6 stays open, not closed (correction, Codex review -
an earlier version of this entry said "closed" while saying exactly this in
the same paragraph). `engine/client/input/in_joy.c`'s `Joy_Init()` runs
unconditionally for any non-dedicated build (`input.c`'s `IN_Init`),
`joy_enable` defaults on, SDL's `GameController` subsystem self-initialises
with no Xbox-specific gate (`joy_sdl2.c`), and `3rdparty/mainui`'s own
`Gamepad.cpp` is the same code the psvita/nswitch ports already rely on
(`divergences.md`). This session's work was entirely verification:
confirming the above by reading the code, then a live human test - a real
host gamepad, already bound in xemu's own config
(`[input.bindings] port1_driver = 'usb-xbox-gamepad'`) - navigated the WON
main menu and started a new game with it. What closes X6: the same test,
on a real Xbox.

An unrelated curiosity hit while preparing for this test, left unresolved
because it turned out not to matter: `.sweep/boot-attempt/
resonance3d-x74-boot.iso` (X7.4's own headless test build) loads straight
into `c0a0` gameplay within ~10-15s of boot with no human input, which
looked at first like a scripted `+map` command-line argument processed via
`Cbuf_ExecStuffCmds()` (`engine/common/host.c:1357`, `cmd.c:274-325`) -
disabling `valve.rc`'s `stuffcmds` line on a repacked copy of that disc had
no effect on it, and nxdk's own `xboxrt` doesn't appear to construct a real
argv for Xbox in the first place, so that theory is wrong. **Resolved
during XG.1 (2026-09-15): it is the disc's `valve/userconfig.cfg` - `map
c1a3` plus `cl_showfps 2`, the X7.4 step-4 boot injection divergence #49
already describes as test-only and not in the repo** - read straight off
the disc after a spawn-spot probe returned `c1a3`'s player start instead
of `c0a0`'s. It didn't get in the way of the human test above, but it did
mislead one round of XG.1's measurements (see there).

## XM — Console-shaped menus (proposed, 2026-09-15)

**Status: in progress, 2026-09-16 - the "Where the code lives" prerequisite
is done (an owned `mainui_cpp` fork exists, verified against the current
tree); no screen has been rewritten yet.** Raised after X6's live controller test:
the WON menu navigates with a pad, but every screen behind it is the PC's
- a resolution table, windowed/borderless toggles, a renderer picker, mouse
sensitivity, touch and gyro pages, a server browser - and the one screen a
console player needs most, a controller mapping with presets, does not
exist. This phase replaces the PC-shaped screens with console-shaped ones
and hides the rest. It is `epics.md` E7's "mainui" half made concrete for
this target; the freevgui/VGUI1 half of E7 is separate and later.

**How a sibling fork did it (read, not assumed - `sultim-t/xash-rt`).**
xash-rt ships its own menu by pointing the `3rdparty/mainui` submodule at a
fork (`sultim-t/xash-rt-menu`, ~20 commits over stock FWGS `mainui_cpp`).
It added no new menu files: its "Graphics" screen is `menus/VideoModes.cpp`
rewritten in place, keeping the stock `ADD_MENU( menu_vidmodes, ... )`
registration and `gfx/shell/head_vidmodes` banner so no new button strip or
header art was needed - the resolution table became a `CMenuSpinControl`
over `vid_mode`, plus `CMenuCheckBox`/`CMenuSpinControl` widgets linked
straight to its `rt_*` cvars via `LinkCvar`, with `onCvarWrite` lambdas for
mutual exclusion and `Draw()` greying options the hardware lacks. It
flattened `Main.cpp` (direct Graphics/Audio/Controls/Load/Save buttons,
Save/Load/New Game under one "Campaign" button; `Configuration.cpp` left in
the tree but unreachable) and guarded its layout changes with
`#if XASH_RAYTRACING`. Nothing controller-specific; `Gamepad.cpp` untouched.
Three things transfer directly: **rewrite in place behind the existing
`ADD_MENU`/`PC_*` ids** (no new art pipeline), **link widgets to cvars, not
to new engine code**, and **guard with a platform macro** - ours is
`XASH_XBOX`, already defined for every engine TU and not yet used anywhere
in `mainui`.

**What our vendored `mainui` already gives us (verified in the tree).**
`ADD_MENU`/`ADD_MENU3` registration (`BaseMenu.h:231-264`; `menus/Video.cpp:
33-53` is the write-your-own-show-function form); `CMenuFramework::AddButton`
against the `btns_main.bmp` strip or a custom bitmap (`controls/Framework.h:
27-74`, auto-stacked at (72, 230+50n)); `CMenuEditable::LinkCvar/WriteCvar/
ResetCvar/DiscardChanges` with `onCvarGet/Write/Change` hooks
(`controls/Editable.h:21-89`); `CMenuSlider`, `CMenuSpinControl` (over a
`CStringArrayModel`), `CMenuCheckBox`, `CMenuSwitch`, `CMenuDropDown`. Pad
navigation is already in `Utils.h:185-307` (A/B/X/D-pad → Enter/Escape/
Backspace/arrows - what X6 exercised). `menus/Gamepad.cpp` already has
`joy_side/forward/pitch/yaw` sliders with invert and `joy_axis_binding` spin
controls; `Audio.cpp:64-72` already carries `vibration_enable`/
`vibration_length`; `Controls.cpp`'s key-grab dialog works through
`KeynumToString`, and the engine's key table names every pad button with a
default bind (`engine/client/input/in_keys.c:109-127`), so a pad mapping
screen can reuse `CMenuKbActListModel` rather than invent a binding store.
The stick deadzone cvars exist (`in_joy.c:56-59`). Art: banners and
backgrounds load through `PIC_Load` (TGA/BMP/PNG), only the animated button
strip is BMP-only (`Btns.cpp:27`).

**Scope - the screens.** Each is a rewrite-in-place or a hide, never a new
registration unless the stock one is unusable:

0. **Menu art - the background and the animated title, for BOTH content
   profiles (added 2026-09-16, after the X7.5 disc booted to a flat grey
   menu with text buttons; extended 2026-09-18 with step 7, the art this
   fork authors rather than inherits, and the provenance rule that goes
   with it).** The requirement: the menu must
   render Valve's own art whether the disc carries the 25th-anniversary
   (HL25) content or the pre-anniversary (older Steam / WON) content,
   detected from what is on the disc, never assumed. Read, not guessed,
   before this item was written - `3rdparty/mainui`, the engine, the
   real Steam install, Ferrum56 and xash-rt:

   **Why the menu is flat grey today - not a renderer gap.** `tools/
   xbox-cc.sh:329` defines `XASH_LOW_MEMORY=1` (X3); `host.c:1239`
   exposes it as `host_lowmemorymode`; `BaseMenu.cpp:1167` reads that
   into `uiStatic.lowmemory`; and `CMenuBackgroundBitmap::LoadBackground`
   returns before loading anything when it is set (`controls/
   BackgroundBitmap.cpp:351-352`), leaving `s_state` at `DRAW_COLOR` and
   `Draw()` painting `colorBase` = `0xFF505050` (`:61`, `:146-150`). The
   same flag also disables the WON bitmap button strip (`Btns.cpp:37-38`),
   the blurred PicButton text (`PicButton.cpp:311`), menu sounds
   (`BaseMenu.cpp:1000-1001`) and one font path (`FontManager.cpp:91`).
   The X7.5 row's "still-loading pic or a `ref_soft` gap" guess was
   wrong and is corrected there. The fix is a finer gate, not lifting
   `XASH_LOW_MEMORY` (X3's other uses of it stay).

   **What the two profiles actually consist of** (real Steam install
   measured; the older profile from mainui's own loader and Ferrum56's
   notes):
   - *HL25*: `liblist.gam` says `animated_title 1`, `hd_background 1`
     (→ `GFL_ANIMATED_TITLE`/`GFL_HD_BACKGROUND`, `cl_gameui.c:402-405`).
     Background = `resource/BackgroundLayout.txt`, `resolution 800 600`,
     12 tiles of 256x256 32-bit TGA (1.85 MiB on disk, 3 MiB raw RGBA);
     the `HD_` layout is `3840 1600` with 105 tiles (18 MiB) and is
     what `hd_background 1` makes mainui try FIRST (`BackgroundBitmap.cpp:
     223-224`) - for a 640x480 CRT that is ~6.9 Mpixel of texture for
     nothing. Title = `CMenuAnimatedBanner` (`controls/AnimatedBanner.cpp`),
     eleven `resource/logo*.tga` layers (`logo`, `logo_blip`, `logo_blip2`
     at 800x86; `logo_big_blurred_{0..3}` and `_blip_{0..3}` at ~466x270),
     ~4.6 MiB raw RGBA, all-or-nothing (`TryLoad`, `:49-84`), composited
     with `PIC_DrawTrans` - a TGA state machine, NOT a video, and already
     NOT gated by `lowmemory`. No `gfx/shell/*.bmp` at all: the button
     strip and banners are gone from HL25 content, so text buttons are
     the correct HL25 rendering, not a fallback failure.
   - *Pre-HL25 (older Steam / WON)*: background = `gfx/shell/splash.bmp`
     (`ART_BACKGROUND`, `BackgroundBitmap.h:23`, single image), buttons =
     `gfx/shell/btns_main.bmp` (BMP-only strip, `Btns.cpp:27`; Ferrum56
     measured its atlas at 4 MiB raw RGBA) plus `head_*.bmp` banners
     (~3.4 MiB raw across 24), title = `media/logo.avi` through
     `CMenuMovieBanner` (`controls/MovieBanner.cpp`), enabled only when
     the WON background is the chosen one (`s_bEnableLogoMovie`,
     `BackgroundBitmap.cpp:336-340`). Old Steam content may carry BOTH the
     tiled `resource/background` and the `gfx/shell` art; mainui's
     existing ladder (`UpdatePreference`, `:307-343`, cvar
     `ui_prefer_won_background`) already decides which one to DRAW by
     file presence - but not which one to LOAD: `LoadBackground`
     (`:345-370`) calls `LoadSteamBackground` and then `LoadWONBackground`
     unconditionally, both before `UpdatePreference` runs, so a disc that
     carries both sets has both resident (review, round 1, caught the
     first draft calling the ladder "the detection, nothing new to
     write"). The detection logic transfers; the load order does not. On
     Xbox the two loads become exclusive: probe for each set's files
     first (`FileExists`, no `PIC_Load`), apply the preference to the
     probe result, load only the chosen set, and free it before loading
     the other when `ui_prefer_won_background` flips - a small change on
     the owned fork, `#if XASH_XBOX`-shaped, with its own ledger row.

   **`logo.avi` is dead on Xbox by construction, three times over, and
   stays dead.** The engine has exactly one AVI backend, ffmpeg
   (`common/backends.h:59-61`, `defaults.h:92-98`, keyed solely on
   `HAVE_FFMPEG`, which `tools/xbox-cc.sh` never defines - so the Xbox
   build links `avi_ffmpeg.c`'s null stubs, `:811-870`, 14 `AVI_*`
   symbols in the map); `ref_soft`'s `GL_UpdateTexture`, the call
   `AVI_Think` pushes frames through, is an empty stub (`ref/soft/
   r_draw.c:216-221`); and `UI_DrawLogo` refuses any AVI that does not
   resolve through `FS_GetDiskPath` (`cl_gameui.c:303-310`). Degradation
   is clean and already verified in code: `AVI_GetVideoInfo` fails →
   `gameui.drawLogo = false` → `GetLogoLength() <= 0` → the movie banner
   draws nothing (`MovieBanner.cpp:81`). A pre-HL25 disc therefore gets a
   title-less WON menu. Baking `logo.avi` into a frame strip offline is
   XB's kind of work and NOT this phase's - the HL25 title is the one
   that ships, and it is TGA.

   **What the two sibling projects did, and what transfers.** Ferrum56
   ports `CMenuAnimatedBanner` line by line (`vgui-rs/src/widget.rs`,
   `AnimatedLogo`) but bakes every asset offline into `.mpak` (DXT1
   background, RGBA logo layers, zero runtime decoders), never implements
   `hd_background`/`animated_title` (asset presence decides), never
   implements the `logo.avi` fallback, and refuses to pack the WON button
   atlas for Xbox until a P8 bake exists (`docs/plans/won_menu_ui_plan.md:
   436-462`: 8.48 MB raw against a ~10-12 MiB GPU heap, 1.7-4.2 s off a
   DVD). What transfers: the memory arithmetic, the "prefer the small
   layout" decision, and the alpha-blend (not additive) rule for the logo
   layers, and - decided 2026-09-16, after the numbers below - the
   offline bake itself, as this item's second stage (below), not as a
   copy of Ferrum56's archives. xash-rt inherits stock mainui for both background and
   logo unchanged, forces text buttons (`GFL_RENDER_PICBUTTON_TEXT`), and
   then bypasses the static art entirely with a live background MAP
   (`ui_renderworld 1` + a one-line `chapterbackgrounds.txt`, `c2a5x`)
   plus readability tints. **Rejected for Xbox**: X7.4 measured the first
   map at +15 MiB; a menu that costs a map load is the opposite of what
   this port needs.

   **What a texture actually costs in `ref_soft` - read from the
   uploader, after review round 1 caught the first draft assuming an
   8-bit palettized store that does not exist.** `pixel_t` is an
   `unsigned short` (`ref/soft/r_local.h:84`), `GL_CalcTextureSize` is
   `width * height * 2` (`r_image.c:82-85`), `GL_UploadTexture` allocates
   FOUR mip levels for every upload (`:235`, `:244-252` - x1.33 of the
   base), and any texture with `TF_HAS_ALPHA` gets a second, full-
   resolution `pixel_t` buffer at mip 0 (`:254-255`). Resident cost is
   therefore `w*h*2*1.33`, plus `w*h*2` when alpha is kept - the 8-bit
   quantization at `:337`/`:746` is the palette the 16-bit pixels are
   built FROM, not the store. Applied: the 12 background tiles are 12 x
   256x256 = **2.0 MiB** (2.6 MiB if imagelib keeps their alpha - 32-bit
   TGAs; the content is opaque, so likely not, but measured, not
   assumed); the eleven title layers are 1.18 Mpixel at mip 0 (three at
   800x86, eight at ~450x270), all with real alpha, so **~3.0 MiB of
   mips plus ~2.25 MiB of alpha = ~5.2 MiB**; `logo.tga` alone is ~310
   KiB; the WON button strip, if it is the 2048x512 atlas Ferrum56
   measured, is ~2.7 MiB of mips (plus 2 MiB if alpha) AND is halved to
   1024x256 by `GL_SetTextureDimensions`' 1024 cap (`:94-110`) before
   any of that - a correctness question for the pre-HL25 profile, not
   just a size one; the 24 `head_*.bmp` banners at ~460x80 are ~2.3
   MiB. `PIC_Load` images are `TF_IMAGE`, resident until freed, not per
   map. Against X7.4's measured 17.4 MiB free after the first map, the
   animated title as-is is a third of the headroom - which is why the
   acceptance threshold below is the MEASURED heap delta, with a stated
   ceiling, and not these estimates.

   **Two stages, decided 2026-09-16: measure the retail files first, then
   bake them for the console.** Retail art is authored for 800x600 and
   up; the target is a 640x480 CRT with a 16-bit software rasterizer
   today and the NV2A after X9. Loading the retail files unchanged (stage
   A) gives the honest baseline and proves the code path; the bake
   (stage B) is what makes the art fit, and it lives in `tools/
   asset-studio` - XB's tool - as an XM-owned output. Read before
   deciding its shape, because three facts rule out the obvious "reuse
   Ferrum56's packer as-is":
   - `tools/asset-studio` already carries `UiPacker` (`src/formats/
     UiPacker.cpp`, `--pack-ui`/`--pack-menu`): a composited, POT-padded,
     DXT1, NV2A-swizzled background blob and an RGBA8 dictionary of menu
     art, refused for `--target xbox` (`main.cpp:340`) until a P8/16-bit
     encode exists. **Neither archive has a consumer in this engine**:
     Ferrum56's Rust engine read them; here `mainui` loads through
     `PIC_Load` → imagelib (`cl_gameui.c:492-510`), and the mpak design
     itself notes the UI archive is only ever mounted by accident
     (`mpak-loading-design.md`, "`UiPacker` ... today it is mounted as
     `maps/menu.bsp`, harmlessly but wrongly"). So the bake's output is
     not an archive: it is **ordinary files, in formats imagelib already
     decodes, under the names and layout `mainui` already reads**, laid
     over the retail tree by the disc packager. No new loader, and the
     unchanged-content path (stage A) stays valid for any disc built
     without the overlay.
   - **DXT is for X9's renderer, not X5's.** imagelib keeps `.dds` data
     compressed (`img_dds.c:180`, `PF_DXT1`) and only decompresses under
     `IMAGE_FORCE_RGBA` (`img_utils.c:1364-1366`); `ref_soft`'s uploader
     merely turns mips off for a compressed pic (`r_image.c:309-315`) and
     then reads the buffer as RGBA in its pixel loop (`:257-261`) - and
     refuses to process one (`:736-738`). Under `ref_soft` the bake wins
     on resolution, mips and alpha only; under `ref/nv2a` it also wins on
     format (DXT1 for the opaque background, DXT5 or 16-bit for the alpha
     layers). One bake tool, two format profiles selected by renderer -
     and XB's own finding applies: `swizzleNV2A` is wrong for DXT
     ("DXT laid out linear by block row"), so the NV2A profile does NOT
     swizzle compressed output.
   - **The title code reads TGA headers by hand** (`UI_GetTargaImageDimensions`,
     `AnimatedBanner.cpp:30-47`) and positions everything by
     `ScreenWidth/1024` (`:86-122`), so baked title layers stay TGA and
     the fork has to know they are already at canvas scale - a small,
     explicit change, not an accident of resizing.

   What the bake does, per asset, with the arithmetic from the uploader
   above:
   - *Background*: **`ref_soft` profile DONE, 2026-09-16 - a new
     `tools/asset-studio` mode, `--bake-menu-art --game-dir <gamedir>
     --output <dir>`, and a new `tools/xbox-xiso.sh --menu-bake <dir>`
     overlay flag.** `UiPacker::bakeBackgroundArt` composites whichever
     layout the gamedir carries (reusing `parseBackgroundLayout`/
     `readTga`/`blit`, the exact functions `packMenuBackground` already
     used, so the composite is provably built the same way), bilinear-
     resizes the composite to 640x480, and re-tiles it at 256x256 (3x2 =
     6 tiles, the right column and bottom row genuinely partial - 128 and
     224 px, confirmed by reading the written files' own headers, not
     assumed), writing a new `resource/BackgroundLayout.txt` (`resolution
     640 480`) plus plain uncompressed TGA tiles - ordinary files under
     the names `mainui` already reads, not an archive, per this section's
     own "three facts". A new `writeTga` matches retail's own on-disk
     convention exactly (row 0 first, `0x08` descriptor - verified
     correct by booting a real disc with baked tiles and reading the
     screen, not assumed from the TGA spec) so a baked tile is
     interchangeable with a retail one from the engine's own load path.
     **Measured, same binary, `memstats`, same disc content on both
     sides**: `2512` `VirtualMemory` pages retail (unbaked) vs `2448`
     baked - a **64-page (0.25 MiB) saving**, real but well under the
     ~1.0-1.2 MiB this section originally estimated from the uploader's
     own formula - the gap between the two is not explained (worth
     instrumenting `Mem_Calloc` directly before trusting the formula for
     anything else, not attempted this session). **Visual**: content
     changed mid-session (the Steam install this was tested against was
     switched from a 25th-anniversary build - which has genuinely solid
     black 800x600 tiles, confirmed earlier by reading raw pixel bytes -
     to an older, pre-25th-anniversary Steam release with real
     photographic art, Gordon Freeman and the lambda logo) - the baked
     result is recognisably the same image, correctly oriented, with
     smooth colour continuity verified pixel-by-pixel across the tile
     seam at y=255/y=256 (not just eyeballed); a visual difference from
     the unbaked screenshot exists and was not root-caused before this
     was written up - worth a second look before calling this fully
     closed, not blocking given the memory number is what step 3's
     ceiling actually needs. NV2A profile (one 1024x512 DXT1 `.dds`) is
     not implemented - X9's to add once it exists to consume it. Either
     profile keeps `GFL_HD_BACKGROUND` masked (step 2) so the 105-tile
     layout is never tried.
   - *Animated title*: keep the layers and the state machine - the
     blurred strips are a parallax backdrop, pre-compositing them would
     kill the effect - but scale every layer by the same 0.625 the
     runtime would apply (`ScreenWidth/1024` at 640), drop mips, and keep
     alpha: 1.18 Mpixel → ~0.46 Mpixel, ~0.9 MiB of pixels plus ~0.9 MiB
     of alpha = **~1.8 MiB against 5.2 MiB retail** (~2.1 MiB if the
     mips cannot be dropped after all), which is what puts the animated
     title under the ceiling at all. Requires the fork to
     (a) treat baked layers as canvas-scaled and (b) honour a no-mips
     request for UI pics - `mipCount` is a hard 4 in `GL_UploadTexture`
     (`r_image.c:235`) regardless of `TF_NOMIPMAP`, so that one is a
     `ref_soft` change with its own ledger row, measured before and
     after. NV2A profile: DXT5 `.dds` layers once the fork's dimension
     reader accepts DDS.

     **Tool written and run against the real HL25 install, 2026-09-18:
     `tools/xbox-title-bake.py`. The arithmetic above is confirmed to
     the second decimal; requirement (b) is downgraded from necessary to
     optional by it; requirement (a) is untouched and still unwritten.**
     Standard library only, for the same reason `tools/xbox-*.sh` are
     shell: a bake that needs a `pip install` is a bake that stops being
     run. It reads the eleven layers `TryLoad` demands rather than
     globbing a directory - `AnimatedBanner.cpp:49-84` is all-or-nothing,
     so a partial bake turns the title off entirely instead of dropping
     one element, and `--check` re-reads its own manifest against that
     same fixed list. Resize is bilinear (a 0.625 reduction, where four
     taps cover the source; the area filter the Duke needs in step 7 is
     for its 27x one) and **premultiplied**, which is the half worth
     writing a separate tool for at all: `UiPacker.cpp`'s own comment
     above `resizeBilinear` flags exactly this and leaves it for whoever
     extends it to the title, because colour under a fully transparent
     texel is undefined - retail's layers carry black there - so
     averaging raw RGB across a soft edge rims every glow with a dark
     halo. Output is 32-bit uncompressed TGA with retail's own
     descriptor, `0x08`, bottom-up, so a baked layer drops into the same
     `PIC_Load` path as the file it replaces; that row order was read out
     of `img_tga.c:137-148` (bit 5 set means the loader fills forward and
     the first row is the top; clear means it starts at the last row and
     steps back) and checked against retail's own headers, not assumed
     from the TGA spec.

     **Measured, host-side, using `ref_soft`'s own cost formula
     (`r_image.c:235-255`, `w*h*2*1.33` plus `w*h*2` where alpha is
     kept): 5.24 MiB retail → 2.05 MiB baked, a 3.19 MiB saving, mips
     and alpha both kept.** That lands on this bullet's own
     "~2.1 MiB if the mips cannot be dropped after all" branch, which
     settles (b): the no-mips `ref_soft` change would take it to
     ~1.75 MiB, a further 0.3 MiB, and is worth doing on its own merits
     for every UI pic, but the animated title fits the ceiling without
     it. **What this number is NOT**: a measured heap delta. It is the
     uploader's formula applied to real baked file dimensions, not
     `memstats` on a disc - no disc has been built with this bake, so it
     does not close anything, and the step-4 style before/after reading
     is still owed. **(a) remains the blocker and cannot be solved by
     this tool**: `CMenuAnimatedBanner` reads each layer's true size from
     its TGA header (`UI_GetTargaImageDimensions`, `:30-47`) and then
     multiplies by `scale` when drawing, so baked layers - already at
     scale - would render at 0.39 of their intended size. The tool
     therefore records the factor it used twice, in two formats, for two
     readers - a distinction found by checking rather than assuming, when
     the first version wrote only the first: `title-bake.json` beside the
     art, this tool's own manifest for `--check` and for a human, and
     `resource/title_bake.txt` inside the overlay, a `TITLE_BAKE_SCALE`
     key followed by its value. **`mainui` has no JSON parser at all.**
     It reads token streams through `EngFuncs::COM_ParseFile`, which is
     exactly how `colors.lst` is read (`BaseMenu.cpp:1003-1020`: a KEY
     line, then its value), so the second file is the one the fork side
     can actually consume, and it is what tells `CMenuAnimatedBanner`
     that its layers are already at scale rather than leaving the factor
     hard-coded in two places that can drift apart. `--check` refuses a
     bake whose marker and manifest disagree.
   - *Pre-HL25 strip and banners*: bake `btns_main.bmp` to a 1024x256
     atlas deliberately (so the 1024 cap never halves it blind) with the
     button rects rescaled - `Btns.cpp` slices by fixed pixel constants,
     so the fork gains a scale factor read from the baked atlas's size;
     `head_*.bmp` banners and `splash.bmp` to the 640 canvas, 8-bit or
     16-bit per profile. Ferrum56's P8 arithmetic (4 MiB → 1 MiB for the
     atlas) is the reference point; the measured post-traversal number
     is the acceptance.
   - *Packaging*: `tools/xbox-xiso.sh` gains `--menu-bake <dir>`, an
     overlay copied onto the staged gamedir AFTER the retail copy, so the
     Steam install is never modified and a disc without the overlay
     still works (stage A). The bake's own hash goes into the disc's
     `CHANGELOG.txt` provenance, next to the XBE's. `tools/asset-studio`
     gets the bake as a new mode (`--bake-menu-art --steam <dir> --out
     <dir> --renderer soft|nv2a`), sharing `UiPacker`'s layout parser
     and compositor - the archive writers stay for Ferrum56's benefit,
     unused here.

   Sequencing of the two stages: A first, always - the retail numbers are
   the baseline the bake is judged against, and the `ref_soft`-profile
   bake follows as soon as A's numbers say what to shrink (expected: the
   title). The NV2A profile waits for X9, like every other DXT consumer.

   **The work, in order, each measured with `tools/xemu-debug.py
   memstats`. Acceptance ceiling: the whole of a profile's menu art -
   background, title, buttons, banners - may cost at most 3 MiB of
   measured heap delta (under a fifth of X7.4's post-map free memory,
   leaving X8's own targets intact); anything over it takes that step's
   stated fallback, and the number is recorded here either way. How the
   delta is taken - review round 2 caught the first draft treating a
   snapshot at the first menu as "the whole profile": the `head_*.bmp`
   banners are NOT loaded with the menu. Each screen loads its own in
   its one-time `_Init()` (`CMenuOptions::_Init` → `banner.SetPicture`,
   `menus/Configuration.cpp:48-50`), `_Init()` is deferred until that
   screen is first shown (`controls/BaseWindow.cpp:31-36`, `controls/
   ItemsHolder.cpp:283-294`), and `CImage` has no unloading destructor by
   design (`Image.h:40-44`, `:58-67`) - so banners accumulate as screens
   are visited and never come back. An initial-menu reading can pass
   under 3 MiB and then blow through it during ordinary navigation,
   which is exactly the cost the ceiling exists to catch. So: the
   acceptance reading is taken AFTER a deterministic, pad-driven
   traversal that opens every retained screen of the shipping menu
   (whatever XM.1 leaves reachable: Options, Video, Controller, Audio,
   Load, Save, New Game's chapter list, and every dialog that carries a
   banner) and returns to Main; three numbers are recorded per profile -
   the first-menu delta, the peak during traversal, and the resident
   delta back at Main - and the ceiling applies to the resident delta
   after traversal, the peak recorded beside it. On HL25 content there
   are no banners to accumulate (no `gfx/shell` art), so the traversal
   there mostly proves the point; on pre-HL25 content it IS the
   measurement, and the alternative - freeing a screen's banner on close
   so the baseline returns - is a fork change to weigh only if the
   traversal number says the banners are what breaks the ceiling:**
   1. **DONE, 2026-09-16.** Replaced the `uiStatic.lowmemory` early return
      in `LoadBackground` and in `CBtnsManager::LoadBmpButtons`
      (`Btns.cpp:37`, which the first draft left under `lowmemory` and
      therefore left the pre-HL25 bitmap buttons impossible, review round
      1) with a new engine cvar, `ui_xbox_menu_art` (`host.c`, default 1
      on Xbox, 0 elsewhere - not read-only, unlike `host_lowmemorymode`,
      so it can be flipped for measurement). **Found live, not in review:
      the first implementation cached the cvar into `uiStatic` at
      `UI_Init` time, the same place `lowmemory` is cached - but
      `UI_Init` is exactly the point `BaseMenu.cpp`'s own comment on
      `LoadBackground` names as too early ("can't do this in Init, since
      these are dependent on cvar values set from user configs"), so a
      `valve/userconfig.d/*.cfg` override was silently ignored: `s_state`
      read `DRAW_STEAM` (background loaded) on a disc built specifically
      to test the opposite.** Fixed by reading the cvar live at each of
      the two call sites instead of caching it - confirmed by reading
      `CMenuBackgroundBitmap::s_state`'s own address out of the guest's
      memory directly (not inferred from a screenshot): `0` (`DRAW_COLOR`)
      with the override, `2` (`DRAW_STEAM`) without, on the identical
      binary. The blurred PicButton text (`PicButton.cpp:311`), menu
      sounds (`BaseMenu.cpp:1000-1001`) and the font path
      (`FontManager.cpp:91`) stay under `lowmemory` until each is
      measured on its own - they are not part of "renders the retail
      art". **Measured, same binary, `memstats` before/after**: `2224`
      `VirtualMemory` pages with the gate off, `2512` with it on - a
      **288-page (1.125 MiB) delta** for the background alone, under the
      lower end of the ~2.0-2.6 MiB estimate above and comfortably under
      the 3 MiB ceiling. **A second timing bug found, then a regression
      from the first attempt at fixing it, both Codex review - net
      result: `LoadBmpButtons()` stays exactly where upstream has it.**
      Round 1 found `LoadBmpButtons()` was called too early to ever see a
      `userconfig.d` override, live read or not - from `UI_Precache()`,
      called once from the FIRST `UI_VidInit()` (`BaseMenu.cpp:1035-1038`),
      itself called from `CL_Init` (`host.c:1282-1283`) - before
      `userconfigd` execs (`host.c:1372-1374`; `config.cfg` execs even
      later than `userconfigd`, so neither reaches this call site). The
      first fix moved the call (and the `buttons_draw_size` computation
      reading its result) into the same lazy block `LoadBackground`
      uses. **Round 2 found that move was itself a real regression**:
      every `CMenuPicButton` snapshots `uiStatic.buttons_draw_size` into
      its own `size` at CONSTRUCTION time, and construction happens
      inside `UI_Precache`'s own `entry->m_pfnPrecache()` loop - before
      the deferred block ever runs. Deferring the call left every menu
      button with a zero-sized interaction rectangle (mouse focus/clicks
      read `ItemsHolder.cpp`'s `m_scSize`, not the drawn bitmap) for its
      first frame, a cross-platform regression, not an Xbox-only risk.
      **Reverted**: `LoadBmpButtons()`/`buttons_draw_size` are back at
      their exact upstream call site in `UI_Precache`, unmoved. The
      honest cost: `Btns.cpp`'s live read of `ui_xbox_menu_art` still
      cannot see a `userconfig.d` (or even `config.cfg`) override at that
      early point, so a same-boot A/B of the button-strip gate is not
      possible the way the background gate's is - a genuine test needs
      two separate builds (flip the compiled default, rebuild). **Not
      attempted this session**: this session's own HL25 disc ships no
      `gfx/shell/btns_main.bmp` at all, so `LoadBmpButtons()` finds
      nothing to load either way regardless of timing or gate value -
      this whole question only matters for the pre-HL25 profile (step 5),
      which needs its own disc first. **Superseded in part, 2026-09-17,
      by XM.1 item 13**: that item stops the strip drawing on Xbox
      entirely (every row becomes text, so the menu can be translated),
      and `Btns.cpp:42`'s early return then skips the load on both
      profiles regardless of `ui_xbox_menu_art`. So the two-build A/B
      this step deferred is no longer worth building - the strip's cost
      becomes zero, not smaller, and what needs measuring in its place is
      the glyph atlas item 13 ships. The background half of this gate is
      untouched and still matters exactly as written.
   2. **DONE, 2026-09-16, but the measurement did not confirm what it
      changed.** Masked `GFL_HD_BACKGROUND` on Xbox
      (`cl_gameui.c:402-403`, `#if !XASH_XBOX`, divergence row below) so
      `hd_background 1` no longer makes `LoadSteamBackground` try
      `resource/HD_BackgroundLayout.txt` first. **The `memstats` delta
      before and after this change was identical (2512 pages either
      way)** - inconclusive on whether the 800x600/12-tile layout was
      actually what loaded before the mask too (`LoadSteamBackground`
      only falls back to the small layout when the HD layout FILE itself
      is missing, not when a tile it references fails to load - a
      partial HD load stopping after roughly the same number of tiles as
      the small layout has is a live possibility, not read out). Left
      open rather than asserted: a live count of `s_SteamBackground`'s
      entries (or a breakpoint on the per-tile `PIC_Load` call) would
      settle it, not attempted this session. **A separate, unrelated
      finding this step's testing produced, worth recording precisely so
      it is not re-discovered as a "rendering bug" later: the retail
      800x600 background tiles are genuine RGB(0,0,0), alpha 255, opaque
      black - verified by reading raw pixel bytes out of
      `resource/background/800_2_b_loading.tga` on the host, 20 random
      samples all `(0,0,0,255)`, and cross-checked on `800_1_a_loading.tga`.
      The menu therefore paints solid black either way, gate on or off,
      by design of the retail content, not a `ref_soft` texture-upload
      bug** - HL25's redesigned menu evidently puts all its visual weight
      on the animated title layered over a plain black backdrop, not on
      the tiled background image the classic WON menu had. Step 4's
      measurement is where this port's own "does the menu look right"
      question actually gets answered.
   3. **DONE, 2026-09-17.** Make the two background loads exclusive as
      described above (probe, prefer, load one, free on flip), so a
      mixed-content disc is measured with ONE set resident, never both.
      Confirmed the two loads were NOT exclusive before this: `LoadBackground()`
      (`3rdparty/mainui/controls/BackgroundBitmap.cpp`) called
      `LoadSteamBackground()` AND `LoadWONBackground()` unconditionally
      whenever each existed, then `UpdatePreference()` only ever decided
      which one to DRAW - the loser stayed loaded (and resident) for the
      rest of the session, on any platform. **Fixed**: load both once
      (re-parsing just to decide existence risks disagreeing with the real
      loaders' own validation, so probing by actually loading is the
      honest option), decide via the existing `UpdatePreference()`, then
      free whichever side lost via two new helpers, `FreeWONBackground()`/
      `FreeSteamBackground()` - `EngFuncs::PIC_Free(name)` frees by name
      (`menu_int.h:65` → `GL_FreeImage` → `GL_FindTexture`/`GL_FreeTexture`,
      a real free, already used this way elsewhere in mainui - `ColorPicker.cpp`,
      `LoadGame.cpp`), so each Steam tile's load-time filename now lives in
      `bimage_t` alongside its handle (`Image_LoadBMP`'s own `PIC_Load` name
      strings were never retained before). The runtime flip path (`Draw()`'s
      `ui_prefer_won_background` `FCVAR_CHANGED` handler) previously only
      called `UpdatePreference()`, safe when both were always resident;
      now it (re)loads whichever side just became preferred if it isn't
      loaded, then frees the side losing the flip. Not `#if XASH_XBOX`-
      guarded: a mixed-content disc wastes the same memory holding two
      resident backgrounds on any platform, and freeing the one never
      drawn changes nothing a player can see - Form 1, entirely within
      this project's own `mainui_cpp` fork (divergence #57).
      **Verified live**, since no real disc anywhere carries both content
      sets: baked a synthetic one with `tools/asset-studio --bake-menu-art`'s
      existing output overlaid (`tools/xbox-xiso.sh --menu-bake`) onto the
      real classic WON install, so the disc genuinely has both
      `gfx/shell/splash.bmp` and a `resource/BackgroundLayout.txt` +
      tiles. Read live off the guest (`.map` RVA+Base addresses for the
      class's static members, direct memory reads, the pattern used
      throughout this item): `s_state=DRAW_STEAM` (the correct pick -
      `ui_prefer_won_background` defaults `"0"`, Steam preferred when
      both exist), `s_WONBackground.hImage=0` (freed, correctly), and
      `s_SteamBackground` fully intact - 6 valid entries, handles and
      tile coordinates/sizes matching the bake exactly. The exclusivity
      mechanism itself is confirmed correct at the data level.
      **Codex review round 1 found two real failure modes** in the
      runtime flip path (a player toggling `ui_prefer_won_background`
      after `LoadBackground()` already freed the loser), both fixed:
      `s_bGameHasSteamBackground`/`s_bGameHasWONBackground` only turned
      true on a gamedir-specific load, never the base-directory fallback
      - harmless before this row, since both sides always stayed loaded
      and `UpdatePreference()`'s own secondary check (what's actually
      resident) caught a base-only background regardless; with the loser
      now freed, that secondary signal disappears for the un-shown side,
      so a base-only background read as permanently unavailable on a
      flip. Both branches now set the flag. Separately, the flip handler
      froze the old background unconditionally after attempting to load
      the new one, even when that load failed (`PIC_Load` returns 0
      under memory pressure or a missing file) - discarding a working
      background for one silently not there; it now only frees the
      losing side once the replacement actually loaded, restoring the
      previous state otherwise. `LoadSteamBackground()` also left a
      partial tile list behind when a later tile in the layout failed to
      parse, which a subsequent `Count() > 0` check could mistake for a
      complete background - it now rolls back exactly what the failed
      attempt appended. Re-verified live after the fix: same disc, same
      `s_state=DRAW_STEAM`/`s_WONBackground.hImage=0` result as before.
      **Codex review round 2 found the reason `s_WONBackground.hImage=0`
      wasn't actually proof of freed memory**: `UI_Precache()`
      (`BaseMenu.cpp`) unconditionally preloads the same splash art under
      the key `"gfx/shell/splash"` (no extension) as part of its general
      icon precache, completely independent of this file's own
      `ART_BACKGROUND` (`"gfx/shell/splash.bmp"`) - and `GL_LoadTexture`
      caches by the EXACT string a caller passes (`r_image.c:587`),
      before imagelib's own extension resolution ever runs, so the two
      different keys created two SEPARATE resident copies of the
      identical decoded image. Freeing `s_WONBackground`'s copy left
      `UI_Precache()`'s untouched regardless - the live handle check
      wasn't wrong about ITS handle, but it wasn't proof of actual
      residency either. Fixed by loading and freeing under the SAME key
      `UI_Precache()` already uses (new `ART_BACKGROUND_PIC` constant) -
      `FileExists()` still needs `ART_BACKGROUND`'s real extension (a
      literal filesystem check, no probing of its own), but `PIC_Load`/
      `PIC_Free`/the stored `img.name` all move to the extensionless key,
      so both call sites share one cache entry and freeing it frees the
      one real resident copy. Re-verified live: `s_WONBackground.name`
      now reads `"gfx/shell/splash"`; `VirtualMemory` on the same
      synthetic disc dropped from 3501 to 3350 pages (~0.59 MiB) versus
      the pre-this-fix reading - real freed memory, not just a
      zeroed local handle. **Second finding, same round**:
      `LoadSteamBackground()` called `PIC_Load` for a tile BEFORE parsing
      its remaining fields (a "scaled" attribute, two coordinates); any
      of those three later parses failing left that texture loaded but
      never appended to `s_SteamBackground`, unreachable to the
      round-1 rollback (which only walks entries already in the vector).
      Restructured so `PIC_Load` is the last fallible step, immediately
      before `AddToTail` - a malformed layout line can no longer strand
      a texture. This was Codex review's SECOND round on this fix; per
      the standing two-round cap, both findings were fixed rather than
      re-submitted for a third pass.
      **Found and set aside, not fixed here**: the on-screen result was a
      solid black background under otherwise-correct WON-style menu text
      - traced one level further (dumping the loaded tiles' own pixel
      data) to the CURRENT Steam install's actual `resource/background/
      800_*.tga` source files themselves reading back mostly zero-alpha
      at the sampled points, i.e. this is what the real content composites
      to today, not a bug this fix introduced - confirmed by packaging
      the exact same synthetic mixed disc against the PRE-fix binary
      (`oldfont-fix.xbe`) and getting byte-identical solid-black output.
      Whether that is expected (the 25th-anniversary build's tiles really
      are solid black per divergence #60's own earlier finding) or a
      real bake/alpha-compositing bug is unresolved and out of scope for
      this row - a background-art issue, not a residency issue. No
      regression on either single-profile disc (classic, Steam), each
      re-booted with the fixed binary and screenshotted unchanged.
   4. **DONE, 2026-09-16 - and it needed one more fix than "confirm it
      draws as-is".** First attempt: nothing drew, on a disc with all
      eleven TGAs present and readable. Diagnosed by breaking on
      `CMenuAnimatedBanner::TryLoad` (not by guessing from the black
      screen) - the breakpoint never fired, even from a guest reset, so
      `Main.cpp:271`'s `if( FBitSet( gMenu.m_gameinfo.flags,
      GFL_ANIMATED_TITLE ))` was taking the FALSE branch and adding
      `movieBanner` instead (itself correctly invisible on Xbox, per
      XG.1... no, per this same item's own earlier finding on `logo.avi`).
      **Root cause, checked directly against the real Steam install, not
      assumed: `gameinfo.txt` does not carry `animated_title`/
      `hd_background` at all, even though `liblist.gam` sets both to 1**
      - and `filesystem/gameinfo.c`'s `FS_ParseGameInfo` prefers
      `gameinfo.txt` whenever it exists and is not older than
      `liblist.gam` (true here: real mtimes checked), never re-deriving
      from `liblist.gam` in that case. This corrects this session's own
      earlier claim (X7.5, "gameinfo.txt already carries everything
      liblist.gam does") - wrong for exactly these two fields, and it is
      also the real explanation for step 2's "measured inconclusive"
      result: `GFL_HD_BACKGROUND` was never being set from this disc
      either way, mask or no mask, so masking it changed nothing to
      measure. **Fix**: `tools/xbox-xiso.sh` now copies `animated_title`/
      `hd_background` from a staged gamedir's `liblist.gam` into its
      `gameinfo.txt` when the latter is missing them - forwarding what
      the disc's own `liblist.gam` already says, never inventing a value,
      never touching a title whose `liblist.gam` doesn't set the flag.
      **Confirmed after the fix**: `TryLoad()` now fires and returns true
      (read directly from the guest, not inferred), and the screenshot
      shows the real HL25 title - "HALF-LIFE" over the blurred parallax
      backdrop, drifting between two captures a few seconds apart. No
      alpha bug like Ferrum56's (`CHANGELOG.md:1403-1466`) - the blend
      looked correct on sight, not separately measured pixel-by-pixel.
      **Measured, same binary (`menuart5.xbe`), same-session**: `2512`
      `VirtualMemory` pages with only the background loaded (title
      correctly skipped, pre-fix disc) vs `3836` with the title fully
      loaded too - a **1324-page (5.17 MiB) delta**, matching the
      ~5.2 MiB estimate closely. Comfortably busts the 3 MiB ceiling on
      its own, exactly as predicted - the stage-B bake (~2.1 MiB
      estimate above) is the next real step, not yet done. **A second,
      smaller measurement, worth recording on its own**: hiding just ONE
      of the eleven required TGAs (so `TryLoad()` fails and adds no
      banner at all) only recovered ~256 pages (1.0 MiB) against the
      full title's 1324 - most of the other ten files' textures stayed
      resident despite the failed load adding nothing visible, matching
      `CImage`'s documented no-unload-on-failure design (`Image.h:40-44`,
      already named in this item's own arithmetic section) - a real,
      measured consequence of that design, not just a theoretical risk
      from reading the header comment.
   5. Pre-HL25 profile: with a disc built from older content. **Correction,
      2026-09-16: "the local Steam install is HL25 only" no longer
      holds** - Half-Life's own Steam distribution is not a fixed target;
      partway through this same session the local install changed from a
      25th-anniversary build (genuinely solid black `resource/background/
      *.tga`, all eleven `resource/logo*.tga` layers present,
      `liblist.gam` setting `animated_title`/`hd_background`) to an
      older, pre-anniversary release (real photographic art in the same
      12 tiles - confirmed live, not assumed, by the screenshot changing
      from black to Gordon Freeman's face - `resource/logo*.tga` gone
      except an unrelated `logo_game.tga`, `liblist.gam` no longer
      setting either flag) - apparently a Steam beta-branch switch on the
      user's own account, not a bug in anything this project owns. Two
      things follow: divergence #59's gameinfo sync (copies a flag from
      `liblist.gam` only when it is actually set there) degrades to a
      correct no-op on this older content, exactly as its own "closes
      if..." row describes - checked live, not just reasoned about, since
      the sync ran against this exact content and correctly added
      nothing. And this older Steam release, while genuinely different
      art, is STILL not the classic WON content this step actually needs
      to test (`gfx/shell` there still carries only the `kb_*.lst` files,
      no `btns_main.bmp`, no `splash.bmp`) - a real pre-WON/box-retail
      install (in progress on the user's own machine via an InstallShield
      extraction as this is written) is what closes this step, not a
      different Steam vintage. **Update, 2026-09-16, later the same day:
      that install exists (`G:\Games\Half Life`, Sierra/WON 1.0.0.9 -
      `hlversion "1009"`, `liblist.gam` only, no `gameinfo.txt`, no
      `resource/` at all; `gfx/shell/btns_main.bmp` and `splash.bmp` live
      inside `pak0.pak`, 3453 entries) and a disc built from it boots to
      the real WON menu - "New game / Hazard course / Configuration / Load
      game / Multiplayer / Previews / Quit" over the grey lambda
      `splash.bmp`, i.e. `DRAW_WON`, the first pre-Steam boot this port has
      done.** Two things had to be learned first. (a) That content ships
      no `delta.lst` (the file only exists from 1.1.0.0 on), so
      `SV_Init` → `Delta_InitFields` → `Sys_Error` fired before any
      renderer existed and the host restarted into it every ~3 s - the
      disc sat on the nxdk splash for as long as anyone watched, and the
      halted CPU (kernel idle wait, a title-thread stack that happened to
      end in `stat()`) sent the diagnosis down a `stat("D:/")` path, a
      disc-size path, a file-count path and an intro-video path, each
      ruled out live, before a milestone breakpoint trace read the
      `Sys_Error` off the running engine (`docs/r3d/lessons.md`, both new
      rules). `tools/xbox-xiso.sh` now refuses such a gamedir unless
      `--delta-lst` supplies one from a later install (divergence #61) -
      a packaging gate, not an engine change, since `delta.lst` is Valve
      content like everything else on the disc. (b) **Open, the next
      thing this step owes:** the WON bitmap button strip renders as
      colour garbage - a field of multicoloured blocks down the right
      half of the menu area and a yellow band along the bottom edge -
      while the text buttons draw correctly on top of it, and the
      `Previews` entry (WON-only) is present. That is exactly the "if the
      strip does not survive, the buttons are wrong, not just costly"
      case anticipated below, though the failure is corruption, not the
      1024 cap halving it; it needs the strip's real format (8-bit
      paletted BMP out of a pak, through `PIC_Load`) read against what
      `ref_soft` uploads before deciding between a loader fix and
      `GFL_RENDER_PICBUTTON_TEXT`. Not measured against the ceiling yet.

      **Update, same day, later still: read against what `ref_soft`
      uploads, per the line above - the loader is provably innocent, on
      both fonts and pictures.** The description text next to each item
      is ALSO corrupted (noticed live, watching the boot), and that text
      draws through the completely unrelated console-font path
      (`gfx.wad`'s `CONCHARS` lump, `img_wad.c`'s `Image_LoadFNT`, its own
      "oldstyle font" branch for content whose file size predates the
      HL 1.1.0.0 `qfont_t` layout - exactly this content's vintage,
      confirmed live: computed size 26450 vs the lump's real 34580) - a
      second image, decoded by a second loader, corrupted the same way.
      Both were checked the only way that settles it: reproduce the
      engine's own byte-exact parsing on the host and look at the result.
      `tools/asset-studio`-style host ports of `Image_LoadBMP`'s atlas
      math (`Btns.cpp:35-195`) and of `Image_LoadFNT`'s oldstyle path
      against the real `btns_main.bmp` (extracted from `pak0.pak`) and the
      real `CONCHARS` lump (extracted from `gfx.wad`, WAD3's own header -
      `filepos`/`size` read from the lump directory, not guessed) both
      render perfectly legible output - all 62 WON button labels on the
      atlas, the full ASCII console font on CONCHARS. Both loaders are
      correct. Live memory then confirmed where it actually breaks:
      `tools/xemu-debug.py` breakpoints on `R_DrawStretchPicImplementation`
      (`ref/soft/r_draw.c:38`) caught a real glyph draw
      (`s1=48,t1=80,s2=64,t2=96`, a 16x16 cell) and a real button draw
      (`s1=0,t1=0,s2=156,t2=26`, button 0's normal state) mid-frame;
      reading `pic->pixels[0]`/`pic->alpha_pixels` at those exact
      coordinates - decoded back to RGB through the exact inverse of
      `GL_UploadTexture`'s major/minor bit-packing (`r_image.c:259-271`,
      verified term-by-term against `MOVE_BIT`/`GET_BIT`, `r_local.h:
      77-79`) - found the uploaded buffer reads **zero everywhere**: a
      cell known from the host repro to be blank AND a cell known to hold
      real ink (index 0, RGB `(46,40,22)`, expected pixel_t `0x241C`)
      both come back `0x0000`, foreground indistinguishable from
      background. A wrong bit-packing formula would produce plausible-but-
      wrong nonzero values, not a clean, uniform zero matching
      `Mem_Calloc`'s own initial state (`r_image.c:252` - the buffer
      `GL_UploadTexture` allocates for `tex->pixels[j]`) - the per-pixel
      conversion loop (`:257-280`) is not actually writing real source
      data into it, for either asset, even though both textures register
      the correct dimensions and both allocations land at valid,
      consistently-reproducible addresses across a fresh `system_reset`.
      **Found and fixed, same day: `pic->buffer` itself, checked live at
      `GL_UploadTexture`'s own entry (breakpoint on `r_image.c:214`,
      before any of its own code runs) - not empty, but the WRONG SIZE.**
      `pic->size` read `32768` (`width*height`, one byte per pixel) where
      every consumer below - `GL_ResampleTexture`, the per-pixel mip
      loop - assumes `width*height*4` (RGBA). Root cause:
      `GL_ProcessImage` (this same file) computes
      `img_flags |= IMAGE_FORCE_RGBA` for an indexed `pic->type` but never
      applies it - `img_flags` is a dead local, set and never read again,
      upstream's own gap (checked: not something this fork introduced).
      `Con_LoadFixedWidthFont("gfx/conchars", ..., TF_KEEP_SOURCE)` (the
      console font's own load, `engine/client/console.c:592`) is the
      specific trigger: `TF_KEEP_SOURCE` becomes `IL_KEEP_8BIT`
      (`ref/soft/r_image.c`'s own `GL_LoadTexture`), which makes
      `Image_AddIndexedImageToPack` store raw palette-index bytes instead
      of expanding them - legitimate, upstream behaviour, meant to keep
      `pic->original` around for later re-tinting - but nothing after it
      in `ref_soft` ever performs that expansion, so `GL_UploadTexture`
      read index bytes 4-at-a-time as if they were already RGBA: real
      data for the first `width*height/4` "pixels" (reinterpreted index
      bytes, which is what the semi-plausible-looking early rows were),
      zero (`Mem_Calloc`'s own initial state, never written) past that
      up to the true end of the small allocation, then genuinely unmapped
      memory - measured precisely: `pic->size=32768`, real data through
      row 31 of a 256-wide image (`32768/4/256=32`), zero rows 32-91,
      unreadable beyond. **Fixed** in `GL_UploadTexture`: expand an
      indexed `pic->buffer` to RGBA before the mip loop touches it, using
      `pic->palette` - always `(R,G,B,A)` byte order regardless of
      source format, since `Image_GetPaletteBMP`/`Image_GetPaletteLMP`
      both funnel through `Image_SetPalette`'s `HostFourCC` into the same
      `d_currentpal` (`img_utils.c:284-346`) - unlike `Image_LoadBMP`'s
      own indexed case, which reads the BMP file's raw on-disk `(B,G,R,A)`
      palette directly and was never affected (it expands to RGBA itself,
      inline, before `GL_UploadTexture` ever sees it - which is exactly
      why the button atlas, BMP-sourced, did not show this exact defect
      even though its own row-200 sample from earlier looked close-but-
      not-exact against the host repro; worth a second look, not
      re-opened here). **Verified live, same disc, same binary**: the
      screen's colour-noise grid and the yellow/black bottom band are
      gone; real console-font glyphs render at each menu row's hint-text
      position and on the bottom status line instead - legible, not
      noise, but NOT the intended text: `"PQ RST"`, `"YZ WX"`, `"HIJI"`,
      `"ghabc"` are consecutive-alphabet slices. **That is a second,
      separate, pre-existing bug, analysed the same day (not fixed in
      the same change):** the console font is being loaded through the
      FIXED-width path. `Con_LoadConsoleFont` (`engine/client/console.c:
      554-595`) only takes the legacy variable-width path
      (`Con_LoadVariableWidthFont("gfx/conchars.fnt")`) when
      `con_oldfont` is set, and nothing sets it on Xbox - default `"0"`,
      the only setter is the `-oldfont` command-line parameter
      (`Con_VidInit`, `:2241-2242`), and an Xbox boot has no command
      line. With it off, it tries FWGS's own modern font resources
      (`font%i_<charset>.fnt` behind a `fonts.wad` CRC gate, then
      `fonts/font%i`) - those ship in xash-extras, are in no retail
      install and not on any disc this project packs, so both fail - and
      falls to `Con_LoadFixedWidthFont("gfx/conchars", ...,
      TF_KEEP_SOURCE)` (`:592`), which never reads a glyph table: it
      assumes a 16x16 grid of `font_width/16` cells (`cl_font.c:105-115`)
      and gives character `c` the cell at `(c%16, c/16)`. The WON
      `CONCHARS` lump is a variable-width qfont: its `charinfo` table
      is valid (dumped from the lump: `' '` 4px, `'!'` 3px, `'I'` 3px,
      `'M'`/`'W'` 9px, glyph rows 12px apart - `'J'` starts at pixel row
      12, `rowheight=12`), glyphs packed edge to edge, so a 16px cell
      holds whatever two or three neighbours sit there - `'S'` (83:
      column 3, row 5) draws the slice containing `"RST"`, which is
      exactly what the screen shows. Why Steam content never showed it:
      its `CONCHARS` is the 1.1.0.0+ layout, where `Image_LoadFNT`
      computes `image.width = font.width * QCHAR_WIDTH` - a real 16px
      grid, so the fixed fallback happens to coincide; the oldstyle
      branch (width hardcoded to 256, height from the header) has no
      such grid to coincide with. The variable-width loader would be
      right for both vintages: the WAD layer exposes a `QFONT` lump as
      `gfx/conchars.fnt` (`filesystem/wad.c:83`), and
      `Con_LoadVariableWidthFont` reads exactly the header and table
      dumped above (`cl_font.c:120-160`); the fixed fallback stays as
      the safety net behind it. **Fixed** the same way this was predicted:
      `con_oldfont` now defaults to `"1"` under `#if XASH_XBOX` in
      `engine/client/console.c` (divergence #63) - a disc without
      xash-extras can never succeed on the modern branch anyway, so
      going straight to the legacy, variable-width path is the honest
      default, and the existing fallback chain still handles a `gfx.wad`
      with no `CONCHARS` at all. **Verified live on both discs**: the
      classic WON disc now shows the real menu description text
      ("Start a new game.", "Learn how to play Half-Life", ...) and the
      correct bottom version line instead of alphabet-slice gibberish;
      a Steam-content disc, rebuilt with the same fixed binary, is
      unchanged - same correct text as before, no regression (its
      `CONCHARS` was already on the lucky 16px-grid layout). One
      unrelated, pre-existing issue surfaced during verification and is
      NOT part of this fix: the classic disc's "Quit" menu item shows
      the unresolved localisation key `GameUI_QuitConfirmationText`
      instead of its string - a missing/unresolved titles.txt entry, not
      a font or glyph-cell problem; not investigated further here.
      `r_numImages` was read live along the way and ruled out as a factor
      (38 of `MAX_TEXTURES` 1024, nowhere near the `XASH_LOW_MEMORY` cap).
      **The 1024-cap question, closed 2026-09-17**: the ladder picks
      `splash.bmp` + the bitmap strip correctly (confirmed by every
      screenshot in this step, buttons and description text both
      correct). The real retail `btns_main.bmp` is `156x4836`, 8bpp
      (read from its own BMP header, not assumed) - `Btns.cpp`'s own
      atlas-sizing loop (`:53-116`, this fork's own code, not upstream's)
      computes `pic_count = min(4836/26/3, 62) = 62` exactly (`4836/78`
      is a clean `62.0`, not a truncated fit), `cell_w=156+2=158`,
      `cell_h=(26+2)*3=84`, and searches `{256,512,1024}²` for the
      smallest POT page holding `cols*rows >= 62`: every combination
      under `1024x1024` falls short (`512x1024` fits only 36, `1024x512`
      only 36), and `1024x1024` fits `6*12=72`, so a single
      `#btns_atlas_0.bmp` page is chosen - by construction, never
      handed to `GL_SetTextureDimensions`' strict `>1024` halving check
      at a size that check could touch. **Verified live, not just by the
      arithmetic**: scanned `r_images[]` (`ref/soft/r_image.c:20`,
      `sizeof(image_t)=312` computed from the linker map's own
      `_r_images`/`_r_numImages` address delta divided by `MAX_TEXTURES`,
      not guessed) for the `#btns_atlas_0.bmp` entry and read its real
      `srcWidth`/`srcHeight`/`width`/`height` directly: `1024x1024` on
      all four, exactly as computed - the strip survives the cap
      untouched, buttons are correct, not a `GFL_RENDER_PICBUTTON_TEXT`
      downgrade case. The missing title on WON content degrades exactly
      as the code reads: no banner, no error, confirmed by every one of
      this step's own screenshots showing a clean menu with no
      placeholder art and no console error spam.
      **Acceptance-ceiling measurement: not done this session, and after
      a second Codex review round this is now a deliberate deferral, not
      an oversight.** A raw `VirtualMemory` reading at the WON main menu
      was taken with the current merged binary (PRs #78-#80): `3277`
      pages (12.80 MiB), recorded as diagnostic context only - not a
      ceiling measurement, for the reasons below.

      Two drafts of a measurement procedure were written this session
      and both were caught by Codex review as unsound before either was
      run, which is worth recording so the next attempt doesn't repeat
      either mistake. First draft: a before/after-traversal comparison
      on ONE build, which cannot see the background+button cost at all
      (`UI_Precache()`, `BaseMenu.cpp:867-913`, loads both before the
      first menu frame ever draws - there is no "before" moment left to
      diff against on one build), and whose traversal covered only the
      four top-level screens, missing that `Configuration` alone opens
      five further screens (Controls, Audio, Video, Touch, Gamepad),
      each with its own distinct banner loaded on first visit
      (confirmed: `#define ART_BANNER` is a separate file per screen,
      not shared - `head_controls`, `head_audio`, `head_vidmodes`,
      `head_touch`, `head_gamepad`), plus Controls' own child Advanced
      Controls (`head_advanced`). Second draft: a two-build `ui_xbox_
      menu_art` on/off comparison, meant to fix the first problem by
      isolating background+button cost as a delta - which it does
      correctly (a live flip already measured the background's own
      piece, 288 pages, item 1) - but Codex's second round caught that
      per-screen banners are NOT gated by this cvar at all
      (`Configuration.cpp:50`'s `banner.SetPicture` and every sibling
      screen's own `_Init` are unconditional), so identical banner costs
      appear in BOTH builds and cancel out of the delta entirely - the
      "whole profile" claim was wrong specifically for the part
      (banners) that this step's own earlier finding says is the
      likeliest thing to blow the ceiling during ordinary navigation.

      **Why this isn't worth a third draft right now, and what actually
      needs to happen first**: the full stock FWGS screen graph reachable
      today (`Configuration`'s children, `Multiplayer`'s own children -
      Internet/LAN game each with their own `ART_BANNER_INET`/
      `ART_BANNER_LAN`, `ServerBrowser.cpp:1403-1413`, Player Setup,
      Controls again - `Touch`'s three children, each with a banner)
      is NOT what ships on Xbox: XM.1 (this item's own stated successor,
      "Sequencing" above) explicitly flattens Main and hides `Touch*`,
      `Gyro`, `ServerBrowser`/`Multiplayer` entirely under
      `#if XASH_XBOX` until Xbox netcode exists - Codex's own suggested
      alternative fix was exactly this: narrow the ceiling to the
      post-XM.1 shipping screen graph rather than the full stock one.
      Measuring the current, pre-XM.1 tree in full would spend real
      effort precisely costing screens (Multiplayer, ServerBrowser,
      Touch, Gyro) that will not exist in the shipped build at all - a
      wasted, and misleadingly pessimistic, number. The full, correct
      whole-profile ceiling measurement is therefore sequenced to run
      AFTER XM.1 trims the reachable screen graph down to what will
      actually ship (the 2026-09-17 design, XM.1 below: Main with Resume
      / New Game / Training Room / Load / Save / Options; Options with
      Controller / Audio / Display / Game - and text headings in place of
      every `head_*.bmp`, so the banner-accumulation half of this
      measurement is zero by construction on that graph), against a
      genuine, minimal screen list, using the decomposition both review
      rounds converged
      on: background+button-strip cost from the `ui_xbox_menu_art` A/B
      (two builds, since the button gate can't see a same-boot config
      override - step 1's own prior finding), PLUS the surviving
      screens' banner-accumulation cost from a single build's first-
      menu-vs-post-traversal resident delta (valid for banners
      specifically, since they are the only thing that changes state
      between those two readings within one build). Both halves need a
      human at a controller either way: `xemu`'s emulated Xbox
      controller port is fed by the host's real gamepad/keyboard-to-pad
      mapping inside its own GUI, not by anything `tools/xemu-debug.py`'s
      GDB-stub connection can inject (QEMU monitor `sendkey` reaches an
      emulated PS/2 keyboard the guest has none of).
   6. Both profiles: `ui_prefer_won_background` stays a real cvar so a
      pre-HL25 disc that carries both sets can be flipped - and it is a
      candidate for the Video screen (item 2) only if a disc that has
      both actually exists to test it with.
   7. **Art this fork authors rather than inherits - written 2026-09-18,
      not staged and not measured.** Everything in steps 1-6 is retail
      content this port reshapes. The Controller screen (item 5) needs
      one picture that has no retail ancestor at all: the pad diagram its
      button assignments hang off. That makes it the first menu asset
      whose *source* the fork owns, and the first that could quietly
      become the one file nobody can regenerate or review as a diff, so
      the provenance rule is written here once rather than per asset.

      **Generator: `tools/xbox-menu-art.py`, standard library only.** It
      bakes `assets/gfx/shell/duke.tga` from a master render and, with
      `--check`, re-renders and compares byte for byte, so the tracked
      asset can be proven still to come from its stated source. The same
      `--check` validates the already-shipped `legend_button.tga`
      (item 12) by shape rather than by bytes - square, pure white
      wherever visible, one radially symmetric alpha island that never
      grows outward again, which is what `Legend.cpp:157` relies on when
      it tints one image into six glyphs. Byte equality would be the
      wrong test there: that file was made by a different tool and
      shipped on a verified disc, so a mismatch would mean "different
      tool", not "wrong file". Its edge is also softer than it looks -
      measured off the file, alpha is solid to r=12.5 and only reaches
      zero at r=17.2 - and `--regen-legend` reproduces that falloff
      rather than a crisp disc, so the option cannot quietly restyle
      something that already shipped.

      **Two formats, because the master and the console asset have
      opposite requirements.** The master arrived as an 8600x4800 32-bit
      TGA: 165 MB, of which 4582x3394 is content and the rest is the
      drawing program's canvas padding. Tracking that is 954 times the
      bytes of the same picture for a file no build step reads. **Rule:
      a master is tracked as PNG RGBA, cropped to its own content, at
      four times the baked width** - here `assets/resource/XboxDuke.png`,
      1120x830, 173 KiB - which is lossless, keeps the alpha, and leaves
      enough headroom to re-bake if the NV2A ever draws this menu at
      720p, without tracking the original export. **Game art stays
      uncompressed 32-bit TGA**, the convention every other asset on
      these discs already uses. PNG would also work on the console -
      `img_utils.c:99-101` registers the decoder and `img_png.c` is in
      the Xbox build - and would be roughly a third the disc bytes, but
      inflate wants the compressed and decompressed buffers live at once,
      and on 64 MB that peak is worth more than 140 KiB of DVD.

      **Size, chosen by looking rather than by arithmetic: 280x208.**
      Baked at 200, 240 and 280 and compared at 1:1, which is how a
      640x480 CRT shows them: at 200 the face-button letters and the
      XBOX wordmark stop resolving. Flattened to opaque, not kept with
      alpha - the HL25 tiles behind it are solid black (measured in step
      3), so the two are indistinguishable on screen while the opaque one
      skips the second full-resolution alpha buffer `r_image.c:254-255`
      allocates for any texture that keeps it. Cost by the same formula
      as the title above: **228 KiB on disc, 151 KiB resident.** The
      reduction is 27x, so the resize is an area average, not the
      four-tap bilinear the title bake uses, and premultiplied for the
      same reason. A vector fallback stays in the script for when no
      master is present; it is a diagram, not the illustration, and says
      so when it runs.

      **What this step has NOT done, and what it costs the ceiling.**
      `duke.tga` is written into `assets/` and now staged onto every
      Xbox disc build too (`tools/xbox-xiso.sh`, divergence #83), but
      nothing loads it yet: item 5's Controller screen is unported, so
      there is still no consumer and no `memstats` reading. When it
      ships, its 151 KiB counts against this item's 3 MiB ceiling in
      the same way a banner does, not in the way the background does -
      `CImage` has no unloading destructor by design (`Image.h:40-44`),
      so it is resident from the first visit to Controller until the
      title exits, and it belongs in the post-traversal resident delta
      step 5 defines, not the first-menu one.

   **Exit for this item:** the HL25 disc (`../XISOS/2026-09-16_hl-title-
   disc/` or its successor) boots to the tiled 800x600 background and a
   title (animated, or the static/middle fallback - whichever the ceiling
   allowed, recorded with all three `memstats` numbers: first menu, peak
   during the full-screen traversal, resident back at Main); a pre-HL25
   disc boots to the WON splash with bitmap buttons, or with text buttons
   if the strip did not fit, did not survive the 1024 cap, or the
   post-traversal banner residency broke the ceiling (recorded either
   way, same three numbers), and no title; on a disc carrying both sets,
   only the displayed profile's textures are resident (measured, not
   asserted); the `logo.avi` path is verified to no-op rather than error
   on both. Hardware per the usual rule.

**The design, 2026-09-17 - the target for every item below is the
"Half-Life Xbox Menu" canvas
(https://claude.ai/code/artifact/51a8c032-54b1-4e1d-bf22-2ca867eb8584):
screens drawn at 640x480 and shown at 2x, one artboard per screen,
each with a note naming the `menus/*.cpp` it maps to.**

**Expanded the same day, from ten artboards to twenty-one - read before
the items below, because it moves a whole half of the menu out of XM.1.**
The canvas gained three groups: **multiplayer** (Multiplayer hub, System
Link browser, Host Game, Direct Connect, Split Screen, Player Profile,
Internet Games, Master Servers), **pause menus** (single-player Paused,
multiplayer Game Menu), and a **shared on-screen keyboard**. Its own two
new titles say what it is aiming at: "Multiplayer - System Link over
lwIP, master servers configurable, no Xbox Live" and "Pause menus -
START in game". The single-player screens that were already drawn are
unchanged except Main, which gains a Multiplayer row.

That split is now the plan's own: **XM.1 stays the single-player
console shape** (Main through Game, plus the single-player pause
screen - nothing in it needs a socket), and everything multiplayer
becomes **XM.3**, sequenced behind a new networking phase, **XN**,
because the Xbox build has no networking at all today: `tools/xbox-cc.sh:290`
compiles the engine with `-DXASH_NO_NETWORK=1`, which `net_ws_private.h:17`
resolves to `platform/stub/net_stub.h`. Every multiplayer artboard on
the canvas rests on a stack that is stubbed out in the binary that boots
today - the design is a target for XM.3, not a description of what the
menu can be wired to now. X0's own analysis (lines 208-230) already
scoped the lwIP port layer as conditional, "only if the console build
ever wants real networking"; this design answers that conditional yes,
so XN below turns it into a real phase with its own exit.

**Four corrections to the new notes, from reading the files they name**
(the same pass that caught `sv_aim`/`GameOptions.cpp` in the first
design): (a) there is no `menus/LanGame.cpp` - the LAN browser is
`menus/ServerBrowser.cpp`, the same class serving both screens through
`m_bLanOnly` (`:384`, `:940-943`) and registered twice (`menu_langame`,
`menu_internetgames`, `:457-458`); it already issues `localservers` and
`internetservers <filter>` (`:943`, `:982`), so System Link and Internet
Games are a reskin over existing plumbing, not new protocol code. (b) The
master list is not a new `masters.vdf`: `engine/common/masterlist.c` is
already a multi-master implementation (`NET_AddMaster` `:486`,
`NET_AddMasterStatic` `:527`, the `addmaster`/`addmasterstatic` commands)
and it already reads its list from **`xashcomm.lst`** (`:648`) - so the
Master Servers screen extends that file and inherits the engine's own
dedupe and per-master accounting, rather than inventing a format beside
it. It does NOT follow that the screen is a menu-only change, which this
note first claimed and Codex review corrected: that format has no
disabled state, the shipped default master is added unconditionally
before the file is even read (`:768-776`), and the runtime commands are
add and clear-all only - so enable, remove, test and Restore Defaults
are engine surface XN has to add first (XN step 4, XM.3 item 4). (c) The engine already HAS an on-screen keyboard -
`engine/client/input/in_osk.c`, 4 lines x 13 keys, left trigger
backspace, right trigger space, behind `osk_enable` (default `"0"`,
`:20`), already exposed by `Gamepad.cpp:86` - and it is a different
keyboard from the canvas's: 10 wide, SHIFT/SPACE/BACKSPACE/ACCEPT, an
A/X/Y/START/B legend, and a caller-supplied title, initial value and
character set. Which one ships is a real decision XM.3 has to make and
record, not a detail (below). (d) **Split Screen has no engine behind it
at all**: grepping the whole engine for split-screen, a second local
client or a second viewport finds nothing - GoldSrc and Xash are
single-local-client engines, and a second client is not a menu change
but an engine one (a second `cl` state, a second view, input from a
second port, and a listen server hosting both). The canvas's own note
says to grey the row out until it is measured; this plan goes further
and keeps it out of XM.3's exit entirely (see XN's risks). What it keeps is
the WON menu's identity - the left-aligned column of orange labels with a
console-font description beside each one (the exact text #78/#79 made
legible), the black ground, the version string bottom-right, selection
as the brighter-orange WON highlight rather than a box - and what it adds
is the console layer: a bottom legend of Xbox button glyphs with the
verbs each screen answers to (A Select / B Back everywhere; X and Y only
where a screen uses them; the D-pad glyph where a row adjusts a value),
pad-only navigation, and every screen inside the title-safe area (labels
start 72 px in and 48 px down at 1x, the legend sits at y=438, all within
`ui-architecture.md` §7's 90%). Three decisions the earlier draft left
open, made by the design: (a) **sub-screen headings are text** - an
uppercase orange heading over a gradient rule replaces every `head_*.bmp`
banner, so on the shipping screen graph the per-screen banner residency
that item 0 step 5's ceiling deferral is about does not exist at all; the
bitmap art left is item 0's own (background, title, button strip, per
profile) plus the Duke diagram and the save previews. (b) **No Quit,
Previews, Multiplayer or Console on Main** - a console title has no
desktop to return to and no server list yet. (c) **Every row is drawn as
text, and the WON button strip does not draw on Xbox at all** - reversing
this plan's own earlier call (that row labels come from the strip where
the strip has them, `PC_*` ids through `PicButton.cpp:259`, text only
where the 1998 strip had no such word). The design asked for this and the
plan had not noticed: the canvas's NewGame artboard names `PC_EASY` /
`PC_MEDIUM` / `PC_DIFFICULT` / `PC_CANCEL` and then says **"text-button
rendering"** - the design naming the exact picture ids and choosing text
over them. That note is the evidence, not the fact that an HTML mockup
draws text, which proves nothing by itself. The reason to reverse is not
fidelity but reach: **the strip's labels are English baked into a bitmap,
so no menu built on them can ever be translated**, and the canvas's own
typefaces (Barlow Semi Condensed for UI text, IBM Plex Mono for the help
lines) stop being stand-ins for the strip and become a real font that
ships. Item 13 owns the change and every consequence: the strip stops
being resident at all (`Btns.cpp:42` already refuses to load it when
`renderPicbuttonText` is set), a memory *win* against item 0's 3 MiB
ceiling rather than a cost - though a glyph atlas arrives in its place
and has to be measured the same way - and the `PC_*` picture-id question
retires entirely, an argument that only ever existed while a strip drew
(divergence #67 spent three review rounds on which id the Game row should
borrow). Nothing new in
input either: `Utils.h:190-222` already answers A as Enter, B as Escape
and X as Backspace for every menu - the legend shows what the pad
already does. Two corrections to the canvas's own notes, found by
reading the files they name: `sv_aim` and `crosshair` live in
`AdvancedControls.cpp:86-91`, not `GameOptions.cpp`, and its "Autosave"
toggle has no setting behind it (item 8).

1. **Main** (`Main.cpp`): Resume Game (in-game only), New Game, Training
   Room (`PC_HAZARD_COURSE`, `:203` - kept, it is the WON menu), Load
   Game, Save Game (in-game only), Options, each with its description.
   Hidden under `XASH_XBOX`: Console (`:179`), Disconnect (`:193`),
   Multiplayer (`:209`), Custom Game (`:221`), Previews (`:226`), Quit
   (`:231`). **The expanded design puts a Multiplayer row back on Main**
   ("System Link, split screen, player profile"), between Save Game and
   Options, and hides only Console, Disconnect, Custom Game, Previews and
   Quit - but it also says that row is visible "only when the NIC has a
   link", and there is no link-state query in the engine to ask (nor a
   network stack under it, see the design note above). So the row lands
   in **XM.3**, with XN's link check behind it; XM.1 keeps Multiplayer
   hidden, which is what shipped. Load and Save go straight to `UI_LoadGame_Menu` /
   `UI_SaveGame_Menu` (`LoadGame.cpp:497-502`), retiring `SaveLoad.cpp`'s
   three-button hub (`:60-62`) and splitting `Main.cpp:378-384`'s single
   `saveRestore` button into the two rows. Backdrop per item 0's two
   profiles: on HL25 content the BackgroundLayout tiles (solid black,
   measured) under `CMenuAnimatedBanner` exactly as the canvas animates
   it (`AnimatedBanner.cpp`: `logo.tga`, the two blips, the four
   `logo_big_blurred_%i` layers drifting 3.7x as far, `:195`); on
   pre-HL25 content the WON `splash.bmp` under the same rows and no title
   (step 5: it degrades clean). Version string as today, `xbox-i386` in
   place of `win32-i386` - **not done in this row**: the string comes
   from the engine's own build-platform code, not `mainui`, and this
   row's scope stayed inside `Main.cpp`; a small follow-up.

   **DONE, 2026-09-17.** Implemented and verified live on both discs.
   **A real infrastructure bug found first, blocking**: every `#if
   XASH_XBOX` written into this file compiled clean but took the WRONG
   branch - the flattened Xbox layout never appeared, the retail
   Multiplayer/Previews/Quit column stayed, on a build that otherwise
   linked and booted fine. Root cause, found by preprocessing the file
   directly rather than guessing: `tools/xbox-mainui-sweep.sh`'s include
   path (`-I3rdparty/mainui/sdk_includes/...`) never reaches
   `3rdparty/library_suffix/include/build.h`, the engine header
   `patches/library_suffix-xbox-subplatform.patch` teaches to `#define
   XASH_XBOX 1` under `defined _WIN32 && defined NXDK` - `mainui` builds
   against its OWN, entirely separate `sdk_includes/public/build.h`
   copy of the same platform-detection matrix, which had no Xbox
   sub-platform branch at all, so the macro was silently undefined
   (0 in `#if`) for every file in this fork's own menu module. This is
   exactly why the earlier `ui_xbox_menu_art`/`uiStatic.lowmemory` gates
   in `BackgroundBitmap.cpp`/`Btns.cpp` were built as RUNTIME checks
   instead of `#if XASH_XBOX` - a choice made correctly, for a reason
   this row is the first to write down. **Fixed**: added the identical
   `#if defined NXDK` branch to `sdk_includes/public/build.h`'s own
   `_WIN32` case, same condition, same reasoning, as the engine's patch.
   Confirmed by preprocessing `Main.cpp` directly (`tools/xbox-cxx.sh
   ... -E`) before touching the link at all - the Xbox-only code
   survived - and checked the fix does not silently change behaviour
   elsewhere: `Main.cpp` was, before this row, the ONLY file in the
   whole `mainui` tree that referenced `XASH_XBOX` (grepped), so nothing
   else could have been depending on it staying undefined. **A second,
   real bug found live, not by review**: Save Game's row is hidden
   outside a game, but its position in the stacking loop reserved a
   vertical slot regardless of visibility - Load Game sat one row too
   high, leaving a visible gap above Configuration on the actual boot
   screenshot. Fixed to skip the slot entirely when hidden, the same
   way `resumeGame`/`hazardCourse` already do; re-verified live, rows
   pack tight. **Verified live on both profiles**: classic WON disc
   shows New Game / Hazard Course / Load Game / Configuration, tight,
   no gap, Multiplayer/Previews/Quit gone; HL25 disc (text-button path,
   no strip at all) shows New Game / Training Room / Load Game /
   Options with the same set removed - confirming the flattening
   applies identically whether the profile draws bitmap buttons or
   text. The in-game state (Resume Game and Save Game appearing,
   Load Game's slot shifting up to make room) is reasoned through
   from the code and not live-tested - reaching it needs a controller
   in a running map, the same limitation this item's other rows already
   name. **Codex review found a real bug the screenshots couldn't
   catch**: `AddItem()`'s registration order IS keyboard/pad focus
   order (`ItemsHolder.cpp`), independent of on-screen position -
   `loadGame` was registered before `configuration` unconditionally,
   but the retail (non-Xbox) layout still draws `configuration` ABOVE
   `loadGame`, so pressing Down at Configuration on the PC build would
   have jumped backward on screen to Load Game, then forward past it to
   Multiplayer - exactly the kind of thing that never shows up in a
   screenshot. Fixed by making the registration order `#if XASH_XBOX`
   too, matching each platform's own visual stacking; `saveGame` stays
   registered (permanently invisible) on non-Xbox either way, since an
   invisible item is skipped by navigation regardless of its slot.
   Re-verified live: no visual change on the Xbox layout, exactly as
   expected for a registration-order-only fix. **Codex review round 2**
   caught something visible in every single screenshot taken this
   session, unnoticed until it named it: the two small white squares in
   the top-right corner of every WON menu boot screenshot are the
   retail desktop `minimizeBtn`/`quitButton` icons - `QMF_MOUSEONLY`
   only means "not reachable by keyboard/pad focus", not "invisible";
   they are ordinary visible-by-default items, drawn every frame
   regardless of input method. A console has no window to minimize or
   close, so their setup, `AddItem()` registration and positioning all
   moved under `#if !XASH_XBOX` - not just left unfocusable in the
   corner. Re-verified live: the corner is empty on the boot screenshot,
   no other change.
2. **New Game** (`NewGame.cpp`): unchanged - Easy / Medium / Difficult /
   Cancel with descriptions, under a text heading.
3. **Load Game / Save Game** (`LoadGame.cpp`, one screen in its two modes
   as today): Load (or Save) / Delete / Done on the left with "Saved games
   are kept on the Xbox hard disk"; on the right the existing `CMenuTable
   savesList` (`:164`) as Saved game / Chapter / Date, `CMenuSavePreview`
   (`:68`, `save/<name>.bmp`) with the name, chapter and difficulty line
   beside it; Delete through the existing `CMenuYesNoMessageBox` ("Delete
   this saved game?", `:362`), answered A/B; legend A Load (Save) /
   X Delete / B Back. `head_load`/`head_save` (`:29-30`) become the
   heading.
4. **Options** (`Configuration.cpp`): Controller / Audio / Display / Game /
   Done with descriptions. Controls → Controller (`:58`), Video → Display
   (`:62`), Game is a new row; Touch (`:64`), Joystick (`:66`) and Update
   (`:68`) hidden.

   **DONE in part, 2026-09-17 - only the dependency-free half.** Renaming
   Controls → Controller and Video → Display, and hiding Joystick,
   before items 5 and 7 exist would label a button with a screen that
   is not there, or remove the only way to reach gamepad-axis settings
   with nothing built to replace it - a real regression, not a
   simplification with a due date. So this pass did only what has no
   such dependency: hid Touch (`#if !XASH_XBOX` around `:64` - a
   touchscreen settings screen on hardware with no touchscreen, zero
   functional loss) and Update (same guard around `:68` - needs
   `UI_OpenUpdatePage`'s internet access, which XN has not built), and
   added the new Game row, pointing at `UI_GameOptions_Menu`
   (originally `PC_ADV_OPT`, reused from how `PlayerSetup.cpp` reaches
   the same menu on PC - **corrected after merge, three times**: counted
   from `PC_NEW_GAME = 0`, its real ordinal is 60, inside the WON strip's
   populated 0-61 range, not past it, so it drew that strip's actual
   "Adv. Options" bitmap instead of falling to text. Switching to
   `PC_ADV_OPT2` (ordinal 68, unused anywhere in this codebase) only
   moved the same bug to an id that is unpopulated in *today's* strip,
   not one that is guaranteed unpopulated - `Btns.cpp`'s `pic_count` is
   measured from the loaded bitmap's own height, not a fixed constant,
   so any future or modded strip reaching that far reopens it. Dropping
   `EDefaultBtns` entirely for a plain `CMenuPicButton` member fixed the
   picture but broke shutdown: the member never went into `m_apBtns`, so
   `~CMenuFramework()` dereferenced a null slot there destroying every
   other button the screen owns - a crash on `UI_Shutdown`, not something
   a boot-and-screenshot check could ever catch. The actual fix
   heap-allocates the button and stores it in `m_apBtns` with
   `m_iBtnsNum` incremented in the same statement, exactly matching
   `AddButton()`'s own ownership contract - only the `SetPicture()` call
   is left out).

   **DONE, 2026-09-20 - the dependency was cleared.** Once items 5 and 7
   both actually landed, the "Video" row renamed to "Display" (the same
   rule item 5's own "Controller" rename already applied), and the
   standalone "Controls" button hid entirely rather than renaming to
   "Controller" as this bullet's own original text expected - the actual
   implementation took a different path than planned here: item 5
   renamed the *Joystick* row to "Controller" instead, leaving `Controls.cpp`
   itself to become the Controller screen's own Customize sub-screen
   (divergence #85), reachable only through it. Keeping the old
   "Controls" button pointing directly at that now-repurposed screen
   would have been a second, stale-labelled door into it, not a
   simplification - hidden instead, matching how this fork already
   retires a screen's old direct entry point once a new one absorbs its
   role (`Video.cpp`'s hub, `AdvancedControls.cpp`). Full account in
   `divergences.md` #87.
5. **Controller** (replaces `Gamepad.cpp` in place; `Controls.cpp` becomes
   its Customize sub-screen) - three screens. **Scheme**: Standard /
   Southpaw / Legacy / Custom as one horizontal row with a description
   line under it, the scheme's mapping list (Left trigger, Left stick,
   Stick click, D-pad, Back) beside the Duke diagram with the scheme's
   labels drawn by code, and Settings… / Customize… / Done. A scheme is a
   complete controller *profile*, not a bind table: the `bind` pairs over
   `in_keys.c:107-129`'s pad names through `KEY_SetBinding` PLUS
   `joy_axis_binding` (`in_joy.c:60`; that cvar, not any bind, is what
   makes Southpaw Southpaw), applied through the existing cvar and bind
   systems, never a second store. Standard's table is the canvas's
   Customize table: Fire = right trigger, Secondary fire = left trigger,
   Jump = A, Crouch = left stick click, Use = B, Reload = X, Next weapon =
   Y, Previous weapon = White, Flashlight = Black, Walk = right stick
   click, Weapon slots = D-pad, Quick save = Back, Pause menu = Start -
   deliberately NOT `in_keys.c`'s stock defaults, which put `+attack` on
   `R1_BUTTON` (a shoulder the Duke does not have) and `+attack2` on the
   trigger. **Start is `cancelselect` in every scheme and is not
   rebindable** (Codex review of this plan, round 1): it is the stock
   binding (`in_keys.c:115`), the HL client's `cancelselect` closes an
   open weapon menu or else issues `escape` (`deps/hlsdk/cl_dll/
   ammo.cpp:286`, `:762`), and in a map every pad press runs its binding
   and nothing else (`in_keys.c:862-865`) - so it is the ONLY pad route
   from gameplay back to Main, and the canvas's "Pause menu = Start"
   means exactly that route, never the `pause` command (which keeps no
   pad button: the menu is the pause). Quick save on Back replaces
   Back's stock `pause` (`:113`). Legacy = those stock defaults verbatim, so a player who
   already has them keeps them by name; Southpaw = Standard with the
   sticks swapped in `joy_axis_binding`; Custom = whatever Customize left,
   selected automatically once a rebind departs from every preset. Before
   the table is typed in: read which `K_*` names nxdk's SDL2 joystick
   backend actually emits for the Duke's triggers, White and Black off
   what divergence #53's seeding (PR #65) already writes. The seeding note
   stands verbatim: a `config.cfg` saved before gamepad binds existed
   opens with `unbindall` and carries no gamepad lines at all, so the
   migration has to run before a player ever finds this screen - the
   screen is where a player changes what was seeded, and whatever it
   writes is what the seeding respects next boot; test both paths (an old
   keyboard-only config, seeded once then customised, and a fresh one).
   **Settings**: Look sensitivity 1-10 (one slider driving `joy_pitch` and
   `joy_yaw` together, `in_joy.c:48-49`), Invert look (`joy_pitch`'s
   sign), Vibration on/off (`vibration_enable`, `cl_mobile.c:24`, moved
   here from `Audio.cpp:72`). Dropped from the earlier draft: per-axis
   deadzone rows (`joy_*_deadzone` keep their defaults, `in_joy.c:56-59`)
   and rumble duration (`vibration_length` stays hidden at 1.0); the
   honesty note stands - there is no strength setting to expose,
   amplitude is randomised in `Platform_Vibrate` when the caller passes
   none (`joy_sdl2.c:328-355`), and a real one is engine work with its
   own ledger row, XM.2 at the earliest. **Customize**: `Controls.cpp`
   rewritten in place - `CMenuKeysModel`/`CMenuKbActListModel` (`:36-39`)
   filtered to pad keys, the existing grab dialog ("Press a key or
   button", `:239`), A reassign / X clear / Y defaults / B back;
   `AdvancedControls.cpp`'s mouse rows hidden. Start's row is shown
   (Pause menu) but locked - not reassignable, not clearable, for the
   reason above. **Y is NOT `ResetKeysList`** (Codex review of this plan,
   round 2): that function runs `unbindall` (`:124`) and then binds only
   what `gfx/shell/kb_def.lst` lists (`:112-149`) - a keyboard-only file
   with no gamepad entries (divergence #53) - and the one-shot seeder
   `Key_SeedGamepadDefaults` (`in_keys.c:589-606`) fills EMPTY binds
   only while `joy_binds_seeded` is still 0, so a pad wiped after
   seeding stays wiped across reboots. Y instead applies the selected
   scheme's complete profile (Standard when the scheme is Custom): an
   Xbox-only routine that issues every pad `bind` of the scheme's
   table, sets `joy_axis_binding`, and re-pins Start - never `unbindall`,
   never an empty binding left behind for the seeder to ignore. X clear
   does leave that action empty, by the player's choice; Y is the way
   back. The
   diagram: one 32-bit TGA under `gfx/shell/`, no captions baked in, with
   labels as text at fixed anchors, so schemes swap strings, not art, and
   the image is language-neutral. **The art exists as of 2026-09-18 and
   is not this bullet's to design any more - item 0 step 7 owns it.**
   That step supersedes this one's "512x256, the canvas's inline Duke
   drawing is the reference for the pose": a real master render was
   supplied, `assets/gfx/shell/duke.tga` is baked from it at 280x208
   opaque (228 KiB on disc, 151 KiB resident - the size chosen by
   comparing three bakes at 1:1, where 200 loses the face-button letters),
   and `tools/xbox-menu-art.py` regenerates and verifies it. What is left
   for this item is only the wiring: staging it on the disc, loading it,
   and placing the label anchors around it.

   **Implementation, sequenced into five branches (2026-09-19), all five
   built.** `3rdparty/mainui/ControllerSchemes.h`/`.cpp` (mainui
   submodule, `xm1/controller-schemes`) is the shared model - the four
   scheme tables, detection, and application; full account, and a real
   Codex-found engine bug it fixed along the way (switching
   `joy_axis_binding` could leave a stale cached axis value driving
   movement/look forever), in `divergences.md` #82. The Duke diagram
   (`assets/gfx/shell/duke.tga`) is staged onto every Xbox disc build
   (`tools/xbox-xiso.sh`, `divergences.md` #83). The Settings sub-screen
   (`ControllerSettings.cpp`, `xm1/controller-settings`) - look
   sensitivity, invert look, vibration, the on-screen-keyboard toggle
   carried over from the old Gamepad screen - and the Customize
   sub-screen (`Controls.cpp` rewritten in place, `xm1/controller-
   customize`) - a filtered action list, a locked Start row, A/X/Y/B
   reassign/clear/reset/back - are both built (`divergences.md` #84,
   #85). **Reordered from this note's own original 3-4-5 listing**: the
   Controller screen's own Settings…/Customize… buttons need those two
   sub-screens to already exist to compile against, so this session
   built Settings and Customize before the Controller screen that wires
   buttons to both - a sequencing change only, not a design one.
   Finally, `Gamepad.cpp` itself is rewritten into the real Controller
   screen - the Scheme switch, description, Duke diagram with its
   eight-item live mapping list, and the two sub-screen buttons, all
   tied together (`divergences.md` #86). Submodule PRs for all five
   merged; main-repo pointer-bump PRs for the first four merged, the
   fifth open as of this note. **Live pad verification was attempted,
   not just deferred, and remains genuinely incomplete** (`divergences.md`
   #86's own detailed account) - a real disc built and booted clean, but
   synthetic pad input never reached the guest across four independent
   troubleshooting attempts, a local xemu/ViGEmBus tooling problem this
   session could not resolve, unrelated to this branch's own code
   (already reviewed and fixed across four rounds). Carried forward as a
   tracked gap for whoever next has a working pad-input setup to close,
   for #84/#85 too, since this screen is the first real on-screen path to
   either.
6. **Audio** (`Audio.cpp`): Sound / Music / HEV suit volume (`volume` /
   `MP3Volume` / `suitvolume`, `:64-66`) as D-pad-adjusted rows; Speaker
   output a read-only row from the Dashboard. Hidden: `room_off` /
   `dsp_coeff_table` (`:69-70` - the earlier draft's "keep DSP" is
   reversed: DSP has no player-facing meaning on a fixed console),
   `snd_mute_losefocus` (`:71`), and both vibration rows (`:67`, `:72`),
   which move to Controller.

   **DONE in part, 2026-09-17.** The three sliders and the hides, exactly
   as above, under `#if XASH_XBOX`/`#else` with the stock screen kept
   verbatim. Vibration hidden now (not deferred like Options' Joystick
   row) is the plan's own explicit call, not a new judgment - a real but
   narrow gap next to Joystick's, since `vibration_enable` still
   defaults on (`cl_mobile.c`) and just cannot be turned off until
   Controller ships, where losing gamepad-axis configuration entirely
   would have been worse. **The read-only Speaker output row is NOT
   built this pass**: nxdk exposes it only as a raw EEPROM value
   (`ExQueryNonVolatileSetting( XC_AUDIO, ... )`, `xboxkrnl.h:4229`),
   with no documented bit layout anywhere in this vendored copy or its
   samples (checked directly, not assumed) - unlike Display's video
   output row, whose fields (`XVideoGetEncoderSettings`,
   `VIDEO_WIDESCREEN`/`VIDEO_LETTERBOX`) are named and clear
   (`video.h:41-42`, confirmed for divergence #64's own PR round 2).
   Decoding `XC_AUDIO` without that documentation would be exactly the
   "never guess a constant" this project's own rules forbid - a real
   follow-up once the bit layout is found or reverse-engineered on
   hardware, not attempted here. Verified: classic disc links clean (0
   duplicate, 0 undefined) and boots to an unchanged Main, screenshotted;
   `menu_audio`'s static pointer reads as a real non-null heap address
   post-boot. NOT verified, same limit as items 4/8: `_Init()` only runs
   from `Show()`, so the Options → Audio navigation and on-screen
   rendering need a pad live. This screen's own `AddItem` order was
   written already matching visual order (heading, three sliders, Done
   last) from the two lessons items 4/8 already paid for - not
   re-discovered here, applied. **Codex review round 1 found a different
   overlap than item 8's**: `CMenuSlider` draws its own label above its
   `SetCoord` y (`Slider.cpp:205-206`'s `textHeight = y - charHeight*1.5`,
   ~39px for this medium font), so the first slider at y=260 put its
   label at ~221, inside the heading's own 200-232 span - a layout defect
   `AddButton`'s formula (item 8's bug) had nothing to do with. Fixed by
   reusing stock `Audio.cpp`'s own first-slider y=280 (already proven
   safe there, nothing sits above it) instead of inventing a new offset;
   the remaining sliders and Done shift down the same 20px. Re-verified:
   clean link, no regression on the boot screenshot, `menu_audio` still
   constructs non-null. **Codex review round 2 asked for the pad-live
   check itself, not just static/boot verification, before this ships**
   - correct as a gap, but not a new one: it is the exact same "needs a
   pad live" limit already named above and already accepted, at the
   same depth, for items 4 and 8's merged PR (#85) - this project's own
   standing workflow (`AGENTS.md`) is explicit that "xemu is not
   validation, hardware closes phases", so no amount of emulator
   scripting in this session substitutes for it, and blocking every
   Xbox screen on hardware access this session does not have would stop
   XM.1 entirely rather than move it forward one screen at a time. Not
   fixed here, and not silently accepted either: named as the same
   already-tracked hardware-verification debt every screen in this item
   carries, closed together once a controller reaches this build.
7. **Display** (`VideoOptions.cpp` in place; `Video.cpp` and
   `VideoModes.cpp` hidden entirely - the earlier draft's "VideoModes.cpp
   in place" is superseded): Brightness and Gamma (`:292-293`), Screen
   size (`viewsize`, `:289`), HUD size (`hud_scale`, `:300`, a spin the
   canvas defaults to Large, "for a TV at couch distance"), Texture
   filtering (`sw_texfilt` under `ref_soft`, `:354`; `gl_texture_nearest`
   after X9, `:374`); Video output a read-only row, derived live and
   never a literal (Codex review of this plan, round 2 - the canvas's
   "640x480 · 4:3 · 60 Hz, set in Dashboard" is the NTSC 4:3 case, not
   the string): resolution and refresh from `XVideoGetMode()`
   (`deps/nxdk/lib/hal/video.c:315` - PAL consoles run 640x480 at 50 Hz,
   `REFRESH_50HZ`/`REFRESH_60HZ`, `video.h:48-49`), aspect from
   `XVideoGetEncoderSettings()` (`video.c:208`) and its
   `VIDEO_WIDESCREEN`/`VIDEO_LETTERBOX` bits (`video.h:41-42`); §8's
   standing policy that aspect comes from the Dashboard, not a player
   setting, unchanged. Hidden: `r_detailtextures`, `r_ripple`, `gl_vbo`,
   `gl_overbright` (`:295-298`); no resolution table, no windowed/
   borderless, no `r_refdll`, no 10-second test mode. **XM.2, after X9**,
   exactly as before: any mode choice the driver actually honours (today
   the Xbox backend hardcodes `XVideoSetMode( 640, 480 )` and clamps every
   later request back to it - divergence #50 - and `ref_soft` refuses any
   display-transform scale, `r_glblit.c`), a widescreen-presentation
   choice only once X9's separate-axis `R_SetDisplayTransform` work lands,
   and X5's `vid_scale` lever (2.0 → 320x240 render, presented 2x2) as a
   candidate "Performance mode" toggle only if X9's hardware numbers make
   it worth a player's choice.

   **DONE in part, 2026-09-17 - two of the five original rows dropped on
   correction, not on trim.** Brightness and Gamma keep their stock
   sliders and preview image entirely unchanged - both already work
   identically on Xbox, and `gfx/shell/gamma.bmp` degrades clean on HL25
   content the same way every other WON-only bitmap in this fork already
   does (checked directly: present in the classic `pak0.pak`, absent
   from the built HL25 stage, no error either way). Texture filtering
   needed no Xbox-specific code at all: `ref_gl` is never compiled or
   linked for Xbox (`tools/xbox-link-check.sh` builds `ref/soft`
   exclusively), so `r_refdll_loaded` is always `"soft"` at runtime, and
   stock `VideoOptions.cpp::Reload()`'s own existing `soft_active` branch
   already links the row to `sw_texfilt` - the identical runtime
   dispatch PC uses, unmodified. The three `gl_*`/`r_*` checkboxes this
   item planned to hide (`r_detailtextures`, `r_ripple`, `gl_vbo`,
   `gl_overbright`) are removed rather than merely grayed, for the same
   reason: stock's own `Reload()` already grays every one of them
   permanently through its existing `gl_active` check, since `gl_active`
   can never be true on Xbox - a row that can never be un-grayed is
   clutter, not a setting, so it is gone rather than kept as a dead
   placeholder.

   **"Screen size" and "HUD size" are dropped, and researching them
   found the plan's own framing of both was wrong** - checked against
   their real client-side consumers rather than trusted. `viewsize` does
   not control inventory or status-bar visibility in the real client at
   all: its only real effect (`SCR_TileClear`, `cl_scrn.c`) is a
   Quake-era letterboxed-viewport shrink, a legacy aesthetic with no
   couch-TV benefit on an already-640x480 screen. The "no inventory"/
   "no status bar" bands this item's own citation leaned on
   (`VideoOptions.cpp`'s own disabled `CMenuVidPreview::Draw`) were never
   real client behaviour - only a hand-drawn approximation inside that
   one preview thumbnail, dead code since `LEGACY_VIEWSIZE` is compiled
   to 0. `hud_scale` fares worse, and is a real, load-bearing finding:
   `CL_GetScreenInfo` (`cl_dll/dll_int/cl_game.c`) can only enlarge the
   HUD when the reference width it is compared against exceeds the real
   screen width, but Xbox's screen is a fixed 640
   (`XVideoSetMode(640,480,...)`, divergence #50) and
   `hud_scale_minimal_width` defaults to exactly 640 too - so **under
   that default, no value of `hud_scale` can ever enlarge anything on
   this console**. The canvas's "Large, for a TV" cannot be built by
   wiring a UI control to this cvar alone; making it actually work needs
   `hud_scale_minimal_width` lowered as its own, separately-verified
   engine change (a real divergence, not a UI hookup) - a tracked
   follow-up, not guessed into this row. Neither row is a design loss so
   much as a design intent this fork cannot honestly deliver with the
   cvars named for it.

   **Added instead: a read-only "Video output" row - real hardware
   state, never a literal**, exactly as this item's own note already
   demanded. **The first way of wiring it in was wrong, and Codex review
   round 1 is why**: a new `pfnGetVideoInfo` field appended to
   `ui_enginefuncs_t` extends a struct explicitly commented "menu
   interface is freezed at version 0.75" - appending past that point is
   the established way to grow it (`pfnRandomFloat`/`pfnGetModeString`/
   `COM_SaveFile` were all added the same way), but `udll_int.cpp`'s
   `GetMenuAPI` copies `sizeof(ui_enginefuncs_t)` bytes sized from
   `mainui`'s OWN compiled struct, not the engine's - on the PC dynamic-
   module boundary this header is shared with (every platform, not only
   Xbox's single static link), a newer `mainui` loaded by an older engine
   would read past the end of the engine's shorter table and later call
   whatever garbage landed in the new slot as a function pointer. Xbox's
   build could never hit that path, but the header itself is not
   Xbox-only, so the fix is the primitive this codebase already trusts
   for exactly this class of value: `vid_width`/`vid_height` are already
   read-only cvars, so `vid_refresh`/`vid_aspect` are now two more,
   populated by `VID_SetXboxVideoCvars()` (`vid_common.c`) right after
   X5's own `XVideoSetMode` call - the same point `.refresh` first
   becomes the real negotiated 50 or 60, not the literal `0` that call
   passed in - from `XVideoGetMode()`, and `XVideoGetEncoderSettings()`'s
   real `VIDEO_WIDESCREEN`/`VIDEO_LETTERBOX` bits for aspect, a cached
   hardware query, not a stub. Zero new ABI surface: every other
   platform's build simply never calls the function that sets them, and
   `VideoOptions.cpp` reads all four through the `EngFuncs` cvar wrappers
   it already had (`GetCvarFloat`/`GetCvarString`), no new wrapper needed
   either. **Verified live, not just linked**: booted the classic disc
   and read `vid_refresh`/`vid_aspect` directly out of guest memory
   rather than trusting compilation - `vid_refresh.value = 60.0`,
   `vid_aspect.string = "4:3"`, both real negotiated hardware state, not
   the cvars' own declared `"0"`/`""` defaults.

   `Configuration.cpp`'s "Video" button now points straight at
   `VideoOptions.cpp` instead of `Video.cpp`'s own two-choice hub ("Video
   options" / "Video modes") - the hub's other choice, `VideoModes.cpp`,
   is hidden entirely on Xbox, so the hub would offer exactly one working
   option. `Video.cpp`/`VideoModes.cpp` themselves are untouched, same
   "hidden, not deleted" convention item 9 already established: nothing
   on Xbox reaches them any more, confirmed by grep, not merely
   asserted.

   Verified: classic disc links clean (0 duplicate, 0 undefined) and
   boots to an unchanged Main, screenshotted; the full engine sweep
   matches its established 203 ok/29 failing baseline exactly, so the
   two new cvars introduced no regression anywhere else in the tree;
   `menu_video`'s and `menu_vidoptions`'s static pointers both read as
   real non-null heap addresses post-boot, confirming both objects
   construct without crashing. NOT verified: `_Init()`/`_VidInit()`
   themselves, same limit as every screen since item 4 - the actual
   Options → Display navigation, the live video-output string, and the
   visual layout need a pad live.
8. **Game** (new screen; `GameOptions.cpp` reshaped): Auto-aim (`sv_aim`)
   and Crosshair (`crosshair`) moved in from `AdvancedControls.cpp:86-91`;
   Fast weapon switch (`hud_fastswitch`, a client cvar no mainui screen
   links today - a new row). Hidden: `fps_max`, `cl_allowdownload`,
   `cl_cmdrate` / `cl_updaterate` / `rate` (`:113-140`) and the
   `developer`-gated rows (`:192-197`). One correction to the canvas: its
   "Autosave" toggle is dropped - GoldSrc autosaves where a map's
   `trigger_autosave` says, unconditionally, and no engine or client cvar
   turns that off, so the row would be wired to nothing.

   **DONE, 2026-09-17.** `GameOptions.cpp` reshaped exactly as above under
   `#if XASH_XBOX`/`#else`, the stock network/performance screen kept
   verbatim for every other platform. A text heading (`CMenuAction`,
   `QMF_INACTIVE|QMF_DROPSHADOW`, `QM_BIGFONT` - the same primitive
   `GameOptions.cpp` already used for its own stock "Select network
   mode:" label, no new class needed) in place of `ART_BANNER`, per the
   design's own decision that sub-screen headings are text on Xbox - the
   first screen actually built against that decision, so it is the
   reference for the ones after it. The three checkboxes follow
   `Audio.cpp`'s own already-stock idiom exactly (verified by reading
   it, not assumed): `LinkCvar` stages a value and reads it into the
   control immediately (`CMenuEditable::LinkCvar` calls `UpdateCvar(true)`
   on link, `Editable.cpp:30-36`), a single Done button overrides
   `SaveAndPopMenu()` to `WriteCvar()` each one then chain to the base,
   and B/Escape - inherited, no override needed - hides without writing
   (`CMenuBaseWindow::KeyDown`'s Escape branch calls `Hide()` directly,
   `BaseWindow.cpp:98-102`, never the save path), an implicit discard
   with no separate Cancel button, matching the design's single "Done"
   row. **Verified live only in part - the honest limit of what this
   session's tools can reach**: the classic and HL25 discs link clean (0
   duplicate, 0 undefined) and boot to an unchanged Main (screenshotted,
   no regression), and `menu_options`/`menu_gameoptions` (both static
   pointers, `ADD_MENU3`'s own `cmd = new type()` in `_Precache`,
   `BaseMenu.h:246-256`) read as real, non-null heap addresses after
   boot - both objects construct without crashing. What is NOT verified:
   `_Init()` itself, which is what actually calls `LinkCvar`/builds the
   screen's rows, only runs from `Show()`
   (`BaseWindow.cpp:31-34`), never from construction - so the Options →
   Game navigation, and everything the new screen actually draws, needs
   a pad reaching it live, the same limitation already named for every
   in-game and Controller-scheme state in this item. Considered and
   rejected as disproportionate for a three-checkbox screen: forcing
   `Show()` early by writing a fabricated return address and redirecting
   `EIP` to `UI_GameOptions_Menu` through the GDB stub - technically
   reachable with this session's own tools, but `tools/xemu-debug.py`
   has no register or memory WRITE support today (`read_regs`/`read_mem`
   only), and building that now, for this, risked corrupting a running
   guest to save one screenshot. **That gap is exactly why Codex review
   still found a real bug**: `AddButton()`'s own auto-position
   (`(72, 230 + 50 * n)`, `Framework.cpp:106-123`, `UI_BUTTONS_HEIGHT=42`)
   put the Done button at y=230-272, overlapping both the heading
   (200-232) and Auto-aim (260-292) - a live rendering and hit-target
   defect no compile or link check could catch. Fixed the same way
   `AdvancedControls.cpp`'s own Done button already avoids this: a
   manual `CMenuPicButton` member, explicitly positioned at `(72, 440)`,
   below all three checkboxes, not `AddButton`'s auto-placed one.
   Re-verified: no overlap in the corrected coordinates (checked by the
   same arithmetic the finding used), clean link, no regression on the
   boot screenshot, the screen's own menu object still constructs.
   **Codex review round 2 found the same class of bug divergence #66
   already found in `Main.cpp`, on a new screen**: `AddItem()`'s
   registration order is pad focus order (`ItemsHolder.cpp`'s cursor is
   a plain sequential index into `m_pItems[]`, `Up`/`Down`
   incrementing/decrementing it - read directly to confirm, no
   geometric awareness at all), independent of screen position - `done`
   was registered right after the heading while sitting visually last,
   so Down from the top would reach it before Auto-aim. **Worth writing
   down once, for every screen after this one**: `AdvancedControls.cpp`
   has this EXACT same mismatch in stock, unmodified upstream code (its
   own `done` registers second, positions last, `:200-201`) and it is
   not a bug there - PC navigation is mouse-first, and a click goes
   straight to screen position, bypassing the index entirely. Xbox has
   no mouse, so sequential Up/Down is the ONLY way to move focus, and
   registration order has to match visual order on every screen built
   for it - a correctness requirement this port adds that stock upstream
   never had to meet. Fixed: `done` now registers last. Re-verified:
   clean link, no regression on the boot screenshot, the screen's own
   menu object still constructs.
9. **Hidden under `#if XASH_XBOX`**, in two groups the expanded design
   now separates. **Permanently** (nothing later un-hides these):
   `Touch*`, `Gyro`, `InputDevices`, `AdvancedControls` (its two
   surviving rows moved to Game), `CustomGame`, `FileDialog`, `Zoo`,
   `Video.cpp` / `VideoModes.cpp`, `SaveLoad.cpp`'s hub. **Until XN
   lands**, then rebuilt by XM.3 rather than un-hidden as they stand:
   `ServerBrowser` (both registrations), `Multiplayer`, `PlayerSetup`,
   `CreateGame`. Hidden, not deleted, either way: the files stay stock
   for the merge diff.

   **DONE, 2026-09-18.** Every file this bullet names is wrapped
   `#if !XASH_XBOX ... #endif` right after its own `#include`s, the
   same minimal placement item 12's `Legend.{h,cpp}` established -
   whole-file rather than just the button or the `ADD_MENU` line,
   because several of these files call each other's show functions
   internally and a partial guard would leave those calls pointing at
   now-undefined symbols. `Gamepad.cpp`'s "Gyroscope" and `Controls.cpp`'s
   "Adv. Controls" buttons are guarded the same way; `ConnectionProgress.cpp`'s
   disconnect handler and `YesNoMessageBox.cpp`'s stray cvar-confirmation
   callback are fixed to match. A second Codex round then found the same
   button-only blind spot again, this time against every `ADD_MENU*`
   registration in the tree: `ServerInfo.cpp`, `PlayerIntroduceDialog.cpp`
   and `ConnectionWarning.cpp` (the last needing a fourth `udll_int.cpp`
   stub, `UI_ConnectionWarning_f`) were still their own live console
   commands with no button left. Two more registrations were checked and
   deliberately kept, not missed: `ConnectionProgress.cpp` (the generic
   connect/loading screen every singleplayer map start uses too, not
   PC-multiplayer-specific) and `ScriptMenu.cpp`'s `menu_serveroptions`/
   `menu_useroptions` (their own screens are unreachable like the rest,
   but `UI_LoadScriptConfig()` needs both objects to exist and runs
   unconditionally from `BaseMenu.cpp`'s own `UI_UpdateMenu`, so hiding
   their registration would null-crash Xbox's first menu frame). Full
   account, including the reachability gap the earlier button-only
   survey missed twice over (`ADD_MENU`'s own console command, confirmed
   live since this fork's dev console works) and a real bug Codex review
   caught in an earlier draft of this change (a misread `#if`/`#else`
   nesting briefly repointed the non-Xbox Save\\Load row instead of
   touching anything on Xbox): `divergences.md` #76.
10. **Single-player pause** (new screen, XM.1): the canvas's "Paused" -
   chapter and difficulty under the heading, the game scene dimmed
   behind it with the HUD still faintly visible, and Resume Game / Save
   Game / Load Game / Options / Quit to Main Menu ("Unsaved progress
   will be lost.", through the existing `CMenuYesNoMessageBox`). Legend
   A Select / B Resume / START Resume. **It supersedes Main's in-game
   variant**: START in a map opens this, not Main, and Main's own
   `connected`-gated Resume Game and Save Game rows (which shipped in
   XM.1's first slice, PR #83, and are what START opens until this lands)
   come back out when it does - the design keeps those rows drawn on the
   Main artboard as the in-game state it had before this screen existed,
   but it is one in-game menu, not two. It needs no socket, so it belongs
   here rather than XM.3 - but it does need two things this phase has
   not checked: that the engine really pauses (the note says `sv pause`;
   `pausable` is already forced to 1 for singleplayer in `Main.cpp:178`)
   and that the menu can draw over a dimmed live scene rather than the
   menu background, which is `CMenuBackgroundBitmap::DrawInGameBackground`'s
   job today (a flat black fill, `BackgroundBitmap.cpp:66-69`) and would
   become a dim instead. Measure both before writing the screen.

   **DONE, 2026-09-18 - and both prerequisites checked out the opposite
   of how this note framed them: real, complete machinery already there,
   not something needing new engine work.** `pause`/`SV_Pause_f`
   (`engine/server/sv_client.c:1435-1446,1630-1655`) toggles `sv.paused`
   and broadcasts `svc_setpause` to every client, the listen server's own
   local one included; `sv_pausable` already defaults to `"1"`
   (`sv_main.c:137`), so `Main.cpp:178`'s own set is belt-and-suspenders,
   not compensation for a 0 default. On the client, `cl.paused` freezes
   simulation through `CL_EmitEntities`'s own early-out
   (`cl_frame.c:1289`) while `V_RenderView` keeps drawing the scene every
   frame regardless of it - exactly the frozen-but-visible effect this
   screen needs, reachable with the same `EngFuncs::ClientCmd` mechanism
   `Audio.cpp`'s own `vibrate` already uses. **This note's own premise
   for the second prerequisite was simply wrong**: `DrawInGameBackground`
   already fills at `uiColorBlack` (`BaseMenu.cpp:75`), which is
   `0x80000000` - 50% alpha, not the opaque black this note assumed - so
   no code change was needed there at all. The real gate is
   `ui_renderworld` (default `"0"`, `cl_main.c:86`), the single flag
   `V_RenderView` checks (`cl_view.c:395`) to skip the 3D draw entirely
   while a menu is up - already flipped by `AnimatedBanner.cpp`/
   `MovieBanner.cpp` for their own reasons, not new engine territory.
   Both facts were checked by reading the real render/pause path
   directly, not by a boot test - a pause screen's correctness has no
   `memstats`-style number to measure, so this is one of the few things
   in this plan verified by source reading alone, end to end, and named
   as such rather than dressed up as something it isn't.

   A new file, `Pause.cpp`, not a reshape - there is no stock in-game
   pause menu in `mainui` to diverge from (GoldSrc opens `Main` itself
   in-game). Repoints the ONE place that decides what opens when the
   menu system activates with nothing already showing
   (`BaseMenu.cpp:641-647`) to open Pause instead of Main whenever
   `CL_IsActive()` is true - confirmed by grep to be the only call site
   that matters: the other three `UI_Main_Menu()` sites are connection-
   flow returns that cannot fire mid-game, or `UI_ShowMessageBox`'s rare
   system-error fallback, left alone rather than touched speculatively.
   Resume Game / Save Game / Load Game / Options / Quit to Main Menu
   (through the existing `CMenuYesNoMessageBox`, the same idiom
   `Main.cpp`'s own `DisconnectDialogCb` already uses) all present;
   `Hide()` is overridden as the single place every exit path goes
   through (Resume's own button, B/Escape's inherited default, and
   Quit's confirmed callback alike) so the pause toggle and
   `ui_renderworld` reset happen exactly once per close regardless of
   which path was taken, rather than duplicating that logic per button.

   **Chapter title is real, not dropped - corrected in Codex review
   (round 1)**: an earlier version of this note claimed classic
   Half-Life's SDK has no chapter-title concept at all, checked only
   against `deps/hlsdk/cl_dll/` (the client DLL, which never sees it).
   Two real mechanisms exist. Worldspawn's own `chaptertitle` key
   (`deps/hlsdk/dlls/world.cpp:663-674`) fires a one-time `env_message`
   0.3s after map start, not something to read back later - but
   `engine/server/sv_save.c`'s own `gTitleComments[]`, a complete
   mapname-prefix-to-title-token table for every retail HL/OpFor/BShift
   map already used to build save-file comments (and already resolved
   through `L()` by `LoadGame.cpp:286`), does not depend on a given map
   re-setting `chaptertitle` - most mid-chapter maps do not. Pulling a
   `SV_ResolveLevelTitle()` out of that file's own `SaveBuildComment`
   (a pure refactor, save comments unchanged) and populating a new
   Xbox-only read-only `sv_leveltitle` cvar from it once a map activates
   (`SV_UpdateLevelTitleCvar`, called from `SV_ActivateServer` in
   `sv_init.c`) closes the gap with the same zero-new-ABI-surface shape
   as the Display screen's own `vid_refresh`/`vid_aspect`. Difficulty IS
   shown, and is real: the same `skill` cvar `NewGame.cpp`'s own
   Easy/Medium/Hard rows already write, read fresh in `_VidInit()`
   (which reruns every visit, unlike `_Init()`'s one-time `AddItem`
   calls - confirmed directly from `WasInit()`'s own guard,
   `ItemsHolder.cpp:283-295`, not assumed from this screen's own
   behaviour looking right). One real string-lookup mistake caught
   before it shipped: the heading first tried `L( "StringsList_235" )`
   on the assumption that a Paused string sat near
   `HazardCourseDialogCb`'s own `StringsList_234` - checked against
   `MenuStrings.cpp`'s real compiled table and found empty
   (`EMPTY_STRINGS_5`, index 235 included). Fixed with item 13's own
   established idiom instead: `L( "Paused" )`, the literal itself as the
   key, translatable via `mainui_<lang>.txt`, English if nobody has.

   **Codex review (round 1) found two further real defects, both fixed
   before this PR opened.** First (high severity): the first version
   toggled `pause` unconditionally on every show and hide, but
   `SV_Pause_f` is a state inversion, not a settable flag, and Xbox's
   BACK button is independently bound to raw `"pause"`
   (`engine/client/input/in_keys.c`) outside this menu entirely - so a
   player already paused by BACK before opening this screen would be
   silently unpaused by it, and vice versa on close. Fixed with a new
   Xbox-only read-only `cl_ispaused` cvar mirroring `cl.paused`, synced
   once per frame in `Host_ClientFrame` right after `CL_ReadPackets`
   (`cl_main.c`) via `Cvar_FullSet` (`Cvar_SetValue` was tried first and
   silently no-ops on a read-only cvar - it routes through
   `Cvar_CanSet`, which refuses `FCVAR_READ_ONLY` outright). `_VidInit()`
   now only issues `pause` - and records that it did, in a new
   `m_bDidPause` member - when `cl_ispaused` reads 0; `Hide()` only
   reverses it when `m_bDidPause` is true. Second (medium severity):
   once a menu is active, raw input goes straight to the UI and never
   reaches gameplay bindings, and the inherited escape predicate
   (`UI::Key::IsEscape`, `Utils.h`) only recognizes Escape/B - Xbox's
   START is bound to `"cancelselect"` (`in_keys.c`), not pause, so it
   did nothing here. Fixed with a `KeyDown` override recognizing
   `K_START_BUTTON` and routing it through the same `Hide()` every
   other close path already uses.

   **Codex review (round 2) found two more real defects, both fixed
   before this PR opened.** First (high severity): the activation branch
   tested only `CL_IsActive()`, which says nothing about player count -
   pressing START in a multiplayer session would open this screen too,
   pause the whole match (`sv_pausable` defaults to `"1"`), and offer
   Save/Load that make no sense there. Fixed by gating with the same
   `host_serverstate`/`maxplayers == 1` test `Main.cpp`'s own
   `QuitDialogCb` already uses to mean "really singleplayer" - a
   multiplayer session now still opens Main on START, exactly as before
   this item existed. Second (high severity): `CMenuLoadGame`'s own
   `SaveGame()`/`LoadGame()` (`LoadGame.cpp`) both finish by calling
   `UI_CloseMenu()`, which clears the entire window stack without
   calling `Hide()` on anything still in it - harmless for every other
   caller, since no other screen's own `Hide()` does real work, but this
   screen's unpause and `ui_renderworld` reset live there, so saving or
   loading from Pause left the game paused with the world still hidden
   afterward. Rather than hiding Pause before opening Save/Load (which
   would have broken the normal cancel-and-return-to-Pause navigation,
   since Save/Load stay layered on top of Pause on the stack exactly
   like Options does), the cleanup moved into a public
   `ClosedExternally()` method polled once per frame from the same hook
   that opens this screen (`UI_Pause_CheckClosed`) for the specific
   visible-to-not-visible edge that means it left the stack some way
   other than its own `Hide()` - idempotent, so it changes nothing on
   every normal close path where `Hide()` already ran.

   Verified: `mainui` sweep clean (83/83, both rounds - the new file's
   own object growing from a false-start 544 bytes - `#if XASH_XBOX`
   checked before any `#include`, seeing the macro `build.h` only
   defines transitively through `Framework.h` as undefined, compiling
   the whole file to nothing and only failing at the link as an
   undefined `UI_Pause_Menu` symbol, not at compile time - to a real
   ~24 KB once the guard moved after the includes, matching every other
   file's own structure); the full engine sweep still 203/29, unchanged
   by the touched shared files (`cl_main.c`, `sv_init.c`, `sv_save.c`,
   `sv_main.c`, `server.h`); classic disc links clean (0 duplicate, 0
   undefined) and boots to an unchanged Main, screenshotted, both before
   and after the round 2 fixes. NOT verified: `_Init()`/`_VidInit()`
   themselves, and - more than any screen before it - the actual in-game
   behaviour this whole item is about: pressing START in a live
   singleplayer map but not a multiplayer one, the dim, the
   chapter/difficulty text, the pause round-trip on every exit path
   including the one Codex's own round-1 finding named (opening and
   closing this screen while already paused by BACK), and Save/Load's
   own cleanup firing correctly when reached from here. None of that is
   reachable without a pad in a running map, a stronger version of the
   same limit every screen since item 4 has carried, named here because
   this is the first item where the untested behaviour is the entire
   point of the work rather than a screen's cosmetic layout.
11. **On-screen keyboard**: **moves to XM.3** - the design gives it three
   callers (master address, player name, server name) and all three are
   multiplayer screens, so it earns its place exactly when they do, and
   not before. The decision it carries: the engine already has an OSK
   (`in_osk.c`, above), a different shape from the canvas's, driven by
   the engine's own text-field path rather than by a menu screen. Reusing
   it costs no new code and gets every engine text field at once, but
   ships a keyboard that does not match the design or the legend;
   building the canvas's costs a new `mainui` screen and leaves two
   keyboards in the tree. XM.3 picks one, in writing, before either is
   built - noting that E7's own design already called for "on-screen
   keyboard as a `mainui` window" (`epics.md`), which is the canvas's
   answer, not `in_osk.c`'s.
12. **The legend**: one shared control every screen draws from its own
   list of (glyph, verb) pairs - A/B/X/Y as filled discs in the Duke's
   colours, the D-pad as an outline, START as its own glyph on the pause
   screens - at the same y on every screen. It describes, it does not
   dispatch: dispatch is `Utils.h`'s existing mapping plus each control's
   own left/right handling on the rows the D-pad adjusts. Shared with
   XM.3, which adds no new glyph vocabulary - every multiplayer screen's
   legend is drawn from the same five.

   **DONE, 2026-09-18.** A new control, `CMenuLegend`
   (`3rdparty/mainui/controls/Legend.{h,cpp}`), wired into every screen
   XM.1 has shipped so far: Main, New Game, Load/Save Game, Options,
   Audio, Display, Game and Pause - the eight screens that existed
   before this item, none of which had carried a legend of their own
   until now. Per-screen lists follow the canvas's own explicit ones
   where it gives one (`LoadGame`: A Load (Save) / X Delete / B Back;
   `Pause`: A Select / B Resume / START Resume) and its generic baseline
   everywhere else (A Select / B Back, plus D-pad Adjust on Audio and
   Display, the two screens with D-pad-adjustable sliders - Game's three
   rows are checkboxes, toggled with A, so it gets no D-pad entry).

   Needs exactly one new asset: a plain white, alpha-edged circle
   (`assets/gfx/shell/legend_button.tga`, 32x32, generated rather than
   hand-drawn - a filled disc has no pose or perspective to get right,
   unlike item 5's own Duke diagram), tinted per button at draw time via
   `UI_DrawPic`'s existing colour parameter (already used for exactly
   this by every other tinted-picture draw in this codebase) with the
   canvas's own four values (A `#6dbe45`, B `#d9322d`, X `#2f79c8`, Y
   `#f0a91c`) and a white letter centred on top. The D-pad (an outlined
   diamond) and START (an outlined box containing the word itself, since
   it has no natural single-letter abbreviation) are both drawn from
   `UI_FillRect`/`UI_DrawRectangleExt` directly - no second asset. A new
   `LEGEND_COLOR` key joins `HEADING_COLOR` in `colors.lst`
   (`uiColorLegend`, `#cfc6b6`, the canvas's own exact value rather than
   a placeholder default like `HEADING_COLOR`'s own).

   **Real pixel coordinates, not this codebase's usual virtual space -
   found only by reading `Main.cpp`'s own Xbox layout closely, not
   assumed.** Every Xbox screen's `SetCoord`/`SetRect` call takes
   coordinates in a virtual 1024x768 space that `uiStatic.scaleX/Y`
   (`ScreenWidth/1024`, `ScreenHeight/768`) scales back down to the real
   screen at draw time - stock behaviour, unchanged by this fork. Seven
   of the eight screens above use plain literals in that space with no
   conversion (`Pause.cpp`'s own `SetCoord( 72, 510 )` for its Quit
   button, for instance). `Main.cpp`'s own Xbox `VidInit(bool)` is the
   one exception: it explicitly computes
   `configuration_voffset = ( 404 / 480.0 ) * 768.0` to anchor Options at
   a specific REAL pixel row, because a plain `404` there would not land
   at real y=404 once scaled back down - it would land higher up the
   screen, at `404 * (480/768) = 252.5`. The canvas's own legend position
   ("at 1x", `y=438`) is a real-pixel measurement in exactly this same
   sense, and the legend has to land there regardless of which
   convention happens to be true of whatever screen hosts it - so rather
   than asking every calling screen to remember `Main.cpp`'s own
   `(real/actual)*virtual` formula (or getting it wrong once and
   misplacing the row relative to everything else on screen), `SetRealCoord()`
   does that exact conversion once, inside `CMenuLegend::VidInit()`,
   recomputed fresh from stored real-target fields on every call rather
   than mutating `pos.x/y` in place - `VidInit()` can run again on a
   later `Show()` (`BaseWindow.cpp`'s own contract), and mutating
   destructively would drift the position further off target each time.
   An earlier version of this note assumed a real-pixel collision
   between the legend and Display's own "Video output" row (`y=450`) and
   Game's own Done button (`y=440`), and moved both - wrong, once the
   real/virtual distinction above is accounted for: those two elements
   use the SAME uncorrected literal-coordinate convention Pause's own
   button column does, so their true real position is far higher up the
   screen than a face-value reading suggests, nowhere near the legend's
   own real y=438. Both moves were reverted before this shipped, rather
   than leaving an unexplained, unnecessary diff on two previously
   verified screens.

   **Codex review (round 1) found two more real defects, both fixed
   before this PR opened.** First: LoadGame's own legend advertised "A
   Load (Save)" and "X Delete" unconditionally, but input goes to
   whichever control has focus first (`ItemsHolder.cpp`), and X-delete
   exists only inside `CMenuTable::KeyDown` (`Table.cpp`) - A could just
   as easily cancel or delete depending on focus, and X did nothing at
   all unless the saves table specifically was focused. Fixed by
   changing A's verb to "Select" (the same focus-agnostic verb every
   other screen's legend already uses) and adding a
   `CMenuLoadGame::Think()` override that toggles the X entry's own
   visibility against `ItemAtCursor() == &savesList` once per frame - a
   new `CMenuLegend::SetVisible()`, replacing the now-unused `SetVerb()`
   this fix made dead code. Second: the new file and its
   `uiColorLegend`/`LEGEND_COLOR` additions had no `#if XASH_XBOX` guard
   at all, so both build systems (which glob every `controls/*.cpp` file
   unconditionally) compiled the whole class on every platform,
   contradicting this note's own "Xbox only" framing. Fixed by wrapping
   `Legend.h`/`.cpp` the same way `Pause.cpp` wraps itself, and guarding
   `uiColorLegend` end to end - unlike `uiColorHeading`, which stays
   unconditional on its own established precedent, since nothing on
   Xbox reads `uiColorLegend` except this file.

   **Codex review (round 2) found two more real defects, both fixed
   before this PR opened.** First: the round-1 fix above still wasn't
   enough - `CMenuTable::KeyDown` buzzes instead of deleting on an empty
   saves list, and save mode's synthetic "new save" row (index 0) is
   deliberately not deletable either, so focus on the table alone still
   advertised Delete in cases where X does nothing. Fixed by also
   checking the Delete button's own grayed state in `Think()`
   (`!(remove.iFlags & QMF_GRAYED)`) - reusing `UpdateGame()`'s own
   already-computed verdict for exactly this condition rather than
   duplicating its predicate. Second: the new literal legend keys
   ("Select", "Back", "Delete", "Adjust", "Resume") had no entries in
   `assets/resource/mainui_german.txt`, the one shipped translation
   file - `L()` falls back to English on a miss, so every German screen
   would have shown an English legend, a real regression against item
   13's own already-proven localization. Fixed by adding all five keys
   with real German translations.

   Verified live, not just linked: booted the classic disc to Main and
   read the rendered legend directly off a screenshot - a green "A
   Select" and a red "B Back" both draw exactly where `SetRealCoord`
   targets them, at the real bottom of the screen, not the middle a
   virtual-space misread would have produced. `mainui` sweep clean
   (84/84, the one new file included); the full engine sweep was not
   re-run for this item, since nothing under `engine/` changed; classic
   disc links clean (0 duplicate, 0 undefined) with the new control and
   asset staged and packed. NOT verified: the D-pad and START glyphs
   specifically (Audio/Display/Pause are all reachable from Main but not
   screenshotted this pass), and whether the per-screen verb lists read
   correctly against what each screen's pad input actually does - both
   need a pad, the same limit every screen since item 4 has carried.

   **A genuine tooling bug found and fixed along the way, unrelated to
   this item's own code**: `tools/xbox-link-check.sh`'s manifest-drift
   check (`CURRENT_SRCS`/`MANIFEST_SRCS`, compared with `comm`) assumes
   `sweep-manifest.txt` never repeats a source line - true when
   `tools/xbox-sweep.sh` runs to completion once, false when an earlier
   invocation is killed mid-run (an interrupted session, this one
   included) while another instance's own truncate-and-append (`: >
   "$MANIFEST"`, then `exec 5>> "$MANIFEST"`) is still open on the same
   file: the killed run's own late writes interleave with the next
   clean run's, leaving ~30 duplicated lines. `comm -13`'s multiset
   comparison then reports each duplicate's SECOND occurrence as a
   source "no longer exists," failing the link-check's baseline gate
   deterministically - not flakily, though it read as flaky at first,
   since a single clean re-sweep (no other sweep running concurrently)
   produces a duplicate-free manifest and passes every time. Confirmed
   directly: `sort | uniq -d` on the manifest's own source column named
   exactly the files the tool was reporting as missing. No code change
   needed - a clean, uncontended sweep is the fix - but worth recording
   since it cost real time to trace and could bite the next session that
   interrupts a sweep mid-run.
13. **Text rendering and localization** (new, 2026-09-17 - the work
   decision (c) above reverses into). It **retrofits items 1, 4, 6 and 8,
   which already shipped**, so it is not a screen but a change under all
   of them. Its point is one sentence: *a menu whose words are pixels in
   a 1998 bitmap cannot be translated*, and three separate things make
   that true here, each of them invisible in English - which is exactly
   how all three survived this long. The steps are ordered because each gates the
   next, and the last one is deliberately last: **step 5 is the only
   irreversible-looking one** - the moment the strip stops drawing, every
   row's label is whatever the string layer returns, so the font (1) and
   both dictionaries (3, 4) have to be on the disc before it, or the menu
   ships in console glyphs reading `GameUI_NewGame`.
   1. **A real font on the disc, proved to be the one drawing. DONE,
      2026-09-17** (the staging and the proof; the measurement it produced
      is below and it changed this item's budget). Xbox
      already compiles a real rasterizer, not conchars:
      `tools/xbox-mainui-sweep.sh:164` passes
      `-DMAINUI_USE_CUSTOM_FONT_RENDER -DMAINUI_USE_STB`, so
      `FontManager.cpp:490-498` selects `CStbFont` (stb_truetype).
      **But it needs the file**: `FindFontDataFile`
      (`FontManager.cpp:537-548`) maps "Trebuchet MS" →
      `gfx/fonts/FiraSans-Regular.ttf` and "Tahoma" →
      `gfx/fonts/tahoma.ttf`, and on failure `CFontBuilder::Create`
      falls back to `CBitmapFont` **silently**
      (`FontManager.cpp:506-514`), which is engine conchars
      (`BitmapFont.cpp:147-153`) with a charset of ASCII 33-126 plus
      Cyrillic and nothing else (`:87-93`). Both TTFs exist in-repo
      (`3rdparty/extras/xash-extras/gfx/fonts/`) and `tools/xbox-xiso.sh`
      stages neither, so **today every Xbox menu string is drawn by the
      fallback, and Spanish, French and German are unrenderable whatever
      strings we pass**. Work: stage the fonts, then **prove which
      backend is live** by reading it out of the guest the way item 0
      read `s_state` and `r_images[]` - eyeballing cannot tell, the
      fallback is silent and English looks the same either way.

      **The face is decided and shipped: Fira Sans Condensed** (2026-09-17,
      after building all three candidates as real discs and reading the
      output at 640x480 rather than arguing from a mockup). The canvas
      asked for Barlow Semi Condensed, and **Barlow lost on coverage, not
      on looks**: parsing each TTF's own `cmap` gives Barlow **0/96
      Cyrillic and 119/128 Latin Extended-A**, against **382/382 across
      every uploaded range** for both Fira faces - so the canvas's own
      choice would have quietly capped how far this menu can ever be
      translated, the one thing item 13 exists to enable. Worth naming the
      irony: Barlow's narrowness is exactly what long German and Russian
      strings want, and it cannot draw Russian at all. Condensed Fira
      keeps that narrower column, covers everything, sits on the same OFL
      licence and superfamily already staged, and measured **identical
      atlas buckets** to the upright face - the swap is free. **A
      prediction this item made and got wrong, recorded because it nearly
      decided the question**: condensed faces were argued to break down at
      12-25 px under an unhinted rasterizer. Compared at 3x on real
      output, they do not - all three read cleanly at the row size, and
      that legibility argument should carry no weight here. The font lives
      in **`assets/fonts/`, not `3rdparty/extras/xash-extras/`**, because
      that path is an upstream submodule and nothing this fork owns may be
      added to it; `assets/` is this fork's own home for shipped content.
      IBM Plex Mono is still not taken: a second family for 10-11 px help
      text is a whole atlas, and step 2 already conceded that face.

      The reasoning that got here, kept because it constrains any future
      swap: the canvas asks for Barlow Semi Condensed and IBM Plex Mono,
      and FiraSans is merely what the mapper happens to name. **The tempting cheap option does not work** - dropping
      Barlow in under the filename `FindFontDataFile` already expects
      cannot render the design, because the canvas uses three Barlow
      weights (700 headings, 600 rows, 500 setting labels) and two mono
      weights, while `FindFontDataFile` **ignores its `weight` argument
      entirely** (`FontManager.cpp:535-545`) and `CStbFont` stores the
      requested weight without synthesizing it
      (`StbFont.cpp:49-56`, `:103-112`), so every request would rasterize
      the one regular face. The choice was either to ship a separate TTF
      per weight and teach `FindFontDataFile` to select on weight, or to
      relax the design. **Decided on the measured numbers below: one real
      weight per family, recorded as a deliberate departure from the
      canvas.** An extra weight is a whole extra 446-glyph atlas for a
      difference of 100 units of stroke thickness, and the budget it
      would eat buys the focus glow instead - which is the same amber the
      eye actually reads as "this row is selected". Licensing blocks
      nothing either way (Fira, Barlow and IBM Plex are all OFL).

      **What a face actually costs, computed from the code rather than
      guessed - this is what decided both open questions in this item.**
      A face is one RGBA8888 texture (`BaseFontBackend.cpp:193`, uploaded
      whole at `:211`), starting 256x256 (`:78`, `:98`) and **doubling**
      height-first to fit (`:153-167`); there is no paging despite the
      comment, every glyph shares the one image (`:215-220`). Its glyphs
      are rasterized **eagerly at creation, never lazily on draw**
      (`UploadGlyphsForRanges`, `:111-207`), and the ranges
      (`FontManager.cpp:444-451` plus `table_cp1251[64]`,
      `utflib.cpp:222`) come to **446 glyphs for every face, always** -
      94 ASCII + 64 Latin-1 + 128 Latin Extended-A + 96 Cyrillic + 64
      cp1251. Crucially, **face identity is `(name, tall, weight, blur,
      flags)`** (`BaseFontBackend.h:84`), so *every distinct type size is
      already its own 446-glyph atlas* - size is the expensive axis, not
      weight. On Xbox `scaleY` is 480/768 = **0.625** (`BaseMenu.cpp:1074`),
      so the five faces at `FontManager.cpp:78-104` are really tall 11
      (console), 12 (small), 16 (default), 16 (bold) and 25 (big) - four
      at 256x256 and one at 256x512, **1,572,864 B ≈ 1.5 MiB**. The 48
      staged caches corroborate every bucket exactly (each file is
      `atlas_bytes + 8130`, with deltas of 2^18/2^19/2^20).

      **MEASURED 2026-09-17, and the prediction above was wrong - the
      real figure is 2.75 MiB, not 1.5.** Staged the fonts and read the
      live texture table (`r_images[]` at `0x00f1d4bc`, stride 312,
      `name` at offset 0 and `width`/`height` at +260/+262). The *face
      set* was predicted exactly right - talls 11, 12, 16, 16@1000 and
      25 - and `GetTextureName` puts the backend in the name, so
      `#Trebuchet MS_16_500_stb_font.bmp` and its siblings are proof the
      TTF path is live and not the silent fallback. But **every face
      except tall 12 came out one bucket larger than predicted**:
      256x512 for default, bold and console, 512x512 for big, only small
      at 256x256. The
      bucket-doubling rule was right; predicting which bucket a given
      tall lands in from the staged PC-resolution caches was not. Two
      lessons, and the second is the reusable one: **`r_images[]` gives
      exact texture bytes for anything resident, so no atlas cost in this
      item ever needs predicting again** - and a prediction that survives
      review is still a prediction.

      **And a second correction, Codex review round 2 - the first pass
      counted the wrong bytes entirely.** Dimensions are not residency:
      `ref_soft` does not keep textures as RGBA at all. It converts to
      16-bit `pixel_t`, allocates **four mip levels**
      (`r_image.c:302-353`, `GL_CalcTextureSize` = `w*h*2`,
      `:82-85`), and allocates a **separate full-resolution alpha plane**
      when `TF_HAS_ALPHA` is set. So `w*h*4` is neither the colour
      allocation nor the total. `image_t` carries the real figure -
      `size` at +280 accumulates the colour mips, `alpha_pixels` at +300
      says whether the extra plane exists - and read that way, on the
      shipping four-face disc:

      | texture | colour mips | alpha plane | resident |
      |---|---|---|---|
      | `Trebuchet MS_25_500` (512x512) | 696,320 | 524,288 | **1,220,608** |
      | `Trebuchet MS_16_500` (256x512) | 348,160 | 262,144 | **610,304** |
      | `Trebuchet MS_16_1000` (256x512) | 348,160 | 262,144 | **610,304** |
      | `Trebuchet MS_12_500` (256x256) | 174,080 | 131,072 | **305,152** |
      | **fonts total** | | | **2,746,368 B = 2.619 MiB** |
      | `#btns_atlas_0` (1024x1024) | 2,785,280 | none | **2,785,280 B = 2.656 MiB** |

      Two things fall out that the RGBA arithmetic hid completely.
      **The strip is 2.656 MiB resident, not 4.0** - it carries no alpha
      plane at all, which is what item 0's "~2.7 MiB of mips" reading
      (`:2168-2171`) was right about all along. And **the fonts' alpha
      planes are 1.15 MiB, 43% of their total** - the single largest
      lever in this item, bigger than weights, sizes and the glow put
      together.

      **Only Fira Sans ships; `tahoma.ttf` does not, and that is a
      licensing decision with a visible cost - Codex review round 1.**
      The tree carries both and `FindFontDataFile` maps the console face
      ("Tahoma", `FontManager.cpp:102`) to the second, but Fira Sans is
      SIL OFL 1.1 while **Wine's Tahoma is LGPL 2.1-or-later**, and its
      own notice points at a `COPYING.LIB` this tree does not have - so
      shipping it would put wrong provenance and an incomplete licence on
      every disc. Staged it anyway during the first measurement, which is
      how the cost is known rather than guessed: **with Tahoma the help
      and status lines render in a real antialiased face; without it they
      fall back to conchars**, screenshotted both ways. That is not a
      regression (they were conchars before this item too, and the face
      is short enough that `CBitmapFont` loads no texture at all, so it
      costs nothing) - it is an improvement declined on licence. **The
      follow-up is to give the console face an unencumbered TTF**, either
      by shipping an OFL mono/sans for it or by teaching
      `FindFontDataFile` that "Tahoma" resolves to the face already on
      the disc; it measured **512 KiB** as its own tall-11 atlas, which
      on these numbers is a fair price for legible help text on a TV.
      What must not happen is shipping the LGPL file to get it.

      **That 2.75 MiB is new cost, not existing cost - do not read it as a
      baseline.** It follows from this item's own first paragraph and an
      earlier draft got it backwards: because the TTFs are not staged
      today, every face falls back to `CBitmapFont`, whose
      `UploadGlyphsForRanges` is **empty** (`BitmapFont.cpp:95-98`) - no
      atlas is generated at all. What is resident instead is a single
      shared embedded bitmap, `#XASH_SYSTEMFONT_001.bmp` loaded from
      `menufont.h`'s compiled-in array (`BitmapFont.cpp:47-56`), shared by
      every face above the console height because `PIC_Load` keys on the
      name, with shorter faces drawing through the console instead. So
      **staging the fonts introduces those five atlases rather than
      reusing them**, and step 2's "+0" below means "+0 *beyond the five
      faces the engine already asks for*" - it avoids a sixth, seventh and
      eighth face, not the first five.
      The 1024 texture cap that halved the button atlas is **not binding
      here** - the largest face is 256x512, four doublings clear. Net
      against item 0's 3 MiB ceiling: **the strip's ~2.7 MiB leaves, and
      what replaces it is bounded by how many distinct sizes this item
      chooses to ask for** - which is why step 2 snaps to the existing
      handles instead of taking the canvas's sizes literally. A trimmed
      charset stays available as the lever if the measured number
      disappoints, since all 446 glyphs are paid for per face whether a
      screen draws them or not.

      **Net, on measured resident bytes, in the shipped configuration**:
      fonts **2.619 MiB** in, strip **2.656 MiB** out - **≈ −0.04 MiB. It
      is a wash**, and confirmed by reading `r_images[]` on a disc that
      has both changes: four font atlases, **no `btns_atlas` entry at
      all**. **Codex review caught this being claimed prematurely** and
      the correction matters more than the number: an earlier cut of this
      work staged the font while leaving the strip loading, which is not
      a wash but **5.275 MiB against a 3 MiB ceiling** - both allocations
      resident at once. Text and bitmaps are alternatives here, not
      layers, so steps 1 and 5 cannot be separate releases however
      convenient the slicing looks. Real text
      costs this console almost exactly what the retail bitmap strip cost
      it, to within 39 KiB, which is a better answer than either of the
      two this item predicted on its way here (−1.9 MiB, then −1.75) and
      the only one actually measured. **So the memory argument neither
      supports nor blocks this work, and should stop being offered as a
      reason for it**: the reason is that the menu becomes translatable.

      Three consequences for what is still unbuilt. **The glow is no
      longer a small ask.** Its two faces are blur variants at tall 16,
      and on these numbers a face at that size is 610,304 B with a
      further doubling for the heavy one - order **+1.8 MiB**, not the
      +768 KiB predicted, which would blow item 0's 3 MiB ceiling on its
      own. It stays decided in principle and **ships only if measured
      under that ceiling**, and it is the first thing to drop. **The
      alpha planes are the real lever**: 1.15 MiB of the fonts' 2.62 is a
      full-resolution 16-bit plane allocated only when `TF_HAS_ALPHA` is
      set (`r_image.c:322-323`), and a glyph atlas whose colour is
      already a coverage value may not need a second plane to say the
      same thing - worth one experiment before any charset trimming,
      because it is nearly half the cost and costs no glyphs. **And the
      charset lever is now second, not first**: Cyrillic plus cp1251 is
      160 of 446 glyphs for languages this fork has no strings in, still
      worth taking, but smaller than the alpha question.

   2. **Style it to the canvas.** The canvas is 1280x960, i.e. 640x480 at
      2x, so every number here is the canvas's own halved - real values
      this plan did not have before. **Heading**: Barlow 700, 32 px,
      uppercase, letter-spacing 0.75, `#ffb020`, over a 310x1.5 rule
      fading `#c8861c` → transparent. Our three shipped headings use
      `QM_BIGFONT`, which is `hBigFont` at 40 px (`FontManager.cpp:84`,
      `BaseItem.cpp:100-118`) coloured `uiColorHelp` grey - wrong on both
      counts against the design. **Row label**: Barlow 600, **20 px on
      top-level screens, 17 on sub-screens, 18 on Controller** - the
      canvas disagrees with itself across artboards. **Help/status**: IBM
      Plex Mono 400, 10 px, `#8ea8bd`. **Setting label** 14 px `#e6dccb`,
      **value** 13-14 px `#f1e8d8` or `#ffb020`, **legend** 12 px
      `#cfc6b6` with 10 px glyph letters on the pad colours (A `#6dbe45`,
      B `#d9322d`, X `#2f79c8`, Y `#f0a91c`).

      **Decided: snap every one of those onto the five faces that already
      exist, rather than ask for the canvas's sizes literally.** Step 1's
      arithmetic is the reason - each distinct `tall` is another eager
      446-glyph RGBA atlas, so taken at face value the canvas needs about
      eight faces (heading 32, rows at both 20 and 17, label 14, value 13,
      legend 12, mono 10 and 19), and **the 32 px heading alone lands in
      the 512x512 bucket at 1 MiB, double today's big font** - roughly
      2.5-3 MiB of fonts before a single extra weight, which would eat the
      whole ceiling on its own. The mapping, in real Xbox pixels: heading →
      `hBigFont` (**25**, against the design's 32 - the one visible
      concession, and the only place the departure shows), rows → default
      (**16**, which is within a pixel of the canvas's own sub-screen 17
      and settles the 20/17/18 disagreement by construction), setting
      label / value / legend → small (**12**, exact on the legend), help →
      console (**11**). That is **zero new faces and zero added bytes**.
      Two departures to record with it: the heading is 7 px shy of the
      canvas, and the help line loses its monospace face (the console font
      is Tahoma, `FontManager.cpp:102`, and a real mono family would be a
      whole second family of atlases for a texture the eye barely reads at
      11 px). If the heading's size turns out to matter on a TV more than
      it does in the artboard, adding that one face back is a **+1 MiB**
      decision to take on its own evidence, not a default. **The palette is a real
      divergence in every role, not a nudge**: stock is
      `uiPromptTextColor` `#F0B418` base, `uiPromptFocusColor` **pure
      yellow `#FFFF00`** focus and `uiColorHelp` neutral grey `#A0A0A0`
      (`BaseMenu.cpp:61-71`, defaulted onto every item at
      `BaseItem.cpp:65-66`), while the design wants dim amber `#c8861c` →
      bright amber `#ffb020` and a cool blue-grey `#8ea8bd` for help -
      note the *shape* of the gap, that stock's base is roughly the
      design's focus and stock's focus is a yellow the canvas never uses.
      **Most of it can be content rather than code**: `UI_ApplyCustomColors`
      parses `HELP_COLOR` / `PROMPT_TEXT_COLOR` / `PROMPT_FOCUS_COLOR` and
      the rest from `gfx/shell/colors.lst` (`BaseMenu.cpp:940-975`), and
      our disc is Xbox-only content, so the base and focus ambers can ship
      there at zero `mainui` divergence - the cheapest form this fork has.
      **But not all of it, and the exception is one we created**: the three
      headings already shipped set `colorBase = uiColorHelp`
      (`Audio.cpp:155-158`, `GameOptions.cpp:145-148`), and `colors.lst`
      has a single `HELP_COLOR` feeding that one global, so pointing it at
      the design's blue-grey `#8ea8bd` for the help lines would drag the
      headings blue-grey with it - the canvas wants them amber `#ffb020`.
      So the heading needs a colour of its own, set in code under `#if
      XASH_XBOX` (or a distinct parsed role added), which is a real if
      small divergence to record - and **the acceptance check has to read
      heading and help colour independently**, since a single sampled
      colour cannot tell the two apart.

      **DONE, 2026-09-17, base and heading colours - not help.** Checked
      first, as this note asked: the classic WON install's `pak0.pak` has
      no `colors.lst` (grepped, 0 hits) and the built stage carries none
      either, so there is nothing to win or lose against - a genuinely
      free slot. Added the distinct parsed role rather than a code-only
      `#if XASH_XBOX` special case: `uiColorHeading` (`BaseMenu.h`/`.cpp`)
      and a new `HEADING_COLOR` key in `UI_ApplyCustomColors`, defaulting
      to `uiColorHelp`'s own current value so a build with no
      `colors.lst` looks exactly as already shipped, not silently
      different. `Audio.cpp`'s and `GameOptions.cpp`'s headings now read
      `uiColorHeading`; `GameOptions.cpp`'s own stock, non-Xbox
      `networkMode` label (the "Select network mode:" text sharing the
      same primitive) is untouched, still `uiColorHelp`, since it is
      retail's own text. `assets/gfx/shell/colors.lst` (new, fork-owned,
      staged by `tools/xbox-xiso.sh` alongside the font - **fatal if the
      source is missing, same as the font/dictionary gates, corrected in
      Codex review round 1** after the first draft argued the engine's
      own graceful runtime handling of a disc that never had this file
      licensed the packaging script tolerating its own tracked source
      vanishing too - two different questions with two different
      answers; verified with a negative fixture, source removed then
      restored byte-identical) sets `HEADING_COLOR` and
      `PROMPT_TEXT_COLOR`/`PROMPT_FOCUS_COLOR` to the canvas's amber pair.
      **`HELP_COLOR` is deliberately left unset, not an oversight**: it
      is one global shared by two roles the canvas draws differently - a
      slider's or checkbox's own label (`Slider.cpp:165/206`,
      `CheckBox.cpp:48`, the canvas's tan `.lbl`) and a screen's
      status/help line (`PicButton.cpp:252`, the canvas's blue-grey
      `.hint`) - so setting it either colour gets the other role wrong.
      That needs a second new role, the same shape as `HEADING_COLOR`,
      not guessed here - a real, named follow-up, not a silent gap.
      **Verified live, base colour only - the honest limit of a boot
      screenshot**: the built disc's row labels sample as `#ce8e18`
      against a `#c8861c` target (Main's own screenshot, sampled from
      solid glyph pixels, not an edge) - close enough to be unmistakably
      the new amber and nothing like stock's `#F0B418` base or `#FFFF00`
      focus, confirming the whole `colors.lst` → `UI_ApplyCustomColors` →
      global pipeline actually runs on Xbox, not just compiles. **NOT
      verified live**: `HEADING_COLOR` and `PROMPT_FOCUS_COLOR` specifically,
      since neither Audio's nor Game's heading, nor a focused row, is
      reachable without a pad - the same limitation already accepted for
      every one of these screens' own content since item 8. Both parse
      through the identical mechanism the base colour just proved live,
      which is evidence, not the same thing as having watched it happen.
      **The focus glow is the one
      thing the design asks for that Xbox currently cannot draw**: the
      canvas's focus is colour *plus* `text-shadow: 0 0 16px
      rgba(255,176,32,0.55)`, which is precisely what
      `PicButton.cpp:311-353`'s two-layer `hHeavyBlur` + `hLightBlur`
      path does - and those two handles are created only outside
      `lowmemory` (`FontManager.cpp:91-100`), which `tools/xbox-cc.sh:329`'s
      `-DXASH_LOW_MEMORY=1` makes permanently true on Xbox
      (`host.c:1239` → `BaseMenu.cpp:1191`). Xbox therefore lands on
      `:354-379`: one flat `UI_DrawString`, drop-shadowed, no bloom.
      **Decided: lift the two blur faces out of the `lowmemory` gate on
      Xbox and draw the glow.** It is the cheapest thing in this item and
      the most visible - **+768 KiB total**, against the ~2.7 MiB the strip
      gives back. The blur radii scale with everything else (`2*scale` → 1,
      `8*scale` → 5, `FontManager.cpp:94`, `:98`) and blur widens the glyph
      cell, since `GetHeight()` adds `GetEfxOffset() = blur + outline`
      (`BaseFontBackend.h:75`, `:82`) - so the light face is **free** in
      bucket terms (blur 1 at tall 16 stays 256x256, ≈262 KiB) and the
      heavy face costs **exactly one doubling** (≈524 KiB). Both confirmed
      against the staged cache, which holds the matching
      `Trebuchet MS_22_1000`, `_g21.00` and `_g81.75` atlases at precisely
      those bucket sizes. Note this is the one place the `lowmemory` gate
      is lifted rather than respected, so it is a real divergence to
      record - and the reason it is affordable is exactly the reason the
      gate existed: the strip it was protecting against is gone.
   3. **Make our own strings reachable by a translator. DONE, 2026-09-17
      - and the premise below was wrong, which is the useful part.** This
      step expected to invent keys for ~20 strings and ship a
      `mainui_english.txt` defining them. Neither is needed: **`mainui`'s
      own convention is that the key IS the original English string** -
      `3rdparty/mainui/translations/mainui_skeleton_*.txt` says it
      outright, "Tokens on left are original strings, on right is
      translation", and every entry is an English sentence. So `L( "Done" )`
      is already a correct, translatable call: the literal is the lookup
      key, and `L()` returning its argument on a miss is not a bug to
      route around but **English as the built-in fallback**. No menu
      source file needed changing, and no `mainui_english.txt` needs to
      exist. What was actually missing was simply a translation file, so
      this step reduces to shipping one - `assets/resource/mainui_<lang>.txt`,
      staged by `tools/xbox-xiso.sh`. Measured against the real build
      rather than estimated: preprocessing the four reshaped screens for
      Xbox gives **35 strings compiled in, 15 resolved by the shipped
      dictionaries, 5 by the compiled-in `StringsList_` table and 15
      literals**, not the "roughly twenty misses" this plan claimed. `L()`
      (`Utils.h:148,150`, `MenuStrings.cpp:75-89`) is a dictionary lookup
      that **returns its argument unchanged on a miss** (`:88`) - so a
      missing key is invisible in English and permanent in every other
      language. Today the `GameUI_*` keys resolve (checked against
      `gameui_english.txt`), while **`"Done"`, `"Game"`, `"Crosshair"`,
      `"Fast weapon switch"`, `"Controls"`, `"Touch"`, `"Update"` and
      every status line this fork wrote are misses** - roughly twenty
      strings that look localized and are not. `Localize_InitLanguage`
      already loads `resource/mainui_<lang>.txt`
      (`MenuStrings.cpp:456-474`) and **nothing ships one** (only
      uncommitted skeletons in `3rdparty/mainui/translations/`). So: give
      every string this fork wrote a real key, ship `mainui_english.txt`
      (UTF-16LE with BOM, `:334-379`), stage it on both profiles. Status
      lines need no change of mechanism - `PicButton.cpp:243-255` draws
      `szStatusText` independently of all three button branches, so they
      already localize once they have keys.
   4. **Stage the dictionaries onto the classic profile - this one gates
      step 5, and skipping it ships a visibly broken menu. DONE,
      2026-09-17**: `tools/xbox-xiso.sh --resource-dir <dir>` supplies
      them under delta.lst's own provenance rule (the user's install,
      never this repository), and the pack **fails** when a profile has
      neither them nor the flag - tested by running it without, which
      refused and produced no ISO.
      Checked, not assumed: the retail WON install has no `resource/`
      directory at all, its `pak0.pak` contains neither `gameui_english`
      nor `strings.lst`, and the built classic stage
      (`.sweep-xiso/hl/valve/`) therefore carries **no localization
      dictionary of any kind**. The `GameUI_*` keys have **no compiled-in
      fallback**, so `L( "GameUI_NewGame" )` returns its own key as the
      string - and the moment step 2 stops the strip drawing, that key
      *is* the visible label. Main alone would read "GameUI_NewGame",
      "GameUI_TrainingRoom", "GameUI_LoadGame", "GameUI_Options"
      (`Main.cpp:222-250`), with Audio and Game the same. **This is
      invisible today and stays invisible right up until the change
      lands**, for a reason worth writing down: the `StringsList_*` keys
      *do* resolve, from a compiled-in English table seeded into the
      dictionary at init (`MenuStrings.cpp:35`, `:485-492`; `:546` lets a
      real `strings.lst` override it), which is exactly why the classic
      disc's status lines read correct English in every screenshot taken
      so far while its row labels were bitmaps. Two different mechanisms,
      one of which has no fallback. So: stage `gameui_<lang>.txt` and
      `valve_<lang>.txt` alongside `mainui_<lang>.txt` on **both**
      profiles and for **every** language shipped - `tools/xbox-xiso.sh`
      supplies the classic profile only `delta.lst` today (`:660-678`) and
      needs the same treatment for these. And make it a packaging
      postcondition rather than a hope, the way `xbox-xiso.sh` already
      refuses a disc with no `delta.lst` (divergence #77's own precedent).
      **Specify that check carefully, because the obvious version of it
      passes while broken**: matching on a `GameUI_*` / `StringsList_*`
      prefix would not catch step 3's own new keys at all - those are
      whatever namespace this fork picks (say `R3D_Done`), and a missing
      one renders its own literal exactly like the others while a
      prefix-matching validator reports success. So **pick and write down
      the key namespace** for this fork's strings, then make the check
      namespace-independent by testing the actual failure mode: **every
      visible key on every shipping screen - not a representative sample -
      resolved through the shipped dictionary, failing if any lookup
      returns its input unchanged** (`MenuStrings.cpp:88`, which is the
      one observable signature a miss has). And prove the validator itself
      works before trusting it: include a fixture with one deliberately
      missing non-`GameUI_*` key and require the check to fail on it -
      otherwise the build goes green for the same reason English did.
   5. **Turn the strip off. DONE, 2026-09-17**, together with steps 1, 3
      and 4 - they are one release, for the budget reason above.
      `uiStatic.renderPicbuttonText` is set from
      the gameinfo flag at `BaseMenu.cpp:1212`; setting it under `#if
      XASH_XBOX` both routes every `CMenuPicButton` to its text branch
      and makes `Btns.cpp:42`'s existing early return skip loading the
      atlas - one line for both halves. Items 1/4/6/8's `PC_*` ids go
      inert, and divergence #67's `PC_ADV_OPT` → `PC_ADV_OPT2` →
      no-picture sequence becomes a correct fix for a case that no longer
      arises; keep it, but the ledger should record that a general rule
      replaced it. Note this is only visible on the classic WON profile:
      the HL25 disc ships no `btns_main.bmp` at all, so it already draws
      text buttons today and is the closest thing to a preview of the
      result that exists.
   6. **Prove it is localizable, not merely text. DONE, 2026-09-17 - and
      it found a real defect no amount of English testing could have.**
      Shipped `assets/resource/mainui_german.txt` (31 tokens, UTF-16LE
      like the retail dictionaries), packed a disc with
      `--ui-language german`, booted it, and photographed the result.
      **The menu rows are correctly German and their umlauts render
      exactly right** - "Trainingsgelände" with a real ä, drawn through
      the `mainui` font. That is the whole chain working: `vfs.cfg` →
      `ui_language` → `Localize_InitLanguage` → dictionary override →
      Fira Sans Condensed glyph.
      **The help lines beneath them are mangled**: "früher" renders as
      "frher" with the ü silently dropped, and "ändern" as "?dern".
      **DONE, 2026-09-17 - fixed, and the diagnosis above was wrong in a
      useful way.** `PicButton.cpp:243-255` draws `szStatusText` via
      `EngFuncs::DrawConsoleString`, which reads as "the engine's console"
      but is not: it is a **local `mainui` function**
      (`EngineCallback.cpp:57-69`) that calls `UI_DrawString` with
      `uiStatic.hConsoleFont` - the exact same rasterizer already drawing
      Fira Sans Condensed correctly for every row label. The real engine
      developer console (`Con_DrawString`, `con_oldfont`, divergence #63)
      is a wholly separate path this bug never touched. So the root cause
      was the identical shape to `"Trebuchet MS"`'s own fix, just never
      applied to this second family: `hConsoleFont` requests `"Tahoma"`
      (`FontManager.cpp:34`, `:102`), `FindFontDataFile` mapped that only
      to `tahoma.ttf` - the Wine/LGPL file this fork deliberately never
      stages - so `CStbFont::Create` silently fell back to `CBitmapFont`,
      ASCII plus Cyrillic only. Fixed by mapping `"Tahoma"` to the same
      `FiraSansCondensed-Regular.ttf` already on the disc, under `#if
      XASH_XBOX`, mirroring `"Trebuchet MS"`'s own branch exactly - no
      second font, since this face is tall 11 and a second family for the
      smallest text on screen would be a whole extra atlas nobody could
      tell apart from the UI face at that size. **This promotes the
      console-face follow-up from cosmetic to required**: the note under
      step 1 costed an unencumbered TTF for that face at 512 KiB and
      called it a fair price for legibility - it is not a matter of
      legibility, it is the difference between a menu that can be
      translated and one that can
      only translate half of itself. **Re-verified after the fix, same
      German disc**: "Trainingsgelände" and "früher" both now render
      correctly, help lines included. **Measured with `memstats`, not
      assumed - but this is NOT the deferred whole-profile acceptance
      measurement (item 0, `not done this session` above), and should
      not be read as one.** Adding the fifth font atlas costs `+162
      pages = +648 KiB` of kernel `VirtualMemory`
      (`tools/xemu-debug.py memstats`, same disc before/after this fix),
      corroborating the `r_images[]`-based prediction of `610,304` B for
      a tall-11 face at that bucket. Separately, the end-to-end move from
      the last pre-item-13 build (strip resident, no font work at all) to
      this one is `3277 → 3508` pages, a net `+231 pages = +0.902 MiB`.
      **That number is complete for what it claims and no more.** All
      five font faces are created eagerly, once, in the menu-wide
      `UI_VidInit()` (`BaseMenu.cpp:1049,1090`) before any screen's own
      `_VidInit` runs - unlike the `head_*.bmp` banners item 0's own
      deferred measurement is about, which load lazily per screen
      (`Configuration.cpp:50` and each sibling's `_Init`) and are exactly
      why a single boot-to-Main snapshot was ruled insufficient there. So
      this delta genuinely does capture the whole cost of item 13's own
      work on the four screens that exist today (`Main`/`Configuration`/
      `GameOptions`/`Audio`, all already reshaped to text headings, zero
      banner cost on this graph by construction) - it is not partial for
      THAT claim. What it is not, and does not attempt to be, is the
      *whole-profile* ceiling verdict item 0 defers until `Controller`
      and `Display` exist and a real pad traversal is run: this
      measurement's baseline and its subject are the same narrow screen
      set, background cost cancels out of the delta identically on both
      sides, and no traversal into a fifth screen happened. Read it as
      "item 13's total cost is +0.902 MiB, measured" - not as "the
      profile passes the ceiling", which remains the same deliberate
      deferral recorded above, unchanged by this work. The only test that
      means anything: ship a real second
      language file on the disc, flip `ui_language`
      (`engine/common/filesystem_engine.c:43`), and screenshot a screen
      whose words include accented glyphs. English
      proves nothing here - a baked bitmap label, a missing dictionary key
      and a font that cannot draw the glyph all look identical and correct
      in English, which is how this plan carried all three at once.

**Where the code lives - decided by the sibling's example and this
project's own precedent. DONE, 2026-09-16.** Substantive `mainui` changes
cannot live as uncommitted patches, and a menu rewrite is far past what a
patch file should carry. Did what `deps/hlsdk` already does (divergence
#44's precedent) and what xash-rt did: forked FWGS `mainui_cpp` to
`mathiasdbo/mainui_cpp`, a `resonance3d` branch created from the exact
commit already vendored (`8843994`, no source content changed), `.gitmodules`
re-pointed at it with `branch = resonance3d`. One thing the plan had not
anticipated: `miniutl` (divergence #20's `winlite.h` fix) is nested a
level deeper, inside `mainui_cpp` itself (`FWGS/miniutl`, not this repo's
own `.gitmodules`) - the patch this project auto-applied at sweep time
could not fold into the outer fork alone, so it needed its own fork too
(`mathiasdbo/MiniUTL`, `resonance3d` branch, the fix now a real commit),
pinned by the mainui fork's own `.gitmodules`. `tools/xbox-mainui-sweep.sh`'s
patch-auto-apply preflight is replaced with a `git submodule sync
--recursive` + `update --init --recursive` the sweep now runs itself,
unconditionally, every time - found in Codex review round 1: an already-
initialised checkout caches the OLD FWGS urls in its own `.git/config`,
and `git submodule update`, even `--init --recursive`, does not re-read
`.gitmodules` for a submodule already registered, so it would otherwise
try to fetch this fork's commits from upstream and fail. `sync` is local-
only and a no-op when already correct, so running it unconditionally
self-heals either case with no manual step for a caller to remember.
Verified against the exact
same commits so nothing else moved: the mainui sweep still reports 82 ok /
0 failing, and the baseline engine+menu link still reports 0 duplicate / 0
undefined. Divergence ledger: #57 (this move) and #20 (updated to point
here instead of at the retired patch file); the ledger's "the menu does
not need rewriting" bullet was already narrowed to the framework ahead of
this. Every future screen change goes `#if XASH_XBOX`-guarded where it
alters a screen the PC build still shows, same as planned.

**Art and memory.** Reuse every existing `PC_*` button id where the
content provides the strip - **corrected 2026-09-16: HL25 content ships
NO `gfx/shell/*.bmp` at all** (only the three `kb_*.lst` files; no `.pak`
either), so on that profile every label renders through mainui's
text-button path; the strip exists only on pre-HL25 content, and item 0
above covers both. The `head_*` banners are NOT reused: the design
replaces every one with a text heading, which is also what makes item 0
step 5's deferred ceiling measurement tractable - on the shipping screen
graph there is no banner to accumulate, so the post-traversal number is
item 0's own art plus the Duke diagram and the save previews. New art is
one file: the Duke diagram - **baked 2026-09-18 and costed, superseding
this paragraph's own 512x256 estimate.** It is 280x208 opaque, which
`ref_soft`'s uploader makes 151 KiB resident rather than the 512 KiB a
512x256x32 reading suggests (that arithmetic counted four bytes per texel
for a renderer that stores two, and no mip chain); item 0 step 7 has the
measurement and the reasoning. It is still XB's bake that decides the
final GPU format, not this phase. Title-safe layout (90%) for every rewritten
screen; minimum legible text at 640x480 on a CRT per `ui-architecture.md`
§7 - the canvas's 2x rendering is the check: anything unreadable at 2x on
a monitor is unreadable at 1x on a CRT.

**Sequencing.** Item 0 (retail menu art, both profiles) first - it is the
smallest change, it is what a player sees before any button is pressed,
and its `memstats` numbers set the budget every later screen's art has
to fit under. Then XM.1 (the single-player screens of the 2026-09-17
design: Main, New Game, Load/Save, Options, Controller with its Settings
and Customize sub-screens, Audio, Display, Game, the single-player pause
screen, the hidden PC screens); X7.5 (a disc
per title, so a menu change can be tested on a real
build without hand-packing) is done for `hl` as of 2026-09-16, and XM.1
runs alongside X8. XM.2 (any mode or
widescreen-presentation control) strictly after X9, because it is X9's
backend work that gives such a control anything to drive - doing the
screens first means X9 is then tested through the menu a player will
actually use. **XM.3 (the multiplayer screens) strictly after XN**, for
the same reason and more sharply: every one of its screens is a view
onto a socket, and there is no socket in the build today - a menu
written against the stub would be eight screens nothing can answer.
XN itself is optional to the project's spine (a single-player port
reaches X10 without it); it is sequenced whenever multiplayer stops
being optional to the person deciding, and nothing in X7-X11 waits on
it.

**Exit, XM.1:** on the disc, with a pad only, against the canvas screen
by screen: Main reaches New Game, Training Room, Load Game and Options,
and shows Resume Game and Save Game only once a map is loaded; Options
shows exactly Controller, Audio, Display and Game; Display's brightness
and gamma changes are visible on the next frame and its output row
matches what the Dashboard is set to under NTSC 4:3, widescreen and
PAL50 settings alike (xemu's EEPROM flips each); from a running map,
START alone opens the in-game menu under every scheme, Custom included -
the single-player pause screen, landed in item 10 (2026-09-18) but not
yet pad-verified doing any of this live: that it actually opens on
START, dims the scene, freezes it, shows the real chapter title and
difficulty, unpauses on every exit path exactly once (Codex round 1's
own START-closes-the-menu fix included), and leaves a game already
paused by BACK still paused after visiting it;
Controller shows the Duke with the selected scheme's labels, applies
Southpaw (and in a map the stick roles
really swap, not only the labels), Settings' sensitivity, invert and
vibration take effect in a map, Customize rebinds one action, X clears
one, Y restores the selected scheme completely (every gameplay action
bound again, checked in a map, never an empty bind left) - and after a
save-quit-boot every change, the Y restore included, is still there; the same sequence from an old keyboard-only `config.cfg`
first shows the seeded defaults, then the customisation; Game's three
toggles change their cvars; every screen's legend matches what the pad
actually does on it; no `head_*.bmp` is resident on any shipping screen
(read out of `r_images[]`, the way step 5 read the atlas) and no hidden
PC screen is reachable by any button; **every row on every screen is
text in the shipped font and the button strip is not resident at all**
(both read out of `r_images[]`, and the live font backend read out of
the guest rather than inferred from the screen, since the conchars
fallback is silent), the type and palette match item 13's chosen values
against the canvas, and **a second language file on the disc with
`ui_language` flipped actually changes the words on screen, accented
glyphs included** - the single test that distinguishes a localizable
menu from an English one that merely draws text - and the single-player pause
screen, once item 10 lands, is what START opens in a map: it pauses the
game, dims the scene with the HUD still readable, its Save Game / Load
Game / Options rows reach the same screens Main's do, B or START resumes,
and Quit to Main Menu confirms before leaving (leaving the map, so Main
is then its out-of-game self, with no Resume or Save row).
**Exit, XM.2:** a mode or
presentation change made in Display is visible on the next frame, on
hardware. Verified on xemu first, closed on hardware, same rule as every
other phase.

### XM.3 — the multiplayer screens (proposed 2026-09-17, blocked on XN)

Eight artboards, each a view onto something XN has to provide first.
The engine files they map to, corrected against the tree rather than
taken from the canvas's notes:

1. **Multiplayer hub** (`menus/Multiplayer.cpp`, rebuilt): System Link /
   Internet Games / Direct Connect / Split Screen / Player Profile /
   Done, with a network status line (IP, DHCP state) from the lwIP
   `netif` at top right. Spectate and Advanced stay hidden. This is also
   the row XM.1 left hidden on Main, and its "only when the NIC has a
   link" condition needs a link-state query XN adds - not a menu change.
2. **System Link** (`menus/ServerBrowser.cpp` in its `m_bLanOnly` mode,
   `:384`, `:940-943` - NOT a `LanGame.cpp`): the existing
   `CMenuGameListModel` and `localservers` broadcast, re-laid-out as
   Server / Map / Players / Ping with the selected server's rules under
   it, refreshed on a timer. Legend A Join / Y Host / X Refresh / B Back.
3. **Internet Games** (the same class in its other mode, `:982`'s
   `internetservers <filter>`): the same table fed from the master list
   instead of broadcast, a "only maps on this disc" filter applied
   client-side on the A2S_INFO reply (Y toggles it), and a
   Master Servers… row. The canvas's own performance note - query each
   master, dedupe by `ip:port`, then A2S_INFO the results 12 at a time,
   because the fan-out and not the master query is what costs on 64 MB -
   is a real constraint for XN to honour in the engine, not something
   the menu can enforce.
4. **Master Servers** (new screen, no upstream equivalent - and, corrected
   by Codex review round 1, **not a menu-only change either**). The first
   draft here said the screen need only write `xashcomm.lst` because
   `masterlist.c` is already multi-master. It is multi-master (`:486`,
   `:527`) and it does read that file (`:648`), but it cannot express
   what this screen shows: the persisted format is four keys -
   `master`/`master6`/`mastergs`/`masterstatic` (`:671-688`, written back
   at `:723-746`) - and every entry in it is enabled by definition, so
   there is **no disabled state to write**; `NET_InitMasters` adds the
   shipped `http://xash.su/server-list` static master unconditionally and
   BEFORE it loads the file (`:768-776`), so a "disabled" default would
   still be queried; and the only runtime commands are `addmaster`,
   `addmasterstatic`, `clearmasters` and `listmasters` (`:760-763`) -
   add and clear-everything, no per-entry remove, no enable, no re-test,
   and writing the file does not rebuild the in-memory list. So the
   engine side of this screen moves into **XN step 4**: enumerate, add,
   remove, enable/disable, test and reload as real engine surface, a
   persisted format that can carry a disabled shipped default, and the
   in-memory list rebuilt atomically when it changes. The screen is then
   what the design draws - per-row enable, A edit, Y add, X remove, Test,
   Restore Defaults - over that. The canvas's own warning is worth
   keeping in the screen: a named master needs DNS, and an Xbox with no
   name server from DHCP needs a numeric address.
5. **Direct Connect** (new): address on a D-pad numeric keypad (digits,
   dot, colon, backspace), port 27015 assumed, the last three addresses
   kept on the hard disk, `connect <addr>`.
6. **Host Game** (`menus/CreateGame.cpp`, trimmed): server name, map,
   mode (deathmatch/teamplay), max players 2-8, `mp_fraglimit`,
   `mp_timelimit`, `mp_friendlyfire`, `mp_weaponstay`; Start runs the
   listen server through `map`. Password and dedicated hidden.
7. **Player Profile** (`menus/PlayerSetup.cpp`, trimmed): name (through
   whichever keyboard XM.1 item 11's decision picked), model with the
   existing `CMenuPlayerModelView` preview, top/bottom colour swatches,
   spray. Persisted in `config.cfg` like every other cvar.
8. **Multiplayer pause** ("Game Menu"): explicitly NOT paused - the
   design says so on the screen, in a warning line, because a listen
   server cannot pause for one player - Resume / Player Profile /
   Options / Disconnect (confirmed), with a mini scoreboard from the
   client's own player info beside it and a pointer to the full one on
   BACK.

**Not in XM.3: Split Screen.** The canvas draws it (two player slots,
port 2 joining on START, a shared map/mode/limits block) and its note
already hedges - "grey out the row until it's measured". The engine
reason is harder than performance: there is no second local client
anywhere in Xash or GoldSrc, so this is a second `cl` state, a second
view, a second input port and a listen server serving both, on a 733 MHz
CPU already the constraint everywhere else in this plan. It stays drawn
and greyed, with no exit criterion, until somebody measures a second
viewport at all - not something XM.3 promises to deliver.

**Exit, XM.3:** on the disc, with a pad only, two Xboxes (or one Xbox
and one PC) on the same network: Main shows Multiplayer only with a link
up; System Link finds a game hosted from the other machine within one
refresh and joins it; Host Game starts a listen server the other machine
finds; Direct Connect joins by typed address; Internet Games lists
servers from at least one enabled master, its disc-map filter hides what
it should, and Master Servers' edits survive a reboot in `xashcomm.lst`;
Player Profile's name and colours show up on the other machine; the
multiplayer Game Menu opens on START without pausing, and Disconnect
confirms and returns to the Multiplayer hub. Verified on xemu first
where its networking allows, closed on hardware - the same rule as
every other phase, and on this phase hardware is the only honest test.

**Risks, named.** (1) Fork maintenance: the `mainui` submodule tracks
FWGS's `continuous` branch closely; an owned branch means merging upstream
`mainui` ourselves - bounded, since every change is behind `XASH_XBOX` and
confined to a handful of `menus/*.cpp`. (2) Display-mode enumeration on
nxdk: which of 480i/480p/720p/1080i the SDL2 video driver actually honours
is unmeasured (divergence #50 pinned 640x480; `hal/video.c`'s mode table
has 720x480 and HD modes) - measure before designing the spin control's
list. (3) Presets are only as good as the bind system: `+duck` is
hold-to-crouch in GoldSrc, and a "toggle crouch" preset would need a bind
alias, not engine code - decide per preset, in the plan, before writing
the table. (4) The Standard scheme's table assumes the Duke's triggers,
White and Black arrive as specific `K_*` names from nxdk's SDL2 backend -
unverified in this document; read them off divergence #53's seeding
before the table exists, or Standard silently binds Fire to a button the
pad never sends.

## XM tooling — synthetic pad input for live verification (proposed 2026-09-19)

**Why this exists.** Since item 4, nearly every XM.1 screen has only ever
been verified by booting to a static screenshot - never confirmed to
actually respond correctly to real pad input, because there was no way to
simulate a button press. Named explicitly, repeatedly, as "NOT verified
live... needs a pad" for items 4, 6, 7, 8, 9, 10, 12 and 13. Item 5
(Controller: scheme switching, live rebinding) is the next unstarted XM.1
item and is exactly the kind of screen that most needs this. `AGENTS.md`'s
own rule stands regardless of anything below: xemu is not validation, and
this tooling only ever narrows a gap - it never closes a phase.

**Mechanism**: xemu is a real open-source project (`xemu-project/xemu`
upstream) but this fork keeps no local checkout of it and has not built a
custom xemu for this - the binary in daily use here
(`G:\Games\Xemu\xemu.exe`, v0.8.136, commit `fc24584`, built 2026-06-08,
per its own `xemu.log`) is an ordinary prebuilt release, run as-is. It
reads whatever real controller Windows/SDL hands it, matched by SDL
joystick GUID in `xemu.toml`. `SDL_JoystickAttachVirtual` was considered
and rejected - it only injects into the calling process's own SDL
instance, not a foreign process like xemu.exe. Patching xemu's own public
SDL input path and building a custom binary was not attempted this pass -
not because it is impossible (it demonstrably is not, the source is
public), but because it commits this fork to building and maintaining a
custom xemu just for testing, a materially bigger and more fragile
undertaking than driving the existing binary from outside. The external
route chosen instead: an OS-level virtual gamepad driver, ViGEmBus (a
Windows kernel-mode virtual-XInput driver; archived/EOL upstream since
2023, but still the practical current standard - its Python client
wrapper, `vgamepad`, is itself still actively maintained) plus `vgamepad`
to create a virtual Xbox 360 controller. If Phase 2 finds this external
route too fragile (GUID drift across xemu versions being the likeliest
failure), patching xemu's own SDL path directly is the fallback, not a
closed door.

**Three phases**: 1) a feasibility spike proving the mechanism works at
all; 2) a durable harness (`tools/xemu-pad.py` for input, a new read-only
`cursor` command in `tools/xemu-debug.py` for state verification via the
`uiStatic.menu` → `CWindowStack` → `CMenuItemsHolder::m_iCursor` chain,
offsets resolved live through a new C++-aware sibling to
`tools/xbox-offsetof.sh`); 3) using that harness to actually verify
divergence #76's still-open claim (Options → Joystick/Controls no longer
showing Gyroscope/Adv. Controls).

**Phase 1, DONE, 2026-09-19, against xemu v0.8.136 (commit `fc24584`,
built 2026-06-08 - `xemu.log`'s own version banner; the bundled SDL2's
own exact version is not printed anywhere and stays unknown).** Confirmed
live: a virtual pad created via `vgamepad.VX360Gamepad()` is
indistinguishable from real hardware to Windows (`VID_045E&PID_028E`),
and pressing its buttons from an external Python process moved Main's
own focus in a running xemu guest, on that exact build. Two real
blockers found only by live experimentation, not documentation: xemu
drops all controller input while its own window lacks OS focus unless
`xemu.toml`'s `[input] background_input_capture = true` is set; and the
virtual pad's SDL GUID, as computed by an external `pysdl2` probe (a
different SDL2 build than xemu's own), did not match what xemu's own
"press a button to bind" capture UI produced - only the latter worked.
Whether a probe built against xemu's own exact SDL version would agree
instead is untested; until then, capture through xemu's own UI is this
fork's working method, not a claimed general limitation of SDL GUIDs.
Full account: `divergences.md` #77.

**Phase 2, DONE, 2026-09-19.** `tools/xemu-pad.py` (input, a `serve`/
client split - the virtual pad only exists while its owning process is
alive, so one persistent process holds it open for a whole test session)
and a new **read-only** `cursor` command in `tools/xemu-debug.py`
(`CMenuItemsHolder::m_iCursor`, reached by a pointer chase from
`uiStatic`), plus a new `tools/xbox-offsetof-cxx.sh` (a C++-aware sibling
to `tools/xbox-offsetof.sh` - every field the chase needs is protected or
private, and plain `offsetof()` on one is a hard compile error) and a thin
`tools/xemu-pad-scenario.py` composing the two. Verified live, end to
end, against the classic disc's real build: pressed Down, read the
cursor via an actual memory value (not a screenshot) and confirmed it
moved. Full account, every probed offset, and one real unexplained
timing finding (three cursor steps for two presses, likely UI
auto-repeat during the placeholder 200ms hold): `divergences.md` #78.
Codex review round 2 found two more real bugs in the harness itself
(a timed-out command's outcome conflated "never happened" with
"already in flight, do not retry"; a dispatch exception mid-press
could leave the virtual pad stuck holding a button down for the rest
of a session) - both fixed, `divergences.md` #79.

**Phase 3, DONE 2026-09-19.** `tools/verify-entry-76.py`, built on Phase 2's
harness, navigated Main -> Options -> Controls and Main -> Options ->
Joystick with a real synthetic press, screenshotting each and confirming
divergence #76's own still-open claim: neither screen shows its PC-only
row (Adv. Controls, Gyroscope). Live testing overturned assumptions a
source read alone had gotten wrong - Main to Options is five Down
presses, not three (AdjustCursor skips two always-hidden array slots the
same way `divergences.md` #78 already knew Main did); Options, Controls
and Gamepad all start their own item array with a decorative banner
picture, shifting every real button one index higher than a naive count
of `AddButton` calls suggests - a first version of the script, unaware of
this, checked the wrong index and would have passed even with a
forbidden row restored there, caught by an actual live run popping the
wrong button's own confirmation dialog. The fixed script activates each
screen's own OK/Done button and confirms, by window pointer, that doing
so closes back to Options rather than opening a third window. The
auto-repeat finding above needed a longer settle between presses (400ms),
not a shorter hold, to actually stop reproducing. Full account:
`divergences.md` #80; the result itself is recorded on #76.

## XN — Networking over lwIP (proposed 2026-09-17, optional to the spine)

**Why this exists now:** the menu design of 2026-09-17 asks for eight
multiplayer screens (XM.3), and every one of them is a view onto a socket
this build does not have. X0 parked networking with a two-item list -
"complete upstream's stub" first, "only if the console build ever wants
real networking, write the lwIP port layer" second. The design is that
"only if", answered yes. X0's first item is long done (divergence #29
completed `net_stub.h` so `net_ws.c`, `xrcon.c` and both `common/http/*.c`
compile clean against it); this phase is the second.

**What is actually missing, measured rather than assumed.** Less than the
`XASH_NO_NETWORK` flag suggests:

- **`net_chan.c` is not a blocker.** It shows FAIL in the broad sweep on
  `#include <bzlib.h>`, but that include is upstream's own
  `#if !XASH_DEDICATED` (`:21-23`) and `tools/xbox-link-check.sh` already
  builds the vendored `3rdparty/bzip2` (seven sources, `BZ_NO_STDIO` on
  `bzlib.c`, `:354-371`) and compiles the file with its include path -
  `.sweep/engine_common_net_chan.obj` is real and in every link this
  project ships. The sweep FAIL is a flag gap between the broad sweep and
  the real link, one of the known 29, not missing functionality.
- **lwIP is already up, and already used.** nxdk builds `libnxdk_net.lib`
  with `LWIP_SOCKET`/`LWIP_UDP` on, its headers are already on the engine
  include path (`tools/xbox-cc.sh:251-253`), and this project already
  sends real UDP over it from inside the engine - the boot log channel,
  `engine/platform/xbox/net_xbox_log.c`, divergence #13.
- **What is stubbed is `net_ws.c`'s sockets, and only those.**
  `-DXASH_NO_NETWORK=1` (`tools/xbox-cc.sh:290`) makes
  `net_ws_private.h:17` select `platform/stub/net_stub.h`, whose socket
  calls are all the same always-fails macro.

**The work, in order:**

1. **Write an Xbox/lwIP platform header and select it explicitly**, which
   is more than dropping a flag (Codex review of this plan, round 1,
   caught the first draft saying otherwise): Xbox is a sub-platform OF
   `XASH_WIN32` (the `build.h` patch nests it inside `defined _WIN32`),
   and `net_ws_private.h:17-25` tests `XASH_NO_NETWORK`, then
   `XASH_WIN32`, then others - so simply dropping `XASH_NO_NETWORK`
   selects `platform/win32/net.h`, whose first act is
   `#include <ws2tcpip.h>` (`:18`). That is WinSock, which nxdk does not
   have; `net_ws.c` would then want `WSAStartup`/`WSACleanup`
   (`:2009`, `:2072`), `WSAGetLastError` in four places and
   `net_ws_private.h:35`'s own `NET_ErrorString`. So: a new
   `engine/platform/xbox/net_xbox.h` selected by an `#elif XASH_XBOX`
   branch placed BEFORE the `XASH_WIN32` one, providing what that header
   provides - socket types, error reporting (lwIP sets `errno`, there is
   no `WSAGetLastError`), startup/shutdown (lwIP needs its own init, not
   `WSAStartup`), and the close/ioctl spellings - over
   `<lwip/sockets.h>`. X0's own remaining question is then the real one
   inside this: whether `net_ws.c`'s field-by-field use of
   `sockaddr_in6`/`sockaddr_storage`/`in6_addr` compiles against lwIP's
   versions or needs a translation shim. Expect IPv6 to be the sore
   point: lwIP's v6 is a build option and nxdk's `lwipopts.h` decides it -
   read that first, and if v6 is off, the honest shape is a v4-only Xbox
   path, not a fake v6 one. Audit the other users of the same header in
   the same pass, not after: `xrcon.c` and both `common/http/*.c`, which
   divergence #29 got compiling against the stub and which will follow
   this branch instead.
2. **One netif lifecycle, shared.** The log channel brings lwIP up on its
   own today, and divergence #33 already records its DHCP being
   unreliable enough to lose a whole boot's logs. Two independent
   bring-ups is one too many: give the engine a single init that both the
   log channel and `net_ws.c` use, with DHCP state observable rather than
   assumed.
3. **A link/address query** the menu can ask - for XM.3's Multiplayer row
   ("visible only when the NIC has a link") and the hub's status line
   (IP, DHCP state). This is engine surface that does not exist in any
   form today, on any platform, so it is ours to define narrowly: link
   up, address, DHCP state, nothing more.
4. **Server discovery within the memory budget, and a master list the
   menu can actually edit.** `localservers` (broadcast) and
   `internetservers` (masters) already exist in the engine and are what
   XM.3's two browsers call. What needs measuring on this hardware is the
   fan-out the design's own note names: dedupe by `ip:port` across
   masters, then A2S_INFO the result set in bounded batches (the canvas
   says 12 at a time) - on 64 MB the reply storm, not the master query,
   is the cost. The master LIST, corrected by Codex review round 1, is
   engine work and not just a file the menu writes: `masterlist.c` is
   multi-master (`:486`, `:527`) and reads `xashcomm.lst` (`:648`), but
   its persisted keys carry no disabled state (`:671-688`, `:723-746`),
   `NET_InitMasters` adds the shipped `xash.su` static master
   unconditionally before loading that file (`:768-776`), and the only
   commands are add / add-static / clear-all / list (`:760-763`) - no
   per-entry remove, no enable, no reload. XN owns: enumerate, add,
   remove, enable/disable, test and reload as engine surface; a format
   that can represent a disabled shipped default (so Restore Defaults
   means something and a disabled default is genuinely not queried); and
   an in-memory list rebuilt atomically when any of that changes.
   Persistence tested across a reboot, not just across a menu close.
5. **Then, and only then, XM.3.**

**Exit, XN:** on hardware, two machines on one network: the engine
resolves a link and an address and says so; `localservers` finds a game
hosted by the other machine and `connect` joins it; a listen server
started here is joinable from the other machine; `internetservers`
against at least one master returns a server list without exhausting
memory (measured with `memstats`, against X3's ceiling, during the
fan-out - not after it); and the master list survives a reboot with a
shipped default disabled and genuinely not queried, and re-enabled by
Restore Defaults. No menu is required to pass this: the console and the
commands - including the new per-entry ones step 4 adds - are enough,
and keeping the exit that way is what stops XN and XM.3 from being one
untestable lump.

**Risks, named.** (1) IPv6: upstream's `net_ws.c` is v6-aware throughout
and lwIP's v6 is optional - a v4-only Xbox path is a divergence with a
real surface, not a one-liner. (2) DHCP on real hardware has already
bitten this project once (divergence #33); a menu that reports "no link"
because DHCP silently failed is worse than no menu. (3) Scope: this is
the first phase in this plan that adds a whole subsystem rather than
porting one, and it is optional to the single-player spine - X10 (first
playable) does not wait on it, and neither does anything in X7-X11.
(4) Split screen (XM.3's own excluded item) is NOT solved by this phase
and is not networking: it is a second local client, an engine change of
a different kind, and no part of XN brings it closer.

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

**Corrected, X6 review, 2026-09-15: the prerequisite below was wrong,
disproved by both the actual code and a live controller test.** An earlier
specialist review claimed the client module needs
`engine/platform/sdl2/sys_sdl2.c`'s Xbox `SDL_Init`
(`SDL_INIT_TIMER | SDL_INIT_EVENTS` only) to add `SDL_INIT_GAMECONTROLLER`
up front, because `cl_dll/inputw32.cpp:31-33,148` drives the gamepad
through `SDL_GameController`. It does not need to: `IN_Init` already calls
`Joy_Init` → `Platform_JoyInit` (`engine/client/input/in_joy.c:619`), which
calls `SDL_InitSubSystem( SDL_INIT_GAMECONTROLLER )` itself on demand
(`engine/platform/sdl2/joy_sdl2.c:370-371`) - and `IN_Init` runs before the
client module loads, so the subsystem is already up by the time
`inputw32.cpp` touches it. X6's live test (main menu navigated with a real
gamepad, no `sys_sdl2.c` change made) confirms this end to end. No engine
change needed here; delete this bullet's original claim rather than carry
it forward as a live prerequisite.

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

### X7.5 — Discs: one XISO per title — `hl` done, 2026-09-16

`tools/xbox-xiso.sh <title> --steam <path> --xbe <path> [--out <path>]`
(replacing the hand-rolled `extract-xiso` invocation used through X5
bring-up) copies the title's gamedir and its basedir from the given Steam
install root, drops in the already-built `default.xbe`, and packs the
result with `extract-xiso` - never copying retail content into the repo
(staged under the gitignored `.sweep-xiso/`), none of it is ours to
redistribute. Disc-size and load-time work (`.mpak`, X8) is deliberately
NOT here; a raw, uncompressed retail tree is fine for bring-up.

**`gameinfo.txt`/`video.cfg` need no generation step, which this plan's
own earlier wording did not make clear.** The assumption going in was that
this phase would have to port `filesystem/gameinfo.c`'s `liblist.gam` →
`gameinfo.txt` conversion (`FS_InitGameInfo`/`FS_ParseGenericGameInfo`,
read in full while scoping this - a hand hazard: `common/defaults.h:185`'s
`DEFAULT_MAX_EDICTS` is 1200 today, "was 900 before HL25", while an
already-extracted test disc's own `gameinfo.txt` from an earlier session
shows `max_edicts 900` and is missing fields the current `FS_WriteGameInfo`
always emits - proof that file was generated by an older engine build, not
a reference to copy). Checked directly against a real local Steam install
(`D:\SteamLibrary\steamapps\common\Half-Life`) before writing a single
line of conversion code: every gamedir (`valve`, `gearbox`, `bshift`)
already ships its own `gameinfo.txt`, written by the modern Xash3D FWGS
build Valve now distributes Half-Life with - the file this session had
been treating as a stale artifact was in fact copied verbatim from this
same real install in an earlier session. Its `video.cfg` already sets
`r_refdll ""` too, which is what actually matters for the renderer choice
(X5); the numeric fields (resolution, refresh, fullscreen) are read but
overridden by the Xbox video path's own hardcoded 640x480 (divergence
#50), so they need no Xbox-specific edit either. "As upstream generates
it" in the original wording above meant "ship the file upstream already
generated," not "regenerate it here" - the script does neither generation
nor editing, only copies. If either fact ever stops holding for some
future title, `filesystem/gameinfo.c` is exactly where to read the real
algorithm to port faithfully, not guess - not attempted this pass because
it was not needed.

**`hl` end to end, this session:** packed `.sweep-xiso/hl.iso` (631,701,504
bytes - `valve/`'s own 601 MiB, no compression, matching the plan's own
statement of what this phase costs) from the real Steam install and the
already-built, real client+server+freevgui XBE (`.sweep/game-hl/
default-printf2.xbe`, X7.4/divergence #54's build). Booted in xemu with no
test-only boot injection (divergence #49's `userconfig.cfg` hack is not
part of this disc at all): `verify-image` confirmed identity against the
linker's raw PE output, then the real WON main menu appeared - New Game,
Training Room, Options, Load Game, Multiplayer, Previews, Quit - the first
time this fork has booted a title disc built entirely from the documented,
scriptable pipeline rather than a hand-packed one-off. (The menu's
background is a flat colour rather than the retail art - **cause found
2026-09-16, and it was neither of this row's first guesses**: `XASH_LOW_
MEMORY=1` reaches `uiStatic.lowmemory`, and `CMenuBackgroundBitmap::
LoadBackground` returns before loading anything under it, `controls/
BackgroundBitmap.cpp:351-352`; the grey is its `DRAW_COLOR` default.
Owned by XM's item 0, not by packaging - the tiles are on the disc.)
`op4`/`bshift` are wired into the same
title table (`gearbox`/`bshift` gamedirs, `valve` basedir, real Steam
content confirmed present for both) but untested - no built XBE exists for
either yet (X7.6, not started); the script warns loudly rather than
implying they work.

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
- **Menu art bake (owned by XM item 0, built in this tool). Background
  DONE, 2026-09-16; title and the NV2A profile are NOT built - corrected
  here after review caught this bullet claiming both, still in the
  original planning tense, after the background half actually shipped.**
  A `--bake-menu-art` mode scales the retail background to a 640x480
  canvas, re-tiles it with a rewritten `BackgroundLayout.txt`, and writes
  ordinary TGA files `mainui` already decodes as an overlay for `tools/
  xbox-xiso.sh --menu-bake` - not an archive, since this engine's
  `mainui` loads through `PIC_Load` and `UiPacker`'s other archives have
  no consumer here. Shares `UiPacker`'s layout parser and compositor.
  Measured: a real 0.25 MiB saving, same binary, same content. Not yet
  built: the animated title's own scale-and-drop-mips bake (busts the 3
  MiB ceiling on its own without it - the actual reason this item
  exists), and the NV2A/DXT profile for either asset (X9's to add once it
  exists to consume it; when it lands, the DXT profile inherits the
  block-row rule above and does not swizzle). Full design and arithmetic
  under XM item 0 itself, which is the current source of truth for
  status, not this cross-reference.
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

**FIXED, 2026-09-21** — the Xbox atlas's own DXT1 compression call
(`PakManager.cpp`) has always passed `punchThroughAlpha=false`, meaning
masked (`{`-prefixed - fences, grates, vents) textures have never carried
usable alpha in the baked atlas at all, blocking X9's own planned "native
alpha test" slice on baker work nobody had traced to its real cause. An
X9-I scoping pass (forked research, comparing alpha test against skinning/
dynamic-lighting/aspect-ratio candidates) found this was the actual
blocker, not anything in `ref/nv2a` itself: `WadParser::decodeMiptex`
already writes correct per-pixel alpha for every texture (255 unless a
masked texture's own transparent-key palette index, which gets 0) before
any of it reaches the atlas, and `TextureOpt::compressToDXT`'s own
punch-through logic already decides per 4×4 block from that alpha, not
from an external per-region flag — so the atlas's earlier reasoning ("no
per-region masked tracking exists for a shared, shelf-packed atlas") was
solving a problem the per-pixel design had already made moot.

The first version of the fix was the single-line flip to `true`, verified
by baking the real retail `crossfire.bsp` (6 real masked textures used:
`{blue`, `{fence2`, `{fence3`, `{grate2a`, `{ladder3a`, `{rail1`) both ways
and decoding every BC1 block in both atlases. That verification pass
looked clean at the block-classification level, but `asset-pipeline-
specialist` review didn't stop there — it independently decoded the new
atlas texel-by-texel against the pre-compression RGBA source and found a
real regression the block counts alone couldn't show: **10 texels that
were genuinely opaque in the source got encoded as fully transparent**,
including 2 texels inside a completely unrelated *non-masked* texture.
Root cause: the atlas packer placed each texture's footprint at an
arbitrary (non-4-pixel-aligned) pixel offset, so a texture's edge could
land mid-block alongside whatever the packer had placed next to it —
including, for an unlucky placement, a masked texture's transparent
border bleeding into a neighboring texture's own block.

Fixed in two parts, both verified against the same texel-level decode:

1. **Block-aligned atlas placement** (`PakManager.cpp`, Xbox-target
   branch of `addTexture()`): every texture now reserves a 4-pixel-
   aligned footprint with a 1px border, with edge pixels replicated into
   the border/padding rather than left as whatever the previous
   occupant's leftovers were. This alone took the regression from 10 real
   mismatches down to 6, by construction eliminating any *cross-texture*
   or *cross-void* bleed — no block can straddle two unrelated textures
   or a texture and empty atlas space anymore.
2. **Per-block `blockNeedsTransparency` gating** (`TextureOpt.cpp`,
   `compressToDXT`): rather than passing the caller's blanket
   `punchThroughAlpha` flag straight through to
   `rgbcx::encode_bc1`'s `use_transparent_texels_for_black`, each 4×4
   block is now checked for at least one real (source alpha < 128) texel
   before punch-through mode is even offered to the encoder. This is a
   preventive fix for a *different* sub-case — a block with no genuine
   masking need at all being miscoded anyway — and was confirmed (by
   re-running the same texel-level decode before and after) to leave the
   6 residual mismatches from step 1 untouched, correctly isolating them
   as a separate root cause rather than papering over the same one.

The 6 remaining mismatches (out of 4,194,304 texels checked in this map,
0.00014%) were a distinct root cause from either fix above: a block that
legitimately contains some real transparent texels could still have an
adjacent near-black-but-opaque texel absorbed into the transparent
selector by `rgbcx`'s own block-wide least-squares color fit — the
decision is fundamentally per-block, not per-texel, so no per-texel lift
threshold can guarantee safety on its own (see the comment in
`TextureOpt.cpp::compressToDXT` for the full reasoning).

`tools/codex-review.sh --base main` raised two MUST-FIX-OR-JUSTIFY
findings against this state (round 1), both real:

1. **Atlas overflow was still only a warning, not a bake failure**
   (`PakManager.cpp`). `addTexture()`'s own overflow branch only ever
   printed to stderr and handed the caller a zero-initialized
   `TextureLocation` — `BspParser.cpp` stored that as if packing had
   succeeded, so an overflowed map could still "successfully" bake with
   some faces silently aliased onto atlas slot 0. **Fixed**: `addTexture()`
   now sets a `mAtlasOverflowed` flag, and `save()` checks it first and
   refuses to write, so `main()`'s existing `if (!pak.save(...))` fatal
   path (already there for a failed file open) now also catches this.
   Verified this isn't theoretical: baking the full 125-map retail
   corpus found 2 maps (`disposal`, `rocket_frenzy`) that genuinely
   overflow the 2048×2048 atlas even after this branch's block-alignment
   fix. Checked against `main` with a second worktree build to see
   whether the alignment fix's footprint growth was what tipped them
   over — it wasn't: both already overflowed on unmodified `main` (90
   and 121 warnings respectively, vs. 104 and 143 on this branch) with
   the OLD unaligned packer, so this is a pre-existing atlas-capacity
   limit this branch made slightly worse, not one it introduced. No map
   in the corpus crossed from fitting-before to overflowing-after. The
   other 123 maps bake clean on both. (Both overflowing maps also hit
   an unrelated, already-tracked, pre-existing `PakManager.cpp:214`
   `std::abort()` on vertex count exceeding the u16 index limit before
   reaching this atlas code at all — not in scope here, not caused by
   this branch.)
2. **The `blockNeedsTransparency` gate doesn't guarantee an opaque
   texel can't still land on selector 3 within a block that DOES need
   transparency** (`TextureOpt.cpp`) — exactly the 6-mismatch residual
   above. First response to round 1: a written justification (not a
   fix), reasoning that `rgbcx`'s forced-selector path reaches deep,
   unfamiliar internals that couldn't be trusted without a broad visual
   regression sweep this session had no way to run.

   Round 2 (`tools/codex-review.sh`'s policy cap) re-raised the same
   finding at HIGH severity/confidence 1.0 and rejected that
   justification outright ("the observed percentage does not bound
   other maps... this can produce visible pinholes"). Since this
   session's own toolchain already had a working texel-level
   round-trip verifier (used throughout this fix), the theoretical risk
   named in the justification was actually testable, not just
   arguable — so **fixed for real** instead of re-justifying:
   `TextureOpt.cpp`'s `fixOpaqueTexelsPunchedThrough()` decodes the
   block `compressToDXT` already produced, and for exactly the texels
   that landed on selector 3 despite a real (source) alpha >= 128,
   reassigns them to whichever of the block's own color0/color1/
   midpoint is closest by RGB distance, then re-encodes with
   `rgbcx::encode_bc1`'s `pForce_selectors` pinning that corrected
   selector set. Every already-correct texel (real transparent -> 3,
   real opaque -> non-3) keeps its existing selector untouched, so only
   the specific wrong texels move.

   `pForce_selectors`'s own contract
   (`rgbcx.cpp:1088-1106`/`1128-1148` — `memcpy(sels, pForce_selectors,
   16)`) guarantees the re-encode's selector *bits* equal what's
   forced, regardless of which endpoint candidate wins the search — but
   whether the winning candidate still emits `color0<=color1` (3-color
   mode; the only mode where index 3 means transparent, versus a valid
   opaque blend in 4-color mode) isn't something the library's contract
   promises on its own. Rather than trust that by reading the code, the
   retry's result is decoded again and checked against the exact
   invariant this function exists to guarantee (every real-transparent
   texel decodes to 3, every real-opaque texel doesn't) — the corrected
   block is only ever adopted if verified strictly correct; otherwise
   the pre-retry block (whatever `compressToDXT` already produced) is
   kept unchanged. This makes the pass monotonic: it can fix or no-op,
   never regress, regardless of any rgbcx internal this project hasn't
   independently proven.

   Re-verified with the same texel-level round-trip tool used
   throughout this fix: **0 mismatches** on the same `crossfire.bsp`
   bake (down from 6), and the block classification counts (below) are
   byte-for-byte identical before and after this pass, confirming it
   touched only the specific wrong texels and nothing else.

   asset-pipeline-specialist reviewed this fix in a second pass and
   confirmed it correct by tracing the actual `rgbcx.cpp` internals
   (not just reading the comment): the `rc0<=rc1` re-verification is
   genuinely load-bearing, not defensive paranoia — `bc1_find_sels4`
   (`rgbcx.cpp:1084-1119`) still evaluates a 4-color candidate against
   the forced selector array too, and a 4-color winner is a real,
   reachable outcome (`bc1_encode4` always emits `lc16>hc16`), so the
   retry's mode genuinely isn't guaranteed without checking. The review
   also flagged that "0 mismatches on `crossfire.bsp`" alone was the
   same "doesn't bound other maps" shape of claim codex round 2 had
   already rejected once — the fix's *safety* (fix-or-no-op, never
   regress) is proven structurally regardless of map, but its
   *completeness* wasn't demonstrated beyond one map, and no regression
   test existed to protect this defect class going forward.

   Closed both gaps rather than re-asserting the single-map result:
   added `tools/asset-studio/tests/texture_opt_test.cpp`, a real
   regression test on a 16-texel block found by brute-force random
   search (one real-transparent texel among fifteen varied opaque
   ones) that reproduces the actual defect — confirmed by temporarily
   disabling `fixOpaqueTexelsPunchedThrough` and re-running this exact
   data, which fails without it (texel 9 wrongly decodes transparent)
   and passes with it restored; it now runs on every `ctest` invocation.
   Promoted the ad hoc verification script to `tools/verify-atlas-
   alpha.py`, a real reusable tool with the project's usual `tools/*.py`
   documentation. Then re-ran the full 125-map retail corpus, this time
   decoding and comparing **every texel of every successfully-baked
   map's atlas** against its pre-compression source (not just checking
   bake success/failure as the earlier corpus sweep had) — **121 maps
   checked, 0 opaque-source-but-transparent-decoded mismatches on ANY
   of them**, 4 skipped (the same pre-existing, unrelated
   `contamination`/`disposal`/`pool_party`/`rocket_frenzy` vertex-limit
   and atlas-overflow cases documented above). This is what actually
   answers codex's objection: not a stronger claim about one map, but
   the same check run exhaustively across the whole corpus.

Final verified numbers on the same `crossfire.bsp` bake: 199,517 3-color
(punch-through-capable) blocks, of which 195,661 are fully transparent
(uniform masked-out regions) and 3,856 are genuine mixed
transparent/opaque edge blocks; 62,627 blocks remain 4-color/opaque
(unchanged by the selector-correction pass, as expected — it only ever
reassigns a selector within an already-3-color block, never changes
3-color/4-color classification). `ref/nv2a` itself is unaffected by this
change today (it doesn't read alpha or enable alpha test yet) — this
closes the baker-side half of what blocks that slice, not the slice
itself.

## X9 — `ref/nv2a`

The hardware renderer, replacing X5's software path against a picture already
known to be correct, consuming the format XB decided.

**X9-A (bring-up), 2026-09-20 - the module scaffolding and build/link/
selection wiring are done and verified; the renderer has not yet actually
run, and this is NOT closed as a rendering milestone.** A new `ref/nv2a/`
module (mirroring `ref/soft`'s own structure) builds, isolates and links
cleanly (`tools/xbox-link-check.sh` and `tools/xbox-game-link-check.sh`
both report 0 duplicate/0 undefined with it included). Its intended job -
reusing X4's own already-proven pbkit/VP20/fp20 sequence to draw one fixed
textured triangle every frame, deliberately not touching MPAK geometry,
register combiners beyond the one stage X4 proved, VP20 skinning, or X4's
own still-open swizzled-texture defect - has not been observed happening
even once. Runtime memory reads DID confirm every piece of the static
module/build/link/selection wiring is correct (the renderer name
registers, the static-library table resolves to the real linked
`GetRefAPI`, the function table's `R_Init` entry matches this module's own
compiled address), but `R_Init` itself was never actually entered on the
one live boot attempted, traced to a real test-environment artifact (a
stale `E:`-side `video.cfg` on the shared xemu HDD shadowing the disc's
own copy, per this project's own `fs_rootdir`-before-`fs_rodir` search
order) rather than a defect found in the module. Whether the triangle
actually draws correctly - the thing this slice exists to prove - remains
completely unverified and is the concrete next step, not a formality.
Full account in `divergences.md` #88. (Correction, 2026-09-22: "X4's own
already-proven ... sequence" was itself wrong - X4's checkerboard triangle
never rendered as a checkerboard, only as a smooth two-texel gradient that
was eyeballed as one, because its 0..1 UVs address a LINEAR texture NV2A
samples in texel units; see X9-J below and `divergences.md` #100.)

**X9-B, 2026-09-20 - one real face from a real baked map, coded and
statically verified; not yet seen rendering.** `Mod_ProcessRenderData`
(previously a stub) now parses a real, nv2a-profile `.mpak`'s own baked
vertex/index/`FaceDrawCmd`/DXT-atlas sections (a private mirror of
`filesystem/mpak_layout.c`'s own layout math, validated in Python against
a real bake before being ported to C - the computed offsets landed exactly
on the real file size and read back sane, real geometry), and `R_EndFrame`
draws it with a real look-at/perspective camera and the real DXT1 atlas -
no shader or combiner change needed, X9-A's own transform shader already
does the one matrix-multiply-plus-divide either camera needs. Compiles,
isolates and links cleanly, same bar as X9-A. Not yet observed actually
rendering: a real map's own boot left the guest unresponsive to this
project's debug tooling well past X9-A's own much simpler menu-boot
timings, compounding #88's own already-open `r_refdll` gap - not chased
into a fifth retry, per this project's own "stop and document" rule. One
accepted, documented simplification (a per-vertex, not per-pixel, UV
atlas remap - exact for a non-tiling face, visibly flat instead of
repeating for a tiling one, which the very first real face tested happens
to be). Full account in `divergences.md` #89.

**X9-C, 2026-09-20 - the real lightmap bound and combined with the
diffuse texture, coded and statically verified; not yet seen rendering.**
A second, genuinely new Cg shader pair (`ref/nv2a/vs_lit.vs.cg`/
`ps_lit.ps.cg`, a second texcoord passthrough and a `diffuse × lightmap ×
2` pixel shader) compiled fresh through `cgc`/`vp20compiler`/
`fp20compiler` - the first slice needing a real new shader rather than a
reused `.inl`, and the toolchain was confirmed present and working before
committing to this approach. `nv2a_load_map_geometry()` now reads each
vertex's own lightmap UV into a second UV pair and binds the face's real
RGBA8 lightmap on texture stage 1. Compiles, isolates and links cleanly,
same bar as X9-A/X9-B. Not yet observed rendering - the same live-boot
blockers X9-B already hit apply unchanged and were not re-attempted. One
real mistake this slice's own "compile, don't hand-derive" discipline
caught before it could ship: an earlier round of research had guessed at
the combiner's own register values by reading `nv_regs.h` in isolation:
wrong, in a way the real compiler output (used instead) proved. Full
account in `divergences.md` #90.

**X9-D, 2026-09-20 - scaled from one hard-coded face to a whole map's
real baked geometry, coded and statically verified; not yet seen
rendering.** `nv2a_load_map_geometry()` no longer reads `FaceDrawCmd` at
all - the baker's shared index buffer is already one flat, contiguous
list of every face's own triangles in bake order, so the whole map draws
with no per-face lookup. Every one of the header's own `vCount`/`iCount`
records is now remapped/packed directly into GPU-visible buffers
allocated at their real final size, replacing X9-B/C's own small
fixed-size stack arrays (a real stack-overflow risk at real-map scale -
`crossfire.mpak` alone is 19,480 vertices/34,326 indices), behind a new
upfront memory-budget check that rejects cleanly rather than attempting
a doomed giant allocation. `nv2a_draw_map_geometry()`'s own batched
indexed-draw loop needed no changes at all - written generically from
X9-B onward, this is the first slice to actually need more than one
batch (144, by real cross-check against `crossfire.mpak`). Compiles,
isolates and links cleanly through both `tools/xbox-link-check.sh` and
`tools/xbox-game-link-check.sh hl` (including a full real `hl`
server-module link), same bar as every prior X9 slice. Full account in
`divergences.md` #91.

**Live rendering CONFIRMED, 2026-09-20 (divergences #92/#93) - the first
time in this project's history.** The "engine won't reach a menu at all"
blocker that appeared right after fixing #88's own stale-`video.cfg`
issue turned out to be a testing-methodology gap, not a new engine
defect: every X9 boot attempt had linked the still-stubbed client
module, which `CL_LoadProgs()` correctly, deliberately hard-fails
against (missing required HL SDK exports), halting the console before
either renderer's own frame loop is ever reached again - on both
`ref_soft` and `ref_nv2a` alike, unrelated to anything X9 itself does.
The real fix already existed, already proven in X7.4 step 2
(divergences #41-48): `tools/xbox-game-link-check.sh hl --include-client
--include-freevgui` links the real client against `3rdparty/freevgui`.
Rebuilding X9-D's own `crossfire.mpak` test disc with that combination
produced a real, valid XBE (0 duplicate, 0 undefined) that, booted with
`r_refdll "nv2a"`, shows `nv2a_map_ready == 1` and a `debug_frame_count`
that genuinely advances (10,572 → 16,124 across two live reads) - a
real .mpak parsed, accepted, and drawn, sustained, not a static wire-up
claim. A brightness-boosted screenshot of the running guest shows real,
distinct, differently-tinted polygon silhouettes - genuine per-face
texture/lightmap variation, not a solid placeholder.

**Photometric check, same day (divergence #94) - a real attempt, an
honestly incomplete answer.** The map's own real baked lightmap data at
this fixed camera's own framing vertices is genuinely dim (11-14%
brightness) against a mostly-bright atlas (82% average) - real,
byte-verified, but only proof the SOURCE data is plausible, not that
the renderer reproduces it. A deeper, quantitative check followed:
reproducing the exact camera matrix in Python, projecting every
referenced vertex, and comparing a predicted `diffuse × lightmap × 2`
against the real screenshot at each vertex's own screen position - only
9.7% matched. That result does NOT confirm a bug either: the check has
no real triangle rasterization, so "the nearest vertex landing on a
pixel" often isn't "the true nearest surface at that pixel," and a high
mismatch rate is an expected property of the method itself. The
likeliest concrete bug this mismatch pattern suggested - a depth-test
misconfiguration - was checked directly against `pbkit.c` and ruled
out: `DEPTH_FUNC=LEQUAL`, `DEPTH_WRITE_ENABLE=1`, `DEPTH_TEST_ENABLE`
set once (correctly skipped on later, identical calls - not "forced
every frame" - and never disabled afterward by anything in `ref/nv2a`),
clear value `0xFFFFFF` (far) - a real, correctly-configured setup.
**Genuinely still open**: neither
a pixel-perfect ground truth (real rasterization in the check) nor a
`ref_soft` cross-check of the same view exists yet.

**X9-E, 2026-09-21 - the real, live player camera, replacing the fixed
debug one; coded, math independently verified, live-boot confirmed.**
`GL_RenderFrame` (previously a no-op stub) now builds the real view/
projection matrix every frame from the engine's own real, live
`vieworigin`/`viewangles`/`fov_y` (`ref_viewpass_t`, via the same real
per-frame hook - `engine/client/cl_view.c` - every other ref module
uses), using the engine's own canonical `AngleVectors()` for the camera
axes instead of re-deriving them from an arbitrary eye/target/up hint
the way the old fixed camera did. Filters out the menu's own
playermodel-preview pass (`RF_DRAW_WORLD`), which also calls
`GL_RenderFrame` and would otherwise clobber the real world view. A new
`nv2a_view_valid` guard keeps the draw path falling back to the
already-proven test triangle if a map loads before the first real
frame, rather than ever drawing with a degenerate all-zero matrix.
Verified two ways before trusting it: independently reproducing the
exact matrix chain in Python from the engine's own real, captured
`rvp` values and testing OFF-AXIS points (not just dead ahead, which
codex review correctly flagged as unable to detect a handedness bug -
a point with zero right/up displacement lands at screen center
regardless of axis sign) - points offset along the camera's own real
`right`/`up` land on the geometrically correct side of the screen in
all four directions, sanity-checked against what the OLD, negated
convention would have produced instead; and a live boot showing real,
distinct geometry roughly centered in the frame, a qualitatively more
normal FPS-camera composition than X9-D's own fixed-camera screenshot.
Full account in `divergences.md` #95 -
this closes `divergences.md` #91's own "no real camera/view system"
scope limit; "no visibility/frustum culling" carries forward unchanged,
since the whole map is still submitted every frame regardless of what
this real camera can actually see.

**X9-F, 2026-09-21 - frustum culling, closing the "no visibility/frustum
culling" half of #91/#95's own carried-forward scope limit.** `ref/nv2a`
no longer submits a whole map's entire index buffer every frame
regardless of what the real X9-E camera can see. `nv2a_compute_mpak_layout()`
now captures the `FaceDrawCmds` lump (previously skipped - a perfect,
gap-free per-BSP-face tiling of the index buffer, confirmed against the
real `crossfire.mpak`) into a per-face table built once at map load:
world-space AABBs from the already-remapped vertex positions (the baked
format itself carries none), and each face's own index range snapped
outward to a 6-index/3-dword boundary so it stays addressable in the
packed index buffer without ever shifting a triangle boundary. Validated,
not trusted - a tiling that doesn't check out leaves the table unbuilt
and the draw path degrades to the old, unculled behaviour automatically.
`GL_RenderFrame` now also builds a real 5-plane frustum from the same
forward/right/up/eye its view matrix already uses (a direct port of
`ref/gl/gl_frustum.c`'s own `GL_FrustumInitProj`), and
`nv2a_draw_map_geometry()`'s own batching loop - unchanged in shape - is
now fed by a streaming cull-and-merge pass instead of one call over the
whole buffer. A new `nv2a_r_nocull` convar (mirroring `ref/gl`'s own)
makes the required on/off visual A/B a real toggle. Measured, not
assumed, before committing to the design: a Python prototype against the
real `crossfire.mpak` and the real camera X9-E captured live shows the
spawn view culling to ~35% of the index buffer in 50 runs, and BSP face
order is already spatially coherent enough (19-62 runs at every camera
tried) that no gap-merging heuristic is needed. Full account in
`divergences.md` #96 - PVS is what remains of #91/#95's own scope limit,
and the baked archive already carries real, populated, byte-for-byte
GoldSrc BSP30 visibility/node/leaf/marksurface lumps for it, so that next
slice needs no baker work either.

**X9-G, 2026-09-21 - real PVS culling, closing the last half of #91/#95's
own carried-forward scope limit; coded and statically verified, not yet
live-boot confirmed.** `ref/nv2a` no longer submits every frustum-visible
face regardless of whether a wall stands between it and the camera.
Needed no baker or format work at all, confirming what X9-F's own exit
paragraph predicted: the v7 "classic core" lumps (`docs/r3d/mpak-loading-
design.md`) make a map archive a real, valid BSP30 file, so the engine
already loads a real `mnode_t`/`mleaf_t`/marksurfaces/visdata tree for it
through its completely standard, unmodified `Mod_LoadBmodelLumps` path -
the same tree `ref/gl` and `ref/soft` already read PVS from. A new
function, `nv2a_mark_leaves()`, ports `ref/gl`'s own `R_FindViewLeaf` +
`R_MarkLeaves` (folded into one function, since this module has no
separate per-frame setup pass to host the first half in): finds which
leaf the real, live camera stands in via the engine's own
`gEngfuncs.Mod_PointInLeaf`, including the reference's own crossing-line-
contents merge for a camera sitting on a leaf boundary; fetches the real
PVS row via `gEngfuncs.R_FatPVS`; and marks every face any visible leaf's
own real marksurfaces list references, into a new per-face
`nv2a_map_face_visible[]` table. `nv2a_map_faces[i]` (X9-F's own table)
and the engine's `WORLDMODEL->surfaces[i]` are the same face at the same
index - both are built by walking the archive's one real `LUMP_FACES` in
write order - so a marksurface pointer's offset from `WORLDMODEL->surfaces`
is directly a valid `nv2a_map_faces` index, no translation table needed;
verified by bounds-checking that offset against `WORLDMODEL->numsurfaces`
rather than trusting it blindly, the same discipline this module already
applies to every archive-sourced index. `nv2a_draw_map_geometry()`'s own
streaming cull-and-merge pass (X9-F, unchanged in shape) now skips a face
if it fails EITHER the frustum test or the PVS test. A new
`nv2a_r_novis` convar (mirroring `ref/gl`'s own `r_novis`) makes PVS
culling a real, independent A/B toggle, the same pattern `nv2a_r_nocull`
already established for the frustum test - including porting the
reference's own `FCVAR_CHANGED` handling, so toggling the convar forces
an immediate recompute rather than waiting for the next leaf crossing.
`CHECKVISBIT`/`MAX_MAP_LEAFS`/`FATPVS_RADIUS` are ordinarily engine-
internal (`engine/common/mod_local.h`, which `ref/gl` includes directly
since it shares a build with the engine); `ref/nv2a` is compiled as an
isolated static-lib unit, so these three constants are mirrored locally
under an `NV2A_` prefix rather than pulling in that header's much larger
surface (`edict_t`, `entvars_t`, `studio.h`) for three values - the same
"private, narrowly-scoped mirror" precedent `nv2a_compute_mpak_layout()`
already set for the mpak layout types. Fails open, not closed, at every
new failure point, matching this module's own established discipline:
an allocation failure for the new visibility table leaves it `NULL` and
the draw path treats that exactly like `nv2a_map_faces == NULL` already
means (frustum-only, never a black screen); the table defaults to
all-visible at map load, so the very first frame after a map loads draws
correctly even before `nv2a_mark_leaves()` has run once; a stale viewleaf
pointer from a PREVIOUS map is never dereferenced, because
`nv2a_free_map_resources()` (already called on every unload, before any
new load) now also resets it to `NULL`. Compiles clean
(`tools/xbox-cc.sh`, single file); the standalone
`tools/xbox-ref-isolate.sh nv2a` self-check could not be used to confirm
"0 leaked" the way X9-F's own entry did - re-running it against the
*unmodified* pre-X9-G tree in this same session reproduced a nonzero
leaked-symbol count too, an environment/toolchain discrepancy unrelated
to this change (confirmed identical on both sides of a `git stash`), not
chased further per this project's own "stop and escalate with data"
rule rather than "fresh hypothesis" discipline - filed as an open
question below, not swept under this entry. The real gate,
`tools/xbox-sweep.sh` (203 of 232 files compiling clean, the 29 failures
all other-platform backends - SDL2/win32/vitasdk/dos - unrelated to this
change and expected for an Xbox-scoped sweep) followed by
`tools/xbox-link-check.sh` and `tools/xbox-game-link-check.sh hl
--include-client --include-freevgui`, is recorded in
`docs/r3d/divergences.md` #97 with its own real duplicate/undefined
counts. Not yet live-boot confirmed: no real hardware or xemu run has
observed this slice's own effect (a further reduction in submitted
geometry beyond X9-F's frustum-only numbers, and pixel-identical output
with `nv2a_r_novis` forced on vs. off) the way X9-F's own entry did
end-to-end - that is the concrete next step for this slice, not a
formality, following the same "coded and statically verified, not yet
seen rendering" bar X9-B/C/D each shipped at before a later slice
confirmed them live. Full account in `divergences.md` #97.

**X9-H, 2026-09-21 - backface culling, addressing the "backface culling
(cheap...)" bullet this section already carried forward since X9-F.
NOT closed - single-file compile only, ships disabled by default
(codex review, real finding: shipping this enabled before the real
Xbox link/sweep/boot chain and a live `nv2a_r_nocullback` A/B check
both complete would not match this project's own "compile and boot
before calling anything done" rule - see below and
`divergences.md` #98).** `ref/nv2a`
no longer submits a face that survives the frustum and PVS tests but
faces directly away from the camera. `nv2a_draw_map_geometry()` now
reads the engine's own already-loaded world model's real
`msurface_t::plane`/`::flags` for each baked face - the SAME "same face,
same index" bridge X9-G's PVS code already established and bounds-
checks, reused rather than re-derived - and applies the reference's own
default world-surface cull test verbatim: `dist = PlaneDiff(camera_eye,
plane)`, negated if `SURF_PLANEBACK` is set, culled if `dist <=
BACKFACE_EPSILON`. Confirmed, not assumed, that this is really what the
reference does by default for ordinary world geometry: `ref/gl`'s own
`R_SetupGL(true)` sets `GL_Cull(GL_FRONT)` once per frame before the world
draw runs, and nothing in `gl_rsurf.c` overrides it for world surfaces
(grepped the whole file), so `R_CullSurface`'s `GL_FRONT` branch applies
uniformly - sky surfaces included, since the reference's own
`SURF_DRAWSKY` special-case runs AFTER `R_CullSurface`, not instead of
it. Needed no baker or format work, same reason X9-G needed none: the
real `mplane_t`/`SURF_PLANEBACK` data was already sitting in the engine's
already-loaded world model, just unread by this renderer's own per-face
loop until now. `BACKFACE_EPSILON`/`PlaneDiff()` are used directly,
already in scope; only `SURF_PLANEBACK`'s literal value is mirrored
locally (matching the established precedent for constants this isolated
module can't cheaply pull a whole header in for). A new `nv2a_r_nocullback`
convar (this module's own dev-aid - the reference's own backface culling
is controlled by an internal state variable, not a user convar) matches
the `nv2a_r_nocull`/`nv2a_r_novis` A/B-toggle pattern. Compiles clean
(`tools/xbox-cc.sh`, re-verified after every edit). Specialist review
(`reference-renderer-specialist`, `nv2a-xbox-specialist`,
`upstream-fidelity-specialist`) run per `AGENTS.md`; findings folded in
before this paragraph was written. **Live-boot A/B done, 2026-09-22**:
a real disc, booted live in xemu, with this convar temporarily flipped
enabled showed 0 of 307,200 pixels differing from the shipped baseline
at the real, live camera `GL_RenderFrame` captures every frame (X9-E/
#95, not X9-B/D's earlier fixed camera - an earlier draft of this
entry named the wrong one, corrected per codex-review). Codex-review's
own real finding against this entry's first draft: a pixel-identical
result alone can't rule out the test simply never running - so this
was checked directly, not left as an assumption, by reading the
module's own existing live stats (`nv2a_cull_faces_total/_visible/
_backface_only`): 4385 total faces (exactly `crossfire.mpak`'s own
real count), 299 visible, and **68 real faces the backface test itself
rejected** this frame - genuine, exercised work, not a no-op, and the
image stayed pixel-identical anyway. Real evidence, on the same bar
#96's own `nv2a_r_nocull` A/B used to flip its default - flipping this
one is left to a smaller, dedicated follow-up rather than folded into
this docs-only change. Default stays disabled here. Full account in
`divergences.md` #98.

**X9-I, 2026-09-22 - native alpha test for masked world textures. NOT
closed - statically verified only, ships disabled by default (same
honest gap #97/#98 already carried, not yet a live-boot A/B).** XB's
atlas alpha fix (PR #123) removed the blocker this bullet list named:
masked ('{'-prefixed) textures now bake with real per-pixel alpha, so
`nv2a_draw_map_geometry()` can finally enable NV2A's own fixed-function
alpha-test stage (`NV097_SET_ALPHA_TEST_ENABLE`/`_FUNC`/`_REF`, a real
D3D8-style hardware stage, not a shader trick - fp20 on this GPU has no
per-pixel discard). A real reference-fidelity check (not assumed)
found `ref/gl` and `ref/soft` disagree on the mechanism for masked
WORLD-brush surfaces, and neither is ported verbatim: `ref/gl` only
alpha-tests them via a separate entity+rendermode (`kRenderTransAlpha`)
outside Quake-compat mode, machinery X9 doesn't have yet (no entity
render list at all); `ref/soft` does unconditional per-texel masking
with no entity gate, which is what this slice matches instead - the
cheaper, correct-for-this-module's-actual-scope choice, not a
shortcut taken without checking the alternative. Enabling the test
GLOBALLY per draw call (NV2A has no per-triangle toggle) rather than
bucketing masked/opaque faces into contiguous runs turned out to be
safe without that extra machinery: PR #123's own encoder gate plus its
exhaustive 121-map texel-level corpus verification already proves no
ordinary opaque texel can reach the transparent BC1 selector on any
real retail content this baker has produced.

Two real review rounds, two real fixes, not a rubber stamp either
time. `nv2a-xbox-specialist` found two comment overclaims (fixed - an
unstated combiner-alpha doubling the first version of this comment
never mentioned, and "verified atlas-wide" overstating what an
empirical 121/125-map check actually proves) and one real dangling-
state gap (fixed - the test-triangle fallback path never reset the
new alpha-test registers, inert today only by coincidence). Then
`tools/codex-review.sh` round 1 found a deeper, genuinely different
bug in the SAME area the specialist had just touched: the diffuse
atlas samples through real bilinear filtering (`NV097_SET_TEXTURE_
FILTER`), so a fragment at a masked cutout's own edge gets a real,
non-binary alpha - by design, that's what lets a filtered alpha test
antialias a cutout edge - but the shader's own alpha-doubling (fixed,
not removed, by the specialist round) meant that value got compared
against `NV097_SET_ALPHA_REF=0x80` after being doubled, silently
moving the real pass/fail boundary to 25% opacity instead of the
intended 50%. Fixed at the actual source, `ps_lit.ps.cg`, not by
picking a compensating reference constant: color and alpha are no
longer one shared `float4*float4*2.0f` expression - RGB keeps its
GoldSrc-convention `×2`, alpha passes through the diffuse texture's
own real, filtered value untouched, recompiled through the real
`cgc -profile fp20` / `fp20compiler` toolchain (X9-C's own precedent:
compile, don't hand-derive) and confirmed in the actual compiled
combiner output, not assumed (`out.a = unsigned(tex0.a)`, no
`scale_by_two()` on that channel, where the pre-fix version read a
doubled A×B product). Full static chain clean: `tools/xbox-cc.sh`, `tools/xbox-ref-isolate.sh`
(5 objects, 99 symbols renamed, 0 leaked), `tools/xbox-link-check.sh`
and `tools/xbox-game-link-check.sh hl --include-client
--include-freevgui` (0 duplicate, 0 undefined, real valid XBE with the
real hl server+client+freevgui modules). `nv2a_r_noalphatest` ships
`"1"` (disabled), matching `nv2a_r_nocullback`'s own precedent.
**Booted live, 2026-09-22** (same real disc/boot infrastructure
as X9-H's own entry above, first real live boot this session - one
genuine, previously-undocumented bug found and fixed along the way,
see `lessons.md`'s new entry on `.mpak` placement): with this convar
temporarily flipped enabled, 0 of 307,200 pixels differed from the
shipped baseline at the real, live camera `GL_RenderFrame` captures
every frame (X9-E/#95) - no crash, no corruption. Codex-review
correctly rejected calling this "A/B done" though: unlike X9-H's own
backface-culling check (which has a real live counter proving the test
actively culled 68 real faces), alpha test has no equivalent
software-side counter, and crossfire.bsp's own masked textures weren't
confirmed to be in this specific camera's frame at all - so this boot
proves the feature doesn't crash or corrupt anything, not that its
actual cutout behavior was ever exercised. Default stays disabled.
Full account in `divergences.md` #99.

**X9-J, 2026-09-22 - the map finally renders lit AND textured; two
texture-setup bugs that had masked each other since X9-B/X9-C.** Every
real map (c1a0, crossfire) had rendered near-black and perfectly
monochrome since X9-B - screenshot mean ~1.8/255, pixel values only
{0,1,2}, at most 1/255 of channel difference - and every session from
#92 onward had brightness-boosted its screenshots and moved on, with
#94's own 9.7% photometric match filed as inconclusive. Not one bug but
two, each hiding the other, found by refusing to debug the product:
a temporary lightmap-only debug pixel shader showed the lightmap term
took exactly two values across the whole frame (0 on 254,979 px, 1 on
50,263 px), and a diffuse-only one showed the atlas term was one flat
colour - both texture-addressing statements, not lighting ones. Bug 1:
the lightmap page is bound LINEAR (`LU_IMAGE_A8B8G8R8` + pitch +
`IMAGE_RECT`), and NV2A addresses linear textures in unnormalized texel
units, like GL rectangle textures (xemu `psh.c` normalizes only
`f.linear` stages by `textureSize()`; the nxdk mesh sample this module's
stage registers were copied from feeds integer UVs such as `{44,143}` to
its 256x256 linear texture), while the remap loop copied the baker's
`lu/lv / LM_PAGE_SIZE` verbatim - so clamp + bilinear only ever blended
page texels (0,0) and (1,0), the ramp strip's own 0 and 1. The live
vertex buffer's `uv1` and the baked luxels at exactly those texels were
verified correct (mean 105/255) BEFORE blaming the GPU: the data was
right, the addressing wrong. Bug 2: the DXT1 atlas was bound with the
same linear pattern and no `BASE_SIZE_U/V`, and NV2A sizes compressed
textures from those log2 fields alone (`IMAGE_RECT`/pitch size linear
textures only; xemu `texture.c` forces `width = 1 << log_width, pitch =
0` for non-linear formats; the XDK documents the same bits as "only set
for swizzled or compressed", preserved in Cxbx-Reloaded
`src/core/hle/D3D8/XbD3D8Types.h:565-570` - the swizzled half of that
sentence is not relied on here, X4's `SZ_` defect being still open) - a
2^0 x 2^0 texture, so every fragment read the first BC1 block's texel (0,0),
(239,255,214) pale green-white. The original {0,1,2} is exactly that
colour x {0,1}/255 x the combiner's x2; with only the lightmap fixed
the frame was lit but flat (mean 228/255, R/G/B 218/240/226); with both,
textured and lit at sane exposure - mean 90.4/255, ~251K px with real
colour spread. Both bugs trace to the same wrong baseline: X4's linear
path was never "proven" - its 0..1 UVs on an 8x8 linear checkerboard
produced the smooth orange-to-white gradient every X4/X9-A screenshot
shows, a two-texel blend misread as success, and each later slice
copied a stage setup with a green checkmark that was never earned.
Final shape (`ref/nv2a/r_context.c` only, no baker/format/shader
change): atlas log2 dimensions validated power-of-two <= 4096 at map
load and stored in two statics rather than recomputed per frame
(fails closed to the test triangle with a console diagnostic);
`BASE_SIZE_U/V` OR'd into stage 0's FORMAT word (pushed `0x0BB10C22`
for 2048x2048 - confirmed by `xmex` disassembly of BOTH the diagnostic
build's per-frame log2 loop and the final load-time-statics build's
XBE, the two producing the same word as claimed; the final build was
also booted live under xemu and its `c1a0` screenshot is pixel-identical
to the already-verified diagnostic build's, 0 of 307,200 pixels
differing); `uv1 = lu *
lightmapWidth, lv * lightmapHeight` from the header fields stage 1 also
pushes to `IMAGE_RECT`, and because that scale is only the correct
inverse of the baker's fixed 1024-luxel `LM_PAGE_SIZE` divisor - a codex
review round caught that a same-but-wrong-size square page (checked only
`lightmapWidth == lightmapHeight`, not against the actual constant)
would pass that weaker check yet still silently mis-scale every luxel -
the load-time check now requires `lightmapWidth == lightmapHeight ==
1024` exactly, rather than scrambling rows silently the way the
deferred trim-to-usedHeight bake optimisation would; the atlas's
pitch/`IMAGE_RECT` pushes kept on purpose (ignored
for a compressed format, but the exact sequence the live measurement
used, and what nxdk_pgraph_tests - hardware-captured goldens - push
too); the bring-up triangle's UVs moved to texel units and, booted live, no
longer show the old 0..1-UV gradient - but under both the mesh sample's
convolution filter constant and a plain-bilinear one they still rendered
as a smooth orange/white blend rather than a crisp checkerboard; booted
a third time with a NEAREST filter (bring-up path only, the real map's
stages unchanged) the triangle renders a real, crisp 8x8 checkerboard,
confirming the blend was bilinear filtering of a single-texel-period
pattern, not a remaining UV bug, and making NEAREST this canary's
permanent, more sensitive filter; the
file header's "linear path X4 already proved correct"
claim corrected. Ruled out with live evidence so they
are not re-chased: baked-data corruption (GPU copies byte-identical to
the `.mpak`), attribute binding, the vertex program, fog/dither/alpha
test, the final combiner. Still real, still open: both conventions are
verified against xemu 0.8.136's source and corroborated by nxdk's mesh
sample, nxdk_pgraph_tests' hardware-captured goldens and Cxbx-Reloaded's
XDK-derived headers, but not measured on a physical Xbox by this
project; `FILTER 0x04074000` decodes to a 3x3 Gaussian convolution
filter per the register enums (MIN=7/MAG=4, `CONVOLUTION_2D_LOD0`), but
xemu maps that enum to plain `GL_LINEAR` (a `TODO: unimplemented`
comment in its own constant tables), so what hardware does with it is
unobserved; swapping it for a plain-bilinear constant (`0x02020000`,
`TENT_LOD0` both ways) on the test triangle, live, produced the same
smooth blend, ruling the filter constant's specific decode out as the
checkerboard-vs-blend cause on xemu; a NEAREST filter (`0x01010000`),
live, renders a real crisp checkerboard, confirming the blend under
either prior filter was bilinear/tent filtering of a single-texel-period
pattern - unrelated to the UV fix - and this bring-up path now ships
with NEAREST as its permanent, more sensitive canary filter, unrelated
to `nv2a_draw_map_geometry`'s own unchanged `0x04074000`; multi-page
lightmaps now need a
per-page V offset, not a scale; X4's swizzled-path defect is still open
and was judged against this same wrong baseline. Full account in
`divergences.md` #100, which also closes #94 and annotates #89-#93.

**X9-K, 2026-09-22 - the per-map diffuse atlas is gone: every texture draws
with NV2A's own native hardware WRAP addressing (MPAK v9).** X9-B's atlas +
per-vertex `rect[0]+frac(u)*rect[2]` remap (X9-J's own entry above) worked
for a face whose UVs fit one texture repeat, but `frac()` collapses every
repeat past the first onto the same fractional value - unacceptable on
ordinary tiling floors/walls, and subdividing tiled faces at bake time to
fix it overflows the u16 vertex-index limit on large outdoor maps
(crossfire's worst face alone needed ~22,560 tiles). User-supplied
architectural decision: drop the shared atlas, independently resample each
source texture to a power-of-two per axis at bake time, and address it with
NV2A's own native WRAP mode - no atlas, no remap, tiling handled by the GPU
for free. `MpakHeader6`/`mpak_header_t` move to v9: `atlasWidth`/
`atlasHeight`/`lenAtlas` are replaced by a texture directory
(`MpakTextureInfo{width,height,log2Width,log2Height,dataLength}`, POT
dimensions rounded UP - never down, so no source texture silently loses
resolution - clamped to NV2A's real 4096px edge with a bake-time diagnostic
rather than a silent clamp) and a batch table
(`MpakTextureBatch{textureIndex,startFace,faceCount,startIndex,indexCount}`,
one per texture at least one face actually uses - NOT one per directory
entry: unreferenced miptexs still get baked, real measured waste 15-18% of
both test maps' texture-data bytes, a real inefficiency left for a later
pass, not a budget risk). `BspParser.cpp`'s face loop now `stable_sort`s
every face by `(textureIndex, pvsCluster)` before emission, so a texture's
faces land in one contiguous run; `outVert.u/v` (`rawU/rawV` divided by the
source's NATIVE pre-resample width/height, unchanged since X9-B) already
were the exact raw/repeating UV format WRAP addressing needs - normalizing
by native size keeps the UV space resolution-independent of the later POT
resample, the same separation ref/gl's own `GL_ResampleTexture` +
normalized-UV combination relies on (reference-renderer-specialist review:
specifically ref/gl's `GL_ARB_TEXTURE_NPOT_EXT`-unsupported fallback path,
`gl_image.c:467` - ref/gl's default path never resamples world textures at
all, since real GPUs support NPOT; NV2A has no NPOT support to fall back
FROM, so this port hits that path unconditionally).

The one genuinely hard correctness problem: X9-G's PVS visibility system
(`nv2a_mark_leaves()`) computes its face index from `world->surfaces - surf`
- a pointer-difference into the ENGINE's own classic model, built from the
raw passthrough BSP lumps in ORIGINAL face order, completely independent of
any mpak-side sort. Sorting faces by texture ID breaks the identity that
index used to have with the archive's own face order. Fixed with a new
`originalToSortedFaceIndex[]` lump (the sort's inverse permutation) and one
new translation step in `nv2a_mark_leaves()` before it writes
`nv2a_map_face_visible` - double bounds-checked (against the original count
for the raw index, the sorted count for the translated one) and fails OPEN
(all-visible, frustum-only) rather than mis-culling if the table is ever
missing or malformed. The same original-vs-sorted gap existed one level
down in the backface-cull test too (`nv2a_draw_map_geometry`'s own
`world->surfaces[i]` read) - caught and fixed with a second, locally-derived
inverse array, even though that path defaults off (`nv2a_r_nocullback`
default `"1"`) and so wasn't live by default.

Implemented via a Workflow (parallel baker + renderer implementers against
one pinned v9 struct contract, five specialist reviewers, a fix round, a
static gate) - and it earned its own adversarial review round: three of the
five specialists independently found a real, live-by-default rendering bug
neither implementer's own report mentioned - `nv2a_map_face_t::dword_begin/
dword_end` (and the no-per-face-table batch fallback) round each face's own
index range OUTWARD to a 3-dword/6-index boundary, a no-op when the whole
map shared one texture (X9-B..X9-J) but a real, systemic mistextured seam
once a DIFFERENT texture is bound per batch (X9-K) - almost no batch
boundary naturally lands dword-aligned, so the first/last triangle of
nearly every batch could draw under the WRONG (neighbouring) bound texture,
and one direction of the mis-round doesn't self-correct (draws AFTER, and
so permanently overwrites, the correct triangle). Fixed at the baker:
`BspParser.cpp`'s pass 2 now pads each batch's own contributed index run to
an exact multiple of 6 with one degenerate zero-area triangle, absorbed
into the last real face's own `FaceDrawCmd.index_count` so the existing
gapless-partition invariant still holds - since batches are contiguous by
construction, padding every batch's end also aligns the next batch's start.
A second, separate finding (console-performance-specialist): the per-batch
loop rebound stage 0's full 7-register state UNCONDITIONALLY for every
batch before checking whether any of that batch's own faces survive
frustum+PVS culling - a guaranteed ~100-130 texture-stage reprograms a
frame regardless of camera position, undercutting the X9-F/G/H culling work
already merged. Fixed by binding lazily, only once a batch is known to have
a real, visible draw range. A third, structural finding (three reviewers
independently): `filesystem/mpak_layout.c` - the real, production C reader
`mpak_layout.h`'s own comment says must move in lockstep with the header -
was never updated, a hard build break in the actual engine target; fixed,
with its unit test rewritten to build and check a real v9 archive.

Live-confirmed under xemu on both real test maps, at the exact debug camera
pose X9-J's own screenshots used: `c1a0` (Black Mesa Inbound's reception/
corridor) mean 86.3/255, 247,900 px with channel spread > 8 (X9-J's fix:
90.4/255 - the small delta is expected, real texture content shifting under
POT resampling, not a regression); `crossfire` - the map that made the old
atlas+subdivision approach mathematically impossible (u16 index overflow) -
mean 114.9/255, 232,909 px with channel spread > 8, its large outdoor
ground/fence textures now tiling correctly instead of stretching. Both
re-baked clean at MPAKv9 with real, measured headroom against every
fixed-capacity cap this slice added: `NV2A_MAP_MAX_TEXTURES`/
`NV2A_MAP_MAX_BATCHES` (both 512) at 164/129 (c1a0) and 120/91 (crossfire) -
32%/25% at most; `NV2A_MAP_BUDGET_BYTES` (24 MiB) at ~58-59%; total
diffuse-texture GPU memory actually DROPPED versus the old fixed 2 MiB
atlas (real per-texture totals ~535-537 KiB on both maps, a ~74% reduction,
because a map's real diffuse footprint was always well under the atlas's
own fixed worst-case size). Two bake-time safety nets the old atlas already
had were missing for v9 and are added: a texture exceeding NV2A's 4096px
edge now fails the bake loudly (`isNativeSizeRepresentable()`) instead of
being silently clamped, and unique-texture/batch counts are checked against
the renderer's own 512 caps at bake time (a duplicated, cross-referenced
literal - no shared header exists between the C++ baker and the C engine
for this constant) instead of only failing on the console at load time.

Two more real issues surfaced only by a direct live comparison against
`ref_soft` on `crossfire` (the outdoor map, where they actually show), after
the Workflow itself had already reported a static gate pass - a reminder
that structural verification (index alignment, byte-exact archives) does
not substitute for actually looking at the rendered picture. First:
`ref/nv2a` has never read `MpakFaceInfo::flags` (SURF_DRAWSKY), baked since
v6 but unused by this renderer - sky faces always drew as ordinary geometry
with the WAD's literal "sky" placeholder texture, invisible as a defect
under the old atlas+remap (one flat stretched patch) but glaringly obvious
under correct native WRAP tiling (a dense pattern filling the sky). Fixed:
`nv2a_load_map_geometry()` now reads that lump and every sky-flagged face
is excluded from every draw run, unconditionally. Second, found while
investigating the first: `NV097_SET_TEXTURE_FORMAT_MIPMAP_LEVELS` has been
`1` (no mip chain) in this file's entire history, confirmed unchanged
before this session - the old atlas+remap's flat-stretch behaviour
accidentally hid the total absence of mipmapping; correct WRAP tiling of a
small texture across a distant/oblique face (crossfire's outdoor
scaffolding) now aliases into a busy moire pattern with nothing to filter
between mip levels. Ruled out as a data or addressing bug first, not
assumed: all 120 of crossfire's baked textures were independently decoded
straight from the archive bytes and confirmed pixel-correct (every sign
fully legible), and WRAP vs CLAMP was A/B tested live with no visible
difference. A real mip chain is real, substantial, separate work - not
bundled into this change; tracked as its own follow-up, user-confirmed
scope decision (`divergences.md` #101, item (g)).

Full account, evidence and every specialist finding (including the ones
deliberately deferred - `nearestPowerOfTwo()`'s round-up-vs-nearest choice,
now resolved to always-round-up; `tools/verify-atlas-alpha.py`'s new v9
staleness; `UiPacker.cpp`'s unrelated menu-background archive silently
inheriting `version=9`; the sky-skip and mip-aliasing findings above) in
`divergences.md` #101.

**X9-L, 2026-09-23 - first run on a real Original Xbox; texture state
fixed.** The first hardware run halted on a texture filter value xemu
accepts (`0x01010000`, zero convolution-kernel field). Once that was fixed,
the hardware showed X9-K's exact xemu corruption, which proved the
corruption was ours, not an xemu bug. Root cause: no texture FORMAT word set
`BORDER_SOURCE_COLOR`, so every texture sampled at (u/2, v/2). Also fixed:
the nxdk sample's convolution filter (`0x04074000`) was replaced with plain
bilinear. The crossfire spawn now matches `ref_soft`'s look in xemu and on
hardware. The bring-up test triangle had the same half-coordinate sampling
and is fixed by the same bit: it now renders correctly on hardware too.
Full account in `divergences.md` #102; the method is in `lessons.md`.

Open items that X9-L surfaced or carried forward, in suggested order:

1. ~~**`nv2a_mark_leaves()` hides visible world faces.**~~ **Done,
   X9-M (`divergences.md` #103): the PVS code was right.** The hidden
   faces belonged to brush entities (doors, ladders, fences), and
   `ref/nv2a` had no entity pass. It now draws brush entities the way
   ref/gl does. Left from that work: `kRenderTransColor` entities, and
   studio models, sprites and beams, which still have no path.
2. ~~**Mipmaps.**~~ **Done, X9-N (`divergences.md` #105, MPAK v10):**
   full DXT1 mip chains and trilinear filtering. ~~Check the v10
   texture-directory rule (chain lengths, 128-byte padding) in the
   baker's verify pass and the PC tests, not only in `ref/nv2a`~~ **Done:**
   `filesystem/mpak_texture_dir.h` is now the one shared rule
   (`Mpak_ValidateTextureDirectory`), header-only so `ref/nv2a` (which
   cannot link `filesystem/mpak_layout.c`), `filesystem/tests/mpak_layout.c`
   and `tools/asset-studio`'s `PakManager::verifyFile` all run the literal
   same comparisons instead of `ref/nv2a` keeping its own private copy.
   `ref/nv2a` still passes `data = NULL` (no padding-CONTENT check at Xbox
   load time - NV2A never samples padding, so the console's accept/reject
   set is unchanged); the PC tests and the baker's verify pass both check
   it for real. `buildDxt1MipChain` also gained direct unit tests
   (`tools/asset-studio/tests/dxt1_mip_chain_test.cpp`), moved out of
   `PakManager.cpp`'s anonymous namespace with no behaviour change to make
   that possible. Left from the original X9-N work:
   - confirm the level layout on real hardware with a one-colour-per-level
     debug bake (xemu is not proof);
   - ~~add a bake-time mirror of the renderer's 24 MiB per-map budget, and
     measure the 125-map corpus against it~~ **Done, X9-Q (#135):**
     `PakManager::save()` now fails the bake instead of shipping an archive
     that would silently fall back to the bring-up triangle on the
     console. 11 of 125 retail maps fail: 7 genuinely exceed the 24 MiB
     ledger (worst, `boot_camp`, at 110.2%), and 4 hit the long-standing
     u16 vertex-count limit (`PakManager::addVertex`'s "vertex count
     exceeded 65536"), unchanged by v10, before ever reaching the ledger
     check. Full corpus table below. The 7 over-budget maps are an open
     item (see below).
   - if distant fences or grates vanish too early, use coverage-preserving
     alpha scaling for masked mips;
   - ~~revive `tools/verify-atlas-alpha.py` as a per-texture, per-level
     check.~~ **Done, X9-Q (`divergences.md` #101 (b), #99's dated
     follow-up):** rewritten for v10's texture directory and per-level
     mip chains, paired with a new `R3D_DUMP_TEXTURE_RGBA` baker hook.
     0 mismatches across 121 of the 125 retail maps (the other 4 fail to
     bake for the same unrelated, already-tracked u16 vertex-index limit;
     `CHANGELOG.md`, `divergences.md` #99).
3. ~~**`nv2a_r_nocull 1` loses the xemu Vulkan device.**~~ **Done, X9-O
   (`divergences.md` #107):** the setting now matches ref/gl's `r_nocull`
   instead of switching to the whole-batch fallback, which drew triggers
   and frozen doors. The device loss did not reproduce. A zero clip-space
   w in the vertex shader's divide is still a possible inf/NaN source.
4. ~~**Bring-up test triangle's `vs.inl` writes only `.xy`.**~~ **Done,
   X9-P:** `ref/nv2a/vs.vs.cg` (the X4 source, which reproduced the old
   `vs.inl` byte for byte) now writes q = 1, with `c[4]` uploaded.
5. ~~**Stage 1 stays enabled when only stage 0 is used.**~~ **Done, X9-P:**
   the test triangle sets stage 1's `TEXTURE_CONTROL0` to 0.
6. ~~Carried from X9-K: `tools/verify-atlas-alpha.py` is stale for v9, and
   `UiPacker.cpp`'s menu archive inherits `version=9`.~~ **Done, X9-Q
   (`divergences.md` #101 (b),(c)):** the tool is fixed above (item 2);
   `UiPacker::packMenuBackground` now writes its own
   `MPAK_UI_BACKGROUND_VERSION` instead of inheriting whatever the map
   format's `version` default happens to be.
7. **Real-hardware check, by default.** Every `ref/nv2a` phase ends with a
   hardware boot (the FTP deploy plus back-buffer dump recipe in
   `lessons.md`). xemu accepted two register values real hardware rejects or
   renders differently.

### X9-Q — bake-time 24 MiB budget check, 125-map corpus

`PakManager::save()` now mirrors `nv2a_load_map_geometry`'s own per-map
memory ledger (`NV2A_MAP_BUDGET_BYTES` and the `total_bytes` sum with its
strict `>` compare, `ref/nv2a/r_context.c`) at bake time, for the
same reason the `NV2A_MAP_MAX_TEXTURES`/`NV2A_MAP_MAX_BATCHES` checks
already exist there: a map that does not fit must break the build, not
silently fail to draw its real geometry on the console. `Nv2aMapBudget.h`
holds the duplicated constant and formula, cross-referenced back to
`r_context.c` the same way the two 512 caps already are. On failure the
baker prints the ERROR line and the byte breakdown, then deletes the
rejected archive so no later stage or re-stage can pick up a failed bake
as if it were current.

Measured against the full 125-map retail corpus (`--target xbox --profile
nv2a`, one `AssetStudio` invocation per map):

- **121 of 125 maps produce a ledger figure**; the other 4
  (`contamination`, `disposal`, `pool_party`, `rocket_frenzy`) hit the
  unrelated, already-tracked u16 vertex-index limit (`PakManager::addVertex`)
  before the code ever reaches the budget check. These are the same four
  long-standing vertex-count failures recorded in the X9-K entry above;
  the limit counts vertices, not bytes per vertex, so v10 did not change it.
  After this gate, **114 of 125** retail maps bake for the Xbox target.
- **7 of the 121 genuinely exceed the 24 MiB cap** and fail the bake:
  `boot_camp` (110.2%), `rustmill` (104.1%), `t0a0b` (104.0%), `c2a4e`
  (102.6%), `c2a2e` (101.9%), `c2a4c` (101.1%), `t0a0a` (100.2%). All
  seven are large, multi-lightmap-page maps (2 lightmap layers, not 1) —
  the lightmap atlas term alone is 8,388,608 bytes on each, over a third
  of the whole 24 MiB cap before a single vertex or texture byte is
  counted, and it counts twice (once in the loaded archive, once in its
  contiguous GPU copy). Measured on `boot_camp`, `t0a0a` and `c1a0`
  (2026-09-24): the atlas's fixed 1024x1024 RGBA8 pages hold only **5-7%**
  real luxels (the BSP lighting lump's luxel count against the pages'
  capacity). Fitting the atlas to its content is the fix - see open item 8
  below.
- **Median across the 121 computed ledgers: 14,646,382 bytes (58.2%
  of budget).**
- `c1a0` (58.2%) and `crossfire` (59.6%) land within a point of X9-K's own
  v9 figure of ~58-59% (`fork-plan.md:8142`) — the mip-chain growth in
  `textureData` this slice's own X9-N added is real but small next to the
  archive's total on-disk size, which dominates the ledger for almost
  every map (see "Largest term" below).
- For all but a handful of maps the ledger's own single largest term is
  `file` — the whole archive's on-disk size, which is also the last term
  in the ref's own sum (`buffersize`, `r_context.c nv2a_load_map_geometry`). This is
  expected, not a formula bug: `file` already includes the raw BSP lumps
  (visibility, nodes, leaves, planes, clipnodes, lighting, miptex,
  entities) that the other seven terms never touch, so it is the largest
  single number in the sum for nearly every map in the corpus.

Full 125-row table, sorted by ledger bytes descending (the real max row,
not a truncated view — `lessons.md`'s "NEVER read a truncated view of
sorted or ranked output" entry):

| Map | Status | Ledger bytes | % of 24 MiB | Largest term |
|---|---|---:|---:|---|
| boot_camp | **OVER BUDGET** | 27725074 | 110.2% | file=16494592 |
| rustmill | **OVER BUDGET** | 26197933 | 104.1% | file=15376384 |
| t0a0b | **OVER BUDGET** | 26169415 | 104.0% | file=15435776 |
| c2a4e | **OVER BUDGET** | 25814747 | 102.6% | file=15028224 |
| c2a2e | **OVER BUDGET** | 25638675 | 101.9% | file=15022080 |
| c2a4c | **OVER BUDGET** | 25443799 | 101.1% | file=14813184 |
| t0a0a | **OVER BUDGET** | 25228101 | 100.2% | file=14612480 |
| c5a1 | OK | 24796110 | 98.5% | file=14508032 |
| c2a2a | OK | 24734343 | 98.3% | file=14346240 |
| t0a0 | OK | 24515908 | 97.4% | file=14133248 |
| c3a2a | OK | 24321134 | 96.6% | file=14032896 |
| c2a5e | OK | 18244113 | 72.5% | file=11472896 |
| c2a5f | OK | 17372888 | 69.0% | file=10887168 |
| doublecross | OK | 17036080 | 67.7% | file=10582016 |
| c2a1a | OK | 17024774 | 67.7% | file=10588160 |
| c1a1f | OK | 16883729 | 67.1% | file=10543104 |
| c3a1 | OK | 16690457 | 66.3% | file=10346496 |
| c2a2b1 | OK | 16670463 | 66.2% | file=10352640 |
| c3a1a | OK | 16607152 | 66.0% | file=10186752 |
| hldemo2 | OK | 16520215 | 65.6% | file=10223616 |
| c1a2b | OK | 16234821 | 64.5% | file=10037248 |
| c2a2d | OK | 16231114 | 64.5% | file=10098688 |
| c2a4b | OK | 16212311 | 64.4% | file=10082304 |
| c3a2c | OK | 16154472 | 64.2% | file=9938944 |
| c1a2 | OK | 16124733 | 64.1% | file=9984000 |
| c1a1c | OK | 16016265 | 63.6% | file=9902080 |
| lambda_bunker | OK | 15914136 | 63.2% | file=9709568 |
| c0a0b | OK | 15806847 | 62.8% | file=9682944 |
| c1a2a | OK | 15750483 | 62.6% | file=9662464 |
| c1a3 | OK | 15723085 | 62.5% | file=9654272 |
| c2a3b | OK | 15660577 | 62.2% | file=9617408 |
| c2a4f | OK | 15634850 | 62.1% | file=9533440 |
| c2a2g | OK | 15567736 | 61.9% | file=9512960 |
| c1a1b | OK | 15491172 | 61.6% | file=9582592 |
| c2a2c | OK | 15430555 | 61.3% | file=9441280 |
| c2a4a | OK | 15418662 | 61.3% | file=9431040 |
| c1a4d | OK | 15405727 | 61.2% | file=9449472 |
| c1a4f | OK | 15365717 | 61.1% | file=9492480 |
| c2a1 | OK | 15344582 | 61.0% | file=9414656 |
| c1a0c | OK | 15274418 | 60.7% | file=9316352 |
| c1a1 | OK | 15257045 | 60.6% | file=9242624 |
| c2a2f | OK | 15256132 | 60.6% | file=9332736 |
| c2a3a | OK | 15240525 | 60.6% | file=9236480 |
| c2a2b2 | OK | 15198996 | 60.4% | file=9273344 |
| c0a0c | OK | 15186856 | 60.3% | file=9390080 |
| c1a0d | OK | 15167381 | 60.3% | file=9189376 |
| subtransit | OK | 15166450 | 60.3% | file=9256960 |
| c1a1a | OK | 15165268 | 60.3% | file=9177088 |
| c1a0b | OK | 15136107 | 60.1% | file=9183232 |
| c2a2h | OK | 15108434 | 60.0% | file=9164800 |
| c4a3 | OK | 15089790 | 60.0% | file=9295872 |
| c1a4g | OK | 15043170 | 59.8% | file=9279488 |
| crossfire | OK | 14998801 | 59.6% | file=9070592 |
| c2a3c | OK | 14849507 | 59.0% | file=8994816 |
| c1a4i | OK | 14842375 | 59.0% | file=9136128 |
| rapidcore | OK | 14698480 | 58.4% | file=8910848 |
| c3a1b | OK | 14679640 | 58.3% | file=8876032 |
| c2a3 | OK | 14665586 | 58.3% | file=8916992 |
| c2a5b | OK | 14662030 | 58.3% | file=8781824 |
| c1a0 | OK | 14652931 | 58.2% | file=8822784 |
| c1a0a | OK | 14646382 | 58.2% | file=8757248 |
| t0a0b1 | OK | 14608408 | 58.0% | file=8833024 |
| c1a3a | OK | 14484535 | 57.6% | file=8724480 |
| c1a4b | OK | 14463030 | 57.5% | file=8763392 |
| c2a5 | OK | 14320513 | 56.9% | file=8650752 |
| hldemo3 | OK | 14311351 | 56.9% | file=8613888 |
| c1a0e | OK | 14302760 | 56.8% | file=8736768 |
| c2a5g | OK | 14229930 | 56.5% | file=8523776 |
| hldemo1 | OK | 14199694 | 56.4% | file=8548352 |
| t0a0c | OK | 14178236 | 56.3% | file=8462336 |
| c0a0d | OK | 14113942 | 56.1% | file=8462336 |
| datacore | OK | 14102934 | 56.0% | file=8413184 |
| gasworks | OK | 14099585 | 56.0% | file=8345600 |
| c3a2d | OK | 14070803 | 55.9% | file=8448000 |
| c1a4 | OK | 13944275 | 55.4% | file=8445952 |
| bounce | OK | 13853447 | 55.0% | file=8241152 |
| c2a3d | OK | 13852410 | 55.0% | file=8284160 |
| c0a0a | OK | 13849660 | 55.0% | file=8278016 |
| c4a1b | OK | 13812950 | 54.9% | file=8335360 |
| c0a0 | OK | 13711774 | 54.5% | file=8183808 |
| c3a2e | OK | 13708489 | 54.5% | file=8134656 |
| c2a4d | OK | 13651411 | 54.2% | file=8032256 |
| t0a0b2 | OK | 13395650 | 53.2% | file=7927808 |
| c2a5c | OK | 13213172 | 52.5% | file=7770112 |
| undertow | OK | 13122161 | 52.1% | file=7714816 |
| c4a1a | OK | 13120357 | 52.1% | file=7759872 |
| c3a2 | OK | 13026933 | 51.8% | file=7569408 |
| c2a5w | OK | 12916845 | 51.3% | file=7561216 |
| c3a2b | OK | 12883931 | 51.2% | file=7593984 |
| c4a1d | OK | 12852344 | 51.1% | file=7557120 |
| c1a2c | OK | 12780112 | 50.8% | file=7477248 |
| c2a5d | OK | 12747781 | 50.7% | file=7436288 |
| t0a0d | OK | 12715279 | 50.5% | file=7395328 |
| c2a5x | OK | 12679131 | 50.4% | file=7421952 |
| c2a3e | OK | 12606528 | 50.1% | file=7284736 |
| c2a2 | OK | 12495987 | 49.7% | file=7264256 |
| snark_pit | OK | 12447263 | 49.5% | file=7155712 |
| c4a1 | OK | 12405015 | 49.3% | file=7286784 |
| c1a4e | OK | 12307540 | 48.9% | file=7157760 |
| c2a4g | OK | 12224666 | 48.6% | file=6930432 |
| c4a1e | OK | 12161730 | 48.3% | file=7114752 |
| c1a4k | OK | 12086036 | 48.0% | file=7049216 |
| c2a1b | OK | 12072145 | 48.0% | file=6940672 |
| c3a2f | OK | 12045933 | 47.9% | file=6881280 |
| c2a5a | OK | 12037352 | 47.8% | file=6899712 |
| c2a4 | OK | 12022530 | 47.8% | file=6887424 |
| c0a0e | OK | 11979369 | 47.6% | file=6848512 |
| stalkyard | OK | 11930407 | 47.4% | file=6828032 |
| c1a3d | OK | 11787754 | 46.8% | file=6715392 |
| c4a2a | OK | 11375763 | 45.2% | file=6432768 |
| c4a1c | OK | 11297379 | 44.9% | file=6369280 |
| frenzy | OK | 11239737 | 44.7% | file=6332416 |
| c1a3b | OK | 11054363 | 43.9% | file=6146048 |
| xen_dm | OK | 11015344 | 43.8% | file=6152192 |
| c4a2 | OK | 11011147 | 43.8% | file=6219776 |
| c1a2d | OK | 10996400 | 43.7% | file=6146048 |
| c4a2b | OK | 10724826 | 42.6% | file=5941248 |
| c1a3c | OK | 10716373 | 42.6% | file=5914624 |
| c1a1d | OK | 10613188 | 42.2% | file=5865472 |
| c1a4j | OK | 10425691 | 41.4% | file=5740544 |
| c4a1f | OK | 10365144 | 41.2% | file=5615616 |
| contamination | NON-BUDGET FAIL (u16 index/vertex overflow, `PakManager::addVertex`) | - | - | - |
| disposal | NON-BUDGET FAIL (u16 index/vertex overflow, `PakManager::addVertex`) | - | - | - |
| pool_party | NON-BUDGET FAIL (u16 index/vertex overflow, `PakManager::addVertex`) | - | - | - |
| rocket_frenzy | NON-BUDGET FAIL (u16 index/vertex overflow, `PakManager::addVertex`) | - | - | - |

125 rows. Fixed by X9-R (open item 8 below): see the re-measured table
that follows.

### X9-R — lightmap atlas packed tallest-first and trimmed: corpus re-measured

Re-measured 2026-09-24 with the X9-R baker (same flags, `--target xbox
--profile nv2a`, one `AssetStudio` run per map). The table above is the
X9-Q measurement and stays as the before picture.

- **121 of 125 bake, all within budget; none over.** The 4 remaining
  failures are the long-standing u16 vertex-count limit, unchanged.
- **Every one of the 121 fits on one lightmap page.** 11 used two before,
  and `ref/nv2a` rejects any vertex on page 1, so those 11 could not draw
  on the Xbox at all until now.
- **Worst: `boot_camp` 64.2%** (was 110.2%). **Median
  34.1%** (was 58.2%).

| Map | Status | Ledger bytes | % of 24 MiB | Lightmap pages |
|---|---|---:|---:|---:|
| boot_camp | OK | 16157970 | 64.2% | 1 |
| t0a0b | OK | 15224903 | 60.5% | 1 |
| c2a5e | OK | 13492753 | 53.6% | 1 |
| rustmill | OK | 13483949 | 53.6% | 1 |
| c2a2e | OK | 13121299 | 52.1% | 1 |
| c2a4c | OK | 12860887 | 51.1% | 1 |
| c2a4e | OK | 12838619 | 51.0% | 1 |
| c5a1 | OK | 12835790 | 51.0% | 1 |
| c2a5f | OK | 12490456 | 49.6% | 1 |
| t0a0a | OK | 12251973 | 48.7% | 1 |
| c1a1f | OK | 11902993 | 47.3% | 1 |
| c2a2a | OK | 11856519 | 47.1% | 1 |
| c2a1a | OK | 11847430 | 47.1% | 1 |
| c3a2a | OK | 11836526 | 47.0% | 1 |
| c2a4b | OK | 11755863 | 46.7% | 1 |
| c3a1 | OK | 11709721 | 46.5% | 1 |
| c2a2b1 | OK | 11427583 | 45.4% | 1 |
| t0a0 | OK | 11408708 | 45.3% | 1 |
| doublecross | OK | 11399984 | 45.3% | 1 |
| c1a2 | OK | 11242301 | 44.7% | 1 |
| c2a2d | OK | 11217610 | 44.6% | 1 |
| c3a2c | OK | 11206504 | 44.5% | 1 |
| c2a3b | OK | 11105825 | 44.1% | 1 |
| c1a2b | OK | 11090245 | 44.1% | 1 |
| hldemo2 | OK | 11047959 | 43.9% | 1 |
| c1a4g | OK | 10947170 | 43.5% | 1 |
| c1a4f | OK | 10876501 | 43.2% | 1 |
| c1a1c | OK | 10838921 | 43.1% | 1 |
| c0a0c | OK | 10795944 | 42.9% | 1 |
| c3a1a | OK | 10774448 | 42.8% | 1 |
| c4a3 | OK | 10666110 | 42.4% | 1 |
| c0a0b | OK | 10498431 | 41.7% | 1 |
| c1a1b | OK | 10444900 | 41.5% | 1 |
| c1a3 | OK | 10316365 | 41.0% | 1 |
| c1a2a | OK | 10310995 | 41.0% | 1 |
| lambda_bunker | OK | 10278040 | 40.8% | 1 |
| c1a4d | OK | 10261151 | 40.8% | 1 |
| c1a4i | OK | 10254855 | 40.7% | 1 |
| c2a4a | OK | 10208550 | 40.6% | 1 |
| c2a2f | OK | 10111556 | 40.2% | 1 |
| c2a2c | OK | 9925531 | 39.4% | 1 |
| c2a1 | OK | 9839558 | 39.1% | 1 |
| subtransit | OK | 9792498 | 38.9% | 1 |
| c2a4f | OK | 9703842 | 38.6% | 1 |
| t0a0b1 | OK | 9693208 | 38.5% | 1 |
| c2a2g | OK | 9440120 | 37.5% | 1 |
| c2a2b2 | OK | 9366292 | 37.2% | 1 |
| c1a0e | OK | 9322024 | 37.0% | 1 |
| c2a3a | OK | 9211213 | 36.6% | 1 |
| c2a3 | OK | 9160562 | 36.4% | 1 |
| c1a3a | OK | 9110583 | 36.2% | 1 |
| c1a1 | OK | 9096661 | 36.1% | 1 |
| c1a0c | OK | 8982962 | 35.7% | 1 |
| c3a1b | OK | 8945240 | 35.5% | 1 |
| c1a0b | OK | 8910187 | 35.4% | 1 |
| crossfire | OK | 8805649 | 35.0% | 1 |
| c2a3c | OK | 8754659 | 34.8% | 1 |
| c2a2h | OK | 8751442 | 34.8% | 1 |
| c2a5 | OK | 8618881 | 34.2% | 1 |
| rapidcore | OK | 8603632 | 34.2% | 1 |
| c1a0d | OK | 8581013 | 34.1% | 1 |
| bounce | OK | 8577799 | 34.1% | 1 |
| hldemo3 | OK | 8544183 | 34.0% | 1 |
| c1a4 | OK | 8537555 | 33.9% | 1 |
| c1a4b | OK | 8499254 | 33.8% | 1 |
| c1a1a | OK | 8480596 | 33.7% | 1 |
| c3a2d | OK | 8434707 | 33.5% | 1 |
| c2a3d | OK | 8347386 | 33.2% | 1 |
| c1a0 | OK | 8230403 | 32.7% | 1 |
| t0a0c | OK | 8181692 | 32.5% | 1 |
| datacore | OK | 8171926 | 32.5% | 1 |
| hldemo1 | OK | 8137614 | 32.3% | 1 |
| c1a0a | OK | 8125550 | 32.3% | 1 |
| c0a0 | OK | 8108446 | 32.2% | 1 |
| c2a5b | OK | 8042894 | 32.0% | 1 |
| c0a0a | OK | 8016956 | 31.9% | 1 |
| c0a0d | OK | 7986326 | 31.7% | 1 |
| c4a1b | OK | 7914710 | 31.5% | 1 |
| t0a0b2 | OK | 7726786 | 30.7% | 1 |
| c2a5g | OK | 7643562 | 30.4% | 1 |
| gasworks | OK | 7611521 | 30.2% | 1 |
| c3a2e | OK | 7482569 | 29.7% | 1 |
| undertow | OK | 7027313 | 27.9% | 1 |
| c4a1d | OK | 6823032 | 27.1% | 1 |
| c4a1a | OK | 6763365 | 26.9% | 1 |
| c3a2b | OK | 6723547 | 26.7% | 1 |
| c2a4d | OK | 6671827 | 26.5% | 1 |
| c2a5x | OK | 6453211 | 25.6% | 1 |
| c2a5w | OK | 6428781 | 25.5% | 1 |
| c4a1 | OK | 6375703 | 25.3% | 1 |
| c2a3e | OK | 6347840 | 25.2% | 1 |
| c1a2c | OK | 6292048 | 25.0% | 1 |
| c2a5c | OK | 6266356 | 24.9% | 1 |
| c4a1e | OK | 6034114 | 24.0% | 1 |
| c1a4k | OK | 6023956 | 23.9% | 1 |
| c3a2 | OK | 5916277 | 23.5% | 1 |
| c1a4e | OK | 5885012 | 23.4% | 1 |
| c2a2 | OK | 5876851 | 23.4% | 1 |
| c2a5d | OK | 5833733 | 23.2% | 1 |
| snark_pit | OK | 5631519 | 22.4% | 1 |
| t0a0d | OK | 5571855 | 22.1% | 1 |
| c2a5a | OK | 5090536 | 20.2% | 1 |
| c2a1b | OK | 5027025 | 20.0% | 1 |
| stalkyard | OK | 5016359 | 19.9% | 1 |
| c2a4 | OK | 5010178 | 19.9% | 1 |
| c0a0e | OK | 4999785 | 19.9% | 1 |
| c3a2f | OK | 4771437 | 19.0% | 1 |
| c2a4g | OK | 4753562 | 18.9% | 1 |
| c1a3d | OK | 4578794 | 18.2% | 1 |
| frenzy | OK | 4292921 | 17.1% | 1 |
| c4a2a | OK | 4232339 | 16.8% | 1 |
| c4a2 | OK | 4228171 | 16.8% | 1 |
| c4a1c | OK | 4055651 | 16.1% | 1 |
| xen_dm | OK | 4035760 | 16.0% | 1 |
| c1a3b | OK | 3910939 | 15.5% | 1 |
| c1a2d | OK | 3656368 | 14.5% | 1 |
| c4a2b | OK | 3319258 | 13.2% | 1 |
| c1a1d | OK | 3273156 | 13.0% | 1 |
| c1a3c | OK | 3245269 | 12.9% | 1 |
| c1a4j | OK | 2987355 | 11.9% | 1 |
| c4a1f | OK | 2468056 | 9.8% | 1 |
| contamination | NON-BUDGET FAIL (u16 vertex count, `PakManager::addVertex`) | - | - | - |
| disposal | NON-BUDGET FAIL (u16 vertex count, `PakManager::addVertex`) | - | - | - |
| pool_party | NON-BUDGET FAIL (u16 vertex count, `PakManager::addVertex`) | - | - | - |
| rocket_frenzy | NON-BUDGET FAIL (u16 vertex count, `PakManager::addVertex`) | - | - | - |

125 rows.

### X9-S — MPAK v11 vertex chunks: corpus re-measured

Re-measured 2026-09-24 with the X9-S baker (MPAK v11, same flags). Two
things changed against the X9-R table above: the four maps over 65,536
vertices now get past the vertex limit, and the ledger's per-face term
counts the real 52-byte `nv2a_map_face_t` (it assumed 32), which adds
~0.5-1.5 points on the large maps.

- **122 of 125 bake**, all within the 24 MiB ledger; worst
  `pool_party` 95.9% (new), then `boot_camp`
  65.0%. Median 34.5%.
- **3 fail the bake-time budget:** `rocket_frenzy` 227.1%, `disposal` 160.5%, `contamination` 111.1%.
- No map fails the vertex limit any more.
- The ledger is not what is actually free: `pool_party` passes it yet runs
  out of contiguous memory loading its lightmap in xemu (item 10).

| Map | Status | Ledger bytes | % of 24 MiB | Lightmap pages |
|---|---|---:|---:|---:|
| rocket_frenzy | **OVER BUDGET** | 57160159 | 227.1% | 2 |
| disposal | **OVER BUDGET** | 40387262 | 160.5% | 2 |
| contamination | **OVER BUDGET** | 27957273 | 111.1% | 1 |
| pool_party | OK | 24138983 | 95.9% | 1 |
| boot_camp | OK | 16367610 | 65.0% | 1 |
| t0a0b | OK | 15388563 | 61.1% | 1 |
| rustmill | OK | 13644449 | 54.2% | 1 |
| c2a5e | OK | 13641013 | 54.2% | 1 |
| c2a2e | OK | 13279839 | 52.8% | 1 |
| c2a4c | OK | 13031027 | 51.8% | 1 |
| c2a4e | OK | 13014119 | 51.7% | 1 |
| c5a1 | OK | 12994870 | 51.6% | 1 |
| c2a5f | OK | 12660936 | 50.3% | 1 |
| t0a0a | OK | 12386953 | 49.2% | 1 |
| c1a1f | OK | 12017013 | 47.8% | 1 |
| c2a2a | OK | 12002819 | 47.7% | 1 |
| c2a1a | OK | 11998910 | 47.7% | 1 |
| c3a2a | OK | 11970086 | 47.6% | 1 |
| c2a4b | OK | 11887763 | 47.2% | 1 |
| c3a1 | OK | 11822381 | 47.0% | 1 |
| c2a2b1 | OK | 11558523 | 45.9% | 1 |
| t0a0 | OK | 11544068 | 45.9% | 1 |
| doublecross | OK | 11511504 | 45.7% | 1 |
| c1a2 | OK | 11392801 | 45.3% | 1 |
| c2a2d | OK | 11344370 | 45.1% | 1 |
| c3a2c | OK | 11326824 | 45.0% | 1 |
| c1a2b | OK | 11248585 | 44.7% | 1 |
| c2a3b | OK | 11203525 | 44.5% | 1 |
| hldemo2 | OK | 11188099 | 44.5% | 1 |
| c1a4g | OK | 11080570 | 44.0% | 1 |
| c1a4f | OK | 11002201 | 43.7% | 1 |
| c1a1c | OK | 10959021 | 43.5% | 1 |
| c0a0c | OK | 10896104 | 43.3% | 1 |
| c3a1a | OK | 10876368 | 43.2% | 1 |
| c4a3 | OK | 10771670 | 42.8% | 1 |
| c0a0b | OK | 10593371 | 42.1% | 1 |
| c1a1b | OK | 10567300 | 42.0% | 1 |
| c1a2a | OK | 10463295 | 41.6% | 1 |
| c1a3 | OK | 10419985 | 41.4% | 1 |
| c1a4d | OK | 10391051 | 41.3% | 1 |
| lambda_bunker | OK | 10372280 | 41.2% | 1 |
| c2a4a | OK | 10356510 | 41.2% | 1 |
| c1a4i | OK | 10355795 | 41.2% | 1 |
| c2a2f | OK | 10235316 | 40.7% | 1 |
| c2a2c | OK | 10047111 | 39.9% | 1 |
| c2a1 | OK | 9963118 | 39.6% | 1 |
| subtransit | OK | 9906298 | 39.4% | 1 |
| c2a4f | OK | 9819482 | 39.0% | 1 |
| t0a0b1 | OK | 9792088 | 38.9% | 1 |
| c2a2g | OK | 9532680 | 37.9% | 1 |
| c2a2b2 | OK | 9456612 | 37.6% | 1 |
| c1a0e | OK | 9390824 | 37.3% | 1 |
| c2a3a | OK | 9304513 | 37.0% | 1 |
| c2a3 | OK | 9263962 | 36.8% | 1 |
| c1a3a | OK | 9212803 | 36.6% | 1 |
| c1a1 | OK | 9185961 | 36.5% | 1 |
| c1a0c | OK | 9073562 | 36.1% | 1 |
| c3a1b | OK | 9024440 | 35.9% | 1 |
| c1a0b | OK | 8987287 | 35.7% | 1 |
| crossfire | OK | 8893349 | 35.3% | 1 |
| c2a3c | OK | 8849199 | 35.2% | 1 |
| c2a2h | OK | 8834842 | 35.1% | 1 |
| c2a5 | OK | 8726021 | 34.7% | 1 |
| rapidcore | OK | 8692512 | 34.5% | 1 |
| c1a0d | OK | 8664393 | 34.4% | 1 |
| bounce | OK | 8661939 | 34.4% | 1 |
| hldemo3 | OK | 8645123 | 34.4% | 1 |
| c1a4 | OK | 8625055 | 34.3% | 1 |
| c1a4b | OK | 8595454 | 34.2% | 1 |
| c1a1a | OK | 8558996 | 34.0% | 1 |
| c3a2d | OK | 8496127 | 33.8% | 1 |
| c2a3d | OK | 8439426 | 33.5% | 1 |
| c1a0 | OK | 8304303 | 33.0% | 1 |
| datacore | OK | 8266366 | 32.8% | 1 |
| t0a0c | OK | 8263932 | 32.8% | 1 |
| hldemo1 | OK | 8231254 | 32.7% | 1 |
| c1a0a | OK | 8199910 | 32.6% | 1 |
| c0a0 | OK | 8182086 | 32.5% | 1 |
| c2a5b | OK | 8121254 | 32.3% | 1 |
| c0a0a | OK | 8089116 | 32.1% | 1 |
| c0a0d | OK | 8060126 | 32.0% | 1 |
| c4a1b | OK | 8038430 | 31.9% | 1 |
| t0a0b2 | OK | 7796186 | 31.0% | 1 |
| c2a5g | OK | 7709362 | 30.6% | 1 |
| gasworks | OK | 7692501 | 30.6% | 1 |
| c3a2e | OK | 7554909 | 30.0% | 1 |
| undertow | OK | 7091973 | 28.2% | 1 |
| c4a1d | OK | 6905752 | 27.4% | 1 |
| c4a1a | OK | 6851865 | 27.2% | 1 |
| c3a2b | OK | 6775847 | 26.9% | 1 |
| c2a4d | OK | 6731647 | 26.7% | 1 |
| c2a5x | OK | 6526951 | 25.9% | 1 |
| c2a5w | OK | 6497361 | 25.8% | 1 |
| c4a1 | OK | 6451523 | 25.6% | 1 |
| c2a3e | OK | 6403680 | 25.4% | 1 |
| c1a2c | OK | 6368368 | 25.3% | 1 |
| c2a5c | OK | 6320676 | 25.1% | 1 |
| c4a1e | OK | 6104474 | 24.3% | 1 |
| c1a4k | OK | 6072356 | 24.1% | 1 |
| c3a2 | OK | 5955097 | 23.7% | 1 |
| c1a4e | OK | 5942612 | 23.6% | 1 |
| c2a2 | OK | 5934751 | 23.6% | 1 |
| c2a5d | OK | 5889993 | 23.4% | 1 |
| snark_pit | OK | 5689179 | 22.6% | 1 |
| t0a0d | OK | 5617195 | 22.3% | 1 |
| c2a5a | OK | 5151736 | 20.5% | 1 |
| c2a1b | OK | 5094405 | 20.2% | 1 |
| stalkyard | OK | 5067379 | 20.1% | 1 |
| c2a4 | OK | 5064618 | 20.1% | 1 |
| c0a0e | OK | 5038685 | 20.0% | 1 |
| c3a2f | OK | 4803137 | 19.1% | 1 |
| c2a4g | OK | 4789282 | 19.0% | 1 |
| c1a3d | OK | 4624834 | 18.4% | 1 |
| frenzy | OK | 4334701 | 17.2% | 1 |
| c4a2a | OK | 4270719 | 17.0% | 1 |
| c4a2 | OK | 4264231 | 16.9% | 1 |
| c4a1c | OK | 4094111 | 16.3% | 1 |
| xen_dm | OK | 4079120 | 16.2% | 1 |
| c1a3b | OK | 3952119 | 15.7% | 1 |
| c1a2d | OK | 3700288 | 14.7% | 1 |
| c4a2b | OK | 3337218 | 13.3% | 1 |
| c1a1d | OK | 3306916 | 13.1% | 1 |
| c1a3c | OK | 3269449 | 13.0% | 1 |
| c1a4j | OK | 3021175 | 12.0% | 1 |
| c4a1f | OK | 2488616 | 9.9% | 1 |

125 rows.

8. ~~**7 retail maps exceed the 24 MiB per-map budget and no longer bake.**~~
   **Done, X9-R (`divergences.md` #108):** the baker packs lightmap blocks
   tallest-first in a pre-pass and trims the atlas to the rows it uses, and
   `lv` is divided by that final height. Every retail map now fits on one
   page and within budget (worst `boot_camp` 64.2%, median 34.1%; table in
   the X9-Q section). Correction to the diagnosis above: the pages were not
   5-7% used - that counted raw luxels only. With each block's 1-luxel
   border and the extra lightstyle blocks, the two-page maps needed 43-68%
   of one page; the waste was the shelf packer taking blocks in face order.
   Left for later: stop holding the whole archive while copying the atlas
   to GPU memory (the atlas still counts twice), RGB565 lightmaps.

9. ~~**Bake the 4 retail maps that exceed 65,536 vertices.**~~ **Done,
   X9-S (`divergences.md` #109, MPAK v11):** vertices are split into
   chunks of at most 65,536, each face inside one chunk with u16 indices
   relative to it; a per-face base lump (multi-chunk maps only) tells
   `ref/nv2a` where to point its vertex attribute arrays. All four now get
   past the vertex limit (2, 2, 2 and 3 chunks). Single-chunk maps are
   byte-identical to v10 apart from the version and header-size fields, and
   crossfire renders pixel-identical in xemu. What still stops them:
   - `pool_party` bakes (95.9% of the 24 MiB ledger) but does not load in
     xemu: the lightmap's contiguous allocation fails - see item 10.
   - `contamination` (111.1%), `disposal` (160.5%) and `rocket_frenzy`
     (227.1%) fail the bake-time budget. `disposal` and `rocket_frenzy`
     also need two lightmap pages even with X9-R's packing, which
     `ref/nv2a` does not support.

10. ~~**Load-time memory: the 24 MiB ledger is not what is actually
    free.**~~ **Done for 124 of 125 maps, X9-T (`divergences.md` #110-#112,
    MPAK v12).** Measured in xemu with a temporary `MmQueryStatistics`
    probe: 43.6 MiB free at `R_Init`, **32.8 MiB on the first frames before
    any map loads**, and a deathmatch map's precaches arrive between the
    table build and `R_NewMap`. What changed:
    - `ref/nv2a` builds its GPU buffers at `R_NewMap`, streamed from the
      archive, after the engine has freed its copy; the lightmap is
      allocated first, and a failed upload is retried once.
    - MPAK v12 puts the classic lumps first; the engine is served only that
      prefix as `maps/<name>.bsp`, `ref/nv2a` reads the rest from
      `maps/<name>.mpak`.
    - One lightmap page up to 4096 rows (no second layer).
    - Xbox `mextrasurf_t` 324 -> 148 bytes; Xbox nv2a maps carry miptex
      headers only.
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
    its GPU buffers. Roadmap X9 item 11.
    Left open from the reviews:
    - The ledger does not see the engine's world structures or the
      precaches, so a map can pass it and still not load (`rocket_frenzy`
      passes at 78%). Replace it with `E(map) + max(prefix + temporaries +
      tables, tables + precache + GPU) + reserve <= measured free`, with
      `E(map)` from lump counts (`memory-budget-specialist`'s model matches
      xemu to ~0.3 MiB) and the precache a measured per-game constant.
    - Only the lightmap should need one large contiguous run: allocate the
      texture data per texture and the vertex buffer per 65,536-vertex
      chunk (`nv2a_bind_vertex_base` already re-points per chunk), and
      measure the largest allocatable run, not just free pages
      (`nv2a-xbox-specialist`). Cycle maps to check fragmentation.
    - A 4096-row `IMAGE_RECT` lightmap is unverified on hardware.
    - The persistent tables still use `Mem_Malloc`, which calls `Sys_Error`
      when it runs out.
    - With header-only miptex the engine reads and decodes each WAD
      texture only for `ref/nv2a` to discard it: drop the worldspawn `wad`
      key in the Xbox nv2a bake, or stop staging the texture WADs.

11. ~~**`rocket_frenzy`: find ~17 MiB.**~~ **Closed, X9-U: the baker refuses
    it (`divergences.md` #113-#116).** Working on it freed memory for every
    map - measured in xemu, ~36.5 MiB is now free before a map loads (was
    32.8) - but `rocket_frenzy` does not fit, and past the engine's own
    limits it is not a memory problem alone:
    - It has 988 edict slots at its load peak (1,016 entities, 278
      `env_glow`) against the game's `max_edicts` of 900; it fails the same
      way on PC Xash. GoldSrc adds 15 slots per player, so it is a
      multiplayer map that needs 8+ slots; Xash uses the flat limit.
    - With the engine's memory cut (bevels on demand, slimmer
      `mextrasurf_t`/`cl_entity_t`, `ref/soft`'s tables at init) it gets
      through its world load, but at `R_NewMap` it had 7.2 MiB free for
      11.5 MiB of GPU buffers.
    - Hosting it (`maxclients` > 1) would also need ~75 MiB for
      `Mod_CalcPHS` - item 13.
    Instead of more cuts with visible or ABI costs, the baker now checks
    every map before and after the bake (`Nv2aMapFit.h`): edict slots at
    the load peak against `max_edicts`, the engine's world load (C1) and
    the renderer's tables and GPU buffers at `R_NewMap` (C3), calibrated in
    xemu on ten maps. Over the 125 retail maps it refuses `rocket_frenzy`
    alone; the tightest that fit are `disposal` (882 of 900 slots, 10.1 MiB
    of C3 headroom), `contamination` (11.4) and `pool_party` (12.0).
    Kept from the work, and useful to every map: face bevels on demand
    (#113), `ref/soft`'s tables at `R_Init` (#114), ref/nv2a's tables built
    at `R_NewMap` and a 24-byte face table, per-map tables from `malloc`, a
    tight style-0 lightmap atlas, only drawn textures baked (#115),
    `HISTORY_MAX` 16 and a 112-byte `mextrasurf_t` on Xbox (#116). The
    `max_edicts 1100` raise tried for it was reverted (~0.8 MiB on every
    map). Left, if it is ever wanted: the unused 1.2 MiB HAL framebuffer
    (an nxdk patch), the client's 64-slot frame arrays of which one-player
    games use 4, lightmap UVs as shorts.

12. **Level transitions: can the console run out between maps?** Every
    X9-T measurement booted one map from a fresh start. On a listen server
    the previous map's unused models are freed only after the new map's
    precaches and `R_NewMap` (`Mod_FreeUnused`, `cl_parse.c:1719`), so
    during a transition both maps' models are resident while the new GPU
    buffers are allocated; X9-T's one upload retry from `R_EndFrame` covers
    only the GPU half of that. Repeated loads can also fragment memory
    until the lightmap's contiguous run no longer fits, even with enough
    free pages. Measure in xemu, with a temporary probe at each `R_NewMap`:
    free pages, the largest allocatable contiguous block (bisect
    `MmAllocateContiguousMemoryEx`) and dlmalloc's `mallinfo`, across a
    single-player `changelevel` chain (e.g. `c1a0` through `c1a4`,
    including the largest maps and a return trip through a landmark) and
    across repeated `map` cycles (`crossfire`, `disposal`, `crossfire`,
    ... five or more times). Any downward drift or failure becomes a fix:
    a GPU arena reserved once at `R_Init` and sub-allocated per map, and
    `Mod_FreeUnused` before the upload.

    **Measured (X9-V, xemu, probe build):**
    - *Deathmatch `map` cycle* (crossfire, disposal, pool_party,
      contamination, boot_camp, disposal, crossfire, disposal), free
      MiB at `R_NewMap` / largest contiguous block / after the upload:
      27.7/27.7/25.5, 20.5/20.4/13.1, 20.6/20.6/15.4, 21.3/21.2/15.5,
      24.6/24.6/21.1, 20.5/20.5/13.2, 25.2/25.2/23.3, 20.2/20.2/13.0.
      Every load became ready and the largest block stays within 0.1 MiB
      of free memory, so there is no fragmentation. `disposal` loses
      0.3 MiB over two revisits; `crossfire` starts 2.5 MiB lower on its
      second load than on the first boot (not yet attributed).
    - *Played by hand with every archive staged*, `c0a0` through `c1a0c`
      (Unforeseen Consequences), 19 minutes, 14 loads including a
      `c1a0`/`c1a0d` back-and-forth through a landmark: 22.3-26.6 MiB
      free at each map start, the largest block equal to free every time
      (0.1 MiB short once), revisits back at the same figures. Level
      changes take 2.0-3.9 s. Frame rate holds at the 60 Hz vsync (59.5-
      60.2 fps per map) except `c1a0b` (54.4 average, 1% low 29 fps);
      three single frames of 261-416 ms during play, none repeating. This
      covers the single-player chain the scripted `changelevel` run was
      meant to measure. Full timeline and per-map table:
      `docs/r3d/reports/x9-item12-blackmesa-frametimes.html` (open it in
      a browser).
    - *The same route on a real Original Xbox* (probe saving to
      `D:\probe_ft.bin`, pulled over FTP; 17.6 minutes, 14 loads):
      memory as in xemu (23.3-26.4 MiB free at each map start, within
      1.4 MiB of xemu per map, no fragmentation, no drift), level changes
      1.7-4.1 s - but **44 fps overall, not 60**: the tram 50, `c0a0e` and
      `c1a0d` 60, and four maps near 30 (`c0a0c` 31, `c0a0d` 31, `c1a0e`
      34, `c1a0c` 30), most frames there at 31-36 ms. xemu caps at the
      60 Hz refresh and hides this. Frame times vary smoothly (no
      16.7/33.3 ms steps), so presentation does not wait on vsync. Report:
      `docs/r3d/reports/x9-item12-xbox-frametimes.html`. The four slow
      maps are X11's first profiling targets (CPU against GPU).
    - *Conclusion:* no drift and no fragmentation across level changes, so
      the GPU arena and the early `Mod_FreeUnused` are not needed. Open:
      `c1a0b`'s frame rate, and the 2.5 MiB `crossfire` difference.
    - *Found on the way:* with every retail archive in the game folder
      the game hung on the boot logo. Each mounted `.mpak` keeps its file
      open and the Xbox POSIX layer had 64 descriptors
      (`compat/xbox/xbox_posix_io.c`); 124 archives took all of them
      before the config was read. Raised to 256. Still open: the limit is
      fixed and running out still hangs silently, so a folder of ~250
      archives would hang again. Cheap guards are a message when the
      table is full or a baker check on archives per folder; the
      structural fix is opening each archive on demand in
      `filesystem/mpak.c` instead of holding its handle for the session.
      Each open file also costs a few hundred bytes of kernel pool, which
      the heap probe does not see (~25-60 KiB for 124 archives, estimated;
      measure on hardware with `MmQueryStatistics`).

13. **Offline PHS for Xbox-hosted multiplayer - done (X9-W), except the
    two PC-compatibility items at the end.** MPAK v13 carries the PHS
    (`PhsBuilder.h`), the Xbox engine reads it (`Mod_ReadBakedPHS`,
    divergence #117), `tools/test-phs.sh` proves it against `Mod_CalcPHS`
    (124 maps, 0 mismatches) and `Nv2aMapFit.h` reports hosting as
    advisory. In xemu, 2-player listen servers load with 21.1 MiB free
    on `crossfire`, 7.8 on `pool_party` and 6.5 on `disposal`.
    - *How many players:* a listen server's own buffers grow with the
      player count - the server's packet entities (players x 64 frames x
      64 x 340 bytes), a 91 KB client slot and 64 frames of 6 KB per
      player, and the local client's packet entities (1.3 MiB) - about
      1.8 MiB per player (memory-budget review; the 2-player xemu figures
      are 3.4-4.1 MiB below single-player beyond the PHS, which these
      terms account for). With them, and each player holding up to 16
      edict slots with its weapons, the bake reports: `pool_party` and
      `disposal` 3 players, `contamination` 4, `boot_camp` 8, the other
      deathmatch maps 10-12 (`rocket_frenzy` is refused outright).
    - *Found: a hosted `changelevel` stalls on the Xbox - it predates
      X9-W; deferred (multiplayer is not the priority).* With `maxplayers
      2` the first level change works (the client reaches `ca_active`) and
      the second does not: the server moves to the next map, the local
      client stays in `ca_connected` waiting for `svc_serverdata` and
      never reaches `R_NewMap` (ref/nv2a then draws its test triangle).
      Measured in xemu on bounce -> datacore -> boot_camp from loose maps:
      identical on the X9-W build and on the build before it (main at
      e1fe5b04), and the same with archives stripped of their PHS; with
      `maxplayers 1` the same chain works on both. Also seen once with
      archives, multiplayer only: after the first change the client was
      active but ref/nv2a's map never became ready (not yet checked on the
      older build). Next step when it is picked up: breakpoints on the
      reconnect path (the client's `new`, `SV_New_f`, and
      `SV_SendResources`, which a multiplayer server sends only once per
      client, `sv_client.c:1976`).
      Until it is fixed, a hosted game cannot change level, and the
      fragmentation check across a hosted map cycle (largest contiguous
      block, as item 12 did for single-player) cannot run: only cold
      loads were measured.
    - *Before hosting ships:* cap `maxplayers` on the Xbox per map (from
      the bake's player count), turn `sv_allowdownload` off (a remote
      client's download loads the whole file, `net_chan.c:977`), and
      measure a multiplayer game's precache, which the model's 1.5 MiB
      margin has to cover today.
    - Not done: a real multiplayer session (a client joining over the
      network, roadmap XN). The original item follows. With `maxclients` above 1
    the engine builds the map's PHS at load (`Mod_CalcPHS`,
    `mod_bmodel.c`): a transient of about `2 x rowbytes x (numleafs + 1)`
    plus the compressed result, ~9-12 MiB on `disposal`/`pool_party` and
    ~75 MiB on `rocket_frenzy` - so an Xbox listen server runs out on the
    large maps however much else is cut (joining as a client never builds
    it). Bake the compressed PHS into the archive with the baker (a new
    optional lump, MPAK version bump), have the engine use it on Xbox
    instead of calling `Mod_CalcPHS`, and prove it byte for byte against
    `Mod_CalcPHS` on the PC test engine over every retail map (the
    `test_bevels` pattern, `tools/test-bevels.sh`). Then add hosting to
    `Nv2aMapFit.h`'s checks (the PHS as a resident term) and measure a
    real multiplayer session in xemu. Related, for PC multiplayer
    compatibility: a map CRC stored at bake time from the retail `.bsp`
    (the Xbox archives' `LUMP_TEXTURES` differs, so `CRC32_MapFile` does),
    and whether `XASH_LOW_MEMORY=1`'s 64-entity client frames can follow a
    busy PC server.

14. **Finish `ref/nv2a`: everything a single-player campaign draws
    (priority, 2026-09-25).** Today ref/nv2a draws the world and brush
    entities only; the hardware frame times of item 12 were measured
    without models, HUD or effects, so every feature below costs frame
    time and item 12's four ~30 fps maps are profiled alongside (X11).
    Inventory (reference-renderer-specialist, against `ref/gl` and
    `ref/soft`, 2026-09-25) and order - each a phase, each ending on
    xemu and on hardware (item 7):
    1. *Frame structure.* Every nv2a draw happens inside `R_EndFrame`,
       but the engine issues the HUD, console and menu before it
       (`V_PostRender`, `cl_view.c:529-573`), so nothing is open to draw
       2D into. Open and clear the pushbuffer in `R_BeginFrame`, draw the
       world from `GL_RenderFrame` as `R_RenderFrame` does
       (`gl_rmain.c:1076`), leave `R_EndFrame` to present; remove the
       `pb_print` overlay.
    2. *Texture manager* (done, X9-X, 2026-09-26). A real table (D1-D7,
       `ref/nv2a/r_image.c`, modelled on `ref/soft/r_image.c` as planned):
       `GL_LoadTexture`, `GL_LoadTextureFromBuffer`, `GL_CreateTexture`,
       `GL_FindTexture`, `GL_FreeTexture`, `GL_UpdateTexture`, `GL_Bind`,
       `GL_TextureName`/`GL_TextureData`/`R_GetTextureOriginalBuffer`/
       `R_OverrideTextureSourceSize`. `RefGetParm` answers every `PARM_TEX_*`
       and falls through to `ENGINE_GET_PARM_`, as planned. Format this
       phase: flat A8B8G8R8 (RGBA32) only, uploaded into a shared 3 MiB
       arena (a first-fit sub-allocator, `nv2a_arena.c`, host-tested) with
       a per-texture fallback allocation when it has no room. No mipmaps,
       no compressed (DXT) input and no swizzling yet - a linear upload
       covers the console font, HUD sprites, menu pictures and the
       engine's five built-in textures; decals and the skybox still need
       their own draw paths (items 10 and 12). World (`.mip`) textures
       never reach this manager at all: the Xbox nv2a bake now drops the
       worldspawn `wad` key (Part A of the same change), so the engine
       takes its own no-pixels path instead of decoding a texture this
       renderer would only discard. XB's DXT1 swizzle bug
       (`UiPacker.cpp:379`) and `menu_bg.mpak` are still untouched -
       nothing reads a compressed 2D texture yet.
    3. *2D* (done, X9-X, 2026-09-26): `R_Set2DMode`, `R_DrawStretchPic`,
       `FillRGBA`, `GL_SetRenderMode` (all five modes plus
       `ScreenFadeModulate`), `VGUI_SetupDrawing`, `Color4ub`/`Color4f` -
       brings up the console, HUD, crosshair, fades, loading plaque and
       menus. Batched through a 256 KiB vertex ring, flushed on a state,
       texture or dirty-flag change; a linear texture's wrap addressing
       (tiled draws past `[0,1]`) is split into per-cell quads on the CPU,
       since this renderer has no hardware wrap mode for it. Not yet
       verified on real hardware (owner-side, post-PR): the 2D pixel-
       centre offset cvar, and whether the console/HUD/menu actually look
       right rather than merely draw something.
    4. *TriAPI* (code complete, xemu-verified live, X9-X, 2026-09-26):
       `R_FillTriAPI` fills `GetMatrix`,
       `TexCoord2f`, `Fog`, `ScreenToWorld` and `FogParams`; the HL
       client's particleman calls `GetMatrix` every time
       `HUD_DrawTransparentTriangles` runs (`CFrustum.cpp:41`), a NULL call
       before this step, safe only because nv2a never called that hook.
       `TriRenderMode`/`Begin`/`Vertex3f(v)`/`End`/`CullFace` now draw for
       real, in either 2D or 3D space, sharing the 2D pass's own batching
       ring and flush logic (generalised, X9-X step 4 Part C) and a new
       host-tested primitive assembler (`nv2a_tri_util.c`) that decomposes
       every TRI_* primitive (points, lines, triangles, quads, fans,
       strips, quad-strips, polygons) into the flat lists the hardware
       draws, with GL's own winding. The engine now also calls
       `gEngfuncs.R_DrawWorldHull()`/`pfnDrawNormalTriangles()`/
       `pfnDrawTransparentTriangles()` once per frame from `GL_RenderFrame`
       (previously never called at all). Fog is a documented no-op
       (divergence #119); translucent entities between the two triangle
       hooks are still out of scope (studio models/sprites, steps 5-6).

       The earlier "no `.xbe` producible" escalation was wrong: main's
       checkout of `deps/nxdk` (same submodule commit, `29638d0b`) already
       has every prebuilt `.lib`/`cxbe`/`extract-xiso` this needs, built
       2026-09-05/09-15 - they were just never copied into this topic's
       worktree. Copying them in (gitignored build outputs, read-only from
       main, never rebuilt) let `tools/xbox-game-link-check.sh hl
       --include-client --include-freevgui` produce two real,
       `server+client+freevgui`, 0-duplicate/0-undefined XBEs: one from
       `e7086f40` (pre-TriAPI baseline) and one from this branch.

       Both booted live in xemu (2026-09-26, map `crossfire`, `-dvd_path`
       pointing at freshly-packed ISOs, `verify-image` confirming
       "identity confirmed" before every check):
       - **(a) crash-free + hooks:** the post-change build ran well past
         60 s past `_nv2a_map_ready` with no halt. A real breakpoint on
         `GetMatrix` hit every frame (particleman's own
         `HUD_DrawTransparentTriangles` -> `CFrustum::CalculateFrustum`
         call, confirmed via `pname=0x0BA7`) and read back a genuine
         perspective matrix: `M[1][1]=1.33333` (`1/tan(fovy/2)`),
         `M[2][2]=-1.000244`, `M[3][2]=-2.000244` - exactly
         `-(zFar+zNear)/(zFar-zNear)` and `-2*zFar*zNear/(zFar-zNear)` for
         zNear=1, zFar=8192, matching `r_context.c:4865` and confirming
         divergence #119 point 2 live, not just in source. `Begin` did
         NOT hit during this idle, no-cvar spawn (nothing draws through
         the TriAPI on a fresh crossfire spawn with no active particles
         and `r_showhull 0` - `CBaseParticle`'s own Draw only runs with
         particles alive, and `R_DrawWorldHull` early-returns at
         `r_showhull.value <= 0`, `mod_dbghulls.c:723`), so `Begin`'s hook
         evidence was captured on a second, `r_showhull 1` boot instead
         (see (b)): a real breakpoint on nv2a's `Begin` hit there.
       - **(b) `r_showhull 1`:** the captured frame shows the expected
         random flat-coloured hull polygons, correctly depth-occluded
         (nearer polygons hide farther ones) and back-face culled (no
         inverted/see-through faces), following the same room silhouette
         visible in the `r_showhull 0` frame from the same spawn point.
       - **(c) `net_graph 1`:** NOT achieved, and not a TriAPI bug. A
         breakpoint confirmed `SCR_DrawNetGraph` runs every frame, and
         `net_graph`'s cvar struct in memory reads `value=1.0`, but the
         function's own first gate, `if( !host.allow_console ) return;`
         (`cl_netgraph.c:650`), fires before reaching
         `ref.dllFuncs.Begin`, confirmed by single-stepping from entry to
         its early `ret`. `host.allow_console` never becomes true here
         because `common/defaults.h` gives `DEFAULT_ALLOWCONSOLE 1` only
         to `XASH_NSWITCH`/`XASH_PSVITA` (`defaults.h:145,151`; `0` for
         every other platform including Xbox, `defaults.h:186`), and the
         one unconditional runtime override,
         `host.allow_console = host.allow_console_init = true`
         (`cl_gameui.c:30`), only runs once GameUI actually initializes -
         this game-link build's own menu module is an empty stub
         ("mainui not swept"), so that never happens. This is a
         pre-existing, separate gap (the menu/mainui sweep, not this
         TriAPI work) tracked on its own; it blocks `net_graph`
         specifically, not the TriAPI plumbing net_graph would have
         exercised (already covered live by (a)/(b) on the 3D side).
       - **(d) no debug cvars, baseline vs post-change:** pixel-diff of
         the two builds' `crossfire` spawn frames (identical userconfig,
         `r_refdll "nv2a"`, no other cvars) is **0 differing pixels of
         307200** (640x480) - `bbox=None`. The TriAPI change is fully
         invisible to the world's static draw when nothing exercises it,
         exactly as required.
       - c1a0 was not repeated (optional per spec, skipped for time).
       - XMEX: skipped, no local clone found (`which xmex` and
         `import xmex` both fail here).

       Real hardware verification is still owner-side, post-PR.
    5. *Studio models* (NPCs, items, the view model with its own depth
       range): **landed, X9-X step 5, 2026-09-26** - new `ref/nv2a/r_studio.c`,
       a whole-function port of `gl_studio.c` routed through the TriAPI
       internals (`Begin`/`TexCoord2f`/`Color4ub`/`Vertex3fv`/`End`/
       `CullFace`/`TriRenderMode`, `r_context.c`), the same way `ref/soft`
       already ported it. Landed: the full `engine_studio_api_t`/
       `r_studio_interface_t` table, CPU skinning (`R_StudioDrawPoints`),
       skins (`Mod_StudioLoadTextures`/`Unload`), render modes, chrome,
       lighting from `ref/common` (`R_LightVec`, `R_EntityDynamicLight`),
       client events and attachments (muzzle-flash elights and
       `HUD_StudioEvent` model sounds do run), the glow shell (tinted by
       `rendercolor`, not the real chrome sprite until step 6 restored
       it), the view model
       with a real depth-range remap (a second view-projection matrix at
       the same viewport-z convention the world already uses, not a
       hardware depth-range register - design note section 2),
       `R_StudioEstimateFrame`/`LerpMovement` (CPU-only, copied as-is),
       and `R_AddEntity`/`R_GetEntityRenderMode`/the translucent sort
       upgraded to accept `mod_studio` alongside brush entities. Not
       landed (see `docs/r3d/divergences.md` #120-125 and the spec's own
       outOfScope list): matrix-palette VP20 skinning (still the
       performance route, gated on a compiled constant layout, X9 notes
       above), player colour remap (`GL_ProcessTexture` stays a stub),
       stencil shadows, repeat addressing/mips on skins, and sprites
       (glow-shell chrome sprite, muzzle-flash sprite - step 6 below).
       Verification: host tests only (the per-mesh render-state mapping
       and the view-model z range) - the picture itself needs a live
       xemu look and real hardware, not run as part of landing this step.
       Follow-up: release the menu's fonts and background on low-memory
       map loads, then restore them on demand (divergence #127).
       The earlier claimed texture-free deadlock was not established;
       the later investigation found an OOM on a stale disc and restored
       both release calls without changing ref/nv2a. Font measurement
       and background preference restoration are corrected on the fix
       branch. See `reports/menu-loading-fix.md` for verification.
    6. *Sprites* and sprite temp-entities (explosions, glows):
       **landed, X9-X step 6, 2026-09-29** - new `ref/nv2a/r_sprite.c`, a
       whole-function port of `gl_sprite.c` through the TriAPI internals
       (as `r_studio.c` ports `gl_studio.c`), with the GL-state mapping and
       the group/angled frame choice host-tested (`nv2a_sprite_util.c`).
       All five orientation types, frame lerping (`r_sprite_lerping`), the
       glow occlusion/distance fade, the sprite 'lightmap' pass
       (`r_sprite_lighting`, `GL_EQUAL`), `R_CullSpriteModel`, and the
       reference's draw order (a second walk of the solid list, sprites
       inside the sorted translucent list). Loading needed nothing: the
       engine already uploads every frame at precache through
       `GL_LoadTexture` (`cl_sprite.c`), so the frames were resident on
       main without ever being drawn. The glow shell's chrome sprite and
       the studio shadow quad bind their sprites again (`TriSpriteTexture`).
       Divergence #132 (clamp, no mips, no fog).
       Memory, measured (xemu, texture table read over the debugger on
       main 9f7300a5, c1a0e after `impulse 101`): 194 world-sprite frames,
       2.29 MiB (RGBA32 2.12, A8Y8 0.13, A8 0.05 by the #131 classifier) -
       the largest `Fexplo1` 380 KiB, `flare6` 320, `XFlare1` 312,
       `explode1` 277, `zerogxplode` 240 - plus 1.22 MiB of HUD sprites.
       This step adds no texture memory (the uploads are unchanged); kept
       eager rather than lazy like the studio skins, because an explosion
       or muzzle flash is on screen for a few frames and a skin-cache
       style placeholder would show as white. Follow-up worth measuring:
       most of those bytes are opaque additive frames (alpha 255
       everywhere), which `R5G6B5` would halve at a banding cost on glows.
       Verified live in xemu (branch vs main, same teleported position
       and view angles in the c1a0e test chamber): the ceiling lamps'
       `env_glow` flares draw as soft additive halos; an MP5 AR grenade's
       explosion draws additive and depth-tested behind the pillar, with
       spark sprites; the MP5 muzzle flash draws at the barrel; HUD, view
       model and world are otherwise unchanged; none of them on main.
       Not verified live: the glow shell's chrome sprite, `SPR_ALPHTEST`
       lit sprites (blood), angled frames; real hardware.
    7. *Effects:* the `CL_DrawEFX` hook, beams (and `ET_BEAM`
       entities), particles, tracers: **landed, X9-X step 7, 2026-09-29** -
       new `ref/nv2a/r_effects.c`, a whole-function port of `gl_beams.c`
       and `gl_rpart.c` through the TriAPI internals (as `r_sprite.c`
       ports `gl_sprite.c`), with the beam noise and a texture-repeat
       splitter host-tested (`nv2a_efx_util.c`). `GL_RenderFrame` now
       calls the engine's `CL_DrawEFX` at `R_DrawEntitiesOnList`'s two
       points (`gl_rmain.c:861`, `:922`); `R_AddEntity` keeps `ET_BEAM`
       entities; `Mod_GetCurrentVis` returns the real PVS row. Found on
       the way: main never called `CL_DrawEFX`, which is also where the
       engine frees dead beams and particles (`cl_efx.c:2081-2090`), so
       on main the pools filled and stayed full (read live on c1a0e: 128
       dead beams, 3360 particles, 303 tracers held forever). Beam
       sprites repeat along V where linear NV2A textures only clamp: the
       TriAPI cuts each beam primitive at the texture's cell boundaries
       (divergence #133, which also lists fog, shade model and the
       zeroed locals). Memory: no new texture - the particle texture is
       the engine's `*particle` (`ref_common.c:531`), already resident on
       main as 8x8 A8 by the #131 classifier, and beams/tracers bind
       sprite frames the engine already uploads; 2 KiB of static beam
       list; the XBE grows 6840320 -> 6868992 bytes (+28 KiB).
       Verified live in xemu (branch vs main, c1a0e, same view angles,
       grabbed only while the engine's lists held the effect): MP5 fire
       draws tracers, spark streaks and the bullet-impact particles; the
       egon draws its textured beam and sine-noise beam from the gun to
       the flare (main: the flare only); the gauss draws its beam and
       impact glows; a tripmine's laser (an `ET_BEAM` entity, the
       `R_BeamDrawCustomEntity` path) crosses the corridor; world, NPCs,
       view model, HUD and main menu unchanged. Not verified live:
       `TE_BEAMTORUS`/`DISK`/`CYLINDER`/`RING`/`FOLLOW` (code ported
       whole, no scene triggered them), beams whose V crosses a cell
       mid-primitive seen up close, frame cost (X11), real hardware.
    8. *Animated lightstyles and dynamic lights:* one mechanism - rebuild
       a face's lightmap from `msurface_t->samples` (every style is in the
       classic prefix's `LUMP_LIGHTING`) times `g_lightstylevalue`, plus
       `R_AddDynamicLights`, written into the atlas (`R_CheckLightMap`,
       `gl_rsurf.c:1400`); models take dlights through
       `R_EntityDynamicLight`.
       **Landed, X9-X step 8, 2026-09-29.**
       - New `ref/nv2a/r_lightmap.c` ports `R_CheckLightMap`,
         `R_BuildLightMap`, `R_AddDynamicLights` and `R_MarkLights`/
         `R_PushDlights`. The arithmetic is in `nv2a_lightmap_util.c`,
         which is host-tested.
       - It rewrites a face's block of the baked atlas in place, from
         `GL_RenderFrame` before the world draw. The GPU is idle then:
         `R_EndFrame`/`R_BeginFrame` spin on `pb_busy()`, and pbkit's
         `pb_cache_flush` drains write-combining before the first draw.
       - Rebuilt faces:
         - world faces the world pass draws, when a style's value
           changed or a dlight reaches them;
         - every face of a brush entity drawn lit, with
           `R_PushDlightsForBmodel` and the entity's matrix.
       - A block holding a dlight is rebuilt without it once the light
         stops reaching it.
       - Each face's block position and size come from `MpakFaceInfo`,
         which every v13 archive already carries, and are checked against
         the engine's surface. On c1a0e no block was rejected.
       - Models needed nothing: `R_EntityDynamicLight` and
         `R_StudioEntityLight` were already in (step 5).
       - Divergence #134.
       - **Verified live in xemu (branch vs main ccbcc81b, c1a0e, same
         teleported origin and view angles):**
         - The test chamber's floor spotlights (style 35) are on at map
           start but were missing on main. They now light the emitter
           arms, ramp and floor.
         - `ent_fire floor_spotlight_1 use` toggles them at run time. The
           lights-off frame is pixel-identical to main's (0 of 307200
           pixels differ), so a rebuilt block reproduces the baked bytes
           exactly.
         - The style-4 strobe fixtures in the pit below flash green
           between grabs; on main they stay dark.
         - The flashlight lights a pool on the floor of the dark
           alien-slave room; main leaves the floor black.
         - An MP5 grenade's explosion lights the floor around it; main
           lights only the models.
       - Cost, per `nv2a_lm_stats`:
         - At most 16384 luxels are rebuilt per frame
           (`NV2A_LM_FRAME_LUXELS`).
         - Measured on c1a0e: a strobe frame rebuilds 20 faces (1804
           luxels); a flashlight frame 26 faces (1526 luxels); a frame
           with nothing changed rebuilds none.
         - The Xbox objects are built at clang's default -O0
           (`xbox-cc.sh` passes no `-O`). There a luxel is 28 + 31 per
           extra style + 63 instructions, about 110-150 P6 cycles, so a
           strobe frame is about 0.35 ms at 733 MHz and a budget-full
           frame about 3 ms. These are estimates: xemu's rdtsc is not
           PIII time, and xemu runs vsync-bound at 60 fps on both builds.
       - Memory:
         - 9 bytes per face (counted in the per-map ledger and in the
           baker's `kNv2aLightmapFaceBytes`).
         - 48 KiB of scratch `r_blocklights`, 8 KiB of lists and 2 KiB
           of style tables.
         - The XBE grows from 6868992 to 6881280 bytes.
       - Not verified: whether the NV2A texture cache can keep stale atlas
         texels after the CPU rewrites them on real hardware. No
         invalidate method is known in nxdk or xemu. Real hardware
         decides.
    9. *Animated textures* (`+0..+9`, `-0..-9`; the engine builds the
       chains, `mod_bmodel.c:3043`); check that frames no face references
       reach the texture directory.
    10. *Sky* (skybox from `R_SetupSky`; check the images are on the disc).
    11. *Water warp* (`!` textures: `GL_SubdivideSurface` and the per-vertex
        sin-table texture coordinates of `EmitWaterPolys`) and underwater
        fog (`R_CheckFog`).
    12. ~~`kRenderTransColor` on brush entities~~ **Done (brush-entity-visibility
        branch, divergences.md #103 point 2):** a synthetic flat-colour
        texel bound to stage 0 in place of the entity's own face texture,
        real `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA` blending against it. Also
        narrowed while touching the same render-mode switch: `{`-masked
        `kRenderTransTexture`/`kRenderGlow` brushes (glass, grates) now
        alpha-test-discard the fully-transparent DXT1 sentinel texel instead
        of blending it as a black smudge (#103 point 5; the sub-threshold
        bilinear-edge case is left open, needs a combiner change). Still
        open: decals; screenshots (the save-game thumbnail);
        `Mod_GetCurrentVis`.

15. **Optional: reflective water (enhancement, off by default).** Once
    item 14's water warp is in and X11's profiling shows frame time to
    spare: a cube map baked per map or per water area by the baker,
    sampled per pixel through the NV2A texture shader's reflection mode,
    with a bump map for ripples and a Fresnel term in the register
    combiners - one more texture stage on water pixels, no extra scene
    pass. Behind a cvar, off by default: it changes the original look.
    Not real-time planar reflection (a second scene pass per view with
    water, which the ~30 fps maps cannot afford). Unprototyped: first
    confirm which texture-shader modes pbkit exposes and the cube-map
    formats.

X9-T load-memory model for every retail map, baked at v12. It uses the
engine's world structures from lump counts, with 32-bit struct sizes and
per-allocation overhead. P is the classic prefix the engine holds while
building them; T+Tmp are `ref/nv2a`'s tables and load temporaries; G is
the GPU buffers, with the lightmap in brackets. The precache is calibrated
from xemu: SP 9.89, DM 8.47 MiB, the largest implied by 18 maps booted in
xemu. The budget is 32.8 MiB, measured before any map loads. "Load peak
free" is what is left while the engine builds the world with the prefix,
tables and temporaries held; "Steady free" is what is left after the GPU
buffers, with the xemu measurement in brackets where there is one. Checks:
- C1: the engine runs out building the world (a fatal `Sys_Error`).
- C2: `ref/nv2a`'s tables do not fit.
- C3: the GPU buffers do not fit.
- MP: `Mod_CalcPHS`, which runs only with `maxclients` > 1.

| Map | Ledger % | E engine | P prefix | T+Tmp | G GPU (lightmap) | Load peak free | Steady free | Flags |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| boot_camp | 22.5 | 7.00 | 2.60 | 1.44 | 4.62 (2.48) | 21.8 | 11.9 (xemu 13.4) | - |
| bounce | 12.2 | 3.05 | 1.33 | 0.56 | 2.61 (1.48) | 27.9 | 18.4 (xemu 19.8) | - |
| c0a0 | 11.1 | 2.81 | 1.26 | 0.51 | 2.40 (1.33) | 28.2 | 17.4 (xemu 18.8) | - |
| c0a0a | 10.8 | 2.75 | 1.21 | 0.50 | 2.34 (1.22) | 28.3 | 17.6 (xemu 19.3) | - |
| c0a0b | 14.2 | 3.64 | 1.60 | 0.64 | 3.05 (1.47) | 26.9 | 15.9 (xemu 16.9) | - |
| c0a0c | 14.7 | 4.08 | 1.89 | 0.67 | 3.17 (1.91) | 26.2 | 15.3 (xemu 16.0) | - |
| c0a0d | 10.6 | 2.79 | 1.21 | 0.51 | 2.27 (1.08) | 28.3 | 17.6 (xemu 18.6) | - |
| c0a0e | 6.7 | 1.46 | 0.65 | 0.27 | 1.46 (0.67) | 30.4 | 19.9 (xemu 22.1) | - |
| c1a0 | 10.7 | 2.57 | 1.01 | 0.52 | 2.30 (0.94) | 28.7 | 17.8 (xemu 19.6) | - |
| c1a0a | 10.7 | 2.65 | 1.07 | 0.51 | 2.31 (0.89) | 28.6 | 17.7 (xemu 19.8) | - |
| c1a0b | 11.6 | 2.87 | 1.20 | 0.53 | 2.50 (1.03) | 28.2 | 17.3 (xemu 19.1) | - |
| c1a0c | 11.5 | 3.31 | 1.36 | 0.62 | 2.44 (1.00) | 27.5 | 16.8 (xemu 18.4) | - |
| c1a0d | 11.0 | 2.81 | 1.05 | 0.58 | 2.33 (0.86) | 28.4 | 17.5 (xemu 19.3) | - |
| c1a0e | 12.5 | 3.02 | 1.54 | 0.46 | 2.75 (1.62) | 27.8 | 16.9 (xemu 16.9) | - |
| c1a1 | 12.0 | 3.28 | 1.35 | 0.61 | 2.56 (1.06) | 27.6 | 16.8 (xemu 18.0) | - |
| c1a1a | 10.8 | 2.63 | 0.98 | 0.55 | 2.31 (0.81) | 28.6 | 17.7 (xemu 19.3) | - |
| c1a1b | 13.9 | 4.49 | 1.85 | 0.84 | 2.90 (1.59) | 25.6 | 15.1 | - |
| c1a1c | 14.5 | 4.32 | 1.76 | 0.83 | 3.04 (1.53) | 25.9 | 15.1 | - |
| c1a1d | 4.4 | 1.22 | 0.52 | 0.23 | 0.94 (0.50) | 30.8 | 20.6 | - |
| c1a1f | 15.8 | 4.14 | 1.70 | 0.79 | 3.36 (1.62) | 26.2 | 15.0 | - |
| c1a2 | 15.3 | 5.19 | 1.98 | 1.05 | 3.12 (1.67) | 24.6 | 14.1 | - |
| c1a2a | 14.0 | 5.05 | 1.82 | 1.05 | 2.80 (1.41) | 24.9 | 14.5 | - |
| c1a2b | 15.0 | 5.36 | 1.99 | 1.10 | 3.03 (1.55) | 24.4 | 13.9 | - |
| c1a2c | 8.5 | 2.64 | 1.02 | 0.53 | 1.76 (0.91) | 28.6 | 18.2 | - |
| c1a2d | 4.9 | 1.49 | 0.57 | 0.31 | 1.01 (0.50) | 30.4 | 20.3 | - |
| c1a3 | 13.8 | 3.68 | 1.46 | 0.72 | 2.93 (1.42) | 26.9 | 15.9 | - |
| c1a3a | 12.6 | 3.57 | 1.40 | 0.71 | 2.65 (1.44) | 27.1 | 16.3 | - |
| c1a3b | 5.5 | 1.43 | 0.56 | 0.28 | 1.16 (0.59) | 30.5 | 20.2 | - |
| c1a3c | 4.3 | 0.91 | 0.40 | 0.16 | 0.95 (0.44) | 31.3 | 21.0 | - |
| c1a3d | 6.0 | 1.62 | 0.64 | 0.32 | 1.28 (0.56) | 30.2 | 19.8 | - |
| c1a4 | 11.5 | 3.35 | 1.48 | 0.60 | 2.43 (1.42) | 27.4 | 16.8 | - |
| c1a4b | 11.2 | 3.46 | 1.41 | 0.66 | 2.33 (1.16) | 27.3 | 16.8 | - |
| c1a4d | 14.0 | 4.49 | 1.74 | 0.89 | 2.88 (1.55) | 25.7 | 15.1 | - |
| c1a4e | 7.9 | 2.19 | 0.95 | 0.39 | 1.70 (0.94) | 29.3 | 18.8 | - |
| c1a4f | 14.9 | 4.49 | 1.86 | 0.85 | 3.13 (1.86) | 25.6 | 14.8 | - |
| c1a4g | 15.3 | 4.95 | 2.11 | 0.90 | 3.19 (2.05) | 24.8 | 14.3 | - |
| c1a4i | 14.0 | 3.90 | 1.77 | 0.68 | 2.99 (1.81) | 26.4 | 15.7 | - |
| c1a4j | 4.0 | 1.25 | 0.52 | 0.23 | 0.83 (0.45) | 30.8 | 20.7 | - |
| c1a4k | 8.2 | 1.95 | 0.94 | 0.33 | 1.78 (1.11) | 29.6 | 19.0 | - |
| c2a1 | 13.1 | 4.39 | 1.72 | 0.84 | 2.70 (1.38) | 25.9 | 15.4 | - |
| c2a1a | 15.9 | 5.13 | 1.88 | 1.05 | 3.26 (1.53) | 24.7 | 14.0 | - |
| c2a1b | 6.7 | 2.28 | 0.84 | 0.47 | 1.35 (0.64) | 29.2 | 19.0 | - |
| c2a2 | 7.9 | 2.05 | 0.85 | 0.40 | 1.68 (0.84) | 29.5 | 19.0 | - |
| c2a2a | 16.3 | 5.20 | 2.06 | 1.01 | 3.37 (1.86) | 24.5 | 13.8 | - |
| c2a2b1 | 15.2 | 4.53 | 1.72 | 0.91 | 3.17 (1.50) | 25.6 | 14.7 | - |
| c2a2b2 | 12.3 | 3.20 | 1.28 | 0.63 | 2.63 (1.22) | 27.7 | 16.8 | - |
| c2a2c | 13.3 | 4.11 | 1.53 | 0.84 | 2.76 (1.38) | 26.3 | 15.6 | - |
| c2a2d | 14.9 | 4.51 | 1.80 | 0.87 | 3.12 (1.61) | 25.6 | 14.8 | - |
| c2a2e | 17.9 | 5.61 | 2.22 | 1.09 | 3.73 (2.03) | 23.9 | 13.0 | - |
| c2a2f | 13.8 | 4.34 | 1.68 | 0.85 | 2.86 (1.55) | 25.9 | 15.3 | - |
| c2a2g | 12.3 | 3.32 | 1.33 | 0.63 | 2.60 (1.08) | 27.5 | 16.6 | - |
| c2a2h | 11.3 | 2.96 | 1.16 | 0.57 | 2.41 (0.97) | 28.1 | 17.2 | - |
| c2a3 | 12.3 | 3.73 | 1.52 | 0.70 | 2.58 (1.38) | 26.8 | 16.2 | - |
| c2a3a | 12.2 | 3.16 | 1.18 | 0.65 | 2.60 (1.12) | 27.8 | 16.8 | - |
| c2a3b | 15.4 | 3.64 | 1.60 | 0.67 | 3.33 (1.83) | 26.9 | 15.6 | - |
| c2a3c | 11.5 | 3.19 | 1.21 | 0.64 | 2.43 (1.09) | 27.8 | 17.0 | - |
| c2a3d | 11.6 | 3.25 | 1.31 | 0.63 | 2.44 (1.38) | 27.6 | 16.9 | - |
| c2a3e | 8.9 | 2.00 | 0.87 | 0.38 | 1.94 (1.02) | 29.6 | 18.8 | - |
| c2a4 | 6.7 | 2.05 | 0.85 | 0.35 | 1.41 (0.66) | 29.6 | 19.3 | - |
| c2a4a | 14.0 | 5.05 | 1.86 | 1.00 | 2.83 (1.52) | 24.9 | 14.5 | - |
| c2a4b | 16.0 | 4.72 | 1.90 | 0.91 | 3.36 (1.88) | 25.3 | 14.4 | - |
| c2a4c | 17.9 | 5.75 | 2.14 | 1.17 | 3.68 (2.00) | 23.7 | 12.9 | - |
| c2a4d | 8.7 | 2.07 | 0.79 | 0.41 | 1.87 (0.67) | 29.5 | 18.8 | - |
| c2a4e | 17.8 | 5.87 | 2.11 | 1.19 | 3.63 (1.81) | 23.6 | 12.8 | - |
| c2a4f | 12.9 | 3.90 | 1.43 | 0.79 | 2.68 (1.17) | 26.7 | 15.9 | - |
| c2a4g | 6.3 | 1.31 | 0.53 | 0.24 | 1.39 (0.44) | 30.7 | 20.1 | - |
| c2a5 | 11.6 | 3.94 | 1.60 | 0.69 | 2.40 (1.28) | 26.6 | 16.2 | - |
| c2a5a | 6.9 | 2.17 | 0.85 | 0.39 | 1.43 (0.69) | 29.4 | 19.1 | - |
| c2a5b | 10.5 | 2.83 | 1.13 | 0.51 | 2.24 (0.84) | 28.3 | 17.6 | - |
| c2a5c | 8.0 | 2.00 | 0.82 | 0.37 | 1.73 (0.69) | 29.6 | 19.0 | - |
| c2a5d | 7.6 | 2.00 | 0.81 | 0.38 | 1.62 (0.70) | 29.6 | 19.1 | - |
| c2a5e | 18.1 | 5.25 | 2.06 | 1.01 | 3.79 (1.73) | 24.5 | 13.3 | - |
| c2a5f | 16.7 | 5.88 | 2.20 | 1.17 | 3.39 (1.67) | 23.6 | 13.0 | - |
| c2a5g | 9.8 | 2.42 | 1.00 | 0.45 | 2.12 (0.86) | 28.9 | 18.1 | - |
| c2a5w | 8.7 | 2.57 | 1.09 | 0.45 | 1.83 (0.91) | 28.7 | 18.3 | - |
| c2a5x | 8.8 | 2.85 | 1.25 | 0.50 | 1.85 (1.03) | 28.2 | 17.9 | - |
| c3a1 | 15.8 | 4.18 | 1.74 | 0.78 | 3.37 (1.62) | 26.1 | 15.0 | - |
| c3a1a | 14.3 | 3.55 | 1.39 | 0.70 | 3.06 (1.22) | 27.1 | 15.9 | - |
| c3a1b | 12.0 | 2.91 | 1.24 | 0.55 | 2.59 (1.27) | 28.1 | 17.1 | - |
| c3a2 | 7.7 | 1.37 | 0.57 | 0.28 | 1.71 (0.61) | 30.6 | 19.7 | - |
| c3a2a | 16.6 | 4.64 | 1.85 | 0.94 | 3.49 (2.05) | 25.4 | 14.3 | - |
| c3a2b | 9.0 | 2.06 | 0.95 | 0.36 | 1.97 (1.06) | 29.4 | 18.7 | - |
| c3a2c | 15.3 | 4.19 | 1.64 | 0.83 | 3.24 (1.64) | 26.1 | 15.0 | - |
| c3a2d | 11.4 | 2.43 | 1.14 | 0.42 | 2.51 (1.31) | 28.8 | 17.7 | - |
| c3a2e | 10.1 | 2.45 | 0.95 | 0.51 | 2.15 (1.03) | 28.9 | 18.1 | - |
| c3a2f | 6.2 | 1.18 | 0.51 | 0.22 | 1.37 (0.53) | 30.9 | 20.2 | - |
| c4a1 | 8.7 | 2.98 | 1.33 | 0.50 | 1.81 (1.12) | 28.0 | 17.9 | - |
| c4a1a | 9.0 | 3.28 | 1.34 | 0.58 | 1.85 (0.97) | 27.6 | 17.5 | - |
| c4a1b | 10.8 | 4.56 | 1.80 | 0.80 | 2.08 (1.19) | 25.6 | 15.8 | - |
| c4a1c | 5.3 | 1.51 | 0.67 | 0.26 | 1.14 (0.55) | 30.4 | 20.1 | - |
| c4a1d | 9.4 | 3.07 | 1.30 | 0.55 | 1.96 (1.12) | 27.9 | 17.6 | - |
| c4a1e | 8.2 | 2.82 | 1.27 | 0.47 | 1.71 (1.08) | 28.2 | 18.1 | - |
| c4a1f | 3.3 | 0.79 | 0.34 | 0.14 | 0.71 (0.23) | 31.5 | 21.3 | - |
| c4a2 | 5.7 | 1.55 | 0.75 | 0.23 | 1.24 (0.77) | 30.3 | 20.0 | - |
| c4a2a | 5.6 | 1.52 | 0.68 | 0.25 | 1.21 (0.59) | 30.4 | 20.0 | - |
| c4a2b | 4.4 | 0.81 | 0.43 | 0.12 | 0.98 (0.47) | 31.4 | 21.1 | - |
| c4a3 | 14.7 | 4.39 | 2.07 | 0.69 | 3.14 (1.89) | 25.7 | 15.0 | - |
| c5a1 | 17.8 | 6.19 | 2.64 | 1.04 | 3.69 (2.30) | 22.9 | 12.5 | - |
| contamination | 38.2 | 10.96 | 3.96 | 2.36 | 7.94 (3.94) | 15.5 | 4.2 | - |
| crossfire | 11.6 | 3.09 | 1.20 | 0.60 | 2.46 (1.05) | 27.9 | 18.4 (xemu 19.1) | - |
| datacore | 11.2 | 3.18 | 1.20 | 0.66 | 2.34 (1.17) | 27.8 | 18.5 | - |
| disposal | 43.4 | 11.22 | 4.43 | 2.26 | 9.24 (4.41) | 14.9 | 2.7 (xemu 2.7) | - |
| doublecross | 14.9 | 3.92 | 1.51 | 0.76 | 3.17 (1.31) | 26.6 | 16.8 | - |
| frenzy | 5.9 | 1.53 | 0.65 | 0.29 | 1.25 (0.69) | 30.3 | 21.4 | - |
| gasworks | 10.3 | 2.62 | 0.94 | 0.56 | 2.17 (0.91) | 28.7 | 19.2 | - |
| hldemo1 | 10.8 | 3.34 | 1.34 | 0.64 | 2.25 (1.11) | 27.5 | 18.4 | - |
| hldemo2 | 14.7 | 4.79 | 1.78 | 0.96 | 3.02 (1.39) | 25.3 | 16.0 | - |
| hldemo3 | 11.6 | 3.59 | 1.41 | 0.68 | 2.41 (1.25) | 27.1 | 18.0 | - |
| lambda_bunker | 13.8 | 3.38 | 1.36 | 0.65 | 2.97 (1.31) | 27.4 | 17.6 | - |
| pool_party | 31.4 | 10.76 | 4.18 | 2.15 | 6.41 (2.94) | 15.7 | 6.0 | - |
| rapidcore | 11.2 | 3.25 | 1.35 | 0.61 | 2.37 (1.09) | 27.6 | 18.4 | - |
| rocket_frenzy | 78.0 | 25.67 | 8.71 | 5.73 | 15.74 (7.89) | -7.3 | -20.1 | C1 ENGINE OOM building world (needs 34.4 of 32.8); C3 GPU buffers do not fit (20.1 MiB short); MP maxclients>1: Mod_CalcPHS needs 74.6 MiB more at load |
| rustmill | 18.4 | 5.51 | 2.11 | 1.05 | 3.83 (1.94) | 24.1 | 14.4 | - |
| snark_pit | 7.7 | 1.94 | 0.74 | 0.40 | 1.64 (0.75) | 29.7 | 20.5 | - |
| stalkyard | 6.7 | 1.75 | 0.68 | 0.36 | 1.43 (0.70) | 30.0 | 21.0 | - |
| subtransit | 13.3 | 3.99 | 1.56 | 0.78 | 2.77 (1.44) | 26.5 | 17.1 | - |
| t0a0 | 15.8 | 4.63 | 1.77 | 0.92 | 3.29 (1.75) | 25.5 | 14.5 | - |
| t0a0a | 16.9 | 4.66 | 1.81 | 0.92 | 3.57 (1.81) | 25.4 | 14.2 | - |
| t0a0b | 21.6 | 5.88 | 2.42 | 1.13 | 4.57 (2.78) | 23.4 | 11.9 | - |
| t0a0b1 | 13.6 | 3.53 | 1.46 | 0.69 | 2.90 (1.66) | 27.1 | 16.1 | - |
| t0a0b2 | 10.7 | 2.56 | 1.10 | 0.48 | 2.32 (1.30) | 28.7 | 17.8 | - |
| t0a0c | 11.1 | 2.82 | 1.10 | 0.57 | 2.37 (1.14) | 28.3 | 17.4 | - |
| t0a0d | 7.1 | 1.57 | 0.62 | 0.32 | 1.54 (0.59) | 30.3 | 19.6 | - |
| undertow | 9.6 | 2.37 | 1.02 | 0.44 | 2.08 (1.09) | 29.0 | 19.6 | - |
| xen_dm | 5.6 | 1.62 | 0.70 | 0.28 | 1.19 (0.67) | 30.2 | 21.4 | - |

X9-M onward (skinning, dynamic lighting, the frame-time exit criterion
itself) sketched in this section's own bullets below, not started.

- ~~Per-map atlas instead of texture arrays — the baker already produces
  it.~~ **Reversed, X9-K, 2026-09-22: the shared atlas + per-vertex remap
  could not represent a tiling texture correctly (see that entry above) -
  the Xbox target now bakes one independently POT-resampled DXT1 image per
  texture and addresses it with native hardware WRAP, no atlas at all.**
- Vertex format with no integer attributes: FLOAT1-4, D3DCOLOR and normalised
  SHORT only.
- ~~Register combiners for `diffuse × lightmap × 2` and native alpha test.~~
  **Done, X9-C (combiner) and X9-I (alpha test) above.**
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
  `tools/asset-studio`, the PC baker — X9 never touches it, XB is where the
  fix belongs, and XB's own section below already names the problem
  correctly. What belongs here instead: verify on hardware that XB's fix
  produced a correct swizzle, since xemu is not validation for this. **Still
  wrong as of 2026-09-15's X4 specialist review: `UiPacker.cpp:379` calls
  `swizzleNV2A` on DXT1 blocks unconditionally for Xbox output, and
  `TextureOpt.cpp:110-132` is unchanged.** Nobody has actually fixed this
  yet — "XB already lists this fix" meant "XB's todo list", not "XB's done
  list", and this bullet's own past-tense "XB's fix" read like the latter.
  Corrected here so this doesn't get counted as closed a second time; the
  real fix (linear-by-block-row layout for `D3DFMT_DXT1`, no Morton step)
  is still XB's work, not started.
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

## XG — Gameplay-fidelity bugs seen live (open ledger and triage plan)

**Status: open, drafted 2026-09-15.** The place for bugs a human sees while
playing a real build - as opposed to bring-up blockers, which each X phase
owns. Rule for every row: no fix is attempted from a hypothesis; each
candidate cause gets the cheapest test that would eliminate it, in order,
and disproofs are recorded here so they are not re-run (`AGENTS.md`,
"Do not guess"; `lessons.md`'s meta-rules).

### XG.1 — Two yaw errors in `c0a0` (player spawns facing backwards; the guard at the door faces sideways) — closed on xemu, 2026-09-16; hardware pending

**Observed** (2026-09-15, xemu, the X7.4 boot disc, `ref_soft`, real pad):
(a) the player starts the tram ride facing the wrong way; (b) the security
guard banging on the door - a `scripted_sequence` at the tram's end - stands
turned to the side instead of facing the door. Both are *yaw* wrong, on
the same build, in the same map; one shared cause is likelier than two
unrelated bugs, and the first suspicion was angle math ("is pi wrong?").
They are still tracked as two: **XG.1a** is the player's *view* yaw
(`cl.viewangles`, a client-side value fed by `svc_setangle` and then by
input), **XG.1b** is the guard's *model* yaw (an entity's `angles`,
delivered by delta and transformed by the renderer) - different code
paths, so a candidate cause must explain and clear BOTH before it closes
XG.1; a cause that clears one closes that half only (review, 2026-09-15:
the first draft's "stop at the first named cause" would have let the
player half close the guard half).

**Eliminated already, by test, same day:**

- **The Xbox `atof`.** nxdk's pdclib declares `atof`/`strtod` but never
  implemented them (its own `TODO` at `pdclib/include/stdlib.h:29`); this
  port's `compat/xbox/xbox_crt_ext.c` supplies a hand-written `atof`, and
  the HL SDK routes every map angle through `atof` (`dlls/util.cpp:1412`
  in `UTIL_StringToVector`, `:2018` for scalar fields) - a wrong parser
  there would make every entity face yaw 0, which is exactly what both
  symptoms look like. Tested on the host, since it is plain C: compile
  `compat/xbox/xbox_crt_ext.c` with `-Datof=xbox_atof`, call it on
  `"180"`, `"270"`, `"-90"`, `"45.5"`, `"  90"`, `"0 270 0"` (the
  tokenised form `UTIL_StringToVector` hands it), `"270 0"`, `"-1"`,
  `"1e2"`, `".5"`, `"360"`, `"abc"` - all twelve correct. Not the cause.

**Cause found, 2026-09-15 - one cause, both halves, read from source and
measured at both ends.** Experiment 1 turned out not to be cheap here after
all: the only PC build in the tree is the dedicated/test configuration, and
a PC client needs an SDL2 development package this machine does not have -
noted, not done. So experiment 2 ran first, with `tools/xemu-debug.py`'s
`GdbRemote` driving breakpoints from the disc's own linker map
(`.sweep/game-hl/xbe-build.map`; guest identity confirmed with
`verify-image` before anything was read). **Probe A** - `MSG_WriteVec3Angles`,
the `svc_setangle` write at `sv_frame.c:545` - fired at spawn with
`(0, 0, 0)`, in a burst of ~8 writes within 20 ms, then never again in a
90 s window (so it is not re-sent every frame; a first reading of the hits
as "every frame" was wrong and is dropped). **Then a correction that
matters, found by reading the spawn spot the SDK actually selected
(`EntSelectSpawnPoint`'s returned edict: origin `(24, 1517, 47)`): those
first probes were not running `c0a0` at all.** The test disc's
`userconfig.cfg` - the X7.4 boot injection divergence #49 describes,
test-only, never in the repo - does `map c1a3`, and `c1a3`'s player start
has no `"angle"` key, so `(0, 0, 0)` there is correct. (That is also the
answer to X6's "loads straight into gameplay" curiosity: `userconfig.cfg`,
not a command line.) Re-run with the disc's `userconfig.cfg` set to
`map c0a0` - the map you reported, whose `info_player_start` says
`"angle" "180"` (read from `c0a0.bsp`'s entity lump) - and with
divergence #54 in the build, probe A writes **`(0, 180, 0)`**. The
pre-fix `c0a0` number was not captured with symbols (the old build's map
was overwritten by the fixed link); the pre-fix state rests on your
observation and on the pdclib source, which is unambiguous. Delivery, the
pad and the renderer are cleared for XG.1a either way: the value is right
at the server before the network. Working back from `sv_game.c:5013-5029`: the engine
turns `"angle"` into `"angles"` with `Q_snprintf( "%g %g %g", ... )`, which
is `Q_vsnprintf` → plain `vsnprintf` on this platform (`public/crtlib.c:
415-422`) → nxdk's pdclib, whose printf core **parses `%f`/`%e`/`%g` and
does nothing**: `_PDCLIB_print.c:452-458` is `case 'f': ... case 'G':
break;` - no output, and the `double` is never consumed, so every later
argument is misaligned too. The SDK's `UTIL_StringToVector` parses the
resulting `"  "` as `0 0 0`, `CBasePlayer::Spawn` copies the spot's zero
angles and sets `fixangle`, and probe A sees `(0, 0, 0)`. XG.1b is the
same path: the guard by the door is placed with `"angle" "270"`
(`runnerguy3` - the one `monster_barney` in `c0a0` placed with an
`"angle"` key; the other two carry none), so it faces yaw 0 - "sideways". Every `"angle"`-placed
entity on the disc has been facing yaw 0, and every `%f`/`%g` on this
platform has printed nothing. Fix: divergence #54, `compat/xbox/
xbox_printf.c`, host-verified against the real C library, and confirmed
live on `c0a0`: probe A reads `(0, 180, 0)` with it, and the picture agrees
- two discs built from the same `c0a0`-booting content, one with the old
XBE and one with #54, captured at the same point of the tram ride
(`.sweep/xg1-shots/c0a0-old-spawn.png` / `c0a0-printf-spawn.png`, Valve's
art, not committed): the old build looks backward into the car (seats and
the rear door ahead), the fixed one looks forward down the track. **XG.1a
is closed on xemu.**

**XG.1b, 2026-09-16 - closed on xemu by direct observation, entity identity
confirmed by reading the BSP, not inferred.** Booted the same #54-fixed
disc through the whole tram ride to the guard's door scene and watched it
live (your own words: "I visually verified looks ok") - the guard faces
the door, not sideways. Not closed on a screenshot: the scene was missed
across sixteen timed captures taken while hunting for it blind
(`.sweep/xg1-shots/xg1b-check1.png` through `check15.png`, not committed -
useful only as a record of the ride's own waypoints: the "Lisa
Mennet"/"Aaron Stackpole"/"Robin Walker" credit panels, the canyon cut
with a stray white HOM-looking triangle worth a second look some other
day, and the reactor descent shaft's "EDUCATION:"/"ASSIGNMENT:"/
"ADMINISTRATIVE SPONSOR:" panels), and a `changelevel` partway through
that this session did not expect at first (review round 1 flagged the
resulting contradiction - the account both trusted `runnerguy3`'s `c0a0`
identity and admitted it might not hold). Checked directly rather than
argued: the tram ride is not one map but a chain of six, each with its own
`trigger_changelevel` read straight from every segment's own entity lump -
`c0a0`→`c0a0a`→`c0a0b`→`c0a0c`→`c0a0d`→`c0a0e`→`c1a0`, `c0a0`'s only
forward target being `c0a0a` - and the one `changelevel` this session
actually saw is necessarily that first hop, since a single boot starting
at `c0a0` cannot reach a later segment without passing through it. Every
waypoint above, credit panels included, therefore happened inside `c0a0`
itself, the same map `runnerguy3` was already confirmed to be placed in
(re-verified here by decoding `c0a0.bsp`'s entity lump directly: version
30, `runnerguy3` present, three `monster_barney` entities total, absent
from all five later segments) - the guard scene you watched is the `c0a0`
scene this row's causal argument is about, not a different map's. A
same-session attempt to read the guard's angle directly -
breaking on the server DLL's `UTIL_StringToVector` (`0x39f850` in
`xbe-build.map`, cdecl `(float *pVector, const char *pString)`) at its
return address and reading the output vector - returned `(0, 0, 0)` for
an `"origin"`-shaped input (`"-1784 24 -500"`) that plainly should not
parse to all zeroes, repeated identically many times in a few
milliseconds of guest time. Not trusted and not chased: far likelier a
bug in that probe script (a breakpoint/continue mechanics issue, unproven)
than a real second parser bug, given the direct observation already
closed this half and the shared-cause argument (XG.1a and XG.1b's
`"angle"` reach `UTIL_StringToVector` through the identical, already
host-verified `xbox_printf.c` conversion - one code path, no per-entity
branch) - recorded so a future session does not reuse that address/method
without re-deriving it. **XG.1 is closed on xemu, both halves. Hardware
remains**, per the usual rule. Two more
findings from the same session, recorded rather than chased: nxdk's
`strtod`/`strtof`/`strtold` are `assert(0); return 0.0` stubs
(`deps/nxdk/lib/xboxrt/libc_extensions/stdlib_ext_.c:89-105`) - not on
this path (the SDK uses `atof`, the engine `Q_atof`), but linked code does
call one: `3rdparty/mainui/miniutl/utlbuffer.cpp:1039` calls `strtod`
(found in review; whether that line is reached at run time is unknown),
and whether the stub aborts or returns 0 depends on how nxdk was built
(`NDEBUG` not confirmed either way) - a caller reaching it gets one or the
other, never a parsed number; and the 3000-call `vsnprintf` scan never
saw a `%g` after `SV_LoadFromFile`, most likely because per-entity logging
spends that budget long before the first `"angle"` entity - not re-run
with a larger budget, so a probe-budget note, not a code finding. The second pad reader (`inputw32.cpp`, below)
stands as its own finding for XM regardless. Kept below for the record:
the hypotheses this displaced and the probes as designed.

**Verified facts that constrain the rest** (read, not assumed):

- The engine, not the SDK, turns a map's `"angle"` key into `"angles"`
  before the game DLL sees it, using its own `Q_atof`, with the `-1`/`-2`
  up/down special cases (`engine/server/sv_game.c:5013-5029`) - portable
  code, shared with PC.
- The player's facing at spawn is a server → client message: the server
  writes `svc_setangle` with `v.angles` whenever `v.fixangle == 1`
  (`engine/server/sv_frame.c:541-554`), and the client copies it straight
  into `cl.viewangles` (`CL_ParseSetAngle`, `engine/client/parse/
  cl_parse.c:1237-1240`). Anything that then *adds* to `cl.viewangles`
  every frame can undo it.
- A studio model's orientation is `R_StudioSetUpTransform` →
  `Matrix3x4_CreateFromEntity( angles, origin )` (`ref/soft/r_studio.c:
  523-541`), with the "stupid quake bug" pitch flip gated on
  `ENGINE_COMPENSATE_QUAKE_BUG` (`:534-535`) - pitch only; a yaw error is
  not that flag.
- nxdk's libm is one x87 instruction per function (`deps/nxdk/lib/pdclib/
  platform/xbox/functions/math/`: `sin.c`/`cos.c` are `fsin`/`fcos`,
  `atan2.c` is `fpatan` with `x` in `st(0)` and `y` in `st(1)`, `fmod.c` a
  single `fprem`), and `M_PI` is the full constant (`platform/xbox/include/
  math.h:45`). Read against the x87 operand rules, the orders are right,
  and one `fprem` pass is exact for angle-sized quotients - a lower prior
  than it first seemed, but not eliminated: no test has run it yet.
- **The pad is read twice.** X6 confirmed the engine's own path
  (`in_joy.c` / `joy_sdl2.c`). Separately, the HL SDK's client DLL opens
  the same `SDL_GameController` itself (`deps/hlsdk/cl_dll/inputw32.cpp:
  748`), reads its axes (`:790-796`), and `IN_JoyMove` (`:953`) adds stick
  deflection to `viewangles` every frame (`:1034`, `:1083-1105`) when
  `in_joystick` is on - and retail's own `controller.cfg`, exec'd from
  `valve.rc`, sets `joystick "1"` and `joyadvanced "1"` with the DirectInput
  axis map (`joyadvaxisx 3`, `joyadvaxisy 1`, `joyadvaxisr 2`,
  `joyadvaxisz 4`). Two readers of one pad means at minimum double look
  sensitivity; and any rest offset in the SDK's axis read becomes a steady
  yaw drift, which in a still screenshot taken seconds after spawn looks
  exactly like "spawned facing the wrong way". This is a real finding on
  its own, whatever XG.1's cause turns out to be, and belongs to XM's
  controller work (one path, the engine's, should own the pad).

**Experiments, cheapest and most decisive first. Stop only when the named
cause explains and clears both XG.1a and XG.1b; otherwise the remaining
half keeps going down the list.**

1. **PC first.** Run this fork's PC build on the same `valve/` content,
   `map c0a0`. If both symptoms reproduce on PC, this is not an Xbox bug
   at all - look at what this fork changed in the engine or on the owned
   `deps/hlsdk` `resonance3d` branch (divergences #44, #49), and stop
   reading Xbox code.
2. **Read the values, don't infer them - as two ordered probes, because
   a single halt cannot compare server and client state that is not yet
   in flight.** On a normal new game, `SV_PutClientInServer` only calls
   the DLL's `pfnClientPutInServer`; its own inline `svc_setangle` is the
   load-game path only (`engine/server/sv_client.c:1294-1302`,
   `:1333-1356`, found in review), so the spawn angle reaches the client
   later, from `fixangle` in the next frame's clientdata
   (`sv_frame.c:540-554`) - a halt at `SV_PutClientInServer` would read a
   correct server edict beside a necessarily stale `cl.viewangles` and
   falsely blame the delivery path. Instead (`tools/xemu-debug.py break`
   on a `verify-image`-confirmed guest, offsets from
   `tools/xbox-offsetof.sh`): **probe A, server**, at `sv_frame.c:541`
   before `:554` clears it - the player edict's `v.angles` and
   `v.fixangle`, and for XG.1b the guard edict's `v.angles` and
   `v.ideal_yaw`; **probe B, client, at two points in the same datagram** - the server
   writes clientdata (carrying `svc_setangle`) before the entity update
   (`sv_frame.c:688-689`), and the client applies entity state only when
   the later `svc_packetentities`/`svc_deltapacketentities` is parsed
   (`cl_parse.c:2658-2666`, found in review round 2), so one halt cannot
   serve both halves: for XG.1a, on the return of `CL_ParseSetAngle`
   (`cl_parse.c:1237-1240`), `cl.viewangles` before any input has run
   that frame; for XG.1b, on the return of `CL_ParsePacketEntities` for
   that same datagram, the guard's `cl_entity_t` `curstate.angles`/
   `latched.prevangles`, correlated with the server frame sequence probe
   A was taken in; **probe C**, the same client reads one `Host_Frame`
   later, so a per-frame change is drift from input (see 3), not
   delivery. Compare
   all of it with the map's own numbers - `info_player_start`'s `"angle"`
   and the guard/`scripted_sequence` `"angles"` from the BSP entity lump.
   A right, B wrong → delivery; A already wrong → entity parsing or the
   SDK; A and B right, C moving → input; everything right and only the
   picture wrong → the renderer (see 4). Each verdict is per half.
3. **Turn off one pad reader at a time.** Repack the test disc with a
   `valve/userconfig.d/xg.cfg` (executed after `config.cfg`,
   `host.c:1346-1348`): first `joystick 0` (SDK reader off), then
   `joy_enable 0` (engine reader off). This experiment speaks to XG.1a
   only: the SDK reader touches the player's `viewangles`
   (`inputw32.cpp:1067-1089`) and nothing else, so it cannot explain the
   guard. If the player's facing is right with the SDK reader off, XG.1a
   is the double read - and the fix is XM's decision about which path
   owns the pad, recorded in the ledger, not a yaw hack - while XG.1b
   continues from probe A/B's own verdict.
4. **Only if 2 says the render is wrong:** a libm probe. Either a
   `tools/x4-triangle`-style standalone XBE, or a `XASH_ENGINE_TESTS`-style
   self-test (`engine/common/tests.h` has no math test today), printing
   `sinf`/`cosf`/`atan2f`/`fmodf` at 0/90/180/270 degrees through
   `DEG2RAD`/`M_PI_F` and `AngleVectors( (0,180,0) )` (expected forward
   `(-1,0,0)`) over the log channel. A wrong number here indicts the x87
   wrappers or the `-march=pentium3 -fno-builtin` float path; a right one
   eliminates libm for good.
5. **Cheap side check, any time:** confirm `GetEngineFeatures` is in the
   generated server export table (`tools/xbox-gen-exports.sh`'s output)
   and read `host.features` on the guest. A missing export silently drops
   `ENGINE_COMPENSATE_QUAKE_BUG` and the step-lerp features - pitch and
   smoothness, not yaw, so it cannot be XG.1's cause, but it is the kind
   of silent gap this port has hit before (#34) and costs one read.

**Fix policy.** Whatever 1-5 names gets fixed where it lives, in the
cheapest form on the ledger: an SDK cause on the owned `deps/hlsdk`
branch (#44's precedent, and then the same fix noted for `hlsdk-opfor`/
`hlsdk-bshift`), an engine cause with a ledger row, a double-read cause
as XM's pad-ownership decision. No `#ifdef` that adds 180 to a yaw.

**Exit:** both XG.1a and XG.1b gone on xemu, each with its cause named by
`file:line` here (one cause if it is genuinely shared, two if not), the PC
build unchanged in behaviour, and every hypothesis tried - including the
ones that failed - recorded in this section. **"PC unchanged" is met here
by construction, not by running experiment 1**: the named cause and fix
are `compat/xbox/xbox_printf.c`, a new file divergence #54 already
documents as compiled only for the Xbox target (its own merge-risk cell:
"not `#ifdef`-gated because the file is only ever compiled for this
target") - no PC translation unit, object, or link input changed, so
there is nothing for a PC build to regress on. That is a stronger claim
than a passing PC test would have been for this specific question (did
*this change* alter PC behaviour), though a weaker one than experiment 1
would have given for the ORIGINAL question (is this even an Xbox bug at
all, or would it reproduce on PC too) - that question went unanswered
(experiment 1, "not done" above; no SDL2 dev package on this machine) and
stays open only as a "was the diagnosis right" curiosity, not as a merge
risk: XG.1a's own probe already read the wrong value at the server, before
any network, renderer or platform-specific code ran, which the PC build
shares byte-for-byte - so the diagnosis does not depend on experiment 1
either. Real-hardware confirmation
per the usual rule.

**Tooling debt found on the way (own item, not XG.1's).**
`tools/xbox-dedicated-link-check.sh` compiles the generated server and
client export tables (`generated_lib_server_exports.c`,
`generated_lib_client_exports.c`, its lines 115-129) but not the two
`ref` ones that `tools/xbox-link-check.sh:627-670` compiles
(`generated_lib_ref_null_exports.c`, `generated_lib_ref_soft_exports.c`),
so since X5 registered both renderers in the static library table the
dedicated link has reported `_lib_ref_null_exports` and
`_lib_ref_soft_exports` undefined. It predates #54 and is unrelated to it;
the dedicated sweep still compiles cleanly and the response file's order
is still what #54's ledger row states. Fix: either compile the two `ref`
tables there too, or - since a dedicated server has no renderer - give the
dedicated build its own `ref`-less library table. Whichever, the script
should then run green and be re-added to the pre-PR checks, where a red
result has been quietly ignorable for too long.

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
