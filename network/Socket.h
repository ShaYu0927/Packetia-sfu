#ifndef XOP_SOCKET_H
#define XOP_SOCKET_H

#if defined(__linux) || defined(__linux__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/route.h>
#include <netdb.h>
#include <netinet/ether.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netpacket/packet.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#define SOCKET int
#ifndef INVALID_SOCKET
#define INVALID_SOCKET (-1)
#endif
#ifndef SOCKET_ERROR
#define SOCKET_ERROR (-1)
#endif

#elif defined(WIN32) || defined(_WIN32)
#define FD_SETSIZE 1024
#define WIN32_LEAN_AND_MEAN
#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#endif
#include <WinSock2.h>
#include <iphlpapi.h>
#include <windows.h>
#include <ws2tcpip.h>
#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2

#else

#endif

#endif // _XOP_SOCKET_H
