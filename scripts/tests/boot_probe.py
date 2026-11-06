"""Shared runner for private native-ABI tests; never opens the user's disk."""
from pathlib import Path
import fcntl
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def cmd_run(argv):
    subprocess.run([str(x) for x in argv], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def prepare_guest_env(out, init, extra_root_setup=None, kernel_cmdline=None, filesystem="ext2", disk_mib=128):
    disk = out / 'root.img'
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():
            continue
        try:
            cmd = (proc / 'cmdline').read_bytes()
        except OSError:
            continue
        if b'qemu-system' in cmd and str(disk).encode() in cmd:
            raise RuntimeError('private test VM is running; refusing to rebuild its disk')

    root = out / 'root'
    shutil.rmtree(root, ignore_errors=True)
    for name in ('bin', 'sbin', 'dev/dri', 'proc', 'sys', 'run', 'tmp', 'etc',
                 'root', 'usr/bin', 'usr/lib', 'home', 'lib/firmware'):
        (root / name).mkdir(parents=True, exist_ok=True)

    (out / 'init.c').write_text(init)
    compiler = os.environ.get('AVORY_TEST_CC')
    if not compiler:
        local = ROOT / 'toolchain/x86_64-linux-musl/bin/x86_64-linux-musl-gcc'
        compiler = str(local) if local.is_file() else (
            shutil.which('x86_64-linux-musl-gcc') or shutil.which('musl-gcc'))
    if not compiler:
        raise RuntimeError('native boot tests require a musl compiler; set AVORY_TEST_CC')

    cmd_run([compiler, '-O2', '-static', out / 'init.c', '-o', root / 'bin/probe-init'])

    if extra_root_setup is not None:
        extra_root_setup(root)

    with disk.open('wb') as stream:
        stream.truncate(disk_mib * 1024**2)

    if filesystem not in ('ext2', 'ext3', 'ext4'):
        raise ValueError('unsupported probe filesystem')
    cmd_run(['mke2fs', '-q', '-t', filesystem, '-F', '-d', root, disk])

    boot = out / 'iso-root'
    shutil.rmtree(boot, ignore_errors=True)
    cmd_run(['xorriso', '-osirrox', 'on', '-indev', ROOT / 'avoryos-x86_64.iso',
             '-extract', '/', boot])

    for directory, _, files in os.walk(boot):
        os.chmod(directory, 0o755)
        for name in files:
            os.chmod(Path(directory) / name, 0o644)

    (boot / 'boot/limine/limine.conf').write_text(
        'timeout: 0\n/Test probe\n    protocol: limine\n'
        '    path: boot():/boot/kernel\n'
        f'    cmdline: {kernel_cmdline or "kpi_amdgpu=0 init=/bin/probe-init"}\n'
        '    resolution: 1280x800\n')

    kernel = Path(os.environ.get('AVORY_TEST_KERNEL', str(ROOT / 'kernel/bin-x86_64/kernel')))
    shutil.copy2(kernel, boot / 'boot/kernel')
    cmd_run(['strip', '--strip-all', boot / 'boot/kernel'])

    iso = out / 'boot.iso'
    cmd_run(['xorriso', '-as', 'mkisofs', '-R', '-r', '-J', '-b',
             'boot/limine/limine-bios-cd.bin', '-no-emul-boot', '-boot-load-size', '4',
             '-boot-info-table', '--efi-boot', 'boot/limine/limine-uefi-cd.bin',
             '-efi-boot-part', '--efi-boot-image', '--protective-msdos-label',
             boot, '-o', iso])
    cmd_run([ROOT / 'limine/limine', 'bios-install', iso])

    return disk, iso


def run_probe(name, init, marker, setup=None, cpus=4, timeout=60, filesystem="ext2",
              gpu="virtio-vga", display="none", qemu_env=None, accel="kvm",
              disk_mib=128, qemu_args=(), started=None, qmp_stdio=False, memory='1G',
              prepare_disk=None, kernel_cmdline=None):
    out = ROOT / 'build' / name
    if out.is_symlink():
        raise RuntimeError('probe output directory is a symlink')
    out.mkdir(parents=True, exist_ok=True)

    with (out / '.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        disk, iso = prepare_guest_env(
            out=out, init=init, extra_root_setup=setup,
            filesystem=filesystem, disk_mib=disk_mib, kernel_cmdline=kernel_cmdline
        )

        if prepare_disk is not None:
            prepare_disk(disk, out)

        serial = out / 'serial.log'
        cmd = [os.environ.get('AVORY_TEST_QEMU', 'qemu-system-x86_64'), '-M', 'q35',
               '-cpu', 'host' if accel == 'kvm' else 'max', '-accel', accel,
               '-smp', str(cpus), '-m', memory, '-drive',
               f'if=pflash,unit=0,format=raw,file={ROOT}/edk2-ovmf/ovmf-code-x86_64.fd,readonly=on',
               '-cdrom', str(iso), '-boot', 'd', '-drive',
               f'file={disk},format=raw,if=none,id=probe', '-device',
               f'nvme,serial={name[:20]},drive=probe', '-nic', 'none', '-vga', 'none',
               '-device', f'{gpu},addr=06.0', '-display', display,
               '-serial', f'file:{serial}', '-monitor', 'none', '-no-reboot']
        cmd.extend(qemu_args)
        if qmp_stdio:
            cmd.extend(["-qmp", "stdio"])

        with (out / 'host.log').open('wb') as output:
            process = subprocess.Popen(cmd, stdout=subprocess.PIPE if qmp_stdio else output,
                                       stdin=subprocess.PIPE if qmp_stdio else None,
                                       stderr=output, env=qemu_env)
            try:
                if started is not None:
                    started(process, out)
                process.wait(timeout=timeout)
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)

        log = serial.read_text(errors='replace')
        if process.returncode or marker not in log:
            raise RuntimeError(f'private regression failed (QEMU exit {process.returncode}); see {serial}')
        marker_prefix = name.upper().replace('-', '_') + ': START'
        if marker_prefix in log:
            print(log[log.index(marker_prefix):])
    return out
