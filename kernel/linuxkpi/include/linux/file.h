#ifndef __AVORY_LINUXKPI_FILE_H
#define __AVORY_LINUXKPI_FILE_H

/* AvoryOS overlay for <linux/file.h>: fd-table access over the native
 * descriptor table (kernel/linuxkpi/src/file.c). */

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/err.h>
#include <linux/fdtable.h>
#include <linux/cleanup.h>

struct fd {
  struct file *file;
  unsigned int flags;
};

#define FDPUT_FPUT 1
#define FDPUT_POS_UNLOCK 2

static inline int file_count(struct file *file) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
  return file_ref_read(&file->f_ref);
#else
  return atomic_read(&file->f_count);
#endif
}

struct file *fget(unsigned int fd);
struct file *fget_raw(unsigned int fd);
struct fd fdget(unsigned int fd);
void fdput(struct fd fd);
static inline struct file *fd_file(struct fd fd) { return fd.file; }
static inline bool fd_empty(struct fd fd) { return !fd.file; }
DEFINE_CLASS(fd, struct fd, fdput(_T), fdget(fd), unsigned int fd)
void fput_many(struct file *file, unsigned int refs);

static inline bool fd_is_open(unsigned int fd) {
  struct fd f = fdget(fd);
  bool ret = !!f.file;
  fdput(f);
  return ret;
}

#endif /* __AVORY_LINUXKPI_FILE_H */
