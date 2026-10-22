#!/usr/bin/env python3
"""Check or temporarily pin power policy for this host's reviewed RTX pair.

No reset, binding, PCI-config, reset-method or persistent configuration writes.
Run --apply only after a fresh host power cycle and before opening QEMU.
"""
from pathlib import Path
import argparse
import importlib.util
import json
import os
import re
import stat
import sys

BRIDGE = '0000:00:01.1'
GPU = '0000:01:00.0'
AUDIO = '0000:01:00.1'
TARGETS = (BRIDGE, GPU, AUDIO)
POLICY = {'power/control': 'on', 'd3cold_allowed': '0'}
VALID = {'power/control': {'on', 'auto'}, 'd3cold_allowed': {'0', '1'}}
DEFAULT_STATE = Path('/run/avory-nouveau-power.json')

spec = importlib.util.spec_from_file_location('nvboot', Path(__file__).with_name('nouveau-boot.py'))
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)


def read(path):
    try:
        return path.read_text().strip()
    except OSError:
        return None


def inspect(sysroot=Path('/sys'), procroot=Path('/proc')):
    pre = boot.inspect(GPU, sysroot=sysroot, procroot=procroot)
    blockers = list(pre['blockers'])
    pci = sysroot/'bus/pci/devices'
    bridge = (pci/BRIDGE).resolve()
    if not (pci/BRIDGE).exists() or read(pci/BRIDGE/'class') != '0x060400':
        blockers.append('reviewed upstream PCI bridge is missing or has changed class')
    for bdf in (GPU, AUDIO):
        if (pci/bdf).resolve().parent != bridge:
            blockers.append(f'{bdf} is not directly below reviewed bridge {BRIDGE}')
    descendants = sorted(p.name for p in pci.iterdir()
                         if re.fullmatch(r'[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]', p.name)
                         and p.name != BRIDGE and p.resolve().is_relative_to(bridge))
    if descendants != sorted((GPU, AUDIO)):
        blockers.append(f'bridge has unreviewed descendants: {descendants}')
    rows = {}
    for bdf in TARGETS:
        dev = pci/bdf
        attrs = ('power_state', 'power/runtime_status', 'reset_method',
                 'power/control', 'd3cold_allowed', 'current_link_speed', 'current_link_width')
        rows[bdf] = {a: read(dev/a) for a in attrs}
        if rows[bdf]['power_state'] != 'D0':
            blockers.append(f'{bdf} is not in observed D0; do not use policy writes as recovery')
        for a in POLICY:
            if rows[bdf][a] not in VALID[a]:
                blockers.append(f'{bdf} {a} is unavailable or unexpected')
        try:
            with (dev/'config').open('rb') as stream:
                config = stream.read(64)
            vendor = int((dev/'vendor').read_text(), 16)
            device = int((dev/'device').read_text(), 16)
            if len(config) != 64 or int.from_bytes(config[:4], 'little') != device << 16 | vendor:
                blockers.append(f'{bdf} has invalid live PCI identity')
            rows[bdf]['config_identity'] = config[:4].hex()
        except OSError as error:
            blockers.append(f'{bdf} config unreadable: {error}')
    idle = read(sysroot/'module/vfio_pci/parameters/disable_idle_d3')
    if idle not in ('Y', '1'):
        blockers.append('vfio_pci.disable_idle_d3 must already be enabled; no module changes performed')
    boot_id = read(procroot/'sys/kernel/random/boot_id')
    if not boot_id:
        blockers.append('host boot ID unavailable')
    return dict(boot_id=boot_id, uptime=read(procroot/'uptime'),
                vfio_disable_idle_d3=idle, bridge_descendants=descendants,
                devices=rows, blockers=blockers, healthy_for_policy=not blockers,
                policy_pinned=all(rows[b][a] == v for b in TARGETS for a, v in POLICY.items()),
                note='Valid config and D0 do not prove that the device will survive a VFIO reset.')


def require_healthy(report):
    if report['blockers']:
        raise RuntimeError('refusing host writes: ' + '; '.join(report['blockers']))


def write_attr(path, value):
    path.write_text(value + '\n')
    if path.read_text().strip() != value:
        raise RuntimeError(f'power policy read-back mismatch: {path}')


def load_state(path, boot_id):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(fd) as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077:
            raise RuntimeError('state must be a private regular file owned by the invoking user (root for CLI writes)')
        if info.st_size > 16384:
            raise RuntimeError('oversize state file')
        saved = json.load(stream)
    if saved.get('boot_id') != boot_id:
        raise RuntimeError('state belongs to another boot; do not restore it')
    original = saved.get('original', {})
    if set(original) != set(TARGETS):
        raise RuntimeError('state has unexpected target devices')
    for bdf in TARGETS:
        if set(original[bdf]) != set(POLICY) or any(original[bdf][a] not in VALID[a] for a in POLICY):
            raise RuntimeError('state has unexpected policy values')
    return saved


def apply(report, state, sysroot=Path('/sys'), writer=write_attr):
    require_healthy(report)
    saved = dict(boot_id=report['boot_id'], original={
        b: {a: report['devices'][b][a] for a in POLICY} for b in TARGETS})
    # Save rollback values before the first write; never overwrite old state.
    fd = os.open(state, os.O_CREAT | os.O_EXCL | os.O_WRONLY | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'w') as stream:
        json.dump(saved, stream, indent=2)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())
    attempted = []
    try:
        for bdf in TARGETS:
            for attr, value in POLICY.items():
                if saved['original'][bdf][attr] != value:
                    attempted.append((bdf, attr))
                    writer(sysroot/'bus/pci/devices'/bdf/attr, value)
    except (OSError, RuntimeError) as error:
        failed = []
        for bdf, attr in reversed(attempted):
            try:
                writer(sysroot/'bus/pci/devices'/bdf/attr, saved['original'][bdf][attr])
            except (OSError, RuntimeError) as rollback_error:
                failed.append(str(rollback_error))
        if not failed:
            state.unlink()
        raise RuntimeError(f'apply failed: {error}; rollback errors: {failed}; state: {state}') from error


def restore(report, state, sysroot=Path('/sys'), writer=write_attr):
    require_healthy(report)
    saved = load_state(state, report['boot_id'])
    if not report['policy_pinned']:
        raise RuntimeError('policy changed since apply; refusing to overwrite newer settings')
    # Reverse dependency order: restore children before the parent bridge.
    attempted = []
    try:
        for bdf in reversed(TARGETS):
            for attr in reversed(POLICY):
                attempted.append((bdf, attr))
                writer(sysroot/'bus/pci/devices'/bdf/attr, saved['original'][bdf][attr])
    except (OSError, RuntimeError) as error:
        failed = []
        for bdf, attr in reversed(attempted):
            try:
                writer(sysroot/'bus/pci/devices'/bdf/attr, POLICY[attr])
            except (OSError, RuntimeError) as rollback_error:
                failed.append(str(rollback_error))
        raise RuntimeError(f'restore failed: {error}; rollback errors: {failed}; state retained: {state}') from error
    state.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--status', action='store_true', help='read-only inventory (default)')
    mode.add_argument('--apply', action='store_true', help='temporarily pin reviewed power policy; requires root')
    mode.add_argument('--restore', action='store_true', help='restore saved policy in the same boot; requires root')
    parser.add_argument('--state', type=Path, default=DEFAULT_STATE)
    args = parser.parse_args()
    if (args.apply or args.restore) and os.geteuid() != 0:
        raise RuntimeError('power-policy writes require root; status is read-only')
    report = inspect()
    if args.apply:
        apply(report, args.state)
        print(f'Power policy pinned for {BRIDGE}, {GPU}, {AUDIO}; rollback saved in {args.state}')
        report = inspect()
    elif args.restore:
        restore(report, args.state)
        print('Saved power policy restored.')
        report = inspect()
    print(json.dumps(report, indent=2))
    return 0 if report['healthy_for_policy'] else 2


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(f'nouveau-host-power: {error}', file=sys.stderr)
        raise SystemExit(2)
