#include "fuse.h"
#include "fuse_protocol.h"
#include "devfs.h"
#include "mount_namespace.h"
#include "arch/uaccess.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../sched/sched.h"
#include "../sched/wait.h"
#include "../socket/epoll.h"

/* Initial native implementation: read-only, immutable user filesystems.
 * Userspace sees the real FUSE protocol on /dev/fuse, not extracted files.
 * Never hold the connection lock over user copies, allocation or RPC waits. */
#define FUSE_CHUNK 65536u
#define FUSE_MAX_REQUESTS 128u
#define FUSE_MAX_CONNECTIONS 128u
#define FUSE_INODE_MAGIC 0x46555345u
struct fuse_request {
  struct fuse_request *next;
  uint32_t refs, in_len, out_capacity, out_len, opcode;
  uint64_t unique;
  int error;
  bool sent, done, listed, background, no_reply;
  uint8_t *in, *out;
};
struct fuse_connection {
  spinlock_t lock;
  wait_queue_t readers, changed;
  struct fuse_request *requests, *tail;
  uint64_t next_unique, mount_flags;
  uint32_t refs, queued, nodes, uid, gid, max_read;
  bool mounted, disconnected, initialized, allow_other, detached;
  vfs_node_t *device;
};
struct fuse_inode {
  uint32_t magic;
  struct fuse_connection *conn;
  uint64_t nodeid, fh;
  uint32_t nlink;
  bool lookup, opening, opened, attrs_valid;
};
static const char *find_byte(const void *data, int byte, size_t length) {
  const unsigned char *p = data;
  for (size_t i = 0; i < length; i++) if (p[i] == (unsigned char)byte) return (const char *)p + i;
  return NULL;
}
static uint32_t connection_count;
static void conn_get(struct fuse_connection *c) {
  __atomic_add_fetch(&c->refs, 1, __ATOMIC_RELAXED);
}
static void conn_put(struct fuse_connection *c) {
  if (__atomic_sub_fetch(&c->refs, 1, __ATOMIC_ACQ_REL) == 0) {
    __atomic_sub_fetch(&connection_count, 1, __ATOMIC_RELAXED);
    kfree(c);
  }
}
static void req_put(struct fuse_request *r) {
  if (__atomic_sub_fetch(&r->refs, 1, __ATOMIC_ACQ_REL) == 0) {
    kfree(r->in); kfree(r->out); kfree(r);
  }
}
static void remove_locked(struct fuse_connection *c, struct fuse_request *r) {
  struct fuse_request **p = &c->requests, *prev = NULL;
  while (*p && *p != r) { prev = *p; p = &(*p)->next; }
  if (*p) {
    *p = r->next;
    if (c->tail == r) c->tail = prev;
    c->queued--; r->listed = false; r->next = NULL;
  }
}
static void notify(struct fuse_connection *c) {
  wait_queue_wake_all(&c->readers);
  wait_queue_wake_all(&c->changed);
  spinlock_acquire(&c->lock);
  vfs_node_t *dev = c->device;
  if (dev && !vfs_node_tryref(dev)) dev = NULL;
  spinlock_release(&c->lock);
  if (dev) { epoll_notify_event(dev, POLLIN | POLLOUT); vfs_close(dev); }
}
static void disconnect(struct fuse_connection *c) {
  struct fuse_request *garbage = NULL;
  spinlock_acquire(&c->lock);
  c->disconnected = true;
  for (struct fuse_request *r = c->requests, *next; r; r = next) {
    next = r->next; r->error = -107; r->done = true;
    if (r->background) {
      remove_locked(c, r); r->next = garbage; garbage = r;
    }
  }
  spinlock_release(&c->lock);
  notify(c);
  while (garbage) { struct fuse_request *next = garbage->next; req_put(garbage); garbage = next; }
}
/* Caller holds c->lock. Publish blocked state before dropping the predicate
 * lock so a reply/disconnect cannot be lost between checking and sleeping. */
static int sleep_locked(struct fuse_connection *c, wait_queue_t *wq) {
  struct thread *t = sched_get_current();
  if (!t || (t->pending_signals & ~t->signal_mask)) return -4;
  wait_queue_entry_t entry = {.thread = t};
  wait_queue_add(wq, &entry);
  t->state = THREAD_BLOCKED;
  if (t->pending_signals & ~t->signal_mask) {
    t->state = THREAD_RUNNING;
    wait_queue_remove(wq, &entry);
    return -4;
  }
  spinlock_release(&c->lock);
  sched_yield();
  wait_queue_remove(wq, &entry);
  t->state = THREAD_RUNNING;
  spinlock_acquire(&c->lock);
  return 0;
}
static struct fuse_request *request_new(uint32_t opcode, uint64_t nodeid,
                                        const void *in, uint32_t len,
                                        uint32_t capacity) {
  if (len > FUSE_CHUNK || capacity > FUSE_CHUNK) return NULL;
  struct fuse_request *r = kcalloc(1, sizeof(*r));
  if (!r) return NULL;
  r->refs = 1; r->opcode = opcode;
  r->in_len = sizeof(struct afuse_in_header) + len;
  r->in = kmalloc(r->in_len);
  r->out = capacity ? kmalloc(capacity) : NULL;
  r->out_capacity = capacity;
  if (!r->in || (capacity && !r->out)) { req_put(r); return NULL; }
  struct thread *t = sched_get_current();
  struct afuse_in_header h = {.len = r->in_len, .opcode = opcode, .nodeid = nodeid,
      .uid = t ? t->fsuid : 0, .gid = t ? t->fsgid : 0,
      .pid = t ? (t->tgid ? t->tgid : t->tid) : 0};
  memcpy(r->in, &h, sizeof(h));
  if (len) memcpy(r->in + sizeof(h), in, len);
  return r;
}
static int submit(struct fuse_connection *c, struct fuse_request *r) {
  spinlock_acquire(&c->lock);
  if (c->disconnected || c->queued >= FUSE_MAX_REQUESTS) {
    int err = c->disconnected ? -107 : -11;
    spinlock_release(&c->lock); return err;
  }
  r->unique = ++c->next_unique;
  ((struct afuse_in_header *)r->in)->unique = r->unique;
  r->listed = true;
  if (c->tail) c->tail->next = r; else c->requests = r;
  c->tail = r; c->queued++;
  spinlock_release(&c->lock);
  notify(c); return 0;
}
static int rpc(struct fuse_connection *c, uint32_t opcode, uint64_t nodeid,
                 const void *in, uint32_t len, void *out, uint32_t capacity) {
  struct fuse_request *r = request_new(opcode, nodeid, in, len, capacity);
  if (!r) return -12;
  int result = submit(c, r);
  if (result) { req_put(r); return result; }
  spinlock_acquire(&c->lock);
  while (!r->done && !c->disconnected) {
    result = sleep_locked(c, &c->changed);
    if (result) break;
  }
  if (!result) result = r->done ? r->error : -107;
  if (!result) {
    result = (int)r->out_len;
    if (r->out_len) memcpy(out, r->out, r->out_len);
  }
  remove_locked(c, r);
  spinlock_release(&c->lock);
  req_put(r); return result;
}
static void background(struct fuse_connection *c, uint32_t opcode, uint64_t nid,
                         const void *in, uint32_t len, bool no_reply) {
  struct fuse_request *r = request_new(opcode, nid, in, len, 0);
  if (!r) { disconnect(c); return; }
  r->background = true; r->no_reply = no_reply;
  int error = submit(c, r);
  if (error) {
    req_put(r);
    /* RELEASE/FORGET must not silently lose server-owned references. */
    if (error != -107) disconnect(c);
  }
}
static int ready(struct fuse_connection *c) {
  spinlock_acquire(&c->lock);
  int result = 0;
  while (!c->initialized && !c->disconnected) {
    result = sleep_locked(c, &c->changed);
    if (result) break;
  }
  if (c->disconnected) result = -107;
  struct thread *t = sched_get_current();
  if (!result && !c->allow_other && t && t->fsuid && t->fsuid != c->uid) result = -13;
  spinlock_release(&c->lock); return result;
}
static uint32_t device_read(vfs_node_t *node, uint32_t off, uint32_t size, uint8_t *buf) {
  (void)off;
  struct fuse_connection *c = node->device;
  spinlock_acquire(&c->lock);
  struct fuse_request *r = NULL;
  for (;;) {
    if (c->disconnected) { spinlock_release(&c->lock); return (uint32_t)-19; }
    for (r = c->requests; r && (r->sent || r->done); r = r->next) {}
    if (r) break;
    if (node->flags & FS_NONBLOCK) { spinlock_release(&c->lock); return (uint32_t)-11; }
    int err = sleep_locked(c, &c->readers);
    if (err) { spinlock_release(&c->lock); return (uint32_t)err; }
  }
  if (size < r->in_len) { spinlock_release(&c->lock); return (uint32_t)-22; }
  r->sent = true;
  __atomic_add_fetch(&r->refs, 1, __ATOMIC_RELAXED);
  spinlock_release(&c->lock);
  unsigned long failed = 0;
  if (is_user_ptr((uint64_t)buf)) failed = copy_to_user(buf, r->in, r->in_len);
  else memcpy(buf, r->in, r->in_len);
  uint32_t result = failed ? (uint32_t)-14 : r->in_len;
  bool drop = false;
  spinlock_acquire(&c->lock);
  if (r->listed && !r->done) {
    if (failed) r->sent = false;
    else if (r->no_reply) { remove_locked(c, r); drop = true; }
  }
  spinlock_release(&c->lock);
  if (failed) notify(c);
  if (drop) req_put(r);
  req_put(r); return result;
}
static uint32_t device_write(vfs_node_t *node, uint32_t off, uint32_t size, uint8_t *buf) {
  (void)off;
  if (size < sizeof(struct afuse_out_header) || size > FUSE_CHUNK + sizeof(struct afuse_out_header))
    return (uint32_t)-22;
  uint8_t *copy = kmalloc(size);
  if (!copy) return (uint32_t)-12;
  if (is_user_ptr((uint64_t)buf)) {
    if (copy_from_user(copy, buf, size)) { kfree(copy); return (uint32_t)-14; }
  } else memcpy(copy, buf, size);
  struct afuse_out_header h; memcpy(&h, copy, sizeof(h));
  if (h.len != size || !h.unique || h.error > 0 || h.error < -4095 ||
      (h.error && size != sizeof(h))) { kfree(copy); return (uint32_t)-22; }
  struct fuse_connection *c = node->device;
  spinlock_acquire(&c->lock);
  struct fuse_request *r = c->requests;
  while (r && r->unique != h.unique) r = r->next;
  int error = 0; bool drop = false, abort = false;
  if (c->disconnected) error = -19;
  else if (!r || !r->sent || r->done || r->no_reply) error = -2;
  else if (size - sizeof(h) > r->out_capacity) error = -22;
  else {
    r->error = h.error; r->out_len = size - sizeof(h);
    if (r->out_len) memcpy(r->out, copy + sizeof(h), r->out_len);
    if (r->opcode == AFUSE_INIT) {
      struct afuse_init_out init = {0};
      if (r->out_len >= 24) memcpy(&init, r->out, r->out_len < sizeof(init) ? r->out_len : sizeof(init));
      if (h.error || r->out_len < 24 || init.major != 7 || init.minor < 9 || init.minor > 26) abort = true;
      else c->initialized = true;
    }
    r->done = true;
    if (r->background) { remove_locked(c, r); drop = true; }
  }
  spinlock_release(&c->lock);
  kfree(copy);
  if (drop) req_put(r);
  if (abort) disconnect(c); else if (!error) notify(c);
  return error ? (uint32_t)error : size;
}
static int device_poll(vfs_node_t *node, int events) {
  struct fuse_connection *c = node->device;
  spinlock_acquire(&c->lock);
  int result = c->disconnected ? POLLERR | POLLHUP : events & POLLOUT;
  for (struct fuse_request *r = c->requests; r; r = r->next)
    if (!r->sent && !r->done) { result |= events & (POLLIN | POLLRDNORM); break; }
  spinlock_release(&c->lock); return result;
}
static void device_close(vfs_node_t *node) {
  struct fuse_connection *c = node->device;
  spinlock_acquire(&c->lock); c->device = NULL; spinlock_release(&c->lock);
  disconnect(c); conn_put(c);
}
static vfs_node_t *device_open(vfs_node_t *template) {
  (void)template;
  if (__atomic_add_fetch(&connection_count, 1, __ATOMIC_RELAXED) > FUSE_MAX_CONNECTIONS) {
    __atomic_sub_fetch(&connection_count, 1, __ATOMIC_RELAXED); return NULL;
  }
  struct fuse_connection *c = kcalloc(1, sizeof(*c));
  vfs_node_t *node = kmalloc(sizeof(*node));
  if (!c || !node) { kfree(c); kfree(node); __atomic_sub_fetch(&connection_count, 1, __ATOMIC_RELAXED); return NULL; }
  spinlock_init(&c->lock); wait_queue_init(&c->readers); wait_queue_init(&c->changed);
  c->refs = 1; c->max_read = FUSE_CHUNK; c->device = node;
  vfs_node_init(node); strcpy(node->name, "fuse");
  node->flags = FS_CHARDEV; node->mask = 0666; node->inode = (10u << 8) | 229u;
  node->refcount = 0; /* open_instance: sys_open_path installs the first ref. */
  node->device = c; node->read = device_read; node->write = device_write;
  node->close = device_close; node->poll = device_poll; node->wait_queue = &c->readers;
  return node;
}
static struct fuse_inode *inode_data(vfs_node_t *node) {
  struct fuse_inode *n = node ? node->device : NULL;
  return n && n->magic == FUSE_INODE_MAGIC ? n : NULL;
}
static int inode_open(vfs_node_t *node) {
  struct fuse_inode *n = inode_data(node);
  struct fuse_connection *c = n->conn;
  int err = ready(c); if (err) return err;
  spinlock_acquire(&c->lock);
  while (n->opening && !c->disconnected) {
    err = sleep_locked(c, &c->changed); if (err) break;
  }
  if (err || c->disconnected || n->opened) {
    if (c->disconnected) err = -107;
    spinlock_release(&c->lock); return err;
  }
  n->opening = true; spinlock_release(&c->lock);
  struct afuse_open_in in = {0}; struct afuse_open_out out;
  bool dir = (node->flags & FS_TYPE_MASK) == FS_DIRECTORY;
  err = rpc(c, dir ? AFUSE_OPENDIR : AFUSE_OPEN, n->nodeid, &in, sizeof(in), &out, sizeof(out));
  spinlock_acquire(&c->lock);
  if (err == sizeof(out)) { n->fh = out.fh; n->opened = true; err = 0; }
  else if (err >= 0) err = -5;
  n->opening = false; spinlock_release(&c->lock);
  wait_queue_wake_all(&c->changed); return err;
}
static void inode_close(vfs_node_t *node) {
  struct fuse_inode *n = inode_data(node);
  if (!n) return;
  if (n->opened) {
    struct afuse_release_in in = {.fh = n->fh};
    background(n->conn, (node->flags & FS_TYPE_MASK) == FS_DIRECTORY ? AFUSE_RELEASEDIR : AFUSE_RELEASE,
                 n->nodeid, &in, sizeof(in), false);
  }
  if (n->lookup) {
    struct afuse_forget_in in = {.nlookup = 1};
    background(n->conn, AFUSE_FORGET, n->nodeid, &in, sizeof(in), true);
  }
  uint32_t remaining = __atomic_sub_fetch(&n->conn->nodes, 1, __ATOMIC_ACQ_REL);
  spinlock_acquire(&n->conn->lock);
  bool last_detached = n->conn->detached && !remaining;
  spinlock_release(&n->conn->lock);
  if (last_detached) disconnect(n->conn);
  conn_put(n->conn); n->magic = 0; kfree(n);
}
static uint32_t inode_nlink(vfs_node_t *node) {
  struct fuse_inode *n = inode_data(node);
  if (!n->attrs_valid && !ready(n->conn)) {
    struct afuse_getattr_in in = {0}; struct afuse_attr_out out;
    int got = rpc(n->conn, AFUSE_GETATTR, n->nodeid, &in, sizeof(in), &out, sizeof(out));
    if (got == sizeof(out) && (out.attr.mode & 0170000) == 0040000 &&
        out.attr.size <= UINT32_MAX && out.attr.ino <= UINT32_MAX) {
      node->mask = out.attr.mode & 07777; node->uid = out.attr.uid; node->gid = out.attr.gid;
      node->length = out.attr.size; node->atime = out.attr.atime;
      node->mtime = out.attr.mtime; node->ctime = out.attr.ctime;
      n->nlink = out.attr.nlink; n->attrs_valid = true;
    }
  }
  return n->nlink;
}
static uint32_t file_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
  struct fuse_inode *n = inode_data(node);
  int err = inode_open(node); if (err) return (uint32_t)err;
  if (offset >= node->length) return 0;
  if (size > node->length - offset) size = node->length - offset;
  uint32_t done = 0;
  uint8_t *scratch = kmalloc(size < n->conn->max_read ? size : n->conn->max_read);
  if (!scratch && size) return (uint32_t)-12;
  while (done < size) {
    uint32_t count = size - done;
    if (count > n->conn->max_read) count = n->conn->max_read;
    struct afuse_read_in in = {.fh = n->fh, .offset = (uint64_t)offset + done, .size = count};
    err = rpc(n->conn, AFUSE_READ, n->nodeid, &in, sizeof(in), scratch, count);
    if (err <= 0) break;
    unsigned long failed = 0;
    if (is_user_ptr((uint64_t)buffer)) failed = copy_to_user(buffer + done, scratch, err);
    else memcpy(buffer + done, scratch, err);
    done += (uint32_t)err - (uint32_t)failed;
    if (failed) { err = -14; break; }
    if ((uint32_t)err < count) break;
  }
  kfree(scratch); return done ? done : err < 0 ? (uint32_t)err : 0;
}
static vfs_node_t *make_inode(struct fuse_connection *, uint64_t, const struct afuse_attr *, bool, const char *);
static vfs_node_t *finddir(vfs_node_t *node, char *name) {
  struct fuse_inode *n = inode_data(node);
  if (ready(n->conn)) return NULL;
  if (n->nodeid == 1) inode_nlink(node);
  size_t len = strlen(name);
  if (!len || len > 127 || strchr(name, '/')) return NULL;
  struct afuse_entry_out out;
  int got = rpc(n->conn, AFUSE_LOOKUP, n->nodeid, name, len + 1, &out, sizeof(out));
  if (got != sizeof(out) || !out.nodeid) return NULL;
  vfs_node_t *result = make_inode(n->conn, out.nodeid, &out.attr, true, name);
  if (!result) {
    struct afuse_forget_in in = {.nlookup = 1}; background(n->conn, AFUSE_FORGET, out.nodeid, &in, sizeof(in), true);
  }
  return result;
}
static int readlink(vfs_node_t *node, char *buffer, uint32_t size) {
  struct fuse_inode *n = inode_data(node);
  int err = ready(n->conn); if (err) return err;
  char *scratch = kmalloc(4096);
  if (!scratch) return -12;
  err = rpc(n->conn, AFUSE_READLINK, n->nodeid, NULL, 0, scratch, 4096);
  if (err > 0) { if ((uint32_t)err > size) err = size; memcpy(buffer, scratch, err); }
  kfree(scratch); return err;
}
static struct dirent *readdir(vfs_node_t *node, uint64_t index) {
  struct fuse_inode *n = inode_data(node);
  if (inode_open(node)) return NULL;
  uint8_t *data = kmalloc(8192);
  if (!data) return NULL;
  uint64_t cookie = 0;
  struct dirent *result = NULL;
  uint32_t seen = 0;
  for (;;) {
    struct afuse_read_in in = {.fh = n->fh, .offset = cookie, .size = 8192};
    int got = rpc(n->conn, AFUSE_READDIR, n->nodeid, &in, sizeof(in), data, 8192);
    if (got <= 0) break;
    uint32_t pos = 0; uint64_t previous = cookie;
    while (pos < (uint32_t)got) {
      if ((uint32_t)got - pos < sizeof(struct afuse_dirent)) goto out;
      struct afuse_dirent *d = (void *)(data + pos);
      if (!d->namelen || d->namelen > 127 || d->ino > UINT32_MAX ||
          d->namelen > (uint32_t)got - pos - sizeof(*d)) goto out;
      uint32_t record = (sizeof(*d) + d->namelen + 7u) & ~7u;
      if (record > (uint32_t)got - pos || find_byte(d->name, '/', d->namelen) || find_byte(d->name, 0, d->namelen)) goto out;
      if (seen++ == index) {
        result = vfs_readdir_buffer();
        memcpy(result->name, d->name, d->namelen); result->name[d->namelen] = 0;
        result->ino = d->ino; result->d_type = d->type;
        goto out;
      }
      cookie = d->off; pos += record;
    }
    if (cookie == previous || seen > 1000000) break;
  }
out:
  kfree(data); return result;
}
static int statfs(vfs_node_t *node, struct statfs_buf *out) {
  struct fuse_inode *n = inode_data(node);
  int err = ready(n->conn); if (err) return err;
  struct afuse_kstatfs st;
  err = rpc(n->conn, AFUSE_STATFS, n->nodeid, NULL, 0, &st, sizeof(st));
  if (err != sizeof(st)) return err < 0 ? err : -5;
  memset(out, 0, sizeof(*out)); out->f_type = 0x65735546;
  out->f_bsize = st.bsize; out->f_frsize = st.frsize;
  out->f_blocks = st.blocks; out->f_bfree = st.bfree; out->f_bavail = st.bavail;
  out->f_files = st.files; out->f_ffree = st.ffree; out->f_namelen = st.namelen; out->f_flags = n->conn->mount_flags & 15;
  return 0;
}
static vfs_node_t *make_inode(struct fuse_connection *c, uint64_t id,
                               const struct afuse_attr *attr, bool lookup, const char *name) {
  uint32_t type = attr->mode & 0170000;
  if ((type != 0100000 && type != 0040000 && type != 0120000) ||
      attr->size > UINT32_MAX || attr->ino > UINT32_MAX) return NULL;
  struct fuse_inode *n = kcalloc(1, sizeof(*n));
  vfs_node_t *node = kmalloc(sizeof(*node));
  if (!n || !node) { kfree(n); kfree(node); return NULL; }
  n->magic = FUSE_INODE_MAGIC; n->conn = c; n->nodeid = id; n->lookup = lookup; n->attrs_valid = lookup; n->nlink = attr->nlink;
  conn_get(c); __atomic_add_fetch(&c->nodes, 1, __ATOMIC_RELAXED);
  vfs_node_init(node); strncpy(node->name, name, 127);
  node->flags = FS_READONLY | FS_DENTRY_NOCACHE |
      (type == 0040000 ? FS_DIRECTORY : type == 0120000 ? FS_SYMLINK : FS_FILE);
  node->filesystem_mount_flags = c->mount_flags & 15;
  node->inode = attr->ino; node->mask = attr->mode & 07777;
  node->uid = attr->uid; node->gid = attr->gid; node->length = attr->size;
  node->atime = attr->atime; node->mtime = attr->mtime; node->ctime = attr->ctime;
  node->device = n; node->close = inode_close; node->statfs = statfs; node->get_nlink = inode_nlink;
  if (type == 0100000) node->read = file_read;
  if (type == 0040000) { node->finddir = finddir; node->readdir = readdir; }
  if (type == 0120000) node->readlink = readlink;
  return node;
}
static int option_number(const char *s, size_t len, uint32_t base, uint32_t *out) {
  if (!len) return -22;
  uint32_t value = 0;
  for (size_t i = 0; i < len; i++) {
    unsigned digit = (unsigned)(s[i] - '0');
    if (digit >= base || value > (UINT32_MAX - digit) / base) return -22;
    value = value * base + digit;
  }
  *out = value; return 0;
}
int fuse_mount_at(vfs_node_t *point, const char *source, const char *type,
                    const char *path, uint64_t flags, const char *options) {
  if (vfs_namespace_active()) return -95;
  if (!options || (point->flags & FS_TYPE_MASK) != FS_DIRECTORY) return -22;
  if (!(flags & 1)) return -30; /* Writable mounts are not implemented. */
  uint32_t fd = UINT32_MAX, uid = UINT32_MAX, gid = UINT32_MAX, rootmode = 0040755, max_read = FUSE_CHUNK;
  bool allow_other = false;
  for (const char *p = options; *p;) {
    const char *end = strchr(p, ','); if (!end) end = p + strlen(p);
    const char *equal = find_byte(p, '=', end - p);
    size_t key = equal ? (size_t)(equal - p) : (size_t)(end - p);
    uint32_t *dest = NULL, base = 10;
    if (key == 2 && !memcmp(p, "fd", 2)) dest = &fd;
    else if (key == 7 && !memcmp(p, "user_id", 7)) dest = &uid;
    else if (key == 8 && !memcmp(p, "group_id", 8)) dest = &gid;
    else if (key == 8 && !memcmp(p, "rootmode", 8)) { dest = &rootmode; base = 8; }
    else if (key == 8 && !memcmp(p, "max_read", 8)) dest = &max_read;
    else if (!equal && key == 11 && !memcmp(p, "allow_other", 11)) allow_other = true;
    else if (!equal && key == 19 && !memcmp(p, "default_permissions", 19)) {}
    else return -22;
    if (dest && (!equal || option_number(equal + 1, end - equal - 1, base, dest))) return -22;
    p = *end ? end + 1 : end;
  }
  if (fd == UINT32_MAX || uid == UINT32_MAX || gid == UINT32_MAX ||
      (rootmode & 0170000) != 0040000 || !max_read) return -22;
  struct thread *t = sched_get_current();
  if (!t || !t->files) return -9;
  spinlock_acquire(&t->files->lock);
  vfs_node_t *dev = fd < t->files->capacity ? t->files->fds[fd] : NULL;
  if (!dev || dev->read != device_read) dev = NULL;
  else vfs_node_ref(dev);
  spinlock_release(&t->files->lock);
  if (!dev) return -9;
  struct fuse_connection *c = dev->device;
  spinlock_acquire(&c->lock);
  int err = c->mounted || c->disconnected ? -16 : 0;
  if (!err) {
    c->mounted = true; c->mount_flags = flags; c->uid = uid; c->gid = gid; c->allow_other = allow_other;
    c->max_read = max_read < FUSE_CHUNK ? max_read : FUSE_CHUNK;
  }
  spinlock_release(&c->lock);
  if (err) { vfs_close(dev); return err; }
  struct afuse_attr attr = {.ino = 1, .mode = 0040755, .nlink = 2, .uid = uid, .gid = gid};
  vfs_node_t *root = make_inode(c, 1, &attr, false, "fuse-root");
  struct afuse_init_in init = {.major = 7, .minor = 26, .max_readahead = FUSE_CHUNK};
  struct fuse_request *request = request_new(AFUSE_INIT, 1, &init, sizeof(init), sizeof(struct afuse_init_out));
  if (!root || !request) err = -12;
  else { request->background = true; err = submit(c, request); if (!err) request = NULL; }
  if (!err) {
    if (vfs_namespace_active()) err = vfs_namespace_mount(point, root, source, type, path, flags);
    else err = vfs_mount_ex(point, root, source, type, path);
  }
  if (request) req_put(request);
  if (err) disconnect(c);
  if (root && (err || vfs_namespace_active())) vfs_close(root);
  vfs_close(dev); return err;
}
bool fuse_is_device(vfs_node_t *node) {
  return node && node->write == device_write && node->read == device_read;
}
void fuse_abort_device(vfs_node_t *node) {
  if (fuse_is_device(node)) disconnect(node->device);
}
bool fuse_is_root(vfs_node_t *node) {
  node = vfs_backing_node(node);
  /* Check the callback before interpreting another filesystem's device. */
  return node && node->close == inode_close && inode_data(node)->nodeid == 1;
}
bool fuse_mount_busy(vfs_node_t *node) {
  node = vfs_backing_node(node);
  return fuse_is_root(node) && __atomic_load_n(&inode_data(node)->conn->nodes, __ATOMIC_ACQUIRE) > 1;
}
void fuse_detach_mount(vfs_node_t *node) {
  node = vfs_backing_node(node);
  if (!fuse_is_root(node)) return;
  struct fuse_connection *c = inode_data(node)->conn;
  spinlock_acquire(&c->lock); c->detached = true; spinlock_release(&c->lock);
  /* The resolving caller still pins the root. Its last close, or the last
   * open inode/mapping on a lazy detach, disconnects the daemon. */
}
void fuse_init(void) {
  vfs_node_t *node = kmalloc(sizeof(*node));
  if (!node) return;
  vfs_node_init(node); strcpy(node->name, "fuse");
  node->flags = FS_CHARDEV | FS_PERSISTENT; node->mask = 0666;
  node->inode = (10u << 8) | 229u; node->open_instance = device_open;
  devfs_register_node("fuse", node);
}
