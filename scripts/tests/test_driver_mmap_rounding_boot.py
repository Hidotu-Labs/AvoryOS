#!/usr/bin/env python3
"""Check page-rounded LinuxKPI device and dma-buf mappings in a private VM."""
from boot_probe import run_probe

INIT = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <unistd.h>

int main(void) {
  puts("DRIVER_MMAP_ROUNDING: START");
  int failures = 0;
  int dev = open("/dev/kpi_dmabuf", O_RDWR);
  int bo = dev < 0 ? -1 : ioctl(dev, _IOWR('K', 1, unsigned long), 2UL);
  const size_t sizes[] = {1, 0x250, 4095, 4096, 4097, 8191, 8192};
  if (bo < 0) {
    printf("DRIVER_MMAP_ROUNDING: allocation failed errno=%d\n", errno);
    failures++;
  } else {
    for (unsigned which = 0; which < 2; which++) {
      for (unsigned fixed = 0; fixed < 2; fixed++) {
        for (unsigned i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
          size_t rounded = (sizes[i] + 4095) & ~(size_t)4095;
          void *reserved = fixed ? mmap(NULL, rounded, PROT_NONE,
              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) : NULL;
          if (reserved == MAP_FAILED) { failures++; continue; }
          unsigned char *p = mmap(reserved, sizes[i], PROT_READ | PROT_WRITE,
              MAP_SHARED | (fixed ? MAP_FIXED : 0), which ? bo : dev, 0);
          if (p == MAP_FAILED) {
            printf("DRIVER_MMAP_ROUNDING: fd-kind=%u fixed=%u size=%zu errno=%d\n",
                   which, fixed, sizes[i], errno);
            failures++;
            if (fixed) munmap(reserved, rounded);
            continue;
          }
          /* Linux permits access through the rounded end, even when the
           * requested byte count ends in the middle of that page. */
          p[0] = 0x37; p[rounded-1] = 0xa6;
          if (p[0] != 0x37 || p[rounded-1] != 0xa6) failures++;
          if (munmap(p, sizes[i])) failures++;
        }
      }
    }
    close(bo);
  }
  if (dev >= 0) close(dev);
  printf("DRIVER_MMAP_ROUNDING: %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
  fflush(stdout); sync(); reboot(RB_POWER_OFF);
  for (;;) pause();
}
'''

if __name__ == '__main__':
    run_probe('driver-mmap-rounding', INIT, 'DRIVER_MMAP_ROUNDING: PASS')
