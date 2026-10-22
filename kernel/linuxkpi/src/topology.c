/* SPDX-License-Identifier: GPL-2.0-only */
/* Package core count consumed by AMD's SMU support. Derive it from the
 * CPU topology exposed to this kernel, not the host's online CPU count. */
#include <linux/init.h>
#include <linux/types.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
unsigned int __num_cores_per_package = 1;

static void kpi_topology_cpuid(unsigned leaf, unsigned subleaf,
                               unsigned *a, unsigned *b, unsigned *c, unsigned *d) {
  __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                   : "0"(leaf), "2"(subleaf));
}

static int __init kpi_topology_init(void) {
  unsigned a, b, c, d, maxleaf, smt = 1, package = 0;
  kpi_topology_cpuid(0, 0, &maxleaf, &b, &c, &d);
  unsigned leaf = maxleaf >= 0x1f ? 0x1f : maxleaf >= 0xb ? 0xb : 0;
  if (leaf) {
    for (unsigned level = 0; level < 32; level++) {
      kpi_topology_cpuid(leaf, level, &a, &b, &c, &d);
      unsigned type = (c >> 8) & 0xff, count = b & 0xffff;
      if (!type || !count)
        break;
      if (type == 1)
        smt = count;
      package = count;
    }
  }
  if (!package) {
    kpi_topology_cpuid(0x80000000, 0, &maxleaf, &b, &c, &d);
    if (maxleaf >= 0x80000008) {
      kpi_topology_cpuid(0x80000008, 0, &a, &b, &c, &d);
      package = (c & 0xff) + 1;
      if (maxleaf >= 0x8000001e) {
        kpi_topology_cpuid(0x8000001e, 0, &a, &b, &c, &d);
        smt = ((b >> 8) & 0xff) + 1;
      }
    }
  }
  if (package >= smt)
    __num_cores_per_package = package / smt;
  return 0;
}
core_initcall(kpi_topology_init);
#endif
