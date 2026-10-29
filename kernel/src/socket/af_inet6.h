#ifndef AF_INET6_H
#define AF_INET6_H
#include <stddef.h>
struct socket;
void af_inet6_init(void);
void inet6_diag_describe(struct socket *sock, char *out, size_t outsz);
#endif
