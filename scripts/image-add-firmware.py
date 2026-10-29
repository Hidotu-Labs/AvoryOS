#!/usr/bin/env python3
"""Add a verified firmware bundle to an existing GPT guest disk atomically."""
import argparse, fcntl, functools, hashlib, importlib.util, json, os, re, subprocess, tempfile
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
_copy_spec=importlib.util.spec_from_file_location('image_copy',Path(__file__).with_name('image_copy.py'))
_copy_module=importlib.util.module_from_spec(_copy_spec);_copy_spec.loader.exec_module(_copy_module)
copy_image_range=_copy_module.copy_image_range


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
                  mtime_ns=disk.stat().st_mtime_ns,ctime_ns=disk.stat().st_ctime_ns,
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
            print('Preparing private firmware partition...',flush=True)
            with part.open('wb') as stream:stream.truncate(size)
            copy_image_range(copy,part,offset,0,size)
            image=runtime.RuntimeImage(part,bundle,temp)
            commands=[];directories=set();pending=set()
            for name in files:
                parent=''
                for component in name.parts[:-1]:
                    ancestor=parent
                    parent+='/'+component
                    if parent in directories:continue
                    # Reissuing debugfs mkdir on an existing directory can
                    # allocate an unattached inode. Probe each shared parent
                    # once and batch only the missing directory creations.
                    existing='' if ancestor in pending else image.command(f'stat "{parent}"')
                    if 'Type:' not in existing:
                        commands.append(f'mkdir "{parent}"');pending.add(parent)
                    directories.add(parent)
                commands.extend([f'rm "/{name}"',f'write "{bundle/name}" "/{name}"'])
            batch=temp/'firmware.commands';batch.write_text('\n'.join(commands)+'\n')
            result=subprocess.run([image.debugfs,'-w','-f',str(batch),str(part)],
                                  capture_output=True,text=True)
            diagnostic='\n'.join(line for line in (result.stdout+result.stderr).splitlines()
                                  if not line.startswith('rm: File not found '))
            if result.returncode or re.search(
                    r'could not allocate|no space left|could not write|short write|I/O error|'
                    r'while opening|while looking up|while resolving|while writing|'
                    r'while closing|while flushing|while trying to resolve',diagnostic,re.I):
                raise RuntimeError('firmware image update failed:\n'+diagnostic[-4000:])
            # The partition is now stable; cache common guest-link lookups
            # during readback without weakening verification of any payload.
            image.image_readlink=functools.lru_cache(maxsize=None)(image.image_readlink)
            for name in files:image.verify('/'+str(name))
            print('Finalizing verified firmware partition...',flush=True)
            copy_image_range(part,copy,0,offset,size)
            current=disk.stat()
            if (current.st_ino,current.st_mtime_ns,current.st_ctime_ns,current.st_size)!=(original.st_ino,original.st_mtime_ns,original.st_ctime_ns,original.st_size):
                raise RuntimeError('guest disk changed during firmware installation; leaving it untouched')
            os.chmod(copy,original.st_mode & 0o777)
            copy.replace(disk)
            partition_cache=ROOT/'build/arch/disk-partition.img'
            if partition_cache.is_file():
                cache_tmp=partition_cache.with_name(partition_cache.name+'.tmp')
                subprocess.run(['cp','--reflink=auto','--sparse=always',str(part),str(cache_tmp)],check=True)
                cache_tmp.replace(partition_cache)
            installed=disk.stat()
            identity.update(inode=installed.st_ino,mtime_ns=installed.st_mtime_ns,ctime_ns=installed.st_ctime_ns)
            stamp.write_text(json.dumps(identity,indent=2)+'\n')
            print('Pinned Nouveau firmware installed; existing desktop filesystem preserved.')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('disk',type=Path);p.add_argument('bundle',type=Path)
    a=p.parse_args()
    try:install(a.disk,a.bundle)
    except (OSError,RuntimeError,ValueError,subprocess.CalledProcessError) as error:raise SystemExit(str(error))
