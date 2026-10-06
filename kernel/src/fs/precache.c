// precache.c — Fast VFS page-cache warming for the dynamic linker + Plasma shell.
//
// Pre-loads the core C runtime / dynamic linker plus the plasmashell/kwin
// hot set at boot so the first startplasma-x11 hits hot cache instead of
// cold storage. Small files are read fully; large libs use readahead windows
// to bound boot time (< ~5ms extra on warm disk).

#include "precache.h"
#include "vfs.h"
#include "../console/klog.h"
#include "../mm/pmm.h"

static const char *const essential_files[] = {
    "/lib/ld-musl-x86_64.so.1",
    "/lib/libc.musl-x86_64.so.1",
    /* Plasma shell hot set: QML engine + shell binary + key frameworks.
     * Missing files are skipped (NULL return), so minimal images pay nothing. */
    "/usr/bin/plasmashell",
    "/usr/bin/kwin_x11",
    "/usr/lib/libQt6Qml.so.6",
    "/usr/lib/libQt6Quick.so.6",
    "/usr/lib/libKF6Plasma.so.6",
    "/usr/lib/libKF6WindowSystem.so.6",
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

  /* Large libs (>256KiB) are warmed with one readahead window instead of
   * page-by-page faults, so boot cost stays bounded. */
  if (node->length >= 256 * 1024) {
    pages_loaded += vfs_cache_readahead(node, 0, 256 * 1024) / PAGE_SIZE;
    vfs_close(node);
    return pages_loaded;
  }

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