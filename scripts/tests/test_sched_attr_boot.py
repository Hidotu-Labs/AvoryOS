#!/usr/bin/env python3
"""Exercise Linux scheduler attribute syscalls in a private, GPU-free guest."""
from pathlib import Path
import importlib.util
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/sched-attr-boot'
spec = importlib.util.spec_from_file_location('boot_helper', ROOT / 'scripts/syscall-restart-boot.py')
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)
boot.OUT = OUT
boot.INIT = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
struct attr {
 uint32_t size,policy; uint64_t flags; int32_t nice; uint32_t priority;
 uint64_t runtime,deadline,period; uint32_t util_min,util_max;
};
static int failures;
#define CHECK(x) do { if (!(x)) { printf("FAIL line=%d errno=%d: %s\n",__LINE__,errno,#x); failures++; } } while(0)
#define SET(pid,a,flags) syscall(314,pid,a,flags)
#define GET(pid,a,size,flags) syscall(315,pid,a,size,flags)
#define ERR(call,e) CHECK((call)==-1 && errno==(e))
static void child_check(int policy,int nice) {
 pid_t child=fork(); CHECK(child>=0);
 if (!child) {
  struct attr a={0};
  _exit(GET(0,&a,sizeof(a),0) || a.policy!=(unsigned)policy || a.nice!=nice || a.flags ? 1 : 0);
 }
 if(child>0) { int status; CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0); }
}
static void *thread_check(void *unused) {
 (void)unused;
 struct attr a={0};
 if(GET(syscall(SYS_gettid),&a,56,0) || a.policy!=3 || a.nice!=8 || a.flags)
  return (void*)1;
 a.policy=0; a.nice=10;
 if(SET(0,&a,0) || GET(0,&a,56,0) || a.policy!=0 || a.nice!=10)
  return (void*)2;
 return NULL;
}
int main(void) {
 setvbuf(stdout,NULL,_IOLBF,0); puts("SCHED_ATTR_BOOT: START");
 struct attr a={.size=sizeof(a)},b={0};
 CHECK(SET(0,&a,0)==0); CHECK(GET(getpid(),&b,sizeof(b),0)==0);
 CHECK(b.size==56 && b.policy==0 && b.priority==0 && b.nice==0);
 /* Short and future structures; get never overwrites the caller's tail. */
 a.size=48; a.nice=7; CHECK(SET(0,&a,0)==0);
 memset(&b,0xAB,sizeof(b)); CHECK(GET(0,&b,48,0)==0);
 CHECK(b.size==48 && b.nice==7 && b.util_min==0xABABABABU);
 a.size=0; CHECK(SET(0,&a,0)==0);
 struct {struct attr a; uint64_t tail;} larger={.a={.size=64,.nice=6}};
 CHECK(SET(0,&larger,0)==0);
 larger.tail=0x12345678; CHECK(GET(0,&larger,64,0)==0 && larger.tail==0x12345678);
 larger.a.size=64;
 larger.tail=1; ERR(SET(0,&larger,0),E2BIG); CHECK(larger.a.size==56);
 a.size=47; ERR(SET(0,&a,0),E2BIG); CHECK(a.size==56);
 a.size=4097; ERR(SET(0,&a,0),E2BIG); CHECK(a.size==56);
 ERR(GET(0,&b,47,0),EINVAL); ERR(GET(0,&b,4097,0),EINVAL);
 ERR(SET(-1,&a,0),EINVAL); ERR(GET(-1,&b,sizeof(b),0),EINVAL);
 ERR(SET(999999,&a,0),ESRCH); ERR(GET(999999,&b,sizeof(b),0),ESRCH);
 ERR(SET(0,NULL,0),EINVAL); ERR(GET(0,NULL,sizeof(b),0),EINVAL);
 ERR(SET(0,(void*)1,0),EFAULT); ERR(GET(0,(void*)1,sizeof(b),0),EFAULT);
 ERR(SET(0,&a,1),EINVAL); ERR(GET(0,&b,sizeof(b),1),EINVAL);
 a.flags=128; ERR(SET(0,&a,0),EINVAL);
 a.flags=0; a.policy=4; ERR(SET(0,&a,0),EINVAL);
 a.policy=6; ERR(SET(0,&a,0),EOPNOTSUPP);
 a.policy=0; a.flags=96|8|16; ERR(SET(0,&a,0),EOPNOTSUPP);
 a.size=48; ERR(SET(0,&a,0),EINVAL);
 a=(struct attr){.size=56,.priority=1}; ERR(SET(0,&a,0),EINVAL);
 a.priority=0; a.nice=999; CHECK(SET(0,&a,0)==0);
 CHECK(GET(0,&b,56,0)==0 && b.nice==19 && b.policy==0);
 CHECK(syscall(SYS_sched_getscheduler,0)==0); /* Nice is not policy. */
 a.nice=-999; CHECK(SET(0,&a,0)==0);
 CHECK(GET(0,&b,56,0)==0 && b.nice==-20);
 a.nice=8; a.policy=3; CHECK(SET(0,&a,0)==0);
 CHECK(GET(0,&b,56,0)==0 && b.policy==3 && b.nice==8);
 CHECK(syscall(SYS_sched_getscheduler,0)==3);
 child_check(3,8);
 pthread_t thread; void *result=(void*)99;
 CHECK(pthread_create(&thread,NULL,thread_check,NULL)==0);
 CHECK(pthread_join(thread,&result)==0 && result==NULL);
 CHECK(GET(0,&b,56,0)==0 && b.policy==3 && b.nice==8);
 a.policy=0; a.nice=0; a.flags=8|16; CHECK(SET(0,&a,0)==0);
 CHECK(GET(0,&b,56,0)==0 && b.policy==3 && b.nice==8);
 a.flags=0; a.policy=5; CHECK(SET(0,&a,0)==0);
 CHECK(GET(0,&b,56,0)==0 && b.policy==5);
 a.policy=2; a.priority=0; ERR(SET(0,&a,0),EINVAL);
 a.priority=100; ERR(SET(0,&a,0),EINVAL);
 a.priority=50; a.flags=1; CHECK(SET(0,&a,0)==0);
 CHECK(GET(0,&b,56,0)==0 && b.policy==2 && b.priority==50 && b.flags==1);
 int priority=0; CHECK(syscall(SYS_sched_getparam,0,&priority)==0 && priority==50);
 priority=60; CHECK(syscall(SYS_sched_setparam,0,&priority)==0);
 CHECK(GET(0,&b,56,0)==0 && b.priority==60);
 child_check(0,0); /* RESET_ON_FORK clears RT policy and negative nice. */
 a=(struct attr){.size=56,.nice=-5,.flags=1}; CHECK(SET(0,&a,0)==0);
 child_check(0,0);
 a.flags=0; a.nice=0; CHECK(SET(0,&a,0)==0);
 priority=0; CHECK(syscall(SYS_sched_setscheduler,0,3,&priority)==0);
 CHECK(GET(0,&b,56,0)==0 && b.policy==3 && b.nice==0);
 CHECK(syscall(SYS_sched_setscheduler,0,0,&priority)==0);
 /* User pages must fault safely, including read-only output and page edges. */
 void *page=mmap(NULL,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 CHECK(page!=MAP_FAILED);
 if(page!=MAP_FAILED) {
  memcpy(page,&a,56); CHECK(mprotect(page,4096,PROT_READ)==0);
  ERR(GET(0,page,56,0),EFAULT);
  CHECK(mprotect((char*)page+4096,4096,PROT_NONE)==0);
  CHECK(mprotect(page,4096,PROT_READ|PROT_WRITE)==0);
  *(uint32_t*)((char*)page+4096-4)=56;
  ERR(SET(0,(char*)page+4096-4,0),EFAULT);
  munmap(page,8192);
 }
 pid_t owner=getpid(),child=fork(); CHECK(child>=0);
 if(!child) {
  if(setuid(1000)) _exit(1);
  a=(struct attr){.size=56,.nice=5};
  if(SET(0,&a,0)) _exit(2);
  a.nice=0; if(SET(0,&a,0)!=-1 || errno!=EPERM) _exit(3);
  a.nice=5; if(SET(owner,&a,0)!=-1 || errno!=EPERM) _exit(4);
  a.flags=1; if(SET(0,&a,0)) _exit(6);
  a.flags=0; if(SET(0,&a,0)!=-1 || errno!=EPERM) _exit(7);
  a.policy=2;a.priority=1; if(SET(0,&a,0)!=-1 || errno!=EPERM) _exit(5);
  _exit(0);
 }
 if(child>0) {int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}
 printf("SCHED_ATTR_BOOT: END failures=%d\n",failures); fflush(NULL);
 reboot(RB_POWER_OFF); for(;;)sleep(1);
}
'''

os.chdir(ROOT)
OUT.mkdir(parents=True, exist_ok=True)
disk, iso = boot.prepare()
# Keep the user's running VM and normal ISO untouched.
shutil.copy2(ROOT / 'kernel/bin-x86_64/kernel', OUT / 'iso-root/boot/kernel')
boot.run(['strip', '--strip-all', OUT / 'iso-root/boot/kernel'])
boot.run(['xorriso', '-as', 'mkisofs', '-R', '-r', '-J', '-b',
          'boot/limine/limine-bios-cd.bin', '-no-emul-boot', '-boot-load-size', '4',
          '-boot-info-table', '--efi-boot', 'boot/limine/limine-uefi-cd.bin',
          '-efi-boot-part', '--efi-boot-image', '--protective-msdos-label',
          OUT / 'iso-root', '-o', iso])
boot.run([ROOT / 'limine/limine', 'bios-install', iso])
serial = OUT / 'serial.log'
cmd = ['qemu-system-x86_64', '-M', 'q35', '-cpu', 'host', '-enable-kvm', '-smp', '4',
       '-m', '1G', '-drive', f'if=pflash,unit=0,format=raw,file={ROOT}/edk2-ovmf/ovmf-code-x86_64.fd,readonly=on',
       '-cdrom', str(iso), '-boot', 'd', '-drive', f'file={disk},format=raw,if=none,id=probe',
       '-device', 'nvme,serial=sched-attr-probe,drive=probe', '-nic', 'none',
       '-vga', 'none', '-device', 'virtio-vga,addr=06.0', '-display', 'none',
       '-serial', f'file:{serial}', '-monitor', 'none', '-no-reboot']
with (OUT / 'host.log').open('wb') as output:
    process = subprocess.Popen(cmd, stdout=output, stderr=output)
    try:
        process.wait(timeout=60)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5)
log = serial.read_text(errors='replace')
assert process.returncode == 0 and 'SCHED_ATTR_BOOT: END failures=0' in log, str(serial)
print(log[log.index('SCHED_ATTR_BOOT: START'):].split('SCHED_ATTR_BOOT: END')[0] +
      'SCHED_ATTR_BOOT: END failures=0')
