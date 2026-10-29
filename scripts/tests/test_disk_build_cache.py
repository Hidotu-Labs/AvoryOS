#!/usr/bin/env python3
"""Pristine reuse must invalidate inputs and undo guest/incremental disk changes."""
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
spec = importlib.util.spec_from_file_location('build_cache', ROOT / 'scripts/disk-build-cache.py')
cache = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cache)


class BuildCacheTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='avory-build-cache-')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.tree = self.base / 'tree'
        self.tree.mkdir()
        (self.tree / 'input').write_bytes(b'original')
        (self.tree / 'guest-link').symlink_to('/guest/path/not/on/host')
        makefile = self.base / 'makefile'
        makefile.write_text('write input /bin/program\n')
        disk = self.base / 'disk'
        with disk.open('wb') as stream:
            stream.truncate(32 * 1024**2)
        subprocess.run(['sfdisk', str(disk)], input='label: gpt\nstart=2048, size=61440, type=L\n',
                       text=True, capture_output=True, check=True)
        part = self.base / 'part'
        with part.open('wb') as stream:
            stream.truncate(30 * 1024**2)
        subprocess.run(['mkfs.ext4', '-q', '-F', str(part)], check=True)
        cache.copy_image_range(part, disk, 0, 1024**2, part.stat().st_size)
        self.args = argparse.Namespace(disk=disk, cache=self.base / 'pristine',
                                      metadata=self.base / 'metadata',
                                      partition_cache=self.base / 'partition-cache',
                                      manifest=self.base / 'apps.json', makefile=makefile,
                                      settings=['size=32'], inputs=[makefile], trees=[self.tree], apps=[])

    def test_restore_resets_guest_changes_and_partition_cache(self):
        cache.save(self.args)
        original = self.args.disk.read_bytes()
        with self.args.disk.open('r+b') as stream:
            stream.seek(2 * 1024**2)
            stream.write(b'guest changed the disk')
        self.args.partition_cache.write_bytes(b'stale incremental cache')
        self.assertEqual(cache.restore(self.args), 0)
        self.assertEqual(original, self.args.disk.read_bytes())
        self.assertEqual(self.args.partition_cache.read_bytes(), original[1024**2:-1024**2])
        subprocess.run(['e2fsck', '-f', '-n', str(self.args.partition_cache)],
                       capture_output=True, check=True)
        self.assertTrue(self.args.manifest.is_file())
        self.assertEqual(self.args.cache.stat().st_mode & 0o222, 0)

    def assert_miss_without_disk_change(self):
        before = self.args.disk.read_bytes()
        self.assertEqual(cache.restore(self.args), 3)
        self.assertEqual(before, self.args.disk.read_bytes())

    def test_changed_inputs_and_settings_invalidate(self):
        cache.save(self.args)
        changes = [lambda: (self.tree / 'input').write_bytes(b'modified'),
                   lambda: (self.tree / 'input').chmod(0o755),
                   lambda: (self.tree / 'new-file').touch(),
                   lambda: (self.tree / 'input').unlink(),
                   lambda: self.args.inputs[0].write_text('changed recipe'),
                   lambda: self.args.settings.append('xorg=virgl')]
        for change in changes:
            change()
            self.assert_miss_without_disk_change()
            cache.save(self.args)
        (self.tree / 'guest-link').unlink()
        (self.tree / 'guest-link').symlink_to('/different/guest/path')
        self.assert_miss_without_disk_change()

    def test_environment_and_cache_corruption_invalidate(self):
        cache.save(self.args)
        with patch.dict(os.environ, {'EXCLUDE_IMAGE_PATHS': 'usr/include'}):
            self.assert_miss_without_disk_change()
        for contents in ('invalid json', '[]', '{}'):
            self.args.metadata.write_text(contents)
            self.assert_miss_without_disk_change()
        cache.save(self.args)
        self.args.cache.chmod(0o644)
        self.assert_miss_without_disk_change()

    def test_missing_cache_and_open_disk(self):
        self.assert_miss_without_disk_change()
        cache.save(self.args)
        with self.args.disk.open('rb'):
            with self.assertRaisesRegex(RuntimeError, 'open by another process'):
                cache.restore(self.args)
            with self.assertRaisesRegex(RuntimeError, 'open by another process'):
                cache.save(self.args)

    def test_same_size_edit_with_restored_mtime_invalidates(self):
        cache.save(self.args)
        path = self.tree / 'input'
        before = path.stat()
        path.write_bytes(b'modified')
        os.utime(path, ns=(before.st_atime_ns, before.st_mtime_ns))
        self.assert_miss_without_disk_change()

    def test_recipe_payload_outside_declared_inputs_invalidates(self):
        (ROOT / 'build').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='avory-recipe-payload-', dir=ROOT / 'build') as folder:
            path = Path(folder) / 'payload'
            path.write_text('original')
            self.args.makefile.write_text(f'write {path.relative_to(ROOT)} /payload\n')
            cache.save(self.args)
            path.write_text('modified')
            self.assert_miss_without_disk_change()

    def test_input_change_during_restore_aborts_before_disk_replace(self):
        cache.save(self.args)
        actual = cache.apps.cache_partition
        def changed(*args):
            actual(*args)
            (self.tree / 'input').write_text('changed during restore')
        with patch.object(cache.apps, 'cache_partition', side_effect=changed):
            self.assert_miss_without_disk_change()


if __name__ == '__main__':
    unittest.main()
