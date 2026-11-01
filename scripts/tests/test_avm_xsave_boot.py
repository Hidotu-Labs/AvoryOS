#!/usr/bin/env python3
"""Check that a VMM-supplied XSAVE area cannot fault the host's XRSTOR.

QEMU sends KVM_SET_XSAVE with its own state before the first KVM_RUN.  The
area here is the worst case: every byte set, so XSTATE_BV names components
the guest does not have, XCOMP_BV requests the compacted format, the header's
reserved bytes are set, and MXCSR has reserved bits.  Before the fix, the
first entry raised #GP in XRSTOR and panicked the host.  Now the entry works,
and the guest's one-instruction HLT is seen by the VMM.
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

#define KVMIO 0xAE
#define KVM_IOC(dir, type, nr, size) \
  (((unsigned)(dir) << 30) | ((unsigned)(size) << 16) | ((unsigned)(type) << 8) | (unsigned)(nr))
#define KVM_CREATE_VM KVM_IOC(0, KVMIO, 0x01, 0)
#define KVM_CREATE_VCPU KVM_IOC(0, KVMIO, 0x41, 0)
#define KVM_SET_USER_MEMORY_REGION KVM_IOC(1, KVMIO, 0x46, 32)
#define KVM_RUN KVM_IOC(0, KVMIO, 0x80, 0)
#define KVM_SET_REGS KVM_IOC(1, KVMIO, 0x82, 144)
#define KVM_SET_SREGS KVM_IOC(1, KVMIO, 0x84, 312)
#define KVM_SET_XSAVE KVM_IOC(1, KVMIO, 0xA5, 4096)
#define KVM_GET_XSAVE KVM_IOC(2, KVMIO, 0xA4, 4096)
#define KVM_EXIT_HLT 5

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
};

int main(void) {
  setbuf(stdout, NULL);
  puts("AVM_XSAVE_BOOT: START");
  int avm = open("/dev/avm", O_RDWR);
  if (avm < 0) goto fail;
  int vm = ioctl(avm, KVM_CREATE_VM, 0);
  if (vm < 0) goto fail;
  uint8_t *mem = mmap(NULL, 4 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) goto fail;
  struct region r = { .slot = 0, .gpa = 0, .size = 4 * 4096, .hva = (uint64_t)mem };
  if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &r) != 0) goto fail;
  int vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
  if (vcpu < 0) goto fail;

  /* One instruction, hlt, at 0x1000. */
  mem[0x1000] = 0xF4;

  /* The worst-case VMM area: every byte set. */
  uint8_t xsave[4096];
  memset(xsave, 0xFF, sizeof(xsave));
  if (ioctl(vcpu, KVM_SET_XSAVE, xsave) != 0) {
    printf("AVM_XSAVE_BOOT: SET_XSAVE failed, errno %d\n", errno);
    goto fail;
  }

  struct regs rg;
  memset(&rg, 0, sizeof(rg));
  rg.rip = 0x1000;
  rg.rflags = 2;
  if (ioctl(vcpu, KVM_SET_REGS, &rg) != 0) goto fail;
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
  if (ioctl(vcpu, KVM_SET_SREGS, &s) != 0) goto fail;

  struct run *run = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
  if (run == MAP_FAILED) goto fail;
  if (ioctl(vcpu, KVM_RUN, 0) != 0) {
    printf("AVM_XSAVE_BOOT: KVM_RUN failed, errno %d\n", errno);
    goto fail;
  }
  int halted = run->exit_reason == KVM_EXIT_HLT;
  printf("AVM_XSAVE_BOOT: %s   guest HLT seen after the worst-case XSAVE area\n",
         halted ? "ok  " : "FAIL");

  /* The area is cleaned: no compacted format, and the reserved header bytes
   * are clear.  GET_XSAVE shows what the guest will see. */
  uint8_t out[4096];
  if (ioctl(vcpu, KVM_GET_XSAVE, out) != 0) goto fail;
  int clean = out[520] == 0 && out[521] == 0 && out[528] == 0 && out[575] == 0;
  printf("AVM_XSAVE_BOOT: %s   compacted format and reserved header cleared\n",
         clean ? "ok  " : "FAIL");

  if (halted && clean) puts("AVM_XSAVE_BOOT: PASS");
  else puts("AVM_XSAVE_BOOT: FAIL");
  reboot(RB_POWER_OFF);
  for (;;) pause();
fail:
  printf("AVM_XSAVE_BOOT: FAIL setup, errno %d\n", errno);
  reboot(RB_POWER_OFF);
  for (;;) pause();
}
'''

out = run_probe('avm-xsave-boot', INIT, 'AVM_XSAVE_BOOT: PASS',
                accel=os.environ.get('AVORY_TEST_ACCEL', 'kvm'), timeout=120)
