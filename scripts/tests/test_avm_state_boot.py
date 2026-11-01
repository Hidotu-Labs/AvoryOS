#!/usr/bin/env python3
"""Check the vCPU and VM state ioctls QEMU uses at startup.

Every capability QEMU asks for must report 1 (multiple address spaces 0).  The
state ioctls are round-tripped before the guest runs: registers, special
registers, FPU, XSAVE, XCRs, LAPIC, MP state, events, debug registers, clock and
TSC frequency.  KVM_RUN returns at once with EINTR for immediate_exit and for a
pending signal the run does not block.  Nothing here enters the guest.
"""
import os
import subprocess
import tempfile
from pathlib import Path

from boot_probe import run_probe

HERE = Path(__file__).resolve().parent


def assemble_guest():
    out = Path(tempfile.mkdtemp(prefix='avm-state-')) / 'guest.bin'
    subprocess.run(['nasm', '-f', 'bin', '-o', str(out),
                    str(HERE / 'avm_xsetbv_guest.asm')], check=True)
    return out.read_bytes()


GUEST = assemble_guest()

TEMPLATE = r'''
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
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
#define KVM_CREATE_VCPU KVM_IOC(0, KVMIO, 0x41, 0)
#define KVM_SET_USER_MEMORY_REGION KVM_IOC(1, KVMIO, 0x46, 32)
#define KVM_SET_TSS_ADDR KVM_IOC(0, KVMIO, 0x47, 0)
#define KVM_SET_IDENTITY_MAP_ADDR KVM_IOC(1, KVMIO, 0x48, 8)
#define KVM_GET_CLOCK KVM_IOC(2, KVMIO, 0x7C, 48)
#define KVM_SET_CLOCK KVM_IOC(1, KVMIO, 0x7B, 48)
#define KVM_RUN KVM_IOC(0, KVMIO, 0x80, 0)
#define KVM_GET_REGS KVM_IOC(2, KVMIO, 0x81, 144)
#define KVM_SET_REGS KVM_IOC(1, KVMIO, 0x82, 144)
#define KVM_GET_SREGS KVM_IOC(2, KVMIO, 0x83, 312)
#define KVM_SET_SREGS KVM_IOC(1, KVMIO, 0x84, 312)
#define KVM_SET_SIGNAL_MASK KVM_IOC(1, KVMIO, 0x8B, 4)
#define KVM_GET_FPU KVM_IOC(2, KVMIO, 0x8C, 416)
#define KVM_SET_FPU KVM_IOC(1, KVMIO, 0x8D, 416)
#define KVM_GET_LAPIC KVM_IOC(2, KVMIO, 0x8E, 1024)
#define KVM_SET_LAPIC KVM_IOC(1, KVMIO, 0x8F, 1024)
#define KVM_GET_MP_STATE KVM_IOC(2, KVMIO, 0x98, 4)
#define KVM_SET_MP_STATE KVM_IOC(1, KVMIO, 0x99, 4)
#define KVM_GET_VCPU_EVENTS KVM_IOC(2, KVMIO, 0x9F, 64)
#define KVM_SET_VCPU_EVENTS KVM_IOC(1, KVMIO, 0xA0, 64)
#define KVM_GET_DEBUGREGS KVM_IOC(2, KVMIO, 0xA1, 128)
#define KVM_SET_DEBUGREGS KVM_IOC(1, KVMIO, 0xA2, 128)
#define KVM_SET_TSC_KHZ KVM_IOC(0, KVMIO, 0xA2, 0)
#define KVM_GET_TSC_KHZ KVM_IOC(0, KVMIO, 0xA3, 0)
#define KVM_GET_XSAVE KVM_IOC(2, KVMIO, 0xA4, 4096)
#define KVM_SET_XSAVE KVM_IOC(1, KVMIO, 0xA5, 4096)
#define KVM_GET_XCRS KVM_IOC(2, KVMIO, 0xA6, 392)
#define KVM_SET_XCRS KVM_IOC(1, KVMIO, 0xA7, 392)
#define KVM_EXIT_INTR 10

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
struct fpu {
  uint8_t fpr[8][16];
  uint16_t fcw, fsw;
  uint8_t ftwx, pad1;
  uint16_t last_opcode;
  uint64_t last_ip, last_dp;
  uint8_t xmm[16][16];
  uint32_t mxcsr, pad2;
};
struct events {
  struct { uint8_t injected, nr, has_error_code, pending; uint32_t error_code; } exception;
  struct { uint8_t injected, nr, soft, shadow; } interrupt;
  struct { uint8_t injected, pending, masked, pad; } nmi;
  uint32_t sipi_vector, flags;
  struct { uint8_t smm, pending, smm_inside_nmi, latched_init; } smi;
  struct { uint8_t pending; } triple_fault;
  uint8_t reserved[26];
  uint8_t exception_has_payload;
  uint64_t exception_payload;
};
struct xcrs { uint32_t nr, flags; struct { uint32_t xcr, reserved; uint64_t value; } x[16]; uint64_t padding[16]; };
struct clock { uint64_t clock; uint32_t flags, pad0; uint64_t realtime, host_tsc; uint32_t pad[4]; };
struct run {
  uint8_t request_interrupt_window, immediate_exit, pad1[6];
  uint32_t exit_reason;
  uint8_t ready_for_interrupt_injection, if_flag;
  uint16_t flags;
  uint64_t cr8, apic_base;
  union { struct { uint64_t hardware_exit_reason; } hw; uint8_t pad[256]; };
};

static int pass = 1;

static void check(int ok, const char *what) {
  printf("AVM_STATE_BOOT: %s   %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) pass = 0;
}

static void on_usr1(int sig) { (void)sig; }

int main(void) {
  setbuf(stdout, NULL);
  puts("AVM_STATE_BOOT: START");

  int avm = open("/dev/avm", O_RDWR);
  if (avm < 0) goto fail;
  int vm = ioctl(avm, KVM_CREATE_VM, 0);
  if (vm < 0) goto fail;

  static const uint32_t caps[] = { 4, 14, 32, 36, 37, 39, 41, 50, 55, 56, 61, 77, 122, 136 };
  int caps_ok = 1;
  for (unsigned i = 0; i < sizeof(caps) / sizeof(caps[0]); i++)
    if (ioctl(vm, KVM_CHECK_EXTENSION, caps[i]) != 1) caps_ok = 0;
  check(caps_ok, "QEMU's required capabilities report 1");
  check(ioctl(vm, KVM_CHECK_EXTENSION, 118) == 0, "multiple address spaces reports 0");
  check(ioctl(vm, KVM_CHECK_EXTENSION, 25) == 1024 + 1, "IRQ routing reports the GSI count plus one");

  check(ioctl(vm, KVM_CREATE_IRQCHIP, 0) == 0, "irqchip created");
  check(ioctl(vm, KVM_SET_TSS_ADDR, 0xfffbd000u) == 0, "TSS address accepted");
  uint64_t idmap = 0xfeffc000u;
  check(ioctl(vm, KVM_SET_IDENTITY_MAP_ADDR, &idmap) == 0, "identity map address accepted");

  struct clock ck;
  memset(&ck, 0, sizeof(ck));
  check(ioctl(vm, KVM_GET_CLOCK, &ck) == 0 && ck.clock != 0, "clock read");
  check(ioctl(vm, KVM_SET_CLOCK, &ck) == 0, "clock set accepted");

  uint8_t *mem = mmap(NULL, 4 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) goto fail;
  struct region r = { .slot = 0, .gpa = 0, .size = 4 * 4096, .hva = (uint64_t)mem };
  if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &r) != 0) goto fail;
  int vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
  if (vcpu < 0) goto fail;
  memcpy(mem + GUEST_ORG, guest, sizeof(guest));
  struct run *run = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
  if (run == MAP_FAILED) goto fail;

  /* The TSC frequency and the immediate and signal exits. */
  int khz = ioctl(vcpu, KVM_GET_TSC_KHZ, 0);
  check(khz > 0, "TSC frequency read");
  check(ioctl(vcpu, KVM_SET_TSC_KHZ, khz) == 0, "the host TSC frequency is accepted");
  check(ioctl(vcpu, KVM_SET_TSC_KHZ, khz + 1) < 0, "another TSC frequency is refused");

  run->immediate_exit = 1;
  int ret = ioctl(vcpu, KVM_RUN, 0);
  check(ret < 0 && errno == EINTR && run->exit_reason == KVM_EXIT_INTR,
        "immediate_exit returns from KVM_RUN with EINTR");
  run->immediate_exit = 0;

  sigset_t blk, all;
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_usr1;
  sigaction(SIGUSR1, &sa, NULL);
  sigemptyset(&blk);
  sigaddset(&blk, SIGUSR1);
  sigprocmask(SIG_BLOCK, &blk, NULL);
  kill(getpid(), SIGUSR1); /* pending, since the thread blocks it */
  struct { uint32_t len; uint8_t set[8]; } sm;
  memset(&sm, 0, sizeof(sm));
  sm.len = 8; /* the run blocks nothing, so the pending signal ends the run */
  check(ioctl(vcpu, KVM_SET_SIGNAL_MASK, &sm) == 0, "signal mask set for KVM_RUN");
  ret = ioctl(vcpu, KVM_RUN, 0);
  check(ret < 0 && errno == EINTR && run->exit_reason == KVM_EXIT_INTR,
        "a pending unblocked signal returns from KVM_RUN with EINTR");
  sm.len = 0;
  ioctl(vcpu, KVM_SET_SIGNAL_MASK, &sm);
  sigemptyset(&all);
  sigaddset(&all, SIGUSR1);
  sigprocmask(SIG_UNBLOCK, &all, NULL);

  /* Registers and special registers. */
  struct regs gr;
  memset(&gr, 0, sizeof(gr));
  gr.rbx = 0x1234;
  gr.rip = GUEST_ORG;
  gr.rflags = 2;
  check(ioctl(vcpu, KVM_SET_REGS, &gr) == 0, "registers set");
  struct regs gg;
  memset(&gg, 0, sizeof(gg));
  check(ioctl(vcpu, KVM_GET_REGS, &gg) == 0 && gg.rbx == 0x1234 && gg.rip == GUEST_ORG,
        "registers read back");

  struct sregs s;
  memset(&s, 0, sizeof(s));
  s.cs = (struct segment){ .limit = 0xFFFF, .type = 0xB, .present = 1, .s = 1 };
  s.ds = s.es = s.ss = s.fs = s.gs = (struct segment){ .limit = 0xFFFF, .type = 0x3, .present = 1, .s = 1 };
  s.tr = (struct segment){ .limit = 0xFFFF, .type = 0xB, .present = 1 };
  s.ldt = (struct segment){ .unusable = 1 };
  s.gdt.limit = 0xFFFF;
  s.idt.limit = 0xFFFF;
  s.cr0 = 0x10;
  check(ioctl(vcpu, KVM_SET_SREGS, &s) == 0, "special registers set");
  struct sregs sg;
  memset(&sg, 0, sizeof(sg));
  check(ioctl(vcpu, KVM_GET_SREGS, &sg) == 0 && sg.cr0 == 0x10 && sg.cs.type == 0xB &&
        sg.cs.limit == 0xFFFF && sg.gdt.limit == 0xFFFF, "special registers read back");

  /* FPU: the legacy part of the XSAVE area, in FXSAVE layout. */
  struct fpu f;
  memset(&f, 0, sizeof(f));
  f.fcw = 0x037F;
  f.mxcsr = 0x1F80;
  f.xmm[0][0] = 0xAB;
  f.fpr[0][0] = 0x5C;
  check(ioctl(vcpu, KVM_SET_FPU, &f) == 0, "FPU set");
  struct fpu fg;
  memset(&fg, 0, sizeof(fg));
  check(ioctl(vcpu, KVM_GET_FPU, &fg) == 0 && fg.fcw == 0x037F && fg.mxcsr == 0x1F80 &&
        fg.xmm[0][0] == 0xAB && fg.fpr[0][0] == 0x5C, "FPU read back");

  /* XSAVE and XCRs. */
  static uint32_t xs[1024];
  memset(xs, 0, sizeof(xs));
  check(ioctl(vcpu, KVM_GET_XSAVE, xs) == 0, "XSAVE read");
  check(ioctl(vcpu, KVM_SET_XSAVE, xs) == 0, "XSAVE written");

  uint32_t a, b, c, d;
  __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0xD), "c"(0));
  uint64_t host_xcr0 = a | ((uint64_t)d << 32);
  struct xcrs x;
  memset(&x, 0, sizeof(x));
  check(ioctl(vcpu, KVM_GET_XCRS, &x) == 0 && x.nr == 1 && x.x[0].value == 1, "XCR0 starts at x87");
  memset(&x, 0, sizeof(x));
  x.nr = 1;
  x.x[0].value = 3;
  check((host_xcr0 & 3) == 3 && ioctl(vcpu, KVM_SET_XCRS, &x) == 0, "XCR0 set to x87 and SSE");
  x.x[0].value = 0;
  check(ioctl(vcpu, KVM_SET_XCRS, &x) < 0, "XCR0 without x87 is refused");
  x.x[0].value = 1ull | (1ull << 40);
  check(ioctl(vcpu, KVM_SET_XCRS, &x) < 0, "XCR0 beyond the host mask is refused");

  /* LAPIC: the task priority register round-trips. */
  char lap[1024];
  memset(lap, 0, sizeof(lap));
  check(ioctl(vcpu, KVM_GET_LAPIC, lap) == 0, "LAPIC read");
  uint32_t tpr = 0x20;
  memcpy(lap + 0x80, &tpr, 4);
  check(ioctl(vcpu, KVM_SET_LAPIC, lap) == 0, "LAPIC written");
  char lap2[1024];
  memset(lap2, 0, sizeof(lap2));
  uint32_t got = 0;
  check(ioctl(vcpu, KVM_GET_LAPIC, lap2) == 0 && (memcpy(&got, lap2 + 0x80, 4), got == 0x20),
        "LAPIC task priority read back");

  /* MP state. */
  uint32_t mp = 99;
  check(ioctl(vcpu, KVM_GET_MP_STATE, &mp) == 0 && mp == 0, "BSP is runnable");
  mp = 1;
  check(ioctl(vcpu, KVM_SET_MP_STATE, &mp) == 0, "MP state set to uninitialized");
  mp = 99;
  check(ioctl(vcpu, KVM_GET_MP_STATE, &mp) == 0 && mp == 1, "MP state read back");
  mp = 0;
  check(ioctl(vcpu, KVM_SET_MP_STATE, &mp) == 0, "MP state back to runnable");

  /* Events and debug registers: nothing pending, reset values. */
  struct events ev;
  memset(&ev, 0, sizeof(ev));
  check(ioctl(vcpu, KVM_GET_VCPU_EVENTS, &ev) == 0, "vCPU events read");
  check(ioctl(vcpu, KVM_SET_VCPU_EVENTS, &ev) == 0, "vCPU events with nothing pending set");
  ev.nmi.pending = 1;
  check(ioctl(vcpu, KVM_SET_VCPU_EVENTS, &ev) < 0, "a pending NMI is refused");

  struct { uint64_t db[4], dr6, dr7, flags, reserved[9]; } dr;
  memset(&dr, 0, sizeof(dr));
  check(ioctl(vcpu, KVM_GET_DEBUGREGS, &dr) == 0 && dr.dr6 == 0xFFFF0FF0ull && dr.dr7 == 0x400,
        "debug registers read at reset");
  check(ioctl(vcpu, KVM_SET_DEBUGREGS, &dr) == 0, "debug registers at reset set");
  dr.db[0] = 1;
  check(ioctl(vcpu, KVM_SET_DEBUGREGS, &dr) < 0, "a breakpoint address is refused");

  if (pass) puts("AVM_STATE_BOOT: PASS");
  else puts("AVM_STATE_BOOT: FAIL");
  reboot(RB_POWER_OFF);
  for (;;) pause();

fail:
  printf("AVM_STATE_BOOT: FAIL setup, errno %d\n", errno);
  reboot(RB_POWER_OFF);
  for (;;) pause();
}
'''

INIT = TEMPLATE.replace('@GUEST@', ', '.join(f'0x{b:02X}' for b in GUEST))
out = run_probe('avm-state-boot', INIT, 'AVM_STATE_BOOT: PASS',
                accel=os.environ.get('AVORY_TEST_ACCEL', 'kvm'), timeout=180)
serial = (out / 'serial.log').read_text(errors='replace')
if 'npt verify FAILED' in serial:
    raise SystemExit('AVM_STATE_BOOT: FAIL kernel reported a fault during the test')
print('AVM_STATE_BOOT: kernel log checked')
