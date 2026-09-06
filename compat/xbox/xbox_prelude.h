/*
xbox_prelude.h - Win32 declarations nxdk omits, force-included

Part of compat/xbox; see that directory's README.

Force-included into every translation unit with `-include`, because the code
that needs these reaches for them through <windows.h>, which comes from nxdk.
Adding them there would be a divergence inside a vendored submodule; adding
them here costs one compiler flag and nothing else.

Keep this SMALL. It is prepended to every file including nxdk's own headers,
so anything defined here that nxdk also defines is a redefinition error at
scale. Every entry is guarded, and every entry is here because something in the
engine failed to compile without it.
*/
#ifndef R3D_XBOX_PRELUDE_H
#define R3D_XBOX_PRELUDE_H

/* Wide-string types. nxdk's winapi has CHAR/WCHAR and the LP*STR narrow
 * spellings but not these. */
#ifndef LPWSTR
typedef unsigned short *LPWSTR;
#endif

#ifndef LPCWSTR
typedef const unsigned short *LPCWSTR;
#endif

#ifndef LPBOOL
typedef int *LPBOOL;
#endif

/* Code-page identifiers for MultiByteToWideChar. Only these two are honoured
 * by the implementation in xbox_win32_ext.c. */
#ifndef CP_ACP
#define CP_ACP   0
#endif

#ifndef CP_UTF8
#define CP_UTF8  65001
#endif

#endif /* R3D_XBOX_PRELUDE_H */
