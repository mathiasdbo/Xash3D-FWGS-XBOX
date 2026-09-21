# The agentic loop — a repo-native multi-agent Workflow

**Status: built 2026-09-21; two `codex-review.sh` rounds run against this
design itself, seven real findings across both, all fixed (the project's
own two-round cap for this PR is now fully used); not yet run on a real
task.** This document is
the design record for `.claude/workflows/agentic-loop.js` and the six
`loop-*` agents in `.claude/agents/` — why it's shaped the way it is, the
model/effort choice per stage and why, the logging convention, and where
XMEX (a separate, external tool) fits in. Not a `divergences.md` entry:
that ledger is scoped to deviation from upstream Xash3D-FWGS engine
behavior, and this is tooling/process, orthogonal to it.

## Why this exists

Driving a phase of this project by hand — research, implement, verify,
iterate, publish — is exactly what this session did manually for X9-F
(frustum culling in `ref/nv2a`): real, careful, but entirely
conversation-driven, with no repeatable structure. This workflow is that
same shape, made explicit and repeatable: one general prompt in, a bounded
set of atomic tasks out, each one researched, implemented, QA'd (with a
bounded automatic retry), and published as its own PR, followed by a
retrospective over what happened.

## The lesson this design is a direct answer to

This project already built a heavier multi-agent Workflow once, and
deleted it. `tools/codex-review.sh:31-37`:

> Replaces `.claude/workflows/pr-review.js` (deleted 2026-09-07): that
> workflow's 6-lens/3-refuter fan-out cost 25-37 Claude subagent calls and
> 2-2.6M subagent tokens PER ROUND, and needed 11 rounds to close one
> branch. This moves review compute onto Codex instead of Claude's own
> token budget — see AGENTS.md's "Review on every PR" section for the
> process this now backs (one review, fix/justify, one re-review, open the
> PR — capped at two rounds).

Two design rules follow directly from that history, and both are load-bearing,
not stylistic:

1. **No stage agent can spawn further subagents.** Every `loop-*` agent's
   own tool list omits `Agent`/`Task`, matching the 8 existing specialists.
   All fan-out — which specialists get consulted for which topic, how many
   topics run per invocation, how many QA retries a task gets — is decided
   in `agentic-loop.js` itself, in the open, computable by reading the
   script rather than a function of what a model decides to do at runtime.
2. **Code review is not re-implemented with more Claude subagents.**
   `loop-qa` calls `tools/codex-review.sh` directly — the exact tool the
   project already settled on for this job, running off Claude's own token
   budget entirely.

**A codex-review pass on this very design found three more real problems**
before it ever ran on a real task, all fixed and re-verified by a clean
syntax/guard-clause pass afterward (see "Verification" below):

1. **Concurrent topics were mutating the same checkout.** `pipeline()`
   deliberately has no barrier between topics — topic B's Implementation
   can be running while topic A is still in its own QA retry loop. Without
   a real, separate checkout per topic, that's a genuine git-corruption
   risk (shared index/HEAD/working tree), not a hypothetical one. Fixed:
   `loop-setup` now creates one isolated git worktree and branch per topic,
   sequentially, before anything else runs; every later stage for that
   topic is told to work only inside its own worktree path.
2. **A final red QA verdict was published unconditionally.** The original
   script called `loop-publisher` the same way whether the task's last QA
   round came back green or red after exhausting its automatic retries and
   spec revision — `loop-publisher`'s own prompt assumed QA had already
   greenlit the change. Fixed: the script now states the final status
   (`GREEN` vs `RED/BLOCKED`) explicitly in Publisher's own prompt, and
   `loop-publisher` opens a normal PR only for `GREEN` — a still-red task
   gets a `[QA BLOCKED]` **draft** PR instead, with QA's own correction
   list leading the description, never worded as if the change were ready.
3. **The loop could exceed the project's real 2-round `codex-review.sh`
   cap.** A task can call `loop-qa` up to 4 times (3 rounds plus one
   post-revision check), and each call is a fresh, stateless agent with no
   memory of how many real review rounds earlier calls already spent.
   Fixed: the script now tracks `codexReviewRoundsUsed` per topic and
   passes `codexReviewRoundsRemaining` into every QA call; QA only invokes
   `codex-review.sh` when at least one round remains, and reports back
   whether it actually did via `usedCodexReview` so the count stays
   accurate across calls that share no other state.

**A second codex-review round, on the fixes above, found four more** — the
project's own real cap on review rounds (two) applies to this design's own
PR exactly like any other, so all four were fixed in this same round,
re-verified by the same clean guard-clause pass, with no third round:

4. **Fresh worktrees had no submodules.** `git worktree add` does not
   initialize submodules in the new checkout, and this project's own real
   build tooling hard-depends on several (`tools/xbox-cc.sh` fails outright
   without `3rdparty/library_suffix`/`3rdparty/opusfile`, and invokes
   `deps/nxdk/bin/activate` from wherever it's run). Every engine-touching
   task would have failed QA's build/link chain before ever testing the
   actual change. Fixed: `loop-setup` now runs `git submodule update --init
   --recursive` inside each worktree right after creating it.
5. **The advertised `maxTasks` cap was only a prompt instruction.** The
   schema had no upper bound on the topic list, and the only JS-side check
   was "non-empty" — a confused `loop-setup` could return more topics than
   asked and the script would run all of them: extra worktrees, extra
   specialist calls, extra branches and PRs. Fixed: the setup schema now
   carries a real `maxItems: maxTasks` (validated at the tool-call layer,
   not just requested in text), plus a JS-side truncation as defense in
   depth that logs exactly what got dropped if it's ever hit.
6. **A dead Research or Implementation call could produce a false GREEN.**
   `agent()` can return `null` (skipped, or a terminal API error after
   retries); the script fed that into the next stage as a harmless-looking
   placeholder string instead of treating it as the terminal failure it is.
   A null Implementation result in particular means no commits exist at
   all — QA's own checks would then run against an untouched worktree, find
   nothing wrong, and could hand back a real GREEN for a task that was
   never implemented. Fixed: a null spec or a null first-implementation
   result now stops that task immediately, before QA or Publisher ever run,
   returned as an explicit `failed` result instead. A null result on a
   *retry* fix or the post-revision calls keeps the last known-good
   (non-null) spec/changes rather than discarding real prior work, letting
   the next QA round re-judge what's actually there.
7. **Specialists were reading a different checkout than their own topic's
   worktree.** The main checkout `loop-setup` runs in is explicitly allowed
   to be dirty or on a different branch, while each topic's own worktree is
   a fresh `origin/main`. Without a shared reference point, a specialist
   could cite code that isn't in the topic's actual worktree, or miss code
   that is — and Research is told to trust specialist input as verified
   fact. Fixed: every specialist call is now given the topic's own
   `worktreePath` explicitly and told to read from there, the same
   checkout Research/Implementation will actually work in.

## Architecture

```
SetUp (once, Haiku)
  |
  v
Specialists (parallel, per topic — bounded, 0-2 specialists per topic typically)
  |
  v
pipeline(topics, researchImplementQaPublish)   <- no barrier between topics;
  |                                                topic 2 can be mid-research
  |                                                while topic 1 is mid-QA-retry
  v
Retrospector (once, Opus, reads every topic's own log)
```

Inside `researchImplementQaPublish`, per topic:

```
Research (Opus) -> Implementation (Sonnet) -> QA (Sonnet), up to 3 rounds
                                                 |
                                    round 3 still red?
                                                 v
                              Research revises the spec (Opus) -> Implementation -> QA
                                     (one revised attempt - then Publisher runs either way,
                                      GREEN as a normal PR or RED as a [QA BLOCKED] draft)
                                                 |
                                                 v
                                          Publisher (Sonnet)
```

The QA retry cap is 3 automatic rounds **per task**, plus one further
attempt after a spec revision. Either way, Publisher runs exactly once at
the end and always publishes something: a task that's still red after
every automatic attempt gets a `[QA BLOCKED]` draft PR with QA's own
correction list, not a silently dropped worktree — never a normal PR
implying the change is ready. `setup.topics` is capped at `args.maxTasks`
(default 4), logged explicitly if any topics were dropped over that cap —
no silent truncation.

## Model / effort per stage, and why

| Stage | Model / effort | Reasoning |
|---|---|---|
| `loop-setup` | Haiku, medium | Bounded, mostly-mechanical triage; runs once per invocation; a shallow miss here (wrong specialist flagged) is cheaply caught downstream |
| `loop-research` | **Opus, high** | The highest-leverage stage, revised up from an initial Sonnet/high call: a bad spec is paid for again through every downstream stage it touches — Implementation (up to 4 attempts), QA (up to 3 rounds), and possibly a full revision-and-retry cycle. That's roughly an 8-10x amplification of any mistake made here, which is exactly the situation where paying more upfront is the token-*optimizing* choice, not the expensive one. Also the tool's specialist-consultation output already does much of the narrow fact-checking (see below), so Research's own job is synthesis and judgment, which is where the model tier matters most. |
| `loop-implementer` | Sonnet, high | Real code-correctness stakes on a memory/console-constrained target; a good spec should make this close to mechanical, but Xbox-class bugs are expensive to catch late |
| `loop-qa` | Sonnet, high | Judgment-heavy synthesis over mechanical tool output; the single most expensive part of review is delegated to `codex-review.sh`, off this budget entirely |
| `loop-publisher` | Sonnet, medium | Mostly mechanical git/gh operations plus precise convention-following; doesn't need heavy reasoning |
| `loop-retrospector` | Opus, high | Runs once per invocation — infrequent enough that the top tier is affordable — over a task (curating candidate `docs/r3d/lessons.md` entries) where a bad proposal costs more than a missed one, per that file's own meta-rules |

**Specialist calls**, made by the workflow script itself (not by a stage
agent) with an explicit override, since the 8 existing specialist `.md`
files don't set a model of their own:

- `reference-renderer-specialist`, `upstream-fidelity-specialist` → **Opus,
  high**. These two exist specifically to catch the class of expensive,
  subtle mistake this project has already paid for once — `lessons.md`'s
  own "NEVER list an optimisation as a port without grepping the reference
  for it" entry: five of nineteen claimed ports were already in the tree,
  and one claim ran backwards even after specialist review existed.
- The other six → **Sonnet, medium**. Narrower, more checklist-shaped
  domains (build/link mechanics, memory arithmetic, performance
  principles).

**Fable** isn't defaulted anywhere in this design. There's no grounded read
yet, this session, on where it's actually strong relative to the other
three tiers, and guessing an untested model into infrastructure work is
exactly what this project's own "verify, don't assume" culture pushes
against. If it's ever tried, the natural first place is `loop-publisher`'s
own PR-description prose — low-stakes, easy to compare against a Sonnet
run on the same input — not a judgment-heavy stage.

## Logging (agents write; the Retrospector reads)

Under `.sweep/agentic-loop/<run-id>/` — the same gitignored scratch
convention every other `.sweep*` staging directory in this repo already
uses (raw, verbose, ephemeral, never committed):

- `00-setup.md` — SetUp's own run header: the prompt, the topic list (and
  any dropped over the cap, with why), the specialists flagged per topic,
  the git state observed.
- `<topic-slug>.md` — one growing log per task, appended in turn by
  Research → Implementation → QA (each retry) → Publisher. Safe without
  locking: within one task, `pipeline()` runs its stages strictly
  sequentially, so appends to one task's own file never race.
- `retrospective.md` — the Retrospector's own final report.

**The Retrospector's write scope is deliberately narrow for now**: it reads
every log and the repo, and writes only its own report. Candidate
`docs/r3d/lessons.md` entries are written out in full, ready-to-paste form,
in that report — for a person to actually commit, not applied
automatically. A `lessons.md` write and an agent-prompt edit are both
hard-to-cleanly-reverse, shared-state changes; starting read-only on the
output side and widening later if the proposals prove consistently good is
the right order to earn that trust, not assume it upfront.

## XMEX — a real, external, complementary tool

[XMEX](https://github.com/mathiasdbo/XEMEX) is a separate, actively
developed project: a Unicorn-Engine-based Xbox XBE micro-execution and
forensic-analysis tool (CLI, Python SDK, and a FastAPI REST/WebSocket
server), independent of this repo. It is **not** a full-system emulator —
no real GPU, no rendering — so it never substitutes for `tools/
xemu-debug.py`'s own screenshot/memory reads on anything about the
renderer or a pixel (see `docs/r3d/lessons.md` for why this project treats
that distinction as load-bearing: xemu is the only validation surface for
rendered output).

Where it earns a real place in this loop, confirmed against its own
current CLI reference (not the original, narrower spec sketch — the real
project has grown well past that):

- **`loop-research`**: `xmex info <xbe>` gives a real, decoded header/
  certificate/section/kernel-thunk table for any already-built XBE — a
  fact worth checking against instead of reasoning about from source alone.
  `xmex disasm-shader <blob> --type {vsh,psh}` disassembles a compiled
  VP20/FP20 shader blob to D3D assembly, directly useful for any topic
  touching `ref/nv2a`'s own `.inl` shader blobs.
- **`loop-implementer`**: the same `disasm-shader` command, as a cheap
  self-check right after producing a new or changed shader blob, before
  handing off to QA.
- **`loop-qa`**: `xmex run <xbe_or_iso> --symbols <the project's own real
  .map file> --max-instructions N` boots straight into Unicorn with call
  telemetry resolved against this project's own real linker maps (nxdk/GCC
  format is explicitly supported) — much faster than a full xemu boot for a
  pure "does this code path run without a kernel-thunk crash" question.
  `xmex run ... --gdb 1234` speaks the same GDB Remote Serial Protocol
  `tools/xemu-debug.py` already speaks to xemu — useful, but its own
  memory-layout assumptions should be spot-checked against `xmex info`'s
  own reported base address before trusting a second read, not assumed to
  transfer unchanged from the xemu case.
- Its telemetry subsystem (memory access heatmap, NV2A pushbuffer sniffer,
  ZoneAllocator tracker) overlaps `memory-budget-specialist`'s and
  `nv2a-xbox-specialist`'s own domains directly enough that, when either is
  consulted for a topic, its findings may already cover what a real XMEX
  run would show.

Every `loop-*` agent that mentions XMEX is told to check for a local clone
before assuming it's unavailable, and to say so once and move on — via the
existing tool chain alone — rather than block on a tool that may not be
present in a given environment.

## Invocation

```
Workflow({ name: 'agentic-loop', args: { prompt: '<general instruction>', runId: '<a real timestamp-based id>', maxTasks: 4 } })
```

`runId` must be supplied by the calling session (the script itself cannot
call `Date.now()`/`new Date()` — see the workflow tool's own resume
contract) — a plain `YYYYMMDD-HHmmss`-shaped string is enough to keep
concurrent runs' logs apart.

## Verification before trusting this on real X9-series work

Done so far: the script parses and its guard clauses run correctly (a
no-`args.prompt` invocation fails in under 50ms with zero agents spawned,
confirmed twice — once before and once after the codex-review fixes
above). Two rounds of `tools/codex-review.sh` on the design itself, both
with real findings, both fixed (see above and the PR history).

Not done yet, deliberately — this is real, live-agent, potentially
PR-creating automation, and its first real run deserves a person watching,
not an autonomous first attempt:

- Each `loop-*.md` is a self-contained prompt — sanity-check one standalone
  via the `Agent` tool on a trivial read-only question first.
- Run the workflow once on a small, low-stakes prompt (a doc fix, not an
  engine change) to confirm end to end: schemas validate, worktrees are
  created and used correctly (not the main checkout), logs land where
  expected, the QA retry loop and — ideally — the 3-round escalation path
  both get exercised at least once, a red final verdict really does
  produce a `[QA BLOCKED]` draft rather than a normal PR, the
  `codex-review.sh` round count stays within 2 across however many QA
  calls a task makes, and the Retrospector produces a real, specific
  report rather than a generic one.
- Landing this file and the agents/workflow script themselves followed the
  repo's own standing workflow: a branch, `tools/codex-review.sh --base
  main` (capped at two rounds like everything else), a PR merged by a
  person — see `CHANGELOG.md`.

## Explicitly deferred

QA consulting a specialist mid-review (kept to a single Research-time
consultation for v1); a cost-tier knob (`args.tier`) scaling the whole
model table cheaper or pricier per run; Fable, anywhere, until it's
actually been tried against a real task; the Retrospector graduating from
"propose" to "apply" on `lessons.md` or on other agents' own prompt files.
