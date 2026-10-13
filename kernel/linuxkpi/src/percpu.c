/* Clone initialized static per-CPU templates before DRM/IDR workers start.
 * Storage is reserved in the kernel image, avoiding allocations in atomic
 * this_cpu_ptr/get_cpu_ptr callers. Native logical CPU indexes are 0..63. */
#include <linux/percpu.h>
#include <linux/string.h>
#include <linux/threads.h>
#include <linux/bug.h>
#include <linuxkpi/native.h>

#define KPI_MAX_CPUS 64
extern char __kpi_percpu_start[], __kpi_percpu_end[], __kpi_percpu_copies[];
unsigned long __per_cpu_offset[NR_CPUS];
static unsigned long kpi_offsets[KPI_MAX_CPUS];

void linuxkpi_percpu_init(void) {
  unsigned long size = __kpi_percpu_end - __kpi_percpu_start;
  for (unsigned int cpu = 1; cpu < KPI_MAX_CPUS; cpu++) {
    char *copy = __kpi_percpu_copies + (cpu - 1) * size;
    memcpy(copy, __kpi_percpu_start, size);
    kpi_offsets[cpu] = (unsigned long)copy - (unsigned long)__kpi_percpu_start;
  }
  for (unsigned int cpu = 0; cpu < NR_CPUS; cpu++)
    __per_cpu_offset[cpu] = kpi_offsets[cpu];
}

unsigned long linuxkpi_percpu_offset(unsigned int cpu) {
  BUG_ON(cpu >= KPI_MAX_CPUS);
  return kpi_offsets[cpu];
}

unsigned int linuxkpi_percpu_cpu(void) {
  int cpu = linuxkpi_cpu_id();
  BUG_ON(cpu < 0 || cpu >= KPI_MAX_CPUS);
  return cpu;
}
