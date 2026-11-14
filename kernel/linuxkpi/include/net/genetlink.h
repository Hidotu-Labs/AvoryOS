#ifndef LINUXKPI_NET_GENETLINK_OVERLAY_H
#define LINUXKPI_NET_GENETLINK_OVERLAY_H
#ifdef LINUXKPI_NET_FRONTEND
#include <linux/netlink.h>
#include <uapi/linux/genetlink.h>
#else
#include_next <net/genetlink.h>
#endif
#endif
