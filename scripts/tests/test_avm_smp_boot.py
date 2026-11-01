#!/usr/bin/env python3
"""Check two guest vCPUs: INIT and SIPI, IPIs, and both reaching HLT.

Phase 3 scope: each vCPU runs in its own host thread, an application processor
blocks in KVM_RUN until SIPI, the boot processor's INIT and SIPI start it, a
fixed IPI from the AP reaches a boot processor that is running in guest mode
(the kick), and both vCPUs halt.  The guests are assembled from avm_smp_bsp.asm
and avm_smp_ap.asm at test time.
"""
import os
import subprocess
import tempfile
from pathlib import Path

from boot_probe import run_probe

HERE = Path(__file__).resolve().parent


def assemble(name, org):
    out = Path(tempfile.mkdtemp(prefix='avm-smp-')) / f'{name}.bin'
    subprocess.run(['nasm', '-f', 'bin', '-o', str(out),
                    str(HERE / f'avm_smp_{name}.asm')], check=True)
    return out.read_bytes()


BSP = assemble('bsp', 0x1000)
AP = assemble('ap', 0x8000)

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

/* ABI subset, as in src/avm/avm_uapi.h. */
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

#define BSP_ORG 0x1000
#define AP_ORG 0x8000
static const uint8_t bsp_code[] = { @BSP@ };
static const uint8_t ap_code[] = { @AP@ };

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

struct vcpu_ctx {
  const char *name;
  int fd;
  struct run *run;
  char out[16];
  int n;
  int halted;
  int err;
};

static void *run_vcpu(void *arg) {
  struct vcpu_ctx *c = arg;
  for (int exits = 0; exits < 5000 && !c->halted; exits++) {
    if (ioctl(c->fd, KVM_RUN, 0) != 0) {
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
      c->err = -(int)c->run->exit_reason - 1;
      break;
    }
  }
  c->out[c->n] = 0;
  return NULL;
}

static void set_segments(int fd) {
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
  ioctl(fd, KVM_SET_SREGS, &s);
}

int main(void) {
  setbuf(stdout, NULL);
  puts("AVM_SMP_BOOT: START");

  int avm = open("/dev/avm", O_RDWR);
  if (avm < 0) goto fail;
  int vm = ioctl(avm, KVM_CREATE_VM, 0);
  if (vm < 0) goto fail;
  if (ioctl(vm, KVM_CREATE_IRQCHIP, 0) != 0) goto fail;

  /* 16 pages: the BSP code at 0x1000, the AP code at 0x8000, and the BSP's
   * flag at 0x9000. */
  uint8_t *mem = mmap(NULL, 16 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) goto fail;
  struct region r = { .slot = 0, .gpa = 0, .size = 16 * 4096, .hva = (uint64_t)mem };
  if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &r) != 0) goto fail;
  memcpy(mem + BSP_ORG, bsp_code, sizeof(bsp_code));
  memcpy(mem + AP_ORG, ap_code, sizeof(ap_code));

  int bsp_fd = ioctl(vm, KVM_CREATE_VCPU, 0);
  int ap_fd = ioctl(vm, KVM_CREATE_VCPU, 1);
  if (bsp_fd < 0 || ap_fd < 0) goto fail;

  struct regs rg;
  memset(&rg, 0, sizeof(rg));
  rg.rip = BSP_ORG;
  rg.rflags = 2;
  if (ioctl(bsp_fd, KVM_SET_REGS, &rg) != 0) goto fail;
  set_segments(bsp_fd);

  struct run *bsp_run = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, bsp_fd, 0);
  struct run *ap_run = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, ap_fd, 0);
  if (bsp_run == MAP_FAILED || ap_run == MAP_FAILED) goto fail;

  /* The AP's registers come from SIPI, so it starts with no setup here. */
  struct vcpu_ctx ap = { .name = "ap", .fd = ap_fd, .run = ap_run };
  struct vcpu_ctx bsp = { .name = "bsp", .fd = bsp_fd, .run = bsp_run };
  pthread_t ap_t, bsp_t;
  if (pthread_create(&ap_t, NULL, run_vcpu, &ap) != 0) goto fail;
  if (pthread_create(&bsp_t, NULL, run_vcpu, &bsp) != 0) goto fail;
  pthread_join(bsp_t, NULL);
  pthread_join(ap_t, NULL);

  printf("AVM_SMP_BOOT: bsp output %s, halted %d, err %d\n", bsp.out, bsp.halted, bsp.err);
  printf("AVM_SMP_BOOT: ap output %s, halted %d, err %d\n", ap.out, ap.halted, ap.err);

  int bsp_ok = strcmp(bsp.out, "BK") == 0 && bsp.halted && bsp.err == 0;
  int ap_ok = strcmp(ap.out, "A") == 0 && ap.halted && ap.err == 0;
  printf("AVM_SMP_BOOT: %s   boot processor reached HLT after the AP's IPI\n", bsp_ok ? "ok" : "FAIL");
  printf("AVM_SMP_BOOT: %s   application processor reached HLT after SIPI\n", ap_ok ? "ok" : "FAIL");
  if (bsp_ok && ap_ok) puts("AVM_SMP_BOOT: PASS");
  else puts("AVM_SMP_BOOT: FAIL");
  reboot(RB_POWER_OFF);
  for (;;) pause();

fail:
  printf("AVM_SMP_BOOT: FAIL setup, errno %d\n", errno);
  reboot(RB_POWER_OFF);
  for (;;) pause();
}
'''


def c_array(data):
    return ', '.join(f'0x{b:02X}' for b in data)


INIT = TEMPLATE.replace('@BSP@', c_array(BSP)).replace('@AP@', c_array(AP))

out = run_probe('avm-smp-boot', INIT, 'AVM_SMP_BOOT: PASS',
                accel=os.environ.get('AVORY_TEST_ACCEL', 'kvm'), timeout=300)
serial = (out / 'serial.log').read_text(errors='replace')
if 'npt verify FAILED' in serial or 'injected #GP' in serial or 'undecodable' in serial:
    raise SystemExit('AVM_SMP_BOOT: FAIL kernel reported a fault during the test')
print('AVM_SMP_BOOT: kernel log checked')
