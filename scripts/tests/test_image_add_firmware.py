#!/usr/bin/env python3
"""Firmware installation preserves existing GPT/ext4 files and rejects bad input."""
from pathlib import Path
import hashlib, importlib.util, json, shutil, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('install',ROOT/'scripts/image-add-firmware.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
with tempfile.TemporaryDirectory(prefix='nouveau-disk-test-') as tmp:
 base=Path(tmp);(base/'scripts').mkdir();shutil.copy2(ROOT/'scripts/image-runtime.py',base/'scripts/image-runtime.py');m.ROOT=base
 root=base/'root';root.mkdir();(root/'keep-my-desktop').write_bytes(b'existing installed system')
 partition=base/'partition';partition.touch();partition.write_bytes(b'')
 with partition.open('r+b') as stream:stream.truncate(32*1024*1024)
 subprocess.run(['mkfs.ext4','-q','-F','-d',str(root),str(partition)],check=True)
 disk=base/'disk.img'
 with disk.open('wb') as stream:stream.truncate(36*1024*1024)
 subprocess.run(['sfdisk',str(disk)],input='label: gpt\nstart=2048,size=65536,type=4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709\n',text=True,stdout=subprocess.DEVNULL,check=True)
 with disk.open('r+b') as dst,partition.open('rb') as src:dst.seek(1048576);shutil.copyfileobj(src,dst)
 before=disk.read_bytes();bundle=base/'bundle';fw=bundle/'lib/firmware/nvidia/ad106/gsp/test.bin';fw.parent.mkdir(parents=True);fw.write_bytes(b'pinned firmware'*1024)
 license=bundle/'lib/firmware/LICENSES/LICENCE.nvidia';license.parent.mkdir();license.write_text('license')
 digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
 (bundle/'manifest.json').write_text(json.dumps(dict(files=[dict(request='nvidia/ad106/gsp/test.bin',sha256=digest(fw))],license_sha256=digest(license))))
 m.install(disk,bundle);after=disk.read_bytes()
 assert before[:1048576]==after[:1048576] and before[-1048576:]==after[-1048576:]
 partition.write_bytes(after[1048576:1048576+32*1024*1024])
 kept=subprocess.check_output(['debugfs','-R','cat /keep-my-desktop',str(partition)],stderr=subprocess.DEVNULL)
 assert kept==b'existing installed system'
 installed=subprocess.check_output(['debugfs','-R','cat /lib/firmware/nvidia/ad106/gsp/test.bin',str(partition)],stderr=subprocess.DEVNULL)
 assert installed==fw.read_bytes()
 inode=disk.stat().st_ino;m.install(disk,bundle);assert inode==disk.stat().st_ino
 fw.write_bytes(b'bad input');old=digest(disk)
 try:m.install(disk,bundle)
 except RuntimeError:pass
 else:raise AssertionError('accepted modified firmware')
 assert old==digest(disk)
 fw.write_bytes(b'pinned firmware'*1024)
 manifest=json.loads((bundle/'manifest.json').read_text());manifest.pop('license_sha256')
 (bundle/'manifest.json').write_text(json.dumps(manifest))
 m.install(disk,bundle)
 print('PASS: atomic GPT firmware install preserves installed files/partition table; cached rerun, bad-bundle rejection and optional license metadata')
