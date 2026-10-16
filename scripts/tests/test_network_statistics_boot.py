#!/usr/bin/env python3
"""Exercise NetworkManager and live counters in a private, GPU-free guest."""
from pathlib import Path
import importlib.util, subprocess, shutil, os, re, http.server, threading, hashlib
ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'build/network-statistics-boot'
NIC=os.environ.get('AVORY_NETWORK_TEST_NIC','rtl8139')
if NIC not in ('rtl8139','e1000'):raise ValueError('AVORY_NETWORK_TEST_NIC must be rtl8139 or e1000')
if NIC!='rtl8139':OUT=OUT/NIC
OUT.mkdir(parents=True,exist_ok=True)
TREE=ROOT/'build/alpine/rootfs'
spec=importlib.util.spec_from_file_location('boot',ROOT/'scripts/syscall-restart-boot.py')
boot=importlib.util.module_from_spec(spec);spec.loader.exec_module(boot);boot.OUT=OUT
boot.INIT=r'''
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/reboot.h>
int main(void){setvbuf(stdout,0,_IOLBF,0);puts("NET_PROBE: START");pid_t p=fork();if(!p){char *a[]={"/usr/bin/bash","/probe.sh",0};execv(a[0],a);_exit(127);}int s;waitpid(p,&s,0);printf("NET_PROBE: END status=%d\n",s);fflush(0);reboot(RB_POWER_OFF);for(;;)sleep(1);}
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
    for binary in ['bash','busybox','NetworkManager','nmcli','dbus-daemon','dbus-send','gdbus','udevadm','curl']:
        cp('/usr/bin/'+binary)
    cp('/lib64/ld-linux-x86-64.so.2')
    cp('/usr/lib/systemd/systemd-udevd')
    cp('/usr/lib/dbus-daemon-launch-helper')
    def host_binary(source,guest):
        source=Path(source)
        shutil.copy2(source,root/guest.lstrip('/'))
        for lib in re.findall(r'\(NEEDED\).*?\[(.*?)\]', subprocess.check_output(['readelf','-d',source],text=True)):
            cp('/usr/lib/'+lib)
    if os.environ.get('AVORY_NETWORK_ICON_PROBE'):
        host_binary(os.environ['AVORY_NETWORK_ICON_PROBE'],'/bin/icon-probe')
    if os.environ.get('AVORY_NETWORK_ICON_MODULE_PROBE'):
        host_binary(os.environ['AVORY_NETWORK_ICON_MODULE_PROBE'],'/bin/icon-module-probe')
        for directory in ['QtQml','org/kde/networkmanager','org/kde/plasma/networkmanagement']:
            for p in (TREE/'usr/lib/qt6/qml'/directory).rglob('*'):
                if p.is_file():cp('/'+str(p.relative_to(TREE)))
        (root/'baseline').mkdir()
        shutil.copy2(os.environ.get('AVORY_NETWORK_ICON_BASELINE_LIBRARY',resolve('/usr/lib/libplasmanm_internal.so')),root/'baseline/libplasmanm_internal.so')
        host_binary(os.environ['AVORY_NETWORK_ICON_LIBRARY'],'/usr/lib/libplasmanm_internal.so')
    for directory in ['usr/share/dbus-1','etc/dbus-1','usr/lib/udev','etc/udev','etc/NetworkManager']:
        if (TREE/directory).exists():shutil.copytree(TREE/directory,root/directory,symlinks=True,dirs_exist_ok=True)
    for file in ['etc/passwd','etc/group','etc/nsswitch.conf','etc/machine-id','etc/resolv.conf']:
        if (TREE/file).is_file():shutil.copy2(TREE/file,root/file)
    for binary in ['sh','mkdir','sleep','cat','kill','grep','rm','mount','chmod','cp','nohup','pidof','tail','sha256sum']:
        (root/'bin'/binary).symlink_to('/usr/bin/busybox')
    builder=(ROOT/'scripts/setup-arch.sh').read_text()
    rc=builder.split("cat <<'EOF' > \"${ROOTFS_DIR}/etc/init.d/rcS\"\n",1)[1].split('\nEOF',1)[0]
    rc=rc.split('# --- pipewire system instance',1)[0]
    (root/'rcS').write_text(rc)
    (root/'probe.sh').write_text(PROBE.replace('@HTTP_PORT@',str(server.server_port)))
PROBE=r'''#!/bin/sh
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export HOME=/root LC_ALL=C PAGER=cat NM_CLI_PAGER=cat
sh /rcS
sleep 2
nmcli general
nmcli device status
devpath=$(nmcli -g GENERAL.DBUS-PATH device show eth0)
echo DEVICE_PATH=$devpath
gdbus call --system --dest org.freedesktop.NetworkManager --object-path "$devpath" --method org.freedesktop.DBus.Properties.Set org.freedesktop.NetworkManager.Device.Statistics RefreshRateMs '<uint32 1000>'
echo STATS_BEFORE
gdbus call --system --dest org.freedesktop.NetworkManager --object-path "$devpath" --method org.freedesktop.DBus.Properties.GetAll org.freedesktop.NetworkManager.Device.Statistics
curl --max-time 10 -o /tmp/network-payload http://10.0.2.2:@HTTP_PORT@/payload
sha256sum /tmp/network-payload
sleep 3
echo STATS_AFTER
gdbus call --system --dest org.freedesktop.NetworkManager --object-path "$devpath" --method org.freedesktop.DBus.Properties.GetAll org.freedesktop.NetworkManager.Device.Statistics
echo ICON_CHECK
if [ -x /bin/icon-probe ]; then
    /bin/icon-probe
    echo ICON_EXIT=$?
fi
tail -60 /var/log/NetworkManager.log
if [ -x /bin/icon-module-probe ]; then
    LD_LIBRARY_PATH=/baseline QML_IMPORT_PATH=/usr/lib/qt6/qml /bin/icon-module-probe
    echo ICON_BASELINE_EXIT=$?
    QML_IMPORT_PATH=/usr/lib/qt6/qml /bin/icon-module-probe
    echo ICON_MODULE_EXIT=$?
fi
echo NET_PROBE_DONE
kill $(pidof NetworkManager) 2>/dev/null || true
'''
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        data=b'x'*262144
        self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
    def log_message(self,*args):pass
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
threading.Thread(target=server.serve_forever,daemon=True).start()
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
cmd=['qemu-system-x86_64','-M','q35','-cpu','host','-enable-kvm','-smp','4','-m','1G','-drive',f'if=pflash,unit=0,format=raw,file={ROOT}/edk2-ovmf/ovmf-code-x86_64.fd,readonly=on','-cdrom',str(iso),'-boot','d','-drive',f'file={disk},format=raw,if=none,id=probe','-device','nvme,serial=network-probe,drive=probe','-device',f'{NIC},netdev=n0,addr=08.0','-netdev','user,id=n0','-vga','none','-device','virtio-vga,addr=06.0','-display','none','-serial',f'file:{OUT}/serial.log','-monitor','none','-no-reboot']
with (OUT/'host.log').open('wb') as host:
    p=subprocess.Popen(cmd,stdout=host,stderr=host)
    try:p.wait(timeout=100)
    finally:
        if p.poll() is None:p.terminate();p.wait(timeout=5)
log=(OUT/'serial.log').read_text(errors='replace')
print(log[log.index('NET_PROBE: START'):])
server.shutdown();server.server_close()
assert p.returncode==0 and 'NET_PROBE: END status=0' in log
assert hashlib.sha256(b'x'*262144).hexdigest()+'  /tmp/network-payload' in log, 'HTTP payload integrity failure'
samples=re.findall(r"'TxBytes': <uint64 (\d+)>, 'RxBytes': <uint64 (\d+)>",log)
assert len(samples)==2, 'missing NetworkManager statistics samples'
before,after=[tuple(map(int,v)) for v in samples]
assert after[0]>before[0] and after[1]-before[1]>=262144, 'counters did not track download'
if os.environ.get('AVORY_NETWORK_ICON_PROBE'):assert 'ICON_EXIT=0' in log and 'PRIMARY_EMPTY=1' in log
if os.environ.get('AVORY_NETWORK_ICON_MODULE_PROBE'):assert 'ICON_BASELINE_EXIT=1' in log and 'ICON_MODULE_EXIT=0' in log
print(f'PASS: {NIC}: NetworkManager live byte counters, HTTP SHA-256 integrity and optional Plasma external wired icon')
