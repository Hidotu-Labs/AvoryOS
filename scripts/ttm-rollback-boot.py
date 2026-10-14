#!/usr/bin/env python3
"""Exercise real TTM rollback in a private GPU-free AvoryOS KVM guest.

--fault-control uses GDB to deliberately remove a fake TTM manager and set
priority 2 before an LRU move. This validates the 0x60 fault reporter, not the
cause of a hardware failure. The disposable guest is terminated afterwards.
No VFIO devices or disk.img are attached. Requires the rebuilt ISO and gdb.
"""
import argparse
import fcntl
import importlib.util
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    'private_boot', ROOT / 'scripts/syscall-restart-boot.py')
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)
MARKER = 'TTM partial-init rollback (4 stages, deferred deletes drained)'


def stage_firmware(root):
    shutil.copytree(ROOT / 'build/firmware/lib/firmware', root / 'lib/firmware',
                    dirs_exist_ok=True)
    (root / 'lib/firmware/test_fw.bin').write_bytes(
        bytes((i * 7 + 3) & 255 for i in range(4096)))


def fault_control(cpus, disk, iso):
    # Bind briefly to choose an unused local port; QEMU exposes only localhost.
    with socket.socket() as connection:
        connection.bind(('127.0.0.1', 0))
        port = connection.getsockname()[1]
    serial = boot.OUT / f'kvm{cpus}-serial.log'
    serial.unlink(missing_ok=True)
    commands = boot.OUT / 'inject.gdb'
    commands.write_text(
        f'set pagination off\nset confirm off\n'
        f'target remote 127.0.0.1:{port}\n'
        'hbreak ttm_resource_move_to_lru_tail\n'
        'continue\n'
        'set $res = (struct ttm_resource *)$rdi\n'
        'printf "CONTROL: entry-rsp=0x%016lx\\n", $rsp\n'
        'printf "CONTROL: removing fake manager type=%u, priority=2\\n", $res->mem_type\n'
        'set $res->bo->priority = 2\n'
        'set $res->bo->bdev->man_drv[$res->mem_type] = 0\n'
        'disable breakpoints\ndetach\nquit\n')
    cmd = ['qemu-system-x86_64', '-M', 'q35', '-cpu', 'host', '-enable-kvm',
           '-smp', str(cpus), '-m', '2G', '-drive',
           f'if=pflash,unit=0,format=raw,file={ROOT}/edk2-ovmf/ovmf-code-x86_64.fd,readonly=on',
           '-cdrom', str(iso), '-boot', 'd', '-drive',
           f'file={disk},format=raw,if=none,id=probe', '-device',
           'nvme,serial=ttm-control,drive=probe', '-vga', 'none', '-device',
           'virtio-vga,addr=06.0,xres=1280,yres=800', '-display', 'none',
           '-serial', f'file:{serial}', '-monitor', 'none', '-no-reboot',
           '-S', '-gdb', f'tcp:127.0.0.1:{port}']
    with (boot.OUT / f'kvm{cpus}-host.log').open('wb') as output:
        process = subprocess.Popen(cmd, stdout=output, stderr=output)
        try:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                try:
                    with socket.create_connection(('127.0.0.1', port), .1):
                        break
                except OSError:
                    if process.poll() is not None:
                        raise RuntimeError('private control VM failed to start')
                    time.sleep(.05)
            with (boot.OUT / 'gdb.log').open('wb') as debugger:
                subprocess.run(['gdb', '-q', '-batch', ROOT / 'kernel/bin-x86_64/kernel',
                                '-x', commands], stdout=debugger,
                               stderr=subprocess.STDOUT, timeout=45, check=True)
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                log = serial.read_text(errors='replace') if serial.exists() else ''
                if '[KPF] thread fault report complete' in log:
                    break
                time.sleep(.1)
            required = ('cr2=0x0000000000000060', 'THREAD FAULT:',
                        'RSI=0x0000000000000000', 'STACK SCAN',
                        '[KPF] thread fault report complete')
            if not all(marker in log for marker in required):
                raise RuntimeError(f'fault-control report incomplete; see {serial}')
            control = re.search(r'entry-rsp=(0x[0-9a-fA-F]+)',
                                (boot.OUT / 'gdb.log').read_text())
            reported = re.search(r'interrupted RSP=(0x[0-9a-fA-F]+)', log)
            if not (control and reported and
                    int(control.group(1), 16) == int(reported.group(1), 16)):
                raise RuntimeError('fault reporter did not preserve the interrupted RSP')
            print('PASS: deliberate fake-manager removal produced 0x60; '
                  'registers, correct interrupted RSP and stack candidates captured')
            print(f'Fault-control serial log: {serial}')
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
    parser.add_argument('--fault-control', action='store_true')
    args = parser.parse_args()
    boot.OUT = ROOT / 'build' / ('ttm-fault-control' if args.fault_control
                               else 'ttm-rollback-boot')
    if boot.OUT.is_symlink():
        raise RuntimeError('private output directory is a symlink')
    boot.OUT.mkdir(parents=True, exist_ok=True)
    with (boot.OUT / '.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        disk, iso = boot.prepare(stage_firmware)
        if args.fault_control:
            fault_control(args.cpus, disk, iso)
        else:
            launcher = [sys.executable, ROOT / 'scripts/qemu-prime-log.py',
                        '--output-root', boot.OUT / 'launch-evidence',
                        '--serial-log', boot.OUT / f'kvm{args.cpus}-serial.log', '--']
            boot.boot_test(args.cpus, disk, iso, ['-device', 'edu,addr=07.0'],
                           launch_prefix=launcher, append_serial=True)
            serial = boot.OUT / f'kvm{args.cpus}-serial.log'
            log = serial.read_text(errors='replace')
            if (MARKER not in log or 'TTM suite complete' not in log or
                    'devres mixed action/resource LIFO' not in log or
                    'devres nested closed group bounds and device isolation' not in log or
                    '[FAIL]' in log or 'THREAD FAULT:' in log or
                    'devmodel suite complete' not in log or
                    'fw suite complete' not in log or
                    'sysfs lifecycle PMM stable' not in log):
                raise RuntimeError(f'TTM rollback failed; see {serial}')
            print(f'PASS: real TTM rollback and orderly guest shutdown ({args.cpus} CPUs)')
            print(f'Rollback serial log: {serial}')


if __name__ == '__main__':
    main()
