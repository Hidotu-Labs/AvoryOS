#!/usr/bin/env python3
"""Stress the kick path: a running vCPU and a memslot toggled from another thread.

Each memslot add or remove kicks every running vCPU with an NMI.  The guest
(avm_kickstorm_guest.asm) does an exit on every loop, and the VMM thread keeps
calling KVM_RUN while a second thread registers and removes a memslot 2000
times.  Before the kick count, two NMIs close together looked stray to the host
and panicked the kernel.  Passing means the kernel stays up and every memslot
change succeeds.
"""
import os
import subprocess
import tempfile
from pathlib import Path

from boot_probe import run_probe

HERE = Path(__file__).resolve().parent


def assemble_guest():
    out = Path(tempfile.mkdtemp(prefix='avm-kickstorm-')) / 'guest.bin'
    subprocess.run(['nasm', '-f', 'bin', '-o', str(out),
                    str(HERE / 'avm_kickstorm_guest.asm')], check=True)
    return out.read_bytes()


GUEST = assemble_guest()

TEMPLATE = r'''
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
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
#define KVM_EXIT_IO 2
#define KVM_EXIT_IO_OUT 1

#define GUEST_ORG 0x1000
/* Open issue: the whole AvoryOS machine goes down silently partway through this
 * test, with no panic line.  It passed at 100 toggles before the log cleanup,
 * and still fails at 100 after it.  Kept at 2000 as the reproducer. */
#define TOGGLES 2000
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

static int vm;
static volatile int stop;
static volatile int toggles_ok, toggles_failed;
static uint8_t *extra;

/* Registers and removes a memslot as fast as it can.  Each change kicks the
 * vCPU, so the vCPU thread sees a steady stream of kick NMIs. */
static void *toggler(void *arg) {
  (void)arg;
  struct region add = { .slot = 1, .flags = 0, .gpa = 0x200000, .size = 4096, .hva = (uint64_t)extra };
  struct region del = add;
  del.size = 0;
  for (int i = 0; i < TOGGLES; i++) {
    int ok = 1;
    for (int tries = 0; tries < 1000; tries++) {
      if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &add) == 0) break;
      if (errno != EBUSY) { ok = 0; break; }
    }
    if (ok) {
      for (int tries = 0; tries < 1000; tries++) {
        if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &del) == 0) break;
        if (errno != EBUSY) { ok = 0; break; }
      }
    }
    if (ok) toggles_ok++;
    else toggles_failed++;
  }
  stop = 1;
  return NULL;
}

int main(void) {
  setbuf(stdout, NULL);
  puts("AVM_KICKSTORM_BOOT: START");

  int avm = open("/dev/avm", O_RDWR);
  if (avm < 0) goto fail;
  vm = ioctl(avm, KVM_CREATE_VM, 0);
  if (vm < 0) goto fail;

  uint8_t *mem = mmap(NULL, 16 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) goto fail;
  struct region r = { .slot = 0, .flags = 0, .gpa = 0, .size = 16 * 4096, .hva = (uint64_t)mem };
  if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &r) != 0) goto fail;
  extra = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (extra == MAP_FAILED) goto fail;

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

  pthread_t t;
  if (pthread_create(&t, NULL, toggler, NULL) != 0) goto fail;

  long exits = 0, bad_exits = 0;
  /* Keep running after the toggles end, so the vCPU always overlaps them. */
  while (!stop || exits < 2000) {
    if (ioctl(vcpu, KVM_RUN, 0) != 0) {
      if (errno == EINTR) continue;
      bad_exits++;
      break;
    }
    if (run->exit_reason == KVM_EXIT_IO && run->io.direction == KVM_EXIT_IO_OUT)
      exits++;
    else
      bad_exits++;
  }
  pthread_join(t, NULL);
  printf("AVM_KICKSTORM_BOOT: %ld guest exits, %d toggles ok, %d failed, %ld bad exits\n",
         exits, toggles_ok, toggles_failed, bad_exits);

  int pass = exits > 0 && toggles_failed == 0 && toggles_ok == TOGGLES && bad_exits == 0;
  if (pass) puts("AVM_KICKSTORM_BOOT: PASS");
  else puts("AVM_KICKSTORM_BOOT: FAIL");
  reboot(RB_POWER_OFF);
  for (;;) pause();

fail:
  printf("AVM_KICKSTORM_BOOT: FAIL setup, errno %d\n", errno);
  reboot(RB_POWER_OFF);
  for (;;) pause();
}
'''

INIT = TEMPLATE.replace('@GUEST@', ', '.join(f'0x{b:02X}' for b in GUEST))
out = run_probe('avm-kickstorm-boot', INIT, 'AVM_KICKSTORM_BOOT: PASS',
                accel=os.environ.get('AVORY_TEST_ACCEL', 'kvm'), timeout=300)
serial = (out / 'serial.log').read_text(errors='replace')
if 'Unexpected Non Maskable' in serial or 'PANIC' in serial:
    raise SystemExit('AVM_KICKSTORM_BOOT: FAIL the kernel panicked on a kick NMI')
print('AVM_KICKSTORM_BOOT: kernel log checked')
