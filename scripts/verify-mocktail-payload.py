#!/usr/bin/env python3
"""Check a payload against the updater's hashes without changing its metadata."""
import hashlib
import json
from pathlib import Path
import sys


def file_hash(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError(f'missing or non-regular payload file: {path}')
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def verify_payload(payload):
    metadata = json.loads((payload / 'roblox_payload.json').read_text())
    for name, key in (('libroblox.so', 'libroblox'),
                      ('sober_apk/base.apk', 'base_apk'),
                      ('sober_apk/split_config.x86_64.apk', 'x86_64_split_apk')):
        if file_hash(payload / name) != metadata['sha256'][key]:
            raise ValueError(f'payload hash mismatch: {name}')
    assets = payload / 'assets'
    if assets.is_symlink() or not assets.is_dir():
        raise ValueError('payload assets must be a real directory')
    files = []
    for entry in assets.rglob('*'):
        if entry.is_symlink() or not (entry.is_file() or entry.is_dir()):
            raise ValueError(f'non-regular asset: {entry}')
        if entry.is_file():
            files.append(entry)
    tree = hashlib.sha256()
    # Match Mocktail 1.0.4 HashAssetTree: sorted relative UTF-8 paths,
    # each folded as SHA256(file) + two spaces + path + NUL.
    for entry in sorted(files, key=lambda p: p.relative_to(assets).as_posix().encode('utf-8')):
        relative = entry.relative_to(assets).as_posix()
        tree.update((file_hash(entry) + '  ' + relative).encode('utf-8') + b'\0')
    if (len(files) != metadata['assets']['file_count'] or
            tree.hexdigest() != metadata['assets']['sha256_tree']):
        raise ValueError('payload asset tree does not match updater metadata')


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit('Usage: verify-mocktail-payload.py PAYLOAD_DIRECTORY')
    try:
        verify_payload(Path(sys.argv[1]))
    except (OSError, ValueError, KeyError, TypeError) as error:
        sys.exit(f'[!] Roblox payload verification failed: {error}')
    print('[+] Roblox library, APKs and asset tree match updater metadata')
