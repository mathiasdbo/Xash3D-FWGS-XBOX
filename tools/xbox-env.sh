#!/usr/bin/env sh
# Build environment for nxdk and the Xbox target.
#
# Do not source this from Git Bash. See "Which shell" below — that is the whole
# point of this file.
#
#   Build nxdk:
#     MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc '
#       cd <repo>/deps/nxdk && eval "$(./bin/activate -s)" && make NXDK_ONLY=y -j8'
#
# Verified working 2026-09-05: 19 libraries, including libSDL2, libpbkit,
# libpdclib, libnxdk_hal, libSDL_ttf and libc++.
#
# That command passes no CFLAGS, so the libraries are -O0. To build them at
# -O2 (or any level) and record the level for the link check, run
# `bash tools/xbox-build-nxdk.sh` instead (it wraps the same make).

# --- Which shell -------------------------------------------------------------
#
# **MSYS2 MinGW-64, not Git Bash.** nxdk expects `/mingw64/bin` on PATH, which
# Git Bash does not have. MSYS2 ships clang 21.1.8 there along with lld-link,
# llvm-lib, llvm-ar and llvm-objcopy.
#
# A separate LLVM in "C:/Program Files/LLVM" is NOT a substitute. It will
# compile individual files and then fail the build in ways that look like
# source bugs.

# --- Use nxdk's own activate script ------------------------------------------
#
# deps/nxdk/bin/activate emits the correct exports. Do not hand-roll PATH and
# NXDK_DIR; that is how the traps below get sprung.
#
#   eval "$(./bin/activate -s)"
#
# The `-s` is mandatory. Sourced with no arguments, activate ends in
#   exec "${@:-${SHELL:-sh}}"
# which spawns a new interactive shell and hangs a non-interactive caller.

# --- NXDK_DIR is an MSYS-form path -------------------------------------------
#
# activate exports NXDK_DIR="/g/Proyectos/.../deps/nxdk". Leave it that way.
#
# Setting it to Windows form (G:/Proyectos/...) looks like it helps — a single
# file will compile — but it breaks make's target matching, because make's
# internal paths are /g/... form. `make $NXDK_DIR/lib/libpdclib.lib` then
# reports **exit 0 while building nothing**, since no rule matches that target
# name. A build that succeeds without producing output is the worst failure
# mode in this file.

# --- The failure this all prevents -------------------------------------------
#
# Get any of the above wrong and nxdk-cc's six include directories never reach
# clang. clang discards non-existent -I paths silently, so the compile falls
# through to the Visual Studio and Windows SDK headers that clang auto-detects
# from the i386-pc-win32 triple, and dies somewhere unrelated:
#
#   pdclib/functions/ctype/isalpha.c:15:14:
#     error: use of undeclared identifier '_PDCLIB_lc_ctype'
#
# having compiled against Windows Kits' ucrt/ctype.h instead of pdclib's.
#
# Confirm rather than assume:
#
#   nxdk-cc -v -c <file> 2>&1 | sed -n '/search starts here/,/End of search/p'
#
# pdclib, xboxrt and winapi must appear. If they do not, the environment is
# wrong regardless of what any other symptom suggests.

# --- Spaces in paths ---------------------------------------------------------
#
# nxdk and make both split on spaces. This repository's path is space-free, but
# $HOME on this machine is not ("C:/Users/Mathias Development"), so keep build
# inputs and outputs inside the repo. The Mortyr Xbox demo works around the same
# problem with junctions (C:\nxdk -> the real, spaced path) and notes that
# $(CURDIR) resolves junctions back to the spaced form, so project Makefiles
# must keep SRCS relative.

# --- Do not run a mutating agent against this tree ----------------------------
#
# A workflow exploring build recipes ran `make clean` here and deleted 19
# freshly built libraries. Build artifacts under deps/nxdk are not cheap to
# reproduce; treat the tree as single-writer.
