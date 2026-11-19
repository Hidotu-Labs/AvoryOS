#include "random.h"
#include "hal/hal.h"
#include "../fb/framebuffer.h"
#include "../lib/csprng.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "vfs.h"

static uint32_t random_read(struct vfs_node *node, uint32_t offset,
                            uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;
  if (buffer && size)
    csprng_get_bytes(buffer, size);
  return size;
}

static uint32_t random_write(struct vfs_node *node, uint32_t offset,
                             uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;
  if (buffer && size)
    csprng_stir(buffer, size);
  return size;
}

static uint32_t null_read(struct vfs_node *node, uint32_t offset,
                          uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;
  (void)size;
  (void)buffer;
  return 0; // EOF immediately
}

static uint32_t null_write(struct vfs_node *node, uint32_t offset,
                           uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;
  (void)buffer;
  return size; // Discard data and succeed
}

static int null_poll(struct vfs_node *node, int events) {
  (void)node;
  return events & (POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM);
}

static uint32_t zero_read(struct vfs_node *node, uint32_t offset,
                          uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;
  if (buffer && size) {
    memset(buffer, 0, size);
  }
  return size;
}

static uint32_t zero_write(struct vfs_node *node, uint32_t offset,
                           uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;
  (void)buffer;
  return size;
}

static uint32_t full_write(struct vfs_node *node, uint32_t offset,
                            uint32_t size, uint8_t *buffer) {
  (void)node; (void)offset; (void)buffer;
  return size ? (uint32_t)-28 : 0; /* ENOSPC */
}

static int zero_poll(struct vfs_node *node, int events) {
  (void)node;
  return events & (POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM);
}

static uint64_t zero_mmap(struct vfs_node *node, uint64_t addr, uint64_t length,
                          uint64_t prot, uint64_t flags, uint64_t offset) {
  (void)node;
  (void)flags;
  (void)offset;
  uint64_t *user_pml4 = vmm_get_active_pml4();
  if (!user_pml4 || !addr || !length)
    return (uint64_t)-1;
  if (length + 0xFFFULL < length)
    return (uint64_t)-1;

  size_t pages = (length + 0xFFFULL) / 0x1000ULL;
  uint64_t map_bytes = (uint64_t)pages * 0x1000ULL;
  if (addr > USER_SPACE_LIMIT || map_bytes > USER_SPACE_LIMIT + 1ULL - addr)
    return (uint64_t)-1;

  uint64_t page_flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
  if (prot & 0x2)
    page_flags |= PAGE_FLAG_RW;
  if (!(prot & 0x4))
    page_flags |= PAGE_FLAG_NX;

  for (size_t i = 0; i < pages; i++) {
    void *phys = pmm_alloc();
    if (!phys) {
      for (size_t j = 0; j < i; j++) {
        uint64_t p = vmm_virt_to_phys(user_pml4, addr + j * 0x1000ULL);
        vmm_unmap_page(user_pml4, addr + j * 0x1000ULL);
        if (p)
          pmm_free_page((void *)p);
      }
      return (uint64_t)-1;
    }
    memset((void *)((uint64_t)phys + pmm_get_hhdm_offset()), 0, 4096);
    if (!vmm_map_page(user_pml4, addr + (i * 0x1000), (uint64_t)phys, page_flags)) {
      pmm_free_page(phys);
      for (size_t j = 0; j < i; j++) {
        uint64_t p = vmm_virt_to_phys(user_pml4, addr + j * 0x1000ULL);
        vmm_unmap_page(user_pml4, addr + j * 0x1000ULL);
        if (p)
          pmm_free_page((void *)p);
      }
      return (uint64_t)-1;
    }
  }
  return addr;
}

void random_register_vfs(void) {
  // /dev/null
  vfs_node_t *null_node = kmalloc(sizeof(vfs_node_t));
  if (null_node) {
    vfs_node_init(null_node);
    strcpy(null_node->name, "null");
    null_node->flags = FS_CHARDEV | FS_PERSISTENT;
    null_node->inode = (1U << 8) | 3U;
    null_node->mask = 0666;
    null_node->read = null_read;
    null_node->write = null_write;
    null_node->poll = null_poll;
    fb_register_device_node("null", null_node);
  }

  // /dev/zero
  vfs_node_t *zero_node = kmalloc(sizeof(vfs_node_t));
  if (zero_node) {
    vfs_node_init(zero_node);
    strcpy(zero_node->name, "zero");
    zero_node->flags = FS_CHARDEV | FS_PERSISTENT;
    zero_node->inode = (1U << 8) | 5U;
    zero_node->mask = 0666;
    zero_node->read = zero_read;
    zero_node->write = zero_write;
    zero_node->poll = zero_poll;
    zero_node->mmap = zero_mmap;
    fb_register_device_node("zero", zero_node);
  }

  // /dev/full: reads zeroes and reports ENOSPC for every nonempty write.
  vfs_node_t *full_node = kmalloc(sizeof(vfs_node_t));
  if (full_node) {
    vfs_node_init(full_node);
    strcpy(full_node->name, "full");
    full_node->flags = FS_CHARDEV | FS_PERSISTENT;
    full_node->inode = (1U << 8) | 7U;
    full_node->mask = 0666;
    full_node->read = zero_read;
    full_node->write = full_write;
    full_node->poll = zero_poll;
    fb_register_device_node("full", full_node);
  }

  // /dev/urandom
  vfs_node_t *unode = kmalloc(sizeof(vfs_node_t));
  if (unode) {
    vfs_node_init(unode);
    strcpy(unode->name, "urandom");
    unode->flags = FS_CHARDEV | FS_PERSISTENT;
    unode->inode = (1U << 8) | 9U;
    unode->mask = 0666;
    unode->read = random_read;
    unode->write = random_write;
    unode->poll = null_poll;
    fb_register_device_node("urandom", unode);
  }

  // /dev/random
  vfs_node_t *rnode = kmalloc(sizeof(vfs_node_t));
  if (rnode) {
    vfs_node_init(rnode);
    strcpy(rnode->name, "random");
    rnode->flags = FS_CHARDEV | FS_PERSISTENT;
    rnode->inode = (1U << 8) | 8U;
    rnode->mask = 0666;
    rnode->read = random_read;
    rnode->write = random_write;
    rnode->poll = null_poll;
    fb_register_device_node("random", rnode);
  }
}
