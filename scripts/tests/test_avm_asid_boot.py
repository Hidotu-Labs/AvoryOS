#!/usr/bin/env python3
"""Check two VMs running side by side: each keeps its own memory and gets its own ASID.

Each VM has one vCPU in its own thread, and both run at once.  Each guest
stores a different marker at the same guest address (0x3000), then checks it
over a long loop.  Each must report its own marker, and the kernel must have
given the two VMs different ASIDs.  The guest is assembled from
avm_asid_guest.asm at test time.
"""
import os
import re
import subprocess
import tempfile
from pathlib import Path

from boot_probe import run_probe

HERE = Path(__file__).resolve().parent


def assemble_guest():
    out = Path(tempfile.mkdtemp(prefix='avm-asid-')) / 'guest.bin'
    subprocess.run(['nasm', '-f', 'bin', '-o', str(out),
                    str(HERE / 'avm_asid_guest.asm')], check=True)
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

struct vm_ctx {
  uint64_t marker;     /* the value this VM's guest must keep */
  int vcpu;
  struct run *run;
  char out[16];
  int n;
  int halted;
  int err;
};

static void *run_vm(void *arg) {
  struct vm_ctx *c = arg;
  for (int exits = 0; exits < 5000 && !c->halted; exits++) {
    if (ioctl(c->vcpu, KVM_RUN, 0) != 0) {
      c->err = errno;
      break;
    }
    if (c->run->exit_reason == KVM_EXIT_IO && c->run->io.direction == KVM_EXIT_IO_OUT &&
        c->run->io.port == 0x3F8 && c->run->io.size == 1) {
      if (c->n < (int)sizeof(c->out) - 1)
        c->out[c->n++] = *((char *)c->run + c->run->io.data_offset);
    } else if (c->run->exit_reason == KVM_EXIT_HLT) {
      c->halted = 1;
    } else {
      c->err = -1;
      break;
    }
  }
  c->out[c->n] = 0;
  return NULL;
}

static int make_vm(struct vm_ctx *ctx, uint64_t marker, int *vm_out) {
  int avm = open("/dev/avm", O_RDWR);
  if (avm < 0) return -1;
  int vm = ioctl(avm, KVM_CREATE_VM, 0);
  if (vm < 0) return -1;
  if (ioctl(vm, KVM_CREATE_IRQCHIP, 0) != 0) return -1;

  uint8_t *mem = mmap(NULL, 4 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) return -1;
  struct region r = { .slot = 0, .gpa = 0, .size = 4 * 4096, .hva = (uint64_t)mem };
  if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &r) != 0) return -1;
  memcpy(mem + GUEST_ORG, guest, sizeof(guest));

  int vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
  if (vcpu < 0) return -1;

  struct regs rg;
  memset(&rg, 0, sizeof(rg));
  rg.rip = GUEST_ORG;
  rg.rflags = 2;
  rg.rbx = marker;
  if (ioctl(vcpu, KVM_SET_REGS, &rg) != 0) return -1;

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
  if (ioctl(vcpu, KVM_SET_SREGS, &s) != 0) return -1;

  ctx->marker = marker;
  ctx->vcpu = vcpu;
  ctx->run = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
  if (ctx->run == MAP_FAILED) return -1;
  *vm_out = vm;
  return 0;
}

int main(void) {
  setbuf(stdout, NULL);
  puts("AVM_ASID_BOOT: START");

  struct vm_ctx a = { 0 }, b = { 0 };
  int vm_a, vm_b;
  if (make_vm(&a, 0x41, &vm_a) != 0 || make_vm(&b, 0x42, &vm_b) != 0) {
    printf("AVM_ASID_BOOT: FAIL setup, errno %d\n", errno);
    goto done;
  }

  pthread_t ta, tb;
  pthread_create(&ta, NULL, run_vm, &a);
  pthread_create(&tb, NULL, run_vm, &b);
  pthread_join(ta, NULL);
  pthread_join(tb, NULL);

  printf("AVM_ASID_BOOT: vm a output %s, halted %d, err %d\n", a.out, a.halted, a.err);
  printf("AVM_ASID_BOOT: vm b output %s, halted %d, err %d\n", b.out, b.halted, b.err);
  int a_ok = strcmp(a.out, "VAK") == 0 && a.halted && a.err == 0;
  int b_ok = strcmp(b.out, "VBK") == 0 && b.halted && b.err == 0;
  printf("AVM_ASID_BOOT: %s   each VM kept its own memory while both ran\n",
         a_ok && b_ok ? "ok" : "FAIL");
  if (a_ok && b_ok) puts("AVM_ASID_BOOT: PASS");
  else puts("AVM_ASID_BOOT: FAIL");

done:
  reboot(RB_POWER_OFF);
  for (;;) pause();
}
'''

INIT = TEMPLATE.replace('@GUEST@', ', '.join(f'0x{b:02X}' for b in GUEST))

out = run_probe('avm-asid-boot', INIT, 'AVM_ASID_BOOT: PASS',
                accel=os.environ.get('AVORY_TEST_ACCEL', 'kvm'), timeout=300)
serial = (out / 'serial.log').read_text(errors='replace')
asids = re.findall(r'\[AVM\] vm asid=(\d+)', serial)
if len(asids) < 2 or len(set(asids[-2:])) != 2:
    raise SystemExit(f'AVM_ASID_BOOT: FAIL expected two distinct VM ASIDs, saw {asids}')
print(f'AVM_ASID_BOOT: ok   the two VMs had ASIDs {asids[-2]} and {asids[-1]}')
