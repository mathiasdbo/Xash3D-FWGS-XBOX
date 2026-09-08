/*
con_xbox.c - Original Xbox debug output channel
Copyright (C) 2026 Resonance3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "common.h"

#if XASH_XBOX

#include <xboxkrnl/xboxkrnl.h>

/*
====================
Xbox_Init / Xbox_Shutdown

Called from Platform_Init/Platform_Shutdown (platform.h), in the
XASH_XBOX arm added ahead of the XASH_WIN32 one for the same reason this
file exists: XASH_XBOX implies XASH_WIN32, so without an Xbox arm this would
fall into Win32_Init, whose only definition (engine/platform/win32/
sys_win.c) does not compile for Xbox.

Genuinely empty, not a placeholder standing in for missing work: Win32_Init
probes for CreateWaitableTimerExW via LoadLibraryW/GetProcAddress
(sys_win.c:47-62). **Corrected 2026-09-07, found in review: this comment
named the wrong function.** nxdk's winapi declares LoadLibraryExA,
LoadLibraryA and the LoadLibrary/LoadLibraryEx macro aliases to them
(deps/nxdk/lib/winapi/libloaderapi.h:14-21) - none of which "unconditionally
return NULL", because none of them is what sys_win.c calls. It calls
LoadLibraryW specifically, which nxdk does not declare, define, or alias at
all. A stronger reason than the one this comment used to give: not "returns
nothing useful", but "does not exist to call". Nothing here needs
replacing, only excluding.
====================
*/
void Xbox_Init( qboolean con_showalways )
{
	( void )con_showalways;
}

void Xbox_Shutdown( void )
{
}

/*
====================
Xbox_ConPrint

The first tier of X0's log channel: nxdk's kernel debugger print, not
hal/debug.h's debugPrint. The two are easy to confuse - both are called
"the debug channel" casually - but they are not interchangeable here.

debugPrint (hal/debug.h) draws text onto the framebuffer with a bitmap font
and needs XVideoSetMode() called first; nxdk's own hello sample sets a video
mode before its first call. That mode-setting is X4/X5 work, not X0 - this
channel has to work before any video decision exists.

DbgPrint (xboxkrnl.h) is the kernel debugger's own printf-style output,
available from process start with no video mode required, and xemu
surfaces it to its own console/log. It costs nothing to have live this
early, and it is what a console with "no stdout to read" actually has.

This replaces Wcon_WinPrint (engine/platform/win32/con_win.c) at its call
site in Sys_Print (engine/common/system.c) - the one call XASH_XBOX inherits
from being a XASH_WIN32 sub-platform that actually needs a replacement
rather than an exclusion, because Xbox needs to print even though it cannot
use con_win.c (SetConsoleTextAttribute, the FOREGROUND_* constants and
ShowWindow, none of which nxdk has - measured, tools/xbox-sweep.sh:
20 errors). Guarding that call out without a replacement would leave
Sys_Print silent on Xbox - sys_con.c:289's "Wcon does the job" comment is
the reason no POSIX-style stdout fallback runs when XASH_WIN32 is true, and
Xbox has no stdout to fall back to regardless.

**Corrected 2026-09-07, twice: this was not the only Wcon_* call site
reachable under XASH_WIN32, and the fix for that was itself incomplete.**
An earlier version of this comment, and of CHANGELOG.md/divergences.md,
called it "the one function" - found wrong in review. Six more Wcon_ sites
exist (Sys_Error x2 in system.c, Sys_DestroyConsole in sys_con.c, two in
host.c, Wcon_Input via Platform_Input in platform.h - the last one found a
full review round after the other five, because the grep that found those
five was scoped to .c files under engine/common/ and platform.h sat outside
it - corrected 2026-09-07, found in review: the original wording here spelled
that as a glob (a slash directly followed by a star, star directly followed
by dot-c), which the compiler reads as the start of a nested comment inside
this very block comment - a real -Wcomment warning, fixed by describing the
scope in words instead of typing the glob; two of the six are behind the
raw _WIN32 macro rather than XASH_WIN32). All six are
excluded with no replacement - Xbox has no console window to show, hide,
destroy, add commands to, or read input from at all.

A seventh, Win32_NanoSleep, is a different case: see Xbox_NanoSleep below.
It needed a real replacement, the same way this function did, because
host.c's frame-pacing loop calls it unconditionally and never checks the
return value. See divergence #7c.

Network logging is the second tier the plan calls for. It is in
net_xbox_log.c, not this file, and it is inert until something calls
Xbox_NetLogInit() explicitly - see that function's comment for why this
function does not call it automatically.

UNVERIFIED, found in review 2026-09-07: this function calls DbgPrint
synchronously and unconditionally on every Sys_Print, with no throttling
and no cost measurement - unlike Xbox_NanoSleep's and Xbox_NetLogInit's
blocking calls below, which both carry their own UNVERIFIED caveats for
exactly this reason. DbgPrint is documented above as costing nothing to
call this early, which is true of nxdk's own claim that it is available
from process start - but nothing in this branch has measured its
per-call cost with host_developer raised, where Con_DPrintf/Con_Reportf
call sites in net_ws.c and cl_main.c alone can fire dozens of times per
frame. If DbgPrint blocks on a debug-kit-speed transport when no debugger
is attached (plausible, not measured - this host has no xemu install and
no real hardware to test against), that cost lands squarely inside the
render loop, on the same frame-timing path Xbox_NanoSleep exists to keep
accurate. Confirm on real hardware or in xemu before relying on this
being cheap enough to leave in a hot path at host_developer > 0; until
then, treat the "costs nothing" claim above as X0's convenience-tier
assumption, not a measured fact.
====================
*/
void Xbox_ConPrint( const char *pMsg )
{
	// Found in review, 2026-09-07: this used to hand pMsg straight to
	// DbgPrint/Xbox_NetLogPrint, unlike every other XASH_WIN32 target's
	// Sys_Print path. Wcon_WinPrint (con_win.c, not compiled for Xbox)
	// recognises IsColorString's "^N" sequences and turns them into real
	// SetConsoleTextAttribute colour changes; DbgPrint and a UDP broadcast
	// are both plain text with nothing to render a colour change as, so
	// the closest match is what Sys_PrintLog already does for its own
	// plain-text sinks (sys_con.c's need_strip path): strip the markers
	// rather than interpret them. Without this, ordinary boot output like
	// host.c's "^3%s:^7 %s is enabled" reached both log tiers with the
	// literal two-character "^3"/"^7" still in the text - not a rare case,
	// this fires on every ordinary boot.
	//
	// Colour codes only, not the legacy \1/\2/\3/\35-\37 control bytes the
	// XASH_WIN32 branch above this one also strips: those exist to pair
	// CRLF for a Windows console and to skip "console pseudo graph" bytes
	// that concept has no Xbox equivalent to protect against, and no
	// review finding raised them - stripping colour codes is what was
	// actually found and confirmed.
	//
	// Truncate to the last MAX_PRINT_MSG-1 bytes before stripping, the same
	// way the XASH_WIN32 branch above truncates into its own buffer[]:
	// COM_StripColors never grows its input (each iteration either copies
	// one byte or skips two without copying), so bounding what goes IN
	// bounds what comes out - stripped[] can never overflow. Con_Printf and
	// Con_DPrintf are themselves bounded to MAX_PRINT_MSG (common.h's own
	// comment on the constant), but Sys_Print's other callers are not
	// provably so, and system.c's Win32 branch already treats that as real
	// enough to guard against rather than assume away.
	static char stripped[MAX_PRINT_MSG];

	const char *msg = pMsg;
	if( Q_strlen( pMsg ) > sizeof( stripped ) - 1 )
		msg = pMsg + Q_strlen( pMsg ) - ( sizeof( stripped ) - 1 );

	COM_StripColors( msg, stripped );

	// "%s", not stripped as the format string: pMsg is engine-formatted
	// text and may itself contain '%'.
	DbgPrint( "%s", stripped );

	// A no-op until Xbox_NetLogInit() has been called and succeeded
	// elsewhere. Always safe to call.
	Xbox_NetLogPrint( stripped );
}

/*
====================
Xbox_NanoSleep

Frame-pacing sleep, called from host.c's frame loop (platform.h,
Platform_NanoSleep). Unlike the Xbox_Init/Xbox_Shutdown stubs, this is a
real implementation, not an exclusion - a false-returning stub here would
not crash anything, but the caller never checks the return value and would
spin measuring near-zero elapsed time every frame instead of sleeping,
silently disabling FPS capping rather than failing loudly.

KeDelayExecutionThread, not Sleep() (deps/nxdk/lib/winapi/synchapi.h:55,
genuinely available): Sleep() is millisecond-granular, and nsec here is
routinely sub-millisecond (host.c:571's own comment: "sleeptime 1 ~ 100
usecs") - a straight nsec/1000000 conversion would round every realistic
call to Sleep(0). KeDelayExecutionThread takes 100ns units, matching the
scale this function is actually called at, and is the same primitive
nxNetInit already uses for its own delay loop (deps/nxdk/lib/nxdk/net.c:
118-128, corrected 2026-09-07, found in review: was off by one, pointing at
the `if (ipv4_dhcp)` guard rather than the netifapi_dhcp_start/while/
KeDelayExecutionThread block it describes) - proven to compile and link in
this tree already, not a new kernel API surface.

UNVERIFIED: timing precision on real hardware or in xemu has not been
observed. This host has no xemu install; only compilation is confirmed,
same caveat as Xbox_NetLogInit's DHCP-wait figure.
====================
*/
qboolean Xbox_NanoSleep( int nsec )
{
	LARGE_INTEGER duration;

	// Negative QuadPart = relative delay, per KeDelayExecutionThread's own
	// doc comment in xboxkrnl.h. Round down rather than up: rounding a
	// sub-100ns request up to one tick is a longer sleep than asked for,
	// where rounding down to zero ticks is simply not sleeping at all -
	// the caller already tolerates "didn't sleep the full amount" per its
	// own comment above ("isn't guaranteed to sleep an exact amount").
	duration.QuadPart = -( ( LONGLONG )nsec / 100 );

	if( duration.QuadPart == 0 )
		return true;

	KeDelayExecutionThread( KernelMode, FALSE, &duration );

	return true;
}

/*
====================
Platform_SetStatus / Platform_ShellExecute

Found using the mechanism-based search this branch's lessons.md entry asked
for: platform.h declares both as plain externs with no dispatcher at all -
every platform provides its own definition directly, the way sys_linux.c
and sys_win.c do, rather than platform.h picking between named per-platform
functions the way Win32_Init/Win32_NanoSleep work. That means there is no
guard to widen; the fix is to provide the symbol.

Both are genuinely empty. Platform_SetStatus (one caller,
engine/server/sv_main.c:657) sets a taskbar/status-line string - Xbox has
neither. Platform_ShellExecute (one caller, engine/client/dll_int/
cl_gameui.c:1080) opens a path or URL in an external shell - a console has
no shell to hand it to.

Not exhaustive: engine/platform/win32/{sys_win.c,con_win.c} also define
Platform_DoubleTime, Platform_Sleep and Platform_MessageBox with the same
"no dispatcher, per-platform symbol" shape, and none of those three needed
fixing here, because all three are ALSO defined in
engine/platform/sdl2/sys_sdl2.c (Platform_DoubleTime/Platform_Sleep under
`#if XASH_TIMER == TIMER_SDL`, Platform_MessageBox under
`#if XASH_MESSAGEBOX == MSGBOX_SDL`), both of which this Xbox build already
selects (`-DXASH_SDL=2`) and sys_sdl2.c already compiles clean. Checked, not
assumed - a fourth accidental landmine would have been to assume "shares
the shape of the other unresolved ones" without checking each one
individually.
====================
*/
void Platform_SetStatus( const char *status )
{
	( void )status;
}

void Platform_ShellExecute( const char *path, const char *parms )
{
	( void )path;
	( void )parms;
}

#endif // XASH_XBOX
