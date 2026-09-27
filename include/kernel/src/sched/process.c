#include "../console/console.h"
#include "../console/klog.h"
#include "../cpu/gdt.h"
#include "../cpu/isr.h"
#include "../cpu/msr.h"
#include "../fb/framebuffer.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../smp/cpu.h"
#include "../syscalls/syscall.h"
#define TSC_PROBES_ENABLE
#include "../lib/tsc.h"
#include "elf.h"
#include "sched.h"

/* The exec stack is registered as a VMA but its pages are mapped on demand.
 * The initial stack builder writes through this helper, which backs each
 * touched page with a freshly zeroed frame. */
static void process_ensure_user_page(uint64_t *pml4, uint64_t va) {
  uint64_t page = va & ~(PAGE_SIZE - 1);
  if (vmm_virt_to_phys(pml4, page))
    return;

  void *phys = pmm_alloc();
  if (!phys)
    return;
  if (!vmm_map_page(pml4, page, (uint64_t)phys,
                    PAGE_FLAG_USER | PAGE_FLAG_RW | PAGE_FLAG_PRESENT)) {
    pmm_free(phys);
    return;
  }
  memset((void *)((uint64_t)phys + pmm_get_hhdm_offset()), 0, PAGE_SIZE);
}

static void process_copy_to_user(uint64_t *pml4, uint64_t dest_user_va,
                                 const void *src_kern_va, size_t size) {
  uint64_t hhdm = pmm_get_hhdm_offset();
  size_t done = 0;
  while (done < size) {
    uint64_t va = dest_user_va + done;
    uint64_t offset = va % PAGE_SIZE;
    uint32_t to_copy = PAGE_SIZE - offset;
    if (to_copy > size - done)
      to_copy = size - done;

    process_ensure_user_page(pml4, va);
    uint64_t phys = vmm_virt_to_phys(pml4, va);
    if (phys != 0) {
      memcpy((void *)(phys + hhdm), (const uint8_t *)src_kern_va + done,
             to_copy);
    }
    done += to_copy;
  }
}

// Removed process_return_ctx as we now use restart_main_session

// MSR constants for TLS base registers
#define IA32_KERNEL_GS_BASE 0xC0000102
#define IA32_FS_BASE 0xC0000100

extern void mm_reset_mmap_state(struct thread *t);

// Inline assembly to perform sysret transition.
// Note: sysret loads CS from STAR MSR and SS from STAR MSR + 8.
// We must clear unnecessary registers to prevent info leaks.
void process_jump_usermode(uint64_t rip, uint64_t user_rsp, uint64_t pml4) {
  uint64_t hhdm = pmm_get_hhdm_offset();

  // DEBUG: Hexdump the entry point
  uint64_t rip_phys = vmm_virt_to_phys((uint64_t *)pml4, rip);
  if (rip_phys) {
    uint8_t *code = (uint8_t *)(rip_phys + hhdm);
    klog_puts("[PROC] RIP Dump: ");
    for (int i = 0; i < 128; i++) {
      uint8_t b = code[i];
      const char *hex = "0123456789ABCDEF";
      klog_putchar(hex[(b >> 4) & 0xF]);
      klog_putchar(hex[b & 0xF]);
      klog_putchar(' ');
    }
    klog_puts("\n");
  }

  // DEBUG: Hexdump the stack
  uint64_t rsp_phys = vmm_virt_to_phys((uint64_t *)pml4, user_rsp);
  if (rsp_phys) {
    uint64_t *stack = (uint64_t *)(rsp_phys + hhdm);
    klog_puts("[PROC] RSP Dump: ");
    for (int i = 0; i < 32; i++) {
      klog_uint64(stack[i]);
      klog_putchar(' ');
    }
    klog_puts("\n");
  }

  // Reset user TLS to 0
  wrmsr(IA32_KERNEL_GS_BASE, 0);
  wrmsr(IA32_FS_BASE, 0);
  struct thread *cur = sched_get_current();
  if (cur) {
    cur->fs_base = 0;
    cur->gs_base = 0;
  }

  register uint64_t asm_rip asm("rcx") = rip;
  register uint64_t asm_rflags asm("r11") = 0x202;
  register uint64_t asm_rsp asm("rdi") =
      user_rsp; // Use rdi temporarily to hold stack

  __asm__ volatile(".intel_syntax noprefix\n"
                   "mov ax, 0x23\n"
                   "mov ds, ax\n"
                   "mov es, ax\n"
                   "mov rsp, %[usr_stack]\n"

                   // Clear remaining general-purpose registers
                   "xor rax, rax\n"
                   "xor rbx, rbx\n"
                   "xor rdx, rdx\n"
                   "xor rdi, rdi\n"
                   "xor rsi, rsi\n"
                   "xor r8, r8\n"
                   "xor r9, r9\n"
                   "xor r10, r10\n"
                   "xor r12, r12\n"
                   "xor r13, r13\n"
                   "xor r14, r14\n"
                   "xor r15, r15\n"
                   "xor rbp, rbp\n"

                   "swapgs\n" // Put user's GS base (0) into active GS, and save
                              // kernel GS base

                   // Jump to user space!
                   "sysretq\n"
                   ".att_syntax prefix\n"
                   :
                   : "c"(asm_rip), "r"(asm_rflags), [usr_stack] "r"(asm_rsp)
                   : "memory");
  while (1)
    ;
}

#define PAGE_SIZE 4096
#define ELF_PIE_BASE 0x0000000000400000ULL
#define ELF_INTERP_BASE 0x0000400000000000ULL

static uint64_t elf_page_flags(uint32_t p_flags) {
  uint64_t flags = PAGE_FLAG_USER | PAGE_FLAG_PRESENT;
  if (p_flags & PF_W)
    flags |= PAGE_FLAG_RW;
  if (!(p_flags & PF_X))
    flags |= PAGE_FLAG_NX;
  return flags;
}

static bool __attribute__((unused))
elf_apply_segment_permissions(uint64_t *pml4, uint64_t start_page,
                               uint64_t end_page, uint32_t p_flags) {
  uint64_t final_flags = elf_page_flags(p_flags);

  for (uint64_t page = start_page; page < end_page; page += PAGE_SIZE) {
    uint64_t phys = vmm_virt_to_phys(pml4, page);
    if (!phys)
      continue;

    vmm_unmap_page(pml4, page);
    if (!vmm_map_page(pml4, page, phys & PAGE_MASK, final_flags))
      return false;
  }

  return true;
}

static bool do_elf_load(const char *path, uint64_t *pml4,
                        uint64_t requested_base, bool is_interp,
                        elf_info_t *out_info, char *interp_path,
                        size_t interp_max_len) {
  (void)pml4;
  struct thread *current_thread = sched_get_current();
  vfs_node_t *file = vfs_resolve_path(path);
  if (!file) {
    klog_puts("[PROC] ELF load failed: File not found: ");
    klog_puts(path);
    klog_puts("\n");
    return false;
  }

  TSC_BEGIN(elf_phdr);
  // Read the ELF Header
  Elf64_Ehdr ehdr;
  if (vfs_read(file, 0, sizeof(Elf64_Ehdr), (uint8_t *)&ehdr) !=
      sizeof(Elf64_Ehdr)) {
    TSC_END(elf_phdr);
    vfs_close(file);
    return false;
  }

  if (ehdr.e_ident[0] != 0x7f || ehdr.e_ident[1] != 'E' ||
      ehdr.e_ident[2] != 'L' || ehdr.e_ident[3] != 'F' ||
      ehdr.e_ident[4] != 2) {
    TSC_END(elf_phdr);
    vfs_close(file);
    return false;
  }
  if (ehdr.e_type != ET_EXEC && ehdr.e_type != ET_DYN) {
    TSC_END(elf_phdr);
    klog_puts("[PROC] ELF load failed: unsupported ELF type\n");
    vfs_close(file);
    return false;
  }

  uint64_t load_base = requested_base;
  if (!is_interp && ehdr.e_type == ET_DYN && load_base == 0) {
    load_base = ELF_PIE_BASE;
  }

  // Read the whole program header table in one vfs_read() when the layout is
  // standard.  A typical binary has 10-15 headers, so this replaces that many
  // VFS round trips with one read (usually a page-cache hit); unusual
  // e_phentsize values fall back to per-header reads in the loop below.
  uint32_t phdr_tab_bytes = (uint32_t)ehdr.e_phnum * ehdr.e_phentsize;
  uint8_t *phdr_tab = NULL;
  if (phdr_tab_bytes > 0 && phdr_tab_bytes <= 64 * 1024 &&
      ehdr.e_phentsize == sizeof(Elf64_Phdr)) {
    phdr_tab = kmalloc(phdr_tab_bytes);
    if (phdr_tab &&
        vfs_read(file, ehdr.e_phoff, phdr_tab_bytes, phdr_tab) !=
            phdr_tab_bytes) {
      kfree(phdr_tab);
      phdr_tab = NULL;
    }
  }
  TSC_END(elf_phdr);

  // Single pass over program headers: collect PT_INTERP, PT_PHDR, and
  // process PT_LOAD segments all at once.
  TSC_BEGIN(elf_segments);
  uint64_t phdr_vaddr = 0;
  for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
    Elf64_Phdr phdr;
    if (phdr_tab) {
      memcpy(&phdr, phdr_tab + (uint32_t)i * ehdr.e_phentsize,
             sizeof(Elf64_Phdr));
    } else {
      uint32_t offset = ehdr.e_phoff + (i * ehdr.e_phentsize);
      if (vfs_read(file, offset, sizeof(Elf64_Phdr), (uint8_t *)&phdr) !=
          sizeof(Elf64_Phdr))
        continue;
    }

    // Collect interpreter path from PT_INTERP.
    if (phdr.p_type == PT_INTERP && interp_path) {
      uint32_t len = phdr.p_filesz;
      if (len >= interp_max_len)
        len = interp_max_len - 1;
      vfs_read(file, phdr.p_offset, len, (uint8_t *)interp_path);
      interp_path[len] = '\0';
    }

    // Track phdr table virtual address (PT_PHDR takes priority; fall back to
    // the PT_LOAD segment that contains the phdr table if PT_PHDR is absent).
    if (phdr.p_type == PT_PHDR) {
      phdr_vaddr = load_base + phdr.p_vaddr;
    } else if (phdr.p_type == PT_LOAD && phdr_vaddr == 0 &&
               ehdr.e_phoff >= phdr.p_offset &&
               ehdr.e_phoff < phdr.p_offset + phdr.p_filesz) {
      phdr_vaddr = load_base + phdr.p_vaddr + (ehdr.e_phoff - phdr.p_offset);
    }

    if (phdr.p_type != PT_LOAD)
      continue;

    uint64_t vaddr = load_base + phdr.p_vaddr;
    uint64_t memsz = phdr.p_memsz;
    uint64_t filesz = phdr.p_filesz;
    uint32_t file_offset = phdr.p_offset;

    // Track uppermost loaded address for the main program brk base.
    if (!is_interp && current_thread && current_thread->mm) {
      uint64_t seg_end = vaddr + memsz;
      if (seg_end > current_thread->mm->brk_base) {
        current_thread->mm->brk_base = seg_end;
        current_thread->mm->brk_current = seg_end;
      }
    }

    if (memsz > 0) {
      uint64_t start_page = vaddr & ~(PAGE_SIZE - 1);
      uint64_t end_page = (vaddr + memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
      uint64_t vma_offset = file_offset - (vaddr - start_page);
      uint64_t vma_file_size = (vaddr - start_page) + filesz;

      // Register segment in VMA list for lazy demand paging via page fault.
      if (current_thread && current_thread->mm) {
        uint64_t prot = 0;
        if (phdr.p_flags & PF_R)
          prot |= 0x1;
        if (phdr.p_flags & PF_W)
          prot |= 0x2;
        if (phdr.p_flags & PF_X)
          prot |= 0x4;

        vma_add(&current_thread->mm->vmas, start_page, end_page, prot,
                MAP_PRIVATE, -1, vma_offset, file, vma_file_size);

        // Queue the PT_LOAD file pages for the async prefetch worker in
        // 256 KB windows.  execve() no longer waits for the whole image: the
        // worker streams the remaining pages while the process starts running,
        // and demand paging fills anything the worker has not reached yet.
        if (filesz > 0 && file) {
          const uint32_t prefetch_chunk = 256 * 1024;
          uint32_t start_off = file_offset & ~(PAGE_SIZE - 1);
          uint32_t end_off = (file_offset + filesz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
          for (uint32_t poff = start_off; poff < end_off; poff += prefetch_chunk) {
            uint32_t remaining = end_off - poff;
            vfs_cache_prefetch_async(file, poff, remaining < prefetch_chunk
                                                     ? remaining
                                                     : prefetch_chunk);
          }
        }
      }
    }
  }
  if (phdr_tab)
    kfree(phdr_tab);
  TSC_END(elf_segments);

  if (out_info) {
    if (is_interp) {
      out_info->interp_base = load_base;
      out_info->interp_entry = load_base + ehdr.e_entry;
    } else {
      out_info->entry = load_base + ehdr.e_entry;
      out_info->load_base = load_base;
      out_info->phdr = phdr_vaddr;
      out_info->phentsize = ehdr.e_phentsize;
      out_info->phnum = ehdr.e_phnum;
    }
  }

  vfs_close(file);
  return true;
}

bool elf_load(const char *path, uint64_t *pml4, elf_info_t *out_info) {
  struct thread *current_thread = sched_get_current();
  if (current_thread && current_thread->mm) {
    current_thread->mm->brk_base = 0;
    current_thread->mm->brk_current = 0;
  }

  char interp_path[256] = {0};
  elf_info_t main_info = {0};

  if (!do_elf_load(path, pml4, 0, false, &main_info, interp_path,
                   sizeof(interp_path))) {
    return false;
  }

  if (interp_path[0] != '\0') {
    elf_info_t interp_info = {0};
    if (!do_elf_load(interp_path, pml4, ELF_INTERP_BASE, true, &interp_info,
                     NULL, 0)) {
      return false;
    }
    if (out_info) {
      *out_info = main_info;
      out_info->interp_base = interp_info.interp_base;
      out_info->interp_entry = interp_info.interp_entry;
    }
  } else {
    if (out_info) {
      *out_info = main_info;
    }
  }

  // User stack: register the region now, map pages lazily.  The initial stack
  // builder maps whichever pages it writes through process_copy_to_user();
  // faults fill the rest as the program grows its stack (MAP_GROWSDOWN).
  uint64_t stack_top = ASCENTOS_USER_STACK_TOP;
  uint64_t stack_size = 4 * PAGE_SIZE;
  uint64_t stack_bottom = stack_top - stack_size;

  // Page-align the brk base upward and set current brk.
  if (current_thread && current_thread->mm) {
    vma_add(&current_thread->mm->vmas, stack_bottom, stack_top, 0x3,
            0x22 | MAP_GROWSDOWN, -1, 0, NULL, 0);
    vma_add(&current_thread->mm->vmas, VDSO_USER_BASE,
            VDSO_USER_BASE + PAGE_SIZE, 0x5, MAP_PRIVATE | MAP_ANONYMOUS, -1,
            0, NULL, 0);

    current_thread->mm->brk_base =
        (current_thread->mm->brk_base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    current_thread->mm->brk_current = current_thread->mm->brk_base;
    mm_reset_mmap_state(current_thread);
  }

  // Pre-fault the entry point code page so context switching into userland does not immediately trigger a page fault
  if (out_info && out_info->entry) {
    uint64_t entry_addr = out_info->interp_entry ? out_info->interp_entry : out_info->entry;
    TSC_BEGIN(elf_entry_fault);
    vmm_handle_page_fault(entry_addr, 0x4, NULL);
    TSC_END(elf_entry_fault);
  }

  return true;
}

uint64_t process_build_initial_stack(uint64_t stack_top, const char *path,
                                     const char **argv, const char **envp,
                                     const elf_info_t *elf_info) {
  // Create a default environment if none provided
  const char *default_envp[] = {"PATH=/opt/bash/bin:/opt/tcc/bin:/",
                                "HOME=/",
                                "TERM=xterm-256color",
                                "USER=root",
                                "PS1=\033[0;32mRoot@AscentOS\033[0m:\\w\\$ ",
                                NULL};
  if (!envp) {
    envp = default_envp;
  }

  if (!argv) {
    const char *temp_argv[2] = {path, NULL};
    argv = temp_argv;
  }

  int argc = 0;
  int envc = 0;
  size_t string_bytes = 0;

  if (argv) {
    while (argv[argc] != NULL) {
      string_bytes += strlen(argv[argc]) + 1;
      argc++;
    }
  }

  if (envp) {
    while (envp[envc] != NULL) {
      string_bytes += strlen(envp[envc]) + 1;
      envc++;
    }
  }

  // Add extra padding to ensure alignment can be satisfied + AT_RANDOM (16) + AT_PLATFORM (8)
  string_bytes += 16 + 8 + 64;

  // 2. Determine number of stack entries (argc, argv ptrs, envp ptrs, auxv
  // pairs) Auxv entries (19 real pairs + 1 NULL pair)
  size_t auxv_pairs = elf_info ? 19 : 1;
  size_t stack_entry_count =
      1 + (size_t)argc + 1 + (size_t)envc + 1 + auxv_pairs * 2;
  size_t pointer_bytes = stack_entry_count * sizeof(uint64_t);

  // 3. Layout the areas from high to low address:
  // [stack_top]
  // Strings Area (aligned)
  // Pointers/Auxv Area (aligned)
  // [final_sp]

  uint64_t string_area_top = stack_top;
  uint64_t string_area_bottom = string_area_top - string_bytes;
  string_area_bottom &= ~0xFULL; // Align string area start

  uint64_t pointer_area_top = string_area_bottom;
  uint64_t pointer_area_bottom = pointer_area_top - pointer_bytes;
  pointer_area_bottom &=
      ~0xFULL; // Ensure pointer area starts on 16-byte boundary

  uint64_t final_sp = pointer_area_bottom;

  // 4. Copy strings to the Strings Area and collect their addresses
  uint64_t *argv_ptrs = kmalloc((argc > 0 ? argc : 1) * sizeof(uint64_t));
  uint64_t *envp_ptrs = kmalloc((envc > 0 ? envc : 1) * sizeof(uint64_t));
  if (!argv_ptrs || !envp_ptrs) {
    if (argv_ptrs)
      kfree(argv_ptrs);
    if (envp_ptrs)
      kfree(envp_ptrs);
    return 0;
  }

  uint64_t current_string = string_area_bottom;
  /* Mirror the argv strings into a kernel-side blob while they are being
   * written to the user stack, so /proc/<pid>/cmdline can report the real
   * command line later without touching user memory.  The strings are packed
   * back to back here in exactly the order Linux lays them out, so the blob is
   * byte-identical to what a reader would find between arg_start and arg_end. */
  uint32_t cmdline_len  = 0;
  uint64_t argv_bytes   = 0;
  char cmdline_buf[sizeof(((struct mm_struct *)0)->saved_cmdline)];
  bool     cmdline_fits = true;
  for (int i = 0; i < argc; i++) {
    argv_ptrs[i] = current_string;
    size_t len = strlen(argv[i]) + 1;
    process_copy_to_user(vmm_get_active_pml4(), current_string, argv[i], len);
    current_string += len;
    argv_bytes += len;
    /* Stop at the first argument that does not fit, so the blob is always a
     * whole-argument prefix of argv: a pattern must never match half a path. */
    if (!cmdline_fits || cmdline_len + len > sizeof(cmdline_buf)) {
      cmdline_fits = false;
      continue;
    }
    memcpy(cmdline_buf + cmdline_len, argv[i], len);
    cmdline_len += (uint32_t)len;
  }
  for (int i = 0; i < envc; i++) {
    envp_ptrs[i] = current_string;
    size_t len = strlen(envp[i]) + 1;
    process_copy_to_user(vmm_get_active_pml4(), current_string, envp[i], len);
    current_string += len;
  }

  // AT_RANDOM data (16 bytes)
  current_string = (current_string + 15) & ~15ULL;
  uint64_t at_random_addr = current_string;
  {
    uint8_t rnd[16];
    for (int i = 0; i < 16; i++)
      rnd[i] = (uint8_t)(i * 7 + 0xA5);
    process_copy_to_user(vmm_get_active_pml4(), current_string, rnd, 16);
    current_string += 16;
  }

  // AT_PLATFORM string ("x86_64")
  current_string = (current_string + 15) & ~15ULL;
  uint64_t at_platform_addr = current_string;
  process_copy_to_user(vmm_get_active_pml4(), at_platform_addr, "x86_64", 7);
  current_string += 8;

  // 5. Build the Pointers/Auxv Area
  uint64_t *stack_entries = kmalloc(stack_entry_count * sizeof(uint64_t));
  if (!stack_entries) {
    kfree(argv_ptrs);
    kfree(envp_ptrs);
    return 0;
  }

  int idx = 0;
  stack_entries[idx++] = (uint64_t)argc;
  for (int i = 0; i < argc; i++)
    stack_entries[idx++] = argv_ptrs[i];
  stack_entries[idx++] = 0; // end argv
  for (int i = 0; i < envc; i++)
    stack_entries[idx++] = envp_ptrs[i];
  stack_entries[idx++] = 0; // end envp

  if (elf_info) {
    stack_entries[idx++] = AT_PAGESZ;
    stack_entries[idx++] = PAGE_SIZE;
    stack_entries[idx++] = AT_PHDR;
    stack_entries[idx++] = elf_info->phdr;
    stack_entries[idx++] = AT_PHENT;
    stack_entries[idx++] = elf_info->phentsize;
    stack_entries[idx++] = AT_PHNUM;
    stack_entries[idx++] = elf_info->phnum;
    stack_entries[idx++] = AT_ENTRY;
    stack_entries[idx++] = elf_info->entry;
    stack_entries[idx++] = AT_UID;
    stack_entries[idx++] = 0;
    stack_entries[idx++] = AT_EUID;
    stack_entries[idx++] = 0;
    stack_entries[idx++] = AT_GID;
    stack_entries[idx++] = 0;
    stack_entries[idx++] = AT_EGID;
    stack_entries[idx++] = 0;
    stack_entries[idx++] = AT_SECURE;
    stack_entries[idx++] = 0;
    stack_entries[idx++] = AT_RANDOM;
    stack_entries[idx++] = at_random_addr;
    stack_entries[idx++] = AT_PLATFORM;
    stack_entries[idx++] = at_platform_addr;
    stack_entries[idx++] = AT_BASE;
    stack_entries[idx++] = elf_info->interp_base;
    stack_entries[idx++] = AT_FLAGS;
    stack_entries[idx++] = 0;
    stack_entries[idx++] = AT_HWCAP;
    stack_entries[idx++] = 0;
    stack_entries[idx++] = AT_CLKTCK;
    stack_entries[idx++] = 100;
    stack_entries[idx++] = AT_SYSINFO_EHDR;
    stack_entries[idx++] = VDSO_USER_BASE;
    stack_entries[idx++] = AT_EXECFN;
    stack_entries[idx++] = argv_ptrs[0];
  }
  stack_entries[idx++] = AT_NULL;
  stack_entries[idx++] = 0;

  struct thread *cur = sched_get_current();
  if (cur && cur->mm) {
    size_t aux_start = 1 + argc + 1 + envc + 1;
    size_t aux_entries = (idx > aux_start) ? (idx - aux_start) : 0;
    if (aux_entries > 64)
      aux_entries = 64;
    for (size_t i = 0; i < aux_entries; i++) {
      cur->mm->saved_auxv[i] = stack_entries[aux_start + i];
    }
    cur->mm->auxv_count = (uint32_t)aux_entries;

    /* Command line for /proc/<pid>/cmdline.  If argv did not fit, keep the
     * longest whole-argument prefix rather than a truncated final token: a
     * pattern must never match against half a path. */
    memset(cur->mm->saved_cmdline, 0, sizeof(cur->mm->saved_cmdline));
    memcpy(cur->mm->saved_cmdline, cmdline_buf, cmdline_len);
    cur->mm->cmdline_len = cmdline_len;
    cur->mm->arg_start   = argc > 0 ? string_area_bottom : 0;
    cur->mm->arg_end     = string_area_bottom + argv_bytes;
  }

  // 6. Copy Pointers/Auxv to user space at final_sp
  process_copy_to_user(vmm_get_active_pml4(), final_sp, stack_entries,
                       idx * sizeof(uint64_t));

  klog_puts("[PROC] Stack built:\n");
  klog_puts("  argc=");
  klog_uint64(argc);
  klog_puts("  random_addr=");
  klog_uint64(at_random_addr);
  klog_puts("  final_sp=");
  klog_uint64(final_sp);
  klog_puts("\n");

  kfree(argv_ptrs);
  kfree(envp_ptrs);
  kfree(stack_entries);

  return final_sp;
}

// Fix up __libc.auxv for TCC-compiled binaries
// TCC's _start calls main() directly, skipping __libc_start_main.
// musl's mallocng reads __libc.auxv to find AT_RANDOM for its secret.
// If auxv is NULL the code crashes. This helper finds the __libc symbol
// in the binary's dynsym and writes the auxv pointer into it.
//
// musl's __libc layout (first 3 fields):
//   offset 0: char *can_do_threads (or similar)
//   offset 8: size_t *auxv              ← we populate this
//   ...
static void process_fixup_libc_auxv(const char *path, uint64_t *pml4,
                                    uint64_t user_sp,
                                    const elf_info_t *elf_info) {
  uint64_t hhdm = pmm_get_hhdm_offset();
  uint64_t load_base = elf_info ? elf_info->load_base : 0;
  vfs_node_t *file = vfs_resolve_path(path);
  if (!file)
    return;

  Elf64_Ehdr ehdr;
  if (vfs_read(file, 0, sizeof(ehdr), (uint8_t *)&ehdr) != sizeof(ehdr))
    return;

  // Find PT_DYNAMIC to locate the DYNAMIC section's VA
  uint64_t dyn_vaddr = 0, dyn_memsz = 0;
  for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
    Elf64_Phdr phdr;
    uint32_t off = ehdr.e_phoff + i * ehdr.e_phentsize;
    if (vfs_read(file, off, sizeof(phdr), (uint8_t *)&phdr) != sizeof(phdr))
      continue;
    if (phdr.p_type == PT_DYNAMIC) {
      dyn_vaddr = load_base + phdr.p_vaddr;
      dyn_memsz = phdr.p_memsz;
      break;
    }
  }
  if (!dyn_vaddr)
    return;

  // Walk the .dynamic entries (already in memory) to find SYMTAB, STRTAB,
  // SYMENT
  uint64_t symtab_va = 0, strtab_va = 0;
  uint64_t syment = 24; // default Elf64_Sym size

  // Read dynamic entries via HHDM (they're mapped in the process pages)
  for (uint64_t off = 0; off < dyn_memsz; off += 16) {
    uint64_t va = dyn_vaddr + off;
    uint64_t phys = vmm_virt_to_phys(pml4, va & ~0xFFFULL);
    if (!phys)
      continue;
    uint64_t *entry = (uint64_t *)(phys + hhdm + (va & 0xFFF));
    uint64_t d_tag = entry[0], d_val = entry[1];
    if (d_tag == 0)
      break; // DT_NULL
    if (d_tag == 6)
      symtab_va = load_base + d_val; // DT_SYMTAB
    if (d_tag == 5)
      strtab_va = load_base + d_val; // DT_STRTAB
    if (d_tag == 11)
      syment = d_val; // DT_SYMENT
  }
  if (!symtab_va || !strtab_va)
    return;

  // Walk the dynsym table looking for "__libc"
  // The dynsym is bounded by strtab (it immediately follows .dynsym in TCC
  // output)
  uint64_t sym_limit = (strtab_va > symtab_va) ? strtab_va : symtab_va + 0x1000;
  for (uint64_t sym_va = symtab_va; sym_va < sym_limit; sym_va += syment) {
    // Read st_name (first 4 bytes of Elf64_Sym)
    uint64_t phys = vmm_virt_to_phys(pml4, sym_va & ~0xFFFULL);
    if (!phys)
      continue;
    uint8_t *sym = (uint8_t *)(phys + hhdm + (sym_va & 0xFFF));
    uint32_t st_name = *(uint32_t *)sym;
    // st_value is at offset 8 in Elf64_Sym
    uint64_t st_value = *(uint64_t *)(sym + 8);

    if (st_name == 0 || st_value == 0)
      continue;

    // Read the name from strtab
    uint64_t name_va = strtab_va + st_name;
    uint64_t name_phys = vmm_virt_to_phys(pml4, name_va & ~0xFFFULL);
    if (!name_phys)
      continue;
    const char *name = (const char *)(name_phys + hhdm + (name_va & 0xFFF));

    // Check for "__libc" (6 chars + NUL)
    if (name[0] == '_' && name[1] == '_' && name[2] == 'l' && name[3] == 'i' &&
        name[4] == 'b' && name[5] == 'c' && name[6] == '\0') {

      klog_puts("[PROC] Found '__libc' symbol at value: ");
      klog_uint64(st_value);
      klog_puts("\n");

      // Found __libc at st_value. Compute auxv address from the stack.
      // Stack layout at user_sp: argc, argv[0..argc-1], NULL, envp[], NULL,
      // auxv[] Read through HHDM.
      uint64_t sp_phys = vmm_virt_to_phys(pml4, user_sp & ~0xFFFULL);
      if (!sp_phys)
        return;
      uint64_t *sp = (uint64_t *)(sp_phys + hhdm + (user_sp & 0xFFF));
      uint64_t argc_val = sp[0];
      // auxv starts after: argc + argv[argc] + NULL + envp[] + NULL
      uint64_t idx = 1 + argc_val + 1; // skip argc + argv + NULL
      // Skip envp
      while (sp[idx] != 0)
        idx++;
      idx++; // skip envp NULL terminator
      // sp[idx] is now the start of auxv
      uint64_t auxv_user_addr = user_sp + idx * sizeof(uint64_t);

      // Write auxv pointer into __libc + 8
      uint64_t libc_auxv_va = load_base + st_value + 8;

      klog_puts("[PROC] Fixed __libc.auxv at ");
      klog_uint64(libc_auxv_va);
      klog_puts(" -> ");
      klog_uint64(auxv_user_addr);
      klog_puts("\n");

      uint64_t libc_phys = vmm_virt_to_phys(pml4, libc_auxv_va & ~0xFFFULL);
      if (!libc_phys)
        return;
      uint64_t *libc_auxv =
          (uint64_t *)(libc_phys + hhdm + (libc_auxv_va & 0xFFF));
      *libc_auxv = auxv_user_addr;

      return;
    }
  }
}

bool process_exec_argv(const char **argv) {
  if (!argv || !argv[0])
    return false;

  klog_puts("[PROC] Executing main session: ");
  klog_puts(argv[0]);
  klog_puts("\n");

  uint64_t *pml4 = vmm_create_pml4();
  if (!pml4) {
    klog_puts("[PROC] Failed to allocate new PML4\n");
    return false;
  }

  // Switch to the newly created, clean address space
  struct thread *current = sched_get_current();
  if (current && current->mm) {
    current->cr3 = (uint64_t)pml4;
    __asm__ volatile("mov %0, %%cr3" ::"r"(current->cr3) : "memory");

    // Destroy old VMA tree nodes before resetting
    extern void vma_list_destroy(struct vma_list * list);
    vma_list_destroy(&current->mm->vmas);
    mm_reset_mmap_state(current);
  }

  elf_info_t elf_info = {0};

  if (!elf_load(argv[0], pml4, &elf_info)) {
    return false;
  }

  klog_puts("[PROC] ELF mapped successfully. Jumping to Ring 3 (RIP: ");
  klog_uint64(elf_info.entry);
  klog_puts(")\n");

  // Map vsyscall page so HotSpot JVM (and glibc) can access gettimeofday,
  // time, and getcpu at the canonical Linux vsyscall address.
  vmm_map_vsyscall_page(pml4);

  // Store the basename of the executable as the thread's comm name
  {
    struct thread *ct = sched_get_current();
    if (ct) {
      const char *base = argv[0];
      for (const char *p = argv[0]; *p; p++)
        if (*p == '/')
          base = p + 1;
      int ci = 0;
      while (base[ci] && ci < 15) {
        ct->comm[ci] = base[ci];
        ci++;
      }
      ct->comm[ci] = '\0';
    }
  }

  uint64_t user_rsp = process_build_initial_stack(
      ASCENTOS_USER_STACK_TOP, NULL, (const char **)argv, NULL, &elf_info);
  if (!user_rsp) {
    klog_puts("[PROC] Exec failed: could not build initial stack\n");
    return false;
  }

  // Fix up __libc.auxv for binaries whose _start skips __libc_start_main
  // (e.g. TCC-compiled programs). Without this, musl's malloc crashes
  // trying to walk a NULL auxv to find AT_RANDOM.
  process_fixup_libc_auxv(argv[0], pml4, user_rsp, &elf_info);

  // Initialize File Descriptors for the main thread
  struct thread *current_thread = sched_get_current();
  if (current_thread && sched_ensure_files(current_thread)) {
    // 0 = stdin, 1 = stdout, 2 = stderr.
    // First try to get the console from the device registry (preferred)
    vfs_node_t *console_node = fb_lookup_device("console");
    // Fall back to VFS lookup if not in registry
    if (!console_node) {
      console_node = vfs_resolve_path("/dev/console");
    }
    current_thread->fds[0] = console_node;
    current_thread->fds[1] = console_node;
    current_thread->fds[2] = console_node;
    current_thread->files->next_fd = 3;
    current_thread->fd_offsets[0] = 0;
    current_thread->fd_offsets[1] = 0;
    current_thread->fd_offsets[2] = 0;
    fd_path_set(current_thread, 0, "/dev/console");
    fd_path_dup(current_thread, 1, 0);
    fd_path_dup(current_thread, 2, 0);

    for (int i = 3; i < MAX_FDS; i++) {
      current_thread->fds[i] = NULL;
    }
  }

  // Note: removed console_clear() here so the prompt stays at current position

  // Set the TSS rsp0 to the kernel stack for hardware interrupts
  tss_set_rsp0(cpu_get_current()->stack_top);

  // Issue the jump to userspace (does not return normally)
  uint64_t actual_entry =
      elf_info.interp_base ? elf_info.interp_entry : elf_info.entry;
  process_jump_usermode(actual_entry, user_rsp, (uint64_t)pml4);
  return true;
}

// ---- Core Dump System ----------------------------------------------------

#define CORE_DUMP_MAX_BYTES (4U * 1024U * 1024U)

struct core_page_record {
  uint64_t virtual_address;
  uint32_t size;
  uint32_t prot;
};

static bool core_append(vfs_node_t *file, uint32_t *offset, const void *data,
                        uint32_t size) {
  if (!file || !offset || !data || *offset > CORE_DUMP_MAX_BYTES ||
      size > CORE_DUMP_MAX_BYTES - *offset)
    return false;
  if (vfs_write(file, *offset, size, (uint8_t *)data) != size)
    return false;
  *offset += size;
  return true;
}

bool process_core_dump_enabled = false;

static void dump_vma_recursive(struct vma *v, vfs_node_t *file, uint64_t cr3,
                               uint32_t *offset, bool *truncated) {
  if (!v || *truncated)
    return;

  dump_vma_recursive(v->left, file, cr3, offset, truncated);

  // Skip device mappings or guards (PROT_NONE)
  if (v->prot != 0 && !(v->flags & MAP_SHARED)) {
    uint64_t hhdm = pmm_get_hhdm_offset();
    for (uint64_t addr = v->start; addr < v->end; addr += PAGE_SIZE) {
      uint64_t phys = vmm_virt_to_phys((uint64_t *)cr3, addr);
      if (phys != 0 && pmm_is_managed(phys)) {
        struct core_page_record record = {
            .virtual_address = addr,
            .size = PAGE_SIZE,
            .prot = v->prot,
        };
        uint32_t needed = sizeof(record) + PAGE_SIZE;
        if (*offset > CORE_DUMP_MAX_BYTES - needed ||
            !core_append(file, offset, &record, sizeof(record)) ||
            !core_append(file, offset, (void *)(phys + hhdm), PAGE_SIZE)) {
          *truncated = true;
          break;
        }
      }
    }
  }

  dump_vma_recursive(v->right, file, cr3, offset, truncated);
}

void process_dump_core(struct thread *t, struct registers *regs, int sig) {
  if (!process_core_dump_enabled || !t || !t->mm)
    return;

  char path[64];
  strcpy(path, "/tmp/core.");
  char tid_str[16];
  // Simple itoa for tid
  uint32_t val = t->tid;
  int i = 0;
  if (val == 0)
    tid_str[i++] = '0';
  else {
    while (val > 0) {
      tid_str[i++] = (val % 10) + '0';
      val /= 10;
    }
  }
  tid_str[i] = '\0';
  // Reverse tid_str
  for (int j = 0; j < i / 2; j++) {
    char tmp = tid_str[j];
    tid_str[j] = tid_str[i - j - 1];
    tid_str[i - j - 1] = tmp;
  }
  strcat(path, tid_str);

  klog_puts("[CORE] Dumping to ");
  klog_puts(path);
  klog_puts("...\n");

  vfs_node_t *tmp_dir = vfs_resolve_path("/tmp");
  if (!tmp_dir)
    return;

  if (vfs_create(tmp_dir, &path[5], 0644) != 0) {
    klog_puts("[CORE] Failed to create core file\n");
    vfs_close(tmp_dir);
    return;
  }
  vfs_close(tmp_dir);

  vfs_node_t *file = vfs_resolve_path(path);
  if (!file)
    return;

  uint32_t file_offset = 0;
  bool truncated = false;

  // 1. Write Header / Metadata (Simplified)
  struct {
    uint32_t magic;
    uint32_t sig;
    uint32_t tid;
    char comm[16];
  } header;
  header.magic = 0x45524F43; // "CORE"
  header.sig = (uint32_t)sig;
  header.tid = t->tid;
  memcpy(header.comm, t->comm, 16);
  if (!core_append(file, &file_offset, &header, sizeof(header))) {
    truncated = true;
    goto out;
  }

  // 2. Write Register State
  if (!core_append(file, &file_offset, regs, sizeof(struct registers))) {
    truncated = true;
    goto out;
  }

  // 3. Write Memory Regions
  // Store compact page records sequentially. Never use virtual addresses as
  // ramfs offsets: high addresses overflow its 32-bit growth calculation.
  dump_vma_recursive(t->mm->vmas.root, file, t->cr3, &file_offset, &truncated);

out:
  vfs_close(file);
  if (truncated)
    klog_puts("[CORE] Dump truncated at 4 MiB\n");
  klog_puts("[CORE] Dump complete.\n");
}
