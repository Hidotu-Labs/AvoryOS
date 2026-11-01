#!/usr/bin/env python3
"""Check that removing a memslot reaches a vCPU that is running.

The guest spins on the word at 0x3000 with no exits.  The VMM waits for the
guest's 'R', then removes the memslot backing 0x3000 from a second thread.  The
kernel must unmap the NPT entry and kick the running vCPU, so the guest's next
read exits to the VMM (KVM_EXIT_MMIO), which answers 0x77.  The guest then
writes 'M'.  If the removal were not seen, the guest would write 'T'.

The guest is assembled from avm_slot_guest.asm at test time.
"""
import os
import subprocess
import tempfile
from pathlib import Path

from boot_probe import run_probe

HERE = Path(__file__).resolve().parent


def assemble_guest():
    out = Path(tempfile.mkdtemp(prefix='avm-slot-')) / 'guest.bin'
    subprocess.run(['nasm', '-f', 'bin', '-o', str(out),
                    str(HERE / 'avm_slot_guest.asm')], check=True)
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
#define KVM_EXIT_MMIO 6
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
    struct { uint64_t phys_addr; uint8_t data[8]; uint32_t len; uint8_t is_write; } mmio;
    struct { uint64_t hardware_exit_reason; } hw;
  };
};

static int vm_fd;
static volatile int ready;
static int remove_result = -1;

/* Removes the memslot that backs 0x3000, once the guest has said it is ready. */
static void *remover(void *arg) {
  (void)arg;
  while (!ready)
    usleep(1000);
  usleep(30000);  /* the guest is in its loop by now */
  struct region gone = { .slot = 1, .flags = 0, .gpa = 0x3000, .size = 0, .hva = 0 };
  remove_result = ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &gone);
  return NULL;
}

int main(void) {
  setbuf(stdout, NULL);
  puts("AVM_SLOT_BOOT: START");

  int avm = open("/dev/avm", O_RDWR);
  if (avm < 0) goto fail;
  vm_fd = ioctl(avm, KVM_CREATE_VM, 0);
  if (vm_fd < 0) goto fail;
  if (ioctl(vm_fd, KVM_CREATE_IRQCHIP, 0) != 0) goto fail;

  uint8_t *mem = mmap(NULL, 4 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) goto fail;
  struct region r0 = { .slot = 0, .gpa = 0, .size = 3 * 4096, .hva = (uint64_t)mem };
  struct region r1 = { .slot = 1, .gpa = 0x3000, .size = 4096, .hva = (uint64_t)(mem + 0x3000) };
  if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &r0) != 0) goto fail;
  if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &r1) != 0) goto fail;

  int vcpu = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
  if (vcpu < 0) goto fail;
  memcpy(mem + GUEST_ORG, guest, sizeof(guest));

  struct regs rg;
  memset(&rg, 0, sizeof(rg));
  rg.rip = GUEST_ORG;
  rg.rflags = 2;
  if (ioctl(vcpu, KVM_SET_REGS, &rg) != 0) goto fail;

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

  struct run *run = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
  if (run == MAP_FAILED) goto fail;

  char out[16];
  int n = 0, halted = 0, mmio_hits = 0, started = 0;
  pthread_t remover_t;
  for (int exits = 0; exits < 100000 && !halted; exits++) {
    if (ioctl(vcpu, KVM_RUN, 0) != 0) {
      printf("AVM_SLOT_BOOT: KVM_RUN failed, errno %d\n", errno);
      break;
    }
    if (run->exit_reason == KVM_EXIT_IO && run->io.direction == KVM_EXIT_IO_OUT &&
        run->io.port == 0x3F8 && run->io.size == 1) {
      char c = *((char *)run + run->io.data_offset);
      if (n < (int)sizeof(out) - 1) out[n++] = c;
      if (c == 'R' && !started) {
        started = 1;
        pthread_create(&remover_t, NULL, remover, NULL);
        ready = 1;
      }
    } else if (run->exit_reason == KVM_EXIT_MMIO) {
      /* The removed page now answers from the VMM: a 16-bit read of 0x3000. */
      if (!run->mmio.is_write && run->mmio.phys_addr == 0x3000 && run->mmio.len == 2) {
        uint16_t answer = 0x77;
        memcpy(run->mmio.data, &answer, sizeof(answer));
        mmio_hits++;
      } else {
        printf("AVM_SLOT_BOOT: unexpected mmio at 0x%llx\n",
               (unsigned long long)run->mmio.phys_addr);
        break;
      }
    } else if (run->exit_reason == KVM_EXIT_HLT) {
      halted = 1;
    } else {
      printf("AVM_SLOT_BOOT: unexpected exit %u, hw reason 0x%llx\n", run->exit_reason,
             (unsigned long long)run->hw.hardware_exit_reason);
      break;
    }
  }
  if (started) pthread_join(remover_t, NULL);
  out[n] = 0;
  printf("AVM_SLOT_BOOT: guest output %s, mmio answers %d, removal result %d\n",
         out, mmio_hits, remove_result);

  int ok = strcmp(out, "RM") == 0 && halted && mmio_hits == 1 && remove_result == 0;
  printf("AVM_SLOT_BOOT: %s   the running vCPU saw the removed memslot\n", ok ? "ok" : "FAIL");
  if (ok) puts("AVM_SLOT_BOOT: PASS");
  else puts("AVM_SLOT_BOOT: FAIL");
  reboot(RB_POWER_OFF);
  for (;;) pause();

fail:
  printf("AVM_SLOT_BOOT: FAIL setup, errno %d\n", errno);
  reboot(RB_POWER_OFF);
  for (;;) pause();
}
'''

INIT = TEMPLATE.replace('@GUEST@', ', '.join(f'0x{b:02X}' for b in GUEST))

out = run_probe('avm-slot-boot', INIT, 'AVM_SLOT_BOOT: PASS',
                accel=os.environ.get('AVORY_TEST_ACCEL', 'kvm'), timeout=300)
serial = (out / 'serial.log').read_text(errors='replace')
if 'npt verify FAILED' in serial:
    raise SystemExit('AVM_SLOT_BOOT: FAIL kernel reported a fault during the test')
print('AVM_SLOT_BOOT: kernel log checked')
