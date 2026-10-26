#!/usr/bin/env python3
"""Exercise the actual host-texture scanout and retained-resource lifetime."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root/'kernel/src/drivers/gpu/virtio_gpu/virtio_gpu.c').read_text()
pins = source[source.index('struct virgl_scanout_ref {'):source.index('static const struct vgpu_scanout_ops scanout_ops;')]
present = source[source.index('static bool virtio_gpu_present_virgl('):source.index('static void virtio_gpu_commit_damage(')]
pre = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define VIRTIO_GPU_MAX_SCANOUTS 2
#define VIRTIO_GPU_CMD_SET_SCANOUT 0x103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH 0x104
#define LOCKDIAG_SPOT_GPU_POLL 0
static int gpu_poll_lock;
#define LOCKDIAG_SPOT_LOCK(s,p) do{(void)(s);assert(!*(p));*(p)=1;}while(0)
#define LOCKDIAG_SPOT_UNLOCK(s,p) do{(void)(s);assert(*(p));*(p)=0;}while(0)
struct drm_device {int unused;};
struct drm_gem_object {struct drm_device *dev;uint32_t id;int refs;};
struct drm_framebuffer {struct drm_gem_object *gem_obj;uint64_t backing_offset;};
struct virtio_gpu_rect {uint32_t x,y,width,height;};
struct virtio_gpu_set_scanout {uint32_t hdr;struct virtio_gpu_rect r;uint32_t scanout_id,resource_id;};
struct virtio_gpu_resource_flush {uint32_t hdr;struct virtio_gpu_rect r;uint32_t resource_id,padding;};
struct vgpu_scanout_head {uint32_t selected_id;bool selection_unknown;};
static struct vgpu_scanout_head scanout_heads[2];
static struct {struct {unsigned frames_presented,present_failures,present_batches;}stats;} gpu;
static unsigned selects,flushes,frees;static bool fail_select,fail_flush;
static void *kmalloc(size_t n){return malloc(n);}static void kfree(void*p){free(p);}
static int virtio_gpu_render_wait(struct drm_gem_object **objects,uint32_t count,uint32_t context){assert(objects&&count==1&&!context);return 0;}
static uint32_t virtio_gpu_virgl_resource_id(struct drm_gem_object*o){return o->id;}
static void ascentdrm_gem_get(struct drm_gem_object*o){assert(o->refs>0);o->refs++;}
static bool ascentdrm_gem_put_last(struct drm_gem_object*o){assert(o->refs>0);return !--o->refs;}
static void ascentdrm_gem_object_free(struct drm_device*d,struct drm_gem_object*o){(void)d;assert(!o->refs);frees++;}
static void init_hdr(uint32_t*h,uint32_t type){*h=type;}
static bool nodata(const void*data,unsigned size){const struct virtio_gpu_set_scanout*r=data;assert(size==sizeof(*r)&&r->hdr==0x103&&r->r.width==320&&r->r.height==240);selects++;return !fail_select;}
static bool flush_rect(unsigned id,unsigned x,unsigned y,unsigned w,unsigned h){assert(id&&x==7&&y==9&&w==320&&h==240);flushes++;return !fail_flush;}
static bool gpu_command_batch(const void *const *requests,const uint32_t *lengths,unsigned count){
 assert(count==2);const struct virtio_gpu_resource_flush *f=requests[1];
 assert(lengths[1]==sizeof(*f)&&f->hdr==VIRTIO_GPU_CMD_RESOURCE_FLUSH);
 bool selected=nodata(requests[0],lengths[0]);
 bool flushed=flush_rect(f->resource_id,f->r.x,f->r.y,f->r.width,f->r.height);
 return selected&&flushed;
}
static void klog_puts(const char*s){(void)s;}
'''
post = r'''
static void scanout_record_selection(uint32_t head,uint32_t id){virgl_scanout_replaced(head,id);}
'''
main = r'''
int main(void){
 struct drm_device dev={0};struct drm_gem_object a={&dev,1,1},b={&dev,2,1};
 struct drm_framebuffer fb={&a,0};struct virtio_gpu_rect view={7,9,320,240};
 assert(virtio_gpu_present_virgl(0,&fb,&view));assert(a.refs==2&&selects==1&&flushes==1&&gpu.stats.present_batches==1);
 assert(virtio_gpu_present_virgl(0,&fb,&view));assert(a.refs==2&&selects==1&&flushes==2);
 /* Userspace drops its handle while the resource is displayed. */
 assert(!ascentdrm_gem_put_last(&a));
 fb.gem_obj=&b;fail_select=true;
 assert(!virtio_gpu_present_virgl(0,&fb,&view));assert(a.refs==1&&b.refs==2&&frees==0);
 /* A timeout could still select b later: retain both until replacement. */
 fail_select=false;assert(virtio_gpu_present_virgl(0,&fb,&view));assert(!a.refs&&b.refs==2&&frees==1);
 fail_flush=true;assert(!virtio_gpu_present_virgl(0,&fb,&view));assert(b.refs==2);
 assert(!ascentdrm_gem_put_last(&b));
 /* Successful switch to a native 2D resource, or head disable, releases b. */
 virgl_scanout_replaced(0,99);assert(!b.refs&&frees==2&&!virgl_scanout_refs[0]);
 puts("PASS: host-texture presentation avoids readback, preserves crop, reuses selection, pins uncertain scanouts and releases resources after replacement");
}
'''
with tempfile.TemporaryDirectory(prefix='virgl-scanout-') as tmp:
    path=Path(tmp)
    (path/'test.c').write_text(pre+pins+post+present+main)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
