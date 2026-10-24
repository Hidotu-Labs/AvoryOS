#!/usr/bin/env python3
"""Keep one random kernel build tag per distinct kernel source state."""

from __future__ import annotations

import hashlib
import secrets
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
STATE = ROOT / "build" / "kernel-build-tag"
INPUT_DIRS = (ROOT / "kernel" / "src", ROOT / "kernel" / "linuxkpi", ROOT / "kernel" / "linux")
INPUT_FILES = (ROOT / "kernel" / "GNUmakefile", *sorted((ROOT / "kernel" / "linker-scripts").glob("*")))


def source_fingerprint() -> str:
    digest = hashlib.sha256()
    files = set(INPUT_FILES)
    for directory in INPUT_DIRS:
        if directory.exists():
            files.update(path for path in directory.rglob("*") if path.is_file())
    for path in sorted(files):
        if not path.is_file():
            continue
        digest.update(path.relative_to(ROOT).as_posix().encode())
        digest.update(b"\0")
        with path.open("rb") as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(chunk)
        digest.update(b"\0")
    return digest.hexdigest()


def main() -> None:
    fingerprint = source_fingerprint()
    previous = STATE.read_text().split() if STATE.exists() else []
    if len(previous) == 2 and previous[0] == fingerprint:
        tag = previous[1]
    else:
        tag = "".join(secrets.choice("ABCDEFGHJKLMNPQRSTUVWXYZ23456789") for _ in range(5))
        STATE.parent.mkdir(parents=True, exist_ok=True)
        temporary = STATE.with_suffix(".tmp")
        temporary.write_text(f"{fingerprint} {tag}\n")
        temporary.replace(STATE)
    print(tag)


if __name__ == "__main__":
    main()
