#!/usr/bin/env python3
"""Compare actual resource slot masks with the preceding hash-table search."""
import ast
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
module=ast.parse((root/'scripts/tests/test_virgl_render_queue.py').read_text())
strings={n.targets[0].id:ast.literal_eval(n.value) for n in module.body
         if isinstance(n,ast.Assign) and isinstance(n.targets[0],ast.Name)
         and isinstance(n.value,ast.Constant) and isinstance(n.value.value,str)}
source=(root/'kernel/src/drivers/gpu/virtio_gpu/virtio_gpu.c').read_text()
jobs=source[source.index('#define GPU_RENDER_JOBS'):source.index('static uint64_t gpu_sync_command_pending;')]
reap=source[source.index('static void gpu_reap_control_locked('):source.index('static bool gpu_control_ready(')]
queue=source[source.index('static int gpu_render_wait_fence('):]
old=r'''
static uint32_t previous_hash(struct drm_gem_object *object,uint32_t mask){
 uint64_t key=(uintptr_t)object>>4;key^=key>>33;key*=0xff51afd7ed558ccdULL;key^=key>>33;return (uint32_t)key&mask;
}
static int former_lookup(struct drm_gem_object **objects,uint32_t count,uint32_t context,bool nowait){
 uint64_t target=0;uint32_t slots=32;
 while(slots<count*2U)slots<<=1;
 struct drm_gem_object **members=calloc(slots,sizeof(*members));assert(members);
 for(uint32_t n=0;n<count;n++){
  uint32_t slot=previous_hash(objects[n],slots-1);
  while(members[slot]&&members[slot]!=objects[n])slot=(slot+1)&(slots-1);
  members[slot]=objects[n];
 }
 LOCKDIAG_SPOT_LOCK(0,&gpu_poll_lock);gpu_reap_control_locked();
 for(uint32_t i=0;i<GPU_RENDER_JOBS;i++){
  struct gpu_render_job *job=&gpu_render_jobs[i];
  if(job->state!=1||(context&&job->context==context))continue;
  bool conflict=false;
  for(uint32_t m=0;m<job->count&&!conflict;m++){
   uint32_t slot=previous_hash(job->objects[m],slots-1);
   while(members[slot]&&members[slot]!=job->objects[m])slot=(slot+1)&(slots-1);
   conflict=members[slot]!=NULL;
  }
  if(conflict&&job->fence>target)target=job->fence;
 }
 bool failed=gpu_render_failed;LOCKDIAG_SPOT_UNLOCK(0,&gpu_poll_lock);free(members);
 if(failed)return -5;
 return target?gpu_render_wait_fence(target,nowait):0;
}
'''
main=r'''
#include <time.h>
int main(void){
 enum{N=1024,REPEATS=64};
 struct drm_gem_object *scene=calloc(2*N,sizeof(*scene));assert(scene);
 struct drm_gem_object **refs=malloc(N*sizeof(*refs)),**query=malloc(N*sizeof(*query));assert(refs&&query);
 for(unsigned i=0;i<N;i++){scene[i].virgl_pending_jobs=(1U<<GPU_RENDER_JOBS)-1;refs[i]=&scene[i];query[i]=&scene[N+i];}
 for(unsigned i=0;i<GPU_RENDER_JOBS;i++)gpu_render_jobs[i]=(struct gpu_render_job){.objects=refs,.count=N,.context=7,.state=1,.fence=i+1};
 double times[2];
 for(int mode=0;mode<2;mode++){
  clock_t start=clock();
  for(unsigned r=0;r<REPEATS;r++)assert(!(mode?gpu_render_wait_objects(query,N,8,true):former_lookup(query,N,8,true)));
  times[mode]=(double)(clock()-start)/CLOCKS_PER_SEC/REPEATS;
 }
 query[N-1]=refs[N-1];
 assert(former_lookup(query,N,8,true)==-16&&gpu_render_wait_objects(query,N,8,true)==-16);
 printf("LOOKUP: 16 pending batches x 1024 BOs, previous_hash=%.3f us slot_mask=%.3f us ratio=%.1fx\n",times[0]*1000000,times[1]*1000000,times[0]/times[1]);
 free(refs);free(query);free(scene);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='virgl-lookup-') as tmp:
    p=Path(tmp)
    (p/'test.c').write_text(strings['pre']+jobs+reap+strings['helpers']+queue+old+main)
    subprocess.run(['cc','-O2',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
