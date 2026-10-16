#!/usr/bin/env python3
"""Real alsa-lib plug/rate conversion against actual kernel PCM ioctl code.

No sound device is used. Interposed device syscalls connect the host library to
the kernel fixture. Requires the host libasound and its configured rate plugin.
"""
from pathlib import Path
import ctypes.util
import os
import subprocess
import tempfile

import test_alsa_playback as fixture

if not ctypes.util.find_library('asound'):
    raise SystemExit('SKIP: host libasound is unavailable')

bridge = r'''
#include <dlfcn.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
static int pcmfd=-1,ctlfd=-1;
int open(const char *path,int flags,...){
    if(strstr(path,"/dev/snd/pcm")){
        pcmfd=memfd_create("test-pcm",0);assert(pcmfd>=0);return pcmfd;
    }
    if(strstr(path,"/dev/snd/control")){
        ctlfd=memfd_create("test-ctl",0);assert(ctlfd>=0);return ctlfd;
    }
    int (*real)(const char*,int,...)=dlsym(RTLD_NEXT,"open");
    va_list ap;va_start(ap,flags);int mode=flags&O_CREAT?va_arg(ap,int):0;va_end(ap);
    return real(path,flags,mode);
}
int __open_2(const char *path,int flags){return open(path,flags);}
int ioctl(int fd,unsigned long req,...){
    va_list ap;va_start(ap,req);void *arg=va_arg(ap,void*);va_end(ap);
    if(fd==pcmfd){
        int r=alsa_pcm_ioctl_internal(NULL,(uint32_t)req,(uintptr_t)arg);
        if(r<0){errno=-r;return -1;}return r;
    }
    if(fd==ctlfd){
        if((req&0xffff)==0x5532)return 0;
        if((req&0xffff)==0x5500){*(int*)arg=0x20008;return 0;}
        if((req&0xffff)==0x5501){
            struct snd_ctl_card_info *i=arg;memset(i,0,sizeof(*i));
            strcpy((char*)i->id,"HDA");strcpy((char*)i->driver,"HDA-Intel");return 0;
        }
    }
    errno=ENOTTY;return -1;
}
void *mmap(void *addr,size_t len,int prot,int flags,int fd,int64_t offset){
    if(fd==pcmfd){
        void *p=calloc(1,len);assert(p);
        if(offset==0x80000000 || offset==0x82000000)alsa_status_page_virt=p;
        else if(offset==0x81000000 || offset==0x83000000)alsa_control_page_virt=p;
        else if(!offset)alsa_dma_buf_virt=p;
        else assert(0);
        return p;
    }
    void *(*real)(void*,size_t,int,int,int,int64_t)=dlsym(RTLD_NEXT,"mmap");
    return real(addr,len,prot,flags,fd,offset);
}
int main(int argc,char **argv){
    assert(argc==5);
    unsigned rate=atoi(argv[1]),channels=atoi(argv[2]);
    int format=atoi(argv[3]); /* ALSA S16_LE=2, FLOAT_LE=14 */
    void *lib=dlopen("libasound.so.2",RTLD_NOW|RTLD_GLOBAL);assert(lib);
    int (*pcm_open)(void**,const char*,int,int)=dlsym(lib,"snd_pcm_open");
    int (*set_params)(void*,int,int,unsigned,unsigned,int,unsigned)=dlsym(lib,"snd_pcm_set_params");
    long (*writei)(void*,const void*,unsigned long)=dlsym(lib,"snd_pcm_writei");
    void *pcm=NULL;
    assert(!pcm_open(&pcm,"test",0,0));
    unsigned latency=atoi(argv[4]);
    assert(!set_params(pcm,format,3,channels,rate,1,latency));
    unsigned frames=rate*10,bytes_per_sample=format==14?4:2;
    void *samples=calloc(frames*channels,bytes_per_sample);assert(samples);
    for(unsigned i=0;i<frames*channels;i++){
        if(format==14)((float*)samples)[i]=((int)(i%200)-100)/400.0f;
        else ((int16_t*)samples)[i]=((int)(i%200)-100)*100;
    }
    long written=0;
    while(written<frames){
        long n=writei(pcm,(uint8_t*)samples+written*channels*bytes_per_sample,frames-written);
        assert(n>0);written+=n;alsa_mmap_pump();
    }
    uint64_t output_frames=accepted/4;
    /* A converter may retain one final period. A 10-second input must not
     * lose the 1.47 seconds lost by the old 1024/4096 geometry override. */
    assert(output_frames<=480000 && output_frames>=480000-2400);
    assert(nonzero_samples>output_frames);
    printf("PASS real alsa-lib: %u Hz/%u channels/%s, %u us buffer -> %llu frames at 48 kHz (10 s input)\n",
           rate,channels,format==14?"float":"S16",latency,(unsigned long long)output_frames);
}
'''

prefix = fixture.prefix.replace('static uint64_t accepted;',
                                 'static uint64_t accepted,nonzero_samples;')
prefix = prefix.replace('return delay_bytes;', 'return accepted?2048:0;')
prefix = prefix.replace('sched_yield();accepted+=n;return n;', '''
    for(unsigned i=0;i<n/2;i++)if(((const int16_t*)p)[i])nonzero_samples++;
    sched_yield();accepted+=n;return n;''')
prefix = prefix.replace('(void)p;(void)r;(void)c;(void)b;return n;', '''
    assert(r==48000 && c==2 && b==16);
    for(unsigned i=0;i<n/2;i++)if(((const int16_t*)p)[i])nonzero_samples++;
    accepted+=n;return n;''')
source = prefix + fixture.definitions + '\n'.join(
    fixture.function(n) for n in fixture.names) + bridge
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path/'test.c').write_text(source)
    (path/'asound.conf').write_text('pcm.test { type plug slave.pcm { type hw card 0 device 0 } }\n')
    subprocess.run(['cc','-D_GNU_SOURCE','-std=gnu11','-O1','-g','-rdynamic',
                    str(path/'test.c'),'-pthread','-ldl','-o',str(path/'test')],check=True)
    cases = [(44100,2,2,100000),(48000,2,14,100000),
             (48000,1,14,100000),(48000,2,2,100000),
             (44100,2,2,40000),(48000,2,14,250000)]
    for rate,channels,fmt,latency in cases:
        subprocess.run([str(path/'test'),str(rate),str(channels),str(fmt),str(latency)],
                       env={**os.environ,'ALSA_CONFIG_PATH':str(path/'asound.conf')},
                       check=True,timeout=15)
