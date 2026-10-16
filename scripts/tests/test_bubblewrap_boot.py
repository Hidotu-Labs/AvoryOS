#!/usr/bin/env python3
"""Run the installed bubblewrap binary with WebKit's namespace and mount options."""
from pathlib import Path
import re
import shutil
import subprocess
from boot_probe import ROOT, run_probe

INIT = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
struct insn {uint16_t code;uint8_t jt,jf;uint32_t k;};
static int failures;
#define CHECK(x) do {if(!(x)){printf("FAIL line=%d errno=%d: %s\n",__LINE__,errno,#x);failures++;}}while(0)
static int payload(void) {
 CHECK(getpid()>0 && getpid()<10);
 CHECK(access("/host-secret",F_OK)==-1 && errno==ENOENT);
 CHECK(access("/proc/self/status",R_OK)==0);
 CHECK(open("/app/forbidden",O_WRONLY|O_CREAT,0600)==-1 && errno==EROFS);
 int fd=open("/tmp/writable",O_WRONLY|O_CREAT,0600);CHECK(fd>=0);if(fd>=0)close(fd);
 CHECK(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==1);
 CHECK(prctl(PR_GET_SECCOMP,0,0,0,0)==2);
 CHECK(syscall(SYS_getppid)==-1 && errno==EACCES);
 CHECK(unshare(CLONE_NEWUTS)==-1 && errno==EPERM);
 CHECK(chroot("/tmp")==-1 && errno==EPERM);
 puts("BUBBLEWRAP_BOOT: payload finished");
 return failures?1:0;
}
static void launch(int network) {
 /* Keep this descriptor open across exec, as WebKit does. */
 int fd=syscall(SYS_memfd_create,"bpf",0);CHECK(fd>=0);
 struct insn filter[]={{0x20,0,0,0},{0x15,0,1,SYS_getppid},
                       {6,0,0,0x00050000|EACCES},{6,0,0,0x7fff0000}};
 CHECK(write(fd,filter,sizeof(filter))==sizeof(filter));CHECK(lseek(fd,0,SEEK_SET)==0);
 char number[32];snprintf(number,sizeof(number),"%d",fd);
 char *args[]={"/usr/bin/bwrap","--unshare-uts","--unshare-ipc","--unshare-pid",
  "--ro-bind","/usr","/usr","--ro-bind","/lib64","/lib64",
  "--ro-bind","/app","/app","--dev","/dev","--proc","/proc",
  "--tmpfs","/tmp","--dir","/run","--symlink","../run","/var/run",
  "--cap-drop","ALL","--seccomp",number,
  network?"--unshare-net":"--clearenv","--chdir","/",
  "/app/probe","payload",NULL};
 pid_t child=fork();CHECK(child>=0);
 if(!child){execv(args[0],args);perror("bwrap exec");_exit(127);}
 if(child>0){int status=0;CHECK(waitpid(child,&status,0)==child);
  printf("BUBBLEWRAP_BOOT: network=%d status=%d\n",network,status);
  CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);}
 close(fd);
}
int main(int argc,char **argv) {
 setvbuf(stdout,NULL,_IOLBF,0);
 if(argc>1&&!strcmp(argv[1],"payload"))return payload();
 puts("BUBBLEWRAP_BOOT: START");launch(0);launch(1);
 printf("BUBBLEWRAP_BOOT: END failures=%d\n",failures);fflush(NULL);
 reboot(RB_POWER_OFF);for(;;)sleep(60);
}
'''

def setup(root):
    staged = ROOT / 'build/alpine/rootfs'
    copied = set()

    def copy_elf(path):
        if path in copied:
            return
        copied.add(path)
        src = staged / path.lstrip('/')
        # Resolve absolute guest symlinks inside the staging tree.
        while src.is_symlink():
            target = src.readlink()
            src = staged / str(target).lstrip('/') if target.is_absolute() else src.parent / target
        dest = root / path.lstrip('/')
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dest)
        dynamic = subprocess.check_output(['readelf', '-d', src], text=True)
        for library in re.findall(r'NEEDED.*\[(.*?)\]', dynamic):
            copy_elf('/usr/lib/' + library)

    copy_elf('/usr/bin/bwrap')
    copy_elf('/lib64/ld-linux-x86-64.so.2')
    # The loader searches /usr/lib only with a cache/config; supply /lib too.
    for source in list((root / 'usr/lib').iterdir()):
        shutil.copy2(source, root / 'lib' / source.name)
    (root / 'app').mkdir()
    (root / 'app').chmod(0o777)  # Prove the mount, rather than DAC, blocks creation.
    shutil.copy2(root / 'bin/probe-init', root / 'app/probe')
    (root / 'host-secret').write_text('must not be visible in sandbox\n')

if __name__ == '__main__':
    run_probe('bubblewrap-boot', INIT, 'BUBBLEWRAP_BOOT: END failures=0', setup, timeout=90)
