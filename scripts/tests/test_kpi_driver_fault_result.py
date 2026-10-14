#!/usr/bin/env python3
"""A rejected device fault must never install generic/anonymous backing."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/src/mm/vmm_fault.c').read_text()
a=s.index('  if (vma_linux) {',s.index('/* Imported Linux drivers can handle'))
b=s.index('\n  if (!vma)',a)
block=s[a:b]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
struct vma {void *linux_vma;};struct mm_struct {int lock;int vmas;};
struct thread {struct mm_struct *mm;};
static struct mm_struct mm;static struct thread task={&mm};static struct thread *current=&task;
static struct vma live;static int result,puts,unmaps,fallback,locked;
#define PF_REJECT(reason,a,b) ((void)(reason),(void)(a),(void)(b),-1)
static bool pf_enable_irqs_for_wait(void){return true;}
static void pf_restore_irqs_after_wait(bool b){(void)b;}
int linuxkpi_vma_fault(void*p,unsigned long a,unsigned long e){(void)p;(void)a;(void)e;return result;}
static void vma_linux_put(void*p){assert(p==(void*)1);puts++;}
static void spinlock_acquire(int*p){(void)p;assert(!locked++);}
static void spinlock_release(int*p){(void)p;assert(locked--==1);}
static struct vma *vma_find(int*p,uint64_t a){(void)p;(void)a;assert(locked);return &live;}
static void vmm_unmap_page(uint64_t*p,uint64_t a){(void)p;(void)a;unmaps++;}
static int fault(void){void *vma_linux=(void*)1;uint64_t cr2=0x1234,target_cr3=0x8000,error_code=2;
'''+block+r'''
 fallback++;return 0;}
int main(void){
 live.linux_vma=(void*)1;result=-1;assert(fault()==-1&&!fallback&&puts==1);
 result=0;assert(!fault()&&!fallback&&puts==2&&!unmaps);
 live.linux_vma=(void*)2;assert(fault()==-1&&!fallback&&puts==3&&unmaps==1);
 puts("PASS: rejected driver faults cannot fabricate CPU backing; success and concurrent unmap release the bridge exactly once");
}
'''
# Avoid collision with libc puts.
code=code.replace('puts,unmaps','releases,unmaps').replace('puts++','releases++').replace('puts==','releases==')
with tempfile.TemporaryDirectory(prefix='kpi-driver-fault-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=gnu11','-O2',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=10)
 old=code.replace('    return PF_REJECT("driver rejected the fault in its own mapping", cr2,\n                     (uint64_t)(int64_t)driver_fault_result);','')
 (p/'old.c').write_text(old)
 subprocess.run(['cc','-std=gnu11','-O2',str(p/'old.c'),'-o',str(p/'old')],check=True)
 r=subprocess.run([str(p/'old')],capture_output=True,timeout=10)
 assert r.returncode!=0,'former generic fallback unexpectedly passed'
 print('PASS: former device-fault fallback fails the CPU/GPU backing regression')
