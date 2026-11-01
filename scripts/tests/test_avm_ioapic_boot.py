#!/usr/bin/env python3
"""Check the APIC and IOAPIC over MMIO, and the VMM's MMIO answers.

Phase 2 scope: xAPIC MMIO on the LAPIC, IOAPIC index and data registers, a
pin raised by the VMM through KVM_IRQ_LINE, the LAPIC timer, and an unmapped
MMIO read that goes to the VMM as KVM_EXIT_MMIO.  The guest is assembled from
avm_ioapic_guest.asm at test time.
"""
import os
import re
import subprocess
import tempfile
from pathlib import Path

from boot_probe import run_probe

HERE = Path(__file__).resolve().parent


def assemble_guest():
    out = Path(tempfile.mkdtemp(prefix='avm-ioapic-')) / 'guest.bin'
    subprocess.run(['nasm', '-f', 'bin', '-o', str(out),
                    str(HERE / 'avm_ioapic_guest.asm')], check=True)
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
#define KVM_IRQ_LINE KVM_IOC(1, KVMIO, 0x61, 8)
#define KVM_CREATE_VCPU KVM_IOC(0, KVMIO, 0x41, 0)
#define KVM_SET_USER_MEMORY_REGION KVM_IOC(1, KVMIO, 0x46, 32)
#define KVM_RUN KVM_IOC(0, KVMIO, 0x80, 0)
#define KVM_SET_REGS KVM_IOC(1, KVMIO, 0x82, 144)
#define KVM_SET_SREGS KVM_IOC(1, KVMIO, 0x84, 312)
#define KVM_EXIT_IO 2
#define KVM_EXIT_HLT 5
#define KVM_EXIT_MMIO 6
#define KVM_EXIT_IO_OUT 1

#define GUEST_ORG 0x1000
static const uint8_t guest[] = { @GUEST@ };

struct region { uint32_t slot, flags; uint64_t gpa, size, hva; };
struct irq_level { uint32_t irq, level; };
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
    struct { uint64_t phys_addr; uint8_t data[8]; uint32_t len; uint8_t is_write; } mmio;
    struct { uint64_t hardware_exit_reason; } hw;
  };
};

int main(void) {
  setbuf(stdout, NULL);
  puts("AVM_IOAPIC_BOOT: START");

  int avm = open("/dev/avm", O_RDWR);
  if (avm < 0) goto fail;
  int vm = ioctl(avm, KVM_CREATE_VM, 0);
  if (vm < 0) goto fail;
  if (ioctl(vm, KVM_CREATE_IRQCHIP, 0) != 0) goto fail;

  uint8_t *mem = mmap(NULL, 4 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) goto fail;
  struct region r = { .slot = 0, .gpa = 0, .size = 4 * 4096, .hva = (uint64_t)mem };
  if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &r) != 0) goto fail;

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
  /* "Unreal" DS: real mode with a 4 GB limit, so the guest's 32-bit
   * addresses (a32 prefix) reach the APIC and IOAPIC pages.  With the 64 KB
   * real-mode limit they raise #GP. */
  s.ds.limit = 0xFFFFFFFF;
  if (ioctl(vcpu, KVM_SET_SREGS, &s) != 0) goto fail;

  struct run *run = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
  if (run == MAP_FAILED) goto fail;

  char out[64];
  int n = 0, halted = 0;
  for (int exits = 0; exits < 5000 && !halted; exits++) {
    if (ioctl(vcpu, KVM_RUN, 0) != 0) {
      printf("AVM_IOAPIC_BOOT: KVM_RUN failed, errno %d\n", errno);
      break;
    }
    if (run->exit_reason == KVM_EXIT_IO && run->io.direction == KVM_EXIT_IO_OUT &&
        run->io.port == 0x3F8 && run->io.size == 1) {
      char c = *((char *)run + run->io.data_offset);
      if (n < (int)sizeof(out) - 1) out[n++] = c;
      if (c == 'R') {
        /* The guest is ready: raise IOAPIC pin 16, then drop it. */
        struct irq_level up = { .irq = 16, .level = 1 };
        struct irq_level down = { .irq = 16, .level = 0 };
        if (ioctl(vm, KVM_IRQ_LINE, &up) != 0 || ioctl(vm, KVM_IRQ_LINE, &down) != 0) {
          puts("AVM_IOAPIC_BOOT: KVM_IRQ_LINE failed");
          break;
        }
      }
    } else if (run->exit_reason == KVM_EXIT_IO) {
      printf("AVM_IOAPIC_BOOT: unexpected io dir %u port 0x%x size %u\n",
             run->io.direction, run->io.port, run->io.size);
      break;
    } else if (run->exit_reason == KVM_EXIT_MMIO) {
      if (run->mmio.is_write) {
        printf("AVM_IOAPIC_BOOT: mmio write 0x%llx len %u\n",
               (unsigned long long)run->mmio.phys_addr, run->mmio.len);
      } else if (run->mmio.phys_addr == 0xD0000000ull && run->mmio.len == 4) {
        /* The unmapped device answers 0x5A. */
        uint32_t answer = 0x5A;
        memcpy(run->mmio.data, &answer, sizeof(answer));
      } else {
        printf("AVM_IOAPIC_BOOT: unexpected mmio read 0x%llx\n",
               (unsigned long long)run->mmio.phys_addr);
        break;
      }
    } else if (run->exit_reason == KVM_EXIT_HLT) {
      halted = 1;
    } else {
      printf("AVM_IOAPIC_BOOT: unexpected exit %u, hw reason 0x%llx\n", run->exit_reason,
             (unsigned long long)run->hw.hardware_exit_reason);
      break;
    }
  }
  out[n] = 0;
  printf("AVM_IOAPIC_BOOT: guest output %s\n", out);

  /* Expected: I (APIC ID 0), M (MMIO answered), R (ready), then T<ticks>
   * O<ioapic count>K.  The ticks digit is at least 3 and the IOAPIC count is
   * exactly 1. */
  int ok = n == 8 && strncmp(out, "IMR", 3) == 0 && out[3] == 'T' &&
           out[4] >= '3' && out[4] <= '9' && out[5] == 'O' && out[6] == '1' &&
           out[7] == 'K';
  printf("AVM_IOAPIC_BOOT: %s   LAPIC, IOAPIC and MMIO behaved as expected\n", ok ? "ok" : "FAIL");
  printf("AVM_IOAPIC_BOOT: %s   guest halted after reporting\n", halted ? "ok" : "FAIL");
  if (ok && halted) puts("AVM_IOAPIC_BOOT: PASS");
  else puts("AVM_IOAPIC_BOOT: FAIL");
  reboot(RB_POWER_OFF);
  for (;;) pause();

fail:
  printf("AVM_IOAPIC_BOOT: FAIL setup, errno %d\n", errno);
  reboot(RB_POWER_OFF);
  for (;;) pause();
}
'''

INIT = TEMPLATE.replace('@GUEST@', ', '.join(f'0x{b:02X}' for b in GUEST))

out = run_probe('avm-ioapic-boot', INIT, 'AVM_IOAPIC_BOOT: PASS',
                accel=os.environ.get('AVORY_TEST_ACCEL', 'kvm'), timeout=240)
serial = (out / 'serial.log').read_text(errors='replace')
if 'npt verify FAILED' in serial or 'injected #GP' in serial or 'undecodable' in serial:
    raise SystemExit('AVM_IOAPIC_BOOT: FAIL kernel reported a fault during the test')
print('AVM_IOAPIC_BOOT: kernel log checked')
