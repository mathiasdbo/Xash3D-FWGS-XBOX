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
- **`filesystem/` compiles for Xbox** — 12 of 12 translation units.
- **`tools/xbox-env.sh`** and **`tools/xbox-cc.sh`** — the build recipe and a
  single-file compile harness.
- **`.gitattributes`** extended so our text files normalise to LF.

### Fixed

- `filesystem/sys.c` calls `lseek` and `dup` without including `<unistd.h>`,
  relying on it arriving transitively. Correct everywhere, not just here —
  divergence #7, and a candidate to send upstream.

### Changed

- `filesystem/sys.c`: ten `#if XASH_WIN32` blocks widened to
  `#if XASH_WIN32 && !XASH_XBOX`. They use the MSVC wide-char CRT, which nxdk
  does not have, and each already had a POSIX `#else` branch. Divergence #6.

### Still open in X0

- `engine/platform/xbox/`, and whether `engine/platform/sdl2/` can host it.
- The debug and network log channel — first on the list per the PS3 port's
  ordering, because a console has no stdout and silent failure is expensive.
- `DEST_OS`, `VIDEO_XBOX`/`INPUT_XBOX` in `common/backends.h`, the
  `common/defaults.h` branch.
- `public/miniz.c`, which does not compile for nxdk.
- XBE packaging from this tree rather than from an nxdk sample.
