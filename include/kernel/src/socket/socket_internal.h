#ifndef SOCKET_INTERNAL_H
#define SOCKET_INTERNAL_H

#include "../lib/list.h"
#include "../sched/wait.h"
#include "socket.h"
#include <stdint.h>

// Internal Socket Subsystem State

// Maximum number of sockets system-wide
#define SOCKET_MAX_COUNT 4096

// Socket allocation table
extern socket_t *socket_table[SOCKET_MAX_COUNT];
extern int socket_count;
extern spinlock_t socket_table_lock;

// Unix Domain Socket Internal Structure
// ────────────────────────────────────── This is the family-specific data
// pointed to by socket->sk for AF_UNIX

typedef struct unix_scm_msg {
  struct unix_scm_msg *next;
  size_t stream_offset;     // Stream byte offset where this SCM was sent
  int count;                // Number of FDs in this message
  struct vfs_node *nodes[]; // Dynamically allocated array of vfs_node_t*
} unix_scm_msg_t;

// Packet struct for SOCK_SEQPACKET and SOCK_DGRAM
typedef struct unix_packet {
  struct unix_packet *next;
  size_t data_len;
  unix_scm_msg_t *scm;      // Attached SCM rights, if any
  bool cred_pending;        // Attached SCM credentials, if any
  int cred_pid;
  int cred_uid;
  int cred_gid;
  uint8_t data[];           // Inline packet payload
} unix_packet_t;

typedef struct unix_sock {
  socket_t *parent;           // Pointer to the parent socket structure
  struct sockaddr_un addr;    // Bound address
  int addr_len;               // Address length
  struct list_head bind_node; // Node in the bound sockets list

  // Connection state
  struct unix_sock *peer;     // Connected peer
  struct unix_sock *listener; // Listening socket we're connecting to
  uint32_t owner_pid;
  uint32_t owner_uid;
  uint32_t owner_gid;
  int backlog;                // Listen backlog
  int accept_queue_len;       // Current accept queue length

  // Queues
  wait_queue_t *wait;            // Points to parent->wait_queue
  struct unix_sock *accept_next; // Next in accept queue


  uint8_t *recv_buf;
  size_t recv_buf_size;
  size_t recv_buf_head;
  size_t recv_buf_tail;
  spinlock_t recv_lock;

  // Packet queue for SOCK_SEQPACKET and SOCK_DGRAM
  unix_packet_t *packet_queue_head;
  unix_packet_t *packet_queue_tail;
  size_t packet_queue_bytes;

  // Send buffer
  uint8_t *send_buf;
  size_t send_buf_size;
  size_t send_buf_head;
  size_t send_buf_tail;
  spinlock_t send_lock;

  // State flags
  bool is_listener;
  bool is_accepted;
  bool is_abstract;       // Abstract namespace (not filesystem)
  bool read_shutdown;     // SHUT_RD - read side shutdown
  bool write_shutdown;    // SHUT_WR - write side shutdown
  bool orphaned;          // Client closed before accept - pending cleanup
  bool accepted_orphaned; // Accepted socket whose peer closed before accept
  bool was_connected;     // Ever reached SS_CONNECTED (survives peer teardown)


  bool passcred;   // SO_PASSCRED - pass credentials in recvmsg
  int rcvtimeo_ms; // SO_RCVTIMEO - receive timeout in ms
  int sndtimeo_ms; // SO_SNDTIMEO - send timeout in ms

  struct vfs_node *bound_vnode; // Filesystem socket inode, if bound to a path

  struct unix_scm_msg *scm_queue_head; // FIFO queue of pending SCM messages (for stream sockets)
  struct unix_scm_msg *scm_queue_tail;
  size_t bytes_written;                // Total stream bytes written into this socket's recv_buf
  size_t bytes_read;                   // Total stream bytes read from this socket's recv_buf
  bool scm_cred_pending;               // Sender credentials for SO_PASSCRED
  int scm_cred_pid;
  int scm_cred_uid;
  int scm_cred_gid;
} unix_sock_t;

// Socket Buffer (sk_buff-like structure)


typedef struct sk_buff {
  uint8_t *data;
  size_t len;
  size_t capacity;
  struct sk_buff *next;

  // Metadata for recvfrom
  uint32_t src_ip;
  uint16_t src_port;
} sk_buff_t;

// Socket Buffer Queue
typedef struct sk_buff_head {
  sk_buff_t *head;
  sk_buff_t *tail;
  size_t len;
  spinlock_t lock;
} sk_buff_head_t;

// AF_NETLINK Socket Internal Structure
typedef struct netlink_sock {
  socket_t *parent;
  int protocol;
  uint32_t groups;
  uintptr_t portid;
  bool passcred;            // SO_PASSCRED — attach SCM_CREDENTIALS in recvmsg
  sk_buff_head_t recv_queue;
  struct list_head list;
} netlink_sock_t;

// Internal Functions

// Socket table management
int socket_table_alloc(void);
void socket_table_free(int idx);
socket_t *socket_table_get(int idx);

// Unix domain socket internal functions
int unix_socket_create(socket_t *sock, int protocol);
void unix_socket_destroy(socket_t *sock);
int unix_socket_bind(socket_t *sock, struct sockaddr *addr, int addrlen);
int unix_socket_connect(socket_t *sock, struct sockaddr *addr, int addrlen);
int unix_socket_listen(socket_t *sock, int backlog);
int unix_socket_accept(socket_t *sock, socket_t **newsock);
ssize_t unix_socket_send(socket_t *sock, const void *buf, size_t len,
                         int flags);
ssize_t unix_socket_recv(socket_t *sock, void *buf, size_t len, int flags);

// Socket buffer management
sk_buff_t *alloc_skb(size_t size);
void free_skb(sk_buff_t *skb);
void skb_queue_tail(sk_buff_head_t *list, sk_buff_t *skb);
void skb_queue_head(sk_buff_head_t *list, sk_buff_t *skb);
sk_buff_t *skb_dequeue(sk_buff_head_t *list);
bool skb_queue_empty(sk_buff_head_t *list);

// Wait queue helpers for sockets
void socket_wait_queue_init(socket_t *sock);
void socket_wait(socket_t *sock);
void socket_wake(socket_t *sock);

// Socket info operations
int socket_getsockname(socket_t *sock, struct sockaddr *addr, int *addrlen);
int socket_getpeername(socket_t *sock, struct sockaddr *addr, int *addrlen);

// Socket File Operations
// ───────────────────────────────────────────────────── These integrate sockets
// with the VFS

uint32_t socket_vfs_read(struct vfs_node *node, uint32_t offset, uint32_t size,
                         uint8_t *buffer);
uint32_t socket_vfs_write(struct vfs_node *node, uint32_t offset, uint32_t size,
                          uint8_t *buffer);
void socket_vfs_open(struct vfs_node *node);
void socket_vfs_close(struct vfs_node *node);
int socket_vfs_poll(struct vfs_node *node, int events);
int socket_vfs_ioctl(struct vfs_node *node, uint32_t request, uint64_t arg);

// Socket State Helpers

static inline bool socket_is_connected(socket_t *sock) {
  return sock->state == SS_CONNECTED;
}

static inline bool socket_is_listening(socket_t *sock) {
  return sock->state == SS_LISTENING;
}

static inline bool socket_is_unconnected(socket_t *sock) {
  return sock->state == SS_UNCONNECTED;
}

static inline bool socket_is_nonblocking(socket_t *sock) {
  return (sock->flags & SOCK_NONBLOCK) != 0;
}

// Error Handling Helpers

static inline void socket_set_error(socket_t *sock, int error) {
  sock->error = error;
}

static inline int socket_get_error(socket_t *sock) {
  int err = sock->error;
  sock->error = 0;
  return err;
}

#endif // SOCKET_INTERNAL_H
