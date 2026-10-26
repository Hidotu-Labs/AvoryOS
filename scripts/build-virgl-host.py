#!/usr/bin/env python3
"""Build the pinned, locally guarded host virglrenderer without system installation."""
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT/'build/virgl-host-src'
BUILD = ROOT/'build/virgl-host-build'
PREFIX = ROOT/'build/virgl-host'
PATCH = ROOT/'scripts/virgl/missing-vertex-buffer.patch'
LEGACY_PATCH = ROOT/'scripts/virgl/missing-vertex-buffer-v1.patch'
LEGACY_BINDING_PATCH = ROOT/'scripts/virgl/nvidia-vertex-binding-v1.patch'
PATCHES = (PATCH, ROOT/'scripts/virgl/fog-semantic.patch',
           ROOT/'scripts/virgl/nvidia-vertex-binding.patch')
COMMIT = 'ca50e008863837e094747a69974dde3ae148aeaa'  # upstream 1.3.0

def run(args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)

def main():
    import fcntl
    (ROOT/'build').mkdir(exist_ok=True)
    with (ROOT/'build/virgl-host.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        for tool in ('git', 'meson', 'ninja', 'cc', 'pkg-config'):
            if not shutil.which(tool):
                raise SystemExit(f'Host virgl build needs {tool}; see docs/virgl.md')
        if not SOURCE.exists():
            run(['git', 'clone', '--depth', '1', '--branch', '1.3.0',
                 'https://gitlab.freedesktop.org/virgl/virglrenderer.git', SOURCE])
        revision = subprocess.check_output(['git', '-C', str(SOURCE), 'rev-parse', 'HEAD'], text=True).strip()
        if revision != COMMIT:
            raise SystemExit('Unexpected host virgl source revision; refusing to overwrite '+str(SOURCE))
        git = ['git', '-C', str(SOURCE), 'apply']
        for patch in PATCHES:
            # Upgrade the earlier installed guard without discarding unrelated
            # local source changes. Only reverse its exact, checked patch.
            legacy = LEGACY_PATCH if patch == PATCH else (LEGACY_BINDING_PATCH if patch.name == 'nvidia-vertex-binding.patch' else None)
            if (legacy is not None and
                subprocess.run([*git, '--reverse', '--check', str(patch)], capture_output=True).returncode and
                subprocess.run([*git, '--reverse', '--check', str(legacy)], capture_output=True).returncode == 0):
                run([*git, '--reverse', str(legacy)])
            if subprocess.run([*git, '--check', str(patch)], capture_output=True).returncode == 0:
                run([*git, str(patch)])
            elif subprocess.run([*git, '--reverse', '--check', str(patch)], capture_output=True).returncode:
                raise SystemExit('Host virgl source does not match '+patch.name+'; refusing to overwrite local changes')
        env = dict(os.environ, CCACHE_DISABLE='1')
        setup = ['meson', 'setup']
        if (BUILD/'build.ninja').exists():
            setup += ['--reconfigure']
        run([*setup, BUILD, SOURCE, '--prefix='+str(PREFIX), '--libdir=lib',
             '-Dtests=false', '-Dvideo=false', '-Dvenus=false', '-Dplatforms=egl,glx',
             '-Dbuildtype=debugoptimized'], env=env)
        run(['ninja', '-C', BUILD, '-j8'], env=env)
        # Replace the library atomically: another running VM may still map the
        # previous inode. Never truncate its executable pages during rebuild.
        libdir = PREFIX/'lib'
        libdir.mkdir(parents=True, exist_ok=True)
        name = 'libvirglrenderer.so.1.11.0'
        temporary = libdir/(name+'.new-'+str(os.getpid()))
        shutil.copy2(BUILD/'src'/name, temporary)
        os.utime(temporary, None)
        os.replace(temporary, libdir/name)
        for link, target in [('libvirglrenderer.so.1', name),
                             ('libvirglrenderer.so', 'libvirglrenderer.so.1')]:
            temporary_link = libdir/(link+'.new-'+str(os.getpid()))
            temporary_link.symlink_to(target)
            os.replace(temporary_link, libdir/link)
        print('Host virgl vertex-buffer guard ready: '+str(PREFIX/'lib/libvirglrenderer.so.1'))

if __name__ == '__main__':
    main()
