#ifndef LINUXKPI_NET_NETLINK_OVERLAY_H
#define LINUXKPI_NET_NETLINK_OVERLAY_H
#ifdef LINUXKPI_NET_FRONTEND
#include <linux/netlink.h>
#else
#include_next <net/netlink.h>
#endif
#endif
