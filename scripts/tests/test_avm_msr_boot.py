#!/usr/bin/env python3
"""Check the MCE setup and the VMM-side MSRs that QEMU writes on vCPU reset.

QEMU calls KVM_X86_SETUP_MCE with its bank count (10 by default), then
KVM_SET_MSRS for TSC, SYSENTER, PAT, MTRR and the machine-check MSRs, and reads
them back with KVM_GET_MSRS.  Each batch must complete in full, or QEMU stops.
A bank past the chosen count must be refused.
"""
import os
from boot_probe import run_probe

INIT = r'''
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <unistd.h>

/* ABI subset copied from the KVM uapi, as in src/avm/avm_uapi.h. */
#define KVMIO 0xAE
#define KVM_IOC(dir, type, nr, size) \
  (((unsigned)(dir) << 30) | ((unsigned)(size) << 16) | ((unsigned)(type) << 8) | (unsigned)(nr))
#define KVM_CREATE_VM KVM_IOC(0, KVMIO, 0x01, 0)
#define KVM_CREATE_VCPU KVM_IOC(0, KVMIO, 0x41, 0)
#define KVM_X86_SETUP_MCE KVM_IOC(1, KVMIO, 0x9C, 8)
#define KVM_GET_MSRS KVM_IOC(3, KVMIO, 0x88, 8)
#define KVM_SET_MSRS KVM_IOC(1, KVMIO, 0x89, 8)

struct msr_entry { uint32_t index, reserved; uint64_t data; };
struct msrs { uint32_t nmsrs, pad; struct msr_entry e[128]; };

static int fails;
#define CHECK(cond, what) do { \
  if (cond) printf("AVM_MSR_BOOT: ok   %s\n", what); \
  else { printf("AVM_MSR_BOOT: FAIL %s (errno %d)\n", what, errno); fails++; } \
} while (0)

/* The batch QEMU writes for an AMD vCPU with 10 MCE banks: 88 entries. */
static uint32_t fill(struct msrs *m) {
  uint32_t n = 0;
#define ADD(i, d) do { m->e[n].index = (i); m->e[n].reserved = 0; m->e[n].data = (d); n++; } while (0)
  ADD(0x010, 0x1234);                   /* IA32_TSC */
  ADD(0x174, 0x8);                      /* SYSENTER_CS */
  ADD(0x175, 0x1000);                   /* SYSENTER_ESP */
  ADD(0x176, 0x2000);                   /* SYSENTER_EIP */
  ADD(0x277, 0x0007040600070406ull);    /* PAT */
  ADD(0x2FF, 0xC00);                    /* MTRRdefType */
  ADD(0x250, 0x0606060606060606ull);    /* MTRRfix64K_00000 */
  ADD(0x258, 0x0505050505050505ull);    /* MTRRfix16K_80000 */
  ADD(0x259, 0x0404040404040404ull);    /* MTRRfix16K_A0000 */
  for (int i = 0; i < 8; i++)
    ADD(0x268 + i, 0x0101010101010101ull * (i + 1));  /* MTRRfix4K_C0000.. */
  for (int i = 0; i < 16; i++)
    ADD(0x200 + i, 0x1000 + i);         /* MTRRphysBase/Mask 0..7 */
  ADD(0x17A, 0);                        /* MCG_STATUS */
  ADD(0x17B, ~0ull);                    /* MCG_CTL */
  for (int i = 0; i < 40; i++)
    ADD(0x400 + i, 0x5000 + i);         /* MC0_CTL.. for 10 banks */
  ADD(0xC0000081, 0x0023001000000000ull);  /* STAR */
  ADD(0xC0000082, 0xFFFFFFFF80001000ull);  /* LSTAR */
  ADD(0xC0000083, 0xFFFFFFFF80002000ull);  /* CSTAR */
  ADD(0xC0000084, 0x4700ull);              /* SFMASK */
  ADD(0xC0000102, 0x0ull);                 /* KERNEL_GS_BASE */
  for (int i = 0; i < 4; i++)
    ADD(0xC0010000 + i, 0x100 + i);        /* K7 EVNTSEL0..3 */
  for (int i = 0; i < 4; i++)
    ADD(0xC0010004 + i, 0x200 + i);        /* K7 PERFCTR0..3 */
#undef ADD
  m->nmsrs = n;
  return n;
}

int main(void) {
  setbuf(stdout, NULL);
  puts("AVM_MSR_BOOT: START");

  int kvm = open("/dev/avm", O_RDWR);
  CHECK(kvm >= 0, "open /dev/avm");
  if (kvm < 0) goto done;

  int vm = ioctl(kvm, KVM_CREATE_VM, 0);
  CHECK(vm >= 0, "KVM_CREATE_VM");
  if (vm < 0) goto done;
  int vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
  CHECK(vcpu >= 0, "KVM_CREATE_VCPU 0");
  if (vcpu < 0) goto done;

  /* MCG_CTL_P with 10 banks, which is what QEMU sends after masking. */
  uint64_t cap = 0x10a;
  CHECK(ioctl(vcpu, KVM_X86_SETUP_MCE, &cap) == 0, "SETUP_MCE MCG_CTL_P | 10 banks");
  cap = 0x100;
  CHECK(ioctl(vcpu, KVM_X86_SETUP_MCE, &cap) == -1 && errno == EINVAL, "SETUP_MCE zero banks is EINVAL");
  cap = 0x121;
  CHECK(ioctl(vcpu, KVM_X86_SETUP_MCE, &cap) == -1 && errno == EINVAL, "SETUP_MCE 33 banks is EINVAL");
  cap = 0x1000000 | 0x10a;
  CHECK(ioctl(vcpu, KVM_X86_SETUP_MCE, &cap) == -1 && errno == EINVAL,
        "SETUP_MCE MCG_SER_P is EINVAL (not implemented)");

  static struct msrs set, get, want;
  uint32_t n = fill(&set);
  want = set;
  CHECK(n == 88, "batch has 88 entries");
  CHECK(ioctl(vcpu, KVM_SET_MSRS, &set) == (int)n, "KVM_SET_MSRS completes the whole batch");

  get = set;
  for (uint32_t i = 0; i < n; i++) get.e[i].data = 0;
  CHECK(ioctl(vcpu, KVM_GET_MSRS, &get) == (int)n, "KVM_GET_MSRS completes the whole batch");

  uint32_t bad = 0;
  for (uint32_t i = 0; i < n; i++)
    if (get.e[i].index != want.e[i].index || get.e[i].data != want.e[i].data) {
      printf("AVM_MSR_BOOT: mismatch index 0x%x got 0x%llx want 0x%llx\n", get.e[i].index,
             (unsigned long long)get.e[i].data, (unsigned long long)want.e[i].data);
      bad++;
    }
  CHECK(bad == 0, "every value reads back as written");

  /* Bank 9 (MISC, 0x427) exists; bank 10 (CTL, 0x428) is past the 10 banks. */
  struct msrs one;
  memset(&one, 0, sizeof(one));
  one.nmsrs = 1;
  one.e[0].index = 0x427;
  one.e[0].data = 1;
  CHECK(ioctl(vcpu, KVM_SET_MSRS, &one) == 1, "bank 9 MISC is accepted");
  one.e[0].index = 0x428;
  CHECK(ioctl(vcpu, KVM_SET_MSRS, &one) == 0, "bank 10 CTL is refused, with 0 done");

  one.e[0].index = 0xDEAD;
  CHECK(ioctl(vcpu, KVM_GET_MSRS, &one) == 0, "unknown MSR is refused, with 0 done");

done:
  if (fails == 0) puts("AVM_MSR_BOOT: PASS");
  else printf("AVM_MSR_BOOT: FAIL %d checks\n", fails);
  reboot(RB_POWER_OFF);
  for (;;) pause();
}
'''

run_probe('avm-msr-boot', INIT, 'AVM_MSR_BOOT: PASS',
          accel=os.environ.get('AVORY_TEST_ACCEL', 'kvm'), timeout=120)
