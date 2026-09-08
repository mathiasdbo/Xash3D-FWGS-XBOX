# Changelog

Changes made by this fork. Upstream's own history is intact and reachable with
`git log upstream/master`; this file records only what Resonance3D adds.

## [Unreleased]

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
