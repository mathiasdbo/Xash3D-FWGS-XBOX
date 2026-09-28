#!/usr/bin/env bash
# live-look.sh - boot one built XBE in xemu, wait for the map's scene, take
# its frame, and diff it against origin/main's frame of the same scene. One
# command, deterministic, for the fast loop's verify stage
# (docs/r3d/fast-loop.md): the agentic loop's v2 live stage was ~15 steps
# of prose a model carried out by hand, twice per look (baseline and
# branch), and every step was a place to drift.
#
#   tools/live-look.sh --xbe <default.xbe> --exe <xbe-build.exe> --linkmap <xbe-build.map>
#       --baseline <dir> --steam <Half-Life dir> --xemu <xemu.exe>
#       --map <name> --mpak <file.mpak> --out <dir>
#       [--cvar '<console line>']... [--gamedir valve] [--frames 60]
#       [--timeout 180] [--resource-dir <dir>] [--ui-language <lang>]
#
# --baseline <dir>: the SCRATCH origin/main's build ran under, holding
# default.xbe (+ .identity) and game-hl/xbe-build.{exe,map} - the layout
# tools/xbox-game-link-check.sh --xbe leaves. Its frame of THIS scene is
# cached in <dir>/look-<key>/, key = sha256 of the overlay (video.cfg,
# 00-live.cfg, the .mpak) plus --frames: the first look at a scene pays a
# baseline pack and boot, every later look at the same scene on the same
# origin/main pays only the branch's. That halves xemu time per look.
#
# The overlay is what run 2 (PR #149) placed on its disc by hand before
# its screenshots: video.cfg with r_refdll "nv2a" (retail selects
# ref_soft), userconfig.d/00-live.cfg with "map <name>" and the cvars, and
# the .mpak at the gamedir ROOT (one inside maps/ is never found -
# lessons.md, the archive-discovery entry).
#
# Per boot: refuse if a GDB stub already answers (another xemu would be
# measured instead - lessons.md, "ALWAYS confirm which module composition
# produced the XBE under test"); launch xemu -s on the packed ISO; poll
# verify-image until the guest provably runs THIS exe (it is not loaded the
# instant the stub answers); wait-scene --frames N (no frame is taken
# unless the map is ready and N frames have run since); fbdump .ppm (for
# the diff) and screenshot .png (for eyes); kill xemu.
#
# Output, in --out: result.txt (key=value lines, below), the branch and
# baseline .ppm/.png, every tool's log. result.txt ends in LIVE-LOOK-OK
# only when both boots confirmed identity, reached the scene and wrote a
# frame, and the diff ran. The caller judges the diff numbers; this
# script judges only whether a look was taken.
#
# Exit: 0 a look was taken; 1 the branch build did not boot to the scene
# or the guest was not running it; 2 the look could not be attempted
# (missing input, busy port, packing failed, baseline could not be taken).
#
# --self-test runs this script against a fake xemu, debugger and packer:
# a cold baseline, a cached one (one boot, not two), a branch that never
# reaches the scene, and a stub that already answers. Needs only bash.
set -u

self_test() {
	local T fail=0 rc
	T="$(mktemp -d)" || exit 2
	mkdir -p "$T/steam/valve" "$T/base/game-hl" "$T/br"
	echo 'r_refdll ""' > "$T/steam/valve/video.cfg"
	for f in base/default.xbe base/default.xbe.identity base/game-hl/xbe-build.exe base/game-hl/xbe-build.map \
		br/default.xbe br/default.xbe.identity br/xbe-build.exe br/xbe-build.map m.mpak; do echo "$f" > "$T/$f"; done
	cat > "$T/xemu" <<XEMU
#!/usr/bin/env bash
echo boot >> "$T/boots"; touch "$T/running"
trap 'rm -f "$T/running"; exit 0' TERM INT
sleep 300 & wait
XEMU
	cat > "$T/dbg" <<DBG
#!/usr/bin/env bash
case "\$1" in
	regs) [ -f "$T/running" ] ;;
	verify-image) if [ -f "$T/wrongimage" ] && grep -q br "\$2"; then echo "verify-image: the guest is NOT running this file."; exit 1; fi
		echo "verify-image: identity confirmed - every file-backed read-only byte matches." ;;
	wait-scene) if [ -f "$T/noscene" ] && grep -q br "\$3"; then echo "wait-scene: TIMEOUT after 1 s"; exit 1; fi
		echo "scene ready: _nv2a_map_ready=1 frames=+60 after 1 s" ;;
	fbdump|screenshot) echo frame > "\$2" ;;
	ppmdiff) echo "ppmdiff: 0 of 307200 pixels differ, bbox=None" ;;
esac
DBG
	printf '#!/usr/bin/env bash\nwhile [ $# -gt 0 ]; do [ "$1" = --out ] && touch "$2"; shift; done\n' > "$T/xiso"
	chmod +x "$T/xemu" "$T/dbg" "$T/xiso"
	run() {
		LIVE_LOOK_DBG="$T/dbg" LIVE_LOOK_XISO="$T/xiso" bash "$0" --xbe "$T/br/default.xbe" --exe "$T/br/xbe-build.exe" \
			--linkmap "$T/br/xbe-build.map" --baseline "$T/base" --steam "$T/steam" --xemu "$T/xemu" \
			--map crossfire --mpak "$T/m.mpak" --out "$T/out" --cvar "r_showhull 1" > "$T/run.log" 2>&1
	}
	run; rc=$?
	[ "$rc" -eq 0 ] && grep -q '^LIVE-LOOK-OK$' "$T/out/result.txt" && grep -q '^baseline_cached=no$' "$T/out/result.txt" \
		|| { echo "self-test: cold look failed (exit $rc)" >&2; cat "$T/run.log" >&2; fail=1; }
	[ "$(wc -l < "$T/boots")" -eq 2 ] || { echo "self-test: a cold look should boot twice" >&2; fail=1; }
	grep -q 'r_refdll "nv2a"' "$T/out/overlay/valve/video.cfg" && grep -q '^map crossfire$' "$T/out/overlay/valve/userconfig.d/00-live.cfg" \
		&& grep -q '^r_showhull 1$' "$T/out/overlay/valve/userconfig.d/00-live.cfg" \
		&& [ -f "$T/out/overlay/valve/m.mpak" ] || { echo "self-test: overlay is wrong" >&2; fail=1; }
	run; rc=$?
	[ "$rc" -eq 0 ] && grep -q '^baseline_cached=yes$' "$T/out/result.txt" || { echo "self-test: warm look did not use the cached baseline" >&2; fail=1; }
	[ "$(wc -l < "$T/boots")" -eq 3 ] || { echo "self-test: a warm look should boot once" >&2; fail=1; }
	touch "$T/noscene"; run; rc=$?
	[ "$rc" -eq 1 ] && ! grep -q LIVE-LOOK-OK "$T/out/result.txt" || { echo "self-test: a branch that never reaches the scene must exit 1 with no OK (got $rc)" >&2; fail=1; }
	[ ! -f "$T/running" ] || { echo "self-test: xemu left running after a failed look" >&2; fail=1; }
	rm -f "$T/noscene"
	# The guest running another image: exit 1, no frame taken. (The poll
	# gives it 120 s; LIVE_LOOK_IDENTITY_WAIT shortens that for the test.)
	touch "$T/wrongimage"; LIVE_LOOK_IDENTITY_WAIT=4 run; rc=$?
	[ "$rc" -eq 1 ] && ! grep -q LIVE-LOOK-OK "$T/out/result.txt" && [ ! -f "$T/out/branch/frame.ppm" ] \
		|| { echo "self-test: a guest running another image must exit 1 with no frame (got $rc)" >&2; fail=1; }
	rm -f "$T/wrongimage"
	# Different cvars are a different scene: a new baseline, not the cached one.
	local k1; k1="$(sed -n 's/^scene_key=//p' "$T/out/result.txt")"
	LIVE_LOOK_DBG="$T/dbg" LIVE_LOOK_XISO="$T/xiso" bash "$0" --xbe "$T/br/default.xbe" --exe "$T/br/xbe-build.exe" \
		--linkmap "$T/br/xbe-build.map" --baseline "$T/base" --steam "$T/steam" --xemu "$T/xemu" \
		--map crossfire --mpak "$T/m.mpak" --out "$T/out" --cvar "r_showhull 0" > "$T/run.log" 2>&1
	[ "$(sed -n 's/^scene_key=//p' "$T/out/result.txt")" != "$k1" ] && grep -q '^baseline_cached=no$' "$T/out/result.txt" \
		|| { echo "self-test: the scene key must follow the cvars" >&2; fail=1; }
	touch "$T/running"; run; rc=$?
	[ "$rc" -eq 2 ] || { echo "self-test: a stub already answering must exit 2 (got $rc)" >&2; fail=1; }
	rm -rf "$T"
	[ "$fail" -eq 0 ] && echo "live-look: self-test passed" && exit 0
	exit 1
}
[ "${1:-}" = "--self-test" ] && self_test

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 2

XBE="" EXE="" LINKMAP="" BASE="" STEAM="" XEMU="" MAP="" MPAK="" OUT=""
GAMEDIR="valve" FRAMES=60 TIMEOUT=180 RESOURCE_DIR="" UI_LANGUAGE=""
CVARS=()
while [ $# -gt 0 ]; do
	case "$1" in
		--xbe) XBE="$2"; shift 2 ;;
		--exe) EXE="$2"; shift 2 ;;
		--linkmap) LINKMAP="$2"; shift 2 ;;
		--baseline) BASE="$2"; shift 2 ;;
		--steam) STEAM="$2"; shift 2 ;;
		--xemu) XEMU="$2"; shift 2 ;;
		--map) MAP="$2"; shift 2 ;;
		--mpak) MPAK="$2"; shift 2 ;;
		--out) OUT="$2"; shift 2 ;;
		--cvar) CVARS+=( "$2" ); shift 2 ;;
		--gamedir) GAMEDIR="$2"; shift 2 ;;
		--frames) FRAMES="$2"; shift 2 ;;
		--timeout) TIMEOUT="$2"; shift 2 ;;
		--resource-dir) RESOURCE_DIR="$2"; shift 2 ;;
		--ui-language) UI_LANGUAGE="$2"; shift 2 ;;
		*) echo "live-look: unknown argument $1" >&2; exit 2 ;;
	esac
done

die2() { echo "live-look: $*" >&2; [ -n "$OUT" ] && [ -d "$OUT" ] && echo "error=$*" >> "$OUT/result.txt"; exit 2; }

for v in XBE EXE LINKMAP BASE STEAM XEMU MAP MPAK OUT; do
	[ -n "${!v}" ] || { echo "live-look: --$(echo "$v" | tr '[:upper:]' '[:lower:]') is required" >&2; exit 2; }
done
mkdir -p "$OUT" || exit 2
: > "$OUT/result.txt" || exit 2
for f in "$XBE" "$XBE.identity" "$EXE" "$LINKMAP" "$MPAK" "$XEMU" "$STEAM/$GAMEDIR/video.cfg" \
	"$BASE/default.xbe" "$BASE/default.xbe.identity" "$BASE/game-hl/xbe-build.exe" "$BASE/game-hl/xbe-build.map"; do
	[ -f "$f" ] || die2 "missing input $f"
done

PY="$(command -v python || command -v python3)" || die2 "no python"
# LIVE_LOOK_DBG / LIVE_LOOK_XISO: substitutes for the debugger and the
# packer, for --self-test only.
DBG() { if [ -n "${LIVE_LOOK_DBG:-}" ]; then "$LIVE_LOOK_DBG" "$@"; else "$PY" tools/xemu-debug.py "$@"; fi; }
sha() { sha256sum "$1" | cut -c1-64; }
native() { if command -v cygpath > /dev/null 2>&1; then cygpath -w "$1"; else printf '%s' "$1"; fi; }
now() { date +%s; }
# \r stripped: xemu-debug.py runs under Windows Python, whose lines may end
# CRLF, and a value copied from its last line would carry the \r into
# every exact comparison made on result.txt later (found in review).
kv() { printf '%s=%s\n' "$1" "$(printf '%s' "$2" | tr -d '\r')" >> "$OUT/result.txt"; }

# The overlay, and the scene key the baseline cache is filed under.
OV="$OUT/overlay"
rm -rf "$OV" && mkdir -p "$OV/$GAMEDIR/userconfig.d" || die2 "cannot build overlay"
if grep -q '^[[:space:]]*r_refdll' "$STEAM/$GAMEDIR/video.cfg"; then
	sed 's/^[[:space:]]*r_refdll.*/r_refdll "nv2a"/' "$STEAM/$GAMEDIR/video.cfg" > "$OV/$GAMEDIR/video.cfg"
else
	{ cat "$STEAM/$GAMEDIR/video.cfg"; echo 'r_refdll "nv2a"'; } > "$OV/$GAMEDIR/video.cfg"
fi
{ echo "map $MAP"; for c in ${CVARS[@]+"${CVARS[@]}"}; do echo "$c"; done; } > "$OV/$GAMEDIR/userconfig.d/00-live.cfg"
cp "$MPAK" "$OV/$GAMEDIR/" && cmp -s "$MPAK" "$OV/$GAMEDIR/$(basename "$MPAK")" || die2 "cannot copy $MPAK into the overlay"
# The key covers everything that decides the baseline frame: the baseline
# XBE itself (a rebuild in the same directory must not reuse an older
# binary's frame - found in review), the overlay, the disc's extra inputs,
# and the frame count.
KEY="$( { sha "$BASE/default.xbe"; sha "$OV/$GAMEDIR/video.cfg"; sha "$OV/$GAMEDIR/userconfig.d/00-live.cfg"; sha "$MPAK"; echo "frames=$FRAMES"; echo "resource-dir=$RESOURCE_DIR"; echo "ui-language=$UI_LANGUAGE"; } | sha256sum | cut -c1-16)"
kv scene_key "$KEY"
kv xbe_sha256 "$(sha "$XBE")"
kv baseline_xbe_sha256 "$(sha "$BASE/default.xbe")"

XEMU_PID=""
stop_xemu() {
	[ -n "$XEMU_PID" ] || return 0
	local winpid; winpid="$(cat "/proc/$XEMU_PID/winpid" 2>/dev/null || true)"
	kill "$XEMU_PID" 2>/dev/null
	for _ in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$XEMU_PID" 2>/dev/null || break; sleep 1; done
	kill -9 "$XEMU_PID" 2>/dev/null
	[ -n "$winpid" ] && command -v taskkill > /dev/null 2>&1 && taskkill //F //PID "$winpid" > /dev/null 2>&1
	wait "$XEMU_PID" 2>/dev/null
	XEMU_PID=""
}
trap stop_xemu EXIT

# boot <label> <xbe> <exe> <linkmap> <dir>: pack, boot, capture into <dir>.
# Returns 0 captured, 1 the build did not reach the scene or was not the
# guest's image, 2 could not attempt.
boot() {
	local label="$1" xbe="$2" exe="$3" lmap="$4" dir="$5" t0 rc
	# Fresh every time: a frame left by an earlier look must never sit
	# beside a failed one (found by this script's own self-test).
	rm -rf "$dir" && mkdir -p "$dir" || return 2
	if DBG regs > /dev/null 2>&1; then
		echo "live-look: a GDB stub already answers - another xemu is running; refusing to measure it." >&2
		return 2
	fi
	t0="$(now)"
	local iso="$REPO/.sweep-xiso/live-$label.iso"
	local pack=( hl --steam "$STEAM" --xbe "$xbe" --out "$iso" --overlay "$OV" )
	[ -n "$RESOURCE_DIR" ] && pack+=( --resource-dir "$RESOURCE_DIR" )
	[ -n "$UI_LANGUAGE" ] && pack+=( --ui-language "$UI_LANGUAGE" )
	bash "${LIVE_LOOK_XISO:-tools/xbox-xiso.sh}" "${pack[@]}" > "$dir/xiso.log" 2>&1 || { echo "live-look: packing $label failed, see $dir/xiso.log" >&2; return 2; }
	kv "${label}_pack_seconds" "$(( $(now) - t0 ))"

	t0="$(now)"
	"$XEMU" -s -dvd_path "$(native "$iso")" > "$dir/xemu.log" 2>&1 &
	XEMU_PID=$!
	# The stub answers before the XBE is loaded: poll until the guest runs
	# exactly this exe (exit 0), up to 120 s. A persistent mismatch is the
	# wrong image, not a slow one.
	local deadline=$(( $(now) + ${LIVE_LOOK_IDENTITY_WAIT:-120} ))
	rc=2
	while [ "$(now)" -lt "$deadline" ]; do
		kill -0 "$XEMU_PID" 2>/dev/null || { echo "live-look: xemu exited during $label boot, see $dir/xemu.log" >&2; XEMU_PID=""; return 2; }
		DBG verify-image "$exe" > "$dir/verify-image.log" 2>&1; rc=$?
		[ "$rc" -eq 0 ] && break
		sleep 3
	done
	kv "${label}_identity" "$(tail -n 1 "$dir/verify-image.log")"
	[ "$rc" -eq 0 ] || { stop_xemu; return 1; }

	DBG wait-scene --map "$lmap" --exe "$exe" --frames "$FRAMES" --timeout "$TIMEOUT" > "$dir/wait-scene.log" 2>&1; rc=$?
	kv "${label}_scene" "$(tail -n 1 "$dir/wait-scene.log")"
	# wait-scene: 1 is a timeout (the build's), 2 is unreadable (the machine's).
	# ...unless xemu itself is gone: an emulator that died under this build
	# (an NV2A assert, say) is the build's defect, 1 (found in review).
	if [ "$rc" -ne 0 ]; then
		local died=0; kill -0 "$XEMU_PID" 2>/dev/null || died=1
		stop_xemu
		[ "$rc" -eq 2 ] && [ "$died" -eq 0 ] && return 2
		return 1
	fi
	kv "${label}_boot_seconds" "$(( $(now) - t0 ))"

	DBG fbdump "$dir/frame.ppm" > "$dir/fbdump.log" 2>&1 \
		&& DBG screenshot "$dir/frame.png" > "$dir/screenshot.log" 2>&1 \
		|| { echo "live-look: $label capture failed, see $dir/fbdump.log / screenshot.log" >&2; stop_xemu; return 2; }
	stop_xemu
	kv "${label}_ppm_sha256" "$(sha "$dir/frame.ppm")"
	kv "${label}_png_sha256" "$(sha "$dir/frame.png")"
	kv "${label}_png" "$dir/frame.png"
	rm -f "$iso"
	return 0
}

# Baseline first, from the cache when this scene was looked at before on
# this origin/main. Only a complete entry counts: it is written into a
# temporary directory and renamed into place after capture.
BL="$BASE/look-$KEY"
if [ -f "$BL/frame.ppm" ] && [ -f "$BL/frame.png" ] && [ -f "$BL/scene.txt" ]; then
	kv baseline_cached yes
	kv baseline_scene "$(cat "$BL/scene.txt")"
else
	kv baseline_cached no
	rm -rf "$BL.tmp"
	boot baseline "$BASE/default.xbe" "$BASE/game-hl/xbe-build.exe" "$BASE/game-hl/xbe-build.map" "$BL.tmp"; rc=$?
	# origin/main not reaching the scene is not the branch's defect, but it
	# leaves nothing to compare against: no look.
	[ "$rc" -eq 0 ] || die2 "the baseline (origin/main) build did not reach the scene (exit $rc) - see $BL.tmp"
	tail -n 1 "$BL.tmp/wait-scene.log" > "$BL.tmp/scene.txt"
	rm -rf "$BL" && mv "$BL.tmp" "$BL" || die2 "cannot file the baseline look under $BL"
fi
kv baseline_ppm_sha256 "$(sha "$BL/frame.ppm")"
kv baseline_png "$BL/frame.png"

boot branch "$XBE" "$EXE" "$LINKMAP" "$OUT/branch"; rc=$?
if [ "$rc" -ne 0 ]; then
	kv verdict "no-look (branch boot exit $rc)"
	exit "$rc"
fi

DBG ppmdiff "$BL/frame.ppm" "$OUT/branch/frame.ppm" > "$OUT/ppmdiff.log" 2>&1 || die2 "ppmdiff could not compare, see $OUT/ppmdiff.log"
kv ppmdiff "$(tail -n 1 "$OUT/ppmdiff.log")"
echo "LIVE-LOOK-OK" >> "$OUT/result.txt"
cat "$OUT/result.txt"
exit 0
