#!/usr/bin/env python3
"""The real S5 path drains asynchronous UART output before removing power."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/src/acpi/acpi.c').read_text()
implementation = source[source.index('void acpi_poweroff(void)'):source.index('void acpi_reboot(void)')]
fixture = r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <setjmp.h>
struct acpi_fadt { uint32_t pm1a_cnt_blk, pm1b_cnt_blk; };
static struct acpi_fadt table={0x604,0x608};
static int present=1, flushed, writes, disabled;
static jmp_buf stop;
#define ACPI_S5_SLP_EN_QEMU 0x2000
#define ACPI_PM1_SLP_EN 0x2000
static struct acpi_fadt *acpi_get_fadt(void) { return present ? &table : NULL; }
static void console_puts(const char *text) { (void)text; }
static void serial_flush_sync(void) { assert(!writes); flushed++; }
static void outw(uint16_t port,uint16_t value) {
  assert(flushed==1 && (port==0x604 || port==0x608));
  assert(value==0x2000 || value==0x3400); writes++;
}
static void hal_irq_disable(void) { disabled++; }
static void hal_cpu_halt(void) { longjmp(stop,1); }
'''
checks = r'''
int main(void) {
  if (!setjmp(stop)) acpi_poweroff();
  assert(flushed==1 && writes==4 && disabled==1);
  flushed=writes=disabled=0; present=0;
  if (!setjmp(stop)) acpi_poweroff();
  assert(!flushed && !writes && disabled==1);
  present=1; table.pm1a_cnt_blk=0; disabled=0;
  if (!setjmp(stop)) acpi_poweroff();
  assert(!flushed && !writes && disabled==1);
}
'''
with tempfile.TemporaryDirectory(prefix='avory-s5-serial-') as temp:
    path = Path(temp)
    (path / 'test.c').write_text(fixture + implementation + checks)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS: UART drain precedes every S5 write; invalid FADT/PM1 paths halt safely')
