#!/usr/bin/env python3
"""Add a verified firmware bundle to an existing GPT guest disk atomically."""
import argparse, fcntl, hashlib, importlib.util, json, os, subprocess, tempfile
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]

def install(disk, bundle):
    disk=disk.resolve();bundle=bundle.resolve()
    manifest=json.loads((bundle/'manifest.json').read_text())
    files={Path('lib/firmware')/row['request']:row['sha256'] for row in manifest['files']}
    # Some locally extracted Nouveau firmware bundles contain only the exact
    # firmware closure. Preserve/install the notice when the bundle provides
    # one, while allowing those hardware-test bundles to be installed too.
    license=Path('lib/firmware/LICENSES/LICENCE.nvidia')
    if 'license_sha256' in manifest:
        files[license]=manifest['license_sha256']
    for name,digest in files.items():
        if '..' in name.parts or name.is_absolute() or hashlib.sha256((bundle/name).read_bytes()).hexdigest()!=digest:
            raise RuntimeError(f'invalid firmware bundle entry: {name}')
    # Exclude live guests before taking a snapshot or replacing the disk inode.
    for fd in Path('/proc').glob('[0-9]*/fd/*'):
        try:
            if fd.resolve()==disk:raise RuntimeError('disk.img is open by another process; shut down the guest first')
        except (FileNotFoundError,PermissionError):pass
    table=json.loads(subprocess.check_output(['sfdisk','--json',str(disk)],text=True))['partitiontable']
    partitions=table['partitions']
    if table['label']!='gpt' or len(partitions)!=1 or partitions[0]['type'].lower()!='4f68bce3-e8cd-4db1-96e7-fbcaf984b709':
        raise RuntimeError('expected the standard single-root GPT AvoryOS disk')
    sector=table['sectorsize'];offset=partitions[0]['start']*sector;size=partitions[0]['size']*sector
    if offset+size>disk.stat().st_size:raise RuntimeError('partition exceeds guest disk')
    with disk.open('rb') as stream:
        stream.seek(offset+1024);superblock=stream.read(120)
    if superblock[56:58]!=b'\x53\xef':raise RuntimeError('root partition is not ext4')
    identity=dict(disk=str(disk),inode=disk.stat().st_ino,size=disk.stat().st_size,
                  filesystem_uuid=superblock[104:120].hex(),files={str(k):v for k,v in files.items()})
    stamp=ROOT/'build/nouveau/desktop-firmware.json';stamp.parent.mkdir(parents=True,exist_ok=True)
    with (stamp.parent/'desktop-firmware.lock').open('a') as guard:
        fcntl.flock(guard,fcntl.LOCK_EX|fcntl.LOCK_NB)
        if stamp.is_file() and json.loads(stamp.read_text())==identity:
            print('Nouveau firmware already installed in the desktop disk.');return
        spec=importlib.util.spec_from_file_location('image_runtime',ROOT/'scripts/image-runtime.py')
        runtime=importlib.util.module_from_spec(spec);spec.loader.exec_module(runtime)
        # Work on a private snapshot; never rebuild or overwrite the installed
        # filesystem in place. Reflinks keep this cheap on the host's Btrfs.
        with tempfile.TemporaryDirectory(prefix='.nouveau-firmware-',dir=disk.parent) as temp:
            temp=Path(temp);copy=temp/'disk.img';part=temp/'root.img'
            original=disk.stat()
            subprocess.run(['cp','--reflink=auto','--sparse=always',str(disk),str(copy)],check=True)
            subprocess.run(['dd',f'if={copy}',f'of={part}','bs=1M',f'skip={offset}',f'count={size}',
                            'iflag=skip_bytes,count_bytes','conv=sparse','status=none'],check=True)
            image=runtime.RuntimeImage(part,bundle,temp)
            for name in files:
                parent=''
                for component in name.parts[:-1]:
                    parent+='/'+component
                    image.command(f'mkdir "{parent}"',write=True)
                image.command(f'rm "/{name}"',write=True)
                image.command(f'write "{bundle/name}" "/{name}"',write=True)
                image.verify('/'+str(name))
            # Write only changed blocks back to the private disk snapshot.
            with part.open('rb') as src,copy.open('r+b') as dst:
                dst.seek(offset)
                while block:=src.read(1024*1024):
                    position=dst.tell();old=dst.read(len(block))
                    if old!=block:dst.seek(position);dst.write(block)
                dst.flush();os.fsync(dst.fileno())
            current=disk.stat()
            if (current.st_ino,current.st_mtime_ns,current.st_size)!=(original.st_ino,original.st_mtime_ns,original.st_size):
                raise RuntimeError('guest disk changed during firmware installation; leaving it untouched')
            os.chmod(copy,original.st_mode & 0o777)
            copy.replace(disk)
            partition_cache=ROOT/'build/arch/disk-partition.img'
            if partition_cache.is_file():
                cache_tmp=partition_cache.with_name(partition_cache.name+'.tmp')
                subprocess.run(['cp','--reflink=auto','--sparse=always',str(part),str(cache_tmp)],check=True)
                cache_tmp.replace(partition_cache)
            identity['inode']=disk.stat().st_ino
            stamp.write_text(json.dumps(identity,indent=2)+'\n')
            print('Pinned Nouveau firmware installed; existing desktop filesystem preserved.')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('disk',type=Path);p.add_argument('bundle',type=Path)
    a=p.parse_args()
    try:install(a.disk,a.bundle)
    except (OSError,RuntimeError,ValueError,subprocess.CalledProcessError) as error:raise SystemExit(str(error))
