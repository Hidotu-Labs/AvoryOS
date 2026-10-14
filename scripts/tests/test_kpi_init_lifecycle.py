#!/usr/bin/env python3
"""Run actual initcall orchestration with a simulated probe exceeding 600s."""
from pathlib import Path
import os
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=Path(os.environ.get('KPI_INITCALL_SOURCE',str(root/'kernel/linuxkpi/src/initcalls.c'))).read_text();s=re.sub(r'^#include.*$','',s,flags=re.M)
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
struct completion {int done;};struct task_struct {void *kpi_thread;};
static struct task_struct task;static unsigned waits,scans,walks,joins;static bool fail_create,abort_worker;
static int aborted_log;
#include <setjmp.h>
#include <string.h>
static jmp_buf abort_env;
#define READ_ONCE(x) (x)
static void msleep(unsigned n){assert(n==2000);}
#define IS_ERR(p) (!(p))
static void init_completion(struct completion*c){c->done=0;}
static void complete(struct completion*c){c->done=1;}
void linuxkpi_pci_scan(void){scans++;}
void linuxkpi_run_initcalls_inline(void){walks++;}
static unsigned long long linuxkpi_monotonic_ms(void){return waits*2000ULL;}
static void klog_puts(const char*s){if(strstr(s,"exited before finishing"))aborted_log++;}
static void klogf(const char*s,...){(void)s;}
static struct task_struct *kthread_run(int (*fn)(void*),void*a,const char*n){(void)fn;(void)a;(void)n;task.kpi_thread=&task;return fail_create?NULL:&task;}
static void kthread_stop(struct task_struct*t){assert(t==&task&&waits==310);joins++;}
static int linuxkpi_initcalls_thread(void *arg);
static unsigned long wait_for_completion_timeout(struct completion*c,unsigned long t){
 assert(t==2000);++waits;
 if(abort_worker){task.kpi_thread=NULL;if(waits==5)longjmp(abort_env,1);return 0;}
 if(waits==1){complete(c);return 1;} /* stray token must not open the boot gate */
 if(waits==310){linuxkpi_initcalls_thread(NULL);return 1;}return 0;
}
'''+s+r'''
int main(void){
 linuxkpi_run_initcalls();assert(waits==310&&scans==1&&walks==1&&joins==1);
 fail_create=true;linuxkpi_run_initcalls();assert(scans==2&&walks==2&&joins==1);
 fail_create=false;abort_worker=true;waits=0;
 if(!setjmp(abort_env)){linuxkpi_run_initcalls();assert(!"aborted walker must never return");}
 assert(aborted_log==1&&joins==1&&scans==2&&walks==2);
 puts("PASS: initcalls remain a boot barrier beyond 600s, join after completion, creation failure is reported, stray completion cannot open boot, and aborted worker is reported while boot remains blocked");
}
'''
# Compile the real function, not a model of the former timeout.
with tempfile.TemporaryDirectory(prefix='kpi-init-lifecycle-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=gnu11','-O2',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=10)
