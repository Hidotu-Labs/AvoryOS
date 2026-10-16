#!/usr/bin/env python3
"""Actual ALSA ioctl/pump functions: honest constraints and one-copy mmap feed."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

source = Path(sys.argv[1] if len(sys.argv) > 1 else
              'kernel/src/drivers/audio/alsa_emu.c').read_text()

def function(name):
    match = re.search(r'(?m)^static (?:void|bool|int|uint\d+_t) ' + name + r'\([^;{]*\{', source)
    start = match.start()
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
typedef struct {pthread_mutex_t mutex;} spinlock_t;
#define SPINLOCK_INIT {PTHREAD_MUTEX_INITIALIZER}
static _Thread_local unsigned held;
static void spinlock_acquire(spinlock_t *l){assert(!held);pthread_mutex_lock(&l->mutex);held++;}
static void spinlock_release(spinlock_t *l){assert(held);held--;pthread_mutex_unlock(&l->mutex);}
struct vfs_node {uint32_t (*write)(struct vfs_node*,uint32_t,uint32_t,uint8_t*);};
typedef struct vfs_node vfs_node_t;
static vfs_node_t *fb_lookup_device(const char *n){(void)n;return NULL;}
static bool is_user_ptr(uint64_t p){return !!p;}
static bool vmm_is_user_addr_range_valid(uint64_t p,size_t n){(void)n;return !!p;}
static unsigned long copy_from_user(void *d,const void *s,unsigned long n){memcpy(d,s,n);return 0;}
#define klogf(...) ((void)0)
static bool hda_is_present(void){return true;}
static bool hda_get_pcm_caps(uint32_t *caps){*caps=0x000201fc;return true;}
static uint32_t delay_bytes;
static unsigned format_changes,resets;
static uint64_t accepted;
static uint32_t hda_get_delay_bytes(void){return delay_bytes;}
static void hda_set_format(uint32_t r,uint8_t c,uint8_t b){assert(r==48000 && c==2 && b==16);format_changes++;}
static void hda_set_queue_limit(uint32_t n){(void)n;}
static void hda_reset_stream(void){resets++;delay_bytes=0;}
static unsigned starts;
static void hda_start_playback(void){starts++;}
static void hda_set_autostart(bool on){(void)on;}
static void *hda_get_wait_queue(void){return NULL;}
void wait_queue_wake_all(void *q){(void)q;}
static uint32_t hda_write_pcm(const void *p,uint32_t n,uint32_t r,uint8_t c,uint8_t b){(void)p;(void)r;(void)c;(void)b;return n;}
uint64_t lapic_timer_get_ms(void){return 0;}
static uint32_t hda_queue_pcm(const void *p,uint32_t n,uint32_t r,uint8_t c,uint8_t b){
    assert(held && r==48000 && c==2 && b==16);(void)p;
    /* Competing query/IRQ pumps must not submit this range twice. */
    sched_yield();accepted+=n;return n;
}
'''
definitions = source[source.index('// ALSA Protocol Versions'):
                     source.index('// alsa-lib passes 0/1')]
names = ['alsa_ioctl_get_int', 'alsa_set_interval', 'alsa_refine_interval',
         'alsa_format_to_bits', 'alsa_interval_exact', 'alsa_frame_bytes',
         'alsa_supported_formats', 'alsa_publish_hw_ptr', 'alsa_avail_frames',
         'alsa_start_if_ready', 'alsa_mmap_pump_locked', 'alsa_mmap_pump', 'alsa_div_ceil', 'alsa_reconcile_hw_params',
         'alsa_pcm_ioctl_internal']
main = r'''
static void any(struct snd_pcm_hw_params *p){
    memset(p,0,sizeof(*p));
    for(unsigned m=0;m<3;m++)for(unsigned b=0;b<8;b++)p->masks[m].bits[b]=~0U;
    for(unsigned i=0;i<12;i++)p->intervals[i].max=~0U;
}
static int ioctl_params(unsigned request,struct snd_pcm_hw_params *p){return alsa_pcm_ioctl_internal(NULL,request,(uintptr_t)p);}
static void *pump(void *p){(void)p;for(unsigned i=0;i<2000;i++)alsa_mmap_pump();return NULL;}
int main(void){
    struct snd_pcm_hw_params p;
    any(&p);p.intervals[3].min=p.intervals[3].max=32000;
    assert(ioctl_params(SNDRV_PCM_IOCTL_HW_REFINE,&p)==-22);
    any(&p);p.intervals[3].min=p.intervals[3].max=44100;
    assert(ioctl_params(SNDRV_PCM_IOCTL_HW_PARAMS,&p)==-22 && !format_changes && !resets);
    any(&p);p.masks[1].bits[0]=1U<<14; /* FLOAT_LE has no direct HDA format */
    assert(ioctl_params(SNDRV_PCM_IOCTL_HW_REFINE,&p)==-22);
    any(&p);p.masks[0].bits[0]=1U<<1; /* noninterleaved unsupported */
    assert(ioctl_params(SNDRV_PCM_IOCTL_HW_PARAMS,&p)==-22 && !resets);
    any(&p);p.intervals[2].min=p.intervals[2].max=1;
    assert(ioctl_params(SNDRV_PCM_IOCTL_HW_REFINE,&p)==-22);
    any(&p);p.intervals[3].min=48000;p.intervals[3].openmin=1;
    assert(ioctl_params(SNDRV_PCM_IOCTL_HW_REFINE,&p)==-22);
    any(&p);p.intervals[3].empty=1;
    assert(ioctl_params(SNDRV_PCM_IOCTL_HW_REFINE,&p)==-22);
    any(&p);p.intervals[3].min=32000;p.intervals[3].max=96000;
    assert(!ioctl_params(SNDRV_PCM_IOCTL_HW_REFINE,&p));
    assert(p.intervals[3].min==48000 && p.intervals[3].max==48000);
    /* Fixing PERIODS alone must not pick a 1024-frame default period. */
    any(&p);p.intervals[7].min=p.intervals[7].max=4;
    assert(!ioctl_params(SNDRV_PCM_IOCTL_HW_REFINE,&p));
    assert(p.intervals[5].min<p.intervals[5].max);
    for(unsigned period=32;period<=20000;period+=19968){
        any(&p);p.masks[1].bits[0]=1U<<2;
        p.intervals[5].min=p.intervals[5].max=period;
        p.intervals[7].min=p.intervals[7].max=4;
        p.intervals[9].min=p.intervals[9].max=period*4;
        assert(!ioctl_params(SNDRV_PCM_IOCTL_HW_PARAMS,&p));
        assert(p.intervals[5].min==period && alsa_buffer_frames==period*4);
    }
    any(&p);p.intervals[5].min=p.intervals[5].max=1024;
    p.intervals[9].min=p.intervals[9].max=4097;
    assert(ioctl_params(SNDRV_PCM_IOCTL_HW_PARAMS,&p)==-22);
    format_changes=resets=0;
    any(&p);p.masks[0].bits[0]=1;p.masks[1].bits[0]=1U<<2;
    p.intervals[2].min=p.intervals[2].max=2;
    p.intervals[3].min=p.intervals[3].max=48000;
    p.intervals[5].min=p.intervals[5].max=1200;
    assert(!ioctl_params(SNDRV_PCM_IOCTL_HW_PARAMS,&p));
    assert(format_changes==1 && resets==1 && alsa_sample_rate==48000 && alsa_channels==2);
    assert(alsa_buffer_frames%1200==0 && alsa_buffer_frames*4<=ALSA_DMA_BUF_SIZE);
    static uint8_t data[ALSA_DMA_BUF_SIZE];
    struct snd_pcm_sw_params sw={.avail_min=1,.start_threshold=~0ULL};
    assert(!alsa_pcm_ioctl_internal(NULL,SNDRV_PCM_IOCTL_SW_PARAMS,(uintptr_t)&sw));
    assert(!alsa_pcm_ioctl_internal(NULL,SNDRV_PCM_IOCTL_PREPARE,0));
    bool old_mmap=alsa_access_mmap;alsa_access_mmap=false;
    struct snd_xferi x={.buf=data,.frames=32};starts=0;
    assert(!alsa_pcm_ioctl_internal(NULL,SNDRV_PCM_IOCTL_WRITEI_FRAMES,(uintptr_t)&x));
    assert(alsa_pcm_state==2 && !starts && alsa_appl_ptr==32);
    assert(!alsa_pcm_ioctl_internal(NULL,SNDRV_PCM_IOCTL_START,0));
    assert(alsa_pcm_state==3 && starts==1);
    assert(!alsa_pcm_ioctl_internal(NULL,SNDRV_PCM_IOCTL_PREPARE,0));
    sw.start_threshold=64;
    assert(!alsa_pcm_ioctl_internal(NULL,SNDRV_PCM_IOCTL_SW_PARAMS,(uintptr_t)&sw));
    starts=0;
    assert(!alsa_pcm_ioctl_internal(NULL,SNDRV_PCM_IOCTL_WRITEI_FRAMES,(uintptr_t)&x));
    assert(alsa_pcm_state==2 && !starts);
    assert(!alsa_pcm_ioctl_internal(NULL,SNDRV_PCM_IOCTL_WRITEI_FRAMES,(uintptr_t)&x));
    assert(alsa_pcm_state==3 && starts==1);
    alsa_access_mmap=old_mmap;
    static struct snd_pcm_mmap_control control;
    alsa_dma_buf_virt=data;alsa_control_page_virt=(void *)&control;
    alsa_buffer_frames=1024;alsa_appl_ptr=0;alsa_mmap_consumed=0;accepted=0;
    pthread_t threads[4];
    for(unsigned round=0;round<5000;round++){
        control.appl_ptr+=1024;
        alsa_mmap_pump();
    }
    assert(alsa_mmap_consumed==5120000 && accepted==5120000*4ULL);
    control.appl_ptr+=1024;
    for(unsigned i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,pump,NULL));
    for(unsigned i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
    assert(alsa_mmap_consumed==control.appl_ptr && accepted==control.appl_ptr*4);
    delay_bytes=512*4;alsa_publish_hw_ptr();
    assert(alsa_hw_ptr==control.appl_ptr-512 && alsa_avail_frames()==512);
    puts("PASS ALSA: incompatible rates/formats/access/channels rejected; valid 48 kHz stereo negotiation retained");
    puts("PASS ALSA: refinement retains ranges; exact small/large periods preserved; incompatible buffer geometry rejected");
    puts("PASS ALSA: >100 seconds of mmap ring wrap and four competing pumps submit each frame once; delay/avail stay correct");
}
'''
if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory)
        (path/'test.c').write_text(prefix + definitions + '\n'.join(function(n) for n in names) + main)
        subprocess.run(['cc','-std=gnu11','-O1','-g','-fsanitize=address,undefined',
                        '-pthread',str(path/'test.c'),'-o',str(path/'test')],check=True)
        subprocess.run([str(path/'test')],check=True,
                       env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
