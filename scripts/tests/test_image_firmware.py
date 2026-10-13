#!/usr/bin/env python3
"""Firmware must be readable after /lib becomes a compatibility directory."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class FirmwareImageTests(unittest.TestCase):
    def test_install_after_lib_replacement_and_reinstall(self):
        with tempfile.TemporaryDirectory(prefix='avory-firmware-test-') as tmp:
            base = Path(tmp)
            tree = base / 'root'
            # Reproduce the final layout: /lib is a real directory while
            # the rootfs overlay's firmware remains under /usr/lib.
            (tree / 'lib').mkdir(parents=True)
            source = tree / 'usr/lib/firmware'
            (source / 'amdgpu').mkdir(parents=True)
            blob = source / 'amdgpu/gc_10_3_6_pfp.bin'
            blob.write_bytes(b'firmware-v1' * 4096)
            test = base / 'test_fw.bin'
            test.write_bytes(bytes((i * 7 + 3) & 255 for i in range(4096)))
            image = base / 'image'
            with image.open('wb') as f:
                f.truncate(16 * 1024 * 1024)
            subprocess.run(['mkfs.ext4', '-q', '-F', '-d', str(tree), str(image)], check=True)
            missing = subprocess.run(['debugfs', '-R', 'stat /lib/firmware/test_fw.bin', str(image)],
                                     capture_output=True, text=True)
            self.assertIn('File not found', missing.stderr)
            for version in (b'firmware-v1', b'firmware-v2'):
                blob.write_bytes(version * 4096)
                installed = subprocess.run(['bash', str(ROOT / 'scripts/image-firmware-install.sh'),
                                            str(image), str(source), str(test)],
                                           capture_output=True, text=True)
                self.assertEqual(installed.returncode, 0, installed.stdout + installed.stderr)
                self.assertIn('Runtime verified: /lib/firmware/test_fw.bin', installed.stdout)
                self.assertIn('Runtime verified: /lib/firmware/amdgpu/gc_10_3_6_pfp.bin', installed.stdout)


if __name__ == '__main__':
    unittest.main()
