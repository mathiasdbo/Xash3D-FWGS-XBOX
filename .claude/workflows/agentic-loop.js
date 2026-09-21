// agentic-loop.js - Resonance3D's own research -> implement -> QA -> publish
// loop, pipelined across a bounded set of atomic tasks, with a 3-round QA
// retry cap per task and a log-reading retrospective at the end.
//
// Design doc: docs/r3d/agentic-loop.md - read it before changing this file.
// Short version of why it looks like this:
//
// This project already built and deleted a heavier multi-agent Workflow
// once (tools/codex-review.sh:31-37 documents `.claude/workflows/
// pr-review.js`: a 6-lens/3-refuter fan-out that cost 25-37 Claude subagent
// calls and 2-2.6M tokens PER ROUND, and needed 11 rounds to close one
// branch). The fix that actually shipped was moving that compute OFF
// Claude's own token budget entirely, onto Codex, via
// tools/codex-review.sh. This script's own QA stage reuses that tool
// directly instead of re-implementing review with more Claude subagents -
// see loop-qa.md for why.
//
// Every agent this script calls is a LEAF (.claude/agents/loop-*.md and the
// 8 existing specialists) - none of them carry Agent/Task in their own
// tool list, so none of them can spawn further subagents. Every fan-out
// decision (which specialists get consulted for which topic, how many
// topics run, how many QA retries a task gets) is made HERE, in the open,
// not left to a model deciding to delegate further at runtime. That is the
// direct, deliberate fix for the deleted workflow's own failure mode: the
// total agent count for a run is computable by reading this file, not a
// function of what a model decides to do.
//
// Model/effort choice per stage lives in each loop-*.md's own frontmatter,
// not repeated here - this script only overrides model/effort for the 8
// existing specialists (which don't set one of their own), pinning the two
// "referee" specialists (reference-renderer-specialist,
// upstream-fidelity-specialist) to opus/high and the rest to sonnet/medium.
// See docs/r3d/agentic-loop.md for the full table and the reasoning.
//
// Two things below exist ONLY because a codex-review pass on this very
// script found real bugs in an earlier version - not speculative hardening:
//   1. loop-setup creates one isolated git worktree+branch per topic before
//      anything else runs. Every topic's own Implementation->QA->Publisher
//      chain runs concurrently with every other topic's (pipeline() has no
//      barrier between topics) - without a real, separate checkout per
//      topic, concurrent implementers would share one index/HEAD/working
//      tree and could corrupt or cross-contaminate each other's changes.
//   2. codexReviewRoundsUsed is tracked HERE, per topic, and threaded into
//      every QA call as codexReviewRoundsRemaining - each QA invocation is
//      a fresh, stateless agent with no memory of a real cap this project
//      actually enforces (2 tools/codex-review.sh rounds per PR, AGENTS.md),
//      and this loop can call QA up to 4 times per topic.

export const meta = {
	name: 'agentic-loop',
	description: 'Research -> implement -> QA -> publish per atomic task, pipelined, 3-round QA retry cap per task, log-reading retrospective at the end',
	phases: [
		{ title: 'SetUp', detail: 'bootstrap; break the prompt into atomic topics; flag relevant specialists per topic' },
		{ title: 'Specialists', detail: 'consult the flagged specialists per topic, in parallel - bounded, not open-ended' },
		{ title: 'Research', detail: 'per-topic spec, grounded in the specialist input already gathered' },
		{ title: 'Implementation', detail: 'apply the spec; apply QA fix-it lists on retries' },
		{ title: 'QA', detail: 'real build/link/sweep chain, tests, tools/codex-review.sh - up to 3 automatic rounds per task' },
		{ title: 'Publisher', detail: 'branch, commits, PR per AGENTS.md; re-triggers checks on a retry instead of opening a new PR' },
		{ title: 'Retrospector', detail: 'reads every stage\'s own log for the run, writes one report' },
	],
}

const SPECIALISTS = [
	'asset-pipeline-specialist',
	'build-toolchain-specialist',
	'console-performance-specialist',
	'memory-budget-specialist',
	'nv2a-xbox-specialist',
	'reference-renderer-specialist',
	'render-architecture-specialist',
	'upstream-fidelity-specialist',
]

// The two "referee" specialists exist specifically to catch the class of
// expensive, subtle mistake docs/r3d/lessons.md's own "NEVER list an
// optimisation as a port without grepping the reference for it" entry is
// about (five of nineteen claimed ports were already in the tree; one
// claim ran backwards even after specialist review existed) - worth the
// top model tier. The other six are narrower, checklist-shaped domains.
const REFEREE_SPECIALISTS = new Set(['reference-renderer-specialist', 'upstream-fidelity-specialist'])

function specialistOverride(name) {
	return REFEREE_SPECIALISTS.has(name)
		? { model: 'opus', effort: 'high' }
		: { model: 'sonnet', effort: 'medium' }
}

// Safe stringify for prompts - agent() can return null (skipped mid-run, or
// a terminal API error after retries per the workflow tool's own contract),
// and JSON.stringify(null) is syntactically fine but silently uninformative
// to hand to the next stage as if it were real content.
function describe(value, label) {
	if (value === null || value === undefined) {
		return `(${label} produced no result - it may have failed or been skipped; proceed cautiously and say so in your own log)`
	}
	return JSON.stringify(value)
}

// A function, not a plain const: maxTasks is a runtime value (from args),
// and topics.maxItems needs it. codex review, real finding: an earlier
// version hardcoded no upper bound at all here, so the advertised
// maxTasks cap was only a prompt-text instruction a confused/non-compliant
// setup agent could exceed - schema validation (enforced at the tool-call
// layer, per the workflow tool's own contract) is a REAL mechanical bound,
// not a request. Combined with the JS-side truncation below (defense in
// depth, not a duplicate of the same guarantee).
function setupSchema(maxTasks) {
	return {
		type: 'object',
		properties: {
			gitState: { type: 'string' },
			topics: {
				type: 'array',
				maxItems: maxTasks,
				items: {
					type: 'object',
					properties: {
						slug: { type: 'string', description: 'short, filename-safe slug, e.g. "pvs-culling" - also the branch name' },
						title: { type: 'string' },
						brief: { type: 'string', description: 'enough context for loop-research to start from cold' },
						specialists: { type: 'array', items: { type: 'string' } },
						worktreePath: { type: 'string', description: 'absolute path to this topic\'s own git worktree, already created (with submodules initialized) and checked out on its own branch' },
					},
					required: ['slug', 'title', 'brief', 'specialists', 'worktreePath'],
				},
			},
			droppedTopics: { type: 'array', items: { type: 'string' } },
		},
		required: ['topics'],
	}
}

const SPEC_SCHEMA = {
	type: 'object',
	properties: {
		title: { type: 'string' },
		problem: { type: 'string' },
		change: { type: 'string' },
		outOfScope: { type: 'array', items: { type: 'string' } },
		verification: { type: 'string' },
		citations: { type: 'array', items: { type: 'string' } },
	},
	required: ['title', 'problem', 'change', 'verification'],
}

const CHANGES_SCHEMA = {
	type: 'object',
	properties: {
		commits: { type: 'array', items: { type: 'string' } },
		filesChanged: { type: 'array', items: { type: 'string' } },
		summary: { type: 'string' },
		openQuestions: { type: 'array', items: { type: 'string' } },
	},
	required: ['commits', 'summary'],
}

const QA_SCHEMA = {
	type: 'object',
	properties: {
		green: { type: 'boolean' },
		stagesRun: { type: 'array', items: { type: 'string' } },
		stagesSkipped: { type: 'array', items: { type: 'string' } },
		usedCodexReview: { type: 'boolean', description: 'true only if tools/codex-review.sh was actually invoked this call - the workflow uses this to track the real 2-round-per-PR cap across otherwise-stateless QA calls' },
		codexReviewVerdict: { type: 'string' },
		corrections: { type: 'array', items: { type: 'string' } },
		summary: { type: 'string' },
	},
	required: ['green', 'summary', 'usedCodexReview'],
}

const PUBLISH_SCHEMA = {
	type: 'object',
	properties: {
		branch: { type: 'string' },
		prNumber: { type: 'number' },
		prUrl: { type: 'string' },
		blocked: { type: 'boolean', description: 'true if this was opened as a [QA BLOCKED] draft because the final verdict was red' },
		summary: { type: 'string' },
	},
	required: ['summary', 'blocked'],
}

const RETRO_SCHEMA = {
	type: 'object',
	properties: {
		summary: { type: 'string' },
		reportPath: { type: 'string' },
		candidateLessons: { type: 'array', items: { type: 'string' } },
	},
	required: ['summary', 'reportPath'],
}

// runId cannot be generated in here (Date.now()/Math.random() throw - see
// the workflow tool's own resume contract). Pass a real one via args; the
// calling session already has a real clock. A missing runId still runs,
// just without a unique log directory across repeated invocations on the
// same day - fine for a one-off smoke test, not for real overlapping runs.
const runId = (args && args.runId) ? String(args.runId) : 'unlabeled-run'
const maxTasks = (args && args.maxTasks) || 4
const prompt = (args && args.prompt) ? String(args.prompt) : ''

if (!prompt) {
	throw new Error('agentic-loop: args.prompt is required - the general instruction this run scopes around. Pass args: { prompt, runId, maxTasks? }.')
}

const logDir = `.sweep/agentic-loop/${runId}`

phase('SetUp')
const setup = await agent(
	`You are loop-setup for the agentic-loop workflow, run id "${runId}".\n\n` +
	`General prompt for this run:\n"""\n${prompt}\n"""\n\n` +
	`Max atomic topics allowed: ${maxTasks}.\n` +
	`Write your run header to ${logDir}/00-setup.md as instructed in your own agent prompt.\n` +
	`For EACH topic you decide on, create its own git worktree+branch under ` +
	`${logDir}/worktrees/<topic-slug>/ as instructed in your own agent prompt, one at a time, and return its ` +
	`absolute path as that topic's "worktreePath" - every later stage for that topic works there, never in ` +
	`the main checkout, since topics run concurrently.\n` +
	`Return the topic list. Each topic's "specialists" array must only contain names from this exact list: ${SPECIALISTS.join(', ')}.`,
	{ agentType: 'loop-setup', phase: 'SetUp', schema: setupSchema(maxTasks) }
)

if (!setup || !Array.isArray(setup.topics) || setup.topics.length === 0) {
	log('agentic-loop: SetUp produced no usable topics - stopping before Research. Check .sweep/agentic-loop/' + runId + '/00-setup.md for why.')
	throw new Error('agentic-loop: no topics to work from')
}

// Defense in depth alongside the schema's own maxItems above (codex
// review, real finding): validation happens at the tool-call layer, but
// this is a hard, computable line of last resort against ever running
// more than maxTasks topics through the real fan-out below, regardless of
// how that bound was supposed to be enforced upstream.
if (setup.topics.length > maxTasks) {
	log(`agentic-loop: SetUp returned ${setup.topics.length} topics, over the ${maxTasks} cap - truncating to the first ${maxTasks}, dropping: ${setup.topics.slice(maxTasks).map(t => t.slug).join(', ')}`)
	setup.topics = setup.topics.slice(0, maxTasks)
}

log(`SetUp: ${setup.topics.length} topic(s) - ${setup.topics.map(t => t.slug).join(', ')}`)
if (Array.isArray(setup.droppedTopics) && setup.droppedTopics.length) {
	log(`SetUp dropped ${setup.droppedTopics.length} topic(s) over the ${maxTasks} cap: ${setup.droppedTopics.join('; ')}`)
}

phase('Specialists')
const specialistResults = await parallel(
	setup.topics.map(topic => async () => {
		const names = Array.isArray(topic.specialists)
			? topic.specialists.filter(n => SPECIALISTS.includes(n))
			: []
		if (names.length === 0) return { slug: topic.slug, findings: [] }

		const findings = await parallel(
			names.map(name => async () => {
				const override = specialistOverride(name)
				const text = await agent(
					`Topic: ${topic.title}\n\n${topic.brief}\n\n` +
					`Read from this exact checkout, not wherever your own current working directory ` +
					`happens to be: ${topic.worktreePath} - this topic's own dedicated git worktree, already ` +
					`checked out on a fresh origin/main. (codex review, real finding: reading from a different ` +
					`checkout than the one Research/Implementation will actually work in risks citing code that ` +
					`isn't there, or missing code that is.)\n\n` +
					`Answer only what's in your own domain for this topic, per your own agent description - ` +
					`cite file:line for anything you assert. This feeds directly into a research spec, not a ` +
					`human-facing report, so be concise and concrete.`,
					{ agentType: name, phase: 'Specialists', label: `${name} · ${topic.slug}`, ...override }
				)
				return { name, text }
			})
		)
		return { slug: topic.slug, findings: findings.filter(Boolean) }
	})
)

const findingsBySlug = Object.fromEntries(
	specialistResults.filter(Boolean).map(r => [r.slug, r.findings])
)

async function researchImplementQaPublish(topic) {
	const topicLog = `${logDir}/${topic.slug}.md`
	const worktreeNote = `Your own dedicated git worktree for this topic (already created, submodules ` +
		`initialized, and checked out on its own branch "${topic.slug}"): ${topic.worktreePath} - do ALL your ` +
		`work there, never in the main checkout (other topics are running concurrently in their own separate ` +
		`worktrees).`
	const specialistInput = findingsBySlug[topic.slug] || []
	const specialistText = specialistInput.length
		? specialistInput.map(f => `### ${f.name}\n${f.text}`).join('\n\n')
		: '(no specialists were flagged as relevant to this topic by loop-setup)'

	// The project's own real cap is 2 tools/codex-review.sh rounds per PR
	// (AGENTS.md). Each QA call is a fresh, stateless agent, so the
	// remaining budget has to be tracked and threaded through HERE, not
	// left to QA's own memory of earlier rounds it doesn't have (codex
	// review, real finding - see docs/r3d/agentic-loop.md).
	let codexReviewRoundsUsed = 0

	let spec = await agent(
		`Topic: ${topic.title}\n\n${topic.brief}\n\n${worktreeNote}\n\n` +
		`Specialist input already gathered for this topic:\n\n${specialistText}\n\n` +
		`Append your log to ${topicLog} as instructed in your own agent prompt, then return your spec.`,
		{ agentType: 'loop-research', phase: 'Research', label: `research · ${topic.slug}`, schema: SPEC_SCHEMA }
	)

	// codex review, real finding: agent() can return null (skipped, or a
	// terminal API error after retries). A null spec here means nothing
	// downstream has anything real to work from - QA could otherwise run
	// its checks against an untouched worktree, find nothing wrong on an
	// empty diff, and hand back a false GREEN for a task that was never
	// actually researched. There is also nothing to publish - no commits
	// exist yet - so this skips Publisher entirely rather than opening an
	// empty PR.
	if (!spec) {
		log(`"${topic.title}": Research produced no result - stopping this task without Implementation/QA/Publisher.`)
		return { topic, spec: null, changes: null, verdict: null, publish: null, failed: true, failureReason: 'Research produced no result' }
	}

	const firstChanges = await agent(
		`Spec for "${topic.title}":\n${describe(spec, 'loop-research')}\n\n${worktreeNote}\n\n` +
		`Append to ${topicLog}. Implement this spec.`,
		{ agentType: 'loop-implementer', phase: 'Implementation', label: `implement · ${topic.slug}`, schema: CHANGES_SCHEMA }
	)

	// Same reasoning as the spec check above: a null first-implementation
	// result means no commits exist for this topic at all - nothing for
	// QA to meaningfully judge and nothing for Publisher to open a PR
	// around, so this stops here too rather than letting QA "pass" an
	// unimplemented spec or Publisher open an empty PR.
	if (!firstChanges) {
		log(`"${topic.title}": Implementation produced no result on its first attempt - stopping this task without QA/Publisher.`)
		return { topic, spec, changes: null, verdict: null, publish: null, failed: true, failureReason: 'Implementation produced no result' }
	}
	let changes = firstChanges

	let verdict = null
	for (let round = 1; round <= 3; round++) {
		const remaining = Math.max(0, 2 - codexReviewRoundsUsed)
		verdict = await agent(
			`Spec:\n${describe(spec, 'loop-research')}\n\n` +
			`Changes made (round ${round}):\n${describe(changes, 'loop-implementer')}\n\n${worktreeNote}\n\n` +
			`codexReviewRoundsRemaining for this PR: ${remaining}.\n\n` +
			`Append to ${topicLog}. Judge per your own agent prompt.`,
			{ agentType: 'loop-qa', phase: 'QA', label: `qa · ${topic.slug} · round ${round}`, schema: QA_SCHEMA }
		)
		if (verdict && verdict.usedCodexReview) codexReviewRoundsUsed++
		if (verdict && verdict.green) break

		if (round === 3) {
			log(`"${topic.title}": still red after 3 QA rounds - escalating to Research for a spec revision.`)
			const revisedSpec = await agent(
				`Topic: ${topic.title}\n\nOriginal spec:\n${describe(spec, 'loop-research')}\n\n` +
				`QA's red verdict after 3 rounds:\n${describe(verdict, 'loop-qa')}\n\n${worktreeNote}\n\n` +
				`This is a revision call, not a first pass - see your own agent prompt's guidance for it. ` +
				`Append to ${topicLog}.`,
				{ agentType: 'loop-research', phase: 'Research', label: `revise · ${topic.slug}`, schema: SPEC_SCHEMA }
			)
			// A failed revision doesn't erase 3 real rounds of prior work -
			// keep the existing spec/changes and let the final QA call
			// judge what's actually there, rather than discarding it.
			if (revisedSpec) spec = revisedSpec
			else log(`"${topic.title}": spec revision produced no result - re-judging the existing implementation as-is.`)

			const revisedChanges = await agent(
				`Revised spec for "${topic.title}":\n${describe(spec, 'loop-research')}\n\n${worktreeNote}\n\n` +
				`Append to ${topicLog}. Implement this spec.`,
				{ agentType: 'loop-implementer', phase: 'Implementation', label: `implement · ${topic.slug} · revised`, schema: CHANGES_SCHEMA }
			)
			if (revisedChanges) changes = revisedChanges
			else log(`"${topic.title}": post-revision implementation produced no result - re-judging the existing implementation as-is.`)

			const revisedRemaining = Math.max(0, 2 - codexReviewRoundsUsed)
			verdict = await agent(
				`Spec:\n${describe(spec, 'loop-research')}\n\n` +
				`Changes made (post-revision):\n${describe(changes, 'loop-implementer')}\n\n${worktreeNote}\n\n` +
				`codexReviewRoundsRemaining for this PR: ${revisedRemaining}.\n\n` +
				`Append to ${topicLog}. Judge per your own agent prompt. This is the LAST automatic round for ` +
				`this task regardless of outcome - do not expect another retry.`,
				{ agentType: 'loop-qa', phase: 'QA', label: `qa · ${topic.slug} · post-revision`, schema: QA_SCHEMA }
			)
			if (verdict && verdict.usedCodexReview) codexReviewRoundsUsed++
			break
		}

		const fixedChanges = await agent(
			`Spec:\n${describe(spec, 'loop-research')}\n\n` +
			`Previous changes:\n${describe(changes, 'loop-implementer')}\n\n` +
			`QA's correction list (round ${round}):\n${describe(verdict, 'loop-qa')}\n\n${worktreeNote}\n\n` +
			`Append to ${topicLog}. Apply the fixes.`,
			{ agentType: 'loop-implementer', phase: 'Implementation', label: `fix · ${topic.slug} · round ${round}`, schema: CHANGES_SCHEMA }
		)
		// Keep the last known-good (non-null) changes on a failed retry,
		// rather than losing track of real prior work - the next QA round
		// then simply re-judges the same code and (correctly) stays red,
		// converging toward the retry cap and a blocked publish instead of
		// silently forgetting what was already implemented.
		if (fixedChanges) changes = fixedChanges
		else log(`"${topic.title}": fix attempt in round ${round} produced no result - re-judging the previous implementation as-is.`)
	}

	// verdict.green is the ONLY thing that decides the shape of the PR
	// loop-publisher opens (normal vs. [QA BLOCKED] draft) - stated
	// explicitly here so a null/red verdict can never be silently treated
	// as "good enough to publish normally" (codex review, real finding).
	const finalStatus = (verdict && verdict.green) ? 'GREEN' : 'RED/BLOCKED'
	const publish = await agent(
		`Topic: ${topic.title}\n\nSpec:\n${describe(spec, 'loop-research')}\n\n` +
		`Final changes:\n${describe(changes, 'loop-implementer')}\n\nQA's FINAL verdict (status: ${finalStatus}):\n` +
		`${describe(verdict, 'loop-qa')}\n\n${worktreeNote}\n\n` +
		`Append to ${topicLog}. Publish per your own agent prompt - follow the GREEN or RED/BLOCKED path ` +
		`that matches the status above exactly.`,
		{ agentType: 'loop-publisher', phase: 'Publisher', label: `publish · ${topic.slug}`, schema: PUBLISH_SCHEMA }
	)

	return { topic, spec, changes, verdict, publish }
}

// pipeline(), not parallel(): each topic runs its own independent
// Research->Implementation->QA-loop->Publisher chain with no barrier
// between topics - topic 2's research can finish while topic 1 is still
// mid-QA-retry. This is the "asynchronous" behaviour between research and
// implementation the original design called for, generalised across every
// stage, not just those two.
const results = await pipeline(setup.topics, researchImplementQaPublish)

phase('Retrospector')
const retro = await agent(
	`Run id "${runId}". Read every file under ${logDir}/ and write your report to ${logDir}/retrospective.md ` +
	`as instructed in your own agent prompt.`,
	{ agentType: 'loop-retrospector', phase: 'Retrospector', schema: RETRO_SCHEMA }
)

return { runId, setup, results, retro }
