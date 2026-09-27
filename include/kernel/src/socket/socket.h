#ifndef SOCKET_H
#define SOCKET_H

#include "../lock/spinlock.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ssize_t is not defined in freestanding headers
typedef int64_t ssize_t;

// Address Families
#define AF_UNSPEC 0
// Socket Address Family Type
typedef uint16_t sa_family_t;

#define AF_UNIX 1
#define AF_INET 2
#define AF_INET6 10
#define AF_NETLINK 16
#define NETLINK_ROUTE 0
#define NETLINK_KOBJECT_UEVENT 15

// Netlink Address Structure
struct sockaddr_nl {
  sa_family_t nl_family;   // AF_NETLINK
  unsigned short nl_pad;   // zero
  uint32_t nl_pid;         // port ID
  uint32_t nl_groups;      // multicast groups mask
};

#define SOL_NETLINK 270
#define NETLINK_ADD_MEMBERSHIP 1
#define NETLINK_DROP_MEMBERSHIP 2

// Socket Types
#define SOCK_STREAM 1    // Stream (connection-oriented)
#define SOCK_DGRAM 2     // Datagram (connectionless)
#define SOCK_RAW 3       // Raw socket
#define SOCK_SEQPACKET 5 // Sequenced packet stream

// Socket States
#define SS_UNCONNECTED 0
#define SS_CONNECTING 1
#define SS_CONNECTED 2
#define SS_DISCONNECTING 3
#define SS_LISTENING 4

// Socket Options Levels
#define SOL_SOCKET 1

// Socket Options
#define SO_REUSEADDR 2
#define SO_TYPE 3
#define SO_ERROR 4
#define SO_DONTROUTE 5
#define SO_BROADCAST 6
#define SO_SNDBUF 7
#define SO_RCVBUF 8
#define SO_KEEPALIVE 9
#define SO_OOBINLINE 10
#define SO_NO_CHECK 11
#define SO_PRIORITY 12
#define SO_LINGER 13
#define SO_BSDCOMPAT 14
#define SO_REUSEPORT 15
#define SO_PASSCRED 16
#define SO_PEERCRED 17
#define SO_RCVLOWAT 18
#define SO_SNDLOWAT 19
#define SO_RCVTIMEO 20
#define SO_SNDTIMEO 21
#define SO_RCVBUFFORCE 33
#define SO_SNDBUFFORCE 32
#define SO_DOMAIN 39
#define SO_PROTOCOL 38
#define SO_ACCEPTCONN 30

// Socket Flags
#define SOCK_CLOEXEC 0x080000 // Close on exec
#define SOCK_NONBLOCK 0x0800  // Non-blocking

// Message Flags
#define MSG_OOB 0x0001
#define MSG_PEEK 0x0002
#define MSG_DONTROUTE 0x0004
#define MSG_CTRUNC 0x0008
#define MSG_TRUNC 0x0020
#define MSG_DONTWAIT 0x0040
#define MSG_EOR 0x0080
#define MSG_WAITALL 0x0100
#define MSG_NOSIGNAL 0x4000
#define MSG_CMSG_CLOEXEC 0x40000000

// Shutdown How
#define SHUT_RD 0   // Disallow further receptions
#define SHUT_WR 1   // Disallow further transmissions
#define SHUT_RDWR 2 // Disallow further receptions and transmissions

// Socket Errors
#define EAFNOSUPPORT 97
#define EPROTONOSUPPORT 93
#define EPROTOTYPE 92
#define EADDRINUSE 98
#define EADDRNOTAVAIL 99
#define ENOTCONN 107
#define ECONNREFUSED 111
#define ECONNRESET 104
#define EISCONN 106
#define EINPROGRESS 115
#define EALREADY 114
#define EAGAIN 11
#define EWOULDBLOCK 11

// Maximum Values
#define UNIX_PATH_MAX 108
#define SOCKET_MAX_FDS 256 // Max sockets per process (separate from MAX_FDS)

// Forward Declarations
struct socket;
struct sock_ops;
struct vfs_node;

// Socket Address Family Type
// (moved up)

// Socket Address Structure (generic)
struct sockaddr {
  sa_family_t sa_family;
  char sa_data[14];
};

// Unix Domain Socket Address
struct iovec {
  void *iov_base;
  size_t iov_len;
};

struct msghdr {
  void *msg_name;           // Source address (for recvmsg)
  uint32_t msg_namelen;     // Address length (socklen_t — 4 bytes, matches Linux ABI)
  uint32_t _pad;            // Implicit ABI padding to align msg_iov to 8 bytes
  struct iovec *msg_iov;    // Scatter/gather array
  size_t msg_iovlen;        // Number of iovec elements
  void *msg_control;        // Ancillary data
  size_t msg_controllen;    // Ancillary data length
  int msg_flags;            // Flags on received message
};

struct sockaddr_un {
  sa_family_t sun_family;
  char sun_path[UNIX_PATH_MAX];
};

struct in_addr {
  uint32_t s_addr;
};

struct sockaddr_in {
  sa_family_t    sin_family;
  uint16_t       sin_port;
  struct in_addr sin_addr;
  uint8_t        sin_zero[8];
};

struct in6_addr { uint8_t s6_addr[16]; };
struct sockaddr_in6 {
  sa_family_t sin6_family;
  uint16_t sin6_port;
  uint32_t sin6_flowinfo;
  struct in6_addr sin6_addr;
  uint32_t sin6_scope_id;
};

#define INADDR_ANY       0x00000000u
#define INADDR_BROADCAST 0xffffffffu
#define INADDR_LOOPBACK  0x7f000001u

#define IPPROTO_IP   0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17
#define IPPROTO_IPV6 41
#define IPPROTO_ICMPV6 58
#define SOL_IPV6 41
#define IPV6_V6ONLY 26

#define SOL_TCP 6
#define TCP_NODELAY   1
#define TCP_MAXSEG    2
#define TCP_KEEPIDLE  4
#define TCP_KEEPINTVL 5
#define TCP_KEEPCNT   6

#define ETIMEDOUT 110
#define ECONNABORTED 103
#define EPIPE 32
#define EHOSTUNREACH 113
#define ENETUNREACH 101
#define ENOBUFS 105

#define SOL_IP  0
#define SOL_UDP 17
#define IPPROTO_UDP_ALIAS 17

// Socket Operations Vector
typedef struct sock_ops {
  int (*bind)(struct socket *sock, struct sockaddr *addr, int addrlen);
  int (*connect)(struct socket *sock, struct sockaddr *addr, int addrlen);
  int (*listen)(struct socket *sock, int backlog);
  int (*accept)(struct socket *sock, struct socket **newsock);
  ssize_t (*send)(struct socket *sock, const void *buf, size_t len, int flags);
  ssize_t (*recv)(struct socket *sock, void *buf, size_t len, int flags);
  ssize_t (*sendto)(struct socket *sock, const void *buf, size_t len, int flags,
                    struct sockaddr *dest_addr, int addrlen);
  ssize_t (*recvfrom)(struct socket *sock, void *buf, size_t len, int flags,
                      struct sockaddr *src_addr, int *addrlen);
  int (*getsockopt)(struct socket *sock, int level, int optname, void *optval,
                    int *optlen);
  int (*setsockopt)(struct socket *sock, int level, int optname,
                    const void *optval, int optlen);
  ssize_t (*sendmsg)(struct socket *sock, struct msghdr *msg, int flags);
  ssize_t (*recvmsg)(struct socket *sock, struct msghdr *msg, int flags);
  int (*shutdown)(struct socket *sock, int how);
  int (*poll)(struct socket *sock, int events);
  int (*ioctl)(struct socket *sock, uint32_t request, uint64_t arg);
  int (*getsockname)(struct socket *sock, struct sockaddr *addr, int *addrlen);
  int (*getpeername)(struct socket *sock, struct sockaddr *addr, int *addrlen);
  void (*destroy)(struct socket *sock);
} sock_ops_t;

// Socket Structure
typedef struct socket {
  int domain;            // AF_UNIX or AF_NETLINK
  int type;              // SOCK_STREAM, SOCK_DGRAM, etc.
  int protocol;          // Protocol (usually 0)
  volatile int state;    // SS_UNCONNECTED, SS_CONNECTED, etc.
  int fd;                // File descriptor for this socket
  int flags;             // Socket flags (SOCK_NONBLOCK, etc.)
  int error;             // Socket error code
  int rcvbuf;            // Receive buffer size
  int sndbuf;            // Send buffer size
  int reuseaddr;         // SO_REUSEADDR flag
  struct sock_ops *ops;  // Operations vector
  void *sk;              // Family-specific socket data
  struct vfs_node *node; // VFS node for this socket
  struct socket *peer;   // Connected peer (for socketpair)
  void *wait_queue;      // Wait queue for blocking operations
  uint64_t refcount;     // Reference count
  volatile bool closing; // Last file reference was closed
  spinlock_t lock;       // Spinlock for synchronization
} socket_t;

// Socket Subsystem Initialization
void socket_init(void);

// Socket Creation/ Destruction
socket_t *socket_create(int domain, int type, int protocol);
void socket_destroy(socket_t *sock);
void socket_get(socket_t *sock); // Increment reference count
bool socket_try_get(socket_t *sock); // Increment unless already destroying
void socket_put(socket_t *sock); // Decrement reference count

// Socket Operations
int socket_bind(socket_t *sock, struct sockaddr *addr, int addrlen);
int socket_connect(socket_t *sock, struct sockaddr *addr, int addrlen);
int socket_listen(socket_t *sock, int backlog);
int socket_accept(socket_t *sock, socket_t **newsock);
ssize_t socket_send(socket_t *sock, const void *buf, size_t len, int flags);
ssize_t socket_recv(socket_t *sock, void *buf, size_t len, int flags);
ssize_t socket_sendto(socket_t *sock, const void *buf, size_t len, int flags,
                      struct sockaddr *dest_addr, int addrlen);
ssize_t socket_recvfrom(socket_t *sock, void *buf, size_t len, int flags,
                        struct sockaddr *src_addr, int *addrlen);

// Socketpair Creation
int socket_create_pair(int domain, int type, int protocol, socket_t *sv[2]);

// Socket FD Management
int socket_alloc_fd(socket_t *sock);
socket_t *socket_from_fd(int fd);
int socket_close_fd(int fd);

// Family Registration
typedef struct net_family {
  int family;
  int (*create)(socket_t *sock, int protocol);
  struct net_family *next;
} net_family_t;

void sock_register_family(net_family_t *family);
net_family_t *sock_lookup_family(int family);

// Default Socket Buffer Sizes
#define SOCKET_DEFAULT_RCVBUF 262144
#define SOCKET_DEFAULT_SNDBUF 262144

// Ancillary Data Support
#define SCM_RIGHTS 0x01
#define SCM_CREDENTIALS 0x02

struct cmsghdr {
  size_t cmsg_len;   // Data byte count, including header
  int cmsg_level;    // Originating protocol
  int cmsg_type;     // Protocol-specific type
};

// Alignment macros for cmsg
#define CMSG_ALIGN(len) (((len) + sizeof(size_t) - 1) & (size_t) ~(sizeof(size_t) - 1))
#define CMSG_DATA(cmsg) ((unsigned char *)(cmsg) + CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_SPACE(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(len))
#define CMSG_LEN(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + (len))
#define CMSG_FIRSTHDR(mhdr) \
    ((size_t)(mhdr)->msg_controllen >= sizeof(struct cmsghdr) ? \
     (struct cmsghdr *)(mhdr)->msg_control : \
     (struct cmsghdr *)0)

#define CMSG_NXTHDR(mhdr, cmsg) \
    (((unsigned char *)(cmsg) + CMSG_ALIGN((cmsg)->cmsg_len) + \
      sizeof(struct cmsghdr) > \
      (unsigned char *)(mhdr)->msg_control + (mhdr)->msg_controllen) ? \
     (struct cmsghdr *)0 : \
     (struct cmsghdr *)((unsigned char *)(cmsg) + CMSG_ALIGN((cmsg)->cmsg_len)))

#endif // SOCKET_H
