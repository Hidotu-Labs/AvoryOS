#!/usr/bin/env python3
"""Firmware installation preserves existing GPT/ext4 files and rejects bad input."""
from pathlib import Path
from unittest.mock import patch
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
 cache=base/'build/arch/disk-partition.img';cache.parent.mkdir(parents=True);cache.write_bytes(b'stale cache')
 before=disk.read_bytes();bundle=base/'bundle';fw=bundle/'lib/firmware/nvidia/ad106/gsp/test.bin';fw.parent.mkdir(parents=True);fw.write_bytes(b'pinned firmware'*1024)
 license=bundle/'lib/firmware/LICENSES/LICENCE.nvidia';license.parent.mkdir();license.write_text('license')
 digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
 (bundle/'manifest.json').write_text(json.dumps(dict(files=[dict(request='nvidia/ad106/gsp/test.bin',sha256=digest(fw))],license_sha256=digest(license))))
 m.install(disk,bundle);after=disk.read_bytes()
 assert before[:1048576]==after[:1048576] and before[-1048576:]==after[-1048576:]
 assert cache.read_bytes()==after[1048576:1048576+32*1024*1024]
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
 # Existing firmware directories must not leak unattached inodes on a repeat install.
 partition.write_bytes(disk.read_bytes()[1048576:1048576+32*1024*1024])
 subprocess.run(['e2fsck','-f','-n',str(partition)],check=True,capture_output=True)
 # Simulate a guest overwriting a firmware data block without replacing disk.img.
 block=int(subprocess.check_output(['debugfs','-R','bmap /lib/firmware/nvidia/ad106/gsp/test.bin 0',str(partition)],stderr=subprocess.DEVNULL))
 superblock=partition.read_bytes()[1024:2048]
 block_size=1024 << int.from_bytes(superblock[24:28],'little')
 inode=disk.stat().st_ino
 with disk.open('r+b') as stream:stream.seek(1048576+block*block_size);stream.write(b'corrupt')
 m.install(disk,bundle)
 assert disk.stat().st_ino!=inode, 'same-inode guest edit incorrectly hit the install stamp'
 partition.write_bytes(disk.read_bytes()[1048576:1048576+32*1024*1024])
 installed=subprocess.check_output(['debugfs','-R','cat /lib/firmware/nvidia/ad106/gsp/test.bin',str(partition)],stderr=subprocess.DEVNULL)
 assert installed==fw.read_bytes()
 # Failure while finalizing the private snapshot must preserve the installed disk/cache.
 fw.write_bytes(b'updated pinned firmware'*1024)
 manifest['files'][0]['sha256']=digest(fw)
 (bundle/'manifest.json').write_text(json.dumps(manifest))
 old=digest(disk);old_cache=digest(cache);inode=disk.stat().st_ino
 real_copy=m.copy_image_range;calls=[]
 def fail_finalization(*args):
  calls.append(args)
  if len(calls)==2:raise OSError('injected private-copy failure')
  return real_copy(*args)
 with patch.object(m,'copy_image_range',side_effect=fail_finalization):
  try:m.install(disk,bundle)
  except OSError:pass
  else:raise AssertionError('accepted failed private snapshot finalization')
 assert disk.stat().st_ino==inode and digest(disk)==old and digest(cache)==old_cache
 print('PASS: atomic firmware install, GPT/desktop preservation, partition-cache refresh, repeated-install fsck, stamp invalidation after guest writes, optional notice and failure rollback')
