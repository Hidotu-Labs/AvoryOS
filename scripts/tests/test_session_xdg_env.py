#!/usr/bin/env python3
"""Session launchers must derive canonical XDG directories when HOME="/".

Root's home is "/" on AvoryOS. A naive "$HOME/..." join then yields
"//.cache"-style paths, which WebKitGTK rejects as sandbox roots (Epiphany
aborts on the resulting critical, see docs/epiphany-pulse-launch-abort.md).

These tests execute the real fragments that set the session environment and
fail when any of them reintroduces a double leading slash.
"""
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Mirrors the upstream xfce4-session /etc/xdg/xfce4/xinitrc fallbacks.
UPSTREAM_XINITRC_FIXTURE = '''\
if test "x$XDG_CONFIG_HOME" = "x" ; then
  XDG_CONFIG_HOME=$HOME/.config
  export XDG_CONFIG_HOME
fi
[ -d "$XDG_CONFIG_HOME" ] || mkdir "$XDG_CONFIG_HOME"
if test "x$XDG_CACHE_HOME" = "x" ; then
  XDG_CACHE_HOME=$HOME/.cache
  export XDG_CACHE_HOME
fi
[ -d "$XDG_CACHE_HOME" ] || mkdir "$XDG_CACHE_HOME"
'''

CANONICAL = {
    'XDG_CONFIG_HOME': '/.config',
    'XDG_CACHE_HOME': '/.cache',
    'XDG_DATA_HOME': '/.local/share',
    'XDG_STATE_HOME': '/.local/state',
}

SESSION_FILES = (
    'AetherDE/scripts/sax11.sh',
    'userland/avory-installer.c',
    'scripts/setup-arch.sh',
    'scripts/avory-installer.sh',
    'scripts/build-minimal.sh',
)


def clean_env(**extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith('XDG_')}
    env.pop('HOME', None)
    env.update(extra)
    return env


def run_shell(code, env):
    return subprocess.run(['sh', '-c', code], env=env, capture_output=True,
                          text=True, timeout=10)


def parse_env(text):
    values = {}
    for line in text.splitlines():
        if '=' in line:
            name, value = line.split('=', 1)
            values[name] = value
    return values


class SessionXdgEnvTests(unittest.TestCase):
    def assert_canonical(self, values, prefix=''):
        for name, expected in CANONICAL.items():
            self.assertEqual(values.get(name), prefix + expected, name)
            self.assertFalse(values[name].startswith('//'), name)

    def wrapper_source(self):
        script = (ROOT / 'scripts/setup-arch.sh').read_text()
        marker = 'cat > "${ROOTFS_DIR}/usr/bin/start-xfce4-wayland" <<\'EOF\'\n'
        self.assertEqual(script.count(marker), 1,
                         'start-xfce4-wayland wrapper heredoc not found')
        body = script.split(marker, 1)[1].split('\nEOF\n', 1)[0]
        return body + '\n'

    def run_wrapper(self, **env_overrides):
        with tempfile.TemporaryDirectory(prefix='avory-xdg-wrapper-') as temp:
            temp = Path(temp)
            wrapper = temp / 'start-xfce4-wayland'
            wrapper.write_text(self.wrapper_source())
            wrapper.chmod(0o755)
            bindir = temp / 'bin'
            bindir.mkdir()
            dump = temp / 'env.dump'
            stub = bindir / 'startxfce4'
            stub.write_text('#!' + (shutil.which('sh') or '/bin/sh')
                            + '\n' + shutil.which('env') + ' > "$DUMP_FILE"\n')
            stub.chmod(0o755)
            env = clean_env(PATH=str(bindir), DUMP_FILE=str(dump), **env_overrides)
            result = subprocess.run([str(wrapper)], env=env, capture_output=True,
                                    text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            return parse_env(dump.read_text())

    def test_wrapper_home_root_yields_canonical_dirs(self):
        self.assert_canonical(self.run_wrapper(HOME='/'))

    def test_wrapper_defaults_missing_home_to_root(self):
        self.assert_canonical(self.run_wrapper())

    def test_wrapper_preserves_explicit_xdg_values(self):
        values = self.run_wrapper(HOME='/', XDG_CACHE_HOME='/custom/cache')
        self.assertEqual(values['XDG_CACHE_HOME'], '/custom/cache')
        self.assertEqual(values['XDG_CONFIG_HOME'], '/.config')
        self.assertEqual(values['XDG_DATA_HOME'], '/.local/share')
        self.assertEqual(values['XDG_STATE_HOME'], '/.local/state')

    def test_wrapper_real_user_home_still_relative(self):
        self.assert_canonical(self.run_wrapper(HOME='/root'), prefix='/root')

    def test_unpatched_xfce_fallback_reproduces_double_slash(self):
        # The fixture must keep modelling the upstream bug: this is the exact
        # derivation that produced //.cache/epiphany in the abort log.
        code = ('mkdir() { :; }\nHOME=/\nunset XDG_CONFIG_HOME XDG_CACHE_HOME\n'
                + UPSTREAM_XINITRC_FIXTURE
                + 'printf "%s\\n" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME"\n')
        result = run_shell(code, clean_env(HOME='/'))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, '//.config\n//.cache\n')

    def test_xfce_xinitrc_fallbacks_are_canonicalised(self):
        script = (ROOT / 'scripts/setup-arch.sh').read_text()
        exprs = re.findall(r"-e '([^']*XDG_(?:CONFIG|CACHE)_HOME=[^']*)'", script)
        self.assertGreaterEqual(len(exprs), 2,
                                'xinitrc canonicalisation sed missing from setup-arch.sh')
        sed_args = []
        for expr in exprs:
            sed_args += ['-e', expr]
        with tempfile.TemporaryDirectory(prefix='avory-xdg-xinitrc-') as temp:
            fixture = Path(temp) / 'xinitrc.fragment'
            fixture.write_text(UPSTREAM_XINITRC_FIXTURE)
            result = subprocess.run(['sed', *sed_args, str(fixture)],
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            patched = result.stdout
        self.assertNotIn('$HOME/.config', patched)
        self.assertNotIn('$HOME/.cache', patched)
        for home, expected in (('/', '/.config\n/.cache\n'),
                               ('/root', '/root/.config\n/root/.cache\n')):
            code = ('mkdir() { :; }\nHOME=' + home + '\n'
                    'unset XDG_CONFIG_HOME XDG_CACHE_HOME\n' + patched
                    + 'printf "%s\\n" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME"\n')
            result = run_shell(code, clean_env(HOME=home))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, expected)

    def test_staged_xfce_xinitrc_is_patched(self):
        staged = ROOT / 'build/alpine/rootfs/etc/xdg/xfce4/xinitrc'
        if not staged.exists():
            self.skipTest('staged rootfs not present')
        text = staged.read_text(errors='replace')
        self.assertNotIn('XDG_CONFIG_HOME=$HOME/.config', text)
        self.assertNotIn('XDG_CACHE_HOME=$HOME/.cache', text)
        self.assertIn('XDG_CONFIG_HOME=${HOME%/}/.config', text)
        self.assertIn('XDG_CACHE_HOME=${HOME%/}/.cache', text)

    def plasma_wrapper_source(self):
        script = (ROOT / 'scripts/configure-accounts.sh').read_text()
        marker = ('cat > "${ROOTFS_DIR}/usr/bin/startplasma-x11"'
                  " << 'WRAPPER_EOF'\n")
        self.assertEqual(script.count(marker), 1,
                         'startplasma-x11 wrapper heredoc not found')
        body = script.split(marker, 1)[1].split('\nWRAPPER_EOF\n', 1)[0]
        return body + '\n'

    def plasma_wrapper_xdg_block(self):
        body = self.plasma_wrapper_source()
        start = body.index('if [ -z "$HOME" ] || [ "$HOME" = "/" ]; then')
        tail = body[start:]
        return tail[:tail.index('\nfi\n') + len('\nfi\n')]

    def test_plasma_wrapper_root_branch_pins_canonical_dirs(self):
        block = self.plasma_wrapper_xdg_block()
        root_branch = block.split('\nelse\n', 1)[0]
        self.assertIn('[ "$HOME" = "/" ]', root_branch)
        self.assertNotIn('$HOME/', root_branch)
        for home, expected in (
                ('', '/.config\n/.cache\n/.local/share\n/.local/state\n'),
                ('/', '/.config\n/.cache\n/.local/share\n/.local/state\n'),
                ('/root',
                 '/root/.config\n/root/.cache\n/root/.local/share'
                 '\n/root/.local/state\n')):
            code = ('mkdir() { :; }\nchmod() { :; }\nHOME=' + home + '\n'
                    'unset XDG_CONFIG_HOME XDG_CACHE_HOME XDG_DATA_HOME'
                    ' XDG_STATE_HOME\n' + block
                    + '\nprintf "%s\\n" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME"'
                      ' "$XDG_DATA_HOME" "$XDG_STATE_HOME"\n')
            result = run_shell(code, clean_env(HOME=home))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, expected)

    def test_staged_plasma_launcher_handles_root_home(self):
        staged = ROOT / 'build/alpine/rootfs/usr/bin/startplasma-x11'
        if not staged.exists():
            self.skipTest('staged rootfs not present')
        text = staged.read_text(errors='replace')
        self.assertIn('export XDG_CACHE_HOME="/.cache"', text)
        self.assertIn('export XDG_CONFIG_HOME="/.config"', text)

    def test_profile_pins_canonical_xdg_dirs(self):
        makefile = (ROOT / 'GNUmakefile').read_text()
        echo_lines = re.findall(r"echo '([^']*)' >> /tmp/bashrc;", makefile)
        pins = [line.replace('$$', '$') for line in echo_lines
                if '_avory_home' in line
                or re.search(r'XDG_(?:CONFIG|CACHE|DATA|STATE)_HOME', line)]
        self.assertGreaterEqual(len(pins), 5,
                                'profile XDG pinning missing from GNUmakefile')
        code = ('mkdir() { :; }\nHOME=/\n' + '\n'.join(pins)
                + '\nprintf "%s\\n" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME"'
                  ' "$XDG_DATA_HOME" "$XDG_STATE_HOME"\n')
        result = run_shell(code, clean_env(HOME='/'))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout,
                         '/.config\n/.cache\n/.local/share\n/.local/state\n')

    def test_session_entry_points_join_canonical_xdg_defaults(self):
        for relative in SESSION_FILES:
            text = (ROOT / relative).read_text().replace('\\"', '"')
            with self.subTest(relative):
                self.assertIn('_avory_home="${_avory_home%/}"', text)
                for match in re.finditer(
                        r'.*export XDG_(?:CONFIG|CACHE|DATA|STATE)_HOME=.*', text):
                    self.assertNotIn('$HOME/', match.group(0),
                                     'unguarded $HOME join in ' + relative)
                for match in re.finditer(r'.*XAUTHORITY=.*', text):
                    self.assertNotIn('"$HOME/', match.group(0))

    def test_wrapper_is_rewritten_unconditionally(self):
        script = (ROOT / 'scripts/setup-arch.sh').read_text()
        self.assertIn('cat > "${ROOTFS_DIR}/usr/bin/start-xfce4-wayland" <<\'EOF\'',
                      script)
        self.assertNotIn('if [ ! -e "${ROOTFS_DIR}/usr/bin/start-xfce4-wayland" ]',
                         script)

    def test_direct_boot_scripts_are_gone(self):
        for relative in ('initrd/startw.sh', 'initrd/startx.sh',
                         'initrd/avoryd/services/wayland.service',
                         'initrd/avoryd/services/x11.service'):
            self.assertFalse((ROOT / relative).exists(), relative)

    def test_makefile_does_not_restore_direct_boot_scripts(self):
        makefile = (ROOT / 'GNUmakefile').read_text()
        self.assertNotIn('write initrd/startx.sh', makefile)
        self.assertNotIn('write initrd/startw.sh', makefile)
        self.assertNotIn('write initrd/avoryd/services/x11.service', makefile)
        self.assertNotIn('write initrd/avoryd/services/wayland.service', makefile)
        # Stale copies from the Arch rootfs must still be removed.
        self.assertIn('echo "rm bin/startx.sh";', makefile)
        self.assertIn('echo "rm bin/startw.sh";', makefile)


if __name__ == '__main__':
    unittest.main()
