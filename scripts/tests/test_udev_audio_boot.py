#!/usr/bin/env python3
"""Boot real udev, WirePlumber and PipeWire against AvoryOS HDA in private KVM.

Requires a built kernel, the normal ISO, staged Arch rootfs and musl toolchain.
Uses only its own root.img/boot.iso, no disk.img, VFIO or host audio device.
Checks coldplug's SOUND_INITIALIZED, a real ALSA sink, and PCM playback exit.
"""
from pathlib import Path
import importlib.util,subprocess,shutil,os,re,time
ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'build/udev-audio-boot'
OUT.mkdir(parents=True,exist_ok=True)
TREE=ROOT/'build/arch/rootfs'
SECONDS=int(os.environ.get('AVORY_AUDIO_SECONDS','2'))
CAPTURE=os.environ.get('AVORY_AUDIO_CAPTURE')=='1'
PULSE=os.environ.get('AVORY_AUDIO_PULSE')=='1'
STRESS=os.environ.get('AVORY_AUDIO_STRESS')=='1'
spec=importlib.util.spec_from_file_location('boot',ROOT/'scripts/syscall-restart-boot.py');boot=importlib.util.module_from_spec(spec);spec.loader.exec_module(boot);boot.OUT=OUT
boot.INIT=r'''
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/reboot.h>
int main(void){setvbuf(stdout,0,_IOLBF,0);puts("AUDIO_PROBE: START");pid_t p=fork();if(!p){char *a[]={"/usr/bin/bash","/probe.sh",0};execv(a[0],a);perror("exec");_exit(127);}int s;waitpid(p,&s,0);printf("AUDIO_PROBE: END status=%d\n",s);fflush(0);reboot(RB_POWER_OFF);for(;;)sleep(1);}
'''
def setup(root):
    def resolve(guest):
        parts=[];todo=guest.split('/');count=0
        while todo:
            part=todo.pop(0)
            if part in ('','.'):continue
            if part=='..':
                if parts:parts.pop()
                continue
            p=TREE.joinpath(*parts,part)
            if p.is_symlink():
                count+=1;assert count<40
                dest=os.readlink(p)
                if dest.startswith('/'):parts=[]
                todo=dest.split('/')+todo
            else:parts.append(part)
        return TREE.joinpath(*parts)
    done=set()
    def cp(guest):
        if guest in done:return
        done.add(guest);src=resolve(guest)
        if not src.is_file():raise RuntimeError('missing '+guest)
        dst=root/guest.lstrip('/');dst.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src,dst)
        with src.open('rb') as stream:
            if stream.read(4)!=b'\x7fELF':return
        desc=subprocess.check_output(['readelf','-d',src],text=True,stderr=subprocess.DEVNULL)
        for needed in re.findall(r'\(NEEDED\).*?\[(.*?)\]',desc):
            dep='/usr/lib/'+needed
            if not resolve(dep).exists():
                dep=next(('/'+str(p.relative_to(TREE)) for p in (TREE/'usr/lib').glob('*/'+needed) if p.is_file()),dep)
            cp(dep)
    for binary in ['bash','busybox','pipewire','pipewire-pulse','wireplumber','wpctl','pactl','pw-cli','dbus-run-session','dbus-daemon','udevadm','pw-play','paplay']:cp('/usr/bin/'+binary)
    cp('/lib64/ld-linux-x86-64.so.2')
    cp('/usr/lib/systemd/systemd-udevd')
    for directory in ['usr/lib/pipewire-0.3','usr/lib/wireplumber-0.5','usr/lib/alsa-lib']:
        if (TREE/directory).exists():
            for p in (TREE/directory).rglob('*'):
                if p.is_file():cp('/'+str(p.relative_to(TREE)))
    for directory in ['support','alsa','audioconvert','audiomixer','audiotestsrc']:
        for p in (TREE/'usr/lib/spa-0.2'/directory).glob('*.so'):cp('/'+str(p.relative_to(TREE)))
    for directory in ['usr/share/pipewire','usr/share/wireplumber','usr/share/alsa','usr/share/alsa-card-profile','usr/share/dbus-1','etc/wireplumber','etc/dbus-1','usr/lib/udev','etc/udev']:
        if (TREE/directory).exists():shutil.copytree(TREE/directory,root/directory,symlinks=True,dirs_exist_ok=True)
    for file in ['etc/passwd','etc/group','etc/nsswitch.conf','etc/machine-id']:
        if (TREE/file).exists():shutil.copy2(TREE/file,root/file)
    for binary in ['sh','mkdir','sleep','cat','kill','id','grep','rm','mount','chmod','cp','nohup','pidof']:
        (root/'bin'/binary).symlink_to('/usr/bin/busybox')
    import wave,math,struct
    with wave.open(str(root/'tone.wav'),'wb') as w:
        w.setparams((2,2,48000,0,'NONE','not compressed'))
        w.writeframes(b''.join(struct.pack('<hh',v,v) for i in range(48000*SECONDS)
                     for v in [int(3000*math.sin(2*math.pi*440*i/48000))]))
    shutil.copy2(ROOT/'config/wireplumber/20-avoryos-hda.conf',root/'etc/wireplumber/wireplumber.conf.d/20-avoryos-hda.conf')
    if STRESS:
        stress=OUT/'audio-stress.c'
        stress.write_text(r'''#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static volatile unsigned checksum;
static void *work(void *p){unsigned char v=(unsigned long)p;for(;;){volatile unsigned char *a=malloc(4*1024*1024);if(a){for(unsigned j=0;j<4*1024*1024;j+=4096)a[j]=v;__atomic_fetch_add(&checksum,a[0],__ATOMIC_RELAXED);free((void*)a);}usleep(1000);}return 0;}
int main(void){pthread_t t[8];for(unsigned long i=0;i<8;i++)pthread_create(t+i,0,work,(void*)i);for(;;)sleep(1);}
''')
        subprocess.run([ROOT/'toolchain/x86_64-linux-musl/bin/x86_64-linux-musl-gcc','-O2','-static','-pthread',stress,'-o',root/'bin/audio-stress'],check=True)
    # Execute the actual image-builder boot script, including its statuses.
    image_builder=(ROOT/'scripts/setup-arch.sh').read_text()
    rcs=image_builder.split("cat <<'EOF' > \"${ROOTFS_DIR}/etc/init.d/rcS\"\n",1)[1].split('\nEOF',1)[0]+'\n'
    (root/'etc/init.d').mkdir(parents=True,exist_ok=True)
    (root/'etc/init.d/rcS').write_text(rcs)
    (root/'etc/init.d/rcS').chmod(0o755)
    probe='''#!/bin/sh
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export HOME=/root
/etc/init.d/rcS
echo RCS_HOSTNAME=$(/usr/bin/busybox hostname)
export XDG_RUNTIME_DIR=/run/pipewire
export PIPEWIRE_RUNTIME_DIR=/run/pipewire
export PULSE_SERVER=unix:/run/pipewire/pulse/native
# Force the real settle tool to wait for a directory deletion notification.
: > /run/udev/queue
(sleep 0.2; rm /run/udev/queue) &
udevadm settle --timeout=3
echo WAITING_SETTLE_EXIT=$?
sleep 3
echo ===DEVICE_PROPERTIES===
udevadm info --query=property --path=/sys/class/sound/card0
echo ===STATUS===
wpctl status
pactl list short sinks
pw-play /tone.wav
echo PLAYBACK_EXIT=$?
echo ===UDEV_LOG===
cat /var/log/udev.log
kill $(pidof wireplumber pipewire-pulse pipewire) 2>/dev/null || true
'''
    if PULSE:probe=probe.replace('pw-play /tone.wav','paplay /tone.wav')
    if STRESS:
        player='paplay /tone.wav' if PULSE else 'pw-play /tone.wav'
        probe=probe.replace(player, '/bin/audio-stress &\nstress_pid=$!\n'+player)
        probe=probe.replace('echo PLAYBACK_EXIT=$?', 'echo PLAYBACK_EXIT=$?\nkill "$stress_pid" 2>/dev/null || true')
    (root/'probe.sh').write_text(probe)

run=boot.run
def big_run(args):
    if str(args[0])=='xorriso' and '-as' in args:
        shutil.copy2(ROOT/'kernel/bin-x86_64/kernel',OUT/'iso-root/boot/kernel')
        subprocess.run(['strip','--strip-all',OUT/'iso-root/boot/kernel'],check=True)
    if str(args[0])=='mke2fs':
        with Path(args[-1]).open('r+b') as f:f.truncate(512*1024**2)
    run(args)
boot.run=big_run
disk,iso=boot.prepare(setup)
serial=OUT/'serial.log'
cmd=['qemu-system-x86_64','-M','q35','-cpu','host','-enable-kvm','-smp','4','-m','1G','-drive',f'if=pflash,unit=0,format=raw,file={ROOT}/edk2-ovmf/ovmf-code-x86_64.fd,readonly=on','-cdrom',str(iso),'-boot','d','-drive',f'file={disk},format=raw,if=none,id=probe','-device','nvme,serial=audio-probe,drive=probe','-audiodev',(f'wav,id=snd0,path={OUT}/hda-output.wav' if CAPTURE else 'none,id=snd0'),'-device','intel-hda','-device','hda-duplex,audiodev=snd0','-vga','none','-device','virtio-vga,addr=06.0','-display','none','-serial',f'file:{serial}','-monitor','none','-no-reboot']
with (OUT/'host.log').open('wb') as host:
    p=subprocess.Popen(cmd,stdout=host,stderr=host)
    try:p.wait(timeout=120)
    finally:
        if p.poll() is None:p.terminate();p.wait(timeout=5)
print('Probe exited',p.returncode,'see',serial)

log=serial.read_text(errors='replace')
startup=log[log.index('AUDIO_PROBE: START'):]
assert p.returncode==0 and '[ !! ]' not in startup, 'rcS startup failed: '+str(serial)
assert 'RCS_HOSTNAME=AvoryOS' in startup and 'WAITING_SETTLE_EXIT=0' in startup, str(serial)
assert 'SOUND_INITIALIZED=1' in startup and 'AvoryOS Audio Stereo' in startup and 'PLAYBACK_EXIT=0' in startup, str(serial)
print(f'PASS: real udev coldplug discovers HDA and completes {SECONDS}-second '+('PulseAudio-bridge' if PULSE else 'PipeWire')+' playback')
