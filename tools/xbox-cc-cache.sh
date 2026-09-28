#!/usr/bin/env bash
# xbox-cc-cache.sh - a content-addressed object cache in front of one
# compile, for tools/xbox-cc.sh and tools/xbox-cxx.sh. Opt-in: those drivers
# call it only when a cache directory is configured (XBOX_CC_CACHE_DIR, or
# the checkout's .sweep/cc-cache-dir file, the same shell-boundary-proof
# channel as .sweep/nxdk-dir - see docs/r3d/lessons.md on exported
# variables dropped by /c/msys64/usr/bin/bash). With neither, nothing here
# runs and the drivers behave exactly as before.
#
#   tools/xbox-cc-cache.sh <cache-dir> <compiler> <compiler args...>
#   tools/xbox-cc-cache.sh --self-test
#
# Why: every build tool this project trusts (xbox-sweep.sh, the game and
# ref isolates, the freevgui sweep, xbox-link-check.sh's third-party set)
# deliberately recompiles every translation unit from scratch, so a stale
# object can never be vouched for. That stays true - each tool still runs
# its driver on every file and writes its manifest from what came back.
# What changes is only what a driver does when it has compiled this exact
# input before: it copies the object instead of regenerating it. For the
# fast loop (docs/r3d/fast-loop.md) a topic touching ref/nv2a recompiles
# the other ~95% of the game link's ~400 units identically to the baseline
# build of origin/main; with the cache, those are copies.
#
# The key is the sha256 of: this script's own content (a key-format change
# invalidates everything), the compiler's path, size, mtime and
# `--version`, every argument except the output path, and the PREPROCESSED
# translation unit with its line markers (`-E`: every header's content,
# every macro, every line number, the patched submodule files - whatever
# the compile actually sees). Two inputs with the same key are the same
# compile, so a hit is the object a fresh compile would give. The key is
# taken again after a successful compile and the object stored only if it
# did not move, so a file saved mid-compile cannot be filed under the
# original input.
#
# Repository and nxdk paths are rewritten to fixed tokens in the arguments
# and inside line markers only, so a git worktree hits what the main
# checkout built. That is sound because __FILE__ - the one way a path
# reaches the object's bytes at -O0 with no -g (neither driver passes -g;
# COFF's .file symbol is the basename, checked) - is made relative for the
# same path forms by the -fmacro-prefix-map this script adds to every
# compile it caches. So with the cache on, __FILE__ reads "./engine/..."
# instead of the absolute path: log text only. A path anywhere else in the
# unit (a -D string) is hashed raw, and a path form the rewrite does not
# know leaves an absolute path in the key: a miss, never a wrong hit.
#
# Turning the cache on or off therefore changes __FILE__ strings in the
# objects, and xbox-link-check.sh's staleness check (the xbox-cc.sh cksum
# in the sweep manifest) does not see it. A link mixing both is still
# correct code - only log strings differ. After changing THIS script,
# delete the cache directory: its cksum in the key already stops old
# entries from hitting, but objects already copied into a .sweep stay.
#
# Anything this script does not understand is a bypass, never a guess: an
# argument outside the plain -c/-o/-I/-D/-U/-f/-W/-m/-std/-target/-include/
# -isystem set (-v, -E, -S, -M*, -save-temps, --serialize-diagnostics...),
# and -fsanitize*/-fprofile*/-fcoverage*/-fplugin*/-Wp,* within it,
# no sha256sum, no -o, a failed preprocess. A bypass runs the compiler with
# the original arguments, unchanged. A failed compile is never cached.
#
# Diagnostics: a hit prints nothing - warnings are not replayed. The sweeps
# count only "error:" lines from FAILED compiles, and failures are never
# cached, so no tool's verdict reads a hit's missing warnings.
#
# <cache-dir>/stats.log gets one "hit|miss|bypass <source>" line per call,
# so a run can measure what the cache bought instead of assuming it.
set -u

self_test() {
	local tmp rc fail=0
	tmp="$(mktemp -d)" || { echo "xbox-cc-cache: mktemp failed" >&2; exit 2; }
	# A stub compiler: -E prints the source's content (so the key follows
	# the input), a compile writes "OBJ:<content>" to -o and counts itself.
	cat > "$tmp/cc" <<'EOF'
#!/usr/bin/env bash
if [ "${1:-}" = "--version" ]; then echo "stub 1.0"; exit 0; fi
mode=c; out=""; src=""
while [ $# -gt 0 ]; do
	case "$1" in
		-E) mode=E ;;
		-o) out="$2"; shift ;;
		-c|-P|-I*|-D*|-f*) ;;
		*) src="$1" ;;
	esac
	shift
done
if grep -q FAIL "$src"; then
	[ "$mode" = c ] && [ -n "$out" ] && printf PARTIAL > "$out"
	echo "$src:1: error: stub failure" >&2; exit 1
fi
if [ "$mode" = E ]; then cat "$src"; [ "$out" = "-" ] || exit 3; exit 0; fi
echo compiled >> "$(dirname "$0")/count"
# An editor saving the source after the key was taken, before the compile
# reads it.
[ -f "$(dirname "$0")/edit-during" ] && printf 'int edited;\n' > "$src" && rm -f "$(dirname "$0")/edit-during"
printf 'OBJ:%s' "$(cat "$src")" > "$out"
exit 0
EOF
	chmod +x "$tmp/cc"
	local C="$tmp/cache"
	printf 'int a;\n' > "$tmp/a.c"
	bash "$0" "$C" "$tmp/cc" -DX -c "$tmp/a.c" -o "$tmp/a1.obj"; rc=$?
	[ "$rc" -eq 0 ] && [ "$(cat "$tmp/a1.obj")" = "OBJ:int a;" ] || { echo "self-test: miss did not compile (rc $rc)" >&2; fail=1; }
	bash "$0" "$C" "$tmp/cc" -DX -c "$tmp/a.c" -o "$tmp/a2.obj"; rc=$?
	[ "$rc" -eq 0 ] && cmp -s "$tmp/a1.obj" "$tmp/a2.obj" || { echo "self-test: hit did not reproduce the object" >&2; fail=1; }
	[ "$(wc -l < "$tmp/count")" -eq 1 ] || { echo "self-test: a hit invoked the compiler" >&2; fail=1; }
	# A different define is a different compile.
	bash "$0" "$C" "$tmp/cc" -DY -c "$tmp/a.c" -o "$tmp/a3.obj"
	[ "$(wc -l < "$tmp/count")" -eq 2 ] || { echo "self-test: a changed argument hit the cache" >&2; fail=1; }
	# Changed input content is a different compile.
	printf 'int b;\n' > "$tmp/a.c"
	bash "$0" "$C" "$tmp/cc" -DX -c "$tmp/a.c" -o "$tmp/a4.obj"
	[ "$(cat "$tmp/a4.obj")" = "OBJ:int b;" ] || { echo "self-test: changed source served a stale object" >&2; fail=1; }
	# A failure exits nonzero, prints the error, and is not cached.
	printf 'FAIL\n' > "$tmp/f.c"
	bash "$0" "$C" "$tmp/cc" -c "$tmp/f.c" -o "$tmp/f.obj" 2> "$tmp/err"; rc=$?
	[ "$rc" -ne 0 ] && grep -q 'error:' "$tmp/err" || { echo "self-test: a failing compile did not fail loudly (rc $rc)" >&2; fail=1; }
	# The stub leaves a partial object behind, as a crashing compiler can:
	# it must not be cached, so the same input fails again.
	bash "$0" "$C" "$tmp/cc" -c "$tmp/f.c" -o "$tmp/f2.obj" 2> /dev/null; rc=$?
	[ "$rc" -ne 0 ] || { echo "self-test: a failed compile was cached and served" >&2; fail=1; }
	# A source edited during its compile must not file the new object under
	# the old input's key: compiling the old input again is a miss.
	printf 'int orig;\n' > "$tmp/e.c"; touch "$tmp/edit-during"
	bash "$0" "$C" "$tmp/cc" -c "$tmp/e.c" -o "$tmp/e1.obj"
	printf 'int orig;\n' > "$tmp/e.c"
	bash "$0" "$C" "$tmp/cc" -c "$tmp/e.c" -o "$tmp/e2.obj"
	[ "$(cat "$tmp/e2.obj")" = "OBJ:int orig;" ] || { echo "self-test: an object compiled while its source changed was served for the original source" >&2; fail=1; }
	grep -q '^unstable ' "$C/stats.log" || { echo "self-test: an edit during the compile was not recorded as unstable" >&2; fail=1; }
	# An unknown argument bypasses: the compiler runs with it, every time.
	local n; n="$(wc -l < "$tmp/count")"
	printf 'int c;\n' > "$tmp/c.c"
	bash "$0" "$C" "$tmp/cc" -v -c "$tmp/c.c" -o "$tmp/c1.obj" 2>/dev/null
	bash "$0" "$C" "$tmp/cc" -v -c "$tmp/c.c" -o "$tmp/c2.obj" 2>/dev/null
	[ "$(wc -l < "$tmp/count")" -eq $((n + 2)) ] || { echo "self-test: -v did not bypass" >&2; fail=1; }
	grep -q '^bypass ' "$C/stats.log" && grep -q '^hit ' "$C/stats.log" && grep -q '^miss ' "$C/stats.log" \
		|| { echo "self-test: stats.log lacks hit/miss/bypass lines" >&2; fail=1; }
	rm -rf "$tmp"
	[ "$fail" -eq 0 ] && echo "xbox-cc-cache: self-test passed" && exit 0
	exit 1
}

[ "${1:-}" = "--self-test" ] && self_test

CACHE="${1:?usage: xbox-cc-cache.sh <cache-dir> <compiler> <args...>}"
shift
CC="${1:?usage: xbox-cc-cache.sh <cache-dir> <compiler> <args...>}"

REPO="$(cd "$(dirname "$0")/.." && pwd)"

stat_line() { mkdir -p "$CACHE" 2>/dev/null && printf '%s %s\n' "$1" "$2" >> "$CACHE/stats.log" 2>/dev/null; true; }

bypass() { stat_line bypass "${SRC:-?}"; exec "$@"; }

command -v sha256sum > /dev/null 2>&1 || bypass "$@"

# One pass over the arguments: find the output, build the preprocess
# command (no -o, no -c) and the compile command (every -o replaced by a
# private temporary - see below), refuse anything outside the known-plain
# set. The -f/-W exclusions first: -fsanitize/-fprofile/-fcoverage embed
# paths or locations, a -fplugin's binary is not in the key, and
# -Wp,-MD,<file> writes a file a hit would not.
OUT=""
SRC=""
PP=()
CMD=( "$CC" )
args=( "$@" )
i=1
while [ "$i" -lt "${#args[@]}" ]; do
	a="${args[$i]}"
	case "$a" in
		-o) OUT="${args[$((i + 1))]:-}"; i=$((i + 2)); continue ;;
		-c) CMD+=( "$a" ) ;;
		-I|-isystem|-include|-target|-D|-U)
			PP+=( "$a" "${args[$((i + 1))]:-}" ); CMD+=( "$a" "${args[$((i + 1))]:-}" ); i=$((i + 2)); continue ;;
		-fsanitize*|-fprofile*|-fcoverage*|-fplugin*|-Wp,*) bypass "$@" ;;
		-I*|-D*|-U*|-f*|-W*|-m*|-std=*|-nostdlib|-nostdinc|-ffreestanding) PP+=( "$a" ); CMD+=( "$a" ) ;;
		-*) bypass "$@" ;;
		*) SRC="$a"; PP+=( "$a" ); CMD+=( "$a" ) ;;
	esac
	i=$((i + 1))
done
[ -n "$OUT" ] && [ -n "$SRC" ] || bypass "$@"

# Path forms the key rewrites to tokens. MSYS2 hands a native clang
# Windows-form paths (C:/x or C:\x); POSIX form is what the drivers hold.
# Longest form first: in the main checkout nxdk lives INSIDE the repo, and
# a repo rule applied first would turn its paths into @REPO@/deps/nxdk
# where a worktree (pointing at the same nxdk from outside) gets @NXDK@ -
# a permanent cross-worktree miss (found in review). clang's own prefix
# maps already pick the longest match, whatever their order (tested).
NXDK="${NXDK_DIR:-}"
forms() {
	[ -n "$1" ] || return 0
	printf '%s\t%s\n' "$1" "$2"
	if command -v cygpath > /dev/null 2>&1; then
		printf '%s\t%s\n' "$(cygpath -m "$1")" "$2" "$(cygpath -w "$1")" "$2"
	fi
}
FORMS="$( { forms "$REPO" @REPO@; forms "$NXDK" @NXDK@; } | awk -F'\t' '{ print length($1) "\t" $0 }' | sort -rn | cut -f2- )"
# ARGV rules rewrite anywhere in an argument. TEXT rules rewrite only the
# path inside a preprocessor line marker (# <n> "<path>" ...): everything
# else in the preprocessed unit is hashed raw, so a repository path that
# reaches the object any other way (a -D string, a computed include) stays
# in the key - a miss, never a wrong hit (found in review: rewriting the
# whole text let a second checkout hit an object carrying the first one's
# -D path). Line numbers are kept: __builtin_LINE and friends read them,
# and a -P key could not tell two sources apart that differ only by lines.
SEDA=()
SEDT=()
while IFS=$'\t' read -r f tok; do
	[ -n "$f" ] || continue
	e="$(printf '%s' "$f" | sed 's/[][\.*^$|]/\\&/g')"
	SEDA+=( -e "s|$e|$tok|g" )
	SEDT+=( -e "/^# [0-9]/s|$e|$tok|g" )
done <<< "$FORMS"
norm_args() { if [ "${#SEDA[@]}" -gt 0 ]; then sed "${SEDA[@]}"; else cat; fi; }
norm_text() { if [ "${#SEDT[@]}" -gt 0 ]; then sed "${SEDT[@]}"; else cat; fi; }

# The key treats every form above as equal in line markers, so __FILE__
# must be equal too: the same forms are mapped out of it for THIS compile,
# here, not left to the caller. (Found testing against real clang: with
# the map left to the drivers, a second checkout hit an object carrying
# the first checkout's absolute __FILE__ strings.)
MAPS=()
while IFS=$'\t' read -r f tok; do
	[ -n "$f" ] || continue
	case "$tok" in @REPO@) MAPS+=( "-fmacro-prefix-map=$f=." ) ;; @NXDK@) MAPS+=( "-fmacro-prefix-map=$f=nxdk" ) ;; esac
done <<< "$FORMS"
PP+=( "${MAPS[@]}" )

# Compiler identity: its resolved path, size and mtime (a rebuild at the
# same --version changes these), and --version itself.
CC_PATH="$(command -v "$CC")"
CC_STAMP="$(stat -c '%s %Y' "$CC_PATH" 2>/dev/null || ls -l "$CC_PATH" 2>/dev/null)"

key() {
	{
		cksum < "$0"
		printf '%s\n%s\n' "$CC_PATH" "$CC_STAMP"
		"$CC" --version 2>&1
		printf '%s\n' "${PP[@]}" | norm_args
		printf '%s\n' '--- preprocessed ---'
		"$CC" "${PP[@]}" -E -o - 2>/dev/null | norm_text
		printf 'pp-exit %s\n' "${PIPESTATUS[0]}"
	} | sha256sum | cut -c1-64
}
KEY="$(key)"
# A failed preprocess still yields a key (its exit is in it), but nothing
# is stored for it: the compile fails, or the re-key below differs.
case "$KEY" in
	[0-9a-f][0-9a-f]*) ;;
	*) bypass "$@" ;;
esac
ENTRY="$CACHE/${KEY:0:2}/$KEY.obj"

if [ -f "$ENTRY" ]; then
	if cp "$ENTRY" "$OUT.cc-cache.$$" 2>/dev/null && mv -f "$OUT.cc-cache.$$" "$OUT"; then
		stat_line hit "$SRC"
		exit 0
	fi
	rm -f "$OUT.cc-cache.$$"
fi

# Compile into a file only this process writes, never the caller-visible
# -o: two invocations sharing an output path (the drivers' default
# /tmp/<base>.obj, say) could otherwise store one's object under the
# other's key (found in review).
TMP="$OUT.cc-cache.$$"
"${CMD[@]}" -o "$TMP" "${MAPS[@]}"
rc=$?
if [ "$rc" -eq 0 ] && [ -f "$TMP" ]; then
	# Re-key after the compile: a source or header edited while it ran
	# would otherwise file the object of the edited input under the key of
	# the original one, a wrong hit for every later compile of the original
	# (found in review, reproduced with a compiler shim). Store only when
	# the input provably did not move.
	if [ "$(key)" = "$KEY" ]; then
		mkdir -p "$CACHE/${KEY:0:2}" 2>/dev/null \
			&& cp "$TMP" "$ENTRY.tmp.$$" 2>/dev/null \
			&& mv -f "$ENTRY.tmp.$$" "$ENTRY" 2>/dev/null
		rm -f "$ENTRY.tmp.$$"
		stat_line miss "$SRC"
	else
		stat_line unstable "$SRC"
	fi
	mv -f "$TMP" "$OUT" || { rm -f "$TMP"; exit 1; }
else
	rm -f "$TMP"
fi
exit "$rc"
