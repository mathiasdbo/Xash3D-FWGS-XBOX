export const meta = {
  name: 'pr-review',
  description: 'Specialist review of a branch or PR: pick lenses from the diff, then refute every finding',
  whenToUse: 'Before opening or merging any PR. Required by AGENTS.md.',
  phases: [
    { title: 'Scope', detail: 'read the diff and choose which specialists apply' },
    { title: 'Review', detail: 'one specialist per applicable lens, in parallel' },
    { title: 'Verify', detail: 'three adversarial refuters per finding' },
  ],
}

// args: { base?: string, head?: string }   defaults to main..HEAD
const BASE = (args && args.base) || 'main'
const HEAD = (args && args.head) || 'HEAD'
const RANGE = `${BASE}..${HEAD}`

const PREAMBLE = `You are reviewing the Resonance3D repository, a fork of Xash3D-FWGS
being ported to the Original Xbox with nxdk (\`deps/nxdk\`).

Review the change \`git diff ${RANGE}\`.

**READ-ONLY.** Do not edit, build, or run anything that writes to the tree — a
session may be working in it. \`make clean\` in \`deps/nxdk\` has destroyed a
build mid-session before. Read files and reason; compile to a scratch path only
if you must, never into the tree.

Standing context, so you do not rediscover it:
- 64 MB shared with the GPU. The NV2A is fixed-function plus VP20 and register
  combiners: no compute, no indirect draw, no bindless textures.
- \`XASH_XBOX\` is a **sub-platform of \`XASH_WIN32\`**, so Win32 code paths are
  live. nxdk's Win32 is a subset — check before assuming a function exists.
- \`common/port.h:47-48\` rewrites \`open\`→\`_open\` and \`read\`→\`_read\`.
- \`compat/xbox/\` implements POSIX file I/O over Win32 because nxdk has none.
- \`AGENTS.md\` ranks divergence forms by maintenance cost; \`docs/r3d/\`
  carries the plan, the divergence ledger and \`lessons.md\`.

Cite \`file:line\` for every claim. Report defects you can point at, not style
preferences. A finding with no concrete failure path is noise.`

const LENSES = {
  posix: {
    label: 'review:posix-semantics',
    when: d => /compat\/xbox\/|filesystem\//.test(d),
    prompt: `Audit the POSIX compatibility layer and its call sites.

Check the cases that bite: partial reads and writes; \`lseek\` past EOF and
return values indistinguishable from an error; 64-bit offsets truncated;
\`O_APPEND\` on every write versus only at open; \`errno\` values callers
actually test; descriptor reuse and double close; \`readdir\` and "." / "..";
buffer sizes and off-by-ones in path handling.

Read the real call sites in \`filesystem/\` — a function is only wrong if
something calls it that way. A previous review found a stub whose justifying
comment inverted the guard it cited, and that bug made every file inside
\`pak0.pak\` unreadable. Comments asserting a path is unreachable are claims:
verify them.`,
  },
  xbox: {
    label: 'review:xbox-platform',
    when: d => /compat\/xbox\/|engine\/platform\/|ref\/|deps\/nxdk|tools\/xbox/.test(d),
    prompt: `Audit the Xbox and nxdk assumptions.

Verify every Win32 or kernel function used actually exists in
\`deps/nxdk/lib/winapi/\` or \`deps/nxdk/lib/xboxkrnl/\` with the signature
assumed — nxdk's Win32 is a subset and the gaps are not where you would guess.
Check FATX path semantics: drive letters, backslashes, no working directory,
path length. Check anything that allocates against the 64 MB budget, and
anything per-frame or per-file-open against a 733 MHz CPU.`,
  },
  upstream: {
    label: 'review:divergence-discipline',
    when: d => true,
    prompt: `Audit this change as an upstream-fidelity reviewer.

Read \`AGENTS.md\`'s divergence ladder and \`docs/r3d/divergences.md\`. For every
edit to an upstream-maintained file, judge whether it is the cheapest form that
works, whether it is recorded accurately, and whether a compat-layer approach
could have avoided it.

For each guard added around upstream code, verify it is genuinely needed AND
that the branch it now falls into is correct for Xbox rather than merely
compiling. **A block guarded unnecessarily is a silent behaviour change.**

Report ledger entries that are missing, and claims in the ledger the diff does
not support.`,
  },
  build: {
    label: 'review:build-and-tooling',
    when: d => /tools\/|wscript|Makefile|CMakeLists|\.gitattributes|\.gitignore/.test(d),
    prompt: `Audit the build and tooling changes.

Where a script replicates something else — \`tools/xbox-cc.sh\` mirrors
\`deps/nxdk/bin/nxdk-cc\` with the include order changed — compare line by line
and report anything dropped, added or reordered beyond the intended difference.
Assess drift risk when the original is updated.

**Then audit the measurement itself.** Every measuring tool in this project has
reported a false result at least once: a sweep that judged success by grepping
for "error:" reported "182 ok, 0 failing" for a directory whose first file
emits twenty errors, and the same sweep later reported the inverse when a
shebang picked the wrong shell. Ask what this tool would report if it were
broken, and whether that is distinguishable from success.`,
  },
  perf: {
    label: 'review:console-performance',
    when: d => /engine\/(common|server|client)\/|ref\//.test(d),
    prompt: `Review performance for a 733 MHz CPU with 64 MB shared with the GPU.

Look for allocations in per-frame or per-entity paths, work done eagerly that
the reference memoises lazily, unconditional logging (a debug line costs
milliseconds on a serial console), and hash or string lookups where an index
would do.

Ironwail's *principles* transfer — less CPU per frame, batch, sort by state,
never allocate in the loop. Its *techniques* do not: no compute shaders, no
indirect draw, no bindless textures on this hardware. Flag anyone conflating
the two.`,
  },
}

const FINDING = {
  type: 'object',
  additionalProperties: false,
  required: ['findings'],
  properties: {
    findings: {
      type: 'array',
      items: {
        type: 'object',
        additionalProperties: false,
        required: ['file', 'line', 'severity', 'summary', 'failure_scenario'],
        properties: {
          file: { type: 'string' },
          line: { type: 'integer' },
          severity: { type: 'string', enum: ['critical', 'high', 'medium', 'low'] },
          summary: { type: 'string' },
          failure_scenario: { type: 'string', description: 'Concrete inputs or state, then the wrong outcome' },
        },
      },
    },
  },
}

const VERDICT = {
  type: 'object',
  additionalProperties: false,
  required: ['real', 'reasoning'],
  properties: {
    real: { type: 'boolean' },
    reasoning: { type: 'string' },
    correction: { type: 'string' },
  },
}

phase('Scope')

const scope = await agent(
  `${PREAMBLE}\n\nList the files changed in \`git diff --name-only ${RANGE}\`, ` +
  `then return them as one newline-separated list and nothing else. No commentary.`,
  { label: 'scope:changed-files', phase: 'Scope' }
)

const diffText = String(scope || '')
const active = Object.entries(LENSES).filter(([, l]) => l.when(diffText))

log(`${active.length} of ${Object.keys(LENSES).length} lenses apply to this change`)
active.forEach(([k]) => log(`  lens: ${k}`))

phase('Review')

const raised = (await parallel(
  active.map(([key, l]) => () => agent(`${PREAMBLE}\n\n---\n\n${l.prompt}`, {
    label: l.label, phase: 'Review', schema: FINDING,
  }).then(r => r ? r.findings.map(f => ({ ...f, lens: key })) : []))
)).filter(Boolean).flat()

log(`${raised.length} findings raised`)

if (raised.length === 0) {
  return { lenses: active.map(([k]) => k), raised: 0, confirmed: [] }
}

phase('Verify')

// Three refuters per finding, told to default to "not real". A finding that
// survives majority refutation is worth acting on; one that does not is noise
// that would otherwise cost a real fix and a real regression risk.
const verified = await pipeline(
  raised,
  (f, orig, i) => parallel([0, 1, 2].map(v => () => agent(
    `${PREAMBLE}\n\n---\n\nA reviewer raised this. Try to REFUTE it.\n\n` +
    `File: ${f.file}:${f.line}\nSeverity claimed: ${f.severity}\n` +
    `Claim: ${f.summary}\nFailure scenario: ${f.failure_scenario}\n\n` +
    `Read the code yourself and trace whether that scenario can actually occur ` +
    `given how this code is called on this target. Default to real=false when ` +
    `the input is never passed, the branch is never taken, or the reviewer ` +
    `misread. Set real=true only with a concrete path to the wrong outcome. ` +
    `Put corrections, including to blast radius, in "correction".`,
    { label: `verify:${f.lens}:${i}:${v}`, phase: 'Verify', schema: VERDICT }
  ))).then(votes => {
    const v = votes.filter(Boolean)
    const realCount = v.filter(x => x.real).length
    return (v.length && realCount > v.length / 2)
      ? { ...f, votes: `${realCount}/${v.length}`,
          corrections: v.map(x => x.correction).filter(c => c && c.trim()) }
      : null
  })
)

const confirmed = verified.filter(Boolean)
log(`${confirmed.length} of ${raised.length} survived refutation`)

return {
  range: RANGE,
  lenses: active.map(([k]) => k),
  raised: raised.length,
  confirmed: confirmed.sort((a, b) => {
    const order = { critical: 0, high: 1, medium: 2, low: 3 }
    return order[a.severity] - order[b.severity]
  }),
}
