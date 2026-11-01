#!/usr/bin/env python3
"""Check string port I/O (INS and OUTS, with and without REP).

The guest (avm_stringio_guest.asm) runs in real mode with paging off.  It does
REP OUTS of "ABC" to port 0x3F8, REP INS of three bytes from port 0x600 into
memory and then REP OUTS of them, a single INS of one byte, and a 5000-byte
REP OUTS to port 0x3F9.  The VMM supplies the INS bytes "xyzq" and reads the
OUTS data.  Expected output on 0x3F8: "ABCxyzqK".  Each exit may carry at most
one run page of data, which is 2048 one-byte elements, so the bulk transfer
must arrive in chunks of 2048 with the last one smaller.
"""
import os
import subprocess
import tempfile
from pathlib import Path

from boot_probe import run_probe

HERE = Path(__file__).resolve().parent


def assemble_guest():
    out = Path(tempfile.mkdtemp(prefix='avm-stringio-')) / 'guest.bin'
    subprocess.run(['nasm', '-f', 'bin', '-o', str(out),
                    str(HERE / 'avm_stringio_guest.asm')], check=True)
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
#define KVM_EXIT_IO_OUT 1

#define GUEST_ORG 0x1000
#define RUN_PAGE_ELEMENTS 2048
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

static int pass = 1;

static void check(int ok, const char *what) {
  printf("AVM_STRINGIO_BOOT: %s   %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) pass = 0;
}

int main(void) {
  setbuf(stdout, NULL);
  puts("AVM_STRINGIO_BOOT: START");

  int avm = open("/dev/avm", O_RDWR);
  if (avm < 0) goto fail;
  int vm = ioctl(avm, KVM_CREATE_VM, 0);
  if (vm < 0) goto fail;
  if (ioctl(vm, KVM_CREATE_IRQCHIP, 0) != 0) goto fail;

  uint8_t *mem = mmap(NULL, 16 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) goto fail;
  struct region r = { .slot = 0, .flags = 0, .gpa = 0, .size = 16 * 4096, .hva = (uint64_t)mem };
  if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &r) != 0) goto fail;

  int vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
  if (vcpu < 0) goto fail;
  memcpy(mem + GUEST_ORG, guest, sizeof(guest));
  memcpy(mem + 0x2000, "ABC", 3);
  struct regs gr;
  memset(&gr, 0, sizeof(gr));
  gr.rip = GUEST_ORG;
  gr.rflags = 2;
  if (ioctl(vcpu, KVM_SET_REGS, &gr) != 0) goto fail;
  struct sregs s;
  memset(&s, 0, sizeof(s));
  s.cs = (struct segment){ .limit = 0xFFFF, .type = 0xB, .present = 1, .s = 1 };
  s.ds = s.es = s.ss = s.fs = s.gs = (struct segment){ .limit = 0xFFFF, .type = 0x3, .present = 1, .s = 1 };
  s.ds.limit = 0xFFFFFFFF; /* unreal segments, so the data is in range */
  s.es.limit = 0xFFFFFFFF;
  s.tr = (struct segment){ .limit = 0xFFFF, .type = 0xB, .present = 1 };
  s.ldt = (struct segment){ .unusable = 1 };
  s.gdt.limit = 0xFFFF;
  s.idt.limit = 0xFFFF;
  s.cr0 = 0x10;
  if (ioctl(vcpu, KVM_SET_SREGS, &s) != 0) goto fail;

  struct run *run = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
  if (run == MAP_FAILED) goto fail;

  /* The VMM's INS source, and what it saw on each output port. */
  const char *in_stream = "xyzq";
  int in_pos = 0;
  char text[16];
  int n = 0, halted = 0;
  uint64_t bulk = 0, bulk_max_count = 0;
  int bulk_zero = 1, bad_size = 0;
  for (int exits = 0; exits < 1000 && !halted; exits++) {
    if (ioctl(vcpu, KVM_RUN, 0) != 0) {
      printf("AVM_STRINGIO_BOOT: KVM_RUN failed, errno %d\n", errno);
      break;
    }
    if (run->exit_reason == KVM_EXIT_IO) {
      uint8_t *data = (uint8_t *)run + run->io.data_offset;
      if (run->io.size != 1) bad_size = 1;
      if (run->io.direction == KVM_EXIT_IO_OUT) {
        for (uint32_t i = 0; i < run->io.count; i++) {
          uint8_t b = data[i * run->io.size];
          if (run->io.port == 0x3F8) {
            if (n < 15) text[n++] = (char)b;
          } else if (run->io.port == 0x3F9) {
            bulk++;
            if (b) bulk_zero = 0;
          }
        }
        if (run->io.port == 0x3F9 && run->io.count > bulk_max_count)
          bulk_max_count = run->io.count;
      } else {
        for (uint32_t i = 0; i < run->io.count; i++)
          data[i * run->io.size] = in_pos < 4 ? (uint8_t)in_stream[in_pos++] : 0;
      }
    } else if (run->exit_reason == KVM_EXIT_HLT) {
      halted = 1;
    } else {
      printf("AVM_STRINGIO_BOOT: unexpected exit %u\n", run->exit_reason);
      break;
    }
  }
  text[n] = 0;
  printf("AVM_STRINGIO_BOOT: guest output %s\n", text);
  check(strcmp(text, "ABCxyzqK") == 0 && halted,
        "REP OUTS, REP INS, and a single INS moved the right bytes");
  check(!bad_size, "every port exit used one-byte elements");
  check(bulk == 5000 && bulk_zero, "5000 bytes of REP OUTS reached the VMM as zeros");
  check(bulk_max_count == RUN_PAGE_ELEMENTS,
        "the bulk transfer was split at one run page of elements per exit");

  if (pass) puts("AVM_STRINGIO_BOOT: PASS");
  else puts("AVM_STRINGIO_BOOT: FAIL");
  reboot(RB_POWER_OFF);
  for (;;) pause();

fail:
  printf("AVM_STRINGIO_BOOT: FAIL setup, errno %d\n", errno);
  reboot(RB_POWER_OFF);
  for (;;) pause();
}
'''

INIT = TEMPLATE.replace('@GUEST@', ', '.join(f'0x{b:02X}' for b in GUEST))
out = run_probe('avm-stringio-boot', INIT, 'AVM_STRINGIO_BOOT: PASS',
                accel=os.environ.get('AVORY_TEST_ACCEL', 'kvm'), timeout=180)
serial = (out / 'serial.log').read_text(errors='replace')
if 'npt verify FAILED' in serial or 'string I/O with paging' in serial:
    raise SystemExit('AVM_STRINGIO_BOOT: FAIL kernel reported a fault during the test')
print('AVM_STRINGIO_BOOT: kernel log checked')
