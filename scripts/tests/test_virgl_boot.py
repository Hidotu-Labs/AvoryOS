#!/usr/bin/env python3
"""Exercise real virglrenderer in a private VM; never opens desktop disk.img.

Uses SDL's offscreen EGL display, so the default host renderer may be software.
This verifies transport/ABI correctness, not host hardware acceleration.
"""
import os
from boot_probe import run_probe

INIT = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <unistd.h>
#include <poll.h>
struct param {uint64_t param,value;};
struct caps {uint32_t id,version;uint64_t addr;uint32_t size,pad;};
struct create {uint32_t target,format,bind,width,height,depth,array_size,last_level,nr_samples,flags,bo,res,size,stride;};
struct map {uint64_t offset;uint32_t handle,pad;};
struct info {uint32_t bo,res,size,blob;};
struct transfer {uint32_t bo,x,y,z,w,h,d,level,offset,stride,layer_stride;};
struct exec {uint32_t flags,size;uint64_t command,handles;uint32_t count;int32_t fence;};
struct prime {uint32_t handle,flags;int32_t fd;};
static int failures;
#define CHECK(x) do {if (!(x)) {printf("VIRGL_BOOT: FAIL line=%d errno=%d\n",__LINE__,errno);failures++;goto done;}}while(0)
int main(void) {
 setvbuf(stdout,NULL,_IOLBF,0);
 puts("VIRGL_BOOT: START");
 int fd=open("/dev/dri/card0",O_RDWR), render=-1, exported=-1, fence=-1;
 unsigned char *pixels=MAP_FAILED; void *large_commands=MAP_FAILED; uint32_t bo=0, imported=0;
 CHECK(fd>=0);
 uint64_t value=0;
 struct param param={1,(uintptr_t)&value};
 CHECK(ioctl(fd,_IOWR('d',0x43,struct param),&param)==0 && value==1);
 unsigned char cap[256];struct caps caps={1,0,(uintptr_t)cap,sizeof(cap),0};
 CHECK(ioctl(fd,_IOWR('d',0x49,struct caps),&caps)==0);
 for(unsigned i=128;i<192;i++) {char path[64];snprintf(path,sizeof(path),"/dev/dri/renderD%u",i);render=open(path,O_RDWR);if(render>=0) {
  uint64_t supported=0;struct param probe={1,(uintptr_t)&supported};
  if(!ioctl(render,_IOWR('d',0x43,struct param),&probe) && supported)break;
  close(render);render=-1;
 }}
 CHECK(render>=0);
 value=0;CHECK(ioctl(render,_IOWR('d',0x43,struct param),&param)==0 && value==1);
 struct create cr={.target=2,.format=2,.bind=2|8,.width=64,.height=64,.depth=1,.array_size=1,.size=16384,.stride=256,.flags=1};
 CHECK(ioctl(render,_IOWR('d',0x44,struct create),&cr)==0 && cr.bo && cr.res);bo=cr.bo;
 struct map map={.handle=bo};
 CHECK(ioctl(render,_IOWR('d',0x41,struct map),&map)==0);
 pixels=mmap(NULL,16384,PROT_READ|PROT_WRITE,MAP_SHARED,render,map.offset);
 CHECK(pixels!=MAP_FAILED);
 for(unsigned i=0;i<16384;i++)pixels[i]=(i*7)^0x53;
 struct transfer tr={.bo=bo,.w=64,.h=64,.d=1,.stride=256};
 CHECK(ioctl(render,_IOWR('d',0x47,struct transfer),&tr)==0);
 memset(pixels,0,16384);
 CHECK(ioctl(render,_IOWR('d',0x46,struct transfer),&tr)==0);
 /* XRGB's unused alpha byte may be canonicalized by host GL. */
 for(unsigned i=0;i<16384;i++)if((i&3)!=3)CHECK(pixels[i]==(unsigned char)((i*7)^0x53));
 uint32_t nop=0;struct exec ex={.flags=2,.size=4,.command=(uintptr_t)&nop,.handles=(uintptr_t)&bo,.count=1,.fence=-1};
 CHECK(ioctl(render,_IOWR('d',0x42,struct exec),&ex)==0 && ex.fence>=0);fence=ex.fence;
 struct pollfd sync_poll={.fd=fence,.events=POLLIN};
 CHECK(poll(&sync_poll,1,5000)==1 && (sync_poll.revents&POLLIN) && !(sync_poll.revents&(POLLERR|POLLNVAL)));
 struct map foreign={.handle=bo};errno=0;
 CHECK(ioctl(fd,_IOWR('d',0x41,struct map),&foreign)==-1 && errno==ENOENT);
 CHECK(ioctl(render,_IO('d',0x1e),0)==-1 && errno==EACCES);
 struct {uint32_t handle,flags;} wait={bo,1};
 CHECK(ioctl(render,_IOWR('d',0x48,wait),&wait)==0);
 ex.flags=1;CHECK(ioctl(render,_IOWR('d',0x42,struct exec),&ex)==0);
 /* Shader/scene batches can reach the maximum command size after the
  * desktop has already fragmented physical memory. Exercise several full
  * packets through the real host, not just tiny bootstrap submissions. */
 large_commands=mmap(NULL,1024*1024,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 CHECK(large_commands!=MAP_FAILED);
 struct exec big={.size=1024*1024,.command=(uintptr_t)large_commands,.handles=(uintptr_t)&bo,.count=1};
 for(unsigned i=0;i<4;i++)CHECK(ioctl(render,_IOWR('d',0x42,struct exec),&big)==0);
 wait.flags=0;CHECK(ioctl(render,_IOWR('d',0x48,wait),&wait)==0);
 puts("VIRGL_BOOT: maximum-size queued command packets passed");
 struct prime prime={.handle=bo,.flags=O_CLOEXEC|O_RDWR,.fd=-1};
 CHECK(ioctl(render,_IOWR('d',0x2d,struct prime),&prime)==0);exported=prime.fd;
 prime.handle=0;CHECK(ioctl(fd,_IOWR('d',0x2e,struct prime),&prime)==0);imported=prime.handle;
 struct info info={.bo=imported};
 CHECK(ioctl(fd,_IOWR('d',0x45,struct info),&info)==0 && info.res==cr.res);
 param.value=0;errno=0;CHECK(ioctl(render,_IOWR('d',0x43,struct param),&param)==-1 && errno==EFAULT);
 struct map invalid={.handle=bo+1};CHECK(ioctl(render,_IOWR('d',0x41,struct map),&invalid)==-1);
 /* Keep more than the old 4095-handle ceiling live, then let final close
  * release the complete table. This also exercises bounded-stack cleanup. */
 uint32_t retained_last=0;
 for(unsigned i=0;i<6000;i++){
  struct create retained={.target=2,.format=2,.bind=2|8,.width=16,.height=16,.depth=1,.array_size=1,.size=4096,.stride=64,.flags=1};
  CHECK(ioctl(render,_IOWR('d',0x44,struct create),&retained)==0);
  retained_last=retained.bo;
 }
 CHECK(retained_last>=6000);
 struct map high_map={.handle=retained_last};
 CHECK(ioctl(render,_IOWR('d',0x41,struct map),&high_map)==0);
 struct info high_info={.bo=retained_last};
 CHECK(ioctl(render,_IOWR('d',0x45,struct info),&high_info)==0&&high_info.res);
 void *high_pixels=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,render,high_map.offset);
 CHECK(high_pixels!=MAP_FAILED);memset(high_pixels,0x5a,4096);munmap(high_pixels,4096);
 puts("VIRGL_BOOT: 6000 simultaneously live resources passed");
 puts("VIRGL_BOOT: capsets, render node, texture roundtrip, submit, sync-file and PRIME passed");
done:
 if(large_commands!=MAP_FAILED)munmap(large_commands,1024*1024);
 if(pixels!=MAP_FAILED)munmap(pixels,16384);
 if(fence>=0)close(fence);if(exported>=0)close(exported);
 if(imported)ioctl(fd,_IOW('d',9,uint64_t),&(uint64_t){imported});
 if(bo)ioctl(render,_IOW('d',9,uint64_t),&(uint64_t){bo});
 if(render>=0)close(render);if(fd>=0)close(fd);
 printf("VIRGL_BOOT: %s failures=%d\n",failures?"FAIL":"PASS",failures);
 fflush(stdout);sync();reboot(RB_POWER_OFF);for(;;)pause();
}
'''
if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--2d', dest='software', action='store_true', help='check fallback without virgl')
    args = parser.parse_args()
    if args.software:
        init = INIT.replace('VIRGL_BOOT:', 'VIRGL_2D_BOOT:').replace('value==1', 'value==0')
        begin, end = init.index(' unsigned char cap'), init.index('done:')
        init = init[:begin] + '''
 struct create cr={.target=2,.format=2,.width=64,.height=64,.depth=1,.array_size=1,.size=16384};
 CHECK(ioctl(fd,_IOWR('d',0x44,struct create),&cr)==-1 && errno==EOPNOTSUPP);
''' + init[end:]
        run_probe('virgl-2d-boot', init, 'VIRGL_2D_BOOT: PASS',
                  accel=os.environ.get('AVORY_TEST_ACCEL', 'kvm'), timeout=180)
    else:
        run_probe('virgl-boot', INIT, 'VIRGL_BOOT: PASS', timeout=180,
                  accel=os.environ.get('AVORY_TEST_ACCEL', 'kvm'),
                  gpu='virtio-vga-gl', display='sdl,gl=on',
                  qemu_env=dict(os.environ, SDL_VIDEODRIVER='offscreen'))
