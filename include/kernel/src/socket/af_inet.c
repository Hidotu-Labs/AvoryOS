#include "socket/af_inet.h"
#include "console/klog.h"
#include "apic/lapic_timer.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "net/ipv4.h"
#include "net/raw_icmp.h"
#include "net/tcp.h"
#include "net/udp.h"
#include "sched/sched.h"
#include "sched/wait.h"
#include "socket/epoll.h"
#include "socket/socket.h"
#include "socket/socket_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct inet_sock {
    socket_t        *parent;
    struct udp_socket *udp;
    struct raw_icmp_socket *raw;
    struct tcp_tcb *tcp;
    /* TCP socket options */
    int              rcvtimeo_ms;   /* SO_RCVTIMEO  */
    int              sndtimeo_ms;   /* SO_SNDTIMEO  */
    bool             tcp_nodelay;   /* TCP_NODELAY  */
    bool             tcp_keepalive; /* SO_KEEPALIVE */
    int              tcp_keepidle_s;  /* TCP_KEEPIDLE  (seconds) */
    int              tcp_keepintvl_s; /* TCP_KEEPINTVL (seconds) */
    int              tcp_keepcnt;     /* TCP_KEEPCNT */
    int              linger_seconds;  /* SO_LINGER, -1 = disabled */
    bool             shut_rd;
    bool             shut_wr;
} inet_sock_t;

static inline uint32_t sa_addr(const struct sockaddr_in *a) {
    const uint8_t *b = (const uint8_t *)&a->sin_addr;
    return ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|
           ((uint32_t)b[2]<<8)|(uint32_t)b[3];
}
static inline uint16_t sa_port(const struct sockaddr_in *a) {
    const uint8_t *b = (const uint8_t *)&a->sin_port;
    return (uint16_t)(((uint16_t)b[0]<<8)|b[1]);
}
static void fill_sin(struct sockaddr_in *out, uint32_t ip, uint16_t port) {
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    uint8_t *pb = (uint8_t *)&out->sin_port;
    pb[0]=(uint8_t)(port>>8); pb[1]=(uint8_t)port;
    uint8_t *ab = (uint8_t *)&out->sin_addr;
    ab[0]=(uint8_t)(ip>>24); ab[1]=(uint8_t)(ip>>16);
    ab[2]=(uint8_t)(ip>>8);  ab[3]=(uint8_t)ip;
}

static int inet_create(socket_t *sock, int protocol) {
    if (sock->type != SOCK_DGRAM && sock->type != SOCK_RAW && sock->type != SOCK_STREAM) return -93;
    if (sock->type == SOCK_RAW && protocol != 0 && protocol != IPPROTO_ICMP) return -93;
    inet_sock_t *isk = kmalloc(sizeof(inet_sock_t));
    if (!isk) return -12;
    memset(isk, 0, sizeof(*isk));
    isk->parent = sock;
    if (sock->type == SOCK_RAW) {
        isk->raw = raw_icmp_alloc();
        if (!isk->raw) { kfree(isk); return -12; }
        isk->raw->wait = sock->wait_queue;
    } else if (sock->type == SOCK_STREAM) {
        isk->tcp = tcp_alloc();
        if (!isk->tcp) { kfree(isk); return -12; }
        isk->tcp_keepidle_s = TCP_KEEPALIVE_IDLE_DEFAULT;
        isk->tcp_keepintvl_s = TCP_KEEPALIVE_INTVL_DEFAULT;
        isk->tcp_keepcnt = TCP_KEEPALIVE_CNT_DEFAULT;
        isk->linger_seconds = -1;
        tcp_attach_socket(isk->tcp, sock->wait_queue, sock->node);
    } else {
        isk->udp = udp_socket_alloc();
        if (!isk->udp) { kfree(isk); return -12; }
        isk->udp->wait_queue = sock->wait_queue;
    }
    sock->sk = isk;
    return 0;
}

static void inet_destroy(socket_t *sock) {
    inet_sock_t *isk = sock->sk;
    if (!isk) return;
    if (isk->udp) {
        isk->udp->vfs_node  = NULL;
        isk->udp->wait_queue = NULL;
        udp_socket_free(isk->udp);
        isk->udp = NULL;
    }
    if (isk->raw) raw_icmp_free(isk->raw);
    if (isk->tcp) { tcp_close(isk->tcp); tcp_put(isk->tcp); isk->tcp = NULL; }
    kfree(isk);
    sock->sk = NULL;
}

static inline void inet_sync_node(socket_t *sock, inet_sock_t *isk) {
    if (sock && sock->node && isk) {
        if (isk->udp) isk->udp->vfs_node = sock->node;
        if (isk->tcp) isk->tcp->vfs_node = sock->node;
    }
}

static int inet_bind(socket_t *sock, struct sockaddr *addr, int addrlen) {
    if (addrlen < (int)sizeof(struct sockaddr_in)) return -22;
    if (addr->sa_family != AF_INET) return -97;
    inet_sock_t *isk = sock->sk;
    if (!isk) return -22;
    inet_sync_node(sock, isk);
    struct sockaddr_in *sin = (struct sockaddr_in *)addr;
    if (isk->raw) { isk->raw->local_ip = sa_addr(sin); return 0; }
    if (isk->tcp) return tcp_bind(isk->tcp, sa_addr(sin), sa_port(sin));
    if (!isk->udp) return -22;
    int r = udp_bind(isk->udp, sa_addr(sin), sa_port(sin));
    if (r == 0) sock->state = SS_UNCONNECTED;
    return r;
}

static int inet_connect(socket_t *sock, struct sockaddr *addr, int addrlen) {
    inet_sock_t *isk = sock->sk;
    if (!isk) return -22;
    inet_sync_node(sock, isk);
    if (addr && addr->sa_family == 0 /* AF_UNSPEC */ && sock->type == SOCK_DGRAM) {
        if (isk->udp) udp_disconnect(isk->udp);
        sock->state = SS_UNCONNECTED;
        return 0;
    }
    if (addrlen < (int)sizeof(struct sockaddr_in)) return -22;
    if (addr->sa_family != AF_INET) return -97;
    struct sockaddr_in *sin = (struct sockaddr_in *)addr;
    if (isk->raw) { isk->raw->remote_ip=sa_addr(sin); isk->raw->connected=true; sock->state=SS_CONNECTED; return 0; }
    if (isk->tcp) {
        if (sock->state == SS_CONNECTED || isk->tcp->state == TCP_ESTABLISHED)
            return -106; /* EISCONN */
        if (sock->state == SS_CONNECTING || isk->tcp->state == TCP_SYN_SENT || isk->tcp->state == TCP_SYN_RECEIVED)
            return -114; /* EALREADY */
        isk->tcp->vfs_node = sock->node;
        isk->tcp->wait_queue = sock->wait_queue;
        tcp_set_nodelay(isk->tcp, isk->tcp_nodelay);
        tcp_set_reuseaddr(isk->tcp, sock->reuseaddr != 0);
        tcp_set_keepalive(isk->tcp, isk->tcp_keepalive,
                          isk->tcp_keepidle_s, isk->tcp_keepintvl_s,
                          isk->tcp_keepcnt);
        if (isk->linger_seconds >= 0)
            tcp_set_linger(isk->tcp, isk->linger_seconds);
        int r = tcp_active_open(isk->tcp, sa_addr(sin), sa_port(sin));
        if (r < 0) return r;
        if (socket_is_nonblocking(sock)) {
            sock->state = SS_CONNECTING;
            return -115; /* EINPROGRESS */
        }
        /* Block on wait queue until established, error, or timeout */
        uint64_t deadline = lapic_timer_get_ticks() + 3000;
        struct thread *self = sched_get_current();
        wait_queue_entry_t wqe = { .thread = self, .next = NULL };
        while (isk->tcp->state == TCP_SYN_SENT && !isk->tcp->error) {
            if (lapic_timer_get_ticks() >= deadline) break;
            wait_queue_add(sock->wait_queue, &wqe);
            if (self) {
                self->wakeup_ticks = lapic_timer_get_ticks() + 50;
                self->state = THREAD_BLOCKED;
            }
            sched_yield();
            if (self) self->wakeup_ticks = 0;
            wait_queue_remove(sock->wait_queue, &wqe);
            if (self) self->state = THREAD_RUNNING;
        }
        if (isk->tcp->state == TCP_ESTABLISHED) { sock->state = SS_CONNECTED; return 0; }
        return isk->tcp->error ? -isk->tcp->error : -110; /* ETIMEDOUT */
    }
    if (!isk->udp) return -22;
    int r = udp_connect(isk->udp, sa_addr(sin), sa_port(sin));
    if (r == 0) sock->state = SS_CONNECTED;
    return r;
}

static int inet_listen(socket_t *sock, int backlog) {
    inet_sock_t *isk = sock->sk;
    if (!isk || !isk->tcp) return -95;
    isk->tcp->wait_queue = sock->wait_queue;
    isk->tcp->vfs_node = sock->node;
    int r = tcp_listen(isk->tcp, backlog);
    if (!r) sock->state = SS_LISTENING;
    return r;
}

static int inet_accept(socket_t *sock, socket_t **out) {
    inet_sock_t *isk = sock->sk;
    if (!isk || !isk->tcp || !out) return -22;
    bool nb = socket_is_nonblocking(sock);
    struct tcp_tcb *child = tcp_accept(isk->tcp, nb);
    if (!child) return nb ? -11 : -4;
    socket_t *ns = socket_create(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!ns) { tcp_abort(child); return -12; }
    inet_sock_t *nisk = ns->sk;
    if (nisk->tcp) tcp_free(nisk->tcp);
    nisk->tcp = child;
    tcp_attach_socket(child, ns->wait_queue, ns->node);
    ns->state = SS_CONNECTED;
    *out = ns;
    return 0;
}

static ssize_t inet_sendto(socket_t *sock, const void *buf, size_t len,
                            int flags, struct sockaddr *dest, int addrlen) {
    inet_sock_t *isk = sock->sk;
    if (!isk) return -22;
    inet_sync_node(sock, isk);
    if (isk->tcp)
        return tcp_send(isk->tcp, buf, len,
                        socket_is_nonblocking(sock) || (flags & MSG_DONTWAIT));
    uint32_t dip;
    uint16_t dport;
    if (dest && addrlen >= (int)sizeof(struct sockaddr_in)) {
        struct sockaddr_in *sin = (struct sockaddr_in *)dest;
        dip   = sa_addr(sin);
        dport = sa_port(sin);
    } else if (isk->raw && isk->raw->connected) {
        dip=isk->raw->remote_ip; dport=0;
    } else if (isk->udp && isk->udp->connected) {
        dip   = isk->udp->remote_ip;
        dport = isk->udp->remote_port;
    } else {
        return -107;
    }
    if (isk->raw) return raw_icmp_send(isk->raw,buf,len,dip);
    if (!isk->udp) return -22;
    return udp_sendto(isk->udp,buf,len,dip,dport);
}

static ssize_t inet_send(socket_t *sock, const void *buf, size_t len, int flags) {
    return inet_sendto(sock, buf, len, flags, NULL, 0);
}

static ssize_t inet_recvfrom(socket_t *sock, void *buf, size_t len, int flags,
                              struct sockaddr *src, int *addrlen) {
    inet_sock_t *isk = sock->sk;
    if (!isk) return -22;
    inet_sync_node(sock, isk);
    if (isk->tcp) return tcp_recv(isk->tcp,buf,len,socket_is_nonblocking(sock)||(flags&MSG_DONTWAIT));
    if (isk->raw) {
        uint32_t rip=0;
        ssize_t r=raw_icmp_recv(isk->raw,buf,len,&rip,socket_is_nonblocking(sock)||(flags&MSG_DONTWAIT));
        if(r>=0&&src&&addrlen&&*addrlen>=(int)sizeof(struct sockaddr_in)){ fill_sin((struct sockaddr_in*)src,rip,0); *addrlen=sizeof(struct sockaddr_in); }
        return r;
    }
    if (!isk->udp) return -22;
    bool nb = socket_is_nonblocking(sock) || (flags & MSG_DONTWAIT);
    int tmo = isk->udp->rcvtimeo_ms;
    uint32_t rip = 0; uint16_t rport = 0;
    ssize_t r = udp_recvfrom(isk->udp, buf, len, &rip, &rport, nb, tmo);
    if (r >= 0 && src && addrlen && *addrlen >= (int)sizeof(struct sockaddr_in)) {
        fill_sin((struct sockaddr_in *)src, rip, rport);
        *addrlen = sizeof(struct sockaddr_in);
    }
    return r;
}

static ssize_t inet_recv(socket_t *sock, void *buf, size_t len, int flags) {
    return inet_recvfrom(sock, buf, len, flags, NULL, NULL);
}

static ssize_t inet_sendmsg(socket_t *sock, struct msghdr *msg, int flags) {
    struct sockaddr *dest = NULL;
    int addrlen = 0;
    if (msg->msg_name && msg->msg_namelen >= sizeof(struct sockaddr_in)) {
        dest = (struct sockaddr *)msg->msg_name;
        addrlen = (int)msg->msg_namelen;
    }
    ssize_t total = 0;
    for (size_t i = 0; i < msg->msg_iovlen; i++) {
        ssize_t r = inet_sendto(sock, msg->msg_iov[i].iov_base,
                                msg->msg_iov[i].iov_len, flags, dest, addrlen);
        if (r < 0) return total > 0 ? total : r;
        total += r;
    }
    return total;
}

static ssize_t inet_recvmsg(socket_t *sock, struct msghdr *msg, int flags) {
    struct sockaddr *src = NULL;
    int addrlen = 0;
    if (msg->msg_name && msg->msg_namelen >= sizeof(struct sockaddr_in)) {
        src = (struct sockaddr *)msg->msg_name;
        addrlen = (int)msg->msg_namelen;
    }
    ssize_t total = 0;
    for (size_t i = 0; i < msg->msg_iovlen; i++) {
        ssize_t r = inet_recvfrom(sock, msg->msg_iov[i].iov_base,
                                  msg->msg_iov[i].iov_len, flags, src, &addrlen);
        if (r < 0) return total > 0 ? total : r;
        if (src) { msg->msg_namelen = (uint32_t)addrlen; src = NULL; }
        total += r;
        if ((size_t)r < msg->msg_iov[i].iov_len) break;
    }
    return total;
}

static int inet_getsockname(socket_t *sock, struct sockaddr *addr, int *addrlen) {
    if (!addr || !addrlen || *addrlen < (int)sizeof(struct sockaddr_in)) return -22;
    inet_sock_t *isk = sock->sk;
    if (!isk) return -22;
    if (isk->tcp) fill_sin((struct sockaddr_in *)addr, isk->tcp->local_ip ? isk->tcp->local_ip : ipv4_get_config()->address, isk->tcp->local_port);
    else if (isk->raw) fill_sin((struct sockaddr_in *)addr, isk->raw->local_ip ? isk->raw->local_ip : ipv4_get_config()->address, 0);
    else if (isk->udp) fill_sin((struct sockaddr_in *)addr, isk->udp->local_ip ? isk->udp->local_ip : ipv4_get_config()->address, isk->udp->local_port);
    else return -22;
    *addrlen = sizeof(struct sockaddr_in);
    return 0;
}

static int inet_getpeername(socket_t *sock, struct sockaddr *addr, int *addrlen) {
    if (!addr || !addrlen || *addrlen < (int)sizeof(struct sockaddr_in)) return -22;
    inet_sock_t *isk = sock->sk;
    if (!isk) return -107;
    if (isk->tcp && isk->tcp->remote_ip) fill_sin((struct sockaddr_in *)addr,isk->tcp->remote_ip,isk->tcp->remote_port);
    else if (isk->raw && isk->raw->connected) fill_sin((struct sockaddr_in *)addr, isk->raw->remote_ip, 0);
    else if (isk->udp && isk->udp->connected) fill_sin((struct sockaddr_in *)addr, isk->udp->remote_ip, isk->udp->remote_port);
    else return -107;
    *addrlen = sizeof(struct sockaddr_in);
    return 0;
}

static int inet_getsockopt(socket_t *sock, int level, int optname,
                            void *optval, int *optlen) {
    inet_sock_t *isk = sock->sk;
    if (!isk) return -22;
    if (level == SOL_SOCKET) {
        if (optname == SO_ERROR && *optlen >= (int)sizeof(int)) {
            int err = 0;
            if (isk->tcp) { err = isk->tcp->error; isk->tcp->error = 0; }
            *(int *)optval = err;
            *optlen = sizeof(int);
            return 0;
        }
        if (optname == SO_RCVTIMEO && *optlen >= (int)sizeof(int)) {
            *(int *)optval = isk->udp ? isk->udp->rcvtimeo_ms : isk->rcvtimeo_ms;
            *optlen = sizeof(int);
            return 0;
        }
        if (optname == SO_SNDTIMEO && *optlen >= (int)sizeof(int)) {
            *(int *)optval = isk->udp ? isk->udp->sndtimeo_ms : isk->sndtimeo_ms;
            *optlen = sizeof(int);
            return 0;
        }
        if (optname == SO_KEEPALIVE && *optlen >= (int)sizeof(int)) {
            *(int *)optval = isk->tcp_keepalive ? 1 : 0;
            *optlen = sizeof(int);
            return 0;
        }
        if (optname == SO_ACCEPTCONN && *optlen >= (int)sizeof(int)) {
            *(int *)optval = (isk->tcp && isk->tcp->state == TCP_LISTEN) ? 1 : 0;
            *optlen = sizeof(int);
            return 0;
        }
    }
    if (level == SOL_TCP || level == IPPROTO_TCP) {
        if (optname == TCP_NODELAY && *optlen >= (int)sizeof(int)) {
            *(int *)optval = isk->tcp_nodelay ? 1 : 0;
            *optlen = sizeof(int);
            return 0;
        }
        if (optname == TCP_MAXSEG && *optlen >= (int)sizeof(int)) {
            *(int *)optval = isk->tcp && isk->tcp->mss ? isk->tcp->mss : TCP_MAX_MSS;
            *optlen = sizeof(int);
            return 0;
        }
        if (optname == TCP_KEEPIDLE && *optlen >= (int)sizeof(int)) {
            *(int *)optval = isk->tcp_keepidle_s;
            *optlen = sizeof(int);
            return 0;
        }
        if (optname == TCP_KEEPINTVL && *optlen >= (int)sizeof(int)) {
            *(int *)optval = isk->tcp_keepintvl_s;
            *optlen = sizeof(int);
            return 0;
        }
        if (optname == TCP_KEEPCNT && *optlen >= (int)sizeof(int)) {
            *(int *)optval = isk->tcp_keepcnt;
            *optlen = sizeof(int);
            return 0;
        }
    }
    /* Unknown option — return ENOPROTOOPT */
    return -92;
}

static int inet_timeval_ms(const void *optval, int optlen) {
    if (optlen >= (int)(2 * sizeof(int64_t))) {
        const int64_t *tv = (const int64_t *)optval;
        int64_t ms = tv[0] * 1000 + tv[1] / 1000;
        if (ms < 0) ms = 0;
        if (ms > 3600000) ms = 3600000;
        return (int)ms;
    }
    if (optlen >= (int)sizeof(int)) {
        int v = *(const int *)optval;
        return v < 0 ? 0 : v;
    }
    return 0;
}

static int inet_setsockopt(socket_t *sock, int level, int optname,
                            const void *optval, int optlen) {
    inet_sock_t *isk = sock->sk;
    if (!isk || !optval) return -22;

    if (level == SOL_SOCKET && optlen >= (int)sizeof(int)) {
        int v = *(const int *)optval;

        if (optname == SO_RCVBUF || optname == SO_RCVBUFFORCE) {
            if (isk->tcp) tcp_set_rcvbuf(isk->tcp, v < 0 ? 0 : (size_t)v);
            if (isk->udp) udp_set_rcvbuf(isk->udp, v < 0 ? 0 : (size_t)v);
            return 0;
        }
        if (optname == SO_SNDBUF || optname == SO_SNDBUFFORCE) {
            if (isk->tcp) tcp_set_sndbuf(isk->tcp, v < 0 ? 0 : (size_t)v);
            return 0;
        }
        if (optname == SO_KEEPALIVE) {
            isk->tcp_keepalive = v != 0;
            if (isk->tcp)
                tcp_set_keepalive(isk->tcp, isk->tcp_keepalive,
                                  isk->tcp_keepidle_s, isk->tcp_keepintvl_s,
                                  isk->tcp_keepcnt);
            return 0;
        }
        if (optname == SO_REUSEADDR) {
            sock->reuseaddr = v;
            if (isk->tcp) tcp_set_reuseaddr(isk->tcp, v != 0);
            return 0;
        }
        if (optname == SO_RCVTIMEO) {
            int ms = inet_timeval_ms(optval, optlen);
            isk->rcvtimeo_ms = ms;
            if (isk->udp) isk->udp->rcvtimeo_ms = ms;
            return 0;
        }
        if (optname == SO_SNDTIMEO) {
            int ms = inet_timeval_ms(optval, optlen);
            isk->sndtimeo_ms = ms;
            if (isk->udp) isk->udp->sndtimeo_ms = ms;
            return 0;
        }
        if (optname == SO_LINGER && optlen >= (int)(2 * sizeof(int))) {
            const int *lv = (const int *)optval;
            if (!lv[0]) isk->linger_seconds = -1;
            else if (lv[1] <= 0) isk->linger_seconds = 0;
            else isk->linger_seconds = lv[1] > 3600 ? 3600 : lv[1];
            if (isk->tcp) tcp_set_linger(isk->tcp, isk->linger_seconds);
            return 0;
        }
        if (optname == SO_BROADCAST) return 0;
    }

    if ((level == SOL_IP || level == IPPROTO_IP) && optname == 1 /* IP_TOS */
        && optlen >= (int)sizeof(int)) {
        if (isk->udp)
            udp_set_tos(isk->udp, *(const int *)optval);
        return 0;
    }

    if ((level == SOL_TCP || level == IPPROTO_TCP) && isk->tcp) {
        if (optlen < (int)sizeof(int)) return -22;
        int v = *(const int *)optval;
        switch (optname) {
        case TCP_NODELAY:
            isk->tcp_nodelay = v != 0;
            tcp_set_nodelay(isk->tcp, v != 0);
            return 0;
        case TCP_MAXSEG:
            tcp_set_mss(isk->tcp, (uint16_t)v);
            return 0;
        case TCP_KEEPIDLE:
            isk->tcp_keepidle_s = v;
            tcp_set_keepalive(isk->tcp, isk->tcp_keepalive,
                              isk->tcp_keepidle_s, isk->tcp_keepintvl_s,
                              isk->tcp_keepcnt);
            return 0;
        case TCP_KEEPINTVL:
            isk->tcp_keepintvl_s = v;
            tcp_set_keepalive(isk->tcp, isk->tcp_keepalive,
                              isk->tcp_keepidle_s, isk->tcp_keepintvl_s,
                              isk->tcp_keepcnt);
            return 0;
        case TCP_KEEPCNT:
            isk->tcp_keepcnt = v;
            tcp_set_keepalive(isk->tcp, isk->tcp_keepalive,
                              isk->tcp_keepidle_s, isk->tcp_keepintvl_s,
                              isk->tcp_keepcnt);
            return 0;
        default:
            return 0;
        }
    }

    if (level == IPPROTO_UDP) return 0;
    return 0;
}

static int inet_shutdown(socket_t *sock, int how) {
    inet_sock_t *isk = sock->sk;
    if (!isk) return -107; /* ENOTCONN */
    if (isk->tcp) {
        if ((how == SHUT_RD || how == SHUT_RDWR) && !isk->shut_rd)
            isk->shut_rd = true;
        if ((how == SHUT_WR || how == SHUT_RDWR) && !isk->shut_wr) {
            isk->shut_wr = true;
            tcp_close(isk->tcp);
        }
    }
    if (sock->wait_queue)
        wait_queue_wake_all((wait_queue_t *)sock->wait_queue);
    return 0;
}

static int inet_poll(socket_t *sock, int events) {
    inet_sock_t *isk = sock->sk;
    if (!isk) return POLLNVAL;
    if (isk->tcp) {
        /* Keep vfs_node in sync so wake() can notify epoll */
        isk->tcp->vfs_node = sock->node;
        int r = 0;
        switch (isk->tcp->state) {
        case TCP_LISTEN:
            /* Readable when there's a connection to accept */
            if ((events & POLLIN) && tcp_accept_pending(isk->tcp))
                r |= POLLIN;
            break;
        case TCP_SYN_SENT:
        case TCP_SYN_RECEIVED:
            /* Don't drive the timer here — let the net worker process the
             * incoming SYN+ACK without lock contention. Report nothing until
             * the state transitions, or POLLERR|POLLOUT on connect failure. */
            if (isk->tcp->error) {
                r |= POLLERR;
                if (events & POLLOUT) r |= POLLOUT;
            }
            break;
        case TCP_ESTABLISHED:
        case TCP_CLOSE_WAIT:
            if ((events & POLLIN)  && tcp_readable(isk->tcp)) r |= POLLIN;
            if ((events & POLLOUT) && tcp_writable(isk->tcp))  r |= POLLOUT;
            if (isk->tcp->error) r |= POLLERR;
            /* CLOSE_WAIT means peer sent FIN — signal RDHUP and POLLIN (EOF) */
            if (isk->tcp->state == TCP_CLOSE_WAIT)
                r |= EPOLLRDHUP | POLLIN;
            break;
        case TCP_RESET:
            r |= POLLERR | POLLHUP;
            if (events & POLLIN)  r |= POLLIN;
            if (events & POLLOUT) r |= POLLOUT;
            break;
        case TCP_FIN_WAIT_1:
        case TCP_FIN_WAIT_2:
        case TCP_LAST_ACK:
        case TCP_TIME_WAIT:
        case TCP_CLOSED:
            r |= POLLHUP;
            if (isk->tcp->error) {
                r |= POLLERR;
                if (events & POLLOUT) r |= POLLOUT;
            }
            if (events & POLLIN) r |= POLLIN;
            break;
        default:
            break;
        }
        return r;
    }
    if (isk->raw) {
        isk->raw->node = sock->node;
        return (((events & POLLIN) && raw_icmp_readable(isk->raw)) ? POLLIN : 0)
               | (events & POLLOUT);
    }
    if (!isk->udp) return POLLNVAL;
    isk->udp->vfs_node = sock->node;
    int revents = 0;
    if ((events & POLLIN) && isk->udp->q_head != isk->udp->q_tail)
        revents |= POLLIN;
    if (events & POLLOUT) revents |= POLLOUT;
    return revents;
}

static sock_ops_t inet_udp_ops = {
    .bind        = inet_bind,
    .connect     = inet_connect,
    .listen      = inet_listen,
    .accept      = inet_accept,
    .send        = inet_send,
    .recv        = inet_recv,
    .sendto      = inet_sendto,
    .recvfrom    = inet_recvfrom,
    .sendmsg     = inet_sendmsg,
    .recvmsg     = inet_recvmsg,
    .getsockopt  = inet_getsockopt,
    .setsockopt  = inet_setsockopt,
    .shutdown    = inet_shutdown,
    .poll        = inet_poll,
    .ioctl       = NULL,
    .getsockname = inet_getsockname,
    .getpeername = inet_getpeername,
    .destroy     = inet_destroy,
};

static int inet_family_create(socket_t *sock, int protocol) {
    sock->ops = &inet_udp_ops;
    return inet_create(sock, protocol);
}

static net_family_t inet_family = {
    .family = AF_INET,
    .create = inet_family_create,
    .next   = NULL,
};

void af_inet_init(void) {
    raw_icmp_init();
    sock_register_family(&inet_family);
    ipv4_set_udp_handler(udp_deliver);
    ipv4_set_icmp_handler(raw_icmp_deliver);
    ipv4_set_tcp_handler(tcp_input_ipv4);
    klog_puts("[OK] AF_INET registered (UDP, TCP, and raw ICMP)\n");
}
