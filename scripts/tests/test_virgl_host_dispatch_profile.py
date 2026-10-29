#!/usr/bin/env python3
"""Run actual profiled host dispatch with deterministic clocks and error paths."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
s = (root/'build/virgl-host-src/src/vrend/vrend_decode.c').read_text()
struct = s[s.index('struct vrend_decode_ctx {'):s.index('static inline uint32_t get_buf_entry')]
helpers = s[s.index('static uint64_t vrend_profile_now('):s.index('static int vrend_decode_ctx_get_fencing_fd(')]
pre = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#define VIRGL_MAX_COMMANDS 12
#define TRACE_FUNC() ((void)0)
#define TRACE_SCOPE_SLOW(x) ((void)0)
#define VREND_DEBUG_ENABLED 0
#define VREND_DEBUG(a,b,fmt,offset,...) ((void)(offset))
#define dbg_dump_cmd_streams 0
struct vrend_context {int unused;};
struct virgl_context {unsigned ctx_id;};
static uint64_t now=100,clocks;static unsigned decoded,checks,errors;static int dispatch_error;
static bool gl_ok=true,switch_ok=true;
static int fake_clock(int id,struct timespec *ts){(void)id;clocks++;now+=100;ts->tv_sec=now/1000000000;ts->tv_nsec=now%1000000000;return 0;}
#define clock_gettime fake_clock
static bool vrend_hw_switch_context(struct vrend_context *ctx,bool b){(void)ctx;(void)b;return switch_ok;}
static bool vrend_debug(struct vrend_context *ctx,int f){(void)ctx;(void)f;return false;}
static void dump_command_stream_to_file(char *p,size_t n){(void)p;(void)n;}
static void vrend_report_buffer_error(struct vrend_context *ctx,unsigned b){(void)ctx;(void)b;errors++;}
static const char *vrend_get_comand_name(unsigned cmd){static const char *names[12]={"draw","constant","vbo","3","4","5","6","7","8","9","10","11"};assert(cmd<12);return names[cmd];}
#define virgl_error(...) ((void)0)
static bool vrend_check_no_error(struct vrend_context *ctx){(void)ctx;checks++;now+=200;return gl_ok;}
static int decode(struct vrend_context *ctx,const uint32_t *buf,uint32_t len){(void)ctx;(void)buf;(void)len;decoded++;now+=1000;return dispatch_error;}
static int (*decode_table[12])(struct vrend_context*,const uint32_t*,uint32_t)={decode,decode,decode,decode,decode,decode,decode,decode,decode,decode,decode,decode};
'''
main = r'''
int main(void){
 struct vrend_context render={0};struct vrend_decode_ctx ctx={.base={7},.grctx=&render};
 uint32_t commands[]={0,1,2};
 assert(!vrend_decode_ctx_submit_cmd(&ctx.base,commands,sizeof(commands)));
 assert(!clocks&&decoded==3&&checks==3); /* No clock reads by default. */
 ctx.profile.enabled=true;ctx.profile.epoch_ns=now;
 assert(!vrend_decode_ctx_submit_cmd(&ctx.base,commands,sizeof(commands)));
 assert(ctx.profile.submissions==1&&ctx.profile.bytes==12);
 assert(ctx.profile.calls[0]==1&&ctx.profile.calls[1]==1&&ctx.profile.calls[2]==1);
 assert(ctx.profile.error_ns&&ctx.profile.switch_ns&&ctx.profile.submit_ns);
 /* Reporting resets counters, retains opt-in and preserves command errors. */
 now+=2000000000ULL;
 assert(!vrend_decode_ctx_submit_cmd(&ctx.base,commands,sizeof(commands)));
 assert(ctx.profile.enabled&&!ctx.profile.submissions&&!ctx.profile.calls[0]);
 gl_ok=false;assert(vrend_decode_ctx_submit_cmd(&ctx.base,commands,4)==EINVAL);gl_ok=true;
 dispatch_error=EIO;assert(vrend_decode_ctx_submit_cmd(&ctx.base,commands,4)==EIO);dispatch_error=0;
 switch_ok=false;assert(vrend_decode_ctx_submit_cmd(&ctx.base,commands,4)==EINVAL);switch_ok=true;
 uint32_t bad_cmd=VIRGL_MAX_COMMANDS;assert(vrend_decode_ctx_submit_cmd(&ctx.base,&bad_cmd,4)==EINVAL);
 uint32_t bad_len=1<<16;unsigned before=decoded;
 assert(!vrend_decode_ctx_submit_cmd(&ctx.base,&bad_len,4));assert(decoded==before&&errors);
 puts("PASS: disabled profiling reads no clocks; per-command/error/context timing, reporting/reset and existing errors are preserved");
}
'''
with tempfile.TemporaryDirectory(prefix='virgl-dispatch-profile-') as tmp:
    p=Path(tmp)
    (p/'test.c').write_text(pre+struct+helpers+main)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',str(p/'test.c'),'-o',str(p/'test')],check=True)
    result=subprocess.run([str(p/'test')],check=True,text=True,capture_output=True)
    print(result.stdout.strip())
    spec=importlib.util.spec_from_file_location('summary',root/'scripts/summarize-virgl-profile.py')
    mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
    summary=mod.summarize(result.stderr)
    assert 'Context 7: 1 reporting windows' in summary and 'draw: 2 calls' in summary
    assert 'No host dispatch samples' in mod.summarize('unrelated QEMU log')
    print('PASS: summary reads real profiler output, separates contexts and handles missing data')
