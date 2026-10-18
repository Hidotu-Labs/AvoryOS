#!/usr/bin/env python3
"""Exercise actual KSM scan budgets, cursor progress, and one-time init."""
from pathlib import Path
import ast
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
parsed=ast.parse((ROOT/'scripts/tests/test_ksm_lifetime.py').read_text())
prefix=next(ast.literal_eval(n.value) for n in parsed.body if isinstance(n,ast.Assign)
            and any(isinstance(t,ast.Name) and t.id=='prefix' for t in n.targets))
prefix=prefix.replace('struct thread { bool is_idle;', 'struct thread { char comm[16]; bool is_idle;')
prefix=prefix.replace('static uint64_t *vmm_get_pte_ptr', 'static unsigned walks;\nstatic bool sparse;\nstatic uint64_t *vmm_get_pte_ptr')
prefix=prefix.replace('(void)cr3; assert(va/PAGE_SIZE<6);', '(void)cr3; if(sparse) {walks++;return NULL;} assert(va/PAGE_SIZE<6);')
s=(ROOT/'kernel/src/mm/ksm.c').read_text()
scan=s[s.index('#define KSM_BUCKETS'):s.index('static void ksm_worker')]
init=s[s.index('void ksm_init(void)'):s.index('void ksm_get_stats')]
assert 'ksm_watchdog_check' not in (ROOT/'kernel/src/apic/lapic_timer.c').read_text()
stubs=r'''
#define SCHED_PRIORITY_BACKGROUND 24
static unsigned creations;
static struct thread daemon;
static void ksm_worker(void) {}
static void *kmalloc(size_t size) { return malloc(size); }
static struct thread *sched_create_kernel_thread(void (*fn)(void),void *arg,bool detached) {
 assert(fn==ksm_worker && !arg && detached);creations++;return &daemon;
}
static void sched_set_priority(struct thread *t,unsigned prio,int nice) {
 assert(t==&daemon && prio==SCHED_PRIORITY_BACKGROUND && nice==19);
}
'''
test=r'''
int main(void) {
 ksm_init();void *pool=ksm_node_pool;ksm_init();
 assert(pool && ksm_node_pool==pool && creations==1 && !strcmp(daemon.comm,"ksmd"));
 struct vma v={.end=1ULL<<40,.flags=MAP_PRIVATE|MAP_ANONYMOUS,.prot=PROT_WRITE};
 struct mm_struct mm={.ref_count=1,.vmas={.root=&v}};
 struct thread t={.mm=&mm,.cr3=4096};global_thread_list=&t;sparse=true;
 for(unsigned run=1;run<=4;run++) {
  walks=0;assert(ksm_scan_pass()==0);
  assert(walks==KSM_MM_WORK_BUDGET-1);
  assert(mm.ksm_scan_cursor==(uint64_t)run*(KSM_MM_WORK_BUDGET-1)*PAGE_SIZE);
  assert(mm.ref_count==1 && !held && !flushes);
 }
 v.end=mm.ksm_scan_cursor+PAGE_SIZE;
 walks=0;assert(!ksm_scan_pass() && walks==1 && mm.ksm_scan_cursor==0);
 /* Ineligible VMA visits must also be bounded; never scan an entire tree. */
 struct vma many[300]={0};
 for(unsigned i=0;i<300;i++) {
  many[i].start=i*PAGE_SIZE;many[i].end=(i+1)*PAGE_SIZE;
  many[i].right=i<299?&many[i+1]:NULL;
 }
 mm.vmas.root=many;assert(!ksm_scan_pass());
 assert(mm.ksm_scan_cursor==KSM_MM_WORK_BUDGET*PAGE_SIZE);
 assert(!ksm_scan_pass() && mm.ksm_scan_cursor==0);
 free(ksm_node_pool);
 puts("PASS: one background daemon, bounded sparse/VMA work, cursor progress/wrap, safe empty batches");
}
'''
with tempfile.TemporaryDirectory(prefix='ksm-budget-') as tmp:
    c=Path(tmp)/'test.c';exe=Path(tmp)/'test'
    c.write_text(prefix+scan+stubs+init+test)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-variable',
                    '-fsanitize=address,undefined','-g',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=10)
