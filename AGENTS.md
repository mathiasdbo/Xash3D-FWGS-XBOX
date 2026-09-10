# Project rules — Resonance3D (fork of Xash3D-FWGS)

This repository **is** Xash3D-FWGS. Start with `docs/r3d/epics.md` for the
top-level plan and `docs/r3d/README.md` for context. **Read
`docs/r3d/lessons.md` at the start of every session.**

## The central rule: minimise divergence

This inverts the rule that governed the previous Rust project. There, the job
was to **replicate** the reference and cite it in every function. Here we
**are** the reference, and the job is not to drift from it any further than
strictly necessary.

- **Every deviation from upstream goes into `docs/r3d/divergences.md` the same
  day it is written.** Not when the phase closes, not in the PR: the same day.
  A fork with no divergence ledger becomes unmanageable by the third merge and
  gets abandoned.
- **Always prefer the cheapest form of divergence**, in this order: new file
  in one of our directories → new file in an upstream directory → `#ifdef`
  → rewriting an upstream function → changing behaviour with no guard
  (forbidden unless it is a genuine upstream bug, and in that case the patch
  goes upstream first).
- **Do not move or rename anything in the upstream tree.** Our code lives in
  `tools/` and `docs/r3d/`. `scripts/` and `Documentation/` already exist
  upstream: put nothing of ours there.
- **Before writing an abstraction, check whether upstream already has it.**
  Already built: the polymorphic renderer (`ref_api.h`), static linking
  (`XASH_STATIC_LIBS`), the platform layer (16 backends), the memory pools
  (`zone.c`) and the searchpath vtable (`searchpath_t`). See the closing
  section of `divergences.md`.
- **Send upstream anything generically useful.** The audio abstraction is the
  clear case: if upstream takes it, the divergence disappears and its
  maintenance cost with it.

## Original Xbox: what constrains everything

- **64 MB shared with the GPU.** NV2A textures need physically contiguous
  memory, outside the normal heap: a budget that only counts the heap is blind
  to half the consumption. One ledger, two allocators.
- **The budget is enforced at bake time, not at runtime.** A map that does not
  fit must break the build, not the console.
- **Never leak per-map memory.** Everything a level loads is released on exit,
  and that is measured, not assumed.
- **No dynamic library loading.** Static single-image build.
- **The NV2A is fixed function plus VP20 and register combiners.** No compute,
  no indirect draw. Anything that assumes modern programmable shaders does not
  apply.

## Code

- C99 and upstream's style. The root `uncrustify.cfg` is the arbiter.
- Never reformat upstream code: it pollutes the merge diff and hides the real
  divergences.
- Comment the *why*, not the *what*. In a divergence, the comment cites the
  `divergences.md` entry.
- Constants and flags: look them up in `common/const.h` or the source. Never
  guess them.

## Read the sibling ports before inventing

No problem in this port is new. Before deriving a solution, read the one that
already works:

- **`engine/platform/psvita/`, `nswitch/`, `dos/`** — the console backends
  already in this tree. They are the template for `engine/platform/xbox/`.
- **`ref/soft/`** — proof that `ref_interface_s` tolerates a non-GL backend.
  When something in the interface looks impossible outside OpenGL, look at how
  the software rasteriser solved it.
- **[xashPS3](https://github.com/Mayo1970/xash3d-fwgs)** — a sibling port of
  this same engine to the PS3, with an exhaustive porting log. It already ate
  the static-linking problems (symbol collisions, the `FI` global, vtable
  deduplication), the toolchain assumptions and the endianness traps. Consult
  it before debugging any of those families.

Cite in the commit what was read, and why the solution here differs, if it
does.

## Do not guess

- **Confirm every assumption** by reading the code or observing the hardware.
  Never from memory, never by analogy.
- **On a failure, isolate rather than rewrite.** Produce the minimal test case
  that reproduces it. Rewriting a whole implementation when something fails
  destroys the information about what was wrong.
- **After two failed attempts, stop and escalate with data** — `nm` output,
  `objdump`, a dump, a trace. Not with a fresh hypothesis.
- When a hypothesis is disproved, write it down. Knowing what it was NOT is
  worth as much as knowing what it was.

## Learning from mistakes

`docs/r3d/lessons.md` is read at the start of every session. It is the only
mechanism that stops the same mistake being made twice, and it is worth more
than any amount of care taken in the moment.

**When a mistake is found — by anyone, including you — reflect on it, abstract
the learning past the specific case, and write it to `docs/r3d/lessons.md`.**

Not every slip. A mistake earns an entry when it has a *shape that will
recur*: a wrong assumption about how something works, a verification that
looked sufficient and was not, a conclusion drawn from the wrong evidence.
A typo does not.

Three triggers, and the first two are the ones that get missed:

1. **A review finds something you were confident about.** Confidence that
   turned out to be misplaced is the highest-value entry there is, because
   nothing else will catch it next time.
2. **A claim you made turns out to be false**, including one that was true of
   an earlier version of the question. Scope moves, and answers carried across
   a scope change are wrong without being incorrect.
3. The user points out an error, or asks you to reflect on one.

The meta-rules for writing a good entry are at the top of `lessons.md`. Follow
them: a badly written rule is worse than none, because it gets followed.

Delete rules that stop being true. A stale rule costs more than a missing one.

## Review on every PR

**Run `tools/codex-review.sh --base main` before opening a PR.** It hands
the diff to Codex (the `openai-codex` Claude Code plugin, run in the
background — a review can take several minutes) with standing project
context from `docs/r3d/codex-review-focus.md`, adversarially: Codex is
instructed to try to break confidence in the change, not validate it, and
defaults to `approve` only when it cannot support a substantive finding.

This replaced a Claude-side multi-agent workflow (`.claude/workflows/
pr-review.js`, removed 2026-09-07) whose 6-lens/3-refuter fan-out cost
25-37 subagent calls and millions of tokens per round, and took 11 rounds
to close one branch. Moving review compute onto Codex keeps the scrutiny
without that cost.

**Fix-or-justify threshold:** any finding with severity `critical`/`high`,
or `confidence >= 0.6`, gets fixed or a written reason for not fixing — a
finding you disagree with gets a written reason, not silence, same as
before. Lower-severity, lower-confidence findings are worth a glance but
are not mandatory to act on.

**Cadence: capped at two rounds.** Run the review, fix or justify what's
actionable, run it once more to catch anything the fixes themselves
introduced — then open the PR regardless of what the second round finds.
Do not run a third round. If round 2 raises something clearly real, fix it
and stop; if it's not clearly real or not worth chasing further, note it as
deliberately deferred in the PR description instead of re-reviewing again.

Add a trap to `docs/r3d/codex-review-focus.md` when a class of defect gets
through — it carries the specific failure shapes this project has already
hit (the POSIX layer's partial-read/write traps, the "measure the
measurement" lesson from `tools/xbox-sweep.sh`'s own history, reference-
fidelity claims that turned out false), which is what makes it find things
a generic review prompt does not.

**It has already earned its place.** The first run of the workflow this
replaced found a stub whose justifying comment inverted the guard it
cited, which would have made every file inside `pak0.pak` unreadable — the
whole of retail Half-Life's content. Three independent lenses found it;
the compiler could not, and neither did the author. That history is why
review runs on every PR rather than only the ones that look risky.

## Specialist review

Named specialists live in `.claude/agents/`. Pick the ones a change actually
touches, and never ask permission first.

| Agent | Use for |
|---|---|
| `nv2a-xbox-specialist` | NV2A capabilities, pbkit, swizzling, the 64 MB budget, nxdk |
| `build-toolchain-specialist` | waf, CMake, the nxdk toolchain, static linking, symbol collisions, packaging |
| `render-architecture-specialist` | The render interface and any backend; whether something belongs in the shared layer |
| `memory-budget-specialist` | Anything that allocates, per-map lifetimes, bake-time budget enforcement |
| `asset-pipeline-specialist` | The baker, the `.mpak` format, `filesystem/`, manifests, asset discovery |
| `upstream-fidelity-specialist` | Divergence form and merge risk. **Run before closing any phase** |
| `console-performance-specialist` | Per-frame and per-entity code; any optimisation proposed for a retro target |
| `reference-renderer-specialist` | What `ref/gl` actually does, by `file:line`. **Run on any claim of the form "the reference does X"**, and on any `docs/r3d/` change that describes renderer behaviour |

## Verification

- Compile and boot before calling anything done. Do not advance with a broken
  tree.
- **xemu is not validation.** It is good for fast iteration, but DMA timing,
  the serial channel and real unified-memory behaviour are not faithfully
  emulated. A phase does not close until it runs on a real Xbox.
- The PC build is the safety net: **every phase must leave PC working**. If PC
  breaks, the only reference point for correct behaviour is gone.
- Add the Xbox build as a mandatory verification step as soon as X0 closes,
  even while the binary does nothing useful yet. The previous project's target
  accumulated six stale fields over seven months because nothing exercised it.
- Smoke-test after deploying, do not just copy.
- **Package real-hardware deliverables into `../XISOS/`.** When a bootable
  Xbox XISO is ready for the user to try (on xemu or, preferably, real
  hardware — see above), copy it into a new timestamped folder under
  `../XISOS/` (sibling to this repo, outside git — these are binary build
  artifacts, never committed). Each folder gets a `CHANGELOG.txt`: an
  incrementing internal version number (v1, v2, ... — this project has no
  formal release scheme yet), what the XISO contains, and what it verifies
  or is still unverified. **The commit hash alone is not provenance here**:
  this tree routinely builds with uncommitted, tooling-applied submodule
  patches (`3rdparty/library_suffix`, `3rdparty/mainui`, `3rdparty/opusfile`
  auto-patched by `tools/xbox-cc.sh`) and an untracked `deps/nxdk` checkout,
  so two XISOs built from the identical commit can differ. Record, verbatim:
  the commit hash, `git status --short` (captures the submodule/untracked
  state at build time), and a SHA-256 of the XISO file itself, so the
  changelog is bound to the literal artifact bytes even if the source tree
  can't be perfectly reconstructed from the hash alone. Never overwrite an
  existing version folder — each one stays reachable so a specific build
  can always be gone back to.

## Workflow

- One branch per phase or feature, off `main`.
- Micro-commits: every small, validated step is committed and pushed.
- Specialist sub-agent review before closing a phase, picking the ones the
  change actually touches. Never ask permission first.
- Update `CHANGELOG.md` and `docs/r3d/divergences.md`, and open a PR. Do not
  advance to the next phase until it is merged.
- After merging, `git fetch upstream` and assess whether to sync.

## Syncing with upstream

- `git fetch upstream && git merge upstream/master`.
- Resolve conflicts **against `divergences.md`**: if a conflict touches a
  recorded divergence, the entry says why it is there. If it touches something
  unrecorded, that is a process bug — record it before resolving.
- Pushing to `upstream` is disabled on purpose
  (`git remote set-url --push upstream no-push`).

## Attribution in git

Commit messages, PR titles and PR descriptions describe the change and why,
and nothing else. No tool trailers, no "generated with" footers. The same
applies to `CHANGELOG.md` and to code comments.

This is separate from the AI disclosure in `README.md`, which stays: the
disclosure belongs where readers of the project can see it, not stamped on
every commit.
