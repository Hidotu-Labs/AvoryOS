/* LinuxKPI file/fd bridge.
 *
 * Presents Linux `struct file` objects backed by native vfs_node descriptors
 * (kernel/src/linuxkpi/native_vfs.c).  fd numbers are native descriptors, so
 * a file created here behaves like any other AVoryOS fd to read/write/close/
 * dup/mmap/poll.
 *
 * Lifetime rules:
 *   - `struct file::f_count` is the only owner.  anon_inode_getfile() returns
 *     with f_count == 1; fd_install() transfers that reference to the fd;
 *     close/fget/fput adjust it; f_op->release() runs at zero.
 *   - f_asc_node is the file's private native owner node and lasts until
 *     f_count reaches zero. Each fd_install gets its own preallocated VFS
 *     wrapper; its last close drops the transferred file reference. Closing
 *     an export must not detach the owner node of a still-live dma-buf.
 *   - Pseudo-fs objects (inode/dentry) are Avory shims; the dentry's
 *     d_op->d_release runs once when the file is released, which is how
 *     dma-buf gets its dmabuf_release callback. */

#include <linux/fs.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/anon_inodes.h>
#include <linux/mount.h>
#include <linux/pseudo_fs.h>
#include <linux/seq_file.h>
#include <linux/poll.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/stdarg.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/log2.h>

#include <linuxkpi/native_vfs.h>
#include <linuxkpi/native_prime.h>
#include <linuxkpi/native_sched.h>
#include <linuxkpi/log.h>
#include <linuxkpi/gpu_diag.h>
#include <linuxkpi/gpu_perf.h>
#include <linux/uaccess.h>
#include <drm/drm_file.h>
#include <drm/drm_drv.h>
#include <drm/drm_ioctl.h>
#include <uapi/drm/amdgpu_drm.h>

/* AMDGPU wraps drm_ioctl; observing its public entry point does not modify
 * vendor code. Weak linkage also permits builds without that driver. */
extern long amdgpu_drm_ioctl(struct file *, unsigned int, unsigned long)
    __attribute__((weak));

/* ── struct file lifetime ───────────────────────────────────────────────── */

static void kpi_inode_free(struct inode *inode);

static struct file *kpi_file_alloc(const char *name,
                                   const struct file_operations *fops,
                                   void *priv, int flags) {
  struct file *file;
  void *node;

  file = kzalloc(sizeof(*file), GFP_KERNEL);
  node = asc_vfs_anon_node();
  if (!file || !node) {
    kfree(file);
    if (node)
      asc_vfs_node_unref(node);
    return NULL;
  }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
  file_ref_init(&file->f_ref, 1);
#else
  atomic_set(&file->f_count, 1);
#endif
  file->f_op = fops;
  file->private_data = priv;
  file->f_flags = flags;
  file->f_mode = FMODE_READ | FMODE_WRITE;
  file->f_asc_node = node;

  asc_vfs_node_set_device(node, file);
  asc_vfs_node_set_nonblocking(node, flags & O_NONBLOCK);
  asc_vfs_node_set_name(node, name ? name : "anon");

  /* One native poll wait queue per file; detachable subscriptions connect
   * Linux wait heads to this queue and each exported node's epoll watchers. */
  file->f_poll_wq = asc_vfs_poll_wq_alloc();
  if (file->f_poll_wq)
    asc_vfs_node_set_wait_queue(node, file->f_poll_wq);
  return file;
}

static void kpi_file_free(struct file *file) {
  struct dentry *dentry = file->f_path.dentry;
  struct inode *inode = file->f_inode;

  linuxkpi_poll_detach(&file->f_poll_links, NULL);

  if (file->f_op && file->f_op->release)
    file->f_op->release(inode, file);

  /* The private owner survives descriptor exports and is released only
   * when the Linux file itself reaches its last reference. */
  if (file->f_asc_node) {
    asc_vfs_node_release_kernel(file->f_asc_node);
    file->f_asc_node = NULL;
  }

  asc_vfs_poll_wq_free(file->f_poll_wq);
  file->f_poll_wq = NULL;

  if (dentry) {
    if (dentry->d_op && dentry->d_op->d_release)
      dentry->d_op->d_release(dentry);
    kfree(dentry);
  }
  if (inode) {
    if (atomic_dec_and_test(&inode->i_count))
      kpi_inode_free(inode);
  }
  kfree(file);
}

struct file *get_file(struct file *file) {
  if (file)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
    file_ref_inc(&file->f_ref);
#else
    atomic_inc(&file->f_count);
#endif
  return file;
}

void fput(struct file *file) {
  if (!file)
    return;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
  if (file_ref_put(&file->f_ref))
#else
  if (atomic_dec_and_test(&file->f_count))
#endif
    kpi_file_free(file);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
/* Linux v6.18 fs/file.c reference-count slow path. Use the stock file_ref.h
 * protocol, including its terminal and saturation zones; freeing remains
 * native and occurs only after a successful final-reference transition. */
bool __file_ref_put(file_ref_t *ref, unsigned long cnt) {
  if (likely(cnt == FILE_REF_NOREF)) {
    if (!atomic_long_try_cmpxchg_release(&ref->refcnt, &cnt, FILE_REF_DEAD))
      return false;
    smp_acquire__after_ctrl_dep();
    return true;
  }
  if (WARN_ONCE(cnt >= FILE_REF_RELEASED, "imbalanced file reference put")) {
    atomic_long_set(&ref->refcnt, FILE_REF_DEAD);
    return false;
  }
  if (cnt > FILE_REF_MAXREF)
    atomic_long_set(&ref->refcnt, FILE_REF_SATURATED);
  return false;
}
#endif

void fput_many(struct file *file, unsigned int refs) {
  while (refs--)
    fput(file);
}

/* ── fd table ───────────────────────────────────────────────────────────── */

int get_unused_fd_flags(unsigned int flags) {
  return asc_vfs_file_fd_alloc(flags);
}

void put_unused_fd(unsigned int fd) { asc_vfs_fd_release_reserved((int)fd); }

void fd_install(unsigned int fd, struct file *file) {
  if (!file)
    return;
  /* Upstream dma_buf_getfile() records the allocation in inode->i_size.
   * Native fstat/lseek use the VFS node instead; leaving its length zero
   * makes Mesa's KMS importer request mmap(..., 0, ...) and reject the
   * DRI3 pixmap even though the AMD buffer itself is valid. */
  if (file->f_inode)
    asc_vfs_node_set_size(file->f_asc_node, file->f_inode->i_size);
  asc_vfs_file_fd_install((int)fd, file->f_asc_node);
}

struct file *fget(unsigned int fd) {
  void *node = asc_vfs_fd_get_node((int)fd);
  struct file *file;

  if (!node)
    return NULL;
  /* Native DRM nodes carry a native drm_file in ->device, not struct file. */
  file = asc_vfs_node_is_linux_file(node) ? asc_vfs_node_device(node) : NULL;
  if (file)
    get_file(file);
  asc_vfs_node_unref(node);
  return file;
}

struct file *fget_raw(unsigned int fd) { return fget(fd); }

struct fd fdget(unsigned int fd) {
  struct fd f = {.file = fget(fd), .flags = 0};

  if (f.file)
    f.flags = FDPUT_FPUT;
  return f;
}

void fdput(struct fd fd) {
  if ((fd.flags & FDPUT_FPUT) && fd.file)
    fput(fd.file);
}

int close_fd(unsigned int fd) {
  return asc_vfs_fd_close((int)fd);
}

/* 32-bit compat ioctls: this kernel has no compat entry points, so forward
 * to the native unlocked handler (mirrors upstream compat_ptr_ioctl). */
long compat_ptr_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
  if (!file->f_op || !file->f_op->unlocked_ioctl)
    return -ENOIOCTLCMD;
  return file->f_op->unlocked_ioctl(file, cmd, arg);
}

/* ── anon inodes ────────────────────────────────────────────────────────── */

struct file *anon_inode_getfile(const char *name,
                                const struct file_operations *fops, void *priv,
                                int flags) {
  return kpi_file_alloc(name, fops, priv, flags);
}

int anon_inode_getfd(const char *name, const struct file_operations *fops,
                     void *priv, int flags) {
  struct file *file = anon_inode_getfile(name, fops, priv, flags);
  int fd;

  if (!file)
    return -ENOMEM;

  fd = get_unused_fd_flags(flags);
  if (fd < 0) {
    fput(file);
    return fd;
  }

  fd_install(fd, file);
  return fd;
}

/* ── pseudo filesystem shim (dma-buf, DRM) ──────────────────────────────── */

/* init_pseudo() records the fs_context's dentry operations here; kern_mount()
 * copies them into the superblock it returns.  The callback runs synchronously
 * inside kern_mount(), so one snapshot slot is enough. */
static struct pseudo_fs_context *kpi_pseudo_ctx;

struct pseudo_fs_context *init_pseudo(struct fs_context *fc,
                                      unsigned long magic) {
  static struct pseudo_fs_context ctx;

  ctx.magic = magic;
  ctx.dops = NULL;
  fc->fs_type_magic = magic;
  kpi_pseudo_ctx = &ctx;
  return &ctx;
}

struct vfsmount *kern_mount(struct file_system_type *type) {
  struct super_block *sb;
  struct vfsmount *mnt;
  struct fs_context fc = {0};
  int ret;

  sb = kzalloc(sizeof(*sb), GFP_KERNEL);
  mnt = kzalloc(sizeof(*mnt), GFP_KERNEL);
  if (!sb || !mnt) {
    kfree(sb);
    kfree(mnt);
    return ERR_PTR(-ENOMEM);
  }

  kpi_pseudo_ctx = NULL;
  if (type && type->init_fs_context) {
    ret = type->init_fs_context(&fc);
    if (ret) {
      kfree(sb);
      kfree(mnt);
      return ERR_PTR(ret);
    }
  }

  sb->s_magic = fc.fs_type_magic;
  sb->s_type = type;
  sb->s_fs_info = NULL;
  sb->s_root = NULL;
  sb->s_dops = kpi_pseudo_ctx ? kpi_pseudo_ctx->dops : NULL;
  mnt->mnt_sb = sb;
  mnt->mnt_root = NULL;
  return mnt;
}

void kern_unmount(struct vfsmount *mnt) {
  if (!mnt)
    return;
  kfree(mnt->mnt_sb);
  kfree(mnt);
}

void kill_anon_super(struct super_block *sb) { (void)sb; }

/* Refcounted pinning of a kern_mount() result, as upstream fs/super.c.  The
 * first pin mounts; the last release unmounts.  Every pseudo fs user (dma-buf
 * uses kern_mount() directly; DRM uses these helpers) gets its own superblock,
 * so one filesystem's dentry ops never overwrite another's. */
int simple_pin_fs(struct file_system_type *type, struct vfsmount **mount,
                  int *count) {
  struct vfsmount *mnt;

  if (!mount || !count)
    return -EINVAL;

  if (++*count == 1) {
    mnt = kern_mount(type);
    if (IS_ERR(mnt)) {
      *count = 0;
      return (int)PTR_ERR(mnt);
    }
    *mount = mnt;
  }
  return 0;
}

void simple_release_fs(struct vfsmount **mount, int *count) {
  if (!mount || !count || *count == 0)
    return;

  if (--*count == 0) {
    kern_unmount(*mount);
    *mount = NULL;
  }
}

static void kpi_inode_free(struct inode *inode) {
  if (!inode)
    return;
  if (inode->i_mapping) {
    xa_destroy(&inode->i_mapping->i_pages);
    kfree(inode->i_mapping);
    inode->i_mapping = NULL;
  }
  kfree(inode);
}

struct inode *alloc_anon_inode(struct super_block *sb) {
  struct address_space *mapping;
  struct inode *inode = kzalloc(sizeof(*inode), GFP_KERNEL);

  mapping = kzalloc(sizeof(*mapping), GFP_KERNEL);
  if (!inode || !mapping) {
    kfree(inode);
    kfree(mapping);
    return ERR_PTR(-ENOMEM);
  }

  inode->i_sb = sb;
  atomic_set(&inode->i_count, 1);
  spin_lock_init(&inode->i_lock);
  xa_init(&mapping->i_pages);
  mapping->host = inode;
  mapping->gfp_mask = GFP_HIGHUSER | __GFP_ZERO;
  inode->i_mapping = mapping;
  return inode;
}

void iput(struct inode *inode) {
  if (!inode)
    return;
  if (atomic_dec_and_test(&inode->i_count))
    kpi_inode_free(inode);
}

struct inode *igrab(struct inode *inode) {
  if (inode)
    atomic_inc(&inode->i_count);
  return inode;
}

struct file *alloc_file_pseudo(struct inode *inode, struct vfsmount *mnt,
                               const char *name, int flags,
                               const struct file_operations *fops) {
  struct file *file;
  struct dentry *dentry;

  file = kpi_file_alloc(name, fops, NULL, flags);
  if (!file)
    return ERR_PTR(-ENOMEM);

  dentry = kzalloc(sizeof(*dentry), GFP_KERNEL);
  if (!dentry) {
    fput(file);
    return ERR_PTR(-ENOMEM);
  }
  dentry->d_name.name = (const unsigned char *)"dmabuf";
  dentry->d_name.len = 6;
  dentry->d_inode = inode;
  dentry->d_op = (inode && inode->i_sb) ? inode->i_sb->s_dops : NULL;
  atomic_set(&dentry->d_count, 1);

  file->f_inode = inode;
  /* Upstream alloc_file() links the file to the inode's address_space; the
   * dma-buf and DRM mmap paths (drm_vma_node_unmap -> unmap_mapping_range)
   * depend on it. */
  file->f_mapping = inode->i_mapping;
  file->f_path.mnt = mnt;
  file->f_path.dentry = dentry;
  return file;
}

struct dentry *dget(struct dentry *dentry) {
  if (dentry)
    atomic_inc(&dentry->d_count);
  return dentry;
}

void dput(struct dentry *dentry) {
  if (!dentry)
    return;
  if (atomic_dec_and_test(&dentry->d_count)) {
    if (dentry->d_op && dentry->d_op->d_release)
      dentry->d_op->d_release(dentry);
    kfree(dentry);
  }
}

char *dynamic_dname(char *buffer, int buflen, const char *fmt, ...) {
  va_list args;

  va_start(args, fmt);
  vsnprintf(buffer, (size_t)buflen, fmt, args);
  va_end(args);
  return buffer;
}

/* ── misc helpers used by imported code ─────────────────────────────────── */

size_t strlcpy(char *dest, const char *src, size_t size) {
  size_t len = strlen(src);

  if (size) {
    size_t n = len < size - 1 ? len : size - 1;
    memcpy(dest, src, n);
    dest[n] = '\0';
  }
  return len;
}

int seq_printf(struct seq_file *m, const char *fmt, ...) {
  va_list args;
  int ret;

  if (!m || !m->buf || m->count >= m->size)
    return -1;

  va_start(args, fmt);
  ret = vsnprintf(m->buf + m->count, m->size - m->count, fmt, args);
  va_end(args);
  if (ret > 0)
    m->count += (size_t)ret;
  return ret;
}

int seq_puts(struct seq_file *m, const char *s) {
  if (!m || !m->buf || !s)
    return -1;
  return seq_printf(m, "%s", s);
}

int seq_putc(struct seq_file *m, char c) {
  if (!m || !m->buf || m->count + 1 >= m->size)
    return -1;
  m->buf[m->count++] = c;
  m->buf[m->count] = '\0';
  return 0;
}

int seq_write(struct seq_file *seq, const void *data, size_t len) {
  if (!seq || !seq->buf || !data || seq->count + len > seq->size)
    return -1;
  memcpy(seq->buf + seq->count, data, len);
  seq->count += len;
  return 0;
}

/* ── native node callbacks ──────────────────────────────────────────────── */

static struct file *kpi_node_file(void *node) {
  return asc_vfs_node_device(node);
}

int linuxkpi_file_ioctl(void *node, unsigned int request, unsigned long arg) {
  struct file *file = kpi_node_file(node);

  if (!file || !file->f_op || !file->f_op->unlocked_ioctl)
    return -ENOTTY;
  bool trace=gpu_diag_level() && ((request>>8)&255)=='d';
  bool cs=false;
  union drm_amdgpu_cs input;
  unsigned long long start=trace ? linuxkpi_monotonic_ms() : 0;
  if (trace && request==DRM_IOCTL_AMDGPU_CS &&
      (file->f_op->unlocked_ioctl==drm_ioctl ||
       (amdgpu_drm_ioctl && file->f_op->unlocked_ioctl==amdgpu_drm_ioctl))) {
    struct drm_file *df=file->private_data;
    cs=df && df->minor && df->minor->dev && df->minor->dev->driver &&
       !strcmp(df->minor->dev->driver->name,"amdgpu") &&
       !copy_from_user(&input,(void __user *)arg,sizeof input);
    if (cs) gpu_diag_record(GPU_DIAG_CS_IN,file,input.in.ctx_id,input.in.bo_list_handle,
                            input.in.num_chunks,input.in.flags,0);
  }
  __UINT64_TYPE__ perf_start = gpu_perf_ioctl_start(request);
  int ret=(int)file->f_op->unlocked_ioctl(file,request,arg);
  gpu_perf_ioctl_finish(request, perf_start, ret);
  if (trace) gpu_diag_record(GPU_DIAG_IOCTL,file,request,arg,
                             linuxkpi_monotonic_ms()-start,0,ret);
  if (trace && ret < 0) {
    if (request == DRM_IOCTL_AMDGPU_GEM_CREATE)
      gpu_diag_prime_error(GPU_PRIME_SOURCE_ALLOC, file, request, 0, ret);
    else if (request == DRM_IOCTL_PRIME_HANDLE_TO_FD)
      gpu_diag_prime_error(GPU_PRIME_SOURCE_EXPORT, file, request, 0, ret);
  }
  if (cs && !ret) {
    union drm_amdgpu_cs output;
    if (!copy_from_user(&output,(void __user *)arg,sizeof output))
      gpu_diag_record(GPU_DIAG_CS_OUT,file,input.in.ctx_id,output.out.handle,0,0,0);
  }
  return ret;
}

static unsigned int kpi_poll_mask_to_native(unsigned int mask) {
  /* Native POLLIN=0x1, POLLOUT=0x4; Linux POLLIN=0x1, POLLOUT=0x4. */
  return mask & 0xFFFF;
}

/* Subscribe each polled VFS node, including separate dma-buf exports. The
 * bridge detaches these before node close or the Linux release callback. */
struct kpi_poll_table {
  struct poll_table_struct pt;
  struct file *owner;
  void *node;
  int error;
};

static void kpi_poll_qproc(struct file *file, wait_queue_head_t *wq,
                           struct poll_table_struct *pt) {
  struct kpi_poll_table *kpt = container_of(pt, struct kpi_poll_table, pt);
  (void)file;
  if (wq && linuxkpi_poll_register(&kpt->owner->f_poll_links, wq,
                                  kpt->node, kpt->owner->f_poll_wq))
    kpt->error = 1;
}

int linuxkpi_file_poll(void *node, int events) {
  struct file *file = kpi_node_file(node);
  struct kpi_poll_table kpt = {0};

  if (!file || !file->f_op || !file->f_op->poll)
    return 0;
  kpt.pt._qproc = kpi_poll_qproc;
  /* dma_buf_poll uses this mask to arm its fence callbacks. */
  kpt.pt._key = (unsigned int)events | POLLERR | POLLHUP;
  kpt.owner = file;
  kpt.node = node;
  unsigned int mask = file->f_op->poll(file, &kpt.pt);
  return (int)kpi_poll_mask_to_native(mask) | (kpt.error ? 0x8 : 0);
}

unsigned int linuxkpi_file_read(void *node, unsigned int offset,
                                unsigned int size, unsigned char *buffer) {
  struct file *file = kpi_node_file(node);
  ssize_t ret;

  (void)offset;
  if (!file || !file->f_op || !file->f_op->read)
    return 0;
  /* F_SETFL updates the native open file. DRM's read loop checks the Linux
   * file flags: leaving those at O_RDWR can park Xorg while draining events
   * from a descriptor it explicitly made nonblocking. */
  if (asc_vfs_node_nonblocking(node))
    file->f_flags |= O_NONBLOCK;
  else
    file->f_flags &= ~O_NONBLOCK;
  ret = file->f_op->read(file, (char __user *)buffer, size, &file->f_pos);
  return (unsigned int)ret;
}

unsigned int linuxkpi_file_write(void *node, unsigned int offset,
                                 unsigned int size, unsigned char *buffer) {
  struct file *file = kpi_node_file(node);
  ssize_t ret;

  (void)offset;
  if (!file || !file->f_op || !file->f_op->write)
    return 0;
  ret = file->f_op->write(file, (const char __user *)buffer, size,
                          &file->f_pos);
  return ret < 0 ? 0 : (unsigned int)ret;
}

/* ── mmap bridge ────────────────────────────────────────────────────────── */

struct kpi_mmap_bridge {
  struct vm_area_struct vma;
  int refs;
  bool closed;
  /* mremap rebase vs sleeping driver fault (TTM waits on dma-fence):
   * triple (vm_start, vm_end, vm_pgoff) must be consistent. Seqcount:
   * rebase marks odd, writes fields, marks even; fault rejects + zaps if
   * the sequence moved across the driver call, so a torn pgoff never backs
   * a texture page (sparse purple/green texels until refault). */
  volatile unsigned seq;
};

void linuxkpi_vma_ref(void *w) {
  struct kpi_mmap_bridge *b = w;

  if (b)
    __atomic_add_fetch(&b->refs, 1, __ATOMIC_ACQ_REL);
}

void linuxkpi_vma_unref(void *w) {
  struct kpi_mmap_bridge *b = w;

  if (!b)
    return;
  if (__atomic_sub_fetch(&b->refs, 1, __ATOMIC_ACQ_REL) != 0)
    return;

  if (!b->closed) {
    b->closed = true;
    if (b->vma.vm_ops && b->vma.vm_ops->close)
      b->vma.vm_ops->close(&b->vma);
  }
  if (b->vma.vm_file)
    fput(b->vma.vm_file);
  kfree(b);
}

/* VM_DONTEXPAND lives only in the Linux-facing vm_area_struct; native mremap
 * asks through here so GEM/dma-buf mappings reject with -EINVAL like upstream
 * instead of going through the native remove/re-add paths. */
bool linuxkpi_vma_no_expand(void *w) {
  struct kpi_mmap_bridge *b = w;

  return b && (b->vma.vm_flags & VM_DONTEXPAND);
}

/* After mremap grew or moved the native VMA, the wrapper created for the new
 * pages only covers that sub-range.  Rebase it onto the whole native node and
 * restore the original file offset so fault-time pgoff stays correct. */
void linuxkpi_vma_rebase(void *w, unsigned long start, unsigned long end,
                         unsigned long pgoff) {
  struct kpi_mmap_bridge *b = w;

  if (!b)
    return;
  __atomic_add_fetch(&b->seq, 1, __ATOMIC_RELEASE);
  __atomic_thread_fence(__ATOMIC_RELEASE);
  b->vma.vm_start = start;
  b->vma.vm_end = end;
  b->vma.vm_pgoff = pgoff >> PAGE_SHIFT;
  __atomic_thread_fence(__ATOMIC_RELEASE);
  __atomic_add_fetch(&b->seq, 1, __ATOMIC_RELEASE);
}

/* Address-space matching for unmap_mapping_range(): the VMA's file mapping
 * (NULL when the backing file has no inode mapping yet). */
void *linuxkpi_vma_mapping(void *w) {
  struct kpi_mmap_bridge *b = w;

  return (b && b->vma.vm_file) ? b->vma.vm_file->f_mapping : NULL;
}

bool linuxkpi_vma_is_shared(void *w) {
  struct kpi_mmap_bridge *b = w;

  return b && (b->vma.vm_flags & VM_SHARED);
}

static unsigned long kpi_prot_to_vm_flags(uint64_t prot, uint64_t flags) {
  unsigned long vm = 0;

  if (prot & PROT_READ)
    vm |= VM_READ | VM_MAYREAD;
  if (prot & PROT_WRITE)
    vm |= VM_WRITE | VM_MAYWRITE;
  if (prot & PROT_EXEC)
    vm |= VM_EXEC | VM_MAYEXEC;
  if (flags & MAP_SHARED)
    vm |= VM_SHARED | VM_MAYSHARE;
  return vm;
}

unsigned long linuxkpi_file_mmap(void *node, unsigned long addr,
                                 unsigned long length, unsigned long prot,
                                 unsigned long flags, unsigned long offset) {
  return linuxkpi_file_mmap_file(kpi_node_file(node), addr, length, prot, flags, offset);
}

unsigned long linuxkpi_file_mmap_file(void *opaque_file, unsigned long addr,
                                      unsigned long length, unsigned long prot,
                                      unsigned long flags, unsigned long offset) {
  struct file *file = opaque_file;
  struct kpi_mmap_bridge *bridge;
  int ret;

  if (!file || !file->f_op || !file->f_op->mmap)
    return (unsigned long)-ENODEV;

  bridge = kzalloc(sizeof(*bridge), GFP_KERNEL);
  if (!bridge)
    return (unsigned long)-ENOMEM;

  bridge->refs = 1;
  bridge->closed = false;
  bridge->vma.vm_start = addr;
  bridge->vma.vm_end = addr + length;
  bridge->vma.vm_pgoff = offset >> PAGE_SHIFT;
  bridge->vma.vm_flags = kpi_prot_to_vm_flags(prot, flags);
  bridge->vma.vm_page_prot = __pgprot(0);
  bridge->vma.vm_file = get_file(file);

  ret = file->f_op->mmap(file, &bridge->vma);
  if (ret) {
    fput(bridge->vma.vm_file);
    kfree(bridge);
    return (unsigned long)(ret < 0 ? ret : -EINVAL);
  }

  linuxkpi_vma_set_pending(bridge);
  return addr;
}

void linuxkpi_file_close(void *node) {
  struct file *file = kpi_node_file(node);

  if (file) {
    linuxkpi_poll_detach(&file->f_poll_links, node);
    /* vfs_close() is tearing the node down: the file must not release it
     * again when its own refcount reaches zero. */
    if (file->f_asc_node == node)
      file->f_asc_node = NULL;
    fput(file);
  }
}

/* ── fault bridge (called from the native page-fault path) ──────────────── */

#define KPI_VM_FAULT_HANDLED 0
#define KPI_VM_FAULT_REJECT (-1)

int linuxkpi_vma_fault(void *w, unsigned long addr, unsigned long error) {
  struct kpi_mmap_bridge *b = w;
  struct vm_fault vmf;
  unsigned long aligned;
  unsigned seq0, seq1;
  int spins, ret;
  extern void zap_vma_ptes(struct vm_area_struct *vma, unsigned long address,
                           unsigned long size);

  if (!b)
    return KPI_VM_FAULT_REJECT;
  if (!b->vma.vm_ops || !b->vma.vm_ops->fault)
    return KPI_VM_FAULT_REJECT;

  /* Wait out a concurrent rebase (3 stores, no sleep) so the triple below
   * is consistent. Bounded: a stuck writer must not hang the fault. */
  for (spins = 0; spins < 10000; spins++) {
    seq0 = __atomic_load_n(&b->seq, __ATOMIC_ACQUIRE);
    if (!(seq0 & 1))
      break;
    __asm__ volatile("pause");
  }
  seq0 = __atomic_load_n(&b->seq, __ATOMIC_ACQUIRE);
  if (seq0 & 1)
    return KPI_VM_FAULT_REJECT; /* never consume an unfinished rebase */

  /* Linux's handle_mm_fault() passes a page-aligned address to
   * vm_ops->fault(); hardware CR2 carries the exact faulting offset.  A
   * handler that inserts a PTE for the page (TTM's does) rejects the
   * unaligned address when it is in the last page of the VMA
   * (addr + PAGE_SIZE > vm_end -> SIGBUS -> NOPAGE -> re-fault livelock). */
  aligned = addr & PAGE_MASK;
  if (aligned < b->vma.vm_start || aligned >= b->vma.vm_end)
    return KPI_VM_FAULT_REJECT;

  __builtin_memset(&vmf, 0, sizeof(vmf));
  vmf.vma = &b->vma;
  vmf.address = aligned;
  vmf.pgoff = b->vma.vm_pgoff +
              ((aligned - b->vma.vm_start) >> PAGE_SHIFT);
  vmf.flags = (error & 0x2) ? FAULT_FLAG_WRITE : 0;

  ret = b->vma.vm_ops->fault(&vmf);
  gpu_diag_record(GPU_DIAG_FAULT,b,aligned,vmf.pgoff,seq0,error,ret);
  if (gpu_diag_resource_enabled())
    gpu_diag_record(GPU_DIAG_CPU_VMA, b, (uintptr_t)b->vma.vm_private_data,
                    aligned, vmf.pgoff, error, ret);
  if (ret & (VM_FAULT_ERROR))
    return KPI_VM_FAULT_REJECT;
  /* The driver slept (TTM dma-fence wait) with IRQs open; a concurrent
   * mremap rebase in that window leaves a PTE for the wrong texture page
   * behind (sparse purple/green texels until a later refault corrects it).
   * Drop that translation and refault with the new triple instead. */
  seq1 = __atomic_load_n(&b->seq, __ATOMIC_ACQUIRE);
  if (seq0 != seq1) {
    zap_vma_ptes(&b->vma, aligned, PAGE_SIZE);
    return KPI_VM_FAULT_REJECT;
  }
  return KPI_VM_FAULT_HANDLED;
}
