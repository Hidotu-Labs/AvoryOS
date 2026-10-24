#ifndef LINUXKPI_NATIVE_EVENTFD_H
#define LINUXKPI_NATIVE_EVENTFD_H

/* A referenced native vnode keeps the eventfd alive after userspace closes
 * or reuses its descriptor. Release it after the fence notification. */
int asc_eventfd_get(int fd, void **node);
void asc_eventfd_put(void *node);
void asc_eventfd_signal(void *node, unsigned int mask);

#endif
