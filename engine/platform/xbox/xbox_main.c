/*
xbox_main.c - the real Xbox entry point, deliberately minimal

Part of engine/platform/xbox; see docs/r3d/fork-plan.md's X1 section.

X1's own exit line is "engine, filesystem, menu and a stub game module link
into one XBE" - the one piece every previous X1 measurement (the ten
duplicate symbols, the undefined-symbol picture, the third-party audio
codecs, the menu module, the stub game module) left unaddressed on purpose:
`_main` has been part of the tracked, deliberately-parked undefined-symbol
set since the very first measurement (`x0-probe-results.md`), because
nothing in this tree defined it.

This is NOT Host_Main. Calling into the real engine boot sequence - even
just Host_Init - is genuinely X2+ work: filesystem/'s own POSIX layer is
already flagged "bigger than it looked" (docs/r3d/fork-plan.md's X2
section), and nothing about a real boot has been measured yet (does
Sys_Error's SDL_INIT_VIDEO-dropped path actually work end to end, does
zone.c's allocator survive real Xbox memory constraints, does the log
channel actually reach a debugger). Calling Host_Main here would be
guessing at all of that at once, in the one place this project's own
methodology says not to guess. This proves something narrower and more
honest: that a real Xbox entry point exists, resolves `_main`, and a real
.xbe can be produced from the result - matching nxdk's own hello-world
sample almost exactly, on purpose, since that sample is nxdk's own proof
that pdclib's crt0 correctly calls whatever function is named `main`.

Deliberately does not touch any engine subsystem (no Sys_*, no Host_*, no
zone.c) - nothing here has been initialized, and touching any of it would
be exactly the kind of "reaches further than what was actually measured"
mistake this project's own review process keeps catching elsewhere.
*/

#include <hal/debug.h>
#include <windows.h>

int main( void )
{
	while( 1 )
	{
		debugPrint( "Resonance3D: X1 entry point reached. Host_Main is not called yet - see docs/r3d/fork-plan.md's X2 section.\n" );
		Sleep( 2000 );
	}

	return 0;
}
