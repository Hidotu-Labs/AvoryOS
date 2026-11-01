#!/usr/bin/env python3
"""Exercise the socket and process setup Chromium's zygote host runs at launch.

Each step prints its result. The browser's zygote host aborts with
"Check failed: . : Invalid argument (22)" at zygote_host_impl_linux.cc:221,
so the first EINVAL here is the call to fix."""
import os
from boot_probe import run_probe

INIT = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
struct ucred { int pid; unsigned int uid; unsigned int gid; };
#ifndef SOCK_SEQPACKET
#define SOCK_SEQPACKET 5
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 02000000
#endif
#ifndef SO_PASSCRED
#define SO_PASSCRED 16
#endif
#ifndef SO_PEERCRED
#define SO_PEERCRED 17
#endif
#ifndef SCM_CREDENTIALS
#define SCM_CREDENTIALS 2
#endif
#ifndef SCM_RIGHTS
#define SCM_RIGHTS 1
#endif
#define STEP(name, expr) do { errno = 0; long _r = (long)(expr); \
  printf("STEP %s -> %ld errno=%d (%s)\n", name, _r, _r < 0 ? errno : 0, _r < 0 ? strerror(errno) : "ok"); \
  if (_r < 0) failed++; } while (0)
static int failed = 0;
int main(void) {
 setbuf(stdout, NULL); puts("ZYGOTE_HOST_SETUP_BOOT: START");
 int fds[2] = {-1, -1};
 STEP("socketpair SEQPACKET|CLOEXEC", socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds));
 int one = 1;
 STEP("setsockopt SO_PASSCRED", setsockopt(fds[0], SOL_SOCKET, SO_PASSCRED, &one, sizeof(one)));
 STEP("setsockopt SO_PASSCRED peer", setsockopt(fds[1], SOL_SOCKET, SO_PASSCRED, &one, sizeof(one)));
 struct ucred cred; socklen_t clen = sizeof(cred);
 STEP("getsockopt SO_PEERCRED", getsockopt(fds[0], SOL_SOCKET, SO_PEERCRED, &cred, &clen));
 STEP("fcntl F_SETFL O_NONBLOCK", fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK));
 STEP("fcntl F_SETFD FD_CLOEXEC", fcntl(fds[0], F_SETFD, FD_CLOEXEC));
 STEP("prctl PR_SET_PDEATHSIG", prctl(PR_SET_PDEATHSIG, SIGKILL));
 STEP("setsid in child-only ok", 0);
 char data = 'x';
 struct iovec iov = {&data, 1};
 char cbuf[CMSG_SPACE(sizeof(int)) + CMSG_SPACE(sizeof(struct ucred))];
 memset(cbuf, 0, sizeof(cbuf));
 struct msghdr msg; memset(&msg, 0, sizeof(msg));
 msg.msg_iov = &iov; msg.msg_iovlen = 1; msg.msg_control = cbuf; msg.msg_controllen = sizeof(cbuf);
 struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
 c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
 *(int *)CMSG_DATA(c) = fds[1];
 msg.msg_controllen = CMSG_SPACE(sizeof(int));
 STEP("sendmsg SCM_RIGHTS", sendmsg(fds[0], &msg, MSG_NOSIGNAL));
 char rbuf = 0; struct iovec riov = {&rbuf, 1};
 char rcbuf[CMSG_SPACE(sizeof(struct ucred)) + CMSG_SPACE(sizeof(int))];
 struct msghdr rmsg; memset(&rmsg, 0, sizeof(rmsg));
 rmsg.msg_iov = &riov; rmsg.msg_iovlen = 1; rmsg.msg_control = rcbuf; rmsg.msg_controllen = sizeof(rcbuf);
 STEP("recvmsg", recvmsg(fds[1], &rmsg, 0));
 { int p2[2]; STEP("socketpair SOCK_STREAM", socketpair(AF_UNIX, SOCK_STREAM, 0, p2)); }
 pid_t p = fork();
 if (!p) _exit(0);
 int st; STEP("waitpid", waitpid(p, &st, 0));
 printf("ZYGOTE_HOST_SETUP_BOOT: %s failed=%d\n", failed ? "FAIL" : "PASS", failed);
 reboot(RB_POWER_OFF); for (;;) pause();
}
'''
run_probe('zygote-host-setup-boot', INIT, 'ZYGOTE_HOST_SETUP_BOOT: PASS', accel=os.environ.get('AVORY_TEST_ACCEL', 'tcg'), timeout=90)
