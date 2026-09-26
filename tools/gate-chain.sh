#!/usr/bin/env bash
# gate-chain.sh - run an ordered list of gate commands and record, for each
# one, whether it ran and what it exited with. Nothing else: no parsing of
# output, no judgement. The caller decides what the table means.
#
#   tools/gate-chain.sh [--base <sha> --expect-files <list>] <plan.tsv> <out-dir>
#   tools/gate-chain.sh --self-test
#
# --base <sha> --expect-files <list>: before any gate runs, an implicit
# first row "changed-files" compares the real change set - `git diff
# --name-only <sha>...HEAD`, plus anything uncommitted - with the sorted
# list in <list>, one path per line. A mismatch is exit 1 on that row and
# nothing else runs: a plan computed from a list an agent typed is only a
# plan for what the agent remembered to list (found in Codex review of
# the loop, 2026-09-26). Both lists are written to <out-dir> for the
# reader.
#
# attestation.txt: written in <out-dir> ONLY when every row ran and
# exited 0, and removed first every run. It binds the result to what
# was run: sha256 of the plan file, sha256 of results.tsv, the commit the
# gates ran on, and the sha256 of the changed-file list when one was
# checked. A later stage with a shell verifies it by content (recompute
# the digests, compare the commit); a table transcribed by a model
# cannot forge it without also forging files another agent will hash.
#
# <plan.tsv>: one gate per line, "<name><TAB><command>", run in order with
# `bash -c` from the repository root. Blank lines and lines starting with
# '#' are ignored. Names are filename-safe labels ("engine-sweep",
# "ref-isolate-nv2a").
#
# Output, in <out-dir>:
#   <name>.log     stdout+stderr of that gate, verbatim
#   results.tsv    "<name>\t<status>\t<exit>\t<seconds>\t<log>" per plan line,
#                  status = ran | not-run
#   results.json   the same table as JSON, for a caller that parses
#
# The chain STOPS at the first nonzero exit: later gates depend on earlier
# ones (a link needs the sweep's objects), so running them would report
# failures that are not their own. They are written as "not-run" with exit
# "-", never omitted - a gate missing from the table is indistinguishable
# from one that was never required.
#
# Exit status: 0 iff every plan line ran and exited 0. 1 if any gate exited
# nonzero (or was therefore not run). 2 if the plan is missing, empty, or
# the output directory cannot be written. There is no path to exit 0
# without a non-empty plan whose every line ran clean: docs/r3d/lessons.md,
# "A measuring tool that cannot report its own failure will eventually
# lie" - this file exists because the agentic loop's own QA stage
# reported GREEN on a diff whose build gates had never run (PR #148; see
# docs/r3d/reports/agentic-loop/). The loop now computes green from
# results.tsv, in the workflow script, from exit codes it required - a
# model no longer gets to say "green".
#
# --self-test runs a three-line plan (true, false, true) and an empty plan
# through this script's own logic and checks the table and the exit codes
# it produces. Run it on any machine (it needs only bash) before trusting
# a result from this script for the first time: lessons.md, "NEVER trust a
# new tool's exit code without testing its failure path too".
set -u

self_test() {
	local tmp plan out rc
	tmp="$(mktemp -d)" || { echo "gate-chain: mktemp failed" >&2; exit 2; }
	plan="$tmp/plan.tsv"; out="$tmp/out"
	printf '%s\t%s\n' "first" "true" "second" "false" "third" "true" > "$plan"
	"$0" "$plan" "$out" > /dev/null 2>&1; rc=$?
	local fail=0
	[ "$rc" -eq 1 ] || { echo "self-test: expected exit 1 for a failing plan, got $rc" >&2; fail=1; }
	[ -f "$out/results.tsv" ] || { echo "self-test: results.tsv missing" >&2; fail=1; }
	local expected
	expected="$(printf 'first\tran\t0\nsecond\tran\t1\nthird\tnot-run\t-\n')"
	local actual
	actual="$(cut -f1-3 "$out/results.tsv" 2>/dev/null)"
	[ "$actual" = "$expected" ] || { echo "self-test: results.tsv columns 1-3 differ from expected:" >&2; printf '%s\n' "$actual" >&2; fail=1; }
	[ -f "$out/first.log" ] && [ -f "$out/second.log" ] || { echo "self-test: per-gate logs missing" >&2; fail=1; }
	[ ! -e "$out/third.log" ] || { echo "self-test: a not-run gate must not have a log" >&2; fail=1; }
	grep -q '"status": "not-run"' "$out/results.json" 2>/dev/null || { echo "self-test: results.json lacks the not-run row" >&2; fail=1; }
	[ ! -e "$out/attestation.txt" ] || { echo "self-test: a failing plan must not be attested" >&2; fail=1; }
	# A clean plan exits 0, every row reads ran/0, and it is attested.
	printf '%s\t%s\n' "only" "true" > "$plan"
	"$0" "$plan" "$out" > /dev/null 2>&1; rc=$?
	[ "$rc" -eq 0 ] || { echo "self-test: expected exit 0 for a clean plan, got $rc" >&2; fail=1; }
	[ "$(cut -f1-3 "$out/results.tsv")" = "$(printf 'only\tran\t0')" ] || { echo "self-test: clean plan table wrong" >&2; fail=1; }
	grep -q '^GATE-CHAIN-OK$' "$out/attestation.txt" 2>/dev/null && grep -q "^plan-sha256=$(sha256sum "$plan" | cut -c1-64)$" "$out/attestation.txt" && grep -q "^results-sha256=$(sha256sum "$out/results.tsv" | cut -c1-64)$" "$out/attestation.txt" || { echo "self-test: attestation missing or its digests do not match the files" >&2; fail=1; }
	# --base/--expect-files: a list that is not the real change set is a red first row and nothing else runs.
	if git -C "$(dirname "$0")/.." rev-parse HEAD > /dev/null 2>&1; then
		printf '%s\n' "this/path/was/never/changed.c" > "$tmp/files.txt"
		"$0" --base HEAD --expect-files "$tmp/files.txt" "$plan" "$out" > /dev/null 2>&1; rc=$?
		[ "$rc" -eq 1 ] && [ "$(cut -f1-3 "$out/results.tsv" | head -1)" = "$(printf 'changed-files\tran\t1')" ] && [ "$(cut -f2 "$out/results.tsv" | tail -1)" = "not-run" ] || { echo "self-test: a wrong file list must be a red changed-files row that stops the chain (rc=$rc)" >&2; fail=1; }
		[ ! -e "$out/attestation.txt" ] || { echo "self-test: a wrong file list must not be attested" >&2; fail=1; }
		: > "$tmp/files.txt"
		if [ -z "$(git -C "$(dirname "$0")/.." status --porcelain)" ]; then
			"$0" --base HEAD --expect-files "$tmp/files.txt" "$plan" "$out" > /dev/null 2>&1; rc=$?
			[ "$rc" -eq 0 ] && grep -q '^files-sha256=' "$out/attestation.txt" || { echo "self-test: an exact file list (empty diff, clean tree) must pass and be attested with files-sha256 (rc=$rc)" >&2; fail=1; }
		fi
	fi
	# An empty plan must never look like a pass.
	: > "$plan"
	"$0" "$plan" "$out" > /dev/null 2>&1; rc=$?
	[ "$rc" -eq 2 ] || { echo "self-test: expected exit 2 for an empty plan, got $rc" >&2; fail=1; }
	"$0" "$tmp/does-not-exist.tsv" "$out" > /dev/null 2>&1; rc=$?
	[ "$rc" -eq 2 ] || { echo "self-test: expected exit 2 for a missing plan, got $rc" >&2; fail=1; }
	# A gate whose command itself is not found is a real failure (127), not a pass.
	printf '%s\t%s\n' "missing-cmd" "this-command-does-not-exist-4f2a" > "$plan"
	"$0" "$plan" "$out" > /dev/null 2>&1; rc=$?
	[ "$rc" -eq 1 ] && [ "$(cut -f3 "$out/results.tsv")" = "127" ] || { echo "self-test: a command-not-found gate must be ran/127 and exit 1" >&2; fail=1; }
	rm -rf "$tmp"
	if [ "$fail" -eq 0 ]; then echo "gate-chain: self-test passed (failing plan, clean plan + attestation, empty plan, missing plan, missing command, changed-files mismatch)"; exit 0; fi
	echo "gate-chain: SELF-TEST FAILED" >&2; exit 1
}

if [ "${1:-}" = "--self-test" ]; then self_test; fi

BASE=""; EXPECT_FILES=""
while [ $# -gt 0 ]; do
	case "$1" in
		--base) BASE="${2:?--base needs a commit}"; shift 2 ;;
		--expect-files) EXPECT_FILES="${2:?--expect-files needs a path}"; shift 2 ;;
		*) break ;;
	esac
done
if { [ -n "$BASE" ] && [ -z "$EXPECT_FILES" ]; } || { [ -z "$BASE" ] && [ -n "$EXPECT_FILES" ]; }; then
	echo "gate-chain: --base and --expect-files go together" >&2
	exit 2
fi
PLAN="${1:?usage: gate-chain.sh [--base <sha> --expect-files <list>] <plan.tsv> <out-dir>   |   gate-chain.sh --self-test}"
OUT="${2:?usage: gate-chain.sh [--base <sha> --expect-files <list>] <plan.tsv> <out-dir>}"

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 2

if [ ! -f "$PLAN" ]; then
	echo "gate-chain: plan $PLAN does not exist" >&2
	exit 2
fi
if ! mkdir -p "$OUT"; then
	echo "gate-chain: cannot create $OUT" >&2
	exit 2
fi
rm -f "$OUT/results.tsv" "$OUT/results.json" "$OUT/attestation.txt"
if [ -e "$OUT/results.tsv" ] || [ -e "$OUT/results.json" ] || [ -e "$OUT/attestation.txt" ]; then
	echo "gate-chain: could not remove a stale results file in $OUT - refusing to risk reporting it" >&2
	exit 2
fi

NAMES=(); CMDS=()
while IFS= read -r line || [ -n "$line" ]; do
	case "$line" in ''|'#'*) continue ;; esac
	name="${line%%	*}"
	cmd="${line#*	}"
	if [ -z "$name" ] || [ "$name" = "$line" ] || [ -z "$cmd" ]; then
		echo "gate-chain: malformed plan line (need <name><TAB><command>): $line" >&2
		exit 2
	fi
	case "$name" in *[!A-Za-z0-9._-]*) echo "gate-chain: gate name must be filename-safe: $name" >&2; exit 2 ;; esac
	NAMES+=("$name"); CMDS+=("$cmd")
done < "$PLAN"

if [ "${#NAMES[@]}" -eq 0 ]; then
	echo "gate-chain: plan $PLAN has no gates - an empty plan is not a pass" >&2
	exit 2
fi

# The implicit first row: the real change set against the expected list.
FILES_SHA=""
if [ -n "$BASE" ]; then
	[ -f "$EXPECT_FILES" ] || { echo "gate-chain: --expect-files $EXPECT_FILES does not exist" >&2; exit 2; }
	{ git diff --name-only "$BASE...HEAD"; git status --porcelain | awk '{print $NF}'; } 2>"$OUT/changed-files.err" | sed 's#\\#/#g' | sort -u > "$OUT/changed-files.actual"
	if [ -s "$OUT/changed-files.err" ]; then
		echo "gate-chain: could not compute the change set against $BASE:" >&2; cat "$OUT/changed-files.err" >&2; exit 2
	fi
	sed 's#\\#/#g' "$EXPECT_FILES" | sed 's/\r$//' | grep -v '^$' | sort -u > "$OUT/changed-files.expected"
	NAMES=("changed-files" "${NAMES[@]}"); CMDS=("(implicit)" "${CMDS[@]}")
fi

STATUS=(); EXITS=(); SECS=()
stopped=0
for i in "${!NAMES[@]}"; do
	if [ "${NAMES[$i]}" = "changed-files" ]; then
		log="$OUT/changed-files.log"
		if diff -u "$OUT/changed-files.expected" "$OUT/changed-files.actual" > "$log" 2>&1; then
			STATUS+=("ran"); EXITS+=("0"); SECS+=("0")
			echo "gate-chain: [changed-files] the plan's file list matches the real change set ($(wc -l < "$OUT/changed-files.actual") path(s))" >&2
		else
			STATUS+=("ran"); EXITS+=("1"); SECS+=("0"); stopped=1
			echo "gate-chain: [changed-files] the plan was computed from a file list that is NOT the real change set - see $log" >&2
		fi
		continue
	fi
	name="${NAMES[$i]}"; cmd="${CMDS[$i]}"
	if [ "$stopped" -ne 0 ]; then
		STATUS+=("not-run"); EXITS+=("-"); SECS+=("-")
		continue
	fi
	log="$OUT/$name.log"
	rm -f "$log"
	echo "gate-chain: [$((i + 1))/${#NAMES[@]}] $name: $cmd" >&2
	start=$(date +%s)
	bash -c "$cmd" > "$log" 2>&1
	rc=$?
	end=$(date +%s)
	STATUS+=("ran"); EXITS+=("$rc"); SECS+=("$((end - start))")
	echo "gate-chain:   exit $rc after $((end - start)) s (log: $log)" >&2
	[ "$rc" -eq 0 ] || stopped=1
done

{
	for i in "${!NAMES[@]}"; do
		if [ "${STATUS[$i]}" = "ran" ]; then logp="$OUT/${NAMES[$i]}.log"; else logp="-"; fi
		printf '%s\t%s\t%s\t%s\t%s\n' "${NAMES[$i]}" "${STATUS[$i]}" "${EXITS[$i]}" "${SECS[$i]}" "$logp"
	done
} > "$OUT/results.tsv"

{
	echo "["
	for i in "${!NAMES[@]}"; do
		if [ "${STATUS[$i]}" = "ran" ]; then
			logp="$OUT/${NAMES[$i]}.log"; ex="${EXITS[$i]}"; sec="${SECS[$i]}"
		else
			logp=""; ex="null"; sec="null"
		fi
		sep=","; [ "$i" -eq $(( ${#NAMES[@]} - 1 )) ] && sep=""
		printf '  {"gate": "%s", "status": "%s", "exit": %s, "seconds": %s, "log": "%s"}%s\n' \
			"${NAMES[$i]}" "${STATUS[$i]}" "$ex" "$sec" "$logp" "$sep"
	done
	echo "]"
} > "$OUT/results.json"

if [ "$stopped" -ne 0 ]; then
	echo "gate-chain: RED - see $OUT/results.tsv" >&2
	exit 1
fi
plan_sha=$(sha256sum "$PLAN" | cut -c1-64); results_sha=$(sha256sum "$OUT/results.tsv" | cut -c1-64)
head_sha=$(git rev-parse HEAD 2>/dev/null || echo "not-a-repository")
[ -n "$BASE" ] && FILES_SHA=$(sha256sum "$OUT/changed-files.actual" | cut -c1-64)
{
	echo "GATE-CHAIN-OK"
	echo "plan-sha256=$plan_sha"
	echo "results-sha256=$results_sha"
	echo "head=$head_sha"
	[ -n "$BASE" ] && echo "base=$BASE" && echo "files-sha256=$FILES_SHA"
	echo "gates=$(IFS=,; echo "${NAMES[*]}")"
} > "$OUT/attestation.txt"
echo "gate-chain: all ${#NAMES[@]} gate(s) ran and exited 0 - $OUT/results.tsv, attested in $OUT/attestation.txt" >&2
exit 0
