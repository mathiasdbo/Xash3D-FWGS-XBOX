# Changelog

Changes made by this fork. Upstream's own history is intact and reachable with
`git log upstream/master`; this file records only what Resonance3D adds.

## [Unreleased]

### Changed — the plan, after checking it against the code

`docs/r3d/xbox-performance-review.md` read `optimization-transfer.md` and the
X-phases against `ref/gl`, the baker and nxdk. Of its nineteen real items,
four were already in the tree, one is present but disabled, and seven are baker
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
