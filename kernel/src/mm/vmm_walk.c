#include "mm/vmm.h"
#include "mm/vmm_internal.h"
#include "drivers/serial.h"
#include <stddef.h>
#include <stdint.h>

void vmm_dbg_out(const char *s) {
  size_t n = 0;
  while (n < 256 && s[n])
    n++;
  serial_write_sync(s, n);
}

void vmm_dbg_hex(uint64_t v) {
  static const char hex[] = "0123456789ABCDEF";
  char b[18];
  b[0] = '0';
  b[1] = 'x';
  for (int i = 0; i < 16; i++)
    b[2 + i] = hex[(v >> ((15 - i) * 4)) & 0xF];
  serial_write_sync(b, sizeof(b));
}

static void vmm_dbg_entry_flags(uint64_t entry) {
  vmm_dbg_out(" flags=");
  vmm_dbg_out((entry & PAGE_FLAG_PRESENT) ? "P" : "-");
  vmm_dbg_out((entry & PAGE_FLAG_RW) ? " RW" : " RO");
  vmm_dbg_out((entry & PAGE_FLAG_USER) ? " US" : " supervisor");
  if (entry & PAGE_FLAG_PS)
    vmm_dbg_out(" PS");
  if (entry & PAGE_FLAG_NX)
    vmm_dbg_out(" NX");
}

static bool vmm_debug_frame_ok(uint64_t phys) {
  if (phys == 0 || (phys & 0xFFF) != 0)
    return false;
  return pmm_is_managed(phys);
}

uint64_t vmm_debug_walk(uint64_t pml4_phys, uint64_t virtual_addr,
                        uint64_t entries[4]) {
  uint64_t hhdm = pmm_get_hhdm_offset();
  uint64_t table = pml4_phys & PAGE_MASK;

  for (int level = 0; level < 4; level++) {
    if (entries)
      entries[level] = 0;
    if (!vmm_debug_frame_ok(table))
      return 0;
    uint64_t *tbl = (uint64_t *)(uintptr_t)(table + hhdm);
    uint64_t index = (virtual_addr >> (39 - level * 9)) & 0x1FF;
    uint64_t e = tbl[index];
    if (entries)
      entries[level] = e;
    if (!(e & PAGE_FLAG_PRESENT))
      return 0;
    if (level == 3)
      return (e & PAGE_MASK) | (virtual_addr & 0xFFFULL);
    if (e & PAGE_FLAG_PS) {
      if (level == 1)
        return (e & 0x000FFFFFC0000000ULL) | (virtual_addr & 0x3FFFFFFFULL);
      if (level == 2)
        return (e & 0x000FFFFFFFE00000ULL) | (virtual_addr & 0x1FFFFFULL);
      return 0; /* PS at PML4: corruption */
    }
    table = e & PAGE_MASK;
  }
  return 0;
}

void vmm_debug_dump_walk(const char *tag, uint64_t pml4_phys,
                         uint64_t virtual_addr) {
  static const char *level_name[4] = {"PML4E", "PDPTE", "PDE", "PTE"};
  uint64_t indices[4];
  uint64_t table = pml4_phys & PAGE_MASK;
  uint64_t hhdm = pmm_get_hhdm_offset();
  uint64_t phys = 0;
  const char *stop = "walk complete";

  for (int i = 0; i < 4; i++)
    indices[i] = (virtual_addr >> (39 - i * 9)) & 0x1FF;

  vmm_dbg_out("[VMMDBG] ");
  vmm_dbg_out(tag);
  vmm_dbg_out(" cr3=");
  vmm_dbg_hex(pml4_phys);
  vmm_dbg_out(" (base=");
  vmm_dbg_hex(table);
  vmm_dbg_out(") va=");
  vmm_dbg_hex(virtual_addr);
  vmm_dbg_out(" idx=");
  for (int i = 0; i < 4; i++) {
    if (i)
      vmm_dbg_out("/");
    vmm_dbg_hex(indices[i]);
  }
  vmm_dbg_out("\n");

  for (int i = 0; i < 4; i++) {
    vmm_dbg_out("  ");
    vmm_dbg_out(level_name[i]);
    vmm_dbg_out(" table=");
    vmm_dbg_hex(table);
    if (!vmm_debug_frame_ok(table)) {
      vmm_dbg_out(" UNMANAGED_TABLE_FRAME\n");
      stop = "unmanaged table frame";
      break;
    }

    uint64_t *entries = (uint64_t *)(uintptr_t)(table + hhdm);
    uint64_t entry = entries[indices[i]];
    vmm_dbg_out(" index=");
    vmm_dbg_hex(indices[i]);
    vmm_dbg_out(" entry=");
    vmm_dbg_hex(entry);
    vmm_dbg_entry_flags(entry);
    if (!(entry & PAGE_FLAG_PRESENT)) {
      vmm_dbg_out("\n");
      stop = "not-present";
      break;
    }

    if (i == 3) {
      phys = (entry & PAGE_MASK) | (virtual_addr & 0xFFFULL);
      vmm_dbg_out(" leaf=4K\n");
      stop = "leaf=4K";
      break;
    }
    if (entry & PAGE_FLAG_PS) {
      if (i == 1) {
        phys = (entry & 0x000FFFFFC0000000ULL) |
               (virtual_addr & 0x3FFFFFFFULL);
        vmm_dbg_out(" leaf=1G\n");
        stop = "leaf=1G";
      } else if (i == 2) {
        phys = (entry & 0x000FFFFFFFE00000ULL) |
               (virtual_addr & 0x1FFFFFULL);
        vmm_dbg_out(" leaf=2M\n");
        stop = "leaf=2M";
      } else {
        vmm_dbg_out(" INVALID_PS_AT_PML4\n");
        stop = "invalid PS at PML4";
      }
      break;
    }
    vmm_dbg_out("\n");
    table = entry & PAGE_MASK;
  }

  vmm_dbg_out("  result=");
  vmm_dbg_hex(phys);
  vmm_dbg_out(" stop=");
  vmm_dbg_out(stop);
  vmm_dbg_out("\n");
}

uint64_t vmm_virt_to_phys(uint64_t *pml4_phys, uint64_t virtual_addr) {
  size_t pml4_index = (virtual_addr >> 39) & 0x1FF;
  size_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
  size_t pd_index   = (virtual_addr >> 21) & 0x1FF;
  size_t pt_index   = (virtual_addr >> 12) & 0x1FF;

  uint64_t *pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)pml4_phys & PAGE_MASK);
  uint64_t  entry;

  entry = pml4_virt[pml4_index];
  if (!(entry & PAGE_FLAG_PRESENT))
    return 0;

  uint64_t *pdpt_virt = (uint64_t *)PHYS_TO_VIRT(entry & PAGE_MASK);
  entry = pdpt_virt[pdpt_index];
  if (!(entry & PAGE_FLAG_PRESENT))
    return 0;
  if (entry & PAGE_FLAG_PS)
    return (entry & 0xFFFFFC0000000ULL) | (virtual_addr & 0x3FFFFFFFULL);

  uint64_t *pd_virt = (uint64_t *)PHYS_TO_VIRT(entry & PAGE_MASK);
  entry = pd_virt[pd_index];
  if (!(entry & PAGE_FLAG_PRESENT))
    return 0;
  if (entry & PAGE_FLAG_PS)
    return (entry & 0xFFFFFFFE00000ULL) | (virtual_addr & 0x1FFFFFULL);

  uint64_t *pt_virt = (uint64_t *)PHYS_TO_VIRT(entry & PAGE_MASK);
  entry = pt_virt[pt_index];
  if (!(entry & PAGE_FLAG_PRESENT))
    return 0;
  return (entry & PAGE_MASK) | (virtual_addr & 0xFFFULL);
}

bool vmm_is_huge_page(uint64_t *pml4_phys, uint64_t virtual_addr) {
  if (!pml4_phys)
    return false;

  size_t pml4_index = (virtual_addr >> 39) & 0x1FF;
  size_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
  size_t pd_index   = (virtual_addr >> 21) & 0x1FF;

  uint64_t *pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)pml4_phys & PAGE_MASK);
  uint64_t  entry = pml4_virt[pml4_index];
  if (!(entry & PAGE_FLAG_PRESENT))
    return false;

  uint64_t *pdpt_virt = (uint64_t *)PHYS_TO_VIRT(entry & PAGE_MASK);
  entry = pdpt_virt[pdpt_index];
  if (!(entry & PAGE_FLAG_PRESENT))
    return false;
  if (entry & PAGE_FLAG_PS)
    return true;

  uint64_t *pd_virt = (uint64_t *)PHYS_TO_VIRT(entry & PAGE_MASK);
  entry = pd_virt[pd_index];
  if (!(entry & PAGE_FLAG_PRESENT))
    return false;
  return (entry & PAGE_FLAG_PS) != 0;
}

bool vmm_huge_slot_free(uint64_t *pml4_phys, uint64_t virtual_addr) {
  if (!pml4_phys)
    return false;

  uint64_t *pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)pml4_phys & PAGE_MASK);
  uint64_t entry = pml4_virt[(virtual_addr >> 39) & 0x1FF];
  if (!(entry & PAGE_FLAG_PRESENT))
    return true;

  uint64_t *pdpt_virt = (uint64_t *)PHYS_TO_VIRT(entry & PAGE_MASK);
  entry = pdpt_virt[(virtual_addr >> 30) & 0x1FF];
  if (!(entry & PAGE_FLAG_PRESENT))
    return true;
  if (entry & PAGE_FLAG_PS)
    return false;

  uint64_t *pd_virt = (uint64_t *)PHYS_TO_VIRT(entry & PAGE_MASK);
  return !(pd_virt[(virtual_addr >> 21) & 0x1FF] & PAGE_FLAG_PRESENT);
}

uint64_t vmm_user_resident_bytes(uint64_t cr3) {
  if (!cr3)
    return 0;

  uint64_t *pml4_virt = (uint64_t *)PHYS_TO_VIRT(cr3 & PAGE_MASK);
  uint64_t pages = 0;

  for (size_t i = 0; i < 256; i++) {
    if (!(pml4_virt[i] & PAGE_FLAG_PRESENT))
      continue;

    uint64_t *pdpt_virt = (uint64_t *)PHYS_TO_VIRT(pml4_virt[i] & PAGE_MASK);
    for (size_t j = 0; j < 512; j++) {
      if (!(pdpt_virt[j] & PAGE_FLAG_PRESENT))
        continue;
      if (pdpt_virt[j] & PAGE_FLAG_PS) {
        pages += 262144; // 1 GB leaf
        continue;
      }

      uint64_t *pd_virt = (uint64_t *)PHYS_TO_VIRT(pdpt_virt[j] & PAGE_MASK);
      for (size_t k = 0; k < 512; k++) {
        if (!(pd_virt[k] & PAGE_FLAG_PRESENT))
          continue;
        if (pd_virt[k] & PAGE_FLAG_PS) {
          pages += 512; // 2 MB leaf
          continue;
        }

        uint64_t *pt_virt = (uint64_t *)PHYS_TO_VIRT(pd_virt[k] & PAGE_MASK);
        for (size_t l = 0; l < 512; l++) {
          if (pt_virt[l] & PAGE_FLAG_PRESENT)
            pages++;
        }
      }
    }
  }

  return pages * PAGE_SIZE;
}

uint64_t *vmm_get_pte_ptr(uint64_t *pml4_phys, uint64_t virtual_addr) {
  if (!pml4_phys)
    return NULL;

  size_t pml4_index = (virtual_addr >> 39) & 0x1FF;
  size_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
  size_t pd_index   = (virtual_addr >> 21) & 0x1FF;
  size_t pt_index   = (virtual_addr >> 12) & 0x1FF;

  uint64_t *pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)pml4_phys & PAGE_MASK);
  uint64_t entry = pml4_virt[pml4_index];
  if (!(entry & PAGE_FLAG_PRESENT))
    return NULL;

  uint64_t *pdpt_virt = (uint64_t *)PHYS_TO_VIRT(entry & PAGE_MASK);
  entry = pdpt_virt[pdpt_index];
  if (!(entry & PAGE_FLAG_PRESENT) || (entry & PAGE_FLAG_PS))
    return NULL;

  uint64_t *pd_virt = (uint64_t *)PHYS_TO_VIRT(entry & PAGE_MASK);
  entry = pd_virt[pd_index];
  if (!(entry & PAGE_FLAG_PRESENT) || (entry & PAGE_FLAG_PS))
    return NULL;

  uint64_t *pt_virt = (uint64_t *)PHYS_TO_VIRT(entry & PAGE_MASK);
  return &pt_virt[pt_index];
}

uint64_t *vmm_get_leaf_entry_ptr(uint64_t *pml4_phys, uint64_t virtual_addr) {
  if (!pml4_phys)
    return NULL;
  size_t pml4_index = (virtual_addr >> 39) & 0x1FF;
  size_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
  size_t pd_index = (virtual_addr >> 21) & 0x1FF;
  size_t pt_index = (virtual_addr >> 12) & 0x1FF;

  uint64_t *pml4 = PHYS_TO_VIRT((uint64_t)pml4_phys & PAGE_MASK);
  uint64_t entry = pml4[pml4_index];
  if (!(entry & PAGE_FLAG_PRESENT))
    return NULL;
  uint64_t *pdpt = PHYS_TO_VIRT(entry & PAGE_MASK);
  if (!(pdpt[pdpt_index] & PAGE_FLAG_PRESENT))
    return NULL;
  if (pdpt[pdpt_index] & PAGE_FLAG_PS)
    return &pdpt[pdpt_index];
  uint64_t *pd = PHYS_TO_VIRT(pdpt[pdpt_index] & PAGE_MASK);
  if (!(pd[pd_index] & PAGE_FLAG_PRESENT))
    return NULL;
  if (pd[pd_index] & PAGE_FLAG_PS)
    return &pd[pd_index];
  uint64_t *pt = PHYS_TO_VIRT(pd[pd_index] & PAGE_MASK);
  return &pt[pt_index];
}
