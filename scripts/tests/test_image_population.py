#!/usr/bin/env python3
"""Exercise the population fast path against real ext4 inodes and data."""
import os
import shlex
import shutil
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PopulationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='avory-populate-test-')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.source = self.base / 'source with spaces'
        self.source.mkdir()
        self.image = self.base / 'image'
        with self.image.open('wb') as stream:
            stream.truncate(32 * 1024 * 1024)
        subprocess.run(['mkfs.ext4', '-q', '-F', str(self.image)], check=True)

    def populate(self, destination='/', **env):
        return subprocess.run([str(ROOT / 'scripts/populate-ext2-dir.sh'),
                               str(self.image), str(self.source), destination],
                              env={**os.environ, **env}, check=True,
                              capture_output=True, text=True)

    def debugfs(self, command):
        result = subprocess.run(['debugfs', '-R', command, str(self.image)],
                                check=True, capture_output=True, text=True)
        return result.stdout

    def test_metadata_symlinks_and_quoted_names(self):
        directory = self.source / 'directory with spaces'
        directory.mkdir()
        file = directory / r'a\x2dfile'
        file.write_bytes(b'file contents\0\xff')
        file.chmod(0o4755)
        os.utime(file, (1700000000, 1700000000))
        (directory / 'link').symlink_to(r'/directory with spaces/a\x2dfile')
        # A directory symlink must remain a link, without copying host files.
        (self.source / 'host-dir-link').symlink_to(self.base, target_is_directory=True)
        directory.chmod(0o2750)
        os.utime(directory, (1700000010, 1700000010))
        self.populate()
        inode = self.debugfs(r'stat "/directory with spaces/a\x2dfile"')
        self.assertRegex(inode, r'Mode:\s+04755')
        self.assertIn('6553f100', inode)  # 1700000000
        inode = self.debugfs('stat "/directory with spaces"')
        self.assertRegex(inode, r'Mode:\s+02750')
        self.assertIn('6553f10a', inode)  # restored after child creation
        self.assertIn('Type: symlink', self.debugfs('stat /host-dir-link'))
        self.assertIn('Type: symlink', self.debugfs('stat "/directory with spaces/link"'))
        dumped = self.base / 'dumped'
        self.debugfs(fr'dump "/directory with spaces/a\x2dfile" {dumped}')
        self.assertEqual(dumped.read_bytes(), file.read_bytes())

    def test_exclusions_and_targeted_overlay(self):
        for relative in ('usr/include/header.h', 'usr/share/doc/readme',
                         'usr/share/man/man1/tool.1', 'usr/lib/libthing.a',
                         'usr/lib/runtime.so'):
            path = self.source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(relative)
        self.populate()
        for path in ('/usr/include', '/usr/share/doc', '/usr/share/man', '/usr/lib/libthing.a'):
            self.assertNotIn('Inode:', self.debugfs(f'stat {path}'))
        self.assertIn('Inode:', self.debugfs('stat /usr/lib/runtime.so'))
        self.source = self.source / 'usr/include'
        self.populate('opt/headers', EXCLUDE_IMAGE_STATIC='')
        self.assertIn('Inode:', self.debugfs('stat /opt/headers/header.h'))
        self.source = self.source.parent / 'lib'
        self.populate('usr/lib', EXCLUDE_IMAGE_STATIC='')
        self.assertIn('Inode:', self.debugfs('stat /usr/lib/libthing.a'))
        self.assertEqual(self.debugfs('cat /usr/lib/runtime.so'), 'usr/lib/runtime.so')

    def test_mixed_file_sizes_preserve_data_metadata_and_directory_times(self):
        folder = self.source / 'mixed'
        folder.mkdir()
        expected = bytes(range(251)) * 8500
        large = folder / 'large'
        large.write_bytes(expected)
        large.chmod(0o4755)
        os.utime(large, (1700000000, 1700000000))
        (folder / 'small').write_text('small file')
        (folder / 'link').symlink_to('/mixed/large')
        folder.chmod(0o2750)
        os.utime(folder, (1700000010, 1700000010))
        self.populate()
        dumped = self.base / 'large.dump'
        self.debugfs(f'dump /mixed/large {dumped}')
        self.assertEqual(dumped.read_bytes(), expected)
        self.assertEqual(self.debugfs('cat /mixed/small'), 'small file')
        self.assertRegex(self.debugfs('stat /mixed/large'), r'Mode:\s+04755')
        self.assertIn('6553f100', self.debugfs('stat /mixed/large'))
        self.assertIn('6553f10a', self.debugfs('stat /mixed'))
        self.assertRegex(self.debugfs('stat /mixed'), r'Mode:\s+02750')
        self.assertIn('Type: symlink', self.debugfs('stat /mixed/link'))
        subprocess.run(['e2fsck', '-f', '-n', str(self.image)], check=True, capture_output=True)

    def test_older_debugfs_falls_back_before_writes(self):
        if Path('/run/host/usr/bin/debugfs').is_file():
            self.skipTest('host debugfs takes precedence over PATH')
        real_debugfs = shutil.which('debugfs')
        binary = self.base / 'bin'
        binary.mkdir()
        wrapper = binary / 'debugfs'
        rejected = self.base / 'rejected-options'
        wrapper.write_text('#!/bin/sh\ncase "$*" in *"?cache_blocks="*)\n'
                           f'echo rejected >> {shlex.quote(str(rejected))}\n'
                           'exit 0;; esac\n'
                           f'exec {shlex.quote(real_debugfs)} "$@"\n')
        wrapper.chmod(0o755)
        (self.source / 'payload').write_text('fallback contents')
        (self.source / 'large').write_bytes(b'x' * (2 * 1024 * 1024))
        self.populate(PATH=str(binary) + os.pathsep + os.environ['PATH'])
        self.assertEqual(rejected.read_text(), 'rejected\n')
        self.assertEqual(self.debugfs('cat /payload'), 'fallback contents')
        subprocess.run(['e2fsck', '-f', '-n', str(self.image)], check=True, capture_output=True)

    def test_invalid_buffer_size_does_not_mutate_image(self):
        (self.source / 'payload').touch()
        before = self.image.read_bytes()
        for value in ('0', '4097', 'invalid'):
            with self.subTest(value=value):
                with self.assertRaises(subprocess.CalledProcessError):
                    self.populate(AVORY_IMAGE_IO_CACHE_BLOCKS=value)
                self.assertEqual(self.image.read_bytes(), before)

    def test_invalid_batch_does_not_mutate_image(self):
        before = self.image.read_bytes()
        for name in ('invalid\nname', 'invalid"name'):
            with self.subTest(name=name):
                path = self.source / name
                path.touch()
                with self.assertRaises(subprocess.CalledProcessError):
                    self.populate()
                self.assertEqual(self.image.read_bytes(), before)
                path.unlink()


if __name__ == '__main__':
    unittest.main()
