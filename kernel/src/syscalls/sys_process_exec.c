#include "../sched/capability.h"
// Process syscall implementation.
#include "../acpi/acpi.h"
#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "../cpu/gdt.h"
#include "../cpu/features.h"
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
  /* Static string: exec_args_free() only frees the array and arena. */
  static char setting[] = "WINEDEBUG=-all";
  char **expanded = kmalloc(((size_t)*envc + 2) * sizeof(*expanded));
  if (!expanded) return false;
  memcpy(expanded, *envp, (size_t)*envc * sizeof(*expanded));
  expanded[*envc] = setting;
  expanded[*envc + 1] = NULL;
  kfree(*envp);
  *envp = expanded;
  (*envc)++;
  return true;
}

static bool exec_vector_entry(const char **vector, int index,
                              bool compat32, const char **out) {
  if (compat32) {
    uint32_t entry32 = 0;
    const uint32_t *u32 = (const uint32_t *)vector + index;
    if (!is_user_range(u32, sizeof(entry32)) ||
        copy_from_user(&entry32, u32, sizeof(entry32)) != 0)
      return false;
    *out = (const char *)(uintptr_t)entry32;
    return true;
  }
  const char *entry64 = NULL;
  const char *const *u64 = (const char *const *)vector + index;
  if (!is_user_range(u64, sizeof(entry64)) ||
      copy_from_user(&entry64, u64, sizeof(entry64)) != 0)
    return false;
  *out = entry64;
  return true;
}

/* Kernel copies of execve's argv/envp.  Every string copied from user space
 * lives in one growable arena (offsets during the copy, pointers once done),
 * so a whole exec needs a handful of allocations instead of one per string.
 * Strings added later (shebang interpreter words, script paths) are tracked in
 * `owned` and freed with the rest by exec_args_free(). */
#define EXEC_STR_MAX 4096
#define EXEC_OWNED_MAX 16

struct exec_strings {
  char *arena;
  size_t used, cap;
  void *owned[EXEC_OWNED_MAX];
  int nowned;
};

static void exec_args_free(struct exec_strings *s, char **argv, char **envp) {
  kfree(argv);
  kfree(envp);
  kfree(s->arena);
  for (int i = 0; i < s->nowned; i++)
    kfree(s->owned[i]);
  s->arena = NULL;
  s->used = s->cap = 0;
  s->nowned = 0;
}

static bool exec_strings_own(struct exec_strings *s, void *ptr) {
  if (!ptr || s->nowned >= EXEC_OWNED_MAX)
    return false;
  s->owned[s->nowned++] = ptr;
  return true;
}

/* Copies a NULL-terminated user vector into s->arena.  On success *out_vec
 * holds arena offsets (as pointers); exec_strings_finish() rebases them once
 * the arena has stopped moving. */
static int exec_copy_user_vector(struct exec_strings *s, const char **user_vec,
                                 bool compat32, char ***out_vec,
                                 int *out_count) {
  int count = 0;
  if (user_vec) {
    while (count < 4096) {
      const char *entry = NULL;
      if (!exec_vector_entry(user_vec, count, compat32, &entry))
        return -14;
      if (!entry)
        break;
      count++;
    }
  }
  char **k_vec = kmalloc(((size_t)count + 1) * sizeof(char *));
  if (!k_vec)
    return -12;
  for (int i = 0; i < count; i++) {
    const char *entry = NULL;
    if (!exec_vector_entry(user_vec, i, compat32, &entry) || !entry ||
        !is_user_range(entry, 1)) {
      kfree(k_vec);
      return -14;
    }
    if (s->cap - s->used < EXEC_STR_MAX) {
      size_t ncap = s->cap ? s->cap * 2 : 2 * EXEC_STR_MAX;
      while (ncap - s->used < EXEC_STR_MAX)
        ncap *= 2;
      char *na = krealloc(s->arena, ncap);
      if (!na) {
        kfree(k_vec);
        return -12;
      }
      s->arena = na;
      s->cap = ncap;
    }
    long len = strncpy_from_user(s->arena + s->used, entry, EXEC_STR_MAX);
    if (len < 0 || len >= EXEC_STR_MAX) {
      kfree(k_vec);
      return len < 0 ? -14 : -7; /* E2BIG */
    }
    k_vec[i] = (char *)(uintptr_t)s->used;
    s->used += (size_t)len + 1;
  }
  k_vec[count] = NULL;
  *out_vec = k_vec;
  *out_count = count;
  return 0;
}

static void exec_strings_finish(struct exec_strings *s, char **vec, int count) {
  for (int i = 0; i < count; i++)
    vec[i] = s->arena + (uintptr_t)vec[i];
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
  char stack_path[256];
  char *path = stack_path;
  long path_len = strncpy_from_user(stack_path, user_path, sizeof(stack_path));
  if (path_len < 0)
    return (uint64_t)-14;
  if ((size_t)path_len >= sizeof(stack_path)) {
    path = kmalloc(4096);
    if (!path) return (uint64_t)-12;
    path_len = strncpy_from_user(path, user_path, 4096);
    if (path_len < 0 || path_len >= 4096) {
      if (path != stack_path) kfree(path);
      return path_len < 0 ? (uint64_t)-14 : (uint64_t)-36;
    }
  }

  /* Recording the alias itself as exe_path would make /proc/self/exe point
   * back to itself after exec, breaking the next Chromium service launch. */
  struct thread *exec_caller = sched_get_current();
  if (!strcmp(path, "/proc/self/exe") ||
      !strcmp(path, "/proc/thread-self/exe")) {
    if (!exec_caller || !exec_caller->exe_path[0]) {
      if (path != stack_path) kfree(path);
      return (uint64_t)-2; // ENOENT
    }
    strcpy(path, exec_caller->exe_path);
  }

  TSC_BEGIN(execve_arg_copy);
  struct exec_strings strs = {0};
  int argc = 0;
  char **k_argv = NULL;
  int vec_rc = exec_copy_user_vector(&strs, user_argv, compat32, &k_argv, &argc);
  if (vec_rc < 0) {
    if (path != stack_path) kfree(path);
    exec_args_free(&strs, NULL, NULL);
    return (uint64_t)vec_rc;
  }

  int envc = 0;
  char **k_envp = NULL;
  vec_rc = exec_copy_user_vector(&strs, user_envp, compat32, &k_envp, &envc);
  if (vec_rc < 0) {
    if (path != stack_path) kfree(path);
    exec_args_free(&strs, k_argv, NULL);
    return (uint64_t)vec_rc;
  }
  exec_strings_finish(&strs, k_argv, argc);
  exec_strings_finish(&strs, k_envp, envc);
  if (!exec_wine_debug_env(path, &k_envp, &envc)) {
    if (path != stack_path) kfree(path);
    exec_args_free(&strs, k_argv, k_envp);
    return (uint64_t)-12;
  }
  TSC_END(execve_arg_copy);

  // Shebang (#!) script interpretation loop (up to 4 levels of recursion)
  struct thread *exec_thread = sched_get_current();
  int shebang_depth = 0;
  uint32_t exec_mode = 0, exec_uid = 0, exec_gid = 0;
  uint64_t exec_mount_flags = 0;
  /* Referenced node of the binary that elf_load() will map.  Holding it from
   * here on means the final path is resolved once, not again in the loader. */
  vfs_node_t *exec_file = NULL;
  /* Absolute path of the script the caller exec'd, for the uname policy. */
  char *uname_script = NULL;

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
      if (KLOG_RATELIMITED("PROC execve failed")) {
        klog_puts("[PROC] execve failed path=\"");
        klog_puts(path);
        klog_puts("\" errno=");
        klog_uint64((uint64_t)exec_err);
        klog_puts("\n");
      }
      if (path != stack_path) kfree(path);
      exec_args_free(&strs, k_argv, k_envp);
      return (uint64_t)(-exec_err);
    }
    exec_mode = exec_node->mask;
    exec_mount_flags = vfs_mount_flags(exec_node);
    exec_uid = exec_node->uid;
    exec_gid = exec_node->gid;

    char header[256];
    uint32_t hlen = vfs_read(exec_node, 0, sizeof(header) - 1, (uint8_t *)header);

    if (hlen >= 2 && header[0] == '#' && header[1] == '!') {
      vfs_close(exec_node);
      if (!uname_script && strs.nowned < EXEC_OWNED_MAX - 3) {
        /* Owned by strs, so every exit path frees it with the arena. */
        char *script = kmalloc(256);
        if (script && exec_save_absolute_path(script, 256, path,
                exec_thread ? exec_thread->cwd_path : "/")) {
          exec_strings_own(&strs, script);
          uname_script = script;
        } else {
          kfree(script);
        }
      }
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
        if (path != stack_path) kfree(path);
        exec_args_free(&strs, k_argv, k_envp);
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
        if (path != stack_path) kfree(path);
        exec_args_free(&strs, k_argv, k_envp);
        return (uint64_t)-12;
      }

      /* The script path moves into argv; interpreter words get their own
       * buffers.  All are owned by strs and freed with the arena. */
      char *bin_copy = kmalloc(strlen(interp_bin) + 1);
      char *arg_copy = interp_arg ? kmalloc(strlen(interp_arg) + 1) : NULL;
      char *next_path = kmalloc(strlen(interp_bin) + 1);
      if (!bin_copy || (interp_arg && !arg_copy) || !next_path ||
          strs.nowned + 3 > EXEC_OWNED_MAX) {
        kfree(bin_copy);
        kfree(arg_copy);
        kfree(next_path);
        kfree(new_k_argv);
        if (path != stack_path) kfree(path);
        exec_args_free(&strs, k_argv, k_envp);
        return (uint64_t)-12;
      }
      strcpy(bin_copy, interp_bin);
      strcpy(next_path, interp_bin);
      exec_strings_own(&strs, bin_copy);
      /* Only a heap path is owned.  A short path is still the stack_path
       * buffer, and exec_args_free() must never kfree() a kernel-stack
       * address; argv only needs it until this exec returns. */
      if (path != stack_path)
        exec_strings_own(&strs, path);
      new_k_argv[0] = bin_copy;

      int dst_idx = 1;
      if (interp_arg) {
        strcpy(arg_copy, interp_arg);
        exec_strings_own(&strs, arg_copy);
        new_k_argv[dst_idx++] = arg_copy;
      }
      new_k_argv[dst_idx++] = path; // Reuse script path buffer

      for (int i = 1; i < argc; i++) {
        new_k_argv[dst_idx++] = k_argv[i];
      }
      new_k_argv[new_argc] = NULL;

      kfree(k_argv);
      k_argv = new_k_argv;
      argc = new_argc;
      path = next_path;

      shebang_depth++;
      continue;
    }

    // Binary executable verified; keep its reference for elf_load().
    exec_file = exec_node;
    break;
  }

  if (!exec_file) {
    // Shebang depth limit reached: the last interpreter was never verified.
    if (path != stack_path) kfree(path);
    exec_args_free(&strs, k_argv, k_envp);
    return (uint64_t)-8; // ENOEXEC
  }

  char saved_exe_path[sizeof(exec_caller->exe_path)];
  if (!exec_save_absolute_path(saved_exe_path, sizeof(saved_exe_path), path,
                                exec_caller ? exec_caller->cwd_path : "/")) {
    vfs_close(exec_file);
    if (path != stack_path) kfree(path);
    exec_args_free(&strs, k_argv, k_envp);
    return (uint64_t)-36; // ENAMETOOLONG: never publish a truncated exe link
  }

  TSC_BEGIN(execve_pml4_create);
  uint64_t *new_pml4 = vmm_create_pml4();
  TSC_END(execve_pml4_create);
  if (!new_pml4) {
    vfs_close(exec_file);
    if (path != stack_path) kfree(path);
    exec_args_free(&strs, k_argv, k_envp);
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
      vfs_close(exec_file);
      if (path != stack_path) kfree(path);
      exec_args_free(&strs, k_argv, k_envp);
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
  if (!elf_load(exec_file, new_pml4, &elf_info)) {
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
    if (path != stack_path) kfree(path);
    exec_args_free(&strs, k_argv, k_envp);
    // Note: new_pml4 is leaked if elf_load fails. Fixing that would require
    // vmm_destroy_pml4 or similar, but for now we focus on the reported leak.
    return (uint64_t)-8; // ENOEXEC
  }

  TSC_END(execve_elf_load);

  /* Point of no return: the new image is loaded. Like Linux de_thread(),
   * the old image's other threads must not survive into it - in particular
   * not while exec_close_cloexec() empties the fd table they share. */
  sched_exec_kill_siblings(current);
  if (current->tid != current->tgid)
    klogf("[PROC] execve from non-leader tid=%u tgid=%u: leader kept\n",
          current->tid, current->tgid);

  /* Unshare the fd table if shared with a non-thread parent (CLONE_FILES)
   * so closing FD_CLOEXEC descriptors never tears down the parent's fds. */
  if (sched_unshare_files(current))
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

#ifdef TSC_AUTO_DUMP
  /* Parent-side vfork_wait already dumps completed samples. Avoid rendering
   * a duplicate exec table while the borrowing child still holds it asleep. */
  bool borrowed_vfork = (current->clone_flags & CLONE_VFORK) != 0;
#endif
  /* Successful exec starts with architectural FPU defaults. */
  fpu_reset_current();

  // ENOEXEC fallbacks above retain the original address space and keep waiting.
  vfork_release_parent(current);

  /* Probes are cumulative (fork stats must survive to be averaged), so print
   * the first few launches; fork_probe_maybe_dump() covers later activity. */
  {
#ifdef TSC_AUTO_DUMP /* prctl(0x50544344) still dumps on demand */
    static uint32_t exec_probe_dumps;
    if (!borrowed_vfork && exec_probe_dumps < 4) {
      exec_probe_dumps++;
      tsc_probe_dump();
    }
#endif
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
  {
    /* Script identity survives `#!/usr/bin/env bash`: env re-execs the
     * interpreter with the script path still in argv. */
    bool keep = false;
    if (!uname_script && current->uname_key_script && current->uname_key[0])
      for (int i = 1; i < argc && k_argv[i]; i++)
        if (strcmp(k_argv[i], current->uname_key) == 0) { keep = true; break; }
    if (uname_script) {
      strncpy(current->uname_key, uname_script, sizeof(current->uname_key) - 1);
      current->uname_key[sizeof(current->uname_key) - 1] = 0;
      current->uname_key_script = true;
    } else if (!keep) {
      strcpy(current->uname_key, current->exe_path);
      current->uname_key_script = false;
    }
    extern void uname_policy_on_exec(struct thread *t);
    uname_policy_on_exec(current);
  }
  /* Queue readahead for the windows this executable faulted in last time. */
  vfs_exec_profile_prefetch(current->exe_path);

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
  /* Fault counters describe the new image only, so /proc/<pid>/status shows
   * what this launch cost (Linux keeps them across exec; we want startup). */
  __atomic_store_n(&current->mm->nr_faults, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&current->mm->maj_flt, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&current->mm->fault_cycles, 0, __ATOMIC_RELAXED);
  for (int c = 0; c < 3; c++) {
    __atomic_store_n(&current->mm->fault_class_count[c], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&current->mm->fault_class_cycles[c], 0, __ATOMIC_RELAXED);
  }
  __atomic_store_n(&current->mm->nr_syscalls, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&current->mm->syscall_cycles, 0, __ATOMIC_RELAXED);

  // Now destroy the old VMA tree nodes
  TSC_BEGIN(exec_vma_destroy);
  vma_list_destroy(&old_vmas);
  TSC_END(exec_vma_destroy);

  // Cleanup kernel-side copies
  exec_args_free(&strs, k_argv, k_envp);
  if (path != stack_path) kfree(path);

  // Reset TLS Bases for this process
  wrmsr(IA32_KERNEL_GS_BASE, 0);
  wrmsr(IA32_FS_BASE, 0);
  struct thread *ct = sched_get_current();
  if (ct) {
    ct->fs_base = 0;
    ct->gs_base = 0;
    if (cpu_has_pku_flag) {
      wrpkru(0);
      ct->pkru = 0;
    }
    if (ct->mm)
      ct->mm->pkey_alloc_mask = 1;
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

  if (ct) {
    ct->restore_callee_regs = 1;
    thread_update_fastpath_state(ct);
  }

  return 0;
}


void syscall_register_process_exec(void) {
  syscall_register_raw(SYS_EXECVE, sys_execve);
}
