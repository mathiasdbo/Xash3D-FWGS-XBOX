# The fast loop

`.claude/workflows/fast-loop.js` is the reduced version of the agentic
loop (`docs/r3d/agentic-loop.md`). Its goals are the same: take one
fork-plan slice from a prompt to a PR whose verdict was computed rather
than claimed. It gets there with fewer agents, fewer tokens, and less
wall clock spent on builds and xemu. v2 stays in the tree until the fast
loop has a measured run of its own. Compare the two with the run records,
not with this page.

## Shape

| Stage | Agent | Model | Calls per topic | What it does |
|---|---|---|---|---|
| Setup | `loop-runner` | Haiku (effort medium) | 1 per run | Picks the topics (a named slice is one topic). Runs the setup script, which creates the worktrees (submodules borrow the main checkout's objects via `--reference`) and writes `.sweep/nxdk-dir` and `.sweep/cc-cache-dir`. It proves the environment with one compile and reports whether origin/main's baseline build is already cached. |
| Baseline | `loop-runner` | Haiku | 0-1 per run | Builds origin/main once (sweep and full hl link, `--xbe`) under `.sweep/fast-loop/baseline/<sha>/` in the main checkout. Runs **while Build works** and is skipped when cached. |
| Build | `loop-implementer` | Sonnet | 1, plus 1 cheap retry, plus up to 2 fixes | Research and implementation in one call, self-gated with `tools/fast-gates.sh cheap`. |
| Verify | `loop-runner` | Haiku | 1-3 | `tools/fast-gates.sh full`, then `tools/live-look.sh` when a renderer path changed. Reads the screenshots. |
| Codex | `loop-runner` | Haiku | 0-4 calls, at most 2 counted rounds | `tools/codex-review.sh --base <origin/main>`, **alongside** Verify. A run that does not count (no thread, exit 2) is still a call, and it gets one codex-only retry. |
| Publish | `loop-runner` | Haiku | 1 | Re-checks the verdict on disk, pushes, opens the PR (a draft when RED) and reads back its draft state. |

A typical green topic uses 5-6 agents: setup, build, verify, codex and
publish, plus the baseline build on the first run after origin/main
moves. v2 used 8-10 typically and up to 23. Every agent is a leaf.
`args.dryRun` returns the arithmetic and every generated script without
spawning anything.

## Where the time went, and what replaced it

These come from runs 1 and 2 (`docs/r3d/reports/agentic-loop/`). Run 2's
last QA and live round alone took 96 of 230 minutes.

- **The same large build, again and again.** Every sweep and isolate
  recompiles every unit from scratch, which is deliberate: no stale object
  is ever vouched for. `tools/xbox-cc-cache.sh`, turned on per checkout
  by `.sweep/cc-cache-dir`, keeps that property and makes a repeated
  compile a copy:
  - The key is the preprocessed unit, every argument, the compiler
    identity and the script itself.
  - Repository and nxdk paths are tokens in the key, and are mapped out
    of `__FILE__` in the compile. As a result, a topic worktree reuses
    origin/main's objects for every unit the topic did not change.
  - It is off unless configured. Human runs are unchanged.
- **Two builds and two boots per look.** v2 packed and booted the
  baseline beside the branch every time. `tools/live-look.sh` caches
  origin/main's frame per scene (a key over the overlay, the `.mpak` and
  the frame count) under the baseline build's directory. A look then
  costs one pack and one boot, and none at all when the XBE is the one
  already looked at.
- **The baseline was on the critical path.** It now builds while Build
  researches and implements.
- **Codex waited for the build.** It now runs beside the full tier.
  - **What the cheap tier covers.** It compiles every changed engine `.c`
    file and isolates a changed renderer, which covers what cost run 1 a
    round.
  - **What it does not.** It does not compile headers, C++, `ref/null`,
    the SDK or build infrastructure. `meta.txt`'s `cheap_unchecked=`
    lists those paths.
  - **The residual risk.** For such a change, codex can review code that
    turns out not to build: the full tier and codex finish together. That
    risk is accepted for the wall-clock time it saves.
  - **Self-reported.** `cheapExit` itself is reported by the implementer.
    The full tier re-runs everything that counts.
- **Prose procedures.** v2's live stage was about 15 steps a model carried
  out by hand, and its gate plan was pasted into every prompt. Now each
  runner stage runs one generated script and returns files verbatim, and
  the gate plan is computed by `tools/fast-gates.sh` from the branch's
  own `git diff`. No file list is typed by an agent, so none needs
  checking.
- **Two thinking agents per round.** Research and implementation are one
  call; the spec hand-off, and the specialists a design note already
  covered, are gone.

With `args.live` set, the xemu mutex wraps the whole verify call, the full
build as well as the look. So with more than one topic, the topics' verifies
run one after another. Splitting the look into its own runner call would
cost an agent per look. Until a multi-topic run shows the wait matters,
the serialisation is accepted.

## How green is decided

The verdict is computed in the script. The functions are `judgeGates`,
`judgeLive`, `codexCounted` and `finalStatus`, and each one is tested
able to say RED (`tools/test_fast_loop.mjs`). GREEN needs all of these:

- **Gates.** Every gate `fast-gates.sh full` planned ran and exited 0, and
  gate-chain attested that exact plan. `meta.txt` names no missing
  baseline and no ungated code. A docs-only change has an empty plan,
  which is green on its own.
- **Live look**, when a renderer path changed (`ref/{nv2a,soft,common}`,
  `engine/client`):
  - `LIVE-LOOK-OK` is present.
  - The booted XBE's hash is the gated XBE's.
  - The branch scene line reads `scene ready:`.
  - The pixel diff matches the expectation Build stated.
  - The runner saw nothing on the PNG that contradicts it.

  Without `args.live`, a renderer change is RED ("never booted").
- **Review.** At least one counted codex round: a thread id, and exit 0,
  or exit 1 with findings.
- **Findings.** Every actionable finding (critical/high, or
  confidence ≥ 0.6) is fixed or justified. A justified one is listed in
  the PR with its reason.

The publish script then re-checks everything on disk before pushing:
- the tree is clean;
- HEAD is the attested commit;
- the plan and results digests match;
- a fresh `fast-gates.sh plan` of the same diff gives the same plan;
- the XBE's hash matches, and so does its `.identity` sidecar;
- the judged PNG's hash matches.

A failed check turns GREEN into a `[QA BLOCKED]` draft. The draft flag is
read back, never assumed.

Two kinds of failure never trigger a fix round, because Build cannot fix
them: no baseline, and `live-look.sh` exit 2 (a stub already answering,
packing failed). The topic ends RED with the reason. A codex run that
does not count gets one codex-only retry, never a second full verify.
AGENTS.md's cap of two review rounds holds: the second round runs only
when code changed after the first.

## Running it

```
Workflow({ name: 'fast-loop', args: {
  prompt: 'X9-Y step 2 per docs/r3d/design/x9-y.md',
  runId: '2026-09-28-x9-y',
  baselines: { engineSweepExpectFail: 29 },
  live: { xemuExe: '/c/xemu/xemu.exe', hlSteamDir: '/d/SteamLibrary/steamapps/common/Half-Life',
          map: 'crossfire', mpakPath: '/c/.../crossfire.mpak' },
} })
```

- `maxTasks`: default 1, at most 3.
- `models`: override `{ build, setup, runner }`, e.g. `build: 'opus'` for
  a hard slice.
- `baselines.engineSweepExpectFail`: needed whenever an engine file
  changes. Without it the sweep exits 0 on any number of failures, so the
  plan records it as missing and the topic cannot go green.

Logs and gate output go under `.sweep/fast-loop/<runId>/<slug>/` in the
main checkout. The run record (the agents and their output tokens, every
gate with its seconds, the live look's pack and boot seconds, the review
ledger, the owner-side items) is the PR body.

## Not measured yet

Nothing in this design has run on the Windows host. The first run must
record these; the record's gate seconds and `live-look` timings carry
most of them:

- **The cache hit rate.** Read it from the cache's `stats.log`.
  Preprocessing still runs on a hit. At `-O0` with no `-g`, how much a
  copy saves over a compile is the number to get.
- **The per-unit driver overhead on MSYS2.** Each `xbox-cc.sh` call is a
  bash process plus nxdk's `activate`. If that dominates a cached sweep,
  the next step is running units in parallel inside the sweeps. That was
  deliberately not done blind: those scripts are this project's trusted
  measuring tools.
- **Whether `-fmacro-prefix-map` sees MSYS2's Windows-form paths.** A
  form the map misses leaves an absolute path in the key. That is a miss,
  never a wrong hit, but it would show as a low hit rate across worktrees.
  Also unverified: how MSYS2 converts an argument like
  `-fmacro-prefix-map=/c/...=.` (a path after one `=`, another `=` after
  it). Running `-E` from two checkouts and diffing the output settles both.
- **Whether `kill` of a native xemu from MSYS2 bash is enough.**
  `live-look.sh` falls back to `taskkill` through `/proc/<pid>/winpid`.

The python gate runs the whole `tools/test_*.py` suite. Its
`test_xemu_pad.py` `SendProtocol` tests are timing-flaky on an unchanged
tree (seen 2026-09-28). A follow-up task is filed; until it lands, a red
`python-tests` row needs its log read.
