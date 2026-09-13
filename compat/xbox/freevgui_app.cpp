/*
freevgui_app.cpp - the three vgui::App platform hooks freevgui's own
xash3d-fwgs backend does not provide

Part of compat/xbox; see that directory's README.

3rdparty/freevgui/app.h declares internalSetMouseArena/getTimeMillis/
platTick as ordinary (non-pure) virtuals, each commented "platform-
dependent" - every OTHER platform backend freevgui ships
(platform/win32/app.cpp, platform/posix/app.cpp) defines real bodies for
them, but platform/xash3d-fwgs/app.cpp - the one backend this project
actually uses, since freevgui's own static-link mode is purpose-built for
Xash3D-FWGS - does not. Found running the first real combined
server+client+freevgui link (tools/xbox-game-link-check.sh hl
--include-client --include-freevgui): 3 undefined vgui::App vtable slots.

Rather than patch the vendored 3rdparty/freevgui submodule (a real,
external project we track at a pinned commit), this is a brand new file
in our own tree providing the missing definitions - Form 1, the cheapest
divergence form, zero merge cost, and the submodule itself stays byte-
identical to upstream.

internalSetMouseArena/platTick: copied verbatim from freevgui's own
posix backend (3rdparty/freevgui/platform/posix/app.cpp) - real, safe,
already-shipped no-ops, not something invented for this project. Correct
for the same reason ref_null and freevgui's font backend (posix's own
FontPlat_CreateSystem, also a real no-op - see tools/xbox-freevgui-
sweep.sh's own header comment) already are: nothing in this engine reads
a mouse "arena" boundary or needs a per-tick platform hook yet, so a
no-op changes no observable behaviour, it only lets the real vgui1 code
that references these vtable slots link.

getTimeMillis: real, not a stand-in - engine/platform/sdl2/sys_sdl2.c's
own Platform_DoubleTime() is the exact same, already-verified-working
Xbox time source every other part of this engine's own frame timing
already relies on (engine/common/system.c:67,77), declared in
engine/platform/platform.h. Converted to milliseconds the same way
freevgui's own win32 backend converts its own GetTickCount() (a plain
cast, no rounding logic needed - vgui only uses this for tick-interval
comparisons, not wall-clock display).
*/
// Declared directly rather than #include "platform/platform.h" - that
// header pulls in filesystem/fscallback.h and other engine-internal
// dependencies this one function prototype does not need.
extern "C" double Platform_DoubleTime( void );
#include "app.h"

using namespace vgui;

void App::internalSetMouseArena( int, int, int, int, bool )
{
	// stub - matches 3rdparty/freevgui/platform/posix/app.cpp
}

long int App::getTimeMillis()
{
	return ( long int )( Platform_DoubleTime() * 1000.0 );
}

void App::platTick()
{
	// stub - matches 3rdparty/freevgui/platform/posix/app.cpp
}
