#!/usr/bin/env bash
# Black Mesa Inbound: the fixed audio benchmark (X12, owner request
# 2026-10-10). The tram ride c0a0 -> c0a0a -> c0a0b -> c0a0c -> c0a0d ->
# c0a0e, recorded by the frame-time recorder, with no input: the opening
# music, the announcement system's VOX sentences, the ambient loops and
# the tram's moving sounds, then on c0a0e the guard's scene - his greeting
# (scripted_sentence !BA_LATE, sound/barney/c1a0_ba_late.wav), the button,
# the tram door opening - and the security door's keypad and unlock
# sequence. Every X12 step records this same route, before and after.
#
#   tools/xemu-route-bmi.sh <label> <xbe> [<cfg line>]...
#     e.g. tools/xemu-route-bmi.sh bmi-music-a .sweep/game-hl/main-d0d8fed4.xbe
#          tools/xemu-route-bmi.sh bmi-music-b .sweep/game-hl/musicbake.xbe "r3d_music_baked 1"
#
# What it does, against the staged disc tree .sweep-xiso/hl (build it with
# tools/xbox-xiso.sh first; a step that needs baked data - the music's
# --music-bake - must already be in it):
#   1. refuses to run if any xemu is running (only ever closes one it
#      started), and refuses a stage with a test cfg it did not write;
#   2. puts <xbe> in as default.xbe, and two route files:
#        valve/userconfig.d/90-route-bmi.cfg  the label, recorder on, the
#                                             extra cfg lines, `map c0a0`
#        valve/maps/c0a0e_load.cfg            the end point, run by
#                                             upstream's SV_SpawnServer
#                                             (`exec maps/%s_load.cfg`,
#                                             sv_init.c) when c0a0e loads;
#   3. packs .sweep-xiso/route-bmi.iso (hl.iso is left alone), boots it
#      with -dvd_path, lets it run, closes xemu gracefully (never a kill:
#      docs/r3d/lessons.md, qcow2 clusters), fetches the session with
#      tools/frametime-report.py fetch-xemu --label <label>;
#   4. removes the two route files from the stage again;
#   5. decides from the recording itself whether the route ran, with
#      `frametime-report.py check`, and exits nonzero when it did not:
#      the session must end with an END chunk, play c0a0 -> c0a0a ->
#      c0a0b -> c0a0c -> c0a0d -> c0a0e in that order, start the guard's
#      line as a dialogue stream on c0a0e (`vox_starts`, required only
#      from a build that has the counter; ROUTE_BMI_MIN replaces it), and
#      be followed by the end marker: c0a0e's cfg, after stopping the
#      recorder, records a short second session labelled <label>-end, so
#      its presence says the end cfg ran to its last line. A fixed sleep
#      and a fetched file say nothing alone - a tram stalled on c0a0b
#      still leaves a recording. The label must be new on the xemu disk
#      (an old session of the same label would be checked instead of this
#      run's) and at most 18 characters (the recorder keeps 22, and "-end"
#      is added).
#
# The end point. Barney's scene on c0a0e is scripted from the map's start
# (trigger_auto -> barnwalkmm -> barnwalk +2 s -> barnspeakmm: the
# sentence, then barnwalk1 +2 s -> his intropush -> traindoormm: the door,
# its sounds, barnwalk2). It needs no input. The security door (barnwalk3
# -> entrymm: the keypad blips, the bolts, the doors) waits for the player
# to walk into a trigger_once; the route fires barnwalk3 itself instead,
# deterministically (`ent_fire barnwalk3 use`, sv_enttools_enable 1), once
# the tram door has opened. The waits are frames (`wait`): 2700 (45 s at
# xemu's and the console's 60 fps) for the tram door, 1200 (20 s) for the
# security door; then the recorder stops. The waits sit in the command
# buffer, so anything the game queues there meanwhile (c0a0e's
# trigger_autosave) runs after the recording, on every run alike.
#
# On the console: copy the same two files, with the same lines, to
# E:\UDATA\<title id>\valve\valve\userconfig.d\90-route-bmi.cfg and
# ...\valve\maps\c0a0e_load.cfg (or onto the disc tree before packing),
# boot, wait about 7 minutes, then `tools/frametime-report.py fetch
# --host <ip> --label <label>`; delete both files afterwards. The label is
# set by the cfg, so nobody has to type it.
#
# xemu timing is ratios only (docs/r3d/lessons.md); and xemu does not run
# the audio callback, so the mixer and the music pump do almost no work
# there - read the span call counts and the music notes before reading a
# sound time (`summary`'s notes list names every music open: xwav or mp3).
set -u

LABEL=${1:?usage: xemu-route-bmi.sh <label> <xbe> [<cfg line>]...}
XBE=${2:?usage: xemu-route-bmi.sh <label> <xbe> [<cfg line>]...}
shift 2
RUN_SECS=${ROUTE_BMI_SECONDS:-540}
MAPS="c0a0,c0a0a,c0a0b,c0a0c,c0a0d,c0a0e"
MINS=${ROUTE_BMI_MIN:-"c0a0e:vox_starts:1?"}
REPO="$(cd "$(dirname "$0")/.." && pwd)"
STAGE="$REPO/.sweep-xiso/hl"
ISO="$REPO/.sweep-xiso/route-bmi.iso"
OUT="${ROUTE_BMI_OUT:-$REPO/.sweep/route-bmi/$LABEL}"
CFG="$STAGE/valve/userconfig.d/90-route-bmi.cfg"
ENDCFG="$STAGE/valve/maps/c0a0e_load.cfg"
XEMU=${XEMU:-/g/Games/Xemu/xemu.exe}
die() { echo "xemu-route-bmi: $*" >&2; exit 1; }
winpath() { cygpath -w "$1" 2>/dev/null || echo "$1"; }
xemu_pids() { powershell -NoProfile -Command "(Get-Process xemu -ErrorAction SilentlyContinue | ForEach-Object { \$_.Id }) -join ' '" | tr -d '\r'; }
cleanup() { rm -f "$CFG" "$ENDCFG"; }

case "$LABEL" in
*[!A-Za-z0-9_-]*|[-_]*) die "label '$LABEL': letters, digits, '-' and '_' only, not starting with '-' or '_'" ;;
esac
[ ${#LABEL} -le 18 ] || die "label '$LABEL' is over 18 characters (the recorder keeps 22, and '-end' is added)"
[ -f "$XBE" ] || die "no xbe $XBE"
[ -d "$STAGE/valve" ] || die "no staged disc tree at $STAGE (run tools/xbox-xiso.sh first)"
[ -z "$(xemu_pids)" ] || die "an xemu is already running ($(xemu_pids)) - not mine, leaving it alone"
for f in "$STAGE"/valve/userconfig.d/*.cfg; do
	[ -e "$f" ] || continue
	case "$(basename "$f")" in
	00-c1a0.cfg) grep -qv '^//' "$f" && die "$f has commands in it - a test cfg left behind? (it should only boot to the menu)" ;;
	*) die "$f is in the stage - a test cfg left behind? remove it first" ;;
	esac
done
[ -e "$ENDCFG" ] && die "$ENDCFG is already in the stage"
mkdir -p "$OUT" || die "no $OUT"
ls "$OUT"/ft_*.bin > /dev/null 2>&1 && die "$OUT already has recordings - pick a new label or folder"
# The label must be new on the disk: check would otherwise read an old run.
for l in "$LABEL" "$LABEL-end"; do
	python "$REPO/tools/frametime-report.py" fetch-xemu --out "$OUT/.precheck" --label "$l" > /dev/null 2>&1 \
		&& { rm -rf "$OUT/.precheck"; die "the xemu disk already has sessions labelled $l - pick a new label"; }
done
rm -rf "$OUT/.precheck"
trap cleanup EXIT

cp "$XBE" "$STAGE/default.xbe" && cmp -s "$XBE" "$STAGE/default.xbe" || die "copying the xbe failed"
{
	echo "// tools/xemu-route-bmi.sh ($LABEL): remove after the run"
	echo "host_frametime_label \"$LABEL\""
	echo "host_frametime_analysis 2"
	for line in "$@"; do echo "$line"; done
	echo "map c0a0"
} > "$CFG" || die "cfg"
mkdir -p "$STAGE/valve/maps"
{
	echo "// tools/xemu-route-bmi.sh: the route's end point on c0a0e"
	echo "sv_enttools_enable 1"
	echo "wait 2700"
	echo "ent_fire barnwalk3 use"
	echo "wait 1200"
	echo "host_frametime_analysis 0"
	echo "wait 10"
	echo "// the end marker: a short session of its own, checked after the run"
	echo "host_frametime_label \"$LABEL-end\""
	echo "host_frametime_analysis 1"
	echo "wait 30"
	echo "host_frametime_analysis 0"
	echo "echo route-bmi: done"
} > "$ENDCFG" || die "end cfg"
cat "$CFG" "$ENDCFG"

"$REPO/deps/nxdk/tools/extract-xiso/build/extract-xiso.exe" -c "$(winpath "$STAGE")" "$(winpath "$ISO")" > "$OUT/xiso.log" 2>&1 \
	|| die "extract-xiso failed (see $OUT/xiso.log)"

"$XEMU" -dvd_path "$(winpath "$ISO")" > "$OUT/xemu.out" 2>&1 &
sleep 15
PIDS=$(xemu_pids)
[ -n "$PIDS" ] || die "xemu did not start (see $OUT/xemu.out) - not reading the disk"
echo "xemu-route-bmi: xemu $PIDS running $ISO; letting the route run ${RUN_SECS}s"
sleep "$RUN_SECS"
[ "$(xemu_pids)" = "$PIDS" ] || die "xemu $PIDS is gone or another started ($(xemu_pids)) - not reading the disk"

powershell -NoProfile -Command "Get-Process -Id $(echo $PIDS | tr ' ' ',') | ForEach-Object { [void]\$_.CloseMainWindow() }; Get-Process -Id $(echo $PIDS | tr ' ' ',') -ErrorAction SilentlyContinue | Wait-Process -Timeout 60"
[ -z "$(xemu_pids)" ] || die "xemu did not close within 60 s - not reading the disk"

cd "$REPO" || exit 1
python tools/frametime-report.py fetch-xemu --out "$OUT" --label "$LABEL" || die "fetch-xemu found nothing for $LABEL"
python tools/frametime-report.py fetch-xemu --out "$OUT" --label "$LABEL-end" \
	|| echo "xemu-route-bmi: no end-marker session $LABEL-end on the disk (check reports it)" >&2
python tools/frametime-report.py summary "$OUT" --label "$LABEL" > "$OUT/summary.txt" 2>&1 \
	|| die "summary failed (see $OUT/summary.txt)"
set -f
# shellcheck disable=SC2086 # MINS is a list of words
MIN_ARGS=$(for m in $MINS; do printf ' --min %s' "$m"; done)
python tools/frametime-report.py check "$OUT" --label "$LABEL" --end-label "$LABEL-end" --maps "$MAPS" $MIN_ARGS \
	> "$OUT/check.txt" 2>&1 || { cat "$OUT/check.txt" >&2; die "the route did not run (see $OUT/check.txt, $OUT/summary.txt)"; }
set +f
cat "$OUT/check.txt"
echo "xemu-route-bmi: done, $OUT"
