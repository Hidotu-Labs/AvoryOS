#!/usr/bin/env python3
"""Check a memslot removal in one VM while another VM runs.

VM A spins on a word backed by memslot 1.  The VMM removes memslot 1 from a
second thread, and VM A's next read must reach the VMM, which answers 0x77.
VM B runs at the same time, with its own memory and its own marker, and must
finish with its own marker.  The kicks from the removal must reach only VM A's
vCPU; VM B must be unaffected.  The guests are assembled from avm_slot_guest.asm
and avm_asid_guest.asm at test time.
"""
import os
import subprocess
import tempfile
from pathlib import Path

from boot_probe import run_probe

HERE = Path(__file__).resolve().parent


def assemble_guest(name):
    out = Path(tempfile.mkdtemp(prefix='avm-multi-')) / f'{name}.bin'
    subprocess.run(['nasm', '-f', 'bin', '-o', str(out),
                    str(HERE / f'{name}.asm')], check=True)
    return out.read_bytes()


SLOT = assemble_guest('avm_slot_guest')
ASID = assemble_guest('avm_asid_guest')

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
#define KVM_EXIT_MMIO 6
#define KVM_EXIT_IO_OUT 1

#define GUEST_ORG 0x1000
static const uint8_t slot_guest[] = { @SLOT@ };
static const uint8_t asid_guest[] = { @ASID@ };

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

struct vm_ctx {
  int vm;
  int vcpu;
  struct run *run;
  int is_slot;
  char out[16];
  int n;
  int halted;
  int mmio_hits;
  int err;
  int started;
  int remove_result;
  pthread_t remover;
};

static volatile int slot_ready;

static void *remover(void *arg) {
  struct vm_ctx *a = arg;
  while (!slot_ready)
    usleep(1000);
  usleep(30000);  /* VM A's guest is spinning by now */
  struct region gone = { .slot = 1, .flags = 0, .gpa = 0x3000, .size = 0, .hva = 0 };
  a->remove_result = ioctl(a->vm, KVM_SET_USER_MEMORY_REGION, &gone);
  return NULL;
}

static void *run_vm(void *arg) {
  struct vm_ctx *c = arg;
  for (int exits = 0; exits < 100000 && !c->halted; exits++) {
    if (ioctl(c->vcpu, KVM_RUN, 0) != 0) {
      c->err = errno;
      break;
    }
    if (c->run->exit_reason == KVM_EXIT_IO && c->run->io.direction == KVM_EXIT_IO_OUT &&
        c->run->io.port == 0x3F8 && c->run->io.size == 1) {
      char ch = *((char *)c->run + c->run->io.data_offset);
      if (c->n < (int)sizeof(c->out) - 1)
        c->out[c->n++] = ch;
      if (c->is_slot && ch == 'R' && !c->started) {
        c->started = 1;
        pthread_create(&c->remover, NULL, remover, c);
        slot_ready = 1;
      }
    } else if (c->run->exit_reason == KVM_EXIT_MMIO) {
      if (c->is_slot && !c->run->mmio.is_write && c->run->mmio.phys_addr == 0x3000 &&
          c->run->mmio.len == 2) {
        uint16_t answer = 0x77;
        memcpy(c->run->mmio.data, &answer, sizeof(answer));
        c->mmio_hits++;
      } else {
        c->err = -2;
        break;
      }
    } else if (c->run->exit_reason == KVM_EXIT_HLT) {
      c->halted = 1;
    } else {
      c->err = -1;
      break;
    }
  }
  c->out[c->n] = 0;
  if (c->started)
    pthread_join(c->remover, NULL);
  return NULL;
}

static int make_vm(struct vm_ctx *c, const uint8_t *code, size_t len, uint64_t bx) {
  int avm = open("/dev/avm", O_RDWR);
  if (avm < 0) return -1;
  c->vm = ioctl(avm, KVM_CREATE_VM, 0);
  if (c->vm < 0) return -1;
  if (ioctl(c->vm, KVM_CREATE_IRQCHIP, 0) != 0) return -1;

  uint8_t *mem = mmap(NULL, 4 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) return -1;
  if (c->is_slot) {
    struct region r0 = { .slot = 0, .gpa = 0, .size = 3 * 4096, .hva = (uint64_t)mem };
    struct region r1 = { .slot = 1, .gpa = 0x3000, .size = 4096, .hva = (uint64_t)(mem + 0x3000) };
    if (ioctl(c->vm, KVM_SET_USER_MEMORY_REGION, &r0) != 0) return -1;
    if (ioctl(c->vm, KVM_SET_USER_MEMORY_REGION, &r1) != 0) return -1;
  } else {
    struct region r0 = { .slot = 0, .gpa = 0, .size = 4 * 4096, .hva = (uint64_t)mem };
    if (ioctl(c->vm, KVM_SET_USER_MEMORY_REGION, &r0) != 0) return -1;
  }
  memcpy(mem + GUEST_ORG, code, len);

  c->vcpu = ioctl(c->vm, KVM_CREATE_VCPU, 0);
  if (c->vcpu < 0) return -1;

  struct regs rg;
  memset(&rg, 0, sizeof(rg));
  rg.rip = GUEST_ORG;
  rg.rflags = 2;
  rg.rbx = bx;
  if (ioctl(c->vcpu, KVM_SET_REGS, &rg) != 0) return -1;

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
  if (ioctl(c->vcpu, KVM_SET_SREGS, &s) != 0) return -1;

  c->run = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, c->vcpu, 0);
  if (c->run == MAP_FAILED) return -1;
  return 0;
}

int main(void) {
  setbuf(stdout, NULL);
  puts("AVM_MULTIVM_BOOT: START");

  struct vm_ctx a = { .is_slot = 1 }, b = { .is_slot = 0 };
  if (make_vm(&a, slot_guest, sizeof(slot_guest), 0) != 0 ||
      make_vm(&b, asid_guest, sizeof(asid_guest), 0x42) != 0) {
    printf("AVM_MULTIVM_BOOT: FAIL setup, errno %d\n", errno);
    goto done;
  }

  pthread_t ta, tb;
  pthread_create(&ta, NULL, run_vm, &a);
  pthread_create(&tb, NULL, run_vm, &b);
  pthread_join(ta, NULL);
  pthread_join(tb, NULL);

  printf("AVM_MULTIVM_BOOT: vm a output %s, mmio answers %d, removal %d, err %d\n",
         a.out, a.mmio_hits, a.remove_result, a.err);
  printf("AVM_MULTIVM_BOOT: vm b output %s, err %d\n", b.out, b.err);
  int a_ok = strcmp(a.out, "RM") == 0 && a.halted && a.mmio_hits == 1 &&
             a.remove_result == 0 && a.err == 0;
  int b_ok = strcmp(b.out, "VBK") == 0 && b.halted && b.err == 0;
  printf("AVM_MULTIVM_BOOT: %s   the removal reached VM A and VM B was unaffected\n",
         a_ok && b_ok ? "ok" : "FAIL");
  if (a_ok && b_ok) puts("AVM_MULTIVM_BOOT: PASS");
  else puts("AVM_MULTIVM_BOOT: FAIL");

done:
  reboot(RB_POWER_OFF);
  for (;;) pause();
}
'''


def c_array(data):
    return ', '.join(f'0x{b:02X}' for b in data)


INIT = TEMPLATE.replace('@SLOT@', c_array(SLOT)).replace('@ASID@', c_array(ASID))

out = run_probe('avm-multivm-boot', INIT, 'AVM_MULTIVM_BOOT: PASS',
                accel=os.environ.get('AVORY_TEST_ACCEL', 'kvm'), timeout=300)
serial = (out / 'serial.log').read_text(errors='replace')
if 'npt verify FAILED' in serial:
    raise SystemExit('AVM_MULTIVM_BOOT: FAIL kernel reported a fault during the test')
print('AVM_MULTIVM_BOOT: kernel log checked')
