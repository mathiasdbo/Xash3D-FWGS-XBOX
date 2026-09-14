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
