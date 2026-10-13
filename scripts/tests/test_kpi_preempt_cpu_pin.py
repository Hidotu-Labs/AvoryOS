#!/usr/bin/env python3
"""Inject migration after CPU lookup in the real LinuxKPI preempt code."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "kernel/linuxkpi/src/preempt.c").read_text()
source = source[:source.index("/* might_sleep() backing")]
source = re.sub(r"^#include.*$", "", source, flags=re.M)
shim = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
static unsigned int preempt_counts[64];
static int cpu, migrations;
static bool irq_enabled, armed;
static void attempt_migration(void) {
  if (armed && irq_enabled && !preempt_counts[cpu]) {
    armed = false;
    cpu ^= 1;
    ++migrations;
  }
}
static int linuxkpi_cpu_id(void) {
  int selected = cpu;
  attempt_migration();
  return selected;
}
static unsigned long save_irq(void) {
  unsigned long flags = irq_enabled ? 512 : 0;
  irq_enabled = false;
  return flags;
}
static void restore_irq(unsigned long flags) {
  irq_enabled = !!(flags & 512);
  attempt_migration();
}
#define local_irq_save(flags) ((flags) = save_irq())
#define local_irq_restore(flags) restore_irq(flags)
#define irqs_disabled() (!irq_enabled)
static int linuxkpi_irq_depth(void) { return 0; }
static int linuxkpi_softirq_depth(void) { return 0; }
static bool linuxkpi_need_resched(void) { return false; }
static void linuxkpi_yield(void) {}
unsigned int __kpi_preempt_count(void);
bool __kpi_in_interrupt(void);
"""
# The real source owns the array. The shim declares it tentatively so the
# injected interrupt can consult the same counters as the imported code.
main = r"""
int main(void) {
  for (int enabled = 0; enabled < 2; ++enabled) {
    for (int iteration = 0; iteration < 1000; ++iteration) {
      cpu = iteration & 1;
      preempt_counts[0] = preempt_counts[1] = 0;
      irq_enabled = enabled;
      armed = true;
      int original = cpu, before = migrations;
      __kpi_preempt_disable();
      assert(cpu == original && "CPU migrated before preemption pin");
      assert(preempt_counts[cpu] == 1);
      assert(preempt_counts[cpu ^ 1] == 0);
      assert(irq_enabled == (bool)enabled);
      __kpi_preempt_disable();
      assert(__kpi_preempt_count() == 2);
      __kpi_preempt_enable();
      assert(cpu == original && __kpi_preempt_count() == 1);
      __kpi_preempt_enable();
      assert(!preempt_counts[0] && !preempt_counts[1]);
      assert(irq_enabled == (bool)enabled);
      assert(migrations == before + enabled);
      if (!enabled) {
        assert(armed);
        restore_irq(512);
        assert(migrations == before + 1);
      }
    }
  }
  puts("PASS: 2000 CPU-lookup migration injections, nested pinning and IRQ restoration");
}
"""
with tempfile.TemporaryDirectory(prefix="kpi-preempt-pin-") as directory:
    path = Path(directory)
    for name, implementation in [
        ("fixed", source),
        ("unprotected CPU lookup", re.sub(r"^  (?:local_irq_save\(flags\);|local_irq_restore\(flags\);)$", "", source, flags=re.M)),
    ]:
        (path / "test.c").write_text(shim + implementation + main)
        subprocess.run(["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable", "-Wno-unused-function", str(path / "test.c"), "-o", str(path / "test")], check=True)
        result = subprocess.run([str(path / "test")], capture_output=True, text=True, timeout=5)
        if name == "fixed":
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
        else:
            assert result.returncode != 0 and "CPU migrated before preemption pin" in result.stderr, result
            print("PASS: original unprotected CPU lookup reproduces migration before pinning")
