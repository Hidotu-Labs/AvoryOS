"""Copy image ranges using filesystem reflinks, with portable I/O fallbacks."""
import ctypes
import errno
import fcntl
import os
from pathlib import Path
import struct
import time

# Linux FICLONERANGE, struct file_clone_range (linux/fs.h).
FICLONERANGE = 0x4020940D
UNSUPPORTED = {errno.EXDEV, errno.EOPNOTSUPP, errno.ENOTTY, errno.EINVAL, errno.ENOSYS}


def _clear_range(fd, offset, length):
    """Discard old destination bytes so source holes cannot retain stale data."""
    libc = ctypes.CDLL(None, use_errno=True)
    fallocate = getattr(libc, 'fallocate', None)
    if fallocate is None:
        return False
    fallocate.argtypes = (ctypes.c_int, ctypes.c_int, ctypes.c_longlong, ctypes.c_longlong)
    fallocate.restype = ctypes.c_int
    if fallocate(fd, 3, offset, length) == 0:  # PUNCH_HOLE | KEEP_SIZE
        return True
    error = ctypes.get_errno()
    if error not in UNSUPPORTED:
        raise OSError(error, os.strerror(error))
    return False


def _extents(fd, start, length):
    end = start + length
    while start < end:
        try:
            data = os.lseek(fd, start, os.SEEK_DATA)
        except OSError as error:
            if error.errno == errno.ENXIO:
                return
            if error.errno in UNSUPPORTED:
                yield start, end
                return
            raise
        if data >= end:
            return
        try:
            hole = min(os.lseek(fd, data, os.SEEK_HOLE), end)
        except OSError as error:
            if error.errno not in UNSUPPORTED:
                raise
            hole = end
        yield data, hole
        start = hole


def copy_image_range(source, destination, source_offset, destination_offset, length):
    """Replace exactly one range, preserving surrounding GPT data and holes."""
    source, destination = Path(source), Path(destination)
    if min(source_offset, destination_offset, length) < 0:
        raise ValueError('negative image copy range')
    started = time.monotonic()
    with source.open('rb', buffering=0) as src, destination.open('r+b', buffering=0) as dst:
        src_info, dst_info = os.fstat(src.fileno()), os.fstat(dst.fileno())
        if (src_info.st_dev, src_info.st_ino) == (dst_info.st_dev, dst_info.st_ino):
            raise ValueError('source and destination must be different files')
        if source_offset + length > src_info.st_size:
            raise ValueError('image copy range exceeds source size')
        if destination_offset + length > dst_info.st_size:
            raise ValueError('image copy range exceeds destination size')
        if not length:
            return
        try:
            fcntl.ioctl(dst.fileno(), FICLONERANGE,
                        struct.pack('=qQQQ', src.fileno(), source_offset, length, destination_offset))
            method = 'reflink'
        except OSError as error:
            if error.errno not in UNSUPPORTED:
                raise
            cleared = _clear_range(dst.fileno(), destination_offset, length)
            ranges = _extents(src.fileno(), source_offset, length) if cleared else [(source_offset, source_offset + length)]
            kernel_copy = hasattr(os, 'copy_file_range')
            for start, end in ranges:
                while start < end:
                    target = destination_offset + start - source_offset
                    count = min(16 * 1024 * 1024, end - start)
                    if kernel_copy:
                        try:
                            copied = os.copy_file_range(src.fileno(), dst.fileno(), count, start, target)
                            if copied <= 0:
                                raise RuntimeError('short kernel image copy')
                            start += copied
                            continue
                        except OSError as error:
                            if error.errno not in UNSUPPORTED:
                                raise
                            kernel_copy = False
                    chunk = os.pread(src.fileno(), count, start)
                    if not chunk:
                        raise RuntimeError('short read during image copy')
                    written = 0
                    while written < len(chunk):
                        step = os.pwrite(dst.fileno(), chunk[written:], target + written)
                        if step <= 0:
                            raise RuntimeError('short write during image copy')
                        written += step
                    start += len(chunk)
            method = 'sparse copy' if cleared else 'copy'
        os.fsync(dst.fileno())
    print(f'Copied {length / 1024**3:.2f} GiB using {method} in {time.monotonic() - started:.2f}s', flush=True)
