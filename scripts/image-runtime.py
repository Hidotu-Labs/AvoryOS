#!/usr/bin/env python3
"""Compare/copy runtime files with symlinks resolved inside the guest root."""
import argparse
from collections import deque
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def resolve(path, readlink):
    pending = deque(path.split('/'))
    parts = []
    links = 0
    while pending:
        part = pending.popleft()
        if part in ('', '.'):
            continue
        if part == '..':
            if parts:
                parts.pop()
            continue
        candidate = '/' + '/'.join(parts + [part])
        target = readlink(candidate)
        if target is None:
            parts.append(part)
            continue
        links += 1
        if links > 40:
            raise RuntimeError(f'symlink loop at {candidate}')
        if target.startswith('/'):
            parts = []
        pending.extendleft(reversed(target.split('/')))
    return '/' + '/'.join(parts)


class RuntimeImage:
    def __init__(self, image, root, tmp):
        self.image, self.root, self.tmp = str(image), Path(root).absolute(), Path(tmp)
        self.debugfs = '/run/host/usr/bin/debugfs' if os.access('/run/host/usr/bin/debugfs', os.X_OK) else 'debugfs'

    def command(self, command, write=False):
        args = [self.debugfs] + (['-w'] if write else [])
        result = subprocess.run(args + ['-R', command, self.image], capture_output=True, text=True, check=True)
        return result.stdout

    def tree_file(self, path):
        def readlink(guest):
            host = self.root / guest.lstrip('/')
            return os.readlink(host) if host.is_symlink() else None
        resolved = resolve(path, readlink)
        host = self.root / resolved.lstrip('/')
        if not host.is_file():
            raise RuntimeError(f'tree file missing: {path} -> {resolved}')
        return host

    def image_readlink(self, guest):
        stat = self.command(f'stat "{guest}"')
        kind = re.search(r'Type:\s+(\w+)', stat)
        if not kind:
            raise RuntimeError(f'image path missing: {guest}')
        if kind[1] != 'symlink':
            return None
        fast = re.search(r'Fast link dest: "(.*)"', stat)
        if fast:
            return fast[1]
        out = self.tmp / 'link'
        out.unlink(missing_ok=True)
        self.command(f'dump "{guest}" "{out}"')
        return out.read_text()

    def image_file(self, path):
        resolved = resolve(path, self.image_readlink)
        out = self.tmp / 'image-file'
        out.unlink(missing_ok=True)
        self.command(f'dump "{resolved}" "{out}"')
        if not out.is_file():
            raise RuntimeError(f'cannot read image file: {path} -> {resolved}')
        return out

    def verify(self, image_path, tree_path=None):
        tree = self.tree_file(tree_path or image_path)
        image = self.image_file(image_path)
        digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        if image.stat().st_size == 0 or digest(image) != digest(tree):
            raise RuntimeError(f'image {image_path} differs from tree {tree_path or image_path}; repopulate the image')
        print(f'Runtime verified: {image_path} (SHA-256 matches staged file)')

    def copy_compat(self):
        names = ['ld-linux-x86-64.so.2', 'libc.so.6', 'libm.so.6']
        # Resolve every source before changing the image. Never read an absolute
        # guest symlink against the host's /lib or /opt.
        sources = {name: self.tree_file('/usr/lib/' + name) for name in names}
        for name, source in sources.items():
            staged = self.tmp / name
            shutil.copyfile(source, staged)
            for directory in ('/lib', '/lib64'):
                target = directory + '/' + name
                self.command(f'rm "{target}"', write=True)
                self.command(f'write "{staged}" "{target}"', write=True)
                self.command(f'set_inode_field "{target}" mode 0100755', write=True)
                self.verify(target, '/usr/lib/' + name)
        # AvoryOS keeps /lib as a real directory for the native runtime
        # loader, so the usrmerge symlink /usr/lib/ld-linux.so.2 cannot
        # satisfy an ELF32 PT_INTERP of /lib/ld-linux.so.2. Stage the
        # multilib loader at its exact interpreter path as well.
        compat_loader = self.tree_file('/usr/lib/ld-linux.so.2')
        staged = self.tmp / 'ld-linux.so.2'
        shutil.copyfile(compat_loader, staged)
        target = '/lib/ld-linux.so.2'
        self.command(f'rm "{target}"', write=True)
        self.command(f'write "{staged}" "{target}"', write=True)
        self.command(f'set_inode_field "{target}" mode 0100755', write=True)
        self.verify(target, '/usr/lib/ld-linux.so.2')
        musl = self.root / 'usr/lib/musl/lib/libc.so'
        if musl.is_file():
            staged = self.tmp / 'musl'
            shutil.copyfile(musl, staged)
            target = '/lib/ld-musl-x86_64.so.1'
            self.command(f'rm "{target}"', write=True)
            self.command(f'write "{staged}" "{target}"', write=True)
            self.command(f'set_inode_field "{target}" mode 0100755', write=True)
            self.verify(target, '/usr/lib/musl/lib/libc.so')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image')
    parser.add_argument('root')
    parser.add_argument('paths', nargs='*')
    parser.add_argument('--copy-compat', action='store_true')
    args = parser.parse_args()
    try:
        with tempfile.TemporaryDirectory(prefix='avory-runtime-') as tmp:
            runtime = RuntimeImage(args.image, args.root, tmp)
            if args.copy_compat:
                runtime.copy_compat()
            else:
                for path in args.paths:
                    runtime.verify(path)
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f'ERROR: {error}\n')


if __name__ == '__main__':
    main()
