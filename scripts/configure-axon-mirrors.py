#!/usr/bin/env python3
"""Keep Axon's generated rootfs mirror list resilient to one stale mirror."""
from pathlib import Path
import sys


runtime = Path(sys.argv[1])
project_root = Path(sys.argv[2]).resolve()
source = runtime.read_text()
single = "    mirrorlist.write_text('Server = https://geo.mirror.pkgbuild.com/$repo/os/$arch\\n')"
multiple = """    mirrorlist.write_text(
        'Server = https://fastly.mirror.pkgbuild.com/$repo/os/$arch\\n'
        'Server = https://geo.mirror.pkgbuild.com/$repo/os/$arch\\n'
        'Server = https://mirror.rackspace.com/archlinux/$repo/os/$arch\\n'
        'Server = https://mirrors.kernel.org/archlinux/$repo/os/$arch\\n'
    )"""
previous = """    mirrorlist.write_text(
        'Server = https://geo.mirror.pkgbuild.com/$repo/os/$arch\\n'
        'Server = https://fastly.mirror.pkgbuild.com/$repo/os/$arch\\n'
        'Server = https://mirror.rackspace.com/archlinux/$repo/os/$arch\\n'
        'Server = https://mirrors.kernel.org/archlinux/$repo/os/$arch\\n'
    )"""

if multiple in source:
    raise SystemExit(0)
if single not in source and previous not in source:
    raise SystemExit(f"can't configure rootfs mirrors in unsupported Axon runtime: {runtime}")

try:
    runtime.resolve().relative_to(project_root)
except ValueError:
    raise SystemExit(
        f"Axon's runtime is outside the AvoryOS checkout and needs mirror fallback support: {runtime}; "
        "use an AvoryOS-managed Axon checkout via AXON_SOURCE_DIR/AXON_BIN"
    )

if previous in source:
    source = source.replace(previous, multiple, 1)
else:
    source = source.replace(single, multiple, 1)
runtime.write_text(source)
