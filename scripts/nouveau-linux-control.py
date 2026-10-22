#!/usr/bin/env python3
"""Prepare an isolated Linux control image; never launch or alter host devices."""
from pathlib import Path
import argparse
import hashlib
import json
import platform
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_newc(root, output):
    """Deterministic uncompressed initramfs; only private staged files."""
    with output.open('wb') as stream:
        def entry(name, mode, data, ino):
            encoded = name.encode() + b'\0'
            fields = (ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(encoded), 0)
            header = b'070701' + ''.join(f'{x:08x}' for x in fields).encode()
            stream.write(header + encoded)
            stream.write(b'\0' * (-(len(header) + len(encoded)) % 4))
            stream.write(data)
            stream.write(b'\0' * (-len(data) % 4))
        paths = sorted(root.rglob('*'))
        for i, path in enumerate(paths, 1):
            if path.is_symlink():
                raise RuntimeError(f'unexpected symlink in control fixture: {path}')
            mode = 0o040755 if path.is_dir() else 0o100755 if path.name == 'init' else 0o100644
            entry(str(path.relative_to(root)), mode, b'' if path.is_dir() else path.read_bytes(), i)
        entry('TRAILER!!!', 0, b'', len(paths) + 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel-release', default=platform.release())
    parser.add_argument('--firmware-version', choices=('535.113.01', '570.144'), default='535.113.01',
                        help='535 uses the C0 lock; 570 uses installed host firmware only in the control image')
    parser.add_argument('--output-dir', type=Path, default=ROOT/'build/nouveau-c4/linux-control')
    args = parser.parse_args()
    out = args.output_dir.absolute()
    # Never overwrite a previous fixture/evidence by accident.
    out.mkdir(parents=True, exist_ok=False)
    stage = out/'root'
    for name in ('proc', 'sys', 'dev', 'modules', 'lib/firmware'):
        (stage/name).mkdir(parents=True, exist_ok=True)
    kernel = Path('/usr/lib/modules')/args.kernel_release/'vmlinuz'
    shutil.copyfile(kernel, out/'vmlinuz')
    dependencies = subprocess.check_output(
        ['modprobe', '--set-version', args.kernel_release, '--show-depends', 'nouveau'], text=True)
    modules = []
    for line in dependencies.splitlines():
        words = line.split()
        if words[0] == 'builtin':
            continue
        if len(words) != 2 or words[0] != 'insmod':
            raise RuntimeError(f'unsupported dependency entry: {line}')
        src = Path(words[1])
        name = src.name.removesuffix('.zst').removesuffix('.xz')
        dst = stage/'modules'/name
        if src.suffix in ('.zst', '.xz'):
            with dst.open('wb') as stream:
                subprocess.run(['zstd' if src.suffix == '.zst' else 'xz', '-dc', str(src)], stdout=stream, check=True)
        else:
            shutil.copyfile(src, dst)
        modules.append({'path': '/modules/'+name, 'sha256': digest(dst)})
    (stage/'modules.txt').write_text(''.join(x['path']+'\n' for x in modules))
    if args.firmware_version == '535.113.01':
        firmware = ROOT/'build/nouveau/firmware'
        manifest = json.loads((firmware/'manifest.json').read_text())
        for blob in manifest['files']:
            src = firmware/'lib/firmware'/blob['request']
            if digest(src) != blob['sha256']:
                raise RuntimeError(f'firmware hash mismatch: {src}')
            dst = stage/'lib/firmware'/blob['request']
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dst)
    else:
        supported = set(subprocess.check_output(
            ['modinfo', '--set-version', args.kernel_release, '-F', 'firmware', 'nouveau'], text=True).splitlines())
        manifest = {'source': 'installed host linux-firmware (not the C0 lock)',
                    'gsp_interface': args.firmware_version, 'files': []}
        for name in ('gsp', 'bootloader', 'booter_load', 'booter_unload'):
            request = f'nvidia/ad106/gsp/{name}-{args.firmware_version}.bin'
            if request not in supported:
                raise RuntimeError(f'installed Nouveau does not declare support for {request}')
            plain = Path('/usr/lib/firmware')/request
            src = next((p for p in (plain, Path(str(plain)+'.zst'), Path(str(plain)+'.xz')) if p.is_file()), None)
            if src is None:
                raise RuntimeError(f'installed firmware missing: {request}')
            dst = stage/'lib/firmware'/request
            dst.parent.mkdir(parents=True, exist_ok=True)
            if src.suffix in ('.zst', '.xz'):
                with dst.open('wb') as stream:
                    subprocess.run(['zstd' if src.suffix == '.zst' else 'xz', '-dc', str(src.resolve())], stdout=stream, check=True)
            else:
                shutil.copyfile(src, dst)
            manifest['files'].append({'request': request, 'sha256': digest(dst),
                                      'size': dst.stat().st_size, 'source': str(src.resolve())})
    (stage/'firmware-version').write_text(args.firmware_version+'\n')
    subprocess.run(['cc', '-static', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(ROOT/'userland/test_nouveau_linux_control.c'), '-o', str(stage/'init')], check=True)
    write_newc(stage, out/'initramfs.cpio')
    (out/'inputs.json').write_text(json.dumps(dict(
        kernel_release=args.kernel_release, kernel_sha256=digest(out/'vmlinuz'),
        initramfs_sha256=digest(out/'initramfs.cpio'), modules=modules,
        firmware=manifest, limitation='Linux control uses an installed kernel, not the imported v6.12 baseline.'), indent=2)+'\n')
    print(f'Prepared {out}; no VM launched and no host driver/device state changed.')


if __name__ == '__main__':
    main()
