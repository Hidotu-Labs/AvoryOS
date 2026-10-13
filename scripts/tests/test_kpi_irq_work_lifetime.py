#!/usr/bin/env python3
"""Run actual irq_work queue/worker with self-requeue and self-free callbacks."""
from pathlib import Path
import re
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/linuxkpi/src/irq_work.c').read_text()
source = source[:source.index('void irq_work_sync(')]
source = re.sub(r'^#include.*$', '', source, flags=re.M)
shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
struct irq_work {struct irq_work *next; void (*func)(struct irq_work*); int queued;};
struct wait_queue_head {int dummy;};
struct kpi_service {int dummy;};
#define DEFINE_SPINLOCK(name) int name
#define spin_lock_irqsave(lock, flags) do{(void)(lock);(flags)=0;}while(0)
#define spin_unlock_irqrestore(lock, flags) do{(void)(lock);(void)(flags);}while(0)
#define wait_event_interruptible_timeout(head, cond, time) ((void)(head),(void)(cond),(void)(time),0)
#define KPI_SERVICE_IDLE_MS 10
#define msecs_to_jiffies(n) (n)
static unsigned kpi_service_gen(struct kpi_service *s){(void)s;return 0;}
static bool kthread_should_stop(void){return false;}
static void kpi_service_forget(struct kpi_service *s){(void)s;}
static bool kpi_service_retire(struct kpi_service *s,unsigned g){(void)s;(void)g;return true;}
static void kpi_service_kick(struct kpi_service *s,bool wake){(void)s;(void)wake;}
'''
main = r'''
static int callbacks;
static void free_callback(struct irq_work *work){++callbacks;free(work);}
static void requeue_callback(struct irq_work *work){
 ++callbacks;
 if(callbacks & 1){
  assert(irq_work_queue(work) && "running callback must permit a new pending instance");
  assert(!irq_work_queue(work) && "new pending instance must reject duplicates");
 }else free(work);
}
int main(int argc,char **argv){
 (void)argv;
 for(int i=0;i<1000;i++){
  struct irq_work *work=malloc(sizeof(*work));assert(work);
  init_irq_work(work,argc>1?requeue_callback:free_callback);
  assert(irq_work_queue(work));assert(!irq_work_queue(work));
  assert(irq_work_kthread(NULL)==0);
  assert(!irq_work_head&&!irq_work_tail);
 }
 assert(callbacks==(argc>1?2000:1000));
 puts("PASS: irq_work callbacks self-free and self-requeue without stale writes or lost claims");
}
'''
with tempfile.TemporaryDirectory(prefix='kpi-irq-work-') as directory:
 path=Path(directory)
 for name,implementation in [('fixed',source),('old post-callback write',source.replace('      work->queued = 0;','').replace('      func(work);','      func(work);\n    if(work) work->queued = 0;'))]:
  (path/'test.c').write_text(shim+implementation+main)
  subprocess.run(['cc','-std=gnu11','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(path/'test.c'),'-o',str(path/'test')],check=True)
  for arguments in [[],['requeue']]:
   result=subprocess.run([str(path/'test'),*arguments],capture_output=True,text=True,timeout=5)
   if name=='fixed':
    assert result.returncode==0,result.stderr
   else:
    assert result.returncode!=0 and ('heap-use-after-free' in result.stderr or 'running callback must permit' in result.stderr),result
  print('PASS: '+name+(' rejects the old callback lifetime violation' if name!='fixed' else ' runs 1000 self-free and 2000 self-requeue callbacks under ASan/UBSan'))
