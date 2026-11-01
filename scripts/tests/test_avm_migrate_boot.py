#!/usr/bin/env python3
"""Check that a vCPU keeps its state when its thread moves between host CPUs.

The guest keeps a value in BX and spins for a long time, writing '.' on each
outer iteration so the vCPU returns to the VMM often.  While it runs, the
test changes the vCPU thread's CPU affinity back and forth between CPU 0 and
CPU 3, which asks the scheduler to move the thread.  The scheduler does not reliably
move a running thread on request, so the test asks for one move, not a fixed
number.  The guest must still hold its BX value at the end ('K'), and the kernel
must log at least one move.
The guest is assembled from avm_migrate_guest.asm at test time.
"""
import os
import re
import subprocess
import tempfile
from pathlib import Path

from boot_probe import run_probe

HERE = Path(__file__).resolve().parent


def assemble_guest():
    out = Path(tempfile.mkdtemp(prefix='avm-migrate-')) / 'guest.bin'
    subprocess.run(['nasm', '-f', 'bin', '-o', str(out),
                    str(HERE / 'avm_migrate_guest.asm')], check=True)
    return out.read_bytes()


GUEST = assemble_guest()

TEMPLATE = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
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

static int vcpu_fd;
static struct run *run;
static volatile int finished;
static int seen_cpu[8];
static int transitions;
static int last_cpu = -1;
static char out[4096];
static int n;
static int halted;
static int err;

static void *run_vcpu(void *arg) {
  (void)arg;
  for (int exits = 0; exits < 2000000 && !halted; exits++) {
    if (ioctl(vcpu_fd, KVM_RUN, 0) != 0) {
      err = errno;
      break;
    }
    if (run->exit_reason == KVM_EXIT_IO && run->io.direction == KVM_EXIT_IO_OUT &&
        run->io.port == 0x3F8 && run->io.size == 1) {
      char c = *((char *)run + run->io.data_offset);
      if (n < (int)sizeof(out) - 1)
        out[n++] = c;
    } else if (run->exit_reason == KVM_EXIT_HLT) {
      halted = 1;
    } else {
      err = -1;
      break;
    }
    /* A short sleep between exits lets the scheduler place this thread again,
     * honouring its current affinity. */
    int cpu_now = sched_getcpu();
    if (cpu_now >= 0 && cpu_now < 8) {
      seen_cpu[cpu_now]++;
      if (last_cpu >= 0 && cpu_now != last_cpu)
        transitions++;
      last_cpu = cpu_now;
    }
    usleep(2000);
  }
  out[n] = 0;
  finished = 1;
  return NULL;
}

int main(void) {
  setbuf(stdout, NULL);
  puts("AVM_MIGRATE_BOOT: START");

  int avm = open("/dev/avm", O_RDWR);
  if (avm < 0) goto fail;
  int vm = ioctl(avm, KVM_CREATE_VM, 0);
  if (vm < 0) goto fail;
  if (ioctl(vm, KVM_CREATE_IRQCHIP, 0) != 0) goto fail;

  uint8_t *mem = mmap(NULL, 4 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) goto fail;
  struct region r = { .slot = 0, .gpa = 0, .size = 4 * 4096, .hva = (uint64_t)mem };
  if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &r) != 0) goto fail;
  memcpy(mem + GUEST_ORG, guest, sizeof(guest));

  vcpu_fd = ioctl(vm, KVM_CREATE_VCPU, 0);
  if (vcpu_fd < 0) goto fail;

  struct regs rg;
  memset(&rg, 0, sizeof(rg));
  rg.rip = GUEST_ORG;
  rg.rflags = 2;
  if (ioctl(vcpu_fd, KVM_SET_REGS, &rg) != 0) goto fail;

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
  if (ioctl(vcpu_fd, KVM_SET_SREGS, &s) != 0) goto fail;

  run = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu_fd, 0);
  if (run == MAP_FAILED) goto fail;

  pthread_t t;
  if (pthread_create(&t, NULL, run_vcpu, NULL) != 0) goto fail;

  /* Move the vCPU thread between CPUs 0 and 3 while the guest runs. */
  int flips = 0;
  while (!finished) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET((flips & 1) ? 3 : 0, &set);
    if (pthread_setaffinity_np(t, sizeof(set), &set) == 0)
      flips++;
    usleep(3000);
  }
  pthread_join(t, NULL);

  printf("AVM_MIGRATE_BOOT: cpus seen %d %d %d %d, transitions %d\n", seen_cpu[0], seen_cpu[1], seen_cpu[2], seen_cpu[3], transitions);
  printf("AVM_MIGRATE_BOOT: guest output %s, halted %d, err %d, affinity changes %d\n",
         out, halted, err, flips);
  int ok = n >= 2 && out[n - 1] == 'K' && halted && err == 0 && transitions >= 1;
  printf("AVM_MIGRATE_BOOT: %s   the guest kept its state across host CPU changes\n", ok ? "ok" : "FAIL");
  if (ok) puts("AVM_MIGRATE_BOOT: PASS");
  else puts("AVM_MIGRATE_BOOT: FAIL");
  reboot(RB_POWER_OFF);
  for (;;) pause();

fail:
  printf("AVM_MIGRATE_BOOT: FAIL setup, errno %d\n", errno);
  reboot(RB_POWER_OFF);
  for (;;) pause();
}
'''

INIT = TEMPLATE.replace('@GUEST@', ', '.join(f'0x{b:02X}' for b in GUEST))

out = run_probe('avm-migrate-boot', INIT, 'AVM_MIGRATE_BOOT: PASS',
                accel=os.environ.get('AVORY_TEST_ACCEL', 'kvm'), timeout=300)
serial = (out / 'serial.log').read_text(errors='replace')
moves = len(re.findall(r'\[AVM\] vcpu moved to host CPU', serial))
if moves < 1:
    raise SystemExit(f'AVM_MIGRATE_BOOT: FAIL the kernel logged {moves} host CPU moves, expected at least 1')
print(f'AVM_MIGRATE_BOOT: ok   the kernel logged {moves} host CPU moves')
