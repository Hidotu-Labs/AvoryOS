#include "mm/zram.h"
#include "../sched/capability.h"
// Process identity, credentials, scheduling, and namespace syscalls.
#include "../acpi/acpi.h"
#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "../cpu/gdt.h"
#include "../cpu/fpu.h"
#include "../cpu/msr.h"
#include "../drivers/storage/nvme.h"
#include "../drivers/net/e1000.h"
#include "../drivers/timer/rtc.h"
#include "../hal/hal.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/numa.h"
#include "../mm/pcid.h"
#include "../mm/pmm.h"
#include "../mm/vma.h"
#include "../mm/vmm.h"

#include "../sched/sched.h"
#include "../sched/namespace.h"
#include "../sched/pid_namespace.h"
#include "../sched/seccomp.h"
#include "../sched/net_namespace.h"
#include "../fs/mount_namespace.h"
#include "../sched/wait.h"
#include "../smp/cpu.h"
#include "sys_io_shared.h"
#include "sys_process_internal.h"
#include "syscall.h"
#include "sysdiag.h"
#define TSC_PROBES_ENABLE
#include "../lib/tsc.h"
#include <stdint.h>
#include <stddef.h>
#ifdef LINUXKPI_LINUX_IMPORTED
#include <linuxkpi/native_pci.h>
#endif

#define PR_SET_NAME 15
#define PR_GET_NAME 16
void process_mount_cleanup(struct mount_namespace **ns) { vfs_namespace_put(*ns); }
// Path Normalization
static void path_normalize(const char *base, const char *rel, char *out) {
  char temp[512] = {0};

  // If relative path starts with '/', it replaces base entirely
  if (rel[0] == '/') {
    strcpy(temp, rel);
  } else {
    strcpy(temp, base);
    if (temp[strlen(temp) - 1] != '/')
      strcat(temp, "/");
    strcat(temp, rel);
  }

  // Collapse elements
  char *collapsed[128];
  int count = 0;

  char *p = temp;
  while (*p) {
    while (*p == '/')
      *p++ = '\0';
    if (!*p)
      break;

    char *elem = p;
    while (*p && *p != '/') {
      p++;
    }
    if (*p == '/') {
      *p = '\0';
      p++;
    }

    if (strcmp(elem, ".") == 0) {
      continue;
    } else if (strcmp(elem, "..") == 0) {
      if (count > 0)
        count--;
    } else {
      if (count < 128) {
        collapsed[count++] = elem;
      }
    }
  }

  // Rebuild out path
  out[0] = '\0';
  for (int i = 0; i < count; i++) {
    strcat(out, "/");
    strcat(out, collapsed[i]);
  }
  if (count == 0)
    strcpy(out, "/");
}

extern spinlock_t tid_lock;
extern struct thread *global_thread_list;

static uint64_t sys_gettid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *current = sched_get_current();
  if (current && current->is_main_session)
    return 1;
  return current ? pid_namespace_tid(current, current) : 0;
}

static uint64_t sys_getpid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *current = sched_get_current();
  if (current) {
    if (current->is_main_session)
      return 1;
    return pid_namespace_visible(current, current->tgid ? current->tgid : current->tid);
  }
  return 0;
}
// sys_getcwd
static uint64_t sys_getcwd(uint64_t buf_ptr, uint64_t size, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  char *buf = (char *)buf_ptr;
  if (!buf)
    return (uint64_t)-14; // EFAULT

  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-1;

  size_t len = strlen(current->cwd_path) + 1;
  if (size < len)
    return (uint64_t)-34; // ERANGE

  memcpy(buf, current->cwd_path, len);
  return len; // Linux getcwd syscall returns length of copied bytes
}

// sys_chdir
static uint64_t sys_chdir(uint64_t path_ptr, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  const char *path = (const char *)path_ptr;
  if (!path)
    return (uint64_t)-14; // EFAULT

  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-1;

  char new_path[256];
  path_normalize(current->cwd_path, path, new_path);

  // Validate the directory exists!
  // Note: we want an absolute lookup here
  vfs_node_t *node = vfs_resolve_path_at(current->cwd_node, path);
  if (!node)
    return (uint64_t)-2; // ENOENT

  if ((node->flags & FS_TYPE_MASK) != FS_DIRECTORY) {
    vfs_close(node);
    return (uint64_t)-20; // ENOTDIR
  }
  if (!vfs_access(node, 1)) {
    vfs_close(node);
    return (uint64_t)-13;
  }

  // If validation passes, update thread
  if (current->cwd_node)
    vfs_close(current->cwd_node);
  current->cwd_node = node; // vfs_resolve_path_at already opened it

  strncpy(current->cwd_path, new_path, 255);
  current->cwd_path[255] = '\0';

  return 0; // Success
}
// sys_sysinfo
struct sysinfo {
  int64_t uptime;
  uint64_t loads[3];
  uint64_t totalram;
  uint64_t freeram;
  uint64_t sharedram;
  uint64_t bufferram;
  uint64_t totalswap;
  uint64_t freeswap;
  uint16_t procs;
  uint16_t pad;
  /* Natural x86-64 alignment places totalhigh at 88, not 82. */
  uint64_t totalhigh;
  uint64_t freehigh;
  uint32_t mem_unit;
};

_Static_assert(sizeof(struct sysinfo) == 112, "x86-64 sysinfo ABI size");
_Static_assert(offsetof(struct sysinfo, totalram) == 32 &&
               offsetof(struct sysinfo, procs) == 80 &&
               offsetof(struct sysinfo, totalhigh) == 88 &&
               offsetof(struct sysinfo, freehigh) == 96 &&
               offsetof(struct sysinfo, mem_unit) == 104,
               "x86-64 sysinfo ABI offsets");

struct compat_sysinfo {
  int32_t uptime;
  uint32_t loads[3];
  uint32_t totalram, freeram, sharedram, bufferram, totalswap, freeswap;
  uint16_t procs, pad;
  uint32_t totalhigh, freehigh, mem_unit;
  uint8_t padding[8];
};
_Static_assert(sizeof(struct compat_sysinfo) == 64, "i386 sysinfo ABI size");
_Static_assert(offsetof(struct compat_sysinfo, mem_unit) == 52,
               "i386 sysinfo memory unit offset");

static uint64_t sys_sysinfo(uint64_t info_ptr, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct sysinfo info;
  /* Initialize ABI padding too. In the packed layout mem_unit occupied byte
   * 98, leaving libc's byte-104 read uninitialized and corrupting RAM sizes
   * returned by sysconf(_SC_PHYS_PAGES). */
  memset(&info, 0, sizeof(info));
  info.uptime = (int64_t)(lapic_timer_get_ms() / 1000);
  info.totalram = pmm_get_total_memory();
  info.freeram = (uint64_t)pmm_get_free_pages() * PAGE_SIZE;

  extern uint64_t shm_total_pages(void) __attribute__((weak));
  extern uint64_t tmpfs_total_pages(void) __attribute__((weak));
  uint64_t shm_pages = shm_total_pages ? shm_total_pages() : 0;
  uint64_t tmpfs_pages = tmpfs_total_pages ? tmpfs_total_pages() : 0;
  info.sharedram = (shm_pages + tmpfs_pages) * PAGE_SIZE;

  extern size_t ext2_buffer_cache_total_kb(void) __attribute__((weak));
  info.bufferram = ext2_buffer_cache_total_kb ? (uint64_t)ext2_buffer_cache_total_kb() * 1024 : 0;

  info.procs = sched_get_thread_count();
  struct zram_stats z; zram_get_stats(&z);
  info.totalswap = z.enabled ? z.capacity_pages * PAGE_SIZE : 0;
  info.freeswap = z.enabled ? (z.capacity_pages - z.stored_pages) * PAGE_SIZE : 0;
  info.mem_unit = 1;
  struct thread *current = sched_get_current();
  if (current && current->seccomp_compat_dispatch) {
    /* Keep all memory counters in one common unit instead of truncating
     * byte counts when the machine has more than 4 GiB of RAM. */
    uint64_t values[8] = {info.totalram, info.freeram, info.sharedram,
        info.bufferram, info.totalswap, info.freeswap, info.totalhigh, info.freehigh};
    uint64_t largest = 0;
    for (unsigned i=0; i<8; i++) if (values[i]>largest) largest=values[i];
    unsigned shift=0;
    while ((largest >> shift)>UINT32_MAX && shift<31) shift++;
    struct compat_sysinfo out = {
        .uptime=(int32_t)info.uptime,
        .loads={(uint32_t)info.loads[0],(uint32_t)info.loads[1],(uint32_t)info.loads[2]},
        .totalram=values[0]>>shift, .freeram=values[1]>>shift,
        .sharedram=values[2]>>shift, .bufferram=values[3]>>shift,
        .totalswap=values[4]>>shift, .freeswap=values[5]>>shift,
        .procs=info.procs, .totalhigh=values[6]>>shift,
        .freehigh=values[7]>>shift, .mem_unit=1U<<shift
    };
    return copy_to_user((void *)info_ptr, &out, sizeof(out)) ? (uint64_t)-14 : 0;
  }
  if (copy_to_user((void *)info_ptr, &info, sizeof(info)) != 0)
    return (uint64_t)-14; // EFAULT
  return 0;
}

// sys_uname
struct utsname {
  char sysname[65];
  char nodename[65];
  char release[65];
  char version[65];
  char machine[65];
  char domainname[65];
};

/* System hostname: set by sethostname(2), reported back through uname(2).
 * OpenRC's `hostname` service fails the whole boot runlevel without it. */
/* Hostname and domain belong to the calling thread's UTS namespace. */

static uint64_t process_set_utsname(uint64_t name_ptr, uint64_t len, int domain) {
  if (len > 64)
    return (uint64_t)-22; // EINVAL: Linux caps the hostname at 64 bytes
  if (len && !name_ptr)
    return (uint64_t)-14; // EFAULT

  char tmp[65];
  if (len && copy_from_user(tmp, (const void *)name_ptr, len) != 0)
    return (uint64_t)-14;
  tmp[len] = '\0';

  return (uint64_t)uts_namespace_set(sched_get_current(), tmp, len, domain);
}

static uint64_t sys_sethostname(uint64_t name_ptr, uint64_t len, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2; (void)a3; (void)a4; (void)a5;
  return process_set_utsname(name_ptr, len, 0);
}

static uint64_t sys_setdomainname(uint64_t name_ptr, uint64_t len, uint64_t a2,
                                  uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2; (void)a3; (void)a4; (void)a5;
  return process_set_utsname(name_ptr, len, 1);
}

static uint64_t sys_uname(uint64_t buf_ptr, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct utsname result = {0};
  struct utsname *buf = &result;
  if (!buf_ptr)
    return (uint64_t)-14; // EFAULT

  strcpy(buf->sysname, "Ascension");
  uts_namespace_read(sched_get_current(), buf->nodename, buf->domainname);
  strcpy(buf->release, AVORY_BUILD_VERSION);

  static char cached_version[80] = "";
  static uint64_t cached_ms = 0;
  uint64_t now_ms = lapic_timer_get_ms();
  if (!cached_version[0] || now_ms - cached_ms >= 1000) {
    char datetime[32];
    rtc_format_datetime(rtc_get_timestamp(), datetime, sizeof(datetime));
    strcpy(cached_version, AVORY_BUILD_NAME " " AVORY_BUILD_VERSION
                             " build " AVORY_BUILD_TAG " ");
    strcat(cached_version, datetime);
    cached_ms = now_ms;
  }
  strcpy(buf->version, cached_version);

  strcpy(buf->machine, "x86_64");
  return copy_to_user((void *)buf_ptr, &result, sizeof(result)) ? (uint64_t)-14 : 0;
}

// sys_getcpu(unsigned *cpu, unsigned *node, struct getcpu_cache *tcache)
// Returns the calling thread's current CPU index and NUMA node.
// AvoryOS has a single NUMA node (0); tcache is ignored.
static uint64_t sys_getcpu(uint64_t cpu_ptr, uint64_t node_ptr, uint64_t tcache,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)tcache;
  (void)a3;
  (void)a4;
  (void)a5;

  struct cpu_info *ci = cpu_get_current();
  uint32_t cpu_id = ci ? ci->cpu_id : 0;
  uint32_t node = (uint32_t)numa_cpu_to_node(cpu_id);

  /* SMAP-safe copies (the old direct stores faulted under SMAP). */
  if (cpu_ptr &&
      copy_to_user((void *)cpu_ptr, &cpu_id, sizeof(cpu_id)) != 0)
    return (uint64_t)-14;
  if (node_ptr &&
      copy_to_user((void *)node_ptr, &node, sizeof(node)) != 0)
    return (uint64_t)-14;
  return 0;
}


static uint64_t sys_uptime(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return lapic_timer_get_ms();
}

static uint64_t sys_prctl(uint64_t option, uint64_t arg2, uint64_t arg3,
                          uint64_t arg4, uint64_t arg5, uint64_t a5) {
  (void)arg3;
  (void)arg4;
  (void)arg5;
  (void)a5;

  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-1;

  switch (option) {
  case 59: // PR_SET_SYSCALL_USER_DISPATCH
    return (uint64_t)syscall_user_dispatch_configure(current, arg2, arg3,
                                                    arg4, arg5);
  case 1: // PR_SET_PDEATHSIG
    if (arg2 == 0 || (arg2 > 0 && arg2 <= 64)) {
      current->pdeath_signal = (int)arg2;
      return 0;
    }
    return (uint64_t)-22;
  case 2: // PR_GET_PDEATHSIG
    if (arg2 && vmm_is_user_addr_range_writable(arg2, sizeof(int))) {
      *(int *)arg2 = current->pdeath_signal;
      return 0;
    }
    return (uint64_t)-14;
  case 3: // PR_GET_DUMPABLE
    return 1;
  case 4: // PR_SET_DUMPABLE
    return 0;
  case PR_SET_NAME: {
    if (!arg2 || !vmm_is_user_addr_range_valid(arg2, 1))
      return (uint64_t)-14; // EFAULT

    const char *name = (const char *)arg2;
    // Copy up to 15 chars + null terminator
    strncpy(current->comm, name, 15);
    current->comm[15] = '\0';
    return 0;
  }
  case PR_GET_NAME: {
    if (!arg2 || !vmm_is_user_addr_range_valid(arg2, 16))
      return (uint64_t)-14; // EFAULT

    char *out = (char *)arg2;
    strncpy(out, current->comm, 16);
    return 0;
  }
  case 7: case 8: case 23: case 24: case 27: case 28:
    return capability_prctl(current, option, arg2, arg3, arg4, arg5);
  case 13: // PR_GET_TIMING
    return 0;
  case 14: // PR_SET_TIMING
    return 0;
  case 19: // PR_GET_ENDIAN
    if (arg2 && vmm_is_user_addr_range_writable(arg2, sizeof(int))) {
      *(int *)arg2 = 1; // PR_ENDIAN_LITTLE
      return 0;
    }
    return (uint64_t)-14;
  case 20: // PR_SET_ENDIAN
    return (arg2 == 1) ? 0 : (uint64_t)-22;
  case 21: // PR_GET_SECCOMP
    return seccomp_mode(current);
  case 22: // PR_SET_SECCOMP
    if (arg2 != 2) return (uint64_t)-22;
    return (uint64_t)seccomp_control(1, 0, arg3);
  case 25: // PR_GET_TSC
    if (arg2 && vmm_is_user_addr_range_writable(arg2, sizeof(int))) {
      *(int *)arg2 = 1; // PR_TSC_ENABLE
      return 0;
    }
    return (uint64_t)-14;
  case 26: // PR_SET_TSC
    return 0;
  case 29: // PR_SET_TIMERSLACK
    return 0;
  case 30: // PR_GET_TIMERSLACK
    return 50000; // 50 microseconds
  case 35: { // PR_SET_MM (0x23)
    if (!current->mm)
      return 0;
    if (arg2 == 8 /* PR_SET_MM_ARG_START */) {
      current->mm->arg_start = arg3;
    } else if (arg2 == 9 /* PR_SET_MM_ARG_END */) {
      current->mm->arg_end = arg3;
    }
    return 0;
  }
  case 36: // PR_SET_CHILD_SUBREAPER
    return 0;
  case 37: // PR_GET_CHILD_SUBREAPER
    if (arg2 && vmm_is_user_addr_range_writable(arg2, sizeof(int))) {
      *(int *)arg2 = 0;
      return 0;
    }
    return (uint64_t)-14;
  case 38: // PR_SET_NO_NEW_PRIVS
    if (arg2 != 1 || arg3 || arg4 || arg5) return (uint64_t)-22;
    __atomic_store_n(&current->no_new_privs, true, __ATOMIC_RELEASE);
    return 0;
  case 39: // PR_GET_NO_NEW_PRIVS
    if (arg2 || arg3 || arg4 || arg5) return (uint64_t)-22;
    return __atomic_load_n(&current->no_new_privs, __ATOMIC_ACQUIRE);
  case 41: // PR_SET_THP_DISABLE
    return 0;
  case 42: // PR_GET_THP_DISABLE
    return 0;
  case 47:
    return capability_prctl(current, option, arg2, arg3, arg4, arg5);
  case 40: { // PR_GET_TID_ADDRESS: pointer width follows the caller ABI.
    uint64_t value = (uint64_t)current->tid_address;
    if (current->seccomp_compat_dispatch) {
      uint32_t narrow = (uint32_t)value;
      return copy_to_user((void *)arg2, &narrow, sizeof(narrow)) ? (uint64_t)-14 : 0;
    }
    return copy_to_user((void *)arg2, &value, sizeof(value)) ? (uint64_t)-14 : 0;
  }
  case 52: // PR_GET_SPECULATION_CTRL
    return 0; // PR_SPEC_NOT_AFFECTED
  case 53: // PR_SET_SPECULATION_CTRL
    return 0;
  case 55: // PR_SET_TAGGED_ADDR_CTRL
    return 0;
  case 56: // PR_GET_TAGGED_ADDR_CTRL
    return 0;
  case 0x59616d61: // PR_SET_PTRACER
    return 0;
  case 0x53564d41: // PR_SET_VMA
    return 0;
  case 0x41555856: { // PR_GET_AUXV (Linux 6.4+)    if (!arg2 || !arg3)
      return (uint64_t)-14; // EFAULT
    if (!current->mm)
      return 0;

    static const uint64_t fallback_auxv[] = {
        6 /* AT_PAGESZ */, 4096,
        17 /* AT_CLKTCK */, 100,
        33 /* AT_SYSINFO_EHDR */, 0x700000000000ULL,
        0 /* AT_NULL */, 0
    };

    const void *src = (current->mm->auxv_count > 0)
                          ? (const void *)current->mm->saved_auxv
                          : (const void *)fallback_auxv;
    size_t aux_bytes = (current->mm->auxv_count > 0)
                           ? (current->mm->auxv_count * sizeof(uint64_t))
                           : sizeof(fallback_auxv);

    size_t to_copy = (arg3 < aux_bytes) ? (size_t)arg3 : aux_bytes;
    if (!vmm_is_user_addr_range_writable(arg2, to_copy))
      return (uint64_t)-14; // EFAULT
    memcpy((void *)arg2, src, to_copy);
    return (uint64_t)to_copy;
  }
  case 0x50544344: { // TEMPORARY "PTCD": dump TSC probe stats (syscall profiler)
    tsc_probe_dump();
    return 0;
  }
  default: {
    char opt_str[32];
    snprintf(opt_str, sizeof(opt_str), "option=0x%llx", (unsigned long long)option);
    sysdiag_record_unimplemented_feature("prctl", opt_str, current, -22);
    return (uint64_t)-22; // EINVAL
  }
  }
}

// sys_umask
static uint64_t sys_umask(struct syscall_regs *regs) {
  uint32_t mask = (uint32_t)regs->rdi;
  struct thread *t = sched_get_current();
  if (!t)
    return 0;

  uint32_t old_mask = t->umask;
  t->umask = mask & 0777;
  return old_mask;
}

// sys_getuid
static uint64_t sys_getuid(struct syscall_regs *regs) {
  (void)regs;
  struct thread *t = sched_get_current();
  if (!t)
    return 0;
  return t->uid;
}

// sys_getgid
static uint64_t sys_getgid(struct syscall_regs *regs) {
  (void)regs;
  struct thread *t = sched_get_current();
  if (!t)
    return 0;
  return t->gid;
}

// sys_geteuid
static uint64_t sys_geteuid(struct syscall_regs *regs) {
  (void)regs;
  struct thread *t = sched_get_current();
  if (!t)
    return 0;
  return t->euid;
}

// sys_getegid
static uint64_t sys_getegid(struct syscall_regs *regs) {
  (void)regs;
  struct thread *t = sched_get_current();
  if (!t)
    return 0;
  return t->egid;
}

// sys_getresuid
static uint64_t sys_getresuid(struct syscall_regs *regs) {
  uint32_t *ruid_ptr = (uint32_t *)regs->rdi;
  uint32_t *euid_ptr = (uint32_t *)regs->rsi;
  uint32_t *suid_ptr = (uint32_t *)regs->rdx;

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  if (ruid_ptr) {
    if (!vmm_is_user_addr_range_valid((uint64_t)ruid_ptr, sizeof(uint32_t)))
      return (uint64_t)-14;
    *ruid_ptr = t->uid;
  }
  if (euid_ptr) {
    if (!vmm_is_user_addr_range_valid((uint64_t)euid_ptr, sizeof(uint32_t)))
      return (uint64_t)-14;
    *euid_ptr = t->euid;
  }
  if (suid_ptr) {
    if (!vmm_is_user_addr_range_valid((uint64_t)suid_ptr, sizeof(uint32_t)))
      return (uint64_t)-14;
    *suid_ptr = t->suid;
  }

  return 0;
}

// sys_getresgid
static uint64_t sys_getresgid(struct syscall_regs *regs) {
  uint32_t *rgid_ptr = (uint32_t *)regs->rdi;
  uint32_t *egid_ptr = (uint32_t *)regs->rsi;
  uint32_t *sgid_ptr = (uint32_t *)regs->rdx;

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  if (rgid_ptr) {
    if (!vmm_is_user_addr_range_valid((uint64_t)rgid_ptr, sizeof(uint32_t)))
      return (uint64_t)-14;
    *rgid_ptr = t->gid;
  }
  if (egid_ptr) {
    if (!vmm_is_user_addr_range_valid((uint64_t)egid_ptr, sizeof(uint32_t)))
      return (uint64_t)-14;
    *egid_ptr = t->egid;
  }
  if (sgid_ptr) {
    if (!vmm_is_user_addr_range_valid((uint64_t)sgid_ptr, sizeof(uint32_t)))
      return (uint64_t)-14;
    *sgid_ptr = t->sgid;
  }

  return 0;
}

// sys_setresuid
static uint64_t sys_setresuid(struct syscall_regs *regs) {
  uint32_t ruid = (uint32_t)regs->rdi, euid = (uint32_t)regs->rsi;
  uint32_t suid = (uint32_t)regs->rdx;
  struct thread *t = sched_get_current();
  if (!t) return (uint64_t)-1;
  uint32_t old_uid = t->uid, old_euid = t->euid, old_suid = t->suid;
  if (!capability_has(t, CAP_SETUID)) {
    if (ruid != UINT32_MAX && ruid != t->uid && ruid != t->euid && ruid != t->suid) return (uint64_t)-1;
    if (euid != UINT32_MAX && euid != t->uid && euid != t->euid && euid != t->suid) return (uint64_t)-1;
    if (suid != UINT32_MAX && suid != t->uid && suid != t->euid && suid != t->suid) return (uint64_t)-1;
  }
  if (ruid != UINT32_MAX) t->uid = ruid;
  if (euid != UINT32_MAX) t->euid = euid;
  if (suid != UINT32_MAX) t->suid = suid;
  t->fsuid = t->euid;
  capability_uid_changed(t, old_uid, old_euid, old_suid);
  return 0;
}

// sys_setresgid
static uint64_t sys_setresgid(struct syscall_regs *regs) {
  uint32_t rgid = (uint32_t)regs->rdi, egid = (uint32_t)regs->rsi;
  uint32_t sgid = (uint32_t)regs->rdx;
  struct thread *t = sched_get_current();
  if (!t) return (uint64_t)-1;
  if (!capability_has(t, CAP_SETGID)) {
    if (rgid != UINT32_MAX && rgid != t->gid && rgid != t->egid && rgid != t->sgid) return (uint64_t)-1;
    if (egid != UINT32_MAX && egid != t->gid && egid != t->egid && egid != t->sgid) return (uint64_t)-1;
    if (sgid != UINT32_MAX && sgid != t->gid && sgid != t->egid && sgid != t->sgid) return (uint64_t)-1;
  }
  if (rgid != UINT32_MAX) t->gid = rgid;
  if (egid != UINT32_MAX) t->egid = egid;
  if (sgid != UINT32_MAX) t->sgid = sgid;
  t->fsgid = t->egid;
  return 0;
}

// sys_setuid (syscall 105)
static uint64_t sys_setuid(struct syscall_regs *regs) {
  uint32_t uid = (uint32_t)regs->rdi;
  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;
  uint32_t old_uid = t->uid, old_euid = t->euid, old_suid = t->suid;
  if (capability_has(t, CAP_SETUID)) {
    t->uid = t->euid = t->suid = t->fsuid = uid;
  } else if (uid == t->uid || uid == t->suid) {
    t->euid = t->fsuid = uid;
  } else {
    return (uint64_t)-1;
  }
  capability_uid_changed(t, old_uid, old_euid, old_suid);
  return 0;
}

// sys_setgid (syscall 106)
static uint64_t sys_setgid(struct syscall_regs *regs) {
  uint32_t gid = (uint32_t)regs->rdi;
  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;
  if (capability_has(t, CAP_SETGID)) {
    t->gid = t->egid = t->sgid = t->fsgid = gid;
  } else if (gid == t->gid || gid == t->sgid) {
    t->egid = t->fsgid = gid;
  } else {
    return (uint64_t)-1;
  }
  return 0;
}

// sys_setreuid (syscall 113)
static uint64_t sys_setreuid(struct syscall_regs *regs) {
  uint32_t ruid = (uint32_t)regs->rdi;
  uint32_t euid = (uint32_t)regs->rsi;
  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;
  uint32_t old_uid = t->uid, old_euid = t->euid, old_suid = t->suid;

  if (!capability_has(t, CAP_SETUID)) {
    if (ruid != UINT32_MAX && ruid != t->uid && ruid != t->euid)
      return (uint64_t)-1;
    if (euid != UINT32_MAX && euid != t->uid && euid != t->euid && euid != t->suid)
      return (uint64_t)-1;
  }

  if (ruid != UINT32_MAX) {
    t->uid = ruid;
  }
  if (euid != UINT32_MAX) {
    t->euid = euid;
  }
  if (ruid != UINT32_MAX || (euid != UINT32_MAX && euid != t->uid)) {
    t->suid = t->euid;
  }
  t->fsuid = t->euid;
  capability_uid_changed(t, old_uid, old_euid, old_suid);
  return 0;
}

// sys_setregid (syscall 114)
static uint64_t sys_setregid(struct syscall_regs *regs) {
  uint32_t rgid = (uint32_t)regs->rdi;
  uint32_t egid = (uint32_t)regs->rsi;
  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  if (!capability_has(t, CAP_SETGID)) {
    if (rgid != UINT32_MAX && rgid != t->gid && rgid != t->egid)
      return (uint64_t)-1;
    if (egid != UINT32_MAX && egid != t->gid && egid != t->egid && egid != t->sgid)
      return (uint64_t)-1;
  }

  if (rgid != UINT32_MAX) {
    t->gid = rgid;
  }
  if (egid != UINT32_MAX) {
    t->egid = egid;
  }
  if (rgid != UINT32_MAX || (egid != UINT32_MAX && egid != t->gid)) {
    t->sgid = t->egid;
  }
  t->fsgid = t->egid;
  return 0;
}

// sys_setfsuid (syscall 122)
static uint64_t sys_setfsuid(struct syscall_regs *regs) {
  uint32_t fsuid = (uint32_t)regs->rdi;
  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;
  // Return the previous fsuid value
  uint32_t old_fsuid = t->fsuid;
  if (capability_has(t, CAP_SETUID) || fsuid == t->uid || fsuid == t->euid ||
      fsuid == t->suid || fsuid == t->fsuid)
    t->fsuid = fsuid;
  return old_fsuid;
}

// sys_setfsgid (syscall 123)
static uint64_t sys_setfsgid(struct syscall_regs *regs) {
  uint32_t fsgid = (uint32_t)regs->rdi;
  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;
  // Return the previous fsgid value
  uint32_t old_fsgid = t->fsgid;
  if (capability_has(t, CAP_SETGID) || fsgid == t->gid || fsgid == t->egid ||
      fsgid == t->sgid || fsgid == t->fsgid)
    t->fsgid = fsgid;
  return old_fsgid;
}

static uint64_t sys_getgroups(struct syscall_regs *regs) {
  int size = (int)regs->rdi;
  uint32_t *list = (uint32_t *)regs->rsi;
  struct thread *t = sched_get_current();
  if (!t || size < 0) return (uint64_t)-22;
  if (size == 0) return t->supplementary_group_count;
  if ((uint32_t)size < t->supplementary_group_count) return (uint64_t)-22;
  if (!list || !vmm_is_user_addr_range_writable((uint64_t)list,
      t->supplementary_group_count * sizeof(uint32_t))) return (uint64_t)-14;
  memcpy(list, t->supplementary_groups,
         t->supplementary_group_count * sizeof(uint32_t));
  return t->supplementary_group_count;
}

static uint64_t sys_setgroups(struct syscall_regs *regs) {
  uint32_t size = (uint32_t)regs->rdi;
  const uint32_t *list = (const uint32_t *)regs->rsi;
  struct thread *t = sched_get_current();
  if (!t || !capability_has(t, CAP_SETGID)) return (uint64_t)-1;
  if (size > MAX_SUPPLEMENTARY_GROUPS) return (uint64_t)-22;
  if (size && (!list || !vmm_is_user_addr_range_valid((uint64_t)list,
      size * sizeof(uint32_t)))) return (uint64_t)-14;
  if (size) memcpy(t->supplementary_groups, list, size * sizeof(uint32_t));
  t->supplementary_group_count = size;
  return 0;
}

// sys_getppid
static uint64_t sys_getppid(struct syscall_regs *regs) {
  (void)regs;
  struct thread *t = sched_get_current();
  if (!t) return 0;
  spinlock_acquire(&tid_lock);
  uint32_t pid = t->parent ? pid_namespace_visible_locked(t, t->parent->tgid) : 0;
  spinlock_release(&tid_lock);
  return pid;
}

// sys_setpgid
static uint64_t sys_setpgid(struct syscall_regs *regs) {
  uint32_t pid = (uint32_t)regs->rdi;
  uint32_t pgid = (uint32_t)regs->rsi;
  struct thread *current = sched_get_current();
  struct thread *target = NULL;
  if (pid) { pid = pid_namespace_global(current, pid); if (!pid) return (uint64_t)-3; }
  if (pgid) { pgid = pid_namespace_global(current, pgid); if (!pgid) return (uint64_t)-1; }

  if (pid == 0) {
    target = current;
  } else {
    // Only allow setting PGID for self or children
    if (current->tid == pid) {
      target = current;
    } else {
      spinlock_acquire(&tid_lock);
      struct thread *c = current->children;
      while (c) {
        if (c->tid == pid) {
          target = c;
          break;
        }
        c = c->sibling_next;
      }
      spinlock_release(&tid_lock);
    }
  }

  if (!target)
    return (uint64_t)-3; // ESRCH

  if (pgid == 0)
    pgid = target->tid;
  target->pgid = pgid;
  return 0;
}

static uint64_t sys_getpgid(uint64_t pid, uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *current = sched_get_current();
  if (pid == 0)
    return pid_namespace_visible(current, current->pgid);
  struct thread *target = sched_get_thread_by_tid(pid_namespace_global(current, (uint32_t)pid));
  if (!target)
    return (uint64_t)-3; // ESRCH
  return pid_namespace_visible(current, target->pgid);
}

static uint64_t sys_getsid(uint64_t pid, uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-3; // ESRCH
  if (pid == 0)
    return pid_namespace_visible(current, current->sid ? current->sid : current->tgid);
  struct thread *target = sched_get_thread_by_tid(pid_namespace_global(current, (uint32_t)pid));
  if (!target)
    return (uint64_t)-3; // ESRCH
  return pid_namespace_visible(current, target->sid ? target->sid : target->tgid);
}

// sys_getpgrp
static uint64_t sys_getpgrp(struct syscall_regs *regs) {
  (void)regs;
  struct thread *t = sched_get_current();
  if (!t)
    return 0;
  return pid_namespace_visible(t, t->pgid);
}

// sys_rseq: restartable sequences (Linux rseq(2) ABI).
//
// Registration model (matches Linux): each thread registers one 32-byte
// struct rseq { cpu_id_start, cpu_id, rseq_cs, flags, node_id, mm_cid }.
// The kernel keeps cpu_id/cpu_id_start/node_id current on every return to
// user mode (see rseq_update_cpu_id callers in syscall.c / isr.c) and
// redirects RIP to abort_ip when a preemption/signal/migration interrupts
// an active critical section (see rseq_maybe_abort).
//
// Preemption *between* user instructions always funnels back through a
// return-to-user path (syscall tail, interrupt return, signal delivery),
// so no user-memory access happens in sched_schedule() itself, where the
// runqueue lock is held and IRQs are masked.
#define RSEQ_LEN 32
#define RSEQ_FLAG_UNREGISTER 1
#define RSEQ_CPU_ID_UNINITIALIZED 0xFFFFFFFFu
#define RSEQ_CS_FLAG_NO_RESTART_ON_PREEMPT 1
#define RSEQ_CS_FLAG_NO_RESTART_ON_SIGNAL 2
#define RSEQ_CS_FLAG_NO_RESTART_ON_MIGRATE 4

struct rseq_cs_abi {
  uint32_t version;
  uint32_t flags;
  uint64_t start_ip;
  uint64_t post_commit_offset;
  uint64_t abort_ip;
};

bool rseq_update_cpu_id(struct thread *t) {
  if (!t || !t->rseq_addr)
    return true;
  struct cpu_info *cpu = cpu_get_current();
  uint32_t id = cpu ? cpu->cpu_id : 0;
  if (__builtin_expect(t->rseq_last_cpu_id == id, 1))
    return true;
  uint64_t ids = ((uint64_t)id << 32) | id;
  uint64_t node_mm = 0;
  // Fault-safe: an unmapped/torn-down rseq area just keeps stale values;
  // the next successful update fixes it. Offsets per Linux struct rseq:
  // cpu_id_start@0, cpu_id@4, flags@16, node_id@20, mm_cid@24.
  unsigned long left = 0;
  left |= copy_to_user((void *)(t->rseq_addr + 0), &ids, sizeof(ids));
  left |= copy_to_user((void *)(t->rseq_addr + 20), &node_mm, sizeof(node_mm));
  if (left == 0)
    t->rseq_last_cpu_id = id;
  return left == 0;
}

bool rseq_maybe_abort(struct thread *t, uint64_t *rip_inout) {
  if (!t || !t->rseq_addr || !rip_inout)
    return false;
  uint64_t cs_ptr = 0;
  if (copy_from_user(&cs_ptr, (const void *)(t->rseq_addr + 8),
                     sizeof(cs_ptr)) != 0)
    return false;
  if (!cs_ptr)
    return false; // Not inside a critical section.
  struct rseq_cs_abi cs;
  if (copy_from_user(&cs, (const void *)cs_ptr, sizeof(cs)) != 0)
    return false;
  if (cs.version != 0)
    return false;
  uint64_t rip = *rip_inout;
  if (rip < cs.start_ip || rip >= cs.start_ip + cs.post_commit_offset)
    return false; // Outside the critical section: nothing to abort.
  if (cs.flags & (RSEQ_CS_FLAG_NO_RESTART_ON_PREEMPT |
                  RSEQ_CS_FLAG_NO_RESTART_ON_SIGNAL |
                  RSEQ_CS_FLAG_NO_RESTART_ON_MIGRATE))
    return false; // Userspace asked to handle this event itself.
  if (t->rseq_sig != 0) {
    uint32_t sig_at_abort = 0;
    if (copy_from_user(&sig_at_abort, (const void *)cs.abort_ip,
                       sizeof(sig_at_abort)) != 0)
      return false;
    if (sig_at_abort != t->rseq_sig)
      return false; // Signature mismatch: refuse to redirect.
  }
  uint64_t zero = 0;
  copy_to_user((void *)(t->rseq_addr + 8), &zero, sizeof(zero));
  *rip_inout = cs.abort_ip;
  return true;
}

uint64_t sys_rseq(struct syscall_regs *regs) {
  uint64_t addr = regs->rdi;
  uint64_t len = regs->rsi;
  uint64_t flags = regs->rdx;
  uint64_t sig = regs->r10; // 4th syscall arg arrives in r10, not rcx.
  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-14; // EFAULT: no thread to register for.

  if (flags & (uint64_t)~(uint64_t)RSEQ_FLAG_UNREGISTER)
    return (uint64_t)-22; // EINVAL: unknown flags.

  if (flags & RSEQ_FLAG_UNREGISTER) {
    // Idempotent: unregistering when nothing is registered succeeds.
    current->rseq_addr = 0;
    current->rseq_len = 0;
    current->rseq_sig = 0;
    current->rseq_last_cpu_id = RSEQ_CPU_ID_UNINITIALIZED;
    return 0;
  }

  if (len != RSEQ_LEN)
    return (uint64_t)-22; // EINVAL: struct rseq is 32 bytes.
  if (addr & 31)
    return (uint64_t)-22; // EINVAL: 32-byte alignment required.
  if (!vmm_is_user_addr_range_writable(addr, RSEQ_LEN))
    return (uint64_t)-14; // EFAULT.
  if (current->rseq_addr && current->rseq_addr != addr)
    return (uint64_t)-16; // EBUSY: already registered elsewhere.

  current->rseq_addr = addr;
  current->rseq_len = RSEQ_LEN;
  current->rseq_sig = (uint32_t)sig;
  current->rseq_last_cpu_id = RSEQ_CPU_ID_UNINITIALIZED;

  // Publish the initial state so glibc never observes UNINITIALIZED:
  // flags/node/mm_cid zero, cpu ids = current CPU. Every copy is checked:
  // returning 0 while leaving cpu_id == UNINITIALIZED makes glibc abort
  // with the same "rseq registration failed" message as a syscall error.
  {
    uint32_t zero = 0;
    unsigned long left = 0;
    left |= copy_to_user((void *)(addr + 16), &zero, sizeof(zero)); // flags
    left |= copy_to_user((void *)(addr + 20), &zero, sizeof(zero)); // node_id
    left |= copy_to_user((void *)(addr + 24), &zero, sizeof(zero)); // mm_cid
    if (left != 0) {
      current->rseq_addr = 0;
      current->rseq_len = 0;
      current->rseq_sig = 0;
      return (uint64_t)-14; // EFAULT.
    }
  }
  if (!rseq_update_cpu_id(current)) {
    current->rseq_addr = 0;
    current->rseq_len = 0;
    current->rseq_sig = 0;
    return (uint64_t)-14; // EFAULT: could not publish cpu_id.
  }
  return 0;
}

// sys_setsid
static uint64_t sys_setsid(struct syscall_regs *regs) {
  (void)regs;
  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-1;

  // POSIX: setsid() operates on the calling *process* (thread group), not an
  // individual thread. Use tgid (== tid for the group leader/single-threaded
  // case) so a non-leader thread of a group-leader process still gets EPERM.
  uint32_t pid = current->tgid ? current->tgid : current->tid;
  // POSIX: A process group leader cannot create a new session.
  // PID 1 (init) is pgid 1 by design (see kernel.c), so setsid() from init
  // correctly returns -EPERM; callers are expected to fork first.
  if (pid == current->pgid) {
    return (uint64_t)-1; // -EPERM
  }

  current->sid = pid;
  current->pgid = pid;
  current->ctty = NULL; // Clear controlling terminal

  klog_debugf("[SETSID] New session created: %llu\n", (unsigned long long)current->sid);

  return pid_namespace_visible(current, current->sid);
}

struct kernel_timeval {
  int64_t tv_sec;
  int64_t tv_usec;
};

struct kernel_itimerval {
  struct kernel_timeval it_interval;
  struct kernel_timeval it_value;
};

#define ITIMER_REAL 0

static void ms_to_timeval(uint64_t ms, struct kernel_timeval *tv) {
  tv->tv_sec = (int64_t)(ms / 1000);
  tv->tv_usec = (int64_t)((ms % 1000) * 1000);
}

static bool timeval_to_ms(const struct kernel_timeval *tv, uint64_t *ms) {
  if (tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec >= 1000000)
    return false;
  if ((uint64_t)tv->tv_sec > (UINT64_MAX - 999) / 1000)
    return false;
  *ms = (uint64_t)tv->tv_sec * 1000 +
        ((uint64_t)tv->tv_usec + 999) / 1000;
  return true;
}

static uint64_t sys_setitimer(uint64_t which, uint64_t new_value_ptr,
                              uint64_t old_value_ptr, uint64_t a3, uint64_t a4,
                              uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  if (which != ITIMER_REAL)
    return (uint64_t)-22;
  if (!new_value_ptr ||
      !vmm_is_user_addr_range_valid(new_value_ptr,
                                    sizeof(struct kernel_itimerval)))
    return (uint64_t)-14;
  if (old_value_ptr &&
      !vmm_is_user_addr_range_writable(old_value_ptr,
                                    sizeof(struct kernel_itimerval)))
    return (uint64_t)-14;

  /* Copy input first because old_value is allowed to alias new_value. */
  struct kernel_itimerval new_value;
  memcpy(&new_value, (const void *)new_value_ptr, sizeof(new_value));

  uint64_t value_ms;
  uint64_t interval_ms;
  if (!timeval_to_ms(&new_value.it_value, &value_ms) ||
      !timeval_to_ms(&new_value.it_interval, &interval_ms))
    return (uint64_t)-22;

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-22;

  uint64_t now = lapic_timer_get_ms();
  struct kernel_itimerval old_value = {0};
  uint64_t remaining_ms =
      t->it_real_next > now ? t->it_real_next - now : 0;
  ms_to_timeval(remaining_ms, &old_value.it_value);
  ms_to_timeval(t->it_real_interval, &old_value.it_interval);

  t->it_real_value = value_ms;
  t->it_real_interval = interval_ms;
  /* Route through the arm/disarm helpers: the tick fast-path skips the
   * whole itimer scan while active_itimer_count==0, so writing it_real_next
   * directly (as here before) leaves the timer armed but never delivered -
   * alarm(2) would never fire. The helpers also maintain
   * earliest_itimer_deadline and handle re-arm accounting. */
  if (value_ms)
    sched_itimer_arm(t, now + value_ms, interval_ms, value_ms);
  else
    sched_itimer_disarm(t);

  if (old_value_ptr)
    memcpy((void *)old_value_ptr, &old_value, sizeof(old_value));
  return 0;
}

static uint64_t sys_alarm(uint64_t seconds, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
  struct thread *t = sched_get_current();
  if (!t) return 0;
  uint64_t now = lapic_timer_get_ticks();
  uint64_t remaining = t->it_real_next > now ? (t->it_real_next - now + 999) / 1000 : 0;
  /* Same accounting as setitimer: bypassing sched_itimer_arm leaves
   * active_itimer_count at zero and the tick scan never delivers SIGALRM. */
  if (seconds)
    sched_itimer_arm(t, now + seconds * 1000, 0, seconds * 1000);
  else
    sched_itimer_disarm(t);
  return remaining;
}

// getrusage (syscall 98)
#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)
#define RUSAGE_THREAD   1

struct rusage {
  struct { int64_t tv_sec; int64_t tv_usec; } ru_utime; // user CPU time
  struct { int64_t tv_sec; int64_t tv_usec; } ru_stime; // system CPU time
  int64_t ru_maxrss;     // maximum resident set size (KB)
  int64_t ru_ixrss;      // integral shared memory size
  int64_t ru_idrss;      // integral unshared data size
  int64_t ru_isrss;      // integral unshared stack size
  int64_t ru_minflt;     // page reclaims (soft page faults)
  int64_t ru_majflt;     // page faults (hard page faults)
  int64_t ru_nswap;      // swaps
  int64_t ru_inblock;    // block input operations
  int64_t ru_oublock;    // block output operations
  int64_t ru_msgsnd;     // IPC messages sent
  int64_t ru_msgrcv;     // IPC messages received
  int64_t ru_nsignals;   // signals received
  int64_t ru_nvcsw;      // voluntary context switches
  int64_t ru_nivcsw;     // involuntary context switches
};

static uint64_t sys_getrusage(uint64_t who, uint64_t usage_ptr, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct rusage *ru = (struct rusage *)usage_ptr;
  if (!ru || !vmm_is_user_addr_range_writable((uint64_t)ru, sizeof(*ru)))
    return (uint64_t)-14; // EFAULT

  int who_int = (int)(int64_t)who;
  if (who_int != RUSAGE_SELF && who_int != RUSAGE_CHILDREN &&
      who_int != RUSAGE_THREAD)
    return (uint64_t)-22; // EINVAL

  memset(ru, 0, sizeof(*ru));

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-22;

  if (who_int == RUSAGE_SELF || who_int == RUSAGE_THREAD) {
    // runtime_total is in LAPIC ticks (1 tick ≈ 1ms)
    // Report as user time (we don't separately track user vs kernel time)
    uint64_t total_ms = t->runtime_total;
    ru->ru_utime.tv_sec = (int64_t)(total_ms / 1000);
    ru->ru_utime.tv_usec = (int64_t)((total_ms % 1000) * 1000);

    // Report resident memory if mm is available
    if (t->mm) {
      // Convert bytes to KB for ru_maxrss (Linux convention)
      uint64_t resident = 0;
      spinlock_acquire(&t->mm->lock);
      // Estimate from brk range + mmap allocations
      if (t->mm->brk_current > t->mm->brk_base)
        resident += t->mm->brk_current - t->mm->brk_base;
      spinlock_release(&t->mm->lock);
      ru->ru_maxrss = (int64_t)(resident / 1024);
      if (ru->ru_maxrss == 0)
        ru->ru_maxrss = 4; // minimum 4 KB
    }
  }
  // RUSAGE_CHILDREN: leave zeroed (we don't accumulate child usage yet)

  return 0;
}

// Resource Limits (getrlimit / prlimit64)
struct rlimit {
  uint64_t rlim_cur;
  uint64_t rlim_max;
};

#define RLIM_INFINITY ((uint64_t)-1)

/* The effective limits are still fixed, but every ABI transfer must use
 * fault-safe user copies: Chromium can pass buffers protected with mprotect. */
static struct rlimit resource_limit_value(uint32_t resource) {
  struct rlimit limit = {RLIM_INFINITY, RLIM_INFINITY};
  if (resource == 7) { /* RLIMIT_NOFILE */
    limit.rlim_cur = limit.rlim_max = MAX_FDS;
  } else if (resource == 4) { /* RLIMIT_CORE */
    limit.rlim_cur = 0;
  } else if (resource == 3) { /* RLIMIT_STACK */
    limit.rlim_cur = limit.rlim_max = 8 * 1024 * 1024;
  }
  return limit;
}

static int resource_limit_read(uint64_t user, struct rlimit *limit) {
  if (copy_from_user(limit, (const void *)user, sizeof(*limit)) != 0)
    return -14;
  return limit->rlim_cur > limit->rlim_max ? -22 : 0;
}

static uint64_t sys_getrlimit(uint64_t resource_arg, uint64_t rlim_ptr,
                              uint64_t a2, uint64_t a3, uint64_t a4,
                              uint64_t a5) {
  (void)a2; (void)a3; (void)a4; (void)a5;
  uint32_t resource = (uint32_t)resource_arg;
  if (resource >= 16) return (uint64_t)-22;
  struct rlimit limit = resource_limit_value(resource);
  struct thread *t = sched_get_current();
  if (t && t->seccomp_compat_dispatch) {
    uint32_t narrow[2] = {
      limit.rlim_cur > UINT32_MAX ? UINT32_MAX : (uint32_t)limit.rlim_cur,
      limit.rlim_max > UINT32_MAX ? UINT32_MAX : (uint32_t)limit.rlim_max
    };
    return copy_to_user((void *)rlim_ptr, narrow, sizeof(narrow)) ? (uint64_t)-14 : 0;
  }
  return copy_to_user((void *)rlim_ptr, &limit, sizeof(limit)) != 0
             ? (uint64_t)-14 : 0;
}

static uint64_t sys_prlimit64(struct syscall_regs *regs) {
  uint32_t pid = (uint32_t)regs->rdi;
  uint32_t resource = (uint32_t)regs->rsi;
  if (resource >= 16) return (uint64_t)-22;
  struct rlimit requested;
  if (regs->rdx) {
    int error = resource_limit_read(regs->rdx, &requested);
    if (error) return (uint64_t)(int64_t)error;
  }
  if (pid != 0) {
    struct thread *current = sched_get_current();
    struct sched_thread_snapshot target;
    if (!sched_get_thread_snapshot(pid_namespace_global(sched_get_current(), pid), &target)) return (uint64_t)-3;
    if (!current || (target.tgid ? target.tgid : target.tid) !=
                        (current->tgid ? current->tgid : current->tid))
      return (uint64_t)-1; /* Other processes remain unsupported. */
  }
  if (regs->r10) {
    struct rlimit previous = resource_limit_value(resource);
    if (copy_to_user((void *)regs->r10, &previous, sizeof(previous)) != 0)
      return (uint64_t)-14;
  }
  /* Preserve the existing fixed-limit policy for valid setter requests. */
  return 0;
}

static uint64_t sys_setrlimit(uint64_t resource_arg, uint64_t rlim_ptr,
                              uint64_t a2, uint64_t a3, uint64_t a4,
                              uint64_t a5) {
  (void)a2; (void)a3; (void)a4; (void)a5;
  if ((uint32_t)resource_arg >= 16) return (uint64_t)-22;
  struct rlimit requested;
  return (uint64_t)(int64_t)resource_limit_read(rlim_ptr, &requested);
}

static uint64_t sys_membarrier(uint64_t cmd, uint64_t flags, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)cmd;
  (void)flags;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  // Stub: success
  return 0;
}

// Registration
static uint64_t sys_sched_yield(uint64_t a0, uint64_t a1, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  sched_yield_user();
  return 0;
}

static uint64_t sys_sched_setaffinity(uint64_t pid, uint64_t len,
                                      uint64_t user_mask_ptr, uint64_t a3,
                                      uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (!user_mask_ptr)
    return (uint64_t)-14; // EFAULT
  if (len < sizeof(uint64_t))
    return (uint64_t)-22; // EINVAL

  struct thread *current = sched_get_current();
  struct thread *target =
      pid == 0 ? current : sched_get_thread_by_tid(pid_namespace_global(sched_get_current(), (uint32_t)pid));
  if (!target)
    return (uint64_t)-3; // ESRCH

  if (!vmm_is_user_addr_range_valid(user_mask_ptr, sizeof(uint64_t)))
    return (uint64_t)-14;

  /* SMAP-safe copy: the old direct dereference faulted under SMAP and made
   * every affinity request fail silently. */
  uint64_t mask = 0;
  if (copy_from_user(&mask, (const void *)user_mask_ptr, sizeof(mask)) != 0)
    return (uint64_t)-14;

  // Sanity check: must have at least one valid CPU in the mask
  uint32_t cpu_count = cpu_get_count();
  uint64_t all_cpus_mask = (1ULL << cpu_count) - 1;
  if (cpu_count >= 64)
    all_cpus_mask = ~0ULL;

  if (!(mask & all_cpus_mask))
    return (uint64_t)-22; // EINVAL

  __atomic_store_n(&target->cpu_affinity, mask & all_cpus_mask, __ATOMIC_RELEASE);
  sched_enforce_affinity(target);
  return 0;
}

static uint64_t sys_sched_getaffinity(uint64_t pid, uint64_t len,
                                      uint64_t user_mask_ptr, uint64_t a3,
                                      uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (!user_mask_ptr)
    return (uint64_t)-14; // EFAULT

  struct thread *current = sched_get_current();
  struct thread *target =
      pid == 0 ? current : sched_get_thread_by_tid(pid_namespace_global(sched_get_current(), (uint32_t)pid));
  if (!target)
    return (uint64_t)-3; // ESRCH

  uint32_t cpu_count = cpu_get_count();
  uint64_t mask = target->cpu_affinity;
  if (mask == 0) {
    mask = (1ULL << cpu_count) - 1;
    if (cpu_count >= 64)
      mask = ~0ULL;
  }

  if (len < sizeof(uint64_t)) {
    return (uint64_t)-22; // EINVAL
  }

  size_t copy_bytes = len;
  if (copy_bytes > 128)
    copy_bytes = 128; // Standard Linux cpuset max 1024 bits

  if (!vmm_is_user_addr_range_writable(user_mask_ptr, copy_bytes))
    return (uint64_t)-14; // EFAULT

  /* SMAP-safe: stage in a kernel buffer and copy out. */
  uint8_t kbuf[128];
  memset(kbuf, 0, copy_bytes);
  memcpy(kbuf, &mask, sizeof(mask));
  if (copy_to_user((void *)user_mask_ptr, kbuf, copy_bytes) != 0)
    return (uint64_t)-14; // EFAULT
  return copy_bytes;
}

#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2
#define SCHED_BATCH 3
#define SCHED_IDLE  5
#define SCHED_FLAG_RESET_ON_FORK 1ULL
#define SCHED_FLAG_KEEP_POLICY 8ULL
#define SCHED_FLAG_KEEP_PARAMS 16ULL
#define SCHED_FLAG_UTIL_CLAMP 96ULL

/* FIFO/RR retain the existing EEVDF compatibility mapping, not RT guarantees. */
static uint64_t process_set_scheduler(struct thread *current,
                                       struct thread *target, uint32_t policy,
                                       int nice, uint32_t priority, bool reset) {
  if (!target)
    return (uint64_t)-3;
  if (policy != SCHED_OTHER && policy != SCHED_BATCH && policy != SCHED_IDLE &&
      policy != SCHED_FIFO && policy != SCHED_RR)
    return policy == 6 ? (uint64_t)-95 : (uint64_t)-22;
  bool rt = policy == SCHED_FIFO || policy == SCHED_RR;
  if ((rt && (priority < 1 || priority > 99)) || (!rt && priority))
    return (uint64_t)-22;
  if (nice < -20) nice = -20;
  if (nice > 19) nice = 19;
  if (!current || (!capability_has(current, CAP_SYS_NICE) &&
      ((current->euid != target->uid && current->euid != target->euid) || rt ||
       nice < target->nice_value || (target->sched_reset_on_fork && !reset))))
    return (uint64_t)-1;
  if (rt) {
    nice = -20 + (99 - priority) / 10;
    if (nice > -10) nice = -10;
  } else if (policy == SCHED_IDLE) {
    nice = 19;
  }
  return sched_set_attributes(target, process_nice_to_sched_priority(nice), (int8_t)nice,
                               (uint8_t)policy, (uint8_t)priority, reset)
             ? 0 : (uint64_t)-3;
}

static uint64_t sys_sched_setparam(uint64_t pid, uint64_t param_ptr,
                                   uint64_t a2, uint64_t a3, uint64_t a4,
                                   uint64_t a5) {
  (void)a2; (void)a3; (void)a4; (void)a5;
  if ((int32_t)pid < 0 || !param_ptr) return (uint64_t)-22;
  int priority;
  if (copy_from_user(&priority, (void *)param_ptr, sizeof(priority)))
    return (uint64_t)-14;
  struct thread *current = sched_get_current();
  struct thread *target = pid == 0 ? current : sched_get_thread_by_tid(pid_namespace_global(sched_get_current(), (uint32_t)pid));
  if (!target) return (uint64_t)-3;
  return process_set_scheduler(current, target, target->sched_policy,
                                target->nice_value, (uint32_t)priority,
                                target->sched_reset_on_fork);
}

static uint64_t sys_sched_getparam(uint64_t pid, uint64_t param_ptr,
                                   uint64_t a2, uint64_t a3, uint64_t a4,
                                   uint64_t a5) {
  (void)a2; (void)a3; (void)a4; (void)a5;
  if ((int32_t)pid < 0 || !param_ptr) return (uint64_t)-22;
  struct thread *target = pid == 0 ? sched_get_current() :
      sched_get_thread_by_tid(pid_namespace_global(sched_get_current(), (uint32_t)pid));
  if (!target) return (uint64_t)-3;
  int priority = target->sched_rt_priority;
  return copy_to_user((void *)param_ptr, &priority, sizeof(priority))
             ? (uint64_t)-14 : 0;
}

/* Linux version 0 is 48 bytes; version 1 adds utilization bounds. */
struct linux_sched_attr {
  uint32_t size, policy;
  uint64_t flags;
  int32_t nice;
  uint32_t priority;
  uint64_t runtime, deadline, period;
  uint32_t util_min, util_max;
};
_Static_assert(sizeof(struct linux_sched_attr) == 56, "sched_attr ABI");

static uint64_t sys_sched_setattr(uint64_t pid, uint64_t attr_ptr,
                                  uint64_t flags, uint64_t a3,
                                  uint64_t a4, uint64_t a5) {
  (void)a3; (void)a4; (void)a5;
  if ((int32_t)pid < 0 || !attr_ptr || (uint32_t)flags)
    return (uint64_t)-22;
  uint32_t size;
  if (copy_from_user(&size, (void *)attr_ptr, sizeof(size)))
    return (uint64_t)-14;
  if (!size) size = 48;
  if (size < 48 || size > 4096) goto bad_size;
  struct linux_sched_attr attr = {0};
  size_t bytes = size < sizeof(attr) ? size : sizeof(attr);
  if (copy_from_user(&attr, (void *)attr_ptr, bytes)) return (uint64_t)-14;
  /* Future extensions may be accepted only if all unknown bytes are zero. */
  for (size_t off = sizeof(attr); off < size;) {
    uint8_t tail[64];
    size_t n = size - off;
    if (n > sizeof(tail)) n = sizeof(tail);
    if (copy_from_user(tail, (void *)(attr_ptr + off), n)) return (uint64_t)-14;
    for (size_t i = 0; i < n; i++) if (tail[i]) goto bad_size;
    off += n;
  }
  if ((int32_t)attr.policy < 0 || (attr.flags & ~127ULL))
    return (uint64_t)-22;
  if ((attr.flags & SCHED_FLAG_UTIL_CLAMP) && size < sizeof(attr))
    return (uint64_t)-22;
  /* No utilization governor or deadline admission control exists yet. */
  if (attr.flags & (2ULL | 4ULL | SCHED_FLAG_UTIL_CLAMP)) return (uint64_t)-95;
  struct thread *current = sched_get_current();
  struct thread *target = pid == 0 ? current : sched_get_thread_by_tid(pid_namespace_global(sched_get_current(), (uint32_t)pid));
  if (!target) return (uint64_t)-3;
  if (attr.flags & SCHED_FLAG_KEEP_POLICY) attr.policy = target->sched_policy;
  if (attr.flags & SCHED_FLAG_KEEP_PARAMS) {
    attr.nice = target->nice_value;
    attr.priority = target->sched_rt_priority;
  }
  return process_set_scheduler(current, target, attr.policy, attr.nice,
                                attr.priority,
                                (attr.flags & SCHED_FLAG_RESET_ON_FORK) != 0);
 bad_size:
  size = sizeof(struct linux_sched_attr);
  if (copy_to_user((void *)attr_ptr, &size, sizeof(size))) return (uint64_t)-14;
  return (uint64_t)-7;
}

static uint64_t sys_sched_getattr(uint64_t pid, uint64_t attr_ptr,
                                  uint64_t size_arg, uint64_t flags,
                                  uint64_t a4, uint64_t a5) {
  (void)a4; (void)a5;
  uint32_t size = (uint32_t)size_arg;
  if ((int32_t)pid < 0 || !attr_ptr || (uint32_t)flags || size < 48 || size > 4096)
    return (uint64_t)-22;
  struct thread *target = pid == 0 ? sched_get_current() :
      sched_get_thread_by_tid(pid_namespace_global(sched_get_current(), (uint32_t)pid));
  if (!target) return (uint64_t)-3;
  struct linux_sched_attr attr = {0};
  attr.size = size < sizeof(attr) ? size : sizeof(attr);
  attr.policy = target->sched_policy;
  attr.nice = target->nice_value;
  attr.priority = target->sched_rt_priority;
  attr.flags = target->sched_reset_on_fork ? SCHED_FLAG_RESET_ON_FORK : 0;
  return copy_to_user((void *)attr_ptr, &attr, attr.size) ? (uint64_t)-14 : 0;
}

static uint64_t sys_sched_get_priority_max(uint64_t policy, uint64_t a1,
                                           uint64_t a2, uint64_t a3,
                                           uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  uint64_t clean_policy = policy & ~0x40000000ULL;
  if (clean_policy == SCHED_FIFO || clean_policy == SCHED_RR)
    return 99;
  if (clean_policy == SCHED_OTHER || clean_policy == SCHED_BATCH || clean_policy == SCHED_IDLE)
    return 0;
  return (uint64_t)-22; // EINVAL
}

static uint64_t sys_sched_get_priority_min(uint64_t policy, uint64_t a1,
                                           uint64_t a2, uint64_t a3,
                                           uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  uint64_t clean_policy = policy & ~0x40000000ULL;
  if (clean_policy == SCHED_FIFO || clean_policy == SCHED_RR)
    return 1;
  if (clean_policy == SCHED_OTHER || clean_policy == SCHED_BATCH || clean_policy == SCHED_IDLE)
    return 0;
  return (uint64_t)-22; // EINVAL
}

// sys_sched_rr_get_interval (syscall 148)
// Writes the SCHED_RR timeslice quantum for the given pid.  This kernel runs
// an EEVDF scheduler with no fixed per-policy quantum; the reported 100 ms
// matches the tick accounting the scheduler itself uses (1 LAPIC tick ~= 1 ms,
// idle quantum initialised to 100 ticks in sched.c).
static uint64_t sys_sched_rr_get_interval(uint64_t pid, uint64_t tp_ptr,
                                          uint64_t a2, uint64_t a3,
                                          uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *current = sched_get_current();
  struct thread *target =
      pid == 0 ? current : sched_get_thread_by_tid(pid_namespace_global(sched_get_current(), (uint32_t)pid));
  if (!target)
    return (uint64_t)-3; // ESRCH
  (void)current;

  if (!tp_ptr || !vmm_is_user_addr_range_writable(tp_ptr, 16))
    return (uint64_t)-14; // EFAULT

  // struct timespec { tv_sec, tv_nsec }: 100 ms quantum.
  ((uint64_t *)tp_ptr)[0] = 0;
  ((uint64_t *)tp_ptr)[1] = 100000000ULL;
  return 0;
}

// sys_times (syscall 100)
// Fills struct tms { utime, stime, cutime, cstime } in clock ticks
// (USER_HZ = 100) and returns elapsed ticks since boot.  User vs system time
// is not tracked separately (getrusage reports the same total as utime) and
// child times are not accumulated (RUSAGE_CHILDREN is zeroed there too).
static uint64_t sys_times(uint64_t buf_ptr, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  if (buf_ptr) {
    if (!vmm_is_user_addr_range_writable(buf_ptr, 4 * sizeof(long)))
      return (uint64_t)-14; // EFAULT
    long *tms = (long *)buf_ptr;
    tms[0] = (long)(t->runtime_total / 10); // utime: ms -> 100 Hz ticks
    tms[1] = 0;                             // stime: not tracked separately
    tms[2] = 0;                             // cutime: children not accumulated
    tms[3] = 0;                             // cstime: children not accumulated
  }

  return lapic_timer_get_ticks() / 10; // boot-relative ticks at 100 Hz
}

// sys_sched_getscheduler (syscall 145)
// Returns the scheduling policy of the given process.
static uint64_t sys_sched_getscheduler(uint64_t pid, uint64_t a1, uint64_t a2,
                                       uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if ((int32_t)pid < 0) return (uint64_t)-22;

  struct thread *current = sched_get_current();
  struct thread *target =
      pid == 0 ? current : sched_get_thread_by_tid(pid_namespace_global(sched_get_current(), (uint32_t)pid));
  if (!target)
    return (uint64_t)-3; // ESRCH

  return target->sched_policy | (target->sched_reset_on_fork ? 0x40000000U : 0);

}

// sys_sched_setscheduler (syscall 144)
// Sets the scheduling policy and parameters for a process.
// Shares the attribute syscall's validation, policy state and EEVDF mapping.
static uint64_t sys_sched_setscheduler(uint64_t pid, uint64_t policy,
                                       uint64_t param_ptr, uint64_t a3,
                                       uint64_t a4, uint64_t a5) {
  (void)a3; (void)a4; (void)a5;
  if ((int32_t)pid < 0 || !param_ptr) return (uint64_t)-22;
  int priority;
  if (copy_from_user(&priority, (void *)param_ptr, sizeof(priority)))
    return (uint64_t)-14;
  struct thread *current = sched_get_current();
  struct thread *target = pid == 0 ? current : sched_get_thread_by_tid(pid_namespace_global(sched_get_current(), (uint32_t)pid));
  if (!target) return (uint64_t)-3;
  return process_set_scheduler(current, target, (uint32_t)policy & ~0x40000000U,
                                target->nice_value, (uint32_t)priority,
                                ((uint32_t)policy & 0x40000000U) != 0);
}

// sys_setpriority (syscall 141)
// Sets the nice value for one process and maps it onto 32 kernel priorities.
#define PRIO_PROCESS 0
#define PRIO_PGRP 1
#define PRIO_USER 2


static uint64_t sys_setpriority(uint64_t which, uint64_t who, uint64_t prio,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3; (void)a4; (void)a5;
  if (which != PRIO_PROCESS) return (uint64_t)-22;
  int nice = (int)(int64_t)prio;
  if (nice < -20) nice = -20;
  if (nice > 19) nice = 19;
  struct thread *current = sched_get_current();
  struct thread *target = who == 0 ? current :
      sched_get_thread_by_tid(pid_namespace_global(sched_get_current(), (uint32_t)who));
  if (!target) return (uint64_t)-3;
  if (!capability_has(current, CAP_SYS_NICE) && current->uid != target->uid &&
      current->euid != target->uid) return (uint64_t)-1;
  if (nice < target->nice_value && !capability_has(current, CAP_SYS_NICE))
    return (uint64_t)-13;
  if (!sched_set_priority(target, process_nice_to_sched_priority(nice), (int8_t)nice))
    return (uint64_t)-3;
  return 0;
}

/* Map Linux nice values onto the scheduler's internal priority range. */
uint8_t process_nice_to_sched_priority(int nice) {
  return (uint8_t)(((nice + 20) * (SCHED_PRIORITY_LEVELS - 1) + 19) / 39);
}

static uint64_t sys_getpriority(uint64_t which, uint64_t who, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2; (void)a3; (void)a4; (void)a5;
  if (which != PRIO_PROCESS) return (uint64_t)-22;
  struct thread *current = sched_get_current();
  struct thread *target = who == 0 ? current :
      sched_get_thread_by_tid(pid_namespace_global(sched_get_current(), (uint32_t)who));
  if (!target) return (uint64_t)-3;
  return (uint64_t)(20 - target->nice_value);
}

// ioprio_set (251) / ioprio_get (252).
// There is no block-I/O scheduler here, so priorities are validated for
// Linux-correct errors and otherwise accepted as no-ops; get reports the
// default best-effort/4. baloo_file sets IDLE on its indexer at startup and
// treats anything but success as fatal, so ENOSYS here breaks file indexing.
#define IOPRIO_WHO_PROCESS 1
#define IOPRIO_WHO_PGRP 2
#define IOPRIO_WHO_USER 3
#define IOPRIO_CLASS_RT 1
#define IOPRIO_CLASS_BE 2
#define IOPRIO_CLASS_IDLE 3
#define IOPRIO_DEFAULT_PRIO ((IOPRIO_CLASS_BE << 13) | 4)

static uint64_t sys_ioprio_set(uint64_t which, uint64_t who, uint64_t ioprio,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3; (void)a4; (void)a5;
  if (which < IOPRIO_WHO_PROCESS || which > IOPRIO_WHO_USER)
    return (uint64_t)-22;
  uint64_t cls = ioprio >> 13;
  uint64_t data = ioprio & 0x1FFF;
  if (cls > IOPRIO_CLASS_IDLE)
    return (uint64_t)-22;
  if ((cls == IOPRIO_CLASS_RT || cls == IOPRIO_CLASS_BE) && data > 7)
    return (uint64_t)-22;
  struct thread *current = sched_get_current();
  if (which == IOPRIO_WHO_PROCESS && who != 0) {
    struct thread *target = sched_get_thread_by_tid(pid_namespace_global(sched_get_current(), (uint32_t)who));
    if (!target) return (uint64_t)-3;
    if (current && !capability_has(current, CAP_SYS_NICE) && current->uid != target->uid &&
        current->euid != target->uid) return (uint64_t)-1;
  }
  if (cls == IOPRIO_CLASS_RT && current && !capability_has(current, CAP_SYS_NICE))
    return (uint64_t)-1; // RT I/O class needs privilege, as on Linux
  return 0;
}

static uint64_t sys_ioprio_get(uint64_t which, uint64_t who, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2; (void)a3; (void)a4; (void)a5;
  if (which < IOPRIO_WHO_PROCESS || which > IOPRIO_WHO_USER)
    return (uint64_t)-22;
  if (which == IOPRIO_WHO_PROCESS && who != 0 &&
      !sched_get_thread_by_tid(pid_namespace_global(sched_get_current(), (uint32_t)who)))
    return (uint64_t)-3;
  return (uint64_t)IOPRIO_DEFAULT_PRIO;
}

// sys_reboot — syscall 169, mirrors Linux reboot(2)
// ---------------------------------------------------------------------------

// Linux reboot(2) magic values
#define LINUX_REBOOT_MAGIC1       0xfee1dead
#define LINUX_REBOOT_MAGIC2       0x28121969
#define LINUX_REBOOT_MAGIC2A      0x05121996
#define LINUX_REBOOT_MAGIC2B      0x16041998
#define LINUX_REBOOT_MAGIC2C      0x20112000

// reboot commands
#define LINUX_REBOOT_CMD_RESTART       0x01234567
#define LINUX_REBOOT_CMD_HALT          0xcdef0123
#define LINUX_REBOOT_CMD_POWER_OFF     0x4321fedc
#define LINUX_REBOOT_CMD_RESTART2      0xa1b2c3d4
#define LINUX_REBOOT_CMD_CAD_ON        0x89abcdef
#define LINUX_REBOOT_CMD_CAD_OFF       0x00000000
#define LINUX_REBOOT_CMD_KEXEC         0x45584543

static uint64_t sys_reboot(uint64_t magic1, uint64_t magic2, uint64_t cmd,
                           uint64_t arg, uint64_t a4, uint64_t a5) {
  (void)arg;
  (void)a4;
  (void)a5;

  if (!capability_has(sched_get_current(), 22)) return (uint64_t)-1;

  // Both magic values must match (as on Linux)
  if (magic1 != LINUX_REBOOT_MAGIC1)
    return (uint64_t)-22; // EINVAL
  if (magic2 != LINUX_REBOOT_MAGIC2  && magic2 != LINUX_REBOOT_MAGIC2A &&
      magic2 != LINUX_REBOOT_MAGIC2B && magic2 != LINUX_REBOOT_MAGIC2C)
    return (uint64_t)-22; // EINVAL

  switch ((uint32_t)cmd) {
  case LINUX_REBOOT_CMD_RESTART:
  case LINUX_REBOOT_CMD_RESTART2:
    klog_puts("[REBOOT] System reboot requested via syscall.\n");
    vmm_defer_sync();
#ifdef LINUXKPI_LINUX_IMPORTED
    linuxkpi_pci_shutdown();
#endif
    e1000_shutdown();
    nvme_shutdown(); /* graceful CC.SHN before resetting the machine */
    acpi_reboot();
    /* noreturn */
    break;

  case LINUX_REBOOT_CMD_POWER_OFF:
    klog_puts("[REBOOT] System power-off requested via syscall.\n");
    vmm_defer_sync();
#ifdef LINUXKPI_LINUX_IMPORTED
    linuxkpi_pci_shutdown();
#endif
    e1000_shutdown();
    nvme_shutdown(); /* graceful CC.SHN before power is removed */
    acpi_poweroff();
    /* noreturn */
    break;

  case LINUX_REBOOT_CMD_HALT:
    klog_puts("[REBOOT] System halt requested via syscall.\n");
    vmm_defer_sync();
#ifdef LINUXKPI_LINUX_IMPORTED
    linuxkpi_pci_shutdown();
#endif
    e1000_shutdown();
    nvme_shutdown();
    hal_irq_disable();
    for (;;) hal_cpu_halt();
    break;

  case LINUX_REBOOT_CMD_CAD_ON:
  case LINUX_REBOOT_CMD_CAD_OFF:
    // Ctrl-Alt-Delete enable/disable -- no-op for now
    return 0;

  default:
    return (uint64_t)-22; // EINVAL
  }

  /* Should never reach here */
  return 0;
}

// ---------------------------------------------------------------------------
// init_module / delete_module / swapon: no-ops with honest errors.
// This kernel has no loadable-module support and no swap activation, so
// after validating arguments these fail without side effects:
//   init_module   -> EPERM  (loading is not implemented)
//   delete_module -> ENOENT (no modules are ever loaded)
//   swapon        -> EPERM  (swap areas cannot be activated)
// ---------------------------------------------------------------------------

static uint64_t sys_init_module(uint64_t image_ptr, uint64_t len,
                                uint64_t param_ptr, uint64_t a3, uint64_t a4,
                                uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (!image_ptr)
    return (uint64_t)-14; // EFAULT
  if (!vmm_is_user_addr_range_valid(image_ptr, len ? 1 : 0))
    return (uint64_t)-14; // EFAULT
  if (param_ptr && !vmm_is_user_addr_range_valid(param_ptr, 1))
    return (uint64_t)-14; // EFAULT
  return (uint64_t)-1; // EPERM: module loading is not supported
}

static uint64_t sys_delete_module(uint64_t name_ptr, uint64_t flags,
                                  uint64_t a2, uint64_t a3, uint64_t a4,
                                  uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  if (!name_ptr || !vmm_is_user_addr_range_valid(name_ptr, 1))
    return (uint64_t)-14; // EFAULT
  // Linux delete_module flags: O_NONBLOCK | O_TRUNC.
  if (flags & ~(uint64_t)(0x800 | 0x200))
    return (uint64_t)-22; // EINVAL
  return (uint64_t)-2; // ENOENT: no modules are loaded, no name can match
}

static uint64_t sys_swapon(uint64_t path_ptr, uint64_t flags, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)flags;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  if (!path_ptr || !vmm_is_user_addr_range_valid(path_ptr, 1))
    return (uint64_t)-14; // EFAULT
  return (uint64_t)-1; // EPERM: swap activation is not supported
}

// ---------------------------------------------------------------------------
// Capability operations use per-thread credentials and enforce irreversible drops.
static uint64_t sys_capget(uint64_t h, uint64_t d, uint64_t a, uint64_t b, uint64_t c, uint64_t e) {
  (void)a; (void)b; (void)c; (void)e; return capability_get(h, d);
}
static uint64_t sys_capset(uint64_t h, uint64_t d, uint64_t a, uint64_t b, uint64_t c, uint64_t e) {
  (void)a; (void)b; (void)c; (void)e; return capability_set(h, d);
}

// sys_seccomp (317)
//
// Verified classic-BPF filter installation and feature discovery.
// ---------------------------------------------------------------------------

#define SECCOMP_SET_MODE_STRICT    0
#define SECCOMP_SET_MODE_FILTER    1
#define SECCOMP_GET_ACTION_AVAIL   2

static uint64_t sys_seccomp(uint64_t op, uint64_t flags, uint64_t uargs,
                             uint64_t a4, uint64_t a5, uint64_t a6) {
  (void)a4; (void)a5; (void)a6;
  return (uint64_t)seccomp_control((uint32_t)op, (uint32_t)flags, uargs);
}

/* Explicit unsupported Landlock ABI, including version queries. Never
 * advertise an ABI version or return success without enforcing the rules. */
static uint64_t sys_landlock_unsupported(uint64_t a0, uint64_t a1, uint64_t a2,
                                        uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
  return (uint64_t)-38; /* ENOSYS */
}

// Namespace preparation is transactional: commit only after all allocations succeed.
static uint64_t sys_unshare(uint64_t flags, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6) {
  (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
  uint64_t known = CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS |
                   CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWPID |
                   CLONE_NEWNET | CLONE_NEWTIME | CLONE_SYSVSEM |
                   CLONE_FILES | CLONE_FS;
  if (flags & ~known) return (uint64_t)-22;
  if (flags & (CLONE_FILES | CLONE_FS | CLONE_SYSVSEM)) return (uint64_t)-95;
  if (flags & (CLONE_NEWCGROUP | CLONE_NEWUSER | CLONE_NEWTIME))
    return (uint64_t)-1;
  if (!flags) return 0;
  struct thread *current = sched_get_current();
  if (!capability_has(current, CAP_SYS_ADMIN)) return (uint64_t)-1;
  struct uts_namespace *uts __attribute__((cleanup(uts_namespace_cleanup))) = NULL;
  struct ipc_namespace *ipc __attribute__((cleanup(ipc_namespace_cleanup))) = NULL;
  struct mount_namespace *mount __attribute__((cleanup(process_mount_cleanup))) = NULL;
  struct pid_namespace *pid __attribute__((cleanup(pid_namespace_cleanup))) = NULL;
  struct net_namespace *net __attribute__((cleanup(net_namespace_cleanup))) = NULL;
  if (flags & CLONE_NEWNET) { net = net_namespace_create(); if (!net) return (uint64_t)-12; }
  if (flags & CLONE_NEWUTS) { uts = uts_namespace_copy(current); if (!uts) return (uint64_t)-12; }
  if (flags & CLONE_NEWIPC) { ipc = ipc_namespace_create(); if (!ipc) return (uint64_t)-12; }
  if (flags & CLONE_NEWNS) { mount = vfs_namespace_prepare(current); if (!mount) return (uint64_t)-12; }
  if (flags & CLONE_NEWPID) { pid = pid_namespace_create(current); if (!pid) return (uint64_t)-12; }
  if (net) { net_namespace_install(current, net); net = NULL; }
  if (uts) { uts_namespace_install(current, uts); uts = NULL; }
  if (ipc) { ipc_namespace_install(current, ipc); ipc = NULL; }
  if (mount) { vfs_namespace_install(current, mount); mount = NULL; }
  if (pid) { pid_namespace_unshare(current, pid); pid = NULL; }
  return 0;
}

void syscall_register_process(void) {

  syscall_register_process_exit();
  syscall_register_process_wait();
  syscall_register_process_clone();
  syscall_register_process_exec();
  syscall_register_pidfd();

  syscall_register(SYS_GETTID, sys_gettid);
  syscall_register(SYS_GETPID, sys_getpid);
  syscall_register(SYS_UNAME, sys_uname);
  syscall_register(SYS_SETHOSTNAME, sys_sethostname);
  syscall_register(SYS_SETDOMAINNAME, sys_setdomainname);
  syscall_register(SYS_SYSINFO, sys_sysinfo);
  syscall_register(SYS_UPTIME, sys_uptime);
  syscall_register(SYS_GETCWD, sys_getcwd);
  syscall_register(SYS_CHDIR, sys_chdir);
  syscall_register(SYS_PRCTL, sys_prctl);
  syscall_register(SYS_SETITIMER, sys_setitimer);
  syscall_register(SYS_ALARM, sys_alarm);
  syscall_register_raw(SYS_UMASK, sys_umask);
  syscall_register_raw(SYS_RSEQ, sys_rseq);
  syscall_register_raw(SYS_GETUID, sys_getuid);
  syscall_register_raw(SYS_GETGID, sys_getgid);
  syscall_register_raw(SYS_GETEUID, sys_geteuid);
  syscall_register_raw(SYS_GETEGID, sys_getegid);
  syscall_register_raw(SYS_GETRESUID, sys_getresuid);
  syscall_register_raw(SYS_GETRESGID, sys_getresgid);
  syscall_register_raw(SYS_SETRESUID, sys_setresuid);
  syscall_register_raw(SYS_SETRESGID, sys_setresgid);
  syscall_register_raw(SYS_GETPPID, sys_getppid);
  syscall_register_raw(SYS_SETPGID, sys_setpgid);
  syscall_register_raw(SYS_GETPGRP, sys_getpgrp);
  syscall_register(SYS_GETPGID, sys_getpgid);
  syscall_register(SYS_GETSID, sys_getsid);
  syscall_register_raw(SYS_SETSID, sys_setsid);
  syscall_register_raw(SYS_SETUID, sys_setuid);
  syscall_register_raw(SYS_SETGID, sys_setgid);
  syscall_register_raw(SYS_SETREUID, sys_setreuid);
  syscall_register_raw(SYS_SETREGID, sys_setregid);
  syscall_register_raw(SYS_GETGROUPS, sys_getgroups);
  syscall_register_raw(SYS_SETGROUPS, sys_setgroups);
  syscall_register_raw(SYS_SETFSUID, sys_setfsuid);
  syscall_register_raw(SYS_SETFSGID, sys_setfsgid);
  syscall_register(SYS_GETRLIMIT, sys_getrlimit);
  syscall_register(SYS_SETRLIMIT, sys_setrlimit);
  syscall_register(SYS_GETRUSAGE, sys_getrusage);
  syscall_register_raw(SYS_PRLIMIT64, sys_prlimit64);
  syscall_register(SYS_MEMBARRIER, sys_membarrier);
  syscall_register(SYS_SCHED_YIELD, sys_sched_yield);
  syscall_register(SYS_SCHED_GETAFFINITY, sys_sched_getaffinity);
  syscall_register(SYS_SCHED_SETAFFINITY, sys_sched_setaffinity);
  syscall_register(SYS_SCHED_GETPARAM, sys_sched_getparam);
  syscall_register(SYS_SCHED_SETPARAM, sys_sched_setparam);
  syscall_register(SYS_SCHED_SETATTR, sys_sched_setattr);
  syscall_register(SYS_SCHED_GETATTR, sys_sched_getattr);
  syscall_register(SYS_SCHED_GET_PRIORITY_MAX, sys_sched_get_priority_max);
  syscall_register(SYS_SCHED_GET_PRIORITY_MIN, sys_sched_get_priority_min);
  syscall_register(SYS_SCHED_RR_GET_INTERVAL, sys_sched_rr_get_interval);
  syscall_register(SYS_TIMES, sys_times);
  syscall_register(SYS_SCHED_GETSCHEDULER, sys_sched_getscheduler);
  syscall_register(SYS_SCHED_SETSCHEDULER, sys_sched_setscheduler);
  syscall_register(SYS_SETPRIORITY, sys_setpriority);
  syscall_register(SYS_GETPRIORITY, sys_getpriority);
  syscall_register(SYS_IOPRIO_SET, sys_ioprio_set);
  syscall_register(SYS_IOPRIO_GET, sys_ioprio_get);
  syscall_register(SYS_REBOOT, sys_reboot);
  syscall_register(SYS_INIT_MODULE, sys_init_module);
  syscall_register(SYS_DELETE_MODULE, sys_delete_module);
  syscall_register(SYS_SWAPON, sys_swapon);
  syscall_register(SYS_GETCPU, sys_getcpu);
  syscall_register(SYS_CAPGET, sys_capget);
  syscall_register(SYS_CAPSET, sys_capset);
  syscall_register(SYS_SECCOMP, sys_seccomp);
  syscall_register(SYS_LANDLOCK_CREATE_RULESET, sys_landlock_unsupported);
  syscall_register(SYS_LANDLOCK_ADD_RULE, sys_landlock_unsupported);
  syscall_register(SYS_LANDLOCK_RESTRICT_SELF, sys_landlock_unsupported);
  syscall_register(SYS_UNSHARE, sys_unshare);
}
