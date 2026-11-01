#!/usr/bin/env python3
"""Check string instructions that touch MMIO, one element per exit.

The guest (avm_stringmmio_guest.asm) runs in real mode with paging off.  Its
REP STOSW writes four words to 0xB8000, which is outside every memslot, so
each element is an MMIO write the VMM sees.  Its REP LODSW reads one word from
that address, and the VMM answers 0x4B4F.  The guest prints "OK" on COM1.
"""
import os
import subprocess
import tempfile
from pathlib import Path

from boot_probe import run_probe

HERE = Path(__file__).resolve().parent


def assemble_guest():
    out = Path(tempfile.mkdtemp(prefix='avm-stringmmio-')) / 'guest.bin'
    subprocess.run(['nasm', '-f', 'bin', '-o', str(out),
                    str(HERE / 'avm_stringmmio_guest.asm')], check=True)
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
#define KVM_EXIT_MMIO 6
#define KVM_EXIT_IO_OUT 1

#define GUEST_ORG 0x1000
#define MMIO_BASE 0xB8000ull
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
    struct { uint64_t phys_addr; uint8_t data[8]; uint32_t len; uint8_t is_write; } mmio;
    struct { uint64_t hardware_exit_reason; } hw;
  };
};

int main(void) {
  setbuf(stdout, NULL);
  puts("AVM_STRINGMMIO_BOOT: START");
  int pass = 1;

  int avm = open("/dev/avm", O_RDWR);
  if (avm < 0) goto fail;
  int vm = ioctl(avm, KVM_CREATE_VM, 0);
  if (vm < 0) goto fail;
  if (ioctl(vm, KVM_CREATE_IRQCHIP, 0) != 0) goto fail;

  /* Guest RAM is 64 KB, so 0xB8000 is outside it and every access there is MMIO. */
  uint8_t *mem = mmap(NULL, 16 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) goto fail;
  struct region r = { .slot = 0, .flags = 0, .gpa = 0, .size = 16 * 4096, .hva = (uint64_t)mem };
  if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &r) != 0) goto fail;

  int vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
  if (vcpu < 0) goto fail;
  memcpy(mem + GUEST_ORG, guest, sizeof(guest));
  struct regs gr;
  memset(&gr, 0, sizeof(gr));
  gr.rip = GUEST_ORG;
  gr.rflags = 2;
  if (ioctl(vcpu, KVM_SET_REGS, &gr) != 0) goto fail;
  struct sregs s;
  memset(&s, 0, sizeof(s));
  s.cs = (struct segment){ .limit = 0xFFFF, .type = 0xB, .present = 1, .s = 1 };
  s.ds = s.es = s.ss = s.fs = s.gs = (struct segment){ .limit = 0xFFFF, .type = 0x3, .present = 1, .s = 1 };
  s.tr = (struct segment){ .limit = 0xFFFF, .type = 0xB, .present = 1 };
  s.ldt = (struct segment){ .unusable = 1 };
  s.gdt.limit = 0xFFFF;
  s.idt.limit = 0xFFFF;
  s.cr0 = 0x10;
  if (ioctl(vcpu, KVM_SET_SREGS, &s) != 0) goto fail;

  struct run *run = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
  if (run == MAP_FAILED) goto fail;

  char text[8];
  int n = 0, halted = 0;
  int writes_ok = 0, writes = 0, reads = 0;
  for (int exits = 0; exits < 100 && !halted; exits++) {
    if (ioctl(vcpu, KVM_RUN, 0) != 0) {
      printf("AVM_STRINGMMIO_BOOT: KVM_RUN failed, errno %d\n", errno);
      break;
    }
    if (run->exit_reason == KVM_EXIT_MMIO) {
      uint64_t off = run->mmio.phys_addr - MMIO_BASE;
      if (run->mmio.is_write) {
        uint16_t word = (uint16_t)(run->mmio.data[0] | (run->mmio.data[1] << 8));
        if (run->mmio.len == 2 && word == 0x0741 && off == (uint64_t)writes * 2)
          writes_ok++;
        writes++;
      } else {
        reads++;
        if (run->mmio.len == 2 && off == 0) {
          run->mmio.data[0] = 0x4F; /* 'O' in AL */
          run->mmio.data[1] = 0x4B; /* 'K' in AH */
        }
      }
    } else if (run->exit_reason == KVM_EXIT_IO && run->io.direction == KVM_EXIT_IO_OUT &&
               run->io.port == 0x3F8) {
      if (n < 7) text[n++] = *((char *)run + run->io.data_offset);
    } else if (run->exit_reason == KVM_EXIT_HLT) {
      halted = 1;
    } else {
      printf("AVM_STRINGMMIO_BOOT: unexpected exit %u\n", run->exit_reason);
      break;
    }
  }
  text[n] = 0;
  printf("AVM_STRINGMMIO_BOOT: guest output %s, writes %d, reads %d\n", text, writes, reads);
  if (writes == 4 && writes_ok == 4)
    printf("AVM_STRINGMMIO_BOOT: ok   REP STOSW made four MMIO writes at 0xB8000\n");
  else {
    printf("AVM_STRINGMMIO_BOOT: FAIL   REP STOSW made %d writes, %d correct\n", writes, writes_ok);
    pass = 0;
  }
  if (strcmp(text, "OK") == 0 && halted && reads == 1)
    printf("AVM_STRINGMMIO_BOOT: ok   REP LODSW read one word through MMIO\n");
  else {
    printf("AVM_STRINGMMIO_BOOT: FAIL   REP LODSW output %s, reads %d\n", text, reads);
    pass = 0;
  }

  if (pass) puts("AVM_STRINGMMIO_BOOT: PASS");
  else puts("AVM_STRINGMMIO_BOOT: FAIL");
  reboot(RB_POWER_OFF);
  for (;;) pause();

fail:
  printf("AVM_STRINGMMIO_BOOT: FAIL setup, errno %d\n", errno);
  reboot(RB_POWER_OFF);
  for (;;) pause();
}
'''

INIT = TEMPLATE.replace('@GUEST@', ', '.join(f'0x{b:02X}' for b in GUEST))
out = run_probe('avm-stringmmio-boot', INIT, 'AVM_STRINGMMIO_BOOT: PASS',
                accel=os.environ.get('AVORY_TEST_ACCEL', 'kvm'), timeout=180)
serial = (out / 'serial.log').read_text(errors='replace')
if 'npt verify FAILED' in serial or 'undecodable MMIO' in serial:
    raise SystemExit('AVM_STRINGMMIO_BOOT: FAIL the kernel could not decode a string MMIO access')
print('AVM_STRINGMMIO_BOOT: kernel log checked')
