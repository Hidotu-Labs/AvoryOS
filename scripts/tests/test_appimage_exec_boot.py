#!/usr/bin/env python3
"""Launch a supplied AppImage runtime in a private guest, without FUSE/GUI."""
from pathlib import Path
import os
import re
import shutil
import subprocess
from boot_probe import ROOT, run_probe

TREE = ROOT / 'build/arch/rootfs'
INIT = r'''
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <unistd.h>
static void require(int ok,const char *what) {
 if(ok)return;printf("APPIMAGE_EXEC_BOOT: FAIL %s errno=%d\n",what,errno);
 fflush(stdout);reboot(RB_POWER_OFF);for(;;)pause();
}
static void write_all(int fd,const char *data,size_t count) {
 while(count){ssize_t n=write(fd,data,count);require(n>0&&(size_t)n<=count,"write");data+=n;count-=n;}
}
static char data[65521],reference[65521];
int main(void) {
 setvbuf(stdout,NULL,_IOLBF,0);puts("APPIMAGE_EXEC_BOOT: START");
 int in=open("/release.AppImage",O_RDONLY);
 int out=open("/Downloads/initial.part",O_CREAT|O_TRUNC|O_WRONLY,0755);
 require(in>=0&&out>=0,"open download fixtures");
 require(read(in,data,21870)==21870,"first download chunk");write_all(out,data,21870);
 require(!close(out),"close initial target");
 require(!rename("/Downloads/initial.part","/Downloads/continued.part"),"change target");
 out=open("/Downloads/continued.part",O_WRONLY|O_APPEND);require(out>=0,"reopen append");
 require(lseek(out,0,SEEK_SET)==0,"seek append fd");
 for(;;){ssize_t n=read(in,data,sizeof(data));require(n>=0,"read release");if(!n)break;write_all(out,data,n);}
 require(!fsync(out)&&!close(out),"finish download");
 require(!rename("/Downloads/continued.part","/Downloads/Forkit-1.3.0.AppImage"),"final rename");
 out=open("/Downloads/Forkit-1.3.0.AppImage",O_RDONLY);require(out>=0,"reopen completed download");
 struct stat a,b;require(!fstat(in,&a)&&!fstat(out,&b)&&a.st_size==b.st_size,"completed size");
 require(lseek(in,0,SEEK_SET)==0,"rewind release");
 for(;;){ssize_t n=read(in,reference,sizeof(reference));require(n>=0,"read reference");if(!n)break;
  size_t done=0;while(done<(size_t)n){ssize_t got=read(out,data+done,n-done);require(got>0,"read completed download");done+=got;}
  require(!memcmp(data,reference,n),"download byte mismatch");}
 close(in);close(out);puts("APPIMAGE_EXEC_BOOT: all downloaded bytes match verified release");
 const char *paths[]={"/Downloads/Forkit-1.3.0.AppImage", "..//Downloads/Forkit-1.3.0.AppImage", "./Forkit-1.3.0.AppImage"};
 const char *dirs[]={"/", "/", "/Downloads"};
 int passed=1;
 for(unsigned test=0;test<3;test++){
  pid_t pid=fork();
  if(!pid){
   require(!chdir(dirs[test]),"runtime cwd");
   char *args[]={(char *)paths[test],"--appimage-offset",NULL};
   execv(args[0],args);printf("APPIMAGE_EXEC_BOOT: exec errno=%d\n",errno);_exit(127);
  }
  int status=-1;
  if(!(pid>0 && waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0)){
   passed=0;printf("APPIMAGE_EXEC_BOOT: FAIL path=%s status=%d errno=%d\n",paths[test],status,errno);
  }
 }
 if(passed)puts("APPIMAGE_EXEC_BOOT: PASS");
 fflush(stdout);reboot(RB_POWER_OFF);for(;;)pause();
}
'''

def setup(root):
    asset = Path(os.environ['AVORY_TEST_APPIMAGE']).resolve()
    (root/'Downloads').mkdir(parents=True, exist_ok=True)
    dest = root/'release.AppImage'
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(asset, dest)
    dest.chmod(0o755)
    copied = set()

    def resolve(guest):
        parts, pending = [], guest.split('/')
        for _ in range(256):
            if not pending:
                return TREE.joinpath(*parts)
            part = pending.pop(0)
            if part in ('', '.'):
                continue
            if part == '..':
                if parts:
                    parts.pop()
                continue
            source = TREE.joinpath(*parts, part)
            if source.is_symlink():
                target = os.readlink(source)
                if target.startswith('/'):
                    parts = []
                pending = target.split('/') + pending
            else:
                parts.append(part)
        raise RuntimeError('symlink loop resolving ' + guest)

    def copy(guest):
        if guest in copied:
            return
        copied.add(guest)
        source = resolve(guest)
        destination = root/guest.lstrip('/')
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        dependencies(source)

    def dependencies(binary):
        desc = subprocess.check_output(['readelf', '-d', binary], text=True)
        for needed in re.findall(r'\(NEEDED\).*?\[(.*?)\]', desc):
            copy('/usr/lib/' + needed)

    copy('/lib64/ld-linux-x86-64.so.2')
    dependencies(asset)


def prepare(disk, out):
    subprocess.run(['mke2fs', '-q', '-F', '-t', 'ext4', '-O',
                    'none,filetype,sparse_super,extent,has_journal',
                    '-d', str(out/'root'), str(disk)], check=True)


if __name__ == '__main__':
    run_probe('appimage-exec-boot', INIT, 'APPIMAGE_EXEC_BOOT: PASS',
              setup=setup, prepare_disk=prepare, filesystem='ext4',
              cpus=2, disk_mib=384, timeout=120,
              accel=os.environ.get('AVORY_TEST_ACCEL', 'tcg'))
