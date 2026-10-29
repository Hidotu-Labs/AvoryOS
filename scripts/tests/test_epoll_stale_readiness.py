#!/usr/bin/env python3
"""Run the actual epoll readiness helper after a notified pipe is drained."""
from pathlib import Path
import argparse
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=ROOT / 'kernel/src/socket/epoll.c')
args = parser.parse_args()
source = args.source.read_text()
start = source.index('static inline uint32_t ep_check_events_mask(')
helper = source[start:source.index('// Helper: Add item to ready list', start)]
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#define EPOLLIN 1u
#define EPOLLOUT 4u
#define EPOLLERR 8u
#define EPOLLHUP 16u
#define EPOLLRDHUP 8192u
#define EPOLLEXCLUSIVE (1u << 28)
#define EPOLLWAKEUP (1u << 29)
#define EPOLLONESHOT (1u << 30)
#define EPOLLET (1u << 31)
typedef struct { uint32_t current_events; unsigned polls; } node_t;
typedef struct { node_t *node; uint32_t registered_events,ready_events; uint32_t removed,oneshot_disabled; } epitem_t;
static int vfs_poll(node_t *node, int watch) {
  assert(!(watch & (EPOLLET|EPOLLONESHOT|EPOLLEXCLUSIVE|EPOLLWAKEUP)));
  node->polls++;
  return node->current_events;
}
'''
tests = r'''
int main(void) {
 node_t pipe = {0};
 for (unsigned mode=0; mode<2; mode++) {
  epitem_t item = {.node=&pipe,.registered_events=EPOLLIN|(mode?EPOLLET:0)};
  // A write notifies epoll, then an earlier batch/reader drains the pipe.
  // Wine's blocking read_request must not be called for this stale candidate.
  item.ready_events=EPOLLIN;pipe.current_events=0;
  unsigned before=pipe.polls;
  assert(ep_check_events(&item)==0);
  assert(pipe.polls==before+1 && item.ready_events==0);
  pipe.current_events=EPOLLIN;item.ready_events=EPOLLIN;
  assert(ep_check_events(&item)==EPOLLIN);
  pipe.current_events=0;assert(ep_check_events(&item)==0);
  // Cached POLLOUT must not hide newly readable data or current hangup.
  pipe.current_events=EPOLLIN|EPOLLHUP;item.ready_events=EPOLLOUT;
  assert(ep_check_events(&item)==(EPOLLIN|EPOLLHUP));
  item.ready_events=EPOLLIN;pipe.current_events=EPOLLOUT;
  assert(ep_check_events(&item)==0); // Unrequested events stay masked.
  pipe.current_events=EPOLLERR|EPOLLHUP|EPOLLRDHUP;
  assert(ep_check_events(&item)==(EPOLLERR|EPOLLHUP));
  // A half-close is readable EOF, not an unsolicited epoll error bit.
  pipe.current_events=EPOLLIN|EPOLLRDHUP;
  assert(ep_check_events(&item)==EPOLLIN);
  item.registered_events |= EPOLLRDHUP;
  assert(ep_check_events(&item)==(EPOLLIN|EPOLLRDHUP));
 }
 epitem_t missing={0};assert(ep_check_events(NULL)==0 && ep_check_events(&missing)==0);
 puts("PASS: LT/ET stale pipe notifications, live readiness, event masks and current error/hangup");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    (tmp / 'test.c').write_text(prefix + helper + tests)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined', str(tmp / 'test.c'),
                    '-o', str(tmp / 'test')], check=True)
    subprocess.run([str(tmp / 'test')], check=True)
