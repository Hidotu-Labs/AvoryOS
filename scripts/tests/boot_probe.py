"""Shared runner for private native-ABI tests; never opens the user's disk."""
from pathlib import Path
import fcntl
import importlib.util
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]

def run_probe(name, init, marker, setup=None, cpus=4, timeout=60, filesystem="ext2"):
    out = ROOT / 'build' / name
    if out.is_symlink():
        raise RuntimeError('probe output directory is a symlink')
    out.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location('private_boot_helper', ROOT / 'scripts/syscall-restart-boot.py')
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    helper.OUT, helper.INIT = out, init
    with (out / '.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        disk, iso = helper.prepare(extra_root_setup=setup, filesystem=filesystem)
        kernel = Path(os.environ.get('AVORY_TEST_KERNEL', str(ROOT / 'kernel/bin-x86_64/kernel')))
        shutil.copy2(kernel, out / 'iso-root/boot/kernel')
        helper.run(['strip', '--strip-all', out / 'iso-root/boot/kernel'])
        helper.run(['xorriso', '-as', 'mkisofs', '-R', '-r', '-J', '-b',
                    'boot/limine/limine-bios-cd.bin', '-no-emul-boot', '-boot-load-size', '4',
                    '-boot-info-table', '--efi-boot', 'boot/limine/limine-uefi-cd.bin',
                    '-efi-boot-part', '--efi-boot-image', '--protective-msdos-label',
                    out / 'iso-root', '-o', iso])
        helper.run([ROOT / 'limine/limine', 'bios-install', iso])
        serial = out / 'serial.log'
        cmd = ['qemu-system-x86_64', '-M', 'q35', '-cpu', 'host', '-enable-kvm',
               '-smp', str(cpus), '-m', '1G', '-drive',
               f'if=pflash,unit=0,format=raw,file={ROOT}/edk2-ovmf/ovmf-code-x86_64.fd,readonly=on',
               '-cdrom', str(iso), '-boot', 'd', '-drive',
               f'file={disk},format=raw,if=none,id=probe', '-device',
               f'nvme,serial={name[:20]},drive=probe', '-nic', 'none', '-vga', 'none',
               '-device', 'virtio-vga,addr=06.0', '-display', 'none',
               '-serial', f'file:{serial}', '-monitor', 'none', '-no-reboot']
        with (out / 'host.log').open('wb') as output:
            process = subprocess.Popen(cmd, stdout=output, stderr=output)
            try:
                process.wait(timeout=timeout)
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)
        log = serial.read_text(errors='replace')
        if process.returncode or marker not in log:
            raise RuntimeError(f'private regression failed; see {serial}')
        print(log[log.index(name.upper().replace('-', '_') + ': START'):])
    return out
