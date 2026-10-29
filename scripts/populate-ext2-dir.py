#!/usr/bin/env python3
"""Populate an ext image with one metadata scan and one debugfs batch."""
import fnmatch
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time


def quote(value):
    # debugfs accepts quoted whitespace, but does not unescape embedded quotes.
    if any(char in value for char in '\n\r\0"'):
        raise ValueError(f'unsupported character in image path: {value!r}')
    return '"' + value + '"'


def buffered_image(debugfs, image):
    # libext2fs accepts I/O options after '?'. A larger buffer keeps inode
    # tables and directory blocks resident across adjacent file operations.
    # Probe read-only: older e2fsprogs releases may not support cache_blocks.
    blocks = int(os.environ.get('AVORY_IMAGE_IO_CACHE_BLOCKS', '128'))
    if blocks < 1 or blocks > 4096:
        raise ValueError('AVORY_IMAGE_IO_CACHE_BLOCKS must be between 1 and 4096')
    candidate = f'{image}?cache_blocks={blocks}'
    probe = subprocess.run([debugfs, '-R', 'stats', candidate],
                           capture_output=True, text=True)
    if probe.returncode == 0 and 'Filesystem magic number:' in probe.stdout:
        return candidate
    return str(image)


def populate(image, source, destination):
    started = time.monotonic()
    image = Path(image).absolute()
    source = Path(source).resolve(strict=True)
    if not source.is_dir():
        raise ValueError(f'Missing source directory: {source}')
    destination = destination.strip('/')
    if any(part in ('.', '..') for part in destination.split('/')):
        raise ValueError('image destination must not contain . or ..')
    debugfs = '/run/host/usr/bin/debugfs'
    if not os.access(debugfs, os.X_OK):
        debugfs = 'debugfs'
    debugfs_image = buffered_image(debugfs, image)
    excludes = os.environ.get('EXCLUDE_IMAGE_PATHS') or 'usr/include usr/share/doc usr/share/man'
    excludes = excludes.split()
    archive_pattern = os.environ.get('EXCLUDE_IMAGE_STATIC', '*.a')

    polkit_gid = None
    if not destination and (source / 'etc/polkit-1/rules.d').is_dir():
        group = source / 'etc/group'
        if not group.is_file():
            raise ValueError('Missing target /etc/group for polkit rules ownership')
        for line in group.read_text().splitlines():
            fields = line.split(':')
            if fields[0] == 'polkitd':
                if len(fields) > 2 and re.fullmatch(r'[0-9]+', fields[2]):
                    polkit_gid = int(fields[2])
                break
        if polkit_gid is None:
            raise ValueError('Missing or invalid target polkitd GID')

    # Keep metadata in process: the shell version spawned stat/printf/readlink
    # helpers several times per inode. Do not follow guest symlinks on the host.
    directories, files = [], []

    def scan(folder, prefix=''):
        with os.scandir(folder) as entries:
            for entry in entries:
                relative = prefix + entry.name
                if any(fnmatch.fnmatchcase(relative, pattern) for pattern in excludes):
                    continue
                info = entry.stat(follow_symlinks=False)
                if stat.S_ISDIR(info.st_mode):
                    directories.append((relative, info))
                    scan(entry.path, relative + '/')
                elif stat.S_ISREG(info.st_mode) or stat.S_ISLNK(info.st_mode):
                    if not archive_pattern or not fnmatch.fnmatchcase(relative, archive_pattern):
                        files.append((relative, info))

    scan(source)

    def target(relative):
        return quote('/' + '/'.join(part for part in (destination, relative) if part))

    with tempfile.TemporaryDirectory(prefix='avory-populate-') as temp:
        commands = Path(temp) / 'commands'
        with commands.open('w') as stream:
            def emit(command):
                stream.write(command + '\n')

            emit('cd /')
            parts = destination.split('/') if destination else []
            for index in range(1, len(parts) + 1):
                emit('mkdir ' + quote('/' + '/'.join(parts[:index])))
            for relative, info in directories:
                path = target(relative)
                emit(f'mkdir {path}')
                emit(f'set_inode_field {path} mode 0{stat.S_IFDIR | stat.S_IMODE(info.st_mode):o}')
                if relative == 'etc/polkit-1/rules.d' and polkit_gid is not None:
                    emit(f'set_inode_field {path} uid 0')
                    emit(f'set_inode_field {path} gid {polkit_gid}')
                    emit(f'set_inode_field {path} mode 040750')
            current_parent = None
            data_image = str(image) + '?cache_blocks=8' if debugfs_image != str(image) else str(image)
            current_image = debugfs_image
            def large(info):
                return stat.S_ISREG(info.st_mode) and info.st_size >= 1024 * 1024
            # Keep directory/short-file metadata buffered, then stream large
            # file data with a small buffer; avoid linear large-buffer scans.
            for relative, info in sorted(files, key=lambda item: large(item[1])):
                wanted_image = data_image if large(info) else debugfs_image
                if wanted_image != current_image:
                    emit('close')
                    emit('open -w ' + quote(wanted_image))
                    current_image = wanted_image
                    current_parent = None
                parent, _, leaf = relative.rpartition('/')
                if parent != current_parent:
                    emit('cd ' + target(parent))
                    current_parent = parent
                path = quote(leaf)
                host = source / relative
                if stat.S_ISLNK(info.st_mode):
                    emit(f'symlink {path} {quote(os.readlink(host))}')
                else:
                    if not os.access(host, os.R_OK):
                        # Some package payloads are intentionally execute-only
                        # (for example dbus-daemon-launch-helper). debugfs must
                        # read the host payload, so stage a temporary readable
                        # copy while keeping the installed mode unchanged.
                        staged = Path(temp) / 'payload' / relative
                        staged.parent.mkdir(parents=True, exist_ok=True)
                        original_mode = stat.S_IMODE(info.st_mode)
                        try:
                            os.chmod(host, original_mode | stat.S_IRUSR)
                            try:
                                shutil.copyfile(host, staged)
                            finally:
                                os.chmod(host, original_mode)
                        except OSError as error:
                            staged.unlink(missing_ok=True)
                            raise RuntimeError(
                                f'Cannot stage unreadable file {host}: {error}') from error
                        host = staged
                    emit(f'rm {path}')
                    emit(f'write {quote(str(host))} {path}')
                    # debugfs write copies the host file's complete mode,
                    # including setuid/setgid bits; avoid a second lookup.
                    emit(f'set_inode_field {path} mtime @{int(info.st_mtime)}')
            if current_image != debugfs_image:
                emit('close')
                emit('open -w ' + quote(debugfs_image))
            # Children update directory mtimes; restore parents last for caches.
            for relative, info in reversed(directories):
                emit(f'set_inode_field {target(relative)} mtime @{int(info.st_mtime)}')

        # Validate and generate the entire batch before mutating the image.
        # rm_rf is unavailable on some e2fsprogs releases; retain the old
        # best-effort subtree cleanup for targeted overlays.
        if destination:
            subprocess.run([debugfs, '-w', debugfs_image, '-R', 'rm_rf ' + target('')],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        log = Path(temp) / 'debugfs.log'
        with log.open('w') as output:
            result = subprocess.run([debugfs, '-w', '-f', str(commands), debugfs_image],
                                    stdout=output, stderr=subprocess.STDOUT)
        errors = []
        with log.open() as output:
            for line in output:
                if line.startswith('rm: File not found '):
                    continue
                if re.search(r'could not allocate|no space left|could not write|short write|'
                             r'I/O error|while opening|while looking up|while resolving|'
                             r'while writing|while setting|while trying to resolve|while closing|while flushing',
                             line, re.IGNORECASE):
                    errors.append(line.rstrip())
        if result.returncode or errors:
            raise RuntimeError('Image population failed: check input paths and DISK_SIZE_MIB\n' +
                               '\n'.join(errors[-20:]))
    print(f'Populated {len(files)} files/links and {len(directories)} directories '
          f'from {source} in {time.monotonic() - started:.2f}s')


def main():
    if len(sys.argv) != 4 or not sys.argv[2] or not sys.argv[3]:
        print(f'Usage: {sys.argv[0]} <img_path> <src_dir> <dst_dir>', file=sys.stderr)
        return 1
    try:
        populate(*sys.argv[1:])
    except (OSError, ValueError, RuntimeError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
