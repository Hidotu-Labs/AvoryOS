#!/usr/bin/env python3
"""Run real headless Firefox and a wake-latency monitor in a private KVM guest.

The desktop image is read only while preparing a separate raw partition copy.
Only a host-local HTTP server and QEMU user networking are used.
"""
import json
import http.server
import threading
import time
import os
import re
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from image_copy import copy_image_range
from boot_probe import run_probe

INIT = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static unsigned long long ms(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000ULL+t.tv_nsec/1000000;}
static void dump(const char *path) {char b[8192];int f=open(path,O_RDONLY);if(f<0)return;int n=read(f,b,sizeof(b)-1);if(n>0){b[n]=0;printf("%s:\n%s\n",path,b);}close(f);}
static void tasks(pid_t pid){char path[128];snprintf(path,sizeof(path),"/proc/%d/syscall",pid);dump(path);snprintf(path,sizeof(path),"/proc/%d/task",pid);DIR *d=opendir(path);if(!d)return;struct dirent *e;while((e=readdir(d))){if(e->d_name[0]=='.')continue;snprintf(path,sizeof(path),"/proc/%d/task/%s/syscall",pid,e->d_name);dump(path);}closedir(d);}
int main(void) {
 setbuf(stdout,NULL);puts("FIREFOX_RESPONSIVENESS_BOOT: START");
 mount("proc","/proc","proc",0,NULL);mount("sysfs","/sys","sysfs",0,NULL);
 mount("tmpfs","/tmp","tmpfs",0,NULL);mount("tmpfs","/run","tmpfs",0,NULL);
 mkdir("/tmp/profile",0700);mkdir("/tmp/runtime",0700);
 FILE *prefs=fopen("/tmp/profile/user.js","w");if(prefs){fputs("user_pref(\"network.proxy.type\",0);\nuser_pref(\"network.captive-portal-service.enabled\",false);\nuser_pref(\"network.connectivity-service.enabled\",false);\nuser_pref(\"browser.shell.checkDefaultBrowser\",false);\nuser_pref(\"browser.startup.homepage_override.mstone\",\"ignore\");\nuser_pref(\"security.sandbox.content.level\",0);\n",prefs);fclose(prefs);}
 setenv("HOME","/root",1);setenv("PATH","/usr/bin:/bin",1);setenv("XDG_RUNTIME_DIR","/tmp/runtime",1);
 setenv("MOZ_DISABLE_CONTENT_SANDBOX","1",1);setenv("MOZ_HEADLESS","1",1);
 dump("/proc/meminfo");dump("/proc/tlbstats");
 pid_t browser=fork();if(browser<0){perror("fork");goto done;}if(!browser){if(setsid()<0){perror("setsid");_exit(126);}execl("/usr/lib/firefox/firefox","firefox","--headless","--no-remote","--profile","/tmp/profile","http://10.0.2.2:__PORT__/",NULL);perror("firefox");_exit(127);}
 unsigned long long start=ms(),last=start,worst=0,window=0;int status=0,exited=0;
 while(ms()-start<180000) {
  unsigned long long before=ms();usleep(10000);unsigned long long delay=ms()-before;if(delay>worst)worst=delay;

  if(ms()-last>=10000){window++;printf("MONITOR: second=%llu worst_wake_ms=%llu\n",(ms()-start)/1000,worst);worst=0;last=ms();dump("/proc/meminfo");dump("/proc/tlbstats");dump("/proc/sched_debug");if(window==2)tasks(browser);}
  if(waitpid(browser,&status,WNOHANG)==browser){printf("BROWSER: exited status=%d\n",status);exited=1;break;}
 }
 int completed=!exited&&window>=17;
 if(!exited){
  int result=kill(-browser,SIGTERM);printf("RECOVERY: terminate result=%d errno=%d\n",result,result?errno:0);if(result&&errno!=ESRCH)completed=0;unsigned long long until=ms()+10000;
  while(ms()<until){if(waitpid(browser,&status,WNOHANG)==browser){exited=1;break;}usleep(10000);}
  kill(-browser,SIGKILL);
 }
 puts("RECOVERY: browser closed; waiting 20 seconds for teardown");
 worst=0;unsigned long long recovery=ms();
 while(ms()-recovery<20000){unsigned long long before=ms();usleep(10000);unsigned long long d=ms()-before;if(d>worst)worst=d;while(waitpid(-1,&status,WNOHANG)>0){}}
 printf("RECOVERY: worst_wake_ms=%llu\n",worst);dump("/proc/meminfo");dump("/proc/tlbstats");
 dump("/proc/sched_debug");dump("/proc/sysdiag");
 printf("FIREFOX_RESPONSIVENESS_BOOT: %s windows=%llu\n",completed?"PASS":"FAIL",window);
 done: fflush(stdout);reboot(RB_POWER_OFF);for(;;)pause();
}
'''


def prepare_desktop_disk(disk, out, files=()):
    root = Path(__file__).resolve().parents[2]
    source = Path(os.environ.get('AVORY_TEST_DESKTOP_FS', root/'disk.img')).resolve()
    if source == disk.resolve():
        raise ValueError('Source filesystem and private destination must differ')
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():
            continue
        try:
            if not (proc/'comm').read_text().startswith('qemu-system'):
                continue
            for arg in (proc/'cmdline').read_bytes().split(b'\0'):
                for field in arg.decode(errors='replace').split(','):
                    if field.startswith('file='):
                        candidate = Path(field[5:])
                        if not candidate.is_absolute():
                            candidate = (proc/'cwd').resolve()/candidate
                        if candidate.resolve() == source:
                            raise RuntimeError('Source filesystem is open by QEMU; use an offline private copy')
        except (FileNotFoundError, PermissionError):
            continue
    # Copy only the filesystem, not its GPT wrapper. QEMU sees a raw FS.
    if os.environ.get('AVORY_TEST_DESKTOP_FS'):
        size, offset = source.stat().st_size, 0
    else:
        table = json.loads(subprocess.check_output(['sfdisk', '-J', source]))['partitiontable']
        part = table['partitions'][0]
        size, offset = part['size'] * table['sectorsize'], part['start'] * table['sectorsize']
    with disk.open('wb') as f:
        f.truncate(size)
    copy_image_range(source, disk, offset, 0, size)
    subprocess.run(['debugfs', '-w', '-R', 'rm /bin/firefox-probe-init', disk], capture_output=True, check=True)
    result = subprocess.run(['debugfs', '-w', '-R', f'write {out}/root/bin/probe-init /bin/firefox-probe-init', disk], text=True, capture_output=True, check=True)
    if 'Allocated inode:' not in result.stdout:
        raise RuntimeError(result.stdout + result.stderr)

    for source, destination in files:
        result = subprocess.run(['debugfs', '-w', '-R', f'write {source} {destination}', disk], text=True, capture_output=True, check=True)
        if 'Allocated inode:' not in result.stdout:
            raise RuntimeError(result.stdout + result.stderr)


def assert_cpu_recovery(log, limit=25):
    """Check the last 30 seconds before browser termination, not startup."""
    rows = [tuple(map(int, row)) for row in
            re.findall(r'^cpu +(\d+) 0 (\d+) (\d+)', log, re.MULTILINE)]
    if len(rows) < 5:
        raise RuntimeError('Missing CPU accounting samples')
    # The final sample follows process-group termination and is excluded.
    first, last = rows[-5], rows[-2]
    delta = [b-a for a, b in zip(first, last)]
    if min(delta) < 0 or sum(delta) <= 0:
        raise RuntimeError(f'Invalid CPU accounting delta: {delta}')
    busy = 100 * (delta[0]+delta[1]) / sum(delta)
    print(f'CPU_RECOVERY: final_30s_busy_percent={busy:.2f}', flush=True)
    if busy > limit:
        raise RuntimeError(f'CPU remained busy after page teardown: {busy:.2f}% > {limit}%')


def main():
    root = Path(__file__).resolve().parents[2]

    state = {'ticks': 0, 'times': [], 'complete': False}
    started = time.monotonic()
    page = b'''<!doctype html><title>Kernel browser stress</title><canvas id=c width=800 height=600></canvas><script>
let rounds=0,keep=[],pending=false,ctx=c.getContext('2d'),start=performance.now();
let timer=setInterval(()=>{if(performance.now()-start>=60000){clearInterval(timer);fetch('/complete');return;}
let a=new Uint8Array(8*1024*1024);for(let i=0;i<a.length;i+=4096)a[i]=rounds;
keep.push(a);if(keep.length>24)keep.shift();for(let i=0;i<400;i++){
ctx.fillStyle='hsl('+((rounds+i)%360)+',80%,50%)';ctx.fillRect(i*17%800,i*31%600,40,40)}
++rounds;if(!pending){pending=true;fetch('/tick?round='+rounds).catch(()=>{}).finally(()=>pending=false);}},100);</script>'''

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def do_GET(self):
            if self.path == '/':
                print(f"BROWSER_WORKLOAD: page served at {time.monotonic()-started:.1f}s", flush=True)
            if self.path == '/complete':
                state['complete'] = True
                print('BROWSER_WORKLOAD: 60-second workload complete', flush=True)
            if self.path.startswith('/tick?'):
                state['ticks'] += 1
                state['times'].append(time.monotonic())
                if state['ticks'] == 1 or state['ticks'] % 100 == 0:
                    print(f"BROWSER_WORKLOAD: progress ticks={state['ticks']} at {time.monotonic()-started:.1f}s", flush=True)
            body = page if self.path == '/' else b'ok'
            self.send_response(200)
            self.send_header('Content-Type', 'text/html')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):
            pass

    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        out = run_probe('firefox-responsiveness-boot', INIT.replace('__PORT__', str(server.server_port)),
                  'FIREFOX_RESPONSIVENESS_BOOT: PASS',
                  cpus=int(os.environ.get('AVORY_TEST_CPUS', '4')), memory='3G', timeout=300,
                  prepare_disk=prepare_desktop_disk, kernel_cmdline='kpi_amdgpu=0 init=/bin/firefox-probe-init',
                  qemu_args=['-netdev', 'user,id=net0', '-device', 'rtl8139,netdev=net0'])
        log = (out/'serial.log').read_text(errors='replace')
        delays = [int(value) for value in re.findall(r'worst_wake_ms=(\d+)', log)]
        if not delays or max(delays) > 1000:
            raise RuntimeError(f'Guest wake latency exceeded 1 second: {delays}')
        for bad in ('[KPF]', '[PMM] CORRUPTION', '[PMM] REF ERROR', 'FATAL:'):
            if bad in log:
                raise RuntimeError(f'{bad} in {out}/serial.log')
    finally:
        server.shutdown()
        server.server_close()
        gaps = [b-a for a,b in zip(state['times'], state['times'][1:])]
        print(f"BROWSER_WORKLOAD: ticks={state['ticks']} complete={state['complete']} max_gap_ms={max(gaps, default=0)*1000:.1f}", flush=True)
    if state['ticks'] < 100 or not state['complete']:
        raise RuntimeError('Firefox did not execute the browser workload')


if __name__ == '__main__':
    main()
