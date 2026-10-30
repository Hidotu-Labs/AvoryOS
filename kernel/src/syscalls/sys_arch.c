// Architecture Syscalls: arch_prctl
#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "../cpu/isr.h"
#include "../cpu/msr.h"
#include "../drivers/timer/rtc.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vma.h"
#include "../mm/vmm.h"
#include "../mm/vmm_internal.h"
#include "../sched/sched.h"
#include "syscall.h"
#include "../include/arch/uaccess.h"
#include <stdint.h>

// arch_prctl sub-commands (Linux x86_64)
#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

// MSR addresses
#define IA32_FS_BASE 0xC0000100
#define IA32_GS_BASE 0xC0000101
#define IA32_KERNEL_GS_BASE 0xC0000102

static int is_canonical_user_addr(uint64_t addr) {
  uint64_t sign = (addr >> 47) & 1ULL;
  uint64_t upper = addr >> 48;
  return (sign == 0) ? (upper == 0) : (upper == 0xFFFF);
}

// sys_arch_prctl
// Linux ABI: arch_prctl(code, addr)
//   rdi = code, rsi = addr
static uint64_t sys_arch_prctl(uint64_t code, uint64_t addr, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  switch (code) {
  case ARCH_SET_FS:
    if (!is_canonical_user_addr(addr)) {
      klogf("[ARCH_PRCTL] Reject non-canonical SET_FS = %llu\n",
            (unsigned long long)addr);
      return (uint64_t)-22; // -EINVAL
    }
    wrmsr(IA32_FS_BASE, addr);
    {
      extern struct thread *sched_get_current(void);
      struct thread *cur = sched_get_current();
      if (cur)
        cur->fs_base = addr;
    }
    /* ld.so calls SET_FS once per process launch and this block printed
     * ~97 chars (~1ms of serial console) plus an MSR readback every time.
     * First few launches only. */
    {
      static uint32_t setfs_dumps;
      if (setfs_dumps < 4) {
        setfs_dumps++;
        struct thread *cur2 = sched_get_current();
        klogf("[ARCH_PRCTL] SET_FS = %llu (MSR readback=0x%016llX, "
              "thread->fs_base=0x%016llX)\n",
              (unsigned long long)addr,
              (unsigned long long)rdmsr(IA32_FS_BASE),
              (unsigned long long)(cur2 ? cur2->fs_base : 0));
      }
    }
    return 0;

  case ARCH_GET_FS:
    // addr is a pointer to a uint64_t in userspace where we store the value.
    if (addr) {
      if (!vmm_is_user_addr_range_valid(addr, sizeof(uint64_t)))
        return (uint64_t)-14;
      *(uint64_t *)addr = rdmsr(IA32_FS_BASE);
    }
    return 0;

  case ARCH_SET_GS:
    if (!is_canonical_user_addr(addr)) {
      klog_puts("[ARCH_PRCTL] Reject non-canonical SET_GS = ");
      klog_uint64(addr);
      klog_puts("\n");
      return (uint64_t)-22; // -EINVAL
    }
    // For user GS, we write to KERNEL_GS_BASE (swapgs swaps it in/out).
    // After sysret + swapgs, this becomes the active GS for userspace.
    wrmsr(IA32_KERNEL_GS_BASE, addr);
    {
      extern struct thread *sched_get_current(void);
      struct thread *cur = sched_get_current();
      if (cur)
        cur->gs_base = addr;
    }
    return 0;

  case ARCH_GET_GS:
    if (addr) {
      if (!vmm_is_user_addr_range_valid(addr, sizeof(uint64_t)))
        return (uint64_t)-14;
      *(uint64_t *)addr = rdmsr(IA32_KERNEL_GS_BASE);
    }
    return 0;

  case 0x5002: // ARCH_SHSTK_DISABLE (20482)
    klog_puts("[ARCH_PRCTL] ARCH_SHSTK_DISABLE handled\n");
    return 0;

  case 0x5005: // ARCH_SHSTK_STATUS
    if (addr && vmm_is_user_addr_range_valid(addr, sizeof(uint64_t))) {
      *(uint64_t *)addr = 0;
      return 0;
    }
    return (uint64_t)-14;

  default:
    klog_puts("[ARCH_PRCTL] Unknown code: ");
    klog_uint64(code);
    klog_puts("\n");
    return (uint64_t)-22; // -EINVAL
  }
}

static uint64_t sys_clock_gettime(uint64_t clk_id, uint64_t tp_ptr, uint64_t a2,
                                  uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if (!tp_ptr)
    return (uint64_t)-14; // EFAULT

  uint64_t ns = lapic_timer_get_ns();
  uint64_t sec = ns / 1000000000ULL;
  uint64_t nsec = ns % 1000000000ULL;

  switch (clk_id) {
  case 0: // CLOCK_REALTIME
  case 4: // CLOCK_REALTIME_COARSE (map to REALTIME)
    ((uint64_t *)tp_ptr)[0] = rtc_get_boot_timestamp() + sec;
    ((uint64_t *)tp_ptr)[1] = nsec;
    return 0;
  case 1: // CLOCK_MONOTONIC
  case 6: // CLOCK_MONOTONIC_COARSE (map to monotonic)
  case 7: // CLOCK_BOOTTIME (map to monotonic for simplicity)
  case 11: // CLOCK_TAI (map to monotonic)
    ((uint64_t *)tp_ptr)[0] = sec;
    ((uint64_t *)tp_ptr)[1] = nsec;
    return 0;
  case 2: // CLOCK_PROCESS_CPUTIME_ID - use monotonic time
  case 3: // CLOCK_THREAD_CPUTIME_ID - use monotonic time
    ((uint64_t *)tp_ptr)[0] = sec;
    ((uint64_t *)tp_ptr)[1] = nsec;
    return 0;
  default:
    return (uint64_t)-22; // EINVAL
  }
}

static uint64_t sys_clock_getres(uint64_t clk_id, uint64_t tp_ptr, uint64_t a2,
                                 uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)clk_id;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if (!tp_ptr)
    return (uint64_t)-14; // EFAULT

  // TSC-based clock has ~1ns resolution
  ((uint64_t *)tp_ptr)[0] = 0;
  ((uint64_t *)tp_ptr)[1] = 1;

  return 0;
}

static uint64_t clock_now_ns(uint64_t clk_id) {
  uint64_t mono_ns = lapic_timer_get_ns();

  switch (clk_id) {
  case 0:  // CLOCK_REALTIME
  case 4:  // CLOCK_REALTIME_COARSE
    return rtc_get_boot_timestamp() * 1000000000ULL + mono_ns;
  case 1:  // CLOCK_MONOTONIC
  case 6:  // CLOCK_MONOTONIC_COARSE
  case 7:  // CLOCK_BOOTTIME
  case 11: // CLOCK_TAI
  case 2:  // CLOCK_PROCESS_CPUTIME_ID
  case 3:  // CLOCK_THREAD_CPUTIME_ID
    return mono_ns;
  default:
    return UINT64_MAX;
  }
}

static int sleep_copy_timespec(uint64_t ptr, int64_t out[2]) {
  struct thread *t = sched_get_current();
  if (t && t->seccomp_compat_dispatch) {
    int32_t narrow[2];
    if (copy_from_user(narrow, (void *)ptr, sizeof(narrow))) return -14;
    out[0]=narrow[0]; out[1]=narrow[1];
  } else if (copy_from_user(out, (void *)ptr, 16)) return -14;
  if (out[0]<0 || out[1]<0 || out[1]>=1000000000) return -22;
  if ((uint64_t)out[0] > (UINT64_MAX-999999999ULL)/1000000000ULL) return -22;
  return 0;
}

static uint64_t sleep_for_timespec(uint64_t req_ptr) {
  uint64_t *req = (uint64_t *)req_ptr;
  uint64_t sec = req[0];
  uint64_t nsec = req[1];

  if (nsec >= 1000000000ULL)
    return (uint64_t)-22; // EINVAL

  uint64_t total_ms = sec * 1000 + nsec / 1000000;
  if (total_ms == 0 && nsec > 0)
    total_ms = 1;

  if (total_ms > 0) {
    struct thread *current = sched_get_current();
    if (current) {
      current->state = THREAD_SLEEPING;
      current->wakeup_ticks = lapic_timer_get_ticks() + total_ms;
      sched_yield();
    }
  }

  return 0;
}

// nanosleep(req, rem) - sleep for specified time
// req and rem are pointers to struct timespec { tv_sec, tv_nsec }
static uint64_t sys_nanosleep(uint64_t req_ptr, uint64_t rem_ptr, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  (void)rem_ptr;

  int64_t req[2];
  int error=sleep_copy_timespec(req_ptr, req);
  if (error) return (uint64_t)error;
  return sleep_for_timespec((uint64_t)req);
}

// clock_nanosleep(clockid, flags, req, rem) - syscall 230
static uint64_t sys_clock_nanosleep(uint64_t clk_id, uint64_t flags,
                                    uint64_t req_ptr, uint64_t rem_ptr,
                                    uint64_t a4, uint64_t a5) {
  (void)rem_ptr;
  (void)a4;
  (void)a5;

  int64_t req[2];
  int error=sleep_copy_timespec(req_ptr, req);
  if (error) return (uint64_t)error;
  if (flags & ~1ULL) return (uint64_t)-22;
  uint64_t now_ns = clock_now_ns(clk_id);
  if (now_ns == UINT64_MAX || clk_id == 3) return (uint64_t)-22;
  if (!(flags & 1ULL)) return sleep_for_timespec((uint64_t)req);
  uint64_t sec = req[0], nsec = req[1];

  uint64_t target_ns = sec * 1000000000ULL + nsec;
  if (target_ns <= now_ns)
    return 0;

  /* Convert remaining ns to ms for the scheduler (round up). */
  uint64_t sleep_ms = (target_ns - now_ns + 999999ULL) / 1000000ULL;

  struct thread *current = sched_get_current();
  if (current) {
    current->state = THREAD_SLEEPING;
    current->wakeup_ticks = lapic_timer_get_ticks() + sleep_ms;
    sched_yield();
  }

  return 0;
}

// gettimeofday(tv, tz) - syscall 97
// tz is ignored in modern Linux; tv contains {tv_sec, tv_usec}
static uint64_t sys_gettimeofday(uint64_t tv_ptr, uint64_t tz_ptr, uint64_t a2,
                                 uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)tz_ptr;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if (!tv_ptr)
    return 0; // Linux allows NULL tv

  uint64_t ns = lapic_timer_get_ns();
  uint64_t sec = ns / 1000000000ULL;
  uint64_t usec = (ns % 1000000000ULL) / 1000ULL;

  uint64_t *tv = (uint64_t *)tv_ptr;
  tv[0] = rtc_get_boot_timestamp() + sec; // tv_sec
  tv[1] = usec;                           // tv_usec

  return 0;
}

// settimeofday(tv, tz) - syscall 164
// NULL/NULL is a harmless no-op probe (returns 0, like Linux).  Actually
// setting the clock is not supported - there is no RTC-write path - so any
// real request fails EPERM after pointer validation.
static uint64_t sys_settimeofday(uint64_t tv_ptr, uint64_t tz_ptr, uint64_t a2,
                                 uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if (!tv_ptr && !tz_ptr)
    return 0;
  // struct timeval is 16 bytes; struct timezone is 8 bytes.
  if (tv_ptr && !vmm_is_user_addr_range_valid(tv_ptr, 16))
    return (uint64_t)-14; // EFAULT
  if (tz_ptr && !vmm_is_user_addr_range_valid(tz_ptr, 8))
    return (uint64_t)-14; // EFAULT
  return (uint64_t)-1; // EPERM: setting the clock is not supported
}

// time(tloc) - syscall 201
// Returns seconds since the Epoch (1970-01-01 00:00:00 UTC).
// If tloc is non-NULL, also stores the value at *tloc.
static uint64_t sys_time(uint64_t tloc_ptr, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  uint64_t ns = lapic_timer_get_ns();
  uint64_t sec = rtc_get_boot_timestamp() + (ns / 1000000000ULL);

  if (tloc_ptr) {
    if (tloc_ptr >= 0x8000000000000000ULL) {
      return (uint64_t)-14; // EFAULT
    }
    *(uint64_t *)tloc_ptr = sec;
  }

  return sec;
}

#define MCL_CURRENT 1
#define MCL_FUTURE 2
#define MLOCK_ONFAULT 1

struct mlock_vma_range {
  uint64_t start;
  uint64_t end;
  uint64_t prot;
};

static bool mlock_snapshot_vmas(struct vma *v, struct mlock_vma_range *ranges,
                                size_t capacity, size_t *count) {
  if (!v)
    return true;
  if (!mlock_snapshot_vmas(v->left, ranges, capacity, count))
    return false;
  if (*count == capacity)
    return false;
  ranges[*count].start = v->start;
  ranges[*count].end = v->end;
  ranges[*count].prot = v->prot;
  (*count)++;
  return mlock_snapshot_vmas(v->right, ranges, capacity, count);
}

/* Validate coverage before touching any pages. Linux reports ENOMEM when an
 * mlock range contains an unmapped hole. */
static bool mlock_range_mapped(struct mm_struct *mm, uint64_t start,
                               uint64_t end) {
  uint64_t cursor = start;
  while (cursor < end) {
    struct vma *v = vma_find(&mm->vmas, cursor);
    if (!v || v->end <= cursor)
      return false;
    cursor = v->end < end ? v->end : end;
  }
  return true;
}

static uint64_t mlock_apply_range(struct thread *thread, uint64_t start,
                                  uint64_t end, bool lock_pages) {
  if (!thread || !thread->mm)
    return (uint64_t)-12; // ENOMEM
  struct mm_struct *mm = thread->mm;
  uint64_t *pml4 = thread->cr3 ? (uint64_t *)thread->cr3
                               : vmm_get_active_pml4();
  if (!mm || !pml4)
    return (uint64_t)-12; // ENOMEM

  spinlock_acquire(&mm->lock);
  bool mapped = mlock_range_mapped(mm, start, end);
  spinlock_release(&mm->lock);
  if (!mapped)
    return (uint64_t)-12; // ENOMEM

  if (lock_pages) {
    struct registers synthetic_regs = {0};
    /* mlock makes the covered pages resident. Use the ordinary fault path so
     * demand-zero, file-backed, and compressed pages follow their normal
     * allocation and swap-in rules. PROT_NONE and LinuxKPI VMAs have no
     * generic fault path here; their pages are not candidates for zram. */
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
      spinlock_acquire(&mm->lock);
      struct vma *v = vma_find(&mm->vmas, va);
      bool covered = v != NULL;
      bool faultable = v && v->prot && !v->linux_vma;
      uint64_t prot = v ? v->prot : 0;
      vmm_lock_acquire_for(pml4, va);
      uint64_t *leaf = vmm_get_leaf_entry_ptr(pml4, va);
      bool resident = leaf && (*leaf & PAGE_FLAG_PRESENT);
      if (resident)
        *leaf |= PAGE_FLAG_MLOCKED;
      vmm_lock_release();
      spinlock_release(&mm->lock);
      if (!covered)
        return (uint64_t)-12;
      uint64_t fault_code = 0x4; // user read, not-present
      if (!(prot & 0x1)) {
        if (prot & 0x2)
          fault_code = 0x6; // user write, not-present
        else if (prot & 0x4)
          fault_code = 0x14; // user instruction fetch, not-present
      }
      if (faultable && !resident) {
        bool faulted = false;
        for (unsigned retry = 0; retry < 3; retry++) {
          if (vmm_handle_page_fault(va, fault_code, &synthetic_regs) != 0)
            break;
          spinlock_acquire(&mm->lock);
          vmm_lock_acquire_for(pml4, va);
          leaf = vmm_get_leaf_entry_ptr(pml4, va);
          if (leaf && (*leaf & PAGE_FLAG_PRESENT)) {
            *leaf |= PAGE_FLAG_MLOCKED;
            faulted = true;
          }
          vmm_lock_release();
          spinlock_release(&mm->lock);
          if (faulted)
            break;
        }
        if (!faulted)
          return (uint64_t)-12;
      }
    }
  } else {
    /* Unlocking a range never faults pages in. Clear its markers under the
     * same lock order used by reclaim, so a racing compressor either sees the
     * lock or wins before munlock returns. */
    spinlock_acquire(&mm->lock);
    if (!mlock_range_mapped(mm, start, end)) {
      spinlock_release(&mm->lock);
      return (uint64_t)-12;
    }
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
      vmm_lock_acquire_for(pml4, va);
      uint64_t *leaf = vmm_get_leaf_entry_ptr(pml4, va);
      if (leaf)
        *leaf &= ~PAGE_FLAG_MLOCKED;
      vmm_lock_release();
    }
    spinlock_release(&mm->lock);
  }
  return 0;
}

static bool mlock_bounds(uint64_t addr, uint64_t len, uint64_t *start,
                         uint64_t *end) {
  if (!len) {
    *start = *end = 0;
    return true;
  }
  if (addr > USER_SPACE_LIMIT || len > USER_SPACE_LIMIT + 1 - addr)
    return false;
  uint64_t raw_end = addr + len;
  *start = addr & ~(PAGE_SIZE - 1ULL);
  *end = (raw_end + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
  return *end >= raw_end && *end <= USER_SPACE_LIMIT + 1;
}

static uint64_t sys_mlock(uint64_t addr, uint64_t len, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *thread = sched_get_current();
  uint64_t start, end;
  if (!mlock_bounds(addr, len, &start, &end))
    return (uint64_t)-12; // ENOMEM
  return start == end ? 0 : mlock_apply_range(thread, start, end, true);
}

static uint64_t sys_munlock(uint64_t addr, uint64_t len, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *thread = sched_get_current();
  uint64_t start, end;
  if (!mlock_bounds(addr, len, &start, &end))
    return (uint64_t)-12; // ENOMEM
  return start == end ? 0 : mlock_apply_range(thread, start, end, false);
}

static uint64_t sys_mlockall(uint64_t flags, uint64_t a1, uint64_t a2,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *thread = sched_get_current();
  if (!thread || !thread->mm)
    return (uint64_t)-12;
  if ((flags & ~(MCL_CURRENT | MCL_FUTURE)) != 0 ||
      !(flags & (MCL_CURRENT | MCL_FUTURE)))
    return (uint64_t)-22; // EINVAL

  struct mm_struct *mm = thread->mm;
  spinlock_acquire(&mm->lock);
  bool prior_future = mm->mlockall_future;
  /* Protect against reclaim while MCL_CURRENT pages are faulted in and
   * marked. zram checks this state under mm->lock. */
  mm->mlockall_future = true;
  size_t capacity = (size_t)mm->vmas.count;
  spinlock_release(&mm->lock);

  if (flags & MCL_CURRENT) {
    struct mlock_vma_range *ranges = NULL;
    if (capacity) {
      if (capacity > SIZE_MAX / sizeof(*ranges)) {
        spinlock_acquire(&mm->lock);
        mm->mlockall_future = prior_future;
        spinlock_release(&mm->lock);
        return (uint64_t)-12;
      }
      ranges = kmalloc(capacity * sizeof(*ranges));
      if (!ranges) {
        spinlock_acquire(&mm->lock);
        mm->mlockall_future = prior_future;
        spinlock_release(&mm->lock);
        return (uint64_t)-12;
      }
    }

    size_t count = 0;
    spinlock_acquire(&mm->lock);
    bool snapshot_ok = mlock_snapshot_vmas(mm->vmas.root, ranges, capacity,
                                           &count);
    spinlock_release(&mm->lock);
    if (!snapshot_ok) {
      kfree(ranges);
      spinlock_acquire(&mm->lock);
      mm->mlockall_future = prior_future;
      spinlock_release(&mm->lock);
      return (uint64_t)-12;
    }
    for (size_t i = 0; i < count; i++) {
      if (!ranges[i].prot)
        continue;
      uint64_t ret = mlock_apply_range(thread, ranges[i].start,
                                       ranges[i].end, true);
      if (ret) {
        kfree(ranges);
        spinlock_acquire(&mm->lock);
        mm->mlockall_future = prior_future;
        spinlock_release(&mm->lock);
        return ret;
      }
    }
    kfree(ranges);
  }

  spinlock_acquire(&mm->lock);
  mm->mlockall_future = (flags & MCL_FUTURE) != 0;
  spinlock_release(&mm->lock);
  return 0;
}

static uint64_t sys_munlockall(uint64_t a0, uint64_t a1, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *thread = sched_get_current();
  if (!thread || !thread->mm)
    return (uint64_t)-12;
  struct mm_struct *mm = thread->mm;
  uint64_t *pml4 = thread->cr3 ? (uint64_t *)thread->cr3
                               : vmm_get_active_pml4();
  spinlock_acquire(&mm->lock);
  mm->mlockall_future = false;
  vmm_lock_acquire_for(pml4, 0);
  for (size_t i = 0; i < 256; i++) {
    if (!(pml4[i] & PAGE_FLAG_PRESENT))
      continue;
    uint64_t *pdpt = (uint64_t *)(pmm_get_hhdm_offset() +
                                  (pml4[i] & PAGE_MASK));
    for (size_t j = 0; j < 512; j++) {
      if (!(pdpt[j] & PAGE_FLAG_PRESENT))
        continue;
      if (pdpt[j] & PAGE_FLAG_PS) {
        pdpt[j] &= ~PAGE_FLAG_MLOCKED;
        continue;
      }
      uint64_t *pd = (uint64_t *)(pmm_get_hhdm_offset() +
                                  (pdpt[j] & PAGE_MASK));
      for (size_t k = 0; k < 512; k++) {
        if (!(pd[k] & PAGE_FLAG_PRESENT))
          continue;
        if (pd[k] & PAGE_FLAG_PS) {
          pd[k] &= ~PAGE_FLAG_MLOCKED;
          continue;
        }
        uint64_t *pt = (uint64_t *)(pmm_get_hhdm_offset() +
                                    (pd[k] & PAGE_MASK));
        for (size_t l = 0; l < 512; l++)
          pt[l] &= ~PAGE_FLAG_MLOCKED;
      }
    }
  }
  vmm_lock_release();
  spinlock_release(&mm->lock);
  return 0;
}

static uint64_t sys_mlock2(uint64_t addr, uint64_t len, uint64_t flags,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (flags & ~MLOCK_ONFAULT)
    return (uint64_t)-22; // EINVAL
  if (flags & MLOCK_ONFAULT)
    return (uint64_t)-95; // EOPNOTSUPP until nonresident range metadata exists
  struct thread *thread = sched_get_current();
  uint64_t start, end;
  if (!mlock_bounds(addr, len, &start, &end))
    return (uint64_t)-12; // ENOMEM
  return start == end ? 0 : mlock_apply_range(thread, start, end, true);
}

// membarrier(cmd, flags, cpu_id) - syscall 324
// Memory barrier syscall - support all commands for GTK
static uint64_t sys_membarrier(uint64_t cmd, uint64_t flags, uint64_t cpu_id,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)flags;
  (void)cpu_id;
  (void)a3;
  (void)a4;
  (void)a5;

  // MEMBARRIER_CMD_QUERY (0) - Return supported commands bitmask
  if (cmd == 0) {
    // Return bitmask of all supported commands
    return 0xFF; // Support everything
  }

  // For all other commands, just return success (0)
  // This includes:
  // - MEMBARRIER_CMD_GLOBAL (1)
  // - MEMBARRIER_CMD_GLOBAL_EXPEDITED (2)
  // - MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED (3)
  // - MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED (4)
  // - MEMBARRIER_CMD_PRIVATE_EXPEDITED (5)
  // - Any other future commands
  return 0;
}

/* syslog(type, bufp, len) - syscall 103.
 *
 * Actions 0/1/5/6/7/8 are console-level controls (no-ops) so busybox
 * `dmesg -n <level>` (the OpenRC dmesg service) succeeds.  Actions
 * 2/3/4/9/10 are served from the klog ring buffer so `dmesg` can actually
 * read the boot log: 9/10 report the buffered size, 2/3 copy it out,
 * 4 copies it out (clear is a no-op: the ring is also exposed at
 * /proc/klog and /dev/klog, so dropping it would break those readers). */
static uint64_t sys_syslog(uint64_t type, uint64_t bufp, uint64_t len,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  switch (type) {
  case 0: /* SYSLOG_ACTION_CLOSE */
  case 1: /* SYSLOG_ACTION_OPEN */
  case 5: /* SYSLOG_ACTION_CLEAR */
  case 6: /* SYSLOG_ACTION_CONSOLE_OFF */
  case 7: /* SYSLOG_ACTION_CONSOLE_ON */
    return 0;
  case 8: /* SYSLOG_ACTION_CONSOLE_LEVEL */
    return len <= 8 ? 0 : (uint64_t)-22;
  case 9:  /* SYSLOG_ACTION_SIZE_UNREAD */
  case 10: /* SYSLOG_ACTION_SIZE_BUFFER */
    return (uint64_t)klog_ring_length();
  case 2: /* SYSLOG_ACTION_READ */
  case 3: /* SYSLOG_ACTION_READ_ALL */
  case 4: /* SYSLOG_ACTION_READ_CLEAR */
    if (len == 0)
      return 0;
    if (!bufp || !vmm_is_user_addr_range_writable(bufp, (size_t)len))
      return (uint64_t)-14; // EFAULT
    return (uint64_t)klog_ring_read(0, (uint32_t)len, (uint8_t *)bufp);
  default:
    return (uint64_t)-38; /* ENOSYS: unknown syslog action */
  }
}

void syscall_register_arch(void) {
  syscall_register(SYS_ARCH_PRCTL, sys_arch_prctl);
  syscall_register(SYS_SYSLOG, sys_syslog);
  syscall_register(SYS_CLOCK_GETTIME, sys_clock_gettime);
  syscall_register(SYS_CLOCK_GETRES, sys_clock_getres);
  syscall_register(SYS_CLOCK_NANOSLEEP, sys_clock_nanosleep);
  syscall_register(SYS_NANOSLEEP, sys_nanosleep);
  syscall_register(SYS_GETTIMEOFDAY, sys_gettimeofday);
  syscall_register(SYS_SETTIMEOFDAY, sys_settimeofday);
  syscall_register(SYS_TIME, sys_time);
  syscall_register(SYS_MLOCK, sys_mlock);
  syscall_register(SYS_MUNLOCK, sys_munlock);
  syscall_register(SYS_MLOCKALL, sys_mlockall);
  syscall_register(SYS_MUNLOCKALL, sys_munlockall);
  syscall_register(SYS_MLOCK2, sys_mlock2);
  syscall_register(SYS_MEMBARRIER, sys_membarrier);
}
