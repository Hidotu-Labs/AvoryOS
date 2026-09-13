/* x86 arch entry points that imported Linux code reaches through the stock
 * x86 headers, but whose upstream implementations live in files AvoryOS does
 * not build (arch/x86/mm/pat/memtype.c, arch/x86/lib/clear_page_64.S).
 *
 * AvoryOS maps all RAM through the HHDM as write-back and never programs PAT
 * or MTRRs:
 *   - cachemode2protval() returns the x86 PAT bits for the requested mode so
 *     the pgprot_t values imported code builds are well formed, but nothing
 *     consumes them for CPU mappings (kmap is an HHDM lookup).
 *   - set_pages_*() cache-mode flips are inert; a page's effective caching is
 *     always write-back.  TTM uses them around its pool pages; the VRAM BAR
 *     itself is mapped separately through ioremap().
 *   - clear_page()'s alternatives target memset over the HHDM.
 *
 * Recorded in docs/linuxkpi-gaps.md (Phase 4). */

#include <asm/page.h>
#include <asm/processor.h>
#include <asm/pgtable_types.h>
#include <asm/set_memory.h>
#include <linux/string.h>
#include <linux/types.h>

#include <linuxkpi/log.h>

unsigned long cachemode2protval(enum page_cache_mode pcm) {
  switch (pcm) {
    case _PAGE_CACHE_MODE_UC:
      return _PAGE_PCD | _PAGE_PWT;
    case _PAGE_CACHE_MODE_UC_MINUS:
      return _PAGE_PCD;
    case _PAGE_CACHE_MODE_WC:
      return _PAGE_PWT;
    case _PAGE_CACHE_MODE_WP:
      return _PAGE_PCD | _PAGE_PWT;
    case _PAGE_CACHE_MODE_WB:
    default:
      return 0;
  }
}

pgprot_t pgprot_writecombine(pgprot_t prot) {
  /* Mapping protections built from this are ignored by the HHDM kmap. */
  return prot;
}

void clear_page_orig(void *page) { memset(page, 0, PAGE_SIZE); }
void clear_page_rep(void *page) { memset(page, 0, PAGE_SIZE); }
void clear_page_erms(void *page) { memset(page, 0, PAGE_SIZE); }

int set_pages_wb(struct page *page, int numpages) {
  (void)page;
  (void)numpages;
  return 0;
}

int set_pages_array_uc(struct page **pages, int addrinarray) {
  (void)pages;
  (void)addrinarray;
  return 0;
}

int set_pages_array_wc(struct page **pages, int addrinarray) {
  (void)pages;
  (void)addrinarray;
  return 0;
}

int set_pages_array_wb(struct page **pages, int addrinarray) {
  (void)pages;
  (void)addrinarray;
  return 0;
}

/* Uniprocessor percpu emulation (docs/linuxkpi-gaps.md P1/P2): stock x86
 * paths index __per_cpu_offset[] and take the address of cpu_info.  One
 * shared instance is correct while per-CPU use is scratch under preempt
 * disable; the array exists so per_cpu_offset()/per_cpu_ptr() link. */
unsigned long __per_cpu_offset[NR_CPUS];

struct cpuinfo_x86 cpu_info;

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
