# Agents

Two groups here, kept deliberately separate.

## Specialist agents

One per real risk area in this project. Pick the ones a change actually
touches; never ask permission first (see `/AGENTS.md`).

They exist because the failure modes here are domain-specific and mostly
silent: a symbol collision that links, a texture that swizzles wrong only on
the console path, a budget that counts half the memory, a divergence from
upstream nobody recorded.

One of them is a check on the others. `reference-renderer-specialist` confirms
or refutes any statement about what the reference renderer does, by reading
it and citing the line. It was added after five of nineteen planned "ports
from Ironwail" turned out to already be in the tree, and one claim about the
reference had been copied into another specialist's prompt while false.

The 8: `asset-pipeline-specialist`, `build-toolchain-specialist`,
`console-performance-specialist`, `memory-budget-specialist`,
`nv2a-xbox-specialist`, `reference-renderer-specialist`,
`render-architecture-specialist`, `upstream-fidelity-specialist`.

## Loop-stage agents (`loop-*`)

The six stages of `.claude/workflows/agentic-loop.js` (SetUp, Research,
Implementation, QA — which also does the live xemu look — Publisher,
Retrospector). See `docs/r3d/agentic-loop.md` for the design, the
model/effort choice per stage and why, and `docs/r3d/reports/agentic-loop/`
for what each real run cost. Since v2 the stages measure and report;
the verdict (green or blocked) is computed in the script from the gate
table QA returns, matched by name against the plan the script wrote. Not meant for ad-hoc standalone
use the way the specialists are — each one's own job only really makes
sense inside that workflow's own control flow, though each carries a
sensible default `model`/`effort` in its frontmatter in case it's ever
invoked directly.

One split governs what a `loop-*` file may say, because per-call facts
kept in two places have already drifted (the audit of v2 found three
pairs disagreeing, one on which ref codex reviews against): an agent file
states the stage's **role**, its **invariants** (what it must never do)
and the **harness traps that never change** between calls; every path,
command, plan line, base SHA, status and configured value lives only in
the workflow's per-call message or in a schema description. If a fact
would have to be edited in both, it belongs in the script.

Neither group can spawn further subagents (no `Agent`/`Task` in any of
their tool lists, specialists and loop-stage agents alike). All fan-out —
which specialist gets called for which topic, how many topics run in
parallel, how many QA retries a task gets — is scripted in the open, in
`agentic-loop.js` itself, not left to a model deciding to delegate further
at runtime. That's a direct, deliberate answer to a real, expensive mistake
this project already made once: see `docs/r3d/agentic-loop.md`'s own
account of `.claude/workflows/pr-review.js`, the multi-agent review
workflow this project built and then deleted for costing 25-37 subagent
calls and 2-2.6M tokens per round.
