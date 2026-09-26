#!/usr/bin/env node
// test_agentic_loop.mjs - checks for .claude/workflows/agentic-loop.js that
// need no Workflow runtime, no agent and no Xbox toolchain:
//
//   node tools/test_agentic_loop.mjs
//
// 1. The script parses the way the Workflow runtime loads it (the body is
//    an async function, so a top-level `return` is legal; `export const
//    meta` must be a pure literal; every phase() title must be in meta).
// 2. Its pure decision functions - the gate plan from changed paths, the
//    judgement of a gate table by name, the cheap-gate judgement, the
//    environment plan, the specialist policy, and the record renderer -
//    are run through fixtures. These are the functions that replaced a
//    model-reported "green" (docs/r3d/lessons.md: NEVER let the agent
//    that ran a check decide whether it passed); if one of them could
//    judge green with nothing having run, this is where it shows.
//
// Exit 0 only if every check passes. Run it after any edit to the script;
// then a dry run (args.dryRun) through the Workflow tool with
// scriptPath pointing at the file - the named-workflow registry can serve
// a cached copy of an edited script.
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const path = process.argv[2] || join(here, '..', '.claude', 'workflows', 'agentic-loop.js');
const src = readFileSync(path, 'utf8');
let fails = 0;
const ok = (c, m) => { if (!c) { fails++; console.log('FAIL:', m); } else console.log('ok:  ', m); };

// ---- 1. parse as the runtime does
if (!/^\s*(\/\/[^\n]*\n|\s)*export const meta = \{/.test(src)) { console.log('FAIL: script must begin with `export const meta = {`'); process.exit(1); }
const body = src.replace(/^export const meta/m, 'const meta');
const AsyncFunction = (async () => {}).constructor;
try { new AsyncFunction('agent', 'parallel', 'pipeline', 'phase', 'log', 'args', 'budget', 'workflow', body); ok(true, 'parses as a workflow body'); }
catch (e) { console.log('FAIL: parse:', e.message); process.exit(1); }
const metaSrc = body.match(/const meta = (\{[\s\S]*?\n\})/);
const meta = new Function('return ' + metaSrc[1])();
ok(!!meta.name && !!meta.description, 'meta has name and description');
const phaseCalls = [...body.matchAll(/phase\('([^']+)'\)/g)].map(x => x[1]);
const metaPhases = (meta.phases || []).map(p => p.title);
ok(phaseCalls.every(p => metaPhases.includes(p)), `every phase() title is in meta.phases (${[...new Set(phaseCalls)].join(', ')})`);

// ---- 2. pure functions, sliced out by name
function fn(name) { const i = src.indexOf(`function ${name}(`); if (i < 0) throw new Error('no ' + name); return src.slice(i, src.indexOf('\n}\n', i) + 3); }
function konst(name) { const m = src.match(new RegExp(`^const ${name} = .*$`, 'm')); if (!m) throw new Error('no const ' + name); return m[0]; }
const specialists = src.slice(src.indexOf('const SPECIALISTS = ['), src.indexOf(']', src.indexOf('const SPECIALISTS = [')) + 1);
const domain = src.slice(src.indexOf('const DOMAIN_SPECIALISTS = ['), src.indexOf('\n]\n', src.indexOf('const DOMAIN_SPECIALISTS = [')) + 3);
const lib = [
  konst('ENGINE_DIRS'), konst('REF_ISOLATED'), konst('XBOX_CODE'), konst('HOST_TESTED'), konst('DOCS_ONLY'), konst('BUILD_INFRA'), konst('LOOP_SELF'), konst('MSYS'),
  specialists, konst('REFEREE_SPECIALISTS'), domain, konst('DESIGN_NOTE'),
  'let baselines = BASELINES; let mode = MODE; let specialistsPolicy = POLICY;',
  fn('gatePlan'), fn('judgeGates'), fn('judgeCheap'), fn('actionable'), fn('envPlan'), fn('designNotesOf'), fn('specialistsFor'), fn('md'), fn('renderRecord'),
  'return { gatePlan, judgeGates, judgeCheap, actionable, envPlan, designNotesOf, specialistsFor, renderRecord };',
].join('\n');
const make = (B, M, P) => new Function('BASELINES', 'MODE', 'POLICY', lib)(B, M, P);
const L = make({ engineSweepExpectFail: 29 }, 'slice', 'auto');
const ctx = { slug: 't', nxdkDir: '/c/main/deps/nxdk' };

let p = L.gatePlan(['docs/r3d/x.md', 'CHANGELOG.md'], ctx);
ok(p.gates.length === 0 && p.docsOnly && !p.needsLive, 'docs-only: no gates, no live');
p = L.gatePlan(['ref/nv2a/r_studio.c'], ctx);
ok(p.gates.map(g => g.name).join(',') === 'host-tests,ref-isolate-nv2a,engine-sweep,game-link-check', 'ref/nv2a: host tests and the ten-second isolate before the sweep, then the game link');
ok(!p.gates.find(g => g.name === 'engine-sweep').command.includes('--expect-fail'), 'ref-only change: the sweep is an object producer, no baseline needed');
ok(p.cheap.map(g => g.name).join(',') === 'host-tests,ref-isolate-nv2a', 'cheap subset is host tests + isolate');
ok(p.gates.every(g => !/msys64/.test(g.command)), 'gate commands are bare invocations, never through /c/msys64/usr/bin/bash');
ok(p.artifacts.find(a => a.gate === 'game-link-check').artifact.endsWith('.identity') && /^rm -f .*\.identity && /.test(p.gates.find(g => g.name === 'game-link-check').command.replace(/^NXDK_DIR='[^']*' /, '')), 'the link gate removes a stale .identity before linking and names the new one for a content check');
ok(!p.artifacts.some(a => a.gate === 'host-tests' || a.gate === 'engine-sweep'), 'host tests and the sweep name no artifact: their files exist after a failure too');
ok(p.uncovered.length === 0, 'ref/nv2a is covered');
p = L.gatePlan(['tools/xbox-sweep.sh', 'patches/opusfile-xbox-stream.patch'], ctx);
ok(p.gates.some(g => g.name === 'engine-sweep') && p.gates.some(g => g.name === 'game-link-check') && p.needsLive, 'build infrastructure is Xbox code: the full chain, never docs-only');
p = L.gatePlan(['tools/gate-chain.sh', '.claude/workflows/agentic-loop.js'], ctx);
ok(p.gates.map(g => g.name).join(',') === 'loop-self-test,gate-chain-self-test,engine-sweep,game-link-check' && p.uncovered.length === 0, 'the loop and the runner run their own tests; the runner is build infrastructure');
p = L.gatePlan(['ref/gl/gl_rmain.c'], ctx);
ok(p.gates.length === 0 && !p.docsOnly && p.uncovered.join(',') === 'ref/gl/gl_rmain.c', 'ref/gl has no gate here and is reported as NOT GATED, not as docs (the chain then blocks the PR: uncovered code is never GREEN on codex alone)');
p = L.gatePlan(['engine/client/cl_view.c'], ctx);
ok(p.gates.find(g => g.name === 'engine-sweep').command.includes('--expect-fail 29'), 'an engine change uses the tracked sweep baseline');
const L0 = make({}, 'slice', 'auto');
p = L0.gatePlan(['engine/client/cl_view.c'], ctx);
ok(p.missing.length === 1 && /engineSweepExpectFail/.test(p.missing[0]), 'an engine change without the baseline is UNCONFIGURED, not silently run');
ok(!L0.judgeGates(p, p.gates.map(g => ({ gate: g.name, status: 'ran', exit: 0, log: 'x' })), null, false).green, 'an unconfigured plan never judges green, even with a green table');
p = L.gatePlan(['tools/asset-studio/src/a.cpp', 'tools/xemu-debug.py'], ctx);
ok(p.gates.map(g => g.name).join(',') === 'python-tests,asset-studio-configure,asset-studio-build,asset-studio-ctest' && !p.needsLive, 'baker + python tool: cmake trio and unittest, no live');
ok(L.gatePlan(['ref\\nv2a\\r_image.c'], ctx).gates.some(g => g.name === 'ref-isolate-nv2a'), 'backslash paths are normalised');

p = L.gatePlan(['ref/nv2a/r_studio.c'], ctx);
const full = p.gates.map(g => ({ gate: g.name, status: 'ran', exit: 0, log: '/l/' + g.name + '.log' }));
const filesRow = { gate: 'changed-files', status: 'ran', exit: 0, log: '/l/changed-files.log' };
const att = (names) => `GATE-CHAIN-OK\nplan-sha256=${'a'.repeat(64)}\nresults-sha256=${'b'.repeat(64)}\nhead=abc\ngates=${names.join(',')}\n`;
const A = att(['changed-files', ...p.gates.map(g => g.name)]);
ok(L.judgeGates(p, [filesRow, ...full], A, true).green, 'every planned gate ran/0 with a log, the changed-files row green and an attestation naming them all -> green');
ok(!L.judgeGates(p, full, A, true).green, 'no changed-files row when one is required -> red');
ok(!L.judgeGates(p, [filesRow, ...full], null, true).green, 'a green table with no attestation -> red (the runner only writes one on all-green)');
ok(!L.judgeGates(p, [filesRow, ...full], att(['changed-files', 'host-tests']), true).green, 'an attestation not naming every planned gate -> red');
ok(!L.judgeGates(p, [filesRow, ...full], 'GATE-CHAIN-OK\nplan-sha256=short\n', true).green, 'an attestation without full digests -> red');
ok(!L.judgeGates(p, [filesRow, ...full.slice(0, 3)], A, true).green, 'a planned gate missing from the table -> red');
ok(!L.judgeGates(p, [filesRow, ...full.map((r, i) => i === 3 ? { ...r, status: 'not-run', exit: null, log: null } : r)], A, true).green, 'a not-run row -> red');
ok(!L.judgeGates(p, [filesRow, ...full.map((r, i) => i === 1 ? { ...r, exit: 1 } : r)], A, true).green, 'exit 1 -> red');
ok(!L.judgeGates(p, [filesRow, ...full.map((r, i) => i === 0 ? { ...r, log: null } : r)], A, true).green, 'ran/0 without a log -> red');
ok(!L.judgeGates(p, null, A, true).green && !L.judgeGates(p, undefined, A, true).green && !L.judgeGates(p, [], A, true).green, 'no table -> red');
ok(L.judgeGates(L.gatePlan(['docs/a.md'], ctx), undefined, null, true).green, 'a docs-only plan with no table is trivially green: nothing to build');
ok(L.judgeGates(p, [filesRow, ...full, { gate: 'extra', status: 'ran', exit: 0, log: 'x' }], A, true).green, 'an extra row is ignored; only the plan counts');
const ep0 = L.envPlan(null); const envRows = ep0.gates.map(g => ({ gate: g.name, status: 'ran', exit: 0, log: 'l' }));
ok(L.judgeGates(ep0, envRows, att(ep0.gates.map(g => g.name)), false).green && !L.judgeGates(ep0, envRows, null, false).green, 'the environment plan needs its attestation but no changed-files row');
ok(L.judgeCheap(p, full.slice(0, 1)).red.join(',') === 'ref-isolate-nv2a', 'the cheap judge names the missing cheap gate');
ok(L.judgeCheap(p, full).green, 'the cheap judge is green when the cheap rows are');
ok(L.actionable([{ severity: 'low', confidence: 0.2 }, { severity: 'high', confidence: 0.1 }, { severity: 'low', confidence: 0.6 }]).length === 2, 'actionable = critical/high or confidence >= 0.6 (AGENTS.md)');
ok(L.envPlan(null).gates.length === 5, 'environment plan without a live look: 5 gates');
const ep = L.envPlan({ xemuExe: 'x', hlSteamDir: '/d/hl', mpakPath: '/m.mpak' });
ok(ep.gates.length === 12 && ep.gates.at(-1).name === 'env-baseline-exe', 'environment plan with a live look: 12 gates ending in the baseline exe check');
ok(ep.gates.find(g => g.name === 'env-baseline-link').command.includes('$(git rev-parse origin/main)') && ep.gates.find(g => g.name === 'env-baseline-link').command.includes('dirname "$(dirname "$(cat .sweep/nxdk-dir)")"'), 'the baseline lives in the main checkout, keyed by the fork point');
ok(ep.gates.every(g => !/msys64/.test(g.command)), 'environment commands are bare invocations too');

const t = { specialists: ['nv2a-xbox-specialist', 'memory-budget-specialist'], designNotes: ['docs/r3d/design/x.md'], predictedFiles: ['ref/nv2a/a.c'] };
ok(L.specialistsFor(t).length === 0, 'a design note on disk -> no specialists');
ok(L.specialistsFor({ ...t, designNotes: ['docs/r3d/fork-plan.md'] }).join(',') === 'nv2a-xbox-specialist', 'a fork-plan line is not a note -> the domain specialist only (slice)');
ok(make({}, 'explore', 'auto').specialistsFor({ ...t, designNotes: [] }).join(',') === 'nv2a-xbox-specialist,memory-budget-specialist', 'explore without a note keeps flagged + domain');
ok(make({}, 'slice', 'none').specialistsFor(t).length === 0 && make({}, 'slice', 'flagged').specialistsFor(t).length === 2, 'policy none / flagged override auto');

const sum = { runId: 'r', prompt: 'p', mode: 'slice', sliceIds: ['X9-X'], specialistsPolicy: 'auto', baseSha: 'abc', startedAt: 's',
  environment: { gates: [{ gate: 'env-compile', status: 'ran', exit: 0, seconds: 3, log: 'l' }], green: true, failures: [] },
  agentCalls: 3, metrics: [{ stage: 'QA', label: 'qa', outputTokens: 123, ok: true }],
  specialists: [{ slug: 't', consulted: [], flagged: ['nv2a-xbox-specialist'], designNotes: ['docs/r3d/design/x.md'] }],
  topics: [{ slug: 't', title: 'T', finalStatus: 'GREEN', failed: false, gates: ['host-tests'], unconfigured: [], artifacts: [{ gate: 'host-tests', artifact: 'a' }],
    qaRounds: [{ round: 1, gatesGreen: true, failures: [], gates: [{ gate: 'host-tests', exit: 0, seconds: 2 }], codexRan: true, codexExit: 0, codexThread: 'th', actionable: 0 }],
    codexRoundsUsed: 1, reviewStatus: '1 of 2', deferred: [], unaddressed: [], optional: ['x | y'], liveStatus: 'verified',
    live: { sceneReady: { mapReady: true, framesAdvanced: 60 }, pixelDiff: { differing: 0, total: 307200, bbox: null }, screenshots: ['a.png'] },
    findings: [{ round: 1, title: 'f', severity: 'high', confidence: 0.9, file: 'x.c', line: 3 }], pr: { url: 'u', blocked: false }, designNoteWritten: null }] };
const rec = L.renderRecord(sum);
ok(rec.includes('| QA | qa | 123 | ok |') && rec.includes('| host-tests | 0 | 2 |') && rec.includes('x \\| y') && rec.includes('## Missed'), 'renderRecord emits the cost and gate rows, escapes pipes, and leaves the human sections');

console.log(fails ? `${fails} check(s) FAILED` : 'agentic-loop: all checks passed');
process.exit(fails ? 1 : 0);
