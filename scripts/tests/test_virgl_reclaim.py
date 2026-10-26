#!/usr/bin/env python3
"""Exercise batched render cleanup, including callback reentry and slot reuse."""
import ast
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
module = ast.parse((root/'scripts/tests/test_virgl_render_queue.py').read_text())
strings = {n.targets[0].id: ast.literal_eval(n.value) for n in module.body
           if isinstance(n, ast.Assign) and isinstance(n.targets[0], ast.Name)
           and isinstance(n.value, ast.Constant) and isinstance(n.value.value, str)}
source = (root/'kernel/src/drivers/gpu/virtio_gpu/virtio_gpu.c').read_text()
jobs = source[source.index('#define GPU_RENDER_JOBS'):source.index('static uint64_t gpu_sync_command_pending;')]
reap = source[source.index('static void gpu_reap_control_locked('):source.index('static bool gpu_control_ready(')]
queue = source[source.index('static int gpu_render_wait_fence('):]
pre = strings['pre'].replace('static int gpu_poll_lock;', 'static int gpu_poll_lock; static unsigned acquisitions;')
pre = pre.replace('*(p)=1;', '*(p)=1;acquisitions++;')
main = r'''
static unsigned callbacks;
static struct drm_gem_object fresh;
static void reenter(void *data,int error){
 (void)data;assert(!error&&!gpu_poll_lock);callbacks++;
 if(callbacks!=1)return;
 /* All completed slots must be available before notifying a waiting client.
  * Another reclaimer must not steal the outer invocation's detached work. */
 for(unsigned i=0;i<GPU_RENDER_JOBS;i++)assert(!gpu_render_jobs[i].state);
 gpu_render_reclaim();
 struct drm_gem_object **r=malloc(sizeof(*r));r[0]=&fresh;
 struct virtio_gpu_ctrl_hdr req={.type=0x207,.ctx_id=8};
 assert(virtio_gpu_render_submit(&req,sizeof(req),r,1));
 assert(gpu_render_jobs[0].state==1&&fresh.virgl_pending_jobs==1);
}
int main(void){
 gpu_render_prepare();assert(gpu_render_capacity==16);
 unsigned start=acquisitions;
 for(unsigned i=0;i<10000;i++)gpu_render_reclaim();
 assert(acquisitions-start==10000);
 printf("PASS: empty cleanup uses 1 lock per pass (previously 16), 10000 passes\n");
 struct drm_device dev={0};struct drm_gem_object old[16];
 fresh=(struct drm_gem_object){.dev=&dev,.refs=1};
 struct virtio_gpu_ctrl_hdr req={.type=0x207,.ctx_id=7};
 for(unsigned i=0;i<16;i++){
  old[i]=(struct drm_gem_object){.dev=&dev,.refs=1};
  struct drm_gem_object **r=malloc(sizeof(*r));r[0]=&old[i];
  assert(virtio_gpu_render_submit_notify(&req,sizeof(req),r,1,reenter,NULL));
 }
 ready=nr;reverse=true;gpu_drain_completions();
 gpu_render_reclaim();
 assert(callbacks==16&&frees==16&&nr==1&&gpu_render_pending==1);
 assert(fresh.refs==1&&fresh.virgl_pending_jobs==1);
 assert(gpu_render_jobs[0].objects[0]==&fresh&&gpu_render_jobs[0].state==1);
 /* The outer cleanup must never clear the newly submitted slot metadata. */
 ready=nr;gpu_drain_completions();start=acquisitions;gpu_render_reclaim();
 assert(acquisitions-start==1&&frees==17&&!fresh.virgl_pending_jobs);
 puts("PASS: one-lock completed cleanup, callback reentry, 16-slot reuse and independent BO lifetimes");
 /* Mixed good/bad replies share a cleanup pass; errors retain their BO/DMA. */
 struct drm_gem_object good={.dev=&dev,.refs=1},bad={.dev=&dev,.refs=1};
 struct drm_gem_object **g=malloc(sizeof(*g)),**b=malloc(sizeof(*b));*g=&good;*b=&bad;
 assert(virtio_gpu_render_submit_notify(&req,sizeof(req),g,1,notify,(void*)1));
 assert(virtio_gpu_render_submit_notify(&req,sizeof(req),b,1,notify,(void*)2));
 reverse=false;ready=1;gpu_drain_completions();
 malformed=true;ready=1;gpu_drain_completions();
 start=acquisitions;gpu_render_reclaim();assert(acquisitions-start==1);
 assert(frees==18&&!good.refs&&bad.refs==1&&bad.virgl_pending_jobs==2);
 assert(gpu_render_jobs[1].state==4&&gpu_render_jobs[1].objects==b);
 assert(notified[1]==1&&!notification_error[1]&&notified[2]==1&&notification_error[2]==-5);
 gpu_render_reclaim();assert(notified[1]==1&&notified[2]==1&&bad.refs==1);
 puts("PASS: mixed successful/error replies, exactly-once callbacks and retained quarantine");
 /* Test teardown only: no real host can still access these mock resources. */
 free(b);
 for(unsigned i=0;i<16;i++)dma_free(gpu_render_jobs[i].dma);
}
'''
with tempfile.TemporaryDirectory(prefix='virgl-reclaim-') as tmp:
    path = Path(tmp)
    (path/'test.c').write_text(pre+jobs+reap+strings['helpers']+queue+main)
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(path/'test.c'), '-o', str(path/'test')], check=True)
    # LeakSanitizer cannot enumerate threads under the sandbox's ptrace layer.
    # Keep address/undefined checks enabled; BO reference counts are asserted.
    subprocess.run([str(path/'test')], check=True,
                   env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))
