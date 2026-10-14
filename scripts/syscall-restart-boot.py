#!/usr/bin/env python3
"""Run the native signal/restart regression in a private, GPU-free KVM guest.

Requires a rebuilt avoryos-x86_64.iso and test_syscall_restart.elf. Never opens
disk.img, attaches VFIO, rebinds a device, or resets host hardware. The guest
powers off through its normal driver shutdown path after reporting results.
"""
from pathlib import Path
import argparse
import fcntl
import os
import re
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/syscall-restart-boot'
INIT = r'''
#include <stdio.h>
#include <unistd.h>
#include <sys/reboot.h>
#include <sys/wait.h>
int main(void) {
 setvbuf(stdout,NULL,_IOLBF,0);
 puts("RESTART_BOOT: START");
 pid_t child=fork();
 if(child==0){
  char *argv[]={"/bin/test_syscall_restart",NULL};
  execv(argv[0],argv);perror("RESTART_BOOT exec");_exit(127);
 }
 int status=0;
 if(child<0 || waitpid(child,&status,0)<0){perror("RESTART_BOOT wait");return 2;}
 printf("RESTART_BOOT: END exit=%d signal=%d\n",
        WIFEXITED(status)?WEXITSTATUS(status):-1,WIFSIGNALED(status)?WTERMSIG(status):0);
 puts("RESTART_BOOT: shutting down guest drivers");
 fflush(NULL);
 reboot(RB_POWER_OFF);
 perror("RESTART_BOOT power-off");
 for(;;)sleep(60);
}
'''


def run(argv):
    subprocess.run([str(x) for x in argv], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def prepare(extra_root_setup=None, kernel_cmdline=None):
    disk = OUT / 'root.img'
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():
            continue
        try:
            cmd = (proc / 'cmdline').read_bytes()
        except OSError:
            continue
        if b'qemu-system' in cmd and str(disk).encode() in cmd:
            raise RuntimeError('private test VM is running; refusing to rebuild its disk')
    root = OUT / 'root'
    shutil.rmtree(root, ignore_errors=True)
    for name in ('bin', 'sbin', 'dev/dri', 'proc', 'sys', 'run', 'tmp', 'etc',
                 'root', 'usr/bin', 'usr/lib', 'home', 'lib/firmware'):
        (root / name).mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / 'userland/test_syscall_restart.elf',
                 root / 'bin/test_syscall_restart')
    (OUT / 'init.c').write_text(INIT)
    run([ROOT / 'toolchain/x86_64-linux-musl/bin/x86_64-linux-musl-gcc',
         '-O2', '-static', OUT / 'init.c', '-o', root / 'bin/probe-init'])
    if extra_root_setup is not None:
        extra_root_setup(root)
    with disk.open('wb') as stream:
        stream.truncate(128 * 1024**2)
    run(['mke2fs', '-q', '-t', 'ext2', '-F', '-d', root, disk])
    boot = OUT / 'iso-root'
    shutil.rmtree(boot, ignore_errors=True)
    run(['xorriso', '-osirrox', 'on', '-indev', ROOT / 'avoryos-x86_64.iso',
         '-extract', '/', boot])
    for directory, _, files in os.walk(boot):
        os.chmod(directory, 0o755)
        for name in files:
            os.chmod(Path(directory) / name, 0o644)
    (boot / 'boot/limine/limine.conf').write_text(
        'timeout: 0\n/Restart regression\n    protocol: limine\n'
        '    path: boot():/boot/kernel\n'
        f'    cmdline: {kernel_cmdline or "kpi_amdgpu=0 init=/bin/probe-init"}\n'
        '    resolution: 1280x800\n')
    iso = OUT / 'boot.iso'
    run(['xorriso', '-as', 'mkisofs', '-R', '-r', '-J', '-b',
         'boot/limine/limine-bios-cd.bin', '-no-emul-boot', '-boot-load-size',
         '4', '-boot-info-table', '--efi-boot', 'boot/limine/limine-uefi-cd.bin',
         '-efi-boot-part', '--efi-boot-image', '--protective-msdos-label',
         boot, '-o', iso])
    run([ROOT / 'limine/limine', 'bios-install', iso])
    return disk, iso


def boot_test(cpus, disk, iso, extra_qemu_args=(), launch_prefix=(),
              append_serial=False):
    serial = OUT / f'kvm{cpus}-serial.log'
    host = OUT / f'kvm{cpus}-host.log'
    serial.unlink(missing_ok=True)
    cmd = ['qemu-system-x86_64', '-M', 'q35', '-cpu', 'host', '-enable-kvm',
           '-smp', str(cpus), '-m', '2G', '-drive',
           f'if=pflash,unit=0,format=raw,file={ROOT}/edk2-ovmf/ovmf-code-x86_64.fd,readonly=on',
           '-cdrom', str(iso), '-boot', 'd', '-drive',
           f'file={disk},format=raw,if=none,id=probe', '-device',
           'nvme,serial=restart-probe,drive=probe', '-vga', 'none', '-device',
           'virtio-vga,addr=06.0,xres=1280,yres=800', '-display', 'none',
           '-serial', f'file:{serial}', '-monitor', 'none', '-no-reboot']
    cmd.extend(extra_qemu_args)
    if append_serial:
        cmd[cmd.index('-serial') + 1] = 'chardev:probe-log'
        cmd.extend(['-chardev',
                    f'socket,id=probe-log,path={OUT}/serial.sock,server=on,wait=off,'
                    f'logfile={serial},logappend=on'])
    cmd = [str(arg) for arg in launch_prefix] + cmd
    with host.open('wb') as output:
        process = subprocess.Popen(cmd, stdout=output, stderr=output)
        try:
            deadline = time.monotonic() + 90
            while process.poll() is None and time.monotonic() < deadline:
                time.sleep(.25)
            if process.poll() is None:
                raise RuntimeError(f'private regression boot timed out; see {serial}')
            log = serial.read_text(errors='replace')
            result = re.search(r'RESTART_BOOT: END exit=(\d+) signal=(\d+)', log)
            if not (result and result.groups() == ('0', '0') and
                    process.returncode == 0 and
                    'PASS: native syscall restart and signal-frame ABI' in log):
                raise RuntimeError(f'native regression failed; see {serial} and {host}')
            print(log[log.index('RESTART_BOOT: START'):])
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cpus', type=int, choices=(1, 8), default=8)
    args = parser.parse_args()
    if OUT.is_symlink():
        raise RuntimeError('test output directory is a symlink')
    OUT.mkdir(parents=True, exist_ok=True)
    with (OUT / '.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        boot_test(args.cpus, *prepare())


if __name__ == '__main__':
    main()
