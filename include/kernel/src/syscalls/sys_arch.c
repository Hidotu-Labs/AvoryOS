// Architecture Syscalls: arch_prctl
#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "../cpu/msr.h"
#include "../drivers/timer/rtc.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "syscall.h"
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
      klog_puts("[ARCH_PRCTL] Reject non-canonical SET_FS = ");
      klog_uint64(addr);
      klog_puts("\n");
      return (uint64_t)-22; // -EINVAL
    }
    wrmsr(IA32_FS_BASE, addr);
    {
      extern struct thread *sched_get_current(void);
      struct thread *cur = sched_get_current();
      if (cur)
        cur->fs_base = addr;
    }
    klog_puts("[ARCH_PRCTL] SET_FS = ");
    klog_uint64(addr);
    klog_puts(" (MSR readback=");
    klog_hex64(rdmsr(IA32_FS_BASE));
    klog_puts(", thread->fs_base=");
    {
      extern struct thread *sched_get_current(void);
      struct thread *cur2 = sched_get_current();
      klog_hex64(cur2 ? cur2->fs_base : 0);
    }
    klog_puts(")\n");
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
    klog_puts("[ARCH_PRCTL] SET_GS = ");
    klog_uint64(addr);
    klog_puts("\n");
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

  if (!req_ptr || !vmm_is_user_addr_range_valid(req_ptr, 16))
    return (uint64_t)-14; // EFAULT

  return sleep_for_timespec(req_ptr);
}

// clock_nanosleep(clockid, flags, req, rem) - syscall 230
static uint64_t sys_clock_nanosleep(uint64_t clk_id, uint64_t flags,
                                    uint64_t req_ptr, uint64_t rem_ptr,
                                    uint64_t a4, uint64_t a5) {
  (void)rem_ptr;
  (void)a4;
  (void)a5;

  if (!req_ptr || !vmm_is_user_addr_range_valid(req_ptr, 16))
    return (uint64_t)-14; // EFAULT

  if (flags & ~1ULL)
    return (uint64_t)-22; // EINVAL

  if (!(flags & 1ULL))
    return sleep_for_timespec(req_ptr);

  uint64_t now_ns = clock_now_ns(clk_id);
  if (now_ns == UINT64_MAX)
    return (uint64_t)-22; // EINVAL

  uint64_t *req = (uint64_t *)req_ptr;
  uint64_t sec = req[0];
  uint64_t nsec = req[1];
  if (nsec >= 1000000000ULL)
    return (uint64_t)-22; // EINVAL

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

static uint64_t sys_mlock(uint64_t addr, uint64_t len, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5) {
  (void)addr;
  (void)len;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  // Stub: pretend we locked the memory
  return 0;
}

static uint64_t sys_munlock(uint64_t addr, uint64_t len, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)addr;
  (void)len;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  // Stub: pretend we unlocked the memory
  return 0;
}

static uint64_t sys_mlockall(uint64_t flags, uint64_t a1, uint64_t a2,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)flags;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  // Stub: pretend we locked all memory
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
  // Stub: pretend we unlocked all memory
  return 0;
}

static uint64_t sys_mlock2(uint64_t addr, uint64_t len, uint64_t flags,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)addr;
  (void)len;
  (void)flags;
  (void)a3;
  (void)a4;
  (void)a5;
  // Stub: pretend we locked the memory with flags
  return 0;
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

void syscall_register_arch(void) {
  syscall_register(SYS_ARCH_PRCTL, sys_arch_prctl);
  syscall_register(SYS_CLOCK_GETTIME, sys_clock_gettime);
  syscall_register(SYS_CLOCK_GETRES, sys_clock_getres);
  syscall_register(SYS_CLOCK_NANOSLEEP, sys_clock_nanosleep);
  syscall_register(SYS_NANOSLEEP, sys_nanosleep);
  syscall_register(SYS_GETTIMEOFDAY, sys_gettimeofday);
  syscall_register(SYS_MLOCK, sys_mlock);
  syscall_register(SYS_MUNLOCK, sys_munlock);
  syscall_register(SYS_MLOCKALL, sys_mlockall);
  syscall_register(SYS_MUNLOCKALL, sys_munlockall);
  syscall_register(SYS_MLOCK2, sys_mlock2);
  syscall_register(SYS_MEMBARRIER, sys_membarrier);
}
