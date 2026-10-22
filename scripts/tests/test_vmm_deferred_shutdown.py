#!/usr/bin/env python3
"""Device teardown must wait for queued AND in-flight retired VMA releases."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/src/mm/vmm_free.c').read_text()
implementation = source[source.index('struct vmm_defer_node {'):]
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include <unistd.h>
typedef pthread_mutex_t spinlock_t;
#define SPINLOCK_INIT PTHREAD_MUTEX_INITIALIZER
#define spinlock_acquire(lock) assert(!pthread_mutex_lock(lock))
#define spinlock_release(lock) assert(!pthread_mutex_unlock(lock))
struct vma_list { int references; };
struct mm_struct { struct vma_list vmas; unsigned pcid; };
static atomic_int released, active, unblock, synchronized;
static int blocking;
struct thread { int state; uint64_t wakeup_ticks; };
static _Thread_local struct thread current;
#define THREAD_SLEEPING 1
#define THREAD_RUNNING 0
static struct thread *sched_get_current(void) { return &current; }
static uint64_t lapic_timer_get_ticks(void) { return 10; }
static void test_yield(void) { assert(current.state==THREAD_SLEEPING && current.wakeup_ticks==11); usleep(1000); }
static void *kmalloc(size_t size) { return malloc(size); }
static void kfree(void *pointer) { free(pointer); }
static void vma_list_init(struct vma_list *list) { list->references=0; }
static void vma_list_destroy(struct vma_list *list) {
  if(blocking) { atomic_store(&active,1); while(!atomic_load(&unblock)) sched_yield(); }
  atomic_fetch_add(&released,list->references); list->references=0;
}
static void vmm_free_user_pages_vma(uint64_t cr3,struct vma_list *list) { (void)cr3; (void)list; }
static void pcid_free(unsigned pcid) { assert(pcid==7); }
#define sched_yield test_yield
'''
checks = r'''
#undef sched_yield
static void *drainer(void *unused) { (void)unused; vmm_defer_drain(1); return NULL; }
static void *barrier(void *unused) { (void)unused; vmm_defer_sync(); atomic_store(&synchronized,1); return NULL; }
int main(void) {
  struct vma_list retired={3};
  vmm_queue_free_user_pages_vma(1,&retired); assert(!retired.references && vmm_defer_pending()==1);
  vmm_defer_sync(); assert(atomic_load(&released)==3 && !vmm_defer_pending());
  struct mm_struct *mm=malloc(sizeof(*mm)); mm->vmas.references=5; mm->pcid=7;
  vmm_queue_free_mm(2,mm); blocking=1;
  pthread_t worker, waiter;
  assert(!pthread_create(&worker,NULL,drainer,NULL));
  while(!atomic_load(&active)) sched_yield();
  assert(!vmm_defer_pending());
  assert(!pthread_create(&waiter,NULL,barrier,NULL));
  usleep(20000); assert(!atomic_load(&synchronized));
  atomic_store(&unblock,1);
  assert(!pthread_join(worker,NULL) && !pthread_join(waiter,NULL));
  assert(atomic_load(&synchronized) && atomic_load(&released)==8 && !vmm_defer_pending());
}
'''
with tempfile.TemporaryDirectory(prefix='vmm-shutdown-') as temp:
    path = Path(temp)
    (path/'test.c').write_text(fixture + implementation + checks)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-pthread', '-fsanitize=address,undefined',
                    str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True, timeout=10)
reboot = (root/'kernel/src/syscalls/sys_process.c').read_text()
for label in ('System reboot requested', 'System power-off requested', 'System halt requested'):
    branch = reboot[reboot.index(label):]
    assert branch.index('vmm_defer_sync();') < branch.index('linuxkpi_pci_shutdown();')
print('PASS: queued and in-flight VMA/file releases finish before reboot/poweroff/halt device removal')
