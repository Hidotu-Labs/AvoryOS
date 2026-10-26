#!/usr/bin/env python3
"""Run the real KSM scan with deterministic stale-TLB and CoW interleavings."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/src/mm/ksm.c').read_text()
scan = source[source.index('#define KSM_BUCKETS'):source.index('static void ksm_worker')]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define PAGE_SIZE 4096
#define PAGE_MASK (~4095ULL)
#define PAGE_FLAG_PRESENT 1
#define PAGE_FLAG_RW 2
#define PAGE_FLAG_USER 4
#define PAGE_FLAG_COW 512
#define MAP_PRIVATE 1
#define MAP_ANONYMOUS 2
#define MAP_SHARED 4
#define MAP_SYSV_SHM 8
#define PROT_WRITE 2
#define THREAD_DEAD 3
#define THREAD_ZOMBIE 4
#define PHYS_TO_VIRT(p) ((void *)(ram + (p)))
typedef int spinlock_t;
static int held;
static bool irq_enabled = true;
static bool hal_irq_enabled(void) { return irq_enabled && !held; }
static void on_mm_lock(spinlock_t *l);
static void spinlock_acquire(spinlock_t *l) { on_mm_lock(l); assert(!*l); *l=1; held++; }
static void spinlock_release(spinlock_t *l) { assert(*l); *l=0; held--; }
struct vma { uint64_t start,end; unsigned flags,prot; void *linux_vma,*file_node; struct vma *left,*right; };
struct mm_struct { uint64_t ksm_scan_cursor; unsigned ref_count; spinlock_t lock; struct {struct vma *root;} vmas; };
struct thread { bool is_idle; unsigned state; struct mm_struct *mm; uint64_t cr3; struct thread *global_next; };
struct ksm_stats { uint64_t pages_shared,pages_sharing,zero_pages_merged,pages_scanned,scan_cycles; bool running; };
static uint8_t ram[10*PAGE_SIZE];
static uint16_t refs[10];
static uint64_t ptes[6];
static unsigned flushes;
static bool race_cow,race_apply;
static spinlock_t *target_lock;
static unsigned mm_acquires;
static void on_mm_lock(spinlock_t *l) {
 if(l!=target_lock)return;
 if(++mm_acquires==2&&race_apply){
  assert(refs[3]==2&&refs[2]==3);
  refs[3]--;refs[7]=1;ptes[2]=7*PAGE_SIZE|7;
  memset(ram+7*PAGE_SIZE,0x77,PAGE_SIZE);
 }
}
struct thread *global_thread_list;
spinlock_t tid_lock;
static uint64_t pmm_get_zero_page_phys(void) { return 8*PAGE_SIZE; }
static uint64_t vmm_get_vsyscall_page_phys(void) { return 9*PAGE_SIZE; }
static bool pmm_is_managed(uint64_t p) { return p>=PAGE_SIZE && p<sizeof(ram); }
static uint16_t pmm_get_ref(void *p) { return refs[(uintptr_t)p/PAGE_SIZE]; }
static void pmm_incref(void *p) { unsigned n=(uintptr_t)p/PAGE_SIZE; assert(refs[n] && refs[n]!=65535); refs[n]++; }
static void pmm_free_page(void *p) {
 unsigned n=(uintptr_t)p/PAGE_SIZE; assert(refs[n]);
 if (refs[n]==65535) return;
 refs[n]--;
 /* Pages 1 and 3 still have writable remote TLB entries until flush #2. */
 if (n==1 || n==3) assert(flushes>=2 || refs[n]>0);
}
static uint64_t *vmm_get_pte_ptr(uint64_t *cr3,uint64_t va) {
 (void)cr3; assert(va/PAGE_SIZE<6); return &ptes[va/PAGE_SIZE];
}
static void tlb_shootdown_context_for(uint64_t cr3) {
 (void)cr3; assert(!held && hal_irq_enabled());
 if (++flushes==1) {
  /* A final remote write arrives just before revocation is acknowledged. */
  assert(refs[4]==2 && !(ptes[3]&PAGE_FLAG_RW));
  memset(ram+4*PAGE_SIZE,0x42,PAGE_SIZE);
  if (race_cow) {
   /* A fault/unmap during the unlocked flush replaces this PTE. */
   assert(refs[2]==2); refs[2]--;
   refs[7]=1; ptes[1]=7*PAGE_SIZE|7;
   memset(ram+7*PAGE_SIZE,0x77,PAGE_SIZE);
  }
 }
}
static void mm_put(struct mm_struct *mm,uint64_t cr3) { (void)cr3; assert(!held); mm->ref_count--; }
static void sched_yield(void) { assert(!held); }
static void klog_puts(const char *s) { (void)s; }
static void klog_uint64(uint64_t n) { (void)n; }
'''
test = r'''
int main(void) {
 struct vma v[6]={0}; struct mm_struct mm={.ref_count=1};
 struct thread t={.mm=&mm,.cr3=4096}; global_thread_list=&t;
 ksm_node_pool=calloc(KSM_MAX_NODES,sizeof(*ksm_node_pool)); assert(ksm_node_pool);
 for (unsigned run=0;run<3;run++) {
  race_cow=run==1;race_apply=run==2;mm_acquires=0;target_lock=&mm.lock;flushes=0;memset(refs,0,sizeof(refs));memset(ram,0,sizeof(ram));
  for(unsigned i=0;i<6;i++) {
   v[i]=(struct vma){.start=i*PAGE_SIZE,.end=(i+1)*PAGE_SIZE,
    .flags=MAP_PRIVATE|MAP_ANONYMOUS,.prot=PROT_WRITE,.right=i<5?&v[i+1]:NULL};
   ptes[i]=(i+1)*PAGE_SIZE|7;refs[i+1]=1;
  }
  memset(ram+2*PAGE_SIZE,0x11,2*PAGE_SIZE);
  refs[5]=2;v[5].file_node=&t;refs[8]=65535;mm.vmas.root=v;
  irq_enabled=false;assert(ksm_scan_pass()==0 && flushes==0);irq_enabled=true;
  assert(ksm_scan_pass()==(race_apply?1:2));assert(flushes==2);
  assert((ptes[0]&PAGE_MASK)==8*PAGE_SIZE && refs[1]==0);
  assert((ptes[3]&PAGE_MASK)==4*PAGE_SIZE && refs[4]==1);
  assert(ram[4*PAGE_SIZE]==0x42 && refs[5]==2 && refs[6]==1);
  if(race_cow) {
   assert((ptes[1]&PAGE_MASK)==7*PAGE_SIZE && refs[7]==1);
   assert(refs[2]==1 && refs[3]==0);
  } else if(race_apply) {
   assert((ptes[2]&PAGE_MASK)==7*PAGE_SIZE && refs[7]==1);
   assert((ptes[1]&PAGE_MASK)==2*PAGE_SIZE && refs[2]==1 && refs[3]==0);
  } else {
   assert((ptes[1]&PAGE_MASK)==(ptes[2]&PAGE_MASK));
   assert(refs[2]==2 && refs[3]==0);
  }
  assert(mm.ref_count==1 && !held);
 }
 free(ksm_node_pool); puts("KSM lifetime: PASS (stale writes, delayed release, CoW revalidation, exclusions)");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-ksm-') as tmp:
    c = Path(tmp)/'test.c'
    c.write_text(prefix+scan+test)
    exe = Path(tmp)/'test'
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-variable',
                    '-fsanitize=address,undefined','-g',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
    mutant=scan.replace('pmm_free_page((void *)snapshot->replacement);', '')
    assert mutant!=scan
    c.write_text(prefix+mutant+test)
    subprocess.run(['cc','-std=gnu11','-Wno-unused-variable',str(c),'-o',str(exe)],check=True)
    assert subprocess.run([str(exe)],capture_output=True).returncode!=0
    print('PASS: leaking a cancelled replacement reference fails the CoW race regression')
