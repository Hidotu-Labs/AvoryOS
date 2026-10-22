#!/usr/bin/env python3
"""Exercise migrated helpers with wakeups, overflow and concurrent teardown."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def source(name):
    return re.sub(r"^#include.*$", "", (ROOT / "kernel/linuxkpi/src" / name).read_text(), flags=re.M)


def run(name, code):
    with tempfile.TemporaryDirectory(prefix="kpi-migration-") as tmp:
        path = Path(tmp) / name
        path.with_suffix(".c").write_text(code)
        subprocess.run(["cc", "-std=gnu11", "-O1", "-g", "-pthread",
                        "-fsanitize=address,undefined", str(path.with_suffix(".c")),
                        "-o", str(path)], check=True)
        subprocess.run([str(path)], check=True, timeout=15)


common = '#include <assert.h>\n#include <stdio.h>\n'
time = source("time.c")
time = time[time.index("void msleep(unsigned int"):time.index("unsigned long msleep_interruptible")]
run("sleep", common + r'''
static int calls;
static unsigned long msecs_to_jiffies(unsigned int n) { return n; }
static long schedule_timeout_uninterruptible(long n) {
    static const long expected[] = {21, 14, 3};
    assert(calls < 3 && n == expected[calls]);
    return ++calls == 3 ? 0 : expected[calls];
}
''' + time + r'''
int main(void) { msleep(20); assert(calls == 3); puts("PASS: msleep covers unrelated early wakeups"); }
''')

run("format", common + r'''
#include <stdarg.h>
#include <string.h>
#define WARN_ON(x) ((void)(x))
struct seq_buf { char *buffer; size_t size, len; };
static void seq_buf_set_overflow(struct seq_buf *s) { s->len = s->size + 1; }
''' + source("seq_buf_format.c") + r'''
int main(void) {
    char b[8]; memset(b, 0xa5, sizeof(b));
    struct seq_buf s = {b, 6, 0};
    assert(!seq_buf_printf(&s, "%s:%d", "ab", 3) && s.len == 4 && !strcmp(b, "ab:3"));
    assert(!seq_buf_printf(&s, "x") && s.len == 5);
    assert(seq_buf_printf(&s, "y") == -1 && s.len == 7 && (unsigned char)b[6] == 0xa5);
    assert(seq_buf_printf(&s, "z") == -1 && (unsigned char)b[6] == 0xa5);
    s.size = 0; s.len = 0;
    assert(seq_buf_printf(&s, "z") == -1 && s.len == 1);
    puts("PASS: seq_buf formatting, exact capacity, overflow and empty buffers");
}
''')

vmalloc = source("vmalloc.c")
vmalloc = vmalloc[vmalloc.index("/* Upstream mm/util.c: grow-only"):vmalloc.index("/* ── init")]
run("kvrealloc", common + r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define KERNEL_VERSION(a,b,c) (((a)<<16)|((b)<<8)|(c))
#define LINUX_VERSION_CODE KERNEL_VERSION(6,12,0)
#define ZERO_SIZE_PTR ((void *)16)
#define ZERO_OR_NULL_PTR(p) ((uintptr_t)(p)<=16)
typedef unsigned int gfp_t;
struct vmap_area { uint64_t start,end; bool owns_pages,is_ioremap; };
static struct vmap_area area;
static void *oldptr;
static size_t oldsize;
static bool mapped, fail_alloc;
static int frees, locked, vmap_lock;
#define spin_lock_irqsave(l,f) do { (void)(l); (f)=0; assert(!locked++); } while(0)
#define spin_unlock_irqrestore(l,f) do { (void)(l); (void)(f); assert(locked--==1); } while(0)
static bool is_vmalloc_addr(const void *p) { return mapped && p==oldptr; }
static struct vmap_area *vmap_area_find_locked(uint64_t p) { assert(locked); return p==area.start?&area:NULL; }
static size_t ksize(const void *p) { assert(p==oldptr); return oldsize; }
static void kvfree(const void *p) { if(!ZERO_OR_NULL_PTR(p)) { frees++; free((void *)p); } }
static void *kvmalloc(size_t n,gfp_t flags) { return fail_alloc?NULL:flags?calloc(1,n):malloc(n); }
''' + vmalloc + r'''
int main(void) {
    assert(kvrealloc(NULL,0,0)==ZERO_SIZE_PTR);
    for(int mode=0;mode<2;mode++) {
        mapped=mode; oldsize=mode?128:64; oldptr=malloc(oldsize); assert(oldptr);
        memset(oldptr,0x6b,oldsize);
        area=(struct vmap_area){(uint64_t)oldptr,(uint64_t)oldptr+oldsize,true,false};
        int before=frees;
        assert(kvrealloc(oldptr,32,0)==oldptr && frees==before);
        fail_alloc=true; assert(!kvrealloc(oldptr,256,1) && frees==before);
        assert(((unsigned char *)oldptr)[oldsize-1]==0x6b); fail_alloc=false;
        unsigned char *p=kvrealloc(oldptr,256,1); assert(p && frees==before+1 && !locked);
        for(size_t i=0;i<256;i++) assert(p[i]==(i<oldsize?0x6b:0));
        free(p);
    }
    mapped=false; oldptr=malloc(32); oldsize=32;
    int before=frees;
    assert(kvrealloc(oldptr,0,0)==ZERO_SIZE_PTR && frees==before+1);
    void *p=kvrealloc(ZERO_SIZE_PTR,32,1); assert(p); free(p);
    puts("PASS: heap/vmap kvrealloc preserves contents, zeroes growth, retains failed allocations and frees zero size");
}
''')

run("shrinker", common + r'''
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <sched.h>
#include <unistd.h>
#define KERNEL_VERSION(a,b,c) (((a)<<16)|((b)<<8)|(c))
#define LINUX_VERSION_CODE KERNEL_VERSION(6,12,0)
#define KPI_LOG_UNUSED 0
#define GFP_KERNEL 0
#define SHRINKER_REGISTERED 1
#define SHRINKER_ALLOCATED 2
#define SHRINK_EMPTY (~0UL-1)
#define SHRINK_STOP (~0UL)
typedef unsigned int gfp_t;
struct list_head { int dummy; };
struct shrink_control { gfp_t gfp_mask; int nid; unsigned long nr_to_scan, nr_scanned; };
struct shrinker {
    unsigned long (*count_objects)(struct shrinker *, struct shrink_control *);
    unsigned long (*scan_objects)(struct shrinker *, struct shrink_control *);
    struct list_head list;
    unsigned flags;
    int refcount, done;
};
#define DEFINE_SPINLOCK(n) pthread_mutex_t n = PTHREAD_MUTEX_INITIALIZER
#define DECLARE_WAIT_QUEUE_HEAD(n) int n
#define spin_lock_irqsave(l,f) do { (f)=0; pthread_mutex_lock(l); } while(0)
#define spin_unlock_irqrestore(l,f) do { (void)(f); pthread_mutex_unlock(l); } while(0)
#define READ_ONCE(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define wait_event(q,c) do { (void)(q); while(!(c)) sched_yield(); } while(0)
#define wake_up_all(q) ((void)(q))
#define INIT_LIST_HEAD(l) ((void)(l))
#define refcount_set(p,n) (*(p)=(n))
#define init_completion(p) (*(p)=0)
#define kzalloc(n,f) calloc(1,n)
#define kfree(p) free(p)
#define klogf(...) ((void)0)
''' + source("shrinker.c") + r'''
static atomic_int entered, release_scan, delete_started, deleted;
static unsigned long count(struct shrinker *s, struct shrink_control *c) { (void)s; (void)c; return 4; }
static unsigned long scan(struct shrinker *s, struct shrink_control *c) {
    (void)s; atomic_store(&entered,1);
    while (!atomic_load(&release_scan)) sched_yield();
    return c->nr_to_scan;
}
static void *reclaim(void *p) { (void)p; assert(linuxkpi_shrink_reclaim(0,1)==1); return NULL; }
static void *remove_shrinker(void *p) {
    atomic_store(&delete_started,1); shrinker_free(p); atomic_store(&deleted,1); return NULL;
}
int main(void) {
    struct shrinker *s=shrinker_alloc(0, "test"); assert(s);
    s->count_objects=count; s->scan_objects=scan; shrinker_register(s);
    struct shrinker skipped={0}; assert(!register_shrinker(&skipped,"skipped"));
    pthread_t r,d; assert(!pthread_create(&r,NULL,reclaim,NULL));
    while(!atomic_load(&entered)) sched_yield();
    assert(!pthread_create(&d,NULL,remove_shrinker,s));
    while(!atomic_load(&delete_started)) sched_yield();
    usleep(20000); assert(!atomic_load(&deleted));
    atomic_store(&release_scan,1); pthread_join(r,NULL); pthread_join(d,NULL);
    assert(atomic_load(&deleted)); unregister_shrinker(&skipped); synchronize_shrinkers();
    struct shrinker slots[KPI_SHRINKER_MAX+1]={0};
    for(int i=0;i<KPI_SHRINKER_MAX;i++) assert(!register_shrinker(&slots[i],"full"));
    assert(register_shrinker(&slots[KPI_SHRINKER_MAX],"overflow")==-ENOSPC);
    for(int i=0;i<KPI_SHRINKER_MAX;i++) unregister_shrinker(&slots[i]);
    assert(!linuxkpi_shrink_reclaim(0,1));
    puts("PASS: shrinker free waits for callback, early-exit pins drain, registry exhaustion fails");
}
''')
