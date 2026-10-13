#ifndef LINUXKPI_NATIVE_PRIME_H
#define LINUXKPI_NATIVE_PRIME_H

/* Opaque dma-buf bridge. Native DRM must not include Linux DRM headers. */
int linuxkpi_prime_export(void *object, __SIZE_TYPE__ size, void *reservation,
                         __UINT64_TYPE__ (*page_phys)(void *, unsigned int),
                         void (*release)(void *), unsigned int flags);
int linuxkpi_prime_get(int fd, void **buffer, __SIZE_TYPE__ *size);
void linuxkpi_prime_put(void *buffer);
void *linuxkpi_prime_native_object(void *buffer);
int linuxkpi_prime_fd(void *buffer, unsigned int flags);
int linuxkpi_prime_map(void *buffer, void **mapping, void **address);
void linuxkpi_prime_unmap(void *mapping);
__UINT64_TYPE__ linuxkpi_prime_page_phys(void *mapping, unsigned int page);
/* A successful begin holds the mapping mutex and buffer reservation until
 * end; pair them on the same thread. Exporter callbacks run unlocked. */
int linuxkpi_prime_begin_cpu(void *buffer, void *mapping);
void linuxkpi_prime_end_cpu(void *buffer, void *mapping);
void *linuxkpi_prime_reservation_new(void);
void linuxkpi_prime_reservation_free(void *reservation);
int linuxkpi_prime_reservation_wait(void *reservation);
__UINT64_TYPE__ linuxkpi_prime_mmap(void *buffer, __UINT64_TYPE__ addr,
                                  __UINT64_TYPE__ length, __UINT64_TYPE__ prot,
                                  __UINT64_TYPE__ flags, __UINT64_TYPE__ offset);
/* The file bridge records vm_ops/file references for imported GEM mappings. */
__UINT64_TYPE__ linuxkpi_file_mmap_file(void *file, __UINT64_TYPE__ addr,
                                      __UINT64_TYPE__ length, __UINT64_TYPE__ prot,
                                      __UINT64_TYPE__ flags, __UINT64_TYPE__ offset);
#endif
