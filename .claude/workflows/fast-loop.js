// fast-loop.js - Resonance3D's reduced loop: build -> verify (+ codex, in
// parallel) -> fix -> publish, per topic. Design record:
// docs/r3d/fast-loop.md. The fuller v2 loop (.claude/workflows/
// agentic-loop.js) stays until this one has a measured run behind it.
//
// What makes it small, and why each cut is safe:
//
//  1. ONE THINKING AGENT PER ROUND. Research and implementation are one
//     call (loop-implementer, Sonnet by default): v2 paid a research agent
//     to write a spec the implementer then re-read, and specialists the
//     design note already covered (run 1's own assessment).
//  2. EVERY OTHER STAGE RUNS ONE GENERATED SCRIPT AND RETURNS FILES
//     VERBATIM (Haiku). tools/fast-gates.sh computes the gate plan from the
//     branch's own diff and runs it through tools/gate-chain.sh;
//     tools/live-look.sh boots xemu and takes the frame; the publish script
//     re-checks the attestation on disk before it pushes. No stage
//     narrates a procedure; no plan text is pasted into prompts.
//  3. THE VERDICT IS COMPUTED HERE from exit codes, attestation digests and
//     tool lines (judgeVerify/judgeLive/finalStatus below) - never reported
//     by a model (lessons.md: "NEVER let the agent that ran a check decide
//     whether it passed").
//  4. LARGE BUILDS ARE PAID ONCE PER origin/main. Setup starts the
//     baseline build of origin/main in the background while Build works;
//     with .sweep/cc-cache-dir (tools/xbox-cc-cache.sh) every unit a topic
//     did not change is a copy of that build's object in the topic's own
//     full link. The baseline XBE and its frame of each scene
//     (tools/live-look.sh) are reused by every topic and every later run on
//     the same origin/main.
//  5. XEMU BOOTS ONCE PER LOOK, and only when a renderer path changed, the
//     gates are green and the XBE differs from the last one looked at.
//  6. CODEX RUNS ALONGSIDE THE FULL BUILD, not after it: the cheap tier
//     (changed-file compile, ref isolate) has already caught what used to
//     waste a codex round (run 1), and a review takes minutes of wall
//     clock the build no longer waits on. Two rounds at most (AGENTS.md).
//
// Agent count, computable from this file (args.dryRun prints it): setup 1,
// baseline 0-1, then per topic build 1-4, verify 1-3, codex 0-4 calls (at
// most 2 counted rounds; an uncounted run and one codex-only retry are
// calls too), publish 1.
// Typical green topic: 5-6 in all. Every agent is a leaf.

export const meta = {
	name: 'fast-loop',
	description: 'Reduced loop: research+implement (self-gated) -> full gates + live xemu look with codex alongside -> fix -> publish; verdict computed from exit codes',
	phases: [
		{ title: 'Setup', detail: 'topics, worktrees, build environment proof; origin/main baseline build in the background' },
		{ title: 'Build', detail: 'research and implement in one agent, self-gated with tools/fast-gates.sh cheap' },
		{ title: 'Verify', detail: 'tools/fast-gates.sh full and tools/live-look.sh; codex-review alongside' },
		{ title: 'Publish', detail: 'attestation re-checked on disk, push, PR with the run record' },
	],
}

// ---------------------------------------------------------------------------
// Arguments
// ---------------------------------------------------------------------------

const A = args || {}
const runId = A.runId ? String(A.runId) : null
const prompt = A.prompt ? String(A.prompt) : ''
const maxTasks = Math.min(Math.max(Number(A.maxTasks) || 1, 1), 3)
const baselines = A.baselines || {}
const live = A.live && A.live.xemuExe && A.live.hlSteamDir && A.live.map && A.live.mpakPath ? A.live : null
const models = { build: 'sonnet', setup: 'haiku', runner: 'haiku', ...(A.models || {}) }
const dryRun = !!A.dryRun

const CODEX_ROUNDS = 2   // AGENTS.md's cap per PR
const MAX_VERIFY = 3     // full-tier runs per topic
const MAX_FIXES = 2      // fix rounds after verify (plus one cheap-tier retry)
const LIVE_FRAMES = 60   // frames past map-ready before the frame counts
const XBE = '.sweep/fast-loop/default.xbe'   // in the topic worktree

// ---------------------------------------------------------------------------
// Pure functions (tools/test_fast_loop.mjs drives them and the whole flow)
// ---------------------------------------------------------------------------

// Single-quote for bash.
function q(s) { return `'${String(s).replace(/'/g, `'\\''`)}'` }

function parseKv(text) {
	const o = {}
	for (const line of String(text || '').split(/\r?\n/)) {
		const m = line.match(/^([A-Za-z0-9_-]+)=(.*)$/)
		if (m && !(m[1] in o)) o[m[1]] = m[2]
	}
	return o
}

function parseRows(tsv) {
	return String(tsv || '').split(/\r?\n/).filter(l => l.trim()).map(l => {
		const [gate, status, exit, seconds, log] = l.split('\t')
		return { gate, status, exit: exit === '-' ? null : Number(exit), seconds: Number(seconds) || 0, log: log || null }
	})
}

function planNames(planTsv) {
	return String(planTsv || '').split(/\r?\n/).filter(l => l.trim() && !l.startsWith('#')).map(l => l.split('\t')[0])
}

// A gate table is green iff every planned gate ran with exit 0 and a log,
// the runner attested exactly that plan, and the plan needed nothing it was
// not given and left no code ungated. An empty plan (docs only) is green on
// its own: fast-gates.sh exits 0 and writes no attestation for it.
function judgeGates(r) {
	const why = []
	if (!r) return { green: false, why: ['no result'] }
	const meta = parseKv(r.meta)
	const names = planNames(r.plan)
	const rows = parseRows(r.results)
	const byName = Object.fromEntries(rows.map(x => [x.gate, x]))
	if (r.gateExit !== 0) why.push(`fast-gates exited ${r.gateExit}`)
	for (const n of names) {
		const x = byName[n]
		if (!(x && x.status === 'ran' && x.exit === 0 && x.log)) why.push(`${n}: ${x ? `${x.status} exit ${x.exit}` : 'no row'}`)
	}
	if (names.length) {
		const a = String(r.attestation || '')
		const listed = ((a.match(/^gates=(.*)$/m) || [])[1] || '').split(',').map(s => s.trim())
		if (!/^GATE-CHAIN-OK$/m.test(a) || !/^plan-sha256=[0-9a-f]{64}$/m.test(a) || !/^results-sha256=[0-9a-f]{64}$/m.test(a) || !names.every(n => listed.includes(n))) why.push('attestation missing or not naming every planned gate')
	}
	if (meta.missing) why.push(`unconfigured: ${meta.missing}`)
	if (meta.not_gated) why.push(`not gated: ${meta.not_gated}`)
	if (!('needs_live' in meta)) why.push('meta.txt missing')
	return { green: why.length === 0, why, meta, rows, names }
}

// The look is judged from the tool's own lines: LIVE-LOOK-OK, the XBE it
// booted is the XBE the gates built, the scene line, the diff numbers
// against the spec's pixel expectation, and whatever the runner saw on the
// PNG that contradicts the expectation.
function judgeLive(result, xbeSha, expect) {
	const kv = parseKv(result && result.liveResult)
	const defects = []
	if (!result || !result.liveResult) return { ok: false, defects: ['no look was taken'], kv }
	if (!/^LIVE-LOOK-OK$/m.test(result.liveResult)) defects.push(`no look: ${kv.verdict || kv.error || 'LIVE-LOOK-OK missing'}`)
	if (!/^[0-9a-f]{64}$/.test(xbeSha || '') || kv.xbe_sha256 !== xbeSha) defects.push(`booted XBE ${kv.xbe_sha256 || '?'} is not the gated XBE ${xbeSha || '?'}`)
	if (!/^scene ready:/.test(kv.branch_scene || '')) defects.push(`scene: ${kv.branch_scene || 'no line'}`)
	const d = (kv.ppmdiff || '').match(/^ppmdiff: (\d+) of (\d+) pixels differ/)
	if (!d) defects.push('no ppmdiff line')
	else if (expect && expect.expectsPixelChange === false && Number(d[1]) > 0) defects.push(`${d[1]} of ${d[2]} pixels differ where the change should be invisible`)
	else if (expect && expect.expectsPixelChange === true && Number(d[1]) === 0) defects.push('frame is pixel-identical to origin/main where the change should show')
	for (const x of (result.defects || [])) defects.push(`seen: ${x}`)
	return { ok: defects.length === 0, defects, kv, pixels: d ? { differing: Number(d[1]), total: Number(d[2]) } : null }
}

// AGENTS.md: critical/high, or confidence >= 0.6, is fix-or-justify.
function actionable(findings) {
	return (findings || []).filter(f => f && (['critical', 'high'].includes(String(f.severity || '').toLowerCase()) || Number(f.confidence) >= 0.6))
}

// A codex round counts only with a thread id and a real verdict: exit 0,
// or exit 1 with findings. Anything else (2 inconclusive, no thread) was
// not a review.
function codexCounted(c) {
	return !!(c && c.threadId && (c.exit === 0 || (c.exit === 1 && (c.findings || []).length > 0)))
}

function findingKey(f) { return `${f.file || '?'}:${f.line || '?'}:${String(f.title || '').slice(0, 60)}` }

function finalStatus(s) {
	const why = []
	if (!s.gatesGreen) why.push('gates red')
	if (s.needsLive && !s.liveOk) why.push(s.liveConfigured ? 'no verified look at the final XBE' : 'renderer change never booted (args.live not configured)')
	if (s.codexRounds < 1) why.push('no counted codex round')
	if (s.unaddressed > 0) why.push(`${s.unaddressed} actionable finding(s) neither fixed nor justified`)
	return { status: why.length ? 'RED' : 'GREEN', why }
}

function gatesArgs() {
	const b = baselines
	return [
		typeof b.engineSweepExpectFail === 'number' ? `--engine-expect-fail ${b.engineSweepExpectFail}` : '',
		typeof b.mainuiExpectFail === 'number' ? `--mainui-expect-fail ${b.mainuiExpectFail}` : '',
		typeof b.mainuiExpectTotal === 'number' ? `--mainui-expect-total ${b.mainuiExpectTotal}` : '',
	].filter(Boolean).join(' ')
}

// ---- Generated scripts. Each is run verbatim by a runner agent.

function setupScript(logRel) {
	return `set -u
MAIN="$(git rev-parse --show-toplevel)" && cd "$MAIN" || exit 2
git fetch -q origin main || { echo "setup: fetch failed"; exit 2; }
BASE="$(git rev-parse origin/main)"
LOG="$MAIN/${logRel}"; mkdir -p "$LOG/wt" || exit 2
NXDK="$MAIN/deps/nxdk"; CC="$MAIN/.sweep/cc-cache"
[ -f "$NXDK/lib/libpdclib.lib" ] || { echo "setup: $NXDK has no built libs - build nxdk in the main checkout first"; exit 2; }
mk() { # <dir> <branch, or - for detached origin/main>
	local d="$1" b="$2" p
	if [ "$b" = - ]; then git worktree add -q --detach "$d" "$BASE" || return 1
	else
		if git show-ref -q --verify "refs/heads/$b" || git ls-remote --exit-code -q origin "refs/heads/$b" > /dev/null; then b="$b-"${q(runId)}; fi
		git worktree add -q -b "$b" "$d" "$BASE" || return 1
	fi
	# Submodules borrow the main checkout's objects: no network, seconds.
	for p in $(git config --file "$d/.gitmodules" --get-regexp '\\.path$' | awk '{print $2}'); do
		[ "$p" = deps/nxdk ] && continue
		git -C "$d" submodule update --init -q --reference "$MAIN/$p" -- "$p" 2>/dev/null || git -C "$d" submodule update --init -q -- "$p" || return 1
	done
	mkdir -p "$d/.sweep" && printf '%s\\n' "$NXDK" > "$d/.sweep/nxdk-dir" && printf '%s\\n' "$CC" > "$d/.sweep/cc-cache-dir" || return 1
	printf 'worktree=%s\tbranch=%s\n' "$d" "$b"
}
for s in "$@"; do mk "$LOG/wt/$s" "$s" || { echo "setup: worktree $s failed"; exit 1; }; done
[ -d "$LOG/wt/_baseline" ] || mk "$LOG/wt/_baseline" - || { echo "setup: baseline worktree failed"; exit 1; }
cd "$LOG/wt/_baseline" || exit 2
tools/xbox-cc.sh public/crtlib.c -o .sweep/env-proof.obj -v > .sweep/env-proof.log 2>&1 || { echo "setup: env-proof compile failed"; tail -20 .sweep/env-proof.log; exit 1; }
grep -q pdclib/include .sweep/env-proof.log || { echo "setup: compile did not reach nxdk's pdclib"; exit 1; }
B="$MAIN/.sweep/fast-loop/baseline/$BASE"
echo "main=$MAIN"; echo "base=$BASE"; echo "log=$LOG"; echo "baseline_dir=$B"
if [ -f "$B/default.xbe.identity" ]; then
	echo "baseline=cached"; cd "$MAIN" && git worktree remove --force "$LOG/wt/_baseline"
else echo "baseline=missing"; fi
echo "SETUP-OK"`
}

function baselineScript(ctx) {
	return `set -u
WB=${q(ctx.log + '/wt/_baseline')}; B=${q(ctx.baselineDir)}
cd "$WB" || exit 2; mkdir -p "$B" || exit 2
if [ ! -f "$B/default.xbe.identity" ]; then
	SCRATCH="$B" tools/xbox-sweep.sh engine filesystem public common compat 3rdparty/library_suffix/src > "$B/sweep.log" 2>&1
	SCRATCH="$B" tools/xbox-game-link-check.sh hl --expect-undefined 0 --include-client --include-freevgui --xbe "$B/default.xbe" > "$B/link.log" 2>&1
	echo "link_exit=$?"
fi
test -f "$B/default.xbe.identity" && test -f "$B/game-hl/xbe-build.exe" && test -f "$B/game-hl/xbe-build.map"; echo "baseline_exit=$?"
tail -3 "$B/link.log" 2>/dev/null
cd ${q(ctx.main)} && git worktree remove --force "$WB" && echo "baseline worktree removed"
echo "BASELINE-DONE"`
}

function verifyScript(ctx, t, round, lastLookSha) {
	const out = `${ctx.log}/${t.slug}/verify-${round}`
	const L = live
	const look = L ? `
if [ "$GE" -eq 0 ] && grep -q '^needs_live=1$' "$OUT/meta.txt" && [ -n "$XS" ] && [ "$XS" != ${q(lastLookSha || 'none')} ]; then
	bash tools/live-look.sh --xbe ${q(XBE)} --exe .sweep/game-hl/xbe-build.exe --linkmap .sweep/game-hl/xbe-build.map \\
		--baseline ${q(ctx.baselineDir)} --steam ${q(L.hlSteamDir)} --xemu ${q(L.xemuExe)} --map ${q(L.map)} --mpak ${q(L.mpakPath)} \\
		--out "$OUT/live" --frames ${LIVE_FRAMES}${L.gamedir ? ` --gamedir ${q(L.gamedir)}` : ''}${L.timeout ? ` --timeout ${Number(L.timeout)}` : ''}${L.resourceDir ? ` --resource-dir ${q(L.resourceDir)}` : ''}${L.uiLanguage ? ` --ui-language ${q(L.uiLanguage)}` : ''}${(t.cvars || []).map(c => ` --cvar ${q(c)}`).join('')} > "$OUT/live.log" 2>&1
	echo "live_exit=$?" >> "$OUT/verify.txt"
fi` : ''
	return `set -u
cd ${q(t.worktree)} || exit 2
OUT=${q(out)}; mkdir -p "$OUT" || exit 2
bash tools/fast-gates.sh full --base ${q(ctx.base)} --out "$OUT" --xbe ${q(XBE)} ${gatesArgs()} > "$OUT/fast-gates.log" 2>&1; GE=$?
XS="$( [ -f ${q(XBE)} ] && sha256sum ${q(XBE)} | cut -c1-64 )"
{ echo "gate_exit=$GE"; echo "xbe_sha256=$XS"; } > "$OUT/verify.txt"
if [ "$GE" -ne 0 ]; then
	F="$(awk -F'\\t' '$2=="ran" && $3!="0" {print $5; exit}' "$OUT/results.tsv" 2>/dev/null)"
	{ echo "--- first failing gate: $F"; tail -n 60 "$F" 2>/dev/null || tail -n 30 "$OUT/fast-gates.log"; } > "$OUT/failure.txt"
fi${look}
echo "VERIFY-DONE $OUT"`
}

function codexScript(ctx, t, round) {
	const out = `${ctx.log}/${t.slug}/codex-${round}.txt`
	return `set -u
cd ${q(t.worktree)} || exit 2
bash tools/codex-review.sh --base ${q(ctx.base)} > ${q(out)} 2>&1; echo "codex_exit=$?" >> ${q(out)}
echo "CODEX-DONE ${out}"`
}

// The publish script re-checks, on disk, everything the verdict rested
// on; a failed check turns a GREEN publish into a [QA BLOCKED] draft
// before anything is pushed. Draft-ness is set by the script and read
// back, never left to a model.
function publishScript(ctx, t, s, v) {
	const dir = v ? `${ctx.log}/${t.slug}/verify-${v.round}` : ''
	const green = s.status === 'GREEN'
	// An empty plan (docs only) has no attestation: HEAD is checked against
	// meta.txt instead, and the re-plan must come out empty too.
	const checks = green ? `
C=0
bad() { echo "CHECK FAILED: $1"; C=1; }
D=${q(dir)}; A="$D/attestation.txt"
val() { sed -n "s/^$1=//p" "$2" | head -n 1; }
sum() { sha256sum "$1" 2>/dev/null | cut -c1-64; }
# Dirty submodule working trees are xbox-cc.sh's own patches, not a change.
[ -z "$(git status --porcelain --untracked-files=all --ignore-submodules=dirty)" ] || bad "tree clean"
if [ -s "$D/plan.tsv" ]; then
	grep -qx GATE-CHAIN-OK "$A" || bad "attestation present"
	[ "$(git rev-parse HEAD)" = "$(val head "$A")" ] || bad "HEAD is the attested commit"
	[ "$(sum "$D/plan.tsv")" = "$(val plan-sha256 "$A")" ] || bad "plan digest"
	[ "$(sum "$D/results.tsv")" = "$(val results-sha256 "$A")" ] || bad "results digest"
else
	[ "$(git rev-parse HEAD)" = "$(val head "$D/meta.txt")" ] || bad "HEAD is the commit the empty plan was computed on"
fi
bash tools/fast-gates.sh plan --tier full --base ${q(ctx.base)} --out "$D/replan" --xbe ${q(XBE)} ${gatesArgs()} > /dev/null || bad "re-plan ran"
cmp -s "$D/plan.tsv" "$D/replan/plan.tsv" || bad "plan is the one this diff computes"
${s.xbeSha ? `[ "$(sum ${q(XBE)})" = ${q(s.xbeSha)} ] || bad "XBE is the gated one"
grep -qx ${q(`sha256_xbe=${s.xbeSha}`)} ${q(`${XBE}.identity`)} || bad "XBE identity sidecar"` : ''}
${s.livePng ? `[ "$(sum ${q(s.livePng)})" = ${q(s.livePngSha || 'none')} ] || bad "live frame is the one judged"` : ''}
# What the verdict consumed as a runner transcribed it, re-read from the
# files the tools wrote (found in review: a transcribed needs_live=0, or a
# hallucinated codex exit, could otherwise carry a GREEN).
RP="$D/replan/meta.txt"
[ "$(val needs_live "$RP")" = ${q(s.needsLive ? '1' : '0')} ] || bad "needs_live is what the verdict used"
[ -z "$(val missing "$RP")" ] || bad "no baseline missing"
[ -z "$(val not_gated "$RP")" ] || bad "no ungated code"
${s.needsLive ? (s.liveDir ? `LR=${q(`${s.liveDir}/result.txt`)}
grep -qx LIVE-LOOK-OK "$LR" || bad "the look's result.txt says LIVE-LOOK-OK"
grep -qx ${q(`xbe_sha256=${s.xbeSha}`)} "$LR" || bad "the look booted the gated XBE"
grep -q '^branch_scene=scene ready:' "$LR" || bad "the look reached the scene"
[ "$(val ppmdiff "$LR" | tr -d '\\r')" = ${q(String(s.livePpmdiff || '').replace(/\r/g, ''))} ] || bad "the pixel diff is the one judged"` : 'bad "a renderer change with no look directory"') : ''}
${(s.codexFiles || []).length ? s.codexFiles.map(c => `grep -qx ${q(`codex_exit=${c.exit}`)} ${q(c.file)} || bad "codex exit as counted"
grep -qF ${q(`codex thread: ${c.threadId}`)} ${q(c.file)} || bad "codex thread as counted"`).join('\n') : 'bad "no counted codex round on disk"'}
echo "checks_failed=$C"
[ "$C" -eq 0 ] || { STATUS=RED; printf '> **Publish-time check failed** - the verdict below was not earned on disk (checks in the run log). Draft.\\n\\n' | cat - "$BODY" > "$BODY.new" && mv "$BODY.new" "$BODY"; }` : ''
	return `set -u
cd ${q(t.worktree)} || exit 2
STATUS=${s.status}
BODY=${q(`${ctx.log}/${t.slug}/pr-body.md`)}
[ -s "$BODY" ] || { echo "publish: $BODY missing"; exit 2; }${checks}
TITLE=${q(t.prTitle)}
[ "$STATUS" = GREEN ] || TITLE="[QA BLOCKED] $TITLE"
git push -q -u origin HEAD || { echo "publish: push failed"; exit 1; }
DRAFT=; [ "$STATUS" = GREEN ] || DRAFT=--draft
# A failure here (a PR already open for the branch, say) must not fall
# through to reading back some other PR's draft state.
gh pr create $DRAFT --title "$TITLE" --body-file "$BODY" --base main || { echo "publish: gh pr create failed"; exit 1; }
[ "$STATUS" = GREEN ] || gh pr ready --undo > /dev/null 2>&1
echo "status=$STATUS"
gh pr view --json url,number,isDraft --jq '"url=" + .url, "number=" + (.number|tostring), "is_draft=" + (.isDraft|tostring)'
echo "PUBLISH-DONE"`
}

function md(v) { return v === null || v === undefined ? '-' : String(v).replace(/\|/g, '\\|') }

function renderRecord(r) {
	const L = []
	L.push(`## Fast-loop record - ${md(r.runId)} / ${md(r.slug)}`, '')
	L.push(`**Status: ${r.status}**${r.why.length ? ` - ${r.why.join('; ')}` : ''}`, '')
	L.push(r.summary ? r.summary : '(no summary)', '')
	L.push('| Stage | Label | Output tokens |', '|---|---|---|')
	for (const m of r.metrics) L.push(`| ${m.stage} | ${md(m.label)} | ${m.outputTokens}${m.ok ? '' : ' (no result)'} |`)
	L.push('', `Agents: ${r.metrics.length}. Output tokens: ${r.metrics.reduce((a, m) => a + m.outputTokens, 0)} (with more than one topic in flight, a delta includes concurrent agents).`, '')
	L.push('### Gates (last verify)', '')
	if (r.gateRows && r.gateRows.length) {
		L.push('| Gate | Status | Exit | Seconds |', '|---|---|---|---|')
		for (const g of r.gateRows) L.push(`| ${md(g.gate)} | ${md(g.status)} | ${md(g.exit)} | ${md(g.seconds)} |`)
		L.push('', `Gate seconds: ${r.gateRows.reduce((a, g) => a + g.seconds, 0)}.`)
	} else L.push('No gates ran (empty plan or no verify result).')
	if (r.gateWhy && r.gateWhy.length) L.push('', `Not green because: ${r.gateWhy.join('; ')}`)
	L.push('', '### Live', '', r.live || 'not needed')
	L.push('', '### Review', '', `Codex rounds counted: ${r.codexRounds}${r.reviewPending ? ' (a second round was due after the fixes and could not be obtained)' : ''}. Fixed: ${r.fixed.length}. Justified: ${r.justified.length}. Optional (below threshold): ${r.optional.length}.`)
	for (const j of r.justified) L.push(`- Not fixed: ${md(j.id)} - ${md(j.reason)}`)
	if (r.unaddressed.length) L.push('', `**Unaddressed actionable findings:** ${r.unaddressed.map(md).join('; ')}`)
	L.push('', '### Owner-side on hardware', '', '- NV2A register acceptance (lessons.md: xemu accepts values hardware rejects)', '- DMA timing and unified-memory behaviour (AGENTS.md, Verification)')
	for (const x of (r.notVerifiable || [])) L.push(`- ${md(x)}`)
	return L.join('\n')
}

// ---------------------------------------------------------------------------
// Schemas
// ---------------------------------------------------------------------------

const SETUP_SCHEMA = {
	type: 'object',
	properties: {
		topics: { type: 'array', maxItems: maxTasks, items: { type: 'object', properties: {
			slug: { type: 'string' }, title: { type: 'string' }, goal: { type: 'string' },
			designNotes: { type: 'array', items: { type: 'string' } },
		}, required: ['slug', 'title', 'goal'] } },
		scriptOutput: { type: 'string', description: 'the setup script\'s complete stdout+stderr, verbatim' },
		scriptExit: { type: 'number' },
	},
	required: ['topics', 'scriptOutput', 'scriptExit'],
}
const RUN_SCHEMA = {
	type: 'object',
	properties: { output: { type: 'string', description: 'complete stdout+stderr, verbatim' }, exit: { type: 'number' } },
	required: ['output', 'exit'],
}
const BUILD_SCHEMA = {
	type: 'object',
	properties: {
		head: { type: 'string' },
		summary: { type: 'string', description: 'what changed and why, citing file:line; becomes the PR summary' },
		cheapExit: { type: 'number', description: 'exit of your last tools/fast-gates.sh cheap run' },
		cheapResults: { type: 'string', description: 'that run\'s results.tsv, verbatim' },
		live: { type: 'object', properties: {
			expected: { type: 'array', items: { type: 'string' } },
			expectsPixelChange: { type: ['boolean', 'null'] },
			cvars: { type: 'array', items: { type: 'string' } },
		} },
		fixed: { type: 'array', items: { type: 'string' }, description: 'ids of findings/defects you fixed this round' },
		justified: { type: 'array', items: { type: 'object', properties: { id: { type: 'string' }, reason: { type: 'string' } }, required: ['id', 'reason'] } },
		openQuestions: { type: 'array', items: { type: 'string' } },
	},
	required: ['head', 'summary', 'cheapExit'],
}
const VERIFY_SCHEMA = {
	type: 'object',
	properties: {
		dir: { type: 'string' },
		verify: { type: 'string', description: 'verify.txt verbatim' },
		plan: { type: 'string', description: 'plan.tsv verbatim ("" if absent)' },
		meta: { type: 'string', description: 'meta.txt verbatim' },
		results: { type: 'string', description: 'results.tsv verbatim ("" if absent)' },
		attestation: { type: ['string', 'null'], description: 'attestation.txt verbatim, null if absent' },
		failure: { type: ['string', 'null'], description: 'failure.txt verbatim, null if absent' },
		liveResult: { type: ['string', 'null'], description: 'live/result.txt verbatim, null if no look ran' },
		observations: { type: 'array', items: { type: 'string' }, description: 'what is on each PNG you Read, per image' },
		defects: { type: 'array', items: { type: 'string' }, description: 'what on the branch frame contradicts the expectation' },
		notVerifiable: { type: 'array', items: { type: 'string' } },
	},
	required: ['verify', 'plan', 'meta', 'results'],
}
const CODEX_SCHEMA = {
	type: 'object',
	properties: {
		exit: { type: 'number', description: 'the codex_exit= value' },
		threadId: { type: ['string', 'null'], description: 'from the "codex thread: <id>" line' },
		verdict: { type: ['string', 'null'] },
		findings: { type: 'array', items: { type: 'object', properties: {
			id: { type: 'string' }, title: { type: 'string' }, severity: { type: 'string' }, confidence: { type: 'number' },
			file: { type: 'string' }, line: { type: 'number' }, body: { type: 'string' },
		}, required: ['id', 'title', 'severity'] } },
	},
	required: ['exit', 'findings'],
}

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------

const metrics = []
async function call(stage, label, text, opts) {
	const before = budget.spent()
	const result = await agent(text, { ...opts, phase: stage, label })
	metrics.push({ stage, label, outputTokens: budget.spent() - before, ok: result !== null && result !== undefined })
	return result
}
const RUN_NOTE = 'Run it exactly as given: write it to a file and start it with `bash <file> <args> > <file>.out 2>&1 &` (one `&`, never also a background flag), then poll <file>.out until its DONE/OK line or the script exits - a build can take many minutes. Do not edit the script, do not run anything it does not, do not interpret the result: return what is asked, verbatim.'
function runner(stage, label, script, extra, schema, argsLine) {
	return call(stage, label, `${RUN_NOTE}\n\nScript${argsLine ? ` (arguments: ${argsLine})` : ''}:\n\`\`\`bash\n${script}\n\`\`\`\n${extra || ''}`, { agentType: 'loop-runner', model: models.runner, schema: schema || RUN_SCHEMA })
}

let xemuLock = Promise.resolve()
function exclusive(fn) { const p = xemuLock.then(fn, fn); xemuLock = p.catch(() => {}); return p }

// ---------------------------------------------------------------------------
// Dry run
// ---------------------------------------------------------------------------

if (dryRun) {
	const ctx = { main: '/c/main', base: '<origin/main>', log: '/c/main/.sweep/fast-loop/dry-run', baselineDir: '/c/main/.sweep/fast-loop/baseline/<origin/main>' }
	const t = { slug: 'topic', worktree: `${ctx.log}/wt/topic`, cvars: ['r_showhull 1'], prTitle: 'X9-?: topic' }
	log(`dry run: agents = setup 1 + baseline 0-1 + per topic (build 1-${2 + MAX_FIXES}, verify 1-${MAX_VERIFY}, codex 0-${MAX_VERIFY + 1} calls, at most ${CODEX_ROUNDS} counted, publish 1); typical green topic 5-6; live ${live ? 'configured' : 'NOT configured (a renderer change cannot publish GREEN)'}`)
	return {
		dryRun: true,
		agents: { runLevel: '1-2', perTopic: { min: 3, typical: 4, max: 2 + MAX_FIXES + MAX_VERIFY + (MAX_VERIFY + 1) + 1 } },
		models,
		scripts: { setup: setupScript('.sweep/fast-loop/dry-run'), baseline: baselineScript(ctx), verify: verifyScript(ctx, t, 1, null), codex: codexScript(ctx, t, 1), publish: publishScript(ctx, t, { status: 'GREEN', xbeSha: '0'.repeat(64) }, { round: 1 }) },
	}
}

if (!runId) throw new Error('fast-loop: args.runId is required (e.g. "2026-09-28-x9-y")')
if (!prompt) throw new Error('fast-loop: args.prompt is required')

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

phase('Setup')
const logRel = `.sweep/fast-loop/${runId}`
const setup = await call('Setup', 'setup',
	`Run id "${runId}". Prompt for this run:\n"""\n${prompt}\n"""\n\n` +
	`1. Decide the topics: at most ${maxTasks}. A prompt that names one fork-plan item/step or one docs/r3d/design note is ONE topic, exactly that, not decomposed. Each: a short kebab-case slug (it becomes the branch name), a title, the goal in two or three sentences, and designNotes = the docs/r3d/design/*.md paths whose header names this slice (list the directory and read the headers; empty if none).\n` +
	`2. From the main checkout, run this script with your slugs as its arguments, in order. ${RUN_NOTE}\n\`\`\`bash\n${setupScript(logRel)}\n\`\`\`\n` +
	`Return the topics, the script's complete output as scriptOutput, and its exit as scriptExit.`,
	{ agentType: 'loop-runner', model: models.setup, effort: 'medium', schema: SETUP_SCHEMA })

const setupKv = parseKv(setup && setup.scriptOutput)
if (!setup || setup.scriptExit !== 0 || !/^SETUP-OK$/m.test(setup.scriptOutput || '') || !setupKv.base || !setupKv.log) {
	log(`fast-loop: setup failed - ${setup ? (setup.scriptOutput || '').split('\n').slice(-5).join(' | ') : 'no result'}`)
	throw new Error('fast-loop: setup did not prove the environment')
}
const ctx = { main: setupKv.main, base: setupKv.base, log: setupKv.log, baselineDir: setupKv.baseline_dir }
const worktrees = [...String(setup.scriptOutput).matchAll(/^worktree=(.+)\tbranch=(\S+)$/gm)].map(m => ({ path: m[1], branch: m[2] }))
const topics = setup.topics.slice(0, maxTasks).map(t => {
	const wt = worktrees.find(w => w.path.endsWith(`/wt/${t.slug}`))
	return { ...t, worktree: wt ? wt.path : null, branch: wt ? wt.branch : null, prTitle: t.title }
})
for (const t of topics) if (!t.worktree) throw new Error(`fast-loop: no worktree reported for topic ${t.slug}`)
log(`Setup: ${topics.length} topic(s) - ${topics.map(t => t.slug).join(', ')}; base ${ctx.base.slice(0, 10)}; baseline ${setupKv.baseline}`)

// The baseline build runs while Build researches and implements; Verify
// waits for it (the cache is warm, and the live look diffs against it).
const baselineP = (async () => {
	if (setupKv.baseline === 'cached') return { ok: true, cached: true }   // setup already removed its worktree
	const r = await runner('Setup', 'baseline build (origin/main)', baselineScript(ctx), 'Return the script\'s complete output and exit.')
	const ok = !!(r && /^baseline_exit=0$/m.test(r.output || ''))
	log(`Setup: baseline build ${ok ? 'ready' : `FAILED - ${r ? (r.output || '').split('\n').slice(-4).join(' | ') : 'no result'}`}`)
	return { ok, cached: false }
})()

// ---------------------------------------------------------------------------
// Per topic
// ---------------------------------------------------------------------------

function buildPrompt(t, extra) {
	return `Topic "${t.title}" (${t.slug}). Goal: ${t.goal}\n` +
		`Design notes that already specify it: ${(t.designNotes || []).join(', ') || 'none - read the fork-plan entry the prompt names, and the code'}.\n` +
		`Worktree: ${t.worktree} (branch ${t.branch}, based on origin/main ${ctx.base}). cd there first and confirm the branch.\n\n` +
		(extra || `Research and implement in this one call: read docs/r3d/lessons.md, the design note(s) and the code they cite; form the plan yourself, citing real file:line locations in your commit messages; implement it with micro-commits (AGENTS.md style and attribution rules). A new divergence gets its docs/r3d/divergences.md entry in the same change.\n`) +
		`\nBefore returning, self-gate: bash tools/fast-gates.sh cheap --base ${ctx.base} --out ${ctx.log}/${t.slug}/cheap-<n> ${gatesArgs()} (n = 1, 2, ... per attempt). Fix what is red and re-run, up to three attempts. Return cheapExit and that run's results.tsv verbatim. Commit everything: the next stage refuses a dirty tree.\n` +
		`If the change touches what the Xbox draws, return live: expected (what the ${live ? live.map : 'map'} spawn frame must show if it is right), expectsPixelChange (true / false / null if you cannot say), and cvars (console lines that make it visible, e.g. "r_showhull 1"; none if the default frame shows it).`
}

async function runTopic(t) {
	const R = { slug: t.slug, runId, status: 'RED', why: [], summary: null, codexRounds: 0, codexFiles: [], liveDir: null, livePpmdiff: null, fixed: [], justified: [], optional: [], unaddressed: [], gateRows: [], gateWhy: [], live: null, notVerifiable: [], publish: null }
	const myMetrics = () => metrics.filter(m => m.label.endsWith(`· ${t.slug}`) || m.label === 'setup' || m.label.startsWith('baseline'))

	phase('Build')
	let build = await call('Build', `build · ${t.slug}`, buildPrompt(t), { agentType: 'loop-implementer', model: models.build, schema: BUILD_SCHEMA })
	if (build && build.cheapExit !== 0) {
		log(`"${t.slug}": cheap tier red after build - one retry`)
		const retry = await call('Build', `cheap retry · ${t.slug}`, buildPrompt(t, `Your previous call returned with the cheap tier red (exit ${build.cheapExit}):\n${build.cheapResults || '(no table)'}\nRead the failing gate's log, fix the cause, commit.\n`), { agentType: 'loop-implementer', model: models.build, schema: BUILD_SCHEMA })
		if (retry) build = { ...build, ...retry, live: retry.live || build.live, summary: `${build.summary}\n\n${retry.summary}` }
	}
	if (!build) { R.why.push('build produced no result') } else { R.summary = build.summary; t.cvars = (build.live && build.live.cvars) || [] }
	const expect = build && build.live

	const baseState = await baselineP

	// Verify rounds. Codex runs alongside a verify while a review is due:
	// there has been none yet, or code changed after the last one and a
	// round remains (AGENTS.md: the second round checks what the fixes
	// introduced; there is never a third).
	let verify = null, gate = null, lastLookSha = null, liveJ = null, fixes = 0, fixedSinceReview = false, codexRetried = false
	// Actionable findings, keyed by an id THIS script assigns (C<round>-<n>):
	// a runner's own labels can repeat across rounds, and a fix naming one
	// would then close two different findings (found in review).
	const open = new Map()
	// findingKey of every finding a fix JUSTIFIED: codex raising it again is
	// not reopened. A finding claimed FIXED that comes back was not fixed and
	// must reopen - that is what the second round is for (found in review).
	const justifiedKeys = new Set()
	const reviewDue = () => R.codexRounds < CODEX_ROUNDS && (R.codexRounds === 0 || fixedSinceReview)
	const doCodex = () => { const n = R.codexRounds + 1; return runner('Verify', `codex ${n} · ${t.slug}`, codexScript(ctx, t, n),
		'When it is done, read the file it printed (CODEX-DONE <file>) in full and return codex_exit as exit, the id from the "codex thread: <id>" line, the verdict line, and every finding (severity and confidence exactly as codex gave them; id = a short stable label).',
		CODEX_SCHEMA).then(c => (c ? { ...c, file: `${ctx.log}/${t.slug}/codex-${n}.txt` } : c)) }
	const takeCodex = c => {
		if (!c) return
		if (!codexCounted(c)) { log(`"${t.slug}": codex round not counted (exit ${c.exit}, thread ${c.threadId || 'none'})`); return }
		R.codexRounds++
		R.codexFiles.push({ file: c.file, exit: c.exit, threadId: c.threadId })
		fixedSinceReview = false
		let n = 0
		for (const f of (c.findings || [])) {
			if (!actionable([f]).length) { R.optional.push(f.title); continue }
			const k = findingKey(f)
			if (justifiedKeys.has(k) || [...open.values()].some(o => findingKey(o) === k)) continue
			open.set(`C${R.codexRounds}-${++n}`, f)
		}
	}
	for (let round = 1; build && round <= MAX_VERIFY; round++) {
		phase('Verify')
		const wantCodex = reviewDue()
		const doVerify = () => runner('Verify', `verify ${round} · ${t.slug}`, verifyScript(ctx, t, round, lastLookSha),
			`When it is done, return from the directory it printed (VERIFY-DONE <dir>): verify.txt, plan.tsv, meta.txt, results.tsv, attestation.txt, failure.txt, live/result.txt - each verbatim (null/"" when absent). If live/result.txt exists, Read the PNG files named by its branch_png= and baseline_png= lines and, per image, write what is actually on screen in observations before comparing; what on the branch frame contradicts this expectation goes in defects: ${JSON.stringify(expect ? expect.expected || [] : [])}. What xemu cannot settle goes in notVerifiable.`,
			VERIFY_SCHEMA)
		const [v, c] = await parallel([
			() => (live ? exclusive(doVerify) : doVerify()),
			() => (wantCodex ? doCodex() : Promise.resolve(null)),
		])
		verify = v ? { ...v, round } : null
		const vk = parseKv(v && v.verify)
		gate = judgeGates(v ? { ...v, gateExit: v.verify ? Number(vk.gate_exit) : NaN } : null)
		R.gateRows = gate.rows || []
		R.gateWhy = gate.why
		R.xbeSha = /^[0-9a-f]{64}$/.test(vk.xbe_sha256 || '') ? vk.xbe_sha256 : null
		const needsLive = !!(gate.meta && gate.meta.needs_live === '1')
		if (v && v.liveResult) {
			liveJ = judgeLive(v, R.xbeSha, expect)
			lastLookSha = liveJ.kv.xbe_sha256 || lastLookSha
			R.live = `${liveJ.ok ? 'verified' : `DEFECT: ${liveJ.defects.join(' | ')}`}; scene: ${liveJ.kv.branch_scene || '-'}; ${liveJ.kv.ppmdiff || ''}; baseline cached: ${liveJ.kv.baseline_cached || '-'}; branch boot ${liveJ.kv.branch_boot_seconds || '?'} s, pack ${liveJ.kv.branch_pack_seconds || '?'} s\n\nObservations: ${(v.observations || []).join(' / ') || '-'}`
			R.livePng = liveJ.kv.branch_png || null
			R.livePngSha = liveJ.kv.branch_png_sha256 || null
			R.notVerifiable = v.notVerifiable || []
			R.liveDir = `${ctx.log}/${t.slug}/verify-${round}/live`
			R.livePpmdiff = liveJ.kv.ppmdiff || null
		}
		// Exit 2 from live-look.sh, or no baseline, is the machine, not the
		// branch: never a fix round - the topic ends RED with the reason.
		const liveInfra = needsLive && !!live && gate.green && (!baseState.ok || vk.live_exit === '2')
		R.liveInfra = !liveInfra ? null : !baseState.ok ? 'the origin/main baseline build failed' : `live-look.sh could not attempt the look (${(liveJ && (liveJ.kv.error || liveJ.kv.verdict)) || 'exit 2'})`
		// A green table on an XBE the look has not seen is not a verified look.
		const liveBad = needsLive && !!live && gate.green && !liveInfra && !(liveJ && liveJ.ok && liveJ.kv.xbe_sha256 === R.xbeSha)
		takeCodex(c)
		// An uncounted review on otherwise-clean code gets one codex-only
		// retry - never a second full verify of unchanged code.
		if (wantCodex && reviewDue() && !codexRetried && gate.green && !liveBad) {
			codexRetried = true
			takeCodex(await doCodex())
		}
		log(`"${t.slug}": verify ${round}: gates ${gate.green ? 'green' : `RED (${gate.why.join('; ')})`}${needsLive ? `; live ${!live ? 'not configured' : liveBad ? 'NOT OK' : 'ok'}` : ''}; codex rounds ${R.codexRounds}, open findings ${open.size}`)

		// A configuration or machine failure is not the branch's: fast-gates
		// exit 2 (plan unwritable, dirty tree), a missing baseline count, or
		// code no gate covers. Stop RED with the reason - a fix round cannot
		// change any of them (found in review: each cost two fix rounds).
		// Two exceptions are the branch's own and fixable (found in review):
		// the full tier refusing a dirty tree (a file the implementer left
		// uncommitted), and a real failing gate next to not_gated (the
		// failure can be fixed; not_gated still makes the final status RED).
		const dirtyTree = /committed tree only/.test((v && v.failure) || '')
		const failedGate = (gate.rows || []).some(x => x.status === 'ran' && x.exit !== 0)
		const stop = !gate.green && ((vk.gate_exit === '2' && !dirtyTree) || !!(gate.meta && gate.meta.missing) || !!(gate.meta && gate.meta.not_gated && !failedGate))
		if (stop) { R.why.push(`cannot be gated as configured: ${gate.why.join('; ')}`); break }
		if (R.liveInfra && open.size === 0) break

		// Clean: done. A review still due here could not be had (the retry
		// above also failed to count); finalStatus decides what that means.
		if (gate.green && !liveBad && open.size === 0) break
		if (fixes >= MAX_FIXES || round === MAX_VERIFY) break
		phase('Build')
		fixes++
		const items = [
			...(!gate.green ? [`GATES RED: ${gate.why.join('; ')}\n${(v && v.failure) || ''}`] : []),
			...(liveBad ? [`LIVE LOOK: ${liveJ ? liveJ.defects.join(' | ') : 'no look on this XBE'}\nObservations: ${((v && v.observations) || []).join(' / ')}`] : []),
			...[...open.entries()].map(([id, f]) => `FINDING ${id} [${f.severity}, confidence ${f.confidence ?? '?'}] ${f.file || ''}:${f.line || ''} ${f.title}\n${f.body || ''}`),
		]
		const fix = await call('Build', `fix ${fixes} · ${t.slug}`, buildPrompt(t,
			`Fix round ${fixes}. Each item below: fix the cause and list its id (FINDING ids as given; "gates"/"live" for those) in fixed, or - for a finding you disagree with - put it in justified with the reason. No finding may be left in neither.\n\n${items.join('\n\n')}\n`),
			{ agentType: 'loop-implementer', model: models.build, schema: BUILD_SCHEMA })
		if (!fix) { R.why.push(`fix ${fixes} produced no result`); break }
		// Ids this script did not issue match nothing: that finding stays open.
		for (const id of (fix.fixed || [])) if (open.has(id)) { R.fixed.push(open.get(id).title); open.delete(id) }
		for (const j of (fix.justified || [])) if (j && open.has(j.id)) { justifiedKeys.add(findingKey(open.get(j.id))); R.justified.push({ id: open.get(j.id).title, reason: j.reason }); open.delete(j.id) }
		if (R.codexRounds > 0) fixedSinceReview = true
		build = { ...build, ...fix, live: fix.live || build.live }
		if (fix.live && fix.live.cvars) t.cvars = fix.live.cvars
	}
	R.unaddressed = [...open.values()].map(f => f.title)
	R.reviewPending = reviewDue() && R.codexRounds > 0
	if (R.liveInfra) R.why.push(`live look not possible: ${R.liveInfra}`)

	const needsLive = !!(gate && gate.meta && gate.meta.needs_live === '1')
	R.needsLive = needsLive
	const fin = finalStatus({
		gatesGreen: !!(gate && gate.green), needsLive, liveConfigured: !!live,
		liveOk: !!(liveJ && liveJ.ok && liveJ.kv.xbe_sha256 === R.xbeSha),
		codexRounds: R.codexRounds, unaddressed: R.unaddressed.length,
	})
	R.status = fin.status
	R.why = [...R.why, ...fin.why]
	if (R.why.length && R.status === 'GREEN') R.status = 'RED'
	if (!needsLive) R.live = 'not needed (no renderer path changed)'
	else if (!live) R.live = 'NOT BOOTED - args.live not configured'

	phase('Publish')
	R.metricsSnapshot = myMetrics()
	const body = `${R.summary || ''}\n\n${renderRecord({ ...R, metrics: R.metricsSnapshot })}`
	const pub = await runner('Publish', `publish · ${t.slug}`, publishScript(ctx, t, R, verify),
		`FIRST write this PR body, byte for byte, to ${ctx.log}/${t.slug}/pr-body.md (the script reads it):\n\`\`\`markdown\n${body}\n\`\`\`\nThen run the script. Return its complete output and exit.`)
	const pk = parseKv(pub && pub.output)
	R.publish = { url: pk.url || null, isDraft: pk.is_draft === 'true', status: pk.status || null }
	if (pk.status && pk.status !== R.status) { R.why.push('publish-time check failed'); R.status = pk.status }
	if (R.publish.url && R.publish.isDraft !== (R.status !== 'GREEN')) log(`"${t.slug}": PR SHAPE WRONG - ${R.status} published with isDraft=${R.publish.isDraft}: ${R.publish.url}`)
	log(`"${t.slug}": ${R.status}${R.why.length ? ` (${R.why.join('; ')})` : ''} ${R.publish.url || '(no PR)'}`)
	return R
}

const results = await parallel(topics.map(t => () => runTopic(t)))
const baseline = await baselineP
return {
	runId,
	base: ctx.base,
	baseline,
	agents: metrics.length,
	outputTokens: metrics.reduce((a, m) => a + m.outputTokens, 0),
	topics: results.filter(Boolean).map(r => ({ slug: r.slug, status: r.status, why: r.why, pr: r.publish && r.publish.url, isDraft: r.publish && r.publish.isDraft, codexRounds: r.codexRounds, live: r.live })),
}
