#!/usr/bin/env python3
"""Replace identical firmware files under DIR with relative symlinks to one copy.

linux-firmware ships per-chip NVIDIA GSP blobs as symlinks to one file per
family; copies made without them cost ~1 GiB on disk.img for ~90 MiB of data.
"""
import hashlib, os, sys
from pathlib import Path

root = Path(sys.argv[1])
first = {}
saved = 0
for path in sorted(p for p in root.rglob('*') if p.is_file() and not p.is_symlink()):
    if path.stat().st_size < 64 * 1024:
        continue
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    key = (path.stat().st_size, h.digest())
    if key not in first:
        first[key] = path
        continue
    saved += path.stat().st_size
    target = os.path.relpath(first[key], path.parent)
    path.unlink()
    path.symlink_to(target)
print(f'dedup-firmware: {saved / 2**20:.0f} MiB of duplicates now symlinks in {root}')
