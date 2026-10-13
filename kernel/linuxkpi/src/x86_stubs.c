/* x86 arch entry points that imported Linux code reaches through the stock
 * x86 headers, but whose upstream implementations live in files AvoryOS does
 * not build (arch/x86/mm/pat/memtype.c, arch/x86/lib/clear_page_64.S).
 *
 * Native PAT entry 7 is WC; entry 3 is UC and entry 0 is WB. TTM pool
 * transitions update the HHDM and track the effective type for CPU aliases.
 *   - clear_page()'s alternatives target memset over the HHDM.
 *
 * Recorded in docs/linuxkpi-gaps.md (Phase 4). */

#include <asm/page.h>
#include <asm/processor.h>
#include <asm/pgtable_types.h>
#include <asm/set_memory.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linuxkpi/native_mm.h>
#include <linuxkpi/gpu_diag.h>
#include <linux/irqflags.h>
#include <linux/percpu.h>

#include <linuxkpi/log.h>

unsigned long cachemode2protval(enum page_cache_mode pcm) {
  switch (pcm) {
    case _PAGE_CACHE_MODE_UC:
      return _PAGE_PCD | _PAGE_PWT;
    case _PAGE_CACHE_MODE_UC_MINUS:
      return _PAGE_PCD;
    case _PAGE_CACHE_MODE_WC:
      return ASC_PAGE_PAT | ASC_PAGE_PCD | ASC_PAGE_PWT;
    case _PAGE_CACHE_MODE_WP:
      return _PAGE_PCD | _PAGE_PWT;
    case _PAGE_CACHE_MODE_WB:
    default:
      return 0;
  }
}

pgprot_t pgprot_writecombine(pgprot_t prot) {
  return __pgprot((pgprot_val(prot) &
                    ~(ASC_PAGE_PAT | ASC_PAGE_PCD | ASC_PAGE_PWT)) |
                   cachemode2protval(_PAGE_CACHE_MODE_WC));
}

void clear_page_orig(void *page) {
  memset(page, 0, PAGE_SIZE);
  /* HHDM alias may be WC after a TTM pool transition; drain WC buffers so a
   * GPU sampling the page does not read stale texels. WB callers only pay an
   * sfence. Matches native WC-copy callers (fb_draw, drm, page_ops.asm). */
  __asm__ volatile("sfence" ::: "memory");
}
void clear_page_rep(void *page) {
  memset(page, 0, PAGE_SIZE);
  __asm__ volatile("sfence" ::: "memory");
}
void clear_page_erms(void *page) {
  memset(page, 0, PAGE_SIZE);
  __asm__ volatile("sfence" ::: "memory");
}

unsigned long linuxkpi_page_cache_flags(__UINT64_TYPE__ phys) {
  struct page *page = pfn_to_page(phys >> PAGE_SHIFT);
  return page ? __atomic_load_n(&page->cache_flags, __ATOMIC_ACQUIRE) : 0;
}

static int kpi_set_page_cache(struct page *page, unsigned long flags) {
  uint64_t phys = page_to_phys(page);
  unsigned long old=__atomic_load_n(&page->cache_flags,__ATOMIC_ACQUIRE);
  if (!asc_pmm_is_managed(phys))
    return -EINVAL;
  if (__atomic_load_n(&page->cache_flags, __ATOMIC_ACQUIRE) == flags)
    return 0;
  uint64_t addr = phys + asc_pmm_get_hhdm_offset();
  /* Pool pages are not in use during a caching transition. CLFLUSH writes
   * back and invalidates their cache lines on the coherent CPU fabric before
   * the mapping changes; MFENCE also drains preceding WC stores. */
  __asm__ volatile("mfence" ::: "memory");
  for (unsigned int i = 0; i < PAGE_SIZE; i += 64)
    __asm__ volatile("clflush (%0)" :: "r"(addr + i) : "memory");
  __asm__ volatile("mfence" ::: "memory");
  if (!asc_vmm_map_page(asc_vmm_get_kernel_pml4(), addr, phys,
                        ASC_PAGE_PRESENT | ASC_PAGE_RW | ASC_PAGE_NX | flags))
    return -ENOMEM;
  /* The native VMM invalidates shared kernel translations on every CPU. */
  __atomic_store_n(&page->cache_flags, flags, __ATOMIC_RELEASE);
  gpu_diag_record(GPU_DIAG_CACHE,page,phys,old,flags,irqs_disabled(),0);
  gpu_diag_cache_check(page,phys,flags);
  return 0;
}

static int kpi_set_pages_cache(struct page **pages, int count,
                               unsigned long flags) {
  if (count < 0 || (count && !pages))
    return -EINVAL;
  if (!count)
    return 0;
  unsigned int *old = kmalloc_array(count, sizeof(*old), GFP_KERNEL);
  if (!old)
    return -ENOMEM;
  int ret = 0, i;
  for (i = 0; i < count; i++) {
    if (!pages[i] || !asc_pmm_is_managed(page_to_phys(pages[i]))) {
      ret = -EINVAL;
      goto out;
    }
    old[i] = __atomic_load_n(&pages[i]->cache_flags, __ATOMIC_ACQUIRE);
  }
  for (i = 0; i < count; i++) {
    ret = kpi_set_page_cache(pages[i], flags);
    if (ret) {
      while (i-- > 0)
        kpi_set_page_cache(pages[i], old[i]);
      break;
    }
  }
out:
  kfree(old);
  return ret;
}

int set_pages_wb(struct page *page, int numpages) {
  if (!page || numpages < 0)
    return -EINVAL;
  for (int i = 0; i < numpages; i++) {
    struct page *p = pfn_to_page(page_to_pfn(page) + i);
    if (!p)
      return -EINVAL;
    int ret = kpi_set_page_cache(p, 0);
    if (ret)
      return ret;
  }
  return 0;
}

int set_pages_array_uc(struct page **pages, int count) {
  return kpi_set_pages_cache(pages, count, ASC_PAGE_PCD | ASC_PAGE_PWT);
}
int set_pages_array_wc(struct page **pages, int count) {
  return kpi_set_pages_cache(pages, count,
                            ASC_PAGE_PAT | ASC_PAGE_PCD | ASC_PAGE_PWT);
}
int set_pages_array_wb(struct page **pages, int count) {
  return kpi_set_pages_cache(pages, count, 0);
}

DEFINE_PER_CPU(struct cpuinfo_x86, cpu_info);

/* ── boot CPU feature record ──────────────────────────────────────────────
 *
 * Imported code's boot_cpu_has() reads boot_cpu_data.x86_capability directly
 * (the record itself is defined in link_stubs.c).  Leaving it zeroed answers
 * "no" to every feature; the Phase 6 C4 PSP bring-up proved that matters:
 * amdgpu's is_virtual_machine() (X86_FEATURE_HYPERVISOR) then missed VFIO
 * passthrough, so AMDGPU_PASSTHROUGH_MODE stayed unset, the APU skipped its
 * HDP flushes and used the MC-framebuffer aperture instead of BAR0, and the
 * PSP never consumed the ring (fence stayed 0 -> "command UNKNOWN CMD(0x0)").
 *
 * Fill the words that map 1:1 to CPUID leaves, mirroring
 * arch/x86/kernel/cpu/common.c (word numbers from cpufeatures.h):
 *   word 0  CPUID.0x00000001:EDX
 *   word 1  CPUID.0x80000001:EDX
 *   word 4  CPUID.0x00000001:ECX  (bit 31 = X86_FEATURE_HYPERVISOR)
 *   word 6  CPUID.0x80000001:ECX
 *   word 9  CPUID.0x00000007:0:EBX
 * The Linux-synthesized words (3, 8, ...) stay zero. */
static void kpi_cpuid(u32 leaf, u32 subleaf, u32 *eax, u32 *ebx, u32 *ecx,
                      u32 *edx) {
  u32 a = 0, b = 0, c = 0, d = 0;

  __asm__ volatile("cpuid"
                   : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                   : "a"(leaf), "c"(subleaf)
                   : "memory");
  if (eax) *eax = a;
  if (ebx) *ebx = b;
  if (ecx) *ecx = c;
  if (edx) *edx = d;
}

void linuxkpi_x86_cpu_init(void) {
  u32 max_leaf = 0, ext_max = 0;
  u32 eax, ebx, ecx, edx;

  kpi_cpuid(0, 0, &max_leaf, &ebx, &ecx, &edx);

  if (max_leaf >= 1) {
    kpi_cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    boot_cpu_data.x86_capability[0] = edx;
    boot_cpu_data.x86_capability[4] = ecx;
  }
  if (max_leaf >= 7) {
    kpi_cpuid(7, 0, &eax, &ebx, &ecx, &edx);
    boot_cpu_data.x86_capability[9] = ebx;
  }

  kpi_cpuid(0x80000000u, 0, &ext_max, &ebx, &ecx, &edx);
  if (ext_max >= 0x80000001u) {
    kpi_cpuid(0x80000001u, 0, &eax, &ebx, &ecx, &edx);
    boot_cpu_data.x86_capability[1] = edx;
    boot_cpu_data.x86_capability[6] = ecx;
  }

  if (boot_cpu_data.x86_capability[4] & (1u << 31))
    klog_puts("[INFO] LinuxKPI: boot CPU features: X86_FEATURE_HYPERVISOR "
              "set (passthrough detection works)\n");
}
