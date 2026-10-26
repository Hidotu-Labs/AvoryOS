#!/usr/bin/env python3
"""Weston selects the production display while respecting explicit options."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='avory-weston-') as temp:
    path=Path(temp)
    weston=path/'weston';weston.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\n');weston.chmod(0o755)
    selector=path/'selector';selector.write_text('#!/bin/sh\n[ "$1" = --display ] || exit 2\necho /dev/dri/card7\n');selector.chmod(0o755)
    session=path/'session'
    session.write_text((root/'config/weston/avory-weston-session.sh').read_text()
                       .replace('/usr/bin/weston',str(weston)).replace('/bin/drm-pick.sh',str(selector)))
    def run(*args):
        return subprocess.run(['sh',str(session),*args],capture_output=True,text=True)
    result=run('--renderer=pixman','--log=/tmp/weston log')
    assert result.returncode==0 and result.stdout.splitlines()==['--drm-device=card7','--renderer=pixman','--log=/tmp/weston log'],result
    for args in (('--drm-device=card4',),('--drm-device','card4'),('--backend=headless',),('-B','headless')):
        result=run(*args);assert result.returncode==0 and result.stdout.splitlines()==list(args),result
    selector.write_text('#!/bin/sh\nexit 1\n')
    result=run();assert result.returncode==1 and not result.stdout,result
print('PASS: Weston production card selection, argument boundaries, explicit device/backend and failed discovery')
