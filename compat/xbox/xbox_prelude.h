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

/* Declarations for the functions compat/xbox implements. They are DEFINED in
 * xbox_win32_ext.c, but nothing declares them: callers reach for them through
 * <windows.h>, which comes from nxdk and does not have them. Without these the
 * calls are implicit-declaration errors under C99 even though the symbols link
 * perfectly well. */
#ifdef __cplusplus
extern "C" {
#endif

unsigned long __stdcall GetModuleFileNameA( void *module, char *filename, unsigned long size );
unsigned long __stdcall GetModuleFileNameW( void *module, LPWSTR filename, unsigned long size );

int __stdcall MultiByteToWideChar( unsigned int page, unsigned long flags,
	const char *src, int src_len, LPWSTR dst, int dst_len );
int __stdcall WideCharToMultiByte( unsigned int page, unsigned long flags,
	LPCWSTR src, int src_len, char *dst, int dst_len,
	const char *def_char, LPBOOL used_def );

#ifdef __cplusplus
}
#endif

#ifndef GetModuleFileName
#define GetModuleFileName GetModuleFileNameA
#endif

/* POINT. nxdk's winapi has RECT but not this; engine/client/dll_int/cl_game.c
 * uses it for cursor coordinates. */
#ifndef R3D_HAVE_POINT
#define R3D_HAVE_POINT
typedef struct tagPOINT { long x, y; } POINT, *PPOINT, *LPPOINT;
#endif

/* SDL hints the engine sets that postdate SDL 2.0.9, which is what
 * XboxDev/nxdk-sdl is pinned to. SDL_SetHint with an unrecognised name is a
 * no-op that returns SDL_FALSE, so defining the strings restores compilation
 * without changing behaviour — the hints simply do nothing here, which is
 * correct, since the features they configure do not exist in 2.0.9 either.
 *
 * If nxdk-sdl is ever rebased onto a newer SDL, delete these: SDL will define
 * them itself and the #ifndef guards will keep it building either way. */
#ifndef SDL_HINT_AUDIODRIVER
#define SDL_HINT_AUDIODRIVER "SDL_AUDIODRIVER"
#endif
#ifndef SDL_HINT_WINDOWS_DPI_AWARENESS
#define SDL_HINT_WINDOWS_DPI_AWARENESS "SDL_WINDOWS_DPI_AWARENESS"
#endif
#ifndef SDL_HINT_JOYSTICK_HIDAPI_PS5
#define SDL_HINT_JOYSTICK_HIDAPI_PS5 "SDL_JOYSTICK_HIDAPI_PS5"
#endif
#ifndef SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE
#define SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE "SDL_JOYSTICK_HIDAPI_PS5_RUMBLE"
#endif
#ifndef SDL_HINT_JOYSTICK_HIDAPI_SWITCH_HOME_LED
#define SDL_HINT_JOYSTICK_HIDAPI_SWITCH_HOME_LED "SDL_JOYSTICK_HIDAPI_SWITCH_HOME_LED"
#endif

/* errno_t. MSVC's Annex K spelling; pdclib is plain C99 and has no such
 * typedef. public/miniz.c uses it on its time path, which stays enabled
 * because filesystem_internal.h gets <time.h> transitively through miniz.h -
 * setting MINIZ_NO_TIME to avoid this broke three filesystem/ files that had
 * been relying on that. */
#ifndef R3D_HAVE_ERRNO_T
#define R3D_HAVE_ERRNO_T
typedef int errno_t;
#endif

/* socklen_t. A POSIX type that arrives from <sys/socket.h> everywhere else;
 * nxdk has no sockets headers at all. engine/platform/stub/net_stub.h assumes
 * it exists - it defines WSAsize_t but not this - so the no-network build
 * needs it even though nothing will ever call a socket function. */
#ifndef R3D_HAVE_SOCKLEN_T
#define R3D_HAVE_SOCKLEN_T
typedef int socklen_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

int __stdcall GetUserNameA( char *buffer, unsigned long *size );
int __stdcall GetUserNameW( LPWSTR buffer, unsigned long *size );
int __stdcall GetCursorPos( LPPOINT point );
int __stdcall SetCursorPos( int x, int y );

unsigned short *_wfullpath( unsigned short *abs, const unsigned short *rel, unsigned int max );

struct _PDCLIB_file_t;
int   fileno( struct _PDCLIB_file_t *stream );

struct tm;
errno_t localtime_s( struct tm *result, const long *timer );
errno_t gmtime_s( struct tm *result, const long *timer );

void *__stdcall GetModuleHandleA( const char *name );
int   __stdcall GetModuleHandleExA( unsigned long flags, const char *name, void **module );
int   __stdcall GetModuleHandleExW( unsigned long flags, LPCWSTR name, void **module );
char *getcwd( char *buf, unsigned int size );

#ifdef __cplusplus
}
#endif

#ifndef GetUserName
#define GetUserName GetUserNameA
#endif

#ifndef GetModuleHandle
#define GetModuleHandle GetModuleHandleA
#endif
#ifndef GetModuleHandleEx
#define GetModuleHandleEx GetModuleHandleExA
#endif

/* GetModuleHandleEx flags. Meaningless with one module - nothing here
 * reference-counts or pins - but callers pass them. */
#ifndef GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
#define GET_MODULE_HANDLE_EX_FLAG_PIN                0x00000001
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x00000002
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS       0x00000004
#endif

#endif /* R3D_XBOX_PRELUDE_H */
