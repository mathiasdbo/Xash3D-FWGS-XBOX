/*
net_xbox_log.c - Original Xbox log channel, second tier: UDP broadcast
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

/* lwip/sockets.h redefines read/write/close as lwip_read/lwip_write/
 * lwip_close (LWIP_POSIX_SOCKETS_IO_NAMES - lwIP's own unconditional
 * default, lwip/opt.h:2046-2047, not something nxdk's lwipopts.h sets;
 * grepping that file for the macro finds nothing, because it never needed
 * to override a default it agrees with).
 *
 * Only `read` has a prior definition actually reachable in THIS file -
 * corrected 2026-09-07, found in review, an earlier version of this
 * comment implied a concrete write/close collision here too. common/port.h
 * (included via common.h:31) maps read -> _read unconditionally under
 * XASH_WIN32. write and close have nothing to collide WITH here: this file
 * never includes compat/xbox/unistd.h, directly or transitively, so the
 * plain-named write()/close() it declares are not in scope to be shadowed
 * in the first place. The #undef for those two is prospective hygiene, not
 * a fix for a live bug in this file - it is what the NEXT file that
 * includes both unistd.h and lwip/sockets.h will need.
 *
 * Corrected 2026-09-07, found in review: an earlier version of this
 * comment said lwip/sockets.h "silently" overrides read -> _read - false,
 * and caught by compiling this file: with the #undef placed AFTER the
 * #include as it was then, clang emitted a real
 * 'read' macro redefined [-Wmacro-redefined] warning at the #include line,
 * because lwip/sockets.h's own #define read(...) collided with the
 * still-live read -> _read from port.h. The #undef below only cleaned up
 * for the REST of the file after that point; it did nothing about the
 * warning already fired at the include itself. Moved above the lwip
 * includes instead, so the prior macro is gone before lwip/sockets.h ever
 * tries to define its own - now genuinely silent, verified by recompiling
 * and confirming the warning is gone.
 *
 * This file DOES need to close a socket (Xbox_NetLogInit's error path
 * below), and lwip_close is not interchangeable with compat/xbox's file
 * close - sockets and file HANDLEs are different descriptor spaces
 * entirely, r3d_fds[] on one side, lwIP's own internal table on the other.
 * So: #undef the bare names rather than lean on any macro definition, and
 * call lwip_close() explicitly by its real name below. That keeps "close"
 * itself unambiguous for whichever file gets read next, and this file's one
 * real close call cannot be silently misrouted by include order. */
#undef read
#undef write
#undef close

#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <nxdk/net.h>

// Arbitrary, high, unregistered. Documented here because it is the one fact
// a developer needs on the receiving end - `nc -ul 30865` or equivalent.
#define XBOX_LOG_UDP_PORT 30865

static qboolean s_net_log_ready      = false;
static qboolean s_net_log_attempted  = false;
static int      s_net_log_socket     = -1;
static struct sockaddr_in s_net_log_addr;

/*
====================
Xbox_NetLogInit

Deliberately NOT called from Xbox_ConPrint, and not called anywhere yet -
this function exists so a future Xbox startup sequence can call it once,
explicitly, when it is ready to pay the cost.

nxNetInit()'s DHCP wait is genuinely blocking: nxdk/net.c's own loop retries
ten times at ~1 second each before giving up (net.c:118-128, corrected
2026-09-07, found in review: was off by one, pointing at the `if
(ipv4_dhcp)` guard rather than the retry loop itself), so this can take up
to ~10 seconds with no cable plugged in or no DHCP server reachable.
Calling this automatically on first print - which is what an earlier draft
of this file did - would freeze the very first line of boot output for that
whole window, which is the opposite of what a log channel is for. Call this
after the DbgPrint tier has already shown the developer something, not
before, and be prepared for it to fail: the return value says so.

UNVERIFIED: whether nxNetInit's blocking duration matches this comment on
real hardware or in xemu has not been observed. This host has no xemu
install; only compilation is confirmed. Confirm on a real run before relying
on the ~10 second figure for anything time-sensitive.

Broadcast, not a configured address: a UDP log has no fixed destination
worth hardcoding, and the developer's machine's IP on a dev network changes.
Anything listening on XBOX_LOG_UDP_PORT on the local subnet receives it -
`nc -ul 30865` from any machine on the same segment.

Returns true if the socket is ready to send. False means Xbox_ConPrint's
network tier stays inactive; the DbgPrint tier is unaffected either way.

Considered and deferred, 2026-09-07, found by the first real run of
tools/codex-review.sh: s_net_log_attempted conflates two different failure
domains into one permanent, un-retriable state. If nxNetInit() succeeds but
socket() or setsockopt() then fails - transient socket exhaustion, say,
since nxdk's lwipopts.h caps the whole engine at lwIP's default
MEMP_NUM_NETCONN of 4 - a caller can never retry, even though nxNetInit()
itself would not need to run again (the guard exists specifically to stop
a SECOND nxNetInit() call, whose own netifapi_netif_add() would assert on
re-adding an already-added interface - see the comment on
s_net_log_attempted below). Splitting "network stack ready" from "socket
ready" into separate retry-eligible state would fix this. Not done here:
this function has no caller yet (nothing wires it into a startup sequence
- that is X1/X2 work, per CHANGELOG's "Still open in X0"), and guessing at
retry semantics - on what trigger, on what cadence - for a caller that
does not exist yet risks designing the wrong retry policy and having to
redo it once the real caller's needs are known. Revisit when that caller
is written.
====================
*/
qboolean Xbox_NetLogInit( void )
{
	int enable_broadcast;

	if( s_net_log_ready )
		return true;

	// s_net_log_attempted, not just s_net_log_ready: found in review. The
	// doc comment above already said "call once", but nothing enforced
	// that - a caller retrying after THIS function returns false (which it
	// documents as a real, expected outcome) would call nxNetInit() a
	// second time. nxNetInit's own netifapi_netif_add() has no re-entry
	// guard for an interface already added, and lwIP's own netif_add()
	// asserts on that condition rather than failing it cleanly
	// (lwip/core/netif.c:412, LWIP_ASSERT("netif already added", ...)) -
	// live in this build, since nforceif/include/arch/cc.h maps
	// LWIP_PLATFORM_ASSERT to a real assert() and nothing here defines
	// LWIP_NOASSERT. Currently unreachable (nothing calls this function
	// yet), which is exactly why it is cheap to close now rather than
	// leave for whichever future startup code adds the first caller.
	if( s_net_log_attempted )
		return false;

	s_net_log_attempted = true;

	if( nxNetInit( NULL ) != 0 )
		return false;

	s_net_log_socket = socket( AF_INET, SOCK_DGRAM, 0 );

	if( s_net_log_socket < 0 )
		return false;

	enable_broadcast = 1;

	if( setsockopt( s_net_log_socket, SOL_SOCKET, SO_BROADCAST,
		&enable_broadcast, sizeof( enable_broadcast )) != 0 )
	{
		// Not fatal by itself, and weaker than it looks: corrected
		// 2026-09-07, found in review - nxdk's lwipopts.h does not override
		// IP_SOF_BROADCAST, leaving it at lwIP's own default of 0
		// (lwip/opt.h:834-835), which compiles the broadcast-permission
		// check this option controls out of udp_sendto_if_src entirely
		// (lwip/core/udp.c:732-741, guarded by
		// `#if LWIP_IPV4 && IP_SOF_BROADCAST`). So sendto() to
		// INADDR_BROADCAST would succeed with or without SO_BROADCAST ever
		// being set, in this build. Kept anyway - it is the portable,
		// standards-correct thing to ask for, and costs nothing to request
		// even where the platform does not enforce it - but there is no
		// path forward if the socket cannot be configured at all.
		//
		// Close it, and reset the static: found in review, an earlier
		// version leaked the descriptor here and left s_net_log_socket
		// holding it, so a caller retrying after this failure (this
		// function's own re-entry guard above only blocks a SECOND call
		// once s_net_log_ready is true, which never happens on this path)
		// would leak one more on every attempt - real budget pressure, since
		// nxdk's lwipopts.h takes lwIP's own default MEMP_NUM_NETCONN of 4
		// sockets total for the whole engine.
		lwip_close( s_net_log_socket );
		s_net_log_socket = -1;
		return false;
	}

	memset( &s_net_log_addr, 0, sizeof( s_net_log_addr ));
	s_net_log_addr.sin_family      = AF_INET;
	s_net_log_addr.sin_port        = htons( XBOX_LOG_UDP_PORT );
	s_net_log_addr.sin_addr.s_addr = INADDR_BROADCAST;

	s_net_log_ready = true;

	return true;
}

/*
====================
Xbox_NetLogPrint

A no-op until Xbox_NetLogInit() has been called and succeeded. Fire-and-
forget: sendto's return value is not checked, because a dropped log line on
a UDP send is not a condition this function has anything useful to do about,
and it must never itself become a source of print-time cost or failure.
====================
*/
void Xbox_NetLogPrint( const char *pMsg )
{
	if( !s_net_log_ready )
		return;

	sendto( s_net_log_socket, pMsg, Q_strlen( pMsg ), 0,
		(struct sockaddr *)&s_net_log_addr, sizeof( s_net_log_addr ));
}

#endif // XASH_XBOX
