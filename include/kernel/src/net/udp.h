#ifndef NET_UDP_H
#define NET_UDP_H

#include "lock/spinlock.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int64_t ssize_t;

#define UDP_PORT_EPHEMERAL_MIN 32768
#define UDP_PORT_EPHEMERAL_MAX 60999
#define UDP_MAX_SOCKETS        2048
#define UDP_DEFAULT_RCVBUF     (64 * 1024)
#define UDP_MIN_RCVBUF         (4 * 1024)
#define UDP_MAX_RCVBUF         (4 * 1024 * 1024)
#define UDP_DEFAULT_RX_DEPTH   256
#define UDP_HASH_SIZE          256
/* IPv4 fragmentation is reassembled by the IP layer. */
#define UDP_PAYLOAD_MAX        1472

struct udp_socket;

/* One queued datagram descriptor; payload lives in the byte ring at
 * data_off (modulo capacity). */
struct udp_rxdesc {
    uint32_t src_ip;
    uint16_t src_port;
    uint16_t len;
    size_t   data_off;
};

struct udp_socket {
    bool     used;
    bool     bound;
    bool     connected;
    spinlock_t lock;            /* protects the receive queue and options */

    uint32_t local_ip;
    uint16_t local_port;
    uint32_t remote_ip;
    uint16_t remote_port;

    uint8_t *rx_data;           /* byte ring */
    size_t   rx_capacity;
    size_t   rx_write;          /* absolute byte cursor */
    size_t   rx_read;           /* absolute byte cursor of oldest queued */
    struct udp_rxdesc *rx_desc; /* descriptor ring */
    uint32_t rx_depth;
    uint32_t q_head, q_tail;

    void    *wait_queue;
    void    *vfs_node;
    bool     nonblocking;
    int      rcvtimeo_ms;
    int      sndtimeo_ms;
    uint8_t  tos;               /* IP_TOS / DSCP passthrough */
    uint64_t dropped;

    int      refs;              /* transient references under the table lock */
    bool     reap;              /* socket layer released us; free when refs 0 */
    struct udp_socket *hash_next;
    struct udp_socket *list_next;
};

void udp_init(void);

struct udp_socket *udp_socket_alloc(void);
void               udp_socket_free(struct udp_socket *s);

int  udp_bind(struct udp_socket *s, uint32_t ip, uint16_t port);
int  udp_connect(struct udp_socket *s, uint32_t ip, uint16_t port);
int  udp_disconnect(struct udp_socket *s);

ssize_t udp_sendto(struct udp_socket *s, const void *buf, size_t len,
                   uint32_t dst_ip, uint16_t dst_port);

ssize_t udp_recvfrom(struct udp_socket *s, void *buf, size_t len,
                     uint32_t *src_ip, uint16_t *src_port,
                     bool nonblocking, int timeout_ms);

void udp_deliver(uint32_t src_ip, uint16_t src_port,
                 uint16_t dst_port, const uint8_t *payload, uint16_t length);

void udp_set_rcvbuf(struct udp_socket *s, size_t bytes);
void udp_set_tos(struct udp_socket *s, int tos);

bool net_phase6_init(void);

/* Snapshot of one UDP socket for /proc/net/udp */
struct udp_entry_snapshot {
    uint32_t local_ip, remote_ip;
    uint16_t local_port, remote_port;
    bool     connected;
};

/* Fill up to 'max' entries; returns number filled. */
int udp_get_snapshot(struct udp_entry_snapshot *out, int max);

#endif
