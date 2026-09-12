/*
memory.h - real MSVC/Win32 legacy header, redirected

Part of compat/xbox; see that directory's README.

On real MSVC, <memory.h> is a legacy C header providing the mem*()
functions (memcpy, memset, memmove, memcmp, ...) - all of which
deps/nxdk/lib/pdclib's real <string.h> already provides. nxdk does not
ship a <memory.h> at all; the real callers found so far (deps/hlsdk's
cl_dll, building the real hl client SDK for the first time,
tools/xbox-game-sweep.sh hl client) only ever want memcpy from it.
*/
#ifndef R3D_COMPAT_MEMORY_H
#define R3D_COMPAT_MEMORY_H

#include <string.h>

#endif
