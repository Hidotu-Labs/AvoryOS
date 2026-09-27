// precache.c — Fast VFS page-cache warming for the dynamic linker.
//
// Pre-loads only the core C runtime / dynamic linker (/lib/ld-musl-x86_64.so.1)
// into the VFS page cache at boot (~1.5 MiB, < 1ms overhead).
// All other programs and shared libraries are demand-cached on their first run.

#include "precache.h"
#include "vfs.h"
#include "../console/klog.h"
#include "../mm/pmm.h"

static const char *const essential_files[] = {
    "/lib/ld-musl-x86_64.so.1",
    "/lib/libc.musl-x86_64.so.1",
    NULL
};

static unsigned int precache_file(const char *path) {
  vfs_node_t *node = vfs_resolve_path(path);
  if (!node)
    return 0;

  if ((node->flags & FS_TYPE_MASK) != FS_FILE || node->length == 0) {
    vfs_close(node);
    return 0;
  }

  unsigned int pages_loaded = 0;
  uint32_t offset = 0;

  while (offset < node->length) {
    vfs_page_t *page = vfs_cache_get_or_create(node, offset);
    if (page) {
      vfs_cache_put(node, page);
      pages_loaded++;
    }
    offset += PAGE_SIZE;
  }

  vfs_close(node);
  return pages_loaded;
}

void precache_hot_files(void) {
  unsigned int total_pages = 0;

  for (int i = 0; essential_files[i] != NULL; i++) {
    total_pages += precache_file(essential_files[i]);
  }

  if (total_pages > 0) {
    klog_puts("[precache] Dynamic linker pre-cached: ");
    klog_uint64(total_pages);
    klog_puts(" pages (");
    klog_uint64((uint64_t)total_pages * (PAGE_SIZE / 1024));
    klog_puts(" KiB)\n");
  }
}