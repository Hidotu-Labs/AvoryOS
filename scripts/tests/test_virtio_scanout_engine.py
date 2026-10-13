#!/usr/bin/env python3
"""Compile the actual native scanout engine; model independent host images."""
from pathlib import Path
import argparse,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
NATIVE=(ROOT/'kernel/src/drivers/gpu/virtio_gpu/virtio_gpu.c').read_text()
GATE=NATIVE[NATIVE.index('static void gpu_scanout_acquire('):NATIVE.index('static wait_queue_t gpu_worker_wait;')]
C=r'''
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include "drivers/gpu/virtio_gpu/virtio_gpu_scanout.h"
#include <linuxkpi/gpu_diag.h>
void gpu_diag_record(unsigned int k,const void *o,uint64_t a,uint64_t b,uint64_t c,uint64_t d,long r){(void)k;(void)o;(void)a;(void)b;(void)c;(void)d;(void)r;}
int gpu_diag_take_sample(void){return 0;}
uint64_t gpu_diag_hash_rect(const void *p,size_t r,unsigned int h,size_t s){(void)p;(void)r;(void)h;(void)s;return 0;}
static unsigned live_dma,allocs,frees,alloc_fail,live_resources,next_id,front[2],transfers;
static unsigned fail_op,fail_at,op_calls,delayed,delay_id,delay_head,delay_w,delay_h;
static uint64_t delay_physical;
static bool outstanding;
static _Thread_local unsigned role;
static _Thread_local bool cpu_reserved;
static struct host_image {bool alive,attached;uint32_t w,h;uintptr_t backing;unsigned char *pixels;}host[8192];
static pthread_mutex_t orchestration=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static bool a_at_ack,b_waiting,b_finished,preempt;
static uint32_t gpu_scanout_busy;
static void native_yield(void){
 if(role==2){pthread_mutex_lock(&orchestration);b_waiting=true;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&orchestration);}
 usleep(30);
}
#define sched_yield native_yield
@GATE@
void *kcalloc(size_t n,size_t bytes){return calloc(n,bytes);}
void kfree(void *p){free(p);}
dma_buffer_t *dma_alloc(size_t n,uint32_t flags){
 assert(!flags);if(++allocs==alloc_fail)return NULL;
 dma_buffer_t *d=calloc(1,sizeof *d);assert(d);d->virt=malloc(n);assert(d->virt);memset(d->virt,0xcc,n);d->phys=(uintptr_t)d->virt;d->size=n;live_dma++;return d;
}
void dma_free(dma_buffer_t *d){
 for(unsigned i=1;i<=next_id;i++)assert(!host[i].alive || host[i].backing!=d->phys);
 assert(live_dma);live_dma--;frees++;memset(d->virt,0xee,d->size);free(d->virt);free(d);
}
enum {CREATE=1,ATTACH,TRANSFER,SELECT,FLUSH,DESTROY};
static bool ready(void){return !outstanding;}
static bool inject(unsigned op,unsigned id){
 assert(!outstanding && !cpu_reserved);
 if(op!=fail_op || ++op_calls!=fail_at)return false;
 outstanding=delayed!=0;
 if(delayed){assert(delayed==op);delay_id=id;}
 return true;
}
static uint32_t resource_id(void){assert(next_id<8191);return ++next_id;}
static void create_now(unsigned id,unsigned w,unsigned h){assert(!host[id].alive);host[id]=(struct host_image){.alive=true,.w=w,.h=h,.pixels=calloc(w*h,4)};assert(host[id].pixels);live_resources++;}
static bool create(unsigned id,unsigned w,unsigned h){if(inject(CREATE,id)){delay_w=w;delay_h=h;return false;}create_now(id,w,h);return true;}
static void attach_now(unsigned id,uint64_t phys){assert(host[id].alive);for(unsigned i=1;i<=next_id;i++)assert(i==id || !host[i].alive || host[i].backing!=phys);host[id].backing=phys;host[id].attached=true;}
static bool attach(unsigned id,uint64_t phys,uint32_t n){assert(n==host[id].w*host[id].h*4);if(inject(ATTACH,id)){delay_physical=phys;return false;}attach_now(id,phys);return true;}
static void destroy_now(unsigned id){for(unsigned h=0;h<2;h++)assert(front[h]!=id);if(host[id].alive){free(host[id].pixels);memset(&host[id],0,sizeof host[id]);assert(live_resources);live_resources--;}}
static bool destroy(unsigned id,bool counted){(void)counted;if(inject(DESTROY,id))return false;destroy_now(id);return true;}
static bool transfer(unsigned id,unsigned w,unsigned h){
 assert(host[id].alive && host[id].attached && host[id].w==w && host[id].h==h);transfers++;
 for(unsigned scan=0;scan<2;scan++)assert(id!=front[scan]);
 bool failed=inject(TRANSFER,id);size_t n=w*h*4;
 memcpy(host[id].pixels,(void*)host[id].backing,failed?n/2:n);return !failed;
}
static void await(bool *condition){struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_sec+=4;while(!*condition)assert(!pthread_cond_timedwait(&changed,&orchestration,&until));}
static bool select_image(unsigned scan,unsigned id,unsigned w,unsigned h){
 assert(scan<2);if(id)assert(host[id].alive && host[id].w==w && host[id].h==h);
 if(inject(SELECT,id)){delay_head=scan;return false;}front[scan]=id;
 if(role==1 && preempt){
  pthread_mutex_lock(&orchestration);a_at_ack=true;pthread_cond_broadcast(&changed);
#ifdef NO_GATE
  await(&b_finished);
#else
  await(&b_waiting);
#endif
  pthread_mutex_unlock(&orchestration);
 }
 return true;
}
static bool flush(unsigned id,unsigned w,unsigned h){assert(host[id].alive && host[id].w==w && host[id].h==h);return !inject(FLUSH,id);}
static const struct vgpu_scanout_ops ops={ready,resource_id,create,attach,destroy,transfer,select_image,flush};
static void complete_late(void){
 assert(outstanding);outstanding=false;
 if(delayed==CREATE)create_now(delay_id,delay_w,delay_h);
 else if(delayed==ATTACH)attach_now(delay_id,delay_physical);
 else if(delayed==TRANSFER)memcpy(host[delay_id].pixels,(void*)host[delay_id].backing,host[delay_id].w*host[delay_id].h*4);
 else if(delayed==SELECT)front[delay_head]=delay_id;
 else if(delayed==DESTROY)destroy_now(delay_id);
 delayed=fail_op=0;op_calls=0;
}
static void no_failure(void){fail_op=fail_at=op_calls=delayed=alloc_fail=0;assert(!outstanding);}
static void draw(struct vgpu_scanout_head *head,unsigned scan,unsigned tag,unsigned width){
 unsigned char source[12+6*40];memset(source,0xad,sizeof source);
 struct vgpu_scanout_source s={source,sizeof source,12,8,6,40,2,1,width,4};
 unsigned char expected[64];
 for(unsigned y=0;y<4;y++)for(unsigned x=0;x<width;x++){
  uint32_t value=0xff000000|(tag<<8)|(y*width+x);
  memcpy(source+12+(y+1)*40+(x+2)*4,&value,4);memcpy(expected+(y*width+x)*4,&value,4);
 }
 struct vgpu_scanout_image *image=vgpu_scanout_prepare(head,&ops,scan,width,4);assert(image && image->storage->phys!=front[scan]);
 cpu_reserved=true;assert(vgpu_scanout_copy(image,&s));cpu_reserved=false;
 /* Producer disappears/repaints immediately after end_cpu; QEMU must read
  * only the independent immutable scanout storage, including on retries. */
 memset(source,0x5a,sizeof source);
 assert(vgpu_scanout_publish(head,&ops,scan,image));assert(front[scan]==head->selected_id);
 assert(!memcmp(host[front[scan]].pixels,expected,width*4*4));
}
static void frames(void){
 struct vgpu_scanout_head heads[2]={0};
 for(unsigned frame=0;frame<300;frame++){
  unsigned scan=frame%2,width=frame%11==0?2:4;draw(&heads[scan],scan,frame,width);
  assert(!heads[scan].selection_unknown && heads[scan].current->images[0].storage!=heads[scan].current->images[1].storage);
 }
 for(unsigned scan=0;scan<2;scan++)assert(vgpu_scanout_detach(&heads[scan],&ops,scan));
 assert(!live_resources && !live_dma);
 struct vgpu_scanout_source bad={(void*)1,64,0,4,4,16,0,0,4,4};assert(vgpu_scanout_source_valid(&bad));
 bad.offset=1;assert(!vgpu_scanout_source_valid(&bad));bad.offset=0;bad.pitch=12;assert(!vgpu_scanout_source_valid(&bad));
 bad.pitch=16;bad.x=1;assert(!vgpu_scanout_source_valid(&bad));bad.x=0;bad.view_width=8193;assert(!vgpu_scanout_source_valid(&bad));
 puts("PASS: 300 cropped/padded snapshots, two heads, resize retirement, independent DMA pairs and producer reuse; invalid layouts rejected");
}
static void resources(void){
 for(unsigned operation=CREATE;operation<=ATTACH;operation++)for(unsigned failure=1;failure<=2;failure++){
  struct vgpu_scanout_head h={0};fail_op=operation;fail_at=failure;delayed=operation;op_calls=0;
  assert(!vgpu_scanout_prepare(&h,&ops,0,4,4) && outstanding && h.retired && live_dma);
  unsigned old=allocs;assert(!vgpu_scanout_prepare(&h,&ops,0,4,4) && allocs==old);
  complete_late();vgpu_scanout_collect(&h,&ops);assert(!h.retired && !live_dma && !live_resources);no_failure();
 }
 for(unsigned nth=1;nth<=2;nth++){
  struct vgpu_scanout_head h={0};alloc_fail=allocs+nth;
  assert(!vgpu_scanout_prepare(&h,&ops,0,4,4));assert(!live_dma && !live_resources && !h.retired);no_failure();
 }
 for(unsigned operation=TRANSFER;operation<=FLUSH;operation++){
  struct vgpu_scanout_head h={0};draw(&h,0,1,4);
  struct vgpu_scanout_image *image=vgpu_scanout_prepare(&h,&ops,0,4,4);
  unsigned char source[64];memset(source,0x33,64);struct vgpu_scanout_source s={source,64,0,4,4,16,0,0,4,4};
  assert(vgpu_scanout_copy(image,&s));fail_op=operation;fail_at=1;delayed=operation;op_calls=0;
  assert(!vgpu_scanout_publish(&h,&ops,0,image) && outstanding);
  unsigned char before[64];memcpy(before,image->storage->virt,64);
  assert(!vgpu_scanout_prepare(&h,&ops,0,4,4));assert(!memcmp(before,image->storage->virt,64));
  complete_late();assert(vgpu_scanout_publish(&h,&ops,0,image));assert(front[0]==h.selected_id && !memcmp(host[front[0]].pixels,source,64));
  no_failure();assert(vgpu_scanout_detach(&h,&ops,0));assert(!live_resources && !live_dma);
 }
 struct vgpu_scanout_head h={0};draw(&h,0,9,4);fail_op=DESTROY;fail_at=1;op_calls=0;delayed=DESTROY;
 assert(!vgpu_scanout_detach(&h,&ops,0) && outstanding && live_dma);
 complete_late();vgpu_scanout_collect(&h,&ops);assert(!live_dma && !live_resources && !h.retired);no_failure();
 puts("PASS: delayed create/attach/upload/select/flush/unref, partial creation, allocation failures and late-host backing retention; safe immutable retries");
}
static struct vgpu_scanout_head concurrent_head;
static void *present_thread(void *arg){
 role=(uintptr_t)arg;
#ifndef NO_GATE
 gpu_scanout_acquire();
#endif
 draw(&concurrent_head,0,role,4);
#ifndef NO_GATE
 gpu_scanout_release();
#endif
 if(role==2){pthread_mutex_lock(&orchestration);b_finished=true;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&orchestration);}
 return NULL;
}
static void concurrent(void){
 for(unsigned iteration=0;iteration<100;iteration++){
  no_failure();role=0;draw(&concurrent_head,0,3,4);
  preempt=true;a_at_ack=b_waiting=b_finished=false;
  pthread_t a,b;assert(!pthread_create(&a,NULL,present_thread,(void*)1));
  pthread_mutex_lock(&orchestration);await(&a_at_ack);pthread_mutex_unlock(&orchestration);
  assert(!pthread_create(&b,NULL,present_thread,(void*)2));pthread_join(a,NULL);pthread_join(b,NULL);preempt=false;
  assert(!gpu_scanout_busy && front[0]==concurrent_head.selected_id);
  role=0;assert(vgpu_scanout_detach(&concurrent_head,&ops,0));assert(!live_resources && !live_dma);
 }
 puts("PASS: 100 forced cross-producer ACK interleavings through the actual native gate and actual scanout engine");
}
int main(int argc,char **argv){
 const char *test=argc>1?argv[1]:"all";
 if(!strcmp(test,"all") || !strcmp(test,"frames"))frames();
 if(!strcmp(test,"all") || !strcmp(test,"resources"))resources();
 if(!strcmp(test,"all") || !strcmp(test,"concurrent"))concurrent();
 assert(!live_dma && !live_resources && !gpu_scanout_busy);return 0;
}
'''
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--scenario',choices=['all','frames','resources','concurrent'],default='all');parser.add_argument('--sanitize',action='store_true');args=parser.parse_args()
 with tempfile.TemporaryDirectory(prefix='avory-scanout-engine-') as tmp:
  p=Path(tmp);(p/'mm').mkdir();(p/'lib').mkdir()
  (p/'mm/dma_alloc.h').write_text('#include <stdint.h>\n#include <stddef.h>\ntypedef struct {void *virt;uint64_t phys;size_t size,pages;uint32_t flags;}dma_buffer_t;\ndma_buffer_t *dma_alloc(size_t,uint32_t);void dma_free(dma_buffer_t*);\n')
  (p/'mm/heap.h').write_text('#include <stddef.h>\nvoid *kcalloc(size_t,size_t);void kfree(void*);\n')
  (p/'lib/string.h').write_text('#include <string.h>\n')
  (p/'test.c').write_text(C.replace('@GATE@',GATE));engine=ROOT/'kernel/src/drivers/gpu/virtio_gpu/virtio_gpu_scanout.c'
  flags=['cc','-O2','-pthread','-Wall','-Wextra','-Werror','-I'+str(p),'-I'+str(ROOT/'kernel/src'),'-I'+str(ROOT/'kernel/linuxkpi/include')]
  if args.sanitize:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
  subprocess.run(flags+[str(p/'test.c'),str(engine),'-o',str(p/'test')],check=True)
  subprocess.run([str(p/'test'),args.scenario],check=True,timeout=30)
  if args.scenario in ['all','concurrent']:
   subprocess.run(flags+['-DNO_GATE','-Wno-unused-function',str(p/'test.c'),str(engine),'-o',str(p/'old')],check=True)
   result=subprocess.run([str(p/'old'),'concurrent'],capture_output=True,timeout=10)
   assert result.returncode!=0,'unserialized scanout passed forced interleaving'
   print('PASS: bypassing transaction ownership fails the forced concurrent publication test')
if __name__=='__main__':main()
