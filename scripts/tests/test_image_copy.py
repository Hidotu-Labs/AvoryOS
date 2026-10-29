#!/usr/bin/env python3
"""Range copies must preserve GPT bytes and replace stale sparse contents."""
import errno
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from image_copy import copy_image_range


class CopyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='avory-image-copy-')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.source, self.target = self.base / 'source', self.base / 'target'
        self.length = 4 * 1024 * 1024
        with self.source.open('wb') as f:
            f.truncate(self.length)
            f.seek(1024 * 1024)
            f.write(b'payload' * 1024)
        self.target.write_bytes(b'H' * 4096 + b'X' * self.length + b'T' * 4096)

    def verify(self):
        contents = self.target.read_bytes()
        self.assertEqual(contents[:4096], b'H' * 4096)
        self.assertEqual(contents[-4096:], b'T' * 4096)
        self.assertEqual(contents[4096:-4096], self.source.read_bytes())

    def test_clone_or_native_copy(self):
        copy_image_range(self.source, self.target, 0, 4096, self.length)
        self.verify()
        # Shared extents must retain copy-on-write isolation.
        with self.source.open('r+b') as f:
            f.write(b'changed source')
        self.assertEqual(self.target.read_bytes()[4096:4110], b'\0' * 14)

    def test_sparse_fallback(self):
        with patch('image_copy.fcntl.ioctl', side_effect=OSError(errno.EOPNOTSUPP, 'unsupported')):
            copy_image_range(self.source, self.target, 0, 4096, self.length)
        self.verify()

    def test_buffered_fallback_and_partial_writes(self):
        original = os.pwrite
        def partial(fd, data, offset):
            return original(fd, data[:1024], offset)
        with patch('image_copy.fcntl.ioctl', side_effect=OSError(errno.EXDEV, 'cross device')), \
             patch('image_copy.os.copy_file_range', side_effect=OSError(errno.EXDEV, 'cross device')), \
             patch('image_copy.os.pwrite', side_effect=partial):
            copy_image_range(self.source, self.target, 0, 4096, self.length)
        self.verify()

    def test_dense_fallback_clears_old_holes(self):
        with patch('image_copy.fcntl.ioctl', side_effect=OSError(errno.EOPNOTSUPP, 'unsupported')), \
             patch('image_copy._clear_range', return_value=False):
            copy_image_range(self.source, self.target, 0, 4096, self.length)
        self.verify()

    def test_invalid_bounds_before_mutation(self):
        before = self.target.read_bytes()
        for src, off, dest, length in [(self.source, -1, 0, 1),
                                      (self.source, 0, 0, self.length + 1),
                                      (self.source, 0, self.length * 2, 1),
                                      (self.target, 0, 0, 4096)]:
            with self.assertRaises(ValueError):
                copy_image_range(src, self.target, off, dest, length)
            self.assertEqual(before, self.target.read_bytes())

    def test_gpt_install_and_cache_round_trip(self):
        disk, partition, cache = (self.base / name for name in ('disk', 'partition', 'cache'))
        with disk.open('wb') as f:
            f.truncate(64 * 1024 * 1024)
        subprocess.run(['sfdisk', str(disk)], input='label: gpt\nstart=2048, size=126976, type=L\n',
                       text=True, check=True, capture_output=True)
        with partition.open('wb') as f:
            f.truncate(62 * 1024 * 1024)
        subprocess.run(['mkfs.ext4', '-q', '-F', str(partition)], check=True)
        command = [sys.executable, str(ROOT / 'scripts/incremental-disk-apps.py'), '--disk', str(disk)]
        subprocess.run(command + ['--install-partition', str(partition)], check=True)
        subprocess.run(command + ['--cache-from-disk', '--partition-cache', str(cache)], check=True)
        self.assertEqual(partition.read_bytes(), cache.read_bytes())
        subprocess.run(['e2fsck', '-f', '-n', str(cache)], check=True, capture_output=True)


if __name__ == '__main__':
    unittest.main()
