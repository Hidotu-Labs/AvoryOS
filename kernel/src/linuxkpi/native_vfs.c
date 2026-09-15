/* Native side of the LinuxKPI VFS/file bridge.
 *
 * Creates native vfs_node descriptors whose operations forward into the
 * Linux-API file bridge (kernel/linuxkpi/src/file.c), and exposes the native
 * fd table through the small opaque API in linuxkpi/native_vfs.h.
 *
 * This file is native: it must never include Linux headers. */

#include "sched/sched.h"
#include "sched/wait.h"
#include "fs/devfs.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "mm/heap.h"

#include <linuxkpi/native_vfs.h>

/* O_CLOEXEC lives in uapi/linux/fcntl.h and has this value on x86_64; the
 * native fd_flags CLOEXEC marker is internal to the syscall layer. */
#define KPI_O_CLOEXEC 0x80000u
#define KPI_FD_FLAGS_CLOEXEC_BIT (1u << 24)

/* Same reservation marker as src/syscalls/sys_io_shared.h: alloc_fd() parks
 * this in the slot so a second allocation cannot take it. */
#define KPI_FD_RESERVED ((vfs_node_t *)-1)

/* ── node callbacks: forward to the Linux bridge ────────────────────────── */

static uint32_t kpi_node_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                              uint8_t *buffer) {
  return linuxkpi_file_read(node, offset, size, buffer);
}

static uint32_t kpi_node_write(vfs_node_t *node, uint32_t offset, uint32_t size,
                               uint8_t *buffer) {
  return linuxkpi_file_write(node, offset, size, buffer);
}

static int kpi_node_ioctl(vfs_node_t *node, uint32_t request, uint64_t arg) {
  return linuxkpi_file_ioctl(node, request, arg);
}

static uint64_t kpi_node_mmap(vfs_node_t *node, uint64_t addr, uint64_t length,
                              uint64_t prot, uint64_t flags, uint64_t offset) {
  return linuxkpi_file_mmap(node, addr, length, prot, flags, offset);
}

static int kpi_node_poll(vfs_node_t *node, int events) {
  return linuxkpi_file_poll(node, events);
}

static void kpi_node_close(vfs_node_t *node) {
  linuxkpi_file_close(node);
}

/* ── node allocation ────────────────────────────────────────────────────── */

void *asc_vfs_anon_node(void) {
  vfs_node_t *node = kmalloc(sizeof(*node));

  if (!node)
    return NULL;
  vfs_node_init(node);
  node->flags = FS_FILE;
  node->mask = 0666;
  node->read = kpi_node_read;
  node->write = kpi_node_write;
  node->ioctl = kpi_node_ioctl;
  node->mmap = kpi_node_mmap;
  node->poll = kpi_node_poll;
  node->close = kpi_node_close;
  node->device = NULL;
  return node;
}

void asc_vfs_node_set_name(void *node, const char *name) {
  vfs_node_t *n = node;

  if (!n || !name)
    return;
  strncpy(n->name, name, sizeof(n->name) - 1);
  n->name[sizeof(n->name) - 1] = '\0';
}

void asc_vfs_node_set_device(void *node, void *file) {
  vfs_node_t *n = node;

  if (n)
    n->device = file;
}

void *asc_vfs_node_device(void *node) {
  vfs_node_t *n = node;

  return n ? n->device : NULL;
}

__UINT32_TYPE__ asc_vfs_node_inode(void *node) {
  vfs_node_t *n = node;

  return n ? n->inode : 0;
}

void asc_vfs_node_set_wait_queue(void *node, void *wq) {
  vfs_node_t *n = node;

  if (n)
    n->wait_queue = wq;
}

void asc_vfs_node_ref(void *node) {
  if (node)
    vfs_node_ref((vfs_node_t *)node);
}

void asc_vfs_node_unref(void *node) {
  if (node)
    vfs_close((vfs_node_t *)node);
}

void asc_vfs_node_release_kernel(void *node) {
  vfs_node_t *n = node;

  if (!n)
    return;
  /* An fd-owned node is torn down by vfs_close(); when the node's close
   * callback (and through it fput()) reaches here, the refcount is already 0
   * and vfs_close() is about to kfree the node itself.  Leave it alone. */
  if (__atomic_load_n(&n->refcount, __ATOMIC_ACQUIRE) == 0)
    return;

  /* Detach the file before dropping the last reference: the node's close
   * callback would otherwise fput() the file whose free path called us. */
  n->close = NULL;
  n->device = NULL;
  asc_vfs_node_unref(n);
}

/* ── /dev registration for Linux-API devices ────────────────────────────── */

/* The metadata node stores the open callback in ->device; open_instance is
 * only invoked for opens of the registered metadata node.  The fresh node is
 * returned to sys_open with refcount 0 so the descriptor owns the initial
 * reference (the same convention as rfkill/driverctl). */
static vfs_node_t *kpi_devnode_open_instance(vfs_node_t *metadata) {
  void *(*open_fn)(void *) = (void *(*)(void *))metadata->device;
  vfs_node_t *node;

  if (!open_fn)
    return NULL;
  node = (vfs_node_t *)open_fn(metadata);
  if (node) {
    /* The descriptor stands for the registered node, so fstat() must report
     * the same type and dev_t.  Open callbacks build the instance with
     * asc_vfs_anon_node(), which leaves it a plain FS_FILE with inode 0;
     * libdrm identifies a DRM node exactly that way (fstat -> S_ISCHR +
     * st_rdev -> drmGetDevice2), so without this Mesa's loader sees no
     * amdgpu device on /dev/dri/renderD129 and falls back to llvmpipe. */
    node->flags = (node->flags & ~FS_TYPE_MASK) |
                  (metadata->flags & FS_TYPE_MASK);
    node->inode = metadata->inode;
    node->refcount = 0;
  }
  return node;
}

int asc_vfs_register_devnode(const char *name, void *(*open_fn)(void *)) {
  vfs_node_t *node;

  if (!name || !open_fn)
    return -22; /* EINVAL */

  node = kmalloc(sizeof(*node));
  if (!node)
    return -12; /* ENOMEM */
  vfs_node_init(node);
  strncpy(node->name, name, sizeof(node->name) - 1);
  node->name[sizeof(node->name) - 1] = '\0';
  node->flags = FS_CHARDEV | FS_PERSISTENT;
  node->mask = 0666;
  node->device = (void *)open_fn;
  node->open_instance = kpi_devnode_open_instance;
  devfs_register_node(name, node);
  return 0;
}

/* ── dynamic nested devnodes (e.g. /dev/dri/card1) ──────────────────────── */

#define KPI_DEVNODE_REGISTRY_MAX 32

struct kpi_path_devnode {
  char dir[32];
  char name[64];
  uint32_t rdev;
  vfs_node_t *metadata;
};

static struct kpi_path_devnode kpi_path_devnodes[KPI_DEVNODE_REGISTRY_MAX];
static int kpi_path_devnode_count;

/* The native `inode` word is reported verbatim as st_rdev by fill_kstat(), so
 * it must carry the userspace dev_t ABI encoding (glibc/musl
 * <sys/sysmacros.h>: minor low byte | major << 8 | minor-high << 12), the same
 * convention every native chardev (evdev, DRM card0, ALSA, ...) follows.
 * Callers pass the kernel dev_t (MKDEV(), 20-bit minor); open callbacks that
 * need it back (DRM sets inode->i_rdev and calls iminor()) decode the word
 * with Linux's new_decode_dev(). */
static uint32_t kpi_userspace_devt(uint32_t kdevt) {
  uint32_t major = (kdevt >> 20) & 0xfff;
  uint32_t minor = kdevt & 0xfffff;

  return (minor & 0xff) | (major << 8) | ((minor & ~0xffu) << 12);
}

int asc_vfs_register_devnode_at(const char *dir, const char *name,
                                __UINT32_TYPE__ rdev,
                                void *(*open_fn)(void *)) {
  vfs_node_t *node;
  struct kpi_path_devnode *entry;

  if (!dir || !name || !open_fn)
    return -22; /* EINVAL */
  if (strlen(dir) >= sizeof(kpi_path_devnodes[0].dir) ||
      strlen(name) >= sizeof(kpi_path_devnodes[0].name))
    return -22;

  for (int i = 0; i < kpi_path_devnode_count; i++) {
    if (!strcmp(kpi_path_devnodes[i].dir, dir) &&
        !strcmp(kpi_path_devnodes[i].name, name))
      return -17; /* EEXIST */
  }
  if (kpi_path_devnode_count >= KPI_DEVNODE_REGISTRY_MAX)
    return -12; /* ENOMEM */

  node = kmalloc(sizeof(*node));
  if (!node)
    return -12;
  vfs_node_init(node);
  strncpy(node->name, name, sizeof(node->name) - 1);
  node->name[sizeof(node->name) - 1] = '\0';
  node->flags = FS_CHARDEV | FS_PERSISTENT;
  node->mask = 0666;
  node->device = (void *)open_fn;
  node->open_instance = kpi_devnode_open_instance;
  node->inode = kpi_userspace_devt((uint32_t)rdev);

  entry = &kpi_path_devnodes[kpi_path_devnode_count++];
  strncpy(entry->dir, dir, sizeof(entry->dir) - 1);
  entry->dir[sizeof(entry->dir) - 1] = '\0';
  strncpy(entry->name, name, sizeof(entry->name) - 1);
  entry->name[sizeof(entry->name) - 1] = '\0';
  /* readdir's d_ino should match the node's st_ino, which is the encoded
   * word (the registry value only feeds drm_dri_readdir). */
  entry->rdev = kpi_userspace_devt((uint32_t)rdev);
  entry->metadata = node;
  return 0;
}

void asc_vfs_unregister_devnode_at(const char *dir, const char *name) {
  if (!dir || !name)
    return;

  for (int i = 0; i < kpi_path_devnode_count; i++) {
    struct kpi_path_devnode *entry = &kpi_path_devnodes[i];
    if (strcmp(entry->dir, dir) != 0 || strcmp(entry->name, name) != 0)
      continue;

    vfs_node_t *metadata = entry->metadata;
    kpi_path_devnodes[i] = kpi_path_devnodes[kpi_path_devnode_count - 1];
    kpi_path_devnode_count--;
    if (metadata)
      vfs_close(metadata);
    return;
  }
}

void *asc_vfs_poll_wq_alloc(void) {
  wait_queue_t *wq = kmalloc(sizeof(*wq));

  if (!wq)
    return NULL;
  wait_queue_init(wq);
  return wq;
}

void asc_vfs_poll_wq_free(void *wq) {
  if (wq)
    kfree(wq);
}

void *asc_vfs_devnode_lookup(const char *dir, const char *name) {
  if (!dir || !name)
    return NULL;
  for (int i = 0; i < kpi_path_devnode_count; i++) {
    if (!strcmp(kpi_path_devnodes[i].dir, dir) &&
        !strcmp(kpi_path_devnodes[i].name, name))
      return kpi_path_devnodes[i].metadata;
  }
  return NULL;
}

const char *asc_vfs_devnode_name_at(const char *dir, unsigned int index,
                                    __UINT32_TYPE__ *rdev_out) {
  unsigned int seen = 0;

  if (!dir)
    return NULL;
  for (int i = 0; i < kpi_path_devnode_count; i++) {
    if (strcmp(kpi_path_devnodes[i].dir, dir) != 0)
      continue;
    if (seen++ == index) {
      if (rdev_out)
        *rdev_out = kpi_path_devnodes[i].rdev;
      return kpi_path_devnodes[i].name;
    }
  }
  return NULL;
}

/* ── kernel-side fd-less open ───────────────────────────────────────────── */

void *asc_vfs_kernel_open(const char *path) {
  vfs_node_t *node;
  bool node_owned = true;

  if (!path)
    return NULL;
  node = vfs_resolve_path((char *)path);
  if (!node)
    return NULL;

  if (node->open_instance) {
    vfs_node_t *metadata = node;
    node = node->open_instance(metadata);
    if (!node) {
      vfs_close(metadata);
      return NULL;
    }
    /* The per-open node carries no reference yet; the metadata node is
     * persistent, exactly as sys_open() treats it. */
    node_owned = false;
  }

  vfs_open(node);
  if (node_owned)
    vfs_close(node);
  return node;
}

int asc_vfs_kernel_ioctl(void *node, unsigned int request,
                         __UINT64_TYPE__ arg) {
  vfs_node_t *n = node;

  if (!n || !n->ioctl)
    return -25; /* ENOTTY */
  return n->ioctl(n, request, (uint64_t)arg);
}

int asc_vfs_kernel_poll(void *node, int events) {
  vfs_node_t *n = node;

  if (!n || !n->poll)
    return 0;
  return n->poll(n, events);
}

unsigned int asc_vfs_kernel_read(void *node, unsigned int offset,
                                 unsigned int size, unsigned char *buffer) {
  vfs_node_t *n = node;

  if (!n || !n->read)
    return 0;
  return n->read(n, offset, size, buffer);
}

unsigned int asc_vfs_kernel_write(void *node, unsigned int offset,
                                  unsigned int size,
                                  unsigned char *buffer) {
  vfs_node_t *n = node;

  if (!n || !n->write)
    return 0;
  return n->write(n, offset, size, buffer);
}

unsigned int asc_vfs_kernel_size(void *node) {
  vfs_node_t *n = node;

  return n ? (unsigned int)n->length : 0;
}

void asc_vfs_kernel_close(void *node) {
  if (node)
    vfs_close((vfs_node_t *)node);
}

/* ── fd table ───────────────────────────────────────────────────────────── */

int asc_vfs_fd_alloc(unsigned int flags) {
  struct thread *t = sched_get_current();
  int fd;

  (void)flags;
  if (!t || !t->files)
    return -1;
  fd = alloc_fd(t);
  return fd;
}

void asc_vfs_fd_install(int fd, void *node, unsigned int flags) {
  struct thread *t = sched_get_current();

  if (!t || !t->files || fd < 0 || fd >= MAX_FDS || !node)
    return;

  spinlock_acquire(&t->files->lock);
  t->fds[fd] = (vfs_node_t *)node;
  t->fd_offsets[fd] = 0;
  t->fd_flags[fd] = 0;
  if (flags & KPI_O_CLOEXEC)
    t->fd_flags[fd] = KPI_FD_FLAGS_CLOEXEC_BIT;
  spinlock_release(&t->files->lock);
}

void asc_vfs_fd_release_reserved(int fd) {
  struct thread *t = sched_get_current();

  if (!t || !t->files || fd < 0 || fd >= MAX_FDS)
    return;

  spinlock_acquire(&t->files->lock);
  if (t->fds[fd] == KPI_FD_RESERVED)
    t->fds[fd] = NULL;
  spinlock_release(&t->files->lock);
}

void *asc_vfs_fd_lookup(int fd) {
  struct thread *t = sched_get_current();
  vfs_node_t *node;

  if (!t || !t->files || fd < 0 || fd >= MAX_FDS)
    return NULL;

  node = t->fds[fd];
  if (!node || node == KPI_FD_RESERVED)
    return NULL;
  return node;
}
