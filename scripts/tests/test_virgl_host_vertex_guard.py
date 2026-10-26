#!/usr/bin/env python3
"""Replay the indexed draw that crashed NVIDIA through real host virglrenderer."""
import argparse
import os
from pathlib import Path
import resource
import signal
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--library-dir',type=Path,default=ROOT/'build/virgl-host/lib')
parser.add_argument('--expect-crash',action='store_true',help='negative control: require SIGSEGV from the unpatched NVIDIA path')
args=parser.parse_args()
source=ROOT/'build/virgl-host-src'
build=ROOT/'build/virgl-host-build'
out=ROOT/'build/virgl-host-test'
if not (args.library_dir/'libvirglrenderer.so.1').exists():
    raise SystemExit('Build the local renderer first: make virgl-host')
out.mkdir(parents=True,exist_ok=True)
(out/'check.h').write_text('#include <assert.h>\n#define ck_assert_int_eq(a,b) assert((a)==(b))\n#define ck_assert_int_ne(a,b) assert((a)!=(b))\n#define ck_assert_int_ge(a,b) assert((a)>=(b))\n')
includes=[out,build,build/'src',build/'src/gallium',source/'src',source/'src/mesa',
          source/'src/mesa/pipe',source/'src/mesa/compat',source/'src/gallium/include',
          source/'src/gallium/auxiliary',source/'tests']
command=['cc','-O2','-g','-DHAVE_CONFIG_H=1','-include',str(build/'config.h')]
command += ['-I'+str(p) for p in includes]
command += [str(ROOT/'scripts/tests/virgl_host_vertex_repro.c'),str(source/'tests/testvirgl.c'),
            str(source/'tests/testvirgl_encode.c'),str(build/'src/gallium/libgallium.a'),
            str(build/'src/mesa/libmesa.a'),'-L'+str(args.library_dir.resolve()),
            '-Wl,-rpath,'+str(args.library_dir.resolve()),'-lvirglrenderer','-lm','-lpthread']
command += subprocess.check_output(['pkg-config','--cflags','--libs','epoxy'],text=True).split()
command += ['-o',str(out/'vertex-repro')]
subprocess.run(command,check=True)
# Deliberate negative controls must not create another large host core dump.
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
env=dict(os.environ,LD_LIBRARY_PATH=str(args.library_dir.resolve())+':'+os.environ.get('LD_LIBRARY_PATH',''))
result=subprocess.run([str(out/'vertex-repro')],env=env,timeout=30)
if args.expect_crash:
    if result.returncode!=-signal.SIGSEGV:
        raise SystemExit(f'Expected NVIDIA SIGSEGV, got status {result.returncode}')
    print('PASS: unpatched renderer reproduces the indexed-draw SIGSEGV')
elif result.returncode:
    raise SystemExit(f'Guarded indexed draw failed: status {result.returncode}')
