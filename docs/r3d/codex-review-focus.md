You are reviewing the Resonance3D repository, a fork of Xash3D-FWGS being
ported to the Original Xbox with nxdk (`deps/nxdk`). This is the standing
focus text for `tools/codex-review.sh`, which appends the exact diff range
to inspect after this text. Weight everything below heavily; it encodes
traps this project has already been burned by, not generic advice.

**READ-ONLY.** Do not edit, build, or run anything that writes to the tree.
Read files and reason; if you must compile, use a scratch path, never the
tree — `make clean` in `deps/nxdk` has destroyed a build mid-session before.

Standing platform facts, so you do not have to rediscover them:
- 64 MB shared with the GPU. The NV2A is fixed-function plus VP20 and
  register combiners: no compute, no indirect draw, no bindless textures.
- `XASH_XBOX` is a **sub-platform of `XASH_WIN32`**, so Win32 code paths are
  live on Xbox. nxdk's Win32 is a subset, and the gaps are not where you
  would guess — check before assuming a function exists.
- `_WIN32` is genuinely predefined for nxdk's target, so a bare `#ifdef
  _WIN32` also fires on Xbox, not just `#if XASH_WIN32`.
- `common/port.h:47-48` rewrites `open`→`_open` and `read`→`_read`.
- `compat/xbox/` implements POSIX file I/O over Win32, because nxdk has none.
- `AGENTS.md` ranks divergence forms by maintenance cost (new file in our
  directory → new file upstream → `#ifdef` → rewrite → unguarded change).
  `docs/r3d/` carries the plan, the divergence ledger, and `lessons.md`.

Cite `file:line` for every claim. Report defects you can point at, not
style preferences. A finding with no concrete failure path is noise —
prefer one strong, defensible finding over several weak ones, and say so
directly if the change looks safe.

## The specific traps that have already cost real bugs here

**POSIX layer** (relevant when the diff touches `compat/xbox/` or
`filesystem/`): partial reads and writes; `lseek` past EOF and return
values indistinguishable from an error; 64-bit offsets truncated;
`O_APPEND` applied on every write versus only checked at open; `errno`
values callers actually test; descriptor reuse and double close; `readdir`
and `.`/`..`; buffer sizes and off-by-ones in path handling. Read the real
call sites in `filesystem/` — a function is only wrong if something calls
it that way. A previous review found a stub whose justifying comment
inverted the guard it cited, which would have made every file inside
`pak0.pak` unreadable — the whole of retail Half-Life's content. Comments
asserting a path is unreachable are claims: verify them, don't trust them.

**Xbox/nxdk platform assumptions** (relevant when the diff touches
`compat/xbox/`, `engine/platform/`, `ref/`, `deps/nxdk`, or `tools/xbox*`):
verify every Win32 or kernel function used actually exists in
`deps/nxdk/lib/winapi/` or `deps/nxdk/lib/xboxkrnl/` with the signature
assumed. Check FATX path semantics: drive letters, backslashes, no working
directory, path length. Check anything that allocates against the 64 MB
budget, and anything per-frame or per-file-open against a 733 MHz CPU.

**Divergence discipline** (always relevant): read `AGENTS.md`'s divergence
ladder and `docs/r3d/divergences.md`. For every edit to an
upstream-maintained file, judge whether it is the cheapest form that works,
whether it is recorded accurately in the ledger, and whether a
compat-layer approach could have avoided it entirely. For each guard added
around upstream code, verify it is genuinely needed AND that the branch it
now falls into is correct for Xbox rather than merely compiling — **a
block guarded unnecessarily is a silent behaviour change**, not a style
nit. Report ledger entries that are missing, and claims in the ledger the
diff does not actually support.

**Build and tooling** (relevant when the diff touches `tools/`, `wscript`,
`Makefile`, `CMakeLists`, `.gitattributes`, or `.gitignore`): where a
script replicates something else — `tools/xbox-cc.sh` mirrors
`deps/nxdk/bin/nxdk-cc` with the include order changed — compare line by
line and report anything dropped, added, or reordered beyond the intended
difference. Assess drift risk when the original is updated.

Then audit the measurement itself, specifically. Every measuring tool in
this project has reported a false "clean" result at least once — this is
the single most recurring bug class in this codebase's own history:
- A sweep that judged success by grepping for "error:" reported "182 ok, 0
  failing" for a directory whose first file emitted twenty real errors.
- The same sweep, later, fell through to an implicit `exit 0` at end of
  file regardless of how many files failed to compile — nothing after its
  early-out guards ever set a nonzero exit.
- A drift-checker script silently produced a false clean pass when its
  `sed`/`grep` pipeline was invoked from a mismatched shell runtime,
  corrupting its own comparison with no error text.
- The same drift-checker separately reported a false clean pass when its
  extraction pattern matched zero lines — the comparison loop simply never
  ran, and "nothing missing" looked identical to "nothing was checked."

For an agent prompt or a Workflow script in the diff (`.claude/agents/`,
`.claude/workflows/`): find where a pass/fail verdict is produced. If a
model fills in a boolean, or the list of checks that "ran" is free text,
that is the defect - the loop's first run reported green on a diff whose
build gates never ran (PR #148). A verdict must be computed from recorded
exit codes matched by name against a list derived from what changed. Any
sentence that lets the agent choose which checks are relevant, skip one,
or grade its own list is the defect, even when the verdict is computed
elsewhere - the first loop's QA prompt said "skip any stage the change
plainly doesn't touch" and "green only when every stage you judged
relevant passed", and "judged relevant" was the hole. And
for any checkout a script creates (`git worktree add`), ask whether the
commands it will run there need built artifacts the checkout does not
have: both real runs lost their gates to a worktree with nxdk's sources
and none of its libraries.

For ANY tool, script, or check in this diff: ask specifically what it
would print if it were broken, unset, or fed empty input — and whether
that output is distinguishable from a genuine pass. A check that can only
ever report success is worse than no check, because it gets believed.

**Performance** (relevant when the diff touches `engine/common/`,
`engine/server/`, `engine/client/`, or `ref/`): review for a 733 MHz CPU
with 64 MB shared with the GPU. Look for allocations in per-frame or
per-entity paths, and work redone every frame that a cheap cache would
remove — but check whether the reference itself already memoises it before
calling the redone work a divergence worth "fixing"; the reference is
mostly eager (bone setup, `R_StudioSetupBones`, `gl_studio.c:815`, is a
named example), so adding a cache there is usually new work, not a
restoration. Flag unconditional logging (a debug line costs real
milliseconds on a serial console) and hash or string lookups where an
index would do. Ironwail's *principles* transfer to this port — less CPU
per frame, batch, sort by state, never allocate in the loop. Its
*techniques* do not: no compute shaders, no indirect draw, no bindless
textures on this hardware. Flag anyone conflating the two.

**Reference-fidelity claims** (relevant when the diff touches `docs/r3d/`,
`ref/`, or `.claude/agents/`): for every statement of the form "the
reference does / does not / already / never X" — in a document, a plan, a
code comment, or a specialist's standing prompt — find it in `ref/gl`,
`ref/soft`, `engine/client`, or `engine/common/mod_bmodel.c` and cite
`file:line`, or name the function that would contain it and does not.
Check the default, not the capability: `r_vbo` exists and defaults to off
(`gl_opengl.c:34`). Check the mechanism, not the name: the bone "cache" at
`gl_studio.c:72` is not a per-frame memo, and world dlights are lightmap
sub-uploads, not per-pixel lights. Five of nineteen "ports from Ironwail"
claimed in this project's own history were already in the tree, and one
ran the other way — every one was a claim of exactly this shape, unverified
until someone checked. A claim without a line is itself a finding.

## Output discipline

Every finding must include the affected file, `line_start`/`line_end`, a
confidence score, and a concrete recommendation — not a vague "consider
reviewing X." If a conclusion depends on an inference rather than something
directly readable in the diff, say so explicitly in the finding body and
keep the confidence honest rather than inflating it.
