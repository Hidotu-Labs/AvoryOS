#ifndef __AVORY_LINUXKPI_FS_H
#define __AVORY_LINUXKPI_FS_H

/* AvoryOS overlay for <linux/fs.h>.
 *
 * The native VFS is not a Linux VFS, so this provides only the Linux-facing
 * file/inode/dentry/path objects imported drivers touch, all bridged to
 * native `struct vfs_node` descriptors by linuxkpi/src/file.c:
 *
 *   struct file  -- a real (bridged) open file; f_count owns its lifetime.
 *   struct inode -- allocated by the pseudo-fs shim for dma-buf; only i_ino,
 *                   i_size/i_bytes and i_private carry meaning.
 *   struct dentry -- a thin object owning d_fsdata and d_op->d_release; the
 *                   bridge calls d_release when the last file reference goes.
 */

#include <linux/types.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/compiler.h>
#include <linux/kdev_t.h>
#include <linux/mutex.h>
#include <linux/xarray.h>
#include <uapi/linux/fcntl.h>

struct module;
struct inode;
struct dentry;
struct vfsmount;
struct file;
struct kiocb;
struct poll_table_struct;
struct vm_area_struct;
struct address_space;
struct super_block;
struct file_system_type;
struct fs_context;
struct seq_file;

typedef u32 fmode_t;

#define FMODE_READ 0x1
#define FMODE_WRITE 0x2
#define FMODE_LSEEK 0x4
#define FMODE_PREAD 0x8
#define FMODE_PWRITE 0x10
#define FMODE_EXEC 0x20
#define FMODE_NOWAIT 0x40
#define FMODE_CAN_READ 0x100
#define FMODE_CAN_WRITE 0x200
#define FMODE_UNSIGNED_OFFSET 0x400

/* i_flags, the small subset imported code checks. */
#define S_IFMT 00170000
#define S_IFSOCK 0140000
#define S_IFLNK 0120000
#define S_IFREG 0100000
#define S_IFBLK 0060000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFIFO 0010000
#define S_ISUID 0004000
#define S_ISGID 0002000
#define S_ISVTX 0001000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

struct qstr {
  const unsigned char *name;
  unsigned int len;
  unsigned int hash;
};

struct path {
  struct vfsmount *mnt;
  struct dentry *dentry;
};

/* Address space: the page-cache side of an inode.  AvoryOS uses it for
 * shmem-backed GEM objects (linuxkpi/src/shmem.c) and pairs it with the
 * imported xarray implementation for page lookup.  Order matches upstream
 * closely enough for the helpers; a_ops is unused so far. */
struct address_space {
  struct inode *host;
  struct xarray i_pages;
  gfp_t gfp_mask;
  atomic_long_t nrpages;
  unsigned long flags;
  const struct address_space_operations *a_ops;
  void *private_data;
};

struct inode {
  unsigned long i_ino;
  loff_t i_size;
  unsigned long i_bytes;
  unsigned long i_flags;
  dev_t i_rdev; /* device node number (iminor/imajor) */
  struct super_block *i_sb;
  void *i_private;
  atomic_t i_count;
  spinlock_t i_lock;
  struct address_space *i_mapping;
};

static inline void inode_set_bytes(struct inode *inode, loff_t n) {
  inode->i_bytes = (unsigned long)n;
}

struct dentry {
  struct qstr d_name;
  void *d_fsdata;
  const struct dentry_operations *d_op;
  struct inode *d_inode;
  struct dentry *d_parent;
  unsigned int d_flags;
  atomic_t d_count;
};

struct dentry_operations {
  int (*d_revalidate)(struct dentry *, unsigned int);
  int (*d_weak_revalidate)(struct dentry *, unsigned int);
  int (*d_hash)(const struct dentry *, struct qstr *);
  int (*d_compare)(const struct dentry *, unsigned int, const char *,
                   const struct qstr *);
  int (*d_delete)(const struct dentry *);
  int (*d_init)(struct dentry *);
  void (*d_release)(struct dentry *);
  void (*d_prune)(struct dentry *);
  void (*d_iput)(struct dentry *, struct inode *);
  char *(*d_dname)(struct dentry *, char *, int);
};

struct file_operations {
  struct module *owner;
  loff_t (*llseek)(struct file *, loff_t, int);
  ssize_t (*read)(struct file *, char __user *, size_t, loff_t *);
  ssize_t (*write)(struct file *, const char __user *, size_t, loff_t *);
  ssize_t (*read_iter)(struct kiocb *, void *);
  ssize_t (*write_iter)(struct kiocb *, void *);
  int (*iopoll)(struct kiocb *kiocb, void *poll_table, bool spin);
  int (*iterate_shared)(struct file *, void *);
  __poll_t (*poll)(struct file *, struct poll_table_struct *);
  long (*unlocked_ioctl)(struct file *, unsigned int, unsigned long);
  long (*compat_ioctl)(struct file *, unsigned int, unsigned long);
  int (*mmap)(struct file *, struct vm_area_struct *);
  int (*open)(struct inode *, struct file *);
  int (*flush)(struct file *, void *);
  int (*release)(struct inode *, struct file *);
  int (*fsync)(struct file *, loff_t, loff_t, int datasync);
  int (*fasync)(int, struct file *, int);
  void (*show_fdinfo)(struct seq_file *m, struct file *filp);
};

struct file {
  const struct file_operations *f_op;
  void *private_data;
  struct inode *f_inode;
  struct path f_path;
  unsigned int f_flags;
  fmode_t f_mode;
  atomic_t f_count;
  loff_t f_pos;
  struct address_space *f_mapping; /* == file_inode(file)->i_mapping */
  void *f_asc_node; /* native vfs_node_t * (Avory bridge, opaque here) */
  void *f_poll_wq;  /* native wait_queue_t * backing poll(2) (Avory) */
};

static inline struct inode *file_inode(const struct file *f) {
  return f->f_inode;
}

struct file *get_file(struct file *f);
void fput(struct file *f);

long compat_ptr_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

char *file_path(struct file *file, char *buf, int buflen);

/* ── device numbers ─────────────────────────────────────────────────────── */

/* MINORBITS/MAJOR/MINOR/MKDEV come from the imported <linux/kdev_t.h>. */

/* As upstream: imajor()/iminor() take an inode (the DRM core calls them on
 * file_inode(file)); the raw dev_t helpers are MAJOR()/MINOR(). */
static inline unsigned int imajor(const struct inode *inode) {
  return MAJOR(inode->i_rdev);
}
static inline unsigned int iminor(const struct inode *inode) {
  return MINOR(inode->i_rdev);
}

/* old_encode_dev()/old_decode_dev()/new_encode_dev() come from the imported
 * <linux/kdev_t.h>, as upstream. */

/* ── file_operations helpers (upstream fs.h / fs/open.c) ────────────────── */

/* No modules: a file's f_op never changes owner. */
static inline const struct file_operations *fops_get(
    const struct file_operations *fops) {
  return fops;
}

static inline void replace_fops(struct file *filp,
                                const struct file_operations *fops) {
  filp->f_op = fops;
}

static inline loff_t noop_llseek(struct file *file, loff_t offset, int whence) {
  (void)offset;
  (void)whence;
  return file->f_pos;
}

/* Legacy char-device registration (upstream fs.h).  Only the DRM legacy
 * minor path uses these and AvoryOS has no major registry, so the
 * implementations in linuxkpi/src/drm_compat.c refuse politely. */
int register_chrdev(unsigned int major, const char *name,
                    const struct file_operations *fops);
void unregister_chrdev(unsigned int major, const char *name);

struct file *file_clone_open(struct file *file);

#endif /* __AVORY_LINUXKPI_FS_H */
