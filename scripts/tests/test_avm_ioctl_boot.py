#!/usr/bin/env python3
"""Check the /dev/avm ioctl surface from a booted AvoryOS init.

Phase 1 scope: the system, VM and vCPU ioctls, memslot validation, the
kvm_run page, and one flat real-mode guest run through KVM_RUN.
"""
import os
from boot_probe import run_probe

INIT = r'''
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <unistd.h>

/* ABI subset copied from the KVM uapi, as in src/avm/avm_uapi.h. */
#define KVMIO 0xAE
#define KVM_IOC(dir, type, nr, size) \
  (((unsigned)(dir) << 30) | ((unsigned)(size) << 16) | ((unsigned)(type) << 8) | (unsigned)(nr))
#define KVM_GET_API_VERSION KVM_IOC(0, KVMIO, 0x00, 0)
#define KVM_CREATE_VM KVM_IOC(0, KVMIO, 0x01, 0)
#define KVM_GET_VCPU_MMAP_SIZE KVM_IOC(0, KVMIO, 0x04, 0)
#define KVM_CHECK_EXTENSION KVM_IOC(0, KVMIO, 0x03, 0)
#define KVM_CAP_IRQCHIP 0
#define KVM_CAP_DESTROY_MEMORY_REGION_WORKS 21
#define KVM_CAP_JOIN_MEMORY_REGIONS_WORKS 30
#define KVM_CAP_INTERNAL_ERROR_DATA 40
#define KVM_CAP_USER_MEMORY 3
#define KVM_CAP_NR_VCPUS 9
#define KVM_CAP_IMMEDIATE_EXIT 136
#define KVM_CREATE_VCPU KVM_IOC(0, KVMIO, 0x41, 0)
#define KVM_SET_USER_MEMORY_REGION KVM_IOC(1, KVMIO, 0x46, 32)
#define KVM_GET_DIRTY_LOG KVM_IOC(1, KVMIO, 0x42, 16)
#define KVM_RUN KVM_IOC(0, KVMIO, 0x80, 0)
#define KVM_SET_REGS KVM_IOC(1, KVMIO, 0x82, 144)
#define KVM_SET_SREGS KVM_IOC(1, KVMIO, 0x84, 312)
#define KVM_TPR_ACCESS_REPORTING KVM_IOC(3, KVMIO, 0x92, 40)
#define KVM_SET_VAPIC_ADDR KVM_IOC(1, KVMIO, 0x93, 8)
struct tpr_access_ctl { uint32_t enabled, flags; uint32_t reserved[8]; };
struct vapic_addr { uint64_t vapic_addr; };

#define KVM_EXIT_IO 2
#define KVM_EXIT_HLT 5
#define KVM_EXIT_IO_OUT 1

struct region { uint32_t slot, flags; uint64_t gpa, size, hva; };
struct regs {
  uint64_t rax, rbx, rcx, rdx, rsi, rdi, rsp, rbp;
  uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
  uint64_t rip, rflags;
};
struct segment {
  uint64_t base; uint32_t limit; uint16_t selector;
  uint8_t type, present, dpl, db, s, l, g, avl, unusable, padding;
};
struct dtable { uint64_t base; uint16_t limit; uint16_t padding[3]; };
struct sregs {
  struct segment cs, ds, es, fs, gs, ss, tr, ldt;
  struct dtable gdt, idt;
  uint64_t cr0, cr2, cr3, cr4, cr8, efer, apic_base, interrupt_bitmap[4];
};
struct run {
  uint8_t request_interrupt_window, immediate_exit, pad1[6];
  uint32_t exit_reason;
  uint8_t ready_for_interrupt_injection, if_flag;
  uint16_t flags;
  uint64_t cr8, apic_base;
  union {
    struct { uint8_t direction, size; uint16_t port; uint32_t count; uint64_t data_offset; } io;
    struct { uint64_t hardware_exit_reason; } hw;
  };
};

static int fails;
#define CHECK(cond, what) do { \
  if (cond) printf("AVM_IOCTL_BOOT: ok   %s\n", what); \
  else { printf("AVM_IOCTL_BOOT: FAIL %s (errno %d)\n", what, errno); fails++; } \
} while (0)

int main(void) {
  setbuf(stdout, NULL);
  puts("AVM_IOCTL_BOOT: START");

  int kvm = open("/dev/avm", O_RDWR);
  CHECK(kvm >= 0, "open /dev/avm");
  int kvm_link = open("/dev/kvm", O_RDWR);
  CHECK(kvm_link >= 0, "/dev/kvm symlink opens the same device");
  if (kvm_link >= 0) close(kvm_link);
  if (kvm < 0) goto done;

  CHECK(ioctl(kvm, KVM_GET_API_VERSION, 0) == 12, "KVM_GET_API_VERSION == 12");
  CHECK(ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0) == 4096, "KVM_GET_VCPU_MMAP_SIZE == 4096");
  CHECK(ioctl(kvm, KVM_CHECK_EXTENSION, KVM_CAP_USER_MEMORY) == 1, "KVM_CAP_USER_MEMORY reported");
  CHECK(ioctl(kvm, KVM_CHECK_EXTENSION, KVM_CAP_IRQCHIP) == 1, "KVM_CAP_IRQCHIP reported");
  CHECK(ioctl(kvm, KVM_CHECK_EXTENSION, KVM_CAP_NR_VCPUS) == 8, "KVM_CAP_NR_VCPUS is 8");
  CHECK(ioctl(kvm, KVM_CHECK_EXTENSION, KVM_CAP_DESTROY_MEMORY_REGION_WORKS) == 1, "KVM_CAP_DESTROY_MEMORY_REGION_WORKS reported");
  CHECK(ioctl(kvm, KVM_CHECK_EXTENSION, KVM_CAP_JOIN_MEMORY_REGIONS_WORKS) == 1, "KVM_CAP_JOIN_MEMORY_REGIONS_WORKS reported");
  CHECK(ioctl(kvm, KVM_CHECK_EXTENSION, KVM_CAP_INTERNAL_ERROR_DATA) == 1, "KVM_CAP_INTERNAL_ERROR_DATA reported");
  CHECK(ioctl(kvm, KVM_CHECK_EXTENSION, 1000) == 0, "unknown cap reports 0");
  CHECK(ioctl(kvm, 0xDEAD, 0) == -1 && errno == ENOTTY, "unknown system ioctl is ENOTTY");

  int vm = ioctl(kvm, KVM_CREATE_VM, 0);
  CHECK(vm >= 0, "KVM_CREATE_VM returns a VM fd");
  if (vm < 0) goto done;

  void *mem = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(mem != MAP_FAILED, "guest RAM host pages");
  if (mem == MAP_FAILED) goto done;

  struct region ok = { .slot = 0, .gpa = 0, .size = 4096, .hva = (uint64_t)mem };
  CHECK(ioctl(vm, KVM_SET_USER_MEMORY_REGION, &ok) == 0, "memslot 0 at GPA 0");

  /* Two pages at 2 MB, which needs its own page-directory entry. */
  struct region high = { .slot = 1, .gpa = 0x200000, .size = 8192, .hva = (uint64_t)mem + 4096 };
  CHECK(ioctl(vm, KVM_SET_USER_MEMORY_REGION, &high) == 0, "memslot 1 at GPA 2 MB");

  struct region overlap = { .slot = 1, .gpa = 0, .size = 4096, .hva = (uint64_t)mem };
  CHECK(ioctl(vm, KVM_SET_USER_MEMORY_REGION, &overlap) == -1 && errno == EEXIST,
        "overlapping memslot is EEXIST");

  struct region misaligned = { .slot = 2, .gpa = 0x100000, .size = 0x800, .hva = (uint64_t)mem };
  CHECK(ioctl(vm, KVM_SET_USER_MEMORY_REGION, &misaligned) == -1 && errno == EINVAL,
        "unaligned memslot size is EINVAL");

  /* Dirty logging is accepted.  AVM reports every page of a logged slot as
   * dirty, so one page gives bit 0 set and nothing else. */
  void *dmem = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(dmem != MAP_FAILED, "dirty-log host page");
  struct region logged = { .slot = 3, .flags = 1, .gpa = 0x400000, .size = 4096, .hva = (uint64_t)dmem };
  CHECK(ioctl(vm, KVM_SET_USER_MEMORY_REGION, &logged) == 0, "dirty-logged memslot is accepted");
  uint8_t bitmap[64] = {0};
  struct { uint32_t slot, pad; uint64_t bitmap; } dl = { .slot = 3, .bitmap = (uint64_t)bitmap };
  CHECK(ioctl(vm, KVM_GET_DIRTY_LOG, &dl) == 0 && bitmap[0] == 1 && bitmap[1] == 0,
        "dirty log reports the one page as dirty");
  struct { uint32_t slot, pad; uint64_t bitmap; } dl0 = { .slot = 0, .bitmap = (uint64_t)bitmap };
  CHECK(ioctl(vm, KVM_GET_DIRTY_LOG, &dl0) == -1 && errno == ENOENT,
        "dirty log of a slot without logging is ENOENT");
  struct region badflags = { .slot = 4, .flags = 4, .gpa = 0x500000, .size = 4096, .hva = (uint64_t)dmem };
  CHECK(ioctl(vm, KVM_SET_USER_MEMORY_REGION, &badflags) == -1 && errno == EINVAL,
        "unknown memslot flag is EINVAL");

  int vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
  CHECK(vcpu >= 0, "KVM_CREATE_VCPU 0 returns a vCPU fd");
  CHECK(ioctl(vm, KVM_CREATE_VCPU, 0) == -1 && errno == EEXIST, "duplicate vCPU id is EEXIST");

  /* QEMU sets the VAPIC address to 0 unconditionally, which must succeed.
   * A real page and enabling TPR reports are refused. */
  struct vapic_addr va = { .vapic_addr = 0 };
  CHECK(ioctl(vcpu, KVM_SET_VAPIC_ADDR, &va) == 0, "KVM_SET_VAPIC_ADDR 0 is accepted");
  va.vapic_addr = 0x1000;
  CHECK(ioctl(vcpu, KVM_SET_VAPIC_ADDR, &va) == -1 && errno == EINVAL,
        "KVM_SET_VAPIC_ADDR to a real page is EINVAL");
  struct tpr_access_ctl tpr;
  memset(&tpr, 0, sizeof(tpr));
  CHECK(ioctl(vcpu, KVM_TPR_ACCESS_REPORTING, &tpr) == 0, "TPR reporting off is accepted");
  tpr.enabled = 1;
  CHECK(ioctl(vcpu, KVM_TPR_ACCESS_REPORTING, &tpr) == -1 && errno == EINVAL,
        "TPR reporting on is EINVAL");

  /* Flat real-mode guest at GPA 0:
   *   mov dx, 0x3f8 ; mov al, 'K' ; out dx, al ; hlt */
  static const uint8_t guest[] = {0xBA, 0xF8, 0x03, 0xB0, 'K', 0xEE, 0xF4};
  memcpy(mem, guest, sizeof(guest));

  struct regs r;
  memset(&r, 0, sizeof(r));
  r.rip = 0;
  r.rflags = 2;
  CHECK(ioctl(vcpu, KVM_SET_REGS, &r) == 0, "KVM_SET_REGS");

  struct sregs s;
  memset(&s, 0, sizeof(s));
  s.cs = (struct segment){ .limit = 0xFFFF, .type = 0xB, .present = 1, .s = 1 };
  s.ds = s.es = s.ss = s.fs = s.gs =
      (struct segment){ .limit = 0xFFFF, .type = 0x3, .present = 1, .s = 1 };
  s.tr = (struct segment){ .limit = 0xFFFF, .type = 0xB, .present = 1 };
  s.ldt = (struct segment){ .unusable = 1 };
  s.gdt.limit = 0xFFFF;
  s.idt.limit = 0xFFFF;
  s.cr0 = 0x10;
  CHECK(ioctl(vcpu, KVM_SET_SREGS, &s) == 0, "KVM_SET_SREGS");

  struct run *run = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
  CHECK(run != MAP_FAILED, "mmap the vCPU kvm_run page");
  if (run == MAP_FAILED) goto done;

  int guest_k = 0, guest_halted = 0;
  for (int exits = 0; exits < 8 && !guest_halted; exits++) {
    if (ioctl(vcpu, KVM_RUN, 0) != 0) {
      printf("AVM_IOCTL_BOOT: KVM_RUN failed, errno %d\n", errno);
      break;
    }
    if (run->exit_reason == KVM_EXIT_IO && run->io.direction == KVM_EXIT_IO_OUT &&
        run->io.port == 0x3F8 && run->io.size == 1) {
      char c = *((char *)run + run->io.data_offset);
      printf("AVM_IOCTL_BOOT: guest wrote '%c' to COM1\n", c);
      guest_k = (c == 'K');
    } else if (run->exit_reason == KVM_EXIT_HLT) {
      guest_halted = 1;
    } else {
      printf("AVM_IOCTL_BOOT: unexpected exit %u\n", run->exit_reason);
      break;
    }
  }
  CHECK(guest_k, "guest wrote K to COM1 through KVM_EXIT_IO");
  CHECK(guest_halted, "guest halted through KVM_EXIT_HLT");

done:
  if (fails == 0) puts("AVM_IOCTL_BOOT: PASS");
  else printf("AVM_IOCTL_BOOT: FAIL %d checks\n", fails);
  reboot(RB_POWER_OFF);
  for (;;) pause();
}
'''

out = run_probe('avm-ioctl-boot', INIT, 'AVM_IOCTL_BOOT: PASS',
                accel=os.environ.get('AVORY_TEST_ACCEL', 'kvm'), timeout=120)

# The probe maps one memslot.  The kernel must report that every NPT entry
# matches the VMM's own page table, and must not report a mismatch.
serial = (out / 'serial.log').read_text(errors='replace')
if 'npt verify FAILED' in serial:
    raise SystemExit('AVM_IOCTL_BOOT: FAIL NPT verification line missing or failed')
print('AVM_IOCTL_BOOT: NPT verification ok')
