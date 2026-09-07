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
