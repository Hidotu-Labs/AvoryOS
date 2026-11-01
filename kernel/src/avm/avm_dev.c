/* /dev/kvm: the system fd, VM fds and vCPU fds.
 *
 * Memslots are pinned and mapped into the NPT (avm_mem.c).  KVM_RUN enters
 * the guest through the SVM backend (svm.c), and the kvm_run page is mapped
 * into the VMM by mmap on the vCPU fd. */

#include "avm_internal.h"
#include "avm_cpuid.h"
#include "avm_ioeventfd.h"
#include "avm_irq.h"
#include "avm_irqroute.h"
#include "avm_state.h"
#include "avm_vm_misc.h"
#include "avm_lapic.h"
#include "avm_msr.h"
#include "avm_smp.h"
#include "svm.h"

#include "../console/klog.h"
#include "../fs/devfs.h"
#include "../fs/vfs.h"
#include "../include/arch/uaccess.h"
#include "../include/linux/errno.h"
#include "../lib/string.h"
#include "../lock/spinlock.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../syscalls/sys_io_shared.h"

static vfs_node_t g_avm_node;

static int avm_vcpu_ioctl(vfs_node_t *node, uint32_t request, uint64_t arg);

static vfs_node_t *avm_new_node(const char *name, void *device,
                                ioctl_type_t ioctl, close_type_t close) {
  vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
  if (!node)
    return NULL;
  vfs_node_init(node);
  strncpy(node->name, name, sizeof(node->name) - 1);
  node->flags = FS_FILE;
  node->mask = 0600;
  node->device = device;
  node->ioctl = ioctl;
  node->close = close;
  return node;
}

/* Puts a node in the calling thread's fd table and returns the fd number.
 * On failure the node is closed and freed, so callers never leak it. */
static int avm_install_fd(vfs_node_t *node) {
  struct thread *t = sched_get_current();
  int fd = t ? alloc_fd(t) : -1;
  if (fd < 0) {
    if (node->close)
      node->close(node);
    kfree(node);
    return t ? -EMFILE : -EINVAL;
  }

  t->fds[fd] = node;
  t->fd_offsets[fd] = 0;
  t->fd_flags[fd] = O_RDWR;
  return fd;
}

static void avm_vm_put(struct avm_vm *vm) {
  spinlock_acquire(&vm->lock);
  uint32_t refs = --vm->refs;
  spinlock_release(&vm->lock);
  if (refs)
    return;

  avm_ioeventfd_destroy(vm);
  avm_irqfd_destroy(vm);
  for (uint32_t i = 0; i < AVM_MAX_VCPUS; i++) {
    struct avm_vcpu *vcpu = vm->vcpus[i];
    if (!vcpu)
      continue;
    svm_vcpu_destroy(vcpu);
    pmm_decref((void *)vcpu->run_pa);
    kfree(vcpu);
  }
  avm_mem_vm_destroy(vm);
  kfree(vm);
}

static void avm_vcpu_close(vfs_node_t *node) {
  struct avm_vcpu *vcpu = node->device;
  if (!vcpu)
    return;
  struct avm_vm *vm = vcpu->vm;
  node->device = NULL;
  /* The vCPU object is owned by the VM; avm_vm_put frees it at the end. */
  avm_vm_put(vm);
}

static void avm_vm_close(vfs_node_t *node) {
  struct avm_vm *vm = node->device;
  if (!vm)
    return;
  node->device = NULL;
  avm_vm_put(vm);
}

/* mmap on a vCPU fd maps its kvm_run page.  The mapping holds its own frame
 * reference, so munmap cannot free the page under the vCPU. */
static uint64_t avm_vcpu_mmap(vfs_node_t *node, uint64_t addr, uint64_t length,
                              uint64_t prot, uint64_t flags, uint64_t offset) {
  (void)prot;
  (void)flags;
  struct avm_vcpu *vcpu = node->device;
  if (!vcpu || offset != 0 || length > AVM_PAGE_SIZE || addr == 0)
    return (uint64_t)-1;

  pmm_incref((void *)vcpu->run_pa);
  if (!vmm_map_page(vmm_get_active_pml4(), addr, vcpu->run_pa,
                    PAGE_FLAG_PRESENT | PAGE_FLAG_USER | PAGE_FLAG_RW)) {
    pmm_decref((void *)vcpu->run_pa);
    return (uint64_t)-1;
  }
  return addr;
}

static int avm_create_vcpu(struct avm_vm *vm, uint64_t id) {
  if (id >= AVM_MAX_VCPUS)
    return -EINVAL;

  spinlock_acquire(&vm->lock);
  bool taken = vm->vcpus[id] != NULL;
  spinlock_release(&vm->lock);
  if (taken)
    return -EEXIST;

  struct avm_vcpu *vcpu = kmalloc(sizeof(*vcpu));
  if (!vcpu)
    return -ENOMEM;
  memset(vcpu, 0, sizeof(*vcpu));
  vcpu->vm = vm;
  vcpu->id = (uint32_t)id;
  vcpu->cpuid_nent = avm_cpuid_default(vcpu->cpuid, vcpu->id);
  avm_lapic_init(vcpu);
  avm_smp_init_vcpu(vcpu);

  uint64_t run_pa = (uint64_t)pmm_alloc();
  if (!run_pa) {
    kfree(vcpu);
    return -ENOMEM;
  }
  memset((void *)(run_pa + pmm_get_hhdm_offset()), 0, AVM_PAGE_SIZE);
  vcpu->run_pa = run_pa;
  vcpu->run = (struct kvm_run *)(run_pa + pmm_get_hhdm_offset());

  int err = svm_vcpu_init(vcpu);
  if (err) {
    pmm_decref((void *)run_pa);
    kfree(vcpu);
    return err;
  }

  spinlock_acquire(&vm->lock);
  if (vm->vcpus[id]) {
    /* Another thread created this id while we were allocating. */
    spinlock_release(&vm->lock);
    svm_vcpu_destroy(vcpu);
    pmm_decref((void *)run_pa);
    kfree(vcpu);
    return -EEXIST;
  }
  vm->vcpus[id] = vcpu;
  vm->refs++;
  spinlock_release(&vm->lock);

  vfs_node_t *node =
      avm_new_node("avm-vcpu", vcpu, avm_vcpu_ioctl, avm_vcpu_close);
  if (!node) {
    /* Undo the slot and reference taken above. */
    spinlock_acquire(&vm->lock);
    vm->vcpus[id] = NULL;
    vm->refs--;
    spinlock_release(&vm->lock);
    svm_vcpu_destroy(vcpu);
    pmm_decref((void *)run_pa);
    kfree(vcpu);
    return -ENOMEM;
  }
  node->mmap = avm_vcpu_mmap;
  return avm_install_fd(node);
}

/* Removes a memslot: its NPT entries go, the pages are unpinned, and running
 * vCPUs are kicked so they re-walk the NPT on their next entry. */
static int avm_unset_memslot(struct avm_vm *vm, uint32_t id) {
  spinlock_acquire(&vm->lock);
  if (vm->slot_busy) {
    spinlock_release(&vm->lock);
    return -EBUSY;
  }
  if (!vm->slots[id].used) {
    spinlock_release(&vm->lock);
    return -EINVAL;
  }
  vm->slot_busy = true;
  struct avm_slot slot = vm->slots[id];
  spinlock_release(&vm->lock);

  avm_mem_unmap_slot(vm, &slot);

  spinlock_acquire(&vm->lock);
  vm->slots[id].used = false;
  vm->slot_busy = false;
  spinlock_release(&vm->lock);
  return 0;
}

static int avm_set_memslot(struct avm_vm *vm,
                           const struct kvm_userspace_memory_region *r) {
  if (r->slot >= AVM_MAX_SLOTS)
    return -EINVAL;
  if (r->flags & ~(uint32_t)(KVM_MEM_READONLY | KVM_MEM_LOG_DIRTY_PAGES))
    return -EINVAL;
  if (r->memory_size == 0)
    return avm_unset_memslot(vm, r->slot);
  if (r->memory_size & (AVM_PAGE_SIZE - 1))
    return -EINVAL;
  if (r->guest_phys_addr & (AVM_PAGE_SIZE - 1))
    return -EINVAL;
  if (r->userspace_addr & (AVM_PAGE_SIZE - 1))
    return -EINVAL;

  struct avm_slot slot = {
      .id = r->slot,
      .gpa = r->guest_phys_addr,
      .size = r->memory_size,
      .hva = r->userspace_addr,
      .used = true,
      .readonly = r->flags & KVM_MEM_READONLY,
      .log_dirty = r->flags & KVM_MEM_LOG_DIRTY_PAGES,
  };

  spinlock_acquire(&vm->lock);
  if (vm->slot_busy || vm->slots[r->slot].used) {
    /* Replacing a live slot would need its NPT entries unmapped first. */
    spinlock_release(&vm->lock);
    return vm->slot_busy ? -EBUSY : -EEXIST;
  }
  for (uint32_t i = 0; i < AVM_MAX_SLOTS; i++) {
    struct avm_slot *s = &vm->slots[i];
    if (!s->used)
      continue;
    /* Reject overlap with any other slot in guest-physical space. */
    if (slot.gpa < s->gpa + s->size && s->gpa < slot.gpa + slot.size) {
      spinlock_release(&vm->lock);
      return -EEXIST;
    }
  }
  vm->slot_busy = true;
  spinlock_release(&vm->lock);

  /* Pinning can fault and sleep, so it runs with the lock dropped. */
  int err = avm_mem_map_slot(vm, &slot);

  spinlock_acquire(&vm->lock);
  if (!err)
    vm->slots[r->slot] = slot;
  vm->slot_busy = false;
  spinlock_release(&vm->lock);
  if (err == -EINVAL)
    return -EINVAL;
  return err;
}

/* Dirty bit for page idx of a slot with `pages` pages.  The bitmap is one bit
 * per page, little-endian within each byte. */
static uint8_t dirty_byte(uint64_t pages, uint64_t idx) {
  uint64_t first = idx * 8;
  if (first + 8 <= pages)
    return 0xFF;
  if (first >= pages)
    return 0;
  return (uint8_t)((1u << (pages - first)) - 1);
}

/* KVM_GET_DIRTY_LOG.  AVM does not track guest writes, so every page of a
 * logged slot is reported dirty.  The VMM then copies the whole slot, which is
 * slower than KVM but correct.  The bitmap is sized to whole 64-page words, as
 * KVM does, and the padding bits are zero. */
static int avm_get_dirty_log(struct avm_vm *vm, uint64_t arg) {
  struct kvm_dirty_log log;
  if (copy_from_user(&log, (const void *)arg, sizeof(log)))
    return -EFAULT;
  if (log.slot >= AVM_MAX_SLOTS)
    return -EINVAL;

  spinlock_acquire(&vm->lock);
  struct avm_slot s = vm->slots[log.slot];
  spinlock_release(&vm->lock);
  if (!s.used || !s.log_dirty)
    return -ENOENT;

  uint64_t pages = s.size / AVM_PAGE_SIZE;
  uint64_t bytes = ((pages + 63) / 64) * 8;
  uint8_t buf[64];
  for (uint64_t off = 0; off < bytes; off += sizeof(buf)) {
    uint64_t n = bytes - off < sizeof(buf) ? bytes - off : sizeof(buf);
    for (uint64_t i = 0; i < n; i++)
      buf[i] = dirty_byte(pages, off + i);
    if (copy_to_user((void *)(log.dirty_bitmap + off), buf, n))
      return -EFAULT;
  }
  return 0;
}

static int avm_check_extension(uint64_t cap);

/* KVM_IOEVENTFD: validates the request, then assigns or deassigns it.  The
 * length must be 0 (any) or a power of two up to 8, and datamatch needs a
 * length. */
static int avm_ioeventfd_ioctl(struct avm_vm *vm, const struct kvm_ioeventfd *a) {
  if (a->flags & ~KVM_IOEVENTFD_VALID_FLAG_MASK)
    return -EINVAL;
  bool datamatch = a->flags & KVM_IOEVENTFD_FLAG_DATAMATCH;
  bool pio = a->flags & KVM_IOEVENTFD_FLAG_PIO;
  bool sized = a->len == 1 || a->len == 2 || a->len == 4 || a->len == 8;
  if (datamatch ? !sized : (a->len != 0 && !sized))
    return -EINVAL;
  if (pio && a->addr > 0xFFFF)
    return -EINVAL;
  if (a->flags & KVM_IOEVENTFD_FLAG_DEASSIGN)
    return avm_ioeventfd_deassign(vm, a);
  return avm_ioeventfd_assign(vm, a);
}

/* KVM_ENABLE_CAP on a VM.  No VM capability is enabled here yet, so every one
 * is refused with EINVAL, as KVM refuses a capability it does not support.
 * QEMU ignores the result for KVM_CAP_MAX_VCPU_ID, which it sends without
 * checking. */
static int avm_vm_enable_cap(const struct kvm_enable_cap *cap) {
  (void)cap;
  return -EINVAL;
}

static int avm_vm_ioctl(vfs_node_t *node, uint32_t request, uint64_t arg) {
  struct avm_vm *vm = node->device;
  if (!vm)
    return -EINVAL;

  switch (request) {
  case KVM_CHECK_EXTENSION:
    return avm_check_extension(arg);
  case KVM_ENABLE_CAP: {
    struct kvm_enable_cap cap;
    if (copy_from_user(&cap, (const void *)arg, sizeof(cap)))
      return -EFAULT;
    return avm_vm_enable_cap(&cap);
  }
  case KVM_IOEVENTFD: {
    struct kvm_ioeventfd a;
    if (copy_from_user(&a, (const void *)arg, sizeof(a)))
      return -EFAULT;
    return avm_ioeventfd_ioctl(vm, &a);
  }
  case KVM_CREATE_IRQCHIP: {
    spinlock_acquire(&vm->lock);
    bool already = vm->irqchip;
    vm->irqchip = true;
    spinlock_release(&vm->lock);
    return already ? -EEXIST : 0;
  }
  case KVM_CREATE_PIT2: {
    struct kvm_pit_config cfg;
    if (copy_from_user(&cfg, (const void *)arg, sizeof(cfg)))
      return -EFAULT;
    if (cfg.flags)
      return -EINVAL;
    return avm_irq_create_pit(vm);
  }
  case KVM_IRQ_LINE: {
    struct kvm_irq_level lvl;
    if (copy_from_user(&lvl, (const void *)arg, sizeof(lvl)))
      return -EFAULT;
    return avm_irq_set_line(vm, lvl.irq, lvl.level);
  }
  case KVM_CREATE_VCPU:
    return avm_create_vcpu(vm, arg);
  case KVM_SET_USER_MEMORY_REGION: {
    struct kvm_userspace_memory_region r;
    if (copy_from_user(&r, (const void *)arg, sizeof(r)))
      return -EFAULT;
    return avm_set_memslot(vm, &r);
  }
  case KVM_GET_DIRTY_LOG:
    return avm_get_dirty_log(vm, arg);
  default:
    return avm_vm_misc_ioctl(vm, request, arg);
  }
}

/* KVM_SET_CPUID2 replaces the vCPU's table.  It is read from the VMM before
 * the vCPU runs, so the table is not guarded against a concurrent KVM_RUN. */
static int avm_set_cpuid2(struct avm_vcpu *vcpu, uint64_t arg) {
  struct kvm_cpuid2 hdr;
  if (copy_from_user(&hdr, (const void *)arg, sizeof(hdr)))
    return -EFAULT;
  if (hdr.nent > AVM_MAX_CPUID)
    return -E2BIG;
  if (copy_from_user(vcpu->cpuid, (const void *)(arg + sizeof(hdr)),
                     hdr.nent * sizeof(vcpu->cpuid[0])))
    return -EFAULT;
  vcpu->cpuid_nent = hdr.nent;
  return 0;
}

/* QEMU's MSR buffer is 4096 bytes: an 8-byte header, then 16 bytes per entry.
 * That makes 255 entries the most it ever sends in one call. */
#define AVM_MAX_MSR_BATCH 255

/* KVM_GET_MSRS and KVM_SET_MSRS.  Stops at the first MSR that fails and
 * returns how many were done, as KVM does. */
static int avm_vcpu_msrs(struct avm_vcpu *vcpu, uint64_t arg, bool set) {
  struct kvm_msrs hdr;
  if (copy_from_user(&hdr, (const void *)arg, sizeof(hdr)))
    return -EFAULT;
  if (hdr.nmsrs > AVM_MAX_MSR_BATCH)
    return -E2BIG;

  uint32_t done = 0;
  for (; done < hdr.nmsrs; done++) {
    uint64_t at = arg + sizeof(hdr) + done * sizeof(struct kvm_msr_entry);
    struct kvm_msr_entry e;
    if (copy_from_user(&e, (const void *)at, sizeof(e)))
      return -EFAULT;
    int err = set ? avm_msr_set(vcpu, e.index, e.data)
                  : avm_msr_get(vcpu, e.index, &e.data);
    if (err)
      break;
    if (!set && copy_to_user((void *)at, &e, sizeof(e)))
      return -EFAULT;
  }
  return (int)done;
}

static int avm_vcpu_ioctl(vfs_node_t *node, uint32_t request, uint64_t arg) {
  struct avm_vcpu *vcpu = node->device;
  if (!vcpu)
    return -EINVAL;

  switch (request) {
  case KVM_SET_CPUID2:
    return avm_set_cpuid2(vcpu, arg);
  case KVM_GET_MSRS:
    return avm_vcpu_msrs(vcpu, arg, false);
  case KVM_SET_MSRS:
    return avm_vcpu_msrs(vcpu, arg, true);
  case KVM_SET_REGS:
    if (copy_from_user(&vcpu->regs, (const void *)arg, sizeof(vcpu->regs)))
      return -EFAULT;
    vcpu->state_dirty = true;
    return 0;
  case KVM_SET_SREGS:
    if (copy_from_user(&vcpu->sregs, (const void *)arg, sizeof(vcpu->sregs)))
      return -EFAULT;
    vcpu->state_dirty = true;
    return 0;
  case KVM_RUN:
    return svm_vcpu_run(vcpu);
  default:
    return avm_vcpu_state_ioctl(vcpu, request, arg);
  }
}

/* Reports what AVM really implements.  Anything not listed is 0, so a VMM
 * does not rely on a feature that is missing. */
static int avm_check_extension(uint64_t cap) {
  switch (cap) {
  case KVM_CAP_IRQCHIP:
  case KVM_CAP_USER_MEMORY:
  case KVM_CAP_EXT_CPUID:
  case KVM_CAP_DESTROY_MEMORY_REGION_WORKS:
  case KVM_CAP_JOIN_MEMORY_REGIONS_WORKS:
  case KVM_CAP_INTERNAL_ERROR_DATA:
  case KVM_CAP_SET_GUEST_DEBUG:
  case KVM_CAP_PIT_STATE2:
  case KVM_CAP_X86_ROBUST_SINGLESTEP:
  case KVM_CAP_READONLY_MEM:
  case KVM_CAP_IOEVENTFD:
  case KVM_CAP_IOEVENTFD_ANY_LENGTH:
  case KVM_CAP_SET_TSS_ADDR:
  case KVM_CAP_SET_IDENTITY_MAP_ADDR:
  case KVM_CAP_MP_STATE:
  case KVM_CAP_IRQFD:
  case KVM_CAP_ADJUST_CLOCK:
  case KVM_CAP_VCPU_EVENTS:
  case KVM_CAP_DEBUGREGS:
  case KVM_CAP_XSAVE:
  case KVM_CAP_XCRS:
  case KVM_CAP_GET_TSC_KHZ:
  case KVM_CAP_SIGNAL_MSI:
  case KVM_CAP_IMMEDIATE_EXIT:
  case KVM_CAP_PIT2:
    return 1;
  case KVM_CAP_NR_VCPUS:
  case KVM_CAP_MAX_VCPUS:
    return AVM_MAX_VCPUS;
  case KVM_CAP_NR_MEMSLOTS:
    return AVM_MAX_SLOTS;
  case KVM_CAP_MCE:
    return AVM_MCE_BANKS;
  case KVM_CAP_IRQ_ROUTING:
    /* QEMU takes the number of GSIs as this value minus one. */
    return AVM_MAX_GSI + 1;
  default:
    return 0;
  }
}

/* KVM_GET_SUPPORTED_CPUID reports the default table, which is the most the
 * host can back.  A buffer that is too small gets -E2BIG, and the caller
 * retries with a larger one. */
static int avm_get_supported_cpuid(uint64_t arg) {
  struct kvm_cpuid2 hdr;
  if (copy_from_user(&hdr, (const void *)arg, sizeof(hdr)))
    return -EFAULT;

  struct kvm_cpuid_entry2 *e = kmalloc(AVM_MAX_CPUID * sizeof(*e));
  if (!e)
    return -ENOMEM;
  uint32_t n = avm_cpuid_default(e, 0);
  int err = 0;
  if (hdr.nent < n) {
    err = -E2BIG;
  } else if (copy_to_user((void *)(arg + sizeof(hdr)), e, n * sizeof(*e))) {
    err = -EFAULT;
  } else {
    hdr.nent = n;
    if (copy_to_user((void *)arg, &hdr, sizeof(hdr)))
      err = -EFAULT;
  }
  kfree(e);
  return err;
}

/* The MSR list is the set KVM_GET_MSRS accepts.  The count is written back
 * even when the buffer is too small, so the caller knows what to allocate. */
static int avm_get_msr_index_list(uint64_t arg) {
  struct kvm_msr_list hdr;
  if (copy_from_user(&hdr, (const void *)arg, sizeof(hdr)))
    return -EFAULT;

  uint32_t have = hdr.nmsrs;
  uint32_t n = avm_msr_count();
  int err = 0;
  if (have < n) {
    err = -E2BIG;
  } else if (copy_to_user((void *)(arg + sizeof(hdr)), avm_msr_indices(),
                          n * sizeof(uint32_t))) {
    return -EFAULT;
  }
  hdr.nmsrs = n;
  if (copy_to_user((void *)arg, &hdr, sizeof(hdr)))
    return -EFAULT;
  return err;
}

/* No feature MSRs are reported.  Returning an empty list is the answer the
 * VMM expects when the kernel has none to offer. */
static int avm_get_msr_feature_list(uint64_t arg) {
  struct kvm_msr_list hdr;
  if (copy_from_user(&hdr, (const void *)arg, sizeof(hdr)))
    return -EFAULT;
  hdr.nmsrs = 0;
  if (copy_to_user((void *)arg, &hdr, sizeof(hdr)))
    return -EFAULT;
  return 0;
}

static int avm_dev_ioctl(vfs_node_t *node, uint32_t request, uint64_t arg) {
  (void)node;

  switch (request) {
  case KVM_GET_API_VERSION:
    return KVM_API_VERSION;
  case KVM_CHECK_EXTENSION:
    return avm_check_extension(arg);
  case KVM_GET_SUPPORTED_CPUID:
    return avm_get_supported_cpuid(arg);
  case KVM_GET_MSR_INDEX_LIST:
    return avm_get_msr_index_list(arg);
  case KVM_GET_MSR_FEATURE_INDEX_LIST:
    return avm_get_msr_feature_list(arg);
  case KVM_X86_GET_MCE_CAP_SUPPORTED: {
    uint64_t cap = AVM_MCG_CAP_SUPPORTED;
    if (copy_to_user((void *)arg, &cap, sizeof(cap)))
      return -EFAULT;
    return 0;
  }
  case KVM_GET_VCPU_MMAP_SIZE:
    /* One page holds the kvm_run header and the exit payload. */
    return (int)AVM_PAGE_SIZE;
  case KVM_CREATE_VM: {
    struct avm_vm *vm = kmalloc(sizeof(*vm));
    if (!vm)
      return -ENOMEM;
    memset(vm, 0, sizeof(*vm));
    spinlock_init(&vm->lock);
    avm_irq_vm_init(vm);
    vm->refs = 1;
    vm->asid = avm_smp_alloc_asid();
    klog_puts("[AVM] vm asid=");
    klog_uint64(vm->asid);
    klog_puts("\n");
    int err = avm_mem_vm_init(vm);
    if (err) {
      kfree(vm);
      return err;
    }

    vfs_node_t *vm_node = avm_new_node("avm-vm", vm, avm_vm_ioctl, avm_vm_close);
    if (!vm_node) {
      avm_mem_vm_destroy(vm);
      kfree(vm);
      return -ENOMEM;
    }
    return avm_install_fd(vm_node);
  }
  default:
    return -ENOTTY;
  }
}

void avm_init(void) {
  vfs_node_init(&g_avm_node);
  strcpy(g_avm_node.name, "avm");
  g_avm_node.flags = FS_CHARDEV;
  g_avm_node.mask = 0666;
  /* Misc device: major 10, minor 232, as on Linux. */
  g_avm_node.inode = (10u << 8) | 232u;
  g_avm_node.ioctl = avm_dev_ioctl;
  devfs_register_node("avm", &g_avm_node);

  /* /dev/kvm is a link to the AVM device, so tools that open /dev/kvm reach
   * the same ioctl interface. */
  vfs_node_t *dev_dir = vfs_resolve_path("/dev");
  if (dev_dir) {
    vfs_symlink(dev_dir, "kvm", "/dev/avm");
    vfs_close(dev_dir);
  }

  if (svm_supported() && svm_cpu_enable() == 0)
    klog_puts("[AVM] /dev/avm ready; AMD SVM enabled on the boot CPU\n");
  else
    klog_puts("[AVM] /dev/avm registered; SVM not usable on this CPU\n");
}
