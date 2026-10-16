#!/usr/bin/env python3
"""Private KVM tests for UTS clone/unshare and rejection of incomplete sandboxes."""
from pathlib import Path
import importlib.util
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/uts-namespace-boot'
spec = importlib.util.spec_from_file_location('boot_helper', ROOT / 'scripts/syscall-restart-boot.py')
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)
boot.OUT = OUT
boot.INIT = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
static int failures;
#define CHECK(x) do { if (!(x)) {printf("FAIL line=%d errno=%d: %s\n",__LINE__,errno,#x);failures++;} } while(0)
static ino_t identity(void) {
 char path[128]; struct stat st;
 snprintf(path,sizeof(path),"/proc/%ld/ns/uts",syscall(SYS_gettid));
 return stat(path,&st) ? 0 : st.st_ino;
}
static void check_name(const char *name) {
 struct utsname u;CHECK(uname(&u)==0);CHECK(!strcmp(u.nodename,name));
}
static void *thread_work(void *arg) {
 ino_t id=identity();
 if(id!=*(ino_t*)arg || unshare(CLONE_NEWUTS))return (void*)1;
 if(identity()==id || sethostname("thread",6))return (void*)2;
 struct utsname u; if(uname(&u) || strcmp(u.nodename,"thread"))return (void*)3;
 return NULL;
}
int main(int argc,char **argv) {
 if(argc>1 && !strcmp(argv[1],"uts-exec")) {
  struct utsname u;
  return uname(&u) || strcmp(u.nodename,"shared") ||
         identity()!=(ino_t)strtoul(argv[2],NULL,10);
 }
 setvbuf(stdout,NULL,_IOLBF,0);puts("UTS_BOOT: START");
 CHECK(sethostname("parent",6)==0);check_name("parent");
 CHECK(syscall(SYS_setdomainname,"parent.domain",13)==0);
 ino_t original=identity();CHECK(original!=0);
 pid_t child=syscall(SYS_clone,CLONE_NEWUTS|SIGCHLD,0,0,0,0);CHECK(child>=0);
 if(!child) {
  ino_t private_id=identity();CHECK(private_id && private_id!=original);
  check_name("parent");CHECK(sethostname("child",5)==0);check_name("child");
  CHECK(syscall(SYS_setdomainname,"child.domain",12)==0);
  struct utsname u;CHECK(uname(&u)==0 && !strcmp(u.domainname,"child.domain"));
  pid_t shared=fork();CHECK(shared>=0);
  if(!shared) {CHECK(identity()==private_id);CHECK(sethostname("shared",6)==0);_exit(failures?1:0);}
  if(shared>0) {int st;CHECK(waitpid(shared,&st,0)==shared && WIFEXITED(st) && WEXITSTATUS(st)==0);}
  check_name("shared");
  shared=fork();CHECK(shared>=0);
  if(!shared) {
   char idbuf[32];snprintf(idbuf,sizeof(idbuf),"%lu",(unsigned long)private_id);
   execl("/bin/probe-init","probe-init","uts-exec",idbuf,NULL);_exit(99);
  }
  if(shared>0) {int st;CHECK(waitpid(shared,&st,0)==shared && WIFEXITED(st) && WEXITSTATUS(st)==0);}
  CHECK(unshare(CLONE_NEWUTS)==0);CHECK(identity()!=private_id);check_name("shared");
  CHECK(sethostname("isolated-again",14)==0);
  _exit(failures?1:0);
 }
 if(child>0) {int st;CHECK(waitpid(child,&st,0)==child && WIFEXITED(st) && WEXITSTATUS(st)==0);}
 check_name("parent");CHECK(identity()==original);
 struct utsname u;CHECK(uname(&u)==0 && !strcmp(u.domainname,"parent.domain"));
 int fd=open("/proc/sys/kernel/hostname",O_RDONLY);CHECK(fd>=0);
 char buf[80]={0};CHECK(read(fd,buf,79)==7 && !strcmp(buf,"parent\n"));close(fd);
 pthread_t thread;void *result=(void*)99;
 CHECK(pthread_create(&thread,NULL,thread_work,&original)==0);
 CHECK(pthread_join(thread,&result)==0 && result==NULL);
 check_name("parent");CHECK(identity()==original);
 /* Reject unsupported mixed requests atomically: never change even UTS. */
 errno=0;CHECK(unshare(CLONE_NEWUTS|CLONE_NEWUSER)==-1 && errno==EPERM);
 CHECK(identity()==original);check_name("parent");
 CHECK(unshare(CLONE_NEWUTS|CLONE_FILES)==-1 && errno==EOPNOTSUPP);
 CHECK(identity()==original);
 errno=0;CHECK(syscall(SYS_clone,0x3C020011,0,0,0,0)==-1 && errno==EPERM);
 CHECK(identity()==original);
 CHECK(uname((void*)1)==-1 && errno==EFAULT);
 CHECK(sethostname((void*)1,5)==-1 && errno==EFAULT);
 CHECK(sethostname("too-long",65)==-1 && errno==EINVAL);
 /* Root confinement and pivot behavior are exercised by the mount boot suite. */
 CHECK(chroot("/")==0);
 CHECK(syscall(SYS_seccomp,1,0,NULL)==-1 && errno==EFAULT);
 CHECK(prctl(PR_SET_SECCOMP,2,NULL)==-1 && errno==EFAULT);
 child=fork();CHECK(child>=0);
 if(!child) {
  if(setuid(1000))_exit(1);
  if(sethostname("bad",3)!=-1 || errno!=EPERM)_exit(2);
  if(unshare(CLONE_NEWUTS)!=-1 || errno!=EPERM)_exit(3);
  if(syscall(SYS_clone,CLONE_NEWUTS|SIGCHLD,0,0,0,0)!=-1 || errno!=EPERM)_exit(4);
  _exit(0);
 }
 if(child>0){int st;CHECK(waitpid(child,&st,0)==child && WIFEXITED(st) && WEXITSTATUS(st)==0);}
 check_name("parent");
 printf("UTS_BOOT: END failures=%d\n",failures);fflush(NULL);
 reboot(RB_POWER_OFF);for(;;)sleep(1);
}
'''

os.chdir(ROOT)
OUT.mkdir(parents=True, exist_ok=True)
disk, iso = boot.prepare()
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
       '-device', 'nvme,serial=uts-probe,drive=probe', '-nic', 'none', '-vga', 'none',
       '-device', 'virtio-vga,addr=06.0', '-display', 'none',
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
assert process.returncode == 0 and 'UTS_BOOT: END failures=0' in log, str(serial)
print(log[log.index('UTS_BOOT: START'):].split('UTS_BOOT: END')[0]+'UTS_BOOT: END failures=0')
