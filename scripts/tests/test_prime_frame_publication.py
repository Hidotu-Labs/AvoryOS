#!/usr/bin/env python3
"""Regression entry point for the replacement native scanout engine."""
from pathlib import Path
import subprocess, sys
subprocess.run([sys.executable, str(Path(__file__).with_name('test_virtio_scanout_engine.py')),
                '--scenario', 'frames'], check=True)
