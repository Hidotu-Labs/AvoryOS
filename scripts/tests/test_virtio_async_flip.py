#!/usr/bin/env python3
"""Execute actual async flip submission/worker with close and RMFB races."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/src/drivers/gpu/virtio_gpu/virtio_gpu.c').read_text()
code = source[source.index('static int virtio_gpu_submit_flip('):source.index('static uint32_t gpu_append_dec(')]
pre = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#define VIRTIO_GPU_MAX_SCANOUTS 16
#define THREAD_BLOCKED 1
#define THREAD_RUNNING 2
#define DRM_EVENT_FLIP_COMPLETE 2
struct drm_device { int unused; };
struct drm_gem_object { void *virt_addr; unsigned size; int refs; };
struct drm_framebuffer { struct drm_gem_object *gem_obj; unsigned backing_offset,width,height,pitch,bpp; };
struct drm_file { struct drm_device *dev; };
struct vfs_node { int refs; };
struct vgpu_scanout_source { void *pixels; unsigned size,offset,width,height,pitch,view_width,view_height; };
struct vgpu_scanout_image { int unused; };
struct drm_event_vblank { struct { unsigned type,length; } base; uint64_t user_data; unsigned sequence,tv_sec,tv_usec,crtc_id; };
struct thread { int state; unsigned wakeup_ticks; };
typedef struct { struct thread *thread; } wait_queue_entry_t;
static struct { unsigned num_scanouts; } gpu;
static unsigned gpu_async_flip_state;
static bool gpu_flip_worker_started;
static int gpu_flip_wait,scanout_heads[16],scanout_ops;
static struct { struct drm_framebuffer framebuffer; struct drm_file *file; struct vfs_node *node;
 unsigned crtc,head; uint64_t cookie; bool event; } gpu_async_flip;
static struct thread thread;
static struct vgpu_scanout_image image;
static int cpu,events,begins,fail_publish,closed;
static jmp_buf finished;
static unsigned ascentdrm_crtc_scanout_id(unsigned crtc) { assert(crtc==7); return 0; }
static bool vgpu_scanout_source_valid(const struct vgpu_scanout_source *s) { return s->width && s->height; }
static void ascentdrm_gem_get(struct drm_gem_object *o) { assert(o->refs>0); o->refs++; }
static void vfs_node_ref(struct vfs_node *n) { assert(n->refs>0); n->refs++; }
static void wait_queue_wake_all(int *q) { (void)q; }
static void wait_queue_add(int *q,wait_queue_entry_t *e) { (void)q;(void)e; }
static void wait_queue_remove(int *q,wait_queue_entry_t *e) { (void)q;(void)e; }
static struct thread *sched_get_current(void) { return &thread; }
static void sched_yield(void) { assert(!gpu_async_flip_state); longjmp(finished,1); }
static void ascentdrm_console_takeover(void) { assert(!cpu); }
static void gpu_scanout_acquire(void) { assert(!cpu); }
static void gpu_scanout_release(void) { assert(!cpu); }
static struct vgpu_scanout_image *vgpu_scanout_prepare(int *h,int *ops,unsigned head,unsigned w,unsigned height) {
 (void)h;(void)ops;assert(!head && w==4 && height==4);return &image; }
static int ascentdrm_gem_begin_cpu(struct drm_gem_object *o) { assert(o->refs>0 && !cpu && !closed);cpu=1;begins++;return 0; }
static bool vgpu_scanout_copy(struct vgpu_scanout_image *i,const struct vgpu_scanout_source *s) { assert(i==&image && s->width==4 && cpu);return true; }
static void ascentdrm_gem_end_cpu(struct drm_gem_object *o) { (void)o;assert(cpu);cpu=0; }
static bool vgpu_scanout_publish(int *h,int *ops,unsigned head,struct vgpu_scanout_image *i) {
 (void)h;(void)ops;(void)head;assert(i==&image && !cpu);return !fail_publish; }
static void vblank_wait_next(void) { assert(!cpu); }
static unsigned vblank_sequence(void) { return 42; }
static void vblank_timestamp(unsigned seq,unsigned *sec,unsigned *usec) { assert(seq==42);*sec=3;*usec=456; }
static void ascentdrm_file_send_event(struct drm_file *f,struct drm_event_vblank *e,struct vfs_node *n) {
 (void)f;assert(n->refs==1 && !closed && e->user_data==123 && e->sequence==42 && e->crtc_id==7);events++; }
static bool ascentdrm_gem_put_last(struct drm_gem_object *o) { return --o->refs==0; }
static void ascentdrm_gem_object_free(struct drm_device *d,struct drm_gem_object *o) { (void)d;assert(!o->refs); }
static void vfs_close(struct vfs_node *n) { assert(n->refs>0);if (!--n->refs)closed++; }
static void klog_puts(const char *s) { (void)s;assert(fail_publish); }
'''
post = r'''
int main(void) {
 static int scenario;
 for (scenario=0;scenario<3;scenario++) {
  static struct drm_device dev;static struct drm_file file;static struct vfs_node node;
  static struct drm_gem_object object;static struct drm_framebuffer framebuffer;
  file=(struct drm_file){.dev=&dev};node=(struct vfs_node){.refs=1};
  object=(struct drm_gem_object){.size=64,.refs=1};
  framebuffer=(struct drm_framebuffer){.gem_obj=&object,.width=4,.height=4,.pitch=16,.bpp=32};
  gpu.num_scanouts=1;gpu_flip_worker_started=true;gpu_async_flip_state=0;
  cpu=events=begins=closed=0;fail_publish=scenario==2;
  assert(!virtio_gpu_submit_flip(&file,&node,7,&framebuffer,123,scenario!=1));
  assert(!begins && !events && object.refs==2 && node.refs==2);
  assert(virtio_gpu_submit_flip(&file,&node,7,&framebuffer,456,true)==-16);
  assert(object.refs==2 && node.refs==2);
  vfs_close(&node);assert(node.refs==1 && !closed);
  object.refs--;memset(&framebuffer,0,sizeof framebuffer);
  if (!setjmp(finished)) virtio_gpu_flip_worker();
  assert(events==(scenario==0) && begins==1 && !object.refs && closed==1 && !gpu_async_flip_state);
 }
 puts("PASS: actual async queue returns before fence work, EBUSY retains no refs, snapshots survive RMFB/fd close, event lifetime is retained, failed publication emits no success");
}
'''
with tempfile.TemporaryDirectory(prefix='virtio-async-flip-') as temp:
    directory = Path(temp)
    (directory/'test.c').write_text(pre+code+post)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(directory/'test.c'),'-o',str(directory/'test')],check=True)
    subprocess.run([str(directory/'test')],check=True)
