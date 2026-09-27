#include "net/dhcp.h"
#include "apic/lapic_timer.h"
#include "console/klog.h"
#include "lib/string.h"
#include "lock/spinlock.h"
#include "net/core.h"
#include "net/ipv4.h"
#include "sched/sched.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DHCP_SERVER_PORT   67
#define DHCP_CLIENT_PORT   68
#define DHCP_MAGIC_COOKIE  0x63825363u

#define DHCP_DISCOVER  1
#define DHCP_OFFER     2
#define DHCP_REQUEST   3
#define DHCP_DECLINE   4
#define DHCP_ACK       5
#define DHCP_NAK       6
#define DHCP_RELEASE   7
#define DHCP_INFORM    8

#define OPT_SUBNET_MASK      1
#define OPT_ROUTER           3
#define OPT_DNS              6
#define OPT_HOSTNAME         12
#define OPT_REQUESTED_IP     50
#define OPT_LEASE_TIME       51
#define OPT_MSG_TYPE         53
#define OPT_SERVER_ID        54
#define OPT_PARAM_REQUEST    55
#define OPT_RENEWAL_TIME     58
#define OPT_REBIND_TIME      59
#define OPT_END              255

#define DHCP_INITIAL_TIMEOUT_MS   4000u
#define DHCP_MAX_TIMEOUT_MS      64000u
#define DHCP_TOTAL_TIMEOUT_MS   120000u
#define DHCP_RENEW_RETRY_MS       5000u

struct eth_hdr {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t type;
} __attribute__((packed));

struct ipv4_hdr {
    uint8_t  ver_ihl;
    uint8_t  dscp;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint8_t  src[4];
    uint8_t  dst[4];
} __attribute__((packed));

struct udp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed));

struct dhcp_msg {
    uint8_t  op;         
    uint8_t  htype;     
    uint8_t  hlen;      
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint8_t  ciaddr[4];
    uint8_t  yiaddr[4];
    uint8_t  siaddr[4];
    uint8_t  giaddr[4];
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
} __attribute__((packed));

#define DHCP_OPTIONS_MAX 312
#define DHCP_FRAME_MAX   (sizeof(struct eth_hdr) + sizeof(struct ipv4_hdr) + \
                          sizeof(struct udp_hdr) + sizeof(struct dhcp_msg) + \
                          DHCP_OPTIONS_MAX)

static spinlock_t        dhcp_lock = SPINLOCK_INIT;
static struct dhcp_lease current_lease;
static bool              lease_valid;
static bool              static_fallback_set;
static uint32_t          fallback_addr, fallback_mask, fallback_gw;

static volatile bool     rx_got_offer;
static volatile bool     rx_got_ack;
static volatile bool     rx_got_nak;
static struct dhcp_lease rx_pending;  
static uint32_t          active_xid;

static net_rx_handler_t  prev_rx_handler;


static inline uint16_t be16(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}
static inline uint32_t be32(uint32_t v) {
    return ((v & 0x000000ffu) << 24) | ((v & 0x0000ff00u) << 8) |
           ((v & 0x00ff0000u) >> 8)  | ((v & 0xff000000u) >> 24);
}
static inline void put32_be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static inline uint32_t get32_be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}


static uint16_t inet_checksum(const void *data, size_t len) {
    const uint8_t *b = (const uint8_t *)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += ((uint32_t)b[0] << 8) | b[1];
        b += 2; len -= 2;
    }
    if (len) sum += (uint32_t)b[0] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint16_t udp_checksum(const uint8_t src[4], const uint8_t dst[4],
                              const void *udp_start, uint16_t udp_len) {
    uint32_t sum = 0;
    for (int i = 0; i < 4; i += 2)
        sum += ((uint32_t)src[i] << 8) | src[i+1];
    for (int i = 0; i < 4; i += 2)
        sum += ((uint32_t)dst[i] << 8) | dst[i+1];
    sum += 17u;
    sum += (uint32_t)udp_len;
    const uint8_t *b = (const uint8_t *)udp_start;
    uint16_t rem = udp_len;
    while (rem > 1) {
        sum += ((uint32_t)b[0] << 8) | b[1];
        b += 2; rem -= 2;
    }
    if (rem) sum += (uint32_t)b[0] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}


typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   pos;
    bool     overflow;
} opt_writer_t;

static void opt_u8(opt_writer_t *w, uint8_t tag, uint8_t val) {
    if (w->pos + 3 > w->cap) { w->overflow = true; return; }
    w->buf[w->pos++] = tag;
    w->buf[w->pos++] = 1;
    w->buf[w->pos++] = val;
}
static void opt_u32(opt_writer_t *w, uint8_t tag, uint32_t val) {
    if (w->pos + 6 > w->cap) { w->overflow = true; return; }
    w->buf[w->pos++] = tag;
    w->buf[w->pos++] = 4;
    put32_be(w->buf + w->pos, val);
    w->pos += 4;
}
static void opt_bytes(opt_writer_t *w, uint8_t tag,
                      const uint8_t *data, uint8_t len) {
    if (w->pos + 2u + len > w->cap) { w->overflow = true; return; }
    w->buf[w->pos++] = tag;
    w->buf[w->pos++] = len;
    memcpy(w->buf + w->pos, data, len);
    w->pos += len;
}
static void opt_end(opt_writer_t *w) {
    if (w->pos < w->cap) w->buf[w->pos++] = OPT_END;
    else w->overflow = true;
}

typedef struct {
    const uint8_t *buf;
    size_t         len;
    size_t         pos;
} opt_reader_t;

static bool opt_next(opt_reader_t *r, uint8_t *tag,
                     const uint8_t **data, uint8_t *out_len) {
    while (r->pos < r->len) {
        uint8_t t = r->buf[r->pos++];
        if (t == 0) continue;             
        if (t == OPT_END) return false;    
        if (r->pos >= r->len) return false;
        uint8_t l = r->buf[r->pos++];
        if (r->pos + l > r->len) return false;
        *tag     = t;
        *data    = r->buf + r->pos;
        *out_len = l;
        r->pos  += l;
        return true;
    }
    return false;
}

static size_t build_dhcp_frame(uint8_t *out, size_t out_cap,
                               const struct net_device *dev,
                               uint32_t xid, uint8_t msg_type,
                               uint32_t requested_ip, uint32_t server_id,
                               uint16_t secs) {
    static const uint8_t broadcast_mac[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
    static const uint8_t zero_ip[4]  = {0};
    static const uint8_t bcast_ip[4] = {255,255,255,255};

    uint8_t dhcp_buf[sizeof(struct dhcp_msg) + DHCP_OPTIONS_MAX];
    struct dhcp_msg *msg = (struct dhcp_msg *)dhcp_buf;
    memset(msg, 0, sizeof(*msg));
    msg->op    = 1;    
    msg->htype = 1;
    msg->hlen  = 6;
    msg->xid   = be32(xid);
    msg->secs  = be16(secs);
    msg->flags = be16(0x8000); 
    memcpy(msg->chaddr, dev->mac, 6);
    msg->magic = be32(DHCP_MAGIC_COOKIE);

    uint8_t opts[DHCP_OPTIONS_MAX];
    opt_writer_t w = { .buf = opts, .cap = sizeof(opts), .pos = 0, .overflow = false };
    opt_u8(&w, OPT_MSG_TYPE, msg_type);

    if (msg_type == DHCP_REQUEST) {
        if (requested_ip) opt_u32(&w, OPT_REQUESTED_IP, requested_ip);
        if (server_id)    opt_u32(&w, OPT_SERVER_ID, server_id);
    }
    const uint8_t prl[] = { OPT_SUBNET_MASK, OPT_ROUTER, OPT_DNS,
                             OPT_LEASE_TIME, OPT_RENEWAL_TIME, OPT_REBIND_TIME };
    opt_bytes(&w, OPT_PARAM_REQUEST, prl, sizeof(prl));
    opt_end(&w);
    if (w.overflow) return 0;

    size_t dhcp_len = sizeof(struct dhcp_msg) + w.pos;
    memcpy(dhcp_buf + sizeof(struct dhcp_msg), opts, w.pos);

    /* Sizes */
    uint16_t udp_payload = (uint16_t)dhcp_len;
    uint16_t udp_total   = (uint16_t)(sizeof(struct udp_hdr) + udp_payload);
    uint16_t ip_total    = (uint16_t)(sizeof(struct ipv4_hdr) + udp_total);
    size_t   frame_total = sizeof(struct eth_hdr) + ip_total;
    if (frame_total > out_cap) return 0;

    uint8_t *p = out;

    struct eth_hdr *eth = (struct eth_hdr *)p;
    memcpy(eth->dst, broadcast_mac, 6);
    memcpy(eth->src, dev->mac, 6);
    eth->type = be16(0x0800);
    p += sizeof(struct eth_hdr);

    struct ipv4_hdr *ip = (struct ipv4_hdr *)p;
    memset(ip, 0, sizeof(*ip));
    ip->ver_ihl   = 0x45;
    ip->total_len = be16(ip_total);
    ip->id        = be16(xid & 0xffff);
    ip->frag      = be16(0x4000); 
    ip->ttl       = 64;
    ip->proto     = 17; 
    memcpy(ip->src, zero_ip, 4);
    memcpy(ip->dst, bcast_ip, 4);
    ip->checksum  = be16(inet_checksum(ip, sizeof(*ip)));
    p += sizeof(struct ipv4_hdr);

    struct udp_hdr *udp = (struct udp_hdr *)p;
    udp->src_port = be16(DHCP_CLIENT_PORT);
    udp->dst_port = be16(DHCP_SERVER_PORT);
    udp->length   = be16(udp_total);
    udp->checksum = 0;
    p += sizeof(struct udp_hdr);

    memcpy(p, dhcp_buf, dhcp_len);

    uint8_t src_z[4] = {0};
    udp->checksum = be16(udp_checksum(src_z, bcast_ip,
                                       udp, udp_total));

    return frame_total;
}

static void parse_dhcp_reply(const uint8_t *frame, size_t len) {
    const size_t min_len = sizeof(struct eth_hdr) + sizeof(struct ipv4_hdr) +
                           sizeof(struct udp_hdr) + sizeof(struct dhcp_msg);
    if (len < min_len) return;

    const struct eth_hdr *eth = (const struct eth_hdr *)frame;
    if (be16(eth->type) != 0x0800) return;

    const struct ipv4_hdr *ip =
        (const struct ipv4_hdr *)(frame + sizeof(*eth));
    if ((ip->ver_ihl >> 4) != 4) return;
    if (ip->proto != 17) return;
    size_t ip_hlen = (size_t)(ip->ver_ihl & 0x0f) * 4;
    if (ip_hlen < 20) return;

    const struct udp_hdr *udp =
        (const struct udp_hdr *)((const uint8_t *)ip + ip_hlen);
    if (be16(udp->dst_port) != DHCP_CLIENT_PORT) return;
    if (be16(udp->src_port) != DHCP_SERVER_PORT) return;

    uint16_t udp_len = be16(udp->length);
    if (udp_len < sizeof(*udp) + sizeof(struct dhcp_msg)) return;
    size_t udp_offset = sizeof(*eth) + ip_hlen;
    if (udp_offset + udp_len > len) return;

    const struct dhcp_msg *msg =
        (const struct dhcp_msg *)((const uint8_t *)udp + sizeof(*udp));

    if (msg->op != 2) return;
    if (be32(msg->magic) != DHCP_MAGIC_COOKIE) return;
    if (be32(msg->xid) != active_xid) return;

    size_t opts_start = sizeof(*eth) + ip_hlen + sizeof(*udp) +
                        sizeof(struct dhcp_msg);
    size_t opts_len   = (udp_offset + udp_len) >
                        (opts_start + sizeof(*udp))
                        ? (udp_offset + udp_len) - opts_start
                        : 0;
    if (opts_start + opts_len > len) opts_len = len - opts_start;

    opt_reader_t r = {
        .buf = frame + opts_start,
        .len = opts_len,
        .pos = 0
    };

    uint8_t   msg_type  = 0;
    struct dhcp_lease tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.address = get32_be(msg->yiaddr);

    uint8_t tag, olen;
    const uint8_t *odata;
    while (opt_next(&r, &tag, &odata, &olen)) {
        switch (tag) {
        case OPT_MSG_TYPE:
            if (olen >= 1) msg_type = odata[0];
            break;
        case OPT_SUBNET_MASK:
            if (olen == 4) tmp.netmask = get32_be(odata);
            break;
        case OPT_ROUTER:
            if (olen >= 4) tmp.gateway = get32_be(odata);
            break;
        case OPT_DNS:
            if (olen >= 4) tmp.dns[0] = get32_be(odata);
            if (olen >= 8) tmp.dns[1] = get32_be(odata + 4);
            break;
        case OPT_LEASE_TIME:
            if (olen == 4) tmp.lease_time = get32_be(odata);
            break;
        case OPT_SERVER_ID:
            if (olen == 4) tmp.server_id = get32_be(odata);
            break;
        case OPT_RENEWAL_TIME:
            if (olen == 4) tmp.renew_time = get32_be(odata);
            break;
        case OPT_REBIND_TIME:
            if (olen == 4) tmp.rebind_time = get32_be(odata);
            break;
        default:
            break;
        }
    }

    if (msg_type == 0 || tmp.address == 0) return;

    if (tmp.address == 0xffffffffu) return;

    spinlock_acquire(&dhcp_lock);
    switch (msg_type) {
    case DHCP_OFFER:
        if (!rx_got_offer) {
            rx_pending    = tmp;
            rx_got_offer  = true;
        }
        break;
    case DHCP_ACK:
        rx_pending  = tmp;
        rx_got_ack  = true;
        break;
    case DHCP_NAK:
        rx_got_nak  = true;
        break;
    default:
        break;
    }
    spinlock_release(&dhcp_lock);
}

static void dhcp_rx_handler(struct net_packet *pkt) {
    if (prev_rx_handler)
        prev_rx_handler(pkt);
    if (pkt && pkt->length >= 14)
        parse_dhcp_reply(pkt->data, pkt->length);
}

static void apply_lease(const struct dhcp_lease *l) {
    struct dhcp_lease tmp = *l;
    tmp.obtained_ms = lapic_timer_get_ticks();

    if (!tmp.renew_time && tmp.lease_time)
        tmp.renew_time  = tmp.lease_time / 2;
    if (!tmp.rebind_time && tmp.lease_time)
        tmp.rebind_time = (tmp.lease_time * 7) / 8;

    spinlock_acquire(&dhcp_lock);
    current_lease = tmp;
    lease_valid   = true;
    spinlock_release(&dhcp_lock);

    struct ipv4_config cfg;
    cfg.address = tmp.address;
    cfg.netmask = tmp.netmask ? tmp.netmask : 0xffffff00u;
    cfg.gateway = tmp.gateway;
    ipv4_apply_config(&cfg);

    klog_puts("[DHCP] Lease applied: ");
    klog_hex32(tmp.address);
    klog_puts("/");
    klog_hex32(tmp.netmask);
    klog_puts(" gw=");
    klog_hex32(tmp.gateway);
    klog_puts(" lease=");
    klog_uint64(tmp.lease_time);
    klog_puts("s T1=");
    klog_uint64(tmp.renew_time);
    klog_puts("s T2=");
    klog_uint64(tmp.rebind_time);
    klog_puts("s\n");
}

static uint32_t next_xid(const struct net_device *dev) {
    static uint32_t counter = 0;
    uint32_t seed = ((uint32_t)dev->mac[2] << 24) |
                    ((uint32_t)dev->mac[3] << 16) |
                    ((uint32_t)dev->mac[4] <<  8) |
                     (uint32_t)dev->mac[5];
    return seed ^ ((uint32_t)lapic_timer_get_ticks() & 0xffffffffu)
                ^ (++counter * 0x9e3779b9u);
}

static bool dhcp_exchange(struct net_device *dev, uint32_t timeout_ms,
                           struct dhcp_lease *lease_out) {
    uint8_t frame[NET_FRAME_MAX];

    uint32_t xid  = next_xid(dev);
    active_xid    = xid;
    rx_got_offer  = false;
    rx_got_ack    = false;
    rx_got_nak    = false;

    uint64_t deadline = lapic_timer_get_ticks() + timeout_ms;

    size_t flen = build_dhcp_frame(frame, sizeof(frame), dev, xid,
                                   DHCP_DISCOVER, 0, 0, 0);
    if (!flen) return false;
    if (dev->ops->transmit(dev, frame, flen) != 0) return false;
    dev->stats.tx_packets++;

    while (!rx_got_offer && !rx_got_nak &&
           lapic_timer_get_ticks() < deadline)
        sched_yield();

    if (!rx_got_offer) return false;

    spinlock_acquire(&dhcp_lock);
    struct dhcp_lease offer = rx_pending;
    rx_got_offer = false;
    spinlock_release(&dhcp_lock);

    flen = build_dhcp_frame(frame, sizeof(frame), dev, xid,
                             DHCP_REQUEST, offer.address, offer.server_id, 0);
    if (!flen) return false;
    if (dev->ops->transmit(dev, frame, flen) != 0) return false;
    dev->stats.tx_packets++;

    while (!rx_got_ack && !rx_got_nak &&
           lapic_timer_get_ticks() < deadline)
        sched_yield();

    if (!rx_got_ack) return false;

    spinlock_acquire(&dhcp_lock);
    *lease_out = rx_pending;
    spinlock_release(&dhcp_lock);

    return lease_out->address != 0;
}

void dhcp_set_static_fallback(uint32_t address, uint32_t netmask,
                               uint32_t gateway) {
    spinlock_acquire(&dhcp_lock);
    fallback_addr        = address;
    fallback_mask        = netmask;
    fallback_gw          = gateway;
    static_fallback_set  = true;
    spinlock_release(&dhcp_lock);
}

bool dhcp_start(void) {
    struct net_device *dev = net_device_default();
    if (!dev) {
        klog_puts("[DHCP] No network device — skipping\n");
        goto use_fallback;
    }

    prev_rx_handler = net_get_rx_handler(); /* save existing (ipv4) handler */

    net_set_rx_handler(dhcp_rx_handler);

    spinlock_init(&dhcp_lock);
    lease_valid   = false;
    rx_got_offer  = false;
    rx_got_ack    = false;
    rx_got_nak    = false;

    uint32_t wait_ms  = DHCP_INITIAL_TIMEOUT_MS;
    uint64_t absolute = lapic_timer_get_ticks() + DHCP_TOTAL_TIMEOUT_MS;
    uint32_t attempt  = 0;
    struct dhcp_lease lease;

    while (lapic_timer_get_ticks() < absolute) {
        attempt++;
        klog_puts("[DHCP] DISCOVER attempt ");
        klog_uint64(attempt);
        klog_puts(" (timeout ");
        klog_uint64(wait_ms);
        klog_puts("ms)\n");

        if (dhcp_exchange(dev, wait_ms, &lease)) {
            apply_lease(&lease);
            klog_puts("[DHCP] Lease obtained\n");
            return true;
        }

        wait_ms = wait_ms < DHCP_MAX_TIMEOUT_MS / 2
                  ? wait_ms * 2 : DHCP_MAX_TIMEOUT_MS;
        rx_got_nak = false;
    }

    klog_puts("[DHCP] All attempts exhausted\n");

use_fallback:
    spinlock_acquire(&dhcp_lock);
    bool has_fallback = static_fallback_set;
    uint32_t fa = fallback_addr, fm = fallback_mask, fg = fallback_gw;
    spinlock_release(&dhcp_lock);

    if (has_fallback) {
        struct dhcp_lease fl;
        memset(&fl, 0, sizeof(fl));
        fl.address    = fa;
        fl.netmask    = fm;
        fl.gateway    = fg;
        fl.lease_time = 0; 
        apply_lease(&fl);
        klog_puts("[DHCP] Using static fallback address\n");
    }
    return false;
}

const struct dhcp_lease *dhcp_current_lease(void) {
    return lease_valid ? &current_lease : NULL;
}

bool dhcp_renew(void) {
    spinlock_acquire(&dhcp_lock);
    bool valid = lease_valid;
    struct dhcp_lease saved = current_lease;
    spinlock_release(&dhcp_lock);

    if (!valid) return false;

    struct net_device *dev = net_device_default();
    if (!dev) return false;

    klog_puts("[DHCP] Renewing lease\n");

    rx_got_offer = false;
    rx_got_ack   = false;
    rx_got_nak   = false;

    struct dhcp_lease new_lease;
    uint32_t xid = next_xid(dev);
    active_xid   = xid;
    uint8_t frame[NET_FRAME_MAX];
    size_t flen = build_dhcp_frame(frame, sizeof(frame), dev, xid,
                                    DHCP_REQUEST, saved.address,
                                    saved.server_id, 0);
    if (!flen) return false;
    if (dev->ops->transmit(dev, frame, flen) != 0) return false;
    dev->stats.tx_packets++;

    uint64_t deadline = lapic_timer_get_ticks() + DHCP_RENEW_RETRY_MS;
    while (!rx_got_ack && !rx_got_nak &&
           lapic_timer_get_ticks() < deadline)
        sched_yield();

    if (!rx_got_ack) {
        klog_puts("[DHCP] Renewal failed\n");
        return false;
    }
    spinlock_acquire(&dhcp_lock);
    new_lease = rx_pending;
    spinlock_release(&dhcp_lock);

    if (new_lease.address) {
        apply_lease(&new_lease);
        klog_puts("[DHCP] Lease renewed\n");
        return true;
    }
    return false;
}

bool net_phase5_init(void) {
    klog_puts("[NET] starting DHCP client\n");
    bool got_lease = dhcp_start();
    if (!got_lease)
        klog_puts("[DHCP] lease acquisition failed\n");
    return got_lease;
}
