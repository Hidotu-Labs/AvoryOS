#!/usr/bin/env python3
"""Exercise native PRIME imports/publication and verify QEMU's host image.

Requires a built ISO, the local musl toolchain, QEMU, xorriso, mke2fs and Pillow.
Uses a private minimal root disk and TCG VM; never opens disk.img or VFIO.
"""
from pathlib import Path
import argparse,json,os,re,socket,subprocess,tempfile,time
from PIL import Image
ROOT=Path(__file__).resolve().parents[2]
GUEST=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <uapi/kpi_dmabuf.h>
/* Native DRM userspace ABI declarations inserted from drm.h. */
@ABI@
#define W 1280
#define H 800
static int card;
static uint32_t crtc;
static uint32_t fbs[2];
static uint32_t *pixels[2];
static uint32_t pitches[2];
static void check(int condition,const char *what){if(!condition){perror(what);exit(1);}}
static uint32_t pattern(unsigned b,unsigned x,unsigned y){if(!y && x<8)return 0xabc000|(x<<4)|b;return ((x*3+b*43)&255)<<16 | ((y*5+b*71)&255)<<8 | ((x^y^b*113)&255);}
static void *stream(void *arg){
 unsigned b=(uintptr_t)arg;
 for(unsigned i=0;i<200;i++){
  struct drm_mode_crtc_page_flip flip={.crtc_id=crtc,.fb_id=fbs[b]};
  check(ioctl(card,0xC01864B0,&flip)==0,"PAGE_FLIP");
  struct drm_mode_fb_dirty_cmd dirty={.fb_id=fbs[b]};
  check(ioctl(card,0xC01864B1,&dirty)==0,"DIRTYFB");usleep(5000);
 }
 return NULL;
}
int main(void){
 int diag=open("/proc/gpu_debug",O_WRONLY);check(diag>=0,"open GPU recorder");
 check(write(diag,"2\n",2)==2,"arm GPU recorder");
 card=open("/dev/dri/card0",O_RDWR);check(card>=0,"open DRM");
 check(ioctl(card,0x641E,0)==0,"SET_MASTER");
 uint32_t crtcs[16]={0},connectors[16]={0};
 struct drm_mode_card_res res={.crtc_id_ptr=(uintptr_t)crtcs,.connector_id_ptr=(uintptr_t)connectors,.count_crtcs=16,.count_connectors=16};
 check(ioctl(card,0xC04064A0,&res)==0 && res.count_crtcs && res.count_connectors,"GETRESOURCES");crtc=crtcs[0];
 for(unsigned b=0;b<2;b++){
  uint32_t handle,offset=0;
  if(!b){
   pitches[b]=(W+8)*4;offset=64;size_t size=offset+(size_t)pitches[b]*H;
   int dev=open("/dev/kpi_dmabuf",O_RDWR);check(dev>=0,"open dma-buf device");
   int fd=ioctl(dev,KPI_DMABUF_IOC_ALLOC,(size+4095)/4096);check(fd>=0,"ALLOC");
   void *mapped=mmap(NULL,(size+4095)&~(size_t)4095,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);check(mapped!=MAP_FAILED,"PRIME mmap");
   pixels[b]=(uint32_t*)((unsigned char*)mapped+offset);
   struct drm_prime_handle import={.fd=fd};check(ioctl(card,0xC00C642E,&import)==0,"PRIME_FD_TO_HANDLE");handle=import.handle;
  }else{
   struct drm_mode_create_dumb create={.width=W,.height=H,.bpp=32};
   check(ioctl(card,0xC02064B2,&create)==0,"CREATE_DUMB");handle=create.handle;pitches[b]=create.pitch;
   struct drm_mode_map_dumb map={.handle=handle};check(ioctl(card,0xC01064B3,&map)==0,"MAP_DUMB");
   pixels[b]=mmap(NULL,create.size,PROT_READ|PROT_WRITE,MAP_SHARED,card,map.offset);check(pixels[b]!=MAP_FAILED,"native mmap");
  }
  for(unsigned y=0;y<H;y++)for(unsigned x=0;x<W;x++)((uint32_t*)((unsigned char*)pixels[b]+y*pitches[b]))[x]=pattern(b,x,y);
  if(!b){
    /* ARGB shares the XRGB byte layout; VirtIO ignores its alpha byte. */
    struct drm_mode_fb_cmd2 fb={.width=W,.height=H,.pixel_format=0x34325241};
   fb.handles[0]=handle;fb.pitches[0]=pitches[b];fb.offsets[0]=offset;
   check(ioctl(card,0xC06864B8,&fb)==0,"ADDFB2");fbs[b]=fb.fb_id;
  }else{
   struct drm_mode_fb_cmd fb={.width=W,.height=H,.pitch=pitches[b],.bpp=32,.depth=24,.handle=handle};
   check(ioctl(card,0xC01C64AE,&fb)==0,"ADDFB");fbs[b]=fb.fb_id;
  }
 }
 struct drm_mode_crtc mode={.crtc_id=crtc,.fb_id=fbs[0],.set_connectors_ptr=(uintptr_t)connectors,.count_connectors=1,.mode_valid=1};
 mode.mode.hdisplay=W;mode.mode.vdisplay=H;mode.mode.vrefresh=60;
 check(ioctl(card,0xC06864A2,&mode)==0,"SETCRTC");
 pthread_t threads[2];check(!pthread_create(&threads[0],NULL,stream,(void*)0),"thread 0");check(!pthread_create(&threads[1],NULL,stream,(void*)1),"thread 1");
 pthread_join(threads[0],NULL);pthread_join(threads[1],NULL);
 struct drm_mode_crtc_page_flip flip={.crtc_id=crtc,.fb_id=fbs[0]};check(ioctl(card,0xC01864B0,&flip)==0,"final PAGE_FLIP");
 for(unsigned y=0;y<H;y++)for(unsigned x=0;x<W;x++)((uint32_t*)((unsigned char*)pixels[0]+y*pitches[0]))[x]=pattern(2,x,y);
 struct drm_mode_fb_dirty_cmd dirty={.fb_id=fbs[0]};check(ioctl(card,0xC01864B1,&dirty)==0,"final DIRTYFB");
 check(write(diag,"dump\n",5)==5,"dump GPU recorder");close(diag);
 puts("PRIME_WIRE: 400 concurrent flips and 400 damage commits complete; holding final pattern");fflush(stdout);
 for(;;)sleep(1);
}
'''

def run(cmd,**kw):
    result=subprocess.run(cmd,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,**kw)
    if result.returncode:
        raise RuntimeError(f'{cmd[0]} failed: {result.stderr.decode(errors="replace")}')
    return result

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso',type=Path,default=ROOT/'avoryos-x86_64.iso')
    parser.add_argument('--timeout',type=int,default=90)
    parser.add_argument('--cpus',type=int,default=2)
    parser.add_argument('--accel',choices=['tcg','kvm'],default='tcg')
    args=parser.parse_args()
    if args.cpus < 1 or args.cpus > 64:parser.error('--cpus must be between 1 and 64')
    logs=ROOT/'build/logs';logs.mkdir(parents=True,exist_ok=True)
    serial=logs/'virtio-prime-wire.log';screen=logs/'virtio-prime-wire.ppm'
    h=(ROOT/'kernel/src/drivers/gpu/drm/drm.h').read_text()
    abi=h[h.index('struct drm_mode_card_res {'):h.index('#define DRM_MODE_PAGE_FLIP_EVENT')]
    a=h.index('struct drm_prime_handle {');abi+='\n'+h[a:h.index('/* ── Per-file',a)]
    a=h.index('struct drm_mode_create_dumb {');abi+='\n'+h[a:h.index('struct drm_version {',a)]
    a=h.index('struct drm_mode_fb_cmd2 {');abi+='\n'+h[a:h.index('/* ── GEM PRIME',a)]
    with tempfile.TemporaryDirectory(prefix='avory-prime-wire-') as tmp:
        p=Path(tmp);root=p/'root';root.mkdir()
        for name in ['bin','sbin','dev/dri','etc','proc','sys','run','tmp','usr/bin','usr/lib','lib/firmware','home']:(root/name).mkdir(parents=True,exist_ok=True)
        c=p/'probe.c';c.write_text(GUEST.replace('@ABI@',abi))
        compiler=ROOT/'toolchain/x86_64-linux-musl/bin/x86_64-linux-musl-gcc'
        run([str(compiler),'-static','-O2','-Wall','-Wextra','-Werror','-I'+str(ROOT/'kernel/linuxkpi/include'),str(c),'-o',str(root/'bin/prime-wire'),'-lpthread'])
        disk=p/'root.img'
        with disk.open('wb') as f:f.truncate(128*1024**2)
        run(['mke2fs','-q','-t','ext2','-F','-d',str(root),str(disk)])
        boot=p/'boot';run(['xorriso','-osirrox','on','-indev',str(args.iso),'-extract','/',str(boot)])
        for directory,_,files in os.walk(boot):
            os.chmod(directory,0o755)
            for file in files:os.chmod(Path(directory)/file,0o644)
        conf=boot/'boot/limine/limine.conf'
        conf.write_text('timeout: 0\n/PRIME wire test\n    protocol: limine\n    path: boot():/boot/kernel\n    cmdline: kpi_amdgpu=0 init=/bin/prime-wire\n    resolution: 1280x800\n')
        iso=p/'boot.iso'
        run(['xorriso','-as','mkisofs','-R','-r','-J','-b','boot/limine/limine-bios-cd.bin','-no-emul-boot','-boot-load-size','4','-boot-info-table',str(boot),'-o',str(iso)])
        run([str(ROOT/'limine/limine'),'bios-install',str(iso)])
        qmp=p/'qmp.sock';host=(logs/'virtio-prime-wire-host.log').open('wb')
        cpu_args=['-cpu','host'] if args.accel=='kvm' else []
        proc=subprocess.Popen(['qemu-system-x86_64','-M','q35','-accel',args.accel,*cpu_args,'-m','2G','-smp',str(args.cpus),'-cdrom',str(iso),'-boot','d','-vga','none','-device','virtio-vga,xres=1280,yres=800','-drive',f'file={disk},format=raw,if=none,id=probe','-device','nvme,drive=probe,serial=prime-wire','-display','none','-serial',f'file:{serial}','-monitor','none','-qmp',f'unix:{qmp},server=on,wait=off','-no-reboot'],stdout=host,stderr=host)
        conn=None
        try:
            deadline=time.monotonic()+args.timeout
            while not qmp.exists() and proc.poll() is None and time.monotonic()<deadline:time.sleep(.1)
            conn=socket.socket(socket.AF_UNIX);conn.connect(str(qmp));conn.settimeout(10)
            stream=conn.makefile('rwb',buffering=0)
            json.loads(stream.readline())
            def command(execute,arguments=None):
                stream.write((json.dumps({'execute':execute,**({'arguments':arguments} if arguments else {})})+'\n').encode())
                while True:
                    result=json.loads(stream.readline())
                    if 'return' in result:return result['return']
                    if 'error' in result:raise RuntimeError(result['error'])
            command('qmp_capabilities')
            expected={}
            for frame in range(3):
                expected[frame]=bytes(channel for y in range(800) for x in range(1280) for channel in ((0xab,0xc0,(x<<4)|frame) if not y and x<8 else (((x*3+frame*43)&255),((y*5+frame*71)&255),((x^y^frame*113)&255))))
            active_samples=0
            while proc.poll() is None and time.monotonic()<deadline:
                command('screendump',{'filename':str(screen)})
                with Image.open(screen) as im:
                    if im.size==(1280,800):
                        rgb=im.convert('RGB');marker=rgb.getpixel((0,0))
                        if marker[:2]==(0xab,0xc0) and marker[2] in expected:
                            frame=marker[2]
                            assert rgb.tobytes()==expected[frame], f'Corrupted host frame {frame}; inspect {screen}'
                            if frame<2:
                                active_samples+=1
                            else:
                                assert active_samples, 'No active presentation sample was captured'
                                log=serial.read_text(errors='replace')
                                if '[GPUDBG] frozen flight recorder end' not in log:
                                    time.sleep(.02)
                                    continue
                                assert 'stable_copy_mismatch=0' in log, 'GPU recorder found a copy mismatch'
                                assert 'cache_mismatch=0' in log, 'GPU recorder found a cache alias mismatch'
                                assert 'count COPY_CHECK=64' in log, 'Bounded native pixel checks did not run'
                                capture=log[log.rfind('[GPUDBG] frozen flight recorder begin'):]
                                samples=re.findall(r'^sample (\d+) .* COPY_CHECK .*$',capture,re.M)
                                assert len(samples)==64, 'Pixel checks lost after routine-event wrap'
                                total=int(re.search(r' events=(\d+)',capture)[1])
                                sequences=[int(s) for s in re.findall(r'^(\d+) \d+ \d+ [A-Z_]+ ',capture,re.M)]
                                assert sequences==list(range(max(1,total-2047),total+1)), 'Serial export lost or merged event rows'
                                lifetime=re.search(r'\[KPI-RCU\] lifetime test: reads=(\d+) retired=(\d+) premature=(\d+)',log)
                                assert lifetime and int(lifetime[1])>0 and lifetime.groups()[1:] == ('512','0'), 'Native RCU object-lifetime self-test failed'
                                im.save(logs/'virtio-prime-wire.png')
                                print(f'PASS: PRIME ADDFB2 pitch/offset + native dumb ADDFB; 400 concurrent flips + 400 damage commits; {active_samples} active host frames and final 1,024,000 pixels match')
                                return
                if serial.exists() and '[PANIC]' in serial.read_text(errors='replace'):
                    raise RuntimeError(f'Guest panicked; inspect {serial}')
                time.sleep(.02)
            raise RuntimeError(f'Guest did not publish the expected image; inspect {serial} and {screen}')
        finally:
            if conn:conn.close()
            proc.terminate()
            try:proc.wait(timeout=5)
            except subprocess.TimeoutExpired:proc.kill();proc.wait()
            host.close()
if __name__=='__main__':main()
