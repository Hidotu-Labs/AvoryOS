#!/usr/bin/env python3
"""Exercise launcher recovery with real files/processes and a fake AppRun."""
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / 'scripts/mocktail-session.sh'


class SessionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='avory-session-test-')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.data = self.base / 'data with spaces'
        self.tmp = self.base / 'tmp'
        self.tmp.mkdir()
        self.cache = self.data / 'android/data/cache'
        self.appdata = self.data / 'android/data/files/appData'
        self.cache.mkdir(parents=True)
        self.appdata.mkdir(parents=True)
        self.clean = self.data / '.avory-session-clean'
        self.runtime_cache = self.base / 'runtime-cache'
        self.env = dict(os.environ, MOCKTAIL_DATA_ROOT=str(self.data), TMPDIR=str(self.tmp),
                        MOCKTAIL_CACHE_ROOT=str(self.runtime_cache))
        # Model Avory's directory rename restriction rather than silently
        # relying on Linux host semantics. Files may move across parents.
        test_bin = self.base / 'bin'
        test_bin.mkdir()
        mv = test_bin / 'mv'
        mv.write_text('''#!/usr/bin/env python3
import errno, os, pathlib, sys
args = [a for a in sys.argv[1:] if a not in ('-T', '--')]
if len(args) != 2:
    sys.exit('unexpected mv arguments')
src, dst = map(pathlib.Path, args)
if os.environ.get('MOCKTAIL_TEST_RENAME_FAIL') == '1':
    sys.exit('injected rename I/O failure')
if src.is_dir() and not src.is_symlink() and src.parent.resolve() != dst.parent.resolve():
    sys.exit(os.strerror(errno.EOPNOTSUPP))
if dst.exists() or dst.is_symlink():
    sys.exit('backup target already exists')
os.rename(src, dst)
''')
        mv.chmod(0o755)
        self.env['PATH'] = str(test_bin) + os.pathsep + os.environ['PATH']
        for relative in ('current.json', 'payloads/example/libroblox.so',
                         'auth/cookies', 'android/data/files/appData/LocalStorage/appStorage.json',
                         'android/data/files/appData/rbx-storage-sc/token',
                         'android/data/files/appData/rbx-storage.id',
                         'android/data/files/appData/logs/last.log', 'payloads/example/file.lock'):
            path = self.data / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('keep ' + relative)
        self.preserved = {p: p.read_bytes() for p in self.data.rglob('*') if p.is_file()}

    def command(self, *args):
        return ['sh', '-c', '. "$1"; shift; mocktail_session "$@"', 'launcher', str(HELPER), *args]

    def run_session(self, *args, **env):
        return subprocess.run(self.command(*args), env=dict(self.env, **env),
                              capture_output=True, text=True, timeout=5)

    def seed_caches(self):
        (self.cache / 'rbx-storage.db').write_bytes(b'partial database')
        (self.appdata / 'rbx-storage').mkdir()
        (self.appdata / 'rbx-storage/asset').write_bytes(b'partial asset')
        for suffix in ('', '-wal', '-shm', '-journal'):
            (self.appdata / ('rbx-storage.db' + suffix)).write_text('sidecar' + suffix)

    def assert_preserved(self):
        for path, contents in self.preserved.items():
            self.assertEqual(path.read_bytes(), contents)

    def test_upgrade_recovers_without_losing_userdata(self):
        self.seed_caches()
        endpoint = self.tmp / f'mocktail-{os.getuid()}'
        endpoint.mkdir()
        (endpoint / 'stale.socket').touch()
        result = self.run_session('sh', '-c', 'test -d "$MOCKTAIL_DATA_ROOT/android/data/cache"')
        self.assertEqual(result.returncode, 0, result.stderr)
        backup, = (self.data / 'avory-recovery').iterdir()
        cache_index = backup / 'android/data/cache'
        self.assertTrue(cache_index.is_symlink())
        self.assertEqual(cache_index.resolve().parent, self.cache.parent)
        self.assertTrue(cache_index.resolve().name.startswith('cache.avory-recovery.'))
        self.assertEqual((backup / 'android/data/cache/rbx-storage.db').read_bytes(), b'partial database')
        self.assertEqual((backup / 'android/data/files/appData/rbx-storage/asset').read_bytes(), b'partial asset')
        for suffix in ('', '-wal', '-shm', '-journal'):
            self.assertTrue((backup / ('android/data/files/appData/rbx-storage.db' + suffix)).exists())
        self.assertTrue(self.clean.exists())
        self.assertFalse(endpoint.exists())
        self.assert_preserved()

    def test_cross_parent_directory_move_is_rejected_by_guest_model(self):
        result = subprocess.run(['mv', '-T', '--', str(self.cache), str(self.base / 'moved')],
                                env=self.env, capture_output=True, text=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Operation not supported', result.stderr)
        self.assertTrue(self.cache.exists())

    def test_rename_failure_keeps_original_cache_and_stops_launch(self):
        self.seed_caches()
        called = self.base / 'called'
        result = self.run_session('touch', str(called), MOCKTAIL_TEST_RENAME_FAIL='1')
        self.assertEqual(result.returncode, 1)
        self.assertEqual((self.cache / 'rbx-storage.db').read_bytes(), b'partial database')
        self.assertEqual((self.appdata / 'rbx-storage/asset').read_bytes(), b'partial asset')
        self.assertFalse(called.exists())
        self.assertFalse(self.clean.exists())
        self.assertFalse(list(self.cache.parent.glob('cache.avory-recovery.*')))
        self.assert_preserved()

    def test_clean_restart_reuses_cache_and_forwards_arguments(self):
        self.clean.touch()
        self.seed_caches()
        result = self.run_session('printf', '%s\n', 'argument with spaces', 'second')
        self.assertEqual(result.stdout, 'argument with spaces\nsecond\n')
        self.assertEqual((self.cache / 'rbx-storage.db').read_bytes(), b'partial database')
        self.assertFalse((self.data / 'avory-recovery').exists())

    def test_failure_retains_unclean_state_and_exit_status(self):
        self.clean.touch()
        self.seed_caches()
        result = self.run_session('sh', '-c', 'exit 37')
        self.assertEqual(result.returncode, 37)
        self.assertFalse(self.clean.exists())
        self.assertEqual(self.run_session('true').returncode, 0)
        self.assertFalse((self.cache / 'rbx-storage.db').exists())
        self.assert_preserved()

    def start_running(self):
        self.clean.touch()
        self.seed_caches()
        ready = self.base / 'ready'
        child = self.base / 'child'
        process = subprocess.Popen(self.command('sh', '-c',
            'echo $$ > "$1"; touch "$2"; exec sleep 30', 'fake-app', str(child), str(ready)),
            env=self.env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(lambda: process.poll() is None and process.kill())
        deadline = time.monotonic() + 3
        while not ready.exists() and time.monotonic() < deadline:
            time.sleep(.01)
        self.assertTrue(ready.exists())
        child_pid = int(child.read_text())
        def reap_child():
            try:
                os.kill(child_pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=3)
        self.addCleanup(reap_child)
        return process, child_pid

    def test_concurrent_launch_never_changes_live_cache(self):
        process, _ = self.start_running()
        result = self.run_session('true', MOCKTAIL_RECOVER='1')
        self.assertEqual(result.returncode, 1)
        self.assertIn('already running', result.stderr)
        self.assertEqual((self.cache / 'rbx-storage.db').read_bytes(), b'partial database')
        self.assertIsNone(process.poll())

    def test_forced_kill_recovers_on_next_launch(self):
        process, child_pid = self.start_running()
        # Model a power cut: neither supervisor nor app gets an exit handler.
        os.kill(child_pid, signal.SIGKILL)
        process.kill()
        process.wait(timeout=3)
        self.assertFalse(self.clean.exists())
        result = self.run_session('true')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.cache / 'rbx-storage.db').exists())
        self.assert_preserved()

    def test_term_is_forwarded_and_remains_unclean(self):
        process, child_pid = self.start_running()
        process.terminate()
        self.assertEqual(process.wait(timeout=3), 143)
        self.assertFalse(self.clean.exists())
        self.assertFalse(list(self.tmp.glob('avory-mocktail-*.session')))

    def test_manual_recovery_of_existing_clean_marker(self):
        self.clean.touch()
        self.seed_caches()
        result = self.run_session('true', MOCKTAIL_RECOVER='1')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.cache / 'rbx-storage.db').exists())
        self.assert_preserved()

    def test_native_runtime_cache_is_recovered_and_recreated(self):
        android = self.runtime_cache / 'android'
        android.mkdir(parents=True)
        (android / 'asset').write_text('partial asset')
        result = self.run_session('sh', '-c', 'test -d "$MOCKTAIL_CACHE_ROOT/android"')
        self.assertEqual(result.returncode, 0, result.stderr)
        backup, = (self.data / 'avory-recovery').iterdir()
        self.assertEqual((backup / 'runtime-cache/android/asset').read_text(), 'partial asset')
        self.assertFalse((android / 'asset').exists())
        self.assert_preserved()

    def test_root_home_runtime_paths_and_parent_preparation(self):
        launcher = (ROOT / 'scripts/setup-mocktail.sh').read_text().split("<< 'LAUNCHER_EOF'\n", 1)[1].split('\nLAUNCHER_EOF', 1)[0]
        preparation = launcher.split('MOCKTAIL_HOME=', 1)[1].split('# Default to the software OpenGL', 1)[0]
        # Replace mkdir with an observer so HOME=/ does not touch host root.
        code = ('mkdir() { printf "mkdir:%s\\n" "$@"; }; MOCKTAIL_HOME=' + preparation +
                '\nprintf "cache:%s\\nstate:%s\\n" "$MOCKTAIL_CACHE_ROOT" "$XDG_STATE_HOME"')
        env = dict(self.env)
        for name in ('XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME', 'XDG_STATE_HOME', 'MOCKTAIL_CACHE_ROOT'):
            env.pop(name, None)
        env['HOME'] = '/'
        result = subprocess.run(['sh', '-c', code], env=env, capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('cache:/.cache/mocktail\n', result.stdout)
        self.assertIn('mkdir:/.cache/mocktail\n', result.stdout)
        self.assertIn('state:/.local/state\n', result.stdout)
        self.assertNotIn('//', result.stdout)
        env['XDG_CACHE_HOME'] = str(self.base / 'custom cache')
        result = subprocess.run(['sh', '-c', code], env=env, capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f'cache:{self.base}/custom cache/mocktail\n', result.stdout)
        self.assertIn(f'mkdir:{self.base}/custom cache/mocktail\n', result.stdout)

    def test_cache_creation_error_does_not_start_app(self):
        # A regular file cannot serve as the runtime cache root.
        self.clean.touch()
        self.runtime_cache.write_text('blocked')
        called = self.base / 'called'
        result = self.run_session('touch', str(called))
        self.assertEqual(result.returncode, 1)
        self.assertIn('Cannot prepare Android cache directory', result.stderr)
        self.assertFalse(called.exists())


if __name__ == '__main__':
    unittest.main()
