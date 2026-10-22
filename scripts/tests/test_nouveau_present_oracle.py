#!/usr/bin/env python3
"""Check real guest pixel generation against strict host scanout comparison."""
from pathlib import Path
import importlib.util
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, root/'scripts'/filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


boot = load('nvboot', 'nouveau-boot.py')
capture = load('nvcapture', 'nouveau-prime-capture.py')
uapi = root/'build/nouveau-618/linux/include/uapi'
with tempfile.TemporaryDirectory(prefix='nouveau-c6-present-') as temp:
    directory = Path(temp)
    binary = directory/'present'
    flags = ['cc', '-static', '-O2', '-Wall', '-Wextra', '-Werror',
             '-I', str(uapi/'drm'), '-iquote', str(uapi/'linux'), '-I', str(root/'userland')]
    subprocess.run([*flags, str(root/'userland/test_nouveau_present.c'), '-o', str(binary)], check=True)
    software = subprocess.run([str(binary), '--software'], capture_output=True, text=True, check=True)
    boot.verify_c6_present(software.stdout, software=True)
    assert subprocess.run([str(binary), '--unknown'], capture_output=True).returncode == 2
    harness = directory/'pixels.c'
    harness.write_text('''
#define NOUVEAU_PRESENT_ENTRY presentation_main
#include "test_nouveau_present.c"
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    unsigned int frame = atoi(argv[1]);
    printf("P6\\n1280 800\\n255\\n");
    for (unsigned int vertical = 0; vertical < PRESENT_HEIGHT; vertical++)
        for (unsigned int horizontal = 0; horizontal < PRESENT_WIDTH; horizontal++) {
            uint32_t color = present_pixel(horizontal, vertical, frame);
            putchar(color >> 16);
            putchar((color >> 8) & 255);
            putchar(color & 255);
        }
    return 0;
}
''')
    generator = directory/'pixels'
    subprocess.run([*flags, str(harness), '-o', str(generator)], check=True)
    for frame in range(2):
        image = subprocess.run([str(generator), str(frame)], capture_output=True, check=True).stdout
        screenshot = directory/f'fixture-{frame}.ppm'
        screenshot.write_bytes(image)
        expected = capture.expected_rgb(frame)
        capture.verify_ppm(screenshot, expected)
        for bad in (image[:-1], image.replace(b'1280 800', b'1280 801', 1),
                    image[:-1]+bytes([image[-1] ^ 1]), b'P6\n1280 800\n255\n'+bytes(len(expected)),
                    b'P6\n1280 800\n255\n'+capture.expected_rgb(1-frame)):
            screenshot.write_bytes(bad)
            try:
                capture.verify_ppm(screenshot, expected)
            except RuntimeError:
                pass
            else:
                raise AssertionError('bad, stale, black or truncated scanout accepted')
    observer = capture.FrameCapture(directory)
    try:
        observer.finish()
    except RuntimeError:
        pass
    else:
        raise AssertionError('missing screenshots accepted')
    original = capture.screendump
    capture.screendump = lambda endpoint, destination: destination.write_bytes(
        b'P6\n1280 800\n255\n'+capture.expected_rgb(len(observer.frames)))
    for frame in range(2):
        observer(f'NOUVEAU_C6_PRESENT: READY frame={frame} width=1280 height=800')
    observer.finish()
    assert (directory/'capture.json').is_file()
    capture.screendump = original

record = dict(kernel_sha256='kernel', cpus=1, firmware={'files':['matched']},
              pcie_root_port=True, gpu_only=True, oracle_kind='c6-present',
              oracle_source_sha256='all-sources-and-capture', oracle_uapi_sha256={'drm_mode.h':'abi'})
ready = dict(record, status='pass', software=True)
assert boot.readiness_matches(ready, record)
for key, value in [('oracle_kind','c6-prime'), ('oracle_source_sha256','changed-capture'),
                   ('oracle_uapi_sha256',{'drm_mode.h':'different'})]:
    assert not boot.readiness_matches(dict(ready, **{key:value}), record)
log = ('NOUVEAU_C6_PRESENT: START linear PRIME presentation\n'
       'driver=nouveau pci=10de:2803\n'
       'busid=pci:0000:00:06.0 import=1\n'
       'NOUVEAU_C6_PRESENT: READY frame=0 width=1280\n'
       'NOUVEAU_C6_PRESENT: READY frame=1 width=1280\n'
       'NOUVEAU_C6_PRESENT: END presentation status=pass\n')
boot.verify_c6_present(log)
for invalid in (software.stdout, log.replace('READY frame=1','missing'),
                log.replace('END presentation status=pass','incomplete'),
                log+'NOUVEAU_C6_PRIME: FAIL cleanup\n'):
    try:
        boot.verify_c6_present(invalid)
    except RuntimeError:
        pass
    else:
        raise AssertionError('incomplete guest markers accepted')
for args in (['--c6-present','--c6-prime'], ['--c6-present','--vfio-only'],
             ['--c6-present','--qemu-extra=-qmp stdio']):
    assert subprocess.run(['python3', str(root/'scripts/nouveau-boot.py'), *args], capture_output=True).returncode == 2
print('PASS: actual C pixels match host oracle; single-byte corruption/black/wrong-size/missing captures fail; readiness and modes are isolated')
