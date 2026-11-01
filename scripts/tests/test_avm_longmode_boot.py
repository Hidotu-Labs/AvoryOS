#!/usr/bin/env python3
"""Check the guest-visible MSRs and the SYSCALL path in a long-mode guest.

The VMM starts the guest in long mode with identity paging (2 MB pages).  The
guest assembly is in avm_longmode_guest.asm.  Expected output: 'PPPPPS', then a
HLT.  The probe also checks, from the VMM side, that invalid values are refused
and that the values the guest left behind read back through KVM_GET_MSRS.
"""
import os
import subprocess
import tempfile
from pathlib import Path

from boot_probe import run_probe

HERE = Path(__file__).resolve().parent


def assemble_guest():
    out = Path(tempfile.mkdtemp(prefix='avm-longmode-')) / 'guest.bin'
    subprocess.run(['nasm', '-f', 'bin', '-o', str(out),
                    str(HERE / 'avm_longmode_guest.asm')], check=True)
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
#define KVM_CREATE_VCPU KVM_IOC(0, KVMIO, 0x41, 0)
#define KVM_SET_USER_MEMORY_REGION KVM_IOC(1, KVMIO, 0x46, 32)
#define KVM_RUN KVM_IOC(0, KVMIO, 0x80, 0)
#define KVM_SET_REGS KVM_IOC(1, KVMIO, 0x82, 144)
#define KVM_SET_SREGS KVM_IOC(1, KVMIO, 0x84, 312)
#define KVM_GET_MSRS KVM_IOC(3, KVMIO, 0x88, 8)
#define KVM_SET_MSRS KVM_IOC(1, KVMIO, 0x89, 8)
#define KVM_EXIT_IO 2
#define KVM_EXIT_HLT 5
#define KVM_EXIT_IO_OUT 1

#define GUEST_ORG 0x1000
#define PML4_GPA 0x2000
#define PDPT_GPA 0x3000
#define PD_GPA 0x4000
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
struct msr_entry { uint32_t index, reserved; uint64_t data; };
struct msrs { uint32_t nmsrs, pad; struct msr_entry e[8]; };
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
  if (cond) printf("AVM_LONGMODE_BOOT: ok   %s\n", what); \
  else { printf("AVM_LONGMODE_BOOT: FAIL %s (errno %d)\n", what, errno); fails++; } \
} while (0)

/* Reads one MSR through KVM_GET_MSRS.  Returns the value, or 0 on failure. */
static uint64_t get_msr(int vcpu, uint32_t index, int *ok) {
  struct msrs m;
  memset(&m, 0, sizeof(m));
  m.nmsrs = 1;
  m.e[0].index = index;
  *ok = ioctl(vcpu, KVM_GET_MSRS, &m) == 1;
  return m.e[0].data;
}

int main(void) {
  setbuf(stdout, NULL);
  puts("AVM_LONGMODE_BOOT: START");

  int avm = open("/dev/avm", O_RDWR);
  if (avm < 0) goto fail;
  int vm = ioctl(avm, KVM_CREATE_VM, 0);
  if (vm < 0) goto fail;

  uint8_t *mem = mmap(NULL, 16 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) goto fail;
  struct region r = { .slot = 0, .gpa = 0, .size = 16 * 4096, .hva = (uint64_t)mem };
  if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &r) != 0) goto fail;
  int vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
  if (vcpu < 0) goto fail;

  /* Identity map of the first 2 MB, with one 2 MB page. */
  *(uint64_t *)(mem + PML4_GPA) = PDPT_GPA | 0x3;
  *(uint64_t *)(mem + PDPT_GPA) = PD_GPA | 0x3;
  *(uint64_t *)(mem + PD_GPA) = 0x83;
  memcpy(mem + GUEST_ORG, guest, sizeof(guest));

  struct regs rg;
  memset(&rg, 0, sizeof(rg));
  rg.rip = GUEST_ORG;
  rg.rflags = 2;
  rg.rsp = 0x8000;
  if (ioctl(vcpu, KVM_SET_REGS, &rg) != 0) goto fail;

  struct sregs s;
  memset(&s, 0, sizeof(s));
  s.cs = (struct segment){ .selector = 0x08, .limit = 0xFFFFFFFF, .type = 0xB,
                           .present = 1, .s = 1, .l = 1, .g = 1 };
  s.ds = s.es = s.ss = s.fs = s.gs =
      (struct segment){ .selector = 0x10, .limit = 0xFFFFFFFF, .type = 0x3,
                        .present = 1, .s = 1, .g = 1 };
  s.tr = (struct segment){ .selector = 0x18, .limit = 0xFFFF, .type = 0xB, .present = 1 };
  s.ldt = (struct segment){ .unusable = 1 };
  s.gdt.limit = 0xFFFF;
  s.idt.limit = 0xFFFF;
  s.cr0 = 0x80000011;   /* PG | PE */
  s.cr3 = PML4_GPA;
  s.cr4 = 0x20;         /* PAE */
  s.efer = 0x500;       /* LME | LMA */
  if (ioctl(vcpu, KVM_SET_SREGS, &s) != 0) goto fail;

  /* The GS base and KernelGSBase, set through the MSR path after SREGS. */
  struct msrs set;
  memset(&set, 0, sizeof(set));
  set.nmsrs = 2;
  set.e[0].index = 0xC0000101; set.e[0].data = 0x12340000;  /* GS base */
  set.e[1].index = 0xC0000102; set.e[1].data = 0x56780000;  /* KernelGSBase */
  CHECK(ioctl(vcpu, KVM_SET_MSRS, &set) == 2, "VMM sets GS base and KernelGSBase");

  /* Invalid values the VMM may not set: a non-canonical LSTAR, and an EFER
   * bit we do not support (FFXSR).  Each batch stops at the first refusal. */
  struct msrs bad;
  memset(&bad, 0, sizeof(bad));
  bad.nmsrs = 1;
  bad.e[0].index = 0xC0000082; bad.e[0].data = 0x0000800000000000ull;
  CHECK(ioctl(vcpu, KVM_SET_MSRS, &bad) == 0, "non-canonical LSTAR is refused");
  bad.e[0].index = 0xC0000080; bad.e[0].data = 0x500 | (1ull << 14);
  CHECK(ioctl(vcpu, KVM_SET_MSRS, &bad) == 0, "EFER with FFXSR is refused");

  struct run *run = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
  if (run == MAP_FAILED) goto fail;

  char out[16];
  int n = 0, halted = 0;
  for (int exits = 0; exits < 64 && !halted; exits++) {
    if (ioctl(vcpu, KVM_RUN, 0) != 0) {
      printf("AVM_LONGMODE_BOOT: KVM_RUN failed, errno %d\n", errno);
      break;
    }
    if (run->exit_reason == KVM_EXIT_IO && run->io.direction == KVM_EXIT_IO_OUT &&
        run->io.port == 0x3F8 && run->io.size == 1) {
      if (n < (int)sizeof(out) - 1) out[n++] = *((char *)run + run->io.data_offset);
    } else if (run->exit_reason == KVM_EXIT_HLT) {
      halted = 1;
    } else {
      printf("AVM_LONGMODE_BOOT: unexpected exit %u, hw reason 0x%llx\n", run->exit_reason,
             (unsigned long long)run->hw.hardware_exit_reason);
      break;
    }
  }
  out[n] = 0;
  printf("AVM_LONGMODE_BOOT: guest output %s\n", out);
  CHECK(strcmp(out, "PPPPPS") == 0 && halted, "guest checks passed and SYSCALL reached LSTAR");

  /* The guest's writes and the hardware swap read back through the VMM. */
  int ok = 0;
  uint64_t lstar = get_msr(vcpu, 0xC0000082, &ok);
  CHECK(ok && (lstar & ~0xFFFull) == GUEST_ORG, "LSTAR left by the guest points into its page");
  uint64_t star = get_msr(vcpu, 0xC0000081, &ok);
  CHECK(ok && star == 0x0000000800000000ull, "STAR left by the guest reads back");
  uint64_t efer = get_msr(vcpu, 0xC0000080, &ok);
  CHECK(ok && (efer & 0x501) == 0x501 && !(efer & 0x1000),
        "EFER has SCE, LME and LMA, and no SVME");
  uint64_t kgs = get_msr(vcpu, 0xC0000102, &ok);
  CHECK(ok && kgs == 0x56780000ull, "KernelGSBase is back to the VMM value after swapgs");

  if (fails == 0) puts("AVM_LONGMODE_BOOT: PASS");
  else printf("AVM_LONGMODE_BOOT: FAIL %d checks\n", fails);
  reboot(RB_POWER_OFF);
  for (;;) pause();
fail:
  printf("AVM_LONGMODE_BOOT: FAIL setup, errno %d\n", errno);
  reboot(RB_POWER_OFF);
  for (;;) pause();
}
'''

INIT = TEMPLATE.replace('@GUEST@', ', '.join(f'0x{b:02X}' for b in GUEST))
out = run_probe('avm-longmode-boot', INIT, 'AVM_LONGMODE_BOOT: PASS',
                accel=os.environ.get('AVORY_TEST_ACCEL', 'kvm'), timeout=180)
serial = (out / 'serial.log').read_text(errors='replace')
if 'npt verify FAILED' in serial:
    raise SystemExit('AVM_LONGMODE_BOOT: FAIL kernel reported a fault during the test')
print('AVM_LONGMODE_BOOT: kernel log checked')
