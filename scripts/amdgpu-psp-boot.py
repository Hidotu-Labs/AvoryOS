#!/usr/bin/env python3
"""Prepare a disposable AMDGPU initialization probe; --run attaches Raphael.

Default: preparation only. --software-control boots with no passed hardware.
--run performs one physical-device initialization and orderly guest power-off.
It never retries, invokes a reset helper, or changes host configuration. If the
guest cannot power off, leave QEMU alive rather than force-closing the GPU.
"""
import argparse
import fcntl
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    'private_boot', ROOT / 'scripts/syscall-restart-boot.py')
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)
boot.OUT = ROOT / 'build/amdgpu-psp-boot'
boot.INIT = r'''
#include <stdio.h>
#include <unistd.h>
#include <sys/reboot.h>
int main(void) {
 setvbuf(stdout,NULL,_IOLBF,0);
 puts("PSP_BOOT: reached userspace after kernel initialization");
 puts("PSP_BOOT: orderly guest power-off; inspect driver result in serial log");
 fflush(NULL);
 reboot(RB_POWER_OFF);
 perror("PSP_BOOT power-off");
 for(;;)sleep(60);
}
'''


def stage_firmware(root):
    shutil.copytree(ROOT / 'build/firmware/lib/firmware', root / 'lib/firmware',
                    dirs_exist_ok=True)
    (root / 'lib/firmware/test_fw.bin').write_bytes(
        bytes((i * 7 + 3) & 255 for i in range(4096)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--run', action='store_true')
    mode.add_argument('--software-control', action='store_true')
    parser.add_argument('--driver-defaults', action='store_true',
                        help='Use the normal MODE2/PSP recovery defaults')
    parser.add_argument('--post-resume-trial', action='store_true',
                        help='Explicit one-boot hardware trial after host sleep; host reboot remains possible')
    args = parser.parse_args()
    if args.post_resume_trial and (not args.run or not args.driver_defaults):
        parser.error('--post-resume-trial requires --run --driver-defaults')
    if boot.OUT.is_symlink():
        raise RuntimeError('private output directory is a symlink')
    boot.OUT.mkdir(parents=True, exist_ok=True)
    with (boot.OUT / '.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        recovery = '' if args.driver_defaults else 'kpi_amdgpu_mode2_reset=0 kpi_amdgpu_psp_reinit=0 '
        cmdline = ('kpi_amdgpu=1 kpi_prime_offload=1 ' + recovery + 'amdgpu.runpm=0 '
                   'amdgpu.ppfeaturemask=0xfff73fff drm.debug=0x4 init=/bin/probe-init')
        if args.software_control:
            cmdline = 'kpi_amdgpu=0 init=/bin/probe-init'
        disk, iso = boot.prepare(stage_firmware, cmdline)
        # Test the current build rather than the kernel in a possibly older ISO.
        shutil.copy2(ROOT / 'kernel/bin-x86_64/kernel', boot.OUT / 'iso-root/boot/kernel')
        boot.run(['strip', '--strip-all', boot.OUT / 'iso-root/boot/kernel'])
        boot.run(['xorriso', '-as', 'mkisofs', '-R', '-r', '-J', '-b',
                  'boot/limine/limine-bios-cd.bin', '-no-emul-boot', '-boot-load-size', '4',
                  '-boot-info-table', '--efi-boot', 'boot/limine/limine-uefi-cd.bin',
                  '-efi-boot-part', '--efi-boot-image', '--protective-msdos-label',
                  boot.OUT / 'iso-root', '-o', iso])
        boot.run([ROOT / 'limine/limine', 'bios-install', iso])
        serial = boot.OUT / ('software-serial.log' if args.software_control else 'physical-serial.log')
        cmd = [sys.executable, ROOT / 'scripts/qemu-prime-log.py',
               '--output-root', boot.OUT / 'launch-evidence', '--serial-log', serial,
               '--', 'qemu-system-x86_64', '-M', 'q35', '-cpu', 'host', '-enable-kvm',
               '-smp', '8', '-m', '4G', '-drive',
               f'if=pflash,unit=0,format=raw,file={ROOT}/edk2-ovmf/ovmf-code-x86_64.fd,readonly=on',
               '-cdrom', iso, '-boot', 'd', '-drive',
               f'file={disk},format=raw,if=none,id=probe', '-device',
               'nvme,serial=psp-probe,drive=probe', '-vga', 'none', '-device',
               'virtio-vga,addr=06.0,xres=1280,yres=800', '-device', 'edu,addr=07.0',
               '-display', 'none', '-chardev',
               f'socket,id=probe-log,path={boot.OUT}/serial.sock,server=on,wait=off,'
               f'logfile={serial},logappend=on', '-serial', 'chardev:probe-log',
               '-monitor', 'none', '-no-reboot']
        if not args.software_control:
            rom = ROOT / 'build/vfio/vbios.rom'
            device = 'vfio-pci,host=0e:00.0,addr=03.0,rombar=1'
            # Match run-c7-prime, which supplies the saved APU VBIOS. The
            # bare physical ROM BAR is not a usable substitute on this host.
            if rom.exists():
                if rom.read_bytes()[:2] != b'\x55\xaa':
                    raise RuntimeError('saved VBIOS has no option-ROM signature')
                device += f',romfile={rom}'
            else:
                raise RuntimeError('saved Raphael VBIOS is required for this probe')
            cmd.extend(['-device', device])
        if args.post_resume_trial:
            cmd.insert(cmd.index('--'), '--allow-resumed-raphael')
        (boot.OUT / 'prepared-command.json').write_text(
            json.dumps([str(x) for x in cmd], indent=2) + '\n')
        print(f'Prepared private ISO/root disk: {boot.OUT}', flush=True)
        if not (args.run or args.software_control):
            print('Preparation only: no QEMU process or hardware access started.')
            return 0
        if args.run:
            dev = Path('/sys/bus/pci/devices/0000:0e:00.0')
            assert (dev / 'vendor').read_text().strip() == '0x1002'
            assert (dev / 'device').read_text().strip() == '0x164e'
            assert (dev / 'driver').resolve().name == 'vfio-pci'
        # Start after preparation. The launcher captures/flushes serial and
        # stderr and owns its lock while QEMU exists, even if this caller exits.
        serial_offset = serial.stat().st_size if serial.exists() else 0
        with (boot.OUT / 'controller.log').open('ab', buffering=0) as output:
            process = subprocess.Popen([str(x) for x in cmd], stdout=output, stderr=output,
                                       start_new_session=True)
        state = {'pid': process.pid, 'mode': 'physical' if args.run else 'software',
                 'serial': str(serial), 'serial_offset': serial_offset, 'status': 'running'}
        (boot.OUT / 'controller.json').write_text(json.dumps(state, indent=2) + '\n')
        deadline = time.monotonic() + 120
        while process.poll() is None and time.monotonic() < deadline:
            time.sleep(.2)
        if process.poll() is None:
            state['status'] = 'still-running-needs-orderly-shutdown'
            (boot.OUT / 'controller.json').write_text(json.dumps(state, indent=2) + '\n')
            print(f'Guest has not powered off. QEMU remains alive; no forced close or retry. PID={process.pid}')
            return 2
        # Preserve the shared history, but judge this attempt's bytes only.
        with serial.open('rb') as stream:
            stream.seek(serial_offset)
            log = stream.read().decode(errors='replace')
        state.update(status='exited', returncode=process.returncode)
        (boot.OUT / 'controller.json').write_text(json.dumps(state, indent=2) + '\n')
        orderly = ('PSP_BOOT: reached userspace' in log and
                   'System power-off requested via syscall' in log and process.returncode == 0)
        if args.software_control:
            assert orderly and '[FAIL]' not in log and 'THREAD FAULT:' not in log, serial
            print('PASS: software control reached userspace, passed boot suites and powered off.')
        else:
            if 'PSP ring-create enter' not in log:
                print(f'AMDGPU failed before PSP ring creation; inspect {serial}. No retry.')
                return 1
            if not (orderly and 'PSP ring-create exit ret=0' in log and
                    'PSP create ring failed' not in log and
                    'PSP firmware loading failed' not in log and
                    'pci amdgpu probe 0000:00:03.0 failed' not in log and
                    'THREAD FAULT:' not in log):
                print(f'Physical PSP test failed; evidence retained in {boot.OUT}. No retry.')
                return 1
            print('PASS: this physical attempt completed PSP ring creation and orderly power-off.')
            print('A single successful attempt does not establish warm-restart stability.')
        return 0


if __name__ == '__main__':
    sys.exit(main())
