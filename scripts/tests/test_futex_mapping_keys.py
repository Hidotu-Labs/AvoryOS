#!/usr/bin/env python3
"""Actual futex keys must survive KSM/CoW on private memory without PRIVATE."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'kernel/src/syscalls/sys_futex.c').read_text()
def function(marker):
 a=s.index(marker);b=s.index('{',a)+1;depth=1
 while depth:
  depth+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]
prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define EFAULT 14
#define EINVAL 22
#define MAP_PRIVATE 2
#define MAP_SHARED 1
#define MAP_SYSV_SHM 4
#define FUTEX_HASH_SIZE 256
struct futex_key {uint64_t space,address;};
struct vma {uint64_t start,end;unsigned flags;};
struct vma_list {struct vma v;};
struct mm_struct {int lock;struct vma_list vmas;};
struct thread {struct mm_struct *mm;uint64_t cr3;};
static struct thread *self;
static unsigned walks,held;
static uint64_t frame=0x10000;
static struct thread *sched_get_current(void){return self;}
static void spinlock_acquire(int *l){assert(!*l);*l=1;held++;}
static void spinlock_release(int *l){assert(*l);*l=0;held--;}
static bool vmm_is_user_addr_range_valid(uint64_t p,unsigned n){return p>=4096 && p+n<0x800000000000ULL;}
static struct vma *vma_find(struct vma_list *list,uint64_t addr){
 assert(held);return addr>=list->v.start && addr<list->v.end?&list->v:NULL;
}
static uint64_t vmm_virt_to_phys(uint64_t *cr3,uint64_t addr){
 assert(held && (uint64_t)cr3==self->cr3);walks++;return frame?frame+(addr&4095):0;
}
'''
test=r'''
static struct futex_key key(uint64_t va,bool private){
 struct futex_key k;assert(!futex_get_key((uint32_t *)va,private,&k));assert(!held);return k;
}
int main(void){
 struct mm_struct a={.vmas.v={0x4000,0x8000,MAP_PRIVATE}};
 struct mm_struct b={.vmas.v={0x4000,0x9000,MAP_PRIVATE}};
 struct thread ta={&a,0x1000},tb={&b,0x2000};self=&ta;
 struct futex_key parked=key(0x4004,false);
 frame=0x90000; /* KSM or CoW replaces the page while a waiter is parked. */
 struct futex_key wake=key(0x4004,false);
 assert(futex_key_equal(parked,wake));assert(futex_hash_key(parked)==futex_hash_key(wake));
 assert(!walks && futex_key_equal(parked,key(0x4004,true)));
 self=&tb;assert(!futex_key_equal(parked,key(0x4004,false)));
 /* Even two private VMAs backed by the same zero/KSM frame stay distinct. */
 a.vmas.v.flags=b.vmas.v.flags=MAP_SHARED;
 self=&ta;parked=key(0x4004,false);self=&tb;wake=key(0x8004,false);
 assert(futex_key_equal(parked,wake) && !parked.space && walks==2);
 b.vmas.v.flags=MAP_SYSV_SHM;assert(futex_key_equal(parked,key(0x8004,false)));
 struct futex_key out;
 assert(futex_get_key((uint32_t *)0x9004,false,&out)==-EFAULT && !held);
 assert(futex_get_key((uint32_t *)0x4001,false,&out)==-EINVAL && !held);
 assert(futex_get_key((uint32_t *)4,false,&out)==-EFAULT && !held);
 frame=0;assert(futex_get_key((uint32_t *)0x4004,false,&out)==-EFAULT && !held);
 self=NULL;assert(futex_get_key((uint32_t *)0x4004,false,&out)==-EFAULT);
 puts("PASS: private mapping wait/wake survives frame replacement; mm isolation and shared aliases preserved");
}
'''
with tempfile.TemporaryDirectory(prefix='futex-mapping-') as tmp:
 c=Path(tmp)/'test.c';exe=Path(tmp)/'test'
 body=prefix+function('static inline uint32_t futex_hash_key(')+function('static inline bool futex_key_equal(')+function('static inline int futex_get_key(')+test
 c.write_text(body)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)

 # Recreate the former physical-key path for a deterministic negative control.
 control=body.replace('if ((v->flags & MAP_PRIVATE) &&', 'if (false && (v->flags & MAP_PRIVATE) &&')
 control=control.replace('assert(futex_key_equal(parked,wake));',
   'if (!futex_key_equal(parked,wake)) {puts("REPRODUCED: page replacement changes the parked futex key");return 42;}',1)
 c.write_text(control)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
 result=subprocess.run([str(exe)],capture_output=True,text=True)
 assert result.returncode==42 and 'REPRODUCED:' in result.stdout,result.stdout+result.stderr
 print('PASS: former physical-key behavior reproduces the unmatched wake')
