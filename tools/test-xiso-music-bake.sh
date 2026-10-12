#!/usr/bin/env bash
# tools/xbox-xiso.sh's validate_music_bake (divergence #171) against a real
# bake and against corrupted copies of it: every failure case must fail
# with its own message, the real bake must pass with its track count.
#
#   MSYSTEM=MINGW64 /c/msys64/usr/bin/bash tools/test-xiso-music-bake.sh [<bake dir> [<staged gamedir>]]
#
# Defaults: .sweep/music-bake (tools/asset-studio --music) and the staged
# disc's .sweep-xiso/hl/valve, whose media/ must hold the retail mp3s the
# bake was made from. A 48 kHz bake (--music-rate 48000, #173: files
# <Track>.48k.xwav named by its manifest's "# file" line) runs the same
# cases, plus one with that line removed; with SOURCE_RATE_BAKE=<dir of
# the 44.1 kHz bake> also one with that bake's real file under the 48 kHz
# name. Run in MSYS2 bash, the shell xbox-xiso.sh is
# documented for (docs/r3d/lessons.md: a pass under another shell is not
# a pass). Exit 0 iff every case did what it should.
R="$(cd "$(dirname "$0")/.." && pwd)"
T=$R/.sweep/music-xiso-test
BAKE=${1:-$R/.sweep/music-bake}
GD=${2:-$R/.sweep-xiso/hl/valve}
[ -f "$BAKE/music_manifest.txt" ] || { echo "harness: no bake at $BAKE"; exit 1; }
N=$(ls "$BAKE"/media/*.xwav | wc -l)
rm -rf "$T"; mkdir -p "$T"
awk '/^le32_at\(\) \{/{on=1} /^MUSIC_STAGED=/{on=0} on' "$R/tools/xbox-xiso.sh" > "$T/vmb.sh"
. "$T/vmb.sh"
type validate_music_bake > /dev/null 2>&1 || { echo "harness: validate_music_bake not extracted - broken harness"; exit 1; }
ok=0; bad=0
expect() { # <want 0|1> <label> <message pattern> <cmd...>
	local want=$1 label=$2 pat=$3; shift 3
	out=$("$@" 2>$T/vmb.err); rc=$?
	if { [ "$want" = 0 ] && [ $rc -eq 0 ] && [ "$out" = "$pat" ]; } || { [ "$want" = 1 ] && [ $rc -eq 1 ] && grep -q -- "$pat" $T/vmb.err; }; then
		echo "pass: $label (rc $rc) $out $(head -1 $T/vmb.err)"; ok=$((ok+1))
	else
		echo "FAIL: $label (rc $rc) $out $(cat $T/vmb.err)"; bad=$((bad+1))
	fi
}
expect 0 "the real bake" "$N" validate_music_bake "$BAKE" "$GD"

cp -r "$BAKE" "$T/trunc"; f=$(ls "$T"/trunc/media/*.xwav | head -1); head -c 100000 "$f" > "$f.tmp" && mv "$f.tmp" "$f"
expect 1 "a truncated track" "truncated or corrupt" validate_music_bake "$T/trunc" "$GD"

cp -r "$BAKE" "$T/failed"; sed -i '0,/ ok$/s/ ok$/ FAILED/' "$T/failed/music_manifest.txt"
expect 1 "a FAILED manifest row" "FAILED - the bake failed" validate_music_bake "$T/failed" "$GD"

cp -r "$BAKE" "$T/extra"; cp "$(ls "$T"/extra/media/*.xwav | head -1)" "$T/extra/media/Extra.xwav"
expect 1 "an unlisted .xwav" "does not list" validate_music_bake "$T/extra" "$GD"

cp -r "$BAKE" "$T/missing"; rm "$(ls "$T"/missing/media/*.xwav | tail -1)"
expect 1 "a listed track missing" "does not exist" validate_music_bake "$T/missing" "$GD"

SUFFIX=$(tr -d '\r' < "$BAKE/music_manifest.txt" | grep -E '^# file ' | tail -1 | awk '{ print $3 }'); [ -n "$SUFFIX" ] || SUFFIX=.xwav
mkdir -p "$T/gd/media"; cp "$GD"/media/*.mp3 "$T/gd/media/"; v=$(basename "$(ls "$BAKE"/media/*.xwav | head -1)" "$SUFFIX"); printf 'x' >> "$T/gd/media/$v.mp3"
expect 1 "a source mp3 of another size" "$v.mp3; the staged one" validate_music_bake "$BAKE" "$T/gd"
rm "$T/gd/media/$v.mp3"
expect 1 "a source mp3 missing" "$v.mp3; the staged one" validate_music_bake "$BAKE" "$T/gd"

cp -r "$BAKE" "$T/notrailer"; sed -i '/^# result ok/d' "$T/notrailer/music_manifest.txt"
expect 1 "no result line (a run that failed as a whole)" "did not succeed as a whole" validate_music_bake "$T/notrailer" "$GD"

cp -r "$BAKE" "$T/badcount"; sed -i "s/^# result ok .*/# result ok $((N - 1))/" "$T/badcount/music_manifest.txt"
expect 1 "a result line with another count" "did not succeed as a whole" validate_music_bake "$T/badcount" "$GD"

cp -r "$BAKE" "$T/chunk"; f=$(ls "$T"/chunk/media/*.xwav | head -1); o=$(head -c 512 "$f" | grep -obUa r3dm | head -1 | cut -d: -f1)
printf '\x00\x10\x00\x00' | dd of="$f" bs=1 seek=$((o + 8 + 24)) conv=notrunc 2>/dev/null
expect 1 "an r3dm chunk size the engine refuses" "the engine plays only 8192" validate_music_bake "$T/chunk" "$GD"

mkdir -p "$T/nomanifest/media"
expect 1 "no manifest" "has no music_manifest.txt" validate_music_bake "$T/nomanifest" "$GD"

# a file whose sample rate is not the one its name promises (#173): a
# source-rate (44.1 kHz) track renamed to the 48 kHz name, or the reverse
cp -r "$BAKE" "$T/rate"; f=$(ls "$T"/rate/media/*.xwav | head -1); o=$(head -c 512 "$f" | grep -obUa 'fmt ' | head -1 | cut -d: -f1)
if [ "$SUFFIX" = .xwav ]; then printf '\x80\xbb\x00\x00' > "$T/rate.bin"; else printf '\x44\xac\x00\x00' > "$T/rate.bin"; fi
dd if="$T/rate.bin" of="$f" bs=1 seek=$((o + 8 + 4)) conv=notrunc 2>/dev/null
expect 1 "a file whose fmt rate is not its name's" "sample rate" validate_music_bake "$T/rate" "$GD"
if [ -n "${SOURCE_RATE_BAKE:-}" ] && [ "$SUFFIX" != .xwav ]; then
	# the case as it would happen: the source-rate bake's real file under the 48 kHz name
	cp -r "$BAKE" "$T/renamed"; f=$(ls "$T"/renamed/media/*"$SUFFIX" | head -1); n=$(basename "$f" "$SUFFIX")
	cp "$SOURCE_RATE_BAKE/media/$n.xwav" "$f"
	expect 1 "a source-rate $n.xwav renamed to $n$SUFFIX" "sample rate 44100 Hz" validate_music_bake "$T/renamed" "$GD"
fi

if [ "$SUFFIX" != .xwav ]; then
	cp -r "$BAKE" "$T/nofile"; sed -i '/^# file /d' "$T/nofile/music_manifest.txt"
	expect 1 "a $SUFFIX bake whose manifest lost its # file line" "does not exist" validate_music_bake "$T/nofile" "$GD"
fi
echo "harness: $ok passed, $bad failed"
rm -rf "$T"
[ $bad -eq 0 ]
