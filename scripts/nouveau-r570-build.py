#!/usr/bin/env python3
"""Assess unmodified v6.18.42 Nouveau against the existing 6.12 DRM/LinuxKPI.

All imported component sources, generated Makefiles, objects, firmware and
kernel output are separate. Does not change kernel/linux or the desktop ISO.
"""
from pathlib import Path
import argparse
import fcntl
import hashlib
import json
import shutil
import subprocess
import zlib

ROOT = Path(__file__).resolve().parents[1]
COMMIT = '856a9b51680cbbed1ace1207b29e424a616cff48'
COMPONENT = Path('drivers/gpu/drm/nouveau')


def verify_component(cache, tree):
    listing = subprocess.check_output(['git', '-C', str(cache), 'ls-tree', '-rz', COMMIT, '--', str(COMPONENT)])
    count = 0
    expected = set()
    for row in listing.split(b'\0'):
        if not row:
            continue
        metadata, name = row.split(b'\t', 1)
        mode, kind, blob = metadata.split()
        if kind != b'blob':
            raise RuntimeError('unexpected non-file in upstream component')
        path = tree/name.decode()
        data = path.readlink().as_posix().encode() if mode == b'120000' else path.read_bytes()
        actual = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
        if actual != blob.decode():
            raise RuntimeError(f'upstream component was modified: {path}')
        expected.add(path)
        count += 1
    if {p for p in (tree/COMPONENT).rglob('*') if p.is_file() or p.is_symlink()} != expected:
        raise RuntimeError('component has unexpected added or removed files')
    return count


def overlay(base, dest, remaining):
    dest.mkdir(parents=True, exist_ok=True)
    for entry in base.iterdir():
        target = dest/entry.name
        if entry.name == remaining[0]:
            if len(remaining) > 1:
                overlay(entry, target, remaining[1:])
        else:
            target.symlink_to(entry.absolute(), target_is_directory=entry.is_dir())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('-j', '--jobs', type=int, default=8)
    parser.add_argument('--prepare-only', action='store_true')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('jobs must be positive')
    out = ROOT/'build/nouveau-r570'
    out.mkdir(parents=True, exist_ok=True)
    with (out/'build.lock').open('a') as lock, (ROOT/'kernel/.linux-import.lock').open('a') as imported:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fcntl.flock(imported, fcntl.LOCK_SH | fcntl.LOCK_NB)
        cache = ROOT/'build/nouveau-c0/linux-candidate'
        actual = subprocess.check_output(['git', '-C', str(cache), 'rev-parse', 'v6.18.42^{}'], text=True).strip()
        if actual != COMMIT:
            raise RuntimeError('upstream tag no longer matches reviewed component commit')
        tree = out/'linux'
        if not tree.exists():
            overlay(ROOT/'kernel/linux', tree, COMPONENT.parts)
            archive = subprocess.Popen(['git', '-C', str(cache), 'archive', COMMIT, str(COMPONENT)], stdout=subprocess.PIPE)
            extracted = subprocess.run(['tar', '-xf', '-', '-C', str(tree)], stdin=archive.stdout)
            archive.stdout.close()
            if archive.wait() or extracted.returncode:
                raise RuntimeError('component extraction failed; preserve and inspect the incomplete staging tree')
            (out/'component.json').write_text(json.dumps({'tag': 'v6.18.42', 'commit': COMMIT,
                'component': str(COMPONENT), 'base_linux_pin': (ROOT/'kernel/linux/.pin').read_text()}, indent=2)+'\n')
        elif not (out/'component.json').exists():
            raise RuntimeError('component staging incomplete; inspect it before rebuilding')
        saved = json.loads((out/'component.json').read_text())
        if saved['commit'] != COMMIT or saved['base_linux_pin'] != (ROOT/'kernel/linux/.pin').read_text():
            raise RuntimeError('component or base pin changed; prepare a new isolated assessment')
        count = verify_component(cache, tree)
        (out/'source-integrity.json').write_text(json.dumps({'commit': COMMIT,
            'verified_git_blobs': count, 'upstream_driver_unmodified': True}, indent=2)+'\n')
        control = ROOT/'build/nouveau-c4/linux-control-570-ready'
        manifest = json.loads((control/'inputs.json').read_text())['firmware']
        firmware = out/'firmware'
        firmware.mkdir(exist_ok=True)
        lines = ['/* Private r570 candidate firmware manifest. */', '#ifndef KPI_NOUVEAU_FW_MANIFEST_H',
                 '#define KPI_NOUVEAU_FW_MANIFEST_H',
                 'static const struct { const char *name; unsigned int size, crc32; } kpi_nouveau_fw_manifest[] = {']
        for blob in manifest['files']:
            src = control/'root/lib/firmware'/blob['request']
            data = src.read_bytes()
            if hashlib.sha256(data).hexdigest() != blob['sha256']:
                raise RuntimeError(f'firmware changed since successful control: {src}')
            blob['crc32'] = zlib.crc32(data)
            dst = firmware/'lib/firmware'/blob['request']
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dst)
            lines.append(f'  {{"{blob["request"]}", {len(data)}u, 0x{blob["crc32"]:08x}u}},')
        lines.extend(['};', '#define KPI_NOUVEAU_FW_COUNT 4u', '#endif', ''])
        (firmware/'kpi_nouveau_fw_manifest.h').write_text('\n'.join(lines))
        (firmware/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
        kbuild = out/'Makefile.kbuild'
        subprocess.run(['python3', str(ROOT/'scripts/linux/kbuild-subset.py'), '--tree', str(tree),
            '--config', str(ROOT/'kernel/linuxkpi/include/generated/autoconf.h'),
            '--prefix', '../build/nouveau-r570/linux', '--out', str(kbuild),
            '--report', str(out/'kbuild-report.txt')], check=True,
            stdout=(out/'kbuild-generate.log').open('w'), stderr=subprocess.STDOUT)
        text = (ROOT/'kernel/GNUmakefile').read_text()
        text = text.replace('override OUTPUT := kernel\n', 'override OUTPUT := kernel-r570\n')
        text = text.replace('obj-$(ARCH)', 'obj-nouveau-r570-$(ARCH)')
        text = text.replace('-include linux/Makefile.kbuild', f'-include {kbuild}')
        needle = ': linux/%.c GNUmakefile'
        if text.count(needle) != 1:
            raise RuntimeError('Linux source pattern changed; refusing to guess')
        text = text.replace(needle, ': ../build/nouveau-r570/linux/%.c GNUmakefile')
        text = text.replace('-I ../build/nouveau/firmware', '-I ../build/nouveau-r570/firmware')
        (out/'Makefile.kbuild').write_text(kbuild.read_text().replace('obj-$(ARCH)', 'obj-nouveau-r570-$(ARCH)'))
        makefile = out/'kernel.make'
        makefile.write_text(text)
        if args.prepare_only:
            print(f'Prepared isolated upstream Nouveau {COMMIT}; no active import changed.')
            return
        with (out/'build.log').open('w') as log:
            subprocess.run(['make', '-k', '-C', str(ROOT/'kernel'), '-f', str(makefile),
                            f'-j{args.jobs}', 'KPI_AMDGPU=1', 'KPI_NOUVEAU=1'],
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        shutil.copyfile(ROOT/'kernel/bin-x86_64/kernel-r570', out/'kernel')
        print(f'Built isolated r570 candidate: {out}/kernel')


if __name__ == '__main__':
    main()
