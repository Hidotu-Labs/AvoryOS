/* AvoryOS LinuxKPI kernel configuration.
 *
 * This file is checked in and maintained by hand.  AvoryOS does not run
 * Kconfig: imported Linux code sees only the symbols defined here, and a
 * symbol that is absent is "disabled" (Linux's IS_ENABLED() treats an
 * undefined CONFIG as 0, so disabled symbols must NOT be defined as 0).
 *
 * Keep the set minimal.  Every symbol should be justified by an imported
 * file that reads it.  Phase 0 only needs enough for leaf library files;
 * later phases grow this file (DRM, TTM, amdgpu, ...).
 *
 * Deliberately absent:
 *   CONFIG_MODULES          no module loader; everything is linked in
 *   CONFIG_PM*              no runtime/system power management yet
 *   CONFIG_ACPI             native ACPI only has MADT/FADT/MCFG/HPET
 *   CONFIG_IOMMU_SUPPORT    DMA is identity-mapped on x86 for now
 *   CONFIG_DEBUG_FS         no debugfs yet
 *   CONFIG_TRACEPOINTS      tracepoints compile to no-ops
 *   CONFIG_JUMP_LABEL       static branches fall back to atomics
 *   CONFIG_HAVE_STATIC_CALL static calls fall back to indirect calls
 *   CONFIG_KASAN/KCSAN/KMSAN, CONFIG_MEMCG, CONFIG_SLUB/SLAB
 */
#ifndef __AVORY_LINUXKPI_AUTOCONF_H
#define __AVORY_LINUXKPI_AUTOCONF_H

/* Architecture. */
#define CONFIG_X86 1
#define CONFIG_X86_64 1
#define CONFIG_64BIT 1
#define CONFIG_MMU 1
#define CONFIG_SMP 1

/* Pointer-sized physical and DMA addresses.  Without these, Linux's types.h
 * falls back to typedef'ing phys_addr_t/dma_addr_t as u32 (x86_64 selects
 * both symbols in Kconfig).  A missing PHYS_ADDR_T_64BIT silently truncates
 * page_to_phys() results to 32 bits. */
#define CONFIG_PHYS_ADDR_T_64BIT 1
#define CONFIG_ARCH_DMA_ADDR_T_64BIT 1

/* x86_64 selects this, which makes <linux/mem_encrypt.h> pull in
 * <asm/mem_encrypt.h> (__sme_pa/__sme_va).  asm/processor.h's load_cr3()
 * needs __sme_pa; MEM_ENCRYPT itself stays off. */
#define CONFIG_ARCH_HAS_MEM_ENCRYPT 1

/* Architected page-table geometry: 4-level x86_64 (matches the native VMM). */
#define CONFIG_PGTABLE_LEVELS 4

/* The direct map base is AvoryOS's runtime HHDM offset, not Linux's compile
 * time 0xffff888000000000: this makes upstream's __pa()/__va() and the
 * virt_to_page() macro in asm/page.h agree with pmm_get_hhdm_offset().
 * page.c defines page_offset_base and pins it at page_init(). */
#define CONFIG_DYNAMIC_MEMORY_LAYOUT 1

/* x86 cache geometry: 64-byte lines on every x86_64 machine; the internode
 * shift matches what Linux uses for non-VSMP x86_64. */
#define CONFIG_X86_L1_CACHE_SHIFT 6
#define CONFIG_X86_INTERNODE_CACHE_SHIFT 6

/* Core kernel. */
#define CONFIG_BUG 1
#define CONFIG_BASE_SMALL 0
#define CONFIG_GENERIC_BUG 1
#define CONFIG_GENERIC_BUG_RELATIVE_POINTERS 1
/* WARN/BUG entries carry file:line; the native decoder in
 * kernel/src/cpu/bug_table.c matches that 12-byte layout. */
#define CONFIG_DEBUG_BUGVERBOSE 1
#define CONFIG_PRINTK 1
#define CONFIG_HZ 1000
#define CONFIG_HZ_1000 1

/* Debugging poison value for bad kernel pointers (x86_64 default). */
#define CONFIG_ILLEGAL_POINTER_VALUE 0xdead000000000000

/* Buses and drivers. */
#define CONFIG_PCI 1
#define CONFIG_PCI_MSI 1

/* Graphics (Phase 3+). */
#define CONFIG_DRM 1
#define CONFIG_DRM_KMS_HELPER 1
#define CONFIG_DMA_SHARED_BUFFER 1
#define CONFIG_SYNC_FILE 1
#define CONFIG_DRM_GEM_SHMEM_HELPER 1
#define CONFIG_DRM_VGEM 1
#define CONFIG_DRM_VKMS 1
#define CONFIG_DRM_SIMPLEDRM 1
#define CONFIG_CRC32 1

/* Allocators used by imported library code. */
#define CONFIG_GENERIC_ALLOCATOR 1

/* Phase 4: TTM and the GPU scheduler.  DRM_VRAM_HELPER is the 6.6 name for
 * the VRAM GEM helper (drm_gem_vram_helper.c); bochs selects it together with
 * TTM and TTM_HELPER.  CONFIG_DRM_BOCHS is the C5 TTM canary. */
#define CONFIG_DRM_TTM 1
#define CONFIG_DRM_TTM_HELPER 1
#define CONFIG_DRM_VRAM_HELPER 1
#define CONFIG_DRM_SCHED 1
#define CONFIG_DRM_BOCHS 1

/* Phase 5 C3: the real firmware loader declarations in <linux/firmware.h>
 * (request_firmware/release_firmware) instead of the !CONFIG_FW_LOADER inline
 * stubs.  Implementation: linuxkpi/src/firmware.c over the native VFS. */
#define CONFIG_FW_LOADER 1

/* Phase 5 C5: x86 always selects HAS_IOMEM.  With it set, stock
 * <linux/platform_device.h> declares devm_platform_ioremap_resource() extern
 * (implemented in linuxkpi/src/platform.c) instead of the -EINVAL inline. */
#define CONFIG_HAS_IOMEM 1

/* Phase 6 C1: amdgpu and the DRM helper libraries its Kconfig selects.
 *
 * The object list is generated from the pinned 6.6 Makefiles by
 * scripts/linux/kbuild-subset.py; these symbols control both the generated
 * list and the #if arms in the sources.  Deliberately absent (see
 * docs/linuxkpi-gaps.md P6 C1): HWMON (weak stubs), POWER_SUPPLY (stock
 * stub), INTERVAL_TREE (amdgpu_vm.c defines its own), PROC_FS, PERF_EVENTS,
 * COMPAT, VGA_SWITCHEROO, HSA_AMD, DRM_AMD_ACP, DRM_AMDGPU_SI/CIK,
 * HMM_MIRROR, DRM_AMD_SECURE_DISPLAY, DRM_FBDEV_EMULATION, DRM_DP_AUX_BUS. */
#define CONFIG_DRM_AMDGPU 1
#define CONFIG_DRM_AMD_DC 1
#define CONFIG_DRM_AMD_DC_FP 1
#define CONFIG_DRM_DISPLAY_HELPER 1
#define CONFIG_DRM_DISPLAY_DP_HELPER 1
#define CONFIG_DRM_DISPLAY_HDMI_HELPER 1
#define CONFIG_DRM_DISPLAY_HDCP_HELPER 1
#define CONFIG_DRM_BUDDY 1
#define CONFIG_DRM_EXEC 1
#define CONFIG_DRM_SUBALLOC_HELPER 1
#define CONFIG_I2C_ALGOBIT 1

/* Phase 6 C2: amdgpu_irq.c uses the IRQ domain API unconditionally, and x86
 * provides arch_thread_struct_whitelist() (so the stock sched/task.h fallback
 * must stay disabled). */
#define CONFIG_IRQ_DOMAIN 1
#define CONFIG_HAVE_ARCH_THREAD_STRUCT_WHITELIST 1

/* The kernel boots up to 4 CPUs (tests run at -smp 4); stock headers size
 * cpumasks and the percpu offset array from this. */
#define CONFIG_NR_CPUS 4

#endif /* __AVORY_LINUXKPI_AUTOCONF_H */
