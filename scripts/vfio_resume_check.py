#!/usr/bin/env python3
"""Reject Raphael passthrough after host suspend; perform no device writes."""
import os
from pathlib import Path
import re
import sys
import time


def check_raphael_resume(command, sysroot=Path('/sys'), clock=time.clock_gettime,
                         allow_resumed=False):
    """Return read-only evidence, or raise before a matching device is opened.

    This prevents the reported post-S3 launch, not firmware recovery. Do not
    infer host resume from a guest PSP response or issue speculative resets.
    """
    devices = []
    for index, arg in enumerate(command[:-1]):
        if arg != '-device':
            continue
        fields = str(command[index + 1]).split(',')
        if fields[0] != 'vfio-pci':
            continue
        host = next((item[5:] for item in fields[1:] if item.startswith('host=')), '')
        if not re.fullmatch(r'(?:[0-9a-fA-F]{4}:)?[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]', host):
            continue
        bdf = host.lower() if host.count(':') == 2 else '0000:' + host.lower()
        dev = sysroot / 'bus/pci/devices' / bdf
        try:
            vendor = int((dev / 'vendor').read_text().strip(), 16)
            device = int((dev / 'device').read_text().strip(), 16)
        except (OSError, ValueError) as error:
            raise RuntimeError(f'cannot identify VFIO device {bdf}: {error}') from error
        if (vendor, device) == (0x1002, 0x164e):
            devices.append(bdf)
    if not devices:
        return {'raphael_devices': []}

    success = None
    try:
        success = int((sysroot / 'power/suspend_stats/success').read_text().strip())
        if success < 0:
            success = None
    except (OSError, ValueError):
        pass
    sleep_gap = None
    try:
        boot = clock(time.CLOCK_BOOTTIME)
        mono = clock(time.CLOCK_MONOTONIC)
        sleep_gap = max(0.0, boot - mono)
    except (AttributeError, OSError, ValueError):
        pass
    if success is None and sleep_gap is None:
        raise RuntimeError('cannot determine host suspend history for Raphael VFIO; refusing launch')
    resumed = (success is not None and success > 0) or (sleep_gap is not None and sleep_gap > 1.0)
    if resumed and not allow_resumed:
        raise RuntimeError(
            'Raphael VFIO launch blocked: this host has resumed from suspend '
            f'(successful suspends={success}, suspended seconds={sleep_gap}). '
            'This device has caused whole-host Data Fabric resets during guest '
            'initialization after resume. Reboot the host before launching. '
            'MODE2 and PSP ring replacement do not establish post-resume recovery.')
    return {'raphael_devices': devices, 'successful_suspends': success,
            'suspended_seconds': sleep_gap, 'resumed_host_trial': bool(resumed and allow_resumed)}


def main():
    command = sys.argv[1:]
    if command[:1] == ['--']:
        command = command[1:]
    if not command:
        sys.exit('a command after -- is required')
    try:
        check_raphael_resume(command)
    except RuntimeError as error:
        sys.exit(str(error))
    os.execvpe(command[0], command, os.environ)


if __name__ == '__main__':
    main()
