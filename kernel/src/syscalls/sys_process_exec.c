#include "../sched/capability.h"
// Process syscall implementation.
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
#include "sys_ptrace.h"
#include "syscall.h"
#include "sysdiag.h"
#define TSC_PROBES_ENABLE
#include "../lib/tsc.h"
#include <stdint.h>
#include <stddef.h>
#ifdef LINUXKPI_LINUX_IMPORTED
#include <linuxkpi/native_pci.h>
#endif

#include "sys_process_internal.h"

extern void mm_reset_mmap_state(struct thread *t);

#define MMAP_REGION_BASE 0x7F0000000000ULL
#define IA32_KERNEL_GS_BASE 0xC0000102
#define IA32_FS_BASE 0xC0000100

// Close descriptors marked FD_CLOEXEC only after the replacement image has
// loaded successfully. Failed execve() must leave the descriptor table intact.
static void exec_close_cloexec(struct thread *t) {
  if (!t || !t->files)
    return;

  spinlock_acquire(&t->files->lock);
  uint32_t cap = t->files->capacity;
  spinlock_release(&t->files->lock);

  for (uint32_t fd = 0; fd < cap; fd++) {
    spinlock_acquire(&t->files->lock);
    if (fd >= t->files->capacity) {
      spinlock_release(&t->files->lock);
      break;
    }
    vfs_node_t *node = t->files->fds[fd];
    if (!fd_is_valid_node(node) ||
        !(t->files->fd_flags[fd] & FD_FLAGS_CLOEXEC_BIT)) {
      spinlock_release(&t->files->lock);
      continue;
    }

    t->files->fds[fd] = NULL;
    t->files->fd_offsets[fd] = 0;
    t->files->fd_flags[fd] = 0;
    fd_path_clear(t, fd);
    if ((uint32_t)fd < t->files->next_fd)
      t->files->next_fd = (uint32_t)fd;
    spinlock_release(&t->files->lock);
    vfs_close(node);
  }
}

// sys_execve

/* Keep normal Wine launches quiet. Explicit WINEDEBUG settings still win. */
static bool exec_wine_debug_env(const char *path, char ***envp, int *envc) {
  const char *name = strrchr(path, '/');
  name = name ? name + 1 : path;
  if (strcmp(name, "wine") && strcmp(name, "wine64") &&
      strcmp(name, "wine-preloader") && strcmp(name, "wine64-preloader"))
    return true;
  for (int i = 0; i < *envc; i++)
    if (strncmp((*envp)[i], "WINEDEBUG=", 10) == 0) return true;
  const char *setting = "WINEDEBUG=-all";
  size_t setting_size = strlen(setting) + 1;
  char **expanded = kmalloc(((size_t)*envc + 2) * sizeof(*expanded));
  if (!expanded) return false;
  char *value = kmalloc(setting_size);
  if (!value) { kfree(expanded); return false; }
  memcpy(value, setting, setting_size);
  memcpy(expanded, *envp, (size_t)*envc * sizeof(*expanded));
  expanded[*envc] = value;
  expanded[*envc + 1] = NULL;
  kfree(*envp);
  *envp = expanded;
  (*envc)++;
  return true;
}

static const char *exec_vector_entry(const char **vector, int index,
                                      bool compat32) {
  return compat32 ? (const char *)(uintptr_t)((const uint32_t *)vector)[index]
                  : vector[index];
}

/* A procfs symlink is resolved relative to /proc/<pid>, not the exec cwd.
 * Anchor relative executable names before replacing the address space. Keep
 * dot/dot-dot components: collapsing them lexically changes symlink traversal. */
static bool exec_save_absolute_path(char *out, size_t capacity,
                                    const char *path, const char *cwd) {
  if (!out || !capacity || !path || !path[0])
    return false;
  const char *base = (cwd && cwd[0] == '/') ? cwd : "/";
  size_t prefix = path[0] == '/' ? 0 : strlen(base);
  size_t length = strlen(path);
  bool slash = prefix && base[prefix - 1] != '/';
  if (prefix >= capacity || (size_t)slash >= capacity - prefix ||
      length >= capacity - prefix - (size_t)slash)
    return false;
  if (prefix)
    memcpy(out, base, prefix);
  if (slash)
    out[prefix++] = '/';
  memcpy(out + prefix, path, length + 1);
  return true;
}

static uint64_t sys_execve(struct syscall_regs *regs) {
  bool compat32 = regs->user_cs == 0x1b;
  const char **user_argv = (const char **)regs->rsi;
  const char **user_envp = (const char **)regs->rdx;
  const char *user_path = (const char *)regs->rdi;

  if (!user_path)
    return (uint64_t)-14; // EFAULT

  // Snapshot the pathname before inspecting self-exec aliases or switching CR3.
  char *path = kmalloc(4096);
  if (!path) return (uint64_t)-12;
  long path_len = strncpy_from_user(path, user_path, 4096);
  if (path_len < 0 || path_len >= 4096) {
    kfree(path);
    return path_len < 0 ? (uint64_t)-14 : (uint64_t)-36;
  }

  /* Recording the alias itself as exe_path would make /proc/self/exe point
   * back to itself after exec, breaking the next Chromium service launch. */
  struct thread *exec_caller = sched_get_current();
  if (!strcmp(path, "/proc/self/exe") ||
      !strcmp(path, "/proc/thread-self/exe")) {
    if (!exec_caller || !exec_caller->exe_path[0]) {
      kfree(path);
      return (uint64_t)-2; // ENOENT
    }
    strcpy(path, exec_caller->exe_path);
  }

  TSC_BEGIN(execve_arg_copy);
  int argc = 0;
  if (user_argv)
    while (exec_vector_entry(user_argv, argc, compat32))
      argc++;
  char **k_argv = kmalloc((argc + 1) * sizeof(char *));
  if (!k_argv) {
    kfree(path);
    return (uint64_t)-12;
  }
  for (int i = 0; i < argc; i++) {
    size_t len = strlen(exec_vector_entry(user_argv, i, compat32));
    k_argv[i] = kmalloc(len + 1);
    memcpy(k_argv[i], exec_vector_entry(user_argv, i, compat32), len + 1);
  }
  k_argv[argc] = NULL;

  int envc = 0;
  if (user_envp)
    while (exec_vector_entry(user_envp, envc, compat32))
      envc++;
  char **k_envp = kmalloc((envc + 1) * sizeof(char *));
  if (!k_envp) {
    kfree(path);
    for (int i = 0; i < argc; i++)
      kfree(k_argv[i]);
    kfree(k_argv);
    return (uint64_t)-12;
  }
  for (int i = 0; i < envc; i++) {
    size_t len = strlen(exec_vector_entry(user_envp, i, compat32));
    k_envp[i] = kmalloc(len + 1);
    memcpy(k_envp[i], exec_vector_entry(user_envp, i, compat32), len + 1);
  }
  k_envp[envc] = NULL;
  if (!exec_wine_debug_env(path, &k_envp, &envc)) {
    kfree(path);
    for (int i = 0; i < argc; i++) kfree(k_argv[i]);
    kfree(k_argv);
    for (int i = 0; i < envc; i++) kfree(k_envp[i]);
    kfree(k_envp);
    return (uint64_t)-12;
  }
  TSC_END(execve_arg_copy);

  // Shebang (#!) script interpretation loop (up to 4 levels of recursion)
  struct thread *exec_thread = sched_get_current();
  int shebang_depth = 0;
  uint32_t exec_mode = 0, exec_uid = 0, exec_gid = 0;
  uint64_t exec_mount_flags = 0;

  while (shebang_depth < 4) {
    vfs_node_t *exec_base = (path[0] == '/') ? fs_root :
        (exec_thread && exec_thread->cwd_node ? exec_thread->cwd_node : fs_root);
    vfs_node_t *exec_node = vfs_resolve_path_at(exec_base, path);
    int exec_err = 0;
    if (!exec_node) {
      exec_err = 2; // ENOENT
    } else if (!vfs_access(exec_node, 1) ||
               (exec_node->flags & FS_TYPE_MASK) == FS_DIRECTORY) {
      exec_err = 13; // EACCES, and Linux also refuses to exec a directory
    }
    if (exec_err) {
      if (exec_node) vfs_close(exec_node);
      klog_puts("[PROC] execve failed path=\"");
      klog_puts(path);
      klog_puts("\" errno=");
      klog_uint64((uint64_t)exec_err);
      klog_puts("\n");
      kfree(path);
      for (int i = 0; i < argc; i++) kfree(k_argv[i]);
      kfree(k_argv);
      for (int i = 0; i < envc; i++) kfree(k_envp[i]);
      kfree(k_envp);
      return (uint64_t)(-exec_err);
    }
    exec_mode = exec_node->mask;
    exec_mount_flags = vfs_mount_flags(exec_node);
    exec_uid = exec_node->uid;
    exec_gid = exec_node->gid;

    char header[256];
    uint32_t hlen = vfs_read(exec_node, 0, sizeof(header) - 1, (uint8_t *)header);
    vfs_close(exec_node);

    if (hlen >= 2 && header[0] == '#' && header[1] == '!') {
      header[hlen] = '\0';
      for (char *c = header; *c; c++) {
        if (*c == '\n' || *c == '\r') {
          *c = '\0';
          break;
        }
      }

      // Skip whitespace after '#!'
      char *p = header + 2;
      while (*p == ' ' || *p == '\t') p++;
      if (*p == '\0') {
        kfree(path);
        for (int i = 0; i < argc; i++) kfree(k_argv[i]);
        kfree(k_argv);
        for (int i = 0; i < envc; i++) kfree(k_envp[i]);
        kfree(k_envp);
        return (uint64_t)-8; // ENOEXEC
      }

      char *interp_bin = p;
      char *interp_arg = NULL;
      while (*p && *p != ' ' && *p != '\t') p++;
      if (*p != '\0') {
        *p++ = '\0';
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '\0') {
          interp_arg = p;
          char *end = p + strlen(p) - 1;
          while (end >= p && (*end == ' ' || *end == '\t' || *end == '\r')) {
            *end = '\0';
            end--;
          }
        }
      }

      int added = interp_arg ? 2 : 1;
      int new_argc = argc + added;
      char **new_k_argv = kmalloc((new_argc + 1) * sizeof(char *));
      if (!new_k_argv) {
        kfree(path);
        for (int i = 0; i < argc; i++) kfree(k_argv[i]);
        kfree(k_argv);
        for (int i = 0; i < envc; i++) kfree(k_envp[i]);
        kfree(k_envp);
        return (uint64_t)-12;
      }

      new_k_argv[0] = kmalloc(strlen(interp_bin) + 1);
      strcpy(new_k_argv[0], interp_bin);

      int dst_idx = 1;
      if (interp_arg) {
        new_k_argv[dst_idx] = kmalloc(strlen(interp_arg) + 1);
        strcpy(new_k_argv[dst_idx], interp_arg);
        dst_idx++;
      }
      new_k_argv[dst_idx] = path; // Reuse script path buffer
      dst_idx++;

      for (int i = 1; i < argc; i++) {
        new_k_argv[dst_idx++] = k_argv[i];
      }
      new_k_argv[new_argc] = NULL;

      kfree(k_argv[0]);
      kfree(k_argv);
      k_argv = new_k_argv;
      argc = new_argc;

      path = kmalloc(strlen(interp_bin) + 1);
      strcpy(path, interp_bin);

      shebang_depth++;
      continue;
    }

    // Binary executable verified
    break;
  }

  char saved_exe_path[sizeof(exec_caller->exe_path)];
  if (!exec_save_absolute_path(saved_exe_path, sizeof(saved_exe_path), path,
                                exec_caller ? exec_caller->cwd_path : "/")) {
    kfree(path);
    for (int i = 0; i < argc; i++) kfree(k_argv[i]);
    kfree(k_argv);
    for (int i = 0; i < envc; i++) kfree(k_envp[i]);
    kfree(k_envp);
    return (uint64_t)-36; // ENAMETOOLONG: never publish a truncated exe link
  }

  TSC_BEGIN(execve_pml4_create);
  uint64_t *new_pml4 = vmm_create_pml4();
  TSC_END(execve_pml4_create);
  if (!new_pml4) {
    kfree(path);
    for (int i = 0; i < argc; i++)
      kfree(k_argv[i]);
    kfree(k_argv);
    for (int i = 0; i < envc; i++)
      kfree(k_envp[i]);
    kfree(k_envp);
    return (uint64_t)-12;
  }

  // EARLY CR3 SWITCH:
  struct thread *current = sched_get_current();
  uint64_t old_cr3 = current->cr3;
  struct mm_struct *old_mm = current->mm;
  struct vma_list old_vmas;
  uint64_t old_brk_base = old_mm->brk_base;
  uint64_t old_brk_current = old_mm->brk_current;
  uint64_t old_mmap_next_addr = old_mm->mmap_next_addr;
  bool old_compat32 = old_mm->compat32;
  bool shared_mm = false;

  /* Pre-allocate the replacement address space.  Dropping the shared reference
   * before knowing that this allocation succeeds is what made the -ENOMEM path
   * below wrong: for a moment this thread held no reference at all to the mm it
   * was still running on, so a sibling exiting in that window could free it
   * under us. */
  struct mm_struct *new_mm = NULL;
  spinlock_acquire(&current->mm->lock);
  bool may_be_shared = (current->mm->ref_count > 1);
  spinlock_release(&current->mm->lock);
  if (may_be_shared) {
    new_mm = kmalloc(sizeof(struct mm_struct));
    if (!new_mm) {
      /* Nothing has been modified yet: free the execve scratch buffers and
       * report the failure, staying on the current address space. */
      kfree(path);
      for (int i = 0; i < argc; i++)
        kfree(k_argv[i]);
      kfree(k_argv);
      for (int i = 0; i < envc; i++)
        kfree(k_envp[i]);
      kfree(k_envp);
      return (uint64_t)-12;
    }
  }

  // Unshare mm_struct if shared (e.g. after vfork or in a thread)
  spinlock_acquire(&current->mm->lock);
  if (new_mm && current->mm->ref_count > 1) {
    shared_mm = true;
    current->mm->ref_count--;
    spinlock_release(&current->mm->lock);

    vma_list_init(&new_mm->vmas);
    new_mm->ref_count = 1;
    new_mm->pcid = pcid_alloc();
    new_mm->brk_base = 0;
    new_mm->brk_current = 0;
    new_mm->mmap_next_addr = MMAP_REGION_BASE;
    new_mm->mlockall_future = false;
    spinlock_init(&new_mm->lock);
    current->mm = new_mm;

    vma_list_init(&old_vmas);
  } else {
    spinlock_release(&current->mm->lock);
    /* Every other owner is gone by now, so the pre-allocated mm is not needed. */
    if (new_mm)
      kfree(new_mm);
    // Save the old VMA list before destroying it — we need it to
    // identify MAP_SHARED pages when freeing the old address space.
    old_vmas = current->mm->vmas;
    vma_list_init(&current->mm->vmas); // Reset to empty for the new program
    current->mm->mlockall_future = false;
  }

  current->cr3 = (uint64_t)new_pml4;
  cpu_set_active_cr3(current->cr3);
  __asm__ volatile("mov %0, %%cr3" ::"r"(current->cr3) : "memory");

  // Reset memory placement for the candidate image. Process-visible state is
  // reset only after loading succeeds so a failed exec is non-destructive.
  mm_reset_mmap_state(current);

  elf_info_t elf_info = {0};
  TSC_BEGIN(execve_elf_load);
  if (!elf_load(path, new_pml4, &elf_info)) {
    TSC_END(execve_elf_load);
    klog_puts("[PROC] execve failed path=\"");
    klog_puts(path);
    klog_puts("\" errno=8 (elf_load)\n");
    current->cr3 = old_cr3;
    cpu_set_active_cr3(current->cr3);
    __asm__ volatile("mov %0, %%cr3" ::"r"(current->cr3) : "memory");
    if (shared_mm) {
      // A vfork/CLONE_VM exec temporarily detached from the shared mm.
      // ENOEXEC (including a #! script) must leave the caller on the original
      // address space with its program break intact.
      struct mm_struct *failed_mm = current->mm;
      if (failed_mm->pcid) {
        pcid_free(failed_mm->pcid);
        failed_mm->pcid = 0;
      }
      vma_list_destroy(&failed_mm->vmas);
      kfree(failed_mm);


      current->mm = old_mm;
      spinlock_acquire(&old_mm->lock);
      old_mm->ref_count++;
      spinlock_release(&old_mm->lock);
    } else {
      vma_list_destroy(&current->mm->vmas);
      current->mm->vmas = old_vmas;
      current->mm->brk_base = old_brk_base;
      current->mm->brk_current = old_brk_current;
      current->mm->mmap_next_addr = old_mmap_next_addr;
      current->mm->compat32 = old_compat32;
    }
    kfree(path);
    for (int i = 0; i < argc; i++)
      kfree(k_argv[i]);
    kfree(k_argv);
    for (int i = 0; i < envc; i++)
      kfree(k_envp[i]);
    kfree(k_envp);
    // Note: new_pml4 is leaked if elf_load fails. Fixing that would require
    // vmm_destroy_pml4 or similar, but for now we focus on the reported leak.
    return (uint64_t)-8; // ENOEXEC
  }

  TSC_END(execve_elf_load);

  exec_close_cloexec(current);

  uint32_t old_exec_euid = current->euid;
  if ((exec_mode & 04000) && !current->no_new_privs && !(exec_mount_flags & 2))
    current->euid = current->suid = current->fsuid = exec_uid;
  if ((exec_mode & 02000) && !current->no_new_privs && !(exec_mount_flags & 2))
    current->egid = current->sgid = current->fsgid = exec_gid;
  capability_exec(current, old_exec_euid);

  current->fs_base = 0;
  current->gs_base = 0;

  /* exec replaces the mappings backing a fork-inherited alternate stack.
   * Keeping its registration makes SA_ONSTACK handlers target the old image
   * and can kill a process while delivering a recoverable Wasm SIGILL). */
  current->ss_sp = 0;
  current->ss_size = 0;
  current->ss_flags = SS_DISABLE;
  syscall_user_dispatch_configure(current, 0, 0, 0, 0);

  // Reset custom signal handlers to SIG_DFL after exec; preserve SIG_IGN (POSIX requirement).
  for (int i = 0; i < 64; i++) {
    if ((uint64_t)current->signal_handlers[i].sa_handler != (uint64_t)SIG_IGN) {
      memset(&current->signal_handlers[i], 0, sizeof(struct k_sigaction));
    }
  }
  /* exec preserves pending signals, including full queued siginfo payloads.
   * Dispositions change above, but delivery belongs to the new image. */

  // The old address space is gone, so any robust list registered in it is too.
  current->robust_list = 0;

  // Same for rseq: the old registration pointed into the dead address space
  // (Linux flush_thread() clears it on exec).
  current->rseq_addr = 0;
  current->rseq_len = 0;
  current->rseq_sig = 0;
  current->rseq_last_cpu_id = 0xFFFFFFFFu;

  // We are now safely loaded into the new address space!
  TSC_BEGIN(execve_stack);
  uint64_t user_rsp = process_build_initial_stack(
      ASCENTOS_USER_STACK_TOP, path, (const char **)k_argv,
      (const char **)k_envp, &elf_info);
  TSC_END(execve_stack);

  /* Parent-side vfork_wait already dumps completed samples. Avoid rendering
   * a duplicate exec table while the borrowing child still holds it asleep. */
  bool borrowed_vfork = (current->clone_flags & CLONE_VFORK) != 0;
  /* Successful exec starts with architectural FPU defaults. */
  fpu_reset_current();

  // ENOEXEC fallbacks above retain the original address space and keep waiting.
  vfork_release_parent(current);

  /* Probes are cumulative (fork stats must survive to be averaged), so print
   * the first few launches; fork_probe_maybe_dump() covers later activity. */
  {
    static uint32_t exec_probe_dumps;
    if (!borrowed_vfork && exec_probe_dumps < 4) {
      exec_probe_dumps++;
      tsc_probe_dump();
    }
  }

  uint64_t actual_entry =
      elf_info.interp_base ? elf_info.interp_entry : elf_info.entry;

  // Store the basename of the executable as the thread's comm name
  {
    const char *base = path;
    for (const char *p = path; *p; p++)
      if (*p == '/')
        base = p + 1;
    int ci = 0;
    while (base[ci] && ci < 15) {
      current->comm[ci] = base[ci];
      ci++;
    }
    current->comm[ci] = '\0';
  }

  // Automatic desktop interactive prioritization (KDE Plasma, KWin, Xorg)
  if (strcmp(current->comm, "kwin_x11") == 0 ||
      strcmp(current->comm, "kwin_x11-bin") == 0 ||
      strcmp(current->comm, "kwin_wayland") == 0 ||
      strcmp(current->comm, "kwin") == 0 ||
      strcmp(current->comm, "plasmashell") == 0 ||
      strcmp(current->comm, "Xorg") == 0 ||
      strcmp(current->comm, "Xwayland") == 0) {
    sched_set_priority(current, process_nice_to_sched_priority(-10), -10);
    eevfd_set_slice(&current->se, EEVFD_INTERACTIVE_SLICE_NS);
  }

  if (current->is_forked_child) {
    klog_proc_exec(current->tid, path);
#if SYSCALL_LOG == 2
    if (klog_path_is_browser(path))
      current->trace_syscalls = true;
#elif SYSCALL_LOG == 3
    if (klog_path_is_sddm(path))
      current->trace_syscalls = true;
#endif
  }

  // Publish only after successful exec; argv and AT_EXECFN retain the caller path.
  strcpy(current->exe_path, saved_exe_path);

  // Map the vsyscall page into the new address space
  vmm_map_vsyscall_page((uint64_t *)current->cr3);

  // Hand the old address space (from fork) to background teardown now that
  // the new one is loaded.  We've already switched CR3, so no CPU can be
  // walking these tables.  Queued nodes drain on idle CPUs (bounded by the
  // reap-queue safety net), so execve no longer pays the ~60 us page-table
  // walk inline.  The queue steals old_vmas (re-initing it), making the
  // destroy below a no-op in the queued case; on OOM it falls back to the
  // inline free internally.  Shared mm (vfork/thread exec) has nothing to
  // free: old_vmas there is an empty placeholder.
  if (old_cr3 != 0 && !shared_mm) {
    TSC_BEGIN(exec_free_old);
    vmm_queue_free_user_pages_vma(old_cr3, &old_vmas);
    TSC_END(exec_free_old);
  }
  // Now destroy the old VMA tree nodes
  TSC_BEGIN(exec_vma_destroy);
  vma_list_destroy(&old_vmas);
  TSC_END(exec_vma_destroy);

  // Cleanup kernel-side copies
  for (int i = 0; i < argc; i++)
    kfree(k_argv[i]);
  kfree(k_argv);
  for (int i = 0; i < envc; i++)
    kfree(k_envp[i]);
  kfree(k_envp);
  kfree(path);

  // Reset TLS Bases for this process
  wrmsr(IA32_KERNEL_GS_BASE, 0);
  wrmsr(IA32_FS_BASE, 0);
  struct thread *ct = sched_get_current();
  if (ct) {
    ct->fs_base = 0;
    ct->gs_base = 0;
  }

  // Set the return registers for the syscall exit handler to jump to.
  // We MUST clear all general purpose registers to prevent info leaks from the
  // previous process image (e.g. from the shell into a new glibc program).
  // Note: RCX and R11 are clobbered by the syscall instruction itself and
  // are not present in our struct syscall_regs.
  regs->rip = actual_entry;
  regs->rsp = user_rsp;
  regs->rax = 0;
  regs->rbx = 0;
  regs->rdx = 0;
  regs->rsi = 0;
  regs->rdi = 0;
  regs->rbp = 0;
  regs->r8 = 0;
  regs->r9 = 0;
  regs->r10 = 0;
  regs->r12 = 0;
  regs->r13 = 0;
  regs->r14 = 0;
  regs->r15 = 0;
  regs->user_cs = elf_info.compat32 ? 0x1b : 0x2b;
  regs->user_rcx = regs->user_rsi = regs->user_rdi = 0;
  regs->rflags = 0x202; // IF | reserved bit 1

  ptrace_exec_event(regs);

  return 0;
}


void syscall_register_process_exec(void) {
  syscall_register_raw(SYS_EXECVE, sys_execve);
}
