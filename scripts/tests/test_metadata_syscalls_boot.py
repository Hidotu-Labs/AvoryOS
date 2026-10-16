#!/usr/bin/env python3
"""Exercise package-extraction metadata syscalls in a private KVM guest.

Build kernel/bin-x86_64/kernel first. Uses its own root image and ISO; never
opens disk.img or assigns a host GPU. Requires the existing bootable ISO.
"""
from pathlib import Path
import importlib.util
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/metadata-syscalls-boot'
spec = importlib.util.spec_from_file_location('boot_helper', ROOT / 'scripts/syscall-restart-boot.py')
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)
boot.OUT = OUT
boot.INIT = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <unistd.h>
#define EMPTY 0x1000
#define NOFOLLOW 0x100
#define CHMOD2 452
static int failures;
#define CHECK(expr) do { if (!(expr)) { printf("FAIL line=%d errno=%d: %s\n",__LINE__,errno,#expr); failures++; } } while(0)
int main(void) {
 setvbuf(stdout,NULL,_IOLBF,0);
 puts("METADATA_BOOT: START");
 mkdir("/probe",0755);
 int fd=open("/probe/target",O_CREAT|O_RDWR,0600);
 CHECK(fd>=0);
 CHECK(write(fd,"abc",3)==3);
 /* x86-64 callers may zero-extend the signed int AT_FDCWD argument. */
 CHECK(chdir("/probe")==0);
 CHECK(syscall(SYS_linkat,(unsigned)AT_FDCWD,"target",(unsigned)AT_FDCWD,"hardcopy",0)==0);
 int linked=open("hardcopy",O_RDONLY);char content[3]={0};
 CHECK(linked>=0 && read(linked,content,3)==3 && content[0]=='a' && content[2]=='c');
 if(linked>=0)close(linked);
 CHECK(chdir("/")==0);
 CHECK(symlink("target","/probe/link")==0);
 CHECK(symlink("not-created-yet","/probe/dangling")==0);
 struct stat st;
 /* Package archives routinely set ownership before extracting link targets. */
 CHECK(syscall(SYS_lchown,"/probe/dangling",123,456)==0);
 CHECK(lstat("/probe/dangling",&st)==0 && st.st_uid==123 && st.st_gid==456);
 CHECK(syscall(SYS_lchown,"/probe/link",123,456)==0);
 CHECK(fstat(fd,&st)==0 && st.st_uid==0 && st.st_gid==0);
 CHECK(lstat("/probe/link",&st)==0 && st.st_uid==123);
 int dir=open("/probe",O_PATH|O_DIRECTORY);
 CHECK(dir>=0);
 CHECK(syscall(SYS_fchownat,dir,"dangling",234,567,NOFOLLOW)==0);
 CHECK(lstat("/probe/dangling",&st)==0 && st.st_uid==234 && st.st_gid==567);
 CHECK(syscall(SYS_fchownat,dir,"target",345,678,0)==0);
 CHECK(fstat(fd,&st)==0 && st.st_uid==345 && st.st_gid==678);
 /* systemd copies rights through O_PATH descriptors and empty names. */
 int pathfd=open("/probe/target",O_PATH);
 CHECK(pathfd>=0);
 CHECK(syscall(SYS_fchownat,pathfd,"",0,0,EMPTY)==0);
 CHECK(syscall(CHMOD2,pathfd,"",0640,EMPTY)==0);
 CHECK(fstat(fd,&st)==0 && st.st_uid==0 && st.st_gid==0 && (st.st_mode&07777)==0640);
 CHECK(syscall(SYS_fchownat,pathfd,"",(unsigned)-1,789,EMPTY)==0);
 CHECK(fstat(fd,&st)==0 && st.st_uid==0 && st.st_gid==789);
 CHECK(syscall(SYS_fchown,pathfd,0,0)==0);
 CHECK(syscall(CHMOD2,dir,"link",0777,NOFOLLOW)==-1 && errno==EOPNOTSUPP);
 CHECK(fstat(fd,&st)==0 && (st.st_mode&07777)==0640);
 CHECK(syscall(CHMOD2,pathfd,"",0600,0)==-1 && errno==ENOENT);
 CHECK(syscall(CHMOD2,pathfd,"",0600,0x8000)==-1 && errno==EINVAL);
 CHECK(syscall(SYS_fchownat,pathfd,"",0,0,0x8000)==-1 && errno==EINVAL);
 CHECK(syscall(SYS_fchownat,99999,"",0,0,EMPTY)==-1 && errno==EBADF);
 CHECK(syscall(SYS_fchownat,pathfd,"child",0,0,0)==-1 && errno==ENOTDIR);
 /* A non-owner must not acquire metadata rights through AT_EMPTY_PATH. */
 pid_t child=fork();CHECK(child>=0);
 if(child==0){
  if(setuid(1000)!=0)_exit(2);
  errno=0;
  if(syscall(CHMOD2,pathfd,"",0777,EMPTY)!=-1 || errno!=EPERM)_exit(3);
  errno=0;
  if(syscall(SYS_fchownat,pathfd,"",0,0,EMPTY)!=-1 || errno!=EPERM)_exit(4);
  _exit(0);
 }
 if(child>0){int status=0;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}
 /* An unlinked but open inode must still accept descriptor metadata changes. */
 CHECK(unlink("/probe/target")==0);
 CHECK(syscall(CHMOD2,pathfd,"",0600,EMPTY)==0);
 CHECK(syscall(SYS_fchownat,pathfd,"",111,222,EMPTY)==0);
 CHECK(fstat(fd,&st)==0 && st.st_uid==111 && st.st_gid==222 && (st.st_mode&07777)==0600);
 close(pathfd);close(fd);close(dir);
 printf("METADATA_BOOT: END failures=%d\n",failures);
 fflush(NULL);
 reboot(RB_POWER_OFF);
 for(;;)sleep(1);
}
'''

os.chdir(ROOT)
OUT.mkdir(parents=True,exist_ok=True)
disk, iso = boot.prepare()
# Use the current build without replacing the ISO open in the user's QEMU.
shutil.copy2(ROOT/'kernel/bin-x86_64/kernel', OUT/'iso-root/boot/kernel')
boot.run(['strip','--strip-all',OUT/'iso-root/boot/kernel'])
boot.run(['xorriso','-as','mkisofs','-R','-r','-J','-b',
          'boot/limine/limine-bios-cd.bin','-no-emul-boot','-boot-load-size','4',
          '-boot-info-table','--efi-boot','boot/limine/limine-uefi-cd.bin',
          '-efi-boot-part','--efi-boot-image','--protective-msdos-label',
          OUT/'iso-root','-o',iso])
boot.run([ROOT/'limine/limine','bios-install',iso])
serial=OUT/'serial.log'
cmd=['qemu-system-x86_64','-M','q35','-cpu','host','-enable-kvm','-smp','4',
     '-m','1G','-drive',f'if=pflash,unit=0,format=raw,file={ROOT}/edk2-ovmf/ovmf-code-x86_64.fd,readonly=on',
     '-cdrom',str(iso),'-boot','d','-drive',f'file={disk},format=raw,if=none,id=probe',
     '-device','nvme,serial=metadata-probe,drive=probe','-vga','none','-device',
     'virtio-vga,addr=06.0','-display','none','-serial',f'file:{serial}',
     '-monitor','none','-no-reboot']
with (OUT/'host.log').open('wb') as output:
    process=subprocess.Popen(cmd,stdout=output,stderr=output)
    try:
        process.wait(timeout=60)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5)
log=serial.read_text(errors='replace')
assert process.returncode==0 and 'METADATA_BOOT: END failures=0' in log, str(serial)
print(log[log.index('METADATA_BOOT: START'):].split('METADATA_BOOT: END')[0]+'METADATA_BOOT: END failures=0')
