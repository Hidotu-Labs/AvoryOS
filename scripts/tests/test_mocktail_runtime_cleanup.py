#!/usr/bin/env python3
"""Run the installer's actual retirement blocks against disposable fixtures."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / 'scripts/setup-mocktail.sh').read_text()


class RuntimeCleanupTests(unittest.TestCase):
    def test_selected_runtime_survives_host_cleanup(self):
        block = SOURCE[SOURCE.index('# Retire the runtimes being replaced.'):]
        block = block.split('${SUDO} rm -f', 1)[0]
        for version in ('1.0.3', '1.0.4', 'continuous'):
            with self.subTest(version=version), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                for name in ('1.0.3', '1.0.4', 'continuous'):
                    runtime = root / 'opt' / ('mocktail-' + name)
                    runtime.mkdir(parents=True)
                    (runtime / 'AppRun').write_text('fixture')
                selected = root / 'opt' / ('mocktail-' + version)
                subprocess.run(['bash', '-eu', '-c', block], check=True,
                               env={**os.environ, 'ROOTFS_DIR': tmp,
                                    'BUNDLE_ROOT': str(selected), 'SUDO': ''})
                self.assertTrue((selected / 'AppRun').is_file())
                for name in ('1.0.4', 'continuous'):
                    if name != version:
                        self.assertFalse((root / 'opt' / ('mocktail-' + name)).exists())

    def test_selected_runtime_never_sent_to_image_removal(self):
        # Extract the commands after the runtime has been copied into the image.
        start = SOURCE.index('# Drop runtimes from previous installs')
        block = SOURCE[start:SOURCE.index('LEGACY_CMDS=', start)]
        for version in ('1.0.3', '1.0.4', 'continuous'):
            with self.subTest(version=version), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                debugfs = root / 'debugfs'
                debugfs.write_text('#!/bin/sh\nprintf "%s\\n" "$3" >> "$REMOVE_LOG"\n')
                debugfs.chmod(0o755)
                log = root / 'commands'
                selected = '/opt/mocktail-' + version
                subprocess.run(['bash', '-eu', '-c', block], check=True,
                               env={**os.environ, 'DEBUGFS_BIN': str(debugfs),
                                    'PART_IMG': str(root / 'disposable.img'),
                                    'RUNTIME_INSTALL_DIR': selected, 'REMOVE_LOG': str(log)})
                removed = set(log.read_text().splitlines())
                self.assertNotIn('rm_rf ' + selected, removed)
                self.assertEqual(removed, {'rm_rf ' + p for p in
                                 ('/opt/mocktail/usr', '/opt/mocktail-continuous',
                                  '/opt/mocktail-1.0.4') if p != selected})


if __name__ == '__main__':
    unittest.main()
