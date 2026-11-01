#!/usr/bin/env python3
"""Check KVM_IRQFD, KVM_SET_GSI_ROUTING and KVM_SIGNAL_MSI.

The VMM routes GSI 5 to MSI vector 0x50 and creates an irqfd on it.  Signalling
the eventfd must raise vector 0x50 in the guest, and KVM_SIGNAL_MSI must raise
vector 0x51 directly.  The guest (avm_irqfd_guest.asm) writes 'I' for 0x50 and
'M' for 0x51, then 'K'.  Expected: the output is 'K' with 'I' and 'M' before it.
"""
import os
import subprocess
import tempfile
from pathlib import Path

from boot_probe import run_probe

HERE = Path(__file__).resolve().parent


def assemble_guest():
    out = Path(tempfile.mkdtemp(prefix='avm-irqfd-')) / 'guest.bin'
    subprocess.run(['nasm', '-f', 'bin', '-o', str(out),
                    str(HERE / 'avm_irqfd_guest.asm')], check=True)
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
#define KVM_CREATE_VM KVM_IOC(0, KVMIO, 0x01, 0)
#define KVM_CREATE_IRQCHIP KVM_IOC(0, KVMIO, 0x60, 0)
#define KVM_CREATE_VCPU KVM_IOC(0, KVMIO, 0x41, 0)
#define KVM_SET_USER_MEMORY_REGION KVM_IOC(1, KVMIO, 0x46, 32)
#define KVM_SET_GSI_ROUTING KVM_IOC(1, KVMIO, 0x6A, 8)
#define KVM_IRQFD KVM_IOC(1, KVMIO, 0x76, 32)
#define KVM_SIGNAL_MSI KVM_IOC(1, KVMIO, 0xA5, 32)
#define KVM_RUN KVM_IOC(0, KVMIO, 0x80, 0)
#define KVM_SET_REGS KVM_IOC(1, KVMIO, 0x82, 144)
#define KVM_SET_SREGS KVM_IOC(1, KVMIO, 0x84, 312)
#define KVM_EXIT_IO 2
#define KVM_EXIT_HLT 5
#define KVM_EXIT_IO_OUT 1
#define KVM_IRQFD_FLAG_DEASSIGN 1u

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
struct route_entry {
  uint32_t gsi, type, flags, pad;
  union {
    struct { uint32_t lo, hi, data, devid; } msi;
    uint32_t pad8[8];
  } u;
};
struct route { uint32_t nr, flags; struct route_entry e[1]; };
struct irqfd { uint32_t fd, gsi, flags, resamplefd; uint8_t pad[16]; };
struct msi { uint32_t lo, hi, data, flags, devid; uint8_t pad[12]; };

static int pass = 1;

static void check(int ok, const char *what) {
  printf("AVM_IRQFD_BOOT: %s   %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) pass = 0;
}

int main(void) {
  setbuf(stdout, NULL);
  puts("AVM_IRQFD_BOOT: START");

  int avm = open("/dev/avm", O_RDWR);
  if (avm < 0) goto fail;
  int vm = ioctl(avm, KVM_CREATE_VM, 0);
  if (vm < 0) goto fail;
  check(ioctl(vm, KVM_CREATE_IRQCHIP, 0) == 0, "irqchip created");

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
  s.tr = (struct segment){ .limit = 0xFFFF, .type = 0xB, .present = 1 };
  s.ldt = (struct segment){ .unusable = 1 };
  s.gdt.limit = 0xFFFF;
  s.idt.limit = 0xFFFF;
  s.cr0 = 0x10;
  if (ioctl(vcpu, KVM_SET_SREGS, &s) != 0) goto fail;

  /* GSI 5 -> MSI, address 0xFEE00000 (APIC 0), vector 0x50. */
  struct route rt;
  memset(&rt, 0, sizeof(rt));
  rt.nr = 1;
  rt.e[0].gsi = 5;
  rt.e[0].type = 2; /* KVM_IRQ_ROUTING_MSI */
  rt.e[0].u.msi.lo = 0xFEE00000u;
  rt.e[0].u.msi.data = 0x50;
  check(ioctl(vm, KVM_SET_GSI_ROUTING, &rt) == 0, "MSI route for GSI 5 set");

  struct route bad;
  memset(&bad, 0, sizeof(bad));
  bad.nr = 1;
  bad.e[0].gsi = 5;
  bad.e[0].type = 7;
  check(ioctl(vm, KVM_SET_GSI_ROUTING, &bad) < 0, "an unknown route type is refused");

  int efd = eventfd(0, EFD_NONBLOCK);
  int efx = eventfd(0, EFD_NONBLOCK);
  if (efd < 0 || efx < 0) goto fail;
  struct irqfd f;
  memset(&f, 0, sizeof(f));
  f.fd = efd;
  f.gsi = 5;
  check(ioctl(vm, KVM_IRQFD, &f) == 0, "irqfd assigned to GSI 5");
  check(ioctl(vm, KVM_IRQFD, &f) < 0, "a duplicate irqfd is refused");
  struct irqfd g;
  memset(&g, 0, sizeof(g));
  g.fd = efx;
  g.gsi = 5;
  g.flags = KVM_IRQFD_FLAG_DEASSIGN;
  check(ioctl(vm, KVM_IRQFD, &g) < 0, "deassigning an unknown irqfd is refused");

  uint64_t one = 1;
  check(write(efd, &one, sizeof(one)) == 8, "eventfd signalled, which raises vector 0x50");

  struct msi m;
  memset(&m, 0, sizeof(m));
  m.lo = 0xFEE00000u;
  m.data = 0x51;
  check(ioctl(vm, KVM_SIGNAL_MSI, &m) == 1, "KVM_SIGNAL_MSI delivers vector 0x51");

  struct run *run = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
  if (run == MAP_FAILED) goto fail;

  char out[16];
  int n = 0, halted = 0;
  for (int exits = 0; exits < 1000 && !halted; exits++) {
    if (ioctl(vcpu, KVM_RUN, 0) != 0) {
      printf("AVM_IRQFD_BOOT: KVM_RUN failed, errno %d\n", errno);
      break;
    }
    if (run->exit_reason == KVM_EXIT_IO && run->io.direction == KVM_EXIT_IO_OUT &&
        run->io.port == 0x3F8 && run->io.size == 1) {
      if (n < (int)sizeof(out) - 1) out[n++] = *((char *)run + run->io.data_offset);
    } else if (run->exit_reason == KVM_EXIT_HLT) {
      halted = 1;
    } else {
      printf("AVM_IRQFD_BOOT: unexpected exit %u\n", run->exit_reason);
      break;
    }
  }
  out[n] = 0;
  printf("AVM_IRQFD_BOOT: guest output %s\n", out);
  int has_i = strchr(out, 'I') != NULL, has_m = strchr(out, 'M') != NULL;
  check(halted && has_i && has_m && n == 3 && out[2] == 'K',
        "guest took both vectors, then halted");

  if (pass) puts("AVM_IRQFD_BOOT: PASS");
  else puts("AVM_IRQFD_BOOT: FAIL");
  reboot(RB_POWER_OFF);
  for (;;) pause();

fail:
  printf("AVM_IRQFD_BOOT: FAIL setup, errno %d\n", errno);
  reboot(RB_POWER_OFF);
  for (;;) pause();
}
'''

INIT = TEMPLATE.replace('@GUEST@', ', '.join(f'0x{b:02X}' for b in GUEST))
out = run_probe('avm-irqfd-boot', INIT, 'AVM_IRQFD_BOOT: PASS',
                accel=os.environ.get('AVORY_TEST_ACCEL', 'kvm'), timeout=180)
serial = (out / 'serial.log').read_text(errors='replace')
if 'npt verify FAILED' in serial:
    raise SystemExit('AVM_IRQFD_BOOT: FAIL kernel reported a fault during the test')
print('AVM_IRQFD_BOOT: kernel log checked')
