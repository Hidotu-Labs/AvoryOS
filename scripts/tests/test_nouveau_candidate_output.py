#!/usr/bin/env python3
"""A copied makefile must replace OUTPUT, leaving normal boot artifacts intact."""
from pathlib import Path
import hashlib,json,os,shutil,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='nouveau-build-output-') as temp:
 base=Path(temp)
 for d in ['scripts/linux','kernel/linux','kernel/bin-x86_64','build/nouveau-c0/firmware/LICENSES','docs/nouveau','tools']:(base/d).mkdir(parents=True,exist_ok=True)
 for name in ['nouveau-c4-build.py','nouveau-firmware-stage.py']:shutil.copyfile(root/'scripts'/name,base/'scripts'/name)
 pin='tag=v6.12.113\ncommit=pinned\n';(base/'scripts/linux/baseline.txt').write_text(pin);(base/'kernel/linux/.pin').write_text(pin)
 (base/'kernel/GNUmakefile').write_text('override OUTPUT := kernel\n-include linux/Makefile.kbuild\nobj-$(ARCH)/test.o: file.c\n')
 (base/'kernel/linux/Makefile.kbuild').write_text('obj-$(ARCH)/test.o: LINUX_CFLAGS += -msse2\n')
 active=base/'kernel/bin-x86_64/kernel';active.write_bytes(b'normal-kernel')
 iso=base/'avoryos-x86_64.iso';iso.write_bytes(b'normal-iso')
 fw=base/'build/nouveau-c0/firmware';(fw/'x.bin').write_bytes(b'pinned');(fw/'LICENSES/LICENCE.nvidia').write_text('notice')
 entry=dict(request='x.bin',canonical_source='x.bin',size=6,sha256=hashlib.sha256(b'pinned').hexdigest())
 (base/'docs/nouveau/c0-lock.json').write_text(json.dumps({'selected':{'firmware':dict(files=[entry],license='LICENSES/LICENCE.nvidia',ref='pin',commit='pinned',gsp_interface='535.113.01')}}))
 make=base/'tools/make';make.write_text('''#!/usr/bin/env python3
from pathlib import Path
import re,sys
args=sys.argv;wd=Path(args[args.index('-C')+1]);mf=Path(args[args.index('-f')+1])
output=re.findall(r'^override OUTPUT := (.+)$',mf.read_text(),re.M)[-1]
(wd/'bin-x86_64'/output).write_bytes(b'candidate-kernel')
''');make.chmod(0o755)
 env=dict(os.environ,PATH=str(base/'tools')+os.pathsep+os.environ['PATH'])
 subprocess.run(['python3',str(base/'scripts/nouveau-c4-build.py'),'-j','1'],env=env,check=True,capture_output=True)
 assert active.read_bytes()==b'normal-kernel' and iso.read_bytes()==b'normal-iso'
 assert (base/'build/nouveau-c4/kernel').read_bytes()==b'candidate-kernel'
 mf=(base/'build/nouveau-c4-kernel.make').read_text()
 assert mf.count('override OUTPUT := kernel-c4\n')==1 and 'obj-nouveau-c4-$(ARCH)/test.o' in mf
 assert 'obj-$(ARCH)' not in mf
 assert 'obj-nouveau-c4-$(ARCH)/test.o' in (base/'build/nouveau-c4/Makefile.kbuild').read_text()
print('PASS: candidate builder replaces OUTPUT and preserves the normal kernel/ISO')
