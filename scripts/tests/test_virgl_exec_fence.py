#!/usr/bin/env python3
"""Exercise actual EXEC ownership with pending, early and failed output fences."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/src/drivers/gpu/virtio_gpu/virtio_gpu_virgl.c').read_text()
abi=s[s.index('struct vg_exec {'):s.index('_Static_assert(sizeof(struct vg_create)')]
exe=s[s.index('static int exec('):s.index('static int ioctl_locked(')]
pre=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define LINUXKPI_LINUX_IMPORTED 1
#define DRM_MAX_HANDLES_PER_FILE 4096
#define MAX_SUBMIT (1024*1024)
#define VIRTIO_GPU_CMD_SUBMIT_3D 0x207
struct drm_gem_object{int refs;};struct drm_file{uint32_t virgl_context;int lock;struct drm_gem_object *handles[4096];};struct virgl_resource{int unused;};
struct virtio_gpu_ctrl_hdr{uint32_t type,flags;uint64_t fence;uint32_t context,padding;};
struct virtio_gpu_cmd_submit{struct virtio_gpu_ctrl_hdr hdr;uint32_t size,padding;};
static struct drm_gem_object bo={1};static struct virgl_resource res;
static unsigned allocations,freed_fences,submits,sync_commands;static bool queued=true,submit_fail,create_fail,export_fail,early;
struct fence{unsigned refs;bool done;};static struct fence *fd_fence;
static void (*pending_notify)(void *,int);static void *pending_data;static struct drm_gem_object **pending_objects;static uint32_t pending_count;
static bool user(uint64_t p,size_t n){return p&&n;}
static int context(struct drm_file*f){f->virgl_context=7;return 0;}
static struct virtio_gpu_ctrl_hdr header(uint32_t t,uint32_t c){return (struct virtio_gpu_ctrl_hdr){.type=t,.context=c};}
static void *kmalloc(size_t n){allocations++;return malloc(n);}static void kfree(void*p){free(p);}
static unsigned acquisitions,locked_gets,max_locked_gets;static bool in_file_lock;
static void spinlock_acquire(int *p){assert(!*p&&!in_file_lock);*p=1;in_file_lock=true;locked_gets=0;acquisitions++;}
static void spinlock_release(int*p){assert(*p&&in_file_lock);*p=0;in_file_lock=false;if(locked_gets>max_locked_gets)max_locked_gets=locked_gets;}
static void ascentdrm_gem_get(struct drm_gem_object*o){assert(o==&bo&&in_file_lock);locked_gets++;o->refs++;}
static struct virgl_resource *resource(struct drm_gem_object*o){return o?&res:NULL;}
static void put(struct drm_gem_object*o){assert(o==&bo&&o->refs>1);o->refs--;}
static int attach(struct drm_file*f,struct virgl_resource*r){assert(!f->lock);(void)r;return 0;}
static int virtio_gpu_render_wait(struct drm_gem_object**o,uint32_t n,uint32_t c){(void)o;(void)n;(void)c;return 0;}
static bool virtio_gpu_render_queued(void){return queued;}
static void *linuxkpi_virgl_fence_create(void){if(create_fail)return NULL;struct fence*f=calloc(1,sizeof(*f));f->refs=1;return f;}
static void linuxkpi_virgl_fence_get(void*p){((struct fence*)p)->refs++;}
static void linuxkpi_virgl_fence_put(void*p){struct fence*f=p;assert(f->refs);if(!--f->refs){freed_fences++;free(f);}}
static void linuxkpi_virgl_fence_complete(void*p,int err){assert(!err);((struct fence*)p)->done=true;linuxkpi_virgl_fence_put(p);}
static int linuxkpi_virgl_fence_export_pending(void*p){assert(!fd_fence);if(export_fail)return -24;fd_fence=p;linuxkpi_virgl_fence_get(p);return 42;}
static int linuxkpi_virgl_fence_export(void){assert(sync_commands);return 43;}
static int linuxkpi_virgl_fence_wait(int fd){assert(fd==42&&fd_fence&&fd_fence->done);return 0;}
static int command(const void*p,uint32_t n){(void)p;(void)n;sync_commands++;return 0;}
static bool virtio_gpu_render_submit_parts(const void*h,uint32_t hs,const void*p,uint32_t ps,struct drm_gem_object**o,uint32_t n,void(*cb)(void*,int),void*d){
 const struct virtio_gpu_cmd_submit *packet=h;assert(!in_file_lock);assert(hs==sizeof(*packet)&&ps==packet->size&&p&&packet->hdr.context==7);
 if(submit_fail)return false;
 submits++;pending_notify=cb;pending_data=d;pending_objects=o;pending_count=n;
 if(early&&cb){pending_notify=NULL;cb(d,0);}return true;
}
static void complete(void){if(pending_notify){pending_notify(pending_data,0);pending_notify=NULL;}for(unsigned i=0;i<pending_count;i++)put(pending_objects[i]);free(pending_objects);pending_objects=NULL;pending_count=0;}
static void close_fence(void){linuxkpi_virgl_fence_put(fd_fence);fd_fence=NULL;}
'''
main=r'''
int main(void){
 struct drm_file f={.handles={[1]=&bo}};uint32_t nop=0,handle=1;
 struct vg_exec p={.flags=2,.size=4,.command=(uintptr_t)&nop,.handles=(uintptr_t)&handle,.count=1,.fence_fd=-1};
 assert(!exec(&f,&p,32)&&p.fence_fd==42&&fd_fence&&!fd_fence->done&&fd_fence->refs==2&&bo.refs==2);
 assert(allocations==1&&!sync_commands); /* BO snapshot only, no command allocation. */
 complete();assert(fd_fence->done&&fd_fence->refs==1&&bo.refs==1);close_fence();assert(freed_fences==1);
 /* Closing the fd before completion leaves the job's reference alive. */
 assert(!exec(&f,&p,32));close_fence();assert(freed_fences==1);complete();assert(freed_fences==2);
 /* Host can finish before fd installation; local reference protects export. */
 early=true;assert(!exec(&f,&p,32)&&fd_fence->done);complete();close_fence();early=false;assert(freed_fences==3);
 submit_fail=true;assert(exec(&f,&p,32)==-5&&bo.refs==1&&!fd_fence&&freed_fences==4);submit_fail=false;
 create_fail=true;assert(exec(&f,&p,32)==-12&&bo.refs==1&&!fd_fence);create_fail=false;
 export_fail=true;assert(exec(&f,&p,32)==-24&&bo.refs==2&&!fd_fence);complete();assert(freed_fences==5&&bo.refs==1);export_fail=false;
 queued=false;assert(!exec(&f,&p,32)&&p.fence_fd==43&&sync_commands==1&&bo.refs==1);
 queued=true;p.flags=0;p.count=65;uint32_t many[65];for(unsigned i=0;i<65;i++)many[i]=1;p.handles=(uintptr_t)many;
 unsigned before=acquisitions;assert(!exec(&f,&p,32)&&acquisitions-before==3&&bo.refs==66);
 complete();assert(bo.refs==1&&max_locked_gets==32);
 unsigned submitted=submits;
 many[32]=0;assert(exec(&f,&p,32)==-22&&bo.refs==1&&submits==submitted);
 many[32]=4096;assert(exec(&f,&p,32)==-22&&bo.refs==1&&submits==submitted);
 many[32]=2;assert(exec(&f,&p,32)==-22&&bo.refs==1&&submits==submitted);
 puts("PASS: 65 duplicate handles pinned in 3 bounded critical sections; invalid, foreign and missing handles release every reference");
 puts("PASS: pending sync-file export, no temporary command allocation, early completion, early fd close, submit/create/export failures and synchronous fallback");
}
'''
with tempfile.TemporaryDirectory(prefix='virgl-exec-fence-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(pre+abi+exe+main)
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
