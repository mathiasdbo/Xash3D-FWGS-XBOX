#!/usr/bin/env node
// test_fast_loop.mjs - checks for .claude/workflows/fast-loop.js that need
// no Workflow runtime, no agent and no Xbox toolchain:
//
//   node tools/test_fast_loop.mjs
//
// 1. The script parses the way the Workflow runtime loads it, and every
//    phase() title is in meta.
// 2. The pure verdict functions (judgeGates, judgeLive, codexCounted,
//    actionable, finalStatus) on fixtures - the functions that decide
//    GREEN, so each is shown able to say RED.
// 3. The WHOLE FLOW against a mocked runtime: agent() returns scripted
//    results by label, and each scenario checks the agents called, the
//    final status and what reached the publish stage. This is where a
//    control-flow bug (a fix round on an infra failure, a third codex
//    round, a GREEN with no look) shows.
// 4. Every generated bash script (dry run) passes `bash -n`, when bash
//    exists.
//
// Exit 0 only if every check passes.
import { readFileSync, writeFileSync, mkdtempSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { tmpdir } from 'node:os';
import { spawnSync } from 'node:child_process';

const here = dirname(fileURLToPath(import.meta.url));
const path = process.argv[2] || join(here, '..', '.claude', 'workflows', 'fast-loop.js');
const src = readFileSync(path, 'utf8');
let fails = 0;
const ok = (c, m) => { if (!c) { fails++; console.log('FAIL:', m); } else console.log('ok:  ', m); };

// ---- 1. parse
if (!/^\s*(\/\/[^\n]*\n|\s)*export const meta = \{/.test(src)) { console.log('FAIL: script must begin with `export const meta = {`'); process.exit(1); }
const body = src.replace(/^export const meta/m, 'const meta');
const AsyncFunction = (async () => {}).constructor;
let wf;
try { wf = new AsyncFunction('agent', 'parallel', 'pipeline', 'phase', 'log', 'args', 'budget', 'workflow', body); ok(true, 'parses as a workflow body'); }
catch (e) { console.log('FAIL: parse:', e.message); process.exit(1); }
const meta = new Function('return ' + body.match(/const meta = (\{[\s\S]*?\n\})/)[1])();
const phaseCalls = [...new Set([...body.matchAll(/phase\('([^']+)'\)/g)].map(x => x[1]))];
ok(phaseCalls.every(p => meta.phases.map(x => x.title).includes(p)), `every phase() title is in meta.phases (${phaseCalls.join(', ')})`);

// ---- 2. pure functions, sliced out by name
function fn(name) { const i = src.indexOf(`function ${name}(`); if (i < 0) throw new Error('no ' + name); return src.slice(i, src.indexOf('\n}\n', i) + 3); }
const lib = new Function(['parseKv', 'parseRows', 'planNames', 'judgeGates', 'judgeLive', 'actionable', 'codexCounted', 'finalStatus'].map(fn).join('\n') +
	'\nreturn { parseKv, judgeGates, judgeLive, actionable, codexCounted, finalStatus };')();

const H = c => c.repeat(64);
function verifyResult({ gates = ['host-tests', 'engine-sweep', 'game-link-check'], failAt = -1, needsLive = 1, xbe = H('a'), missing = '', notGated = '', live = null, attest = true, gateExit } = {}) {
	const plan = gates.map(g => `${g}\tcmd`).join('\n');
	const results = gates.map((g, i) => failAt >= 0 && i > failAt ? `${g}\tnot-run\t-\t0\t` : `${g}\tran\t${i === failAt ? 1 : 0}\t5\t/log/${g}.log`).join('\n');
	const green = failAt < 0;
	const attestation = green && gates.length && attest ? `GATE-CHAIN-OK\nplan-sha256=${H('1')}\nresults-sha256=${H('2')}\nhead=${H('3')}\ngates=${gates.join(',')}\n` : null;
	const ge = gateExit !== undefined ? gateExit : (green ? 0 : 1);
	return {
		verify: `gate_exit=${ge}\nxbe_sha256=${xbe}\n${live && live.exit !== undefined ? `live_exit=${live.exit}\n` : ''}`,
		plan, results, attestation,
		meta: `tier=full\nneeds_live=${needsLive}\nmissing=${missing}\nnot_gated=${notGated}\nhead=${H('3')}\n`,
		failure: green ? null : '--- first failing gate: /log/x.log\nerror: boom',
		liveResult: live ? live.text : null,
		observations: live ? ['the room, lit'] : [], defects: (live && live.defects) || [], notVerifiable: [],
	};
}
const liveText = ({ xbe = H('a'), scene = 'scene ready: _nv2a_map_ready=1 frames=+60 after 9 s', diff = 0, okLine = true } = {}) =>
	`scene_key=k\nxbe_sha256=${xbe}\nbranch_scene=${scene}\nbranch_png=/l/branch/frame.png\nbranch_png_sha256=${H('p')}\nbaseline_cached=yes\nppmdiff=ppmdiff: ${diff} of 307200 pixels differ, bbox=None\n${okLine ? 'LIVE-LOOK-OK\n' : ''}`;

{
	// As the runtime does: gateExit comes from verify.txt's gate_exit line.
	const g = r => lib.judgeGates(r && { ...r, gateExit: Number(lib.parseKv(r.verify).gate_exit) });
	ok(g(verifyResult()).green, 'judgeGates: all ran/0 + attestation + clean meta is green');
	ok(!g(verifyResult({ failAt: 1 })).green, 'judgeGates: a failing gate is red');
	ok(!g(verifyResult({ attest: false })).green, 'judgeGates: no attestation is red');
	const dropped = verifyResult(); dropped.results = dropped.results.split('\n').slice(0, 2).join('\n');
	ok(!g(dropped).green, 'judgeGates: a planned gate with no row is red');
	ok(!g(verifyResult({ gateExit: 0, failAt: 1 })).green, 'judgeGates: a red row with a transcribed exit 0 is still red');
	ok(!g(verifyResult({ missing: 'engine-expect-fail' })).green, 'judgeGates: a missing baseline is red');
	ok(!g(verifyResult({ notGated: 'ref/gl/x.c' })).green, 'judgeGates: ungated code is red');
	ok(g(verifyResult({ gates: [], needsLive: 0 })).green, 'judgeGates: an empty plan (docs only) is green');
	ok(!g(null).green, 'judgeGates: no result is red');
	const noMeta = verifyResult(); noMeta.meta = '';
	ok(!g(noMeta).green, 'judgeGates: no meta.txt is red');

	const lv = (live, sha, exp) => lib.judgeLive({ liveResult: live, defects: [] }, sha, exp);
	ok(lv(liveText(), H('a'), { expectsPixelChange: false }).ok, 'judgeLive: matching XBE, scene, 0 px where none expected is ok');
	ok(!lv(liveText({ xbe: H('b') }), H('a'), null).ok, 'judgeLive: a look at another XBE is not ok');
	ok(!lv(liveText({ okLine: false }), H('a'), null).ok, 'judgeLive: no LIVE-LOOK-OK is not ok');
	ok(!lv(liveText({ scene: 'wait-scene: TIMEOUT' }), H('a'), null).ok, 'judgeLive: no scene line is not ok');
	ok(!lv(liveText({ diff: 12 }), H('a'), { expectsPixelChange: false }).ok, 'judgeLive: pixels changed where none expected');
	ok(!lv(liveText({ diff: 0 }), H('a'), { expectsPixelChange: true }).ok, 'judgeLive: identical frame where a change is expected');
	ok(!lib.judgeLive({ liveResult: liveText(), defects: ['HUD is black'] }, H('a'), null).ok, 'judgeLive: a defect the runner saw is not ok');
	ok(!lib.judgeLive(null, H('a'), null).ok, 'judgeLive: no look is not ok');

	ok(lib.codexCounted({ exit: 0, threadId: 't', findings: [] }), 'codexCounted: exit 0 with a thread counts');
	ok(lib.codexCounted({ exit: 1, threadId: 't', findings: [{}] }), 'codexCounted: exit 1 with findings counts');
	ok(!lib.codexCounted({ exit: 2, threadId: 't', findings: [] }), 'codexCounted: inconclusive does not count');
	ok(!lib.codexCounted({ exit: 0, threadId: null, findings: [] }), 'codexCounted: no thread does not count');
	ok(lib.actionable([{ severity: 'low', confidence: 0.6 }, { severity: 'High' }, { severity: 'low', confidence: 0.5 }]).length === 2, 'actionable: high or confidence >= 0.6');
	const fs = x => lib.finalStatus({ gatesGreen: true, needsLive: false, liveConfigured: true, liveOk: false, codexRounds: 1, unaddressed: 0, ...x }).status;
	ok(fs({}) === 'GREEN', 'finalStatus: green gates + a review is GREEN');
	ok(fs({ gatesGreen: false }) === 'RED', 'finalStatus: red gates');
	ok(fs({ needsLive: true }) === 'RED', 'finalStatus: renderer change without a verified look');
	ok(fs({ needsLive: true, liveConfigured: false }) === 'RED', 'finalStatus: renderer change with live unconfigured');
	ok(fs({ codexRounds: 0 }) === 'RED', 'finalStatus: no review');
	ok(fs({ unaddressed: 1 }) === 'RED', 'finalStatus: an unaddressed finding');
}

// ---- 3. the whole flow, mocked
const LIVE = { xemuExe: '/c/xemu/xemu.exe', hlSteamDir: '/d/Steam Library/Half-Life', map: 'crossfire', mpakPath: '/c/m/crossfire.mpak' };
const setupOut = (baseline = 'cached', slugs = ['t1']) => slugs.map(s => `worktree=/c/Mathias Development/main/.sweep/fast-loop/r/wt/${s}\tbranch=${s}`).join('\n') +
	`\nmain=/c/main\nbase=${H('b')}\nlog=/c/main/.sweep/fast-loop/r\nbaseline_dir=/c/main/.sweep/fast-loop/baseline/${H('b')}\nbaseline=${baseline}\nSETUP-OK\n`;
const codex = (findings = [], exit) => ({ exit: exit !== undefined ? exit : (findings.length ? 1 : 0), threadId: exit === 2 ? null : 'th', verdict: 'v', findings });
const HIGH = { id: 'F1', title: 'null deref', severity: 'high', confidence: 0.9, file: 'ref/nv2a/r.c', line: 3 };

async function scenario(name, { args = {}, script, expect }) {
	const calls = [], logs = [];
	const counters = {};
	const agent = async (text, opts) => {
		calls.push({ label: opts.label, text, opts });
		const kind = opts.label.split(/[ ·]/)[0];
		counters[kind] = (counters[kind] || 0) + 1;
		const r = script[kind];
		return typeof r === 'function' ? r(counters[kind], text) : r;
	};
	let spent = 0;
	const budget = { spent: () => (spent += 10) };
	const parallel = fns => Promise.all(fns.map(f => f()));
	let out, err = null;
	try { out = await wf(agent, parallel, null, () => {}, m => logs.push(m), { prompt: 'X9-Z step 1', runId: 'r', maxTasks: 1, ...args }, budget, null); }
	catch (e) { err = e; }
	const labels = calls.map(c => c.label.split(' · ')[0]);
	const pub = calls.find(c => c.label.startsWith('publish'));
	expect({ out, err, labels, logs, pub, calls }, name);
}

// A fix agent answers with the finding ids the workflow sent it.
const sentIds = text => [...text.matchAll(/^FINDING (\S+) /gm)].map(m => m[1]);
const fixAll = (i, text) => ({ ...buildOk, fixed: sentIds(text) });
const buildOk = { head: H('3'), summary: 'did it', cheapExit: 0, cheapResults: '', live: { expected: ['room lit'], expectsPixelChange: false, cvars: [] }, fixed: [], justified: [] };
// The publish mock reports the status the workflow computed and wrote into
// its script - so a scenario tests the workflow's verdict, not the mock's.
const pubEcho = (i, text) => pubOk(/^STATUS=GREEN$/m.test(text));
const pubOk = green => ({ output: `status=${green ? 'GREEN' : 'RED'}\nurl=https://github.com/o/r/pull/1\nnumber=1\nis_draft=${!green}\nPUBLISH-DONE`, exit: 0 });

await scenario('docs-only topic', {
	script: { setup: { topics: [{ slug: 't1', title: 'T', goal: 'g' }], scriptOutput: setupOut(), scriptExit: 0 }, build: buildOk,
		verify: verifyResult({ gates: [], needsLive: 0 }), codex: codex(), publish: pubEcho },
	expect: ({ out, labels, pub }, n) => {
		ok(out && out.topics[0].status === 'GREEN', `${n}: GREEN`);
		ok(labels.join(',') === 'setup,build,verify 1,codex 1,publish', `${n}: 5 agents, baseline skipped when cached (${labels.join(',')})`);
		ok(/STATUS=GREEN/.test(pub.text) && /re-plan ran/.test(pub.text), `${n}: publish runs the on-disk checks`);
	},
});

await scenario('renderer topic: finding fixed, second review, live verified', {
	args: { live: LIVE },
	script: {
		setup: { topics: [{ slug: 't1', title: 'T', goal: 'g' }], scriptOutput: setupOut('missing'), scriptExit: 0 },
		baseline: { output: 'baseline_exit=0\nBASELINE-DONE', exit: 0 },
		build: buildOk, fix: fixAll,
		verify: i => verifyResult({ xbe: H(i === 1 ? 'a' : 'c'), live: { text: liveText({ xbe: H(i === 1 ? 'a' : 'c') }) } }),
		codex: i => (i === 1 ? codex([HIGH]) : codex()),
		publish: pubEcho,
	},
	expect: ({ out, labels, calls }, n) => {
		ok(out && out.topics[0].status === 'GREEN', `${n}: GREEN (${out && out.topics[0].why})`);
		// verify and codex of one round run concurrently: compare sets, then the order across rounds.
		ok([...labels].sort().join(',') === ['setup', 'baseline build (origin/main)', 'build', 'verify 1', 'codex 1', 'fix 1', 'verify 2', 'codex 2', 'publish'].sort().join(','), `${n}: the nine expected agents (${labels.join(',')})`);
		ok(labels.indexOf('fix 1') > labels.indexOf('verify 1') && labels.indexOf('fix 1') > labels.indexOf('codex 1') && labels.indexOf('verify 2') > labels.indexOf('fix 1') && labels.indexOf('codex 2') > labels.indexOf('fix 1'), `${n}: fix comes between the two rounds`);
		const v2 = calls.find(c => c.label.startsWith('verify 2'));
		ok(v2 && v2.text.includes(`!= '${H('a')}'`), `${n}: second look skips only the XBE already looked at`);
		ok(v2 && v2.text.includes("--steam '/d/Steam Library/Half-Life'"), `${n}: a path with a space is quoted`);
	},
});

await scenario('gates stay red', {
	script: { setup: { topics: [{ slug: 't1', title: 'T', goal: 'g' }], scriptOutput: setupOut(), scriptExit: 0 }, build: buildOk, fix: buildOk,
		verify: verifyResult({ failAt: 2 }), codex: codex(), publish: pubEcho },
	expect: ({ out, labels, pub }, n) => {
		ok(out && out.topics[0].status === 'RED', `${n}: RED`);
		ok(labels.filter(l => l.startsWith('verify')).length === 3 && labels.filter(l => l.startsWith('fix')).length === 2, `${n}: 3 verifies, 2 fixes, then stop (${labels.join(',')})`);
		ok(labels.filter(l => l.startsWith('codex')).length === 2, `${n}: never a third codex round`);
		ok(/STATUS=RED/.test(pub.text) && !/re-plan ran/.test(pub.text), `${n}: RED publishes a draft without the green checks`);
	},
});

await scenario('look at a stale XBE is a fix round, not green', {
	args: { live: LIVE },
	script: { setup: { topics: [{ slug: 't1', title: 'T', goal: 'g' }], scriptOutput: setupOut(), scriptExit: 0 }, build: buildOk, fix: buildOk,
		verify: i => verifyResult({ xbe: H('a'), live: { text: liveText({ xbe: i === 1 ? H('z') : H('a') }) } }), codex: codex(), publish: pubEcho },
	expect: ({ out, labels }, n) => {
		ok(labels.includes('fix 1'), `${n}: the mismatch goes back to Build (${labels.join(',')})`);
		ok(out && out.topics[0].status === 'GREEN', `${n}: GREEN once the look matches`);
	},
});

await scenario('new XBE whose look never ran', {
	args: { live: LIVE },
	script: { setup: { topics: [{ slug: 't1', title: 'T', goal: 'g' }], scriptOutput: setupOut(), scriptExit: 0 }, build: buildOk, fix: fixAll,
		verify: i => verifyResult({ xbe: H(i === 1 ? 'a' : 'c'), live: i === 1 ? { text: liveText({ xbe: H('a') }) } : null }),
		codex: i => (i === 1 ? codex([HIGH]) : codex()), publish: pubEcho },
	expect: ({ out }, n) => ok(out && out.topics[0].status === 'RED', `${n}: RED - the final XBE was never looked at`),
});

await scenario('live look cannot be attempted', {
	args: { live: LIVE },
	script: { setup: { topics: [{ slug: 't1', title: 'T', goal: 'g' }], scriptOutput: setupOut(), scriptExit: 0 }, build: buildOk, fix: buildOk,
		verify: verifyResult({ live: { exit: 2, text: 'error=a GDB stub already answers\n' } }), codex: codex(), publish: pubEcho },
	expect: ({ out, labels }, n) => {
		ok(!labels.some(l => l.startsWith('fix')), `${n}: no fix round for a machine problem (${labels.join(',')})`);
		ok(out && out.topics[0].status === 'RED' && out.topics[0].why.some(w => /live look not possible/.test(w)), `${n}: RED with the reason`);
	},
});

await scenario('renderer change with live unconfigured', {
	script: { setup: { topics: [{ slug: 't1', title: 'T', goal: 'g' }], scriptOutput: setupOut(), scriptExit: 0 }, build: buildOk,
		verify: verifyResult(), codex: codex(), publish: pubEcho },
	expect: ({ out }, n) => ok(out && out.topics[0].status === 'RED', `${n}: RED - never booted`),
});

await scenario('codex inconclusive twice', {
	script: { setup: { topics: [{ slug: 't1', title: 'T', goal: 'g' }], scriptOutput: setupOut(), scriptExit: 0 }, build: buildOk,
		verify: verifyResult({ needsLive: 0 }), codex: codex([], 2), publish: pubEcho },
	expect: ({ out, labels }, n) => {
		ok(labels.join(',') === 'setup,build,verify 1,codex 1,codex 1,publish', `${n}: one codex-only retry, no re-verify (${labels.join(',')})`);
		ok(out && out.topics[0].status === 'RED', `${n}: RED - no counted review`);
	},
});

await scenario('justified finding', {
	script: { setup: { topics: [{ slug: 't1', title: 'T', goal: 'g' }], scriptOutput: setupOut(), scriptExit: 0 }, build: buildOk,
		fix: (i, text) => ({ ...buildOk, justified: sentIds(text).map(id => ({ id, reason: 'upstream behaviour' })) }),
		verify: verifyResult({ needsLive: 0 }), codex: i => (i === 1 ? codex([HIGH]) : codex()), publish: pubEcho },
	expect: ({ out, pub }, n) => {
		ok(out && out.topics[0].status === 'GREEN', `${n}: GREEN`);
		ok(pub && pub.text.includes('Not fixed: null deref - upstream behaviour'), `${n}: the reason is in the PR body`);
	},
});

await scenario('finding neither fixed nor justified', {
	script: { setup: { topics: [{ slug: 't1', title: 'T', goal: 'g' }], scriptOutput: setupOut(), scriptExit: 0 }, build: buildOk, fix: buildOk,
		verify: verifyResult({ needsLive: 0 }), codex: codex([HIGH]), publish: pubEcho },
	expect: ({ out }, n) => ok(out && out.topics[0].status === 'RED', `${n}: RED`),
});

const F2a = { id: 'F2', title: 'use after free', severity: 'high', confidence: 0.9 }
const F2b = { id: 'F2', title: 'leak', severity: 'high', confidence: 0.9 }
await scenario('runner labels repeat across codex rounds', {
	script: { setup: { topics: [{ slug: 't1', title: 'T', goal: 'g' }], scriptOutput: setupOut(), scriptExit: 0 }, build: buildOk,
		// Fix 1 fixes only the first finding it was sent; fix 2 answers with the runner's own label "F2".
		fix: (i, text) => (i === 1 ? { ...buildOk, fixed: sentIds(text).slice(0, 1) } : { ...buildOk, fixed: ['F2'] }),
		verify: verifyResult({ needsLive: 0 }), codex: i => (i === 1 ? codex([HIGH, F2a]) : codex([F2b])), publish: pubEcho },
	expect: ({ out }, n) => ok(out && out.topics[0].status === 'RED', `${n}: RED - a label the workflow did not issue closes nothing`),
});

await scenario('a fix that did not fix', {
	script: { setup: { topics: [{ slug: 't1', title: 'T', goal: 'g' }], scriptOutput: setupOut(), scriptExit: 0 }, build: buildOk, fix: fixAll,
		verify: verifyResult({ needsLive: 0 }), codex: codex([HIGH]), publish: pubEcho },
	// Round 2 raising the same finding again means fix 1 did not fix it: it
	// reopens and goes to a second fix round (AGENTS.md: fix and stop - no
	// third codex round follows, so the second fix is what the PR carries).
	expect: ({ labels, calls }, n) => {
		const fix2 = calls.find(c => c.label.startsWith('fix 2'));
		ok(fix2 && /^FINDING C2-1 .*null deref/m.test(fix2.text), `${n}: the re-raised finding reopens and is sent to a second fix (${labels.join(',')})`);
	},
});

await scenario('a justified finding raised again', {
	script: { setup: { topics: [{ slug: 't1', title: 'T', goal: 'g' }], scriptOutput: setupOut(), scriptExit: 0 }, build: buildOk,
		fix: (i, text) => ({ ...buildOk, justified: sentIds(text).map(id => ({ id, reason: 'by design' })) }),
		verify: verifyResult({ needsLive: 0 }), codex: codex([HIGH]), publish: pubEcho },
	expect: ({ out, labels }, n) => ok(out && out.topics[0].status === 'GREEN' && !labels.includes('fix 2'), `${n}: not reopened (${labels.join(',')})`),
});

await scenario('full tier refuses a dirty tree', {
	script: { setup: { topics: [{ slug: 't1', title: 'T', goal: 'g' }], scriptOutput: setupOut(), scriptExit: 0 }, build: buildOk, fix: fixAll,
		verify: i => (i === 1 ? { ...verifyResult({ needsLive: 0, gates: [], gateExit: 2 }), failure: '--- first failing gate: \nfast-gates: the full tier runs on a committed tree only - commit or remove:\n?? stray.txt' } : verifyResult({ needsLive: 0 })),
		codex: codex(), publish: pubEcho },
	expect: ({ out, labels }, n) => ok(labels.includes('fix 1') && out && out.topics[0].status === 'GREEN', `${n}: one fix round, then GREEN (${labels.join(',')})`),
});

await scenario('ungated path next to a real compile failure', {
	script: { setup: { topics: [{ slug: 't1', title: 'T', goal: 'g' }], scriptOutput: setupOut(), scriptExit: 0 }, build: buildOk, fix: fixAll,
		verify: verifyResult({ needsLive: 0, failAt: 1, notGated: 'ref/gl/x.c' }), codex: codex(), publish: pubEcho },
	expect: ({ out, labels }, n) => ok(labels.includes('fix 1') && out && out.topics[0].status === 'RED', `${n}: the compile failure gets a fix round; still RED (${labels.join(',')})`),
});

await scenario('engine change without its baseline count', {
	script: { setup: { topics: [{ slug: 't1', title: 'T', goal: 'g' }], scriptOutput: setupOut(), scriptExit: 0 }, build: buildOk, fix: fixAll,
		verify: verifyResult({ needsLive: 0, missing: 'engine-expect-fail' }), codex: codex(), publish: pubEcho },
	expect: ({ out, labels }, n) => {
		ok(!labels.some(l => l.startsWith('fix')), `${n}: no fix round for configuration (${labels.join(',')})`);
		ok(out && out.topics[0].status === 'RED' && out.topics[0].why.some(w => /cannot be gated/.test(w)), `${n}: RED with the reason`);
	},
});

await scenario('fast-gates exits 2', {
	script: { setup: { topics: [{ slug: 't1', title: 'T', goal: 'g' }], scriptOutput: setupOut(), scriptExit: 0 }, build: buildOk, fix: fixAll,
		verify: verifyResult({ needsLive: 0, gates: [], gateExit: 2 }), codex: codex(), publish: pubEcho },
	expect: ({ out, labels }, n) => ok(!labels.some(l => l.startsWith('fix')) && out && out.topics[0].status === 'RED', `${n}: RED, no fix round (${labels.join(',')})`),
});

await scenario('machine failure on a look, then a good look after a review fix', {
	args: { live: LIVE },
	script: { setup: { topics: [{ slug: 't1', title: 'T', goal: 'g' }], scriptOutput: setupOut(), scriptExit: 0 }, build: buildOk, fix: fixAll,
		verify: i => (i === 1 ? verifyResult({ xbe: H('a'), live: { exit: 2, text: 'error=port busy\n' } }) : verifyResult({ xbe: H('c'), live: { text: liveText({ xbe: H('c') }) } })),
		codex: i => (i === 1 ? codex([HIGH]) : codex()), publish: pubEcho },
	expect: ({ out }, n) => ok(out && out.topics[0].status === 'GREEN', `${n}: GREEN - the earlier machine failure does not outlive the round (${out && out.topics[0].why})`),
});

await scenario('publish re-checks what the verdict consumed', {
	args: { live: LIVE },
	script: { setup: { topics: [{ slug: 't1', title: 'T', goal: 'g' }], scriptOutput: setupOut(), scriptExit: 0 }, build: buildOk,
		verify: verifyResult({ xbe: H('a'), live: { text: liveText({ xbe: H('a') }) } }), codex: codex(), publish: pubEcho },
	expect: ({ pub }, n) => {
		ok(pub && /needs_live/.test(pub.text) && /not_gated/.test(pub.text) && /missing/.test(pub.text), `${n}: meta re-derived and compared`);
		ok(pub && /verify-1\/live/.test(pub.text) && /LIVE-LOOK-OK/.test(pub.text) && /val ppmdiff/.test(pub.text), `${n}: the look's own result.txt re-read`);
		ok(pub && /codex-1\.txt/.test(pub.text) && /codex_exit=0/.test(pub.text) && /codex thread: th/.test(pub.text), `${n}: the counted codex file re-read`);
		ok(pub && /gh pr create[^\n]*\|\|/.test(pub.text), `${n}: a failed pr create stops the publish`);
	},
});

await scenario('publish-time check fails', {
	script: { setup: { topics: [{ slug: 't1', title: 'T', goal: 'g' }], scriptOutput: setupOut(), scriptExit: 0 }, build: buildOk,
		verify: verifyResult({ needsLive: 0 }), codex: codex(), publish: pubOk(false) },
	expect: ({ out }, n) => ok(out && out.topics[0].status === 'RED' && out.topics[0].isDraft === true, `${n}: the script's RED wins over the computed GREEN`),
});

await scenario('setup fails', {
	script: { setup: { topics: [{ slug: 't1', title: 'T', goal: 'g' }], scriptOutput: 'setup: env-proof compile failed', scriptExit: 1 } },
	expect: ({ err, labels }, n) => ok(err && labels.join(',') === 'setup', `${n}: the run stops before Build`),
});

// ---- 4. generated scripts parse
{
	const logs = [];
	const out = await wf(async () => null, fns => Promise.all(fns.map(f => f())), null, () => {}, m => logs.push(m), { dryRun: true, live: LIVE, baselines: { engineSweepExpectFail: 29 } }, { spent: () => 0 }, null);
	ok(out && out.dryRun && Object.keys(out.scripts).length === 5, 'dry run returns the five generated scripts');
	const bash = spawnSync('bash', ['--version']);
	if (bash.status === 0) {
		const dir = mkdtempSync(join(tmpdir(), 'fast-loop-'));
		for (const [k, s] of Object.entries(out.scripts)) {
			const f = join(dir, `${k}.sh`); writeFileSync(f, s);
			const r = spawnSync('bash', ['-n', f], { encoding: 'utf8' });
			ok(r.status === 0, `generated ${k} script passes bash -n${r.status ? `: ${r.stderr}` : ''}`);
		}
	} else console.log('skip: no bash for the bash -n checks');
}

console.log(fails ? `fast-loop: ${fails} check(s) FAILED` : 'fast-loop: all checks passed');
process.exit(fails ? 1 : 0);
