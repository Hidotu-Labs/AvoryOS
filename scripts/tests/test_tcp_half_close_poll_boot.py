#!/usr/bin/env python3
"""TCP half-close must not look readable until the peer sends data or FIN.

A local shutdown(SHUT_WR) puts the connection in FIN_WAIT_1/FIN_WAIT_2.  Those
states previously reported POLLIN|POLLHUP from inet_poll() while a nonblocking
recvfrom() returned EAGAIN, which makes level-triggered event loops (Firefox's
Socket Thread in particular) spin on a socket that has nothing to read.

The guest connects to a listener on the QEMU host (10.0.2.2), half-closes its
write side, and checks poll()/epoll_wait()/recvfrom() both before and after the
peer sends data and its FIN.
"""
from pathlib import Path
import socket
import threading
import time

from boot_probe import run_probe

ROOT = Path(__file__).resolve().parents[2]

# Host side: accept one connection, wait for the guest's FIN, keep the read
# side open long enough for the no-readiness checks, then send data and close.
server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(('127.0.0.1', 0))
server.listen(1)
port = server.getsockname()[1]
host_events = []
DATA_DELAY_S = 3.0
FIN_DELAY_S = 1.5


def host_peer():
    try:
        conn, _ = server.accept()
        conn.settimeout(30)
        host_events.append('accepted')
        while True:
            chunk = conn.recv(4096)
            if not chunk:
                break
            host_events.append('guest-data')
        host_events.append('guest-fin')
        time.sleep(DATA_DELAY_S)
        conn.sendall(b'hello')
        host_events.append('sent-data')
        time.sleep(FIN_DELAY_S)
        conn.close()
        host_events.append('closed')
    except Exception as exc:  # noqa: BLE001 - reported on assertion failure
        host_events.append(f'error:{exc!r}')


threading.Thread(target=host_peer, daemon=True).start()

INIT = r'''
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT @PORT@
static int failures;

static void fail(const char *what, int value, int err) {
    printf("TCP_HALF_CLOSE_POLL_BOOT: FAIL %s value=%d errno=%d\n",
           what, value, err);
    failures++;
}

static int wait_event(int ep, struct epoll_event *event, int timeout_ms) {
    for (;;) {
        int n = epoll_wait(ep, event, 1, timeout_ms);
        if (n < 0 && errno == EINTR) continue;
        return n;
    }
}

static int wait_poll(int fd, short events, int timeout_ms) {
    struct pollfd p = {.fd = fd, .events = events};
    for (;;) {
        int n = poll(&p, 1, timeout_ms);
        if (n < 0 && errno == EINTR) continue;
        return n < 0 ? -errno : p.revents;
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    puts("TCP_HALF_CLOSE_POLL_BOOT: START");

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fail("socket", fd, errno);
        goto out;
    }

    struct sockaddr_in peer = {0};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(PORT);
    if (inet_pton(AF_INET, "10.0.2.2", &peer.sin_addr) != 1) {
        fail("inet_pton", -1, errno);
        goto out;
    }

    int connected = -1;
    for (int attempt = 0; attempt < 60; attempt++) {
        connected = connect(fd, (struct sockaddr *)&peer, sizeof(peer));
        if (connected == 0) break;
        if (errno != ECONNREFUSED && errno != ENETUNREACH &&
            errno != EHOSTUNREACH && errno != ETIMEDOUT) {
            fail("connect", connected, errno);
            goto out;
        }
        usleep(250000);
    }
    if (connected) {
        fail("connect-retries", connected, errno);
        goto out;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        fail("set-nonblocking", flags, errno);
        goto out;
    }

    int ep = epoll_create1(0);
    if (ep < 0) {
        fail("epoll_create1", ep, errno);
        goto out;
    }
    struct epoll_event interest = {.events = EPOLLIN, .data.fd = fd};
    if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &interest) < 0) {
        fail("epoll_ctl-add", -1, errno);
        goto out;
    }

    if (shutdown(fd, SHUT_WR) < 0) {
        fail("shutdown", -1, errno);
        goto out;
    }

    /* Local FIN only: the read direction is still open and empty. */
    int revents = wait_poll(fd, POLLIN, 500);
    if (revents != 0)
        fail("poll-after-local-fin", revents, errno);

    char buf[16];
    ssize_t got = recv(fd, buf, 5, 0);
    if (got != -1 || errno != EAGAIN)
        fail("recv-after-local-fin", (int)got, errno);

    struct epoll_event event = {0};
    int n = wait_event(ep, &event, 400);
    if (n != 0)
        fail("epoll-after-local-fin", n, errno);

    /* The peer's data must make the same socket readable again. */
    n = wait_event(ep, &event, 8000);
    if (n != 1 || !(event.events & EPOLLIN)) {
        fail("epoll-peer-data", n, errno);
        goto out;
    }
    revents = wait_poll(fd, POLLIN, 0);
    if (!(revents & POLLIN))
        fail("poll-peer-data", revents, errno);
    memset(buf, 0, sizeof(buf));
    got = recv(fd, buf, 5, 0);
    if (got != 5 || memcmp(buf, "hello", 5) != 0)
        fail("recv-peer-data", (int)got, errno);

    /* The peer's FIN must deliver EOF, not EAGAIN. */
    n = wait_event(ep, &event, 8000);
    if (n != 1 || !(event.events & EPOLLIN)) {
        fail("epoll-peer-fin", n, errno);
        goto out;
    }
    revents = wait_poll(fd, POLLIN, 0);
    if (!(revents & POLLIN))
        fail("poll-peer-fin", revents, errno);
    got = recv(fd, buf, sizeof(buf), 0);
    if (got != 0)
        fail("recv-peer-fin", (int)got, errno);

out:
    printf("TCP_HALF_CLOSE_POLL_BOOT: %s failures=%d\n",
           failures ? "FAIL" : "PASS", failures);
    fflush(NULL);
    reboot(RB_POWER_OFF);
    for (;;) pause();
}
'''

if __name__ == '__main__':
    try:
        run_probe('tcp-half-close-poll-boot',
                  INIT.replace('@PORT@', str(port)),
                  'TCP_HALF_CLOSE_POLL_BOOT: PASS',
                  timeout=180,
                  qemu_args=['-netdev', 'user,id=n0',
                             '-device', 'rtl8139,netdev=n0'])
    except Exception:
        print('host peer events:', host_events)
        raise
    finally:
        server.close()
