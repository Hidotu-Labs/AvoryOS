#include "net/udp.h"
#include "apic/lapic_timer.h"
#include "console/klog.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "net/core.h"
#include "net/ipv4.h"
#include "sched/sched.h"
#include "sched/wait.h"
#include "socket/epoll.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static spinlock_t        table_lock = SPINLOCK_INIT;
static struct udp_socket *udp_hash[UDP_HASH_SIZE];
static struct udp_socket *udp_list;
static int               udp_count;
static uint16_t          next_ephemeral = UDP_PORT_EPHEMERAL_MIN;

static inline void put32_be(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16);
    p[2]=(uint8_t)(v>>8);  p[3]=(uint8_t)v;
}
static inline void put16_be(uint8_t *p, uint16_t v) {
    p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v;
}

static uint16_t udp_cksum(uint32_t src_ip, uint32_t dst_ip,
                           const void *udp_seg, uint16_t udp_len) {
    uint32_t sum = 0;
    sum += (src_ip >> 16) & 0xffff;
    sum += src_ip & 0xffff;
    sum += (dst_ip >> 16) & 0xffff;
    sum += dst_ip & 0xffff;
    sum += 17u;
    sum += (uint32_t)udp_len;
    const uint8_t *b = (const uint8_t *)udp_seg;
    uint16_t rem = udp_len;
    while (rem > 1) { sum += ((uint32_t)b[0]<<8)|b[1]; b+=2; rem-=2; }
    if (rem) sum += (uint32_t)b[0]<<8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ------------------------------------------------------------------ */
/* Table helpers (table_lock held)                                     */
/* ------------------------------------------------------------------ */

static bool port_in_use_locked(uint16_t port) {
    for (struct udp_socket *s = udp_hash[port % UDP_HASH_SIZE]; s;
         s = s->hash_next) {
        if (s->used && s->bound && s->local_port == port)
            return true;
    }
    return false;
}

static uint16_t alloc_ephemeral_locked(void) {
    for (uint16_t i = 0; i < (UDP_PORT_EPHEMERAL_MAX - UDP_PORT_EPHEMERAL_MIN + 1);
         i++) {
        uint16_t p = (uint16_t)(((next_ephemeral - UDP_PORT_EPHEMERAL_MIN + i) %
                      (UDP_PORT_EPHEMERAL_MAX - UDP_PORT_EPHEMERAL_MIN + 1)) +
                      UDP_PORT_EPHEMERAL_MIN);
        if (!port_in_use_locked(p)) {
            next_ephemeral = (uint16_t)(p == UDP_PORT_EPHEMERAL_MAX
                             ? UDP_PORT_EPHEMERAL_MIN : p + 1);
            return p;
        }
    }
    return 0;
}

static void hash_insert_locked(struct udp_socket *s) {
    uint32_t idx = s->local_port % UDP_HASH_SIZE;
    s->hash_next = udp_hash[idx];
    udp_hash[idx] = s;
}

static void hash_remove_locked(struct udp_socket *s) {
    uint32_t idx = s->local_port % UDP_HASH_SIZE;
    struct udp_socket **pp = &udp_hash[idx];
    while (*pp) {
        if (*pp == s) {
            *pp = s->hash_next;
            s->hash_next = NULL;
            return;
        }
        pp = &(*pp)->hash_next;
    }
}

static void free_locked(struct udp_socket *s) {
    hash_remove_locked(s);
    struct udp_socket **lp = &udp_list;
    while (*lp) {
        if (*lp == s) {
            *lp = s->list_next;
            break;
        }
        lp = &(*lp)->list_next;
    }
    kfree(s->rx_data);
    kfree(s->rx_desc);
    s->used = false;
    udp_count--;
    kfree(s);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void udp_init(void) {
    spinlock_init(&table_lock);
    memset(udp_hash, 0, sizeof(udp_hash));
    udp_list = NULL;
    udp_count = 0;
    next_ephemeral = UDP_PORT_EPHEMERAL_MIN;
}

struct udp_socket *udp_socket_alloc(void) {
    struct udp_socket *s = kmalloc(sizeof(struct udp_socket));
    if (!s)
        return NULL;
    memset(s, 0, sizeof(*s));
    spinlock_init(&s->lock);

    s->rx_capacity = UDP_DEFAULT_RCVBUF;
    s->rx_depth = UDP_DEFAULT_RX_DEPTH;
    s->rx_data = kmalloc(s->rx_capacity);
    s->rx_desc = kmalloc(sizeof(struct udp_rxdesc) * s->rx_depth);
    if (!s->rx_data || !s->rx_desc) {
        kfree(s->rx_data);
        kfree(s->rx_desc);
        kfree(s);
        return NULL;
    }

    spinlock_acquire(&table_lock);
    if (udp_count >= UDP_MAX_SOCKETS) {
        spinlock_release(&table_lock);
        kfree(s->rx_data);
        kfree(s->rx_desc);
        kfree(s);
        return NULL;
    }
    s->used = true;
    udp_count++;
    s->list_next = udp_list;
    udp_list = s;
    spinlock_release(&table_lock);
    return s;
}

void udp_socket_free(struct udp_socket *s) {
    if (!s)
        return;
    spinlock_acquire(&table_lock);
    s->wait_queue = NULL;
    s->vfs_node = NULL;
    s->reap = true;
    if (s->refs == 0)
        free_locked(s);
    spinlock_release(&table_lock);
}

/* ------------------------------------------------------------------ */
/* Bind / connect                                                      */
/* ------------------------------------------------------------------ */

int udp_bind(struct udp_socket *s, uint32_t ip, uint16_t port) {
    if (!s) return -22;
    spinlock_acquire(&table_lock);
    if (!s->used || s->bound) {
        spinlock_release(&table_lock);
        return -22;
    }
    if (port) {
        if (port_in_use_locked(port)) {
            spinlock_release(&table_lock);
            return -98;
        }
    } else {
        port = alloc_ephemeral_locked();
        if (!port) {
            spinlock_release(&table_lock);
            return -98;
        }
    }
    s->local_ip   = ip;
    s->local_port = port;
    s->bound      = true;
    hash_insert_locked(s);
    spinlock_release(&table_lock);
    return 0;
}

int udp_connect(struct udp_socket *s, uint32_t ip, uint16_t port) {
    if (!s) return -22;
    if (!s->bound) {
        int r = udp_bind(s, 0, 0);
        if (r < 0) return r;
    }
    spinlock_acquire(&s->lock);
    s->remote_ip   = ip;
    s->remote_port = port;
    s->connected   = true;
    spinlock_release(&s->lock);
    return 0;
}

int udp_disconnect(struct udp_socket *s) {
    if (!s) return -22;
    spinlock_acquire(&s->lock);
    s->remote_ip   = 0;
    s->remote_port = 0;
    s->connected   = false;
    spinlock_release(&s->lock);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Transmit                                                            */
/* ------------------------------------------------------------------ */

#define UDP_HDR_LEN 8

ssize_t udp_sendto(struct udp_socket *s, const void *buf, size_t len,
                   uint32_t dst_ip, uint16_t dst_port) {
    if (!s || !buf) return -22;
    if (len > UDP_PAYLOAD_MAX) return -90;

    if (!s->bound) {
        int r = udp_bind(s, 0, 0);
        if (r < 0) return r;
    }

    const struct ipv4_config *cfg = ipv4_get_config();
    if (!cfg || !cfg->address) return -101;

    uint16_t udp_len = (uint16_t)(UDP_HDR_LEN + len);
    uint8_t seg[UDP_HDR_LEN + UDP_PAYLOAD_MAX];
    put16_be(seg + 0, s->local_port);
    put16_be(seg + 2, dst_port);
    put16_be(seg + 4, udp_len);
    seg[6] = 0; seg[7] = 0;
    memcpy(seg + UDP_HDR_LEN, buf, len);

    uint16_t csum = udp_cksum(cfg->address, dst_ip, seg, udp_len);
    put16_be(seg + 6, csum);

    int r = ipv4_send_raw_tos(dst_ip, 17, s->tos, seg, udp_len);
    return r < 0 ? (ssize_t)r : (ssize_t)len;
}

/* ------------------------------------------------------------------ */
/* Receive                                                             */
/* ------------------------------------------------------------------ */

ssize_t udp_recvfrom(struct udp_socket *s, void *buf, size_t len,
                     uint32_t *src_ip, uint16_t *src_port,
                     bool nonblocking, int timeout_ms) {
    if (!s || !buf) return -22;

    uint64_t deadline = 0;
    if (timeout_ms > 0)
        deadline = lapic_timer_get_ticks() + (uint32_t)timeout_ms;
    struct thread *self = sched_get_current();

    for (;;) {
        spinlock_acquire(&s->lock);
        if (!s->used) {
            spinlock_release(&s->lock);
            return -105;
        }
        if (s->q_head != s->q_tail) {
            struct udp_rxdesc *d = &s->rx_desc[s->q_tail % s->rx_depth];
            size_t copy = d->len < len ? d->len : len;
            size_t off = d->data_off % s->rx_capacity;
            size_t first = s->rx_capacity - off;
            if (copy <= first) {
                memcpy(buf, s->rx_data + off, copy);
            } else {
                memcpy(buf, s->rx_data + off, first);
                memcpy((uint8_t *)buf + first, s->rx_data, copy - first);
            }
            if (src_ip)   *src_ip   = d->src_ip;
            if (src_port) *src_port = d->src_port;
            size_t full_len = d->len;
            s->rx_read += d->len;
            s->q_tail++;
            spinlock_release(&s->lock);
            return (ssize_t)(full_len > len ? len : full_len);
        }
        spinlock_release(&s->lock);

        if (nonblocking) return -11;
        if (timeout_ms > 0 && lapic_timer_get_ticks() >= deadline) return -11;

        if (s->wait_queue) {
            wait_queue_t *wq = (wait_queue_t *)s->wait_queue;
            wait_queue_entry_t entry = { .thread = self, .next = NULL };
            wait_queue_add(wq, &entry);
            spinlock_acquire(&s->lock);
            bool empty = s->q_head == s->q_tail;
            bool alive = s->used;
            spinlock_release(&s->lock);
            if (!alive) {
                wait_queue_remove(wq, &entry);
                return -105;
            }
            if (empty && self) {
                self->state = THREAD_BLOCKED;
                if (timeout_ms > 0)
                    self->wakeup_ticks = deadline;
                else
                    self->wakeup_ticks = lapic_timer_get_ticks() + 100;
            }
            if (!empty) {
                wait_queue_remove(wq, &entry);
                continue;
            }
            sched_yield();
            if (self) self->wakeup_ticks = 0;
            wait_queue_remove(wq, &entry);
        } else {
            if (timeout_ms > 0 && self)
                self->wakeup_ticks = deadline;
            else if (self)
                self->wakeup_ticks = lapic_timer_get_ticks() + 50;
            sched_yield();
            if (self) self->wakeup_ticks = 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Delivery                                                            */
/* ------------------------------------------------------------------ */

void udp_deliver(uint32_t src_ip, uint16_t src_port,
                 uint16_t dst_port, const uint8_t *payload, uint16_t length) {
    const struct ipv4_config *cfg = ipv4_get_config();

    spinlock_acquire(&table_lock);
    struct udp_socket *s = NULL;
    for (struct udp_socket *t = udp_hash[dst_port % UDP_HASH_SIZE]; t;
         t = t->hash_next) {
        if (!t->used || !t->bound || t->local_port != dst_port)
            continue;
        if (t->local_ip != 0 && (!cfg || t->local_ip != cfg->address))
            continue;
        if (t->connected &&
            (t->remote_ip != src_ip || t->remote_port != src_port))
            continue;
        s = t;
        break;
    }
    if (s)
        s->refs++;
    spinlock_release(&table_lock);

    if (!s)
        return;

    size_t copy = length > UDP_PAYLOAD_MAX ? UDP_PAYLOAD_MAX : length;
    bool wake = false;

    spinlock_acquire(&s->lock);
    for (;;) {
        size_t free_bytes = s->rx_capacity - (s->rx_write - s->rx_read);
        bool full = (s->q_head - s->q_tail) >= s->rx_depth ||
                    free_bytes < copy;
        if (!full)
            break;
        if (s->connected && s->q_head != s->q_tail) {
            /* Real-time traffic (connected sockets): prefer fresh data,
             * drop the oldest queued datagram. */
            struct udp_rxdesc *old = &s->rx_desc[s->q_tail % s->rx_depth];
            s->rx_read += old->len;
            s->q_tail++;
            s->dropped++;
            continue;
        }
        s->dropped++;
        copy = 0;
        break;
    }

    if (copy) {
        bool was_empty = (s->q_head == s->q_tail);
        size_t off = s->rx_write % s->rx_capacity;
        size_t first = s->rx_capacity - off;
        if (copy <= first) {
            memcpy(s->rx_data + off, payload, copy);
        } else {
            memcpy(s->rx_data + off, payload, first);
            memcpy(s->rx_data, payload + first, copy - first);
        }
        struct udp_rxdesc *d = &s->rx_desc[s->q_head % s->rx_depth];
        d->src_ip   = src_ip;
        d->src_port = src_port;
        d->len      = (uint16_t)copy;
        d->data_off = s->rx_write;
        s->rx_write += copy;
        s->q_head++;
        wake = was_empty;
    }
    spinlock_release(&s->lock);

    if (wake) {
        if (s->wait_queue)
            wait_queue_wake_all((wait_queue_t *)s->wait_queue);
        if (s->vfs_node)
            epoll_notify_event((struct vfs_node *)s->vfs_node, 0x00000001u);
    }

    spinlock_acquire(&table_lock);
    s->refs--;
    if (s->refs == 0 && s->reap)
        free_locked(s);
    spinlock_release(&table_lock);
}

/* ------------------------------------------------------------------ */
/* Options                                                             */
/* ------------------------------------------------------------------ */

void udp_set_rcvbuf(struct udp_socket *s, size_t bytes) {
    if (!s)
        return;
    if (bytes < UDP_MIN_RCVBUF)
        bytes = UDP_MIN_RCVBUF;
    if (bytes > UDP_MAX_RCVBUF)
        bytes = UDP_MAX_RCVBUF;

    uint8_t *nb = kmalloc(bytes);
    if (!nb)
        return;

    spinlock_acquire(&s->lock);
    if (!s->used || s->rx_capacity == bytes) {
        spinlock_release(&s->lock);
        kfree(nb);
        return;
    }
    if (s->q_head != s->q_tail) {
        /* Only grow when data is queued; shrinking would need a re-pack. */
        if (bytes < s->rx_capacity) {
            spinlock_release(&s->lock);
            kfree(nb);
            return;
        }
        /* Copy queued payloads linearly into the new buffer. */
        size_t used = s->rx_write - s->rx_read;
        for (size_t i = 0; i < used; i++)
            nb[i] = s->rx_data[(s->rx_read + i) % s->rx_capacity];
        for (uint32_t q = s->q_tail; q != s->q_head; q++) {
            struct udp_rxdesc *d = &s->rx_desc[q % s->rx_depth];
            size_t rel = d->data_off - s->rx_read;
            d->data_off = rel;
        }
        s->rx_read = 0;
        s->rx_write = used;
    } else {
        s->rx_read = s->rx_write = 0;
    }
    uint8_t *old = s->rx_data;
    s->rx_data = nb;
    s->rx_capacity = bytes;
    spinlock_release(&s->lock);
    kfree(old);
}

void udp_set_tos(struct udp_socket *s, int tos) {
    if (!s)
        return;
    spinlock_acquire(&s->lock);
    if (s->used)
        s->tos = (uint8_t)(tos & 0xff);
    spinlock_release(&s->lock);
}

/* ------------------------------------------------------------------ */
/* Introspection                                                        */
/* ------------------------------------------------------------------ */

int udp_get_snapshot(struct udp_entry_snapshot *out, int max) {
    int n = 0;
    if (!out || max <= 0)
        return 0;
    spinlock_acquire(&table_lock);
    for (struct udp_socket *s = udp_list; s && n < max; s = s->list_next) {
        if (!s->used || !s->bound)
            continue;
        out[n].local_ip    = s->local_ip;
        out[n].local_port  = s->local_port;
        out[n].remote_ip   = s->remote_ip;
        out[n].remote_port = s->remote_port;
        out[n].connected   = s->connected;
        n++;
    }
    spinlock_release(&table_lock);
    return n;
}

bool net_phase6_init(void) {
    udp_init();
    klog_puts("[NET] UDP initialized (dynamic sockets)\n");
    return true;
}
