#!/usr/bin/env python3
"""Run the actual fenced render queue against delayed and malformed host replies."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/src/drivers/gpu/virtio_gpu/virtio_gpu.c').read_text()
jobs = source[source.index('#define GPU_RENDER_JOBS'):source.index('static uint64_t gpu_sync_command_pending;')]
reap = source[source.index('static void gpu_reap_control_locked('):source.index('static bool gpu_control_ready(')]
queue = source[source.index('static int gpu_render_wait_fence('):]
pre = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define GPU_TIMEOUT_MS 1000
#define REQ_CAP 1024
#define DMA_FLAG_32BIT 1
#define VIRTIO_GPU_FLAG_FENCE 1
#define VIRTIO_GPU_RESP_OK_NODATA 0x1100
#define LOCKDIAG_SPOT_GPU_POLL 0
#define DRM_MAX_HANDLES_PER_FILE 4096
static int gpu_poll_lock;
#define LOCKDIAG_SPOT_LOCK(s,p) do{(void)(s);assert(!*(p));*(p)=1;}while(0)
#define LOCKDIAG_SPOT_UNLOCK(s,p) do{(void)(s);assert(*(p));*(p)=0;}while(0)
struct drm_device {int unused;};
struct drm_gem_object {struct drm_device *dev;int refs;uint32_t virgl_pending_jobs;};
static unsigned frees;
static bool ascentdrm_gem_put_last(struct drm_gem_object *o){assert(!gpu_poll_lock&&o->refs>0);return !--o->refs;}
static void ascentdrm_gem_object_free(struct drm_device *d,struct drm_gem_object *o){(void)d;assert(!gpu_poll_lock&&!o->refs);frees++;}
static void kfree(void *p){assert(!gpu_poll_lock);free(p);}
typedef struct {void *virt;uintptr_t phys;size_t size;} dma_buffer_t;
static unsigned dma_calls,dma_budget=~0U;static bool dma_exhausted;
static dma_buffer_t *dma_alloc(size_t n,unsigned flags){(void)flags;assert(!gpu_poll_lock);dma_calls++;if(dma_exhausted||!dma_budget)return NULL;dma_budget--;dma_buffer_t*d=malloc(sizeof(*d));assert(d);d->virt=calloc(1,n);d->phys=(uintptr_t)d->virt;d->size=n;return d;}
static void dma_free(dma_buffer_t*d){assert(!gpu_poll_lock);free(d->virt);free(d);}
struct virtio_gpu_ctrl_hdr {uint32_t type,flags;uint64_t fence_id;uint32_t ctx_id,pad;};
struct virtq_iov {uint64_t phys_addr;uint32_t len;};
struct queue {int unused;};
static struct {bool command_busy;uint64_t next_fence_id;struct queue controlq;
 struct {uint64_t commands_submitted,commands_completed,async_submitted,async_completed,host_errors,invalid_responses,max_frames_in_flight;}stats;}gpu;
static struct pending {struct virtq_iov out,in;void *cookie;unsigned char bytes[REQ_CAP];} pending[16];
static unsigned nr,ready,waits;static bool complete_on_wait,malformed,reverse;
static bool queue_later;
static bool complete_one;
static uint64_t clock_ms;
static uint64_t gpu_sync_command_pending,gpu_batch_fences[3];
static unsigned gpu_batch_pending;
#define GPU_BATCH_COMMANDS 3
static uint64_t lapic_timer_get_ms(void){return clock_ms++;}
static int virtq_submit(struct queue*q,const struct virtq_iov*out,unsigned no,const struct virtq_iov*in,unsigned ni,void*cookie){
 (void)q;assert(gpu_poll_lock&&no==1&&ni==1&&nr<16&&out->len<=REQ_CAP);
 pending[nr]=(struct pending){.out=*out,.in=*in,.cookie=cookie};
 memcpy(pending[nr].bytes,(void*)out->phys_addr,out->len);nr++;return 0;
}
static bool virtq_poll_complete(struct queue*q,void**cookie,uint32_t*used){
 (void)q;assert(gpu_poll_lock);
 for(unsigned i=0;i<nr;i++)assert(!memcmp(pending[i].bytes,(void*)pending[i].out.phys_addr,pending[i].out.len));
 if(!nr||!ready)return false;
 unsigned index=reverse?nr-1:0;struct pending p=pending[index];nr--;ready--;
 memmove(pending+index,pending+index+1,(nr-index)*sizeof(*pending));
 struct virtio_gpu_ctrl_hdr *req=(void*)p.out.phys_addr,*reply=(void*)p.in.phys_addr;
 *reply=(struct virtio_gpu_ctrl_hdr){.type=VIRTIO_GPU_RESP_OK_NODATA,.flags=1,.fence_id=req->fence_id+(malformed?1:0)};
 *cookie=p.cookie;*used=sizeof(*reply);return true;
}
static bool handle_batch_completion(void*c,uint32_t n){(void)c;(void)n;return false;}
static bool handle_sync_completion(void*c,uint32_t n){(void)c;(void)n;return false;}
static void gpu_drain_completions(void);
bool virtio_gpu_render_submit(const void *,uint32_t,struct drm_gem_object **,uint32_t);
static void gpu_wait_for_control(uint64_t start){
 (void)start;assert(!gpu_poll_lock);waits++;clock_ms+=10;
 if(complete_on_wait){ready=complete_one?1:nr;gpu_drain_completions();}
 if(queue_later){
  queue_later=false;complete_on_wait=false;
  struct virtio_gpu_ctrl_hdr req={.type=0x207,.ctx_id=7};
  assert(virtio_gpu_render_submit(&req,sizeof(req),NULL,0));
 }
}
static void gpu_wait_for_control_progress(uint64_t start,const uint64_t *completed){
 if(gpu.stats.commands_completed!=*completed)return;
 gpu_wait_for_control(start);
}
'''
helpers = r'''
static void gpu_drain_completions(void){LOCKDIAG_SPOT_LOCK(0,&gpu_poll_lock);gpu_reap_control_locked();LOCKDIAG_SPOT_UNLOCK(0,&gpu_poll_lock);}
static unsigned notified[17];static int notification_error[17];
static void notify(void *data,int error){unsigned i=(uintptr_t)data;assert(!gpu_poll_lock&&i<17);notified[i]++;notification_error[i]=error;}
'''
main = r'''
int main(void){
 /* Large disjoint lists are the expensive case in resource-heavy scenes.
  * Verify full-pointer membership, duplicates, context ordering and the
  * allocation-free dependency queries without completing the conflicting batch. */
 struct drm_gem_object *scene=calloc(8192,sizeof(*scene));assert(scene);
 struct drm_gem_object **scene_refs=malloc(4095*sizeof(*scene_refs));assert(scene_refs);
 struct drm_gem_object **query=malloc(4095*sizeof(*query));assert(query);
 for(unsigned i=0;i<4095;i++){scene[i].virgl_pending_jobs=1;scene_refs[i]=&scene[i];query[i]=&scene[4096+i];}
 gpu_render_jobs[0]=(struct gpu_render_job){.objects=scene_refs,.count=4095,.context=7,.state=1,.fence=1};
 for(unsigned n=16;n<=4095;n=n==4095?4096:n*2>4095?4095:n*2){
  assert(!gpu_render_wait_objects(query,n,8,true));
  query[n-1]=scene_refs[4094];
  assert(gpu_render_wait_objects(query,n,8,true)==-16);
  assert(!gpu_render_wait_objects(query,n,7,true));
  query[n-1]=&scene[4096+n-1];
 }
 for(unsigned i=0;i<4095;i++)query[i]=&scene[8191];
 assert(!gpu_render_wait_objects(query,4095,8,true));
 query[4094]=scene_refs[4094];
 assert(gpu_render_wait_objects(query,4095,8,true)==-16);
 memset(gpu_render_jobs,0,sizeof(gpu_render_jobs));
 free(query);free(scene_refs);free(scene);
 puts("PASS: large BO membership, late conflicts, duplicates, same-context ordering and allocation-free queries");
 gpu_render_prepare();assert(gpu_render_capacity==16&&virtio_gpu_render_queued());
 unsigned allocated=dma_calls;dma_exhausted=true;
 struct drm_device dev={0};struct drm_gem_object objects[17]={0};
 struct virtio_gpu_ctrl_hdr req={.type=0x207,.ctx_id=7};
 for(unsigned i=0;i<16;i++){
  objects[i]=(struct drm_gem_object){.dev=&dev,.refs=2};
  struct drm_gem_object **refs=malloc(sizeof(*refs));refs[0]=&objects[i];
  assert(virtio_gpu_render_submit_notify(&req,sizeof(req),refs,1,notify,(void *)(uintptr_t)i));
  assert(nr==i+1&&gpu_render_pending==i+1&&!waits);
  assert(!notified[i]);
  assert(!ascentdrm_gem_put_last(&objects[i]));
 }
 struct drm_gem_object *first=&objects[0];
 assert(virtio_gpu_render_wait_bo(first,true)==-16);
 assert(!virtio_gpu_render_wait_bo(&objects[16],true));
 assert(!virtio_gpu_render_wait(&first,1,7)&&gpu_render_pending==16);
 assert(virtio_gpu_wait_idle(true)==-16&&!frees);
 assert(gpu.stats.max_frames_in_flight==16);
 gpu_render_reclaim();assert(!frees);
 /* Queue pressure must never overwrite host-owned DMA or release BOs. */
 assert(!virtio_gpu_render_submit(&req,sizeof(req),NULL,0));
 assert(nr==16&&gpu_render_pending==16&&!frees&&waits);
 ready=8;reverse=true;gpu_drain_completions();gpu_render_reclaim();
 assert(gpu_render_pending==8&&nr==8&&frees==8);
 for(unsigned i=0;i<16;i++)assert(notified[i]==(i>=8)&&!notification_error[i]);
 assert(virtio_gpu_render_submit(&req,sizeof(req),NULL,0)&&nr==9);
 complete_on_wait=true;assert(!virtio_gpu_render_wait(&first,1,8));
 assert(!nr&&!gpu_render_pending&&frees==8);
 gpu_render_reclaim();assert(frees==16);
 for(unsigned i=0;i<16;i++)assert(notified[i]==1&&!notification_error[i]);
 assert(gpu.stats.async_submitted==17&&gpu.stats.async_completed==17);
 /* Fragmented/exhausted DMA after boot cannot break a larger 3D packet. */
 unsigned char large[REQ_CAP]={0};((struct virtio_gpu_ctrl_hdr *)large)->type=0x207;
 assert(virtio_gpu_render_submit(large,sizeof(large),NULL,0));
 assert(dma_calls==allocated);
 ready=nr;gpu_drain_completions();gpu_render_reclaim();
 /* Header/payload snapshots need no temporary command buffer. Rewriting the
  * caller's bytes after submission must not alter host-owned DMA. */
 unsigned char payload[REQ_CAP-sizeof(req)];memset(payload,0x5a,sizeof(payload));
 assert(!virtio_gpu_render_submit_parts(&req,sizeof(req),NULL,1,NULL,0,NULL,NULL));
 assert(!virtio_gpu_render_submit_parts(&req,sizeof(req),payload,~0U,NULL,0,NULL,NULL));
 assert(virtio_gpu_render_submit_parts(&req,sizeof(req),payload,sizeof(payload),NULL,0,NULL,NULL));
 memset(payload,0xa5,sizeof(payload));
 assert(dma_calls==allocated);
 ready=nr;gpu_drain_completions();gpu_render_reclaim();
 /* A future client's submission must not extend an existing WAIT. */
 assert(virtio_gpu_render_submit(&req,sizeof(req),NULL,0));
 queue_later=true;assert(!virtio_gpu_wait_idle(false));
 assert(nr==1&&gpu_render_pending==1&&virtio_gpu_wait_idle(true)==-16);
 ready=nr;gpu_drain_completions();gpu_render_reclaim();
 /* An already-queued unrelated batch must not delay this texture's reader. */
 struct drm_gem_object shared={.dev=&dev,.refs=2},*shared_ref=&shared;
 struct drm_gem_object **shared_refs=malloc(sizeof(*shared_refs));shared_refs[0]=&shared;
 assert(virtio_gpu_render_submit(&req,sizeof(req),shared_refs,1));
 assert(virtio_gpu_render_submit(&req,sizeof(req),NULL,0));
 complete_on_wait=true;complete_one=true;reverse=false;
 assert(!virtio_gpu_render_wait(&shared_ref,1,8));
 assert(nr==1&&gpu_render_pending==1);
 ready=nr;gpu_drain_completions();gpu_render_reclaim();complete_one=false;
 /* An older unrelated batch must not hold a completed newer producer.
  * Test WAIT and cross-context sharing, including immediate slot reuse. */
 for(unsigned mode=0;mode<3;mode++){
  assert(virtio_gpu_render_submit(&req,sizeof(req),NULL,0));
  shared_refs=malloc(sizeof(*shared_refs));shared_refs[0]=&shared;shared.refs++;
  assert(virtio_gpu_render_submit(&req,sizeof(req),shared_refs,1));
  complete_on_wait=true;complete_one=true;reverse=true;
  queue_later=mode==2;
  unsigned before=waits;
  if(mode==0)assert(!virtio_gpu_render_wait_bo(&shared,false));
  else assert(!virtio_gpu_render_wait(&shared_ref,1,8));
  assert(waits==before+1&&nr==(mode==2?2U:1U));
  assert(virtio_gpu_wait_idle(true)==-16);
  ready=nr;gpu_drain_completions();gpu_render_reclaim();
 }
 complete_one=false;reverse=false;
 puts("PASS: BO and cross-context waits ignore older unrelated work and reused producer slots");
 /* One BO can occur repeatedly in several batches and contexts. A newer
  * completion must clear only its own slot, including with duplicate handles.
  * Reusing that slot for another BO must not create a stale dependency. */
 struct drm_gem_object common={.dev=&dev,.refs=1},*common_ref=&common;
 for(unsigned batch=0;batch<3;batch++){
  unsigned count=batch?1:3;
  struct drm_gem_object **r=malloc(count*sizeof(*r));
  for(unsigned i=0;i<count;i++){r[i]=&common;common.refs++;}
  req.ctx_id=batch==2?8:7;
  assert(virtio_gpu_render_submit(&req,sizeof(req),r,count));
 }
 assert(common.virgl_pending_jobs==7);
 assert(virtio_gpu_render_wait_bo(&common,true)==-16);
 assert(gpu_render_wait_objects(&common_ref,1,7,true)==-16);
 reverse=true;ready=1;gpu_drain_completions();gpu_render_reclaim();
 assert(common.virgl_pending_jobs==3&&common.refs==5);
 assert(!virtio_gpu_render_wait(&common_ref,1,7));
 req.ctx_id=9;assert(virtio_gpu_render_submit(&req,sizeof(req),NULL,0));
 assert(!virtio_gpu_render_wait(&common_ref,1,7));
 assert(gpu_render_wait_objects(&common_ref,1,8,true)==-16);
 ready=nr;gpu_drain_completions();gpu_render_reclaim();
 assert(!common.virgl_pending_jobs&&common.refs==1);
 req.ctx_id=7;reverse=false;
 puts("PASS: shared BO masks survive reordered completions, duplicate handles and slot reuse across contexts");
 /* A malformed fence cannot authorize freeing a resource used by the GPU. */
 objects[16]=(struct drm_gem_object){.dev=&dev,.refs=2};
 struct drm_gem_object **refs=malloc(sizeof(*refs));refs[0]=&objects[16];
 assert(virtio_gpu_render_submit_notify(&req,sizeof(req),refs,1,notify,(void *)16));
 assert(!ascentdrm_gem_put_last(&objects[16]));
 malformed=true;ready=1;gpu_drain_completions();gpu_render_reclaim();
 assert(gpu_render_failed&&objects[16].refs==1&&frees==16);
 assert(objects[16].virgl_pending_jobs); /* Quarantine retains the dependency. */
 assert(notified[16]==1&&notification_error[16]==-5);
 gpu_render_reclaim();assert(notified[16]==1&&objects[16].refs==1);
 assert(virtio_gpu_wait_idle(false)==-5);
 assert(!virtio_gpu_render_submit(&req,sizeof(req),NULL,0));
 for(unsigned i=0;i<16;i++)dma_free(gpu_render_jobs[i].dma);
 memset(gpu_render_jobs,0,sizeof(gpu_render_jobs));gpu_render_capacity=0;gpu_render_failed=false;
 gpu_render_prepare();assert(!virtio_gpu_render_queued());
 assert(!virtio_gpu_render_submit(&req,sizeof(req),NULL,0));
 dma_exhausted=false;dma_budget=2;gpu_render_prepare();
 assert(gpu_render_capacity==2&&virtio_gpu_render_queued());
 for(unsigned i=0;i<2;i++)dma_free(gpu_render_jobs[i].dma);
 puts("PASS: 16 in-flight fenced batches, immutable DMA under pressure, reordered completions, WAIT/backpressure, deferred BO release and malformed-fence quarantine");
 puts("PASS: zero runtime DMA allocations with larger packets after allocator exhaustion; partial and unavailable boot pools select bounded queue/fallback");
}
'''
with tempfile.TemporaryDirectory(prefix='virgl-render-queue-') as tmp:
    path = Path(tmp)
    code = pre+jobs+reap+helpers+queue+main
    (path/'test.c').write_text(code)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
    mutant = code.replace('job->state = valid ? 2 : 4;', 'job->state = 2;')
    assert mutant != code
    (path/'mutant.c').write_text(mutant)
    subprocess.run(['cc','-O2',str(path/'mutant.c'),'-o',str(path/'mutant')],check=True)
    assert subprocess.run([str(path/'mutant')],capture_output=True).returncode != 0
    print('PASS: releasing BOs after a malformed fence fails the quarantine regression')
    mutant = code.replace('job->objects[n]->virgl_pending_jobs &= ~(1U << i);',
                          'job->objects[n]->virgl_pending_jobs = 0;')
    assert mutant != code
    (path/'mask-mutant.c').write_text(mutant)
    subprocess.run(['cc','-O2',str(path/'mask-mutant.c'),'-o',str(path/'mask-mutant')],check=True)
    assert subprocess.run([str(path/'mask-mutant')],capture_output=True).returncode != 0
    print('PASS: clearing another pending batch\'s dependency fails the reordered shared-BO regression')

    mutant = code.replace('gpu_render_jobs[i].fence == fences[i]', 'true')
    assert mutant != code
    (path/'reuse-mutant.c').write_text(mutant)
    subprocess.run(['cc','-O2',str(path/'reuse-mutant.c'),'-o',str(path/'reuse-mutant')],check=True)
    assert subprocess.run([str(path/'reuse-mutant')],capture_output=True).returncode != 0
    print('PASS: tracking slots without fence identities fails the immediate-reuse regression')
