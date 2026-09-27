#ifndef AF_NETLINK_H
#define AF_NETLINK_H

#include "socket.h"

// AF_NETLINK Family Registration

/**
 * Initialize and register the AF_NETLINK socket family.
 * Called during socket subsystem initialization.
 */
void af_netlink_init(void);

/**
 * Create an AF_NETLINK socket.
 * @param sock The socket structure to initialize
 * @param protocol Netlink protocol (NETLINK_ROUTE, NETLINK_KOBJECT_UEVENT, etc.)
 * @return 0 on success, negative error code on failure
 */
int netlink_create(socket_t *sock, int protocol);
void netlink_broadcast(int protocol, uint32_t group, const void *data, size_t len);
void netlink_broadcast_drm_hotplug(const char *devpath, uint32_t connector);

#endif // AF_NETLINK_H
