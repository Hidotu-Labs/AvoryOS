#include "net/tcp.h"
#include "apic/lapic_timer.h"
#include "console/klog.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "lock/spinlock.h"
#include "mm/heap.h"
#include "net/core.h"
#include "net/ipv4.h"
#include "net/ipv6.h"
#include "sched/sched.h"
#include "sched/wait.h"
#include "socket/epoll.h"

#define FIN     0x01
#define SYN     0x02
#define RST     0x04
#define ACK     0x10
#define PSH     0x08

#define RETRIES 8

#define TCP_PORT_MIN 32768
#define TCP_PORT_MAX 60999
#define TCP_PORT_COUNT (TCP_PORT_MAX - TCP_PORT_MIN + 1)
#define TCP_PORT_BITMAP_WORDS ((TCP_PORT_COUNT + 31) / 32)

#define TCP_HASH_SIZE 512
#define TCP_TIMER_WORK_MAX 24

/* Reassembly segment (lazily allocated on out-of-order arrival). */
struct tcp_ooo_seg {
  struct tcp_ooo_seg *next;
  uint32_t seq;
  uint16_t len;
  uint8_t data[];
};

/* Actions produced while holding the TCP lock, performed after it is dropped. */
struct tcp_rx_actions {
  bool send_ack;
  bool retransmit;
  uint32_t retx_seq;
  bool output;
};

enum tcp_work_kind {
  WORK_ACK,
  WORK_RETX,
  WORK_RETX_FIN,
  WORK_RETX_SYN,
  WORK_PROBE,
  WORK_OUTPUT,
};

struct tcp_work {
  struct tcp_tcb *t;
  uint32_t seq;
  uint8_t kind;
};

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */

static spinlock_t lock = SPINLOCK_INIT;
static struct tcp_stats stats;

static struct tcp_tcb *hash[TCP_HASH_SIZE];
static struct tcp_tcb *active_list;
static int active_count;

static uint32_t port_bitmap[TCP_PORT_BITMAP_WORDS];
static uint16_t next_ephemeral = TCP_PORT_MIN;
static uint32_t iss_counter = 0x90000000u;

static inline bool seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static inline bool seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
static inline bool seq_gt(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }
static inline bool seq_ge(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }

static void wake(struct tcp_tcb *t);

static uint16_t g16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t g32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |
           p[3];
}

static void p16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void p32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t csum(uint32_t s, uint32_t d, const uint8_t *p, size_t n)
{
    uint64_t x;

    x = (s >> 16) + (s & 0xffff) +
        (d >> 16) + (d & 0xffff) +
        6 + (uint64_t)n;

    while (n >= 4) {
        uint32_t v;
        __builtin_memcpy(&v, p, 4);
        x += __builtin_bswap32(v);
        p += 4;
        n -= 4;
    }

    if (n >= 2) {
        uint16_t v;
        __builtin_memcpy(&v, p, 2);
        x += __builtin_bswap16(v);
        p += 2;
        n -= 2;
    }

    if (n)
        x += (uint64_t)*p << 8;

    while (x >> 16)
        x = (x & 0xffff) + (x >> 16);

    return (uint16_t)~x;
}

static uint16_t csum6(const uint8_t s[16], const uint8_t d[16],
                      const uint8_t *p, size_t n)
{
    uint32_t x = 6 + (uint32_t)n;
    for (int i = 0; i < 16; i += 2) {
        x += g16(s + i);
        x += g16(d + i);
    }
    size_t left = n;
    while (left > 1) { x += g16(p); p += 2; left -= 2; }
    if (left) x += (uint32_t)*p << 8;
    while (x >> 16) x = (x & 0xffff) + (x >> 16);
    return (uint16_t)~x;
}

/* ------------------------------------------------------------------ */
/* Port bitmap (32768 - 60999, Linux default ip_local_port_range)      */
/* ------------------------------------------------------------------ */

static inline uint32_t port_index(uint16_t port)
{
    return (uint32_t)(port - TCP_PORT_MIN);
}

static bool port_bit_test(uint16_t port)
{
    if (port < TCP_PORT_MIN || port > TCP_PORT_MAX)
        return false;
    uint32_t i = port_index(port);
    return (port_bitmap[i / 32] & (1u << (i % 32))) != 0;
}

static void port_bit_set(uint16_t port)
{
    if (port < TCP_PORT_MIN || port > TCP_PORT_MAX)
        return;
    uint32_t i = port_index(port);
    port_bitmap[i / 32] |= (1u << (i % 32));
}

static void port_bit_clear(uint16_t port)
{
    if (port < TCP_PORT_MIN || port > TCP_PORT_MAX)
        return;
    uint32_t i = port_index(port);
    port_bitmap[i / 32] &= ~(1u << (i % 32));
}

static uint16_t alloc_ephemeral_locked(void)
{
    for (uint32_t n = 0; n < TCP_PORT_COUNT; n++) {
        uint16_t candidate = next_ephemeral;
        next_ephemeral = candidate == TCP_PORT_MAX
                             ? TCP_PORT_MIN
                             : (uint16_t)(candidate + 1);
        if (!port_bit_test(candidate)) {
            port_bit_set(candidate);
            return candidate;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Hash table and active list                                          */
/* ------------------------------------------------------------------ */

static void hash_insert_locked(struct tcp_tcb *t)
{
    uint32_t idx = t->local_port % TCP_HASH_SIZE;
    t->hash_next = hash[idx];
    hash[idx] = t;
}

static void hash_remove_locked(struct tcp_tcb *t)
{
    uint32_t idx = t->local_port % TCP_HASH_SIZE;
    struct tcp_tcb **pp = &hash[idx];
    while (*pp) {
        if (*pp == t) {
            *pp = t->hash_next;
            t->hash_next = NULL;
            return;
        }
        pp = &(*pp)->hash_next;
    }
}

static void list_insert_locked(struct tcp_tcb *t)
{
    t->list_prev = NULL;
    t->list_next = active_list;
    if (active_list)
        active_list->list_prev = t;
    active_list = t;
}

static void list_remove_locked(struct tcp_tcb *t)
{
    if (t->list_prev)
        t->list_prev->list_next = t->list_next;
    else
        active_list = t->list_next;
    if (t->list_next)
        t->list_next->list_prev = t->list_prev;
    t->list_prev = NULL;
    t->list_next = NULL;
}

static bool port_owned_by_other_locked(uint16_t port, struct tcp_tcb *except)
{
    for (struct tcp_tcb *o = active_list; o; o = o->list_next) {
        if (o != except && o->used && o->port_owned && o->local_port == port)
            return true;
    }
    return false;
}

static struct tcp_tcb *find_locked(uint32_t ip, uint16_t sp, uint16_t dp)
{
    for (struct tcp_tcb *t = hash[dp % TCP_HASH_SIZE]; t; t = t->hash_next) {
        if (!t->used || t->state == TCP_LISTEN || t->address_family != 4)
            continue;
        if (t->local_port == dp && t->remote_port == sp && t->remote_ip == ip)
            return t;
    }
    return NULL;
}

static struct tcp_tcb *find6_locked(const uint8_t ip[16], uint16_t sp,
                                    uint16_t dp)
{
    for (struct tcp_tcb *t = hash[dp % TCP_HASH_SIZE]; t; t = t->hash_next) {
        if (!t->used || t->state == TCP_LISTEN || t->address_family != 6)
            continue;
        if (t->local_port == dp && t->remote_port == sp &&
            !memcmp(t->remote_ip6, ip, 16))
            return t;
    }
    return NULL;
}

static struct tcp_tcb *find_listener_locked(uint16_t port, uint8_t family)
{
    for (struct tcp_tcb *t = hash[port % TCP_HASH_SIZE]; t; t = t->hash_next) {
        if (!t->used || t->state != TCP_LISTEN || t->local_port != port)
            continue;
        if (t->address_family == family)
            return t;
    }
    return NULL;
}

/* True when every TCB holding this port is a dying one (TIME_WAIT etc). */
static bool port_reusable_locked(uint16_t port)
{
    for (struct tcp_tcb *o = active_list; o; o = o->list_next) {
        if (!o->used || o->local_port != port)
            continue;
        if (o->state == TCP_TIME_WAIT || o->state == TCP_CLOSED ||
            o->state == TCP_RESET)
            continue;
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* TCB lifecycle                                                       */
/* ------------------------------------------------------------------ */

static struct tcp_tcb *tcb_create(void)
{
    struct tcp_tcb *t = kmalloc(sizeof(struct tcp_tcb));
    if (!t)
        return NULL;
    memset(t, 0, sizeof(*t));

    t->rx_buffer = kmalloc(TCP_DEFAULT_RCVBUF);
    if (!t->rx_buffer) {
        kfree(t);
        return NULL;
    }
    t->rx_capacity = TCP_DEFAULT_RCVBUF;

    t->tx_buffer = kmalloc(TCP_DEFAULT_SNDBUF);
    if (!t->tx_buffer) {
        kfree(t->rx_buffer);
        kfree(t);
        return NULL;
    }
    t->tx_capacity = TCP_DEFAULT_SNDBUF;
    return t;
}

static void tcb_init_locked(struct tcp_tcb *t)
{
    t->used = true;
    t->state = TCP_CLOSED;
    t->mss = TCP_MAX_MSS;
    t->rcv_wnd = TCP_DEFAULT_WINDOW;
    t->sndbuf = TCP_DEFAULT_SNDBUF;
    t->linger_seconds = -1;
    t->keepcnt = TCP_KEEPALIVE_CNT_DEFAULT;
    t->keepidle_ms = (uint32_t)TCP_KEEPALIVE_IDLE_DEFAULT * 1000u;
    t->keepintvl_ms = (uint32_t)TCP_KEEPALIVE_INTVL_DEFAULT * 1000u;
    t->rto_ms = TCP_DEFAULT_RTO_MS;
    t->rcv_wscale = TCP_WS_SHIFT;
    t->last_activity = lapic_timer_get_ticks();
    list_insert_locked(t);
    active_count++;
}

static void free_tcb_locked(struct tcp_tcb *t)
{
    if (!t || !t->used)
        return;

    hash_remove_locked(t);
    list_remove_locked(t);

    if (t->syn_accounted && t->listener) {
        if (t->listener->syn_backlog > 0)
            t->listener->syn_backlog--;
        t->syn_accounted = false;
    }

    for (struct tcp_tcb *c = active_list; c; c = c->list_next) {
        if (c != t && c->listener == t) {
            c->listener = NULL;
            c->reap = true;
            c->syn_accounted = false;
        }
    }

    if (t->accept_queue) {
        kfree(t->accept_queue);
        t->accept_queue = NULL;
        t->accept_cap = 0;
        t->accept_head = t->accept_tail = 0;
    }

    struct tcp_ooo_seg *s = t->ooo_head;
    while (s) {
        struct tcp_ooo_seg *next = s->next;
        kfree(s);
        s = next;
    }
    t->ooo_head = NULL;

    if (t->port_owned && !port_owned_by_other_locked(t->local_port, t))
        port_bit_clear(t->local_port);

    t->used = false;
    active_count--;
    kfree(t->rx_buffer);
    t->rx_buffer = NULL;
    kfree(t->tx_buffer);
    t->tx_buffer = NULL;
    kfree(t);
}

struct tcp_tcb *tcp_alloc(void)
{
    struct tcp_tcb *t = tcb_create();
    if (!t)
        return NULL;

    spinlock_acquire(&lock);
    if (active_count >= TCP_MAX_CONNECTIONS) {
        spinlock_release(&lock);
        kfree(t->rx_buffer);
        kfree(t->tx_buffer);
        kfree(t);
        return NULL;
    }
    tcb_init_locked(t);
    spinlock_release(&lock);
    return t;
}

static struct tcp_tcb *alloc_locked(void)
{
    if (active_count >= TCP_MAX_CONNECTIONS)
        return NULL;
    struct tcp_tcb *t = tcb_create();
    if (!t)
        return NULL;
    tcb_init_locked(t);
    return t;
}

void tcp_free(struct tcp_tcb *t)
{
    if (!t)
        return;
    spinlock_acquire(&lock);
    if (t->used) {
        t->socket_ref = false;
        if (t->refs > 0)
            t->reap = true;
        else
            free_tcb_locked(t);
    }
    spinlock_release(&lock);
}

void tcp_put(struct tcp_tcb *t)
{
    if (!t)
        return;

    spinlock_acquire(&lock);
    bool had_ref = t->socket_ref;
    t->socket_ref = false;
    if (had_ref) {
        t->wait_queue = NULL;
        t->vfs_node = NULL;
    }
    enum tcp_state st = t->state;
    spinlock_release(&lock);

    if (had_ref && (st == TCP_ESTABLISHED || st == TCP_CLOSE_WAIT))
        tcp_close(t);

    spinlock_acquire(&lock);
    if (t->used && t->refs == 0 &&
        (t->reap || t->state == TCP_CLOSED || t->state == TCP_RESET ||
         t->state == TCP_LISTEN || t->state == TCP_FIN_WAIT_2))
        free_tcb_locked(t);
    spinlock_release(&lock);
}

void tcp_abort(struct tcp_tcb *t)
{
    if (!t)
        return;
    spinlock_acquire(&lock);
    if (t->used) {
        if (t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT) {
            t->state = TCP_RESET;
            if (!t->error)
                t->error = 104; /* ECONNRESET */
            wake(t);
        }
        t->deadline = 0;
        t->reap = true;
        if (t->refs == 0 && !t->socket_ref)
            free_tcb_locked(t);
    }
    spinlock_release(&lock);
}

void tcp_attach_socket(struct tcp_tcb *t, void *wait_queue, void *vfs_node)
{
    if (!t)
        return;
    spinlock_acquire(&lock);
    if (t->used) {
        t->socket_ref = true;
        t->reap = false;
        t->wait_queue = wait_queue;
        t->vfs_node = vfs_node;
    }
    spinlock_release(&lock);
}

static void tcb_put_locked(struct tcp_tcb *t)
{
    if (!t || !t->used)
        return;
    if (t->refs > 0)
        t->refs--;
    if (t->refs == 0 &&
        (t->reap || t->state == TCP_CLOSED || t->state == TCP_RESET) &&
        !t->socket_ref)
        free_tcb_locked(t);
}

/* ------------------------------------------------------------------ */
/* Wakeups                                                             */
/* ------------------------------------------------------------------ */

static void wake(struct tcp_tcb *t)
{
    if (t->wait_queue)
        wait_queue_wake_all((wait_queue_t *)t->wait_queue);

    if (t->vfs_node) {
        uint32_t ev = 0;
        if (t->rx_head != t->rx_tail || t->peer_closed ||
            t->state == TCP_CLOSE_WAIT || t->state == TCP_TIME_WAIT ||
            t->state == TCP_CLOSED || t->state == TCP_RESET ||
            t->state == TCP_LISTEN)
            ev |= 0x1; /* EPOLLIN */
        if (tcp_writable(t))
            ev |= 0x4; /* EPOLLOUT */
        if (t->error)
            ev |= 0x8; /* EPOLLERR */
        if (t->peer_closed || t->state == TCP_CLOSE_WAIT ||
            t->state == TCP_CLOSED || t->state == TCP_RESET ||
            t->state == TCP_TIME_WAIT)
            ev |= 0x10 | 0x2000; /* EPOLLHUP | EPOLLRDHUP */
        if (ev)
            epoll_notify_event((vfs_node_t *)t->vfs_node, ev);
    }
}

/* ------------------------------------------------------------------ */
/* Segment emission                                                    */
/* ------------------------------------------------------------------ */

static int emit_at(struct tcp_tcb *t, uint32_t seq, uint8_t flags,
                   const void *data, size_t len)
{
    uint8_t segment[1514];
    size_t hdr_len = 20;
    bool is_syn = (flags & SYN) != 0;

    if (!t || len > TCP_MAX_MSS)
        return -90;

    if (is_syn)
        hdr_len = 28; /* MSS + NOP + Window Scale */

    memset(segment, 0, hdr_len + len);

    p16(segment,      t->local_port);
    p16(segment + 2,  t->remote_port);
    p32(segment + 4,  seq);
    p32(segment + 8,  t->rcv_nxt);

    segment[12] = (uint8_t)((hdr_len / 4) << 4);
    segment[13] = flags;

    size_t used = t->rx_head - t->rx_tail;
    size_t space = used < t->rx_capacity ? (t->rx_capacity - used) : 0;
    if (!is_syn)
        space >>= t->rcv_wscale;
    if (space > 65535)
        space = 65535;
    p16(segment + 14, (uint16_t)space);

    if (is_syn) {
        segment[20] = 0x02; /* MSS */
        segment[21] = 0x04;
        segment[22] = (uint8_t)(t->mss >> 8);
        segment[23] = (uint8_t)(t->mss & 0xff);
        segment[24] = 0x01; /* NOP */
        segment[25] = 0x03; /* Window Scale */
        segment[26] = 0x03;
        segment[27] = t->rcv_wscale;
    }

    if (len && data)
        memcpy(segment + hdr_len, data, len);

    if (t->address_family == 6) {
        p16(segment + 16, csum6(t->local_ip6, t->remote_ip6,
                                segment, hdr_len + len));
        return ipv6_send_raw(t->remote_ip6, 6, segment, hdr_len + len);
    }
    p16(segment + 16, csum(t->local_ip, t->remote_ip, segment, hdr_len + len));
    return ipv4_send_raw(t->remote_ip, 6, segment, hdr_len + len);
}

static int emit(struct tcp_tcb *t, uint8_t flags, const void *data, size_t len)
{
    return emit_at(t, t->snd_nxt, flags, data, len);
}

static uint32_t new_iss(void)
{
    iss_counter += 4096;
    return iss_counter;
}

/* ------------------------------------------------------------------ */
/* Transmit engine                                                     */
/* ------------------------------------------------------------------ */

static uint32_t send_window_locked(const struct tcp_tcb *t)
{
    uint32_t w = t->snd_wnd;
    if (t->cwnd && t->cwnd < w)
        w = t->cwnd;
    return w;
}

static void ring_copy_out(const struct tcp_tcb *t, uint32_t seq, void *dst,
                          size_t len)
{
    size_t off = seq % t->tx_capacity;
    size_t first = t->tx_capacity - off;
    if (len <= first) {
        memcpy(dst, t->tx_buffer + off, len);
    } else {
        memcpy(dst, t->tx_buffer + off, first);
        memcpy((uint8_t *)dst + first, t->tx_buffer, len - first);
    }
}

static void tcp_output(struct tcp_tcb *t)
{
    for (;;) {
        uint8_t segbuf[TCP_MAX_MSS];
        uint32_t seq;
        size_t len;
        uint64_t now;

        spinlock_acquire(&lock);
        if (!t->used || !t->tx_buffer || t->error ||
            (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT)) {
            spinlock_release(&lock);
            return;
        }
        now = lapic_timer_get_ticks();

        bool data_avail = seq_lt(t->snd_nxt, t->tx_head);
        uint32_t window = send_window_locked(t);
        uint32_t win_end = t->snd_una + window;
        bool win_open = data_avail && seq_lt(t->snd_nxt, win_end);

        if (!win_open) {
            if (data_avail && window == 0 && !t->persist_deadline)
                t->persist_deadline = now + t->rto_ms;

            if (!data_avail && t->fin_pending && !t->fin_sent &&
                t->snd_nxt == t->tx_head) {
                t->fin_pending = false;
                t->fin_sent = true;
                if (t->state == TCP_CLOSE_WAIT)
                    t->state = TCP_LAST_ACK;
                else
                    t->state = TCP_FIN_WAIT_1;
                seq = t->snd_nxt;
                t->snd_nxt += 1;
                t->deadline = now + t->rto_ms;
                spinlock_release(&lock);
                emit_at(t, seq, FIN | ACK, NULL, 0);
                return;
            }
            spinlock_release(&lock);
            return;
        }

        /* Nagle: hold a small tail segment while earlier data is unacked. */
        bool unacked = seq_gt(t->snd_nxt, t->snd_una);
        if (!t->nodelay && unacked) {
            size_t unsent = t->tx_head - t->snd_nxt;
            size_t flight = t->snd_nxt - t->snd_una;
            if (unsent < t->mss && flight + unsent < t->mss) {
                spinlock_release(&lock);
                return;
            }
        }

        uint32_t avail = win_end - t->snd_nxt;
        len = t->mss;
        if ((uint32_t)len > avail)
            len = avail;
        if ((uint32_t)len > t->tx_head - t->snd_nxt)
            len = t->tx_head - t->snd_nxt;
        if (!len) {
            spinlock_release(&lock);
            return;
        }

        seq = t->snd_nxt;
        ring_copy_out(t, seq, segbuf, len);
        t->snd_nxt += (uint32_t)len;
        if (!t->rtt_pending) {
            t->rtt_pending = true;
            t->rtt_seq = seq;
            t->rtt_start = now;
        }
        t->retransmit = false;
        t->last_activity = now;
        t->bytes_sent += len;
        t->persist_deadline = 0;
        if (!t->deadline)
            t->deadline = now + t->rto_ms;
        spinlock_release(&lock);

        emit_at(t, seq, ACK | PSH, segbuf, len);
    }
}

static void tcp_retransmit_segment(struct tcp_tcb *t, uint32_t seq)
{
    uint8_t segbuf[TCP_MAX_MSS];
    size_t len;

    spinlock_acquire(&lock);
    if (!t->used || !t->tx_buffer || !seq_ge(seq, t->snd_una) ||
        !seq_lt(seq, t->snd_nxt)) {
        spinlock_release(&lock);
        return;
    }
    len = t->mss;
    if ((uint32_t)len > t->snd_nxt - seq)
        len = t->snd_nxt - seq;
    ring_copy_out(t, seq, segbuf, len);
    t->retransmit = true;
    t->last_activity = lapic_timer_get_ticks();
    spinlock_release(&lock);

    emit_at(t, seq, ACK | PSH, segbuf, len);
}

static void tcp_retransmit_fin(struct tcp_tcb *t)
{
    uint32_t seq;
    spinlock_acquire(&lock);
    if (!t->used || !t->fin_sent || t->snd_una == t->snd_nxt) {
        spinlock_release(&lock);
        return;
    }
    seq = t->snd_una; /* FIN is the final unacked sequence byte */
    t->retransmit = true;
    spinlock_release(&lock);
    emit_at(t, seq, FIN | ACK, NULL, 0);
}

static void tcp_retransmit_syn(struct tcp_tcb *t)
{
    uint32_t seq;
    uint8_t flags;

    spinlock_acquire(&lock);
    if (!t->used) {
        spinlock_release(&lock);
        return;
    }
    if (t->state == TCP_SYN_SENT)
        flags = SYN;
    else if (t->state == TCP_SYN_RECEIVED)
        flags = SYN | ACK;
    else {
        spinlock_release(&lock);
        return;
    }
    seq = t->snd_una;
    t->retransmit = true;
    spinlock_release(&lock);
    emit_at(t, seq, flags, NULL, 0);
}

static void tcp_send_probe(struct tcp_tcb *t)
{
    uint32_t seq;
    spinlock_acquire(&lock);
    if (!t->used || t->state != TCP_ESTABLISHED || t->error) {
        spinlock_release(&lock);
        return;
    }
    seq = t->snd_una - 1;
    t->persist_deadline = lapic_timer_get_ticks() + (uint64_t)t->rto_ms * 2;
    spinlock_release(&lock);
    emit_at(t, seq, ACK, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* Bind / connect                                                      */
/* ------------------------------------------------------------------ */

int tcp_bind(struct tcp_tcb *t, uint32_t ip, uint16_t port)
{
    if (!t)
        return -22;

    spinlock_acquire(&lock);
    if (!t->used || t->local_port) {
        spinlock_release(&lock);
        return -22;
    }

    if (!port) {
        port = alloc_ephemeral_locked();
        if (!port) {
            spinlock_release(&lock);
            return -98;
        }
        t->port_owned = true;
    } else if (port_bit_test(port)) {
        if (!t->reuseaddr || !port_reusable_locked(port)) {
            spinlock_release(&lock);
            return -98;
        }
        for (struct tcp_tcb *o = active_list; o; o = o->list_next)
            if (o != t && o->port_owned && o->local_port == port)
                o->port_owned = false;
        t->port_owned = true;
    } else {
        port_bit_set(port);
        t->port_owned = true;
    }

    t->address_family = 4;
    t->local_ip = ip;
    t->local_port = port;
    hash_insert_locked(t);
    spinlock_release(&lock);
    return 0;
}

int tcp_bind6(struct tcp_tcb *t, const uint8_t ip[16], uint16_t port)
{
    if (!t || !ip)
        return -22;

    spinlock_acquire(&lock);
    if (!t->used || t->local_port) {
        spinlock_release(&lock);
        return -22;
    }

    if (!port) {
        port = alloc_ephemeral_locked();
        if (!port) {
            spinlock_release(&lock);
            return -98;
        }
        t->port_owned = true;
    } else if (port_bit_test(port)) {
        if (!t->reuseaddr || !port_reusable_locked(port)) {
            spinlock_release(&lock);
            return -98;
        }
        for (struct tcp_tcb *o = active_list; o; o = o->list_next)
            if (o != t && o->port_owned && o->local_port == port)
                o->port_owned = false;
        t->port_owned = true;
    } else {
        port_bit_set(port);
        t->port_owned = true;
    }

    t->address_family = 6;
    memcpy(t->local_ip6, ip, 16);
    t->local_port = port;
    hash_insert_locked(t);
    spinlock_release(&lock);
    return 0;
}

int tcp_active_open(struct tcp_tcb *t, uint32_t ip, uint16_t port)
{
    if (!t || !ip || !port)
        return -22;

    const struct ipv4_config *cfg = ipv4_get_config();
    if (!cfg || !cfg->address)
        return -101;

    spinlock_acquire(&lock);
    if (!t->used) {
        spinlock_release(&lock);
        return -22;
    }

    t->address_family = 4;
    t->local_ip = cfg->address;
    t->remote_ip = ip;
    t->remote_port = port;

    if (!t->local_port) {
        t->local_port = alloc_ephemeral_locked();
        if (!t->local_port) {
            spinlock_release(&lock);
            return -98;
        }
        t->port_owned = true;
        hash_insert_locked(t);
    }

    t->snd_una = new_iss();
    t->snd_nxt = t->snd_una + 1;
    t->tx_head = t->snd_nxt; /* ring invariant: no unsent/queued bytes yet */
    t->snd_wl1 = 0;
    t->snd_wl2 = 0;
    t->snd_wnd = 0;
    t->cwnd = 0;
    t->ssthresh = 0;
    t->in_recovery = false;
    t->dup_ack_count = 0;
    t->retries = 0;
    t->rtt_pending = false;
    t->persist_deadline = 0;
    t->fin_pending = false;
    t->fin_sent = false;
    t->peer_closed = false;
    t->state = TCP_SYN_SENT;
    t->reap = false;
    t->error = 0;
    t->deadline = lapic_timer_get_ticks() + t->rto_ms;
    t->last_activity = lapic_timer_get_ticks();
    uint32_t seq = t->snd_una;
    spinlock_release(&lock);

    return emit_at(t, seq, SYN, NULL, 0);
}

int tcp_active_open6(struct tcp_tcb *t, const uint8_t ip[16], uint16_t port)
{
    if (!t || !ip || !port)
        return -22;

    const struct ipv6_config *cfg = ipv6_get_config();
    bool link = ip[0] == 0xfe && (ip[1] & 0xc0) == 0x80;
    bool dest_global = (ip[0] & 0xe0) == 0x20;
    bool cfg_global = cfg->global_valid && ((cfg->global[0] & 0xe0) == 0x20);
    if (!link && (!cfg->global_valid || (dest_global && !cfg_global)))
        return -101;

    spinlock_acquire(&lock);
    if (!t->used) {
        spinlock_release(&lock);
        return -22;
    }

    t->address_family = 6;
    memcpy(t->local_ip6, link ? cfg->link_local : cfg->global, 16);
    memcpy(t->remote_ip6, ip, 16);
    t->remote_port = port;

    if (!t->local_port) {
        t->local_port = alloc_ephemeral_locked();
        if (!t->local_port) {
            spinlock_release(&lock);
            return -98;
        }
        t->port_owned = true;
        hash_insert_locked(t);
    }

    t->snd_una = new_iss();
    t->snd_nxt = t->snd_una + 1;
    t->tx_head = t->snd_nxt; /* ring invariant: no unsent/queued bytes yet */
    t->snd_wl1 = 0;
    t->snd_wl2 = 0;
    t->snd_wnd = 0;
    t->cwnd = 0;
    t->ssthresh = 0;
    t->in_recovery = false;
    t->dup_ack_count = 0;
    t->retries = 0;
    t->rtt_pending = false;
    t->persist_deadline = 0;
    t->fin_pending = false;
    t->fin_sent = false;
    t->peer_closed = false;
    t->state = TCP_SYN_SENT;
    t->reap = false;
    t->error = 0;
    t->deadline = lapic_timer_get_ticks() + t->rto_ms;
    t->last_activity = lapic_timer_get_ticks();
    uint32_t seq = t->snd_una;
    spinlock_release(&lock);

    return emit_at(t, seq, SYN, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* Listen / accept                                                     */
/* ------------------------------------------------------------------ */

int tcp_listen(struct tcp_tcb *t, int backlog)
{
    if (!t)
        return -22;

    if (!t->local_port) {
        int r = tcp_bind(t, 0, 0);
        if (r < 0)
            return r;
    }

    if (backlog < 1)
        backlog = 1;
    if (backlog > TCP_MAX_ACCEPT_BACKLOG)
        backlog = TCP_MAX_ACCEPT_BACKLOG;

    struct tcp_tcb **queue = kmalloc(sizeof(struct tcp_tcb *) * (size_t)backlog);
    if (!queue)
        return -105;

    spinlock_acquire(&lock);
    kfree(t->accept_queue);
    t->accept_queue = queue;
    t->accept_cap = (size_t)backlog;
    t->accept_head = t->accept_tail = 0;
    t->backlog = backlog;
    t->syn_backlog = 0;
    t->state = TCP_LISTEN;
    spinlock_release(&lock);
    return 0;
}

struct tcp_tcb *tcp_accept(struct tcp_tcb *t, bool nonblock)
{
    if (!t)
        return NULL;

    for (;;) {
        spinlock_acquire(&lock);
        if (!t->used || t->state != TCP_LISTEN) {
            spinlock_release(&lock);
            return NULL;
        }
        if (t->accept_head != t->accept_tail) {
            struct tcp_tcb *child =
                t->accept_queue[t->accept_tail % t->accept_cap];
            t->accept_tail++;
            child->listener = NULL;
            spinlock_release(&lock);
            return child;
        }
        spinlock_release(&lock);

        if (nonblock)
            return NULL;

        struct thread *cur = sched_get_current();

        if (cur && (cur->pending_signals & ~cur->signal_mask))
            return NULL;

        if (t->wait_queue) {
            wait_queue_t *wq = (wait_queue_t *)t->wait_queue;
            wait_queue_entry_t entry = { .thread = cur, .next = NULL };
            wait_queue_add(wq, &entry);
            if (cur) {
                cur->state = THREAD_BLOCKED;
                cur->wakeup_ticks = lapic_timer_get_ticks() + 100;
            }
            sched_yield();
            if (cur) cur->wakeup_ticks = 0;
            wait_queue_remove(wq, &entry);
        } else {
            sched_yield();
        }
    }
}

/* ------------------------------------------------------------------ */
/* Close / send / receive                                              */
/* ------------------------------------------------------------------ */

int tcp_close(struct tcp_tcb *t)
{
    bool send_fin = false;
    bool send_rst = false;
    uint32_t seq = 0;
    bool kick_output = false;

    if (!t)
        return -22;

    spinlock_acquire(&lock);
    if (!t->used) {
        spinlock_release(&lock);
        return -22;
    }

    uint64_t now = lapic_timer_get_ticks();

    if (t->linger_seconds == 0 &&
        (t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT)) {
        t->state = TCP_RESET;
        if (!t->error)
            t->error = 104;
        t->reap = true;
        t->deadline = 0;
        seq = t->snd_nxt;
        send_rst = true;
        wake(t);
    } else if (!t->fin_sent && !t->fin_pending &&
               (t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT)) {
        t->fin_pending = true;
        if (t->tx_head == t->snd_nxt) {
            /* No queued data: FIN can go out now. */
            t->fin_pending = false;
            t->fin_sent = true;
            if (t->state == TCP_CLOSE_WAIT)
                t->state = TCP_LAST_ACK;
            else
                t->state = TCP_FIN_WAIT_1;
            seq = t->snd_nxt;
            t->snd_nxt += 1;
            t->deadline = now + t->rto_ms;
            send_fin = true;
        } else {
            kick_output = true;
        }
    } else if (t->state == TCP_SYN_SENT || t->state == TCP_SYN_RECEIVED) {
        t->reap = true;
        t->deadline = 0;
    }
    spinlock_release(&lock);

    if (send_rst)
        return emit_at(t, seq, RST | ACK, NULL, 0);
    if (send_fin)
        return emit_at(t, seq, FIN | ACK, NULL, 0);
    if (kick_output) {
        tcp_output(t);
        spinlock_acquire(&lock);
        if (t->used && t->fin_pending && !t->fin_sent &&
            t->tx_head == t->snd_nxt) {
            uint8_t f = FIN | ACK;
            t->fin_pending = false;
            t->fin_sent = true;
            if (t->state == TCP_CLOSE_WAIT)
                t->state = TCP_LAST_ACK;
            else
                t->state = TCP_FIN_WAIT_1;
            seq = t->snd_nxt;
            t->snd_nxt += 1;
            t->deadline = lapic_timer_get_ticks() + t->rto_ms;
            spinlock_release(&lock);
            return emit_at(t, seq, f, NULL, 0);
        }
        spinlock_release(&lock);
    }
    return 0;
}

int tcp_send(struct tcp_tcb *t, const void *buf, size_t len, bool nonblock)
{
    size_t copied = 0;

    if (!t || !buf)
        return -22;
    if (!len)
        return 0;

    for (;;) {
        spinlock_acquire(&lock);
        if (!t->used || !t->tx_buffer) {
            spinlock_release(&lock);
            return copied ? (int)copied : -107;
        }
        if (t->state != TCP_ESTABLISHED) {
            int err = t->error ? -t->error : -107;
            spinlock_release(&lock);
            return copied ? (int)copied : err;
        }
        if (t->error) {
            int err = -t->error;
            spinlock_release(&lock);
            return copied ? (int)copied : err;
        }

        size_t used_bytes = t->tx_head - t->snd_una;
        size_t space = used_bytes < t->tx_capacity
                           ? t->tx_capacity - used_bytes
                           : 0;

        if (space == 0) {
            spinlock_release(&lock);
            if (nonblock || copied > 0)
                return copied ? (int)copied : -11;

            if (t->wait_queue) {
                wait_queue_t *wq = (wait_queue_t *)t->wait_queue;
                struct thread *cur = sched_get_current();
                wait_queue_entry_t entry = { .thread = cur, .next = NULL };
                wait_queue_add(wq, &entry);
                if (cur) {
                    cur->state = THREAD_BLOCKED;
                    cur->wakeup_ticks = lapic_timer_get_ticks() + 50;
                }
                sched_yield();
                if (cur) cur->wakeup_ticks = 0;
                wait_queue_remove(wq, &entry);
            } else {
                sched_yield();
            }
            continue;
        }

        size_t chunk = len - copied;
        if (chunk > space)
            chunk = space;

        size_t off = t->tx_head % t->tx_capacity;
        size_t first = t->tx_capacity - off;
        const uint8_t *src = (const uint8_t *)buf + copied;
        if (chunk <= first) {
            memcpy(t->tx_buffer + off, src, chunk);
        } else {
            memcpy(t->tx_buffer + off, src, first);
            memcpy(t->tx_buffer, src + first, chunk - first);
        }
        t->tx_head += (uint32_t)chunk;
        t->last_activity = lapic_timer_get_ticks();
        spinlock_release(&lock);

        copied += chunk;
        tcp_output(t);

        if (copied == len)
            return (int)copied;
        if (nonblock)
            return (int)copied;
    }
}

int tcp_recv(struct tcp_tcb *t, void *buf, size_t len, bool nonblock)
{
    for (;;) {
        size_t avail;

        if (!t || !buf)
            return -22;

        spinlock_acquire(&lock);

        if (!t->used || !t->rx_buffer || !t->rx_capacity) {
            spinlock_release(&lock);
            return -105;
        }

        avail = t->rx_head - t->rx_tail;

        if (avail) {
            size_t count = avail < len ? avail : len;

            size_t off = t->rx_tail % t->rx_capacity;
            size_t first = t->rx_capacity - off;
            if (count <= first) {
                memcpy(buf, &t->rx_buffer[off], count);
            } else {
                memcpy(buf, &t->rx_buffer[off], first);
                memcpy((uint8_t *)buf + first, &t->rx_buffer[0], count - first);
            }

            size_t used_before = t->rx_head - t->rx_tail;
            size_t old_space = used_before < t->rx_capacity
                                   ? (t->rx_capacity - used_before)
                                   : 0;
            t->rx_tail += count;
            size_t used_after = t->rx_head - t->rx_tail;

            bool send_ack = false;
            size_t new_space = used_after < t->rx_capacity
                                   ? (t->rx_capacity - used_after)
                                   : 0;
            if ((t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT) &&
                (t->unacked_packets > 0 ||
                 (old_space < t->mss && new_space >= t->mss))) {
                t->unacked_packets = 0;
                send_ack = true;
            }

            spinlock_release(&lock);
            if (send_ack)
                emit(t, ACK, NULL, 0);
            return (int)count;
        }

        if (t->peer_closed || t->state == TCP_CLOSE_WAIT ||
            t->state == TCP_TIME_WAIT) {
            spinlock_release(&lock);
            return 0;
        }

        if (t->state == TCP_CLOSED || t->state == TCP_RESET) {
            int err = t->error ? t->error
                               : (t->state == TCP_RESET ? 104 : 107);
            spinlock_release(&lock);
            return -err;
        }

        bool send_flush_ack = false;
        if ((t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT) &&
            t->unacked_packets > 0) {
            t->unacked_packets = 0;
            send_flush_ack = true;
        }

        int err = t->error;

        spinlock_release(&lock);

        if (send_flush_ack)
            emit(t, ACK, NULL, 0);

        if (err)
            return -err;

        if (nonblock)
            return -11;

        struct thread *cur = sched_get_current();

        if (cur && (cur->pending_signals & ~cur->signal_mask))
            return -4;

        if (t->wait_queue) {
            wait_queue_t *wq = (wait_queue_t *)t->wait_queue;
            wait_queue_entry_t entry = { .thread = cur, .next = NULL };
            wait_queue_add(wq, &entry);
            if (cur) {
                cur->state = THREAD_BLOCKED;
                cur->wakeup_ticks = lapic_timer_get_ticks() + 100;
            }
            sched_yield();
            if (cur) cur->wakeup_ticks = 0;
            wait_queue_remove(wq, &entry);
        } else {
            if (cur) cur->wakeup_ticks = lapic_timer_get_ticks() + 50;
            sched_yield();
            if (cur) cur->wakeup_ticks = 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Out-of-order reassembly                                             */
/* ------------------------------------------------------------------ */

static size_t ooo_drain_locked(struct tcp_tcb *t)
{
    size_t total = 0;
    for (;;) {
        struct tcp_ooo_seg *s = t->ooo_head;
        if (!s || s->seq != t->rcv_nxt)
            break;
        size_t used = t->rx_head - t->rx_tail;
        size_t space = used < t->rx_capacity ? (t->rx_capacity - used) : 0;
        if (s->len > space)
            break;

        size_t off = t->rx_head % t->rx_capacity;
        size_t first = t->rx_capacity - off;
        if (s->len <= first) {
            memcpy(t->rx_buffer + off, s->data, s->len);
        } else {
            memcpy(t->rx_buffer + off, s->data, first);
            memcpy(t->rx_buffer, s->data + first, s->len - first);
        }
        t->rx_head += s->len;
        t->rcv_nxt += s->len;
        t->bytes_recv += s->len;
        total += s->len;

        t->ooo_head = s->next;
        t->ooo_count--;
        t->ooo_bytes -= s->len;
        kfree(s);
        wake(t);
    }
    return total;
}

static void ooo_insert_locked(struct tcp_tcb *t, uint32_t seq,
                              const uint8_t *data, size_t len)
{
    if (!len)
        return;
    if ((uint32_t)(seq - t->rcv_nxt) >= t->rx_capacity)
        return;

    uint32_t start = seq;
    uint32_t end = seq + (uint32_t)len;

    /* Trim any parts already covered by queued segments. */
    struct tcp_ooo_seg *s = t->ooo_head;
    while (s && start < end) {
        uint32_t s_start = s->seq;
        uint32_t s_end = s->seq + s->len;
        if (end <= s_start || start >= s_end) {
            s = s->next;
            continue;
        }
        if (start < s_start) {
            end = s_start;
            break;
        }
        start = s_end;
        s = s->next;
    }
    if (start >= end)
        return;

    size_t new_len = end - start;
    if (t->ooo_count >= TCP_MAX_OOO_SEGS ||
        t->ooo_bytes + new_len > TCP_MAX_OOO_BYTES)
        return;

    struct tcp_ooo_seg *ns = kmalloc(sizeof(*ns) + new_len);
    if (!ns)
        return;
    ns->seq = start;
    ns->len = (uint16_t)new_len;
    memcpy(ns->data, data + (start - seq), new_len);

    struct tcp_ooo_seg **pp = &t->ooo_head;
    while (*pp && seq_lt((*pp)->seq, start))
        pp = &(*pp)->next;
    ns->next = *pp;
    *pp = ns;
    t->ooo_count++;
    t->ooo_bytes += new_len;
}

/* ------------------------------------------------------------------ */
/* ACK processing (lock held)                                          */
/* ------------------------------------------------------------------ */

static void ack_input_locked(struct tcp_tcb *t, uint32_t ack, uint8_t flags,
                             size_t len, bool wnd_grew, uint64_t now,
                             struct tcp_rx_actions *act)
{
    if (!(flags & ACK))
        return;

    if (seq_gt(ack, t->snd_una) && seq_le(ack, t->snd_nxt)) {
        uint32_t acked = ack - t->snd_una;
        t->snd_una = ack;
        t->retries = 0;
        t->dup_ack_count = 0;

        /* RTT sample (Karn's algorithm). */
        if (t->rtt_pending && !t->retransmit && seq_ge(ack, t->rtt_seq)) {
            int64_t sample = (int64_t)(now - t->rtt_start);
            if (sample < 1)
                sample = 1;
            if (sample > TCP_MAX_RTO_MS)
                sample = TCP_MAX_RTO_MS;
            if (t->srtt_ms == 0) {
                t->srtt_ms = (uint32_t)sample;
                t->rttvar_ms = (uint32_t)(sample / 2);
            } else {
                int64_t delta = sample - (int64_t)t->srtt_ms;
                if (delta < 0)
                    delta = -delta;
                t->rttvar_ms =
                    (uint32_t)((3 * (int64_t)t->rttvar_ms + delta) / 4);
                t->srtt_ms =
                    (uint32_t)((7 * (int64_t)t->srtt_ms + sample) / 8);
            }
            int64_t rto = (int64_t)t->srtt_ms + 4 * (int64_t)t->rttvar_ms;
            if (rto < TCP_MIN_RTO_MS)
                rto = TCP_MIN_RTO_MS;
            if (rto > TCP_MAX_RTO_MS)
                rto = TCP_MAX_RTO_MS;
            t->rto_ms = (uint32_t)rto;
            t->rtt_pending = false;
        }

        if (t->in_recovery) {
            if (seq_ge(ack, t->recover)) {
                t->in_recovery = false;
                t->cwnd = t->ssthresh ? t->ssthresh : t->mss;
            } else {
                /* NewReno partial ACK: retransmit the new head. */
                act->retransmit = true;
                act->retx_seq = t->snd_una;
                t->cwnd = (t->ssthresh ? t->ssthresh : t->mss) + 3 * t->mss;
                if (seq_ge(t->snd_una, t->snd_nxt))
                    t->in_recovery = false;
            }
        } else {
            if (t->cwnd < t->ssthresh) {
                t->cwnd += acked;
            } else {
                uint64_t inc =
                    ((uint64_t)t->mss * acked) / (t->cwnd ? t->cwnd : 1);
                if (inc < 1)
                    inc = 1;
                t->cwnd += (uint32_t)inc;
            }
            if (t->cwnd > TCP_CWND_MAX_BYTES)
                t->cwnd = TCP_CWND_MAX_BYTES;
        }

        if (seq_ge(ack, t->snd_nxt)) {
            t->deadline = 0;
            if (t->state == TCP_LAST_ACK)
                t->state = TCP_CLOSED;
            else if (t->state == TCP_FIN_WAIT_1) {
                t->state = TCP_FIN_WAIT_2;
                t->deadline = now + TCP_FIN_WAIT_2_TIMEOUT_MS;
            }
        } else {
            t->deadline = now + t->rto_ms;
        }

        act->output = true;
        wake(t);
        return;
    }

    /* Duplicate ACK: same sequence, no data, no window growth. */
    if (ack == t->snd_una && len == 0 && !(flags & (SYN | FIN)) && !wnd_grew &&
        t->snd_una != t->snd_nxt) {
        t->dup_acks++;
        if (++t->dup_ack_count == 3) {
            uint32_t flight = t->snd_nxt - t->snd_una;
            t->ssthresh = flight / 2;
            if (t->ssthresh < 2 * t->mss)
                t->ssthresh = 2 * t->mss;
            t->cwnd = t->ssthresh + 3 * t->mss;
            if (t->cwnd > TCP_CWND_MAX_BYTES)
                t->cwnd = TCP_CWND_MAX_BYTES;
            t->recover = t->snd_nxt;
            t->in_recovery = true;
            t->retransmit = true;
            t->deadline = now + t->rto_ms;
            act->retransmit = true;
            act->retx_seq = t->snd_una;
            stats.retransmits++;
            t->retransmits++;
        } else if (t->dup_ack_count > 3 && t->in_recovery) {
            t->cwnd += t->mss;
            if (t->cwnd > TCP_CWND_MAX_BYTES)
                t->cwnd = TCP_CWND_MAX_BYTES;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Segment processing (lock held)                                      */
/* ------------------------------------------------------------------ */

static void input_locked(struct tcp_tcb *t, uint32_t seq, uint32_t ack,
                         uint8_t flags, const void *payload, size_t len,
                         uint32_t raw_wnd, struct tcp_rx_actions *act)
{
    uint64_t now = lapic_timer_get_ticks();

    t->last_activity = now;
    t->keepalive_probing = false;
    t->keepalive_probes = 0;

    /* Update the peer window (unscaled during the SYN exchange). */
    uint32_t new_wnd;
    if ((flags & SYN) && t->state == TCP_SYN_SENT)
        new_wnd = raw_wnd;
    else
        new_wnd = raw_wnd << t->snd_wscale;

    bool wnd_grew = false;
    if (seq_gt(seq, t->snd_wl1) ||
        (seq == t->snd_wl1 && seq_ge(ack, t->snd_wl2))) {
        wnd_grew = new_wnd > t->snd_wnd;
        t->snd_wnd = new_wnd;
        t->snd_wl1 = seq;
        t->snd_wl2 = ack;
    }

    if (flags & RST) {
        t->error = t->state == TCP_SYN_SENT ? 111 : 104;
        t->state = TCP_RESET;
        t->deadline = 0;
        stats.resets++;
        wake(t);
        return;
    }

    if (t->state == TCP_SYN_SENT) {
        if ((flags & (SYN | ACK)) == (SYN | ACK) && ack == t->snd_nxt) {
            t->snd_una = ack;
            t->rcv_nxt = seq + 1;
            t->state = TCP_ESTABLISHED;
            t->retries = 0;
            t->deadline = 0;
            t->cwnd = TCP_INITIAL_CWND_MSS * t->mss;
            t->ssthresh = t->tx_capacity > 2 * t->mss
                              ? (uint32_t)t->tx_capacity
                              : 2 * t->mss;
            stats.connections_opened++;
            act->send_ack = true;
            act->output = true;
            wake(t);
        }
        return;
    }

    if (t->state == TCP_SYN_RECEIVED) {
        if ((flags & ACK) && ack == t->snd_nxt) {
            t->snd_una = ack;
            t->state = TCP_ESTABLISHED;
            t->retries = 0;
            t->deadline = 0;
            t->cwnd = TCP_INITIAL_CWND_MSS * t->mss;
            t->ssthresh = t->tx_capacity > 2 * t->mss
                              ? (uint32_t)t->tx_capacity
                              : 2 * t->mss;

            struct tcp_tcb *lis = t->listener;
            bool enqueued = false;
            if (lis && lis->accept_queue &&
                lis->accept_head - lis->accept_tail < lis->accept_cap) {
                lis->accept_queue[lis->accept_head % lis->accept_cap] = t;
                lis->accept_head++;
                if (t->syn_accounted) {
                    if (lis->syn_backlog > 0)
                        lis->syn_backlog--;
                    t->syn_accounted = false;
                }
                enqueued = true;
                stats.connections_opened++;
                wake(lis);
            }
            if (!enqueued) {
                stats.accept_overflow++;
                if (t->syn_accounted && lis) {
                    if (lis->syn_backlog > 0)
                        lis->syn_backlog--;
                    t->syn_accounted = false;
                }
                t->reap = true;
                t->state = TCP_CLOSED;
                t->error = 103; /* ECONNABORTED */
                wake(t);
            }
        }
        return;
    }

    ack_input_locked(t, ack, flags, len, wnd_grew, now, act);

    if (seq_lt(seq, t->rcv_nxt)) {
        uint32_t trim = t->rcv_nxt - seq;
        if (trim >= len) {
            stats.duplicates++;
            t->dup_acks++;
            if (t->ooo_ack_count < 8) {
                act->send_ack = true;
                t->ooo_ack_count++;
            }
            return;
        }
        payload = (const uint8_t *)payload + trim;
        len -= trim;
        seq = t->rcv_nxt;
    }

    if (len && seq == t->rcv_nxt) {
        size_t used = t->rx_head - t->rx_tail;
        size_t space = used < t->rx_capacity ? (t->rx_capacity - used) : 0;

        if (len > space) {
            t->dropped_rx++;
            if (t->ooo_ack_count < 8) {
                act->send_ack = true;
                t->ooo_ack_count++;
            }
            return;
        }

        size_t off = t->rx_head % t->rx_capacity;
        size_t first = t->rx_capacity - off;
        if (len <= first) {
            memcpy(&t->rx_buffer[off], payload, len);
        } else {
            memcpy(&t->rx_buffer[off], payload, first);
            memcpy(&t->rx_buffer[0], (const uint8_t *)payload + first,
                   len - first);
        }

        t->rx_head += len;
        t->rcv_nxt += (uint32_t)len;
        t->bytes_recv += len;
        t->ooo_ack_count = 0;

        size_t drained = ooo_drain_locked(t);

        if ((flags & PSH) || ++t->unacked_packets >= 2 || drained) {
            act->send_ack = true;
            t->unacked_packets = 0;
        }
        act->output = true;
        wake(t);
    } else if (len && seq_gt(seq, t->rcv_nxt)) {
        stats.out_of_order++;
        ooo_insert_locked(t, seq, payload, len);
        if (t->ooo_ack_count < 8) {
            act->send_ack = true;
            t->ooo_ack_count++;
        }
    }

    if (flags & FIN) {
        t->rcv_nxt++;
        t->peer_closed = true;

        if (t->state == TCP_ESTABLISHED) {
            t->state = TCP_CLOSE_WAIT;
            t->deadline = 0;
            t->retries = 0;
        } else {
            t->state = TCP_TIME_WAIT;
            t->deadline = now + TCP_TIME_WAIT_TIMEOUT_MS;
        }

        act->send_ack = true;
        wake(t);
    }
}

/* ------------------------------------------------------------------ */
/* Option parsing                                                      */
/* ------------------------------------------------------------------ */

static bool parse_options_locked(struct tcp_tcb *t, const uint8_t *p,
                                 size_t header_len)
{
    bool ws_seen = false;
    size_t opt = 20;
    while (opt < header_len) {
        uint8_t kind = p[opt];
        if (kind == 0)
            break;
        if (kind == 1) {
            opt++;
            continue;
        }
        if (opt + 1 >= header_len)
            break;
        uint8_t opt_len = p[opt + 1];
        if (opt_len < 2 || opt + opt_len > header_len)
            break;
        if (kind == 2 && opt_len == 4) {
            uint16_t mss = g16(p + opt + 2);
            if (mss >= TCP_MIN_MSS && mss <= TCP_MAX_MSS)
                t->mss = mss;
        } else if (kind == 3 && opt_len == 3) {
            uint8_t shift = p[opt + 2];
            if (shift > 14)
                shift = 14;
            t->snd_wscale = shift;
            ws_seen = true;
        }
        opt += opt_len;
    }
    return ws_seen;
}

/* ------------------------------------------------------------------ */
/* Packet input                                                        */
/* ------------------------------------------------------------------ */

void tcp_input_ipv4(uint32_t s, uint32_t d, const uint8_t *p, size_t len)
{
    if (!p || len < 20) {
        stats.malformed++;
        return;
    }

    size_t header_len = (size_t)(p[12] >> 4) * 4;

    if (header_len < 20 || header_len > len) {
        stats.malformed++;
        return;
    }

    if (csum(s, d, p, len)) {
        stats.bad_checksum++;
        return;
    }

    uint16_t sp = g16(p);
    uint16_t dp = g16(p + 2);
    uint8_t flags = p[13];
    uint32_t seq = g32(p + 4);
    uint32_t ack = g32(p + 8);
    uint32_t raw_wnd = g16(p + 14);

    spinlock_acquire(&lock);

    struct tcp_tcb *t = find_locked(s, sp, dp);
    if (t) {
        t->refs++;
        stats.rx_segments++;
        if (flags & SYN) {
            bool ws = false;
            if (header_len > 20)
                ws = parse_options_locked(t, p, header_len);
            if (!ws)
                t->rcv_wscale = 0; /* peer cannot scale our window */
        }

        struct tcp_rx_actions act = {0};
        input_locked(t, seq, ack, flags, p + header_len, len - header_len,
                     raw_wnd, &act);
        spinlock_release(&lock);

        if (act.send_ack)
            emit(t, ACK, NULL, 0);
        if (act.retransmit)
            tcp_retransmit_segment(t, act.retx_seq);
        if (act.output)
            tcp_output(t);

        spinlock_acquire(&lock);
        tcb_put_locked(t);
        spinlock_release(&lock);
        return;
    }

    if (flags & SYN) {
        struct tcp_tcb *lis = find_listener_locked(dp, 4);
        if (lis) {
            if (!lis->accept_queue ||
                lis->syn_backlog >= lis->backlog ||
                active_count >= TCP_MAX_CONNECTIONS) {
                stats.connections_rejected++;
                spinlock_release(&lock);
                return;
            }

            struct tcp_tcb *child = alloc_locked();
            if (child) {
                child->address_family = 4;
                child->listener = lis;
                child->local_ip = d;
                child->remote_ip = s;
                child->local_port = dp;
                child->remote_port = sp;
                child->rcv_nxt = seq + 1;
                child->snd_una = new_iss();
                child->snd_nxt = child->snd_una + 1;
                child->tx_head = child->snd_nxt;
                child->state = TCP_SYN_RECEIVED;
                child->deadline =
                    lapic_timer_get_ticks() + child->rto_ms;
                child->last_activity = lapic_timer_get_ticks();
                bool ws = false;
                if (header_len > 20)
                    ws = parse_options_locked(child, p, header_len);
                if (!ws)
                    child->rcv_wscale = 0;
                hash_insert_locked(child);
                lis->syn_backlog++;
                child->syn_accounted = true;

                uint32_t tx_seq = child->snd_una;
                child->refs++;
                spinlock_release(&lock);

                emit_at(child, tx_seq, SYN | ACK, NULL, 0);

                spinlock_acquire(&lock);
                tcb_put_locked(child);
                spinlock_release(&lock);
                return;
            }
        }
    }

    spinlock_release(&lock);
}

void tcp_input_ipv6(const uint8_t s[16], const uint8_t d[16],
                    const uint8_t *p, size_t len)
{
    if (!p || len < 20) {
        stats.malformed++;
        return;
    }

    size_t header_len = (size_t)(p[12] >> 4) * 4;
    if (header_len < 20 || header_len > len) {
        stats.malformed++;
        return;
    }
    if (csum6(s, d, p, len)) {
        stats.bad_checksum++;
        return;
    }

    uint16_t sp = g16(p);
    uint16_t dp = g16(p + 2);
    uint8_t flags = p[13];
    uint32_t seq = g32(p + 4);
    uint32_t ack = g32(p + 8);
    uint32_t raw_wnd = g16(p + 14);

    spinlock_acquire(&lock);

    struct tcp_tcb *t = find6_locked(s, sp, dp);
    if (t) {
        t->refs++;
        stats.rx_segments++;
        if (flags & SYN) {
            bool ws = false;
            if (header_len > 20)
                ws = parse_options_locked(t, p, header_len);
            if (!ws)
                t->rcv_wscale = 0; /* peer cannot scale our window */
        }

        struct tcp_rx_actions act = {0};
        input_locked(t, seq, ack, flags, p + header_len, len - header_len,
                     raw_wnd, &act);
        spinlock_release(&lock);

        if (act.send_ack)
            emit(t, ACK, NULL, 0);
        if (act.retransmit)
            tcp_retransmit_segment(t, act.retx_seq);
        if (act.output)
            tcp_output(t);

        spinlock_acquire(&lock);
        tcb_put_locked(t);
        spinlock_release(&lock);
        return;
    }

    if (flags & SYN) {
        struct tcp_tcb *lis = find_listener_locked(dp, 6);
        if (lis) {
            if (!lis->accept_queue ||
                lis->syn_backlog >= lis->backlog ||
                active_count >= TCP_MAX_CONNECTIONS) {
                stats.connections_rejected++;
                spinlock_release(&lock);
                return;
            }

            struct tcp_tcb *child = alloc_locked();
            if (child) {
                child->address_family = 6;
                child->listener = lis;
                memcpy(child->local_ip6, d, 16);
                memcpy(child->remote_ip6, s, 16);
                child->local_port = dp;
                child->remote_port = sp;
                child->rcv_nxt = seq + 1;
                child->snd_una = new_iss();
                child->snd_nxt = child->snd_una + 1;
                child->tx_head = child->snd_nxt;
                child->state = TCP_SYN_RECEIVED;
                child->deadline =
                    lapic_timer_get_ticks() + child->rto_ms;
                child->last_activity = lapic_timer_get_ticks();
                bool ws = false;
                if (header_len > 20)
                    ws = parse_options_locked(child, p, header_len);
                if (!ws)
                    child->rcv_wscale = 0;
                hash_insert_locked(child);
                lis->syn_backlog++;
                child->syn_accounted = true;

                uint32_t tx_seq = child->snd_una;
                child->refs++;
                spinlock_release(&lock);

                emit_at(child, tx_seq, SYN | ACK, NULL, 0);

                spinlock_acquire(&lock);
                tcb_put_locked(child);
                spinlock_release(&lock);
                return;
            }
        }
    }

    spinlock_release(&lock);
}

/* ------------------------------------------------------------------ */
/* Timer                                                               */
/* ------------------------------------------------------------------ */

void tcp_timer_tick(uint64_t now)
{
    struct tcp_work work[TCP_TIMER_WORK_MAX];
    int nw = 0;

    spinlock_acquire(&lock);

    struct tcp_tcb *t = active_list;
    while (t) {
        struct tcp_tcb *next = t->list_next;
        if (!t->used) {
            t = next;
            continue;
        }

        if (t->state == TCP_TIME_WAIT && t->deadline && now >= t->deadline) {
            t->state = TCP_CLOSED;
            t->deadline = 0;
            t->reap = true;
        }

        if (t->state == TCP_FIN_WAIT_2 && t->deadline && now >= t->deadline) {
            t->state = TCP_CLOSED;
            t->deadline = 0;
            t->reap = true;
        }

        if ((t->reap || t->state == TCP_CLOSED || t->state == TCP_RESET) &&
            !t->socket_ref && t->refs == 0) {
            free_tcb_locked(t);
            t = next;
            continue;
        }

        if ((t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT) &&
            t->unacked_packets > 0) {
            t->unacked_packets = 0;
            if (nw < TCP_TIMER_WORK_MAX) {
                work[nw].t = t;
                work[nw].seq = t->snd_nxt;
                work[nw].kind = WORK_ACK;
                t->refs++;
                nw++;
            }
        }

        if (t->keepalive &&
            (t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT) &&
            !t->deadline) {
            if (!t->keepalive_probing) {
                if (now - t->last_activity >= t->keepidle_ms) {
                    t->keepalive_probing = true;
                    t->keepalive_probes = 1;
                    t->keepalive_deadline = now + t->keepintvl_ms;
                    if (nw < TCP_TIMER_WORK_MAX) {
                        work[nw].t = t;
                        work[nw].seq = 0;
                        work[nw].kind = WORK_PROBE;
                        t->refs++;
                        nw++;
                    }
                }
            } else if (now >= t->keepalive_deadline) {
                if (t->keepalive_probes >= t->keepcnt) {
                    t->error = 110;
                    t->state = TCP_CLOSED;
                    t->deadline = 0;
                    t->reap = true;
                    stats.timeouts++;
                    wake(t);
                } else {
                    t->keepalive_probes++;
                    t->keepalive_deadline = now + t->keepintvl_ms;
                    if (nw < TCP_TIMER_WORK_MAX) {
                        work[nw].t = t;
                        work[nw].seq = 0;
                        work[nw].kind = WORK_PROBE;
                        t->refs++;
                        nw++;
                    }
                }
            }
        }

        if (t->persist_deadline && now >= t->persist_deadline &&
            t->state == TCP_ESTABLISHED && seq_lt(t->snd_nxt, t->tx_head)) {
            if (nw < TCP_TIMER_WORK_MAX) {
                work[nw].t = t;
                work[nw].seq = 0;
                work[nw].kind = WORK_OUTPUT;
                t->refs++;
                nw++;
            }
        }

        if ((t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT) &&
            (seq_lt(t->snd_nxt, t->tx_head) || t->fin_pending)) {
            if (nw < TCP_TIMER_WORK_MAX) {
                work[nw].t = t;
                work[nw].seq = 0;
                work[nw].kind = WORK_OUTPUT;
                t->refs++;
                nw++;
            }
        }

        if (!t->deadline || now < t->deadline) {
            t = next;
            continue;
        }

        if (t->state == TCP_SYN_SENT || t->state == TCP_SYN_RECEIVED) {
            if (++t->retries > RETRIES) {
                t->state = TCP_CLOSED;
                t->deadline = 0;
                t->error = 110;
                t->reap = true;
                stats.timeouts++;
                wake(t);
            } else {
                t->deadline = now + ((uint64_t)t->rto_ms
                                     << (t->retries > 4 ? 4 : t->retries));
                stats.retransmits++;
                t->retransmits++;
                if (nw < TCP_TIMER_WORK_MAX) {
                    work[nw].t = t;
                    work[nw].seq = 0;
                    work[nw].kind = WORK_RETX_SYN;
                    t->refs++;
                    nw++;
                }
            }
            t = next;
            continue;
        }

        if (t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT ||
            t->state == TCP_FIN_WAIT_1 || t->state == TCP_LAST_ACK) {
            if (t->snd_una == t->snd_nxt) {
                t->deadline = 0;
            } else if (++t->retries > RETRIES) {
                t->state = TCP_CLOSED;
                t->deadline = 0;
                t->error = 110;
                t->reap = true;
                stats.timeouts++;
                wake(t);
            } else {
                uint64_t backoff =
                    (uint64_t)t->rto_ms << (t->retries > 4 ? 4 : t->retries);
                t->deadline = now + backoff;
                stats.retransmits++;
                t->retransmits++;
                t->rtt_pending = false;
                t->retransmit = true;
                t->ssthresh = (t->snd_nxt - t->snd_una) / 2;
                if (t->ssthresh < 2 * t->mss)
                    t->ssthresh = 2 * t->mss;
                t->cwnd = t->mss;
                t->in_recovery = false;
                t->dup_ack_count = 0;
                if (nw < TCP_TIMER_WORK_MAX) {
                    work[nw].t = t;
                    if (t->fin_sent && t->snd_nxt - t->snd_una == 1) {
                        work[nw].kind = WORK_RETX_FIN;
                        work[nw].seq = 0;
                    } else {
                        work[nw].kind = WORK_RETX;
                        work[nw].seq = t->snd_una;
                    }
                    t->refs++;
                    nw++;
                }
            }
        }

        t = next;
    }

    for (t = active_list; t; ) {
        struct tcp_tcb *next = t->list_next;
        if (t->used && t->reap && !t->socket_ref && t->refs == 0)
            free_tcb_locked(t);
        t = next;
    }

    spinlock_release(&lock);

    for (int i = 0; i < nw; i++) {
        struct tcp_tcb *wt = work[i].t;
        if (!wt)
            continue;
        switch (work[i].kind) {
        case WORK_ACK:
            emit_at(wt, work[i].seq, ACK, NULL, 0);
            break;
        case WORK_RETX:
            tcp_retransmit_segment(wt, work[i].seq);
            break;
        case WORK_RETX_FIN:
            tcp_retransmit_fin(wt);
            break;
        case WORK_RETX_SYN:
            tcp_retransmit_syn(wt);
            break;
        case WORK_PROBE:
            tcp_send_probe(wt);
            break;
        case WORK_OUTPUT:
            tcp_output(wt);
            break;
        }
    }

    if (nw) {
        spinlock_acquire(&lock);
        for (int i = 0; i < nw; i++) {
            struct tcp_tcb *wt = work[i].t;
            if (wt && wt->used)
                tcb_put_locked(wt);
            else if (wt && wt->refs > 0)
                wt->refs--;
        }
        spinlock_release(&lock);
    }
}

/* ------------------------------------------------------------------ */
/* Socket option plumbing                                              */
/* ------------------------------------------------------------------ */

void tcp_set_rcvbuf(struct tcp_tcb *t, size_t bytes)
{
    if (!t)
        return;

    if (bytes < TCP_MIN_RCVBUF)
        bytes = TCP_MIN_RCVBUF;
    if (bytes > TCP_MAX_RCVBUF)
        bytes = TCP_MAX_RCVBUF;

    uint8_t *nb = kmalloc(bytes);
    if (!nb)
        return;

    spinlock_acquire(&lock);
    if (!t->used || t->rx_capacity == bytes) {
        spinlock_release(&lock);
        kfree(nb);
        return;
    }

    size_t used = t->rx_head - t->rx_tail;
    if (bytes < used) {
        spinlock_release(&lock);
        kfree(nb);
        return;
    }

    for (size_t i = 0; i < used; i++)
        nb[i] = t->rx_buffer[(t->rx_tail + i) % t->rx_capacity];

    uint8_t *old = t->rx_buffer;
    t->rx_buffer = nb;
    t->rx_capacity = bytes;
    t->rx_tail = 0;
    t->rx_head = used;
    t->rcv_wnd = bytes > 65535 ? 65535 : (uint16_t)bytes;
    spinlock_release(&lock);

    kfree(old);
}

void tcp_set_sndbuf(struct tcp_tcb *t, size_t bytes)
{
    if (!t)
        return;

    if (bytes < TCP_MIN_SNDBUF)
        bytes = TCP_MIN_SNDBUF;
    if (bytes > TCP_MAX_SNDBUF)
        bytes = TCP_MAX_SNDBUF;

    uint8_t *nb = kmalloc(bytes);
    if (!nb)
        return;

    spinlock_acquire(&lock);
    if (!t->used || t->tx_capacity == bytes) {
        spinlock_release(&lock);
        kfree(nb);
        return;
    }

    size_t used = t->tx_head - t->snd_una;
    if (bytes < used) {
        spinlock_release(&lock);
        kfree(nb);
        return;
    }

    for (size_t i = 0; i < used; i++) {
        uint32_t seq = t->snd_una + (uint32_t)i;
        nb[seq % bytes] = t->tx_buffer[seq % t->tx_capacity];
    }

    uint8_t *old = t->tx_buffer;
    t->tx_buffer = nb;
    t->tx_capacity = bytes;
    t->sndbuf = (uint32_t)bytes;
    spinlock_release(&lock);

    kfree(old);
}

void tcp_set_nodelay(struct tcp_tcb *t, bool enabled)
{
    if (!t)
        return;
    spinlock_acquire(&lock);
    if (t->used)
        t->nodelay = enabled;
    spinlock_release(&lock);
}

void tcp_set_keepalive(struct tcp_tcb *t, bool enabled, int idle_s,
                       int intvl_s, int cnt)
{
    if (!t)
        return;
    spinlock_acquire(&lock);
    if (t->used) {
        t->keepalive = enabled;
        if (idle_s > 0)
            t->keepidle_ms = (uint32_t)idle_s * 1000u;
        if (intvl_s > 0)
            t->keepintvl_ms = (uint32_t)intvl_s * 1000u;
        if (cnt > 0)
            t->keepcnt = cnt > 127 ? 127 : (uint8_t)cnt;
        if (!enabled) {
            t->keepalive_probing = false;
            t->keepalive_probes = 0;
        }
    }
    spinlock_release(&lock);
}

void tcp_set_reuseaddr(struct tcp_tcb *t, bool enabled)
{
    if (!t)
        return;
    spinlock_acquire(&lock);
    if (t->used)
        t->reuseaddr = enabled;
    spinlock_release(&lock);
}

void tcp_set_linger(struct tcp_tcb *t, int seconds)
{
    if (!t)
        return;
    spinlock_acquire(&lock);
    if (t->used)
        t->linger_seconds = seconds;
    spinlock_release(&lock);
}

void tcp_set_mss(struct tcp_tcb *t, uint16_t mss)
{
    if (!t)
        return;
    if (mss < TCP_MIN_MSS)
        mss = TCP_MIN_MSS;
    if (mss > TCP_MAX_MSS)
        mss = TCP_MAX_MSS;
    spinlock_acquire(&lock);
    if (t->used)
        t->mss = mss;
    spinlock_release(&lock);
}

/* ------------------------------------------------------------------ */
/* Introspection                                                       */
/* ------------------------------------------------------------------ */

bool tcp_readable(const struct tcp_tcb *t)
{
    return t && (
        t->rx_head != t->rx_tail ||
        t->peer_closed ||
        t->state == TCP_CLOSE_WAIT ||
        t->state == TCP_TIME_WAIT ||
        t->error
    );
}

bool tcp_writable(const struct tcp_tcb *t)
{
    if (!t || t->state != TCP_ESTABLISHED || t->error || !t->tx_buffer)
        return false;
    return (t->tx_head - t->snd_una) < t->tx_capacity;
}

bool tcp_accept_pending(const struct tcp_tcb *t)
{
    return t && t->state == TCP_LISTEN && t->accept_head != t->accept_tail;
}

int tcp_get_snapshot(struct tcp_entry_snapshot *out, int max)
{
    int n = 0;
    if (!out || max <= 0)
        return 0;

    spinlock_acquire(&lock);
    for (struct tcp_tcb *t = active_list; t && n < max; t = t->list_next) {
        if (!t->used)
            continue;
        if (t->state == TCP_CLOSED && t->local_port == 0)
            continue;
        out[n].local_ip = t->local_ip;
        out[n].remote_ip = t->remote_ip;
        out[n].local_port = t->local_port;
        out[n].remote_port = t->remote_port;
        out[n].state = (uint8_t)t->state;
        out[n].address_family = t->address_family;
        out[n]._pad = 0;
        out[n].bytes_sent = t->bytes_sent;
        out[n].bytes_recv = t->bytes_recv;
        out[n].retransmits = t->retransmits;
        out[n].rx_queued = (uint32_t)(t->rx_head - t->rx_tail);
        out[n].cwnd = t->cwnd;
        out[n].srtt_ms = t->srtt_ms;
        n++;
    }
    spinlock_release(&lock);
    return n;
}

int tcp_active_count(void)
{
    spinlock_acquire(&lock);
    int count = active_count;
    spinlock_release(&lock);
    return count;
}

const struct tcp_stats *tcp_get_stats(void)
{
    return &stats;
}

void tcp_init(void)
{
    spinlock_init(&lock);
    memset(hash, 0, sizeof(hash));
    memset(&stats, 0, sizeof(stats));
    memset(port_bitmap, 0, sizeof(port_bitmap));
    active_list = NULL;
    active_count = 0;
    next_ephemeral = TCP_PORT_MIN;
    iss_counter = 0x90000000u;
}

bool net_phase8_init(void)
{
    tcp_init();
    net_core_start_timer();
    klog_puts("[NET] TCP initialized (sliding window + congestion control)\n");
    return true;
}
