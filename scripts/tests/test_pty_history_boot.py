#!/usr/bin/env python3
"""Boot the PTY ioctl matrix and real staged glibc Bash history regression.

Requires a built kernel/ISO, musl-gcc, QEMU and build/arch/rootfs. Uses a
private disk and software emulation by default; AVORY_TEST_ACCEL=kvm opts in.
Never opens disk.img or changes shell configuration.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess

from boot_probe import run_probe

ROOT = Path(__file__).resolve().parents[2]
TREE = ROOT / 'build/arch/rootfs'


def setup(root):
    copied = set()

    def copy_binary(guest):
        if guest in copied:
            return
        copied.add(guest)
        # Resolve absolute guest symlinks inside the staged root.
        parts, pending, links = [], guest.split('/'), 0
        while pending:
            part = pending.pop(0)
            if part in ('', '.'):
                continue
            if part == '..':
                if parts:
                    parts.pop()
                continue
            source = TREE.joinpath(*parts, part)
            if source.is_symlink():
                links += 1
                if links > 40:
                    raise RuntimeError(f'symlink loop resolving {guest}')
                target = os.readlink(source)
                if target.startswith('/'):
                    parts = []
                pending = target.split('/') + pending
            else:
                parts.append(part)
        source = TREE.joinpath(*parts)
        destination = root / guest.lstrip('/')
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        dynamic = subprocess.check_output(['readelf', '-d', source], text=True)
        for dependency in re.findall(r'\(NEEDED\).*?\[(.*?)\]', dynamic):
            copy_binary('/usr/lib/' + dependency)

    copy_binary('/usr/bin/bash')
    copy_binary('/lib64/ld-linux-x86-64.so.2')
    destination = root / 'usr/share/terminfo/x'
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copy2(TREE / 'usr/share/terminfo/x/xterm-256color',
                 destination / 'xterm-256color')


if __name__ == '__main__':
    run_probe('pty-history-boot',
              Path(__file__).with_name('test_pty_history.c').read_text(),
              'PTY_HISTORY_BOOT: PASS failures=0', setup=setup, cpus=1,
              accel=os.environ.get('AVORY_TEST_ACCEL', 'tcg'), timeout=120)
