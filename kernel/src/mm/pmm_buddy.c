#include "mm/pmm_internal.h"
#include "console/klog.h"
#include "lib/string.h"

struct buddy_zone b_zone;

static volatile uint64_t buddy_corruption_reports;

static void buddy_trace(struct buddy_zone *zone, uint64_t phys, size_t order,
                         const char *operation, void *caller) {
  struct buddy_trace_event *event =
      &zone->trace[zone->trace_seq++ % BUDDY_TRACE_SLOTS];
  *event = (struct buddy_trace_event){phys, (uint64_t)caller, order, operation};
}

static bool buddy_link_readable(const struct list_head *node,
                                const struct list_head *head) {
  return node == head ||
         (((uint64_t)node & (PAGE_SIZE - 1)) == 0 &&
          pmm_kernel_ptr_is_managed(node));
}

void buddy_report_corruption(const char *what, size_t order,
                            const struct list_head *node) {
  uint64_t n =
      __atomic_add_fetch(&buddy_corruption_reports, 1, __ATOMIC_RELAXED);
  if (n <= 8) {
    klog_puts(KLOG_CLR_RED "[PMM] CORRUPTION: " KLOG_CLR_RESET);
    klog_puts(what);
    klog_puts(" in free_list[");
    klog_uint64(order);
    klog_puts("] node=");
    klog_hex64((uint64_t)node);
    klog_puts(" caller=");
    klog_hex64((uint64_t)__builtin_return_address(0));
    klog_puts(" - cutting the list at the last good link\n");
  }
}

static const char *buddy_node_bad_reason(const struct buddy_zone *zone,
                                         const struct list_head *node,
                                         size_t order) {
  uint64_t addr = (uint64_t)node;
  const struct list_head *head = &zone->free_list[order];
  uint64_t block_size = (uint64_t)1 << (order + 12);
  const struct buddy_block *block = (const struct buddy_block *)node;

  if (!node)
    return "null node";
  if (!pmm_kernel_ptr_is_managed(node))
    return "node outside managed RAM";
  if (addr & (block_size - 1))
    return "node misaligned for order";
  if (block->order != order)
    return "order field mismatch (header overwritten?)";

  uint64_t pfn = (addr - pmm_physical_memory_offset) / PAGE_SIZE;
  if (pfn < pmm_lowest_page || pfn >= pmm_highest_page)
    return "pfn outside managed range";
  if (pmm_bitmap_test(pmm_bitmap, pfn - pmm_lowest_page))
    return "bitmap says allocated (double-owned or PCP-parked?)";

  uint64_t next = (uint64_t)block->node.next;
  uint64_t prev = (uint64_t)block->node.prev;
  if (next != (uint64_t)head && !pmm_kernel_ptr_is_managed((void *)next))
    return "next outside managed RAM";
  if (prev != (uint64_t)head && !pmm_kernel_ptr_is_managed((void *)prev))
    return "prev outside managed RAM";

  return "links not reciprocal";
}

static void buddy_report_node_detail(const struct buddy_zone *zone,
                                     const struct list_head *node,
                                     size_t order) {
  const struct buddy_block *block = (const struct buddy_block *)node;
  if (__atomic_load_n(&buddy_corruption_reports, __ATOMIC_RELAXED) > 8)
    return;
  klog_puts(" reason=");
  klog_puts(buddy_node_bad_reason(zone, node, order));
  if (pmm_kernel_ptr_is_managed(node)) {
    klog_puts(" stored_order=");
    klog_uint64(block->order);
    klog_puts(" next=");
    klog_hex64((uint64_t)block->node.next);
    klog_puts(" prev=");
    klog_hex64((uint64_t)block->node.prev);
    uint64_t addr = (uint64_t)node;
    uint64_t pfn = (addr - pmm_physical_memory_offset) / PAGE_SIZE;
    if (pfn >= pmm_lowest_page && pfn < pmm_highest_page) {
      klog_puts(" bitmap_alloc_bit=");
      klog_uint64(pmm_bitmap_test(pmm_bitmap, pfn - pmm_lowest_page) ? 1 : 0);
      klog_puts(" refcount=");
      klog_uint64(__atomic_load_n(&pmm_refcounts[pfn - pmm_lowest_page],
                                  __ATOMIC_RELAXED));
    }
    const struct list_head *head = &zone->free_list[order];
    klog_puts(" zone="); klog_hex64((uint64_t)zone);
    klog_puts(" head.next="); klog_hex64((uint64_t)head->next);
    klog_puts(" head.prev="); klog_hex64((uint64_t)head->prev);
    if (buddy_link_readable(block->node.next, head)) {
      klog_puts(" next.prev=");
      klog_hex64((uint64_t)block->node.next->prev);
    }
    if (buddy_link_readable(block->node.prev, head)) {
      klog_puts(" prev.next=");
      klog_hex64((uint64_t)block->node.prev->next);
    }
    uint64_t phys = (uint64_t)node - pmm_physical_memory_offset;
    uint64_t first = zone->trace_seq > BUDDY_TRACE_SLOTS
                         ? zone->trace_seq - BUDDY_TRACE_SLOTS : 0;
    for (uint64_t seq = first; seq < zone->trace_seq; seq++) {
      const struct buddy_trace_event *event =
          &zone->trace[seq % BUDDY_TRACE_SLOTS];
      uint64_t size = PAGE_SIZE << event->order;
      if (phys < event->phys || phys - event->phys >= size)
        continue;
      klog_puts("\n [PMM-HISTORY] seq="); klog_uint64(seq);
      klog_puts(" op="); klog_puts(event->operation);
      klog_puts(" phys="); klog_hex64(event->phys);
      klog_puts(" order="); klog_uint64(event->order);
      klog_puts(" caller="); klog_hex64(event->caller);
    }
  }
}

static bool buddy_zone_node_valid(const struct buddy_zone *zone,
                                  const struct list_head *node, size_t order) {
  uint64_t addr = (uint64_t)node;
  const struct list_head *head = &zone->free_list[order];
  uint64_t block_size = (uint64_t)1 << (order + 12);
  struct buddy_block *block = (struct buddy_block *)node;

  if (!node || !pmm_kernel_ptr_is_managed(node))
    return false;
  if (addr & (block_size - 1))
    return false;
  if (block->order != order)
    return false;

  uint64_t pfn = (addr - pmm_physical_memory_offset) / PAGE_SIZE;
  if (pfn < pmm_lowest_page || pfn >= pmm_highest_page ||
      pmm_bitmap_test(pmm_bitmap, pfn - pmm_lowest_page))
    return false;

  uint64_t next = (uint64_t)block->node.next;
  uint64_t prev = (uint64_t)block->node.prev;
  if (!buddy_link_readable((void *)next, head))
    return false;
  if (!buddy_link_readable((void *)prev, head))
    return false;

  return block->node.next->prev == node && block->node.prev->next == node;
}

bool buddy_node_valid(const struct list_head *node, size_t order) {
  return buddy_zone_node_valid(&b_zone, node, order);
}

static bool buddy_list_nonempty(struct buddy_zone *zone, size_t order) {
  struct list_head *head = &zone->free_list[order];
  uint64_t steps = 0;

  for (;;) {
    struct list_head *first = head->next;
    if (first == head)
      return false;
    if (++steps <= pmm_page_count && buddy_zone_node_valid(zone, first, order))
      return true;

    buddy_report_corruption("first link", order, first);
    buddy_report_node_detail(zone, first, order);
    klog_puts("\n");
    if (pmm_kernel_ptr_is_managed(first)) {
      struct list_head *next = first->next;
      if (steps < pmm_page_count && next != first && next != head &&
          buddy_link_readable(next, head)) {
        head->next = next;
        next->prev = head;
        continue;
      }
    }
    INIT_LIST_HEAD(head);
    return false;
  }
}

static struct buddy_block *buddy_list_pop(struct buddy_zone *zone, size_t order) {
  if (!buddy_list_nonempty(zone, order))
    return NULL;
  struct buddy_block *block =
      list_first_entry(&zone->free_list[order], struct buddy_block, node);
  list_del(&block->node);
  return block;
}

static void buddy_list_validate(struct buddy_zone *zone, size_t order) {
  struct list_head *head = &zone->free_list[order];
  struct list_head *prev = head;
  struct list_head *pos = head->next;
  uint64_t steps = 0;

  while (pos != head) {
    if (++steps > pmm_page_count || !buddy_zone_node_valid(zone, pos, order)) {
      buddy_report_corruption("link", order, pos);
      buddy_report_node_detail(zone, pos, order);
      klog_puts("\n");
      prev->next = head;
      head->prev = prev;
      return;
    }
    prev = pos;
    pos = pos->next;
  }
}

void buddy_zone_init(struct buddy_zone *zone) {
  for (int i = 0; i < MAX_ORDER; i++) {
    INIT_LIST_HEAD(&zone->free_list[i]);
  }
  spinlock_init(&zone->lock);
  zone->trace_seq = 0;
}

__attribute__((optimize("O3"))) void buddy_free_internal(struct buddy_zone *zone,
                                                         uint64_t phys,
                                                         size_t order) {
  uint64_t pfn = phys / PAGE_SIZE;

  if (phys & (((uint64_t)1 << (order + 12)) - 1)) {
    buddy_report_corruption("misaligned free", order, (void *)phys);
    return;
  }

  pmm_bitmap_clear_range(pmm_bitmap, pfn, (1ULL << order));
  buddy_trace(zone, phys, order, "free", __builtin_return_address(0));

  while (order < MAX_ORDER - 1) {
    uint64_t buddy_pfn = pfn ^ (1ULL << order);

    bool buddy_free = true;
    if (buddy_pfn < pmm_lowest_page || buddy_pfn >= pmm_highest_page ||
        !pmm_is_managed(buddy_pfn * PAGE_SIZE) ||
        pmm_bitmap_test(pmm_bitmap, buddy_pfn - pmm_lowest_page)) {
      buddy_free = false;
    }
    if (!buddy_free)
      break;

    struct buddy_block *buddy = virt_to_buddy(buddy_pfn * PAGE_SIZE);
    if (buddy_zone_node_valid(zone, &buddy->node, order)) {
      list_del(&buddy->node);
      pfn = (pfn < buddy_pfn) ? pfn : buddy_pfn;
      order++;
      buddy_trace(zone, pfn * PAGE_SIZE, order, "merge",
                   __builtin_return_address(0));
    } else {
      break;
    }
  }

  struct buddy_block *block = virt_to_buddy(pfn * PAGE_SIZE);
  if (buddy_zone_node_valid(zone, &block->node, order)) {
    buddy_report_corruption("double free of linked block", order,
                            &block->node);
    return;
  }
  block->order = order;
  list_add_tail(&block->node, &zone->free_list[order]);
}

__attribute__((optimize("O3"))) void *buddy_alloc_internal(struct buddy_zone *zone,
                                                          size_t order) {
  if (order >= MAX_ORDER)
    return NULL;

  spinlock_acquire(&zone->lock);

  size_t cur_order = order;
  while (cur_order < MAX_ORDER && !buddy_list_nonempty(zone, cur_order)) {
    cur_order++;
  }

  if (cur_order == MAX_ORDER) {
    spinlock_release(&zone->lock);
    return NULL; // OOM
  }

  struct buddy_block *block =
      list_first_entry(&zone->free_list[cur_order], struct buddy_block, node);
  list_del(&block->node);

  uint64_t pfn = buddy_to_phys(block) / PAGE_SIZE;
  buddy_trace(zone, pfn * PAGE_SIZE, cur_order, "allocate",
               __builtin_return_address(0));

  while (cur_order > order) {
    cur_order--;
    uint64_t buddy_pfn = pfn + (1ULL << cur_order);
    struct buddy_block *buddy = virt_to_buddy(buddy_pfn * PAGE_SIZE);
    buddy->order = cur_order;
    list_add_tail(&buddy->node, &zone->free_list[cur_order]);
  }

  pmm_bitmap_set_range(pmm_bitmap, pfn, 1ULL << order);

  for (size_t i = 0; i < (1ULL << order); i++) {
    pmm_refcounts[pfn + i - pmm_lowest_page] = 1;
  }

  spinlock_release(&zone->lock);
  return (void *)(pfn * PAGE_SIZE);
}

void *buddy_alloc_page_locked(struct buddy_zone *zone) {
  size_t cur_order = 0;
  struct buddy_block *block = NULL;

  while (cur_order < MAX_ORDER) {
    block = buddy_list_pop(zone, cur_order);
    if (block)
      break;
    cur_order++;
  }

  if (!block)
    return NULL;

  uint64_t pfn = buddy_to_phys(block) / PAGE_SIZE;
  buddy_trace(zone, pfn * PAGE_SIZE, cur_order, "pcp-refill",
               __builtin_return_address(0));

  while (cur_order > 0) {
    cur_order--;
    uint64_t buddy_pfn = pfn + (1ULL << cur_order);
    struct buddy_block *buddy = virt_to_buddy(buddy_pfn * PAGE_SIZE);
    buddy->order = cur_order;
    list_add_tail(&buddy->node, &zone->free_list[cur_order]);
  }

  pmm_bitmap_set_range(pmm_bitmap, pfn, 1);
  pmm_refcounts[pfn - pmm_lowest_page] = 1;
  return (void *)(pfn * PAGE_SIZE);
}

void *buddy_alloc_pages_range(struct buddy_zone *zone, size_t count,
                             uint64_t min_phys_addr, uint64_t max_phys_addr) {
  if (count == 0)
    return NULL;

  size_t order = pmm_get_order(count);
  if (order >= MAX_ORDER)
    return NULL;

  spinlock_acquire(&zone->lock);

  for (size_t cur_order = order; cur_order < MAX_ORDER; cur_order++) {
    struct buddy_block *found_block = NULL;
    uint64_t target_phys = 0;
    struct list_head *pos;

    buddy_list_validate(zone, cur_order);

    list_for_each(pos, &zone->free_list[cur_order]) {
      struct buddy_block *block = list_entry(pos, struct buddy_block, node);
      uint64_t phys = buddy_to_phys(block);
      uint64_t block_end = phys + (1ULL << cur_order) * PAGE_SIZE;
      uint64_t allocation_size = (1ULL << order) * PAGE_SIZE;
      uint64_t candidate = phys > min_phys_addr ? phys : min_phys_addr;
      candidate = (candidate + allocation_size - 1) & ~(allocation_size - 1);
      uint64_t allowed_end = block_end < max_phys_addr ? block_end
                                                       : max_phys_addr;
      if (candidate < allowed_end &&
          allocation_size <= allowed_end - candidate) {
        found_block = block;
        target_phys = candidate;
        break;
      }
    }

    if (found_block) {
      list_del(&found_block->node);
      uint64_t pfn = buddy_to_phys(found_block) / PAGE_SIZE;
      buddy_trace(zone, pfn * PAGE_SIZE, cur_order, "allocate-range",
                   __builtin_return_address(0));
      uint64_t target_pfn = target_phys / PAGE_SIZE;

      while (cur_order > order) {
        cur_order--;
        uint64_t half_pages = 1ULL << cur_order;
        uint64_t buddy_pfn;
        if (target_pfn >= pfn + half_pages) {
          buddy_pfn = pfn;
          pfn += half_pages;
        } else {
          buddy_pfn = pfn + half_pages;
        }
        struct buddy_block *buddy = virt_to_buddy(buddy_pfn * PAGE_SIZE);
        buddy->order = cur_order;
        list_add_tail(&buddy->node, &zone->free_list[cur_order]);
      }

      pmm_bitmap_set_range(pmm_bitmap, pfn, 1ULL << order);
      for (size_t i = 0; i < (1ULL << order); i++)
        pmm_refcounts[pfn + i - pmm_lowest_page] = 1;

      spinlock_release(&zone->lock);
      return (void *)(pfn * PAGE_SIZE);
    }
  }

  spinlock_release(&zone->lock);
  return NULL;
}

size_t buddy_zone_free_pages(struct buddy_zone *zone) {
  size_t free_pages = 0;
  spinlock_acquire(&zone->lock);
  for (int order = 0; order < MAX_ORDER; order++) {
    struct list_head *pos;
    buddy_list_validate(zone, order);
    list_for_each(pos, &zone->free_list[order]) {
      free_pages += (1ULL << order);
    }
  }
  spinlock_release(&zone->lock);
  return free_pages;
}
