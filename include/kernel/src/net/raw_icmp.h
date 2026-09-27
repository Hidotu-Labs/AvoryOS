#ifndef NET_RAW_ICMP_H
#define NET_RAW_ICMP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fs/vfs.h"
#include "sched/wait.h"
typedef int64_t ssize_t;
#define RAW_ICMP_MAX_SOCKETS 8
#define RAW_ICMP_QUEUE_DEPTH 8
struct raw_icmp_packet { uint32_t src; uint16_t length; uint8_t data[1500]; };
struct raw_icmp_socket {
  bool used, connected; uint32_t local_ip, remote_ip;
  uint32_t head, tail; struct raw_icmp_packet queue[RAW_ICMP_QUEUE_DEPTH];
  wait_queue_t *wait; vfs_node_t *node;
};
void raw_icmp_init(void);
struct raw_icmp_socket *raw_icmp_alloc(void);
void raw_icmp_free(struct raw_icmp_socket *);
int raw_icmp_send(struct raw_icmp_socket *, const void *, size_t, uint32_t);
ssize_t raw_icmp_recv(struct raw_icmp_socket *, void *, size_t, uint32_t *,
                      bool nonblocking);
bool raw_icmp_readable(struct raw_icmp_socket *);
void raw_icmp_deliver(uint32_t, uint32_t, const uint8_t *, size_t);
#endif
