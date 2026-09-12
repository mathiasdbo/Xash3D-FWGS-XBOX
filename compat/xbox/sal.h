/*
sal.h - empty stub, plus the specific annotations real callers use

Part of compat/xbox; see that directory's README.

SAL is Microsoft's source-annotation language: _In_, _Out_, _Ret_maybenull_ and
so on. The annotations are macros that expand to nothing under any compiler but
MSVC's analyser, so an empty header is correct FOR CODE THAT NEVER LOOKS THEM
UP - but this project's own clang invocation (`-target i386-pc-win32`) defines
_MSC_VER (verified directly: a `#ifdef _MSC_VER / #error` probe fires), which
is enough for deps/hlsdk/external/SDL2/SDL_stdinc.h's own `#if defined(_MSC_VER)
&& (_MSC_VER >= 1600)` branch to take the REAL-SAL-macro path (`_Out_bytecap_`,
`_In_bytecount_`, etc.) instead of its own already-correct empty-macro
fallback - found compiling the real hl client SDK for the first time
(tools/xbox-game-sweep.sh hl client): "use of undeclared identifier 'len'"
inside a function's own parameter list, at exactly these macro call sites,
because this header never defined them at all. Defining the SPECIFIC macros
SDL2's headers actually invoke (found by grepping deps/hlsdk/external/SDL2/*.h
directly, not guessed) as true no-ops is the correct, complete fix matching
this file's own stated design intent - an SDL2-specific
`-DSDL_DISABLE_ANALYZE_MACROS` escape hatch would only fix SDL2, leaving any
other header that assumes a working <sal.h> broken the same way.

It exists because nxdk's SDL config chain includes <sal.h> and nxdk does not
ship one.
*/
#ifndef R3D_COMPAT_SAL_H
#define R3D_COMPAT_SAL_H

#define _In_bytecount_(x)
#define _Inout_z_cap_(x)
#define _Out_bytecap_(x)
#define _Out_cap_(x)
#define _Out_z_cap_(x)
#define _Out_z_bytecap_(x)
#define _Printf_format_string_
#define _Scanf_format_string_impl_

#endif
