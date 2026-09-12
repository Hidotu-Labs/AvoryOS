#ifndef __AVORY_LINUXKPI_SEQ_FILE_H
#define __AVORY_LINUXKPI_SEQ_FILE_H

/* AvoryOS overlay for <linux/seq_file.h>: just enough for the *_describe()
 * helpers compiled in dma-fence.c/dma-resv.c.  No /proc integration. */

#include <linux/string_helpers.h>
#include <linux/types.h>

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

#endif /* __AVORY_LINUXKPI_SEQ_FILE_H */
