#!/usr/bin/env python3
"""Prepare/run an automatic GPU oracle on a private guest disk.

Preparation is the default. --run --software never attaches VFIO. Hardware
--run initializes the existing VFIO-bound GPU; authorize that separately when
initialization is known to reboot the host. This script never rebinds a device,
requests a host PCI reset, or opens disk.img. QEMU's ordinary VFIO startup and
shutdown may still reset its assigned device.
"""
from pathlib import Path
import argparse
import fcntl
import importlib.util
import os
import re
import shutil
import shlex
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
TREE = ROOT/'build/alpine/rootfs'
OUT = ROOT/'build/gpu-pipeline-boot'
INIT = r'''
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/reboot.h>
#include <sys/wait.h>
int main(void) {
 setvbuf(stdout,NULL,_IOLBF,0);
 int fd=open("/proc/gpu_debug",O_WRONLY);
 if(fd>=0){write(fd,"0\n",2);close(fd);}
 puts("GPU_PIPELINE_BOOT: START");
 pid_t child=fork();
 if(child==0){
  char *argv[]={"/bin/test_gpu_pipeline",@SOFTWARE@NULL};
  execv(argv[0],argv);perror("GPU_PIPELINE_BOOT exec");_exit(127);
 }
 int status=0;
 if(child<0 || waitpid(child,&status,0)<0){perror("GPU_PIPELINE_BOOT wait");return 2;}
 printf("GPU_PIPELINE_BOOT: END exit=%d signal=%d\n",
        WIFEXITED(status)?WEXITSTATUS(status):-1,WIFSIGNALED(status)?WTERMSIG(status):0);
 puts("GPU_PIPELINE_BOOT: shutting down guest drivers");
 fflush(NULL);
 reboot(RB_POWER_OFF);
 perror("GPU_PIPELINE_BOOT power-off");
 for(;;)sleep(60);
}
'''

def run(argv, **kwargs):
    return subprocess.run([str(x) for x in argv], check=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kwargs)

class SerialMirror:
    """Append this run's serial bytes to a locked public log and/or terminal.

    Result classification still reads the private per-run log, so an older
    failed boot in the public history cannot fail the current run.
    """
    def __init__(self, source, target=None, live=False):
        self.source, self.live, self.offset = source, live, 0
        self.stream = None
        if target is not None:
            target = Path(target).absolute()
            if target == source.absolute() or target.is_symlink():
                raise RuntimeError('public serial log must be a separate regular file')
            target.parent.mkdir(parents=True, exist_ok=True)
            stream = target.open('ab', buffering=0)
            try:
                fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                stream.close()
                raise RuntimeError(f'another launch owns serial log {target}')
            self.stream = stream
            marker = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
            stream.write(f'\n[GPU-BOOT] {marker} serial={source}\n'.encode())

    def copy(self):
        if (self.stream is None and not self.live) or not self.source.exists():
            return
        with self.source.open('rb') as reader:
            reader.seek(self.offset)
            data = reader.read()
        self.offset += len(data)
        if self.stream is not None:
            self.stream.write(data)
        if self.live and data:
            print(data.decode(errors='replace'), end='', flush=True)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        try:
            self.copy()
        finally:
            if self.stream is not None:
                os.fsync(self.stream.fileno())
                self.stream.close()

def prepare(software, *, compiler=None, kernel=None, iso=None, oracle=None,
            cmdline=None, firmware_extra=None, firmware_exclusive_prefix=None,
            extra_files=None, extra_runtime=()):
    if OUT.is_symlink(): raise RuntimeError('diagnostic output directory is a symlink')
    OUT.mkdir(parents=True, exist_ok=True)
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():continue
        try:cmd=(proc/'cmdline').read_bytes()
        except OSError:continue
        if b'qemu-system' in cmd and str(OUT/'root.img').encode() in cmd:
            raise RuntimeError('private diagnostic VM is running; refusing to rebuild its disk')
    runtime = importlib.util.spec_from_file_location('image_runtime', ROOT/'scripts/image-runtime.py')
    mod = importlib.util.module_from_spec(runtime); runtime.loader.exec_module(mod)
    def source(path):
        def readlink(guest):
            p = TREE/guest.lstrip('/')
            return os.readlink(p) if p.is_symlink() else None
        return TREE/mod.resolve(path, readlink).lstrip('/')
    if oracle is None:
        run(['make', 'gpu-diagnostics'], cwd=ROOT)
        oracle=ROOT/'userland/test_gpu_pipeline.elf'
    with tempfile.TemporaryDirectory(prefix='avory-gpu-boot-') as temp:
        work=Path(temp); tree=work/'root'; tree.mkdir()
        for name in ('bin','sbin','dev/dri','proc','sys','run','tmp','root','usr/bin','usr/lib','lib64','lib/firmware','etc','home'):
            (tree/name).mkdir(parents=True,exist_ok=True)
        shutil.copyfile(oracle,tree/'bin/test_gpu_pipeline')
        os.chmod(tree/'bin/test_gpu_pipeline',0o755)
        copied=set()
        def library(path):
            if path in copied:return
            original=source(path)
            if not original.is_file():raise RuntimeError(f'missing guest runtime {path}')
            dest=tree/path.lstrip('/');dest.parent.mkdir(parents=True,exist_ok=True)
            shutil.copyfile(original,dest);os.chmod(dest,0o755);copied.add(path)
            data=run(['readelf','-d',original]).stdout.decode()
            for name in re.findall(r'\(NEEDED\).*\[([^]]+)\]',data):
                for directory in ('/usr/lib/','/lib/','/lib64/'):
                    if source(directory+name).is_file():library(directory+name);break
                else:raise RuntimeError(f'cannot resolve runtime dependency {name}')
        library('/lib64/ld-linux-x86-64.so.2')
        for runtime_path in extra_runtime:
            library(runtime_path)
        for guest_path, local_path in (extra_files or {}).items():
            guest=Path(guest_path)
            if not guest.is_absolute() or '..' in guest.parts:
                raise RuntimeError('extra guest file must have an absolute non-traversing path')
            destination=tree/str(guest).lstrip('/')
            destination.parent.mkdir(parents=True,exist_ok=True)
            shutil.copyfile(local_path,destination)
            os.chmod(destination,0o755)
        # Include dlopen entry points in addition to ELF NEEDED dependencies.
        for name in ('/usr/lib/libEGL.so.1','/usr/lib/libGLESv2.so.2',
                     '/usr/lib/libEGL_mesa.so.0','/usr/lib/libgbm.so.1',
                     '/usr/lib/dri/radeonsi_dri.so','/usr/lib/dri/swrast_dri.so'):
            library(name)
        data=run(['readelf','-d',oracle]).stdout.decode()
        for name in re.findall(r'\(NEEDED\).*\[([^]]+)\]',data):library('/usr/lib/'+name)
        for path in ('usr/share/glvnd/egl_vendor.d','usr/share/drirc.d'):
            if (TREE/path).is_dir():shutil.copytree(TREE/path,tree/path,dirs_exist_ok=True)
        fw=ROOT/'build/firmware/lib/firmware'
        if fw.is_dir():shutil.copytree(fw,tree/'lib/firmware',dirs_exist_ok=True)
        if firmware_exclusive_prefix:
            prefix=Path(firmware_exclusive_prefix)
            if prefix.is_absolute() or '..' in prefix.parts or not prefix.parts:
                raise RuntimeError('invalid exclusive firmware prefix')
            existing=tree/'lib/firmware'/prefix
            if existing.is_symlink():
                existing.unlink()
            elif existing.exists():
                shutil.rmtree(existing)
        if firmware_extra:
            shutil.copytree(firmware_extra,tree/'lib/firmware',dirs_exist_ok=True)
        (tree/'lib/firmware/test_fw.bin').write_bytes(
            bytes((i * 7 + 3) & 0xff for i in range(4096)))
        (tree/'root/.cache').mkdir(parents=True)
        (work/'init.c').write_text(INIT.replace('@SOFTWARE@','"--software",' if software else ''))
        # The rootfs now uses glibc; this tiny static init needs no target
        # libraries. Allow an explicit compiler, otherwise use the host CC.
        compiler=shlex.split(compiler or os.environ.get('CC', 'cc'))
        run([*compiler,'-static','-O2','-Wall','-Wextra','-Werror',work/'init.c','-o',tree/'bin/gpu-probe-init'])
        disk=OUT/'root.img'
        with disk.open('wb') as f:f.truncate(1024*1024**2)
        run(['mke2fs','-q','-t','ext2','-F','-d',tree,disk])
        boot=work/'boot'
        run(['xorriso','-osirrox','on','-indev',iso or ROOT/'avoryos-x86_64.iso','-extract','/',boot])
        for directory,_,files in os.walk(boot):
            os.chmod(directory,0o755)
            for file in files:os.chmod(Path(directory)/file,0o644)
        if kernel:
            shutil.copyfile(kernel, boot/'boot/kernel')
        opts='kpi_amdgpu=0' if software else 'kpi_amdgpu=1 kpi_prime_offload=1 amdgpu.runpm=0 amdgpu.ppfeaturemask=0xfff73fff drm.debug=0x4'
        if cmdline is not None: opts=cmdline
        (boot/'boot/limine/limine.conf').write_text(
            'timeout: 0\n/GPU pipeline diagnostic\n    protocol: limine\n    path: boot():/boot/kernel\n'
            f'    cmdline: {opts} init=/bin/gpu-probe-init\n    resolution: 1280x800\n')
        iso=OUT/'boot.iso'
        run(['xorriso','-as','mkisofs','-R','-r','-J','-b','boot/limine/limine-bios-cd.bin',
             '-no-emul-boot','-boot-load-size','4','-boot-info-table',
             '--efi-boot','boot/limine/limine-uefi-cd.bin','-efi-boot-part',
             '--efi-boot-image','--protective-msdos-label',boot,'-o',iso])
        run([ROOT/'limine/limine','bios-install',iso])
        (OUT/'mode.txt').write_text('software\n' if software else 'hardware\n')
    print(f'Prepared {OUT}/boot.iso and private root.img ({len(copied)} runtime paths).',flush=True)

def qemu_failure_output(host):
    with host.open('rb') as stream:
        stream.seek(0,2);stream.seek(max(0,stream.tell()-8192))
        detail=stream.read().decode(errors='replace').strip()
    if detail:print('QEMU failure output:\n'+detail,flush=True)
    if 'invalid PCI interrupt pin 255' in detail or 'Unable to power on device, stuck in D3' in detail:
        return '; VFIO could not access/power the GPU during launch: stop retries and cold boot the host before another GPU test'
    return ''

def execute(args, *, vfio_devices=None, label=None):
    if not args.software and vfio_devices is None:
        driver=Path('/sys/bus/pci/devices')/args.gpu/'driver'
        if not driver.is_symlink() or driver.resolve().name!='vfio-pci':
            raise RuntimeError('GPU must already be bound to vfio-pci; no rebinding is performed')
        for proc in Path('/proc').iterdir():
            if not proc.name.isdigit():continue
            try:cmd=(proc/'cmdline').read_bytes()
            except OSError:continue
            if b'qemu-system' in cmd and (b'host='+args.gpu.encode()) in cmd:
                raise RuntimeError('GPU is attached to an existing QEMU VM; refusing a second owner')
    prefix=label or ('software' if args.software else 'hardware')
    serial=OUT/(prefix+'-serial.log')
    host=OUT/(prefix+'-host.log')
    cmd=['qemu-system-x86_64','-M','q35','-m','4G','-cpu','host','-enable-kvm','-smp',str(args.cpus),
         '-drive',f'if=pflash,unit=0,format=raw,file={ROOT}/edk2-ovmf/ovmf-code-x86_64.fd,readonly=on',
         '-cdrom',str(OUT/'boot.iso'),'-boot','d',
         # Preserve the prepared filesystem across boots. The oracle writes
         # cache/temp data; a successful process result is not a filesystem
         # integrity check. QEMU discards this per-run overlay on exit.
         '-drive',f'file={OUT}/root.img,format=raw,if=none,id=probe,snapshot=on',
         '-device','nvme,serial=gpu-oracle,drive=probe','-vga','none',
         '-device','virtio-vga,addr=06.0,xres=1280,yres=800','-display',
         (getattr(args,'gtk_options',None) or 'gtk,zoom-to-fit=on,grab-on-hover=on,window-close=off') if getattr(args,'display','none')=='gtk' else 'none',
         '-serial',f'file:{serial}','-monitor','none','-no-reboot']
    if not args.software and vfio_devices is None:
        dev=f'vfio-pci,host={args.gpu},addr=03.0,rombar=1'
        rom=ROOT/'build/vfio/vbios.rom'
        if rom.is_file():dev+=f',romfile={rom}'
        cmd+=['-device',dev]
    if vfio_devices is not None:
        for dev in vfio_devices: cmd+=['-device',dev]
    if args.edu:
        cmd+=['-device','edu']
    cmd+=shlex.split(getattr(args,'qemu_extra',''))
    serial.unlink(missing_ok=True)
    public_log = getattr(args, 'serial_log', None)
    if public_log is not None and Path(public_log).absolute() == host.absolute():
        raise RuntimeError('serial log must be separate from the QEMU host log')
    with SerialMirror(serial, public_log, getattr(args, 'live_serial', False)) as mirror, host.open('wb') as output:
        process=subprocess.Popen(cmd,stdout=output,stderr=output)
        try:
            deadline=time.monotonic()+args.timeout
            shutdown_deadline=None
            while time.monotonic()<deadline:
                mirror.copy()
                log=serial.read_text(errors='replace') if serial.exists() else ''
                if getattr(args, 'observe_serial', None):
                    args.observe_serial(log)
                if '[PANIC]' in log or '[KPF] THREAD FAULT' in log:
                    raise RuntimeError(f'guest kernel fault; inspect {serial}')
                if args.strict_boot_tests and 'LinuxKPI: boot self-tests timed out' in log:
                    raise RuntimeError(f'LinuxKPI boot self-tests timed out; inspect {serial}')
                end=re.search(r'GPU_PIPELINE_BOOT: END exit=(-?\d+) signal=(\d+)',log)
                # The result marker precedes driver shutdown. Do not kill
                # QEMU here: give native reboot(POWER_OFF) time to dispatch
                # PCI shutdown while guest interrupts/scheduling still work.
                if end and shutdown_deadline is None:
                    shutdown_deadline=time.monotonic()+30
                    deadline=max(deadline,shutdown_deadline)
                status=process.poll()
                if status is not None:
                    if not end:
                        raise RuntimeError(f'VM exited before diagnostic result (QEMU exit={status}); inspect {serial} and {host}')
                    if 'ERROR: no AMD render node; checks did not run' in log:
                        print('AMD initialization failed before rendering; guest failure records:',flush=True)
                        for line in log.splitlines():
                            if any(key in line for key in ('PSP create ring failed',
                                    'PSP firmware loading failed','KCQ enable failed',
                                    ' test failed (','hw_init of IP block',
                                    'amdgpu probe ','Fatal error during GPU init')):
                                print(line,flush=True)
                    oracle=log[log.index('GPU_PIPELINE_BOOT: START'):]
                    if not getattr(args, 'live_serial', False):
                        print(oracle,flush=True)
                    print(f'Full logs: {serial} and {host}',flush=True)
                    clean=re.sub(r'\x1b\[[0-9;]*m','',log)
                    failures=re.findall(r'^\[\s*FAIL\s*\]\s+LinuxKPI:.*$',clean,re.M)
                    if failures:
                        print('Boot self-test failures:\n'+'\n'.join(failures),flush=True)
                        if args.strict_boot_tests:
                            raise RuntimeError('LinuxKPI boot self-tests failed')
                    if status!=0:
                        raise RuntimeError(f'VM shutdown failed (QEMU exit={status}); inspect {host}')
                    if int(end[1])!=0 or end[2]!='0':
                        hint=qemu_failure_output(host)
                        if hint:print(hint.removeprefix('; '),flush=True)
                    return int(end[1]) if end[2]=='0' and int(end[1])>=0 else 2
                if shutdown_deadline is not None and time.monotonic()>=shutdown_deadline:
                    raise RuntimeError(f'guest driver shutdown did not finish within 30 seconds; result is saved in {serial}')
                time.sleep(.25)
            raise RuntimeError(f'diagnostic did not finish; inspect {serial} and {host}')
        except RuntimeError as error:
            hint=qemu_failure_output(host)
            if hint:raise RuntimeError(str(error)+hint) from error
            raise
        finally:
            if process.poll() is None:
                process.terminate()
                try:process.wait(timeout=5)
                except subprocess.TimeoutExpired:process.kill();process.wait()

def main():
    global OUT
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--software',action='store_true')
    parser.add_argument('--run',action='store_true',help='boot the prepared diagnostic after preparing it')
    parser.add_argument('--gpu',default='0000:0e:00.0')
    parser.add_argument('--timeout',type=int,default=180)
    parser.add_argument('--cpus',type=int,default=8)
    parser.add_argument('--edu',action='store_true',help='attach QEMU EDU for virtual PCI/MSI self-tests')
    parser.add_argument('--output-dir',type=Path,default=OUT)
    parser.add_argument('--cc',help='compiler command for the static diagnostic init')
    parser.add_argument('--kernel',type=Path,help='kernel to test in a copy of the boot ISO')
    parser.add_argument('--iso',type=Path,help='source ISO, copied into the private fixture')
    parser.add_argument('--oracle',type=Path,help='existing diagnostic ELF; skip rebuilding userland')
    parser.add_argument('--strict-boot-tests',action='store_true',help='fail on LinuxKPI boot self-test failures')
    args=parser.parse_args()
    if not re.fullmatch(r'[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]',args.gpu):parser.error('invalid GPU BDF')
    if args.timeout<=0:parser.error('--timeout must be positive')
    if args.cpus<=0:parser.error('--cpus must be positive')
    OUT=args.output_dir.absolute()
    if OUT.is_symlink():raise RuntimeError('diagnostic output directory is a symlink')
    OUT.mkdir(parents=True,exist_ok=True)
    with (OUT/'prepare.lock').open('a') as lock:
        try:fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        except BlockingIOError:raise RuntimeError('another diagnostic preparation/run is active')
        prepare(args.software,compiler=args.cc,kernel=args.kernel,iso=args.iso,oracle=args.oracle)
        return execute(args) if args.run else 0

if __name__=='__main__':
    try:raise SystemExit(main())
    except (RuntimeError,subprocess.CalledProcessError) as error:
        if isinstance(error,subprocess.CalledProcessError):print(error.stderr.decode(errors='replace'))
        raise SystemExit(str(error))
