#!/usr/bin/env python3
"""Reuse a pristine disk only while every declared build input is unchanged."""
import argparse
import hashlib
import importlib.util
import json
import os
import re
import stat
import struct
from pathlib import Path
import subprocess
import sys
import tempfile

from image_copy import copy_image_range

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('disk_apps', ROOT / 'scripts/incremental-disk-apps.py')
apps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(apps)


def state(path):
    info = path.lstat()
    return [info.st_ino, info.st_mode, info.st_size, info.st_mtime_ns, info.st_ctime_ns,
            os.readlink(path) if stat.S_ISLNK(info.st_mode) else None]


def fingerprint(inputs, trees, settings):
    digest = hashlib.sha256()

    def record(path, info=None):
        digest.update(os.fsencode(path) + b'\0')
        try:
            if info is None:
                info = path.lstat()
        except FileNotFoundError:
            digest.update(b'missing\0')
            return
        digest.update(struct.pack('=QQIQqq', info.st_dev, info.st_ino, info.st_mode,
                                  info.st_size, info.st_mtime_ns, info.st_ctime_ns))
        if stat.S_ISLNK(info.st_mode):
            digest.update(os.fsencode(os.readlink(path)) + b'\0')

    digest.update(json.dumps([settings, {key: os.environ.get(key) for key in
                                       ('EXCLUDE_IMAGE_PATHS', 'EXCLUDE_IMAGE_STATIC')}],
                             sort_keys=True).encode())
    for path in sorted(set(inputs)):
        record(path)

    def scan(folder):
        with os.scandir(folder) as entries:
            for entry in sorted(entries, key=lambda item: item.name):
                path = Path(entry.path)
                info = entry.stat(follow_symlinks=False)
                record(path, info)
                if stat.S_ISDIR(info.st_mode):
                    scan(path)

    for tree in sorted(set(trees)):
        record(tree)
        if tree.is_dir() and not tree.is_symlink():
            scan(tree)
    return digest.hexdigest()


def build_key(args):
    # Include direct debugfs payloads too, including legacy recipe inputs
    # which were never listed as make prerequisites (e.g. standalone.s).
    sources = re.findall(r"write\s+([^\s\"']+)", args.makefile.read_text())
    payloads = [Path(path) for path in sources
                if path.startswith(('userland/', 'assets/', 'config/', 'initrd/', 'build/'))
                and '$' not in path]
    return fingerprint(args.inputs + payloads, args.trees, args.settings)


def clone_file(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix=target.name + '.', dir=target.parent)
    temp = Path(name)
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.truncate(source.stat().st_size)
        copy_image_range(source, temp, 0, 0, source.stat().st_size)
        return temp
    except BaseException:
        temp.unlink(missing_ok=True)
        raise


def save(args):
    apps.ensure_disk_closed(args.disk)
    before = build_key(args)
    temp = clone_file(args.disk, args.cache)
    try:
        if before != build_key(args):
            print('Build inputs changed while caching; skipping pristine image cache.')
            return
        temp.chmod(0o444)
        temp.replace(args.cache)
        metadata = {'version': 1, 'fingerprint': before, 'cache_state': state(args.cache)}
        args.metadata.parent.mkdir(parents=True, exist_ok=True)
        fd, name = tempfile.mkstemp(prefix=args.metadata.name + '.', dir=args.metadata.parent)
        try:
            with os.fdopen(fd, 'w') as stream:
                json.dump(metadata, stream)
                stream.write('\n')
            Path(name).replace(args.metadata)
        finally:
            Path(name).unlink(missing_ok=True)
        print('Saved pristine disk build cache.')
    finally:
        temp.unlink(missing_ok=True)


def restore(args):
    apps.ensure_disk_closed(args.disk)
    try:
        metadata = json.loads(args.metadata.read_text())
        valid = (isinstance(metadata, dict) and metadata.get('version') == 1 and metadata.get('cache_state') == state(args.cache)
                 and metadata.get('fingerprint') == build_key(args))
    except (FileNotFoundError, ValueError, TypeError):
        valid = False
    if not valid:
        print('Pristine cache miss; rebuilding disk from current inputs.', flush=True)
        return 3
    print('Build inputs unchanged; restoring pristine disk image...', flush=True)
    temp = clone_file(args.cache, args.disk)
    try:
        # Refresh the incremental partition from the pristine image as well:
        # the old cache may include guest writes or earlier app patches.
        apps.cache_partition(temp, args.partition_cache)
        if metadata['fingerprint'] != build_key(args):
            print('Build inputs changed during restore; rebuilding from current inputs.', flush=True)
            return 3
        temp.chmod(0o644)
        temp.replace(args.disk)
        apps.snapshot(args.manifest, args.makefile, args.apps)
    finally:
        temp.unlink(missing_ok=True)
    print('Disk image ready (pristine build cache).', flush=True)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('save', 'restore'))
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--metadata', type=Path, required=True)
    parser.add_argument('--disk', type=Path, default=Path('disk.img'))
    parser.add_argument('--partition-cache', type=Path, default=Path('build/arch/disk-partition.img'))
    parser.add_argument('--manifest', type=Path, default=Path('build/arch/disk-apps.json'))
    parser.add_argument('--makefile', type=Path, default=Path('GNUmakefile'))
    parser.add_argument('--settings', nargs='*', default=[])
    parser.add_argument('--trees', nargs='*', type=Path, default=[])
    parser.add_argument('--inputs', nargs='*', type=Path, default=[])
    parser.add_argument('--apps', nargs='*', default=[])
    args = parser.parse_args()
    try:
        if args.mode == 'restore':
            return restore(args)
        save(args)
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
