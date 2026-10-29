#!/usr/bin/env python3
"""Build the pinned Virgl-only guest Mesa library into a private bundle."""
import argparse
import fcntl
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

ROOT=Path(__file__).resolve().parents[1]
VERSION='26.2.5'
DSO='libgallium-26.2.5-arch1.1.so'
SHA256='794b3d1729075dae7ebd47c7283b7ca0cdb5dec668f7f11cc0a5d82f6138a0a5'
WORK=ROOT/'build/mesa-virgl'
SOURCE=WORK/('mesa-'+VERSION)
OBJECTS=WORK/'obj'
BUNDLE=ROOT/'build/virgl-guest/lib'
PATCHES=[ROOT/'scripts/virgl/guest/resource-membership.patch']


def run(args,**kwargs):
    return subprocess.run([str(arg) for arg in args],check=True,**kwargs)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs',type=int,default=min(8,os.cpu_count() or 1))
    args=parser.parse_args()
    if args.jobs<1:parser.error('--jobs must be positive')
    WORK.mkdir(parents=True,exist_ok=True)
    with (WORK/'build.lock').open('w') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX)
        for tool in ('meson','ninja','cc','c++','pkg-config','patch','strip'):
            if not shutil.which(tool):raise SystemExit('Missing build dependency: '+tool)
        archive=WORK/('mesa-'+VERSION+'.tar.xz')
        if not archive.exists():
            temporary=archive.with_suffix('.download')
            urllib.request.urlretrieve('https://archive.mesa3d.org/'+archive.name,temporary)
            if hashlib.sha256(temporary.read_bytes()).hexdigest()!=SHA256:
                temporary.unlink();raise SystemExit('Mesa archive checksum mismatch')
            temporary.replace(archive)
        if hashlib.sha256(archive.read_bytes()).hexdigest()!=SHA256:
            raise SystemExit('Mesa archive checksum mismatch')
        if not SOURCE.exists():
            with tarfile.open(archive) as tar:tar.extractall(WORK,filter='data')
        # Keep the existing distro DSO ABI name; other guest GPUs retain their
        # installed Mesa because this library is staged in a private directory.
        version_file=SOURCE/'VERSION'
        if version_file.read_text().strip() not in (VERSION,VERSION+'-arch1.1'):
            raise SystemExit('Unexpected source version; refusing to overwrite it')
        version_file.write_text(VERSION+'-arch1.1\n')
        for patch in PATCHES:
            common=['patch','--batch','--silent','-p1','-i',patch]
            if subprocess.run([str(a) for a in common]+['--dry-run','--forward'],cwd=SOURCE,capture_output=True).returncode==0:
                run(common+['--forward'],cwd=SOURCE)
            elif subprocess.run([str(a) for a in common]+['--dry-run','--reverse'],cwd=SOURCE,capture_output=True).returncode:
                raise SystemExit('Source conflicts with '+patch.name)
        env=dict(os.environ,CCACHE_DISABLE='1')
        setup=['meson','setup']
        if (OBJECTS/'build.ninja').exists():setup+=['--reconfigure']
        run(setup+[OBJECTS,SOURCE,'--prefix=/usr','--libdir=lib','-Dgallium-drivers=virgl',
                   '-Dvulkan-drivers=[]','-Dllvm=disabled','-Dplatforms=x11,wayland',
                   '-Dglx=dri','-Dglvnd=enabled','-Dgallium-va=disabled',
                   '-Dbuild-tests=false','-Dbuildtype=release'],env=env)
        target='src/gallium/targets/dri/'+DSO
        run(['ninja','-C',OBJECTS,'-j'+str(args.jobs),target],env=env)
        BUNDLE.mkdir(parents=True,exist_ok=True)
        temporary=BUNDLE/(DSO+'.new')
        shutil.copy2(OBJECTS/target,temporary)
        run(['strip','--strip-unneeded',temporary])
        temporary.replace(BUNDLE/DSO)
        print('Guest Virgl Mesa bundle ready: '+str(BUNDLE/DSO))


if __name__=='__main__':main()
