#!/usr/bin/env python3
"""Check single-step debugging, the machine-check and PIT/IRQCHIP state ioctls.

With KVM_GUESTDBG_SINGLESTEP on, each instruction of avm_singlestep_guest.asm
traps after it runs.  The VMM sees one KVM_EXIT_DEBUG per trap, with the next
RIP.  Breakpoints are refused.  The machine-check and PIT/IRQCHIP round-trips
use the uapi layouts that QEMU uses at startup.
"""
import os
import subprocess
import tempfile
from pathlib import Path

from boot_probe import run_probe

HERE = Path(__file__).resolve().parent


def assemble_guest():
    out = Path(tempfile.mkdtemp(prefix='avm-debug-')) / 'guest.bin'
    subprocess.run(['nasm', '-f', 'bin', '-o', str(out),
                    str(HERE / 'avm_singlestep_guest.asm')], check=True)
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
#define KVM_CHECK_EXTENSION KVM_IOC(0, KVMIO, 0x03, 0)
#define KVM_CREATE_VM KVM_IOC(0, KVMIO, 0x01, 0)
#define KVM_CREATE_IRQCHIP KVM_IOC(0, KVMIO, 0x60, 0)
#define KVM_CREATE_PIT2 KVM_IOC(1, KVMIO, 0x77, 64)
#define KVM_CREATE_VCPU KVM_IOC(0, KVMIO, 0x41, 0)
#define KVM_SET_USER_MEMORY_REGION KVM_IOC(1, KVMIO, 0x46, 32)
#define KVM_RUN KVM_IOC(0, KVMIO, 0x80, 0)
#define KVM_SET_REGS KVM_IOC(1, KVMIO, 0x82, 144)
#define KVM_SET_SREGS KVM_IOC(1, KVMIO, 0x84, 312)
#define KVM_SET_GUEST_DEBUG KVM_IOC(1, KVMIO, 0x9B, 72)
#define KVM_X86_SETUP_MCE KVM_IOC(1, KVMIO, 0x9C, 8)
#define KVM_X86_GET_MCE_CAP_SUPPORTED KVM_IOC(2, KVMIO, 0x9D, 8)
#define KVM_GET_IRQCHIP KVM_IOC(3, KVMIO, 0x62, 520)
#define KVM_SET_IRQCHIP KVM_IOC(2, KVMIO, 0x63, 520)
#define KVM_GET_PIT2 KVM_IOC(2, KVMIO, 0x9F, 112)
#define KVM_SET_PIT2 KVM_IOC(1, KVMIO, 0xA0, 112)
#define KVM_EXIT_IO 2
#define KVM_EXIT_HLT 5
#define KVM_EXIT_DEBUG 4
#define KVM_EXIT_IO_OUT 1
#define KVM_GUESTDBG_ENABLE 1u
#define KVM_GUESTDBG_SINGLESTEP 2u
#define KVM_GUESTDBG_USE_SW_BP 0x10000u

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
    struct { uint32_t exception, pad; uint64_t pc, dr6, dr7; } debug;
    struct { uint64_t hardware_exit_reason; } hw;
  };
};
struct pic_state { uint8_t b[16]; };
struct ioapic_state { uint64_t base; uint32_t ioregsel, id, irr, pad; uint64_t redir[24]; };
struct irqchip { uint32_t chip_id, pad; union { uint8_t dummy[512]; struct pic_state pic; struct ioapic_state ioapic; } chip; };
struct pit_chan { uint32_t count; uint16_t latched; uint8_t cl, sl, st, rs, ws, wl, rw_mode, mode, bcd, gate; int64_t load; };
struct pit2 { struct pit_chan ch[3]; uint32_t flags; uint32_t res[9]; };
struct guest_debug { uint32_t control, pad; uint64_t dr[8]; };

static int pass = 1;

static void check(int ok, const char *what) {
  printf("AVM_DEBUG_BOOT: %s   %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) pass = 0;
}

int main(void) {
  setbuf(stdout, NULL);
  puts("AVM_DEBUG_BOOT: START");

  int avm = open("/dev/avm", O_RDWR);
  if (avm < 0) goto fail;
  int vm = ioctl(avm, KVM_CREATE_VM, 0);
  if (vm < 0) goto fail;
  check(ioctl(vm, KVM_CHECK_EXTENSION, 23) == 1, "KVM_CAP_SET_GUEST_DEBUG reported");
  check(ioctl(vm, KVM_CHECK_EXTENSION, 31) > 0, "KVM_CAP_MCE reports a bank count");
  check(ioctl(vm, KVM_CHECK_EXTENSION, 35) == 1, "KVM_CAP_PIT_STATE2 reported");
  check(ioctl(vm, KVM_CHECK_EXTENSION, 51) == 1, "KVM_CAP_X86_ROBUST_SINGLESTEP reported");
  check(ioctl(vm, KVM_CHECK_EXTENSION, 81) == 1, "KVM_CAP_READONLY_MEM reported");

  uint64_t mce = 0;
  check(ioctl(avm, KVM_X86_GET_MCE_CAP_SUPPORTED, &mce) == 0 && (mce & 0xFF) > 0 && (mce & (1ull << 8)),
        "machine-check capability read (MCG_CTL_P and banks)");
  check(ioctl(vm, KVM_CREATE_IRQCHIP, 0) == 0, "irqchip created");
  struct region r;
  uint8_t *mem = mmap(NULL, 4 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) goto fail;
  r = (struct region){ .slot = 0, .gpa = 0, .size = 4 * 4096, .hva = (uint64_t)mem };
  if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &r) != 0) goto fail;

  struct { uint32_t flags, pad; uint64_t x; } pt;
  memset(&pt, 0, sizeof(pt));
  check(ioctl(vm, KVM_CREATE_PIT2, &pt) == 0, "in-kernel PIT created");

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

  struct guest_debug gd;
  memset(&gd, 0, sizeof(gd));
  check(ioctl(vcpu, KVM_SET_GUEST_DEBUG, &gd) == 0, "guest debug with nothing enabled, as QEMU sets at start");
  gd.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_USE_SW_BP;
  check(ioctl(vcpu, KVM_SET_GUEST_DEBUG, &gd) < 0, "software breakpoints are refused");

  uint64_t mce_set = mce;
  check(ioctl(vcpu, KVM_X86_SETUP_MCE, &mce_set) == 0, "machine-check capability set on the vCPU");
  uint64_t bad = 1ull << 60;
  check(ioctl(vcpu, KVM_X86_SETUP_MCE, &bad) < 0, "an unsupported machine-check capability is refused");

  struct run *run = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
  if (run == MAP_FAILED) goto fail;

  memset(&gd, 0, sizeof(gd));
  gd.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP;
  check(ioctl(vcpu, KVM_SET_GUEST_DEBUG, &gd) == 0, "single-step enabled");

  uint64_t pcs[16];
  int npc = 0, halted = 0;
  char out[8];
  int n = 0;
  for (int exits = 0; exits < 1000 && !halted; exits++) {
    if (ioctl(vcpu, KVM_RUN, 0) != 0) {
      printf("AVM_DEBUG_BOOT: KVM_RUN failed, errno %d\n", errno);
      break;
    }
    if (run->exit_reason == KVM_EXIT_DEBUG) {
      if (npc < 16) pcs[npc++] = run->debug.pc;
    } else if (run->exit_reason == KVM_EXIT_IO && run->io.direction == KVM_EXIT_IO_OUT &&
               run->io.port == 0x3F8) {
      if (n < 7) out[n++] = *((char *)run + run->io.data_offset);
    } else if (run->exit_reason == KVM_EXIT_HLT) {
      halted = 1;
    } else {
      printf("AVM_DEBUG_BOOT: unexpected exit %u\n", run->exit_reason);
      break;
    }
  }
  out[n] = 0;
  static const uint64_t want[] = { 0x1001, 0x1002, 0x1003, 0x1006, 0x1008, 0x1009 };
  int steps_ok = npc == 6;
  for (int i = 0; steps_ok && i < 6; i++)
    steps_ok = pcs[i] == want[i];
  printf("AVM_DEBUG_BOOT: %d debug exits:", npc);
  for (int i = 0; i < npc && i < 16; i++) printf(" %llx", (unsigned long long)pcs[i]);
  printf("\n");
  check(steps_ok, "one debug exit per instruction, with the next RIP");
  check(strcmp(out, "K") == 0 && halted, "the guest finished after stepping");

  gd.control = 0;
  check(ioctl(vcpu, KVM_SET_GUEST_DEBUG, &gd) == 0, "single-step disabled");

  /* PIC, IOAPIC and PIT state, as QEMU saves and restores it. */
  struct irqchip c;
  memset(&c, 0, sizeof(c));
  c.chip_id = 0;
  check(ioctl(vm, KVM_GET_IRQCHIP, &c) == 0, "PIC master read");
  c.chip.pic.b[1] = 0x5A; /* IMR */
  check(ioctl(vm, KVM_SET_IRQCHIP, &c) == 0 && (memset(&c, 0, sizeof(c)), c.chip_id = 0,
        ioctl(vm, KVM_GET_IRQCHIP, &c) == 0) && c.chip.pic.b[1] == 0x5A,
        "PIC mask register round-trips");

  memset(&c, 0, sizeof(c));
  c.chip_id = 2;
  check(ioctl(vm, KVM_GET_IRQCHIP, &c) == 0 && c.chip.ioapic.base == 0xFEC00000ull, "IOAPIC read at its base");
  c.chip.ioapic.redir[3] = 0x25; /* vector 0x25, unmasked */
  check(ioctl(vm, KVM_SET_IRQCHIP, &c) == 0, "IOAPIC redirection entry written");
  memset(&c, 0, sizeof(c));
  c.chip_id = 2;
  check(ioctl(vm, KVM_GET_IRQCHIP, &c) == 0 && c.chip.ioapic.redir[3] == 0x25, "IOAPIC entry read back");
  c.chip_id = 9;
  check(ioctl(vm, KVM_GET_IRQCHIP, &c) < 0, "an unknown interrupt chip is refused");

  struct pit2 p2;
  memset(&p2, 0, sizeof(p2));
  check(ioctl(vm, KVM_GET_PIT2, &p2) == 0 && p2.ch[0].count == 0, "PIT read at reset (not running)");
  p2.ch[0].rw_mode = 3;
  p2.ch[0].mode = 2;
  p2.ch[0].count = 1000;
  check(ioctl(vm, KVM_SET_PIT2, &p2) == 0, "PIT channel 0 set to rate generator");
  memset(&p2, 0, sizeof(p2));
  check(ioctl(vm, KVM_GET_PIT2, &p2) == 0 && p2.ch[0].mode == 2 && p2.ch[0].rw_mode == 3 &&
        p2.ch[0].count > 0 && p2.ch[0].count <= 1000, "PIT channel 0 read back");
  p2.ch[0].rw_mode = 9;
  check(ioctl(vm, KVM_SET_PIT2, &p2) < 0, "an invalid PIT access mode is refused");

  if (pass) puts("AVM_DEBUG_BOOT: PASS");
  else puts("AVM_DEBUG_BOOT: FAIL");
  reboot(RB_POWER_OFF);
  for (;;) pause();

fail:
  printf("AVM_DEBUG_BOOT: FAIL setup, errno %d\n", errno);
  reboot(RB_POWER_OFF);
  for (;;) pause();
}
'''

INIT = TEMPLATE.replace('@GUEST@', ', '.join(f'0x{b:02X}' for b in GUEST))
out = run_probe('avm-debug-boot', INIT, 'AVM_DEBUG_BOOT: PASS',
                accel=os.environ.get('AVORY_TEST_ACCEL', 'kvm'), timeout=180)
serial = (out / 'serial.log').read_text(errors='replace')
if 'npt verify FAILED' in serial:
    raise SystemExit('AVM_DEBUG_BOOT: FAIL kernel reported a fault during the test')
print('AVM_DEBUG_BOOT: kernel log checked')
