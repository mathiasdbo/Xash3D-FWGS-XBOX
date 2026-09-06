# Changelog

Changes made by this fork. Upstream's own history is intact and reachable with
`git log upstream/master`; this file records only what Resonance3D adds.

## [Unreleased]

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
- **183 of 218 translation units compile for Xbox.** Reproduce with
  `MSYSTEM=MINGW64 /c/msys64/usr/bin/bash tools/xbox-sweep.sh engine filesystem
  public common compat`. Of the 35 that do not:
  - **6 are the parked networking group** — `cl_steam.c`, `net_chan.c`,
    `net_ws.c`, `xrcon.c` and the two `common/http/` files.
  - **29 are platform backends for targets that are not Xbox** — `android`,
    `apple`, `dos`, `irix`, `linux`, `misc`, `nswitch`, `posix`, `psvita`,
    `sdl1`, `sdl3` and `win32`. Xbox's own backend belongs in
    `engine/platform/xbox/`, which does not exist yet; until it does, no
    `engine/platform/` subdirectory is claimed by this target.

  So every file this target currently claims compiles, except networking.
- **`tools/xbox-env.sh`**, **`tools/xbox-cc.sh`** and **`tools/xbox-sweep.sh`**
  — the build recipe, a single-file compile harness, and a whole-directory
  sweep that judges by the compiler's exit status.
- **`.claude/workflows/pr-review.js`** — five review lenses (posix, xbox,
  upstream, build, perf) selected from the diff, each finding then put to three
  adversarial refuters that default to rejecting it. Required on every PR; see
  `AGENTS.md`.
- **`.gitattributes`** extended so our text files normalise to LF.

### Fixed

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

- `engine/platform/xbox/`, and whether `engine/platform/sdl2/` can host it.
- The debug and network log channel — first on the list per the PS3 port's
  ordering, because a console has no stdout and silent failure is expensive.
- `DEST_OS`, `VIDEO_XBOX`/`INPUT_XBOX` in `common/backends.h`, the
  `common/defaults.h` branch.
- Root-directory detection: `SDL_GetBasePath` returns `NULL` on nxdk and there
  is no `getcwd`, so the engine has no way to find `valve/` yet.
- Networking — six translation units, parked after pricing both routes.
  Upstream's `XASH_NO_NETWORK` stub is not self-contained (it declares the
  socket structs opaquely, and `net_ws.c` uses them by value), and the lwIP
  route needs a `lwipopts.h` plus an `arch/cc.h` port layer for code X3
  deletes. Completing upstream's stub and sending it back is the right fix.
- XBE packaging from this tree rather than from an nxdk sample.
