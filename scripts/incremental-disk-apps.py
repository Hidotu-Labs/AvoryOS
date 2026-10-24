#!/usr/bin/env python3
"""Refresh changed userland executables inside the existing AvoryOS disk."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def partition_info(disk: Path):
    table = json.loads(subprocess.check_output(
        ["sfdisk", "--json", str(disk)], text=True))["partitiontable"]
    partitions = table["partitions"]
    if table["label"] != "gpt" or len(partitions) != 1:
        raise RuntimeError("expected AvoryOS's single-partition GPT disk")
    row = partitions[0]
    sector = int(table["sectorsize"])
    offset = int(row["start"]) * sector
    size = int(row["size"]) * sector
    if offset + size > disk.stat().st_size:
        raise RuntimeError("root partition extends past disk.img")
    return offset, size


def ensure_disk_closed(disk: Path):
    disk = disk.resolve()
    for fd in Path("/proc").glob("[0-9]*/fd/*"):
        try:
            if fd.resolve() == disk:
                raise RuntimeError("disk.img is open by another process; shut down the guest first")
        except (FileNotFoundError, PermissionError, OSError):
            continue


def quote(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def file_state(path: Path):
    info = path.stat()
    return {
        "mtime_ns": info.st_mtime_ns,
        "size": info.st_size,
        "mode": info.st_mode & 0o7777,
        "sha256": digest(path),
    }


def mappings(makefile: Path, apps):
    app_set = set(apps)
    found = {}
    # The disk recipe is the source of truth for executable destinations.
    # Parsing its debugfs `write source destination` commands keeps this list
    # in sync with the targets already maintained by USERLAND_DISK_APPS.
    for source, destination in re.findall(
            r'write\s+([^\s"\']+)\s+([^\s"\']+)',
            makefile.read_text(errors="replace")):
        if source in app_set:
            found.setdefault(source, set()).add("/" + destination.lstrip("/"))
    missing = sorted(app_set - found.keys())
    if missing:
        raise RuntimeError("disk recipe has no install mapping for: " + ", ".join(missing))
    return {source: sorted(destinations) for source, destinations in sorted(found.items())}


def ext4_uuid(path: Path, offset=0):
    with path.open("rb") as stream:
        stream.seek(offset + 1024 + 104)
        value = stream.read(16)
    if len(value) != 16:
        raise RuntimeError(f"cannot read ext4 UUID from {path}")
    return value


def cache_partition(disk: Path, cache: Path):
    offset, size = partition_info(disk)
    cache.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_name = tempfile.mkstemp(prefix=cache.name + ".", dir=cache.parent)
    os.close(fd)
    temp = Path(temp_name)
    try:
        subprocess.run([
            "dd", f"if={disk}", f"of={temp}", "bs=1M",
            f"skip={offset}", f"count={size}",
            "iflag=skip_bytes,count_bytes", "conv=sparse", "status=none",
        ], check=True)
        if temp.stat().st_size != size:
            raise RuntimeError("partition cache has the wrong size")
        if ext4_uuid(temp) != ext4_uuid(disk, offset):
            raise RuntimeError("partition cache UUID does not match disk.img")
        temp.replace(cache)
    finally:
        temp.unlink(missing_ok=True)
    print(f"Cached ext4 partition at {cache}")


def sync_partition(disk: Path, cache: Path):
    offset, size = partition_info(disk)
    if cache.stat().st_size != size:
        raise RuntimeError("cached partition size differs from disk.img; run make disk-rebuild")
    if ext4_uuid(cache) != ext4_uuid(disk, offset):
        raise RuntimeError("cached partition UUID differs from disk.img; run make disk-rebuild")

    changed = 0
    with cache.open("rb") as source, disk.open("r+b", buffering=0) as target:
        position = 0
        while position < size:
            chunk = source.read(min(4 * 1024 * 1024, size - position))
            if not chunk:
                raise RuntimeError("short read from cached partition")
            target.seek(offset + position)
            old = target.read(len(chunk))
            if old != chunk:
                target.seek(offset + position)
                target.write(chunk)
                changed += 1
            position += len(chunk)
        target.flush()
        os.fsync(target.fileno())
    if changed:
        print(f"Synced {changed} changed partition chunks into disk.img")
    return changed


def partition_matches_disk(disk: Path, cache: Path):
    offset, size = partition_info(disk)
    if cache.stat().st_size != size or ext4_uuid(cache) != ext4_uuid(disk, offset):
        return False
    with cache.open("rb") as source, disk.open("rb") as target:
        position = 0
        while position < size:
            chunk = source.read(min(4 * 1024 * 1024, size - position))
            target.seek(offset + position)
            if target.read(len(chunk)) != chunk:
                return False
            position += len(chunk)
    return True


def run_debugfs(cache: Path, commands):
    if not commands:
        return
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", delete=False) as stream:
        command_path = Path(stream.name)
        stream.write("\n".join(commands) + "\n")
    try:
        result = subprocess.run(["debugfs", "-w", "-f", str(command_path), str(cache)],
                                capture_output=True, text=True)
        diagnostic = result.stdout + result.stderr
        # Replacing an executable is also valid when it was not installed
        # before. Parent directories have already been resolved explicitly.
        errors = "\n".join(line for line in diagnostic.splitlines()
                           if not line.startswith("rm: File not found "))
        bad = re.search(r"could not allocate|no space left|could not write|short write|I/O error|"
                        r"while opening|while looking up|while resolving|while writing|"
                        r"while setting|while trying to resolve",
                        errors, re.IGNORECASE)
        if result.returncode or bad:
            raise RuntimeError("debugfs failed to update disk partition:\n" + diagnostic[-4000:])
    finally:
        command_path.unlink(missing_ok=True)


def install_destination(cache: Path, destination: str, directories: dict):
    """Resolve parent symlinks using the guest filesystem, not the host root.

    debugfs write/rm do not traverse usr/sbin -> bin automatically. Preserve
    the final component: rm/write replace that entry, including a symlink.
    """
    components = destination.split("/")
    leaf = components.pop()
    if not leaf or leaf in (".", ".."):
        raise RuntimeError(f"invalid executable destination: {destination}")
    resolved = []
    links = 0
    while components:
        component = components.pop(0)
        if component in ("", "."):
            continue
        if component == "..":
            if resolved:
                resolved.pop()
            continue
        path = "/" + "/".join(resolved + [component])
        if path not in directories:
            result = subprocess.run(
                ["debugfs", "-R", f"stat {quote(path)}", str(cache)],
                capture_output=True, text=True)
            output = result.stdout
            if result.returncode or not re.search(r"\bType: (directory|symlink)\b", output):
                raise RuntimeError(f"cannot resolve install directory {path}: "
                                   f"{output}{result.stderr}")
            target = None
            if "Type: symlink" in output:
                match = re.search(r'Fast link dest: "(.*)"', output)
                if match:
                    target = match.group(1)
                else:
                    # Long symlinks store the target in data blocks.
                    inode = re.search(r"Inode:\s*(\d+)", output)
                    if not inode:
                        raise RuntimeError(f"cannot read symlink inode for {path}")
                    with tempfile.TemporaryDirectory(prefix="avory-disk-link-") as temp:
                        dumped = Path(temp) / "target"
                        result = subprocess.run(
                            ["debugfs", "-R", f"dump <{inode.group(1)}> {quote(str(dumped))}",
                             str(cache)], capture_output=True, text=True)
                        if result.returncode or not dumped.is_file():
                            raise RuntimeError(f"cannot read symlink target for {path}")
                        target = os.fsdecode(dumped.read_bytes())
                if not target or "\x00" in target or "\n" in target:
                    raise RuntimeError(f"invalid directory symlink target for {path}")
            directories[path] = target
        target = directories[path]
        if target is None:
            resolved.append(component)
            continue
        links += 1
        if links > 40:
            raise RuntimeError(f"too many directory symlinks in {destination}")
        if target.startswith("/"):
            resolved = []
        components = target.split("/") + components
    return "/" + "/".join(resolved + [leaf])


def snapshot(manifest: Path, makefile: Path, apps):
    app_map = mappings(makefile, apps)
    states = {source: file_state(ROOT / source) for source in app_map
              if (ROOT / source).is_file()}
    manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_text(json.dumps({"mappings": app_map, "sources": states}, indent=2) + "\n")
    print(f"Saved incremental disk manifest for {len(app_map)} userland programs")


def verify_copy(cache: Path, destination: str, expected_hash: str):
    with tempfile.TemporaryDirectory(prefix="avory-disk-check-") as temp:
        dumped = Path(temp) / "file"
        result = subprocess.run(
            ["debugfs", "-R", f"dump {quote(destination)} {quote(str(dumped))}", str(cache)],
            capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(f"cannot verify {destination}: {result.stdout}{result.stderr}")
        if not dumped.is_file() or digest(dumped) != expected_hash:
            raise RuntimeError(f"disk copy checksum mismatch for {destination}")


def update(disk: Path, cache: Path, manifest: Path, makefile: Path, apps, sync_only=False):
    ensure_disk_closed(disk)
    lock_path = manifest.with_suffix(".lock")
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open("a") as lock:
        try:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError("another incremental disk update is already running") from error

        if not cache.is_file():
            raise RuntimeError(f"missing {cache}; run make disk-rebuild once to create the incremental cache")
        if not sync_only:
            if not manifest.is_file():
                raise RuntimeError(f"missing {manifest}; run make disk-rebuild once to seed it")
            if not partition_matches_disk(disk, cache):
                print("Guest-side disk changes detected; refreshing the partition cache before patching.")
                cache_partition(disk, cache)
            previous = json.loads(manifest.read_text())
            app_map = mappings(makefile, apps)
            old_map = previous.get("mappings", {})
            old_states = previous.get("sources", {})
            commands = []
            new_states = {}
            updates = []
            directories = {}
            installed = {}
            for source, destinations in app_map.items():
                path = ROOT / source
                if not path.is_file():
                    continue
                state = file_state(path)
                new_states[source] = state
                if old_map.get(source) == destinations and old_states.get(source) == state:
                    continue
                resolved = sorted({install_destination(cache, destination, directories)
                                   for destination in destinations})
                updates.append((source, path, resolved, state))
                for destination in resolved:
                    if destination in installed:
                        if installed[destination] != state["sha256"]:
                            raise RuntimeError(f"conflicting executables map to {destination}")
                        continue
                    installed[destination] = state["sha256"]
                    commands.extend([
                        f"rm {quote(destination)}",
                        f"write {quote(str(path))} {quote(destination)}",
                        f"set_inode_field {quote(destination)} mode 0100755",
                    ])

            run_debugfs(cache, commands)
            for source, path, destinations, state in updates:
                for destination in destinations:
                    verify_copy(cache, destination, state["sha256"])
            if updates:
                print(f"Updated {len(updates)} changed userland program(s): " +
                      ", ".join(source for source, *_ in updates))
            previous = {"mappings": app_map, "sources": new_states}
        changed_chunks = sync_partition(disk, cache)
        if not sync_only:
            manifest.write_text(json.dumps(previous, indent=2) + "\n")
        os.utime(disk, None)
        if not changed_chunks:
            print("Disk filesystem already matched its incremental cache.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--cache-from-disk", action="store_true")
    mode.add_argument("--snapshot", action="store_true")
    mode.add_argument("--update", action="store_true")
    mode.add_argument("--sync-only", action="store_true")
    parser.add_argument("--disk", type=Path, default=ROOT / "disk.img")
    parser.add_argument("--partition-cache", type=Path,
                        default=ROOT / "build/arch/disk-partition.img")
    parser.add_argument("--manifest", type=Path,
                        default=ROOT / "build/arch/disk-apps.json")
    parser.add_argument("--makefile", type=Path, default=ROOT / "GNUmakefile")
    parser.add_argument("--apps", nargs="*", default=[])
    args = parser.parse_args()
    disk = args.disk if args.disk.is_absolute() else ROOT / args.disk
    cache = args.partition_cache if args.partition_cache.is_absolute() else ROOT / args.partition_cache
    manifest = args.manifest if args.manifest.is_absolute() else ROOT / args.manifest
    makefile = args.makefile if args.makefile.is_absolute() else ROOT / args.makefile
    try:
        if args.cache_from_disk:
            cache_partition(disk, cache)
        elif args.snapshot:
            snapshot(manifest, makefile, args.apps)
        else:
            update(disk, cache, manifest, makefile, args.apps, sync_only=args.sync_only)
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"ERROR: {error}\n")


if __name__ == "__main__":
    main()
