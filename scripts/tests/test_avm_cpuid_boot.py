#!/usr/bin/env python3
"""Check the CPUID and MSR-list ioctls, and the CPUID table the guest reads.

The VMM asks for the supported CPUID table and checks the KVM signature leaf,
then checks the MSR index lists, then sets its own one-leaf table.  The guest
(avm_cpuid_guest.asm) reads leaf 0x40000000, which must come from that table,
and leaf 1, which must be absent (zero).  Expected guest output: 'TZK'.
"""
import os
import subprocess
import tempfile
from pathlib import Path

from boot_probe import run_probe

HERE = Path(__file__).resolve().parent


def assemble_guest():
    out = Path(tempfile.mkdtemp(prefix='avm-cpuid-')) / 'guest.bin'
    subprocess.run(['nasm', '-f', 'bin', '-o', str(out),
                    str(HERE / 'avm_cpuid_guest.asm')], check=True)
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
#define KVM_GET_MSR_INDEX_LIST KVM_IOC(3, KVMIO, 0x02, 4)
#define KVM_GET_SUPPORTED_CPUID KVM_IOC(3, KVMIO, 0x05, 8)
#define KVM_GET_MSR_FEATURE_INDEX_LIST KVM_IOC(3, KVMIO, 0x0a, 4)
#define KVM_CREATE_VM KVM_IOC(0, KVMIO, 0x01, 0)
#define KVM_CREATE_IRQCHIP KVM_IOC(0, KVMIO, 0x60, 0)
#define KVM_CREATE_VCPU KVM_IOC(0, KVMIO, 0x41, 0)
#define KVM_SET_USER_MEMORY_REGION KVM_IOC(1, KVMIO, 0x46, 32)
#define KVM_RUN KVM_IOC(0, KVMIO, 0x80, 0)
#define KVM_SET_REGS KVM_IOC(1, KVMIO, 0x82, 144)
#define KVM_SET_SREGS KVM_IOC(1, KVMIO, 0x84, 312)
#define KVM_SET_CPUID2 KVM_IOC(1, KVMIO, 0x90, 8)
#define KVM_GET_MSRS KVM_IOC(3, KVMIO, 0x88, 8)
#define KVM_SET_MSRS KVM_IOC(1, KVMIO, 0x89, 8)
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

struct cpuid_entry2 { uint32_t function, index, flags, eax, ebx, ecx, edx, pad[3]; };
struct cpuid2 { uint32_t nent, pad; struct cpuid_entry2 entries[80]; };
struct msr_list { uint32_t nmsrs; uint32_t indices[8]; };
struct msr_entry { uint32_t index, reserved; uint64_t data; };
struct msrs1 { uint32_t nmsrs, pad; struct msr_entry entries[1]; };

static int pass = 1;

static void check(int ok, const char *what) {
  printf("AVM_CPUID_BOOT: %s   %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) pass = 0;
}

int main(void) {
  setbuf(stdout, NULL);
  puts("AVM_CPUID_BOOT: START");

  int avm = open("/dev/avm", O_RDWR);
  if (avm < 0) goto fail;
  check(ioctl(avm, KVM_CHECK_EXTENSION, 7) == 1, "KVM_CAP_EXT_CPUID reported");

  static struct cpuid2 cp;
  memset(&cp, 0, sizeof(cp));
  check(ioctl(avm, KVM_GET_SUPPORTED_CPUID, &cp) < 0, "a zero-size CPUID buffer is refused");
  cp.nent = 80;
  int r = ioctl(avm, KVM_GET_SUPPORTED_CPUID, &cp);
  int hv_sig = 0, leaf1_hv = 0, cet_hidden = 1, perfcore_hidden = 1;
  for (uint32_t i = 0; i < cp.nent; i++) {
    if (cp.entries[i].function == 0x40000000u && cp.entries[i].ebx == 0x4B4D564Bu) hv_sig = 1;
    if (cp.entries[i].function == 1u && (cp.entries[i].ecx >> 31) & 1) leaf1_hv = 1;
    /* CET shadow stack (7.0 ECX bit 7) and IBT (7.0 EDX bit 20): QEMU writes
     * the CET MSRs when either is set, and we do not provide them. */
    if (cp.entries[i].function == 7u && cp.entries[i].index == 0u &&
        ((cp.entries[i].ecx >> 7) & 1 || (cp.entries[i].edx >> 20) & 1))
      cet_hidden = 0;
    /* AMD PerfCore (8000_0001h ECX bit 23): QEMU writes the F15h counters. */
    if (cp.entries[i].function == 0x80000001u && (cp.entries[i].ecx >> 23) & 1)
      perfcore_hidden = 0;
  }
  check(cet_hidden, "CET is hidden in leaf 7 (ECX bit 7, EDX bit 20)");
  check(perfcore_hidden, "AMD PerfCore is hidden in leaf 0x80000001 ECX");
  check(r == 0 && cp.nent > 0 && hv_sig, "supported CPUID has the KVM hypervisor leaf");
  check(leaf1_hv, "supported CPUID leaf 1 sets the hypervisor bit");

  static struct msr_list ml;
  memset(&ml, 0, sizeof(ml));
  check(ioctl(avm, KVM_GET_MSR_INDEX_LIST, &ml) < 0 && ml.nmsrs == 2,
        "a small MSR list is refused and reports its size");
  ml.nmsrs = 8;
  r = ioctl(avm, KVM_GET_MSR_INDEX_LIST, &ml);
  check(r == 0 && ml.nmsrs == 2 && ml.indices[0] == 0x1Bu && ml.indices[1] == 0xC0000080u,
        "MSR index list has APIC base and EFER");
  struct msr_list fl = { .nmsrs = 4 };
  check(ioctl(avm, KVM_GET_MSR_FEATURE_INDEX_LIST, &fl) == 0 && fl.nmsrs == 0,
        "feature MSR list is empty");

  int vm = ioctl(avm, KVM_CREATE_VM, 0);
  if (vm < 0) goto fail;
  if (ioctl(vm, KVM_CREATE_IRQCHIP, 0) != 0) goto fail;

  uint8_t *mem = mmap(NULL, 4 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) goto fail;
  struct region rg = { .slot = 0, .gpa = 0, .size = 4 * 4096, .hva = (uint64_t)mem };
  if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &rg) != 0) goto fail;

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

  static struct msrs1 m;
  memset(&m, 0, sizeof(m));
  m.nmsrs = 1;
  m.entries[0].index = 0x1B;
  r = ioctl(vcpu, KVM_GET_MSRS, &m);
  check(r == 1 && m.entries[0].data != 0, "APIC base read through KVM_GET_MSRS");

  memset(&m, 0, sizeof(m));
  m.nmsrs = 1;
  m.entries[0].index = 0xC0000080u;
  m.entries[0].data = 0;
  check(ioctl(vcpu, KVM_SET_MSRS, &m) == 1, "EFER written through KVM_SET_MSRS");

  memset(&m, 0, sizeof(m));
  m.nmsrs = 1;
  m.entries[0].index = 0x1A0;  /* IA32_MISC_ENABLE: not in the list, not stored */
  check(ioctl(vcpu, KVM_GET_MSRS, &m) == 0, "an MSR outside the list stops the batch");

  static struct cpuid2 mine;
  memset(&mine, 0, sizeof(mine));
  mine.nent = 1;
  mine.entries[0].function = 0x40000000u;
  mine.entries[0].ebx = 0x54534554u; /* "TEST" */
  check(ioctl(vcpu, KVM_SET_CPUID2, &mine) == 0, "VMM CPUID table set");

  struct run *run = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
  if (run == MAP_FAILED) goto fail;

  char out[16];
  int n = 0, halted = 0;
  for (int exits = 0; exits < 1000 && !halted; exits++) {
    if (ioctl(vcpu, KVM_RUN, 0) != 0) {
      printf("AVM_CPUID_BOOT: KVM_RUN failed, errno %d\n", errno);
      break;
    }
    if (run->exit_reason == KVM_EXIT_IO && run->io.direction == KVM_EXIT_IO_OUT &&
        run->io.port == 0x3F8 && run->io.size == 1) {
      if (n < (int)sizeof(out) - 1) out[n++] = *((char *)run + run->io.data_offset);
    } else if (run->exit_reason == KVM_EXIT_HLT) {
      halted = 1;
    } else {
      printf("AVM_CPUID_BOOT: unexpected exit %u, hw reason 0x%llx\n", run->exit_reason,
             (unsigned long long)run->hw.hardware_exit_reason);
      break;
    }
  }
  out[n] = 0;
  printf("AVM_CPUID_BOOT: guest output %s\n", out);
  check(strcmp(out, "TZK") == 0 && halted, "guest read the VMM's CPUID table");

  if (pass) puts("AVM_CPUID_BOOT: PASS");
  else puts("AVM_CPUID_BOOT: FAIL");
  reboot(RB_POWER_OFF);
  for (;;) pause();

fail:
  printf("AVM_CPUID_BOOT: FAIL setup, errno %d\n", errno);
  reboot(RB_POWER_OFF);
  for (;;) pause();
}
'''

INIT = TEMPLATE.replace('@GUEST@', ', '.join(f'0x{b:02X}' for b in GUEST))
out = run_probe('avm-cpuid-boot', INIT, 'AVM_CPUID_BOOT: PASS',
                accel=os.environ.get('AVORY_TEST_ACCEL', 'kvm'), timeout=180)
serial = (out / 'serial.log').read_text(errors='replace')
if 'npt verify FAILED' in serial:
    raise SystemExit('AVM_CPUID_BOOT: FAIL kernel reported a fault during the test')
print('AVM_CPUID_BOOT: kernel log checked')
