#!/usr/bin/env python3
"""Guest-root links must not escape to host libraries during image builds."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('runtime', ROOT / 'scripts/image-runtime.py')
runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime)


class RuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='avory-runtime-test-')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.tree = self.base / 'root'
        for directory in ('usr/lib', 'usr/lib32', 'opt/avory-glibc/lib', 'lib', 'lib64'):
            (self.tree / directory).mkdir(parents=True, exist_ok=True)
        for name in ('libc.so.6', 'libm.so.6', 'ld-linux-x86-64.so.2'):
            (self.tree / 'opt/avory-glibc/lib' / name).write_bytes(b'guest runtime\0' + name.encode())
            (self.tree / 'usr/lib' / name).symlink_to('/opt/avory-glibc/lib/' + name)
        (self.tree / 'usr/lib32/ld-linux.so.2').write_bytes(b'i686 runtime loader')
        (self.tree / 'usr/lib/ld-linux.so.2').symlink_to('../lib32/ld-linux.so.2')
        # Exercise a block-backed (not fast) absolute symlink as well.
        long_dir = 'opt/' + 'private-' * 12
        (self.tree / long_dir).mkdir()
        (self.tree / long_dir / 'real').write_bytes(b'long link runtime')
        (self.tree / 'usr/lib/long').symlink_to('/' + long_dir + '/real')
        (self.tree / 'usr/lib/relative').symlink_to('../../opt/avory-glibc/lib/libc.so.6')
        self.image = self.base / 'image'
        with self.image.open('wb') as f:
            f.truncate(16 * 1024 * 1024)
        subprocess.run(['mkfs.ext2', '-q', '-F', '-d', str(self.tree), str(self.image)], check=True)
        self.reader = runtime.RuntimeImage(self.image, self.tree, self.base)

    def test_absolute_relative_and_long_links(self):
        for path in ('/usr/lib/libc.so.6', '/usr/lib/relative', '/usr/lib/long'):
            self.reader.verify(path)

    def test_copy_and_verify_compat(self):
        self.reader.copy_compat()
        self.reader.verify('/lib64/libc.so.6', '/usr/lib/libc.so.6')
        self.reader.verify('/lib/ld-linux.so.2', '/usr/lib/ld-linux.so.2')

    def test_mismatch_rejected(self):
        (self.tree / 'opt/avory-glibc/lib/libc.so.6').write_bytes(b'different runtime')
        with self.assertRaisesRegex(RuntimeError, 'differs from tree'):
            self.reader.verify('/usr/lib/libc.so.6')

    def test_population_reports_full_image(self):
        source = self.base / 'oversized'
        source.mkdir()
        (source / 'large').write_bytes(b'x' * 8 * 1024 * 1024)
        image = self.base / 'small-image'
        with image.open('wb') as f:
            f.truncate(4 * 1024 * 1024)
        subprocess.run(['mkfs.ext2', '-q', '-F', str(image)], check=True)
        result = subprocess.run(['bash', str(ROOT / 'scripts/populate-ext2-dir.sh'),
                                 str(image), str(source), '/'], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Could not allocate', result.stderr)

    def test_missing_and_loop_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'missing'):
            self.reader.verify('/usr/lib/missing')
        (self.tree / 'usr/lib/loop').symlink_to('/usr/lib/loop')
        with self.assertRaisesRegex(RuntimeError, 'symlink loop'):
            self.reader.tree_file('/usr/lib/loop')



if __name__ == '__main__':
    unittest.main()
