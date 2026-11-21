#!/usr/bin/env python3
"""Compare a file read through the kernel three ways against a known hash.

Wine loads PE images whose FileAlignment is 0x200 with pread() at offsets that
are 512-byte aligned but not page aligned. A kernel read/readahead bug that
only shows up for that pattern would hand Wine wrong code bytes, which Arxan's
self-checksums detect. Usage: check_exe_reads.py <file> <expected_sha256>
"""
import hashlib, mmap, os, random, sys

path, expected = sys.argv[1], sys.argv[2].lower()
size = os.path.getsize(path)

def report(name, data):
    h = hashlib.sha256(data).hexdigest()
    print(f"{name:28s} {'OK ' if h == expected else 'BAD'} {h}")
    return data

with open(path, 'rb') as f:
    ref = report("sequential read", f.read())

fd = os.open(path, os.O_RDONLY)
try:
    # Odd-sized preads at 0x200-aligned, non-page-aligned offsets, in random
    # order, the way a PE loader fills sections.
    buf = bytearray(size)
    chunks = []
    off = 0
    rng = random.Random(1)
    while off < size:
        n = min(size - off, 0x200 * rng.randint(1, 300))
        chunks.append((off, n))
        off += n
    rng.shuffle(chunks)
    for off, n in chunks:
        part = os.pread(fd, n, off)
        buf[off:off + len(part)] = part
    got = report("pread 0x200-aligned, shuffled", bytes(buf))
finally:
    os.close(fd)

with open(path, 'rb') as f:
    m = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
    report("mmap", m[:])
    m.close()

bad = [i for i in range(0, size, 0x200) if got[i:i + 0x200] != ref[i:i + 0x200]]
if bad:
    print(f"pread differs from sequential read in {len(bad)} 512-byte blocks; first offsets:",
          ", ".join(hex(b) for b in bad[:16]))
