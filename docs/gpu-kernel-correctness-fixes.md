# Kernel correctness fixes during GPU corruption investigation

Two additional defects were found and corrected on 2026-10-13. Neither has
yet been demonstrated to cause the reported Minecraft/tunnel green pixels.

`kernel/linuxkpi/src/page.c:free_pages()` shifted the supplied kernel virtual
address directly into a PFN. `__get_free_pages()` returns a direct-map/HHDM
virtual address, so `pfn_to_page()` rejected it and `__free_pages(NULL)` did
nothing. The fix subtracts the HHDM offset before obtaining the descriptor.
Upstream AMD KFD uses this API for signal and CWSR backing allocations. This
establishes a leak in that API; it does not establish that the game exercises
those KFD paths.

`kernel/src/linuxkpi/fpu.c:kernel_fpu_begin()` selected the per-CPU XSAVE area
before disabling interrupts. Preemption/migration in that interval could
save into the wrong CPU's buffer; `kernel_fpu_end()` then used the actual CPU's
entry. Entry now disables interrupts before any per-CPU lookup or nesting
check. Nested calls still leave interrupt restoration to the outermost end.
AMD display code calls this interface. Its involvement in the render-node
artifact remains unproven.

`scripts/tests/test_kpi_gpu_kernel_correctness.py` compiles the real functions
with mocked PMM and CPU hardware. It verifies 2,000 direct-map frees including
physical RAM above 4 GiB, 2,000 nested FPU sections with migration injected
at an unprotected CPU lookup, full register restoration, and original IRQ
state restoration. Both original implementations fail the same checks. The
existing lazy FPU test also passes its 10,000 integer context switches and
ownership/migration/exec checks.

Existing cache-alias/rollback and MMIO ordering regressions also pass. The
broader PMM reference harness could not compile because its mocks/declarations
do not cover the current zero-pool helpers (`pmm_zero_pool_drain_all`,
`pmm_zero_pool_total_cached`, `page_zero`, and `pmm_get_hhdm_offset`). No native
PMM implementation was changed by these fixes; that harness was not repaired
or counted as a passing check.

The kernel build passed. The default `avoryos-x86_64.iso` was replaced
atomically with the new kernel, preserving its existing Limine configuration.
The embedded stripped kernel checksum was verified. Candidate and backup
are `build/gpu-diagnostics/avoryos-gpu-correctness-x86_64.iso` and
`build/gpu-diagnostics/avoryos-before-gpu-correctness.iso`. The running VM
still uses its old ISO file descriptor until QEMU is fully restarted.
`disk.img` was not modified while in use.

The latest guest trace replay reported **llvmpipe**, after `DRI_PRIME=1` picked
the virtual device and failed to load `virtio_gpu`. Those screenshots cannot
validate AMD rendering. The optional replay helper now requests the verified
AMD guest PCI address explicitly (`pci-0000_00_03_0`); actual renderer logging
remains enabled. The immediate next check is the original affected demo under
the updated kernel, rather than another synthetic test series.
