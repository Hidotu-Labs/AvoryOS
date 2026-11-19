"""Boot the systemd menu entry against a disposable QEMU disk snapshot."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import time

from boot_probe import ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, default=ROOT / 'avoryos-x86_64.iso')
    parser.add_argument('--kernel', type=Path, help='override the kernel embedded in the ISO')
    parser.add_argument('--disk', type=Path, default=ROOT / 'disk.img')
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='tcg')
    parser.add_argument('--desktop', action='store_true',
                        help='exercise the desktop SMP, NVMe and network configuration')
    parser.add_argument('--nvme-queues', type=int, default=1, choices=range(1, 17))
    parser.add_argument('--append', default='', help='extra kernel command-line arguments')
    args = parser.parse_args()

    out = Path(tempfile.mkdtemp(prefix='avory-systemd-boot-'))
    print(f'Boot logs: {out}', flush=True)
    config = out / 'limine.conf'
    entry = (ROOT / 'limine.conf').read_text().split('/AvoryOS (systemd, experimental)\n')[1]
    entry = entry.split('\n/', 1)[0]
    config.write_text('timeout: 0\n/AvoryOS systemd\n' + ''.join(
        line + (' ' + args.append if line.lstrip().startswith('cmdline:') and args.append else '') + '\n'
        for line in entry.splitlines() if not line.lstrip().startswith('module_')
    ))
    iso = out / 'boot.iso'
    serial = out / 'serial.log'
    with (out / 'host.log').open('wb') as host:
        iso_command = [
            'xorriso', '-indev', str(args.iso.resolve()), '-outdev', str(iso),
            '-return_with', 'FAILURE', '32',
            '-map', str(config), '/boot/limine/limine.conf',
        ]
        if args.kernel:
            kernel = out / 'kernel'
            subprocess.run(['strip', '--strip-all', '-o', str(kernel), str(args.kernel)],
                           stdout=host, stderr=host, check=True)
            iso_command.extend(['-map', str(kernel), '/boot/kernel'])
        subprocess.run([*iso_command, '-boot_image', 'any', 'replay'],
                       stdout=host, stderr=host, check=True)
        cpus = '8' if args.desktop else '4'
        nvme = f'nvme,serial=avory-systemd,drive=root,max_ioqpairs={args.nvme_queues}'
        markers = [
            b'Reached target Multi-User System',
            b'Finished Create Static Device Nodes in /dev gracefully.',
            b'Finished Load Kernel Modules.',
            b'Finished Permit User Sessions.',
            b"AvoryOS (type 'install' for installer, or login)",
            b'login: ',
        ]
        network_ready = (b'Started Network Management.',
                         b'Finished Enable Persistent Storage in systemd-networkd.')
        command = [
            os.environ.get('AVORY_TEST_QEMU', 'qemu-system-x86_64'),
            '-M', 'q35', '-accel', args.accel, '-cpu', 'host' if args.accel == 'kvm' else 'max',
            '-smp', cpus, '-m', '2G', '-drive',
            f'if=pflash,format=raw,file={ROOT}/edk2-ovmf/ovmf-code-x86_64.fd,readonly=on',
            '-cdrom', str(iso), '-boot', 'd', '-drive',
            f'file={args.disk.resolve()},format=raw,if=none,id=root,snapshot=on',
            '-device', nvme,
            '-vga', 'none', '-device', 'virtio-vga', '-display', 'none', '-nic', 'none',
            '-serial', f'file:{serial}', '-monitor', 'none', '-no-reboot',
        ]
        if args.desktop:
            command.extend(['-device', 'rtl8139,netdev=testnet',
                            '-netdev', 'user,id=testnet,restrict=on'])
        env = {**os.environ, 'TMPDIR': str(out)}
        with subprocess.Popen(command, stdout=host, stderr=host, env=env) as vm:
            try:
                deadline = time.monotonic() + args.timeout
                while time.monotonic() < deadline and vm.poll() is None:
                    if serial.exists():
                        log = serial.read_bytes()
                        if b'[KPF]' in log:
                            raise SystemExit(f'kernel page fault during systemd boot; see {serial}')
                        boot_complete = all(marker in log for marker in markers)
                        network_started = any(marker in log for marker in network_ready)
                        if boot_complete and (not args.desktop or network_started):
                            print('systemd reached multi-user.target and displayed the AvoryOS login prompt')
                            return
                    time.sleep(0.25)
                status = vm.poll()
                reason = f'timed out after {args.timeout:g}s' if status is None else f'QEMU exited with {status}'
                raise SystemExit(f'systemd boot test failed ({reason}); see {out}')
            finally:
                if vm.poll() is None:
                    vm.terminate()
                    try:
                        vm.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        vm.kill()


if __name__ == '__main__':
    main()
