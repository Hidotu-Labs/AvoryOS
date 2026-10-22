#ifndef __AVORY_LINUXKPI_SHMEM_FS_H
#define __AVORY_LINUXKPI_SHMEM_FS_H

/* AvoryOS overlay for <linux/shmem_fs.h>.
 *
 * There is no tmpfs-backed Linux file system in the LinuxKPI yet, but
 * drm_gem_shmem objects need an anonymous pageable backing with an
 * address_space.  linuxkpi/src/shmem.c implements that store over the native
 * PMM and the imported xarray: shmem_file_setup() returns a bridged struct
 * file whose f_mapping owns the pages, and shmem_read_folio_gfp() faults in
 * zeroed order-0 pages on demand.  The abstraction is deliberately the shmem
 * API so imported helpers stay verbatim. */

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/types.h>

struct file *shmem_file_setup(const char *name, loff_t size, unsigned long flags);
struct vfsmount;
struct file *shmem_file_setup_with_mnt(struct vfsmount *, const char *, loff_t,
                                      unsigned long);

struct folio *shmem_read_folio_gfp(struct address_space *mapping, pgoff_t index,
                                   gfp_t gfp);
static inline struct folio *shmem_read_folio(struct address_space *mapping,
                                             pgoff_t index) {
  return shmem_read_folio_gfp(mapping, index, mapping_gfp_mask(mapping));
}
struct swap_iocb;
int shmem_writeout(struct folio *, struct swap_iocb **, struct list_head *);

/* Stock <linux/shmem_fs.h> declares the page-returning form next to the folio
 * one; TTM's shmem-backed ttm_tt uses it. */
struct page *shmem_read_mapping_page_gfp(struct address_space *mapping,
                                         pgoff_t index, gfp_t gfp_mask);

void shmem_truncate_range(struct inode *inode, loff_t start, loff_t end);

#endif /* __AVORY_LINUXKPI_SHMEM_FS_H */
