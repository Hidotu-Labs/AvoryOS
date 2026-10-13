#!/usr/bin/env python3
"""Populate a disposable ext4 image and inspect actual polkit inode ownership."""
from pathlib import Path
import os, re, subprocess, tempfile

script=Path('scripts/populate-ext2-dir.sh').resolve()
def run(*args, **kw):
    return subprocess.run(args,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,**kw)
def inode(image,path):
    result=run('debugfs','-R',f'stat {path}',str(image),check=True)
    text=result.stdout
    ids=re.search(r'User:\s*(\d+)\s+Group:\s*(\d+)',text)
    mode=re.search(r'Mode:\s*(0[0-7]+)',text)
    assert ids and mode,text
    return int(ids[1]),int(ids[2]),int(mode[1],8)
with tempfile.TemporaryDirectory(prefix='avory-polkit-') as tmp:
    base=Path(tmp);root=base/'root';rules=root/'etc/polkit-1/rules.d'
    rules.mkdir(parents=True);rules.chmod(0o750)
    rule=rules/'10-test.rules';rule.write_text('/* root-owned administrator policy */\n');rule.chmod(0o644)
    group=root/'etc/group';group.write_text('root:x:0:\npolkitd:x:777:\n')
    image=base/'test.img'
    with image.open('wb') as f:f.truncate(32*1024*1024)
    run('mkfs.ext4','-q','-F',str(image),check=True)
    result=run('bash',str(script),str(image),str(root),'/',check=True)
    assert inode(image,'/etc/polkit-1/rules.d')==(0,777,0o750)
    assert inode(image,'/etc/polkit-1/rules.d/10-test.rules')==(0,0,0o644)
    # Re-population repairs an existing bad inode; target GID is not fixed at 102.
    run('debugfs','-w','-R','set_inode_field /etc/polkit-1/rules.d gid 0',str(image),check=True)
    group.write_text('root:x:0:\npolkitd:x:102:\n')
    run('bash',str(script),str(image),str(root),'/',check=True)
    assert inode(image,'/etc/polkit-1/rules.d')==(0,102,0o750)
    # Invalid/missing account data must stop before any disk writes.
    before=image.read_bytes()
    for contents in ('root:x:0:\n','polkitd:x:oops:\n'):
        group.write_text(contents)
        result=run('bash',str(script),str(image),str(root),'/')
        assert result.returncode and 'polkitd GID' in result.stderr
        assert image.read_bytes()==before
    group.unlink()
    result=run('bash',str(script),str(image),str(root),'/')
    assert result.returncode and 'Missing target /etc/group' in result.stderr
    assert image.read_bytes()==before
    print('PASS: actual image root:polkitd 0750, root-owned read-only rules, dynamic GID, repeated repair, and account validation before mutation')

# Verify the repaired inode against AvoryOS's actual group-aware permission code.
def extract(signature):
    source=Path('kernel/src/fs/vfs.c').read_text()
    start=source.index(signature);end=source.index('{',start)+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]+'\n'
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define FS_TYPE_MASK 7
#define FS_DIRECTORY 2
typedef struct {unsigned mask,uid,gid,flags;} vfs_node_t;
struct thread {unsigned fsuid,euid,fsgid,egid,gid,supplementary_group_count,supplementary_groups[8];};
static struct thread self;
static struct thread *sched_get_current(void){return &self;}
'''+extract('bool vfs_in_group(')+extract('bool vfs_access(')+r'''
int main(void){
 vfs_node_t rules={.mask=0750,.uid=0,.gid=0,.flags=FS_DIRECTORY};
 self.fsuid=self.euid=self.fsgid=self.egid=self.gid=102;
 assert(!vfs_access(&rules,4)); /* reproduce image's root:root denial */
 rules.gid=102;
 assert(vfs_access(&rules,4) && vfs_access(&rules,1) && !vfs_access(&rules,2));
 self.fsuid=self.euid=self.fsgid=self.egid=self.gid=1000;
 assert(!vfs_access(&rules,4) && !vfs_access(&rules,1) && !vfs_access(&rules,2));
 puts("PASS: actual VFS reproduces old polkit denial, permits daemon read/search after repair, denies writes and unrelated users");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-polkit-access-') as tmp:
    p=Path(tmp);(p/'test.c').write_text(code)
    run('cc','-O1','-g','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test'),check=True)
    result=run(str(p/'test'),check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
    print(result.stdout,end='')
