// sys_mount_api.c — new mount API (fsopen/fsconfig/fsmount/move_mount)
//
// systemd >= 258 assumes a 5.14 kernel and builds per-unit credential
// directories with this API only (fsopen("tmpfs") ... move_mount), with no
// mount(2) fallback; every service with credentials failed at step
// CREDENTIALS with ENOSYS.  Only in-memory filesystems are supported: other
// types, open_tree and fspick keep answering ENOSYS so callers that do have a
// legacy fallback (libmount, systemd's mount helpers) keep using mount(2).
#include "sys_io_shared.h"
#include "../console/klog.h"
#include "../fs/mount_namespace.h"
#include "../fs/tmpfs.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../sched/capability.h"
#include "../sched/sched.h"
#include "syscall.h"
#include <stdint.h>

#define FSOPEN_CLOEXEC 0x1
#define FSMOUNT_CLOEXEC 0x1

#define FSCONFIG_SET_FLAG 0
#define FSCONFIG_SET_STRING 1
#define FSCONFIG_SET_BINARY 2
#define FSCONFIG_SET_PATH 3
#define FSCONFIG_SET_PATH_EMPTY 4
#define FSCONFIG_SET_FD 5
#define FSCONFIG_CMD_CREATE 6
#define FSCONFIG_CMD_RECONFIGURE 7
#define FSCONFIG_CMD_CREATE_EXCL 8

#define MOVE_MOUNT_F_EMPTY_PATH 0x00000004
#define MOVE_MOUNT_T_EMPTY_PATH 0x00000040

/* MOUNT_ATTR_RDONLY/NOSUID/NODEV/NOEXEC share MS_*'s low bits, the same
 * encoding filesystem_mount_flags uses. */
#define MOUNT_ATTR_MASK 0xfULL

typedef struct {
  char type[16];
  bool created;
  bool mounted;
  uint32_t mode;
  uint32_t uid, gid;
  bool have_mode, have_uid, have_gid;
  uint64_t max_bytes, max_inodes;
  int pending_ro; /* -1 unset, 0 rw, 1 ro: applied by CMD_RECONFIGURE */
  vfs_node_t *root; /* owned once created */
  spinlock_t lock;
} fs_context_t;

/* Mounts made by fsmount() but not yet attached by move_mount().  Entries
 * are weak: they are only compared against a node pinned through the
 * caller's fd, never dereferenced. */
#define DETACHED_MAX 32
static struct {
  vfs_node_t *root;
  char type[16];
} detached[DETACHED_MAX];
static unsigned detached_next;
static spinlock_t detached_lock = SPINLOCK_INIT;

static bool fs_type_supported(const char *type) {
  return strcmp(type, "tmpfs") == 0 || strcmp(type, "ramfs") == 0;
}

static bool fs_context_node(vfs_node_t *node);

static int fs_context_statfs(vfs_node_t *node, struct statfs_buf *buf) {
  (void)node;
  if (!buf)
    return -1;
  memset(buf, 0, sizeof(*buf));
  buf->f_type = 0x09041934; /* ANON_INODE_FS_MAGIC */
  buf->f_bsize = 4096;
  buf->f_namelen = 255;
  return 0;
}

/* Reading an fs context returns queued kernel messages; none are kept, so
 * it is always empty. libmount drains it after every fsconfig() until it
 * sees ENODATA, and spins forever on a 0-byte read. */
static uint32_t fs_context_read(vfs_node_t *node, uint32_t offset,
                                uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;
  (void)size;
  (void)buffer;
  return (uint32_t)-61; // ENODATA
}

static void fs_context_close(vfs_node_t *node) {
  fs_context_t *ctx = node->device;
  if (!ctx)
    return;
  if (ctx->root)
    vfs_close(ctx->root);
  kfree(ctx);
  node->device = NULL;
}

static bool fs_context_node(vfs_node_t *node) {
  return node && node->close == fs_context_close && node->device;
}

static int fd_install_node(struct thread *t, vfs_node_t *node, bool cloexec,
                           uint64_t open_flags, const char *path) {
  int fd = alloc_fd(t);
  if (fd < 0)
    return -24; // EMFILE
  spinlock_acquire(&t->files->lock);
  t->fd_offsets[fd] = 0;
  t->fd_flags[fd] = open_flags | (cloexec ? FD_FLAGS_CLOEXEC_BIT : 0);
  if (!fd_path_set(t, fd, path)) {
    spinlock_release(&t->files->lock);
    fd_release_reserved(t, fd);
    return -12;
  }
  t->fds[fd] = node;
  spinlock_release(&t->files->lock);
  return fd;
}

static uint64_t sys_fsopen(uint64_t fstype_ptr, uint64_t flags, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2; (void)a3; (void)a4; (void)a5;
  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;
  if (flags & ~(uint64_t)FSOPEN_CLOEXEC)
    return (uint64_t)-22;
  if (!capability_has(t, CAP_SYS_ADMIN))
    return (uint64_t)-1;
  char type[16];
  if (!fstype_ptr || !is_user_range((void *)fstype_ptr, 1))
    return (uint64_t)-14;
  long n = strncpy_from_user(type, (void *)fstype_ptr, sizeof(type));
  if (n < 0)
    return (uint64_t)-14;
  if ((uint64_t)n >= sizeof(type))
    return (uint64_t)-19; // ENODEV: no such filesystem type
  if (!fs_type_supported(type))
    return (uint64_t)-38; // see file comment: keep legacy fallbacks alive

  fs_context_t *ctx = kmalloc(sizeof(*ctx));
  vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
  if (!ctx || !node) {
    if (ctx) kfree(ctx);
    if (node) kfree(node);
    return (uint64_t)-12;
  }
  memset(ctx, 0, sizeof(*ctx));
  strcpy(ctx->type, type);
  ctx->pending_ro = -1;
  spinlock_init(&ctx->lock);
  vfs_node_init(node);
  strcpy(node->name, "fscontext");
  node->flags = FS_FILE;
  node->mask = 0600;
  node->device = ctx;
  node->close = fs_context_close;
  node->read = fs_context_read;
  node->statfs = fs_context_statfs;

  int fd = fd_install_node(t, node, flags & FSOPEN_CLOEXEC, 2 /* O_RDWR */,
                           "anon_inode:[fscontext]");
  if (fd < 0) {
    vfs_close(node);
    return (uint64_t)fd;
  }
  return (uint64_t)fd;
}

/* tmpfs size=: bytes with an optional k/m/g suffix. Percentages fall back
 * to the default limit. */
static uint64_t parse_size(const char *s, bool *ok) {
  uint64_t v = 0;
  const char *p = s;
  if (*p < '0' || *p > '9') {
    *ok = false;
    return 0;
  }
  while (*p >= '0' && *p <= '9')
    v = v * 10 + (uint64_t)(*p++ - '0');
  switch (*p) {
  case 'k': case 'K': v <<= 10; p++; break;
  case 'm': case 'M': v <<= 20; p++; break;
  case 'g': case 'G': v <<= 30; p++; break;
  case '%': *ok = false; return 0;
  default: break;
  }
  *ok = *p == '\0';
  return v;
}

static void fs_context_set(fs_context_t *ctx, const char *key,
                           const char *value) {
  bool ok = true;
  if (strcmp(key, "ro") == 0 || strcmp(key, "rw") == 0) {
    ctx->pending_ro = key[1] == 'o';
    return;
  }
  if (!value)
    return;
  if (strcmp(key, "mode") == 0) {
    uint32_t m = 0;
    for (const char *p = value; *p >= '0' && *p <= '7'; p++)
      m = m * 8 + (uint32_t)(*p - '0');
    ctx->mode = m & 07777;
    ctx->have_mode = true;
  } else if (strcmp(key, "size") == 0) {
    uint64_t v = parse_size(value, &ok);
    if (ok && v)
      ctx->max_bytes = v;
  } else if (strcmp(key, "nr_inodes") == 0) {
    uint64_t v = parse_size(value, &ok);
    if (ok && v)
      ctx->max_inodes = v;
  } else if (strcmp(key, "uid") == 0 || strcmp(key, "gid") == 0) {
    uint32_t id = 0;
    for (const char *p = value; *p >= '0' && *p <= '9'; p++)
      id = id * 10 + (uint32_t)(*p - '0');
    if (key[0] == 'u') {
      ctx->uid = id;
      ctx->have_uid = true;
    } else {
      ctx->gid = id;
      ctx->have_gid = true;
    }
  }
  /* source, noswap, huge, ...: accepted, nothing to configure. */
}

static int fs_context_create(fs_context_t *ctx) {
  tmpfs_sb_t *sb = kmalloc(sizeof(tmpfs_sb_t));
  if (!sb)
    return -12;
  memset(sb, 0, sizeof(*sb));
  sb->max_bytes = ctx->max_bytes ? ctx->max_bytes : TMPFS_DEFAULT_MAX_BYTES;
  sb->max_inodes = ctx->max_inodes ? ctx->max_inodes : TMPFS_DEFAULT_MAX_INODES;
  spinlock_init(&sb->lock);
  vfs_node_t *root = tmpfs_create_root(sb);
  if (!root) {
    kfree(sb);
    return -12;
  }
  strncpy(root->name, ctx->type, sizeof(root->name) - 1);
  root->mask = ctx->have_mode ? ctx->mode : 01777;
  if (ctx->have_uid)
    root->uid = ctx->uid;
  if (ctx->have_gid)
    root->gid = ctx->gid;
  ctx->root = root;
  ctx->created = true;
  return 0;
}

static uint64_t sys_fsconfig(uint64_t fd, uint64_t cmd, uint64_t key_ptr,
                             uint64_t value_ptr, uint64_t aux, uint64_t a5) {
  (void)aux; (void)a5;
  struct thread *t = sched_get_current();
  vfs_node_t *node = fd_node_get(t, fd);
  if (!node)
    return (uint64_t)-9;
  if (!fs_context_node(node)) {
    fd_node_put(&node);
    return (uint64_t)-22;
  }
  fs_context_t *ctx = node->device;

  char key[64] = {0}, value[256] = {0};
  bool have_value = false;
  if (cmd <= FSCONFIG_SET_FD) {
    if (!key_ptr || !is_user_range((void *)key_ptr, 1)) {
      fd_node_put(&node);
      return (uint64_t)-22;
    }
    long n = strncpy_from_user(key, (void *)key_ptr, sizeof(key));
    if (n < 0 || (uint64_t)n >= sizeof(key)) {
      fd_node_put(&node);
      return (uint64_t)(n < 0 ? -14 : -22);
    }
  }
  if (cmd == FSCONFIG_SET_STRING) {
    if (!value_ptr || !is_user_range((void *)value_ptr, 1)) {
      fd_node_put(&node);
      return (uint64_t)-22;
    }
    long n = strncpy_from_user(value, (void *)value_ptr, sizeof(value));
    if (n < 0 || (uint64_t)n >= sizeof(value)) {
      fd_node_put(&node);
      return (uint64_t)(n < 0 ? -14 : -22);
    }
    have_value = true;
  }

  int64_t ret = 0;
  spinlock_acquire(&ctx->lock);
  switch (cmd) {
  case FSCONFIG_SET_FLAG:
  case FSCONFIG_SET_STRING:
    /* After CMD_CREATE parameters are staged for CMD_RECONFIGURE; systemd
     * stages "ro" this way once the credentials are written. */
    fs_context_set(ctx, key, have_value ? value : NULL);
    break;
  case FSCONFIG_SET_BINARY:
  case FSCONFIG_SET_PATH:
  case FSCONFIG_SET_PATH_EMPTY:
  case FSCONFIG_SET_FD:
    ret = -22; // no in-memory filesystem takes these
    break;
  case FSCONFIG_CMD_CREATE:
  case FSCONFIG_CMD_CREATE_EXCL:
    ret = ctx->created ? -16 : fs_context_create(ctx);
    break;
  case FSCONFIG_CMD_RECONFIGURE:
    if (!ctx->created) {
      ret = -22;
    } else if (ctx->pending_ro >= 0) {
      /* Superblock read-only: tmpfs files carry the policy of the root. */
      if (ctx->pending_ro)
        ctx->root->filesystem_mount_flags |= 1;
      else
        ctx->root->filesystem_mount_flags &= ~1ULL;
      ctx->pending_ro = -1;
    }
    break;
  default:
    ret = -95;
    break;
  }
  spinlock_release(&ctx->lock);
  fd_node_put(&node);
  return (uint64_t)ret;
}

static uint64_t sys_fsmount(uint64_t fsfd, uint64_t flags, uint64_t attr_flags,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3; (void)a4; (void)a5;
  struct thread *t = sched_get_current();
  if (flags & ~(uint64_t)FSMOUNT_CLOEXEC)
    return (uint64_t)-22;
  /* Propagation and atime attributes are accepted and not tracked. */
  vfs_node_t *node = fd_node_get(t, fsfd);
  if (!node)
    return (uint64_t)-9;
  if (!fs_context_node(node)) {
    fd_node_put(&node);
    return (uint64_t)-22;
  }
  fs_context_t *ctx = node->device;
  spinlock_acquire(&ctx->lock);
  if (!ctx->created || ctx->mounted) {
    spinlock_release(&ctx->lock);
    fd_node_put(&node);
    return (uint64_t)(ctx->mounted ? -16 : -22);
  }
  vfs_node_t *root = ctx->root;
  ctx->mounted = true;
  root->filesystem_mount_flags =
      (root->filesystem_mount_flags & ~MOUNT_ATTR_MASK) |
      (attr_flags & MOUNT_ATTR_MASK);
  vfs_open(root); /* reference held by the new fd */
  spinlock_release(&ctx->lock);

  spinlock_acquire(&detached_lock);
  unsigned slot = detached_next++ % DETACHED_MAX;
  detached[slot].root = root;
  strcpy(detached[slot].type, ctx->type);
  spinlock_release(&detached_lock);
  fd_node_put(&node);

  int fd = fd_install_node(t, root, flags & FSMOUNT_CLOEXEC,
                           O_PATH | O_DIRECTORY, "anon_inode:[mount]");
  if (fd < 0) {
    vfs_close(root);
    return (uint64_t)fd;
  }
  return (uint64_t)fd;
}

/* Absolute mountpoint path for the mount table (/proc/self/mountinfo). */
static bool mount_target_path(struct thread *t, int dfd, const char *path,
                              char *out, size_t size) {
  const char *base = NULL;
  if (path[0] == '/') {
    base = "";
  } else if (dfd == AT_FDCWD) {
    base = t->cwd_path;
  } else {
    spinlock_acquire(&t->files->lock);
    const char *v = FD_IN_RANGE(t, dfd) ? fd_path_value(t, dfd) : NULL;
    if (v && v[0] == '/') {
      strncpy(out, v, size - 1);
      out[size - 1] = '\0';
    } else {
      out[0] = '\0';
    }
    spinlock_release(&t->files->lock);
    if (!out[0])
      return false;
    char tmp[256];
    strncpy(tmp, out, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    int n = path[0] ? snprintf(out, size, "%s/%s", tmp, path)
                    : snprintf(out, size, "%s", tmp);
    return n > 0 && (size_t)n < size;
  }
  int n = path[0] ? snprintf(out, size, "%s%s%s", base,
                             (base[0] && base[strlen(base) - 1] != '/') ? "/" : "",
                             path)
                  : snprintf(out, size, "%s", base);
  return n > 0 && (size_t)n < size;
}

static uint64_t sys_move_mount(uint64_t from_dfd, uint64_t from_path_ptr,
                               uint64_t to_dfd, uint64_t to_path_ptr,
                               uint64_t flags, uint64_t a5) {
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || !capability_has(t, CAP_SYS_ADMIN))
    return (uint64_t)-1;
  char from_path[256], to_path[256];
  if (!from_path_ptr || !to_path_ptr ||
      !is_user_range((void *)from_path_ptr, 1) ||
      !is_user_range((void *)to_path_ptr, 1))
    return (uint64_t)-14;
  long n = strncpy_from_user(from_path, (void *)from_path_ptr, sizeof(from_path));
  if (n < 0) return (uint64_t)-14;
  if ((uint64_t)n >= sizeof(from_path)) return (uint64_t)-36;
  n = strncpy_from_user(to_path, (void *)to_path_ptr, sizeof(to_path));
  if (n < 0) return (uint64_t)-14;
  if ((uint64_t)n >= sizeof(to_path)) return (uint64_t)-36;

  /* Only attaching an fsmount() result is supported; moving an existing
   * mount by path keeps the ENOSYS that sends callers to mount(MS_MOVE). */
  if (!(flags & MOVE_MOUNT_F_EMPTY_PATH) || from_path[0] || (int)from_dfd < 0)
    return (uint64_t)-38;
  vfs_node_t *root = fd_node_get(t, from_dfd);
  if (!root)
    return (uint64_t)-9;
  char type[16] = {0};
  spinlock_acquire(&detached_lock);
  for (unsigned i = 0; i < DETACHED_MAX; i++)
    if (detached[i].root == root) {
      strcpy(type, detached[i].type);
      detached[i].root = NULL;
      break;
    }
  spinlock_release(&detached_lock);
  if (!type[0]) {
    fd_node_put(&root);
    return (uint64_t)-38;
  }

  vfs_node_t *point;
  if (flags & MOVE_MOUNT_T_EMPTY_PATH) {
    if (to_path[0] || (int)to_dfd == AT_FDCWD) {
      fd_node_put(&root);
      return (uint64_t)-22;
    }
    point = fd_node_get(t, to_dfd);
  } else {
    if (!to_path[0]) {
      fd_node_put(&root);
      return (uint64_t)-2;
    }
    vfs_node_t *base = NULL;
    if (to_path[0] != '/' && (int)to_dfd != AT_FDCWD)
      base = fd_node_get(t, to_dfd);
    if (to_path[0] != '/' && (int)to_dfd != AT_FDCWD && !base) {
      fd_node_put(&root);
      return (uint64_t)-9;
    }
    vfs_node_t *dir = base ? base : (to_path[0] == '/' ? fs_root
                                     : (t->cwd_node ? t->cwd_node : fs_root));
    point = vfs_resolve_path_at(dir, to_path);
    if (base)
      fd_node_put(&base);
  }
  if (!point) {
    fd_node_put(&root);
    return (uint64_t)-2;
  }
  if ((point->flags & FS_TYPE_MASK) != FS_DIRECTORY) {
    vfs_close(point);
    fd_node_put(&root);
    return (uint64_t)-20;
  }

  char target[256];
  if (!mount_target_path(t, (int)to_dfd, to_path, target, sizeof(target)))
    target[0] = '\0';

  int result;
  if (vfs_namespace_active()) {
    result = vfs_namespace_mount(point, root, type, type,
                                 target[0] ? target : NULL,
                                 root->filesystem_mount_flags & MOUNT_ATTR_MASK);
    fd_node_put(&root);
  } else {
    /* The initial mount table keeps the reference it is given. */
    result = vfs_mount_ex(point, root, type, type, target[0] ? target : NULL);
    if (result)
      fd_node_put(&root);
  }
  vfs_close(point);
  if (result) {
    /* Leave the mount attachable for a retry. */
    spinlock_acquire(&detached_lock);
    unsigned slot = detached_next++ % DETACHED_MAX;
    detached[slot].root = root;
    strcpy(detached[slot].type, type);
    spinlock_release(&detached_lock);
    return (uint64_t)(result < 0 ? result : -12);
  }
  return 0;
}

void syscall_register_mount_api(void) {
  syscall_register(SYS_FSOPEN, sys_fsopen);
  syscall_register(SYS_FSCONFIG, sys_fsconfig);
  syscall_register(SYS_FSMOUNT, sys_fsmount);
  syscall_register(SYS_MOVE_MOUNT, sys_move_mount);
}
