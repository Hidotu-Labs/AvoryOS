#!/usr/bin/env python3
"""The actual installer must reject stale plain-tree blobs before publication."""
from pathlib import Path
import hashlib
import os
import shutil
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='firmware-pin-') as tmp:
    base=Path(tmp)
    scripts=base/'scripts'
    (scripts/'linux').mkdir(parents=True)
    installer=scripts/'linux-firmware-install.sh'
    shutil.copy2(ROOT/'scripts/linux-firmware-install.sh',installer)
    source=base/'source'
    (source/'amdgpu').mkdir(parents=True)
    blob=source/'amdgpu/test.bin'
    approved=b'approved-firmware-bundle'
    blob.write_bytes(approved)
    (scripts/'linux/firmware-manifest.txt').write_text('amdgpu/test.bin\n')
    (scripts/'linux/firmware-sha256-20260916.txt').write_text(
        hashlib.sha256(approved).hexdigest()+'  amdgpu/test.bin\n')
    env=dict(os.environ,LINUX_FIRMWARE_REF='20260916',LINUX_FIRMWARE_SRC=str(source))
    result=subprocess.run(['bash',str(installer)],env=env,capture_output=True,text=True)
    assert result.returncode==0,result.stdout+result.stderr
    published=base/'build/firmware/lib/firmware/amdgpu/test.bin'
    generated=base/'build/firmware/kpi_fw_manifest.h'
    assert published.read_bytes()==approved
    manifest=generated.read_bytes()
    blob.write_bytes(b'older-host-firmware')
    result=subprocess.run(['bash',str(installer)],env=env,capture_output=True,text=True)
    assert result.returncode!=0 and 'does not match the approved' in result.stderr
    assert published.read_bytes()==approved and generated.read_bytes()==manifest
    assert not list((base/'build/firmware').glob('.stage.*'))
print('PASS: plain source hash enforced; rejected old firmware leaves published bundle intact')
