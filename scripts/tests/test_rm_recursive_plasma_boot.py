#!/usr/bin/env python3
"""Exercise recursive rm on a large Wine-shaped tree in KDE Plasma 6."""
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/tests'))
from boot_probe import run_probe
from test_firefox_responsiveness_boot import prepare_desktop_disk

INIT = r'''
#define _GNU_SOURCE
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
static unsigned long long ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000ULL+t.tv_nsec/1000000; }
static void dump(const char *path) { char b[8192]; int f=open(path,O_RDONLY); if(f<0)return; int n=read(f,b,sizeof(b)-1); if(n>0){b[n]=0;printf("%s:\n%s\n",path,b);} close(f); }
int main(void) {
 setbuf(stdout,NULL); puts("RM_RECURSIVE_PLASMA_BOOT: START");
 mount("proc","/proc","proc",0,NULL); mount("sysfs","/sys","sysfs",0,NULL);
 mount("tmpfs","/tmp","tmpfs",0,NULL); mount("tmpfs","/run","tmpfs",0,NULL);
 mkdir("/tmp/runtime",0700);
 setenv("HOME","/",1); setenv("PATH","/usr/bin:/bin:/usr/sbin:/sbin",1);
 setenv("XDG_RUNTIME_DIR","/tmp/runtime",1); setenv("XDG_SESSION_TYPE","wayland",1);
 setenv("XDG_CURRENT_DESKTOP","KDE",1); setenv("KWIN_DRM_DEVICES","/dev/dri/card0",1);
 pid_t desktop=fork(); if(desktop<0){perror("fork");goto done;}
 if(!desktop){ if(setsid()<0)_exit(126); int f=open("/root/plasma-session.log",O_CREAT|O_TRUNC|O_WRONLY,0600); if(f>=0){dup2(f,1);dup2(f,2);close(f);} execl("/usr/bin/bash","bash","/root/rm-plasma-session.sh",NULL); perror("plasma session");_exit(127); }
 { unsigned long long start=ms(); while(ms()-start<240000){char b[256]={0};int f=open("/root/rm-probe.status",O_RDONLY);if(f>=0){int n=read(f,b,sizeof(b)-1);if(n>0&&strstr(b,"TREE_REMOVED")){close(f);break;}close(f);}usleep(100000);} kill(-desktop,SIGTERM); unsigned long long until=ms()+5000; while(ms()<until&&waitpid(desktop,NULL,WNOHANG)==0)usleep(100000); kill(-desktop,SIGKILL); waitpid(desktop,NULL,WNOHANG); }
 dump("/root/rm-probe.status"); dump("/root/rm-probe.log"); dump("/root/plasma-session.log");
 puts("RM_RECURSIVE_PLASMA_BOOT: DONE");
 done: { int f=open("/dev/kmsg",O_WRONLY); if(f>=0){dprintf(f,"RM_RECURSIVE_PLASMA_BOOT: DONE\\n");close(f);} }
 reboot(RB_POWER_OFF); for(;;)pause();
}
'''


def main():
    out = ROOT / 'build/rm-recursive-plasma-boot'
    out.mkdir(exist_ok=True)
    session = out/'rm-plasma-session.sh'
    launcher = out/'rm-wine-tree-probe.sh'
    autostart = out/'rm-wine-tree-probe.desktop'
    session.write_text('''#!/bin/bash
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export HOME=/
export XDG_RUNTIME_DIR=/tmp/runtime
export XDG_SESSION_TYPE=wayland
export XDG_CURRENT_DESKTOP=KDE
export KWIN_DRM_DEVICES=/dev/dri/card0
unset DISPLAY MOZ_HEADLESS
/etc/init.d/rcS
exec /usr/bin/avory-plasma-wayland
''')
    launcher.write_text('''#!/bin/bash
export HOME=/
set -u
status=/root/rm-probe.status
log=/root/rm-probe.log
rm -rf /.wine
mkdir -p /.wine/drive_c/windows/system32
i=0
while [ "$i" -lt 1400 ]; do
  name=$(printf 'module-%04d-0123456789012345678901234567890123456789.dll' "$i")
  : > "/.wine/drive_c/windows/system32/$name" || exit 20
  i=$((i+1))
done
echo TREE_READY > "$status"
/usr/bin/rm -rf ~/.wine > "$log" 2>&1
result=$?
echo RM_EXIT:$result >> "$status"
if [ -e /.wine ]; then
  echo TREE_REMAINS >> "$status"
else
  echo TREE_REMOVED >> "$status"
fi
''')
    autostart.write_text('[Desktop Entry]\nType=Application\nName=Recursive rm regression\nExec=/root/rm-wine-tree-probe.sh\nOnlyShowIn=KDE;\n')
    for path in (session, launcher):
        path.chmod(0o755)

    def prepare(disk, probe_out):
        prepare_desktop_disk(disk, probe_out, [
            (session, '/root/rm-plasma-session.sh'),
            (launcher, '/root/rm-wine-tree-probe.sh'),
            (autostart, '/etc/xdg/autostart/rm-wine-tree-probe.desktop'),
        ])

    env = dict(os.environ,
               SDL_VIDEODRIVER='offscreen',
               LD_LIBRARY_PATH=str(ROOT/'build/virgl-host/lib') + ':' + os.environ.get('LD_LIBRARY_PATH', ''))
    out = run_probe('rm-recursive-plasma-boot', INIT,
                    'RM_RECURSIVE_PLASMA_BOOT: DONE', cpus=8, memory='4G',
                    timeout=360, prepare_disk=prepare,
                    gpu='virtio-vga-gl', display='sdl,gl=on', qemu_env=env,
                    kernel_cmdline='kpi_amdgpu=0 init=/bin/firefox-probe-init',
                    qemu_args=['-netdev','user,id=net0','-device','rtl8139,netdev=net0'])

    result = subprocess.run(['debugfs', '-R', 'dump /root/rm-probe.status ' + str(out/'rm-probe.status'), out/'root.img'], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError('rm probe status was not written to the guest disk')
    result = subprocess.run(['debugfs', '-R', 'dump /root/rm-probe.log ' + str(out/'rm-probe.log'), out/'root.img'], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError('rm probe log was not written to the guest disk')
    status = (out/'rm-probe.status').read_text(errors='replace')
    log = (out/'rm-probe.log').read_text(errors='replace')
    serial = (out/'serial.log').read_text(errors='replace')
    version = subprocess.run(['debugfs', '-R', 'stat /usr/bin/plasmashell', out/'root.img'], capture_output=True, text=True, check=True)
    if 'Inode:' not in version.stdout:
        raise RuntimeError('The guest did not contain KDE Plasma')
    subprocess.run(['debugfs', '-R', 'dump /usr/bin/plasmashell ' + str(out/'plasmashell.elf'), out/'root.img'], capture_output=True, text=True, check=True)
    version_strings = subprocess.check_output(['strings', '-a', '-el', out/'plasmashell.elf'], text=True)
    versions = re.findall(r'^6\.\d+\.\d+$', version_strings, re.MULTILINE)
    if not versions:
        raise RuntimeError('The guest did not contain KDE Plasma 6')
    if 'RM_EXIT:0' not in status or 'TREE_REMOVED' not in status or 'fts_read failed' in log:
        raise RuntimeError(f'recursive rm failed under Plasma {", ".join(versions)}: {status.strip()}\n{log}')
    if '[RTL8139] receive and interrupt paths enabled' not in serial:
        raise RuntimeError('RTL8139 did not start')
    print(f'PASS: rm -rf ~/.wine removed 1,400 Wine-shaped entries in KDE Plasma {", ".join(versions)} with RTL8139.')


if __name__ == '__main__':
    main()
