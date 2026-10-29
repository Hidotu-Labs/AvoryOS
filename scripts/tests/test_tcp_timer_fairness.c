/* Exercise the real TCP timer with more connections than its work budget.
 * Build with function/data sections and --gc-sections to drop unused paths. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LOCK_SPINLOCK_H
#undef strchr
#undef strrchr
#undef strstr
typedef struct { int locked; } spinlock_t;
#define SPINLOCK_INIT {0}
static void spinlock_init(spinlock_t *s) { s->locked = 0; }
static void spinlock_acquire(spinlock_t *s) { assert(!s->locked); s->locked = 1; }
static void spinlock_release(spinlock_t *s) { assert(s->locked); s->locked = 0; }
#ifdef TEST_TCP_SOURCE
#include TEST_TCP_SOURCE
#else
#include "../../kernel/src/net/tcp.c"
#endif
static uint64_t clock_ms;
static unsigned emissions[80];
uint64_t lapic_timer_get_ticks(void) { return clock_ms; }
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
void wait_queue_wake_all(wait_queue_t *q) { (void)q; }
void epoll_notify_event(vfs_node_t *node, uint32_t events) { (void)node; (void)events; }
int ipv4_send_raw(uint32_t dst, uint8_t proto, const void *data, size_t len) {
    (void)proto; (void)data; (void)len;
    assert(dst < 80); emissions[dst]++; return 0;
}
int ipv6_send_raw(const uint8_t dst[16], uint8_t proto, const void *data, size_t len) {
    (void)dst; (void)proto; (void)data; (void)len; return 0;
}
static struct tcp_tcb connections[72];
static void setup(void) {
    memset(connections, 0, sizeof(connections));
    memset(emissions, 0, sizeof(emissions));
    memset(&stats, 0, sizeof(stats));
    active_list = connections; active_count = 72;
    timer_tree = (struct rb_root){0}; clock_ms = 0;
    for (unsigned i = 0; i < 72; i++) {
        struct tcp_tcb *t = &connections[i];
        t->used = t->socket_ref = true; t->state = TCP_ESTABLISHED;
        t->remote_ip = i; t->address_family = 4;
        t->rx_capacity = 65536; t->mss = 1460; t->rto_ms = 1000;
        t->list_prev = i ? &connections[i-1] : NULL;
        t->list_next = i+1 < 72 ? &connections[i+1] : NULL;
    }
}
int main(void) {
    setup();
    /* More expired ACK deadlines than one timer pass can process. */
    for (unsigned i = 0; i < 72; i++) {
        connections[i].unacked_packets = 1;
        connections[i].ack_deadline = 1;
        tcp_timer_refresh_locked(&connections[i]);
    }
    for (unsigned tick = 0; tick < 3; tick++) {
        clock_ms += 100; tcp_timer_tick(clock_ms);
    }
    for (unsigned i = 0; i < 72; i++) assert(emissions[i] == 1);
    puts("PASS: full delayed-ACK work list rotates across 72 connections");
    setup();
    for (unsigned i = 24; i < 72; i++) {
        connections[i].state = TCP_SYN_SENT; connections[i].deadline = 1;
        tcp_timer_refresh_locked(&connections[i]);
    }
    for (unsigned tick = 0; tick < 3; tick++) {
        for (unsigned i = 0; i < 24; i++) {
            connections[i].unacked_packets = 1;
            connections[i].ack_deadline = clock_ms + 1;
            tcp_timer_refresh_locked(&connections[i]);
        }
        clock_ms += 100; tcp_timer_tick(clock_ms);
    }
    for (unsigned i = 24; i < 72; i++) assert(emissions[i] == 1);
    puts("PASS: busy established connections do not starve SYN retries");
    setup();
    tcp_timer_tick(clock_ms);
    for (unsigned i = 0; i < 72; i++) assert(emissions[i] == 0 && connections[i].refs == 0);
    puts("PASS: idle timer scan terminates without emitting work");
    return 0;
}
