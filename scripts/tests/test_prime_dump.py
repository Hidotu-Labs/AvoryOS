#!/usr/bin/env python3
"""Diagnostic selection preserves AMD defaults and supports the Nouveau VM."""
from pathlib import Path
import runpy
import sys
from unittest.mock import MagicMock, patch

root = Path(__file__).resolve().parents[2]
for arguments, suffix in (([], 'p6-c7-prime-serial.sock'),
                          (['--nouveau'], 'nouveau-c4-serial.sock'),
                          (['--socket', '/tmp/custom-diag.sock'], '/tmp/custom-diag.sock')):
    connection = MagicMock()
    connection.__enter__.return_value = connection
    with patch.object(sys, 'argv', ['prime-dump.py', *arguments]), \
         patch('socket.socket', return_value=connection):
        runpy.run_path(str(root/'scripts/prime-dump.py'), run_name='__main__')
    assert str(connection.connect.call_args.args[0]).endswith(suffix)
    connection.sendall.assert_called_once_with(b'dump\n')
    connection.settimeout.assert_called_once_with(3)
print('PASS: AMD default, Nouveau and explicit socket send the bounded dump command')
