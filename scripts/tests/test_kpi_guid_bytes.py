#!/usr/bin/env python3
"""Exercise UUID-only byte adapter, including exhausted hardware RNG."""
from pathlib import Path
import re, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/linuxkpi/src/guid_bytes.c').read_text()
source=re.sub(r'^#include.*$', '', source, flags=re.M)
a=source.index('static bool guid_hardware_word(')
b=source.index('/* SplitMix64', a)
source=source[:a]+source[b:]
shim=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
typedef uint64_t u64;
static bool hardware;
static bool guid_hardware_word(u64 *w) { *w=0x123456789abcdef0ULL; return hardware; }
static u64 linuxkpi_rdtsc_fence(void) { return 0; }
static u64 linuxkpi_monotonic_ns(void) { return 0; }
'''
main=r'''
static unsigned char ids[4][2000][16];
static void *worker(void *arg) {
 size_t thread=(size_t)arg;
 for(int i=0;i<2000;i++) linuxkpi_guid_bytes(ids[thread][i],16);
 return NULL;
}
static int compare(const void *a,const void *b) { return memcmp(a,b,16); }
int main(void) {
 unsigned char out[26]; memset(out,0xa5,sizeof(out)); hardware=true;
 linuxkpi_guid_bytes(out+1,24); assert(out[0]==0xa5 && out[25]==0xa5);
 for(int i=0;i<3;i++) assert(!memcmp(out+1+i*8, &(u64){0x123456789abcdef0ULL},8));
 linuxkpi_guid_bytes(out+1,0); hardware=false;
 memset(out,0xa5,sizeof(out)); linuxkpi_guid_bytes(out+1,23);
 assert(out[0]==0xa5 && out[24]==0xa5 && out[25]==0xa5);
 pthread_t threads[4];
 for(size_t i=0;i<4;i++) assert(!pthread_create(&threads[i],NULL,worker,(void *)i));
 for(int i=0;i<4;i++) pthread_join(threads[i],NULL);
 /* UUID version/variant bit reservation must not cause repeats in this run. */
 unsigned char *flat=(unsigned char *)ids;
 for(int i=0;i<8000;i++) { flat[i*16+7]=(flat[i*16+7]&15)|64; flat[i*16+8]=(flat[i*16+8]&63)|128; }
 qsort(ids,8000,16,compare);
 for(int i=1;i<8000;i++) assert(memcmp(flat+(i-1)*16,flat+i*16,16));
 puts("PASS: UUID-only bytes, partial bounds, concurrent fallback and 8000 distinct identifiers (not an entropy test)");
}
'''
with tempfile.TemporaryDirectory(prefix='kpi-guid-') as tmp:
 p=Path(tmp)/'test'; p.with_suffix('.c').write_text(shim+source+main)
 subprocess.run(['cc','-std=gnu11','-pthread','-fsanitize=address,undefined',str(p.with_suffix('.c')),'-o',str(p)],check=True)
 subprocess.run([str(p)],check=True,timeout=15)
