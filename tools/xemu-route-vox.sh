#!/usr/bin/env bash
# The dialogue route (X12 item 4, divergence #177): c1a0 -> c1a0e -> c1a1,
# recorded by the frame-time recorder, with no input, built to put the
# dialogue stream's cases in one session:
#   c1a0   Barney's desk line (barney/c1a0_ba_desk, 182 KiB), an
#          announcement (vox words), the suit's logon (fvox/HEV_logon);
#   c1a0e  two scientists at once (c1a0_sci_dis1a, 194 KiB, and dis2a),
#          a save in the middle of their lines and its load (the restore
#          seeks each window), then two more lines, and the level change
#          to c1a1 while the last one is still speaking;
#   c1a1   the alarms' announcements, then the recorder stops.
# The lines are played with `spk` (S_StartLocalSound: the sentence path
# a scripted_sentence takes, on the player's own channel), so the route
# does not depend on where an NPC is standing. The real scenes, with lip
# sync on the NPCs, are the console procedure's manual pass
# (docs/r3d/x12-vox-console-runs.md).
#
#   tools/xemu-route-vox.sh <label> <xbe> [<cfg line>]...
#     e.g. tools/xemu-route-vox.sh route-vox-a .sweep/game-hl/main.xbe
#          tools/xemu-route-vox.sh route-vox-b .sweep/game-hl/voxstream.xbe
#          tools/xemu-route-vox.sh route-vox-c .sweep/game-hl/voxstream.xbe "r3d_vox_stream 0"
#
# What it does, against the staged disc tree .sweep-xiso/hl (build it with
# tools/xbox-xiso.sh first), as tools/xemu-route-bmi.sh does:
#   1. refuses to run if any xemu is running (only ever closes one it
#      started), and refuses a stage with a test cfg it did not write;
#   2. puts <xbe> in as default.xbe, and four route files:
#        valve/userconfig.d/90-route-vox.cfg  the label, recorder on, the
#                                             extra cfg lines, the c1a0e
#                                             steps as aliases, `map c1a0`
#        valve/maps/c1a0_load.cfg, c1a0e_load.cfg, c1a1_load.cfg
#                                             each map's part, run by
#                                             upstream's SV_SpawnServer
#                                             (`exec maps/%s_load.cfg`,
#                                             sv_init.c), on a load from a
#                                             save too;
#   3. packs .sweep-xiso/route-vox.iso (hl.iso is left alone), boots it
#      with -dvd_path, lets it run, closes xemu gracefully (never a kill:
#      docs/r3d/lessons.md, qcow2 clusters), fetches the session with
#      tools/frametime-report.py fetch-xemu --label <label>;
#   4. removes the route files from the stage again;
#   5. decides from the recording itself whether the route ran, with
#      `frametime-report.py check`, and exits nonzero when it did not:
#      the session must end with an END chunk, play c1a0 -> c1a0e -> c1a1
#      in that order (the save loaded on c1a0e is not a new step), start
#      dialogue streams on c1a0 and c1a0e (`vox_starts`, a build with
#      the stream; ROUTE_VOX_MIN replaces those), and be followed by the
#      end marker: c1a1's cfg, after stopping the recorder, records a
#      short second session labelled <label>-end, so its presence says the
#      end cfg ran to its last line. A fixed sleep and a fetched file say
#      nothing alone - a route stalled on c1a0 still leaves a recording.
#      The label must be new on the xemu disk (an old session of the same
#      label would be checked instead of this run's) and at most 18
#      characters (the recorder keeps 22, and "-end" is added).
#
# The save and the load. c1a0e_load.cfg runs `vox_c1a0e`, an alias that
# is step 1 the first time and points itself at step 2: step 1 plays the
# two lines, saves (route_vox_mid) and loads that save, and ends there -
# nothing is queued after a `load` (docs/r3d/lessons.md: a command the
# engine follows up through the command buffer). The load spawns c1a0e
# again, which runs c1a0e_load.cfg again, now step 2.
#
# On the console: copy the same four files, with the same lines, to
# E:\UDATA\<title id>\valve\valve\userconfig.d\ and ...\valve\maps\ (or
# onto the disc tree before packing), boot, wait about 6 minutes, then
# `tools/frametime-report.py fetch --host <ip> --label <label>`; delete
# the files (and the route_vox_mid save) afterwards.
#
# xemu timing is ratios only (docs/r3d/lessons.md), and xemu does not run
# the audio callback, so the mixer paints little there: read the vox_*
# counters (starts, opens, reads, waits) and the sound and vox notes, not
# the sound spans.
set -u

LABEL=${1:?usage: xemu-route-vox.sh <label> <xbe> [<cfg line>]...}
XBE=${2:?usage: xemu-route-vox.sh <label> <xbe> [<cfg line>]...}
shift 2
RUN_SECS=${ROUTE_VOX_SECONDS:-420}
MAPS="c1a0,c1a0e,c1a1"
MINS=${ROUTE_VOX_MIN:-"c1a0:vox_starts:1 c1a0e:vox_starts:4"}
REPO="$(cd "$(dirname "$0")/.." && pwd)"
STAGE="$REPO/.sweep-xiso/hl"
ISO="$REPO/.sweep-xiso/route-vox.iso"
OUT="${ROUTE_VOX_OUT:-$REPO/.sweep/route-vox/$LABEL}"
CFG="$STAGE/valve/userconfig.d/90-route-vox.cfg"
MAPCFG_A="$STAGE/valve/maps/c1a0_load.cfg"
MAPCFG_E="$STAGE/valve/maps/c1a0e_load.cfg"
MAPCFG_1="$STAGE/valve/maps/c1a1_load.cfg"
XEMU=${XEMU:-/g/Games/Xemu/xemu.exe}
die() { echo "xemu-route-vox: $*" >&2; exit 1; }
winpath() { cygpath -w "$1" 2>/dev/null || echo "$1"; }
xemu_pids() { powershell -NoProfile -Command "(Get-Process xemu -ErrorAction SilentlyContinue | ForEach-Object { \$_.Id }) -join ' '" | tr -d '\r'; }
cleanup() { rm -f "$CFG" "$MAPCFG_A" "$MAPCFG_E" "$MAPCFG_1"; }

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
for f in "$MAPCFG_A" "$MAPCFG_E" "$MAPCFG_1"; do
	[ -e "$f" ] && die "$f is already in the stage"
done
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
	echo "// tools/xemu-route-vox.sh ($LABEL): remove after the run"
	echo "host_frametime_label \"$LABEL\""
	echo "host_frametime_analysis 2"
	for line in "$@"; do echo "$line"; done
	echo "alias vox_c1a0e vox_c1a0e_1"
	echo "alias vox_c1a0e_1 \"alias vox_c1a0e vox_c1a0e_2; wait 300; spk !SC_DIS1A; wait 150; spk !SC_DIS2A; wait 240; save route_vox_mid; wait 60; load route_vox_mid\""
	echo "alias vox_c1a0e_2 \"wait 400; spk !SC_DIS3A; wait 500; spk !SC_DIS4A; wait 120; map c1a1\""
	echo "map c1a0"
} > "$CFG" || die "cfg"
mkdir -p "$STAGE/valve/maps"
{
	echo "// tools/xemu-route-vox.sh: c1a0's part"
	echo "wait 300"
	echo "spk !BA_DESK"
	echo "wait 900"
	echo "spk !C1A0_9"
	echo "wait 900"
	echo "spk !HEV_A0"
	echo "wait 400"
	echo "map c1a0e"
} > "$MAPCFG_A" || die "c1a0 cfg"
{
	echo "// tools/xemu-route-vox.sh: c1a0e's part (step 1, then after the load step 2)"
	echo "vox_c1a0e"
} > "$MAPCFG_E" || die "c1a0e cfg"
{
	echo "// tools/xemu-route-vox.sh: the route's end point on c1a1"
	echo "wait 1500"
	echo "host_frametime_analysis 0"
	echo "wait 10"
	echo "// the end marker: a short session of its own, checked after the run"
	echo "host_frametime_label \"$LABEL-end\""
	echo "host_frametime_analysis 1"
	echo "wait 30"
	echo "host_frametime_analysis 0"
	echo "echo route-vox: done"
} > "$MAPCFG_1" || die "c1a1 cfg"
cat "$CFG" "$MAPCFG_A" "$MAPCFG_E" "$MAPCFG_1"

"$REPO/deps/nxdk/tools/extract-xiso/build/extract-xiso.exe" -c "$(winpath "$STAGE")" "$(winpath "$ISO")" > "$OUT/xiso.log" 2>&1 \
	|| die "extract-xiso failed (see $OUT/xiso.log)"

"$XEMU" -dvd_path "$(winpath "$ISO")" > "$OUT/xemu.out" 2>&1 &
sleep 15
PIDS=$(xemu_pids)
[ -n "$PIDS" ] || die "xemu did not start (see $OUT/xemu.out) - not reading the disk"
echo "xemu-route-vox: xemu $PIDS running $ISO; letting the route run ${RUN_SECS}s"
sleep "$RUN_SECS"
[ "$(xemu_pids)" = "$PIDS" ] || die "xemu $PIDS is gone or another started ($(xemu_pids)) - not reading the disk"

powershell -NoProfile -Command "Get-Process -Id $(echo $PIDS | tr ' ' ',') | ForEach-Object { [void]\$_.CloseMainWindow() }; Get-Process -Id $(echo $PIDS | tr ' ' ',') -ErrorAction SilentlyContinue | Wait-Process -Timeout 60"
[ -z "$(xemu_pids)" ] || die "xemu did not close within 60 s - not reading the disk"

cd "$REPO" || exit 1
python tools/frametime-report.py fetch-xemu --out "$OUT" --label "$LABEL" || die "fetch-xemu found nothing for $LABEL"
python tools/frametime-report.py fetch-xemu --out "$OUT" --label "$LABEL-end" \
	|| echo "xemu-route-vox: no end-marker session $LABEL-end on the disk (check reports it)" >&2
python tools/frametime-report.py summary "$OUT" --label "$LABEL" > "$OUT/summary.txt" 2>&1 \
	|| die "summary failed (see $OUT/summary.txt)"
set -f
# shellcheck disable=SC2086 # MINS is a list of words
MIN_ARGS=$(for m in $MINS; do printf ' --min %s' "$m"; done)
python tools/frametime-report.py check "$OUT" --label "$LABEL" --end-label "$LABEL-end" --maps "$MAPS" $MIN_ARGS \
	> "$OUT/check.txt" 2>&1 || { cat "$OUT/check.txt" >&2; die "the route did not run (see $OUT/check.txt, $OUT/summary.txt)"; }
set +f
cat "$OUT/check.txt"
echo "xemu-route-vox: done, $OUT"
