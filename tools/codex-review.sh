#!/usr/bin/env bash
# Run a Codex adversarial review of this branch's diff against a base ref,
# using the openai-codex Claude Code plugin's companion script directly -
# bypassing the /codex:adversarial-review slash command, which carries
# disable-model-invocation: true and so cannot be triggered autonomously.
#
#   tools/codex-review.sh [--base <ref>] [extra focus text...]
#
# Replaces .claude/workflows/pr-review.js (deleted 2026-09-07): that
# workflow's 6-lens/3-refuter fan-out cost 25-37 Claude subagent calls and
# 2-2.6M subagent tokens PER ROUND, and needed 11 rounds to close one
# branch. This moves review compute onto Codex instead of Claude's own
# token budget - see AGENTS.md's "Review on every PR" section for the
# process this now backs (one review, fix/justify, one re-review, open the
# PR - capped at two rounds).
#
# Can take several minutes: Codex runs its own read-only git commands
# against the full diff before answering. Run this in the background and
# poll for output rather than a short foreground wait.
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1

BASE="main"
# gpt-5.6-sol, not the account's own configured default (gpt-5.6-terra,
# ~/.codex/config.toml): sol's own model-cache entry advertises a lower
# default reasoning effort (low vs terra's medium) for the same "everyday
# work" tier, at a real cost this project has now hit directly - the
# account's usage limit was exhausted mid-session running reviews with no
# model pin at all. --model, not documented in codex-companion.mjs's own
# --help banner for adversarial-review but present and load-bearing in its
# arg parser (the same undocumented-but-real shape as --json, found
# earlier in this project's own history with this script), is passed
# straight through unaliased to the underlying `codex` CLI - so this needs
# the real model slug (gpt-5.6-sol), not a short name; codex-companion.mjs
# only knows one alias itself ("spark"), and it is not this one.
# CODEX_REVIEW_MODEL overrides it, e.g. CODEX_REVIEW_MODEL=gpt-5.6-terra
# for a harder review when sol's lighter reasoning misses something subtle.
CODEX_REVIEW_MODEL="${CODEX_REVIEW_MODEL:-gpt-5.6-sol}"
EXTRA=()
while [ $# -gt 0 ]; do
	case "$1" in
		--base)
			BASE="$2"
			shift 2
			;;
		*)
			EXTRA+=("$1")
			shift
			;;
	esac
done

# Dynamic version resolution, not hardcoded: hardcoding e.g. "1.0.6" here
# would go stale the moment the plugin updates, and would then fail against
# a wrong-but-plausible-looking path instead of the newest install actually
# on disk.
CODEX_PLUGIN_DIR=$(ls -d "$HOME/.claude/plugins/cache/openai-codex/codex/"*/ 2>/dev/null | sort -V | tail -1)
if [ -z "$CODEX_PLUGIN_DIR" ]; then
	echo "codex-review: no openai-codex plugin install found under" >&2
	echo "  \$HOME/.claude/plugins/cache/openai-codex/codex/ - is the plugin" >&2
	echo "  installed? /plugin marketplace add openai/codex-plugin-cc, then" >&2
	echo "  /plugin install codex@openai-codex." >&2
	exit 1
fi

COMPANION="${CODEX_PLUGIN_DIR}scripts/codex-companion.mjs"
if [ ! -f "$COMPANION" ]; then
	echo "codex-review: $COMPANION not found - plugin install looks broken." >&2
	exit 1
fi

# The merge-base, computed here rather than left to Codex: adversarial-
# review's own source shows self-collect mode (which fires on any diff
# over 2 files or 256 KB - essentially every real branch diff) never
# receives the merge-base SHA through the prompt, only a human-readable
# target label like "branch diff against main". Codex is left to re-derive
# the range itself, and guessing `git diff main HEAD` instead of the
# merge-base range silently reviews the wrong set of commits if main has
# moved since the branch forked. Compute it here and hand Codex the literal
# range instead of trusting it to reconstruct the same one.
MERGE_BASE=$(git merge-base "$BASE" HEAD 2>/dev/null)
if [ -z "$MERGE_BASE" ]; then
	echo "codex-review: 'git merge-base $BASE HEAD' failed - is '$BASE' a" >&2
	echo "  valid ref, and is HEAD reachable from it?" >&2
	exit 1
fi

FOCUS_FILE="$REPO/docs/r3d/codex-review-focus.md"
if [ ! -f "$FOCUS_FILE" ]; then
	echo "codex-review: $FOCUS_FILE not found." >&2
	exit 1
fi

FOCUS=$(cat "$FOCUS_FILE")
FOCUS="${FOCUS}

The exact range to review is \`git diff ${MERGE_BASE}..HEAD\` (merge-base of
${BASE} and HEAD, computed by this script - not \"${BASE}\" itself, which may
have moved since). Run that diff yourself and review only those commits."

if [ "${#EXTRA[@]}" -gt 0 ]; then
	FOCUS="${FOCUS}

Additional focus for this run: ${EXTRA[*]}"
fi

echo "codex-review: reviewing ${MERGE_BASE}..HEAD (base: $BASE), model $CODEX_REVIEW_MODEL" >&2
echo "codex-review: this can take several minutes." >&2

STDERR_FILE=$(mktemp)

# Stdout and stderr captured SEPARATELY, not merged: found while writing
# this script - `node ... setup --json` printed a Node deprecation warning
# to stderr BEFORE the JSON payload earlier this same session, which would
# have broken JSON.parse if stdout and stderr had been combined with 2>&1.
# Command substitution captures only stdout by default; stderr goes to its
# own file, read separately, never mixed into what gets parsed as JSON.
#
# --json and the focus text as SEPARATE argv entries, not one combined
# string: codex-companion.mjs's own arg parser re-tokenizes a SINGLE argv
# token after the subcommand name (normalizeArgv/splitRawArgumentString),
# treating backslashes as escape characters and stripping quotes - which
# would mangle a Windows path or literal backslash inside the focus text.
# Passing --base, --json and the focus text as separate tokens (3+ total)
# skips that path entirely; the args are used verbatim.
#
# --base "$MERGE_BASE", not "$BASE": found by this script's own first real
# review run. codex-companion.mjs recomputes its OWN merge-base internally
# from whatever --base it receives (`git merge-base HEAD <baseRef>`), to
# build the commit log/diff-stat/changed-file list it actually hands
# Codex. Passing the movable ref name here left two independent
# merge-base computations - this script's own, embedded only in the
# prompt text above, and codex-companion.mjs's internal one driving the
# real collected context - that could diverge if the ref moved between
# them, defeating the whole point of computing MERGE_BASE up front. A SHA
# is not an ancestor-relative ref that can move: passing it directly makes
# codex-companion.mjs's internal recomputation trivially resolve to the
# same SHA, so there is exactly one source of truth for the range instead
# of two that happen to usually agree.
RAW=$(node "$COMPANION" adversarial-review --json --base "$MERGE_BASE" --model "$CODEX_REVIEW_MODEL" --cwd "$REPO" "$FOCUS" 2>"$STDERR_FILE")
STATUS=$?
STDERR_CONTENT=$(cat "$STDERR_FILE")
rm -f "$STDERR_FILE"

# A nonzero exit here is NOT proof the review failed: codex-companion.mjs
# has been observed to propagate the inner Codex CLI's own process exit
# status as its own, even on a run that completed a full turn and printed a
# fully valid, parseable result (parseError: null, a non-null result
# object). Treating STATUS alone as fatal is exactly the bug the node
# parser below already guards against one layer down (see its own comment)
# - just recreated one layer up. So: only hard-fail on STATUS here when
# there is no stdout to even attempt parsing (a real crash-before-output
# case). Otherwise fall through to the node parser and let payload.parseError
# / payload.result be the actual authority, same as a STATUS-0 run - if
# parsing also fails, that failure path reports STATUS for context.
if [ "$STATUS" -ne 0 ] && [ -z "$RAW" ]; then
	echo "codex-review: codex-companion.mjs exited $STATUS with no output -" >&2
	echo "  Codex did not complete a review turn." >&2
	if [ -n "$STDERR_CONTENT" ]; then
		echo "--- stderr ---" >&2
		printf '%s\n' "$STDERR_CONTENT" >&2
	fi
	exit 2
fi
if [ "$STATUS" -ne 0 ]; then
	echo "codex-review: warning - codex-companion.mjs exited $STATUS but printed" >&2
	echo "  output; checking whether it is a valid result before failing." >&2
fi

# A nonempty stderr on a "successful" (exit 0) run is not fatal by itself -
# --json is documented to suppress the script's own progress stream, but
# does not control what Node or a child process writes on its own - so
# surface it as a warning rather than silently discarding it. Distrust a
# clean-looking success: this project has hit exactly that shape of bug
# (a check that goes quiet exactly when something is actually wrong)
# multiple times already, in tools this branch itself wrote.
if [ -n "$STDERR_CONTENT" ]; then
	echo "codex-review: warning - non-empty stderr on a completed run:" >&2
	printf '%s\n' "$STDERR_CONTENT" >&2
fi

# Parse with node, not grep/sed: this project's own lessons.md has separate
# entries about shell text-processing silently doing the wrong thing on
# this exact machine (a mismatched grep/sed build producing clean exit
# codes with corrupted output). node is already a hard dependency for
# codex-companion.mjs itself, so it costs nothing extra to trust it here
# for JSON where a regex would be fragile.
printf '%s' "$RAW" | node -e '
let raw = "";
process.stdin.on("data", d => raw += d);
process.stdin.on("end", () => {
	let payload;
	try {
		payload = JSON.parse(raw);
	} catch (e) {
		console.error("codex-review: stdout was not valid JSON - " + e.message);
		console.error("--- raw output ---");
		console.error(raw);
		process.exit(2);
	}

	// Exit 0 from codex-companion.mjs means "the Codex turn completed", not
	// "the review is trustworthy" - a parse failure on a completed turn
	// still exits 0 with result: null. Check parseError/result explicitly
	// rather than trusting the process exit code - the same silent-success
	// shape this whole script exists to avoid reintroducing.
	if (payload.parseError || payload.result === null || payload.result === undefined) {
		console.error("codex-review: Codex completed a turn but did not return a");
		console.error("  valid structured result - parseError: " + JSON.stringify(payload.parseError));
		console.error("--- raw Codex output ---");
		console.error(payload.rawOutput || "(none)");
		process.exit(2);
	}

	const r = payload.result;
	const sevRank = { critical: 0, high: 1, medium: 2, low: 3 };
	const findings = (r.findings || []).slice().sort(
		(a, b) => (sevRank[a.severity] ?? 9) - (sevRank[b.severity] ?? 9)
	);

	console.log("");
	console.log("verdict: " + r.verdict);
	console.log("summary: " + r.summary);
	console.log("");
	console.log(findings.length + " finding(s):");
	let mustFixCount = 0;
	for (const f of findings) {
		const mustAct = (f.severity === "critical" || f.severity === "high" || f.confidence >= 0.6);
		if (mustAct) mustFixCount++;
		console.log("");
		console.log("[" + f.severity + ", confidence " + f.confidence + ", "
			+ (mustAct ? "MUST FIX OR JUSTIFY" : "optional") + "] "
			+ f.file + ":" + f.line_start + "-" + f.line_end);
		console.log("  " + f.title);
		console.log("  " + f.body);
		console.log("  recommendation: " + f.recommendation);
	}
	if (r.next_steps && r.next_steps.length) {
		console.log("");
		console.log("next steps:");
		for (const s of r.next_steps) console.log("  - " + s);
	}
	console.log("");
	console.log("codex thread: " + payload.threadId + " (resume: codex resume " + payload.threadId + ")");

	// Found by this script own third real review run (no apostrophe above
	// on purpose - this text sits inside a bash single-quoted string, and
	// an apostrophe there ends the string early and breaks the script):
	// printing findings labelled "MUST FIX OR JUSTIFY" and then exiting 0
	// regardless is the exact silent-success shape this project has hit
	// repeatedly in its own tools (xbox-sweep.sh, xbox-check-nxdk-cc-drift.sh,
	// twice each) - a caller checking only the exit code cannot tell
	// "approve" from "needs-attention with blocking findings". Exit nonzero
	// on either signal: the top-level Codex verdict, or any individual
	// finding crossing the fix-or-justify threshold (belt and suspenders -
	// the schema calibration rules say verdict should already track this,
	// but check both rather than trust one).
	const blocking = r.verdict !== "approve" || mustFixCount > 0;
	console.log("");
	console.log(blocking
		? ("codex-review: BLOCKING - verdict=" + r.verdict + ", " + mustFixCount
			+ " finding(s) meet the fix-or-justify threshold. Exiting 1.")
		: "codex-review: clean - verdict=approve, no finding meets the fix-or-justify threshold. Exiting 0.");
	if (blocking) process.exitCode = 1;
});
'
