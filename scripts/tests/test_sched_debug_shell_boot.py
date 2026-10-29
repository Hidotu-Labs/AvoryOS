#!/usr/bin/env python3
"""Check that interactive Bash regains its prompt after cat sched_debug."""
from pathlib import Path
import shutil
from boot_probe import ROOT, run_probe
from test_pty_history_boot import setup as setup_bash, TREE


def setup(root):
    setup_bash(root)
    shutil.copy2(TREE/'usr/bin/cat', root/'usr/bin/cat')

if __name__ == '__main__':
    source=Path(__file__).with_name('test_pty_history.c').read_text()
    source=source.replace('PTY_HISTORY_BOOT','SCHED_DEBUG_SHELL_BOOT')
    source=source.replace('echo HISTORY_EXECUTED\\r','cat /proc/sched_debug > /dev/ttyS0; echo HISTORY_EXECUTED\\r')
    source=source.replace('poll(&p, 1, 600)','poll(&p, 1, 2000)')
    run_probe('sched-debug-shell-boot',source,'SCHED_DEBUG_SHELL_BOOT: PASS failures=0',setup=setup,cpus=8,timeout=100)
