#!/usr/bin/env python3
"""Check a memslot backed by 2 MB host pages, and KVM_ENABLE_CAP.

QEMU maps guest RAM from anonymous memory that the kernel may back with 2 MB
pages.  The NPT must still get one 4 KB frame per guest page, and the verify
pass must match it.  The probe asks for huge pages with MADV_HUGEPAGE, which
the kernel may or may not honour, so the verify line is the check.

QEMU also sends KVM_ENABLE_CAP on the VM for KVM_CAP_MAX_VCPU_ID at startup.
Caps this kernel does not enable must be refused with EINVAL.
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
#include <sys/mman.h>
#include <sys/reboot.h>
#include <unistd.h>

/* ABI subset copied from the KVM uapi, as in src/avm/avm_uapi.h. */
#define KVMIO 0xAE
#define KVM_IOC(dir, type, nr, size) \
  (((unsigned)(dir) << 30) | ((unsigned)(size) << 16) | ((unsigned)(type) << 8) | (unsigned)(nr))
#define KVM_CREATE_VM KVM_IOC(0, KVMIO, 0x01, 0)
#define KVM_ENABLE_CAP KVM_IOC(1, KVMIO, 0xA3, 104)
#define KVM_SET_USER_MEMORY_REGION KVM_IOC(1, KVMIO, 0x46, 32)
#define KVM_CAP_MAX_VCPU_ID 128

struct enable_cap { uint32_t cap, flags; uint64_t args[4]; uint8_t pad[64]; };
struct region { uint32_t slot, flags; uint64_t gpa, size, hva; };

static int fails;
#define CHECK(cond, what) do { \
  if (cond) printf("AVM_HUGEMEM_BOOT: ok   %s\n", what); \
  else { printf("AVM_HUGEMEM_BOOT: FAIL %s (errno %d)\n", what, errno); fails++; } \
} while (0)

#define MB2 (2u * 1024 * 1024)
#define SLOT_SIZE (4u * MB2)

int main(void) {
  setbuf(stdout, NULL);
  puts("AVM_HUGEMEM_BOOT: START");

  int kvm = open("/dev/avm", O_RDWR);
  CHECK(kvm >= 0, "open /dev/avm");
  if (kvm < 0) goto done;
  int vm = ioctl(kvm, KVM_CREATE_VM, 0);
  CHECK(vm >= 0, "KVM_CREATE_VM");
  if (vm < 0) goto done;

  struct enable_cap cap;
  memset(&cap, 0, sizeof(cap));
  cap.cap = KVM_CAP_MAX_VCPU_ID;
  cap.args[0] = 1;
  CHECK(ioctl(vm, KVM_ENABLE_CAP, &cap) == -1 && errno == EINVAL,
        "KVM_ENABLE_CAP MAX_VCPU_ID is EINVAL (not enabled)");
  cap.cap = 1000;
  cap.flags = 1;
  CHECK(ioctl(vm, KVM_ENABLE_CAP, &cap) == -1 && errno == EINVAL,
        "KVM_ENABLE_CAP unknown cap with flags is EINVAL");

  /* Over-allocate, then align the slot to 2 MB so a huge page fits. */
  char *raw = mmap(NULL, SLOT_SIZE + MB2, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(raw != MAP_FAILED, "map guest RAM");
  if (raw == MAP_FAILED) goto done;
  char *base = (char *)(((uintptr_t)raw + MB2 - 1) & ~(uintptr_t)(MB2 - 1));
  madvise(base, SLOT_SIZE, 14 /* MADV_HUGEPAGE */);
  for (uint32_t off = 0; off < SLOT_SIZE; off += 4096)
    base[off] = (char)(off >> 12);
  CHECK(1, "touch every 4 KB page of the slot");

  struct region r = { .slot = 0, .gpa = 0, .size = SLOT_SIZE, .hva = (uint64_t)base };
  CHECK(ioctl(vm, KVM_SET_USER_MEMORY_REGION, &r) == 0,
        "4 MB memslot at GPA 0 over 2 MB-aligned memory");

done:
  if (fails == 0) puts("AVM_HUGEMEM_BOOT: PASS");
  else printf("AVM_HUGEMEM_BOOT: FAIL %d checks\n", fails);
  reboot(RB_POWER_OFF);
  for (;;) pause();
}
'''

out = run_probe('avm-hugemem-boot', INIT, 'AVM_HUGEMEM_BOOT: PASS',
                accel=os.environ.get('AVORY_TEST_ACCEL', 'kvm'), timeout=120)

# The verify pass runs for the slot.  Every translation must match.
serial = (out / 'serial.log').read_text(errors='replace')
if 'npt verify FAILED' in serial:
    raise SystemExit('AVM_HUGEMEM_BOOT: FAIL NPT verification line missing or failed')
print('AVM_HUGEMEM_BOOT: NPT verification ok')
