#!/usr/bin/env python3
"""Run actual synchronous VirtIO GPU commands against a late-completing host."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/src/drivers/gpu/virtio_gpu/virtio_gpu.c').read_text()
sync=s[s.index('static uint64_t gpu_sync_command_pending;'):s.index('static struct pci_device *find_gpu(')]
batch=''
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
#define LOCKDIAG_SPOT_LOCK(s,p) do{(void)(s);assert(!*(p));*(p)=1;}while(0)
#define LOCKDIAG_SPOT_UNLOCK(s,p) do{(void)(s);assert(*(p));*(p)=0;}while(0)
struct virtio_gpu_ctrl_hdr {uint32_t type,flags;uint64_t fence_id;uint32_t ctx,pad;};
struct virtio_gpu_rect {uint32_t x,y,width,height;};
struct virtio_gpu_transfer_to_host_2d {struct virtio_gpu_ctrl_hdr hdr;struct virtio_gpu_rect r;uint64_t offset;uint32_t resource_id,pad;};
struct virtio_gpu_set_scanout {struct virtio_gpu_ctrl_hdr hdr;struct virtio_gpu_rect r;uint32_t scanout_id,resource_id;};
struct virtio_gpu_resource_flush {struct virtio_gpu_ctrl_hdr hdr;struct virtio_gpu_rect r;uint32_t resource_id,pad;};
struct drm_device;struct drm_gem_object;
struct dma {void *virt;uintptr_t phys;};
struct virtq_iov {uintptr_t addr;uint32_t len;};
static unsigned char command_page[4096],present_page[4096];
static struct dma command_dma={command_page,(uintptr_t)command_page},present_dma={present_page,(uintptr_t)present_page};
static struct {bool initialized,command_busy;struct dma *command_dma,*present_dma;int controlq;uint64_t next_fence_id;
 struct {uint64_t commands_submitted,commands_completed,invalid_responses,commands_timed_out,host_errors,present_batches,present_wait_ms,max_present_wait_ms,frames_presented,damage_pixels,present_failures;}stats;}gpu;
static struct pending {void *cookie;struct virtq_iov out,in;unsigned char request[128];}pending[4];
static unsigned nr,ready,submit_attempt,fail_submit,response_fault;
static uint64_t clock_ms;
static uint64_t lapic_timer_get_ms(void){return clock_ms++;}
static bool handle_async_completion(void *cookie,uint32_t used){(void)cookie;(void)used;return false;}
static void init_hdr(struct virtio_gpu_ctrl_hdr *h,uint32_t t){memset(h,0,sizeof *h);h->type=t;}
static void verify_owned(void){
 for(unsigned i=0;i<nr;i++)assert(!memcmp((void*)pending[i].out.addr,pending[i].request,pending[i].out.len));
}
static int submit(int *q,const struct virtq_iov *out,unsigned no,const struct virtq_iov *in,unsigned ni,void *cookie){
 (void)q;assert(gpu_poll_lock && no==1 && ni==1 && out->len<=128 && nr<4);verify_owned();
 if(++submit_attempt==fail_submit)return -1;
 pending[nr]=(struct pending){.cookie=cookie,.out=*out,.in=*in};
 memcpy(pending[nr].request,(void*)out->addr,out->len);nr++;return 0;
}
#define virtq_submit submit
#define virtq_submit_deferred submit
static void virtq_kick(int *q){(void)q;assert(gpu_poll_lock);}
static bool virtq_poll_complete(int *q,void **cookie,uint32_t *used){
 (void)q;assert(gpu_poll_lock);verify_owned();if(!nr || !ready)return false;
 struct pending p=pending[0];memmove(pending,pending+1,--nr*sizeof *pending);ready--;
 const struct virtio_gpu_ctrl_hdr *req=(void*)p.out.addr;struct virtio_gpu_ctrl_hdr *resp=(void*)p.in.addr;
 *resp=(struct virtio_gpu_ctrl_hdr){.type=VIRTIO_GPU_RESP_OK_NODATA,.flags=VIRTIO_GPU_FLAG_FENCE,.fence_id=req->fence_id};
 if(response_fault==1)resp->flags=0;
 if(response_fault==2)resp->fence_id++;
 *cookie=p.cookie;*used=response_fault==3?4:p.in.len;return true;
}
'''
post=r'''
static void reset(void){
 assert(!nr);memset(&gpu,0,sizeof gpu);gpu.initialized=true;gpu.command_dma=&command_dma;gpu.present_dma=&present_dma;
 gpu.next_fence_id=20;ready=submit_attempt=fail_submit=response_fault=0;clock_ms=0;
 assert(!gpu_sync_command_pending && !gpu_poll_lock);
}
static bool command(void){struct virtio_gpu_ctrl_hdr req={.type=0x100},resp;return gpu_command(&req,sizeof req,&resp,sizeof resp,NULL);}
int main(void){
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
 puts("PASS: 100 delayed command ACKs, malformed fences/lengths, DMA quarantine, completion reaping and safe retries");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-virtio-ownership-') as tmp:
 p=Path(tmp);code=pre+sync+batch+post;(p/'test.c').write_text(code)
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
 for variant in [code.replace('gpu.stats.commands_timed_out++;\n            return false;', 'gpu.stats.commands_timed_out++;\n            gpu.command_busy=false;\n            return false;')]:
  assert variant!=code
  (p/'test.c').write_text(variant)
  subprocess.run(['cc','-O2','-Wno-unused-function',str(p/'test.c'),'-o',str(p/'old')],check=True)
  result=subprocess.run([str(p/'old')],capture_output=True)
  assert result.returncode!=0,'premature DMA return after timeout passed'
 print('PASS: the old command timeout path fails the retained-DMA ownership checks')
