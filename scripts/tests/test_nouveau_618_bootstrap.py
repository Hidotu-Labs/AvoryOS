#!/usr/bin/env python3
"""The main kernel build owns the pinned Nouveau Linux import."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
makefile = (ROOT/'GNUmakefile').read_text()
importer = (ROOT/'scripts/linux-import.sh').read_text()
baseline = (ROOT/'scripts/linux/baseline.txt').read_text()

assert 'tag=v6.18.42' in baseline
assert 'commit=856a9b51680cbbed1ace1207b29e424a616cff48' in baseline
assert 'scripts/nouveau-618-build.py' not in makefile
assert 'KPI_NOUVEAU=1' in makefile
assert 'kernel/bin-x86_64/kernel' in makefile
assert 'NOUVEAU_SERIES=1' in importer
assert not (ROOT/'scripts/nouveau-618-build.py').exists()
