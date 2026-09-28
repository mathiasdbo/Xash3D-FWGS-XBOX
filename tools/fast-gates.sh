#!/usr/bin/env bash
# fast-gates.sh - compute the gate plan for a branch FROM ITS OWN DIFF and
# run it through tools/gate-chain.sh. The fast loop's single verification
# command (docs/r3d/fast-loop.md): the implementer's self-gate and the
# verify stage run this same script, one level apart.
#
#   tools/fast-gates.sh <cheap|full|plan> --base <sha> --out <dir>
#       [--xbe <path>] [--engine-expect-fail N]
#       [--mainui-expect-fail N --mainui-expect-total N]
#       [--files <list>]      (plan only: classify this list instead of the diff)
#   tools/fast-gates.sh --self-test
#
# Why the plan is computed here and not in the workflow script: in the v2
# agentic loop the script computed the plan from a file list an agent
# returned, which then needed a changed-files row to prove the list was
# the real diff (Codex review, 2026-09-26), and a plan text pasted into
# every prompt. Here the list IS `git diff --name-only <base>...HEAD` plus
# anything uncommitted, read by the same process that runs the gates, so
# there is no list to forge and nothing to paste.
#
# Tiers:
#   cheap  seconds to a couple of minutes: tool self-tests, host tests,
#          python tests, asset-studio, a compile of each changed engine .c
#          file, and the ten-second ref isolate for a changed renderer.
#          The implementer runs this before handing off.
#   full   the cheap tier minus what the link below repeats (it re-runs
#          both ref isolates, and the sweep recompiles every engine file),
#          then the engine sweep and the full hl link with --xbe. With
#          .sweep/cc-cache-dir set (tools/xbox-cc-cache.sh), every unit the
#          branch did not change is a cache copy of origin/main's build.
#   plan   write the plan and its meta, run nothing.
#
# Output, in --out: plan.tsv (gate-chain's format), files.txt (the diff),
# meta.txt (key=value: tier, base, head, needs_live, xbe, missing=...,
# not_gated=...), and gate-chain's results.tsv/results.json/attestation.txt
# and per-gate logs. meta.txt's `missing` names a baseline the plan needed
# and was not given (an engine change without --engine-expect-fail): the
# plan still runs, but the caller must not call it green - a sweep with no
# expected count exits 0 on any number of failures. `not_gated` names code
# this plan has no check for (ref/gl is PC-only, other platforms'
# backends): the caller must not call it green either.
#
# Exit: gate-chain's own (0 all ran clean, 1 a gate failed, 2 plan
# unwritable), or 0 after `plan`.
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"

# (/|$): a submodule pointer bump is listed as the bare path, with no
# trailing slash (found in review: a 3rdparty/mainui bump got no sweep).
ENGINE_DIRS='^(engine|common|public|filesystem|compat|3rdparty/library_suffix)(/|$)'
XBOX_CODE='^((engine|common|public|filesystem|compat)(/|$)|3rdparty/|ref/(soft|nv2a|null|common)(/|$)|deps/hlsdk(/|$))'
BUILD_INFRA='^(tools/xbox-[A-Za-z0-9_-]+\.sh|tools/gate-chain\.sh|tools/host-tests/run\.sh|patches/|wscript$|scripts/waifulib/)'
HOST_TESTED='^(ref/nv2a/|compat/xbox/|tools/host-tests/)'
DOCS_ONLY='^(docs/|CHANGELOG\.md$|README\.md$|AGENTS\.md$|\.claude/agents/|[^/]+\.md$)'
RENDERER='^ref/(nv2a|soft|common)/|^engine/client/'
SELF_TESTED='^tools/(gate-chain|xbox-cc-cache|live-look|fast-gates)\.sh$'

self_test() {
	local T fail=0 out
	T="$(mktemp -d)" || exit 2
	check() { # <name> <files...> -- <expected gates, space-separated> [-- <meta regex>]
		local name="$1"; shift
		local files=() want="" meta=""
		while [ $# -gt 0 ] && [ "$1" != "--" ]; do files+=( "$1" ); shift; done
		shift; want="$1"; shift
		[ $# -gt 0 ] && { shift; meta="$1"; }
		printf '%s\n' ${files[@]+"${files[@]}"} > "$T/f.txt"
		for tier in cheap full; do
			bash "$0" plan --tier "$tier" --base 0000000 --out "$T/$name-$tier" --files "$T/f.txt" --xbe .sweep/x/default.xbe > /dev/null 2>&1 \
				|| { echo "self-test: $name: plan mode failed" >&2; fail=1; }
		done
		out="$(cut -f1 "$T/$name-full/plan.tsv" | tr '\n' ' ' | sed 's/ $//')"
		[ "$out" = "$want" ] || { echo "self-test: $name: full plan is [$out], expected [$want]" >&2; fail=1; }
		if [ -n "$meta" ] && ! grep -Eq "$meta" "$T/$name-full/meta.txt"; then
			echo "self-test: $name: meta.txt lacks /$meta/:" >&2; cat "$T/$name-full/meta.txt" >&2; fail=1
		fi
	}
	check docs docs/r3d/x.md CHANGELOG.md -- "" -- '^needs_live=0$'
	check nv2a ref/nv2a/r_context.c -- "host-tests engine-sweep game-link-check" -- '^needs_live=1$'
	check engine-nobase engine/client/cl_main.c -- "engine-sweep game-link-check" -- '^missing=.*engine-expect-fail'
	check refgl ref/gl/gl_rmain.c -- "" -- '^not_gated=ref/gl/gl_rmain.c$'
	check tool tools/live-look.sh -- "self-test-live-look"
	check cache tools/xbox-cc-cache.sh -- "self-test-xbox-cc-cache engine-sweep game-link-check"
	check py tools/xemu-debug.py -- "python-tests"
	# The cheap tier of a renderer change is the isolate, not the link.
	printf 'ref/nv2a/r_context.c\n' > "$T/f.txt"
	bash "$0" plan --tier cheap --base 0000000 --out "$T/c" --files "$T/f.txt" > /dev/null 2>&1
	[ "$(cut -f1 "$T/c/plan.tsv" | tr '\n' ' ')" = "host-tests ref-isolate-nv2a " ] || { echo "self-test: cheap nv2a plan is wrong: $(cut -f1 "$T/c/plan.tsv" | tr '\n' ' ')" >&2; fail=1; }
	# An engine .c change gets a per-file compile in the cheap tier.
	printf 'engine/common/host.c\n' > "$T/f.txt"
	bash "$0" plan --tier cheap --base 0000000 --out "$T/e" --files "$T/f.txt" --engine-expect-fail 29 > /dev/null 2>&1
	grep -q '^changed-compile	.*engine/common/host.c' "$T/e/plan.tsv" || { echo "self-test: cheap engine plan lacks changed-compile" >&2; fail=1; }
	bash "$0" plan --tier full --base 0000000 --out "$T/e2" --files "$T/f.txt" --engine-expect-fail 29 > /dev/null 2>&1
	grep -q 'xbox-sweep.sh --expect-fail 29 ' "$T/e2/plan.tsv" && grep -q '^missing=$' "$T/e2/meta.txt" \
		|| { echo "self-test: the engine baseline did not reach the sweep" >&2; fail=1; }
	# Run mode, against a real repository (found in review: plan mode with
	# --files alone let an unreachable empty-plan path and an unchecked
	# diff failure through).
	local R="$T/repo" rc
	# The scratch repository must not inherit the user's git config (a
	# global commit.gpgsign=true made every scratch commit fail - found in
	# review). --self-test exits this process, so the export ends with it.
	export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
	mkdir -p "$R/tools" "$R/docs" "$R/engine" "$T/subsrc" && cp "$0" "$R/tools/fast-gates.sh" && cp "$(dirname "$0")/gate-chain.sh" "$R/tools/" || fail=1
	( cd "$T/subsrc" && git init -q && echo a > f && git add f && git -c user.email=t@t -c user.name=t commit -qm s ) || fail=1
	( cd "$R" && git init -q && echo x > docs/a.md && echo 'int x;' > engine/x.c && git add . \
		&& git -c protocol.file.allow=always submodule add -q "$T/subsrc" sub > /dev/null 2>&1 \
		&& git -c user.email=t@t -c user.name=t commit -qm base && git tag base \
		&& echo y >> docs/a.md && git -c user.email=t@t -c user.name=t commit -qam docs ) || { echo "self-test: could not build the scratch repo" >&2; fail=1; }
	echo dirty >> "$R/sub/f"   # a patched submodule working tree, as xbox-cc.sh leaves one
	bash "$R/tools/fast-gates.sh" full --base base --out "$T/run1" > "$T/run1.log" 2>&1; rc=$?
	[ "$rc" -eq 0 ] && [ ! -s "$T/run1/plan.tsv" ] && grep -qx 'docs/a.md' "$T/run1/files.txt" \
		|| { echo "self-test: a docs-only diff with a dirty submodule must exit 0 with an empty plan (exit $rc)" >&2; cat "$T/run1.log" >&2; fail=1; }
	bash "$R/tools/fast-gates.sh" full --base no-such-ref --out "$T/run2" > /dev/null 2>&1; rc=$?
	[ "$rc" -eq 2 ] || { echo "self-test: an unreadable diff must exit 2, got $rc" >&2; fail=1; }
	( cd "$R" && git mv engine/x.c docs/x.c && git -c user.email=t@t -c user.name=t commit -qm mv )
	bash "$R/tools/fast-gates.sh" plan --tier full --base base --out "$T/run3" > /dev/null 2>&1
	grep -qx 'engine/x.c' "$T/run3/files.txt" && grep -q '^engine-sweep' "$T/run3/plan.tsv" \
		|| { echo "self-test: moving a file out of engine/ must still gate the engine" >&2; fail=1; }
	printf '3rdparty/mainui\n' > "$T/f.txt"
	bash "$0" plan --tier full --base 0000000 --out "$T/sm" --files "$T/f.txt" > /dev/null 2>&1
	grep -q '^missing=.*mainui' "$T/sm/meta.txt" || { echo "self-test: a bare submodule pointer bump must be classified" >&2; fail=1; }
	rm -rf "$T"
	[ "$fail" -eq 0 ] && echo "fast-gates: self-test passed" && exit 0
	exit 1
}
[ "${1:-}" = "--self-test" ] && self_test

TIER="${1:-}"; shift || true
BASE="" OUT="" XBE="" ENGINE_EXPECT="" MAINUI_FAIL="" MAINUI_TOTAL="" FILES_IN=""
[ "$TIER" = "plan" ] && TIER_OF_PLAN="full" || TIER_OF_PLAN="$TIER"
while [ $# -gt 0 ]; do
	case "$1" in
		--base) BASE="$2"; shift 2 ;;
		--out) OUT="$2"; shift 2 ;;
		--xbe) XBE="$2"; shift 2 ;;
		--engine-expect-fail) ENGINE_EXPECT="$2"; shift 2 ;;
		--mainui-expect-fail) MAINUI_FAIL="$2"; shift 2 ;;
		--mainui-expect-total) MAINUI_TOTAL="$2"; shift 2 ;;
		--files) FILES_IN="$2"; shift 2 ;;
		--tier) TIER_OF_PLAN="$2"; shift 2 ;;   # plan only: which tier to write
		*) echo "fast-gates: unknown argument $1" >&2; exit 2 ;;
	esac
done
case "$TIER" in cheap|full|plan) ;; *) echo "usage: fast-gates.sh <cheap|full|plan> --base <sha> --out <dir> ..." >&2; exit 2 ;; esac
[ -n "$BASE" ] && [ -n "$OUT" ] || { echo "fast-gates: --base and --out are required" >&2; exit 2; }
[ -z "$FILES_IN" ] || [ "$TIER" = "plan" ] || { echo "fast-gates: --files is for plan mode only - a run gates the real diff" >&2; exit 2; }

cd "$REPO" || exit 2
mkdir -p "$OUT" || exit 2
# The full tier's attestation names HEAD: gates run on anything else would
# be attested as a commit they never saw. Dirty submodule WORKING TREES are
# not a change: tools/xbox-cc.sh patches 3rdparty/library_suffix and
# 3rdparty/opusfile on every compile, from tracked patches/ (found in
# review: without --ignore-submodules=dirty the full tier refused forever
# once anything had compiled). A submodule POINTER change still shows.
status() { git -c core.quotePath=false status --porcelain --untracked-files=all --ignore-submodules=dirty "$@"; }
if [ "$TIER" = "full" ] && [ -n "$(status)" ]; then
	echo "fast-gates: the full tier runs on a committed tree only - commit or remove:" >&2
	status >&2
	exit 2
fi
if [ -n "$FILES_IN" ]; then
	sort -u "$FILES_IN" | sed '/^$/d' > "$OUT/files.txt"
else
	# The diff on its own, exit checked: in a pipeline its failure (a bad
	# base) left an empty list - an empty plan, "green", having measured
	# nothing (found in review). --no-renames lists a moved file's old path
	# too, and porcelain's "old -> new" is split into both.
	git -c core.quotePath=false diff --no-renames --name-only "$BASE"...HEAD > "$OUT/files.raw" \
		|| { echo "fast-gates: cannot read the diff against $BASE" >&2; exit 2; }
	# -z: porcelain v1 quotes a path with a space even with quotePath off,
	# and the quoted form matches no pattern. In -z form a rename is two
	# entries, new then old; both are kept. Captured to a file first, so a
	# failing status is not lost in the pipeline (found in review).
	status -z > "$OUT/status.raw" || { echo "fast-gates: cannot read git status" >&2; exit 2; }
	while IFS= read -r -d '' e; do
		printf '%s\n' "${e:3}"
		case "${e:0:1}" in R|C) IFS= read -r -d '' o && printf '%s\n' "$o" ;; esac
	done < "$OUT/status.raw" >> "$OUT/files.raw"
	sort -u "$OUT/files.raw" | sed '/^$/d' > "$OUT/files.txt"
fi

touched() { grep -Eq "$1" "$OUT/files.txt"; }
matching() { grep -E "$1" "$OUT/files.txt"; }
q() { printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"; }

GATES=()   # "name<TAB>command"
add() { GATES+=( "$(printf '%s\t%s' "$1" "$2")" ); }
MISSING=()

# Cheap: order is cost, so a ten-second failure is found before a minute.
[ -n "$(matching '^(\.claude/workflows/fast-loop\.js|tools/test_fast_loop\.mjs)$')" ] && add fast-loop-self-test 'node tools/test_fast_loop.mjs'
[ -n "$(matching '^(\.claude/workflows/agentic-loop\.js|tools/test_agentic_loop\.mjs)$')" ] && add loop-self-test 'node tools/test_agentic_loop.mjs'
while IFS= read -r t; do
	[ -n "$t" ] && add "self-test-$(basename "$t" .sh)" "bash $t --self-test"
done < <(matching "$SELF_TESTED")
touched "$HOST_TESTED" && add host-tests 'tools/host-tests/run.sh'
touched '^tools/[^/]+\.py$' && add python-tests '"$(command -v python || command -v python3)" -m unittest discover -s tools -p "test_*.py"'
if touched '^tools/asset-studio/'; then
	add asset-studio-configure '[ -f tools/asset-studio/build/CMakeCache.txt ] || cmake -S tools/asset-studio -B tools/asset-studio/build'
	add asset-studio-build 'cmake --build tools/asset-studio/build --config Release'
	add asset-studio-ctest 'ctest --test-dir tools/asset-studio/build -C Release --output-on-failure'
fi

XBOX=0
{ touched "$XBOX_CODE" || touched "$BUILD_INFRA"; } && XBOX=1
if [ "$XBOX" = 1 ] && [ "$TIER_OF_PLAN" = "cheap" ]; then
	cc=""
	while IFS= read -r f; do
		[ -f "$f" ] || [ -n "$FILES_IN" ] || continue   # a deleted file has nothing to compile
		cc="$cc tools/xbox-cc.sh $(q "$f") -o .sweep/fast-gates-probe.obj &&"
	done < <(matching "$ENGINE_DIRS" | grep -E '\.c$')
	[ -n "$cc" ] && add changed-compile "mkdir -p .sweep &&${cc} true"
	for m in soft nv2a; do
		touched "^ref/($m|common)/" && add "ref-isolate-$m" "tools/xbox-ref-isolate.sh $m"
	done
fi
if [ "$XBOX" = 1 ] && [ "$TIER_OF_PLAN" = "full" ]; then
	expect=""
	if touched "$ENGINE_DIRS"; then
		if [ -n "$ENGINE_EXPECT" ]; then expect="--expect-fail $ENGINE_EXPECT "
		else MISSING+=( "engine-expect-fail (an engine file changed; the tracked failing count from CHANGELOG.md)" ); fi
	fi
	# Without an engine change the sweep is the link's object producer, not
	# a regression gate: link-check refuses to link without its manifest.
	add engine-sweep "tools/xbox-sweep.sh ${expect}engine filesystem public common compat 3rdparty/library_suffix/src"
	if touched '^3rdparty/mainui(/|$)'; then
		if [ -n "$MAINUI_FAIL" ] && [ -n "$MAINUI_TOTAL" ]; then
			add mainui-sweep "tools/xbox-mainui-sweep.sh --expect-fail $MAINUI_FAIL --expect-total $MAINUI_TOTAL"
		else MISSING+=( "mainui-expect-fail/total (3rdparty/mainui changed)" ); fi
	fi
	X="${XBE:-.sweep/fast-gates/default.xbe}"
	# The sidecar goes first so a failed relink cannot leave a stale one.
	add game-link-check "mkdir -p $(q "$(dirname "$X")") && rm -f $(q "$X.identity") && tools/xbox-game-link-check.sh hl --expect-undefined 0 --include-client --include-freevgui --xbe $(q "$X")"
fi

# Code no gate here covers. Tool files are covered by their own tests or
# by the build they drive; loop files by their tests.
COVERED="$XBOX_CODE|$BUILD_INFRA|$HOST_TESTED|$SELF_TESTED|^tools/[^/]+\.py$|^tools/asset-studio/|^\.claude/workflows/(fast|agentic)-loop\.js$|^tools/test_(fast|agentic)_loop\.mjs$"
NOT_GATED="$(grep -Ev "$DOCS_ONLY" "$OUT/files.txt" | grep -Ev "$COVERED" | tr '\n' ' ' | sed 's/ $//')"

# Zero gates must be a zero-byte plan: printf with no arguments still writes
# one newline, which made the empty-plan path below unreachable and every
# docs-only diff exit 2 (found in review).
if [ "${#GATES[@]}" -gt 0 ]; then printf '%s\n' "${GATES[@]}" > "$OUT/plan.tsv"; else : > "$OUT/plan.tsv"; fi
LIVE=0
[ "$XBOX" = 1 ] && touched "$RENDERER" && LIVE=1
{
	echo "tier=$TIER_OF_PLAN"
	echo "base=$BASE"
	echo "head=$(git rev-parse HEAD 2>/dev/null || echo unknown)"
	echo "files=$(wc -l < "$OUT/files.txt" | tr -d ' ')"
	echo "needs_live=$LIVE"
	echo "xbe=$([ "$XBOX" = 1 ] && [ "$TIER_OF_PLAN" = full ] && echo "${XBE:-.sweep/fast-gates/default.xbe}")"
	echo "missing=$(printf '%s; ' ${MISSING[@]+"${MISSING[@]}"} | sed 's/; $//')"
	echo "not_gated=$NOT_GATED"
	# The cheap tier compiles changed engine .c files and isolates a changed
	# renderer; any other Xbox path (a header, C++, ref/null, the SDK, build
	# infrastructure) is compiled first by the full tier. Informational.
	[ "$TIER_OF_PLAN" = cheap ] && echo "cheap_unchecked=$(matching "$XBOX_CODE|$BUILD_INFRA" | grep -Ev "$ENGINE_DIRS.*\.c$|^ref/(soft|nv2a|common)/" | tr '\n' ' ' | sed 's/ $//')"
} > "$OUT/meta.txt"

[ "$TIER" = "plan" ] && { cat "$OUT/plan.tsv" "$OUT/meta.txt"; exit 0; }
if [ ! -s "$OUT/plan.tsv" ]; then
	# Nothing to run (docs only, or only ungated code): gate-chain refuses
	# an empty plan, which is right for it and wrong here - say so plainly.
	echo "fast-gates: empty plan - nothing to run for this diff (see $OUT/meta.txt)."
	: > "$OUT/results.tsv"
	rm -f "$OUT/attestation.txt"
	exit 0
fi
bash tools/gate-chain.sh "$OUT/plan.tsv" "$OUT"
