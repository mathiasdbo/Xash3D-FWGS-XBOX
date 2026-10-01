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
#ifndef SDL_HINT_VIDEO_X11_FORCE_EGL
#define SDL_HINT_VIDEO_X11_FORCE_EGL "SDL_VIDEO_X11_FORCE_EGL"
#endif
#ifndef SDL_HINT_ANDROID_BLOCK_ON_PAUSE_PAUSEAUDIO
#define SDL_HINT_ANDROID_BLOCK_ON_PAUSE_PAUSEAUDIO "SDL_ANDROID_BLOCK_ON_PAUSE_PAUSEAUDIO"
#endif
#ifndef SDL_HINT_ANDROID_BLOCK_ON_PAUSE
#define SDL_HINT_ANDROID_BLOCK_ON_PAUSE "SDL_ANDROID_BLOCK_ON_PAUSE"
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

/* socklen_t. A POSIX type that arrives from <sys/socket.h> everywhere else.
 * nxdk has no <sys/socket.h> of its own, but it does vendor full lwIP
 * headers (<lwip/sockets.h>, used by engine/platform/xbox/net_xbox_log.c) -
 * corrected 2026-09-07; an earlier version of this comment said nxdk has no
 * sockets headers at all, which was true of what the engine's own net_ws.c
 * needs (real BSD sockaddr_in6/sockaddr_storage/in6_addr by value - see
 * docs/r3d/fork-plan.md's "Networking is parked" section) but not of what
 * exists in the tree. **Corrected 2026-09-07, found in review: the consumer
 * named below was wrong.** It is not engine/platform/stub/net_stub.h - that
 * file never mentions socklen_t at all. It is
 * engine/common/net_ws_private.h:90, NET_SockAddrLen's unconditional return
 * type, compiled regardless of which platform stub is selected - so the
 * no-network build still needs a definition even though nothing in it will
 * ever call a socket function.
 *
 * SOCKLEN_T_DEFINED, not just the typedef: lwip/sockets.h:117 checks that
 * exact macro before typedef'ing its own (as u32_t, not int) - the mechanism
 * it documents at :115 for exactly this situation. Defining only the
 * typedef would compile fine until some other file includes both this
 * prelude and <lwip/sockets.h>, and then fail with a redefinition error one
 * layer removed from the actual cause. */
#ifndef R3D_HAVE_SOCKLEN_T
#define R3D_HAVE_SOCKLEN_T
#define SOCKLEN_T_DEFINED
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

int       _fseeki64( struct _PDCLIB_file_t *stream, long long offset, int origin );
long long _ftelli64( struct _PDCLIB_file_t *stream );

struct tm;
errno_t localtime_s( struct tm *result, const long *timer );
errno_t gmtime_s( struct tm *result, const long *timer );

void *__stdcall GetModuleHandleA( const char *name );
int   __stdcall GetModuleHandleExA( unsigned long flags, const char *name, void **module );
int   __stdcall GetModuleHandleExW( unsigned long flags, LPCWSTR name, void **module );

// _set_abort_behavior/_WRITE_ABORT_MSG: found in review, 2026-09-08 (X1,
// third-party audio codec measurement). Real MSVC CRT surface
// (<stdlib.h> there, not <windows.h>), not declared anywhere in nxdk's
// pdclib. 3rdparty/opus/opus/celt/arch.h's celt_fatal() calls this under
// `#if defined(_MSC_VER)` - genuinely true for nxdk's clang target
// (x0-probe-results.md's own §1 measurement: `_MSC_VER 1950`), so this is
// reachable as written, not a landmine that never fires. On real MSVC it
// only toggles whether an assertion-failure dialog box gets shown before
// abort() - Xbox has no such dialog to suppress or show, so the no-op
// definition in xbox_win32_ext.c is not a stub standing in for missing
// behaviour, it is the complete, correct behaviour for this platform.
#define _WRITE_ABORT_MSG 1
unsigned int _set_abort_behavior( unsigned int flags, unsigned int mask );
char *getcwd( char *buf, unsigned int size );

/* SystemParametersInfoA/SPI_*: found compiling the real hl client SDK
 * (deps/hlsdk/cl_dll/inputw32.cpp's mouse-acceleration save/restore
 * around raw mouse look) - see xbox_win32_ext.c for why an unconditional
 * FALSE is the complete, correct behaviour here, not a stub for missing
 * functionality. Only these two actions are declared because they are
 * the only ones any caller in this tree uses yet. */
int __stdcall SystemParametersInfoA( unsigned int action, unsigned int param, void *data, unsigned int wini );

#ifndef SPI_GETMOUSE
#define SPI_GETMOUSE 3
#endif
#ifndef SPI_SETMOUSE
#define SPI_SETMOUSE 4
#endif

#ifdef __cplusplus
}
#endif

#ifndef SystemParametersInfo
#define SystemParametersInfo SystemParametersInfoA
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

/* Small fixed-size copies inline again (docs/r3d/divergences.md #142).
 * Both compile drivers pass -ffreestanding, which in clang implies
 * -fno-builtin (measured: the two produce identical code), so every
 * memcpy/memset/memmove - even a 4-byte one - is a call. The __builtin_
 * spellings are honoured in freestanding mode: clang inlines a constant
 * size up to 32 bytes (memcpy) / 64 (memset) as plain moves, and emits a
 * call to the library function - compat/xbox/xbox_string_fast.c - for
 * anything else, exactly as before. Same semantics, no call for the
 * small ones. C only: in C++ a function-like macro would break
 * `std::memcpy(`. <string.h> first, so its declarations are not
 * rewritten. R3D_NO_BUILTIN_STRING turns it off for one compile (the
 * implementation file defines the real names and #undefs these). */
#if !defined( __cplusplus ) && !defined( R3D_NO_BUILTIN_STRING )
#include <string.h>
#define memcpy( d, s, n )  __builtin_memcpy(( d ), ( s ), ( n ))
#define memset( d, c, n )  __builtin_memset(( d ), ( c ), ( n ))
#define memmove( d, s, n ) __builtin_memmove(( d ), ( s ), ( n ))
#endif

#endif /* R3D_XBOX_PRELUDE_H */
