#include "mm/numa_migrate.h"
#include "mm/numa.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/vmm_internal.h"
#include "mm/tlb_shootdown.h"
#include "mm/page_ops.h"
#include "arch/uaccess.h"
#include "console/klog.h"
#include "lib/string.h"
#include "sched/sched.h"

extern struct thread *find_thread_by_tid_locked(uint32_t tid);
extern spinlock_t tid_lock;

int numa_migrate_page(struct thread *t, uint64_t vaddr, int target_node) {
  if (!t || !t->mm || target_node < 0 || target_node >= MAX_NUMA_NODES)
    return -22; // EINVAL

  struct numa_node *tn = numa_get_node(target_node);
  if (!tn || !tn->online)
    return -22; // EINVAL

  vaddr &= ~0xFFFULL;

  uint64_t *pml4 = (t->cr3 != 0) ? (uint64_t *)t->cr3 : vmm_get_active_pml4();

  spinlock_acquire(&t->mm->lock);
  uint64_t *pte_ptr = vmm_get_pte_ptr(pml4, vaddr);
  if (!pte_ptr || !(*pte_ptr & PAGE_FLAG_PRESENT)) {
    spinlock_release(&t->mm->lock);
    return -2; // ENOENT
  }

  uint64_t old_phys = *pte_ptr & PAGE_MASK;

  // If already on the target node, nothing to do
  if (numa_phys_to_node(old_phys) == target_node) {
    spinlock_release(&t->mm->lock);
    return 0;
  }

  // Allocate new physical page frame on the target node
  void *new_phys = pmm_alloc_page_node(target_node);
  if (!new_phys) {
    spinlock_release(&t->mm->lock);
    return -12; // ENOMEM
  }

  // Copy page data
  if (old_phys == pmm_get_zero_page_phys()) {
    page_zero(PHYS_TO_VIRT((uint64_t)new_phys));
  } else {
    page_copy(PHYS_TO_VIRT((uint64_t)new_phys), PHYS_TO_VIRT(old_phys));
  }

  // Atomically update PTE with new physical address preserving flags
  uint64_t flags = *pte_ptr & ~PAGE_MASK;
  *pte_ptr = (uint64_t)new_phys | flags;

  // Broadcast TLB shootdown
  tlb_shootdown_page(vaddr);

  // Decrement refcount on old physical page (unless zero page)
  if (old_phys != pmm_get_zero_page_phys()) {
    pmm_decref((void *)old_phys);
  }

  spinlock_release(&t->mm->lock);
  return 0;
}

uint64_t sys_move_pages(uint64_t pid, uint64_t count, uint64_t pages_uptr,
                        uint64_t nodes_uptr, uint64_t status_uptr,
                        uint64_t flags) {
  (void)flags;
  if (!pages_uptr || !status_uptr)
    return (uint64_t)-14; // EFAULT
  if (count == 0)
    return 0;
  if (count > 65536)
    return (uint64_t)-7; // E2BIG

  struct thread *t = NULL;
  if (pid == 0) {
    t = sched_get_current();
  } else {
    spinlock_acquire(&tid_lock);
    t = find_thread_by_tid_locked((uint32_t)pid);
    spinlock_release(&tid_lock);
  }

  if (!t || !t->mm)
    return (uint64_t)-3; // ESRCH

  uint64_t *pml4 = (t->cr3 != 0) ? (uint64_t *)t->cr3 : vmm_get_active_pml4();

  #define BATCH_SIZE 64
  uint64_t pages_buf[BATCH_SIZE];
  int nodes_buf[BATCH_SIZE];
  int status_buf[BATCH_SIZE];

  uint64_t processed = 0;
  while (processed < count) {
    uint64_t batch = count - processed;
    if (batch > BATCH_SIZE)
      batch = BATCH_SIZE;

    if (copy_from_user(pages_buf, (const void *)(pages_uptr + processed * sizeof(uint64_t)),
                       batch * sizeof(uint64_t)) != 0)
      return (uint64_t)-14; // EFAULT

    if (nodes_uptr != 0) {
      if (copy_from_user(nodes_buf, (const void *)(nodes_uptr + processed * sizeof(int)),
                         batch * sizeof(int)) != 0)
        return (uint64_t)-14; // EFAULT
    }

    for (uint64_t i = 0; i < batch; i++) {
      uint64_t va = pages_buf[i];

      if (nodes_uptr == 0) {
        // Query Mode
        spinlock_acquire(&t->mm->lock);
        uint64_t *pte_ptr = vmm_get_pte_ptr(pml4, va);
        if (!pte_ptr || !(*pte_ptr & PAGE_FLAG_PRESENT)) {
          status_buf[i] = -2; // ENOENT
        } else {
          uint64_t phys = *pte_ptr & PAGE_MASK;
          status_buf[i] = numa_phys_to_node(phys);
        }
        spinlock_release(&t->mm->lock);
      } else {
        // Migration Mode
        int target_node = nodes_buf[i];
        if (target_node < 0 || target_node >= MAX_NUMA_NODES) {
          status_buf[i] = -22; // EINVAL
        } else {
          status_buf[i] = numa_migrate_page(t, va, target_node);
        }
      }
    }

    if (copy_to_user((void *)(status_uptr + processed * sizeof(int)),
                     status_buf, batch * sizeof(int)) != 0)
      return (uint64_t)-14; // EFAULT

    processed += batch;
  }
  #undef BATCH_SIZE

  return 0;
}

static void migrate_vma_pages(struct thread *t, struct vma *v, unsigned long old_mask, int target_node) {
  if (!v || !t)
    return;

  uint64_t *pml4 = (t->cr3 != 0) ? (uint64_t *)t->cr3 : vmm_get_active_pml4();

  for (uint64_t va = v->start; va < v->end; va += PAGE_SIZE) {
    spinlock_acquire(&t->mm->lock);
    uint64_t *pte_ptr = vmm_get_pte_ptr(pml4, va);
    if (pte_ptr && (*pte_ptr & PAGE_FLAG_PRESENT)) {
      uint64_t phys = *pte_ptr & PAGE_MASK;
      int cur_node = numa_phys_to_node(phys);
      spinlock_release(&t->mm->lock);

      if ((old_mask & (1ULL << (cur_node % 64)))) {
        numa_migrate_page(t, va, target_node);
      }
    } else {
      spinlock_release(&t->mm->lock);
    }
  }
}

static void migrate_vmas_recursive(struct thread *t, struct vma *node, unsigned long old_mask, int target_node) {
  if (!node)
    return;
  migrate_vma_pages(t, node, old_mask, target_node);
  migrate_vmas_recursive(t, node->left, old_mask, target_node);
  migrate_vmas_recursive(t, node->right, old_mask, target_node);
}

uint64_t sys_migrate_pages(uint64_t pid, uint64_t maxnode,
                          uint64_t old_nodes_uptr, uint64_t new_nodes_uptr,
                          uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  if (!old_nodes_uptr || !new_nodes_uptr || maxnode < 1)
    return (uint64_t)-14; // EFAULT

  unsigned long old_mask = 0;
  unsigned long new_mask = 0;
  if (copy_from_user(&old_mask, (const void *)old_nodes_uptr, sizeof(old_mask)) != 0 ||
      copy_from_user(&new_mask, (const void *)new_nodes_uptr, sizeof(new_mask)) != 0)
    return (uint64_t)-14; // EFAULT

  if (new_mask == 0)
    return (uint64_t)-22; // EINVAL

  // Select target node from new_mask
  int target_node = 0;
  for (int i = 0; i < MAX_NUMA_NODES; i++) {
    if ((new_mask & (1ULL << i))) {
      struct numa_node *n = numa_get_node(i);
      if (n && n->online) {
        target_node = i;
        break;
      }
    }
  }

  struct thread *t = NULL;
  if (pid == 0) {
    t = sched_get_current();
  } else {
    spinlock_acquire(&tid_lock);
    t = find_thread_by_tid_locked((uint32_t)pid);
    spinlock_release(&tid_lock);
  }

  if (!t || !t->mm)
    return (uint64_t)-3; // ESRCH

  migrate_vmas_recursive(t, t->mm->vmas.root, old_mask, target_node);
  return 0;
}
