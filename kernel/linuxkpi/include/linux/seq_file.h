#ifndef __AVORY_LINUXKPI_SEQ_FILE_H
#define __AVORY_LINUXKPI_SEQ_FILE_H

/* AvoryOS overlay for <linux/seq_file.h>: just enough for the *_describe()
 * helpers compiled in dma-fence.c/dma-resv.c.  No /proc integration. */

#include <linux/string_helpers.h>
#include <linux/types.h>
#include <linux/fs.h>

struct seq_file {
  char *buf;
  size_t size;
  size_t from;
  size_t count;
  void *private;
  void *file;
};

int seq_printf(struct seq_file *m, const char *f, ...)
    __attribute__((format(printf, 2, 3)));
int seq_puts(struct seq_file *m, const char *s);
int seq_putc(struct seq_file *m, char c);
int seq_write(struct seq_file *seq, const void *data, size_t len);

/* DEBUG_FS is disabled. Stock DRM still declares these operations for its
 * unreachable debugfs files; retain the real API, without success stubs. */
int single_open(struct file *, int (*show)(struct seq_file *, void *), void *);
int single_release(struct inode *, struct file *);
ssize_t seq_read(struct file *, char __user *, size_t, loff_t *);
loff_t seq_lseek(struct file *, loff_t, int);
#define DEFINE_SHOW_ATTRIBUTE(name)                                           \
  static int name##_open(struct inode *inode, struct file *file) {              \
    return single_open(file, name##_show, inode->i_private);                    \
  }                                                                           \
  static const struct file_operations name##_fops = {                           \
    .open = name##_open, .read = seq_read, .llseek = seq_lseek,                   \
    .release = single_release,                                                 \
  }

#endif /* __AVORY_LINUXKPI_SEQ_FILE_H */
