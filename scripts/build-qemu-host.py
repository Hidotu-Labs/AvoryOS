#!/usr/bin/env python3
"""Build pinned QEMU with main-thread Virgl query notifications, locally only."""
import fcntl
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
VERSION = '11.1.2'
SHA256 = '731b5681e4bb18be313231579b8efd0296c5b015fa36dc533874b639ba838016'
SOURCE = ROOT / 'build' / ('qemu-' + VERSION)
BUILD = ROOT / 'build/qemu-host-build'
PREFIX = ROOT / 'build/qemu-host'
PATCH = ROOT / 'scripts/qemu/virgl-query-event-poll.patch'


def run(args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def main():
    (ROOT / 'build').mkdir(exist_ok=True)
    with (ROOT / 'build/qemu-host.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        archive = ROOT / 'build' / ('qemu-' + VERSION + '.tar.xz')
        if not archive.exists():
            temporary = archive.with_suffix('.download')
            urllib.request.urlretrieve('https://download.qemu.org/' + archive.name, temporary)
            if hashlib.sha256(temporary.read_bytes()).hexdigest() != SHA256:
                temporary.unlink()
                raise SystemExit('QEMU source checksum mismatch')
            temporary.replace(archive)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
            raise SystemExit('QEMU source checksum mismatch')
        if not SOURCE.exists():
            # The EDK2 emulator includes an unused absolute X11 include link.
            def safe(member, destination):
                try:
                    return tarfile.data_filter(member, destination)
                except tarfile.AbsoluteLinkError:
                    return None
            with tarfile.open(archive) as tar:
                tar.extractall(ROOT / 'build', filter=safe)
        patch = ['patch', '--batch', '-p1', '-i', PATCH]
        if subprocess.run([*patch, '--forward', '--dry-run'], cwd=SOURCE, capture_output=True).returncode == 0:
            run([*patch, '--forward'], cwd=SOURCE)
        elif subprocess.run([*patch, '--reverse', '--dry-run'], cwd=SOURCE, capture_output=True).returncode:
            raise SystemExit('QEMU sources differ from the pinned patch; preserving local changes')
        BUILD.mkdir(exist_ok=True)
        run([SOURCE / 'configure', '--target-list=x86_64-softmmu',
             '--disable-werror', '--disable-docs', '--disable-tools',
             '--disable-guest-agent', '--disable-download', '--disable-modules',
             '--enable-sdl', '--enable-opengl', '--enable-virglrenderer'], cwd=BUILD)
        run(['ninja', '-C', BUILD, '-j8', 'qemu-system-x86_64'])
        bindir = PREFIX / 'bin'
        bindir.mkdir(parents=True, exist_ok=True)
        share = PREFIX / 'share'
        share.mkdir(exist_ok=True)
        firmware = share / 'qemu'
        if not firmware.exists():
            firmware.symlink_to(SOURCE / 'pc-bios', target_is_directory=True)
        target = bindir / 'qemu-system-x86_64'
        temporary = target.with_name(target.name + '.new-' + str(os.getpid()))
        shutil.copy2(BUILD / target.name, temporary)
        os.utime(temporary, None)
        os.replace(temporary, target)
        print('Local QEMU with Virgl query event polling: ' + str(target))


if __name__ == '__main__':
    main()
