#ifndef NET_TCP_H
#define NET_TCP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Phase 1: connection scaling.
 *
 * TCP control blocks and their receive buffers are heap allocated now, so the
 * old static 64-entry table (32 MB of BSS) is gone.  TCP_MAX_CONNECTIONS is a
 * safety cap only; it can be raised without touching the data path.
 */
#define TCP_MAX_CONNECTIONS 2048
#define TCP_MAX_TCBS TCP_MAX_CONNECTIONS /* legacy alias */

#define TCP_DEFAULT_RCVBUF (64 * 1024)
#define TCP_MIN_RCVBUF     (4 * 1024)
#define TCP_MAX_RCVBUF     (4 * 1024 * 1024)

#define TCP_DEFAULT_SNDBUF (64 * 1024)
#define TCP_MIN_SNDBUF     (4 * 1024)
#define TCP_MAX_SNDBUF     (4 * 1024 * 1024)

#define TCP_DEFAULT_ACCEPT_BACKLOG 128
#define TCP_MAX_ACCEPT_BACKLOG     1024

#define TCP_DEFAULT_WINDOW 65535
#define TCP_MAX_MSS 1460
#define TCP_MIN_MSS 536

/* Phase 2: congestion control / RTT / reassembly tuning. */
#define TCP_INITIAL_CWND_MSS 10
#define TCP_MIN_RTO_MS       200
#define TCP_MAX_RTO_MS       60000
#define TCP_DEFAULT_RTO_MS   1000
#define TCP_WS_SHIFT         7
#define TCP_MAX_OOO_BYTES    (64 * 1024)
#define TCP_MAX_OOO_SEGS     32
#define TCP_CWND_MAX_BYTES   (4 * 1024 * 1024)

/* Keepalive defaults (seconds, matching Linux). */
#define TCP_KEEPALIVE_IDLE_DEFAULT  7200
#define TCP_KEEPALIVE_INTVL_DEFAULT 75
#define TCP_KEEPALIVE_CNT_DEFAULT   9

#define TCP_FIN_WAIT_2_TIMEOUT_MS 60000
#define TCP_TIME_WAIT_TIMEOUT_MS  2000

enum tcp_state { TCP_CLOSED, TCP_LISTEN, TCP_SYN_SENT, TCP_SYN_RECEIVED, TCP_ESTABLISHED, TCP_FIN_WAIT_1,
  TCP_FIN_WAIT_2, TCP_CLOSE_WAIT, TCP_LAST_ACK, TCP_TIME_WAIT, TCP_RESET };

struct tcp_tcb {
  bool used;
  enum tcp_state state;
  uint8_t address_family;       /* 4 or 6; 0 until bound/connected */

  uint32_t local_ip, remote_ip;
  uint16_t local_port, remote_port;
  uint8_t local_ip6[16], remote_ip6[16];

  uint32_t snd_una, snd_nxt, rcv_nxt;
  uint16_t rcv_wnd, mss;
  uint8_t retries;
  uint64_t deadline;

  /* Receive buffer: heap allocated, resizable through SO_RCVBUF. */
  uint8_t *rx_buffer;
  size_t rx_capacity;
  size_t rx_head, rx_tail;

  /* Transmit ring: bytes [snd_una, tx_head) are queued; [snd_nxt, tx_head)
   * is unsent.  Indexed by absolute sequence number modulo tx_capacity. */
  uint8_t *tx_buffer;
  size_t tx_capacity;
  uint32_t tx_head;
  bool fin_pending;
  bool fin_sent;

  /* Congestion control / window scaling (Phase 2). */
  uint32_t cwnd, ssthresh, recover;
  uint32_t snd_wnd;              /* peer window, already scaled */
  uint32_t snd_wl1, snd_wl2;     /* last segment seq/ack that updated window */
  uint8_t snd_wscale, rcv_wscale;
  uint8_t dup_ack_count;
  bool in_recovery;
  bool retransmit;               /* Karn: suppress RTT samples */
  uint32_t srtt_ms, rttvar_ms, rto_ms;
  uint32_t rtt_seq;
  uint64_t rtt_start;
  bool rtt_pending;
  uint64_t persist_deadline;

  /* Out-of-order receive reassembly (Phase 2), lazily allocated. */
  struct tcp_ooo_seg *ooo_head;
  size_t ooo_bytes;
  size_t ooo_count;

  bool peer_closed;
  int error;
  uint8_t unacked_packets;       /* in-order segments since last ACK */
  uint8_t ooo_ack_count;         /* duplicate ACKs sent for the current gap */

  void *wait_queue;
  void *vfs_node;

  struct tcp_tcb *listener;
  struct tcp_tcb **accept_queue;
  size_t accept_head, accept_tail, accept_cap;
  int backlog;
  int syn_backlog;
  bool syn_accounted; /* this child is counted in listener->syn_backlog */

  bool port_owned;    /* owns the port bitmap bit for local_port */
  bool reuseaddr;     /* SO_REUSEADDR: allow TIME_WAIT reuse */
  bool socket_ref;    /* a struct socket still owns this TCB */
  bool reap;          /* connection finished, free when refs drop */
  int refs;           /* transient references (timer, in-flight emit) */

  bool keepalive;
  bool keepalive_probing;
  uint8_t keepcnt;
  uint8_t keepalive_probes;
  uint32_t keepidle_ms, keepintvl_ms;
  uint64_t keepalive_deadline;
  uint64_t last_activity;

  bool nodelay;
  int linger_seconds; /* < 0 disabled, 0 = abortive close (RST) */
  uint32_t sndbuf;

  struct tcp_tcb *hash_next;
  struct tcp_tcb *list_prev, *list_next;

  uint64_t bytes_sent, bytes_recv;
  uint64_t retransmits, dup_acks;
  uint64_t dropped_rx, dropped_tx;
};

struct tcp_stats {
  uint64_t rx_segments, bad_checksum, malformed, resets, retransmits;
  uint64_t duplicates, out_of_order, timeouts;
  uint64_t connections_opened, connections_rejected, accept_overflow;
};

/* Snapshot of one TCP connection for /proc/net/tcp */
struct tcp_entry_snapshot {
  uint32_t local_ip, remote_ip;
  uint16_t local_port, remote_port;
  uint8_t state;   /* enum tcp_state value */
  uint8_t address_family;
  uint16_t _pad;
  uint64_t bytes_sent, bytes_recv;
  uint64_t retransmits;
  uint32_t rx_queued;
  uint32_t cwnd;
  uint32_t srtt_ms;
};

/* Fill up to 'max' entries; returns number filled. */
int tcp_get_snapshot(struct tcp_entry_snapshot *out, int max);
int tcp_active_count(void);

void tcp_init(void);
void tcp_input_ipv4(uint32_t src, uint32_t dst, const uint8_t *, size_t);
void tcp_input_ipv6(const uint8_t src[16], const uint8_t dst[16],
                    const uint8_t *, size_t);
void tcp_timer_tick(uint64_t now);
const struct tcp_stats *tcp_get_stats(void);

struct tcp_tcb *tcp_alloc(void);
void tcp_free(struct tcp_tcb *);
void tcp_put(struct tcp_tcb *);
void tcp_abort(struct tcp_tcb *);
void tcp_attach_socket(struct tcp_tcb *, void *wait_queue, void *vfs_node);

int tcp_active_open(struct tcp_tcb *, uint32_t, uint16_t);
int tcp_active_open6(struct tcp_tcb *, const uint8_t[16], uint16_t);
int tcp_bind(struct tcp_tcb *, uint32_t, uint16_t);
int tcp_bind6(struct tcp_tcb *, const uint8_t[16], uint16_t);
int tcp_send(struct tcp_tcb *, const void *, size_t, bool nonblock);
int tcp_recv(struct tcp_tcb *, void *, size_t, bool);
int tcp_close(struct tcp_tcb *);
int tcp_listen(struct tcp_tcb *, int);
struct tcp_tcb *tcp_accept(struct tcp_tcb *, bool);
bool tcp_readable(const struct tcp_tcb *);
bool tcp_writable(const struct tcp_tcb *);
bool tcp_accept_pending(const struct tcp_tcb *);

/* Socket option plumbing (Phase 1). */
void tcp_set_rcvbuf(struct tcp_tcb *, size_t);
void tcp_set_sndbuf(struct tcp_tcb *, size_t);
void tcp_set_nodelay(struct tcp_tcb *, bool);
void tcp_set_keepalive(struct tcp_tcb *, bool enabled, int idle_s,
                       int intvl_s, int cnt);
void tcp_set_reuseaddr(struct tcp_tcb *, bool);
void tcp_set_linger(struct tcp_tcb *, int seconds);
void tcp_set_mss(struct tcp_tcb *, uint16_t);

bool net_phase8_init(void);
#endif
