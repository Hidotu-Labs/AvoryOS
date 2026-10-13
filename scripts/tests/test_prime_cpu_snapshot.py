#!/usr/bin/env python3
"""Check native PRIME snapshot exclusion against a concurrent GPU submitter."""
from pathlib import Path
import subprocess
import tempfile
import sys
ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'kernel/linuxkpi/src/native_prime.c').read_text()
a=source.index('int linuxkpi_prime_begin_cpu(')
b=source.index('\nvoid *linuxkpi_prime_reservation_new',a)
functions=Path(sys.argv[1]).read_text() if len(sys.argv)>1 else source[a:b]
code=r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define DMA_FROM_DEVICE 2
#define DMA_RESV_USAGE_WRITE 2
#define MAX_SCHEDULE_TIMEOUT 0x7fffffff
struct mutex {pthread_mutex_t lock;bool held;};
struct dma_resv {pthread_mutex_t lock;bool held;};
struct dma_buf {struct dma_resv *resv;size_t size;};
struct native_prime_mapping {struct mutex cpu_lock;struct {void *vaddr;}map;};
enum {GPU_DIAG_FENCE=10,GPU_DIAG_CPU_BEGIN=8,GPU_DIAG_CPU_END=9};
static int gpu_diag_level(void){return 0;}
static unsigned long long linuxkpi_monotonic_ms(void){return 0;}
static void gpu_diag_record(unsigned int k,const void *o,unsigned long long a,unsigned long long b,unsigned long long c,unsigned long long d,long r){(void)k;(void)o;(void)a;(void)b;(void)c;(void)d;(void)r;}
struct dma_resv_iter {int unused;};
struct dma_fence {unsigned long long context,seqno;unsigned long flags;};
#define DMA_RESV_USAGE_BOOKKEEP 4
#define READ_ONCE(x) (x)
#define dma_resv_for_each_fence(cur,resv,usage,fence) for((void)(cur),(void)(resv),(void)(usage),(fence)=NULL;(fence)!=NULL;(fence)=NULL)
static int dma_resv_iter_usage(struct dma_resv_iter *i){(void)i;return 0;}
static int begin_error,lock_error;
static long wait_result=1;
static int begins,ends,waits;
static bool late_fence,pending_writer,blocked;
static unsigned char pixels[1024],snapshot[1024];
static void mutex_lock(struct mutex *m){assert(!m->held);pthread_mutex_lock(&m->lock);m->held=true;}
static void mutex_unlock(struct mutex *m){assert(m->held);m->held=false;pthread_mutex_unlock(&m->lock);}
static int dma_buf_begin_cpu_access(struct dma_buf *b,int dir){
 assert(!b->resv->held && dir==DMA_FROM_DEVICE);begins++;return begin_error;
}
static int dma_buf_end_cpu_access(struct dma_buf *b,int dir){
 assert(!b->resv->held && dir==DMA_FROM_DEVICE);ends++;return 0;
}
static int dma_resv_lock_interruptible(struct dma_resv *r,void *ctx){
 assert(!ctx && !r->held);if(lock_error)return lock_error;
 /* A GPU submission slipped in after begin_cpu_access's fence snapshot. */
 pending_writer=late_fence;
 pthread_mutex_lock(&r->lock);r->held=true;return 0;
}
static void dma_resv_unlock(struct dma_resv *r){assert(r->held);r->held=false;pthread_mutex_unlock(&r->lock);}
static long dma_resv_wait_timeout(struct dma_resv *r,int usage,bool intr,long timeout){
 assert(r->held && usage==DMA_RESV_USAGE_WRITE && intr && timeout==MAX_SCHEDULE_TIMEOUT);
 waits++;if(wait_result>0 && pending_writer){memset(pixels,0x22,sizeof pixels);pending_writer=false;}
 return wait_result;
}
'''+functions+r'''
static void *submit_gpu_write(void *arg){
 struct dma_resv *r=arg;int result=pthread_mutex_trylock(&r->lock);
 blocked=result==EBUSY;
 if(!result){memset(pixels,0x33,sizeof pixels);pthread_mutex_unlock(&r->lock);}
 return NULL;
}
int main(void){
 struct dma_resv r={.lock=PTHREAD_MUTEX_INITIALIZER};struct dma_buf b={.resv=&r};
 struct native_prime_mapping m={.cpu_lock={.lock=PTHREAD_MUTEX_INITIALIZER}};
 for(int i=0;i<100;i++){
  memset(pixels,0x11,sizeof pixels);late_fence=false;blocked=false;
  assert(linuxkpi_prime_begin_cpu(&b,&m)==0);
  memcpy(snapshot,pixels,sizeof pixels/2);
  pthread_t gpu;assert(!pthread_create(&gpu,NULL,submit_gpu_write,&r));pthread_join(gpu,NULL);
  memcpy(snapshot+sizeof pixels/2,pixels+sizeof pixels/2,sizeof pixels/2);
  assert(blocked && memcmp(snapshot,pixels,sizeof pixels)==0);
  linuxkpi_prime_end_cpu(&b,&m);assert(!r.held && !m.cpu_lock.held);
  /* New submissions must proceed once the copy finishes. */
  submit_gpu_write(&r);assert(!blocked && pixels[0]==0x33);
 }
 late_fence=true;assert(!linuxkpi_prime_begin_cpu(&b,&m));
 assert(waits==101 && !pending_writer && pixels[0]==0x22);linuxkpi_prime_end_cpu(&b,&m);
 int n=ends;begin_error=-EIO;assert(linuxkpi_prime_begin_cpu(&b,&m)==-EIO);
 assert(ends==n && !m.cpu_lock.held && !r.held);begin_error=0;
 lock_error=-EINTR;assert(linuxkpi_prime_begin_cpu(&b,&m)==-EINTR);
 assert(ends==++n && !m.cpu_lock.held && !r.held);lock_error=0;
 wait_result=-EINTR;assert(linuxkpi_prime_begin_cpu(&b,&m)==-EINTR);
 assert(ends==++n && !m.cpu_lock.held && !r.held);
 wait_result=0;assert(linuxkpi_prime_begin_cpu(&b,&m)==-ETIMEDOUT);
 assert(ends==++n && !m.cpu_lock.held && !r.held);
 puts("PASS: 100 concurrent PRIME snapshots, late write fence, resumed submissions and begin/lock/wait error cleanup");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-prime-snapshot-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Wno-unused-function','-Werror','-pthread',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
