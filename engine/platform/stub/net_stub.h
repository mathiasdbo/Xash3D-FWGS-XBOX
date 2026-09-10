/*
net_stub.h - stub BSD sockets
Copyright (C) 2020 mittorn

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/
#ifndef NET_STUB_H
#define NET_STUB_H

#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define SOCKET int
typedef int WSAsize_t;
struct in_addr {unsigned long s_addr;};
struct sockaddr_in{ short sin_family;unsigned short sin_port;struct in_addr sin_addr;};
struct sockaddr {short sa_family;int stub[32];};
struct hostent {int h_addr_list[1];};
struct timeval {long tv_sec;long tv_usec;};

// Added for the Original Xbox target (X2 W5): real GoldSrc IPv6 struct
// shapes, not stand-ins - net_ws.c's NET_SockadrToNetadr/NET_NetadrToSockadr
// (engine/common/net_ws.c) reinterpret-cast through these and read/write
// individual fields (sin6_port, sin6_addr.s6_addr), so a smaller "stub"
// layout would compile but corrupt whatever memory follows it. Every
// function that would actually USE one (socket/recvfrom/etc.) is still the
// same always-fails macro below - only the layout is real. Matches the
// standard BSD/RFC 3493 shapes bit for bit (in6_addr = 16 bytes, sockaddr_in6
// = family+port+flowinfo+addr+scope_id, sockaddr_storage sized/aligned for
// the larger of the two).
struct in6_addr { unsigned char s6_addr[16]; };
struct sockaddr_in6 {
	short          sin6_family;
	unsigned short sin6_port;
	unsigned long  sin6_flowinfo;
	struct in6_addr sin6_addr;
	unsigned long  sin6_scope_id;
};
struct sockaddr_storage {
	short ss_family; // real BSD name - net_ws.c reads ->ss_family directly
	char  stub[126]; // sizeof(sockaddr_in6) - sizeof(ss_family), rounded up
};

// WSAStartup's own out-param struct (net_ws.c's #if XASH_WIN32 branch calls
// it even under XASH_NO_NETWORK, since Windows needs it before ANY socket()
// call succeeds - never true here, but the type still has to exist for the
// call to compile). Field layout does not matter - WSAStartup is stubbed
// to (-1) below, so this is never read.
typedef struct { char stub[400]; } WSADATA;

// getaddrinfo/freeaddrinfo: net_ws.c's NET_StringToAdrNB uses these for
// non-blocking DNS resolution. No DNS resolver exists on this platform
// (nxdk's lwIP is wired for one UDP log socket only, divergence #13 - not
// a general sockets/resolver stack), so both are honest, real, in-header
// stand-ins that report "nothing found" rather than macro'd to a bare -1
// like the send/recv family above: getaddrinfo's callers branch on its
// return code AND read **res on success, so a same-shaped failure return
// (a real EAI_* code, *res left NULL) is what keeps them from being
// stubbed as bluntly as a socket op whose only contract is "0 or -1".
struct addrinfo {
	int              ai_flags;
	int              ai_family;
	int              ai_socktype;
	int              ai_protocol;
	size_t           ai_addrlen;
	struct sockaddr *ai_addr;
	char            *ai_canonname;
	struct addrinfo *ai_next;
};
#define EAI_NONAME (-2)
static inline int getaddrinfo( const char *node, const char *service,
	const struct addrinfo *hints, struct addrinfo **res )
{
	(void)node; (void)service; (void)hints;
	*res = NULL;
	return EAI_NONAME;
}
static inline void freeaddrinfo( struct addrinfo *res ) { (void)res; }

#define AF_INET 0
#define AF_INET6 1
#define AF_UNSPEC (-1)
#define PF_INET AF_INET
#define PF_INET6 AF_INET6
#define INADDR_BROADCAST 0
#define INADDR_ANY 0
#define IN6ADDR_ANY_INIT {{0}}
// socket()'s own PF_INET6/SOCK_DGRAM path never runs (socket() is (-1)
// below), so this only needs to exist as an all-zero bind address for
// NET_Config's setup code to copy from - real storage, `static` so it is
// not a distinct symbol in every TU that includes this header.
static const struct in6_addr in6addr_any;
//! Network to host conversion for a word.
#define ntohs(n) ( (((n) & 0xFF00) >> 8) | (((n) & 0x00FF) << 8) )
//! Host to network conversion for a word.
#define htons(n) ( (((n) & 0xFF00) >> 8) | (((n) & 0x00FF) << 8) )
//! Network to host conversion for a double word.
#define ntohl(n) ( (((n) & 0xFF000000) >> 24) | (((n) & 0x00FF0000) >> 8) \
	 | (((n) & 0x0000FF00) << 8) | (((n) & 0x000000FF) << 24) )
//! Host to network conversion for a double word.
#define htonl(n) ( (((n) & 0xFF000000) >> 24) | (((n) & 0x00FF0000) >> 8) \
	 | (((n) & 0x0000FF00) << 8) | (((n) & 0x000000FF) << 24) )

#define WSAStartup(...) (-1)
#define WSACleanup(...) (-1)
#define gethostbyname(...) NULL
#define inet_addr(...) (-1)
#define recvfrom(...) (-1)
#define sendto(...) (-1)
#define socket(...) (-1)
#define ioctlsocket(...) (-1)
#define setsockopt(...) (-1)
#define bind(...) (-1)
#define gethostname(...) (-1)
#define getsockname(...) (-1)
#define connect(...) (-1)
#define send(...) (-1)
#define recv(...) (-1)
#define bind(...) (-1)
#define closesocket(...) (-1)
#define select(...) (-1)
#define listen(...) (-1)
#define accept(...) (-1)

// fd_set/FD_*: xrcon.c (remote console listener) declares one on the stack
// and passes it to select() every poll - select() is (-1) above regardless
// of what is in it, so the macros only need to compile, not track anything
// real. One-word bitset is enough: sockets never validly number above 0
// here (socket() itself is (-1) above), so no call ever sets a real bit.
typedef struct { unsigned long stub; } fd_set;
#define FD_ZERO(set) ((void)(set))
#define FD_SET(fd, set) ((void)(fd), (void)(set))
#define FD_ISSET(fd, set) ( (void)(fd), (void)(set), 0 )

#define WSAGetLastError() (22) //ENETDOWN
#define WSAEINTR           1 //EINTR
#define WSAEBADF           2 //EBADF
#define WSAEACCES          3 //EACCES
#define WSAEFAULT          4 //EFAULT
#define WSAEINVAL          5 //EINVAL
#define WSAEMFILE          6 //EMFILE
#define WSAEWOULDBLOCK     7 //EWOULDBLOCK
#define WSAEINPROGRESS     8 //EINPROGRESS
#define WSAEALREADY        9 //EALREADY
#define WSAENOTSOCK        10 //ENOTSOCK
#define WSAEDESTADDRREQ    11 //EDESTADDRREQ
#define WSAEMSGSIZE        12 //EMSGSIZE
#define WSAEPROTOTYPE      13 //EPROTOTYPE
#define WSAENOPROTOOPT     14 //ENOPROTOOPT
#define WSAEPROTONOSUPPORT 15 //EPROTONOSUPPORT
#define WSAESOCKTNOSUPPORT 16 //ESOCKTNOSUPPORT
#define WSAEOPNOTSUPP      17 //EOPNOTSUPP
#define WSAEPFNOSUPPORT    18 //EPFNOSUPPORT
#define WSAEAFNOSUPPORT    19 //EAFNOSUPPORT
#define WSAEADDRINUSE      20 //EADDRINUSE
#define WSAEADDRNOTAVAIL   21 //EADDRNOTAVAIL
#define WSAENETDOWN        22 //ENETDOWN
#define WSAENETUNREACH     23 //ENETUNREACH
#define WSAENETRESET       24 //ENETRESET
#define WSAECONNABORTED    25 //ECONNABORTED
#define WSAECONNRESET      26 //ECONNRESET
#define WSAENOBUFS         27 //ENOBUFS
#define WSAEISCONN         28 //EISCONN
#define WSAENOTCONN        29 //ENOTCONN
#define WSAESHUTDOWN       30 //ESHUTDOWN
#define WSAETOOMANYREFS    31 //ETOOMANYREFS
#define WSAETIMEDOUT       32 //ETIMEDOUT
#define WSAECONNREFUSED    33 //ECONNREFUSED
#define WSAELOOP           34 //ELOOP
#define WSAENAMETOOLONG    35 //ENAMETOOLONG
#define WSAEHOSTDOWN       36 //EHOSTDOWN

#endif // NET_STUB_H
