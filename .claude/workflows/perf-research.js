// perf-research.js - Resonance3D's research workflow for game performance:
// original Xbox (NV2A + 733 MHz Pentium III), Quake-lineage and GoldSrc
// engine hot paths, and Pentium III class PCs. READ-ONLY: it suggests
// improvements, names the code to change and drafts a CPU-bound and a
// GPU-bound plan; it implements nothing and writes exactly one file, the
// report under docs/r3d/reports/perf-research/. Design record:
// docs/r3d/perf-research.md.
//
// Shape (agent count is computable from this file; args.dryRun prints it):
//   Survey   - up to 4 lens agents in parallel, each a specialist already
//              in .claude/agents/ (cpu/Pentium III, NV2A/GPU, engine
//              hot paths, memory/streaming). Each returns structured
//              candidates with a file:line citation.
//   Referee  - 1 reference-renderer-specialist call that confirms or refutes
//              every claim about what the reference renderer does, because
//              plans here have been wrong about that before (lessons.md).
//   Plan     - 1 perf-research-planner call that merges, drops what was
//              refuted and writes the report.
//
// Every agent is a leaf (no Agent/Task tool), as in the other workflows.

export const meta = {
	name: 'perf-research',
	description: 'Read-only performance research for Xbox / Quake / GoldSrc / Pentium III: lens survey -> reference referee -> ranked improvements, code to change and a CPU-bound / GPU-bound plan',
	phases: [
		{ title: 'Survey', detail: 'parallel lenses: CPU (Pentium III), GPU (NV2A), engine hot paths, memory and streaming' },
		{ title: 'Referee', detail: 'reference-renderer-specialist confirms or refutes every claim about the reference' },
		{ title: 'Plan', detail: 'merge, drop refuted items, write docs/r3d/reports/perf-research/<slug>.md' },
	],
}

// ---------------------------------------------------------------------------
// Arguments
// ---------------------------------------------------------------------------

const A = args || {}
const focus = A.focus ? String(A.focus) : 'whole frame: find the biggest CPU-bound and GPU-bound costs'
const scenario = ['cpu', 'gpu', 'both'].includes(A.scenario) ? A.scenario : 'both'
const target = ['xbox', 'pentium3', 'both'].includes(A.target) ? A.target : 'both'
const paths = Array.isArray(A.paths) ? A.paths.map(String) : []
const slug = String(A.slug || focus).toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '').slice(0, 48) || 'perf-research'
const dryRun = !!A.dryRun

// Model and effort per stage, explicit on every agent() call. The referee
// is the stage whose mistakes lessons.md records (false "port X" items), and
// the planner writes the one deliverable, so both get the top tier; the
// lenses are checklist-shaped domain surveys. Override per run with
// args.models = { lens: { model, effort }, referee: {...}, planner: {...} }.
const MODELS = {
	lens: { model: 'sonnet', effort: 'medium' },
	referee: { model: 'opus', effort: 'high' },
	planner: { model: 'opus', effort: 'high' },
	...(A.models || {}),
}
const reportPath = `docs/r3d/reports/perf-research/${slug}.md`

const TARGETS = {
	xbox: 'Original Xbox: 733 MHz Pentium III (Coppermine-128, 128 KB L2, SSE, no out-of-order surprises to count on), NV2A at 233 MHz (fixed-function T&L, VP20 vertex programs, register combiners, no compute, no indirect draw), 64 MB unified memory, serial-console logging cost. GPU-rich, CPU-poor.',
	pentium3: 'Pentium III class PC (450-1000 MHz, 256-512 KB L2, SSE, 133 MHz FSB, era GPU: TNT2/GeForce 256/2, Voodoo3): CPU and bus bound far more often than fill bound; software T&L fallbacks, 32-byte cache lines, no SSE2, x87 FPU latency.',
}
const targetText = target === 'both' ? `${TARGETS.xbox}\n${TARGETS.pentium3}` : TARGETS[target]

const LENSES = [
	{
		key: 'cpu-p3',
		agentType: 'console-performance-specialist',
		on: scenario !== 'gpu',
		brief: 'CPU cost per frame on a Pentium III: per-frame allocations, hot loops (bones, BSP walk, PVS decompress, entity and edict iteration, particles, lightstyle/dlight surface marking), cache-line and prefetch behaviour, x87/SSE use, branchiness, logging cost. Name the function and the line.',
	},
	{
		key: 'gpu-nv2a',
		agentType: 'nv2a-xbox-specialist',
		on: scenario !== 'cpu' && target !== 'pentium3',
		brief: 'GPU-side cost on the NV2A: fill rate and overdraw, vertex throughput and post-T&L cache, state changes and register-combiner reprogramming, pushbuffer size and DMA fencing, texture formats, swizzle and bandwidth, VP20 limits. Also what is CPU cost spent submitting to the GPU (the Xbox is GPU-rich, CPU-poor).',
	},
	{
		key: 'engine-hotpaths',
		agentType: 'render-architecture-specialist',
		on: true,
		brief: 'Quake-lineage and GoldSrc engine hot paths in engine/ and ref/: world surface chain walk and per-polygon immediate-mode submission, VBO path state, lightmap upload and dynamic light updates, studio model setup, sprite and particle submission, sort-by-state opportunities, and what belongs in the shared layer rather than one backend. Every item must be CPU or GPU bound and say which.',
	},
	{
		key: 'memory-streaming',
		agentType: 'memory-budget-specialist',
		on: true,
		brief: 'Memory and streaming as a performance cost: zone pools and fragmentation, GPU-visible versus heap allocation, per-map load hitches, texture residency and mip bias, mpak loading, bake-time work that can replace per-frame work (the baker is our compute shader), and cache misses caused by data layout.',
	},
].filter(l => l.on)

// ---------------------------------------------------------------------------
// Schemas
// ---------------------------------------------------------------------------

const CANDIDATE = {
	type: 'object',
	properties: {
		id: { type: 'string', description: 'short unique id, prefixed with the lens key' },
		title: { type: 'string' },
		bound: { type: 'string', enum: ['cpu', 'gpu-fill', 'gpu-vertex', 'gpu-state-dma', 'memory', 'io'] },
		targets: { type: 'array', items: { type: 'string', enum: ['xbox', 'pentium3'] } },
		evidence: { type: 'array', items: { type: 'string' }, description: 'file:line citations you opened and read' },
		change: { type: 'string', description: 'the code change, concrete enough to start from' },
		referenceClaim: { type: ['string', 'null'], description: 'any statement about what the reference renderer (ref/gl, ref/soft) does or lacks, verbatim, else null' },
		expectedGain: { type: 'string', description: 'estimate with its basis; say "unmeasured" when it is' },
		risk: { type: 'string' },
		measure: { type: 'string', description: 'which frametime-recorder counter or xemu scene settles it; say hardware-only when xemu cannot' },
		transfers: { type: 'string', description: 'why this technique, not only its principle, runs on the target (no compute, no indirect draw on NV2A)' },
	},
	required: ['id', 'title', 'bound', 'targets', 'evidence', 'change', 'referenceClaim', 'expectedGain', 'risk', 'measure'],
}

const LENS_SCHEMA = {
	type: 'object',
	properties: {
		summary: { type: 'string' },
		candidates: { type: 'array', items: CANDIDATE, maxItems: 12 },
		unverified: { type: 'array', items: { type: 'string' } },
	},
	required: ['summary', 'candidates', 'unverified'],
}

const REFEREE_SCHEMA = {
	type: 'object',
	properties: {
		verdicts: {
			type: 'array',
			items: {
				type: 'object',
				properties: {
					id: { type: 'string' },
					verdict: { type: 'string', enum: ['confirmed', 'refuted', 'unverified'] },
					citation: { type: 'string', description: 'file:line that settles it' },
					note: { type: 'string' },
				},
				required: ['id', 'verdict', 'citation', 'note'],
			},
		},
	},
	required: ['verdicts'],
}

const REPORT_SCHEMA = {
	type: 'object',
	properties: {
		reportPath: { type: 'string' },
		written: { type: 'boolean' },
		counts: {
			type: 'object',
			properties: { proposed: { type: 'number' }, refuted: { type: 'number' }, unverified: { type: 'number' } },
			required: ['proposed', 'refuted', 'unverified'],
		},
		topThree: { type: 'array', items: { type: 'string' } },
	},
	required: ['reportPath', 'written', 'counts', 'topThree'],
}

// ---------------------------------------------------------------------------
// Prompts
// ---------------------------------------------------------------------------

function context() {
	return [
		`Focus: ${focus}`,
		`Bound under study: ${scenario === 'both' ? 'CPU-bound and GPU-bound scenes' : `${scenario.toUpperCase()}-bound scenes`}.`,
		paths.length ? `Start from these paths: ${paths.join(', ')}.` : 'No paths given: find the hot code yourself.',
		`Targets:\n${targetText}`,
		'Read docs/r3d/lessons.md first, then docs/r3d/optimization-transfer.md and docs/r3d/xbox-performance-review.md so you do not re-propose what they already corrected. Read docs/r3d/frametime-recorder.md for the measuring instrument.',
		'You are read-only: change nothing in the tree. Do not guess: open every file you cite. Prefer a real map over an empty one when you reason about cost.',
	].join('\n')
}

function lensPrompt(l) {
	return `${context()}\n\nYour lens (${l.key}): ${l.brief}\n\nReturn at most 12 candidates ranked by expected frame-time gain. Do not list an optimisation as a port without grepping the reference for it (lessons.md); anything you assert about what the reference does goes in referenceClaim so a referee can check it. Items already in the tree are not candidates.`
}

function refereePrompt(candidates) {
	const claims = candidates.filter(c => c.referenceClaim)
	return `${context()}\n\nConfirm or refute each claim below about what the reference renderer (ref/gl, ref/soft, engine/client) does, by reading it and citing file:line. You confirm or refute; you do not design. Also mark 'refuted' any candidate whose change is already in the tree, and 'unverified' what only hardware can settle.\n\n${JSON.stringify(claims.map(c => ({ id: c.id, title: c.title, claim: c.referenceClaim, evidence: c.evidence })))}`
}

function plannerPrompt(lensResults, verdicts) {
	return `${context()}\n\nMerge the survey into ONE report and write it to ${reportPath} (create the directory if needed; write nothing else). Lens findings:\n${JSON.stringify(lensResults)}\n\nReferee verdicts (drop 'refuted' into the Refuted section, keep 'unverified' labelled):\n${JSON.stringify(verdicts)}\n\nThe report must contain, in this order:\n1. Summary and the targets assumed.\n2. Ranked improvements: per item - bound, target(s), file:line to change, the change, expected gain and its basis, risk, how it is measured (and whether xemu can settle it), the divergences.md note it needs if it touches upstream code.\n3. CPU-bound plan: measure first (frametime-recorder counters), then ordered steps, exit criterion with a number, what to do if the number is missed.\n4. GPU-bound plan: same shape, split fill / vertex / state-DMA.\n5. Memory and streaming items, if any.\n6. Refuted claims with the refuting line.\n7. Unverified until hardware.\n8. What this research did not check.\nFinish by returning the structured summary. Set written=false if you could not write the file.`
}

// ---------------------------------------------------------------------------
// Dry run and flow
// ---------------------------------------------------------------------------

if (dryRun) {
	const n = LENSES.length + 2
	log(`dry run: models ${JSON.stringify(MODELS)}; ${LENSES.length} lens(es) [${LENSES.map(l => l.key).join(', ')}] + referee + planner = ${n} agents; scenario ${scenario}, target ${target}; report ${reportPath}`)
	return { dryRun: true, agents: n, models: MODELS, lenses: LENSES.map(l => `${l.key} -> ${l.agentType}`), reportPath, scenario, target }
}

phase('Survey')
const lensResults = (await parallel(LENSES.map(l => async () => {
	const r = await agent(lensPrompt(l), { agentType: l.agentType, ...MODELS.lens, schema: LENS_SCHEMA, phase: 'Survey', label: `survey:${l.key}` })
	return r ? { lens: l.key, ...r } : null
}))).filter(Boolean)

if (lensResults.length === 0) {
	log('perf-research: no lens produced a result - stopping, nothing was written.')
	return { ok: false, reason: 'no lens result' }
}
const candidates = lensResults.flatMap(r => r.candidates)
log(`Survey: ${lensResults.length}/${LENSES.length} lenses, ${candidates.length} candidates`)

phase('Referee')
let verdicts = []
if (candidates.some(c => c.referenceClaim)) {
	const ref = await agent(refereePrompt(candidates), { agentType: 'reference-renderer-specialist', ...MODELS.referee, schema: REFEREE_SCHEMA, phase: 'Referee', label: 'referee' })
	verdicts = ref ? ref.verdicts : []
	if (!ref) log('Referee produced no result - the report will mark every reference claim unverified.')
} else {
	log('Referee: no reference claims to check - skipped.')
}

phase('Plan')
const report = await agent(plannerPrompt(lensResults, verdicts), { agentType: 'perf-research-planner', ...MODELS.planner, schema: REPORT_SCHEMA, phase: 'Plan', label: 'plan' })
if (!report || !report.written) log(`Plan: the report was not written (${report ? 'planner said so' : 'no result'}).`)
else log(`Plan: ${report.reportPath} - ${report.counts.proposed} proposed, ${report.counts.refuted} refuted, ${report.counts.unverified} unverified`)

return { ok: !!(report && report.written), reportPath: report ? report.reportPath : null, counts: report ? report.counts : null, topThree: report ? report.topThree : [], lensesRun: lensResults.map(r => r.lens), refereeVerdicts: verdicts.length }
