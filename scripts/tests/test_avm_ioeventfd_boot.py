#!/usr/bin/env python3
"""Check KVM_IOEVENTFD: matching writes signal an eventfd and never reach the VMM.

The VMM registers three ioeventfds (see avm_ioeventfd_guest.asm): an unfiltered
port, a port that matches only the data 'Y', and an MMIO address.  The guest
writes 'X' to the first, 'N' then 'Y' to the second, 'M' to the MMIO address,
then 'K' to COM1.  Expected: the VMM sees only 'N' and 'K' as exits, and each
eventfd counter reads 1.
"""
import os
import subprocess
import tempfile
from pathlib import Path

from boot_probe import run_probe

HERE = Path(__file__).resolve().parent


def assemble_guest():
    out = Path(tempfile.mkdtemp(prefix='avm-ioeventfd-')) / 'guest.bin'
    subprocess.run(['nasm', '-f', 'bin', '-o', str(out),
                    str(HERE / 'avm_ioeventfd_guest.asm')], check=True)
    return out.read_bytes()


GUEST = assemble_guest()

TEMPLATE = r'''
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <unistd.h>

#define KVMIO 0xAE
#define KVM_IOC(dir, type, nr, size) \
  (((unsigned)(dir) << 30) | ((unsigned)(size) << 16) | ((unsigned)(type) << 8) | (unsigned)(nr))
#define KVM_CHECK_EXTENSION KVM_IOC(0, KVMIO, 0x03, 0)
#define KVM_CREATE_VM KVM_IOC(0, KVMIO, 0x01, 0)
#define KVM_CREATE_IRQCHIP KVM_IOC(0, KVMIO, 0x60, 0)
#define KVM_CREATE_VCPU KVM_IOC(0, KVMIO, 0x41, 0)
#define KVM_IOEVENTFD KVM_IOC(1, KVMIO, 0x79, 64)
#define KVM_SET_USER_MEMORY_REGION KVM_IOC(1, KVMIO, 0x46, 32)
#define KVM_RUN KVM_IOC(0, KVMIO, 0x80, 0)
#define KVM_SET_REGS KVM_IOC(1, KVMIO, 0x82, 144)
#define KVM_SET_SREGS KVM_IOC(1, KVMIO, 0x84, 312)
#define KVM_EXIT_IO 2
#define KVM_EXIT_HLT 5
#define KVM_EXIT_MMIO 6
#define KVM_EXIT_IO_OUT 1
#define KVM_IOEVENTFD_FLAG_DATAMATCH 1u
#define KVM_IOEVENTFD_FLAG_PIO 2u
#define KVM_IOEVENTFD_FLAG_DEASSIGN 4u

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
    struct { uint64_t phys_addr; uint8_t data[8]; uint32_t len; uint8_t is_write; } mmio;
    struct { uint64_t hardware_exit_reason; } hw;
  };
};
struct ioeventfd {
  uint64_t datamatch, addr; uint32_t len; int32_t fd; uint32_t flags; uint8_t pad[36];
};

static int pass = 1;

static void check(int ok, const char *what) {
  printf("AVM_IOEVENTFD_BOOT: %s   %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) pass = 0;
}

static int assign(int vm, uint64_t addr, uint32_t len, int fd, uint32_t flags, uint64_t datamatch) {
  struct ioeventfd a;
  memset(&a, 0, sizeof(a));
  a.addr = addr;
  a.len = len;
  a.fd = fd;
  a.flags = flags;
  a.datamatch = datamatch;
  return ioctl(vm, KVM_IOEVENTFD, &a);
}

static int counter_is_one(int fd) {
  uint64_t v = 0;
  return read(fd, &v, sizeof(v)) == 8 && v == 1;
}

int main(void) {
  setbuf(stdout, NULL);
  puts("AVM_IOEVENTFD_BOOT: START");

  int avm = open("/dev/avm", O_RDWR);
  if (avm < 0) goto fail;
  int vm = ioctl(avm, KVM_CREATE_VM, 0);
  if (vm < 0) goto fail;
  check(ioctl(vm, KVM_CHECK_EXTENSION, 36) == 1, "KVM_CAP_IOEVENTFD reported on the VM fd");
  check(ioctl(vm, KVM_CHECK_EXTENSION, 118) == 0, "multiple address spaces not reported");
  if (ioctl(vm, KVM_CREATE_IRQCHIP, 0) != 0) goto fail;

  uint8_t *mem = mmap(NULL, 4 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) goto fail;
  struct region r = { .slot = 0, .gpa = 0, .size = 4 * 4096, .hva = (uint64_t)mem };
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
  s.ds = s.es = s.ss = s.fs = s.gs =
      (struct segment){ .limit = 0xFFFF, .type = 0x3, .present = 1, .s = 1 };
  s.ds.limit = 0xFFFFFFFF; /* unreal DS, so the 32-bit MMIO address is in range */
  s.tr = (struct segment){ .limit = 0xFFFF, .type = 0xB, .present = 1 };
  s.ldt = (struct segment){ .unusable = 1 };
  s.gdt.limit = 0xFFFF;
  s.idt.limit = 0xFFFF;
  s.cr0 = 0x10;
  if (ioctl(vcpu, KVM_SET_SREGS, &s) != 0) goto fail;

  int efa = eventfd(0, EFD_NONBLOCK);
  int efb = eventfd(0, EFD_NONBLOCK);
  int efc = eventfd(0, EFD_NONBLOCK);
  if (efa < 0 || efb < 0 || efc < 0) goto fail;

  check(assign(vm, 0x600, 1, efa, KVM_IOEVENTFD_FLAG_PIO, 0) == 0, "port 0x600 registered");
  check(assign(vm, 0x601, 1, efb, KVM_IOEVENTFD_FLAG_PIO | KVM_IOEVENTFD_FLAG_DATAMATCH, 'Y') == 0,
        "port 0x601 registered for data 'Y'");
  check(assign(vm, 0xD0000000ull, 1, efc, 0, 0) == 0, "MMIO 0xD0000000 registered");
  check(assign(vm, 0x600, 1, efa, KVM_IOEVENTFD_FLAG_PIO, 0) < 0, "a duplicate registration is refused");
  check(assign(vm, 0x602, 1, efa, KVM_IOEVENTFD_FLAG_PIO | KVM_IOEVENTFD_FLAG_DEASSIGN, 0) < 0,
        "deassigning an unknown registration is refused");
  check(assign(vm, 0x600, 3, efa, KVM_IOEVENTFD_FLAG_PIO, 0) < 0, "an unsupported length is refused");

  struct run *run = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
  if (run == MAP_FAILED) goto fail;

  char out[16];
  int n = 0, halted = 0;
  for (int exits = 0; exits < 1000 && !halted; exits++) {
    if (ioctl(vcpu, KVM_RUN, 0) != 0) {
      printf("AVM_IOEVENTFD_BOOT: KVM_RUN failed, errno %d\n", errno);
      break;
    }
    if (run->exit_reason == KVM_EXIT_IO && run->io.direction == KVM_EXIT_IO_OUT &&
        (run->io.port == 0x3F8 || run->io.port == 0x601) && run->io.size == 1) {
      if (n < (int)sizeof(out) - 1) out[n++] = *((char *)run + run->io.data_offset);
    } else if (run->exit_reason == KVM_EXIT_HLT) {
      halted = 1;
    } else {
      printf("AVM_IOEVENTFD_BOOT: unexpected exit %u\n", run->exit_reason);
      break;
    }
  }
  out[n] = 0;
  printf("AVM_IOEVENTFD_BOOT: VMM saw %s\n", out);
  check(strcmp(out, "NK") == 0 && halted, "the VMM saw only the miss and the final output");
  check(counter_is_one(efa), "port 0x600 eventfd signalled once");
  check(counter_is_one(efb), "port 0x601 eventfd signalled once for the matching data");
  check(counter_is_one(efc), "MMIO eventfd signalled once");

  if (pass) puts("AVM_IOEVENTFD_BOOT: PASS");
  else puts("AVM_IOEVENTFD_BOOT: FAIL");
  reboot(RB_POWER_OFF);
  for (;;) pause();

fail:
  printf("AVM_IOEVENTFD_BOOT: FAIL setup, errno %d\n", errno);
  reboot(RB_POWER_OFF);
  for (;;) pause();
}
'''

INIT = TEMPLATE.replace('@GUEST@', ', '.join(f'0x{b:02X}' for b in GUEST))
out = run_probe('avm-ioeventfd-boot', INIT, 'AVM_IOEVENTFD_BOOT: PASS',
                accel=os.environ.get('AVORY_TEST_ACCEL', 'kvm'), timeout=180)
serial = (out / 'serial.log').read_text(errors='replace')
if 'npt verify FAILED' in serial:
    raise SystemExit('AVM_IOEVENTFD_BOOT: FAIL kernel reported a fault during the test')
print('AVM_IOEVENTFD_BOOT: kernel log checked')
