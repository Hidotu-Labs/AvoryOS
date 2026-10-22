#!/usr/bin/env python3
"""Run actual native KMS snapshot/commit with RMFB during its fence wait."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
v=(root/'kernel/src/drivers/gpu/virtio_gpu/virtio_gpu.c').read_text()
k=(root/'kernel/src/drivers/gpu/drm/drm_kms.c').read_text()
active=v[v.index('static struct drm_framebuffer *virtio_gpu_active_fb('):v.index('static void virtio_gpu_queue_flip(')]
commit=v[v.index('static void virtio_gpu_commit_damage('):v.index('static int virtio_gpu_submit_flip(')]
free_fb=k[k.index('void ascentdrm_framebuffer_free('):]
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#define VIRTIO_GPU_MAX_SCANOUTS 2
#define DRM_MODE_OBJECT_CRTC 1
#define DRM_MODE_OBJECT_PLANE 2
#define LOCKDIAG_SPOT_GPU_POLL 0
static int gpu_poll_lock;
#define LOCKDIAG_SPOT_LOCK(s,p) do{(void)(s);assert(!*(p));*(p)=1;}while(0)
#define LOCKDIAG_SPOT_UNLOCK(s,p) do{(void)(s);assert(*(p));*(p)=0;}while(0)
struct list_head {void *items[4];unsigned count;};
#define list_for_each_entry(p,h,m) for(unsigned idx=0;idx<(h)->count && ((p)=(h)->items[idx],1);idx++)
static void list_del(struct list_head *l){(void)l;}
static void spinlock_acquire(int *l){assert(!*l);*l=1;}
static void spinlock_release(int *l){assert(*l);*l=0;}
struct drm_device {int lock;struct list_head kms_objects;};
struct drm_mode_object {uint32_t id,type;struct list_head list;};
struct virtio_gpu_rect {uint32_t x,y,width,height;};
struct drm_clip_rect {uint16_t x1,y1,x2,y2;};
struct drm_gem_object {struct drm_device *dev;int refcount;void *virt_addr,*prime_buffer;size_t size;bool freed;};
struct drm_framebuffer {struct drm_mode_object base;uint32_t width,height,pitch,bpp,backing_offset;struct drm_gem_object *gem_obj;};
struct drm_plane {struct drm_mode_object base;struct drm_framebuffer *fb;uint32_t src_x,src_y,src_w,src_h;};
struct drm_crtc {struct drm_mode_object base;struct drm_framebuffer *fb;struct drm_plane *primary;uint32_t scanout_id;};
struct vgpu_scanout_source {const void *pixels;uint64_t size,offset;uint32_t width,height,pitch,x,y,view_width,view_height;};
struct vgpu_scanout_damage {uint32_t x,y,width,height;};
static bool observed_partial;static struct vgpu_scanout_damage observed_damage;
struct vgpu_scanout_head {int unused;};struct vgpu_scanout_ops {int unused;};struct vgpu_scanout_image {struct vgpu_scanout_damage upload;uint64_t compared_pixels;};
static struct vgpu_scanout_head scanout_heads[2];static struct vgpu_scanout_ops scanout_ops;static struct vgpu_scanout_image image;
static struct {unsigned num_scanouts;struct {uint64_t present_batches,present_wait_ms,max_present_wait_ms,frames_presented,damage_pixels,present_failures,scanout_pixels,compared_pixels,unchanged_frames,producer_sync_ns,snapshot_copy_ns,transport_wait_ns;}stats;}gpu;
static struct drm_framebuffer *remove_on_begin;
static struct drm_gem_object *live_gem;
static int begin_error,begins,ends,presents,flips,releases;
static uint64_t gpu_prime_enter_seq;static uint32_t gpu_prime_waiters;
static unsigned gpu_scanout_busy;static bool cpu_held,stalled_control;
static void gpu_scanout_acquire(void){assert(!gpu_scanout_busy);gpu_scanout_busy=1;}
static void gpu_scanout_release(void){assert(gpu_scanout_busy);gpu_scanout_busy=0;}
static bool gpu_control_ready(void){return !stalled_control;}
static uint64_t lapic_timer_get_ms(void){return 0;}
static uint64_t lapic_timer_get_ns(void){return 0;}
static unsigned char expected[64],staging[64];
static void ascentdrm_gem_get(struct drm_gem_object *g){assert(!g->freed && g->refcount>0 && g->dev->lock);g->refcount++;}
static bool ascentdrm_gem_put_last(struct drm_gem_object *g){assert(g->refcount>0);return --g->refcount==0;}
static void ascentdrm_gem_object_free(struct drm_device *d,struct drm_gem_object *g){assert(!d->lock && !gpu_scanout_busy && !g->refcount && !g->freed);g->freed=true;releases++;}
static void kfree(void *p){memset(p,0xdd,sizeof(struct drm_framebuffer));}
static void virtio_gpu_complete_flip(uint32_t h){(void)h;assert(!cpu_held && !gpu_poll_lock);flips++;}
void ascentdrm_framebuffer_free(struct drm_device *,struct drm_framebuffer *);
static int ascentdrm_gem_begin_cpu(struct drm_gem_object *g){
 begins++;assert(g==live_gem && !g->freed && g->refcount==2);
 if(remove_on_begin){struct drm_framebuffer *f=remove_on_begin;remove_on_begin=NULL;ascentdrm_framebuffer_free(g->dev,f);}
 assert(!g->freed && g->refcount>=1);cpu_held=!begin_error;return begin_error;
}
static void ascentdrm_gem_end_cpu(struct drm_gem_object *g){assert(!g->freed && cpu_held);cpu_held=false;ends++;}
static bool vgpu_scanout_source_valid(const struct vgpu_scanout_source *s){return s->offset+64<=s->size;}
static struct vgpu_scanout_image *vgpu_scanout_prepare(struct vgpu_scanout_head *h,const struct vgpu_scanout_ops *ops,uint32_t head,uint32_t w,uint32_t height){
 (void)h;(void)ops;assert(!head && (w==4 || w==2) && height==w && !cpu_held);return stalled_control?NULL:&image;
}
static bool vgpu_scanout_copy(struct vgpu_scanout_image *i,const struct vgpu_scanout_source *s){assert(i==&image && cpu_held && !live_gem->freed);memcpy(staging,(unsigned char*)s->pixels+s->offset,64);return true;}
static void vgpu_scanout_invalidate(struct vgpu_scanout_head *head){(void)head;}
static bool vgpu_scanout_copy_damage(struct vgpu_scanout_head *head,struct vgpu_scanout_image *i,const struct vgpu_scanout_source *s,uint64_t id,const struct vgpu_scanout_damage *damage){(void)head;assert(id==9);observed_partial=damage!=NULL;if(damage)observed_damage=*damage;i->upload=(struct vgpu_scanout_damage){0,0,s->view_width,s->view_height};return vgpu_scanout_copy(i,s);}
static bool vgpu_scanout_publish(struct vgpu_scanout_head *h,const struct vgpu_scanout_ops *ops,uint32_t head,struct vgpu_scanout_image *i){
 (void)h;(void)ops;(void)head;assert(i==&image && !cpu_held && !live_gem->freed && !memcmp(staging,expected,64));presents++;return true;
}
'''
post=r'''
int main(void){
 for(int scenario=0;scenario<11;scenario++){
  struct drm_device d={0};unsigned char pixels[80];for(unsigned i=0;i<80;i++)pixels[i]=i*3;
  unsigned offset=scenario==4?8:0;memcpy(expected,pixels+offset,64);memset(staging,0,64);
  struct drm_gem_object gem={.dev=&d,.refcount=1,.size=80,.virt_addr=pixels};live_gem=&gem;
  struct drm_framebuffer f={.base={.id=9},.width=4,.height=4,.pitch=16,.bpp=32,.backing_offset=offset,.gem_obj=&gem};
  struct drm_plane plane={.base={.type=DRM_MODE_OBJECT_PLANE},.fb=&f};
  struct drm_crtc crtc={.base={.type=DRM_MODE_OBJECT_CRTC,.id=3},.fb=&f,.primary=&plane};
  d.kms_objects.items[0]=&crtc;d.kms_objects.items[1]=&plane;d.kms_objects.count=2;
  begins=ends=presents=flips=releases=0;begin_error=0;cpu_held=stalled_control=false;remove_on_begin=NULL;memset(&gpu,0,sizeof gpu);gpu.num_scanouts=1;
  observed_partial=false;
  if(scenario==0)remove_on_begin=&f;
  else if(scenario==1){remove_on_begin=&f;begin_error=-5;}
  else if(scenario==2)crtc.scanout_id=2;
  else if(scenario==3)f.bpp=16;
  else if(scenario==6)crtc.fb=NULL;
  else if(scenario==7)stalled_control=true;
  struct drm_clip_rect empty={0},clips[2]={{1,1,2,3},{3,2,8,4}};
  if(scenario==9){plane.src_x=plane.src_y=1<<16;plane.src_w=plane.src_h=2<<16;clips[0]=(struct drm_clip_rect){0,0,2,2};}
  const struct drm_clip_rect *requested=scenario==5?&empty:scenario>=8?clips:NULL;
  unsigned count=scenario==5?1:scenario==8?2:scenario==9?1:scenario==10?4097:0;
  virtio_gpu_commit_damage(&d,requested,count,3);
  if(scenario==8)assert(observed_partial && observed_damage.x==1 && observed_damage.y==1 && observed_damage.width==3 && observed_damage.height==3);
  else if(scenario==9)assert(observed_partial && !observed_damage.x && !observed_damage.y && observed_damage.width==1 && observed_damage.height==1);
  else assert(!observed_partial);
  if(scenario==0)assert(presents==1 && ends==1 && gem.freed && releases==1 && !crtc.fb && !plane.fb);
  else if(scenario==1)assert(!presents && !ends && gem.freed && releases==1 && !crtc.fb && !plane.fb);
  else{assert(gem.refcount==1 && !gem.freed);ascentdrm_framebuffer_free(&d,&f);assert(gem.freed && releases==1);}
  if(scenario==7)assert(!begins && !presents && !memcmp(staging,(unsigned char[64]){0},64));
  assert(!d.lock && !gpu_scanout_busy && !cpu_held && !gpu_poll_lock);
 }
 puts("PASS: actual native commit retains GEM/FB snapshots across RMFB and fence waits, preserves offsets, releases CPU reservation before transport/vblank, and skips host-owned staging");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-prime-lifetime-') as tmp:
 p=Path(tmp);code=pre+free_fb+active+commit+post;(p/'test.c').write_text(code)
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
 for old in [code.replace('ascentdrm_gem_get(fb->gem_obj);',''),code.replace('crtc->fb = NULL;','(void)crtc;').replace('plane->fb = NULL;','(void)plane;')]:
  (p/'test.c').write_text(old);subprocess.run(['cc','-O2',str(p/'test.c'),'-o',str(p/'old')],check=True)
  assert subprocess.run([str(p/'old')],capture_output=True).returncode!=0
 print('PASS: missing source reference and dangling KMS pointers fail the snapshot-lifetime checks')
