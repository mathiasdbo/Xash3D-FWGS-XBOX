/*
sal.h - empty stub

Part of compat/xbox; see that directory's README.

SAL is Microsoft's source-annotation language: _In_, _Out_, _Ret_maybenull_ and
so on. The annotations are macros that expand to nothing under any compiler but
MSVC's analyser, so an empty header is the whole of a correct implementation.

It exists because nxdk's SDL config chain includes <sal.h> and nxdk does not
ship one.
*/
#ifndef R3D_COMPAT_SAL_H
#define R3D_COMPAT_SAL_H
#endif
