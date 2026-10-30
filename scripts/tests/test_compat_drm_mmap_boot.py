#!/usr/bin/env python3
"""Map native GEM/virgl buffers through the real i386 mmap2 page-offset ABI."""
import os
import subprocess
from boot_probe import run_probe
from test_compat_stat_boot import INIT

CHILD = r'''
typedef unsigned int u32;
typedef unsigned long long u64;
static long call(u32 n,u32 a,u32 b,u32 c,u32 d,u32 e,u32 f){
 long r;register u32 sixth __asm__("ebp")=f;
 __asm__ volatile("int $0x80":"=a"(r):"a"(n),"b"(a),"c"(b),"d"(c),"S"(d),"D"(e),"r"(sixth):"memory");return r;
}
static void finish(int code){call(1,code,0,0,0,0,0);for(;;);}
#define CHECK(x) do{if(!(x))finish(__LINE__);}while(0)
struct map {u64 offset;u32 handle,pad;};
struct create {u32 target,format,bind,width,height,depth,array_size,last_level,nr_samples,flags,bo,res,size,stride;};
struct dumb {u32 height,width,bpp,flags,handle,pitch;u64 size;};
static void check_map(int fd,u32 request,struct map *map,u32 size){
 if(request==0xc01064b3){
  struct {u32 handle,pad;u64 offset;} dumb={map->handle,0,0};
  CHECK(call(54,fd,request,(u32)&dumb,0,0,0)==0);map->offset=dumb.offset;
 }else CHECK(call(54,fd,request,(u32)map,0,0,0)==0);
 CHECK(map->offset>0xffffffffULL && map->offset<0x100000000000ULL);
 CHECK(!(map->offset&4095));
 long p=call(192,0,size,3,1,fd,(u32)(map->offset>>12));
 CHECK((u32)p<0xc0000000U && (u32)p>=4096);
 volatile unsigned char *bytes=(void*)p;
 bytes[0]=0x37;bytes[size-1]=0xa5;CHECK(bytes[0]==0x37 && bytes[size-1]==0xa5);
 CHECK(call(91,p,size,0,0,0,0)==0);
}
void _start(void){
 int fd=call(5,(u32)"/dev/dri/card0",2,0,0,0,0);CHECK(fd>=0);
 struct dumb dumb={.width=64,.height=64,.bpp=32};
 CHECK(call(54,fd,0xc02064b2,(u32)&dumb,0,0,0)==0);
 struct map d={.handle=dumb.handle};check_map(fd,0xc01064b3,&d,(u32)dumb.size);
 struct create cr={.target=0,.format=64,.bind=16,.width=4096,.height=1,.depth=1,.array_size=1,.size=4096};
 CHECK(call(54,fd,0xc0386444,(u32)&cr,0,0,0)==0);
 struct map v={.handle=cr.bo};check_map(fd,0xc0106441,&v,4096);
 call(6,fd,0,0,0,0,0);finish(0);
}
'''

def setup(root):
    source = root.parent / 'compat-drm-mmap.c'
    source.write_text(CHILD)
    subprocess.run(['cc', '-m32', '-O1', '-fomit-frame-pointer', '-fno-builtin',
                    '-fno-pie', '-fno-stack-protector', '-nostdlib', '-static',
                    '-Wl,-e,_start', str(source), '-o', str(root / 'bin/compat-stat')], check=True)

if __name__ == '__main__':
    run_probe('compat-drm-mmap-boot', INIT.replace('COMPAT_STAT_BOOT', 'COMPAT_DRM_MMAP_BOOT'),
              'COMPAT_DRM_MMAP_BOOT: PASS', setup=setup, cpus=2, accel='tcg', timeout=90,
              gpu='virtio-vga-gl', display='sdl,gl=on',
              qemu_env=dict(os.environ, SDL_VIDEODRIVER='offscreen'))
