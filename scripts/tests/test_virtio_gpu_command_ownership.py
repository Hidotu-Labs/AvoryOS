#!/usr/bin/env python3
"""Run actual synchronous VirtIO GPU commands against a late-completing host."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/src/drivers/gpu/virtio_gpu/virtio_gpu.c').read_text()
gates=s[s.index('static bool gpu_gate_try_acquire('):s.index('static void gpu_scanout_acquire(')]
sync=s[s.index('static uint64_t gpu_sync_command_pending;'):s.index('static struct pci_device *find_gpu(')]
batch=s[s.index('static int scanout_present_batch('):s.index('static const struct vgpu_scanout_ops scanout_ops =')]
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define PAGE_SIZE 4096
#define REQ_OFF 0
#define REQ_CAP 1024
#define RESP_OFF 2048
#define RESP_CAP 2048
#define GPU_TIMEOUT_MS 4
#define VIRTIO_GPU_FLAG_FENCE 1
#define VIRTIO_GPU_RESP_OK_NODATA 0x1100
#define VIRTIO_GPU_RESP_OK_MAP_INFO 0x1106
#define VIRTIO_GPU_RESP_ERR_UNSPEC 0x1200
#define VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER 0x1205
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D 0x105
#define VIRTIO_GPU_CMD_SET_SCANOUT 0x103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH 0x104
#define LOCKDIAG_SPOT_GPU_POLL 0
static int gpu_poll_lock;
static bool gpu_worker_started, gpu_irq_installed;
#define LOCKDIAG_SPOT_LOCK(s,p) do{(void)(s);assert(!*(p));*(p)=1;}while(0)
#define LOCKDIAG_SPOT_UNLOCK(s,p) do{(void)(s);assert(*(p));*(p)=0;}while(0)
struct virtio_gpu_ctrl_hdr {uint32_t type,flags;uint64_t fence_id;uint32_t ctx,pad;};
struct virtio_gpu_rect {uint32_t x,y,width,height;};
struct virtio_gpu_transfer_to_host_2d {struct virtio_gpu_ctrl_hdr hdr;struct virtio_gpu_rect r;uint64_t offset;uint32_t resource_id,pad;};
struct virtio_gpu_set_scanout {struct virtio_gpu_ctrl_hdr hdr;struct virtio_gpu_rect r;uint32_t scanout_id,resource_id;};
struct virtio_gpu_resource_flush {struct virtio_gpu_ctrl_hdr hdr;struct virtio_gpu_rect r;uint32_t resource_id,pad;};
struct drm_device;struct drm_gem_object;
struct thread {bool is_idle;unsigned state;uint64_t wakeup_ticks;};
#define THREAD_RUNNING 1
#define THREAD_BLOCKED 2
static struct thread current;
static struct thread *sched_get_current(void){return &current;}
static bool hal_irq_enabled(void){return !gpu_poll_lock;}
typedef unsigned hal_irq_state_t;
static hal_irq_state_t hal_irq_save(void){return 1;}
static void hal_irq_restore(hal_irq_state_t state){assert(state==1);}
typedef struct {bool queued;} wait_queue_t;
typedef struct {struct thread *thread;} wait_queue_entry_t;
static unsigned yields,complete_on_yield,complete_on_add,worker_reap;
static bool progress_on_add;
static void sched_yield(void);
static void wait_queue_add(wait_queue_t *,wait_queue_entry_t *);
static void wait_queue_add_tail(wait_queue_t *q,wait_queue_entry_t *e){wait_queue_add(q,e);}
static void wait_queue_wake_one(wait_queue_t *q){(void)q;}
static void wait_queue_remove(wait_queue_t *q,wait_queue_entry_t *e){(void)e;assert(q->queued);q->queued=false;}
struct used_ring {unsigned idx;};
static struct used_ring used;
struct queue {unsigned last_used_idx;struct used_ring *used;};
struct dma {void *virt;uintptr_t phys;};
struct virtq_iov {uint64_t phys_addr;uint32_t len;};
static unsigned char command_page[4096],present_page[4096];
static struct dma command_dma={command_page,(uintptr_t)command_page},present_dma={present_page,(uintptr_t)present_page};
static struct {bool initialized,command_busy;struct dma *command_dma,*present_dma;struct queue controlq;uint64_t next_fence_id;uint32_t head_resource[2];
 struct {uint64_t commands_submitted,commands_completed,invalid_responses,commands_timed_out,host_errors,present_batches,present_wait_ms,max_present_wait_ms,frames_presented,damage_pixels,present_failures;}stats;}gpu;
static struct pending {void *cookie;struct virtq_iov out,in;unsigned char request[128];}pending[4];
static unsigned nr,ready,submit_attempt,fail_submit,response_fault,kicks;
static bool reverse_complete;
static uint64_t clock_ms;
static uint64_t lapic_timer_get_ms(void){return clock_ms++;}
static uint64_t lapic_timer_get_ticks(void){return clock_ms;}
static unsigned gpu_render_pending;
static bool handle_render_completion(void *cookie,uint32_t used){(void)cookie;(void)used;return false;}
static bool handle_async_completion(void *cookie,uint32_t used){(void)cookie;(void)used;return false;}
static void init_hdr(struct virtio_gpu_ctrl_hdr *h,uint32_t t){memset(h,0,sizeof *h);h->type=t;}
static void verify_owned(void){
 for(unsigned i=0;i<nr;i++)assert(!memcmp((void*)pending[i].out.phys_addr,pending[i].request,pending[i].out.len));
}
static int submit(struct queue *q,const struct virtq_iov *out,unsigned no,const struct virtq_iov *in,unsigned ni,void *cookie){
 (void)q;assert(gpu_poll_lock && no==1 && ni==1 && out->len<=128 && nr<4);verify_owned();
 if(++submit_attempt==fail_submit)return -1;
 pending[nr]=(struct pending){.cookie=cookie,.out=*out,.in=*in};
 memcpy(pending[nr].request,(void*)out->phys_addr,out->len);nr++;return 0;
}
#define virtq_submit submit
#define virtq_submit_deferred submit
static void virtq_kick(struct queue *q){(void)q;assert(gpu_poll_lock);kicks++;}
static bool virtq_poll_complete(struct queue *q,void **cookie,uint32_t *used){
 (void)q;assert(gpu_poll_lock);verify_owned();
 if(!nr || !ready)return false;
 unsigned index=reverse_complete?nr-1:0;
 struct pending p=pending[index];--nr;
 memmove(pending+index,pending+index+1,(nr-index)*sizeof *pending);ready--;
 const struct virtio_gpu_ctrl_hdr *req=(void*)p.out.phys_addr;struct virtio_gpu_ctrl_hdr *resp=(void*)p.in.phys_addr;
 *resp=(struct virtio_gpu_ctrl_hdr){.type=VIRTIO_GPU_RESP_OK_NODATA,.flags=VIRTIO_GPU_FLAG_FENCE,.fence_id=req->fence_id};
 if(response_fault==1)resp->flags=0;
 if(response_fault==2)resp->fence_id++;
 if(response_fault==4)resp->type=VIRTIO_GPU_RESP_ERR_UNSPEC;
 *cookie=p.cookie;*used=response_fault==3?4:p.in.len;return true;
}
'''
batch='''struct vgpu_scanout_damage {uint32_t x,y,width,height;};
static void scanout_record_selection(uint32_t head,uint32_t id){gpu.head_resource[head]=id;}
'''+batch
post=r'''
static void complete_wait(void){
 ready=nr;
 if(worker_reap){
  LOCKDIAG_SPOT_LOCK(0,&gpu_poll_lock);gpu_reap_control_locked();
  assert(!gpu_sync_command_pending && !gpu_batch_pending && gpu.command_busy &&
         (gpu_sync_command_waiter || gpu_batch_waiter));
  LOCKDIAG_SPOT_UNLOCK(0,&gpu_poll_lock);
 }
}
static void wait_queue_add(wait_queue_t *q,wait_queue_entry_t *e){
 assert(!gpu_poll_lock && !q->queued && e->thread->state==THREAD_BLOCKED);q->queued=true;
 if(progress_on_add)gpu.stats.commands_completed++;
 if(complete_on_add){complete_wait();used.idx++;}
}
static void sched_yield(void){
 assert(!gpu_poll_lock);yields++;
 if(complete_on_yield)complete_wait();
 current.state=THREAD_RUNNING;
}
static void reset(void){
 assert(!nr);memset(&gpu,0,sizeof gpu);gpu.initialized=true;gpu.command_dma=&command_dma;gpu.present_dma=&present_dma;
 gpu.next_fence_id=20;ready=submit_attempt=fail_submit=response_fault=0;clock_ms=0;
 kicks=0;reverse_complete=false;gpu_present_batch_enabled=true;
 gpu.controlq.used=&used;used.idx=0;current=(struct thread){.state=THREAD_RUNNING};
 gpu_worker_started=gpu_irq_installed=false;
 yields=complete_on_yield=complete_on_add=worker_reap=0;
 assert(!gpu_sync_command_pending && !gpu_poll_lock && !gpu_sync_command_waiter && !gpu_command_owner && !gpu_command_wait.queued);
 assert(!gpu_batch_pending&&!gpu_batch_waiter);
}
static bool command(void){struct virtio_gpu_ctrl_hdr req={.type=0x100},resp;return gpu_command(&req,sizeof req,&resp,sizeof resp,NULL);}
static int present(bool transfer,bool select){
 struct vgpu_scanout_damage damage={2,3,6,5};
 return scanout_present_batch(0,42,64,48,&damage,transfer,select);
}
int main(void){
 /* Producer completion reaped before registration leaves no used-ring edge.
  * Unrelated pending work must not force the waiter to sleep. */
 reset();gpu_worker_started=gpu_irq_installed=true;gpu_render_pending=1;
 uint64_t completed=gpu.stats.commands_completed;progress_on_add=true;
 gpu_wait_for_control_progress(lapic_timer_get_ms(),&completed);
 assert(!yields&&!gpu_command_wait.queued&&!current.wakeup_ticks);
 progress_on_add=false;completed=gpu.stats.commands_completed;
 gpu_wait_for_control_progress(lapic_timer_get_ms(),&completed);
 assert(yields==1&&!gpu_command_wait.queued&&!current.wakeup_ticks);
 gpu_render_pending=0;
 puts("PASS: reaped producer completion during waiter registration avoids sleep with unrelated work pending");
 for(unsigned cycle=0;cycle<100;cycle++){
  reset();assert(!command() && nr==1 && gpu.command_busy && gpu_sync_command_pending);
  unsigned before=submit_attempt;unsigned char page[4096];memcpy(page,command_page,sizeof page);
  assert(!command() && submit_attempt==before && !memcmp(page,command_page,sizeof page));
  verify_owned();assert(!gpu_control_ready());
  /* The retry reaps the late ACK before writing another request. */
  ready=2;assert(command() && !nr && !gpu.command_busy && !gpu_sync_command_pending);
  assert(gpu.stats.commands_submitted==gpu.stats.commands_completed && gpu.stats.commands_timed_out==1);
  for(unsigned fault=1;fault<=3;fault++){
   reset();response_fault=fault;ready=1;assert(!command() && !nr && !gpu.command_busy);

  }
 }
 for(unsigned race=0;race<4;race++){
  reset();gpu_worker_started=gpu_irq_installed=true;
  worker_reap=race&1;complete_on_add=race&2;complete_on_yield=!complete_on_add;
  assert(command() && !nr && !gpu.command_busy && !gpu_sync_command_pending);
  assert(!gpu_command_wait.queued && !current.wakeup_ticks && !gpu_command_owner);
  assert(yields==(complete_on_add?0:1));
 }
 reset();gpu_worker_started=gpu_irq_installed=true;
 assert(!command() && nr==1 && gpu.command_busy && gpu_sync_command_pending);
 assert(!gpu_sync_command_waiter && !gpu_command_owner && !gpu_command_wait.queued);
 assert(!current.wakeup_ticks);
 ready=1;assert(gpu_control_ready() && !nr);
 reset();gpu_worker_started=true;complete_on_yield=1;
 assert(command() && yields==1 && !gpu_command_wait.queued);
 for(unsigned cycle=0;cycle<50;cycle++){
  reset();assert(present(true,true)<0&&nr==3&&gpu.command_busy&&gpu_batch_pending==7);
  unsigned char retained[4096];memcpy(retained,present_page,sizeof retained);
  unsigned before=submit_attempt;
  assert(present(true,true)<0&&submit_attempt==before&&!memcmp(retained,present_page,sizeof retained));
  ready=1;assert(!gpu_control_ready()&&nr==2&&gpu.command_busy);
  memcpy(retained,present_page,sizeof retained);
  assert(!command()&&nr==2&&!memcmp(retained,present_page,sizeof retained));
  ready=2;assert(gpu_control_ready()&&!nr&&!gpu_batch_pending);
  ready=3;reverse_complete=true;
  assert(present(true,true)==1&&!nr&&!gpu.command_busy&&kicks==2&&gpu.head_resource[0]==42);
 }
 for(unsigned failure=1;failure<=3;failure++){
  reset();fail_submit=failure;ready=3;
  assert(present(true,true)<0&&!nr&&!gpu.command_busy&&!gpu_batch_pending);
  assert(kicks==(failure==1?0u:1u));
 }
 for(unsigned fault=1;fault<=4;fault++){
  reset();ready=3;response_fault=fault;
  assert(present(true,true)<0&&!nr&&!gpu.command_busy&&!gpu_batch_pending);
  assert(gpu.stats.invalid_responses+gpu.stats.host_errors==3);
 }
 for(unsigned race=0;race<4;race++){
  reset();gpu_worker_started=gpu_irq_installed=true;
  worker_reap=race&1;complete_on_add=race&2;complete_on_yield=!complete_on_add;
  assert(present(true,true)==1&&!nr&&!gpu.command_busy&&!gpu_batch_pending);
  assert(!gpu_command_wait.queued&&!current.wakeup_ticks&&!gpu_command_owner&&kicks==1);
  assert(yields==(complete_on_add?0u:1u));
 }
 reset();ready=2;assert(present(false,true)==1&&gpu.stats.commands_submitted==2&&kicks==1);
 reset();ready=1;assert(present(false,false)==1&&gpu.stats.commands_submitted==1&&kicks==1);
 reset();gpu_present_batch_enabled=false;assert(present(true,true)==0&&!nr&&!submit_attempt);
 puts("PASS: delayed ACKs, malformed responses, IRQ/worker/registration races, unlocked sleeps, DMA quarantine and safe retries");
 puts("PASS: old-style one-kick batches, partial submission, reordered/late ACKs, malformed fences, task/IRQ races, disabled fallback and no DMA reuse after timeout");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-virtio-ownership-') as tmp:
 p=Path(tmp);code=pre+gates+sync+batch+post;(p/'test.c').write_text(code)
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
 for variant in [code.replace('gpu.stats.commands_timed_out++;', 'gpu.stats.commands_timed_out++;\n            gpu.command_busy=false;')]:
  assert variant!=code
  (p/'test.c').write_text(variant)
  subprocess.run(['cc','-O2','-Wno-unused-function',str(p/'test.c'),'-o',str(p/'old')],check=True)
  result=subprocess.run([str(p/'old')],capture_output=True)
  assert result.returncode!=0,'premature DMA return after timeout passed'
 print('PASS: the old command timeout path fails the retained-DMA ownership checks')

 variant=code.replace('(completed && gpu.stats.commands_completed != *completed)', 'false')
 assert variant!=code
 (p/'progress-mutant.c').write_text(variant)
 subprocess.run(['cc','-O2','-Wno-unused-function',str(p/'progress-mutant.c'),'-o',str(p/'progress-mutant')],check=True)
 assert subprocess.run([str(p/'progress-mutant')],capture_output=True).returncode!=0
 print('PASS: removing the completion-progress check fails the waiter-registration regression')
