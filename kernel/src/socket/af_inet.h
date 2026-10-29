#ifndef AF_INET_H
#define AF_INET_H

#include "socket.h"

void af_inet_init(void);

void inet_diag_describe(socket_t *sock, char *out, size_t outsz);

#endif
