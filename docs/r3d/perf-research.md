# The perf-research workflow

`.claude/workflows/perf-research.js` is a **read-only** research workflow for
performance on the original Xbox (733 MHz Pentium III + NV2A), Quake-lineage
and GoldSrc engine code, and Pentium III class PCs. It suggests improvements,
names the code to change and drafts a plan for CPU-bound and GPU-bound scenes.
It implements nothing: its only write is one report under
`docs/r3d/reports/perf-research/`. To act on the report, feed a chosen item to
`fast-loop.js` or `agentic-loop.js` as a slice.

## Shape

| Stage | Agent | Model / effort | Calls | What it does |
|---|---|---|---|---|
| Survey | `console-performance-specialist` (CPU / Pentium III), `nv2a-xbox-specialist` (GPU), `render-architecture-specialist` (engine hot paths), `memory-budget-specialist` (memory, streaming) | sonnet / medium | 2-4, parallel | Each returns up to 12 structured candidates: bound, targets, `file:line` evidence, concrete change, expected gain with its basis, risk, how it is measured. |
| Referee | `reference-renderer-specialist` | opus / high | 0-1 | Confirms or refutes every claim about what the reference renderer does, by reading it. Skipped if no candidate makes one. |
| Plan | `perf-research-planner` | opus / high | 1 | Merges, drops refuted items into a "Refuted" section, writes the report. |

3-6 agents per run; `args.dryRun` prints the count without spawning anything.
Every agent is a leaf. The lens set follows `args`: `scenario: 'cpu'` drops the
GPU lens, `scenario: 'gpu'` drops the CPU lens, `target: 'pentium3'` drops the
NV2A lens.

Every `agent()` call passes its model and effort explicitly, so the agent
files' frontmatter is not what decides. The referee and planner get the top
tier because a wrong verdict or plan is paid for by whoever implements it
(`lessons.md`: never list an optimisation as a port without grepping the
reference); the lenses are checklist-shaped surveys. Override per run with
`args.models = { lens: { model, effort }, referee: {...}, planner: {...} }`.

## Arguments

```
Workflow({ name: 'perf-research', args: {
  focus: 'world rendering on a busy map',   // free text, also names the report
  scenario: 'cpu' | 'gpu' | 'both',         // default both
  target:   'xbox' | 'pentium3' | 'both',   // default both
  paths: ['ref/gl/gl_rsurf.c'],             // optional starting points
  slug: 'world-rendering',                  // optional report filename
  dryRun: true } })
```

## Why it is built this way

- **The referee exists because this area has been wrong before.** Five of
  nineteen planned "ports from Ironwail" were already in the tree
  (`optimization-transfer.md`, `xbox-performance-review.md`). Lenses must put
  any statement about the reference in `referenceClaim`, and a separate
  specialist checks it.
- **Bound, not hope.** Every item is CPU, GPU-fill, GPU-vertex, GPU-state/DMA,
  memory or I/O bound, and the plans are separate. The Xbox is GPU-rich and
  CPU-poor; a Pentium III PC is the reverse on many scenes.
- **Principles transfer, techniques may not.** No compute, indirect draw or
  bindless textures on the NV2A. The `transfers` field makes each lens say why.
- **Measured, not estimated.** Each item names the frame-time recorder counter
  (`frametime-recorder.md`) or xemu scene that settles it, and says
  "hardware-only" where xemu timing cannot (xemu is not NV2A timing).
- **Fan-out is scripted in the open**, not delegated at runtime, per the
  decision recorded in `agentic-loop.md`.
