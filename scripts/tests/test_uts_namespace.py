#!/usr/bin/env python3
"""Actual UTS implementation: ownership, teardown, OOM and concurrent reads."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/src/sched/namespace.c').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
typedef struct { unsigned locked; } spinlock_t;
static void spinlock_init(spinlock_t *s) {s->locked=0;}
static void spinlock_acquire(spinlock_t *s) {
 while(__atomic_exchange_n(&s->locked,1,__ATOMIC_ACQUIRE)) {}
}
static void spinlock_release(spinlock_t *s) {__atomic_store_n(&s->locked,0,__ATOMIC_RELEASE);}
typedef int hal_irq_state_t;
static int hal_irq_save(void) {return 0;}
static void hal_irq_restore(int f) {(void)f;}
struct uts_namespace;
struct thread {spinlock_t namespace_lock;struct uts_namespace *uts_ns;unsigned euid;};
static bool capability_has(struct thread *t,unsigned cap) {(void)cap;return t && t->euid==0;}
#define CAP_SYS_ADMIN 21
static int live,fail_alloc;
static void *kmalloc(size_t n) {
 if(fail_alloc)return NULL;
 void *p=malloc(n);if(p)__atomic_add_fetch(&live,1,__ATOMIC_RELAXED);return p;
}
static void kfree(void *p) {if(p){__atomic_sub_fetch(&live,1,__ATOMIC_RELAXED);free(p);}}
''' + source + r'''
static struct thread parent,child,sibling;
static void *replace(void *unused) {
 (void)unused;
 for(unsigned i=0;i<10000;i++) {
  struct uts_namespace *n=uts_namespace_copy(&parent);assert(n);
  uts_namespace_install(&child,n);
  assert(!uts_namespace_set(&child,"child",5,0));
 }
 return NULL;
}
static void *read_names(void *unused) {
 (void)unused;
 for(unsigned i=0;i<10000;i++) {
  char a[65],b[65];uts_namespace_read(&child,a,b);
  assert(!strcmp(a,"parent") || !strcmp(a,"child"));
  assert(!strcmp(b,"localhost"));assert(uts_namespace_id(&child));
 }
 return NULL;
}
int main(void) {
 assert(uts_namespace_id(&parent)==0x4E530008);
 assert(!uts_namespace_set(&parent,"parent",6,0));
 uts_namespace_inherit(&child,&parent);
 assert(uts_namespace_id(&child)==uts_namespace_id(&parent));
 struct uts_namespace *n=uts_namespace_copy(&parent);assert(n && live==1);
 uts_namespace_install(&child,n);
 assert(uts_namespace_id(&child)!=uts_namespace_id(&parent));
 uts_namespace_inherit(&sibling,&child);
 assert(!uts_namespace_set(&sibling,"shared",6,0));
 char a[65],b[65];uts_namespace_read(&child,a,b);assert(!strcmp(a,"shared"));
 uts_namespace_read(&parent,a,b);assert(!strcmp(a,"parent"));
 uts_namespace_release(&child);assert(live==1);
 uts_namespace_read(&sibling,a,b);assert(!strcmp(a,"shared"));
 uts_namespace_release(&sibling);assert(live==0);
 fail_alloc=1;assert(!uts_namespace_copy(&parent));fail_alloc=0;
 child.euid=1000;assert(uts_namespace_set(&child,"bad",3,0)==-1);child.euid=0;
 assert(uts_namespace_set(&child,"bad",65,0)==-22);
 uts_namespace_inherit(&child,&parent);
 pthread_t writer,reader;assert(!pthread_create(&writer,NULL,replace,NULL));
 assert(!pthread_create(&reader,NULL,read_names,NULL));
 pthread_join(writer,NULL);pthread_join(reader,NULL);
 uts_namespace_release(&child);assert(live==0);
 puts("PASS: UTS namespace isolation, sharing, teardown, OOM and concurrent replacement");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-uts-') as temp:
    src = Path(temp) / 'test.c'
    exe = Path(temp) / 'test'
    src.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', '-g', src, '-o', exe], check=True)
    subprocess.run([exe], check=True)
