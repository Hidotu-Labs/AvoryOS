#ifndef LINUXKPI_NET_SOCK_OVERLAY_H
#define LINUXKPI_NET_SOCK_OVERLAY_H
#ifdef LINUXKPI_NET_FRONTEND
struct sock;
#else
#include_next <net/sock.h>
#endif
#endif
