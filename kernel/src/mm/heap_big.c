#include "mm/heap_internal.h"
#include "lib/string.h"
#include "mm/pmm.h"

spinlock_t heap_vmem_lock = SPINLOCK_INIT;
struct big_alloc *heap_big_alloc_head = NULL;

void heap_big_init(void) {
  spinlock_init(&heap_vmem_lock);
  heap_big_alloc_head = NULL;
}

void *heap_big_alloc(size_t size) {
  size_t total_size = size + sizeof(struct big_alloc);
  size_t pages = (total_size + PAGE_SIZE - 1) / PAGE_SIZE;

  void *blocks = pmm_alloc_pages(pages);
  if (!blocks) {
    heap_oom_reclaim();
    blocks = pmm_alloc_pages(pages);
    if (!blocks)
      return NULL;
  }

  uint64_t vaddr = (uint64_t)blocks + pmm_get_hhdm_offset();
  struct big_alloc *b = (struct big_alloc *)vaddr;
  b->magic = BIG_MAGIC;
  b->pages = pages;

  spinlock_acquire(&heap_vmem_lock);
  b->next = heap_big_alloc_head;
  b->prev = NULL;
  if (heap_big_alloc_head)
    heap_big_alloc_head->prev = b;
  heap_big_alloc_head = b;
  spinlock_release(&heap_vmem_lock);

  return (void *)((uint8_t *)b + sizeof(struct big_alloc));
}

bool heap_big_free(void *ptr, uint64_t page_base) {
  (void)ptr;
  struct big_alloc *b = (struct big_alloc *)page_base;

  spinlock_acquire(&heap_vmem_lock);
  if (b->prev)
    b->prev->next = b->next;
  else
    heap_big_alloc_head = b->next;
  if (b->next)
    b->next->prev = b->prev;

  size_t pages = b->pages;
  b->magic = 0;
  spinlock_release(&heap_vmem_lock);

  uint64_t phys_addr = page_base - pmm_get_hhdm_offset();
  pmm_free_pages((void *)phys_addr, pages);
  return true;
}

size_t heap_big_ksize(const void *ptr, uint64_t page_base) {
  struct big_alloc *b = (struct big_alloc *)page_base;
  if ((uint64_t)ptr != page_base + sizeof(struct big_alloc))
    return 0;
  if (b->pages * PAGE_SIZE < sizeof(struct big_alloc))
    return 0;
  return (b->pages * PAGE_SIZE) - sizeof(struct big_alloc);
}

static void heap_u64_to_str(uint64_t val, char *buf) {
  if (val == 0) {
    buf[0] = '0';
    buf[1] = '\0';
    return;
  }
  char temp[32];
  int i = 0;
  while (val > 0) {
    temp[i++] = (val % 10) + '0';
    val /= 10;
  }
  int j = 0;
  while (i > 0) {
    buf[j++] = temp[--i];
  }
  buf[j] = '\0';
}

void heap_big_get_info(char *buf) {
  char num_buf[32];
  spinlock_acquire(&heap_vmem_lock);
  strcat(buf, "\nBig Allocations:\n");
  uint32_t big_count = 0;
  uint64_t big_pages = 0;
  for (struct big_alloc *b = heap_big_alloc_head; b; b = b->next) {
    big_count++;
    big_pages += b->pages;
  }

  strcat(buf, "  Count: ");
  heap_u64_to_str(big_count, num_buf);
  strcat(buf, num_buf);
  strcat(buf, "\n");

  strcat(buf, "  Total Pages: ");
  heap_u64_to_str(big_pages, num_buf);
  strcat(buf, num_buf);
  strcat(buf, "\n");

  strcat(buf, "  Total Size: ");
  heap_u64_to_str(big_pages * PAGE_SIZE / 1024, num_buf);
  strcat(buf, num_buf);
  strcat(buf, " kB\n");
  spinlock_release(&heap_vmem_lock);
}
