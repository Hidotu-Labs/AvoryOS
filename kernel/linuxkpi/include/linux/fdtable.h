#ifndef __AVORY_LINUXKPI_FDTABLE_H
#define __AVORY_LINUXKPI_FDTABLE_H

/* AvoryOS overlay for <linux/fdtable.h>: descriptor allocation on top of the
 * native per-process fd table (kernel/linuxkpi/src/file.c). */

struct file;

int get_unused_fd_flags(unsigned int flags);
void put_unused_fd(unsigned int fd);
void fd_install(unsigned int fd, struct file *file);
int close_fd(unsigned int fd);

#endif /* __AVORY_LINUXKPI_FDTABLE_H */
