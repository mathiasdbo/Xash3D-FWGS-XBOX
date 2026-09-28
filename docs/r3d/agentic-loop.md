# The agentic loop — a repo-native multi-agent Workflow

**Status: v2, 2026-09-26.** Built 2026-09-21 (PR #119), run twice on real
work (PR #148, the texture manager and 2D pass; PR #149, the TriAPI), and
rewritten from what those two runs showed. The evidence is in
`docs/r3d/reports/agentic-loop/`, one record per run; every later run
writes its own. This document is the design record for
`.claude/workflows/agentic-loop.js` and the six `loop-*` agents in
`.claude/agents/`: why the loop is shaped the way it is, what each stage
costs and why, and how to call it. Not a `divergences.md` entry: that
ledger is scoped to deviation from upstream engine behaviour, and this is
tooling.

**See also `docs/r3d/fast-loop.md`** (2026-09-28): the reduced loop - one
thinking agent per round, every other stage a Haiku runner of a generated
script, the gate plan computed from the branch's own diff, origin/main's
build and scene frames cached. This v2 loop stays until that one has a
measured run.

## Why this exists

Driving a phase by hand — research, implement, verify, iterate, publish —
is what the X9 series was until X9-F, and it works. It is also
conversation-driven and unrepeatable. The loop is that same shape made
explicit: one instruction in, a bounded set of atomic topics out, each
researched, implemented, gated, reviewed, looked at, and published as its
own PR, with a record of what it cost.

The loop's own first design rule came from a workflow this project had
already built and deleted: `.claude/workflows/pr-review.js`, whose
6-lens/3-refuter fan-out cost 25–37 Claude subagent calls and 2–2.6 M
tokens per round and took eleven rounds to close one branch
(`tools/codex-review.sh:31-37`). So: **no stage agent can spawn
subagents**, every fan-out decision is made in the script, and **code
review is not re-implemented with Claude subagents** — QA calls
`tools/codex-review.sh`, on Codex's quota. Both rules stand.

## What two real runs taught

| | Run 1 (PR #148) | Run 2 (PR #149) |
|---|---|---|
| Topics | 1 | 1 |
| Agents / tokens / wall clock | 12 / ~1.72 M / ~2 h 09 | not recorded / not recorded / ~3 h 50 |
| QA rounds | red, red, green (round 3: reading only, codex spent) | 2 codex rounds + a live round |
| Build gates | isolate, link, game link **never ran** — worktree had no built nxdk | ran only after the built nxdk was copied in by hand |
| What a human found after | did not compile for Xbox (two includes); HUD render-mode bug, visible only on screen | nothing yet; inside the run, two docs commits escalated an environment gap that did not exist |
| What went well | research found and fixed a real baker waste (2.7 MiB/map); codex found 5 real bugs | codex found a real `GetMatrix` bug; the live round verified the change on screen |

Three things follow, and v2 is built on them:

1. **A model's "green" is not a gate.** Run 1's QA schema had a boolean
   the model filled in; it returned true with three required gates unrun.
2. **A fresh worktree is not a build environment.** `git worktree add` +
   `submodule update` gives nxdk's sources; `lib/*.lib`, `cxbe` and
   `extract-xiso` are untracked outputs of `make NXDK_ONLY=y`. Both runs
   hit this, in two different disguises.
3. **Static review does not see the picture.** The one defect two codex
   rounds and a QA pass missed was found by one screenshot.

And two cost observations: with one topic the pipeline across topics buys
nothing and the run is a serial chain of every stage; and the specialist
stage re-asked what a 161-line design note from the day before already
answered.

## What changed in v2

1. **Green is computed in the script.** `gatePlan(filesChanged)` derives
   the required gates from the paths the implementer's branch changes
   (table below). Every stage is handed the same plan; the QA stage runs
   it through `tools/gate-chain.sh`, which records each gate's exit code
   and lists the rest as `not-run` after the first failure, and returns
   the table verbatim. The script matches the table against the plan
   **by name**: a gate can be misreported, it cannot be omitted, marked
   run without a log, or overruled. Two more binds close what Codex's
   review of this branch found still open. The runner's implicit first
   row, `changed-files`, compares the list the plan was computed from
   with `git diff --name-only <fork point>...HEAD` plus anything
   uncommitted, so a path the implementer forgot to list is a red row,
   not a missing gate. And the runner writes `attestation.txt` only on an
   all-green run — sha256 of the plan file, sha256 of `results.tsv`, the
   commit, the gate names — which the script requires (naming every
   planned gate) and `loop-publisher` verifies by recomputing the
   digests and comparing the commit. The exit codes an agent transcribes
   stay the residual trust, so `loop-publisher` takes that second,
   independent look before the GREEN path — by content, not existence
   (the audit's critic found that the host-test binary, the sweep
   manifest and a stale `.identity` all exist after a failure): the
   isolate's `manifest.txt`, which its tool writes only after every
   check passes; and the XBE, whose `sha256sum` on disk must equal the
   `sha256_xbe` in its `.identity` (removed before every relink) and the
   hash QA reported. `tools/gate-chain.sh --self-test`
   checks the runner's own failure paths (empty plan, missing plan,
   failing gate, command not found) — `lessons.md`, "never trust a new
   tool's exit code without testing its failure path".
2. **The worktree is given the main checkout's built nxdk and must prove
   it.** `loop-setup` initialises every submodule except `deps/nxdk` and
   writes the main checkout's nxdk path into the worktree's
   `.sweep/nxdk-dir`. Every tool that resolves nxdk (`tools/xbox-cc.sh`,
   `xbox-cxx.sh`, the three link checks, `xbox-xiso.sh`, and every
   `nxdk-link`/`cxbe`/`extract-xiso` site) now reads `NXDK_DIR` from the
   environment, else that file, else its own `deps/nxdk` — nxdk's own
   `activate` keeps a pre-set value. The file matters because
   `lessons.md` records `/c/msys64/usr/bin/bash` dropping exported
   variables with no error; gate commands are therefore bare
   invocations, and the variable travels in the checkout as well as
   inline. Setup then runs a fixed **environment plan** through
   `tools/gate-chain.sh` in the first worktree — the nxdk-dir file, the
   built libraries, `cxbe`, one compile of `public/crtlib.c` with `-v`,
   and a check that clang's include search list actually reached an nxdk
   `pdclib/include` (the only one a worktree without its own `deps/nxdk`
   can find is the main checkout's); with a live configuration also
   `extract-xiso`, the Steam root, xemu, the baked `.mpak`, and the
   baseline build of pristine `origin/main` into the **main checkout's**
   `.sweep/agentic-loop-baseline/<fork point>/` — once per fork point,
   surviving the worktrees, with a final check that the linked `.exe`
   landed there (which proves `SCRATCH` crossed the shell boundary). The
   script judges the rows by name and stops the run on any red row when
   a topic touches Xbox code. The main checkout's nxdk is only ever
   read. A re-run of the same slice branches as `<slug>-<runId>`, never
   `-B` over a branch that may have a PR.
3. **Codex rounds are spent gate-first, on the fork point, and counted
   from evidence.** Gates → codex 1 → fix → gates → live → (fix → gates
   →) codex 2 → fix → gates → publish. A red table never reaches codex.
   A round counts only when the QA stage returns the `codex thread:` id
   and an exit code the script's own contract makes a verdict of (0, or
   1 with findings); exit 2 "inconclusive", a missing plugin or a usage
   limit is not a round and never reads as clean — two of those mark the
   review "not completed" and the PR is blocked, as does a QA call that
   was asked for a round and did not run it; GREEN needs at least one
   completed round. Codex reviews against
   `origin/main`'s SHA at setup (`baseSha`), never the local `main` ref,
   which is stale in any checkout not pulled since the last GitHub merge
   and would widen the diff by a whole merged PR. Round N+1 runs only if
   a round remains **and the diff changed** since round N — a fix, or a
   live fix — so a clean round on an unchanged diff ends the review (a
   second run on the same diff is variance-hunting), and the live look
   sits before codex 2 so a screen-found fix, the riskiest code in the
   PR, is reviewed while a round is unused. Every finding has a home:
   fixed, listed as justified with its reason, or — when a fix call
   returned nothing — listed as unaddressed and the PR blocked;
   findings below the fix-or-justify threshold reach the PR as "worth a
   glance". The v1 topology — three QA rounds, then a spec revision,
   then one more — is gone: it had never been exercised past round
   three and cost up to four QA and four implementation calls per topic.
4. **The implementer self-gates on the cheap subset, and the script
   reads its table.** It gets the predicted plan and the runner command,
   iterates on failures before handing off, and returns its own
   `results.tsv` rows. The script judges the cheap rows (compile, unit
   tests: seconds) and gives the implementer one retry before any QA
   agent is spent. Every hand-off round runs only that cheap subset in
   the implementer; the full plan — sweep, link, minutes — runs once per
   round, at QA, so the same tree is never built twice per round. An
   empty `filesChanged` is a failed topic, never an empty plan judged
   green. A compile error caught here costs seconds; at QA it cost two
   agents and a codex round in run 1.
5. **A live stage looks at the screen — the right screen.** A disc
   packed from retail alone boots `ref_soft` on a stub menu, so
   `tools/xbox-xiso.sh --overlay` now stages what run 2 placed on the
   disc by hand: `video.cfg` with `r_refdll "nv2a"`, a
   `userconfig.d/00-live.cfg` with `map <name>` and the spec's cvars, and
   the caller's baked `.mpak`. Setup's environment plan builds the
   **baseline** (pristine `origin/main`, same gates, its own `SCRATCH`)
   so there is something to diff against — and that build is the
   strongest environment proof there is. For a change touching Xbox
   code, with `args.live` configured: two discs, one boot at a time,
   `verify-image`, then `tools/xemu-debug.py wait-scene` (the map-ready
   flag reads 1 and `host.framecount` has advanced 60 frames since — a
   loading plaque is not a scene), `fbdump`, `screenshot`, `ppmdiff`
   baseline against branch, and **read the PNGs** (the Read tool shows
   images) against the spec's `live.expected` list. Research writes that
   list, the cvars, and whether a correct change should alter the spawn
   frame (`expectsPixelChange`); the script turns a pixel diff that
   contradicts it, a scene that never became ready, or a booted XBE
   whose hash is not the gated one into a defect — and a look whose own
   claims are not backed by the tools' printed lines (`scene ready:`,
   `ppmdiff:`), a real booted-XBE hash, and a sha256 per screenshot is a
   defect too, never a pass with the check skipped; the publisher
   recomputes the screenshot hashes. A look counts as a look at this XBE
   only once it passed those checks. The look runs after
   the first green table and again whenever the XBE hash changes (a
   green link row that returns no hash is an incomplete answer: one
   retry, then blocked — never a sentinel that keys a look to every
   round); GREEN requires the last look to have been at the final XBE.
   A defect gets one live fix; a second defective look blocks the PR.
   Findings a codex round returned in the same round as a live defect
   are carried to the next round, never dropped. A renderer topic
   (`ref/soft`, `ref/nv2a`) without `args.live` is refused at setup: the
   two defects static review missed on this project were visual. The
   emulator, its GDB port and the pad bridge are one per machine, so the
   live stage is serialised across topics in the script (a plain promise
   chain) and checks for a leftover instance before launching. What xemu cannot settle is listed for the PR body
   (`lessons.md`: xemu is not validation for register values).
6. **Slice mode, and a specialist only where there is no note.** The
   script detects a slice in the prompt by pattern (a fork-plan id such
   as `X9-X`, "item N step M", or a design-note path), logs the match and
   tells setup: one topic, not decomposed. `designNotes` is a file check
   against `docs/r3d/design/*.md` headers, never a fork-plan line. Under
   the default policy: a note on disk means **no** consultation (run 1
   paid two specialists to restate a note one of them had written the
   day before; research greps the reference itself and codex's focus
   checks reference claims); a topic with no note gets the **one** domain
   specialist its predicted files sit in (`ref/nv2a` →
   `nv2a-xbox-specialist`, the baker or `filesystem/` →
   `asset-pipeline-specialist`, the build → `build-toolchain-specialist`),
   and research writes that input up as the missing note and commits it
   on the branch with a `Refereed: no` header line — paid once per
   slice, not once per run. `AGENTS.md` wants `reference-renderer-specialist`
   on any renderer design; until a human runs that pass and changes the
   line, the PR lists the note as unrefereed under its owner-side
   heading. Run 2 had no note, and its one real bug was a hardware fact a
   note carries. An
   explore topic without a note keeps setup's flagged list as well. Every
   item a note or specialist marks unverified travels in the spec to the
   live look and the PR.
7. **The loop keeps a record it computed.** The script measures each
   agent call's output tokens (`budget.spent()` deltas) and renders the
   record's measured sections itself — scope, cost per stage, every gate
   table with exit codes and seconds, the review's findings and their
   fate, the live result — so no number passes through a model's
   transcription; the retrospector writes that block verbatim into
   `docs/r3d/reports/agentic-loop/<run-id>.md`, adds only observations,
   an empty Missed for a human and candidate lessons (checked against
   `lessons.md`'s headings so an adopted rule is never re-proposed), and
   pushes the file to the published branch, reporting `pushed` only
   after a fetch shows the commit there. Every stage log and gate table
   is written under the **main checkout's** `.sweep/agentic-loop/<run-id>/`,
   outside the worktrees, and each GREEN topic's success-only artifacts
   are copied there before its worktree is removed — which happens only
   when the tree is clean and its HEAD is on the remote; anything else
   stays for a human. Every stage log append begins with a UTC
   timestamp, so the retrospector can lay out where a run's hours went
   from the logs' own times; the PR body ends by pointing at the
   record's Missed section. `runId` is now required. `args.dryRun` prints the plan and the agent-count arithmetic
   without spawning an agent.

## Architecture

```
SetUp (Haiku, once): mode, topics, one worktree per topic, NXDK_DIR, one compile as proof
  |
Specialists (per topic, parallel; skipped when a design note covers the topic)
  |
pipeline(topics, chain)          <- no barrier between topics
  |
Retrospector (Sonnet, once): tracked run record with the measured cost
```

Per topic, one bounded loop (at most 8 QA rounds):

```
Research (Opus) -> Implementation (Sonnet; cheap gates red -> one retry, no QA spent)
  -> QA round: full plan via gate-chain.sh; codex on the fork point iff the table is green,
               a round remains, and the diff changed since the last conclusive round
       table red?      -> fix (cheap gates) -> next round            (2 retries, then [QA BLOCKED])
       XBE not looked at? -> Live: two discs, wait-scene, ppmdiff, read the PNGs
                             defect? -> fix -> next round           (2 looks, then [QA BLOCKED])
       actionable findings? -> fix or justify -> next round         (a null fix = unaddressed = RED)
       nothing pending and (diff unchanged since codex, or no round left) -> done
  -> Publisher: normal PR on GREEN, [QA BLOCKED] draft on RED
```

GREEN means: every planned gate ran with exit 0, nothing in the plan was
left unconfigured, the last live look (when one was due) found no defect
and was at the final XBE, no actionable finding was left unaddressed,
and the review did not end inconclusive twice. Codex findings never
block past the two rounds; each is fixed or listed in the PR with its
reason, per `AGENTS.md`.

## The gate plan

Computed from `git diff --name-only origin/main...HEAD`; every command runs
from the worktree, bare (the tools pin their own PATH), with `NXDK_DIR`
inline and in the worktree's `.sweep/nxdk-dir`.

| Paths touched | Gate(s), in order |
|---|---|
| `ref/nv2a/`, `compat/xbox/`, `tools/host-tests/` | `tools/host-tests/run.sh` |
| `tools/*.py` | `python -m unittest discover -s tools -p "test_*.py"` |
| `tools/asset-studio/` | cmake configure (if needed), build, `ctest` |
| any Xbox code (`engine/`, `common/`, `public/`, `filesystem/`, `compat/`, `3rdparty/`, `ref/soft|nv2a|null/`, `deps/hlsdk`) **or build infrastructure** (`tools/xbox-*.sh`, `tools/gate-chain.sh`, `tools/host-tests/run.sh`, `patches/`, `wscript`, `scripts/waifulib/`) | `xbox-ref-isolate.sh <soft|nv2a>` for each touched module first (ten seconds; it reads nothing the sweep makes, and it is where run 1's defect would have surfaced), then `xbox-sweep.sh` over the engine directories — **with `--expect-fail <baseline>` when an engine directory changed** (the sweep exits 0 without a baseline even when files fail, by design; `CHANGELOG.md` holds the tracked count, passed in as `args.baselines.engineSweepExpectFail`; missing on an engine change = the plan is unconfigured and cannot go green) — then `xbox-game-link-check.sh hl --expect-undefined 0 --include-client --include-freevgui --xbe …` (which runs the game sweeps, isolates, the freevgui sweep and the baseline link itself, and leaves the bootable image) |
| `.claude/workflows/agentic-loop.js`, `tools/test_agentic_loop.mjs` | `node tools/test_agentic_loop.mjs`; `tools/gate-chain.sh` → its `--self-test` |
| `3rdparty/mainui/` | `xbox-mainui-sweep.sh --expect-fail … --expect-total …` from `args.baselines` |
| docs, `.claude/agents/`, `*.md` only | no gates; codex still reviews |
| code no rule above covers (`ref/gl`, other targets' platform backends) | no gate, and the plan says **NOT GATED** with the paths, never "docs only"; the PR opens as a draft naming them — "every phase must leave PC working" is a check this loop cannot run, so codex alone never makes such a change GREEN |

Not covered, on purpose, for now: a PC build (`waf`) — the loop's gates
are Xbox-only, and "every phase must leave PC working" is still checked
by a person; `ref/gl` changes therefore get no build gate here, and say
so.

## Model / effort per stage

| Stage | Model / effort | Why |
|---|---|---|
| `loop-setup` | Haiku, medium | mechanical; its one judgement (slice vs explore) is cheap to overrule via `args.mode` |
| `loop-research` | Opus, high | every downstream stage pays for a wrong spec; unchanged from v1 |
| `loop-implementer` | Sonnet, high | real correctness stakes; v1 considered and rejected lowering it — the five codex findings in run 1 were implementation bugs, and each QA round they cause costs two agents. Self-gating is the cheaper lever |
| `loop-qa` | Sonnet, high | runs and reports; judgement moved into the script. The same agent does the live look, which is where its reading of a PNG matters |
| `loop-publisher` | Sonnet, medium | git/gh plus a factual PR body |
| `loop-retrospector` | Sonnet, high | was Opus; it now writes a record from numbers the script measured rather than curating lessons alone, and the human decides what enters `lessons.md` |
| specialists | referees Opus/high, the rest Sonnet/medium | unchanged |

## Cost

Agents per run = 2 (setup, retrospector) + specialists (0–2 per topic;
0 under the default policy for a slice or when a design note exists) +
per topic: research 1, implementation 1–2, QA 1–8, fixes 0–5, live
looks 0–4 (one per XBE the gates produce after a fix), publisher 1. A
one-topic slice with one fix per codex round is 11 (setup, research,
implement, QA+codex 1, live, fix, QA+codex 2, fix, QA, publish,
retrospector); docs-only is 7; the hard ceiling per topic is 21,
reachable only through every retry path at once. Run 1 used 12 for
a weaker result.
Wall clock is still the serial sum for one topic, dominated by builds
and the two codex runs; the levers that cut it are self-gating (fewer
QA/fix pairs) and the skipped specialist stage.

`Workflow({ name: 'agentic-loop', args: { prompt, dryRun: true,
dryRunTopics: [{ slug, files: [...] }], baselines, live } })` prints the
per-topic plan, what is unconfigured, and the arithmetic, with zero
agents.

## Invocation

```
Workflow({ name: 'agentic-loop', args: {
  prompt: 'X9-X step 5: studio models (fork-plan item 14)',
  runId: '20260926-1430',            // required; the script has no clock
  startedAt: '2026-09-26T14:30',     // for the record
  mode: 'auto',                      // 'slice' | 'explore' | 'auto'
  specialists: 'auto',               // 'auto' | 'none' | 'flagged'
  maxTasks: 4,
  baselines: { engineSweepExpectFail: 29, mainuiExpectFail: 0, mainuiExpectTotal: 86 },
  live: { xemuExe: 'G:/Games/Xemu/xemu.exe', hlSteamDir: '/d/SteamLibrary/steamapps/common/Half-Life',
          map: 'crossfire', mpakPath: '/g/bakes/crossfire.mpak',   // asset-studio --target xbox --profile nv2a
          gamedir: 'valve', resourceDir: '...', uiLanguage: 'english' },
} })
```

**The agents' shell on Windows.** Every gate is a bash script and
`tools/gate-chain.sh` runs each plan line through `bash -c`, so the
agents' Bash tool must be a real bash, not `C:\Windows\System32\bash.exe`
(the WSL launcher, which is what a bare `bash` resolves to in PowerShell
when no distribution is installed). Claude Code on Windows uses Git Bash
when it finds one and PowerShell otherwise. To point it at MSYS2's bash,
set `CLAUDE_CODE_GIT_BASH_PATH` to `C:\msys64\usr\bin\bash.exe` as a
**Windows user environment variable**, then quit Claude Code completely
and reopen it from a fresh terminal. The `env` block of a settings file
is not enough: found on the owner's machine 2026-09-26, Claude Code
chooses its bash before it applies that block, so the override arrives
too late and the running shell stays Git's. The only honest check is
`cygpath -w /` from the Bash tool: it must print `C:\msys64`, not
`C:\Program Files\Git`. `echo $MSYSTEM` proves nothing (a settings
block can inject it under either bash), and `which clang` fails under
Git Bash even though every `tools/xbox-*.sh` pins
`/c/msys64/mingw64/bin` itself. `lessons.md` records MSYS2's bash
dropping exported variables at its boundary: the loop relies on nothing
exported, `NXDK_DIR` travels in `.sweep/nxdk-dir`. To run a tool by hand
from PowerShell: `$env:MSYSTEM = "MINGW64"; & C:\msys64\usr\bin\bash.exe
tools/gate-chain.sh --self-test`, or use the MSYS2 MINGW64 shell
`tools/xbox-env.sh` already prescribes.

`baselines` come from `CHANGELOG.md`'s tracked counts, never from the
script. Without `live`, the live stage is recorded as not run, the PR
title says `[not booted]`, and a topic touching a renderer module is
refused at setup. The loop runs only where its gates run: the owner's Windows
machine with MSYS2 at `C:\msys64` and a built `deps/nxdk` in the main
checkout.

## Logging and the record

`<main checkout>/.sweep/agentic-loop/<run-id>/` (gitignored, kept after
the run): `00-setup.md`, one `<slug>.md` per topic appended by every
stage in turn, the gate runner's `gates-<slug>-<round>/` directories with
every log, the environment plan's table, the live screenshots and
`.ppm` frames, `artifacts-<slug>/` copies of each GREEN topic's
success-only artifacts, and `retrospective-record.md`. Always the main
checkout's path, never a worktree's: worktrees are removed.
`docs/r3d/reports/agentic-loop/<run-id>.md` (tracked): the run record,
its measured sections rendered by the script, pushed to the published
branch. The first two records were reconstructed by hand and say so.

## Verification

Done: `node tools/test_agentic_loop.mjs` parse-checks the script under
the workflow runtime's own body shape (a top-level `return`, a pure
`meta` literal, phase titles matched) and runs fixtures through its pure
decision functions — the gate plan from changed paths, the judgement of a
table by name (a missing gate, a `not-run` row, a nonzero exit, a row
without a log and an unconfigured plan are each red), the cheap judge,
the environment plan, the specialist policy and the record renderer; a
no-prompt invocation throws before any agent; a dry run spawns none and
prints the plan (run it with `scriptPath` pointing at the file: the
named-workflow registry can serve a cached copy of an edited script); `tools/gate-chain.sh --self-test` and
`tools/host-tests/run.sh` pass on a Linux host, and the runner's failure
paths are the ones the self-test exercises. The `NXDK_DIR` change to the
tools is a `${VAR:-default}` at each site with the default unchanged.

Not done, deliberately, because it needs the owner's machine: a first v2
run on a small slice, watching that the worktree proof passes, the gate
table matches the plan, codex runs only after green, the live look
produces readable screenshots, a red table really yields a `[QA BLOCKED]`
draft, and the retrospector's record lands on the branch. The first two
run records are the baseline the third is compared against.

## Explicitly deferred

A PC (`waf`) build gate; QA consulting a specialist mid-review; a
cost-tier knob scaling the model table; the retrospector applying
`lessons.md` entries itself; a reference-renderer pass over a design
note's claims run by the loop itself (the `Refereed:` header marks the
gap; a human closes it); a first v2 run's own seconds column and
timeline, which decide whether a sweep can be skipped on a round that
changed no engine file.
## History

v2 was shaped by an eight-lens adversarial audit of v1 against the two
runs (cost structure, gate integrity, the worktree environment,
design-note redundancy, live verification, codex economics, prompt load,
persistence), each finding checked by a refuter and the whole closed by
a completeness critic: 19 agents, about 2.8 M output tokens, 72 minutes
— more than run 1 itself cost. Of the ten findings verified, seven were
confirmed with amendments and three were "refuted" only because the
branch had already fixed them while the audit ran; the critic's fourteen
residual items are all applied. Worth its price once, for a loop that
will run many times; not a cadence.

Landing v2 followed the project's own cadence and then some: two codex
rounds on this branch found five real findings (the gate plan trusting
the implementer's file list; green judged from transcribed exit codes;
fix rounds able to empty the plan; a renderer change publishable green
without a look; `wait-scene` certifying an unloaded scene), all fixed
here with the attestation, the changed-files row, plan widening, the
real-diff renderer check and the continuity rule. A third run, made
before the second's fixes had been pulled, added two more that were
also fixed (ungated code blocking the PR; the live look validated by
the tools' own lines and screenshot hashes) and one about
`divergences.md` #119's wording on NV2A fan and strip primitives, which
is PR #149's ledger entry on `main`, not this branch: deferred to a
ledger edit of its own — the hardware has the primitive ops
(`deps/nxdk/lib/pbkit/nv_regs.h`), the entry's "with GL's own
vertex-reuse semantics" qualifier is the real reason, and the sentence
should say why decomposition was chosen anyway.

v1 (PR #119) landed with seven real findings from two codex rounds on
the design itself, all fixed there: one worktree per topic instead of a
shared checkout; a red final verdict publishing as a blocked draft rather
than a normal PR; the two-round codex cap tracked in the script rather
than in a stateless agent; submodules initialised in each worktree; the
`maxTasks` cap enforced by schema and code; a null research or
implementation result stopping the topic instead of becoming a false
green; specialists reading the topic's own worktree. All seven survive
in v2. What v1 could not know it lacked is what the two runs showed.
