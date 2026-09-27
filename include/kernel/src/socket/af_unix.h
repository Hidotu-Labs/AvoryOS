#ifndef AF_UNIX_H
#define AF_UNIX_H

#include "socket.h"

void af_unix_init(void);

int unix_create(socket_t *sock, int protocol);

void unix_destroy(socket_t *sock);

int unix_unbind_by_path(const char *path);

sock_ops_t *unix_get_ops(void);

net_family_t *unix_get_family(void);

#endif // AF_UNIX_H
