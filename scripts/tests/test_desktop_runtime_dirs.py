#!/usr/bin/env python3
"""Exercise image profile permissions and the boot-time Xwayland setup."""
from pathlib import Path
import os
import stat
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class DesktopRuntimeDirs(unittest.TestCase):
    def test_profile_repairs_private_runtime_permissions(self):
        source = (ROOT / 'scripts/configure-image-root.sh').read_text()
        profile = source.split("cat > \"${CONFIG_DIR}/bashrc\" <<'EOF'\n", 1)[1]
        profile = profile.split('\nEOF\n', 1)[0]
        start = profile.index('export XDG_RUNTIME_DIR=')
        block = profile[start:profile.index('\n# Root HOME', start)]
        with tempfile.TemporaryDirectory() as directory:
            runtime = Path(directory) / 'runtime'
            env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime))
            for mode in (None, 0o1777):
                if mode is not None:
                    runtime.chmod(mode)
                subprocess.run(['sh', '-ec', block], env=env, check=True)
                self.assertEqual(stat.S_IMODE(runtime.stat().st_mode), 0o700)
                self.assertEqual(runtime.stat().st_uid, os.getuid())

    def test_boot_hook_survives_reprovisioning_and_fresh_tmp(self):
        source = (ROOT / 'scripts/configure-accounts.sh').read_text()
        # Execute the actual provisioning fragment without changing accounts.
        provision = source.split('mkdir -p "${ROOTFS_DIR}/etc"', 1)[0]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            hook = root / 'sbin/avory-early-init'
            hook.parent.mkdir()
            hook.write_text('#!/bin/sh\n# existing hook\ntrue\n')
            for _ in range(2):
                subprocess.run(['sh', '-ec', provision, 'configure-accounts.sh',
                                str(root)], check=True)
            body = hook.read_text()
            self.assertIn('# existing hook\ntrue\n', body)
            self.assertEqual(body.count('# BEGIN avory desktop sockets'), 1)
            self.assertTrue(os.access(hook, os.X_OK))
            # The guest /tmp starts empty on every boot. Run against a private
            # directory, then verify it also repairs existing incorrect modes.
            guest_tmp = root / 'tmp'
            block = body.replace('/tmp/', str(guest_tmp) + '/')
            for mode in (None, 0o755):
                if mode is not None:
                    for child in guest_tmp.iterdir():
                        child.chmod(mode)
                subprocess.run(['sh', '-ec', block], check=True)
                for name in ('.X11-unix', '.ICE-unix'):
                    self.assertEqual(stat.S_IMODE((guest_tmp / name).stat().st_mode),
                                     0o1777)


if __name__ == '__main__':
    unittest.main()
