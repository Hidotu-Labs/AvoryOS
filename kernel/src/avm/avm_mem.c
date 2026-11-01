/* Guest memory: pins a memslot's host pages and maps them into the NPT.
 *
 * A memslot's pages are the VMM's own anonymous pages.  Each one is made
 * private and resident through the normal fault path, then gets a frame
 * reference and the PTE MLOCKED bit.  The reference keeps the frame alive,
 * and the bit keeps zram swap and NUMA migration away from it.  KSM only
 * merges frames with a refcount of 1, so a pinned frame is never a candidate.
 *
 * Limitation: a fork of the VMM would turn the pinned pages copy-on-write.
 * The VMM must not fork while a memslot is live. */

#include "avm_internal.h"
#include "avm_smp.h"

#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "../cpu/isr.h"
#include "../include/linux/errno.h"
#include "../lib/string.h"
#include "../mm/pmm.h"
#include "../mm/vma.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"

/* NPT entries use the x86-64 PTE layout: P, RW and U/S set on every level. */
#define NPT_PRESENT 0x7ull
/* A read-only slot: present and user, without the write bit. */
#define NPT_PRESENT_RO (NPT_PRESENT & ~0x2ull)
#define NPT_LEVELS 4
#define NPT_ENTRIES 512
#define PROT_WRITE_BIT 0x2ull

static uint64_t *npt_table(uint64_t pa) {
  return (uint64_t *)(pa + pmm_get_hhdm_offset());
}

/* Returns the leaf entry for gpa.  Missing tables are created when alloc is
 * set, and NULL means there is no leaf or the table allocation failed. */
static uint64_t *npt_leaf(uint64_t root, uint64_t gpa, bool alloc) {
  uint64_t *table = npt_table(root);
  for (int level = NPT_LEVELS; level > 1; level--) {
    uint64_t *entry = &table[(gpa >> (12 + 9 * (level - 1))) & (NPT_ENTRIES - 1)];
    if (!(*entry & 1)) {
      if (!alloc)
        return NULL;
      uint64_t pa = (uint64_t)pmm_alloc();
      if (!pa)
        return NULL;
      memset(npt_table(pa), 0, AVM_PAGE_SIZE);
      *entry = pa | NPT_PRESENT;
    }
    table = npt_table(*entry & PAGE_MASK);
  }
  return &table[(gpa >> 12) & (NPT_ENTRIES - 1)];
}

/* Frees the tables under table_pa and drops the reference on every mapped
 * frame.  level 1 holds leaves; their frames belong to the pinned pages. */
static void npt_free_level(uint64_t table_pa, int level) {
  uint64_t *table = npt_table(table_pa);
  for (int i = 0; i < NPT_ENTRIES; i++) {
    uint64_t e = table[i];
    if (!(e & 1))
      continue;
    uint64_t pa = e & PAGE_MASK;
    if (level > 1)
      npt_free_level(pa, level - 1);
    else
      pmm_decref((void *)pa);
  }
  pmm_free((void *)table_pa);
}

int avm_mem_vm_init(struct avm_vm *vm) {
  uint64_t root = (uint64_t)pmm_alloc();
  if (!root)
    return -ENOMEM;
  memset(npt_table(root), 0, AVM_PAGE_SIZE);
  vm->npt_root = root;
  return 0;
}

void avm_mem_vm_destroy(struct avm_vm *vm) {
  if (vm->npt_root) {
    npt_free_level(vm->npt_root, NPT_LEVELS);
    vm->npt_root = 0;
  }
}

/* Makes one page of the VMM's address space private, writable and resident,
 * then takes a reference and sets MLOCKED.  Writing through the fault path
 * breaks any copy-on-write sharing first, so the frame belongs to the VMM
 * alone. */
static int avm_pin_page(struct thread *t, uint64_t va, uint64_t *pa_out) {
  uint64_t *pml4 = t->cr3 ? (uint64_t *)t->cr3 : vmm_get_active_pml4();

  spinlock_acquire(&t->mm->lock);
  struct vma *v = vma_find(&t->mm->vmas, va);
  bool eligible = v && (v->prot & PROT_WRITE_BIT) &&
                  (v->flags & MAP_PRIVATE) && (v->flags & MAP_ANONYMOUS) &&
                  !v->linux_vma && !v->file_node;
  spinlock_release(&t->mm->lock);
  if (!eligible)
    return -EINVAL;

  for (unsigned retry = 0; retry < 3; retry++) {
    vmm_lock_acquire_for(pml4, va);
    uint64_t *leaf = vmm_get_leaf_entry_ptr(pml4, va);
    bool present = leaf && (*leaf & PAGE_FLAG_PRESENT);
    if (present && (*leaf & PAGE_FLAG_RW) && !(*leaf & PAGE_FLAG_COW)) {
      /* The leaf may be a 2 MB huge page, whose frame is the base of the
       * range.  vmm_virt_to_phys adds the offset of va inside it, so each 4 KB
       * page gets its own frame. */
      uint64_t pa = vmm_virt_to_phys(pml4, va) & PAGE_MASK;
      *leaf |= PAGE_FLAG_MLOCKED;
      pmm_incref((void *)pa);
      vmm_lock_release();
      *pa_out = pa;
      return 0;
    }
    /* Error code 0x6 is a user write to a page that is not present.  0x7 is
     * a user write to a present page that is read-only or copy-on-write. */
    uint64_t code = present ? 0x7 : 0x6;
    vmm_lock_release();

    struct registers regs = {0};
    if (vmm_handle_page_fault(va, code, &regs) != 0)
      return -EFAULT;
  }
  return -EFAULT;
}

/* Clears the NPT leaves for the first count pages of a slot and drops their
 * references.  Used to unwind a failed memslot. */
static void avm_unmap_pages(uint64_t root, uint64_t gpa, uint64_t count) {
  for (uint64_t i = 0; i < count; i++) {
    uint64_t *leaf = npt_leaf(root, gpa + i * AVM_PAGE_SIZE, false);
    if (leaf && (*leaf & 1)) {
      pmm_decref((void *)(*leaf & PAGE_MASK));
      *leaf = 0;
    }
  }
}

int avm_mem_map_slot(struct avm_vm *vm, const struct avm_slot *slot) {
  struct thread *t = sched_get_current();
  if (!t || !t->mm)
    return -EINVAL;

  uint64_t pages = slot->size / AVM_PAGE_SIZE;
  for (uint64_t i = 0; i < pages; i++) {
    uint64_t pa;
    int err = avm_pin_page(t, slot->hva + i * AVM_PAGE_SIZE, &pa);
    if (err) {
      avm_unmap_pages(vm->npt_root, slot->gpa, i);
      return err;
    }

    uint64_t *leaf = npt_leaf(vm->npt_root, slot->gpa + i * AVM_PAGE_SIZE, true);
    if (!leaf) {
      pmm_decref((void *)pa);
      avm_unmap_pages(vm->npt_root, slot->gpa, i);
      return -ENOMEM;
    }
    *leaf = pa | (slot->readonly ? NPT_PRESENT_RO : NPT_PRESENT);
  }

  /* Check every translation against the VMM's own page tables before the
   * slot is published.  A mismatch means the guest would see other memory. */
  uint64_t *pml4 = t->cr3 ? (uint64_t *)t->cr3 : vmm_get_active_pml4();
  for (uint64_t i = 0; i < pages; i++) {
    uint64_t gpa = slot->gpa + i * AVM_PAGE_SIZE;
    uint64_t *leaf = npt_leaf(vm->npt_root, gpa, false);
    uint64_t npt_pa = leaf ? (*leaf & PAGE_MASK) : ~0ull;
    uint64_t host_pa = vmm_virt_to_phys(pml4, slot->hva + i * AVM_PAGE_SIZE) & PAGE_MASK;
    if (npt_pa != host_pa) {
      klog_puts("[AVM] npt verify FAILED: gpa=");
      klog_hex64(gpa);
      klog_puts(" npt=");
      klog_hex64(npt_pa);
      klog_puts(" host=");
      klog_hex64(host_pa);
      klog_puts("\n");
      avm_unmap_pages(vm->npt_root, slot->gpa, pages);
      return -EFAULT;
    }
  }

  /* A vCPU running now may have the old NPT cached.  Kicking it out makes it
   * re-enter, and each entry flushes the guest TLB. */
  avm_smp_kick_all(vm);
  return 0;
}

/* Waits until no vCPU of the VM is inside VMRUN.  Each one still inside is
 * kicked out.  Once a vCPU has been seen outside, it cannot use NPT entries
 * cleared before this call, because every later entry walks the NPT again.  A
 * halted vCPU is not in VMRUN, so it does not hold this up. */
static void avm_wait_no_vmrun(struct avm_vm *vm) {
  for (uint32_t i = 0; i < AVM_MAX_VCPUS; i++) {
    struct avm_vcpu *v = vm->vcpus[i];
    if (!v)
      continue;
    while (__atomic_load_n(&v->in_vmrun, __ATOMIC_SEQ_CST)) {
      avm_smp_kick(v);
      lapic_timer_sleep(1);
    }
  }
}

int avm_mem_unmap_slot(struct avm_vm *vm, const struct avm_slot *slot) {
  struct thread *t = sched_get_current();
  uint64_t *pml4 = (t && t->cr3) ? (uint64_t *)t->cr3 : vmm_get_active_pml4();
  uint64_t pages = slot->size / AVM_PAGE_SIZE;

  /* The NPT entries go first, so no vCPU can reach the pages through them
   * again.  Then every vCPU must leave VMRUN, since one inside it may still be
   * using them.  Only after that are the references dropped.  Dropping them
   * earlier let a running guest write into a page the kernel had freed. */
  for (uint64_t i = 0; i < pages; i++) {
    uint64_t *leaf = npt_leaf(vm->npt_root, slot->gpa + i * AVM_PAGE_SIZE, false);
    if (leaf && (*leaf & 1))
      *leaf = 0;
  }
  avm_smp_kick_all(vm);
  avm_wait_no_vmrun(vm);

  /* The VMM's mapping still points at each page until its MLOCKED bit goes, so
   * the page is found through it. */
  for (uint64_t i = 0; i < pages; i++) {
    uint64_t va = slot->hva + i * AVM_PAGE_SIZE;
    pmm_decref((void *)(vmm_virt_to_phys(pml4, va) & PAGE_MASK));
  }

  /* The VMM's pages are no longer pinned, so swap and migration may move them. */
  for (uint64_t i = 0; i < pages; i++) {
    uint64_t va = slot->hva + i * AVM_PAGE_SIZE;
    vmm_lock_acquire_for(pml4, va);
    uint64_t *leaf = vmm_get_leaf_entry_ptr(pml4, va);
    if (leaf)
      *leaf &= ~PAGE_FLAG_MLOCKED;
    vmm_lock_release();
  }
  return 0;
}
