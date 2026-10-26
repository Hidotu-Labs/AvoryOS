#!/usr/bin/env python3
"""Exercise console VT query dispatch and Linux output ABI used by Xorg."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def extract(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


console = (ROOT / 'kernel/src/fb/framebuffer.c').read_text()
shared = (ROOT / 'kernel/src/syscalls/sys_io_shared.h').read_text()
syscall = (ROOT / 'kernel/src/syscalls/sys_ioctl.c').read_text()
queries = syscall[syscall.index('  case VT_OPENQRY: {'):
                  syscall.index('  case VT_RELDISP:', syscall.index('  case VT_OPENQRY: {'))]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "terminal.h"
struct vfs_node { int unused; };
struct thread { int pgid, sid, tgid; struct vfs_node *ctty; };
struct termios console_termios;
static int kd_mode, copy_fault;
static struct thread current = {.pgid = 1};
static struct thread *sched_get_current(void) { return &current; }
static unsigned fb_get_width(void) { return 800; }
static unsigned fb_get_height(void) { return 600; }
static void fb_set_kd_mode(int mode) { kd_mode = mode; }
static int fb_get_kd_mode(void) { return kd_mode; }
static bool vmm_is_user_addr_range_valid(uint64_t p, size_t n) {
    (void)n; return p > 4096;
}
static int copy_to_user(void *dst, const void *src, size_t n) {
    if (copy_fault || (uintptr_t)dst < 4096) return -1;
    memcpy(dst, src, n); return 0;
}
'''
code += extract(shared, 'struct vt_mode {') + ';\n'
code += extract(shared, 'struct vt_stat {') + ';\n'
code += extract(console, 'static int console_dev_ioctl(')
code += '''
static int dispatch(uint32_t request, uint64_t arg) {
    int result = console_dev_ioctl(NULL, request, arg);
    if (result != -25) return result;
    uint64_t ret = 0;
    switch (request) {
''' + queries + '''
    default: return -25;
    }
    return (int)ret;
}
'''
code += r'''
int main(void) {
    struct vfs_node console = {0}, other = {0};
    current.sid = current.tgid = 42;
    assert(!console_dev_ioctl(&console, 0x540E, 0));
    assert(current.ctty == &console);
    assert(!console_dev_ioctl(&console, 0x540E, 0));
    assert(console_dev_ioctl(&other, 0x540E, 0) == -1);
    assert(current.ctty == &console);
    assert(!console_dev_ioctl(&other, 0x5422, 0));
    assert(current.ctty == &console);
    assert(!console_dev_ioctl(&console, 0x5422, 0));
    assert(!current.ctty);
    current.sid = 41;
    assert(console_dev_ioctl(&console, 0x540E, 0) == -1);
    assert(!current.ctty);
    current.sid = 42;
    /* Xorg initializes vtno to -1 before querying /dev/tty0. */
    struct { int vt; uint32_t guard; } query = {-1, 0x12345678};
    assert(!dispatch(VT_OPENQRY, (uintptr_t)&query.vt));
    assert(query.vt == 1 && query.guard == 0x12345678);
    struct { struct vt_mode value; uint32_t guard; } mode;
    memset(&mode, 0xa5, sizeof(mode));
    assert(!dispatch(VT_GETMODE, (uintptr_t)&mode.value));
    struct vt_mode zero = {0};
    assert(!memcmp(&mode.value, &zero, sizeof(zero)));
    assert(mode.guard == 0xa5a5a5a5);
    struct { struct vt_stat value; uint32_t guard; } state;
    memset(&state, 0xa5, sizeof(state));
    assert(!dispatch(VT_GETSTATE, (uintptr_t)&state.value));
    assert(state.value.v_active == 1 && state.value.v_signal == 0);
    assert(state.value.v_state == (1 << state.value.v_active));
    assert(state.guard == 0xa5a5a5a5);
    unsigned requests[] = {VT_OPENQRY, VT_GETMODE, VT_GETSTATE};
    for (unsigned i = 0; i < 3; i++) {
        assert(dispatch(requests[i], 0) == -14);
        assert(dispatch(requests[i], 1) == -14);
        copy_fault = 1;
        assert(dispatch(requests[i], (uintptr_t)&query) == -14);
        copy_fault = 0;
    }
    assert(!dispatch(VT_ACTIVATE, 1));
    assert(!dispatch(VT_WAITACTIVE, 1));
    assert(!dispatch(KDSETMODE, KD_GRAPHICS));
    int kd = -1;
    assert(!dispatch(KDGETMODE, (uintptr_t)&kd) && kd == KD_GRAPHICS);
    puts("PASS: console VT queries fill Linux ABI outputs, preserve bounds, reject bad pointers and retain graphics mode switching");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-vt-') as tmp:
    path = Path(tmp)
    (path / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined', '-I', str(ROOT / 'kernel/src/fb'),
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
