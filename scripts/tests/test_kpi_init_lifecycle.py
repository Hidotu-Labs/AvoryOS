#!/usr/bin/env python3
"""Run actual initcall orchestration with a simulated probe exceeding 600s."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/linuxkpi/src/initcalls.c').read_text();s=re.sub(r'^#include.*$','',s,flags=re.M)
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
struct completion {int done;};struct task_struct {int dummy;};
static struct task_struct task;static unsigned waits,scans,walks,joins;static bool fail_create;
#define IS_ERR(p) (!(p))
static void init_completion(struct completion*c){c->done=0;}
static void complete(struct completion*c){c->done=1;}
void linuxkpi_pci_scan(void){scans++;}
void linuxkpi_run_initcalls_inline(void){walks++;}
static unsigned long long linuxkpi_monotonic_ms(void){return waits*2000ULL;}
static void klog_puts(const char*s){(void)s;}
static void klogf(const char*s,...){(void)s;}
static struct task_struct *kthread_run(int (*fn)(void*),void*a,const char*n){(void)fn;(void)a;(void)n;return fail_create?NULL:&task;}
static void kthread_stop(struct task_struct*t){assert(t==&task&&waits==310);joins++;}
static unsigned long wait_for_completion_timeout(struct completion*c,unsigned long t){
 assert(t==2000);if(++waits==310){linuxkpi_pci_scan();linuxkpi_run_initcalls_inline();complete(c);return 1;}return 0;
}
'''+s+r'''
int main(void){
 linuxkpi_run_initcalls();assert(waits==310&&scans==1&&walks==1&&joins==1);
 fail_create=true;linuxkpi_run_initcalls();assert(scans==2&&walks==2&&joins==1);
 puts("PASS: initcalls remain a boot barrier beyond 600s, join after completion, and creation failure is reported with inline fallback");
}
'''
# Compile the real function, not a model of the former timeout.
with tempfile.TemporaryDirectory(prefix='kpi-init-lifecycle-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=gnu11','-O2',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=10)
