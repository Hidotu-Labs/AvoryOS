#!/usr/bin/env python3
"""Verify first Wine prefix initialization in KDE Plasma 6 on RTL8139."""
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from boot_probe import run_probe
from test_firefox_responsiveness_boot import prepare_desktop_disk

INIT = r'''
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
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
static int read_pid(const char *path) { char b[32]={0}; int f=open(path,O_RDONLY); if(f<0)return -1; int n=read(f,b,sizeof(b)-1); close(f); return n>0?atoi(b):-1; }
static void append_status(const char *s) { int f=open("/root/wine-probe.status",O_WRONLY|O_APPEND); if(f>=0){write(f,s,strlen(s));close(f);} }
static void parent_path_probe(void) {
 const char *parent_path="/root/wine-parent-probe";
 const char *child_path="/root/wine-parent-probe/child";
 struct stat expected, actual;
 if(mkdir(parent_path,0700)||mkdir(child_path,0700)){puts("OPENAT_PARENT_AFTER_RMDIR: SETUP_FAILED");return;}
 int child=open(child_path,O_RDONLY|O_DIRECTORY);
 int parent=open(parent_path,O_RDONLY|O_DIRECTORY);
 if(child<0||parent<0||rmdir(child_path)){puts("OPENAT_PARENT_AFTER_RMDIR: SETUP_FAILED");if(child>=0)close(child);if(parent>=0)close(parent);return;}
 int reopened=openat(child,"..",O_RDONLY|O_DIRECTORY);
 int passed=reopened>=0&&!fstat(parent,&expected)&&!fstat(reopened,&actual)&&expected.st_ino==actual.st_ino&&expected.st_dev==actual.st_dev;
 printf("OPENAT_PARENT_AFTER_RMDIR: %s errno=%d\n",passed?"PASS":"FAIL",passed?0:errno);
 if(reopened>=0)close(reopened);close(parent);close(child);rmdir(parent_path);
}
int main(void) {
 setbuf(stdout,NULL); puts("WINE_NOTEPAD_PLASMA_BOOT: START");
 int probe=open("/root/wine-probe.log",O_CREAT|O_TRUNC|O_WRONLY,0600);if(probe>=0)close(probe);
 mount("proc","/proc","proc",0,NULL); mount("sysfs","/sys","sysfs",0,NULL);
 mount("tmpfs","/tmp","tmpfs",0,NULL); mount("tmpfs","/run","tmpfs",0,NULL);
 parent_path_probe();
 mkdir("/tmp/runtime",0700);
 setenv("HOME","/",1); setenv("PATH","/usr/bin:/bin:/usr/sbin:/sbin",1);
 setenv("XDG_RUNTIME_DIR","/tmp/runtime",1); setenv("XDG_SESSION_TYPE","wayland",1);
 setenv("XDG_CURRENT_DESKTOP","KDE",1); setenv("KWIN_DRM_DEVICES","/dev/dri/card0",1);
 pid_t desktop=fork();
 if(desktop<0){perror("fork");goto done;}
 if(!desktop){
   if(setsid()<0)_exit(126);
   int f=open("/root/plasma-session.log",O_CREAT|O_TRUNC|O_WRONLY,0600);
   if(f>=0){dup2(f,1);dup2(f,2);close(f);}
   execl("/usr/bin/bash","bash","/root/wine-plasma-session.sh",NULL);
   perror("plasma session");_exit(127);
 }
 {
   unsigned long long start=ms();
   int wineboot_status=-1;
   while(ms()-start<90000) {
     wineboot_status=read_pid("/root/wineboot-exit");
     if(wineboot_status>=0)break;
     usleep(100000);
   }
   int wine_pid=read_pid("/root/wine-pid");
   if(wineboot_status==0)append_status("FIRST_PREFIX_WINEBOOT_EXITED_SUCCESSFULLY\n");
   else if(wineboot_status>0)append_status("FIRST_PREFIX_WINEBOOT_EXITED_WITH_ERROR\n");
   else append_status("FIRST_PREFIX_WINEBOOT_STILL_RUNNING_AT_90S\n");
   if(wineboot_status==0)usleep(3000000);
   FILE *prefix=fopen("/root/wine-first.log","a");
   if(prefix){struct stat st;fprintf(prefix,"PREFIX_KERNEL32_DIRECT: %s\n",stat("/.wine/drive_c/windows/system32/kernel32.dll",&st)==0?"yes":"no");fprintf(prefix,"PREFIX_KERNEL32_DOSDEVICE: %s\n",stat("/.wine/dosdevices/c:/windows/system32/kernel32.dll",&st)==0?"yes":"no");fclose(prefix);}
   if(wine_pid>0)kill(wine_pid,SIGTERM);
   kill(-desktop,SIGTERM);
   unsigned long long until=ms()+5000;
   while(ms()<until && waitpid(desktop,NULL,WNOHANG)==0) usleep(100000);
   kill(-desktop,SIGKILL); waitpid(desktop,NULL,WNOHANG);
 }
 dump("/root/wine-probe.status"); dump("/root/wine-probe.log");
 dump("/root/wine-first.log"); dump("/root/wine-notepad.log");
 dump("/root/wine-launcher.log"); dump("/root/plasma-session.log");
 sync();
 puts("WINE_NOTEPAD_PLASMA_BOOT: DONE");
done:
 { int f=open("/dev/kmsg",O_WRONLY); if(f>=0){dprintf(f,"WINE_NOTEPAD_PLASMA_BOOT: DONE\\n");close(f);} }
 reboot(RB_POWER_OFF); for(;;)pause();
}
'''


def extract(out):
    result = subprocess.run(['debugfs', '-R', 'dump /root/wine-probe.status ' + str(out/'wine-probe.status'), out/'root.img'], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError('Wine probe status was not written to the guest disk')
    result = subprocess.run(['debugfs', '-R', 'dump /root/wine-probe.log ' + str(out/'wine-probe.log'), out/'root.img'], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError('Wine probe log was not written to the guest disk')
    result = subprocess.run(['debugfs', '-R', 'stat /usr/bin/plasmashell', out/'root.img'], capture_output=True, text=True, check=True)
    if 'Type: regular' not in result.stdout:
        raise RuntimeError('Could not inspect the installed Plasma shell')
    subprocess.run(['debugfs', '-R', 'dump /usr/bin/plasmashell ' + str(out/'plasmashell.elf'), out/'root.img'], capture_output=True, text=True, check=True)
    logs = (out/'wine-probe.log').read_text(errors='replace')
    for name in ('wine-first.log', 'wine-notepad.log', 'wine-launcher.log'):
        result = subprocess.run(['debugfs', '-R', f'dump /root/{name} ' + str(out/name), out/'root.img'], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(f'Wine diagnostic snapshot {name} was not written')
        logs += '\n' + (out/name).read_text(errors='replace')
    return (out/'wine-probe.status').read_text(errors='replace'), logs


def main():
    work = ROOT / 'build/wine-notepad-plasma-boot'
    work.mkdir(exist_ok=True)
    session = work/'wine-plasma-session.sh'
    launcher = work/'wine-notepad-probe.sh'
    autostart = work/'wine-notepad-probe.desktop'
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
exec 3>/root/wine-launcher.log
BASH_XTRACEFD=3
set -x
export HOME=/
export WINEPREFIX=/.wine
if [ -e /.wine ]; then /usr/bin/busybox rm -rf /.wine; fi
if [ -e /.wine ]; then echo CLEAN_PREFIX_FAILED > /root/wine-probe.status; exit 1; fi
echo STARTED_FIRST_PREFIX > /root/wine-probe.status
: > /root/wine-notepad.log
/usr/bin/wineboot -u > /root/wine-first.log 2>&1
wineboot_status=$?
echo "$wineboot_status" > /root/wineboot-exit
if [ "$wineboot_status" -eq 0 ]; then
  /usr/bin/wine notepad.exe > /root/wine-notepad.log 2>&1 &
  wine_pid=$!
  echo "$wine_pid" > /root/wine-pid
  wait "$wine_pid"
fi
    ''')
    autostart.write_text('[Desktop Entry]\nType=Application\nName=Wine Notepad reproduction probe\nExec=/root/wine-notepad-probe.sh\nOnlyShowIn=KDE;\n')
    for path in (session, launcher):
        path.chmod(0o755)

    def prepare(disk, out):
        prepare_desktop_disk(disk, out, [
            (session, '/root/wine-plasma-session.sh'),
            (launcher, '/root/wine-notepad-probe.sh'),
            (autostart, '/etc/xdg/autostart/wine-notepad-probe.desktop'),
        ])

    env = dict(os.environ,
               AVORY_TEST_DESKTOP_FS=str(ROOT/'build/arch/disk-partition.img'),
               SDL_VIDEODRIVER='offscreen',
               LD_LIBRARY_PATH=str(ROOT/'build/virgl-host/lib') + ':' + os.environ.get('LD_LIBRARY_PATH', ''))

    out = run_probe('wine-notepad-plasma-boot', INIT,
                    'WINE_NOTEPAD_PLASMA_BOOT: START', cpus=8, memory='4G',
                    timeout=420, prepare_disk=prepare,
                    accel='tcg',
                    gpu='virtio-vga-gl', display='sdl,gl=on', qemu_env=env,
                    kernel_cmdline='kpi_amdgpu=0 init=/bin/firefox-probe-init',
                    qemu_args=['-netdev','user,id=net0','-device','rtl8139,netdev=net0',
                               '-audiodev','none,id=snd0','-device','intel-hda',
                               '-device','hda-duplex,audiodev=snd0'])

    status, wine_log = extract(out)
    version_strings = subprocess.check_output(['strings','-a','-el',out/'plasmashell.elf'], text=True)
    versions = re.findall(r'^6\.\d+\.\d+$', version_strings, re.MULTILINE)
    if not versions:
        raise RuntimeError('The guest did not contain KDE Plasma 6')
    if 'FIRST_PREFIX_WINEBOOT_EXITED_SUCCESSFULLY' not in status:
        raise RuntimeError(f'First Wine prefix bootstrap did not finish successfully: {status.strip()}\n{wine_log[-4000:]}')
    if 'PREFIX_KERNEL32_DIRECT: yes' not in wine_log:
        raise RuntimeError('Wine prefix bootstrap exited without creating kernel32.dll')
    if re.search(r"Assertion .*alloc_shared_object", wine_log):
        raise RuntimeError('wineserver hit alloc_shared_object assertion')
    serial = (out/'serial.log').read_text(errors='replace')
    if 'OPENAT_PARENT_AFTER_RMDIR: PASS' not in serial:
        raise RuntimeError('openat(dirfd, "..") failed after the directory was removed')
    if '[RTL8139] receive and interrupt paths enabled' not in serial:
        raise RuntimeError('RTL8139 did not start')
    if '[PMM] CORRUPTION:' in serial:
        raise RuntimeError('Kernel failure during Plasma/Wine reproduction')
    (out/'wine-validation.log').write_text(
        f'First Wine prefix bootstrap completed in KDE Plasma {", ".join(versions)} with RTL8139.\n')
    print((out/'wine-validation.log').read_text(), end='')


if __name__ == '__main__':
    main()
