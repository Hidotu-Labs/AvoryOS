#ifndef __AVORY_LINUXKPI_FILE_H
#define __AVORY_LINUXKPI_FILE_H

/* AvoryOS overlay for <linux/file.h>: fd-table access over the native
 * descriptor table (kernel/linuxkpi/src/file.c). */

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/err.h>
#include <linux/fdtable.h>

struct fd {
  struct file *file;
  unsigned int flags;
};

#define FDPUT_FPUT 1
#define FDPUT_POS_UNLOCK 2

static inline int file_count(struct file *file) {
  return atomic_read(&file->f_count);
}

struct file *fget(unsigned int fd);
struct file *fget_raw(unsigned int fd);
struct fd fdget(unsigned int fd);
void fdput(struct fd fd);
void fput_many(struct file *file, unsigned int refs);

static inline bool fd_is_open(unsigned int fd) {
  struct fd f = fdget(fd);
  bool ret = !!f.file;
  fdput(f);
  return ret;
}

#endif /* __AVORY_LINUXKPI_FILE_H */
