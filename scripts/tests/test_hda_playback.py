#!/usr/bin/env python3
"""Actual HDA queue/IRQ code: underrun reentry, IRQ state and SMP producers."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

source = Path(sys.argv[1] if len(sys.argv) > 1 else
              'kernel/src/drivers/audio/hda.c').read_text()

def extract(name):
    name = name.split('(')[0].split()[-1]
    start = re.search(r'(?m)^(?:static\s+)?(?:inline\s+)?(?:void|uint32_t|uint64_t)\s+' + re.escape(name) + r'\([^;{]*\{', source).start()
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
#define HDA_BDL_ENTRIES 16
#define HDA_BUFFER_SIZE 2048
#define HDA_HW_BYTES (HDA_BDL_ENTRIES*HDA_BUFFER_SIZE)
#define HDA_RING_SIZE (512*1024)
#define HDA_MAX_QUEUE_BYTES (128*1024)
#define HDA_DEFAULT_QUEUE_BYTES 16384
#define HDA_MIN_QUEUE_BYTES (HDA_HW_BYTES+HDA_BUFFER_SIZE)
#define HDA_SAFETY_MAX_SCALE 4
#define HDA_SD_CTL 0
#define HDA_SD_LPIB 1
#define HDA_SD_STS 2
#define HDA_REG_INTSTS 3
#define HDA_SD_CTL_RUN 2
#define HDA_SD_CTL_IOCE 4
#define HDA_SD_CTL_FEIE 8
#define HDA_SD_CTL_DEIE 16
#define HDA_SD_STS_BCIS 4
#define HDA_SD_STS_FIFOE 8
#define HDA_SD_STS_DESE 16
#define THREAD_RUNNING 1
#define THREAD_BLOCKED 2
#define SPINLOCK_RFLAGS_IF 512
typedef uint64_t hal_irq_state_t;
typedef struct {pthread_mutex_t mutex; hal_irq_state_t flags;} spinlock_t;
#define SPINLOCK_INIT {PTHREAD_MUTEX_INITIALIZER,0}
static _Thread_local bool irq_enabled=true, stream_held;
static hal_irq_state_t hal_irq_save(void){bool old=irq_enabled;irq_enabled=false;return old?512:0;}
static void hal_irq_restore(hal_irq_state_t f){irq_enabled=!!f;}
static void hal_irq_disable(void){irq_enabled=false;}
static void hal_irq_enable(void){irq_enabled=true;}
static void spinlock_acquire(spinlock_t *l){assert(!stream_held);hal_irq_state_t f=hal_irq_save();pthread_mutex_lock(&l->mutex);l->flags=f;stream_held=true;}
static void spinlock_release(spinlock_t *l){assert(stream_held);hal_irq_state_t f=l->flags;stream_held=false;pthread_mutex_unlock(&l->mutex);hal_irq_restore(f);}
static spinlock_t hda_stream_lock=SPINLOCK_INIT;
static bool hda_present=true,hda_is_playing;
static bool hda_autostart=true;
static uint32_t current_sample_rate=48000;
static uint8_t current_channels=2,current_bits=16;
static uint32_t ring_head,ring_tail,ring_count,hda_dma_pending,hda_underflow_count;
static uint32_t hda_last_lpib,total_played_blocks;
static uint64_t hda_lpib_pos,hda_refill_pos,total_played_bytes;
static uint8_t hda_playing_desc;
static uint16_t hda_slot_bytes[HDA_BDL_ENTRIES];
static uint8_t ring[HDA_RING_SIZE],dma[HDA_BDL_ENTRIES][HDA_BUFFER_SIZE];
static uint8_t *hda_ring=ring,*hda_buffers[HDA_BDL_ENTRIES];
static uint32_t hda_queue_limit=HDA_DEFAULT_QUEUE_BYTES,hda_queue_base=HDA_DEFAULT_QUEUE_BYTES,hda_safety_scale=1,hda_last_grow_ms;
static uint64_t hda_last_underrun_ms;
static void (*hda_tick_callback)(void);
static unsigned hda_wait_queue;
struct registers {int unused;};
static uint8_t ctl,status=HDA_SD_STS_BCIS;
static uint32_t lpib;
static uint8_t hda_sd_read8(unsigned r){return r==HDA_SD_STS?status:ctl;}
static void hda_sd_write8(unsigned r,uint8_t v){if(r==HDA_SD_CTL)ctl=v;}
static uint32_t hda_sd_read32(unsigned r){return r==HDA_SD_LPIB?lpib:ctl;}
static void hda_sd_write32(unsigned r,uint32_t v){(void)r;ctl=v;}
static uint32_t hda_read32(unsigned r){(void)r;return 1U<<31;}
static uint64_t lapic_timer_get_ticks(void){return 1000;}
static void wait_queue_wake_all(void *q){(void)q;assert(!stream_held);}
static void hda_apply_format(uint32_t r,uint8_t c,uint8_t b){assert(stream_held);(void)r;(void)c;(void)b;}
static void hda_start_stream(void);
uint32_t hda_queue_pcm(const void *,uint32_t,uint32_t,uint8_t,uint8_t);
uint32_t hda_get_free_bytes(void);
struct thread {int state;uint64_t wakeup_ticks;};
typedef struct {struct thread *thread;void *next,*thread_next,*thread_prev,*wq;} wait_queue_entry_t;
static struct thread thread={.state=THREAD_RUNNING};
static bool user,fail_copy;
static unsigned allocations,sleeps;
static uint8_t scratch_source[8192];
static void *kmalloc(size_t n){assert(!stream_held);void *p=malloc(n);if(p)allocations++;return p;}
static void kfree(void *p){assert(!stream_held);if(p)allocations--;free(p);}
static bool is_user_ptr(uint64_t p){return user && p>=(uintptr_t)scratch_source && p<(uintptr_t)scratch_source+sizeof(scratch_source);}
static unsigned long copy_from_user(void *d,const void *s,unsigned long n){assert(!stream_held && irq_enabled);if(fail_copy)return n;memcpy(d,s,n);return 0;}
static struct thread *sched_get_current(void){return &thread;}
static void wait_queue_add(void *q,wait_queue_entry_t *e){(void)q;assert(!irq_enabled && e->thread->wakeup_ticks);}
static void wait_queue_remove(void *q,wait_queue_entry_t *e){(void)q;(void)e;}
static bool thread_has_pending_signal(struct thread *t){(void)t;return false;}
#define sched_yield test_sched_yield
static void hda_isr(struct registers *);
static void sched_yield(void){assert(irq_enabled && !stream_held);assert(++sleeps<10);lpib=(lpib+2048)%HDA_HW_BYTES;irq_enabled=false;hda_isr(NULL);irq_enabled=true;}
'''
prefix=prefix.replace('#define HDA_BDL_ENTRIES 16',re.search(r'#define HDA_BDL_ENTRIES \d+',source).group(0))
names = ['hda_fill_dma_slot(', 'hda_refill_future_slots(', 'hda_apply_queue_limit(', 'hda_note_underrun(',
         'hda_safety_tick(', 'hda_start_stream(void) {', 'hda_service_descriptors(',
         'hda_isr(', 'hda_inflight_bytes(', 'hda_queue_pcm(',
         'hda_get_delay_bytes(', 'hda_get_free_bytes(', 'hda_reset_stream(',
         'hda_write_pcm(']
# Find definitions rather than earlier forward declarations/calls.
def definition(name):
    if name == 'hda_queue_pcm(':
        return extract('uint32_t hda_queue_pcm(')
    return extract(name)

main = r'''
static unsigned callbacks;
static void tick(void){
    assert(!irq_enabled && !stream_held);callbacks++;
    uint8_t sample[16]={0};
    assert(hda_queue_pcm(sample,sizeof(sample),48000,2,16)==sizeof(sample));
    (void)hda_get_delay_bytes();(void)hda_get_free_bytes();
    assert(!irq_enabled); /* helpers must not enable IRQs inside an ISR */
}
static void *producer(void *arg){
    uint8_t samples[16];memset(samples,(uintptr_t)arg,sizeof(samples));
    for(unsigned i=0;i<1024;i++)assert(hda_queue_pcm(samples,16,48000,2,16)==16);
    assert(irq_enabled);return NULL;
}
int main(void){
    for(unsigned i=0;i<HDA_BDL_ENTRIES;i++)hda_buffers[i]=dma[i];
    uint8_t samples[HDA_HW_BYTES*2];memset(samples,0x11,sizeof(samples));
    irq_enabled=false;
    assert(hda_queue_pcm(samples,8192,48000,2,16)==8192);
    assert(!irq_enabled && hda_get_delay_bytes()==8192 && !irq_enabled);
    hda_tick_callback=tick;
    /* An empty DMA ring previously called tick recursively from queue_pcm. */
    hda_underflow_count=4;hda_dma_pending=0;
    assert(hda_queue_pcm(samples,16,48000,2,16)==16);
    assert(!callbacks && !irq_enabled);
    lpib=2048;hda_isr(NULL);
    assert(callbacks==1 && !irq_enabled);
    hda_reset_stream();assert(!irq_enabled && !hda_is_playing);
    hda_tick_callback=NULL;irq_enabled=true;lpib=0;
    hda_queue_limit=HDA_MAX_QUEUE_BYTES;
    pthread_t workers[4];
    for(uintptr_t i=0;i<4;i++)assert(!pthread_create(&workers[i],NULL,producer,(void *)(i+1)));
    for(unsigned i=0;i<4;i++)assert(!pthread_join(workers[i],NULL));
    assert(hda_get_delay_bytes()==65536);
    unsigned counts[5]={0};
    for(unsigned i=0;i<HDA_BDL_ENTRIES;i++)for(unsigned j=0;j<hda_slot_bytes[i];j+=16){
        uint8_t v=dma[i][j];assert(v>=1 && v<=4);counts[v]++;
        for(unsigned k=0;k<16;k++)assert(dma[i][j+k]==v);
    }
    for(unsigned j=0;j<ring_count;j+=16){
        uint8_t v=ring[(ring_tail+j)%HDA_RING_SIZE];assert(v>=1 && v<=4);counts[v]++;
        for(unsigned k=0;k<16;k++)assert(ring[(ring_tail+j+k)%HDA_RING_SIZE]==v);
    }
    for(unsigned i=1;i<=4;i++)assert(counts[i]==1024);
    hda_reset_stream();lpib=0;
    /* Two ordinary 1024-frame writes must fill all look-ahead before the
     * hardware reaches it, rather than leave two silent descriptors. */
    assert(hda_queue_pcm(samples,4096,48000,2,16)==4096);
    assert(hda_slot_bytes[0]==2048 && hda_slot_bytes[2]==0);
    assert(hda_queue_pcm(samples,4096,48000,2,16)==4096);
    for(unsigned i=0;i<4;i++)assert(hda_slot_bytes[i]==2048);
    assert(hda_dma_pending==8192 && !ring_count);
    lpib=1024;assert(hda_get_delay_bytes()==7168);
    assert(hda_dma_pending==8192); /* query must not retire a live descriptor */
    hda_reset_stream();lpib=0;
    for(unsigned i=0;i<HDA_BDL_ENTRIES;i++)memset(samples+i*2048,i+1,2048);
    assert(hda_queue_pcm(samples,HDA_HW_BYTES,48000,2,16)==HDA_HW_BYTES);
    for(unsigned step=0;step<10000;step++){
        for(unsigned j=0;j<2048;j++)assert(dma[step%HDA_BDL_ENTRIES][j]==(uint8_t)(step+1));
        memset(samples,(uint8_t)(step+HDA_BDL_ENTRIES+1),2048);
        assert(hda_queue_pcm(samples,2048,48000,2,16)==2048);
        lpib=((step+1)*2048)%HDA_HW_BYTES;
        irq_enabled=false;hda_isr(NULL);assert(!irq_enabled);irq_enabled=true;
        assert(hda_get_delay_bytes()==HDA_HW_BYTES);
    }
    assert(total_played_bytes==10000*2048ULL);
    /* A ~96-ms delayed completion interrupt must retire every completed
     * period without replaying data or touching the currently playing slot. */
    hda_reset_stream();lpib=0;
    assert(HDA_BDL_ENTRIES>9);
    for(unsigned i=0;i<HDA_BDL_ENTRIES+9;i++)memset(samples+i*2048,i+1,2048);
    assert(hda_queue_pcm(samples,(HDA_BDL_ENTRIES+9)*2048,48000,2,16)==(HDA_BDL_ENTRIES+9)*2048);
    lpib=9*2048;irq_enabled=false;hda_isr(NULL);irq_enabled=true;
    for(unsigned i=0;i<9;i++)for(unsigned j=0;j<2048;j++)
        assert(dma[i][j]==i+HDA_BDL_ENTRIES+1);
    for(unsigned j=0;j<2048;j++)assert(dma[9][j]==10);
    assert(total_played_bytes==9*2048 && hda_get_delay_bytes()==HDA_HW_BYTES);
    hda_reset_stream();lpib=0;user=true;
    assert(hda_write_pcm(scratch_source,8192,48000,2,16)==8192);
    assert(!allocations && irq_enabled);
    fail_copy=true;
    assert(!hda_write_pcm(scratch_source,16,48000,2,16) && !allocations);
    fail_copy=false;hda_reset_stream();lpib=0;hda_queue_limit=HDA_MIN_QUEUE_BYTES;
    assert(hda_queue_pcm(samples,HDA_HW_BYTES,48000,2,16)==HDA_HW_BYTES);
    assert(hda_write_pcm(scratch_source,8192,48000,2,16)==8192);
    assert(sleeps && !allocations && irq_enabled && !thread.wakeup_ticks);
    hda_reset_stream();lpib=0;current_bits=32;
    assert(hda_queue_pcm(samples,15,48000,2,32)==8);
    assert(hda_get_delay_bytes()==8);
    puts("PASS HDA: IRQ flags preserved, no recursive underrun callbacks, one callback per IRQ");
    puts("PASS HDA: four CPU producers retain every sample; staged user copy, faults, blocking wake and 32-bit frame alignment");
    puts("PASS HDA: >100 seconds of descriptor/ring wrap preserve sample order and DMA delay");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path/'test.c').write_text(prefix + '\n'.join(definition(n) for n in names) + main)
    subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                    '-pthread', str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True,
                   env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
