# Agentic-loop run records

One file per run of `.claude/workflows/agentic-loop.js`, committed on the
run's own topic branch so it lands with the PR. The loop's stage logs live
under `.sweep/agentic-loop/<run-id>/`, which is gitignored: the first two
runs' logs and retrospectives are gone with the scratch directory, and the
owner's own numbers for them (agents, tokens, tool calls, wall clock) were
recalled from the Workflow UI, not read from anything the loop wrote. This
directory is the loop's memory across runs; `loop-retrospector` reads every
file here before writing the next one.

The measured sections (Scope, Cost, Gates, Review, Live) are rendered by
the workflow script from its own measurements and written verbatim; the
retrospector adds only Observations, Missed and Lessons. A number in a
record therefore never passed through a model's transcription. Each
record has the same sections, so runs can be compared:

- **Scope** - the prompt, the topic, the PR, whether a design note or an
  inventoried `fork-plan.md` step already existed for it.
- **Cost** - agents by stage, output tokens by stage (the script samples
  `budget.spent()` around every agent call), wall clock (the caller passes
  `startedAt` through `args` and the retrospector notes when it ran; the
  script itself cannot read a clock).
- **Gates** - every mechanical gate the script required, whether it ran, its
  exit code and log path. A green verdict in this loop is computed from this
  table by the script, never reported by a model.
- **Review** - codex-review rounds used, findings above the fix-or-justify
  threshold, and what each round's diff had already been through.
- **Live** - what was booted, on what, what was looked at and what was not.
- **Missed** - what a human found after the loop declared the task done.
  Filled in post-merge by whoever found it; the most valuable field here.
- **Lessons** - candidate `docs/r3d/lessons.md` entries the retrospector
  proposes, in that file's own entry shape. A human decides whether they
  earn a place; nothing is written to `lessons.md` by the loop.

The first two records below were reconstructed after the fact from PRs
#148 and #149, their commit messages and the owner's notes, so their cost
figures are approximate and their gate tables are partial. Every later
record is written by the loop itself.
