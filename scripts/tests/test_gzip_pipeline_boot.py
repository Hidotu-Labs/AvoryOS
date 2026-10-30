#!/usr/bin/env python3
"""Exercise dd's pipe-skip fallback before gzip, as used by installers."""
from pathlib import Path
import gzip
import os
import shutil
import subprocess
from boot_probe import ROOT, run_probe

INIT = r'''
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <unistd.h>
static void finish(int ok, const char *why) {
 if (!ok) {
  const char *paths[]={"/tmp/dd1.err", "/tmp/dd2.err", "/tmp/progress.err"};
  for(int i=0;i<3;i++){FILE *f=fopen(paths[i],"r"); if(f){char b[512];size_t n;while((n=fread(b,1,sizeof(b),f)))fwrite(b,1,n,stdout);fclose(f);}}
 }
 printf("GZIP_PIPELINE_BOOT: %s %s\n", ok ? "PASS" : "FAIL", why);
 reboot(RB_POWER_OFF);
 for (;;) pause();
}
static void child_exit(int sig) { (void)sig; }
static void check_result(void) {
 FILE *actual=fopen("/tmp/result","rb"), *expected=fopen("/root/expected","rb");
 if(!actual || !expected) finish(0,"open decompressed payload");
 int a,b;
 do { a=fgetc(actual); b=fgetc(expected); if(a!=b)finish(0,"payload mismatch"); } while(a!=EOF);
 fclose(actual);fclose(expected);
}
int main(void) {
 setbuf(stdout, NULL);
 puts("GZIP_PIPELINE_BOOT: START");
 signal(SIGCHLD,child_exit);
 int input=open("/root/installer",O_RDONLY), independent=open("/root/installer",O_RDONLY);
 if(input<0 || independent<0)finish(0,"open fixture");
 int alias=dup(input);
 if(alias<0 || lseek(alias,7,SEEK_SET)!=7 || lseek(input,0,SEEK_CUR)!=7 ||
    lseek(independent,0,SEEK_CUR)!=0)finish(0,"dup position / independent open");
 pid_t child=fork();
 if(child==0) _exit(lseek(input,11,SEEK_SET)==11 ? 0 : 1);
 int status;
 if(child<0 || waitpid(child,&status,0)!=child || status || lseek(alias,0,SEEK_CUR)!=11)
  finish(0,"fork shared position");
 close(input);close(alias);close(independent);
 int p[2];
 if (pipe(p)) finish(0, "pipe");
 for (int end=0; end<2; end++) {
  for (int whence=0; whence<3; whence++) {
   errno=0;
   if (lseek(p[end], 0, whence)!=-1 || errno!=ESPIPE)
    finish(0, "pipe lseek must return ESPIPE");
  }
  close(p[end]);
 }
 if (socketpair(AF_UNIX, SOCK_STREAM, 0, p)) finish(0, "socketpair");
 errno=0;
 if (lseek(p[0],0,SEEK_SET)!=-1 || errno!=ESPIPE)
  finish(0, "socket lseek must return ESPIPE");
 close(p[0]); close(p[1]);
 /* First dd makes stdin a pipe. Second dd must discard the prefix by read,
  * because seeking a pipe cannot skip it. gzip must see 1f 8b immediately. */
 if (system("/bin/dd if=/root/installer bs=4096 2>/tmp/dd1.err | "
            "/bin/dd bs=$(read n < /root/prefix; echo $n) skip=1 iflag=fullblock 2>/tmp/dd2.err | "
            "/bin/gzip -dc > /tmp/result")) finish(0,"dd | dd skip | gzip");
 check_result();
 /* Makeself progress mode: successive child processes share one open input. */
 if (system("/bin/sh /root/progress.sh | /bin/gzip -dc > /tmp/result"))
  finish(0,"Makeself progress | gzip");
 check_result();
 finish(1, "dd pipe skip and gzip decompression");
}
'''


def setup(root):
    # Use the same GNU tools and glibc as the desktop image.
    source = ROOT / 'build/arch/rootfs/usr'
    pending = [source / 'bin' / name for name in ('bash', 'busybox', 'gzip')]
    copied = set()
    for binary in pending:
        if binary.name in copied:
            continue
        copied.add(binary.name)
        destination = root / ('bin' if binary.parent.name == 'bin' else 'usr/lib') / binary.name
        shutil.copy2(binary, destination)
        dynamic = subprocess.check_output(['readelf', '-d', str(binary)], text=True)
        for line in dynamic.splitlines():
            if '(NEEDED)' in line:
                pending.append(source / 'lib' / line.split('[')[1].split(']')[0])
    (root / 'bin/dd').symlink_to('busybox')
    shutil.copy2(root / 'bin/bash', root / 'bin/sh')
    for path in ('lib64/ld-linux-x86-64.so.2', 'usr/lib/ld-linux-x86-64.so.2'):
        destination = root / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source / 'lib/ld-linux-x86-64.so.2', destination)
    (root / 'lib').mkdir(exist_ok=True)
    for library in (root / 'usr/lib').iterdir():
        shutil.copy2(library, root / 'lib' / library.name)
    archive = gzip.compress(b'AvoryOS gzip pipeline works!\n', mtime=0)
    prefix = b'#' * 173
    installer = os.environ.get('AVORY_TEST_INSTALLER')
    if installer:
        # Read only the installer's first compressed member; never run it.
        import re
        with open(installer, 'rb') as stream:
            prefix = b''.join(stream.readline() for _ in range(519))
            size = int(re.search(rb'filesizes="(\d+)"', prefix).group(1))
            archive = stream.read(size)
    # The first pipeline exercises the independent pipe-skip path too.
    probe = root / 'root/prefix'
    probe.write_text(str(len(prefix)))
    (root / 'root/installer').write_bytes(prefix + archive)
    (root / 'root/expected').write_bytes(gzip.decompress(archive))
    # Same sequence as MS_dd_Progress, with bounded chunks and an inherited
    # regular-file stdin. Each dd runs in a separate child process.
    block = 65536 if len(archive) > 65536 else 16
    count, remainder = divmod(len(archive), block)
    script = f"dd bs={len(prefix)} count=0 skip=1 2>/tmp/progress.err\n"
    script += f"i=0; while [ $i -lt {count} ]; do dd bs={block} count=1 2>>/tmp/progress.err; i=$((i+1)); done\n"
    if remainder:
        script += f"dd bs={remainder} count=1 2>>/tmp/progress.err\n"
    (root / 'root/progress.sh').write_text('(\n' + script + ') < /root/installer\n')



if __name__ == '__main__':
    run_probe('gzip-pipeline-boot', INIT, 'GZIP_PIPELINE_BOOT: PASS',
              setup=setup, accel='tcg', cpus=1, timeout=90)
