/* Mount tables are namespace owned; inode contents remain shared across bind
 * mounts. View nodes carry path ancestry separately from inode identity, which
 * is necessary for bind mounts, '..', and read-only mounts of writable inodes.
 */
#include "mount_namespace.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../sched/namespace.h"
#include "../sched/sched.h"

struct mount_link {
  uint64_t id, parent_id, flags;
  vfs_node_t *point, *target;
  char source[64], type[32], path[256];
  bool detached;
  struct mount_link *next;
};
struct mount_namespace {
  uint32_t id;
  uint32_t refs;
  spinlock_t lock;
  struct mount_link *mounts;
  vfs_node_t *initial_root;
};
static uint64_t next_id = 0x10000;
static vfs_node_t *make_view(vfs_node_t *, vfs_node_t *, uint64_t);
static struct mount_namespace *current_ns(void) {
  struct thread *t = sched_get_current();
  return t ? t->mount_ns : NULL;
}
uint32_t vfs_namespace_id(struct thread *task) {
  if (!task)
    return 0x4e530003;
  spinlock_acquire(&task->namespace_lock);
  uint32_t id = task->mount_ns ? task->mount_ns->id : 0x4e530003;
  spinlock_release(&task->namespace_lock);
  return id;
}
bool vfs_namespace_active(void) { return current_ns() != NULL; }
vfs_node_t *vfs_task_root(void) {
  struct thread *t = sched_get_current();
  return t && t->fs_root_node ? t->fs_root_node : fs_root;
}
vfs_node_t *vfs_backing_node(vfs_node_t *n) {
  while (n && n->view_backing)
    n = n->view_backing;
  return n;
}
static bool same_inode(vfs_node_t *a, vfs_node_t *b) {
  a = vfs_backing_node(a);
  b = vfs_backing_node(b);
  return a && b && (a == b || (a->inode == b->inode && a->device == b->device));
}
static void sync_view(vfs_node_t *n) {
  vfs_node_t *b = n->view_backing;
  n->length = b->length;
  n->mask = b->mask;
  n->uid = b->uid;
  n->gid = b->gid;
  n->atime = b->atime;
  n->mtime = b->mtime;
  n->ctime = b->ctime;
}
uint64_t vfs_mount_flags(vfs_node_t *n) {
  struct mount_namespace *ns = current_ns();
  if (!ns || !n || !n->view_backing)
    return false;
  uint64_t flags = 0;
  spinlock_acquire(&ns->lock);
  for (struct mount_link *m = ns->mounts; m; m = m->next)
    if (m->id == n->mount_id) {
      flags = m->flags;
      break;
    }
  spinlock_release(&ns->lock);
  return flags;
}
bool vfs_mount_readonly(vfs_node_t *n) { return (vfs_mount_flags(n) & 1) != 0; }
static uint32_t view_read(vfs_node_t *n, uint32_t o, uint32_t z, uint8_t *b) {
  uint32_t r = vfs_read(n->view_backing, o, z, b);
  sync_view(n);
  return r;
}
static uint32_t view_write(vfs_node_t *n, uint32_t o, uint32_t z, uint8_t *b) {
  if (vfs_mount_readonly(n))
    return (uint32_t)-30;
  uint32_t r = vfs_write(n->view_backing, o, z, b);
  sync_view(n);
  return r;
}
static void view_close(vfs_node_t *n) {
  vfs_close(n->view_backing);
  if (n->view_parent)
    vfs_close(n->view_parent);
}
static struct dirent *view_readdir(vfs_node_t *n, uint32_t i) {
  return vfs_readdir(n->view_backing, i);
}
#define VIEW_MUTATE(name, decl, args, call)                                    \
  static int view_##name decl {                                                \
    if (vfs_mount_readonly(n))                                                 \
      return -30;                                                              \
    int r = call args;                                                         \
    sync_view(n);                                                              \
    return r;                                                                  \
  }
VIEW_MUTATE(create, (vfs_node_t * n, char *s, uint16_t p),
            (n->view_backing, s, p), vfs_create)
VIEW_MUTATE(mkdir, (vfs_node_t * n, char *s, uint16_t p),
            (n->view_backing, s, p), vfs_mkdir)
VIEW_MUTATE(unlink, (vfs_node_t * n, char *s), (n->view_backing, s), vfs_unlink)
VIEW_MUTATE(rmdir, (vfs_node_t * n, char *s), (n->view_backing, s), vfs_rmdir)
VIEW_MUTATE(chmod, (vfs_node_t * n, uint16_t p), (n->view_backing, p),
            vfs_chmod)
VIEW_MUTATE(chown, (vfs_node_t * n, uint32_t u, uint32_t g),
            (n->view_backing, u, g), vfs_chown)
VIEW_MUTATE(truncate, (vfs_node_t * n, uint32_t z), (n->view_backing, z),
            vfs_truncate)
VIEW_MUTATE(symlink, (vfs_node_t * n, char *s, char *t),
            (n->view_backing, s, t), vfs_symlink)
VIEW_MUTATE(rename, (vfs_node_t * n, char *s, char *t), (n->view_backing, s, t),
            vfs_rename)
VIEW_MUTATE(mknod, (vfs_node_t * n, char *s, uint16_t p, uint32_t f, void *d),
            (n->view_backing, s, p, f, d), vfs_mknod)
VIEW_MUTATE(fallocate, (vfs_node_t * n, int m, uint32_t o, uint32_t z),
            (n->view_backing, m, o, z), vfs_fallocate)
static int view_readlink(vfs_node_t *n, char *b, uint32_t z) {
  return vfs_readlink(n->view_backing, b, z);
}
static vfs_node_t *view_get_link(vfs_node_t *n) {
  return n->view_backing->get_link ? n->view_backing->get_link(n->view_backing)
                                   : NULL;
}
static int view_statfs(vfs_node_t *n, struct statfs_buf *b) {
  return vfs_statfs(n->view_backing, b);
}
static int view_poll(vfs_node_t *n, int e) {
  return vfs_poll(n->view_backing, e);
}
static int view_ioctl(vfs_node_t *n, uint32_t r, uint64_t a) {
  return n->view_backing->ioctl ? n->view_backing->ioctl(n->view_backing, r, a)
                                : -25;
}
static uint64_t view_mmap(vfs_node_t *n, uint64_t a, uint64_t l, uint64_t p,
                          uint64_t f, uint64_t o) {
  if ((p & 2) && (f & 1) && vfs_mount_readonly(n))
    return (uint64_t)-13;
  return n->view_backing->mmap
             ? n->view_backing->mmap(n->view_backing, a, l, p, f, o)
             : 0;
}
static vfs_node_t *view_instance(vfs_node_t *n) {
  vfs_node_t *b = n->view_backing->open_instance(n->view_backing);
  if (!b)
    return NULL;
  vfs_node_t *v = make_view(b, n->view_parent, n->mount_id);
  if (v)
    v->refcount = 0;
  return v;
}
static uint32_t view_nlink(vfs_node_t *n) {
  return n->view_backing->get_nlink
             ? n->view_backing->get_nlink(n->view_backing)
             : 1;
}
static vfs_node_t *make_view(vfs_node_t *b, vfs_node_t *parent, uint64_t id) {
  b = vfs_backing_node(b);
  if (!b)
    return NULL;
  vfs_node_t *n = kmalloc(sizeof(*n));
  if (!n)
    return NULL;
  vfs_node_init(n);
  n->view_backing = b;
  vfs_open(b);
  n->view_parent = parent;
  if (parent)
    vfs_open(parent);
  n->mount_id = id;
  n->inode = b->inode;
  n->device = b->device;
  n->impl = b->impl;
  strncpy(n->name, b->name, 127);
  n->name[127] = 0;
  n->flags = (b->flags & ~(FS_PERSISTENT | FS_MOUNTPOINT | FS_PAGE_CACHE)) |
             FS_DENTRY_NOCACHE;
  n->read = view_read;
  n->write = b->write ? view_write : NULL;
  n->close = view_close;
  n->readdir = view_readdir;
  n->finddir = vfs_namespace_finddir;
  n->create = b->create ? view_create : NULL;
  n->mkdir = b->mkdir ? view_mkdir : NULL;
  n->unlink = b->unlink ? view_unlink : NULL;
  n->rmdir = b->rmdir ? view_rmdir : NULL;
  n->chmod = b->chmod ? view_chmod : NULL;
  n->chown = b->chown ? view_chown : NULL;
  n->truncate = b->truncate ? view_truncate : NULL;
  n->symlink = b->symlink ? view_symlink : NULL;
  n->rename = b->rename ? view_rename : NULL;
  n->mknod = b->mknod ? view_mknod : NULL;
  n->fallocate = b->fallocate ? view_fallocate : NULL;
  n->readlink = view_readlink;
  n->get_link = b->get_link ? view_get_link : NULL;
  n->statfs = view_statfs;
  n->poll = view_poll;
  n->ioctl = view_ioctl;
  n->mmap = b->mmap ? view_mmap : NULL;
  n->mmap_shared_only = b->mmap_shared_only;
  n->mmap_private_pmm_refs = b->mmap_private_pmm_refs;
  n->mmap_shared_pmm_refs = b->mmap_shared_pmm_refs;
  n->open_instance = b->open_instance ? view_instance : NULL;
  n->get_nlink = view_nlink;
  n->wait_queue = b->wait_queue;
  sync_view(n);
  return n;
}
vfs_node_t *vfs_namespace_root_view(vfs_node_t *n) {
  vfs_node_t *view =
      make_view(n, n->view_parent, n->mount_id ? n->mount_id : 1);
  if (view) {
    strncpy(view->name, n->name, 127);
    view->name[127] = 0;
  }
  return view;
}

bool vfs_namespace_node_path(vfs_node_t *n, char *buffer, size_t size) {
  if (!n || !n->view_backing || size < 2)
    return false;
  struct thread *task = sched_get_current();
  vfs_node_t *root =
      task && task->namei_path_root ? task->namei_path_root : vfs_task_root();
  const char *parts[128];
  size_t count = 0, total = 1;
  for (vfs_node_t *p = n; p && p->view_parent; p = p->view_parent) {
    if (p->mount_id == root->mount_id && same_inode(p, root))
      break;
    if (count == 128)
      return false;
    parts[count++] = p->name;
    total += strlen(p->name) + 1;
  }
  if (total >= size)
    return false;
  size_t used = 0;
  buffer[used++] = '/';
  while (count) {
    const char *s = parts[--count];
    size_t z = strlen(s);
    if (used > 1)
      buffer[used++] = '/';
    memcpy(buffer + used, s, z);
    used += z;
  }
  buffer[used] = 0;
  return true;
}
static struct mount_link *new_link(vfs_node_t *p, vfs_node_t *t, uint64_t id,
                                   const char *s, const char *f,
                                   const char *path) {
  struct mount_link *m = kmalloc(sizeof(*m));
  if (!m)
    return NULL;
  memset(m, 0, sizeof(*m));
  m->id = id;
  m->parent_id = p ? p->mount_id : 1;
  m->point = p;
  m->target = t;
  if (p)
    vfs_open(p);
  vfs_open(t);
  strncpy(m->source, s ? s : "none", 63);
  strncpy(m->type, f ? f : "unknown", 31);
  strncpy(m->path, path ? path : "/", 255);
  return m;
}
int vfs_namespace_seed(struct mount_namespace *ns, vfs_node_t *p, vfs_node_t *t,
                       uint64_t id, const char *s, const char *f,
                       const char *path) {
  struct mount_link *m = new_link(p, t, id, s, f, path);
  if (!m)
    return -12;
  struct mount_link **tail = &ns->mounts;
  while (*tail)
    tail = &(*tail)->next;
  *tail = m;
  return 0;
}
static void ns_put(struct mount_namespace *ns) {
  if (!ns || __atomic_sub_fetch(&ns->refs, 1, __ATOMIC_ACQ_REL))
    return;
  if (ns->initial_root)
    vfs_close(ns->initial_root);
  struct mount_link *m = ns->mounts;
  while (m) {
    struct mount_link *q = m->next;
    if (m->point)
      vfs_close(m->point);
    vfs_close(m->target);
    kfree(m);
    m = q;
  }
  kfree(ns);
}
static struct mount_namespace *ns_copy(struct mount_namespace *old) {
  struct mount_namespace *ns = kmalloc(sizeof(*ns));
  if (!ns)
    return NULL;
  memset(ns, 0, sizeof(*ns));
  ns->id = namespace_allocate_id();
  ns->refs = 1;
  spinlock_init(&ns->lock);
  if (!old) {
    if (vfs_initial_mounts_copy(ns)) {
      ns_put(ns);
      return NULL;
    }
    /* Boot mountpoints can be adopted in place, so their inode mount_id is
     * already the mounted ID. Derive parent contexts from the mount paths. */
    for (struct mount_link *m = ns->mounts; m; m = m->next) {
      size_t best = 0;
      m->parent_id = fs_root->mount_id ? fs_root->mount_id : 1;
      for (struct mount_link *q = ns->mounts; q; q = q->next) {
        if (q == m)
          continue;
        size_t z = strlen(q->path);
        if (z > best && z < strlen(m->path) && !strncmp(m->path, q->path, z) &&
            (z == 1 || m->path[z] == '/')) {
          best = z;
          m->parent_id = q->id;
        }
      }
    }
    return ns;
  }
  spinlock_acquire(&old->lock);
  struct mount_link **tail = &ns->mounts;
  for (struct mount_link *m = old->mounts; m; m = m->next) {
    struct mount_link *n =
        new_link(m->point, m->target, m->id, m->source, m->type, m->path);
    if (!n) {
      spinlock_release(&old->lock);
      ns_put(ns);
      return NULL;
    }
    n->flags = m->flags;
    n->parent_id = m->parent_id;
    n->detached = m->detached;
    *tail = n;
    tail = &n->next;
  }
  spinlock_release(&old->lock);
  return ns;
}
struct mount_namespace *vfs_namespace_prepare(struct thread *parent) {
  struct mount_namespace *ns = ns_copy(parent ? parent->mount_ns : NULL);
  if (!ns)
    return NULL;
  vfs_node_t *root = parent ? parent->fs_root_node : NULL;
  if (root) {
    vfs_open(root);
    ns->initial_root = root;
  } else
    ns->initial_root =
        make_view(fs_root, NULL, fs_root->mount_id ? fs_root->mount_id : 1);
  if (!ns->initial_root) {
    ns_put(ns);
    return NULL;
  }
  return ns;
}
void vfs_namespace_put(struct mount_namespace *ns) { ns_put(ns); }
struct mount_namespace *vfs_namespace_lookup_copy(struct thread *task) {
  struct mount_namespace *ns = vfs_namespace_prepare(task);
  if (ns)
    ns->id = vfs_namespace_id(task);
  return ns;
}
void vfs_namespace_install(struct thread *task,
                           struct mount_namespace *prepared) {
  if (!task || !prepared)
    return;
  spinlock_acquire(&task->namespace_lock);
  struct mount_namespace *old = task->mount_ns;
  vfs_node_t *oldroot = task->fs_root_node;
  task->mount_ns = prepared;
  task->fs_root_node = prepared->initial_root;
  vfs_open(task->fs_root_node);
  spinlock_release(&task->namespace_lock);
  if (oldroot)
    vfs_close(oldroot);
  ns_put(old);
}
int vfs_namespace_clone(struct thread *c, struct thread *p, bool separate) {
  if (!c || !p)
    return -22;
  c->mount_ns = NULL;
  c->fs_root_node = NULL;
  if (separate) {
    c->mount_ns = vfs_namespace_prepare(p);
    if (!c->mount_ns)
      return -12;
  } else if (p->mount_ns) {
    c->mount_ns = p->mount_ns;
    __atomic_add_fetch(&c->mount_ns->refs, 1, __ATOMIC_RELAXED);
  }
  if (p->fs_root_node) {
    c->fs_root_node = p->fs_root_node;
    vfs_open(c->fs_root_node);
  } else if (c->mount_ns) {
    c->fs_root_node = c->mount_ns->initial_root;
    vfs_open(c->fs_root_node);
  }
  return 0;
}
void vfs_namespace_release(struct thread *t) {
  if (!t)
    return;
  spinlock_acquire(&t->namespace_lock);
  struct mount_namespace *ns = t->mount_ns;
  vfs_node_t *root = t->fs_root_node;
  t->mount_ns = NULL;
  t->fs_root_node = NULL;
  spinlock_release(&t->namespace_lock);
  if (root)
    vfs_close(root);
  ns_put(ns);
}
int vfs_namespace_unshare(struct thread *t) {
  if (!t)
    return -22;
  struct mount_namespace *ns = vfs_namespace_prepare(t);
  if (!ns)
    return -12;
  vfs_namespace_install(t, ns);
  vfs_node_t *cwd = vfs_namespace_resolve(vfs_task_root(),
                                          t->cwd_path[0] ? t->cwd_path : "/");
  if (cwd) {
    if (t->cwd_node)
      vfs_close(t->cwd_node);
    t->cwd_node = cwd;
  }
  return 0;
}

vfs_node_t *vfs_namespace_finddir(vfs_node_t *dir, char *name) {
  vfs_node_t *b = vfs_backing_node(dir);
  if (!b || !b->finddir)
    return NULL;
  if (!strcmp(name, ".")) {
    vfs_open(dir);
    return dir;
  }
  if (!strcmp(name, "..")) {
    vfs_node_t *p = (dir->mount_id == vfs_task_root()->mount_id &&
                     same_inode(dir, vfs_task_root()))
                        ? dir
                        : (dir->view_parent ? dir->view_parent : dir);
    vfs_open(p);
    return p;
  }
  vfs_node_t *raw = b->finddir(b, name);
  if (!raw)
    return NULL;
  uint64_t id = dir->mount_id ? dir->mount_id : 1;
  vfs_node_t *target = raw;
  struct mount_namespace *ns = current_ns();
  if (ns) {
    spinlock_acquire(&ns->lock);
    for (int hops = 0; hops < 32; hops++) {
      struct mount_link *hit = NULL;
      for (struct mount_link *m = ns->mounts; m; m = m->next)
        if (!m->detached && m->parent_id == id &&
            same_inode(m->point, target)) {
          hit = m;
          break;
        }
      if (!hit)
        break;
      target = hit->target;
      id = hit->id;
    }
    vfs_open(target);
    spinlock_release(&ns->lock);
  } else
    vfs_open(target);
  vfs_node_t *view = make_view(target, dir, id);
  if (view) {
    strncpy(view->name, name, 127);
    view->name[127] = 0;
  }
  vfs_close(target);
  vfs_close(raw);
  return view;
}
vfs_node_t *vfs_namespace_resolve(vfs_node_t *dir, const char *path) {
  if (!path || !*path)
    return NULL;
  vfs_node_t *root = vfs_task_root();
  vfs_node_t *cur = path[0] == '/' ? root : (dir ? dir : root);
  struct thread *task = sched_get_current();
  uint64_t policy = task ? task->namei_resolve_flags : 0;
  if ((policy & 8) && path[0] == '/') {
    if (task)
      task->namei_error = -18;
    return NULL;
  }
  if (cur == fs_root)
    cur = root;
  if (cur && !cur->view_backing && task && cur == task->cwd_node &&
      task->cwd_path[0] == '/') {
    cur = vfs_namespace_resolve(root, task->cwd_path);
    if (!cur)
      return NULL;
  } else
    vfs_open(cur);
  if (strlen(path) >= 4096) {
    vfs_close(cur);
    return NULL;
  }
  /* The syscall caller and symlink walk already use kernel stack space.
   * Three PATH_MAX arrays here consumed over 12 KiB of a 16 KiB stack,
   * corrupting adjacent allocations during ordinary sandbox path lookup. */
  struct { char pending[4096], link[4096]; } *scratch = kmalloc(sizeof(*scratch));
  if (!scratch) {
    if (task) task->namei_error = -12;
    vfs_close(cur);
    return NULL;
  }
  char *pending = scratch->pending;
  strcpy(pending, path);
  char *p = pending;
  int links = 0;
  while (*p) {
    while (*p == '/')
      p++;
    if (!*p)
      break;
    char part[128];
    int z = 0;
    while (*p && *p != '/') {
      if (z >= 127)
        goto fail;
      part[z++] = *p++;
    }
    part[z] = 0;
    if ((cur->flags & FS_TYPE_MASK) != FS_DIRECTORY || !vfs_access(cur, 1))
      goto fail;
    if (!strcmp(part, "."))
      continue;
    if ((policy & 8) && !strcmp(part, "..") &&
        cur->mount_id == root->mount_id && same_inode(cur, root)) {
      if (task)
        task->namei_error = -18;
      goto fail;
    }
    vfs_node_t *n = vfs_namespace_finddir(cur, part);
    if (!n)
      goto fail;
    if ((policy & 1) && n->mount_id != cur->mount_id) {
      if (task)
        task->namei_error = -18;
      vfs_close(n);
      goto fail;
    }
    if ((n->flags & FS_TYPE_MASK) == FS_SYMLINK) {
      if ((policy & 4) || ((policy & 2) && n->get_link)) {
        if (task)
          task->namei_error = -40;
        vfs_close(n);
        goto fail;
      }
      if (n->get_link) {
        vfs_node_t *target = n->get_link(n);
        vfs_close(n);
        if (!target)
          goto fail;
        vfs_close(cur);
        cur = target;
        continue;
      }
      char *link = scratch->link;
      int l = vfs_readlink(n, link, sizeof(scratch->link) - 1);
      vfs_close(n);
      if (l < 0 || ++links > 40)
        goto fail;
      link[l] = 0;
      size_t rest = strlen(p);
      if ((size_t)l + rest + 1 >= sizeof(scratch->pending))
        goto fail;
      memmove(pending + l, p, rest + 1);
      memcpy(pending, link, l);
      p = pending;
      if (link[0] == '/') {
        if (policy & 8) {
          if (task)
            task->namei_error = -18;
          goto fail;
        }
        vfs_close(cur);
        cur = root;
        vfs_open(cur);
      }
      continue;
    }
    vfs_close(cur);
    cur = n;
  }
  kfree(scratch);
  return cur;
fail:
  kfree(scratch);
  vfs_close(cur);
  return NULL;
}
static bool source_contains(struct mount_namespace *ns, struct mount_link *m,
                            vfs_node_t *source) {
  if (m->point && m->point->view_backing) {
    for (vfs_node_t *n = m->point; n; n = n->view_parent)
      if (n->mount_id == source->mount_id && same_inode(n, source))
        return true;
    return false;
  }
  /* Boot entries have no path-view parents. Compare their path relative to
   * the source mount; source itself may be below that mount's root. */
  const char *parts[128];
  size_t count = 0;
  vfs_node_t *n = source;
  while (n && n->view_parent && n->view_parent->mount_id == source->mount_id) {
    if (count == 128)
      return false;
    parts[count++] = n->name;
    n = n->view_parent;
  }
  char relative[256];
  size_t used = 0;
  relative[used++] = '/';
  while (count) {
    const char *x = parts[--count];
    size_t z = strlen(x);
    if (used + z + 1 >= sizeof(relative))
      return false;
    if (used > 1)
      relative[used++] = '/';
    memcpy(relative + used, x, z);
    used += z;
  }
  relative[used] = 0;
  const char *point = m->path;
  for (struct mount_link *q = ns->mounts; q; q = q->next)
    if (q->id == m->parent_id) {
      size_t z = strlen(q->path);
      if (z > 1 && !strncmp(point, q->path, z))
        point += z;
      break;
    }
  return used == 1 || (!strncmp(point, relative, used) &&
                       (point[used] == '/' || point[used] == 0));
}
static int namespace_mount_tree(vfs_node_t *p, vfs_node_t *t, const char *s,
                                const char *f, const char *path, uint64_t flags,
                                uint64_t excluded_mount) {
  struct mount_namespace *ns = current_ns();
  if (!ns)
    return -22;
  char canonical[256];
  if (vfs_namespace_node_path(p, canonical, sizeof(canonical)))
    path = canonical;
  struct mount_link *m = new_link(
      p, t, __atomic_fetch_add(&next_id, 1, __ATOMIC_RELAXED), s, f, path);
  if (!m)
    return -12;
  m->flags = flags | ((flags & 0x1000) ? (vfs_mount_flags(t) & 15) : 0);
  spinlock_acquire(&ns->lock);
  if (flags & 0x4000) {
    /* Copy the subtree with new IDs and mapped parent contexts. This includes
     * covered mounts; dropping those exposes hidden backing filesystem data. */
    size_t count = 1;
    for (struct mount_link *q = ns->mounts; q; q = q->next)
      count++;
    struct {
      uint64_t old_id, new_id;
    } *map = kmalloc(count * sizeof(*map));
    if (!map) {
      spinlock_release(&ns->lock);
      if (m->point)
        vfs_close(m->point);
      vfs_close(m->target);
      kfree(m);
      return -12;
    }
    map[0].old_id = t->mount_id;
    map[0].new_id = m->id;
    size_t mapped = 1;
    struct mount_link *copies = m;
    for (size_t index = 0; index < mapped; index++) {
      for (struct mount_link *q = ns->mounts; q; q = q->next) {
        if (q->detached || q->id == excluded_mount ||
            q->parent_id != map[index].old_id)
          continue;
        bool seen = false;
        for (size_t k = 0; k < mapped; k++)
          if (map[k].old_id == q->id) {
            seen = true;
            break;
          }
        if (seen)
          continue;
        if (index == 0 && !source_contains(ns, q, t))
          continue;
        struct mount_link *c =
            new_link(q->point, q->target,
                     __atomic_fetch_add(&next_id, 1, __ATOMIC_RELAXED),
                     q->source, q->type, q->path);
        if (!c) {
          kfree(map);
          spinlock_release(&ns->lock);
          while (copies) {
            struct mount_link *next = copies->next;
            if (copies->point)
              vfs_close(copies->point);
            vfs_close(copies->target);
            kfree(copies);
            copies = next;
          }
          return -12;
        }
        c->flags = q->flags;
        c->parent_id = map[index].new_id;
        char sourcepath[256];
        if (vfs_namespace_node_path(t, sourcepath, sizeof(sourcepath))) {
          size_t z = strlen(sourcepath);
          const char *suffix = q->path;
          if (z > 1 && !strncmp(suffix, sourcepath, z) &&
              (suffix[z] == '/' || !suffix[z]))
            suffix += z;
          size_t destlen = strlen(m->path), suflen = strlen(suffix);
          if (destlen + suflen < sizeof(c->path)) {
            strcpy(c->path, m->path);
            if (destlen == 1 && m->path[0] == '/' && suffix[0] == '/')
              destlen = 0;
            strcpy(c->path + destlen, suffix);
          }
        }
        map[mapped].old_id = q->id;
        map[mapped++].new_id = c->id;
        c->next = copies;
        copies = c;
      }
    }
    kfree(map);
    struct mount_link *last = copies;
    while (last->next)
      last = last->next;
    last->next = ns->mounts;
    ns->mounts = copies;
  } else {
    m->next = ns->mounts;
    ns->mounts = m;
  }
  spinlock_release(&ns->lock);
  return 0;
}

int vfs_namespace_mount(vfs_node_t *p, vfs_node_t *t, const char *s,
                        const char *f, const char *path, uint64_t flags) {
  return namespace_mount_tree(p, t, s, f, path, flags, 0);
}

int vfs_namespace_setattr(vfs_node_t *p, uint64_t set, uint64_t clear,
                          bool recursive) {
  struct mount_namespace *ns = current_ns();
  if (!ns)
    return -22;
  if ((set | clear) & ~15ULL)
    return -95;
  spinlock_acquire(&ns->lock);
  struct mount_link *root = NULL;
  size_t count = 1;
  for (struct mount_link *m = ns->mounts; m; m = m->next) {
    count++;
    if (!m->detached && m->id == p->mount_id && same_inode(m->target, p))
      root = m;
  }
  if (!root) {
    spinlock_release(&ns->lock);
    return -22;
  }
  uint64_t *ids = recursive ? kmalloc(count * sizeof(*ids)) : NULL;
  if (recursive && !ids) {
    spinlock_release(&ns->lock);
    return -12;
  }
  root->flags = (root->flags & ~clear) | set;
  if (recursive) {
    size_t used = 1;
    ids[0] = root->id;
    for (size_t i = 0; i < used; i++)
      for (struct mount_link *m = ns->mounts; m; m = m->next) {
        if (m->detached || m == root || m->parent_id != ids[i])
          continue;
        bool seen = false;
        for (size_t j = 0; j < used; j++)
          if (ids[j] == m->id) {
            seen = true;
            break;
          }
        if (seen)
          continue;
        m->flags = (m->flags & ~clear) | set;
        ids[used++] = m->id;
      }
    kfree(ids);
  }
  spinlock_release(&ns->lock);
  return 0;
}
int vfs_namespace_move(vfs_node_t *source, vfs_node_t *target) {
  struct mount_namespace *ns = current_ns();
  if (!ns)
    return -22;
  for (vfs_node_t *n = target; n; n = n->view_parent)
    if (n->mount_id == source->mount_id && same_inode(n, source))
      return -22;
  char path[256];
  if (!vfs_namespace_node_path(target, path, sizeof(path)))
    return -36;
  vfs_open(target);
  spinlock_acquire(&ns->lock);
  vfs_node_t *old = NULL;
  for (struct mount_link *m = ns->mounts; m; m = m->next)
    if (!m->detached && m->id == source->mount_id &&
        same_inode(m->target, source)) {
      old = m->point;
      m->point = target;
      m->parent_id = target->mount_id;
      strcpy(m->path, path);
      break;
    }
  spinlock_release(&ns->lock);
  if (!old) {
    vfs_close(target);
    return -22;
  }
  vfs_close(old);
  return 0;
}
int vfs_namespace_remount(vfs_node_t *p, uint64_t flags) {
  struct mount_namespace *ns = current_ns();
  if (!ns)
    return -22;
  int r = -22;
  spinlock_acquire(&ns->lock);
  for (struct mount_link *m = ns->mounts; m; m = m->next)
    if (!m->detached && m->id == p->mount_id) {
      m->flags = flags;
      r = 0;
      break;
    }
  spinlock_release(&ns->lock);
  return r;
}
int vfs_namespace_umount(vfs_node_t *p, unsigned flags) {
  if (flags & ~(2u | 8u))
    return -22;
  struct mount_namespace *ns = current_ns();
  if (!ns)
    return -22;
  int r = -22;
  spinlock_acquire(&ns->lock);
  for (struct mount_link *m = ns->mounts; m; m = m->next)
    if (!m->detached && m->id == p->mount_id) {
      m->detached = true;
      r = 0;
      break;
    }
  spinlock_release(&ns->lock);
  return r;
}
int vfs_namespace_chroot(vfs_node_t *root) {
  struct thread *t = sched_get_current();
  if (!t)
    return -22;
  vfs_node_t *n = make_view(root, NULL, root->mount_id);
  if (!n)
    return -12;
  spinlock_acquire(&t->namespace_lock);
  vfs_node_t *old = t->fs_root_node;
  t->fs_root_node = n;
  spinlock_release(&t->namespace_lock);
  if (old)
    vfs_close(old);
  return 0;
}
int vfs_namespace_pivot(vfs_node_t *newroot, vfs_node_t *oldpoint) {
  struct thread *t = sched_get_current();
  struct mount_namespace *ns = current_ns();
  if (!t || !ns)
    return -1;
  if (!newroot->view_backing || newroot->mount_id == vfs_task_root()->mount_id)
    return -22;
  /* put_old must be beneath new_root; the special same-node pivot is used by
   * bubblewrap. */
  bool below = false;
  for (vfs_node_t *n = oldpoint; n; n = n->view_parent) {
    if (n->mount_id == newroot->mount_id && same_inode(n, newroot)) {
      below = true;
      break;
    }
  }
  if (!below)
    return -22;
  vfs_node_t *old = vfs_task_root();
  /* Moving new_root out of the old tree exposes its covered mountpoint.
   * Do not copy it (or its descendants) back into put_old: bubblewrap stages
   * on /tmp, and retaining that staging mount hides the host runtime files
   * at /oldroot/tmp. Ordinary recursive bind mounts still copy every mount. */
  int r = namespace_mount_tree(oldpoint, old, "oldroot", "bind", "/oldroot",
                               0x1000 | 0x4000, newroot->mount_id);
  if (r)
    return r;
  return vfs_namespace_chroot(newroot);
}
static bool mount_below(struct mount_namespace *ns, struct mount_link *m,
                        uint64_t root) {
  for (size_t depth = 0; depth < 512; depth++) {
    if (m->id == root || m->parent_id == root)
      return true;
    if (m->id == m->parent_id)
      return false;
    struct mount_link *parent = NULL;
    for (struct mount_link *q = ns->mounts; q; q = q->next)
      if (!q->detached && q->id == m->parent_id) {
        parent = q;
        break;
      }
    if (!parent)
      return false;
    m = parent;
  }
  return false;
}
int vfs_get_mounts_task(struct thread *task, vfs_mount_info_t *b, int max) {
  if (!task)
    return vfs_initial_mounts_get(b, max);
  spinlock_acquire(&task->namespace_lock);
  struct mount_namespace *ns = task->mount_ns;
  vfs_node_t *root = task->fs_root_node;
  if (ns)
    __atomic_add_fetch(&ns->refs, 1, __ATOMIC_RELAXED);
  if (root)
    vfs_open(root);
  spinlock_release(&task->namespace_lock);
  if (!ns) {
    if (root)
      vfs_close(root);
    return vfs_initial_mounts_get(b, max);
  }
  int i = 0;
  spinlock_acquire(&ns->lock);
  uint64_t rootid = root ? root->mount_id : 1;
  const char *prefix = "/";
  for (struct mount_link *m = ns->mounts; m; m = m->next)
    if (!m->detached && m->id == rootid) {
      prefix = m->path;
      break;
    }
  size_t prefixlen = strlen(prefix);
  for (struct mount_link *m = ns->mounts; m && i < max; m = m->next) {
    if (m->detached || !mount_below(ns, m, rootid))
      continue;
    b[i].mount_id = m->id;
    b[i].parent_id = m->id == rootid ? rootid : m->parent_id;
    b[i].flags = m->flags;
    const char *path = m->path;
    if (m->id == rootid)
      path = "/";
    else if (prefixlen > 1 && !strncmp(path, prefix, prefixlen) &&
             (path[prefixlen] == '/' || !path[prefixlen]))
      path += prefixlen;
    strncpy(b[i].mountpoint, *path ? path : "/", 127);
    b[i].mountpoint[127] = 0;
    strncpy(b[i].target, m->target->name, 127);
    b[i].target[127] = 0;
    strncpy(b[i].dev_name, m->source, 63);
    b[i].dev_name[63] = 0;
    strncpy(b[i].fs_type, m->type, 31);
    b[i].fs_type[31] = 0;
    i++;
  }
  spinlock_release(&ns->lock);
  if (root)
    vfs_close(root);
  ns_put(ns);
  return i;
}
int vfs_namespace_mounts(vfs_mount_info_t *b, int max) {
  return vfs_get_mounts_task(sched_get_current(), b, max);
}
