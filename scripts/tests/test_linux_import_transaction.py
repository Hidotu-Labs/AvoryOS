#!/usr/bin/env python3
"""A failed staged Kbuild must preserve the previous imported tree and pin."""
from pathlib import Path
import os, shutil, subprocess, tempfile
ROOT = Path(__file__).resolve().parents[2]
baseline = dict(line.split('=', 1) for line in
                (ROOT/'scripts/linux/baseline.txt').read_text().splitlines())
candidates = ([Path(os.environ['LINUX_TEST_SOURCE'])] if 'LINUX_TEST_SOURCE' in os.environ else
              [ROOT/'build/nouveau-c0/linux-candidate', ROOT/'build/linux-src'])
source = None
for candidate in candidates:
    if not (candidate/'.git').exists():
        continue
    probe = subprocess.run(['git','-C',str(candidate),'rev-parse',
                            baseline['tag']+'^{commit}','HEAD'], capture_output=True,text=True)
    if probe.returncode == 0 and probe.stdout.splitlines() == [baseline['commit']]*2:
        source = candidate
        break
assert source is not None, 'set LINUX_TEST_SOURCE to a local clone checked out at the selected baseline; tests never fetch'
with tempfile.TemporaryDirectory(prefix='linux-import-transaction-') as tmp:
    root = Path(tmp)
    (root/'scripts/linux').mkdir(parents=True)
    (root/'kernel/linux').mkdir(parents=True)
    (root/'kernel/linuxkpi/include/generated').mkdir(parents=True)
    shutil.copy2(ROOT/'scripts/linux-import.sh', root/'scripts/linux-import.sh')
    shutil.copy2(ROOT/'scripts/linux/baseline.txt', root/'scripts/linux/baseline.txt')
    patches=root/'scripts/linux/patches';series='.'.join(baseline['tag'].split('.')[:2])
    (patches/series).mkdir(parents=True);(patches/'v0.0').mkdir()
    (patches/series/'selected.patch').write_text('--- /dev/null\n+++ b/series-marker\n@@ -0,0 +1 @@\n+selected series\n')
    (patches/'v0.0'/'wrong.patch').write_text('invalid patch for another series\n')
    (root/'scripts/linux/subset.txt').write_text('include\narch/x86/include\nlib/math/div64.c\n?absent-optional.c\n')
    (root/'scripts/linux/files.txt').write_text('lib/math/div64.c\n?absent-optional.c\n')
    (root/'scripts/linux/kbuild-subset.py').write_text('raise SystemExit(43)\n')
    (root/'kernel/linuxkpi/include/generated/autoconf.h').write_text('#define CONFIG_X86_64 1\n')
    (root/'kernel/linux/marker').write_text('last working import\n')
    (root/'kernel/linux/.pin').write_text('tag=previous\ncommit=preserve-me\n')
    env = dict(os.environ, LINUX_SRC_DIR=str(source), LINUX_VERSION=baseline['tag'])
    result = subprocess.run(['bash',str(root/'scripts/linux-import.sh')],env=env,capture_output=True,text=True,timeout=60)
    assert result.returncode != 0 and 'kbuild-subset.py failed' in result.stdout+result.stderr, result.stdout+result.stderr
    assert 'selected.patch' in result.stdout+result.stderr and 'wrong.patch' not in result.stdout+result.stderr
    assert (root/'kernel/linux/marker').read_text() == 'last working import\n'
    assert (root/'kernel/linux/.pin').read_text() == 'tag=previous\ncommit=preserve-me\n'
    assert not [p for p in (root/'kernel').glob('.linux-import.*') if p.is_dir()], 'staging directory leaked'
    print('PASS: optional absent sources skipped; failed generation preserves active import and pin; staged tree cleaned')

    # Interrupt publication after moving the old import out of the way.
    (root/'scripts/linux/kbuild-subset.py').write_text(
        'import sys\nfrom pathlib import Path\n'
        'Path(sys.argv[sys.argv.index("--out")+1]).write_text("")\n')
    (root/'test-bin').mkdir()
    move = root/'test-bin/mv'
    move.write_text('#!/bin/sh\n/bin/mv "$@" || exit $?\n'
                    'if [ "$1" = "$C1_IMPORT_FINAL" ]; then kill -TERM "$PPID"; fi\n')
    move.chmod(0o755)
    env.update(PATH=str(root/'test-bin')+os.pathsep+env['PATH'],
               C1_IMPORT_FINAL=str(root/'kernel/linux'))
    result = subprocess.run(['bash',str(root/'scripts/linux-import.sh')],env=env,capture_output=True,text=True,timeout=60)
    assert result.returncode != 0, 'publication interruption was not injected'
    assert (root/'kernel/linux/marker').read_text() == 'last working import\n'
    assert (root/'kernel/linux/.pin').read_text() == 'tag=previous\ncommit=preserve-me\n'
    assert not [p for p in (root/'kernel').glob('.linux-*.*') if p.is_dir()]
    print('PASS: interrupted publication restores the previous import and pin')
