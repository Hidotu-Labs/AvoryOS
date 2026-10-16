/* Private devpts instances: only PTYs allocated through this instance's ptmx
 * appear in its lookup/readdir. Existing initial-instance PTYs remain hidden.
 */
#include "../drivers/pty.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../sched/sched.h"
#include "vfs.h"
struct devpts_instance {
  uint64_t id;
  uint32_t refs;
};
static uint64_t next_instance = 1;
static void instance_get(struct devpts_instance *i) {
  __atomic_add_fetch(&i->refs, 1, __ATOMIC_RELAXED);
}
static void instance_put(struct devpts_instance *i) {
  if (!__atomic_sub_fetch(&i->refs, 1, __ATOMIC_ACQ_REL))
    kfree(i);
}
static void root_close(vfs_node_t *n) { instance_put(n->device); }
static void master_close(vfs_node_t *n) {
  pty_pair_t *p = n->device;
  struct devpts_instance *i = p->devpts_instance;
  p->master_open = false;
  ptmx_close(n);
  instance_put(i);
}
static void slave_close(vfs_node_t *n) {
  pty_pair_t *p = n->device;
  struct devpts_instance *i = p->devpts_instance;
  pty_slave_close(n);
  instance_put(i);
}
static vfs_node_t *master_instance(vfs_node_t *n) {
  struct devpts_instance *i = n->device;
  int index = pty_alloc_pair();
  if (index < 0)
    return NULL;
  pty_pair_t *p = pty_get_pair(index);
  vfs_node_t *m = kmalloc(sizeof(*m));
  if (!m) {
    p->allocated = false;
    return NULL;
  }
  vfs_node_init(m);
  struct thread *t = sched_get_current();
  p->devpts_uid = t ? t->fsuid : 0;
  p->devpts_gid = t ? t->fsgid : 0;
  p->devpts_instance = i;
  instance_get(i);
  strcpy(m->name, "ptmx");
  m->flags = FS_CHARDEV;
  m->inode = (128u << 8) | index;
  m->mask = 0666;
  m->device = p;
  m->read = ptmx_read;
  m->write = ptmx_write;
  m->ioctl = ptmx_ioctl;
  m->poll = ptmx_poll;
  m->close = master_close;
  m->mmap = ptmx_mmap;
  m->wait_queue = p->master_waitq;
  p->master_node = m;
  m->refcount = 0;
  return m;
}
static vfs_node_t *pts_lookup(vfs_node_t *n, char *name) {
  struct devpts_instance *i = n->device;
  if (!strcmp(name, ".") || !strcmp(name, "..")) {
    vfs_open(n);
    return n;
  }
  vfs_node_t *r = kmalloc(sizeof(*r));
  if (!r)
    return NULL;
  vfs_node_init(r);
  strncpy(r->name, name, 127);
  r->name[127] = 0;
  r->flags = FS_CHARDEV | FS_DENTRY_NOCACHE;
  if (!strcmp(name, "ptmx")) {
    instance_get(i);
    r->device = i;
    r->mask = 0666;
    r->inode = (5u << 8) | 2;
    r->open_instance = master_instance;
    r->close = root_close;
    return r;
  }
  int idx = 0;
  if (!*name)
    goto fail;
  for (char *p = name; *p; p++) {
    if (*p < '0' || *p > '9' || idx >= PTY_MAX_PAIRS)
      goto fail;
    idx = idx * 10 + *p - '0';
  }
  pty_pair_t *p = pty_get_pair(idx);
  if (!p || p->devpts_instance != i || p->locked)
    goto fail;
  instance_get(i);
  r->device = p;
  r->inode = (136u << 8) | idx;
  r->mask = 0620;
  r->uid = p->devpts_uid;
  r->gid = p->devpts_gid;
  r->read = pty_slave_read;
  r->write = pty_slave_write;
  r->ioctl = pty_slave_ioctl;
  r->poll = pty_slave_poll;
  r->open = pty_slave_open;
  r->close = slave_close;
  r->mmap = pty_slave_mmap;
  r->wait_queue = p->slave_waitq;
  return r;
fail:
  kfree(r);
  return NULL;
}
static struct dirent *pts_readdir(vfs_node_t *n, uint32_t index) {
  struct dirent *d = vfs_readdir_buffer();
  memset(d, 0, sizeof(*d));
  d->d_type = DT_DIR;
  if (index < 2) {
    strcpy(d->name, index ? ".." : ".");
    return d;
  }
  if (index == 2) {
    strcpy(d->name, "ptmx");
    d->d_type = DT_CHR;
    return d;
  }
  uint32_t found = 3;
  for (int i = 0; i < PTY_MAX_PAIRS; i++) {
    pty_pair_t *p = pty_get_pair(i);
    if (!p || p->devpts_instance != n->device)
      continue;
    if (found++ == index) {
      int z = 0;
      if (i >= 10)
        d->name[z++] = '0' + i / 10;
      d->name[z++] = '0' + i % 10;
      d->name[z] = 0;
      d->ino = (136u << 8) | i;
      d->d_type = DT_CHR;
      return d;
    }
  }
  return NULL;
}
static int pts_statfs(vfs_node_t *n, struct statfs_buf *b) {
  (void)n;
  memset(b, 0, sizeof(*b));
  b->f_type = 0x1cd1;
  b->f_bsize = 4096;
  b->f_namelen = 255;
  return 0;
}
vfs_node_t *devpts_create_root(void) {
  struct devpts_instance *i = kmalloc(sizeof(*i));
  if (!i)
    return NULL;
  vfs_node_t *n = kmalloc(sizeof(*n));
  if (!n) {
    kfree(i);
    return NULL;
  }
  i->id = __atomic_fetch_add(&next_instance, 1, __ATOMIC_RELAXED);
  i->refs = 1;
  vfs_node_init(n);
  strcpy(n->name, "pts");
  n->device = i;
  n->inode = i->id;
  n->flags = FS_DIRECTORY | FS_DENTRY_NOCACHE;
  n->mask = 0755;
  n->finddir = pts_lookup;
  n->readdir = pts_readdir;
  n->statfs = pts_statfs;
  n->close = root_close;
  return n;
}
