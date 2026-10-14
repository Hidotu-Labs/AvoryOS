#!/usr/bin/env python3
"""Execute overlay sequence readers while a writer is paused mid-update."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/linuxkpi/include/linux/seqlock.h').read_text();s=re.sub(r'^#include.*$','',s,flags=re.M)
code=r'''
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <stdio.h>
typedef pthread_mutex_t spinlock_t;
typedef struct {unsigned sequence;spinlock_t lock;}seqlock_t;
typedef struct {unsigned sequence;}seqcount_t;
#define spin_lock_init(p) pthread_mutex_init(p,NULL)
#define spin_lock pthread_mutex_lock
#define spin_unlock pthread_mutex_unlock
'''+s+r'''
static seqlock_t sl;static seqcount_t raw;
static atomic_bool begun,done;static unsigned observed;
static void *reader(void *arg){atomic_store(&begun,true);observed=arg?read_seqcount_begin(&raw):read_seqbegin(&sl);atomic_store(&done,true);return NULL;}
int main(void){
 seqlock_init(&sl);
 for(int mode=0;mode<2;mode++){
  atomic_store(&begun,false);atomic_store(&done,false);
  if(mode)write_seqcount_begin(&raw);else write_seqlock(&sl);
  pthread_t t;pthread_create(&t,NULL,reader,(void*)(long)mode);while(!atomic_load(&begun))usleep(50);
  usleep(20000);assert(!atomic_load(&done));
  if(mode)write_seqcount_end(&raw);else write_sequnlock(&sl);
  pthread_join(t,NULL);assert(!(observed&1));
  if(mode){assert(!read_seqcount_retry(&raw,observed));write_seqcount_begin(&raw);write_seqcount_end(&raw);assert(read_seqcount_retry(&raw,observed));}
  else {assert(!read_seqretry(&sl,observed));write_seqlock(&sl);write_sequnlock(&sl);assert(read_seqretry(&sl,observed));}
 }
 puts("PASS: seqcount/seqlock readers reject in-progress writers and retry completed changes");
}
'''
with tempfile.TemporaryDirectory(prefix='kpi-seqcount-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=gnu11','-O2','-pthread',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=10)
 # Original read begin returned the odd count immediately and retry could
 # accept it unchanged, allowing a torn count/time pair in drm_vblank.c.
 old=code.replace('unsigned int seq;\n  do {\n    seq = __atomic_load_n(&sl->sequence, __ATOMIC_ACQUIRE);\n    if (seq & 1)\n      __asm__ volatile("pause" ::: "memory");\n  } while (seq & 1);\n  return seq;', 'return __atomic_load_n(&sl->sequence, __ATOMIC_ACQUIRE);')
 (p/'old.c').write_text(old)
 subprocess.run(['cc','-std=gnu11','-O2','-pthread',str(p/'old.c'),'-o',str(p/'old')],check=True)
 r=subprocess.run([str(p/'old')],capture_output=True,timeout=10);assert r.returncode!=0
 print('PASS: former odd-sequence reader fails the regression')
