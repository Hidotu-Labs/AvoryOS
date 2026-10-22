/* Minimal shmem-backed page store for drm_gem_shmem / drm_gem_get_pages.
 *
 * The DRM shmem helper creates a shmem file, then faults zeroed pages out of
 * its address_space with shmem_read_folio_gfp() and keeps them until
 * shmem_truncate_range().  AvoryOS has no tmpfs-in-the-Linux-VFS yet, so this
 * implements exactly that contract over the imported xarray and the native
 * PMM: pages are order-0 and zeroed. The mapping owns one reference and
 * every successful read returns a separate caller reference.
 *
 * The file returned by shmem_file_setup() is a bridged kernel-internal file
 * (no fd); drm_gem_object_release() fputs() it, and the file release hook
 * frees the address_space.  Pages are expected to have been truncated by then
 * (drm_gem_shmem_free does that before the fput). */

#include <linux/anon_inodes.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/xarray.h>

/* Keep lookup + get_page atomic against removal, and publish a newly
 * allocated page only once. A mutex allows allocation to sleep. */
static DEFINE_MUTEX(shmem_page_lock);

struct file *shmem_file_setup_with_mnt(struct vfsmount *mnt, const char *name,
                                      loff_t size, unsigned long flags) {
  /* A caller-supplied tmpfs mount needs mount policy and ownership that the
   * native anonymous page store cannot provide. Default GEM uses NULL. */
  if (mnt)
    return ERR_PTR(-EOPNOTSUPP);
  return shmem_file_setup(name, size, flags);
}

static void shmem_free_page(struct page *page) {
  if (!page)
    return;
  folio_lock(page_folio(page));
  page->mapping = NULL;
  folio_unlock(page_folio(page));
  put_page(page);
}

int shmem_writeout(struct folio *folio, struct swap_iocb **plug,
                   struct list_head *list) {
  (void)plug;
  (void)list;
  /* No native swap backend: keep the resident copy dirty and locked. The
   * caller handles the failure and retains the backup's ownership. */
  folio_mark_dirty(folio);
  return -EOPNOTSUPP;
}

/* Frees every page in [start, end] (page indices) and returns the count.
 * The xa_find() loop (rather than xa_for_each) keeps the iteration valid
 * while entries are being erased. */
static unsigned long shmem_truncate_pages(struct address_space *mapping,
                                          pgoff_t start, pgoff_t end) {
  struct page *page;
  unsigned long index = start;
  unsigned long freed = 0;

  if (!mapping || start > end)
    return 0;

  mutex_lock(&shmem_page_lock);
  while ((page = xa_find(&mapping->i_pages, &index, end, XA_PRESENT))) {
    if (xa_erase(&mapping->i_pages, index) == page) {
      shmem_free_page(page);
      freed++;
    }
    if (index == ULONG_MAX)
      break;
    index++;
  }
  if (freed) {
    if (atomic_long_read(&mapping->nrpages) >= (long)freed)
      atomic_long_sub(freed, &mapping->nrpages);
    else
      atomic_long_set(&mapping->nrpages, 0);
  }
  mutex_unlock(&shmem_page_lock);
  return freed;
}

void shmem_truncate_range(struct inode *inode, loff_t start, loff_t end) {
  pgoff_t first, last;

  if (!inode || !inode->i_mapping)
    return;
  first = start <= 0 ? 0 : (pgoff_t)(start >> PAGE_SHIFT);
  last = end < 0 ? ULONG_MAX : (pgoff_t)(end >> PAGE_SHIFT);
  shmem_truncate_pages(inode->i_mapping, first, last);
}

unsigned long invalidate_mapping_pages(struct address_space *mapping,
                                       pgoff_t start, pgoff_t end) {
  return shmem_truncate_pages(mapping, start, end);
}

struct folio *shmem_read_folio_gfp(struct address_space *mapping, pgoff_t index,
                                   gfp_t gfp) {
  struct page *page;
  int ret;

  if (!mapping)
    return ERR_PTR(-EINVAL);

  mutex_lock(&shmem_page_lock);
  page = xa_load(&mapping->i_pages, index);
  if (page) {
    get_page(page);
    mutex_unlock(&shmem_page_lock);
    return page_folio(page);
  }

  page = alloc_pages(gfp | __GFP_ZERO, 0);
  if (!page) {
    mutex_unlock(&shmem_page_lock);
    return ERR_PTR(-ENOMEM);
  }
  page->mapping = mapping;
  page->index = index;

  ret = xa_insert(&mapping->i_pages, index, page, GFP_KERNEL);
  if (ret) {
    shmem_free_page(page);
    mutex_unlock(&shmem_page_lock);
    return ERR_PTR(ret);
  }
  atomic_long_inc(&mapping->nrpages);
  get_page(page);
  mutex_unlock(&shmem_page_lock);
  return page_folio(page);
}

/* Stock <linux/shmem_fs.h> also exports the page-returning form; TTM's
 * shmem-backed ttm_tt uses it.  A folio is order-0 here, so it is the page. */
struct page *shmem_read_mapping_page_gfp(struct address_space *mapping,
                                         pgoff_t index, gfp_t gfp) {
  struct folio *folio = shmem_read_folio_gfp(mapping, index, gfp);

  if (IS_ERR(folio))
    return ERR_CAST(folio);
  return &folio->page;
}

static int shmem_file_release(struct inode *inode, struct file *file) {
  if (file && file->f_mapping) {
    shmem_truncate_pages(file->f_mapping, 0, ULONG_MAX);
    xa_destroy(&file->f_mapping->i_pages);
    kfree(file->f_mapping);
    file->f_mapping = NULL;
    if (inode)
      inode->i_mapping = NULL;
  }
  return 0;
}

static const struct file_operations shmem_file_ops = {
    .release = shmem_file_release,
};

struct file *shmem_file_setup(const char *name, loff_t size,
                              unsigned long flags) {
  struct address_space *mapping;
  struct inode *inode;
  struct file *file;

  (void)name;
  (void)flags;

  inode = kzalloc(sizeof(*inode), GFP_KERNEL);
  mapping = kzalloc(sizeof(*mapping), GFP_KERNEL);
  if (!inode || !mapping) {
    kfree(inode);
    kfree(mapping);
    return ERR_PTR(-ENOMEM);
  }

  inode->i_size = size;
  atomic_set(&inode->i_count, 1);
  spin_lock_init(&inode->i_lock);

  xa_init(&mapping->i_pages);
  mapping->host = inode;
  mapping->gfp_mask = GFP_HIGHUSER | __GFP_ZERO;
  inode->i_mapping = mapping;

  file = anon_inode_getfile("shmem", &shmem_file_ops, NULL, 0);
  if (IS_ERR(file) || !file) {
    xa_destroy(&mapping->i_pages);
    kfree(mapping);
    kfree(inode);
    return file ? file : ERR_PTR(-ENOMEM);
  }

  file->f_inode = inode;
  file->f_mapping = mapping;
  return file;
}
