#!/usr/bin/env python3
"""io_uring UAPI in sys_io_uring.c matches Linux include/uapi/linux/io_uring.h.

Extracts the kernel's UAPI structs/constants and checks sizes, field
offsets, and opcode/setup/register flag values against the host Linux
headers, so a layout drift that would corrupt the shared SQ/CQ rings
fails here instead of in the guest.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SRC = (ROOT / 'kernel/src/syscalls/sys_io_uring.c').read_text()


def section(start_marker, end_marker):
    start = SRC.index(start_marker)
    end = SRC.index(end_marker, start)
    return SRC[start:end]


uapi = section('// UAPI (matches Linux', '// Context')

# Rename kernel identifiers so both can coexist with <linux/io_uring.h>.
renamed = uapi
renamed = re.sub(r'\bio_uring_', 'kio_uring_', renamed)
renamed = re.sub(r'\bio_sqring_', 'kio_sqring_', renamed)
renamed = re.sub(r'\bio_cqring_', 'kio_cqring_', renamed)
renamed = re.sub(r'\bIORING_OP_', 'KIORING_OP_', renamed)
renamed = re.sub(r'\bIORING_SETUP_', 'KIORING_SETUP_', renamed)
renamed = re.sub(r'\bIORING_REGISTER_', 'KIORING_REGISTER_', renamed)
renamed = re.sub(r'\bIORING_UNREGISTER_', 'KIORING_UNREGISTER_', renamed)
renamed = re.sub(r'\bIORING_ENTER_', 'KIORING_ENTER_', renamed)
renamed = re.sub(r'\bIORING_OFF_', 'KIORING_OFF_', renamed)
renamed = re.sub(r'\bIORING_FEAT_', 'KIORING_FEAT_', renamed)
renamed = re.sub(r'\bIOSQE_', 'KIOSQE_', renamed)
renamed = re.sub(r'\bIO_URING_OP_SUPPORTED', 'KIO_URING_OP_SUPPORTED', renamed)

code = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <linux/io_uring.h>
'''
code += renamed
code += r'''
#define CHECK_SIZE(k, l) _Static_assert(sizeof(struct k) == sizeof(struct l), #k " size")
#define CHECK_OFF(k, f, l, g) _Static_assert(offsetof(struct k, f) == offsetof(struct l, g), #k "." #f)
#define CHECK_VAL(k, l) _Static_assert((long)(k) == (long)(l), #k)

CHECK_SIZE(kio_uring_sqe, io_uring_sqe);
CHECK_OFF(kio_uring_sqe, opcode, io_uring_sqe, opcode);
CHECK_OFF(kio_uring_sqe, flags, io_uring_sqe, flags);
CHECK_OFF(kio_uring_sqe, fd, io_uring_sqe, fd);
CHECK_OFF(kio_uring_sqe, off, io_uring_sqe, off);
CHECK_OFF(kio_uring_sqe, addr, io_uring_sqe, addr);
CHECK_OFF(kio_uring_sqe, len, io_uring_sqe, len);
CHECK_OFF(kio_uring_sqe, op_flags, io_uring_sqe, rw_flags);
CHECK_OFF(kio_uring_sqe, user_data, io_uring_sqe, user_data);
CHECK_OFF(kio_uring_sqe, buf_index, io_uring_sqe, buf_index);
CHECK_OFF(kio_uring_sqe, personality, io_uring_sqe, personality);
CHECK_OFF(kio_uring_sqe, splice_fd_in, io_uring_sqe, splice_fd_in);
CHECK_SIZE(kio_uring_cqe, io_uring_cqe);
CHECK_OFF(kio_uring_cqe, user_data, io_uring_cqe, user_data);
CHECK_OFF(kio_uring_cqe, res, io_uring_cqe, res);
CHECK_SIZE(kio_uring_params, io_uring_params);
CHECK_OFF(kio_uring_params, sq_entries, io_uring_params, sq_entries);
CHECK_OFF(kio_uring_params, features, io_uring_params, features);
CHECK_OFF(kio_uring_params, sq_off, io_uring_params, sq_off);
CHECK_OFF(kio_uring_params, cq_off, io_uring_params, cq_off);
CHECK_SIZE(kio_sqring_offsets, io_sqring_offsets);
CHECK_OFF(kio_sqring_offsets, head, io_sqring_offsets, head);
CHECK_OFF(kio_sqring_offsets, array, io_sqring_offsets, array);
CHECK_OFF(kio_sqring_offsets, user_addr, io_sqring_offsets, user_addr);
CHECK_SIZE(kio_cqring_offsets, io_cqring_offsets);
CHECK_OFF(kio_cqring_offsets, cqes, io_cqring_offsets, cqes);
CHECK_OFF(kio_cqring_offsets, user_addr, io_cqring_offsets, user_addr);
CHECK_SIZE(kio_uring_probe, io_uring_probe);
CHECK_OFF(kio_uring_probe, last_op, io_uring_probe, last_op);
CHECK_OFF(kio_uring_probe, ops_len, io_uring_probe, ops_len);

CHECK_VAL(KIORING_OP_NOP, IORING_OP_NOP);
CHECK_VAL(KIORING_OP_READV, IORING_OP_READV);
CHECK_VAL(KIORING_OP_WRITEV, IORING_OP_WRITEV);
CHECK_VAL(KIORING_OP_FSYNC, IORING_OP_FSYNC);
CHECK_VAL(KIORING_OP_READ_FIXED, IORING_OP_READ_FIXED);
CHECK_VAL(KIORING_OP_WRITE_FIXED, IORING_OP_WRITE_FIXED);
CHECK_VAL(KIORING_OP_POLL_ADD, IORING_OP_POLL_ADD);
CHECK_VAL(KIORING_OP_POLL_REMOVE, IORING_OP_POLL_REMOVE);
CHECK_VAL(KIORING_OP_SYNC_FILE_RANGE, IORING_OP_SYNC_FILE_RANGE);
CHECK_VAL(KIORING_OP_OPENAT, IORING_OP_OPENAT);
CHECK_VAL(KIORING_OP_CLOSE, IORING_OP_CLOSE);
CHECK_VAL(KIORING_OP_READ, IORING_OP_READ);
CHECK_VAL(KIORING_OP_WRITE, IORING_OP_WRITE);
CHECK_VAL(KIORING_OP_LAST, IORING_OP_LAST);
CHECK_VAL(KIORING_SETUP_CQSIZE, IORING_SETUP_CQSIZE);
CHECK_VAL(KIORING_SETUP_CLAMP, IORING_SETUP_CLAMP);
CHECK_VAL(KIORING_SETUP_IOPOLL, IORING_SETUP_IOPOLL);
CHECK_VAL(KIORING_SETUP_SQPOLL, IORING_SETUP_SQPOLL);
CHECK_VAL(KIORING_ENTER_GETEVENTS, IORING_ENTER_GETEVENTS);
CHECK_VAL(KIORING_ENTER_SQ_WAKEUP, IORING_ENTER_SQ_WAKEUP);
CHECK_VAL(KIORING_OFF_SQ_RING, IORING_OFF_SQ_RING);
CHECK_VAL(KIORING_OFF_CQ_RING, IORING_OFF_CQ_RING);
CHECK_VAL(KIORING_OFF_SQES, IORING_OFF_SQES);
CHECK_VAL(KIORING_REGISTER_BUFFERS, IORING_REGISTER_BUFFERS);
CHECK_VAL(KIORING_REGISTER_FILES, IORING_REGISTER_FILES);
CHECK_VAL(KIORING_REGISTER_EVENTFD, IORING_REGISTER_EVENTFD);
CHECK_VAL(KIORING_REGISTER_PROBE, IORING_REGISTER_PROBE);
CHECK_VAL(KIORING_REGISTER_ENABLE_RINGS, IORING_REGISTER_ENABLE_RINGS);
CHECK_VAL(KIOSQE_FIXED_FILE, IOSQE_FIXED_FILE);
CHECK_VAL(KIO_URING_OP_SUPPORTED, IO_URING_OP_SUPPORTED);

static unsigned kpow2(unsigned v) {
  if (v < 8) return 8;
  v--; v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
  return v + 1;
}

int main(void) {
  assert(kpow2(0) == 8 && kpow2(1) == 8 && kpow2(8) == 8);
  assert(kpow2(9) == 16 && kpow2(257) == 512 && kpow2(4096) == 4096);
  puts("PASS: io_uring UAPI matches Linux headers (sizes, offsets, opcodes)");
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='avory-iouring-abi-') as tmp:
    p = Path(tmp)
    (p / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    str(p / 'test.c'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
