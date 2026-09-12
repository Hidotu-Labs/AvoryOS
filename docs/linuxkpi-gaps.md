# LinuxKPI gap log

Missing, stubbed, or deliberately divergent Linux APIs, as encountered while
compiling/running upstream code.  Every entry names the imported file that
needed it, the current workaround, and what a complete implementation needs.

Update this file in the same change that introduces or closes a gap.

## Phase 4 gaps (TTM + scheduler bring-up, 2026-09-12)

- **`current` is the `task_struct` shadow everywhere**
  (`linuxkpi/include/asm/current.h`): the overlay used to cast the native
  `struct thread *` to `task_struct *`, while `sched.h` returned the shadow.
  Imported code that dereferences `current->…` (`drm_sched`'s
  `last_user`/`group_leader`) and dma-fence's `cb.task = current` therefore
  operated on a native thread (whose first field is `rsp`), so
  `wake_up_state()` woke a garbage pointer and every fence wait timed out.
  Both definitions now return `linuxkpi_current_task()`.
- **`timer_setup()` memsets the timer** (`linuxkpi/include/linux/timer.h`):
  the overlay did not initialize `running`, so an `INIT_DELAYED_WORK()` on
  stack/heap memory that was not already zero (e.g. `drm_sched_init()` on a
  stack `drm_gpu_scheduler`) made `del_timer_sync()`/`cancel_delayed_work_sync()`
  wait forever on a stale `running=1`.  Stock `__init_timer()` memsets; the
  overlay now does too.
- **`schedule_timeout()` has a pending-wake rendezvous**
  (`linux/src/linuxkpi/native_sched.c`, `kpi_wake_pending`/`kpi_timeout_active`
  in `kernel/src/sched/sched.h`): `sched_wakeup()` ignores running threads, so
  a wake that arrived after the waiter committed to sleeping but before it
  set `THREAD_SLEEPING` was lost and `schedule_timeout()` returned 0 at its
  deadline.  `linuxkpi_wake_thread()` now arms a per-thread pending flag while
  a KPI timeout is active; the sleeper consumes it under the run-queue lock.
  Waitqueue and kthread-start wakes are deliberately excluded (arming them
  made every later `msleep` return early).
- **`struct rb_node` layout matched to stock Linux** (`kernel/src/lib/rbtree.h`,
  C3): the native struct had `rb_left` before `rb_right` while stock
  `<linux/rbtree_types.h>` has `rb_right` first.  Imported code embeds the
  stock layout but calls native `rb_insert_color`/`rb_next`/`rb_first`, so
  every imported rbtree silently corrupted itself.  The first real user was
  drm_sched's FIFO run queue (`rb_add_cached` in `drm_sched_rq_update_fifo`):
  entities were never selected, so jobs sat unsignaled.  Lesson for future
  imports: any type whose operations are shared between native and imported
  code must have an identical layout; putting the operations in a renamed
  native namespace (`asc_*`, as done for radix-tree) is the alternative.
- **Imported `WARN()`/`BUG()` are decoded from `__bug_table`**
  (`kernel/src/cpu/bug_table.c`, hook in `kernel/src/cpu/isr.c`): stock
  `<asm/bug.h>` emits `ud2` plus a table entry; the native invalid-opcode
  handler had no decoder, so any imported WARN panicked.  WARNs now print
  `WARNING: at file:line (imported WARN)` and resume at RIP+2;
  `BUG()`/genuine bad opcodes still panic.  This required
  `CONFIG_DEBUG_BUGVERBOSE` in `autoconf.h` (12-byte entries with file:line;
  without it entries are 8 bytes and the decoder would not match).
  `BUGFLAG_ONCE` is honoured with a 64-entry address registry; the table
  lives in `.rodata`, which is not written to.
- **`ww_mutex_trylock()` returns 1/0, not 0/-EBUSY** (see the corrected
  Phase 3 entry above).  Found by the first C2 boot: TTM's
  `WARN_ON(!dma_resv_trylock())` fired on every successful trylock.
- **Page cache-mode API is inert** (`linuxkpi/src/x86_stubs.c`): TTM calls
  `set_pages_wb()`, `set_pages_array_uc()`, `set_pages_array_wc()`,
  `set_pages_array_wb()`, `cachemode2protval()` and `pgprot_writecombine()`.
  All RAM is HHDM write-back and AvoryOS has no PAT/MTRR programming, so the
  cache flips are accepted and ignored; `cachemode2protval()` returns the
  correct x86 PAT bits (so the pgprot values are well formed) but the kmap
  paths that consume them are plain HHDM lookups.  `clear_page_orig/rep/erms`
  are `memset()` over the HHDM (`clear_page()` in stock x86 `page_64.h`
  references them through the alternatives machinery).
- **`struct page::private` is `unsigned long`**, matching upstream (it was
  `void *` through Phase 3).  TTM stores an allocation order and a helper
  pointer there; only `page.c` wrote the field before, and it now writes 0.
- **`<linux/highmem.h>` includes `<linux/mm.h>`** again, as upstream does.
  The Phase 2 overlay had the dependency reversed, which hid `PFN_UP`,
  `struct shrinker`, `fault_flag_allow_retry_first()` and friends from TUs
  such as TTM's `ttm_pool.c`.  `mm.h` now also pulls `<linux/pfn.h>`,
  `<linux/shrinker.h>` and `<linux/mmap_lock.h>` like upstream.
- **`<linux/mmap_lock.h>` is a no-op overlay**: `mmap_read_lock/unlock`,
  `mmap_write_*`, trylock and assert variants are inert inlines.  AvoryOS
  keeps VMA state under native locks and nothing walks a Linux mm; TTM's
  fault path uses `mmap_read_unlock()` around `dma_resv` waits.
- **`pagefault_disable/enable()` and `migrate_disable/enable()` are no-ops**
  (`linuxkpi/include/linux/{uaccess,preempt}.h`): there is no highmem and no
  page migration, and the stock `<linux/io-mapping.h>` inlines call them.
- **`drain_workqueue()` equals `flush_workqueue()`**
  (`linuxkpi/src/workqueue.c`): upstream's "draining" state that blocks new
  submissions does not exist; the call waits for pending/running work only.
- **`struct mm_struct` is still a placeholder** but now carries `pgd`,
  `init_mm` is a zeroed instance, and `swp_entry_t`/`enum fault_flag` are
  defined in the `mm_types.h` overlay because stock `<linux/pgtable.h>` needs
  them for its inline helpers.  No imported code walks an mm.
- **`task_struct::group_leader` is the task itself and `exit_code` stays 0**
  (`linuxkpi/src/task.c`): there is no shared `signal_struct`, so per-process
  identity is approximated by per-thread identity.  `drm_sched` uses it only
  for its per-user submission bookkeeping and compares it against itself;
  `SIGKILL` now comes from `<uapi/linux/signal.h>` through the `sched.h`
  overlay.
- **`shmem_read_mapping_page_gfp()` implemented** in `linuxkpi/src/shmem.c`
  (page-returning wrapper over the existing folio store); TTM's
  shmem-backed `ttm_tt` path uses it.  The `shmem_fs.h` overlay now declares
  it next to the folio form.

## Deliberate divergences

- **`struct page`** (`linuxkpi/include/linux/mm_types.h`): trimmed layout with
  an extra `pfn` field; no fields/flags area, no folio embedding.  `PageTail`
  is an AvoryOS-only flag bit backed by a plain `compound_head` pointer.
  `linuxkpi/include/linux/page-flags.h` is a compact overlay, not upstream's.
- **`asm-generic/memory_model.h`** is shadowed with an empty file:
  `pfn_to_page()`/`page_to_pfn()` are real functions over AvoryOS's sparse
  mem_map instead of the FLATMEM/SPARSEMEM macros.
- **`struct page::_refcount` is authoritative** for pages handed to Linux
  code; the native PMM reference is dropped exactly once when it reaches zero.
  Native CoW/refcount paths must not touch such pages (documented in page.c).
- **`struct vm_area_struct`** is a Linux-facing view allocated by the mmap
  bridge; the native `struct vma` remains the authoritative mapping record.

## Configuration gaps

- `CONFIG_PHYS_ADDR_T_64BIT` / `CONFIG_ARCH_DMA_ADDR_T_64BIT` were missing
  from `linuxkpi/include/generated/autoconf.h`.  Without them
  `<linux/types.h>` typedefs `phys_addr_t`/`dma_addr_t` as u32, and GCC
  compiled `page_to_phys()` with a 32-bit shift that truncated every
  physical address above 4 GB.  Fixed in Phase 2 (found by the page
  roundtrip self-test).
- `CONFIG_DYNAMIC_MEMORY_LAYOUT` added so upstream `__pa()`/`__va()` use
  `page_offset_base`, which AvoryOS pins to the runtime HHDM offset.

## Phase 3 gaps (DRM core bring-up, 2026-09-12)

- **shmem backing is a shim, not tmpfs** (`linuxkpi/src/shmem.c`):
  `shmem_file_setup()` returns a bridged kernel file whose address_space is an
  xarray of zeroed order-0 PMM pages; `shmem_read_folio_gfp()`,
  `shmem_truncate_range()` and `invalidate_mapping_pages()` operate on that.
  No swap, no reclaim, no `shmem_read_mapping_page_gfp` page-cache semantics
  beyond allocate/lookup/free.
- **`struct folio` is order-0 only** (`linuxkpi/include/linux/mm_types.h`,
  `page-flags.h`, `pagevec.h`): a folio wraps one page; `folio_batch` is the
  upstream layout with a no-op unevictable move.
- **kobject/sysfs is dynamic for class devices** (`linuxkpi/src/kobject.c` +
  `kernel/src/fs/sysfs.c`): `class_create()` creates `/sys/class/<name>`;
  `device_add()`/`device_del()` create/remove
  `/sys/class/<class>/<dev-name>`; `device_add_groups()` materializes
  `struct device_attribute` files with real show/store (and honours
  `is_visible`); `class_create_file()` materializes class attributes (DRM's
  `version`).  Still inert: `sysfs_create_file()` on a non-device kobject
  (needs `ktype->sysfs_ops`), `bin_attribute`s (EDID), symlink creation and
  uevent broadcast.  `device_add()` also wires `kobj.parent`/`kobj.name` for
  device-hierarchy reads.  The per-attribute show/store contexts are not
  freed on removal.
- **Dynamic devnode registry is a fixed table** (`linuxkpi/src/native_vfs.c`,
  `KPI_DEVNODE_REGISTRY_MAX 32`): nested devnodes below /dev are matched by
  dir/leaf name, enumerated by the owning directory's readdir, and removed by
  `device_del()`; `device_add()`'s devnode hook registers DRM minors.  No
  refcounting beyond the metadata node's persistent reference; each open
  descriptor owns its own per-open node.
- **Platform bus is synchronous and minimal** (`linuxkpi/src/platform.c`):
  name-match only, no deferred probe, no OF/ACPI enumeration, no refcounts.
- **`register_chrdev()` is a successful no-op** (`drm_compat.c`): the native
  devfs owns /dev nodes; DRM major 226 has no registry.  `file_clone_open()`
  returns -ENODEV.
- **`struct task_struct` is a per-thread shadow** (`linuxkpi/src/task.c`) with
  `kpi_thread` first; `task_tgid()` returns an opaque token built from the
  native tgid, not a `struct pid`.  Shadows are leaked with their thread until
  a thread-exit hook exists.
- **Initcalls run in a kthread with a bounded wait**
  (`linuxkpi/src/initcalls.c`): the walker itself stays native
  (`kernel/src/linuxkpi/init.c`), but it is invoked from a `kpi/initcalls`
  thread like Linux's `kernel_init`, capped at 30 s.  A wedged initcall is
  reported and boot continues, so a partially initialized driver must still
  fail safely at use time.
- **i2c/regulator/component/panel-quirk/aperture stubs** (`drm_compat.c`,
  `link_stubs.c`): EDID DDC returns -EIO, regulators are absent, component
  add/del are no-ops, panel orientation quirk is UNKNOWN.  `_printk` now
  routes imported messages through `vklogf` (512-byte line buffer);
  `kvasprintf` truncates at 512 bytes.
- **`ksize()` is real** (`linuxkpi/src/slab.c` → `heap_ksize()` in
  `kernel/src/mm/heap.c`): slab allocations report their cache object size and
  big allocations their page-payload capacity, so DRM's
  `drmm_add_final_kfree()` capacity check can pass.  The exact requested size
  is still not tracked, only the usable capacity (same contract as upstream).
- **`__sw_hweight32/64` are hand-written asm**
  (`kernel/src/arch/x86_64/hweight.asm`): imported x86 `hweight*()` emits a
  bare `call __sw_hweight*` with an empty clobber list and relies on the
  upstream register-preserving convention; a C implementation silently
  corrupts live caller registers.
- **ww_mutex cannot wound** (`linuxkpi/src/ww_mutex.c`): recursive
  acquisition with the same acquire context returns `-EALREADY` (as
  upstream, which DRM's `modeset_lock()` treats as success), but multi-lock
  acquire sequences still cannot back off under contention; they rely on the
  plain mutex FIFO.  **Correction (C2, 2026-09-12):** `ww_mutex_trylock()`
  must return `1` on success and `0` when busy, not `0`/`-EBUSY`: upstream
  `kernel/locking/mutex.c` and the bool casts in `<linux/dma-resv.h>` /
  `drm_modeset_lock.c` all rely on the truthy convention.  The Phase 3
  "0/-EBUSY convention" note was wrong and caused
  `WARN_ON(!dma_resv_trylock())` to fire in TTM's BO init.
- **rbtree augmented internals are an excerpt** (`linuxkpi/src/rbtree_aug.c`,
  verbatim upstream `lib/rbtree.c`): native `kernel/src/lib/rbtree.c` still
  owns the base `rb_*` API; importing upstream `lib/rbtree.c` requires
  renaming the native symbols first.

## Phase 2 gaps

- **File/VFS bridge is single-level and shim-shaped** (`linuxkpi/src/file.c` +
  `kernel/src/linuxkpi/native_vfs.c`).  `struct file` owns a native `vfs_node_t`
  (`f_asc_node`) and each node's `device` points back at the file.  There is no
  inode/dentry cache, no mount tree and no fs open/release dispatch beyond the
  pseudo-fs path dma-buf uses (`alloc_file_pseudo` + dentry `d_release`).
  `fget()` resolves a descriptor through the native fd table, so only fds
  installed by `fd_install()` are visible; kernel-internal files without an fd
  are not findable.  fd-owned nodes are released by the vfs when the descriptor
  closes; kernel-internal files (shmem backing files, etc.) release their node
  from `kpi_file_free()` (`asc_vfs_node_release_kernel()`), and
  `linuxkpi_file_close()` clears `f_asc_node` before `fput()` so the fd path
  cannot release it twice.  Drivers that expect `fget_raw`/`fdget` to see
  kernel fds or that keep a `struct file` beyond its node's lifetime need this
  to grow.
- **`poll` bridging wakes event-driven consumers** (`linuxkpi_file_poll`):
  the poll table's qproc records the file's native wait queue in the Linux
  `wait_queue_head`, and `__kpi_wake_up()` calls `linuxkpi_wake_poll_queue()`
  so a device's `poll_wait()`/`wake_up()` pair reaches `sys_poll()` waiters.
  Limits: one native queue is recorded per `wait_queue_head` (the last poller
  wins if several files poll the same head), and a head can retain a stale
  queue pointer if its file closes while another file still polls that head.
  The Phase 3 DRM bridge is the first user; the vkms `PAGE_FLIP_EVENT` path
  exercises the wake end to end (atomic commit -> `drm_poll()` POLLIN ->
  `drm_read()`).
- **`unmap_mapping_range()` is a no-op stub** (`linuxkpi/src/mmap.c`).  It
  ignores the address_space because the native VMA tree, not the page cache,
  owns PTE teardown.  TTM's `ttm_bo_vm` will call it when buffers are
  invalidated; until it walks every process mapping, userspace can keep
  touching stale BOs after a move.  `struct inode` now carries an `i_mapping`
  (`alloc_anon_inode()` allocates an address_space), but the mapping walk
  itself is still unimplemented.
- **`mmu_notifier` is a type-only overlay**
  (`linuxkpi/include/linux/mmu_notifier.h`): `CONFIG_MMU_NOTIFIER` is not set
  in `autoconf.h`, so imported code takes the `#else` arms.  dma-resv.c only
  includes the header for `struct mmu_notifier_range`; TTM (v6.6) does not use
  the API.  HMM/userptr-style users will need real register/release hooks.
- **VM split/remap paths drop the Linux wrapper** (`kernel/src/mm/vma.c`):
  `vma_mprotect()`, `sys_mremap` and the grows-down path re-add native VMAs
  with plain `vma_add()`, so only the sys_mmap/teardown/clone paths call
  `vma_attach_linux()`.  A dma-buf mapping that is mprotected or mremapped can
  therefore lose its `vm_ops` (close fires via the old wrapper reference while
  the new VMA has none).  Phase 6/7 (amdgpu VM) must route every re-add through
  `vma_attach_linux()` to preserve exactly-once close semantics.
- **Shrinker API is inert** (`linuxkpi/src/shrinker.c`): registration succeeds
  and the callback is never invoked, so TTM's pool never proactively evicts
  under memory pressure.  Reclaim currently has no shrinker caller at all; the
  native PMM does not run the Linux reclaimer.
- **`vm_ops->fault()` bridge installs PTEs on demand** but only for VMAs created
  through the Linux mmap bridge; `vmf_insert_page()` uses the native PMM's
  mapping flags and takes a page reference, while `remap_pfn_range()` does not
  (pfn mappings stay the exporter's responsibility).  page_mkwrite/COW and
  `VM_PFNMAP`-style tracking (`vm_normal_page`) are not modeled.
