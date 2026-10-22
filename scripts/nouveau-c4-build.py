#!/usr/bin/env python3
"""Build the C4 candidate without writing the normal kernel output or ISO."""
from pathlib import Path
import argparse, fcntl, hashlib, importlib.util, shutil, subprocess
ROOT=Path(__file__).resolve().parents[1]
BUILD_LOCK=None
IMPORT_LOCK=None

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('-j','--jobs',type=int,default=8);a=p.parse_args()
 if a.jobs<1:p.error('job count must be positive')
 global BUILD_LOCK, IMPORT_LOCK
 (ROOT/'build/nouveau-c4').mkdir(parents=True,exist_ok=True)
 BUILD_LOCK=(ROOT/'build/nouveau-c4/build.lock').open('a')
 fcntl.flock(BUILD_LOCK,fcntl.LOCK_EX|fcntl.LOCK_NB)
 IMPORT_LOCK=(ROOT/'kernel/.linux-import.lock').open('a')
 fcntl.flock(IMPORT_LOCK,fcntl.LOCK_SH|fcntl.LOCK_NB)
 baseline=dict(x.split('=',1) for x in (ROOT/'scripts/linux/baseline.txt').read_text().splitlines() if '=' in x)
 pin=dict(x.split('=',1) for x in (ROOT/'kernel/linux/.pin').read_text().splitlines() if '=' in x)
 if any(pin.get(k)!=baseline[k] for k in ('tag','commit')):raise RuntimeError('import the selected Linux baseline before building C4')
 spec=importlib.util.spec_from_file_location('nvstage',ROOT/'scripts/nouveau-firmware-stage.py');mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
 mod.stage(ROOT/'build/nouveau-c0/firmware',ROOT/'build/nouveau/firmware',ROOT/'docs/nouveau/c0-lock.json')
 text=(ROOT/'kernel/GNUmakefile').read_text();needle='override OUTPUT := kernel\n'
 if text.count(needle)!=1:raise RuntimeError('kernel output declaration changed; refusing to guess')
 out=ROOT/'build/nouveau-c4';out.mkdir(parents=True,exist_ok=True)
 # Imported Kbuild carries target-specific FPU/include flags; relocate those
 # targets together with all object/dependency/profile paths.
 kbuild=ROOT/'kernel/linux/Makefile.kbuild'
 fragment=out/'Makefile.kbuild'
 fragment.write_text(kbuild.read_text().replace('obj-$(ARCH)', 'obj-nouveau-c4-$(ARCH)'))
 text=text.replace(needle,'override OUTPUT := kernel-c4\n').replace('obj-$(ARCH)', 'obj-nouveau-c4-$(ARCH)')
 text=text.replace('-include linux/Makefile.kbuild', f'-include {fragment}')
 makefile=ROOT/'build/nouveau-c4-kernel.make';makefile.write_text(text)
 log=out/'build-candidate.log'
 with log.open('w') as stream:
  subprocess.run(['make','-C',str(ROOT/'kernel'),'-f',str(makefile),f'-j{a.jobs}','KPI_AMDGPU=1','KPI_NOUVEAU=1'],stdout=stream,stderr=subprocess.STDOUT,check=True)
 source=ROOT/'kernel/bin-x86_64/kernel-c4';shutil.copyfile(source,out/'kernel.tmp');(out/'kernel.tmp').replace(out/'kernel')
 print(f'C4 candidate: {out}/kernel\nSHA256: {hashlib.sha256((out/"kernel").read_bytes()).hexdigest()}\nBuild log: {log}')
if __name__=='__main__':
 try:main()
 except (OSError,RuntimeError,subprocess.CalledProcessError) as error:raise SystemExit(str(error))
