/* LinuxKPI mmap helpers (header: linux/mm.h).
 *
 * The authoritative mapping record is the native `struct vma`; imported
 * drivers see a Linux-facing `struct vm_area_struct` owned by the bridge in
 * file.c.  Everything here installs PTEs into the *current* address space
 * (CR3), which is the user process during mmap/fault syscalls. */

#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/string.h>

#include <linuxkpi/native_mm.h>

/* Page-table flag derived from the Linux VMA.  Normal RAM mappings are
 * write-back; VM_IO/VM_PFNMAP mappings ask for uncached like the native
 * framebuffer mappings do. */
static unsigned long kpi_vma_pte_flags(struct vm_area_struct *vma) {
  unsigned long flags = ASC_PAGE_USER;

  if (vma->vm_flags & VM_WRITE)
    flags |= ASC_PAGE_RW;
  if (!(vma->vm_flags & VM_EXEC))
    flags |= ASC_PAGE_NX;
  if (vma->vm_flags & VM_IO)
    flags |= ASC_PAGE_PCD;
  return flags;
}

static int kpi_map_phys(uint64_t addr, uint64_t phys, unsigned long size,
                        unsigned long pte_flags) {
  __UINT64_TYPE__ *pml4 = asc_vmm_get_active_pml4();

  if (!pml4)
    return -EINVAL;

  size = (size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
  for (unsigned long off = 0; off < size; off += PAGE_SIZE) {
    if (!asc_vmm_map_page(pml4, addr + off, phys + off, pte_flags))
      return -ENOMEM;
    /* A stale translation may exist from an earlier vma in this spot. */
    asc_invlpg(addr + off);
  }
  return 0;
}

int remap_pfn_range_notrack(struct vm_area_struct *vma, unsigned long addr,
                            unsigned long pfn, unsigned long size,
                            pgprot_t prot) {
  (void)prot;

  if (addr < vma->vm_start || addr + size > vma->vm_end)
    return -EINVAL;
  if (addr & ~PAGE_MASK)
    return -EINVAL;

  return kpi_map_phys(addr, (uint64_t)pfn << PAGE_SHIFT, size,
                      kpi_vma_pte_flags(vma));
}

int remap_pfn_range(struct vm_area_struct *vma, unsigned long addr,
                    unsigned long pfn, unsigned long size, pgprot_t prot) {
  return remap_pfn_range_notrack(vma, addr, pfn, size, prot);
}

int vm_insert_page(struct vm_area_struct *vma, unsigned long addr,
                   struct page *page) {
  if (addr < vma->vm_start || addr + PAGE_SIZE > vma->vm_end)
    return -EINVAL;
  get_page(page);
  return kpi_map_phys(addr, page_to_phys(page), PAGE_SIZE,
                      kpi_vma_pte_flags(vma));
}

vm_fault_t vmf_insert_page(struct vm_fault *vmf, struct page *page) {
  int ret = vm_insert_page(vmf->vma, vmf->address, page);

  return ret ? VM_FAULT_SIGBUS : VM_FAULT_NOPAGE;
}

vm_fault_t vmf_insert_pfn_prot(struct vm_area_struct *vma, unsigned long addr,
                               unsigned long pfn, pgprot_t prot) {
  int ret;

  (void)prot;
  if (addr < vma->vm_start || addr + PAGE_SIZE > vma->vm_end)
    return VM_FAULT_SIGBUS;
  ret = kpi_map_phys(addr, (uint64_t)pfn << PAGE_SHIFT, PAGE_SIZE,
                     kpi_vma_pte_flags(vma));
  return ret ? VM_FAULT_SIGBUS : VM_FAULT_NOPAGE;
}

vm_fault_t vmf_insert_pfn(struct vm_area_struct *vma, unsigned long addr,
                          unsigned long pfn) {
  return vmf_insert_pfn_prot(vma, addr, pfn, __pgprot(0));
}

vm_fault_t vmf_insert_mixed(struct vm_area_struct *vma, unsigned long addr,
                            unsigned long pfn) {
  return vmf_insert_pfn_prot(vma, addr, pfn, __pgprot(0));
}

void zap_vma_ptes(struct vm_area_struct *vma, unsigned long address,
                  unsigned long size) {
  __UINT64_TYPE__ *pml4 = asc_vmm_get_active_pml4();

  if (!pml4 || address < vma->vm_start || address + size > vma->vm_end)
    return;

  size = (size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
  for (unsigned long off = 0; off < size; off += PAGE_SIZE) {
    asc_vmm_unmap_page(pml4, address + off);
    asc_invlpg(address + off);
  }
}

/* Invalidate userspace mappings of an address_space over a file range, the
 * way Linux does (used by TTM/amdgpu BO eviction and device unplug).
 *
 * The native walker (kernel/src/mm/mapping_unmap.c) finds every address space
 * with a Linux-bridged VMA whose file mapping is `mapping` and whose file
 * range overlaps [holebegin, holebegin+holelen), then zaps the PTEs.  A zero
 * holelen means "to the end of the file"; `even_cows == 0` skips private
 * mappings.  Pages stay owned by the backing object, exactly like munmap of a
 * driver mapping. */
void unmap_mapping_range(struct address_space *mapping, loff_t const holebegin,
                         loff_t const holelen, int even_cows) {
  uint64_t begin, end;

  if (!mapping)
    return;

  begin = holebegin < 0 ? 0 : (uint64_t)holebegin;
  if (holelen <= 0) {
    end = (uint64_t)-1;
  } else {
    end = begin + (uint64_t)holelen;
    if (end < begin)
      end = (uint64_t)-1; /* overflow: to the end */
  }

  /* Linux rounds the start down and the length up to pages. */
  begin &= ~(uint64_t)(PAGE_SIZE - 1);
  if (end != (uint64_t)-1)
    end = (end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

  vma_unmap_mapping_range(mapping, begin, end, even_cows != 0);
}

void vma_set_file(struct vm_area_struct *vma, struct file *file) {
  if (vma->vm_file == file)
    return;
  get_file(file);
  if (vma->vm_file)
    fput(vma->vm_file);
  vma->vm_file = file;
}
