#!/usr/bin/env python3
"""Check the in-kernel LAPIC: a real-mode guest counts five timer ticks.

Phase 2 scope: x2APIC MSRs, the periodic LAPIC timer, timer delivery through
EVENTINJ, EOI, and the in-kernel wait for a halted guest with IF set.  The
guest is assembled from avm_lapic_guest.asm at test time.
"""
import os
import subprocess
import tempfile
from pathlib import Path

from boot_probe import run_probe

HERE = Path(__file__).resolve().parent


def assemble_guest():
    out = Path(tempfile.mkdtemp(prefix='avm-lapic-')) / 'guest.bin'
    subprocess.run(['nasm', '-f', 'bin', '-o', str(out),
                    str(HERE / 'avm_lapic_guest.asm')], check=True)
    return out.read_bytes()


GUEST = assemble_guest()

TEMPLATE = r'''
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <unistd.h>

/* ABI subset, as in src/avm/avm_uapi.h. */
#define KVMIO 0xAE
#define KVM_IOC(dir, type, nr, size) \
  (((unsigned)(dir) << 30) | ((unsigned)(size) << 16) | ((unsigned)(type) << 8) | (unsigned)(nr))
#define KVM_CREATE_VM KVM_IOC(0, KVMIO, 0x01, 0)
#define KVM_CREATE_IRQCHIP KVM_IOC(0, KVMIO, 0x60, 0)
#define KVM_CREATE_VCPU KVM_IOC(0, KVMIO, 0x41, 0)
#define KVM_SET_USER_MEMORY_REGION KVM_IOC(1, KVMIO, 0x46, 32)
#define KVM_RUN KVM_IOC(0, KVMIO, 0x80, 0)
#define KVM_SET_REGS KVM_IOC(1, KVMIO, 0x82, 144)
#define KVM_SET_SREGS KVM_IOC(1, KVMIO, 0x84, 312)
#define KVM_EXIT_IO 2
#define KVM_EXIT_HLT 5
#define KVM_EXIT_IO_OUT 1

#define GUEST_ORG 0x1000
static const uint8_t guest[] = { @GUEST@ };

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

int main(void) {
  setbuf(stdout, NULL);
  puts("AVM_LAPIC_BOOT: START");

  int avm = open("/dev/avm", O_RDWR);
  if (avm < 0) goto fail;
  int vm = ioctl(avm, KVM_CREATE_VM, 0);
  if (vm < 0) goto fail;

  /* The in-kernel LAPIC needs the irqchip; a second create must fail. */
  if (ioctl(vm, KVM_CREATE_IRQCHIP, 0) != 0) { puts("AVM_LAPIC_BOOT: irqchip create failed"); goto fail; }
  if (ioctl(vm, KVM_CREATE_IRQCHIP, 0) != -1 || errno != EEXIST) { puts("AVM_LAPIC_BOOT: second irqchip not EEXIST"); goto fail; }
  puts("AVM_LAPIC_BOOT: ok   KVM_CREATE_IRQCHIP");

  /* Four host pages: code and IVT at GPA 0, the tick counter at 0x2000. */
  uint8_t *mem = mmap(NULL, 4 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) goto fail;
  struct region r = { .slot = 0, .gpa = 0, .size = 4 * 4096, .hva = (uint64_t)mem };
  if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &r) != 0) { puts("AVM_LAPIC_BOOT: memslot failed"); goto fail; }

  int vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
  if (vcpu < 0) goto fail;

  memcpy(mem + GUEST_ORG, guest, sizeof(guest));

  struct regs rg;
  memset(&rg, 0, sizeof(rg));
  rg.rip = GUEST_ORG;
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

  char out[16];
  int n = 0, halted = 0;
  for (int exits = 0; exits < 1000 && !halted; exits++) {
    if (ioctl(vcpu, KVM_RUN, 0) != 0) {
      printf("AVM_LAPIC_BOOT: KVM_RUN failed, errno %d\n", errno);
      break;
    }
    if (run->exit_reason == KVM_EXIT_IO && run->io.direction == KVM_EXIT_IO_OUT &&
        run->io.port == 0x3F8 && run->io.size == 1) {
      char c = *((char *)run + run->io.data_offset);
      printf("AVM_LAPIC_BOOT: out '%c'\n", c);
      if (n < (int)sizeof(out)) out[n++] = c;
    } else if (run->exit_reason == KVM_EXIT_HLT) {
      halted = 1;
    } else {
      printf("AVM_LAPIC_BOOT: unexpected exit %u, hw reason 0x%llx\n", run->exit_reason,
             (unsigned long long)run->hw.hardware_exit_reason);
      break;
    }
  }

  /* The guest prints the tick count digit, then 'K'.  The count must be at
   * least five; a late tick can make it six or more. */
  int ticks_ok = n >= 2 && out[0] >= '5' && out[0] <= '9' && out[1] == 'K';
  printf("AVM_LAPIC_BOOT: %s   guest counted at least five ticks\n", ticks_ok ? "ok" : "FAIL");
  printf("AVM_LAPIC_BOOT: %s   guest halted after reporting\n", halted ? "ok" : "FAIL");
  if (ticks_ok && halted) puts("AVM_LAPIC_BOOT: PASS");
  else puts("AVM_LAPIC_BOOT: FAIL");
  reboot(RB_POWER_OFF);
  for (;;) pause();

fail:
  printf("AVM_LAPIC_BOOT: FAIL setup, errno %d\n", errno);
  reboot(RB_POWER_OFF);
  for (;;) pause();
}
'''

INIT = TEMPLATE.replace('@GUEST@', ', '.join(f'0x{b:02X}' for b in GUEST))

out = run_probe('avm-lapic-boot', INIT, 'AVM_LAPIC_BOOT: PASS',
                accel=os.environ.get('AVORY_TEST_ACCEL', 'kvm'), timeout=180)
serial = (out / 'serial.log').read_text(errors='replace')
if 'npt verify FAILED' in serial or 'injected #GP' in serial:
    raise SystemExit('AVM_LAPIC_BOOT: FAIL kernel reported a fault during the test')
print('AVM_LAPIC_BOOT: kernel log checked')
