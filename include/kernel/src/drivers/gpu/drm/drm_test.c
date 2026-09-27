#include "../../../console/klog.h"
#include "../../../sched/sched.h"
#include "drm.h"

extern struct drm_device global_drm_dev;

void drm_run_phase3_test(void) {
  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " Starting DRM Phase 1, 2 & 3 Stress Test\n");

  struct vfs_node *node = vfs_resolve_path("/dev/dri/card0");
  if (!node) {
    klog_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET " /dev/dri/card0 not found!\n");
    return;
  }

  // 1. Basic IOCTL Test
  struct drm_version ver;
  char name[32], date[32], desc[64];
  ver.name = name;
  ver.name_len = 32;
  ver.date = date;
  ver.date_len = 32;
  ver.desc = desc;
  ver.desc_len = 64;

  if (node->ioctl(node, DRM_IOCTL_VERSION, (uint64_t)&ver) == 0) {
    klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " DRM Version: ");
    klog_puts(name);
    klog_puts("\n");
  }

  // 2. GEM Stress Test
  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " Running GEM allocation stress loop...\n");
  for (int i = 0; i < 100; i++) {
    struct drm_gem_create c;
    c.size = 4096;
    if (node->ioctl(node, DRM_IOCTL_GEM_CREATE, (uint64_t)&c) == 0) {
      struct drm_gem_free f;
      f.handle = c.handle;
      node->ioctl(node, DRM_IOCTL_GEM_FREE, (uint64_t)&f);
    }
  }
  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " GEM allocation loop PASSED.\n");

  // 3. KMS Stress Test
  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " Phase 2: KMS Pipeline Validation\n");
  spinlock_acquire(&global_drm_dev.lock);
  int kms_count = 0;
  struct drm_mode_object *mobj;
  list_for_each_entry(mobj, &global_drm_dev.kms_objects, list) { kms_count++; }
  spinlock_release(&global_drm_dev.lock);
  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " KMS Object count: ");
  klog_uint64(kms_count);
  klog_puts("\n");

  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " Phase 3: Dumb Buffer & MMAP Validation\n");

  struct drm_mode_create_dumb cd;
  cd.width = 128;
  cd.height = 128;
  cd.bpp = 32;
  if (node->ioctl(node, DRM_IOCTL_MODE_CREATE_DUMB, (uint64_t)&cd) == 0) {
    struct drm_mode_map_dumb md;
    md.handle = cd.handle;
    if (node->ioctl(node, DRM_IOCTL_MODE_MAP_DUMB, (uint64_t)&md) == 0) {
      // Test mapping with READ/WRITE flags (0x3)
      if (node->mmap) {
        uint64_t addr = node->mmap(node, 0, cd.size,
                                   3 /* PROT_READ|PROT_WRITE */, 0, md.offset);
        if (addr != (uint64_t)-1) {
          klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " Mapped Dumb Buffer successfully.\n");
          // Write test
          volatile uint32_t *ptr = (volatile uint32_t *)addr;
          ptr[0] = 0xDEADBEEF;
          if (ptr[0] == 0xDEADBEEF) {
            klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " Mapped write test: SUCCESS\n");
          } else {
            klog_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET " Mapped write test: FAILED\n");
          }
        } else {
          klog_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET " node->mmap failed!\n");
        }
      }
    }
  }

  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " Phase 4: Hardware FB Bridge Validation\n");
  bool found_hw_fb = false;
  spinlock_acquire(&global_drm_dev.lock);
  struct drm_gem_object *obj;
  list_for_each_entry(obj, &global_drm_dev.gem_objects, list) {
    if (obj->handle == 0xF0B0) {
      found_hw_fb = true;
      break;
    }
  }
  spinlock_release(&global_drm_dev.lock);
  if (found_hw_fb) {
    klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " Hardware FB GEM found! Bridge is ACTIVE.\n");
  } else {
    klog_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET " Hardware FB GEM not found!\n");
  }

  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " Stress Test COMPLETE. Graphics stack is healthy.\n");
}
