# xbox-opt.sh - the Xbox build's optimisation level, in one place.
#
# Sourced (never run) by tools/xbox-cc.sh and tools/xbox-cxx.sh, which put
# "$XBOX_OPT" on every compile, and by tools/xbox-sweep.sh,
# tools/xbox-mainui-sweep.sh and tools/xbox-link-check.sh, which record it
# in the sweep manifests and refuse to link objects swept at another level.
#
#   XBOX_OPT   -O2 (default), or -O0 -O1 -O3 -Os -Oz -Og
#
# Override it with `env`, which survives the explicit
# /c/msys64/usr/bin/bash invocation that drops an `export` from this dev
# shell (docs/r3d/lessons.md):
#
#   env XBOX_OPT=-O0 /c/msys64/usr/bin/bash tools/xbox-sweep.sh ...
#
# Why -O2: until 2026-09-29 no Xbox compile passed any -O flag, so every
# object was clang's default -O0 - not a decision, an omission (nothing in
# git history or docs chose it; PR #162 found it disassembling a per-texel
# helper). The reference PC build is -O3 (scripts/waifulib/
# compiler_optimizations.py, the 'humanrights' release type).
#
# -O2 over -Os, measured 2026-09-30 (xemu, c1a0e; docs/r3d/fork-plan.md X11):
#   XBE bytes        -O0 6881280   -O2 5492736   -Os 4886528
#   Image pages      -O0 3332      -O2 2994      -Os 2846 (11.70 vs 11.12 MiB)
#   lightmap rebuild -O0 235.7k    -O2 128.9k    -Os 135.2k cycles (median)
#   lightmap idle    -O0 75k       -O2 38.6k     -Os 43.0k  cycles (median)
# -Os saves 0.58 MiB resident and costs 5-11% CPU; the hardware is CPU-
# bound (44 fps at -O0) with 22-26 MiB free at each map start, so -O2.
#
# Debug info (-g) is deliberately not added: tools/xemu-debug.py resolves
# symbols from the lld-link map and labels raw stack dwords against it; it
# never walks EBP chains or reads DWARF, so neither -g nor frame pointers
# buy it anything. -g would also make tools/xbox-cc-cache.sh bypass every
# compile (paths reach the object through debug info).
XBOX_OPT="${XBOX_OPT:--O2}"
case "$XBOX_OPT" in
	-O0|-O1|-O2|-O3|-Os|-Oz|-Og) ;;
	*)
		echo "xbox-opt: XBOX_OPT='$XBOX_OPT' is not one of -O0 -O1 -O2 -O3 -Os -Oz -Og" >&2
		exit 2
		;;
esac
