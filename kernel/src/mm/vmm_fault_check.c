#include "mm/vmm.h"
#include "mm/vmm_internal.h"
#include "cpu/isr.h"
#include "console/klog.h"
#include "sched/sched.h"
#include "smp/cpu.h"
#include "vma.h"

#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

static struct vmm_fault_reject pf_reject[MAX_CPUS];
static volatile uint32_t pf_reject_seq[MAX_CPUS];

static uint32_t pf_reject_cpu_index(void) {
  struct cpu_info *cpu = cpu_get_current();
  return cpu && cpu->cpu_id < MAX_CPUS ? cpu->cpu_id : 0;
}

int pf_reject_record(uint64_t cr2, uint64_t error_code,
                     const struct registers *regs,
                     const struct thread *current, const char *reason,
                     const char *file, uint32_t line, uint64_t detail,
                     uint64_t detail2) {
  uint32_t cpu = pf_reject_cpu_index();
  struct vmm_fault_reject *r = &pf_reject[cpu];
  r->reason = reason;
  r->file = file;
  r->line = line;
  r->cr2 = cr2;
  r->err_code = error_code;
  r->rip = regs ? regs->rip : 0;
  r->detail = detail;
  r->detail2 = detail2;
  r->tid = current ? current->tid : 0;
  r->seq = pf_reject_seq[cpu] + 1;
  pf_reject_seq[cpu] = r->seq;
  return -1;
}

bool vmm_get_last_fault_reject(struct vmm_fault_reject *out) {
  if (!out)
    return false;
  uint32_t cpu = pf_reject_cpu_index();
  uint32_t seq = pf_reject_seq[cpu];
  if (seq == 0)
    return false;
  *out = pf_reject[cpu];
  return out->seq == seq;
}

bool vmm_is_user_addr_range_valid(uint64_t addr, size_t size) {
  if (addr > USER_SPACE_LIMIT || (addr + size) > 0x800000000000ULL) {
    klog_puts("[VMM] Range validation failed: out of bounds\n");
    return false;
  }

  struct thread *current = sched_get_current();
  if (!current)
    return false;

  uint64_t start_page = addr & ~0xFFFULL;
  uint64_t end_page = (addr + size + 0xFFF) & ~0xFFFULL;

  spinlock_acquire(&current->mm->lock);

  uint64_t page = start_page;
  while (page < end_page) {
    struct vma *v = vma_find(&current->mm->vmas, page);
    if (!v)
      v = vma_find_growdown(&current->mm->vmas, page, 8 * 1024 * 1024);

    if (!v) {
      klog_puts("[VMM] Range validation failed (No VMA) at ");
      klog_hex64(page);
      klog_puts(" in thread ");
      klog_uint64(current->tid);
      klog_puts("\n[VMM] Active VMAs:\n");
      vma_dump(&current->mm->vmas);
      spinlock_release(&current->mm->lock);
      return false;
    }

    if (v->prot == PROT_NONE) {
      klog_puts("[VMM] Range validation failed (PROT_NONE) at 0x");
      klog_uint64(page);
      klog_puts("\n");
      spinlock_release(&current->mm->lock);
      return false;
    }

    page = v->end < end_page ? v->end : end_page;
  }

  spinlock_release(&current->mm->lock);
  return true;
}

bool vmm_is_user_addr_range_writable(uint64_t addr, size_t size) {
  if (size == 0)
    return true;
  if (addr > USER_SPACE_LIMIT || size > 0x800000000000ULL - addr)
    return false;

  struct thread *current = sched_get_current();
  if (!current || !current->mm)
    return false;

  uint64_t start_page = addr & ~0xFFFULL;
  uint64_t end_page = (addr + size + 0xFFF) & ~0xFFFULL;

  spinlock_acquire(&current->mm->lock);

  uint64_t page = start_page;
  while (page < end_page) {
    struct vma *v = vma_find(&current->mm->vmas, page);
    if (!v)
      v = vma_find_growdown(&current->mm->vmas, page, 8 * 1024 * 1024);

    if (!v || !(v->prot & PROT_WRITE)) {
      spinlock_release(&current->mm->lock);
      return false;
    }

    page = v->end < end_page ? v->end : end_page;
  }

  spinlock_release(&current->mm->lock);
  return true;
}
