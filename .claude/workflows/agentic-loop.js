// agentic-loop.js - Resonance3D's research -> implement -> QA -> publish loop,
// one chain per atomic topic, pipelined across topics, with a computed
// build gate, gate-first codex rounds, an optional live xemu look, and a
// retrospective that writes a tracked run record.
//
// Design record: docs/r3d/agentic-loop.md. Run records: docs/r3d/reports/
// agentic-loop/. Read both before changing this file.
//
// Second version, 2026-09-26, after two real runs (PRs #148 and #149; the
// records under docs/r3d/reports/agentic-loop/ have the evidence). The
// first version's shape mostly survives; what changed, and why:
//
//  1. GREEN IS COMPUTED HERE, NEVER REPORTED BY A MODEL. Run 1's QA
//     returned green with three required build gates unrun, on code that
//     did not compile for the Xbox. Now this script derives the gate plan
//     from the files the implementer actually changed (gatePlan below),
//     hands every stage the same plan, and judges the structured per-gate
//     exit codes tools/gate-chain.sh recorded. An agent can still misreport
//     an exit code; it cannot omit a gate, mark one as run without a log,
//     or say "green". lessons.md: "A measuring tool that cannot report its
//     own failure will eventually lie" - applied to the loop itself.
//  2. THE WORKTREE IS GIVEN A BUILD ENVIRONMENT AND MADE TO PROVE IT. Both
//     runs lost their gates to the same cause: a fresh worktree has
//     deps/nxdk's sources, never its built output. loop-setup now points
//     every worktree at the main checkout's built nxdk (NXDK_DIR, honoured
//     by nxdk's own activate and by every tools/xbox-*.sh site since the
//     same-day tools change) and compiles one untouched file there before
//     Research starts. A failed proof stops the run before any agent has
//     researched anything against an environment that cannot build.
//  3. CODEX ROUNDS ARE SPENT ONLY ON A DIFF THAT PASSES THE GATES, and the
//     3-round + spec-revision topology is gone. Run 1 spent one of its two
//     rounds on non-compiling code and its third round was reading only.
//     Now: gates -> codex 1 -> fix -> gates -> codex 2 -> fix -> gates ->
//     live -> publish. Still-red gates after the retry cap publish as a
//     [QA BLOCKED] draft, per v1; codex findings never block past the two
//     rounds (AGENTS.md's own cadence), they are fixed or listed in the PR.
//  4. A LIVE STAGE CAN LOOK AT THE SCREEN. Run 1's HUD render-mode bug was
//     found only by the owner booting xemu. When the change touches Xbox
//     code and args.live is configured, a QA call boots the built XBE in
//     xemu, screenshots it, and reads the PNG (the Read tool shows images).
//  5. SPECIALISTS ARE SKIPPED WHEN A DESIGN NOTE EXISTS, and a prompt that
//     names a fork-plan slice is not decomposed further ("slice" mode).
//  6. THE LOOP KEEPS ITS OWN RECORD. Per-stage output tokens are measured
//     here (budget.spent() deltas) and the retrospector writes a tracked
//     run record instead of a gitignored file nobody can read back.
//
// Unchanged, and still load-bearing: every agent is a leaf (no Agent/Task
// tool), so the agent count for a run is computable from this file - run
// with args.dryRun to see it without spawning anything; one isolated git
// worktree per topic; the 2-round codex cap tracked here, not in a
// stateless agent; the [QA BLOCKED] draft for a red final verdict.

export const meta = {
	name: 'agentic-loop',
	description: 'Research -> implement -> computed build gate -> codex (2 rounds, gate-first) -> live xemu look -> publish, per atomic topic; tracked run record at the end',
	phases: [
		{ title: 'SetUp', detail: 'mode, topics, one worktree per topic, NXDK_DIR from the main checkout, one compile as proof' },
		{ title: 'Specialists', detail: 'flagged specialists per topic; skipped when a design note already covers the topic' },
		{ title: 'Research', detail: 'per-topic spec' },
		{ title: 'Implementation', detail: 'apply the spec, self-gate with tools/gate-chain.sh; apply fix lists on retries' },
		{ title: 'QA', detail: 'run the computed gate plan; codex-review only when gates pass and a round remains' },
		{ title: 'Live', detail: 'boot the built XBE in xemu, screenshot, look' },
		{ title: 'Publisher', detail: 'push and open the PR, normal or [QA BLOCKED] draft' },
		{ title: 'Retrospector', detail: 'write the tracked run record from the stage logs and the metrics measured here' },
	],
}

// ---------------------------------------------------------------------------
// Arguments
// ---------------------------------------------------------------------------

const runId = args && args.runId ? String(args.runId) : null
const prompt = args && args.prompt ? String(args.prompt) : ''
const maxTasks = (args && args.maxTasks) || 4
// 'slice': the prompt names one fork-plan item/step or design note - no
// decomposition beyond what it names, specialists off unless forced.
// 'explore': a general prompt - decompose, consult flagged specialists.
// 'auto': loop-setup decides from the prompt and says which in its log.
const modeArg = (args && args.mode) || 'auto'
// 'auto': skip specialists for a topic that has a design note or an
// inventoried fork-plan step; otherwise consult the flagged ones. 'none'
// and 'flagged' force either way. Under 'auto' a design note on disk means
// no consultation at all, referees included (the note carries a
// "Refereed:" header for the reference-renderer pass AGENTS.md asks for;
// the publisher lists an unrefereed note as owner-side); a note-less topic
// gets its one domain specialist.
const specialistsPolicy = (args && args.specialists) || 'auto'
// Tracked sweep baselines. tools/xbox-sweep.sh exits 0 without
// --expect-fail even when files fail (by design, for ad-hoc sweeps), so a
// sweep gate on an engine change is only a gate WITH its baseline. The
// numbers move (34 -> 29 in this repo's history) and CHANGELOG.md is their
// source of truth, so they are passed in, never hardcoded here.
const baselines = (args && args.baselines) || {}
// Live verification needs the owner's machine: { xemuExe, hlSteamDir,
// map, mpakPath, gamedir?: 'valve', resourceDir?, uiLanguage? }. map and
// mpakPath name the scene to look at (a baked archive from
// tools/asset-studio --target xbox --profile nv2a for that map). Absent
// -> the live stage is recorded as not run, the PR says so, and a topic
// that touches a renderer module is refused at setup: run 1's escaped
// defect was visual, and static review cannot see a picture.
const live = (args && args.live) || null
const dryRun = !!(args && args.dryRun)
const startedAt = (args && args.startedAt) ? String(args.startedAt) : '(not supplied)'

if (!prompt) {
	throw new Error('agentic-loop: args.prompt is required. Pass args: { prompt, runId, maxTasks?, mode?, specialists?, baselines?, live?, dryRun?, startedAt? }.')
}
if (!runId && !dryRun) {
	// v1 defaulted this to "unlabeled-run"; two real runs later neither
	// run's logs could be told apart or found. A real id is not optional.
	throw new Error('agentic-loop: args.runId is required (a YYYYMMDD-HHmmss string from the calling session; this script has no clock).')
}

// A slice is detected here, by pattern, not judged by the setup agent: a
// fork-plan id ("X9-X"), an "item N step M" reference, or a design-note
// path. Both real runs were slice prompts that still got decomposed into
// a list of one plus a specialist triage. What matched is logged, so the
// run record can say why the run was scoped as it was.
const SLICE_PATTERNS = [/\bX\d+-[A-Z]\b/, /\bitem\s+\d+(?:,?\s+step\s+\d+)?/i, /docs\/r3d\/design\/[\w-]+\.md/]
const sliceIds = SLICE_PATTERNS.map(re => (prompt.match(re) || [null])[0]).filter(Boolean)
const modeDecided = modeArg !== 'auto' ? modeArg : (sliceIds.length ? 'slice' : null)

const logDir = `.sweep/agentic-loop/${runId || 'dry-run'}`
// Gate commands are BARE invocations (tools/xbox-sweep.sh ..., relying on
// each script's own shebang and PATH pin), not the "MSYSTEM=MINGW64
// /c/msys64/usr/bin/bash tools/..." form the tools' headers show:
// lessons.md's entry on that shell records it dropping exported variables
// with no error, and notes the prefix is not load-bearing - every tool
// pins mingw64 on PATH itself. NXDK_DIR travels two ways, so no shell
// boundary can lose it: an inline prefix on each command, and the
// .sweep/nxdk-dir file loop-setup writes in the worktree, which every
// tool reads when the variable is absent.
const MSYS = ''

// ---------------------------------------------------------------------------
// Specialists
// ---------------------------------------------------------------------------

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
// The two referees catch the expensive class of mistake lessons.md's "NEVER
// list an optimisation as a port without grepping the reference for it" is
// about; worth the top tier. The other six are checklist-shaped domains.
const REFEREE_SPECIALISTS = new Set(['reference-renderer-specialist', 'upstream-fidelity-specialist'])
function specialistOverride(name) {
	return REFEREE_SPECIALISTS.has(name) ? { model: 'opus', effort: 'high' } : { model: 'sonnet', effort: 'medium' }
}
// The specialist whose domain a note-less topic sits in. Run 2's one real
// bug (GetMatrix's near/far against the TriAPI's own draw matrix) was a
// hardware fact of exactly the kind docs/r3d/design/x9-x-textures-and-2d.md
// carries for steps 2-3 - and step 4 had no such note. One consultation,
// written up as the note, paid once per slice instead of once per run.
const DOMAIN_SPECIALISTS = [
	[/^(ref\/nv2a\/|engine\/platform\/xbox\/)/, 'nv2a-xbox-specialist'],
	[/^(tools\/asset-studio\/|filesystem\/)/, 'asset-pipeline-specialist'],
	[/^(scripts\/waifulib\/|wscript|tools\/xbox-[\w-]+\.sh$)/, 'build-toolchain-specialist'],
]
const DESIGN_NOTE = /^docs\/r3d\/design\/[\w-]+\.md$/
function designNotesOf(topic) {
	return (topic.designNotes || []).map(String).map(x => x.replace(/\\/g, '/')).filter(x => DESIGN_NOTE.test(x))
}

// ---------------------------------------------------------------------------
// Gate plan - pure, from the files a change touched
// ---------------------------------------------------------------------------
//
// Every command runs from the topic's worktree root. NXDK_DIR is the main
// checkout's built nxdk (from loop-setup); tools/xbox-cc.sh, xbox-cxx.sh,
// xbox-link-check.sh, xbox-game-link-check.sh and xbox-xiso.sh all honour
// it. The plan is an ordered list; tools/gate-chain.sh stops at the first
// failure because later gates depend on earlier ones (a link needs the
// sweep's objects).

const ENGINE_DIRS = /^(engine|common|public|filesystem|compat|3rdparty\/library_suffix)\//
const REF_ISOLATED = /^ref\/(soft|nv2a)\//
const XBOX_CODE = /^(engine|common|public|filesystem|compat|3rdparty\/|ref\/(soft|nv2a|null)\/|deps\/hlsdk)/
// Broad on purpose: the host tests take seconds, and a list of exactly
// which ref/nv2a files they cover would drift (run 2 added one).
const HOST_TESTED = /^(ref\/nv2a\/|compat\/xbox\/|tools\/host-tests\/)/
const DOCS_ONLY = /^(docs\/|CHANGELOG\.md$|README\.md$|AGENTS\.md$|\.claude\/agents\/|[^/]+\.md$)/
// Build infrastructure IS Xbox code for the plan: tools/xbox-*.sh are the
// gates, patches/ is applied into the submodules before every compile
// (tools/xbox-cc.sh), wscript and scripts/waifulib/ are the PC build.
const BUILD_INFRA = /^(tools\/xbox-[\w-]+\.sh$|tools\/gate-chain\.sh$|tools\/host-tests\/run\.sh$|patches\/|wscript$|scripts\/waifulib\/)/
const LOOP_SELF = /^(\.claude\/workflows\/agentic-loop\.js|tools\/test_agentic_loop\.mjs)$/

function gatePlan(filesChanged, ctx) {
	const files = (filesChanged || []).map(f => String(f).replace(/\\/g, '/').replace(/^\.\//, ''))
	const touched = re => files.some(f => re.test(f))
	const nxdk = ctx.nxdkDir ? `NXDK_DIR='${ctx.nxdkDir}' ` : ''
	const sh = cmd => `${nxdk}${cmd}`
	const gates = []
	const missing = []
	const codeFiles = files.filter(f => !DOCS_ONLY.test(f))
	const covered = []

	// `artifact`: a file the tool itself writes only on success, in the
	// worktree. The script cannot read files; loop-publisher can, and is
	// told to confirm each one exists before opening a normal PR - the
	// agent-transcribed exit code stays the residual trust, but a lie now
	// has to survive a second, independent stage looking at the disk.
	// Cheap gates first: a ref compile error - run 1's exact defect - is
	// found in seconds by the isolate, not after the sweep's minutes.
	if (touched(LOOP_SELF)) { gates.push({ name: 'loop-self-test', command: 'node tools/test_agentic_loop.mjs' }); covered.push(LOOP_SELF) }
	if (touched(/^tools\/gate-chain\.sh$/)) { gates.push({ name: 'gate-chain-self-test', command: 'bash tools/gate-chain.sh --self-test' }); covered.push(/^tools\/gate-chain\.sh$/) }
	if (touched(HOST_TESTED)) { gates.push({ name: 'host-tests', command: sh('tools/host-tests/run.sh') }); covered.push(HOST_TESTED) }
	if (touched(/^tools\/[^/]+\.py$/)) { gates.push({ name: 'python-tests', command: 'python -m unittest discover -s tools -p "test_*.py"' }); covered.push(/^tools\/[^/]+\.py$/) }
	if (touched(/^tools\/asset-studio\//)) {
		covered.push(/^tools\/asset-studio\//)
		gates.push({ name: 'asset-studio-configure', command: '[ -f tools/asset-studio/build/CMakeCache.txt ] || cmake -S tools/asset-studio -B tools/asset-studio/build' })
		gates.push({ name: 'asset-studio-build', command: 'cmake --build tools/asset-studio/build --config Release' })
		gates.push({ name: 'asset-studio-ctest', command: 'ctest --test-dir tools/asset-studio/build -C Release --output-on-failure' })
	}

	const xboxCode = touched(XBOX_CODE) || touched(BUILD_INFRA)
	let xbePath = null
	if (xboxCode) {
		covered.push(XBOX_CODE, BUILD_INFRA)
		// The sweep is a REGRESSION GATE on an engine change (needs the
		// tracked baseline) and an OBJECT PRODUCER for the link otherwise
		// (a fresh worktree has none; xbox-link-check.sh:89-93 refuses to
		// link without them). Without a baseline on an engine change the
		// gate is unconfigured - recorded as missing, never silently run
		// as if 0 failing were the baseline.
		// The isolate reads nothing the sweep produced and the link re-runs
		// it anyway: as a gate it is the ten-second fail-fast, placed first.
		for (const m of ['soft', 'nv2a']) {
			if (touched(new RegExp(`^ref/(${m}|common)/`))) gates.push({ name: `ref-isolate-${m}`, command: sh(`tools/xbox-ref-isolate.sh ${m}`), artifact: `.sweep/ref-${m}-isolated/manifest.txt` })
		}
		const engineTouched = touched(ENGINE_DIRS)
		const sweepDirs = 'engine filesystem public common compat 3rdparty/library_suffix/src'
		if (engineTouched && typeof baselines.engineSweepExpectFail !== 'number') {
			missing.push('baselines.engineSweepExpectFail (the tracked "N failing" count from CHANGELOG.md; an engine file changed)')
		}
		const expect = engineTouched && typeof baselines.engineSweepExpectFail === 'number' ? `--expect-fail ${baselines.engineSweepExpectFail} ` : ''
		gates.push({ name: 'engine-sweep', command: sh(`tools/xbox-sweep.sh ${expect}${sweepDirs}`) })

		if (touched(/^3rdparty\/mainui\//)) {
			if (typeof baselines.mainuiExpectFail !== 'number' || typeof baselines.mainuiExpectTotal !== 'number') {
				missing.push('baselines.mainuiExpectFail and baselines.mainuiExpectTotal (3rdparty/mainui changed)')
			} else {
				gates.push({ name: 'mainui-sweep', command: sh(`tools/xbox-mainui-sweep.sh --expect-fail ${baselines.mainuiExpectFail} --expect-total ${baselines.mainuiExpectTotal}`) })
			}
		}
		// The strongest link this project has: engine + third-party + real
		// client + server + freevgui, 0 duplicate / 0 undefined, and it
		// runs the game sweeps, isolates and the baseline link itself. --xbe
		// leaves the bootable image the live stage needs (with its
		// .identity sidecar, which tools/xbox-xiso.sh requires).
		xbePath = `.sweep/agentic-loop-${ctx.slug}/default.xbe`
		// The .identity sidecar is removed before the link so a stale one
		// from an earlier round cannot survive a failed relink; the publisher
		// checks it by CONTENT (sha256 of the XBE on disk == its sha256_xbe
		// == the hash QA reported), not by existence.
		gates.push({ name: 'game-link-check', command: sh(`rm -f ${xbePath}.identity && tools/xbox-game-link-check.sh hl --expect-undefined 0 --include-client --include-freevgui --xbe ${xbePath}`), artifact: `${xbePath}.identity` })
	}

	// Gates cheap enough to be a hand-off condition for the implementer:
	// no link, no codex, no xemu. The link family recompiles hundreds of
	// files and belongs at QA (once per round), not inside every edit loop.
	const cheap = gates.filter(g => /^(host-tests|python-tests|asset-studio-|ref-isolate-)/.test(g.name))
	return {
		gates,
		cheap,
		missing,
		codeFiles,
		docsOnly: codeFiles.length === 0,
		// Code this loop has no gate for (ref/gl is PC-only; engine/platform
		// backends for other targets): named in the plan and the PR, never
		// reported as "docs only".
		uncovered: codeFiles.filter(f => !covered.some(re => re.test(f))),
		needsLive: xboxCode,
		xbePath,
		// tools/gate-chain.sh's plan format: "<name><TAB><command>" per line.
		planText: gates.map(g => `${g.name}\t${g.command}`).join('\n'),
		artifacts: gates.filter(g => g.artifact).map(g => ({ gate: g.name, artifact: g.artifact })),
	}
}

// The implementer's own table, judged on the cheap subset only: a red row
// there means a compile or unit-test failure the implementer was told to
// fix before handing off, and it costs one more implementer call, never a
// QA agent (audit of runs 1-2: a compile error that reached QA cost two
// agents and a codex round).
function judgeCheap(plan, returned) {
	const byName = Object.fromEntries((returned || []).map(g => [g.gate, g]))
	const red = plan.cheap.filter(g => !(byName[g.name] && byName[g.name].status === 'ran' && byName[g.name].exit === 0))
	return { green: red.length === 0, red: red.map(g => g.name) }
}

// The environment plan loop-setup runs in the first worktree, through
// tools/gate-chain.sh, before Research. Every command reads the nxdk
// location from .sweep/nxdk-dir, which setup writes first, so the script
// can state the plan without knowing the path. "env-nxdk-reached" is the
// assertion the proof compile alone could not make: clang -v lists its
// include search path, and the only pdclib/include a worktree without its
// own deps/nxdk can find is the main checkout's.
function envPlan(liveCfg) {
	const gates = [
		{ name: 'env-nxdk-dir', command: 'test -s .sweep/nxdk-dir && test -d "$(cat .sweep/nxdk-dir)"' },
		{ name: 'env-nxdk-libs', command: 'test -f "$(cat .sweep/nxdk-dir)/lib/libpdclib.lib" && test -f "$(cat .sweep/nxdk-dir)/lib/libpbkit.lib" && test -f "$(cat .sweep/nxdk-dir)/lib/libSDL2.lib"' },
		{ name: 'env-cxbe', command: 'test -f "$(cat .sweep/nxdk-dir)/tools/cxbe/cxbe.exe" || test -x "$(cat .sweep/nxdk-dir)/tools/cxbe/cxbe"' },
		{ name: 'env-compile', command: 'tools/xbox-cc.sh public/crtlib.c -o .sweep/env-proof.obj -v > .sweep/env-proof.log 2>&1' },
		{ name: 'env-nxdk-reached', command: 'grep -q "pdclib/include" .sweep/env-proof.log && ! grep -i "ignoring nonexistent directory" .sweep/env-proof.log | grep -q pdclib' },
	]
	if (liveCfg) {
		gates.push({ name: 'env-extract-xiso', command: 'test -f "$(cat .sweep/nxdk-dir)/tools/extract-xiso/build/extract-xiso.exe"' })
		if (liveCfg.hlSteamDir) gates.push({ name: 'env-steam', command: `test -f "${liveCfg.hlSteamDir}/${liveCfg.gamedir || 'valve'}/liblist.gam"` })
		if (liveCfg.xemuExe) gates.push({ name: 'env-xemu', command: `test -f "${liveCfg.xemuExe}"` })
		if (liveCfg.mpakPath) gates.push({ name: 'env-mpak', command: `test -f "${liveCfg.mpakPath}"` })
		// The baseline build: the pristine first worktree IS origin/main,
		// so the same gates the topic will run, under their own SCRATCH,
		// leave the XBE the live look diffs against - and prove the
		// libraries, cxbe and the link before Research is paid. Skipped
		// when a previous run's baseline for this checkout still exists.
		// In the MAIN checkout (nxdk-dir's grandparent), keyed by the fork
		// point: it survives the worktree's removal and a second run on the
		// same origin/main skips the build. env-baseline-exe proves SCRATCH
		// reached the link (the exe is written under $SCRATCH), the one
		// variable .sweep/nxdk-dir cannot carry across the shell boundary.
		const B = '"$(dirname "$(dirname "$(cat .sweep/nxdk-dir)")")/.sweep/agentic-loop-baseline/$(git rev-parse origin/main)"'
		gates.push({ name: 'env-baseline-sweep', command: `B=${B}; mkdir -p "$B" && { [ -f "$B/default.xbe.identity" ] || SCRATCH="$B" tools/xbox-sweep.sh engine filesystem public common compat 3rdparty/library_suffix/src; }` })
		gates.push({ name: 'env-baseline-link', command: `B=${B}; [ -f "$B/default.xbe.identity" ] || SCRATCH="$B" tools/xbox-game-link-check.sh hl --expect-undefined 0 --include-client --include-freevgui --xbe "$B/default.xbe"` })
		gates.push({ name: 'env-baseline-exe', command: `B=${B}; test -f "$B/game-hl/xbe-build.exe" && test -f "$B/default.xbe.identity"` })
	}
	return { gates, missing: [], planText: gates.map(g => `${g.name}\t${g.command}`).join('\n') }
}

// Green iff every planned gate appears in the returned table as ran with
// exit 0, and nothing the plan needed was left unconfigured. The returned
// table is matched BY NAME against the plan this script wrote, so a stage
// cannot narrow the plan; a docs-only change has an empty plan and is
// trivially green here (codex still reviews it).
// `attestation`: the text of the attestation.txt tools/gate-chain.sh writes
// only on an all-green run, returned verbatim by the stage. The script
// cannot hash files, so it only demands the attestation exist and name
// every planned gate; loop-publisher recomputes its digests on disk
// (Codex review of this loop, 2026-09-26: exit codes a model transcribes
// are not a verdict without a file another agent can check).
function judgeGates(plan, returned, attestation, needFilesRow) {
	const byName = Object.fromEntries((returned || []).map(g => [g.gate, g]))
	const required = [...(needFilesRow && plan.gates.length ? [{ name: 'changed-files' }] : []), ...plan.gates]
	const failures = required.filter(g => {
		const r = byName[g.name]
		return !(r && r.status === 'ran' && r.exit === 0 && r.log)
	}).map(g => ({ gate: g.name, got: byName[g.name] || null }))
	if (plan.gates.length) {
		const a = String(attestation || '')
		const gatesLine = (a.match(/^gates=(.*)$/m) || [])[1] || ''
		const named = gatesLine.split(',').map(x => x.trim())
		if (!/^GATE-CHAIN-OK$/m.test(a) || !/^plan-sha256=[0-9a-f]{64}$/m.test(a) || !/^results-sha256=[0-9a-f]{64}$/m.test(a) || !required.every(g => named.includes(g.name))) {
			failures.push({ gate: 'attestation', got: attestation ? 'present but incomplete or not naming every planned gate' : null })
		}
	}
	return { green: failures.length === 0 && plan.missing.length === 0, failures, missing: plan.missing }
}

// AGENTS.md "Review on every PR": critical/high, or confidence >= 0.6, is
// fix-or-justify; the rest is optional.
function actionable(findings) {
	return (findings || []).filter(f => f && (['critical', 'high'].includes(String(f.severity || '').toLowerCase()) || Number(f.confidence) >= 0.6))
}

function gateInstructions(plan, round, slug, outRoot, files) {
	// outRoot: the absolute run log dir in the MAIN checkout; gate-chain
	// output there survives the worktree's removal (audit, persist-1).
	const out = outRoot ? `${outRoot}/gates-${slug}-${round}` : `.sweep/agentic-loop/gates-${slug}-${round}`
	const uncovered = (plan.uncovered || []).length ? `\nNOT GATED (no check in this loop covers these code paths; the PR must say so): ${plan.uncovered.join(', ')}` : ''
	if (plan.gates.length === 0) return (plan.docsOnly ? 'Gate plan: none - this change touches no code (docs only). Do not invent a gate.' : `Gate plan: none - this loop has no gate for the code touched.${uncovered}`)
	return [
		`Gate plan (${plan.gates.length} gate(s), computed by the workflow from the files changed - run exactly these, in this order, nothing else counts):`,
		plan.gates.map((g, i) => `  ${i + 1}. ${g.name}: ${g.command}`).join('\n'),
		plan.missing.length ? `UNCONFIGURED (the run cannot go green until the caller supplies these as args): ${plan.missing.join('; ')}` : '',
		`How: write the plan to .sweep/agentic-loop/gates.tsv with a literal TAB between name and command (printf '%s\\t%s\\n' name 'command' per line, or the exact text below), then run`,
		(files
			? `  printf '%s\\n' ${files.map(f => `'${f}'`).join(' ')} > .sweep/agentic-loop/files-${round}.txt\n  bash tools/gate-chain.sh --base ${files.base} --expect-files .sweep/agentic-loop/files-${round}.txt .sweep/agentic-loop/gates.tsv ${out}\n(the runner's first row, changed-files, compares that list with git diff --name-only ${files.base}...HEAD plus anything uncommitted; a mismatch is red and nothing else runs - the plan was computed from this list, so it must be the real change set)`
			: `  bash tools/gate-chain.sh .sweep/agentic-loop/gates.tsv ${out}`),
		`and read ${out}/results.tsv. Report every row as it is - name, status, exit, log path - and return the full text of ${out}/attestation.txt as "attestation" (the runner writes it only when every row ran and exited 0; null if it is not there). Never run a gate outside gate-chain.sh and report it as if it were in the table.`,
		`The chain can run for many minutes (a sweep compiles hundreds of files; the game link more): launch gate-chain.sh with a single & (or the Bash tool's own background option, never both), redirect its stderr to a file, and poll for ${out}/results.tsv - a foreground call hits the tool's timeout and looks like a hung gate.`,
		uncovered.trim(),
		'Plan text (TAB-separated):',
		plan.planText,
	].filter(Boolean).join('\n')
}

// ---------------------------------------------------------------------------
// Schemas
// ---------------------------------------------------------------------------

const GATE_ROW = {
	type: 'object',
	properties: {
		gate: { type: 'string' },
		status: { type: 'string', enum: ['ran', 'not-run'] },
		exit: { type: ['number', 'null'] },
		seconds: { type: ['number', 'null'] },
		log: { type: ['string', 'null'] },
	},
	required: ['gate', 'status', 'exit', 'log'],
}

function setupSchema(maxItems) {
	return {
		type: 'object',
		properties: {
			mode: { type: 'string', enum: ['slice', 'explore'] },
			gitState: { type: 'string' },
			nxdkDir: { type: 'string', description: 'absolute MSYS-form path (/c/..., /g/...) of the MAIN checkout\'s deps/nxdk, which has the built libraries; every worktree gate exports NXDK_DIR to it' },
			mainDir: { type: 'string', description: 'absolute path of the MAIN checkout (the repository root you were started in), in the form the agents\' shell accepts; every stage log and gate-chain output directory is rooted here, outside any worktree, so the retrospector\'s cleanup cannot remove the evidence' },
			baseSha: { type: 'string', description: 'git rev-parse origin/main right after the fetch - the commit every topic branched from; codex-review is run against this SHA, never against the local main ref, which is stale in any checkout not pulled since the last GitHub merge' },
			envGates: { type: 'array', items: GATE_ROW, description: 'every row of results.tsv from running the environment plan through tools/gate-chain.sh in the first worktree, verbatim' },
			envAttestation: { type: ['string', 'null'], description: 'the text of the attestation.txt the runner wrote for the environment plan, verbatim; null if none' },
			topics: {
				type: 'array',
				maxItems,
				items: {
					type: 'object',
					properties: {
						slug: { type: 'string', description: 'filename-safe; also the branch name' },
						title: { type: 'string' },
						brief: { type: 'string' },
						specialists: { type: 'array', items: { type: 'string' } },
						designNotes: { type: 'array', items: { type: 'string' }, description: 'paths of docs/r3d/design/*.md notes or the fork-plan item/step that already specify this topic; empty if none' },
						predictedFiles: { type: 'array', items: { type: 'string' }, description: 'repo-relative paths this topic will most likely change; used only to predict the gate plan before Implementation' },
						worktreePath: { type: 'string' },
					},
					required: ['slug', 'title', 'brief', 'specialists', 'designNotes', 'predictedFiles', 'worktreePath'],
				},
			},
			droppedTopics: { type: 'array', items: { type: 'string' } },
		},
		required: ['mode', 'nxdkDir', 'mainDir', 'baseSha', 'envGates', 'topics'],
	}
}

const SPEC_SCHEMA = {
	type: 'object',
	properties: {
		title: { type: 'string' },
		problem: { type: 'string' },
		change: { type: 'string' },
		outOfScope: { type: 'array', items: { type: 'string' } },
		verification: { type: 'string', description: 'what a live look should show if the change is right, in terms a screenshot can settle' },
		unverifiedFromNote: { type: 'array', items: { type: 'string' }, description: 'every item the design note or the specialist marked unverified / hardware-only, verbatim - carried to the live look and the PR' },
		live: {
			type: 'object',
			properties: {
				cvars: { type: 'array', items: { type: 'string' }, description: 'console lines the live boot should run after loading the map (e.g. "r_showhull 1"), so the change is exercised on screen; empty if the default scene exercises it' },
				expectsPixelChange: { type: 'boolean', description: 'true if a correct change alters the spawn frame against the baseline build with those cvars; false if it must be pixel-identical (a refactor, a code path nothing on screen exercises)' },
				expected: { type: 'array', items: { type: 'string' }, description: 'what the screenshots should show, one observable per line, in words a person reading a PNG can confirm or deny' },
			},
			required: ['cvars', 'expectsPixelChange', 'expected'],
		},
		designNoteWritten: { type: ['string', 'null'], description: 'path of the docs/r3d/design note you wrote and committed when none existed, else null' },
		citations: { type: 'array', items: { type: 'string' } },
	},
	required: ['title', 'problem', 'change', 'verification', 'unverifiedFromNote', 'live'],
}

const CHANGES_SCHEMA = {
	type: 'object',
	properties: {
		commits: { type: 'array', items: { type: 'string' } },
		filesChanged: { type: 'array', items: { type: 'string' }, description: 'every repo-relative path this topic\'s branch changes against origin/main (git diff --name-only origin/main...HEAD) - the gate plan is computed from this' },
		gates: { type: 'array', items: { type: 'object', properties: { gate: { type: 'string' }, status: { type: 'string', enum: ['ran', 'not-run'] }, exit: { type: ['number', 'null'] }, seconds: { type: ['number', 'null'] }, log: { type: ['string', 'null'] } }, required: ['gate', 'status', 'exit', 'log'] }, description: 'every row of the results.tsv from YOUR OWN tools/gate-chain.sh run of the plan you were given, verbatim; empty only if the plan was empty' },
		selfGate: { type: 'string', description: 'the results.tsv path you ran, or why you could not run the plan' },
		justified: { type: 'array', items: { type: 'string' }, description: 'on a fix round: findings deliberately NOT fixed, each with the reason' },
		summary: { type: 'string' },
		openQuestions: { type: 'array', items: { type: 'string' } },
	},
	required: ['commits', 'filesChanged', 'gates', 'summary'],
}

const QA_SCHEMA = {
	type: 'object',
	properties: {
		gateResultsPath: { type: 'string', description: 'the results.tsv tools/gate-chain.sh wrote this round' },
		attestation: { type: ['string', 'null'], description: 'the full text of attestation.txt next to that results.tsv, verbatim; null if the runner did not write one (it writes it only when every row ran and exited 0)' },
		gates: { type: 'array', items: GATE_ROW, description: 'every row of results.tsv, verbatim' },
		codexRan: { type: 'boolean', description: 'true only if tools/codex-review.sh was actually invoked this call' },
		codexExit: { type: ['number', 'null'] },
		codexThread: { type: ['string', 'null'], description: 'the id from the "codex thread: <id>" line tools/codex-review.sh prints - evidence the round really ran; null if it did not run' },
		xbeSha256: { type: ['string', 'null'], description: 'the sha256_xbe= value from the game-link-check gate\'s .identity sidecar this round, verbatim; null if that gate did not run' },
		codexFindings: {
			type: 'array',
			items: {
				type: 'object',
				properties: {
					title: { type: 'string' },
					severity: { type: 'string' },
					confidence: { type: 'number' },
					file: { type: 'string' },
					line: { type: ['number', 'null'] },
					recommendation: { type: 'string' },
				},
				required: ['title', 'severity', 'confidence'],
			},
		},
		corrections: { type: 'array', items: { type: 'string' }, description: 'what loop-implementer must do next, concrete, with the real error text' },
		summary: { type: 'string' },
	},
	required: ['gates', 'codexRan', 'summary'],
}

const LIVE_SCHEMA = {
	type: 'object',
	properties: {
		booted: { type: 'boolean' },
		xbeSha256Booted: { type: ['string', 'null'], description: 'sha256_xbe= from the .identity sidecar of the branch XBE you packed and booted, verbatim' },
		sceneReady: { type: 'object', properties: { mapReady: { type: 'boolean' }, framesAdvanced: { type: 'number' }, seconds: { type: 'number' } }, required: ['mapReady', 'framesAdvanced'], description: 'tools/xemu-debug.py wait-scene\'s result for the branch boot' },
		pixelDiff: { type: ['object', 'null'], properties: { differing: { type: 'number' }, total: { type: 'number' }, bbox: { type: ['string', 'null'] } }, description: 'tools/xemu-debug.py ppmdiff baseline vs branch at the spawn frame with the spec\'s cvars; null if no baseline boot' },
		verifyImage: { type: 'string', description: 'tools/xemu-debug.py verify-image output, or why it was not run' },
		screenshots: { type: 'array', items: { type: 'string' } },
		screenshotSha256: { type: 'array', items: { type: 'string' }, description: 'sha256sum of each file in screenshots, same order - the publisher recomputes them' },
		pixelDiffLine: { type: ['string', 'null'], description: 'the exact line tools/xemu-debug.py ppmdiff printed ("ppmdiff: N of M pixels differ, bbox=...")' },
		waitSceneLine: { type: ['string', 'null'], description: 'the exact line tools/xemu-debug.py wait-scene printed for the branch boot ("scene ready: ... frames=+N after S s")' },
		observations: { type: 'array', items: { type: 'string' }, description: 'what the PNGs actually show, in words, per screenshot' },
		defects: { type: 'array', items: { type: 'string' }, description: 'anything on screen or in the readbacks that contradicts the spec\'s verification section' },
		notVerifiable: { type: 'array', items: { type: 'string' }, description: 'what xemu cannot settle and must stay owner-side on hardware' },
		summary: { type: 'string' },
	},
	required: ['booted', 'xbeSha256Booted', 'sceneReady', 'pixelDiff', 'pixelDiffLine', 'waitSceneLine', 'screenshots', 'screenshotSha256', 'observations', 'defects', 'summary'],
}

const PUBLISH_SCHEMA = {
	type: 'object',
	properties: {
		branch: { type: 'string' },
		prNumber: { type: 'number' },
		prUrl: { type: 'string' },
		blocked: { type: 'boolean', description: 'the path you took: true for the [QA BLOCKED] draft' },
		isDraft: { type: ['boolean', 'null'], description: 'what GitHub reports after creation: gh pr view <number> --json isDraft --jq .isDraft; null if no PR exists' },
		summary: { type: 'string' },
	},
	required: ['summary', 'blocked', 'isDraft'],
}

const RETRO_SCHEMA = {
	type: 'object',
	properties: {
		summary: { type: 'string' },
		recordPath: { type: 'string' },
		pushed: { type: 'boolean', description: 'true only if origin/<branch> was seen at the record commit after a fetch' },
		commitSha: { type: ['string', 'null'], description: 'git rev-parse HEAD of the record commit; null if not committed' },
		finishedAt: { type: ['string', 'null'], description: 'date -u +%FT%TZ when you ran' },
		worktreesRemoved: { type: 'array', items: { type: 'string' } },
		worktreesKept: { type: 'array', items: { type: 'string' } },
		candidateLessons: { type: 'array', items: { type: 'string' } },
	},
	required: ['summary', 'recordPath', 'pushed'],
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function describe(value, label) {
	if (value === null || value === undefined) return `(${label} produced no result - it may have failed or been skipped; say so in your own log)`
	return JSON.stringify(value)
}

// Every agent call goes through here so the run record can say what each
// stage cost. budget.spent() is output tokens across the whole run, so
// with more than one topic in flight a delta includes concurrent agents'
// tokens - said so in the record. Wall clock is not measurable here
// (scripts have no clock); the caller passes startedAt/finishedAt.
const metrics = []
let agentCalls = 0
async function call(stage, label, agentPrompt, opts) {
	const before = budget.spent()
	agentCalls++
	const result = await agent(agentPrompt, { ...opts, phase: stage, label })
	metrics.push({ stage, label, outputTokens: budget.spent() - before, ok: result !== null && result !== undefined })
	return result
}

// ---------------------------------------------------------------------------
// Dry run: the whole plan, zero agents
// ---------------------------------------------------------------------------
//
// Workflow({ name: 'agentic-loop', args: { prompt, dryRun: true, baselines,
// live, dryRunTopics: [{ slug, files: [...] }] } }) prints the gate plan per
// predicted topic, what is unconfigured, and the agent-count arithmetic -
// "computable by reading this file" made literal.

if (dryRun) {
	const topics = (args.dryRunTopics && args.dryRunTopics.length) ? args.dryRunTopics : [{ slug: 'topic', files: args.dryRunFiles || [] }]
	const report = topics.map(t => {
		const plan = gatePlan(t.files, { slug: t.slug, nxdkDir: '/c/path/to/main/checkout/deps/nxdk' })
		return { slug: t.slug, files: t.files, gates: plan.gates.map(g => `${g.name}: ${g.command}`), missing: plan.missing, notGated: plan.uncovered, needsLive: plan.needsLive, liveConfigured: !!live }
	})
	// Counted from the chain below: research 1, implementation 1 (+1
	// self-gate retry), QA up to MAX_ROUNDS (8), fixes up to 2 gate + 2
	// codex + 1 live, live looks up to 4 (one per XBE the gates produced
	// after a fix), publisher 1 = 21.
	const perTopic = { min: 4, typical: 8, max: 21 }
	const runLevel = 2
	const specialistsNote = specialistsPolicy === 'none' ? 0 : 'flagged per topic (0-2 typical; 0 under "auto" for a slice or when a design note exists)'
	log(`dry run: ${topics.length} topic(s); agents = ${runLevel} (setup, retrospector) + specialists + per topic ${perTopic.min}-${perTopic.max} (research, 1-2 implement, 1-8 QA, 0-5 fix, 0-4 live, publish); typical one-topic slice ${runLevel + perTopic.typical}`)
	for (const r of report) {
		log(`dry run: ${r.slug}: ${r.gates.length} gate(s)${r.missing.length ? `, UNCONFIGURED: ${r.missing.join('; ')}` : ''}${r.notGated.length ? `, NOT GATED: ${r.notGated.join(', ')}` : ''}${r.needsLive ? (r.liveConfigured ? ', live stage on' : ', live stage NEEDED but args.live not given') : ''}`)
	}
	log(`dry run: mode ${modeDecided || 'decided by setup'}${sliceIds.length ? ` (slice ids in prompt: ${sliceIds.join(', ')})` : ''}`)
	const env = envPlan(live)
	log(`dry run: setup's environment plan has ${env.gates.length} gate(s): ${env.gates.map(g => g.name).join(', ')}`)
	return { dryRun: true, mode: modeDecided || 'setup decides', sliceIds, specialistsPolicy, environmentPlan: env.gates.map(g => `${g.name}: ${g.command}`), perTopicAgents: perTopic, runLevelAgents: runLevel, specialists: specialistsNote, topics: report }
}

// ---------------------------------------------------------------------------
// SetUp
// ---------------------------------------------------------------------------

phase('SetUp')
const envPlanForSetup = envPlan(live)
const setup = await call('SetUp', 'setup',
	`You are loop-setup for the agentic-loop workflow, run id "${runId}".\n\n` +
	`General prompt for this run:\n"""\n${prompt}\n"""\n\n` +
	(modeDecided
		? `Mode: ${modeDecided}${modeDecided === 'slice' ? ` - the workflow matched "${sliceIds.join('", "')}" in the prompt. One topic, exactly what the prompt names; do not decompose it and leave its "specialists" array empty (the workflow picks any consultation itself).` : '.'} Return that mode.\n`
		: `Mode: you decide - "slice" if the prompt names one fork-plan item/step or one docs/r3d/design note and needs no decomposition, else "explore" - and return it.\n`) +
	`Max topics: ${maxTasks}.\n` +
	`designNotes is a FILE CHECK, not a judgement: list docs/r3d/design/*.md, read each header, and return only the paths whose header names this topic's slice (e.g. "Input for roadmap X9 item 14, steps 2-3"); a fork-plan inventory line is not a design note. Empty if none.\n` +
	`Write your run header to ${logDir}/00-setup.md.\n` +
	`For EACH topic create its own git worktree+branch under ${logDir}/worktrees/<slug>/ as your own agent prompt instructs (branch <slug>; if that branch already exists - a re-run of the same slice - use <slug>-${runId}, never -B: the existing branch may have an open PR) (submodules initialised EXCEPT deps/nxdk), write the main checkout's built nxdk path (absolute MSYS form) into <worktree>/.sweep/nxdk-dir, and return it as nxdkDir. Return git rev-parse origin/main (right after your fetch) as baseSha.\n` +
	`Then, in the FIRST worktree, run this environment plan through tools/gate-chain.sh (write it to .sweep/agentic-loop/env-gates.tsv with a literal TAB between name and command; run: bash tools/gate-chain.sh .sweep/agentic-loop/env-gates.tsv .sweep/agentic-loop/env-gates) and return every results.tsv row verbatim as envGates plus the text of its attestation.txt as envAttestation - the workflow judges them by name and stops the run on any red row when a topic touches Xbox code:\n${envPlanForSetup.planText}\n` +
	`For each topic also return designNotes (existing docs/r3d/design/*.md or fork-plan item/step that already specify it) and predictedFiles (the repo-relative paths it will most likely change).\n` +
	`Each topic's "specialists" array may only contain names from: ${SPECIALISTS.join(', ')}.`,
	{ agentType: 'loop-setup', schema: setupSchema(maxTasks) })

if (!setup || !Array.isArray(setup.topics) || setup.topics.length === 0) {
	log(`agentic-loop: SetUp produced no usable topics - stopping. See ${logDir}/00-setup.md.`)
	throw new Error('agentic-loop: no topics to work from')
}
if (setup.topics.length > maxTasks) {
	log(`agentic-loop: SetUp returned ${setup.topics.length} topics over the ${maxTasks} cap - truncating, dropping: ${setup.topics.slice(maxTasks).map(t => t.slug).join(', ')}`)
	setup.topics = setup.topics.slice(0, maxTasks)
}
const mode = modeDecided || setup.mode
if (sliceIds.length) log(`SetUp: slice detected from the prompt (${sliceIds.join(', ')})`)
// Every later stage works inside a worktree, where a relative
// .sweep/agentic-loop/<run-id>/ would resolve INSIDE the worktree and be
// removed with it. From here on, logs and gate output go to the absolute
// run directory in the main checkout (audit, persist-1).
const absLogDir = `${String(setup.mainDir).replace(/[\\/]+$/, '')}/${logDir}`
log(`SetUp: mode ${mode}; ${setup.topics.length} topic(s) - ${setup.topics.map(t => t.slug).join(', ')}; nxdkDir ${setup.nxdkDir}`)
if (Array.isArray(setup.droppedTopics) && setup.droppedTopics.length) log(`SetUp dropped ${setup.droppedTopics.length} topic(s): ${setup.droppedTopics.join('; ')}`)

// The environment proof gates the run. A worktree that cannot compile one
// untouched engine file cannot run a single gate below; researching
// against it would only produce a [QA BLOCKED] draft hours later.
const anyXboxTopic = setup.topics.some(t => (t.predictedFiles || []).some(f => XBOX_CODE.test(String(f).replace(/\\/g, '/'))))
const envJudged = judgeGates(envPlanForSetup, setup.envGates, setup.envAttestation, false)
log(`SetUp: environment plan ${envJudged.green ? 'green' : `RED (${envJudged.failures.map(f => `${f.gate}: ${f.got ? `exit ${f.got.exit}, ${f.got.log}` : 'missing from the table'}`).join('; ')})`}`)
if (!envJudged.green && anyXboxTopic) {
	throw new Error(`agentic-loop: the worktree environment failed its plan (${envJudged.failures.map(f => f.gate).join(', ')}) and at least one topic touches Xbox code - fix the environment (is the main checkout's deps/nxdk built? is .sweep/nxdk-dir right? ${live ? 'are args.live paths right?' : ''}) before spending any agent on research. Logs: ${envJudged.failures.map(f => f.got && f.got.log).filter(Boolean).join(', ') || logDir + '/worktrees/<first>/.sweep/agentic-loop/env-gates/'}`)
}
if (!envJudged.green) log('SetUp: environment plan red, but no topic predicts Xbox code - continuing; any Xbox gate that does appear will stay red.')
const rendererTopics = setup.topics.filter(t => (t.predictedFiles || []).some(f => REF_ISOLATED.test(String(f).replace(/\\/g, '/'))))
if (rendererTopics.length && !live) {
	throw new Error(`agentic-loop: ${rendererTopics.map(t => t.slug).join(', ')} touch(es) a renderer module and args.live is not configured - the two defects static review missed on this project were visual, so a renderer change without a live look is refused before any research is paid. Pass args.live { xemuExe, hlSteamDir, map, mpakPath }.`)
}

// ---------------------------------------------------------------------------
// Specialists
// ---------------------------------------------------------------------------

phase('Specialists')
function specialistsFor(topic) {
	const flagged = (topic.specialists || []).filter(n => SPECIALISTS.includes(n))
	if (specialistsPolicy === 'none') return []
	if (specialistsPolicy === 'flagged') return flagged
	// 'auto': a design note on disk IS the consultation - run 1 paid two
	// specialists to restate a 161-line note one of them had written the
	// day before. Research greps the reference for any "the reference does
	// X" claim itself and codex's focus checks the same class, so no
	// referee runs either. Without a note, the topic gets the ONE domain
	// specialist its predicted files sit in, and Research writes that
	// input up as the note (design-4 of the audit): once per slice, not
	// once per run. An explore topic without a note keeps setup's flagged
	// list as well.
	if (designNotesOf(topic).length) return []
	const files = (topic.predictedFiles || []).map(f => String(f).replace(/\\/g, '/'))
	const domain = DOMAIN_SPECIALISTS.filter(([re]) => files.some(f => re.test(f))).map(([, name]) => name)
	const set = new Set(mode === 'slice' ? domain : [...flagged, ...domain])
	return [...set]
}

const specialistResults = await parallel(setup.topics.map(topic => async () => {
	const names = specialistsFor(topic)
	const skipped = (topic.specialists || []).filter(n => SPECIALISTS.includes(n) && !names.includes(n))
	if (skipped.length) log(`Specialists: ${topic.slug}: skipping ${skipped.join(', ')} (${designNotesOf(topic).length ? 'a design note on disk covers the topic' : 'slice mode keeps only the domain specialist'}; policy "${specialistsPolicy}")`)
	if (names.length && !designNotesOf(topic).length) log(`Specialists: ${topic.slug}: no design note - consulting ${names.join(', ')}; Research writes the note`)
	if (names.length === 0) return { slug: topic.slug, findings: [] }
	const findings = await parallel(names.map(name => async () => {
		const text = await call('Specialists', `${name} · ${topic.slug}`,
			`Topic: ${topic.title}\n\n${topic.brief}\n\n` +
			(topic.designNotes && topic.designNotes.length ? `Design notes that already specify this topic (read them first; refute or confirm what they claim in your domain, do not restate them): ${topic.designNotes.join(', ')}\n\n` : '') +
			`Read from this exact checkout, not your own working directory: ${topic.worktreePath} (this topic's worktree on a fresh origin/main).\n\n` +
			`Answer only what is in your own domain for this topic, per your own agent description; cite file:line for anything you assert. This feeds a research spec, not a human report: concise and concrete.`,
			{ agentType: name, ...specialistOverride(name) })
		return { name, text }
	}))
	return { slug: topic.slug, findings: findings.filter(Boolean) }
}))
const findingsBySlug = Object.fromEntries(specialistResults.filter(Boolean).map(r => [r.slug, r.findings]))

// ---------------------------------------------------------------------------
// Per-topic chain
// ---------------------------------------------------------------------------

// xemu, its GDB port 1234 and the pad bridge are one per machine. Topics
// run concurrently, so the live look is serialised here: pure JS, no
// clock, no extra agent (audit env-5).
let liveLock = Promise.resolve()
async function withLive(fn) {
	const prev = liveLock
	let release
	liveLock = new Promise(r => { release = r })
	await prev.then(() => {}, () => {})
	try { return await fn() } finally { release() }
}

const LIVE_FRAMES = 60   // frames host.framecount must advance after the map is ready before a screenshot counts
const GATE_RETRIES = 2   // fix rounds allowed for a red gate table before publishing blocked
const CODEX_ROUNDS = 2   // AGENTS.md's cap per PR
// Rounds through the QA loop, each one QA call: 1 initial + 2 gate retries
// + 1 null/no-hash retry + 2 codex-fix re-checks + 1 live-fix re-check + 1
// inconclusive-or-unrun-codex retry = 8. Every path through an iteration
// either breaks or consumes one of those budgets, so the loop cannot run
// longer; the audit's critic checked the wanted-but-not-run path.
const MAX_ROUNDS = 8

async function researchImplementQaPublish(topic) {
	const topicLog = `${absLogDir}/${topic.slug}.md`
	const ctx = { slug: topic.slug, nxdkDir: setup.nxdkDir }
	const worktreeNote = `Your dedicated git worktree for this topic (branch "${topic.slug}", submodules initialised except deps/nxdk): ${topic.worktreePath}. Do ALL work there, never in the main checkout. The tools find the main checkout's built nxdk through .sweep/nxdk-dir in the worktree (${setup.nxdkDir}); the gate plan's commands also carry NXDK_DIR inline. Invoke tools bare (tools/xbox-sweep.sh ...), never through /c/msys64/usr/bin/bash - that shell drops environment variables (lessons.md).`
	const specialistInput = findingsBySlug[topic.slug] || []
	const specialistText = specialistInput.length ? specialistInput.map(f => `### ${f.name}\n${f.text}`).join('\n\n') : '(no specialists consulted for this topic)'
	const notes = designNotesOf(topic)
	const designText = notes.length
		? `Design notes that already specify this topic - read them first, cite them, do not re-derive them, and copy every item they mark unverified into unverifiedFromNote: ${notes.join(', ')}`
		: `No design note exists for this topic.` + (specialistInput.length
			? ` Before the spec, write one: docs/r3d/design/${topic.slug}.md in the shape of docs/r3d/design/x9-x-textures-and-2d.md (header naming the slice, the specialist it came from, the base commit, and the line "Refereed: no (agentic-loop ${runId})" - AGENTS.md wants reference-renderer-specialist on any renderer design; the publisher lists the note as unrefereed until a human runs that pass and changes the line; the specialist's findings; an explicit list of what is unverified until hardware), commit it on this branch, and return its path as designNoteWritten. The next run then finds a file instead of paying for the same consultation.`
			: '')
	const predictedPlan = gatePlan(topic.predictedFiles, ctx)
	// Hand-off rounds run the CHEAP subset in the implementer (compile,
	// unit tests, seconds); the full plan - sweep, link, minutes - runs
	// once per round, at QA. Running it in both was the double cost the
	// audit's codex lens measured.
	const cheapPlanOf = pl => ({ ...pl, gates: pl.cheap, planText: pl.cheap.map(g => `${g.name}\t${g.command}`).join('\n') })

	const result = { topic, spec: null, changes: null, plan: null, qaRounds: [], codexRoundsUsed: 0, live: null, publish: null, finalStatus: 'RED/BLOCKED', failed: false, failureReason: null }

	const spec = await call('Research', `research · ${topic.slug}`,
		`Topic: ${topic.title}\n\n${topic.brief}\n\n${designText}\n\n${worktreeNote}\n\n` +
		`Specialist input already gathered:\n\n${specialistText}\n\n` +
		`The gates this topic's change will have to pass, as predicted from the files it is expected to touch (the real plan is recomputed from the files actually changed):\n${gateInstructions(predictedPlan, 'predicted', topic.slug, absLogDir)}\n\n` +
		`Append your log to ${topicLog} (begin with the output of date -u +%FT%TZ), then return the spec. Its "verification" must say what a live xemu look should show if the change is right.`,
		{ agentType: 'loop-research', schema: SPEC_SCHEMA })
	if (!spec) {
		log(`"${topic.title}": Research produced no result - stopping this topic.`)
		return { ...result, failed: true, failureReason: 'Research produced no result' }
	}
	result.spec = spec

	let changes = await call('Implementation', `implement · ${topic.slug}`,
		`Spec for "${topic.title}":\n${describe(spec, 'loop-research')}\n\n${worktreeNote}\n\n` +
		`Implement the spec. Before returning, self-gate: compute your own filesChanged with git diff --name-only origin/main...HEAD, then run the cheap gates below - compile and unit tests; QA runs the sweep and the link (if they no longer match what you changed, run the closest subset and say so). Iterate on failures yourself - a compile error caught here costs seconds; caught by QA it costs two agents.\n${gateInstructions(cheapPlanOf(predictedPlan), 'impl', topic.slug, absLogDir)}\n\n` +
		`Append to ${topicLog} (begin the append with the output of date -u +%FT%TZ). Return every file your branch changes against origin/main in filesChanged - the QA gate plan is computed from it.`,
		{ agentType: 'loop-implementer', schema: CHANGES_SCHEMA })
	if (!changes) {
		log(`"${topic.title}": Implementation produced no result - stopping this topic.`)
		return { ...result, failed: true, failureReason: 'Implementation produced no result' }
	}
	result.changes = changes
	if (!Array.isArray(changes.filesChanged) || changes.filesChanged.length === 0) {
		// An empty file list would compute an empty plan and judge green
		// with nothing built - and there is nothing to publish anyway.
		log(`"${topic.title}": Implementation reports no files changed against origin/main - stopping this topic.`)
		return { ...result, failed: true, failureReason: 'Implementation changed no files' }
	}

	let plan = gatePlan(changes.filesChanged, ctx)
	// A fix round can only WIDEN the plan (Codex review of this loop, round
	// 2): the plan is recomputed from the union of every file list seen so
	// far, and a fix result with no files is treated as no result, so an
	// empty or forgetful answer can never shrink the gates to nothing.
	let seenFiles = [...changes.filesChanged]
	const applyFix = (fixed) => {
		if (!fixed || !Array.isArray(fixed.filesChanged) || fixed.filesChanged.length === 0) return false
		seenFiles = [...new Set([...seenFiles, ...fixed.filesChanged])]
		changes = { ...fixed, filesChanged: seenFiles }
		result.changes = changes
		plan = gatePlan(seenFiles, ctx)
		return true
	}
	{
		const cheap = judgeCheap(plan, changes.gates)
		if (!cheap.green) {
			log(`"${topic.title}": implementer's own cheap gates red (${cheap.red.join(', ')}) - one implementer retry before QA.`)
			const again = await call('Implementation', `self-gate retry · ${topic.slug}`,
				`Spec:\n${describe(spec, 'loop-research')}\n\nYour previous result:\n${describe(changes, 'loop-implementer')}\n\n${worktreeNote}\n\n` +
				`Your own gate table shows these cheap gates red: ${cheap.red.join(', ')}. Read their logs, fix the cause, re-run the plan with tools/gate-chain.sh until they are green (or say precisely why they cannot be), and return the new table. Append to ${topicLog} (begin the append with the output of date -u +%FT%TZ).\n${gateInstructions(cheapPlanOf(plan), 'selfgate2', topic.slug, absLogDir)}`,
				{ agentType: 'loop-implementer', schema: CHANGES_SCHEMA })
			if (!applyFix(again)) log(`"${topic.title}": self-gate retry produced no usable result - keeping the first implementation.`)
		}
	}

	// One loop, one state: every round is a QA call that runs the full plan
	// and, when a codex round remains AND the diff changed since the last
	// conclusive one, tools/codex-review.sh on the fork point. After a
	// green table: the live look (when the XBE changed since the last
	// look), then the codex findings. A fix of any kind marks the diff
	// changed, so the next round re-gates and, if a round remains, reviews
	// it - a live-found fix is the riskiest code in the PR and is never
	// left unreviewed while a round is unused (audit, codex-3).
	let gatesGreen = false
	let gateRetries = 0
	let codexUsed = 0
	let codexInconclusive = 0
	let nullQa = 0
	let diffChangedSinceCodex = true
	let currentSha = null
	let lastLookedSha = null
	let liveLooks = 0
	let liveFixes = 0
	let liveStatus = plan.needsLive ? (live ? 'pending' : 'not-run (args.live not configured)') : 'not-needed'
	const deferred = []      // findings justified rather than fixed, every round
	const unaddressed = []   // actionable findings a null fix result left untouched - a red verdict
	const optional = []      // below the fix-or-justify threshold: the PR lists them as worth a glance
	let reviewStatus = 'no round run'
	let carriedFindings = []   // actionable findings from a round whose live defect took the fix slot; fixed next round
	const baselineDir = `${String(setup.mainDir).replace(/[\\/]+$/, '')}/.sweep/agentic-loop-baseline/${setup.baseSha}`

	for (let round = 1; round <= MAX_ROUNDS; round++) {
		const wantCodex = codexUsed < CODEX_ROUNDS && diffChangedSinceCodex && codexInconclusive < 2
		const verdict = await call('QA', `qa · ${topic.slug} · round ${round}`,
			`Spec:\n${describe(spec, 'loop-research')}\n\nChanges so far:\n${describe(changes, 'loop-implementer')}\n\n${worktreeNote}\n\n` +
			`${gateInstructions(plan, round, topic.slug, absLogDir, Object.assign([...changes.filesChanged], { base: setup.baseSha }))}\n\n` +
			(plan.xbePath ? `Return xbeSha256: the sha256_xbe= value from ${plan.xbePath}.identity after the game-link-check gate ran (null if it did not).\n` : '') +
			(wantCodex
				? `If and only if EVERY gate in the table ran with exit 0 (or the plan is empty because the change is docs-only), run tools/codex-review.sh --base ${setup.baseSha} (the fork point; never a movable ref) backgrounded, poll, read its printed verdict lines (it prints text: "verdict:", one line per finding with severity and confidence, "codex thread: <id>", and its exit code - there is no JSON file); set codexRan true, return codexExit, the "codex thread: <id>" id as codexThread, and every finding with severity and confidence. codexReviewRoundsRemaining for this PR: ${CODEX_ROUNDS - codexUsed}. If any gate is red, do NOT run codex - return the table and a correction list.`
				: `Do not run tools/codex-review.sh this round (codexRan false): ${codexUsed >= CODEX_ROUNDS ? 'both rounds for this PR are spent' : codexInconclusive >= 2 ? 'two runs came back inconclusive' : 'the diff has not changed since the last conclusive round'}. Run the gates, return the table verbatim.`) +
			`\n\nAppend to ${topicLog} (begin the append with the output of date -u +%FT%TZ). You do not decide green or red - the workflow computes that from your table.`,
			{ agentType: 'loop-qa', schema: QA_SCHEMA })

		if (!verdict) {
			result.qaRounds.push({ round, gates: null, gatesGreen: false, gateFailures: [], missing: [], codexRan: false, actionableFindings: [], corrections: null, summary: '(QA produced no result)' })
			nullQa++
			log(`"${topic.title}": QA round ${round} produced no result${nullQa > 1 ? ' twice - publishing as [QA BLOCKED]' : ' - retrying once'}.`)
			if (nullQa > 1) { gatesGreen = false; break }
			continue
		}
		const judged = judgeGates(plan, verdict.gates, verdict.attestation, true)
		// The XBE hash keys the live look to the build it saw. A QA agent
		// that ran the link gate green but returned no hash gets a
		// per-round sentinel instead: the look still happens, only the
		// booted-hash cross-check is skipped, and the record says so.
		const linkRow = (verdict.gates || []).find(g => g.gate === 'game-link-check')
		if (verdict.xbeSha256) currentSha = verdict.xbeSha256
		else if (linkRow && linkRow.status === 'ran' && linkRow.exit === 0) {
			// A green link row without the hash it was asked for is an
			// incomplete answer, handled like no answer: one retry, then
			// blocked. (A sentinel here would key a fresh live look to every
			// such round - the audit's critic counted eight.)
			result.qaRounds.push({ round, gates: verdict.gates, gatesGreen: false, gateFailures: [], missing: [], codexRan: false, actionableFindings: [], corrections: null, summary: '(link gate green but xbeSha256 not returned)' })
			nullQa++
			log(`"${topic.title}": QA round ${round} ran the link gate green but returned no xbeSha256${nullQa > 1 ? ' - twice; publishing as [QA BLOCKED]' : ' - retrying once'}.`)
			if (nullQa > 1) { gatesGreen = false; break }
			continue
		}

		// A codex round counts only when evidence says it ran to a verdict:
		// a thread id and an exit code the script's own contract makes a
		// verdict of (0 clean; 1 with findings). Exit 2 "inconclusive", a
		// missing plugin, a usage limit: not a round, and never "clean"
		// (audit, codex-1).
		let findings = carriedFindings
		carriedFindings = []
		if (wantCodex && !verdict.codexRan) {
			// Asked for a round and did not run it: not a clean round, not
			// a free pass through the loop either (audit, codex-1 residual).
			codexInconclusive++
			reviewStatus = 'not run by QA while a round remained'
			log(`"${topic.title}": QA round ${round} did not run codex although a round remained - counted as inconclusive${codexInconclusive >= 2 ? '; twice, review marked not completed' : ''}.`)
		}
		if (verdict.codexRan) {
			const conclusive = typeof verdict.codexThread === 'string' && verdict.codexThread.length > 0 &&
				(verdict.codexExit === 0 || (verdict.codexExit === 1 && (verdict.codexFindings || []).length > 0))
			if (conclusive) {
				codexUsed++
				diffChangedSinceCodex = false
				findings = [...findings, ...actionable(verdict.codexFindings)]
				optional.push(...(verdict.codexFindings || []).filter(f => !actionable([f]).length))
				reviewStatus = `${codexUsed} of ${CODEX_ROUNDS} round(s) completed`
			} else {
				codexInconclusive++
				reviewStatus = `not completed (codex exit ${verdict.codexExit}, thread ${verdict.codexThread || 'none'})`
				log(`"${topic.title}": codex run inconclusive (exit ${verdict.codexExit}, thread ${verdict.codexThread || 'none'}) - not counted as a round${codexInconclusive >= 2 ? '; two inconclusive runs, review marked not completed' : ''}.`)
			}
		}
		result.qaRounds.push({ round, gates: verdict.gates, attestation: verdict.attestation || null, gatesGreen: judged.green, gateFailures: judged.failures, missing: judged.missing, codexRan: !!verdict.codexRan, codexExit: verdict.codexExit, codexThread: verdict.codexThread || null, actionableFindings: findings, corrections: verdict.corrections, summary: verdict.summary })
		log(`"${topic.title}": QA round ${round}: gates ${judged.green ? 'green' : `RED (${judged.failures.map(f => f.gate).join(', ') || judged.missing.join('; ')})`}${verdict.codexRan ? `; codex: ${reviewStatus}${findings.length ? `, ${findings.length} actionable` : ''}` : ''}`)

		if (!judged.green) {
			gatesGreen = false
			if (judged.missing.length && judged.failures.length === 0) {
				log(`"${topic.title}": gate plan unconfigured (${judged.missing.join('; ')}) - no fix round can turn this green; publishing blocked.`)
				break
			}
			if (gateRetries >= GATE_RETRIES) {
				log(`"${topic.title}": gates still red after ${GATE_RETRIES} fix round(s) - publishing as [QA BLOCKED].`)
				break
			}
			gateRetries++
			const fixed = await call('Implementation', `fix gates · ${topic.slug} · ${gateRetries}`,
				`Spec:\n${describe(spec, 'loop-research')}\n\nPrevious changes:\n${describe(changes, 'loop-implementer')}\n\n` +
				`The gate table came back RED. Failures (by name, with what the table showed):\n${JSON.stringify(judged.failures, null, 2)}\n\nQA's correction list:\n${describe(verdict.corrections, 'loop-qa corrections')}\n\n${worktreeNote}\n\n` +
				`Read each failing gate's log, fix the cause, re-run the cheap gates below yourself until they are green (QA re-runs the full plan), and return. Append to ${topicLog} (begin the append with the output of date -u +%FT%TZ).\n${gateInstructions(cheapPlanOf(plan), `fix${gateRetries}`, topic.slug, absLogDir)}`,
				{ agentType: 'loop-implementer', schema: CHANGES_SCHEMA })
			if (applyFix(fixed)) diffChangedSinceCodex = true
			else log(`"${topic.title}": gate fix ${gateRetries} produced no usable result - re-judging the previous implementation.`)
			continue
		}
		gatesGreen = true

		// The live look, when the change touches Xbox code, the machine is
		// configured, and this XBE has not been looked at yet.
		if (plan.needsLive && live && currentSha && currentSha !== lastLookedSha) {
			liveLooks++
			const look = await withLive(() => call('Live', `live · ${topic.slug}${liveLooks > 1 ? ' · again' : ''}`,
				`Spec (its "verification" and "live" sections say what the screen must show):\n${describe(spec, 'loop-research')}\n\nChanges:\n${describe(changes, 'loop-implementer')}\n\n${worktreeNote}\n\n` +
				`The gates are green. tools/xbox-game-link-check.sh --xbe left the branch build at ${plan.xbePath} (its .identity sidecar and .map beside it; the linked .exe at .sweep/game-hl/xbe-build.exe). The baseline build of origin/main is at ${baselineDir}/default.xbe (linked .exe at ${baselineDir}/game-hl/xbe-build.exe). Live configuration from the caller: ${JSON.stringify(live)}.\n` +
				`The emulator is one per machine; other topics may be queued behind you. Before launching, confirm nothing answers on the GDB port (python tools/xemu-debug.py regs must FAIL to connect), and kill every xemu you launch before returning.\n` +
				`Do, from the worktree:\n` +
				`(1) Build the overlay at .sweep/agentic-loop/live-${topic.slug}/overlay/${live.gamedir || 'valve'}/: video.cfg = the retail ${live.hlSteamDir}/${live.gamedir || 'valve'}/video.cfg with its r_refdll line replaced by r_refdll "nv2a"; userconfig.d/00-live.cfg = the line map ${live.map} followed by these lines: ${JSON.stringify((spec.live && spec.live.cvars) || [])}; and a copy of ${live.mpakPath} at the gamedir root (a .mpak inside maps/ is never found - lessons.md).\n` +
				`(2) Pack TWO discs with the same overlay: NXDK_DIR='${setup.nxdkDir}' tools/xbox-xiso.sh hl --steam "${live.hlSteamDir}" --overlay <overlay dir> --xbe <xbe> --out <iso>${live.resourceDir ? ` --resource-dir "${live.resourceDir}"` : ''}${live.uiLanguage ? ` --ui-language ${live.uiLanguage}` : ''} - once with the baseline XBE (.sweep-xiso/agentic-${topic.slug}-baseline.iso), once with the branch XBE (.sweep-xiso/agentic-${topic.slug}.iso).\n` +
				`(3) For EACH disc in turn: launch "${live.xemuExe}" -s -dvd_path <iso> backgrounded; python tools/xemu-debug.py verify-image <that build's xbe-build.exe> and stop if it does not say identity confirmed; python tools/xemu-debug.py wait-scene --map <that build's .map> --exe <that build's xbe-build.exe> --frames ${LIVE_FRAMES} and take NO screenshot unless it exits 0 (return its result as sceneReady for the branch boot); then python tools/xemu-debug.py fbdump ${absLogDir}/${topic.slug}-live-<baseline|branch>.ppm and screenshot ${absLogDir}/${topic.slug}-live-<baseline|branch>.png; kill that xemu.\n` +
				`(4) python tools/xemu-debug.py ppmdiff <baseline.ppm> <branch.ppm> and return its numbers as pixelDiff. The spec says expectsPixelChange=${spec.live ? spec.live.expectsPixelChange : 'unknown'}.\n` +
				`(5) READ every PNG you took (the Read tool shows images) and write what is actually on screen, per screenshot, in observations. Compare with the spec's live.expected list: ${JSON.stringify((spec.live && spec.live.expected) || [])}. Anything that contradicts it, or a scene that never became ready, goes in defects.\n` +
				`Items the design note or specialist marked unverified (check what a screenshot can settle; the rest go to notVerifiable): ${JSON.stringify(spec.unverifiedFromNote || [])}. lessons.md: xemu is not validation for NV2A register values, so list what only hardware can settle in notVerifiable. Return xbeSha256Booted from the branch XBE's .identity. Append to ${topicLog} (begin the append with the output of date -u +%FT%TZ).`,
				{ agentType: 'loop-qa', schema: LIVE_SCHEMA }))
			result.live = look
			// The look's own claims are validated before it counts as a look
			// at this XBE (Codex review, round 3): the booted hash must be
			// real and equal to the gated one, the tool lines must be the
			// tools' own, and every screenshot must come with a hash the
			// publisher recomputes. A look that fails these is a defect, not
			// a pass with the check skipped.
			const defects = []
			if (!look) defects.push('live stage produced no result')
			else {
				if (!look.booted) defects.push(`did not boot: ${look.summary}`)
				if (!look.sceneReady || !look.sceneReady.mapReady) defects.push('the map never became ready (wait-scene)')
				else if (!(Number(look.sceneReady.framesAdvanced) >= LIVE_FRAMES)) defects.push(`only ${look.sceneReady.framesAdvanced} frame(s) advanced after ready; ${LIVE_FRAMES} required`)
				if (!/^scene ready: /.test(String(look.waitSceneLine || ''))) defects.push('no "scene ready:" line from tools/xemu-debug.py wait-scene was returned')
				if (!/^[0-9a-f]{64}$/i.test(String(look.xbeSha256Booted || ''))) defects.push('no sha256 of the booted XBE was returned')
				else if (look.xbeSha256Booted.toLowerCase() !== String(currentSha).toLowerCase()) defects.push(`booted XBE ${look.xbeSha256Booted} is not the gated one ${currentSha}`)
				if (!Array.isArray(look.screenshots) || look.screenshots.length === 0) defects.push('no screenshot was taken')
				else if (!Array.isArray(look.screenshotSha256) || look.screenshotSha256.length !== look.screenshots.length || !look.screenshotSha256.every(h => /^[0-9a-f]{64}$/i.test(String(h)))) defects.push('screenshots without a sha256 each')
				if (!/^ppmdiff: \d+ of \d+ pixels differ/.test(String(look.pixelDiffLine || ''))) defects.push('no ppmdiff line from tools/xemu-debug.py was returned')
				else {
					const m = look.pixelDiffLine.match(/^ppmdiff: (\d+) of (\d+) pixels differ/)
					if (!look.pixelDiff || Number(m[1]) !== Number(look.pixelDiff.differing) || Number(m[2]) !== Number(look.pixelDiff.total)) defects.push('pixelDiff does not match the ppmdiff line')
				}
				if (spec.live && look.pixelDiff) {
					if (spec.live.expectsPixelChange === false && look.pixelDiff.differing > 0) defects.push(`${look.pixelDiff.differing} of ${look.pixelDiff.total} pixels differ from the baseline where the spec expects none (bbox ${look.pixelDiff.bbox})`)
					if (spec.live.expectsPixelChange === true && look.pixelDiff.differing === 0) defects.push('the spawn frame is pixel-identical to the baseline where the spec expects the change to show')
				} else if (spec.live && !look.pixelDiff) defects.push('no baseline pixel diff was produced')
				defects.push(...(look.defects || []))
			}
			liveStatus = defects.length ? `defect (${defects.join(' | ')})` : 'verified'
			if (!defects.length) lastLookedSha = currentSha
			log(`"${topic.title}": live look ${liveLooks}: ${liveStatus}`)
			if (defects.length) {
				if (liveFixes >= 1) { log(`"${topic.title}": live defects again after a live fix - publishing as [QA BLOCKED].`); break }
				liveFixes++
				const fixed = await call('Implementation', `fix live · ${topic.slug}`,
					`Spec:\n${describe(spec, 'loop-research')}\n\nChanges:\n${describe(changes, 'loop-implementer')}\n\nThe live xemu look found:\n${JSON.stringify(defects, null, 2)}\n\nFull live result:\n${describe(look, 'loop-qa live')}\n\n${worktreeNote}\n\n` +
					`Fix the cause. Re-run the cheap gates below yourself (QA re-runs the full plan and rebuilds the XBE). Append to ${topicLog} (begin the append with the output of date -u +%FT%TZ).\n${gateInstructions(cheapPlanOf(plan), 'livefix', topic.slug, absLogDir)}`,
					{ agentType: 'loop-implementer', schema: CHANGES_SCHEMA })
				if (applyFix(fixed)) diffChangedSinceCodex = true
				else log(`"${topic.title}": live fix produced no usable result - re-judging the previous implementation.`)
				carriedFindings = findings
				continue
			}
		}

		// Codex findings from this round.
		if (findings.length) {
			const fixed = await call('Implementation', `fix codex · ${topic.slug} · round ${codexUsed}`,
				`Spec:\n${describe(spec, 'loop-research')}\n\nPrevious changes:\n${describe(changes, 'loop-implementer')}\n\n` +
				`codex-review round ${codexUsed} of ${CODEX_ROUNDS} returned these actionable findings (AGENTS.md: critical/high or confidence >= 0.6 is fix-or-justify):\n${JSON.stringify(findings, null, 2)}\n\n${worktreeNote}\n\n` +
				`Fix each, or list it in "justified" with the reason - never silence. Re-run the cheap gates below yourself before returning (QA re-runs the full plan). Append to ${topicLog} (begin the append with the output of date -u +%FT%TZ).\n${gateInstructions(cheapPlanOf(plan), `codexfix${codexUsed}`, topic.slug, absLogDir)}`,
				{ agentType: 'loop-implementer', schema: CHANGES_SCHEMA })
			if (applyFix(fixed)) {
				diffChangedSinceCodex = true
				deferred.push(...((fixed.justified || []).map(String)))
			} else {
				unaddressed.push(...findings)
				log(`"${topic.title}": codex fix round ${codexUsed} produced no result - ${findings.length} actionable finding(s) left unaddressed; the verdict is red.`)
				break
			}
			continue
		}

		// Nothing pending. Done when the diff has not changed since the
		// last conclusive codex round, or when no round remains.
		if ((!diffChangedSinceCodex && codexUsed >= 1) || codexUsed >= CODEX_ROUNDS || codexInconclusive >= 2) break
	}
	result.plan = plan
	result.codexRoundsUsed = codexUsed
	result.reviewStatus = reviewStatus
	result.deferred = deferred
	result.unaddressed = unaddressed
	result.optional = optional
	if (liveStatus === 'pending') liveStatus = gatesGreen ? (currentSha ? 'not-run (no look could be taken)' : 'not-run (no XBE was built)') : 'not-run (gates red)'
	result.liveStatus = liveStatus

	// Gates decide the PR's shape; so does a live defect left standing,
	// an unlooked-at final XBE, an unaddressed actionable finding, and a
	// review that never completed.
	// A renderer module in the REAL change set (not setup's prediction)
	// needs a verified look at the final XBE; without a live configuration
	// it is blocked, never GREEN with a "[not booted]" tag - that tag is for
	// engine-side changes only (Codex review of this loop, round 2).
	const rendererTouched = (plan.codeFiles || []).some(f => REF_ISOLATED.test(f))
	// Code this loop has no gate for (ref/gl, other targets' backends) is
	// not green on codex alone: "every phase must leave PC working"
	// (AGENTS.md) is a check this loop cannot run, so the PR opens as a
	// draft naming the paths and a human runs it (Codex review, round 3).
	const ungated = (plan.uncovered || []).length > 0
	const liveOk = !plan.needsLive || (live ? (liveStatus === 'verified' && lastLookedSha === currentSha) : !rendererTouched)
	if (plan.needsLive && !live && rendererTouched) liveStatus = 'not-run (args.live not configured) - a renderer change cannot publish GREEN without a look'
	// A review that never completed a round is not a review: run 1's
	// round 3 was reading only, and that is the round that passed a
	// non-compiling diff.
	const reviewOk = unaddressed.length === 0 && codexInconclusive < 2 && codexUsed >= 1
	result.finalStatus = gatesGreen && liveOk && reviewOk && !ungated ? 'GREEN' : 'RED/BLOCKED'
	const lastGates = [...result.qaRounds].reverse().find(r => r.gates)
	const lastAttestation = lastGates ? lastGates.attestation : null

	const publish = await call('Publisher', `publish · ${topic.slug}`,
		`Topic: ${topic.title}\n\nSpec:\n${describe(spec, 'loop-research')}\n\nFinal changes:\n${describe(changes, 'loop-implementer')}\n\n` +
		`FINAL STATUS: ${result.finalStatus} (computed by the workflow: gates ${gatesGreen ? 'green' : 'RED'}${ungated ? `; NOT GATED: ${plan.uncovered.join(', ')} - no check in this loop covers these paths, a human must build and run them before merge` : ''}; live ${liveStatus}${plan.needsLive && live && lastLookedSha !== currentSha ? ' (final XBE not looked at)' : ''}; review ${reviewStatus}${unaddressed.length ? `; ${unaddressed.length} actionable finding(s) UNADDRESSED` : ''}). Follow the GREEN or RED/BLOCKED path in your own prompt exactly.\n\n` +
		`Gate table for the PR's Verification section (verbatim, one row per gate):\n${describe(lastGates && lastGates.gates, 'gate table')}\n` +
		`Before taking the GREEN path, verify the last QA round's attestation BY CONTENT in the worktree, with these commands and nothing weaker: (a) sha256sum of the plan file (.sweep/agentic-loop/gates.tsv) must equal its plan-sha256 line; (b) sha256sum of the results.tsv next to it must equal its results-sha256 line; (c) git rev-parse HEAD must equal its head= line, and git status --porcelain must be empty; (d) cut -f1 of that gates.tsv must be exactly these names, in order: ${plan.gates.map(g => g.name).join(', ')}. The attestation as QA returned it:\n${describe(lastAttestation, 'attestation')}\n(e) for the live look, sha256sum of each screenshot must equal the hash QA returned for it: ${JSON.stringify(result.live && result.live.screenshots ? result.live.screenshots.map((f, i) => ({ file: f, sha256: result.live.screenshotSha256 && result.live.screenshotSha256[i] })) : [])}. Then: every log path in the table must exist; each isolate's manifest.txt named here must exist (it is written only after every check passes): ${JSON.stringify(plan.artifacts.filter(a => /manifest/.test(a.artifact)))}${plan.xbePath ? `; and the XBE must be the one the gates saw: sha256sum ${plan.xbePath} must equal the sha256_xbe= line of ${plan.xbePath}.identity and equal ${currentSha || '(no hash was reported - take the RED/BLOCKED path)'}` : ''}. If any check fails, the table was not earned - take the RED/BLOCKED path instead, say which check failed, and return blocked true.\n` +
		`codex-review: ${reviewStatus}. Findings justified rather than fixed (list them in the PR with their reasons): ${JSON.stringify(deferred)}. Unaddressed actionable findings (a blocked draft leads with these): ${JSON.stringify(unaddressed)}. Findings below the fix-or-justify threshold, worth a glance: ${JSON.stringify(optional.map(f => f.title))}.\n` +
		`Live: ${liveStatus}${result.live ? `; observations: ${JSON.stringify(result.live.observations)}; pixel diff vs baseline: ${JSON.stringify(result.live.pixelDiff)}; not verifiable in xemu: ${JSON.stringify(result.live.notVerifiable || [])}` : ''}.${plan.needsLive && !live ? ' Put "[not booted]" in the PR title: this Xbox change was never run.' : ''}\n` +
		`Owner-side (hardware) items the PR must list under one "Owner-side on hardware" heading: ${JSON.stringify([...(spec.unverifiedFromNote || []), ...((result.live && result.live.notVerifiable) || []), 'NV2A register acceptance (lessons.md: xemu accepts values hardware rejects)', 'DMA timing and unified-memory behaviour (AGENTS.md, Verification)'])}${spec.designNoteWritten ? `; the design note written by this run, ${spec.designNoteWritten}, is UNREFEREED - AGENTS.md requires reference-renderer-specialist on any docs/r3d change describing renderer behaviour before it is relied on` : ''}${plan.uncovered && plan.uncovered.length ? `; code paths no gate in this loop covers: ${plan.uncovered.join(', ')}` : ''}.\n` +
		`End the PR body with: "After merge, fill the Missed section of docs/r3d/reports/agentic-loop/${runId}.md (pushed to this branch by the retrospector)."\n\n` +
		`After gh pr create, read the PR back - gh pr view <number> --json isDraft --jq .isDraft - and return that value as isDraft; the workflow compares it with the status above.\n\n` +
		`${worktreeNote}\n\nAppend to ${topicLog} (begin the append with the output of date -u +%FT%TZ). Publish.`,
		{ agentType: 'loop-publisher', schema: PUBLISH_SCHEMA })
	result.publish = publish
	// The PR's shape is decided by the computed status, not by the
	// publisher's reading of its prompt: GitHub's own isDraft must equal
	// "status is not GREEN". A mismatch is logged here and handed to the
	// retrospector, which converts the PR to a draft (gh pr ready --undo)
	// before anything else (Codex review, third run).
	const mustBeDraft = result.finalStatus !== 'GREEN'
	if (publish && publish.prNumber) {
		result.prShapeOk = publish.isDraft === mustBeDraft && publish.blocked === mustBeDraft
		if (!result.prShapeOk) log(`"${topic.title}": PR ${publish.prUrl || publish.prNumber} has the WRONG shape (status ${result.finalStatus}, isDraft ${publish.isDraft}, publisher said blocked ${publish.blocked}) - the retrospector converts it to a draft.`)
	}
	return result
}

// pipeline(), not parallel(): topic B's research can finish while topic A
// is mid-QA. With one topic (both real runs so far) this is a serial chain
// and the run's wall clock is the sum of its stages.
const results = await pipeline(setup.topics, researchImplementQaPublish)

// ---------------------------------------------------------------------------
// Retrospector - writes the tracked run record
// ---------------------------------------------------------------------------

phase('Retrospector')
const published = results.filter(Boolean).find(r => r.publish && r.publish.branch)
const runSummary = {
	runId, startedAt, mode, sliceIds, specialistsPolicy, prompt, baseSha: setup.baseSha,
	environment: { gates: setup.envGates, green: envJudged.green, failures: envJudged.failures.map(f => f.gate) },
	agentCalls,
	metrics,
	specialists: setup.topics.map(t => ({ slug: t.slug, consulted: specialistsFor(t), flagged: t.specialists || [], designNotes: designNotesOf(t) })),
	topics: results.filter(Boolean).map(r => ({
		slug: r.topic.slug, title: r.topic.title, finalStatus: r.finalStatus, failed: r.failed, failureReason: r.failureReason,
		gates: r.plan ? r.plan.gates.map(g => g.name) : [], unconfigured: r.plan ? r.plan.missing : [],
		qaRounds: r.qaRounds.map(q => ({ round: q.round, gatesGreen: q.gatesGreen, failures: q.gateFailures.map(f => f.gate), gates: (q.gates || []).map(g => ({ gate: g.gate, exit: g.exit, seconds: g.seconds })), codexRan: q.codexRan, codexExit: q.codexExit, codexThread: q.codexThread, actionable: q.actionableFindings.length })),
		codexRoundsUsed: r.codexRoundsUsed, reviewStatus: r.reviewStatus, deferred: r.deferred, unaddressed: r.unaddressed, optional: (r.optional || []).map(f => f.title), liveStatus: r.liveStatus,
		designNoteWritten: r.spec ? r.spec.designNoteWritten : null, artifacts: r.plan ? r.plan.artifacts : [],
		findings: r.qaRounds.flatMap(q => (q.actionableFindings || []).map(f => ({ round: q.round, title: f.title, severity: f.severity, confidence: f.confidence, file: f.file, line: f.line }))),
		live: r.live ? { sceneReady: r.live.sceneReady, pixelDiff: r.live.pixelDiff, screenshots: r.live.screenshots } : null,
		pr: r.publish ? { number: r.publish.prNumber, url: r.publish.prUrl, blocked: r.publish.blocked, isDraft: r.publish.isDraft, shapeOk: r.prShapeOk } : null,
	})),
}
// The record's measured sections, rendered by the script so the numbers
// never pass through a model's transcription (audit, persist-3). The
// retrospector writes this block verbatim and adds only what it observed
// in the logs, the empty Missed section, and candidate lessons.
function md(v) { return v === null || v === undefined ? '-' : String(v).replace(/\|/g, '\\|') }
function renderRecord(sum) {
	const L = []
	L.push(`# Run ${sum.runId}`, '', `Written by the loop itself (agentic-loop.js v2). The tables below are computed from the workflow's own measurements; the sections after them are the retrospector's.`, '')
	L.push('## Scope', '', `- Prompt: ${sum.prompt}`, `- Mode: ${sum.mode}${sum.sliceIds.length ? ` (slice ids matched: ${sum.sliceIds.join(', ')})` : ''}; specialists policy: ${sum.specialistsPolicy}`, `- Fork point: ${sum.baseSha}`, `- Started: ${sum.startedAt}; finished: (the retrospector writes date -u here)`)
	for (const t of sum.topics) L.push(`- Topic ${t.slug}: ${t.title} -> ${t.finalStatus}${t.failed ? ` (failed: ${t.failureReason})` : ''}${t.pr && t.pr.url ? `; PR ${t.pr.url}${t.pr.blocked ? ' (blocked draft)' : ''}` : ''}`)
	for (const sp of sum.specialists) L.push(`- Specialists for ${sp.slug}: ${sp.consulted.length ? sp.consulted.join(', ') : 'none'} (flagged by setup: ${sp.flagged.join(', ') || 'none'}; design notes: ${sp.designNotes.join(', ') || 'none'})`)
	for (const t of sum.topics) if (t.designNoteWritten) L.push(`- Design note written by this run: ${t.designNoteWritten}`)
	L.push('', '## Cost', '', `Agent calls: ${sum.agentCalls}. Output tokens are budget.spent() deltas around each call: exact with one topic, inclusive of concurrent agents when topics overlapped; a 0 means a cached replay on a resumed run, not a free call.`, '', '| stage | label | output tokens | result |', '|---|---|---|---|')
	for (const m of sum.metrics) L.push(`| ${md(m.stage)} | ${md(m.label)} | ${md(m.outputTokens)} | ${m.ok ? 'ok' : 'none'} |`)
	L.push('', '## Gates', '', `Environment plan: ${sum.environment.green ? 'green' : `RED (${sum.environment.failures.join(', ')})`}`, '', '| gate | status | exit | seconds | log |', '|---|---|---|---|---|')
	for (const g of (sum.environment.gates || [])) L.push(`| ${md(g.gate)} | ${md(g.status)} | ${md(g.exit)} | ${md(g.seconds)} | ${md(g.log)} |`)
	for (const t of sum.topics) {
		L.push('', `Topic ${t.slug}: required ${t.gates.join(', ') || 'none'}${t.unconfigured.length ? `; UNCONFIGURED: ${t.unconfigured.join('; ')}` : ''}`)
		for (const q of t.qaRounds) {
			L.push('', `Round ${q.round}: gates ${q.gatesGreen ? 'green' : `RED (${q.failures.join(', ')})`}${q.codexRan ? `; codex ran (exit ${md(q.codexExit)}, thread ${md(q.codexThread)}, ${q.actionable} actionable)` : ''}`, '', '| gate | exit | seconds |', '|---|---|---|')
			for (const g of q.gates) L.push(`| ${md(g.gate)} | ${md(g.exit)} | ${md(g.seconds)} |`)
		}
		L.push('', `Success-only artifacts checked by the publisher: ${t.artifacts.map(a => `${a.gate}: ${a.artifact}`).join('; ') || 'none'}`)
	}
	L.push('', '## Review', '')
	for (const t of sum.topics) {
		L.push(`Topic ${t.slug}: codex ${t.reviewStatus}; ${t.codexRoundsUsed} conclusive round(s).`)
		if (t.findings.length) { L.push('', '| round | severity | confidence | finding | file:line |', '|---|---|---|---|---|'); for (const f of t.findings) L.push(`| ${md(f.round)} | ${md(f.severity)} | ${md(f.confidence)} | ${md(f.title)} | ${md(f.file)}${f.line ? ':' + f.line : ''} |`) }
		if ((t.deferred || []).length) L.push('', `Justified rather than fixed: ${t.deferred.map(md).join('; ')}`)
		if ((t.unaddressed || []).length) L.push('', `UNADDRESSED (a null fix result): ${t.unaddressed.map(f => md(f.title)).join('; ')}`)
		if ((t.optional || []).length) L.push('', `Below the threshold, worth a glance: ${t.optional.map(md).join('; ')}`)
		L.push('')
	}
	L.push('## Live', '')
	for (const t of sum.topics) L.push(`Topic ${t.slug}: ${t.liveStatus}${t.live ? `; scene ready: ${JSON.stringify(t.live.sceneReady)}; pixel diff: ${JSON.stringify(t.live.pixelDiff)}; screenshots: ${(t.live.screenshots || []).join(', ')}` : ''}`)
	L.push('', '## Observations', '', '(the retrospector: what the stage logs show that the tables do not)', '', '## Missed', '', '(for a human, after merge: what was found that the loop declared done)', '', '## Lessons', '', '(candidate docs/r3d/lessons.md entries, or "none earned"; each marked new or as a second data point for an existing heading)', '')
	return L.join('\n')
}
const recordBlock = renderRecord(runSummary)

const retro = await call('Retrospector', 'retrospector',
	`Run id "${runId}". Read every file under ${absLogDir}/ and every existing record under docs/r3d/reports/agentic-loop/ (they are this loop's memory; the README there gives the record's sections). Read docs/r3d/lessons.md's meta-rules (its first 30 lines) and its headings (grep -n '^### ' docs/r3d/lessons.md): a candidate lesson that duplicates a heading is written as a second data point for that heading, never as a new rule, and a previous record's candidate that now has a heading is noted as adopted.\n\n` +
	`The record's measured sections are rendered below by the workflow. Write the file with this block VERBATIM at the top - do not re-type a number - then fill Observations, leave Missed for a human, and write Lessons; replace the finished placeholder in Scope with the output of date -u +%FT%TZ.\n\n----- RECORD BLOCK -----\n${recordBlock}\n----- END BLOCK -----\n\n` +
	`Write the record FIRST to ${absLogDir}/retrospective-record.md (outside every worktree), then ` +
	(published
		? `copy it to docs/r3d/reports/agentic-loop/${runId}.md in ${published.topic.worktreePath} (branch "${published.topic.slug}", which already has PR ${published.publish.prUrl || published.publish.prNumber}), commit and push it there so the record lands with the PR, and return the commit's git rev-parse HEAD as commitSha and pushed true only if git fetch origin ${published.topic.slug} then shows origin/${published.topic.slug} at that commit. That one file is the only thing you may write in any worktree.`
		: `- since no topic published a branch - leave it there, set pushed false, and say in the summary that a human must commit it.`) +
	`\n\nPer-stage outputTokens above are budget.spent() deltas: exact for one topic, inclusive of concurrent agents when topics overlapped - say which applies. Record each worktree's disk cost (du -sh of the worktree and of <main>/.git/worktrees/<slug>) and the gate seconds from the results.tsv files, so the next decision about submodule cloning is measured. Build the per-stage timeline from the log appends' own timestamps and from ls -l --time-style=full-iso over ${absLogDir} (every stage log and every gates-*/results.tsv carries an mtime): that is where the run's hours went, and it goes in Observations. Leave "Missed" for a human to fill after merge. Candidate lessons.md entries only when one earns its place under that file's meta-rules; otherwise say so.\n\n` +
	`FIRST, before anything else: ${results.filter(Boolean).filter(r => r.publish && r.publish.prNumber && r.prShapeOk === false).map(r => `PR ${r.publish.prUrl || r.publish.prNumber} for "${r.topic.slug}" was opened with the wrong shape for its status ${r.finalStatus}: run gh pr ready --undo ${r.publish.prNumber} so it is a draft, prefix its title with [QA BLOCKED] if missing, and confirm with gh pr view ${r.publish.prNumber} --json isDraft.`).join(' ') || 'every published PR has the shape its status requires; nothing to convert.'}\n` +
	`Last, from the MAIN checkout ${setup.mainDir} (never from inside a worktree), and only after the record is pushed: for each GREEN topic, first copy its success-only artifacts (listed per topic above) and its .sweep/agentic-loop-*/default.xbe.identity into ${absLogDir}/artifacts-<slug>/; then remove the worktree ONLY IF git -C <worktreePath> status --porcelain is empty AND git -C <worktreePath> rev-parse HEAD equals git -C <worktreePath> rev-parse origin/<slug> after a fetch - git worktree remove --force <worktreePath> and git worktree prune. Anything else stays for a human; say which you kept and why. Every stage log and gate table already lives under ${absLogDir}/, outside the worktrees.`,
	{ agentType: 'loop-retrospector', schema: RETRO_SCHEMA })

if (!retro || !retro.pushed) log(`agentic-loop: the run record was NOT pushed - ${retro ? retro.recordPath : '(retrospector produced no result)'}; commit it by hand.`)
return { runId, mode, setup: { mainDir: setup.mainDir, nxdkDir: setup.nxdkDir, baseSha: setup.baseSha, envGates: setup.envGates, envJudged, topics: setup.topics.map(t => t.slug) }, results, runSummary, retro }
