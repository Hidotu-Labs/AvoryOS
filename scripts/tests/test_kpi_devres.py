#!/usr/bin/env python3
"""Run the actual devres implementation with sanitizer and concurrency checks."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = Path(os.environ.get('KPI_DEVICE_SOURCE', ROOT / 'kernel/linuxkpi/src/device.c')).read_text()
start = source.index('struct devres_node {')
end = source.index('/* ── devm_request_irq', start)
implementation = source[start:end]
shim = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define GFP_KERNEL 0
#define ARCH_KMALLOC_MINALIGN 8
#define __aligned(x) __attribute__((aligned(x)))
#define BUG_ON(x) assert(!(x))
static atomic_int warnings, allocations;
#define WARN_ON(x) ((x) ? atomic_fetch_add(&warnings, 1) + 1 : 0)
typedef int gfp_t;
struct list_head {struct list_head *next, *prev;};
struct device {struct list_head devres_head;pthread_mutex_t devres_lock;
 void *kpi_group_node_mark,*kpi_group_alloc_mark;}; /* permits pre-fix control */
typedef void (*dr_release_t)(struct device *,void *);
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define list_entry(p,t,m) container_of(p,t,m)
#define LIST_HEAD(n) struct list_head n={&n,&n}
static void INIT_LIST_HEAD(struct list_head*h){h->next=h->prev=h;}
static int list_empty(const struct list_head*h){return h->next==h;}
static void list_add_tail(struct list_head*n,struct list_head*h){n->prev=h->prev;n->next=h;h->prev->next=n;h->prev=n;}
static void list_del(struct list_head*n){n->prev->next=n->next;n->next->prev=n->prev;}
static void list_del_init(struct list_head*n){list_del(n);INIT_LIST_HEAD(n);}
static void list_move_tail(struct list_head*n,struct list_head*h){list_del(n);list_add_tail(n,h);}
static void list_splice_init(struct list_head*s,struct list_head*d){
 if(list_empty(s))return;struct list_head*f=s->next,*l=s->prev,*a=d->next;
 d->next=f;f->prev=d;l->next=a;a->prev=l;INIT_LIST_HEAD(s);
}
#define list_last_entry(h,t,m) list_entry((h)->prev,t,m)
#define list_for_each_entry(p,h,m) for(p=list_entry((h)->next,__typeof__(*p),m); &(p)->m!=(h);p=list_entry((p)->m.next,__typeof__(*p),m))
#define list_for_each_entry_reverse(p,h,m) for(p=list_entry((h)->prev,__typeof__(*p),m); &(p)->m!=(h);p=list_entry((p)->m.prev,__typeof__(*p),m))
#define list_for_each_entry_safe(p,t,h,m) for(p=list_entry((h)->next,__typeof__(*p),m),t=list_entry((p)->m.next,__typeof__(*p),m);&(p)->m!=(h);p=t,t=list_entry((t)->m.next,__typeof__(*t),m))
static _Thread_local int irq_disabled;
#define spin_lock_init(l) assert(!pthread_mutex_init(l,NULL))
#define spin_lock_irqsave(l,f) do{f=irq_disabled;irq_disabled=1;assert(!pthread_mutex_lock(l));}while(0)
#define spin_unlock_irqrestore(l,f) do{assert(!pthread_mutex_unlock(l));irq_disabled=f;}while(0)
static int fail_alloc;
static void *kmalloc(size_t n,gfp_t g){(void)g;if(fail_alloc)return NULL;void*p=malloc(n);if(p){atomic_fetch_add(&allocations,1);memset(p,0xa5,n);}return p;}
static void *kzalloc(size_t n,gfp_t g){void*p=kmalloc(n,g);if(p)memset(p,0,n);return p;}
static void kfree(void*p){if(p){atomic_fetch_sub(&allocations,1);free(p);}}
static void init_device(struct device*d){memset(d,0,sizeof(*d));INIT_LIST_HEAD(&d->devres_head);spin_lock_init(&d->devres_lock);}
'''
main = r'''
static int order[32], count;
static struct device *record_dev;
static void record(void*p){
 assert(!irq_disabled);assert(!pthread_mutex_trylock(&record_dev->devres_lock));
 assert(!pthread_mutex_unlock(&record_dev->devres_lock));order[count++]=(int)(long)p;
}
static void raw_record(struct device*d,void*p){assert(d==record_dev);record((void*)(long)*(int*)p);}
static void add_raw(struct device*d,int value){int*p=devres_alloc(raw_record,sizeof(*p),0);assert(p&&!*p);*p=value;devres_add(d,p);}
static void add_action(struct device*d,int value){assert(!devm_add_action(d,record,(void*)(long)value));}
static void expect(const int*values,int n){assert(count==n);for(int i=0;i<n;i++)assert(order[i]==values[i]);count=0;}
static void register_later(void*p){struct device*d=p;assert(!devm_add_action(d,record,(void*)7));}
static atomic_int released, producers_done;
static struct device concurrent;
static void concurrent_release(void*p){assert((long)p==123);assert(!irq_disabled);atomic_fetch_add(&released,1);}
static void concurrent_raw(struct device*d,void*p){assert(d==&concurrent&&*(int*)p==123);concurrent_release((void*)123);}
static void *producer(void*p){(void)p;for(int i=0;i<500;i++){
 int*r=devres_alloc(concurrent_raw,sizeof(*r),0);assert(r);*r=123;devres_add(&concurrent,r);
 assert(!devm_add_action(&concurrent,concurrent_release,(void*)123));
 }atomic_fetch_add(&producers_done,1);return NULL;}
static void *releaser(void*p){(void)p;while(atomic_load(&producers_done)!=8)devres_release_all(&concurrent);return NULL;}
int main(void){
 struct device d,other;init_device(&d);init_device(&other);record_dev=&d;
 add_raw(&d,1);add_action(&d,2);add_raw(&d,3);add_action(&d,4);devres_release_all(&d);
 expect((int[]){4,3,2,1},4);
 add_action(&d,1);add_raw(&d,2);add_action(&d,1);
 devm_remove_action(&d,record,(void*)1);devres_release_all(&d);expect((int[]){2,1},2);
 add_action(&d,5);add_action(&d,6);add_action(&d,5);
 devm_release_action(&d,record,(void*)5);expect((int[]){5},1);
 devm_release_action(&d,record,(void*)99);expect(NULL,0);
 devres_release_all(&d);expect((int[]){6,5},2);
 add_action(&d,0);void*outer=devres_open_group(&d,NULL,0);assert(outer);add_raw(&d,1);
 void*inner=devres_open_group(&d,NULL,0);assert(inner&&inner!=outer);add_action(&d,2);
 devres_close_group(&d,NULL);add_raw(&d,3);devres_close_group(&d,outer);add_action(&d,4);
 add_action(&other,9);devres_release_group(&d,outer);expect((int[]){3,2,1},3);
 devres_release_all(&d);expect((int[]){4,0},2);record_dev=&other;
 devres_release_all(&other);expect((int[]){9},1);record_dev=&d;
 /* Crossing groups: retain their boundary markers if only one is selected. */
 void*a=devres_open_group(&d,(void*)0x100,0);add_raw(&d,1);
 void*b=devres_open_group(&d,(void*)0x200,0);add_raw(&d,2);
 devres_close_group(&d,a);add_action(&d,3);devres_close_group(&d,b);
 devres_release_group(&d,a);expect((int[]){2,1},2);
 devres_release_group(&d,b);expect((int[]){3},1);
 /* Latest open group lookup and an outer group left open. */
 outer=devres_open_group(&d,NULL,0);add_raw(&d,1);
 inner=devres_open_group(&d,NULL,0);add_action(&d,2);
 devres_release_group(&d,NULL);expect((int[]){2},1);
 devres_release_group(&d,outer);expect((int[]){1},1);
 assert(!devm_add_action(&d,register_later,&d));devres_release_all(&d);
 assert(!count&&!list_empty(&d.devres_head));devres_release_all(&d);expect((int[]){7},1);
 void*unused=devres_alloc(raw_record,32,0);assert(unused);devres_free(unused);
 assert(!devres_alloc(raw_record,(size_t)-1,0));
 fail_alloc=1;assert(!devres_alloc(raw_record,32,0));
 assert(!devres_open_group(&d,NULL,0));
 assert(devm_add_action_or_reset(&d,record,(void*)8)==-ENOMEM);expect((int[]){8},1);fail_alloc=0;
 assert(!atomic_load(&allocations)&&!atomic_load(&warnings));
 init_device(&concurrent);pthread_t threads[8],drain;
 assert(!pthread_create(&drain,NULL,releaser,NULL));
 for(int i=0;i<8;i++)assert(!pthread_create(&threads[i],NULL,producer,NULL));
 for(int i=0;i<8;i++)assert(!pthread_join(threads[i],NULL));assert(!pthread_join(drain,NULL));
 devres_release_all(&concurrent);
 assert(atomic_load(&released)==8000&&!atomic_load(&allocations)&&!atomic_load(&warnings));
 puts("PASS: actual devres mixed LIFO, latest action removal, nested/closed/crossing groups, isolation, reentrant registration, allocation failure and 8000 concurrent releases (ASan/UBSan)");
}
'''
with tempfile.TemporaryDirectory(prefix='kpi-devres-') as temporary:
    directory = Path(temporary)
    cfile = directory / 'devres.c'
    cfile.write_text(shim + implementation + main)
    executable = directory / 'devres'
    subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-pthread',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(cfile), '-o', str(executable)], check=True)
    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    print(result.stdout.strip())
