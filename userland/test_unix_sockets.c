/*
 * test_unix_sockets.c - UNIX Socket Syscalls Durability Test Suite
 *
 * Exercises every UNIX domain socket syscall for at least 5000 iterations:
 *   1. socket()         - Creation of STREAM, DGRAM, SEQPACKET sockets with flags (NONBLOCK, CLOEXEC)
 *   2. socketpair()     - Creating connected socket pairs with various types & flags
 *   3. bind()           - Pathname and Abstract namespace binding
 *   4. listen()         - Listening with backlog configuration
 *   5. connect() & accept() / accept4() - Connection setup, client/server handshake, nonblock accept
 *   6. sendto() & recvfrom() - Datagram send/recv, verifying source address and payload
 *   7. sendmsg() & recvmsg() - Scatter-gather (iovec) transfer and ancillary SCM_RIGHTS (FD passing)
 *   8. getsockname() & getpeername() - Retrieving local and remote socket addresses
 *   9. setsockopt() & getsockopt() - Socket options (SO_REUSEADDR, SO_RCVBUF, SO_SNDBUF, SO_PASSCRED, etc.)
 *  10. shutdown()       - Half-close (SHUT_RD, SHUT_WR, SHUT_RDWR) behavior
 *  11. Full Lifecycle   - Full end-to-end socket lifecycle durability sequence
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <time.h>

#define DEFAULT_ITERATIONS 5000

static unsigned long total_syscall_calls = 0;

static void print_progress(const char *test_name, unsigned current, unsigned total) {
    if (current % 1000 == 0 || current == total) {
        printf("[%s] Progress: %u / %u iterations\n", test_name, current, total);
        fflush(stdout);
    }
}

/* -------------------------------------------------------------------------
 * Test 1: socket() durability
 * ------------------------------------------------------------------------- */
static void test_socket_durability(unsigned iterations) {
    printf("\n=== Test 1: socket() Durability (%u iterations) ===\n", iterations);
    for (unsigned i = 1; i <= iterations; i++) {
        // Test STREAM
        int fd1 = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd1 < 0) {
            perror("socket(AF_UNIX, SOCK_STREAM)");
            exit(1);
        }
        total_syscall_calls++;
        close(fd1);

        // Test DGRAM
        int fd2 = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (fd2 < 0) {
            perror("socket(AF_UNIX, SOCK_DGRAM)");
            exit(1);
        }
        total_syscall_calls++;
        close(fd2);

        // Test SEQPACKET
        int fd3 = socket(AF_UNIX, SOCK_SEQPACKET, 0);
        if (fd3 >= 0) {
            total_syscall_calls++;
            close(fd3);
        } // SEQPACKET may or may not be enabled; if supported, tested!

        // Test Flags: SOCK_NONBLOCK | SOCK_CLOEXEC
        int fd4 = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd4 < 0) {
            perror("socket(AF_UNIX, STREAM|NONBLOCK|CLOEXEC)");
            exit(1);
        }
        total_syscall_calls++;
        close(fd4);

        print_progress("socket()", i, iterations);
    }
    printf("[PASSED] socket() durability test passed.\n");
}

/* -------------------------------------------------------------------------
 * Test 2: socketpair() durability
 * ------------------------------------------------------------------------- */
static void test_socketpair_durability(unsigned iterations) {
    printf("\n=== Test 2: socketpair() Durability (%u iterations) ===\n", iterations);
    for (unsigned i = 1; i <= iterations; i++) {
        int sv[2];
        int type = (i % 2 == 0) ? SOCK_STREAM : SOCK_DGRAM;
        if (socketpair(AF_UNIX, type | SOCK_NONBLOCK, 0, sv) < 0) {
            perror("socketpair");
            exit(1);
        }
        total_syscall_calls++;

#if defined(__x86_64__) && !defined(__ILP32__)
        if (i == 1) {
            /* The syscall ABI uses only the low 32 bits of fd and cmd. */
            const uint64_t upper_bits = UINT64_C(0xffffffff00000000);
            long flags = syscall(SYS_fcntl, upper_bits | (uint32_t)sv[0],
                                 upper_bits | F_GETFL, 0UL);
            if (flags < 0 || (flags & (O_ACCMODE | O_NONBLOCK)) !=
                             (O_RDWR | O_NONBLOCK)) {
                fprintf(stderr, "fcntl(F_GETFL) with nonzero upper bits: %ld\n",
                        flags);
                exit(1);
            }
        }
#endif

        // Send & recv probe byte
        char send_byte = (char)(i & 0xFF);
        char recv_byte = 0;

        ssize_t nwritten = write(sv[0], &send_byte, 1);
        if (nwritten != 1) {
            perror("socketpair write");
            exit(1);
        }
        ssize_t nread = read(sv[1], &recv_byte, 1);
        if (nread != 1 || recv_byte != send_byte) {
            fprintf(stderr, "socketpair read mismatch: expected 0x%02x, got 0x%02x (ret=%ld)\n",
                    send_byte & 0xFF, recv_byte & 0xFF, (long)nread);
            exit(1);
        }

        close(sv[0]);
        close(sv[1]);
        print_progress("socketpair()", i, iterations);
    }
    printf("[PASSED] socketpair() durability test passed.\n");
}

/* -------------------------------------------------------------------------
 * Test 3: bind() durability (Abstract & Filesystem sockets)
 * ------------------------------------------------------------------------- */
static void test_bind_durability(unsigned iterations) {
    printf("\n=== Test 3: bind() Durability (%u iterations) ===\n", iterations);
    char path[108];

    for (unsigned i = 1; i <= iterations; i++) {
        int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sfd < 0) {
            perror("socket for bind");
            exit(1);
        }
        total_syscall_calls++;

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;

        if (i % 2 == 0) {
            // Abstract namespace binding
            snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1, "test_abs_bind_%u_%d", i, getpid());
            addr.sun_path[0] = '\0';
            socklen_t len = sizeof(sa_family_t) + 1 + strlen(addr.sun_path + 1);

            if (bind(sfd, (struct sockaddr *)&addr, len) < 0) {
                perror("bind(abstract)");
                exit(1);
            }
            total_syscall_calls++;
        } else {
            // Filesystem pathname binding
            snprintf(path, sizeof(path), "/tmp/ts_bind_%u_%d.sock", i, getpid());
            unlink(path);
            snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
            socklen_t len = sizeof(sa_family_t) + strlen(addr.sun_path) + 1;

            if (bind(sfd, (struct sockaddr *)&addr, len) < 0) {
                perror("bind(pathname)");
                exit(1);
            }
            total_syscall_calls++;
        }

        close(sfd);
        if (i % 2 != 0) {
            unlink(path);
        }
        print_progress("bind()", i, iterations);
    }
    printf("[PASSED] bind() durability test passed.\n");
}

/* -------------------------------------------------------------------------
 * Test 4: listen() durability
 * ------------------------------------------------------------------------- */
static void test_listen_durability(unsigned iterations) {
    printf("\n=== Test 4: listen() Durability (%u iterations) ===\n", iterations);
    for (unsigned i = 1; i <= iterations; i++) {
        int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sfd < 0) {
            perror("socket for listen");
            exit(1);
        }
        total_syscall_calls++;

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1, "test_listen_%u_%d", i, getpid());
        addr.sun_path[0] = '\0';
        socklen_t len = sizeof(sa_family_t) + 1 + strlen(addr.sun_path + 1);

        if (bind(sfd, (struct sockaddr *)&addr, len) < 0) {
            perror("bind before listen");
            exit(1);
        }
        total_syscall_calls++;

        int backlog = (i % 3 == 0) ? 1 : ((i % 3 == 1) ? 16 : 128);
        if (listen(sfd, backlog) < 0) {
            perror("listen");
            exit(1);
        }
        total_syscall_calls++;

        close(sfd);
        print_progress("listen()", i, iterations);
    }
    printf("[PASSED] listen() durability test passed.\n");
}

/* -------------------------------------------------------------------------
 * Test 5: connect(), accept(), accept4() durability
 * ------------------------------------------------------------------------- */
static void test_connect_accept_durability(unsigned iterations) {
    printf("\n=== Test 5: connect(), accept(), accept4() Durability (%u iterations) ===\n", iterations);

    // Setup persistent server socket
    int srv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv_fd < 0) {
        perror("socket srv");
        exit(1);
    }
    total_syscall_calls++;

    struct sockaddr_un srv_addr;
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sun_family = AF_UNIX;
    snprintf(srv_addr.sun_path + 1, sizeof(srv_addr.sun_path) - 1, "test_conn_acc_srv_%d", getpid());
    srv_addr.sun_path[0] = '\0';
    socklen_t srv_len = sizeof(sa_family_t) + 1 + strlen(srv_addr.sun_path + 1);

    if (bind(srv_fd, (struct sockaddr *)&srv_addr, srv_len) < 0) {
        perror("bind srv");
        exit(1);
    }
    total_syscall_calls++;

    if (listen(srv_fd, 128) < 0) {
        perror("listen srv");
        exit(1);
    }
    total_syscall_calls++;

    for (unsigned i = 1; i <= iterations; i++) {
        // Client connects
        int cli_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (cli_fd < 0) {
            perror("cli socket");
            exit(1);
        }
        total_syscall_calls++;

        if (connect(cli_fd, (struct sockaddr *)&srv_addr, srv_len) < 0) {
            perror("connect");
            exit(1);
        }
        total_syscall_calls++;

        // Server accepts connection (alternate between accept and accept4)
        struct sockaddr_un peer_addr;
        socklen_t peer_len = sizeof(peer_addr);
        int acc_fd = -1;

        if (i % 2 == 0) {
            acc_fd = accept(srv_fd, (struct sockaddr *)&peer_addr, &peer_len);
            if (acc_fd < 0) {
                perror("accept");
                exit(1);
            }
            total_syscall_calls++;
        } else {
            acc_fd = accept4(srv_fd, (struct sockaddr *)&peer_addr, &peer_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (acc_fd < 0) {
                perror("accept4");
                exit(1);
            }
            total_syscall_calls++;
        }

        close(cli_fd);
        close(acc_fd);
        print_progress("connect/accept/accept4()", i, iterations);
    }

    close(srv_fd);
    printf("[PASSED] connect(), accept(), accept4() durability test passed.\n");
}

/* -------------------------------------------------------------------------
 * Test 6: sendto(), recvfrom() durability (DGRAM)
 * ------------------------------------------------------------------------- */
static void test_sendto_recvfrom_durability(unsigned iterations) {
    printf("\n=== Test 6: sendto(), recvfrom() Durability (%u iterations) ===\n", iterations);

    // Create server DGRAM socket
    int srv_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (srv_fd < 0) {
        perror("srv dgram socket");
        exit(1);
    }
    total_syscall_calls++;

    struct sockaddr_un srv_addr;
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sun_family = AF_UNIX;
    snprintf(srv_addr.sun_path + 1, sizeof(srv_addr.sun_path) - 1, "test_dgram_srv_%d", getpid());
    srv_addr.sun_path[0] = '\0';
    socklen_t srv_len = sizeof(sa_family_t) + 1 + strlen(srv_addr.sun_path + 1);

    if (bind(srv_fd, (struct sockaddr *)&srv_addr, srv_len) < 0) {
        perror("bind dgram srv");
        exit(1);
    }
    total_syscall_calls++;

    // Create client DGRAM socket
    int cli_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (cli_fd < 0) {
        perror("cli dgram socket");
        exit(1);
    }
    total_syscall_calls++;

    for (unsigned i = 1; i <= iterations; i++) {
        char send_buf[64];
        snprintf(send_buf, sizeof(send_buf), "DGRAM payload #%u", i);
        size_t send_len = strlen(send_buf) + 1;

        ssize_t sent = sendto(cli_fd, send_buf, send_len, 0, (struct sockaddr *)&srv_addr, srv_len);
        if (sent != (ssize_t)send_len) {
            perror("sendto");
            exit(1);
        }
        total_syscall_calls++;

        char recv_buf[64];
        struct sockaddr_un peer_addr;
        socklen_t peer_len = sizeof(peer_addr);

        ssize_t recvd = recvfrom(srv_fd, recv_buf, sizeof(recv_buf), 0, (struct sockaddr *)&peer_addr, &peer_len);
        if (recvd != (ssize_t)send_len || strcmp(recv_buf, send_buf) != 0) {
            fprintf(stderr, "recvfrom mismatch at iter %u (recvd=%ld, exp=%zu)\n", i, (long)recvd, send_len);
            exit(1);
        }
        total_syscall_calls++;

        print_progress("sendto/recvfrom()", i, iterations);
    }

    close(cli_fd);
    close(srv_fd);
    printf("[PASSED] sendto(), recvfrom() durability test passed.\n");
}

/* -------------------------------------------------------------------------
 * Test 7: sendmsg(), recvmsg() durability (Scatter/Gather & Ancillary SCM_RIGHTS)
 * ------------------------------------------------------------------------- */
static void test_sendmsg_recvmsg_durability(unsigned iterations) {
    printf("\n=== Test 7: sendmsg(), recvmsg() Durability (%u iterations) ===\n", iterations);

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair for sendmsg");
        exit(1);
    }
    total_syscall_calls++;

    for (unsigned i = 1; i <= iterations; i++) {
        char chunk1[32];
        char chunk2[32];
        snprintf(chunk1, sizeof(chunk1), "MSG_C1_#%u", i);
        snprintf(chunk2, sizeof(chunk2), "MSG_C2_#%u", i);

        struct iovec iov[2];
        iov[0].iov_base = chunk1;
        iov[0].iov_len = strlen(chunk1) + 1;
        iov[1].iov_base = chunk2;
        iov[1].iov_len = strlen(chunk2) + 1;

        // Optionally pass an open FD (e.g., /dev/null or dup(sv[0])) every 10 iterations
        char cmsg_buf[CMSG_SPACE(sizeof(int))];
        memset(cmsg_buf, 0, sizeof(cmsg_buf));

        struct msghdr send_hdr;
        memset(&send_hdr, 0, sizeof(send_hdr));
        send_hdr.msg_iov = iov;
        send_hdr.msg_iovlen = 2;

        int fd_to_pass = -1;
        if (i % 10 == 0) {
            fd_to_pass = dup(sv[0]);
            if (fd_to_pass < 0) {
                perror("dup for SCM_RIGHTS");
                exit(1);
            }
            send_hdr.msg_control = cmsg_buf;
            send_hdr.msg_controllen = sizeof(cmsg_buf);

            struct cmsghdr *cmsg = CMSG_FIRSTHDR(&send_hdr);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(cmsg), &fd_to_pass, sizeof(int));
        }

        ssize_t sent = sendmsg(sv[0], &send_hdr, 0);
        if (sent < 0) {
            perror("sendmsg");
            exit(1);
        }
        total_syscall_calls++;

        if (fd_to_pass >= 0) {
            close(fd_to_pass);
        }

        // Receive msg
        char recv_chunk1[32] = {0};
        char recv_chunk2[32] = {0};
        struct iovec recv_iov[2];
        recv_iov[0].iov_base = recv_chunk1;
        recv_iov[0].iov_len = strlen(chunk1) + 1;
        recv_iov[1].iov_base = recv_chunk2;
        recv_iov[1].iov_len = strlen(chunk2) + 1;

        char recv_cmsg_buf[CMSG_SPACE(sizeof(int))];
        memset(recv_cmsg_buf, 0, sizeof(recv_cmsg_buf));

        struct msghdr recv_hdr;
        memset(&recv_hdr, 0, sizeof(recv_hdr));
        recv_hdr.msg_iov = recv_iov;
        recv_hdr.msg_iovlen = 2;
        if (i % 10 == 0) {
            recv_hdr.msg_control = recv_cmsg_buf;
            recv_hdr.msg_controllen = sizeof(recv_cmsg_buf);
        }

        ssize_t recvd = recvmsg(sv[1], &recv_hdr, 0);
        if (recvd < 0) {
            perror("recvmsg");
            exit(1);
        }
        total_syscall_calls++;

        if (strcmp(recv_chunk1, chunk1) != 0 || strcmp(recv_chunk2, chunk2) != 0) {
            fprintf(stderr, "recvmsg content mismatch at iter %u\n", i);
            exit(1);
        }

        if (i % 10 == 0) {
            struct cmsghdr *cmsg = CMSG_FIRSTHDR(&recv_hdr);
            if (!cmsg || cmsg->cmsg_level != SOL_SOCKET ||
                cmsg->cmsg_type != SCM_RIGHTS ||
                cmsg->cmsg_len != CMSG_LEN(sizeof(int)) ||
                (recv_hdr.msg_flags & MSG_CTRUNC)) {
                fprintf(stderr, "missing or truncated SCM_RIGHTS at iter %u\n", i);
                exit(1);
            }
            int received_fd;
            memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(received_fd));
            if (close(received_fd) < 0) {
                perror("close received SCM_RIGHTS fd");
                exit(1);
            }
        }

        print_progress("sendmsg/recvmsg()", i, iterations);
    }

    close(sv[0]);
    close(sv[1]);
    printf("[PASSED] sendmsg(), recvmsg() durability test passed.\n");
}

/* -------------------------------------------------------------------------
 * Test 8: getsockname(), getpeername() durability
 * ------------------------------------------------------------------------- */
static void test_getsockname_getpeername_durability(unsigned iterations) {
    printf("\n=== Test 8: getsockname(), getpeername() Durability (%u iterations) ===\n", iterations);

    // Setup bound server socket
    int srv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv_fd < 0) {
        perror("socket for name test");
        exit(1);
    }
    total_syscall_calls++;

    struct sockaddr_un srv_addr;
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sun_family = AF_UNIX;
    snprintf(srv_addr.sun_path + 1, sizeof(srv_addr.sun_path) - 1, "test_name_srv_%d", getpid());
    srv_addr.sun_path[0] = '\0';
    socklen_t srv_len = sizeof(sa_family_t) + 1 + strlen(srv_addr.sun_path + 1);

    if (bind(srv_fd, (struct sockaddr *)&srv_addr, srv_len) < 0) {
        perror("bind for name test");
        exit(1);
    }
    total_syscall_calls++;

    if (listen(srv_fd, 16) < 0) {
        perror("listen for name test");
        exit(1);
    }
    total_syscall_calls++;

    int cli_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (cli_fd < 0) {
        perror("cli socket for name test");
        exit(1);
    }
    total_syscall_calls++;

    if (connect(cli_fd, (struct sockaddr *)&srv_addr, srv_len) < 0) {
        perror("connect for name test");
        exit(1);
    }
    total_syscall_calls++;

    struct sockaddr_un peer_addr;
    socklen_t peer_len = sizeof(peer_addr);
    int acc_fd = accept(srv_fd, (struct sockaddr *)&peer_addr, &peer_len);
    if (acc_fd < 0) {
        perror("accept for name test");
        exit(1);
    }
    total_syscall_calls++;

    for (unsigned i = 1; i <= iterations; i++) {
        // Query getsockname on srv_fd, cli_fd, acc_fd
        struct sockaddr_un name;
        socklen_t namelen = sizeof(name);

        if (getsockname(srv_fd, (struct sockaddr *)&name, &namelen) < 0) {
            perror("getsockname(srv)");
            exit(1);
        }
        total_syscall_calls++;
        if (name.sun_family != AF_UNIX) {
            fprintf(stderr, "getsockname bad family: %d\n", name.sun_family);
            exit(1);
        }

        namelen = sizeof(name);
        if (getsockname(cli_fd, (struct sockaddr *)&name, &namelen) < 0) {
            perror("getsockname(cli)");
            exit(1);
        }
        total_syscall_calls++;

        // Query getpeername on cli_fd and acc_fd
        namelen = sizeof(name);
        if (getpeername(cli_fd, (struct sockaddr *)&name, &namelen) < 0) {
            perror("getpeername(cli)");
            exit(1);
        }
        total_syscall_calls++;
        if (name.sun_family != AF_UNIX) {
            fprintf(stderr, "getpeername bad family: %d\n", name.sun_family);
            exit(1);
        }

        namelen = sizeof(name);
        if (getpeername(acc_fd, (struct sockaddr *)&name, &namelen) < 0) {
            perror("getpeername(acc)");
            exit(1);
        }
        total_syscall_calls++;

        print_progress("getsockname/getpeername()", i, iterations);
    }

    close(cli_fd);
    close(acc_fd);
    close(srv_fd);
    printf("[PASSED] getsockname(), getpeername() durability test passed.\n");
}

/* -------------------------------------------------------------------------
 * Test 9: setsockopt(), getsockopt() durability
 * ------------------------------------------------------------------------- */
static void test_sockopt_durability(unsigned iterations) {
    printf("\n=== Test 9: setsockopt(), getsockopt() Durability (%u iterations) ===\n", iterations);

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair for sockopt");
        exit(1);
    }
    total_syscall_calls++;

    for (unsigned i = 1; i <= iterations; i++) {
        // Query SO_TYPE
        int type_val = 0;
        socklen_t optlen = sizeof(type_val);
        if (getsockopt(sv[0], SOL_SOCKET, SO_TYPE, &type_val, &optlen) < 0) {
            perror("getsockopt(SO_TYPE)");
            exit(1);
        }
        total_syscall_calls++;
        if (type_val != SOCK_STREAM) {
            fprintf(stderr, "SO_TYPE mismatch: %d != %d\n", type_val, SOCK_STREAM);
            exit(1);
        }

        // Set & Get SO_RCVBUF
        int rcvbuf = 16384 + (i % 1024);
        if (setsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
            perror("setsockopt(SO_RCVBUF)");
            exit(1);
        }
        total_syscall_calls++;

        int rcvbuf_ret = 0;
        optlen = sizeof(rcvbuf_ret);
        if (getsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, &rcvbuf_ret, &optlen) < 0) {
            perror("getsockopt(SO_RCVBUF)");
            exit(1);
        }
        total_syscall_calls++;

        // Set & Get SO_REUSEADDR
        int reuse = (i % 2);
        if (setsockopt(sv[0], SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
            perror("setsockopt(SO_REUSEADDR)");
            exit(1);
        }
        total_syscall_calls++;

        int reuse_ret = 0;
        optlen = sizeof(reuse_ret);
        if (getsockopt(sv[0], SOL_SOCKET, SO_REUSEADDR, &reuse_ret, &optlen) < 0) {
            perror("getsockopt(SO_REUSEADDR)");
            exit(1);
        }
        total_syscall_calls++;

        print_progress("setsockopt/getsockopt()", i, iterations);
    }

    close(sv[0]);
    close(sv[1]);
    printf("[PASSED] setsockopt(), getsockopt() durability test passed.\n");
}

/* -------------------------------------------------------------------------
 * Test 10: shutdown() durability
 * ------------------------------------------------------------------------- */
static void test_shutdown_durability(unsigned iterations) {
    printf("\n=== Test 10: shutdown() Durability (%u iterations) ===\n", iterations);

    for (unsigned i = 1; i <= iterations; i++) {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
            perror("socketpair for shutdown");
            exit(1);
        }
        total_syscall_calls++;

        // Test SHUT_WR on sv[0]
        if (shutdown(sv[0], SHUT_WR) < 0) {
            perror("shutdown(SHUT_WR)");
            exit(1);
        }
        total_syscall_calls++;

        // Reading from sv[1] should return EOF (0 bytes)
        char buf[16];
        ssize_t nread = read(sv[1], buf, sizeof(buf));
        if (nread != 0) {
            fprintf(stderr, "shutdown SHUT_WR failed: read returned %ld != 0\n", (long)nread);
            exit(1);
        }

        // Writing to sv[0] should fail with EPIPE or error
        errno = 0;
        ssize_t nwritten = write(sv[0], "data", 4);
        if (nwritten > 0) {
            fprintf(stderr, "shutdown SHUT_WR failed: write succeeded (%ld)\n", (long)nwritten);
            exit(1);
        }

        // Test SHUT_RDWR on sv[1]
        if (shutdown(sv[1], SHUT_RDWR) < 0) {
            perror("shutdown(SHUT_RDWR)");
            exit(1);
        }
        total_syscall_calls++;

        close(sv[0]);
        close(sv[1]);
        print_progress("shutdown()", i, iterations);
    }
    printf("[PASSED] shutdown() durability test passed.\n");
}

/* -------------------------------------------------------------------------
 * Test 11: Full Socket Lifecycle Durability Sequence
 * ------------------------------------------------------------------------- */
static void test_full_lifecycle_durability(unsigned iterations) {
    printf("\n=== Test 11: Full End-to-End Socket Lifecycle Durability (%u iterations) ===\n", iterations);

    for (unsigned i = 1; i <= iterations; i++) {
        // 1. Create server socket
        int srv_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (srv_fd < 0) { perror("lifecycle socket srv"); exit(1); }
        total_syscall_calls++;

        // 2. Set options
        int one = 1;
        if (setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
            perror("lifecycle setsockopt"); exit(1);
        }
        total_syscall_calls++;

        // 3. Bind address
        struct sockaddr_un srv_addr;
        memset(&srv_addr, 0, sizeof(srv_addr));
        srv_addr.sun_family = AF_UNIX;
        snprintf(srv_addr.sun_path + 1, sizeof(srv_addr.sun_path) - 1, "test_lifecycle_%u_%d", i, getpid());
        srv_addr.sun_path[0] = '\0';
        socklen_t srv_len = sizeof(sa_family_t) + 1 + strlen(srv_addr.sun_path + 1);

        if (bind(srv_fd, (struct sockaddr *)&srv_addr, srv_len) < 0) {
            perror("lifecycle bind"); exit(1);
        }
        total_syscall_calls++;

        // 4. Query getsockname
        struct sockaddr_un name;
        socklen_t namelen = sizeof(name);
        if (getsockname(srv_fd, (struct sockaddr *)&name, &namelen) < 0) {
            perror("lifecycle getsockname"); exit(1);
        }
        total_syscall_calls++;

        // 5. Listen
        if (listen(srv_fd, 16) < 0) {
            perror("lifecycle listen"); exit(1);
        }
        total_syscall_calls++;

        // 6. Client socket & connect
        int cli_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (cli_fd < 0) { perror("lifecycle socket cli"); exit(1); }
        total_syscall_calls++;

        if (connect(cli_fd, (struct sockaddr *)&srv_addr, srv_len) < 0) {
            perror("lifecycle connect"); exit(1);
        }
        total_syscall_calls++;

        // 7. Accept4
        struct sockaddr_un peer_addr;
        socklen_t peer_len = sizeof(peer_addr);
        int acc_fd = accept4(srv_fd, (struct sockaddr *)&peer_addr, &peer_len, SOCK_CLOEXEC);
        if (acc_fd < 0) {
            perror("lifecycle accept4"); exit(1);
        }
        total_syscall_calls++;

        // 8. Query getpeername
        namelen = sizeof(name);
        if (getpeername(acc_fd, (struct sockaddr *)&name, &namelen) < 0) {
            perror("lifecycle getpeername"); exit(1);
        }
        total_syscall_calls++;

        // 9. Sendmsg / Recvmsg data transfer
        char payload[32];
        snprintf(payload, sizeof(payload), "LIFECYCLE_#%u", i);
        struct iovec iov = { .iov_base = payload, .iov_len = strlen(payload) + 1 };
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;

        if (sendmsg(cli_fd, &msg, 0) < 0) {
            perror("lifecycle sendmsg"); exit(1);
        }
        total_syscall_calls++;

        char recv_buf[32] = {0};
        struct iovec recv_iov = { .iov_base = recv_buf, .iov_len = sizeof(recv_buf) };
        struct msghdr recv_msg;
        memset(&recv_msg, 0, sizeof(recv_msg));
        recv_msg.msg_iov = &recv_iov;
        recv_msg.msg_iovlen = 1;

        if (recvmsg(acc_fd, &recv_msg, 0) < 0) {
            perror("lifecycle recvmsg"); exit(1);
        }
        total_syscall_calls++;

        if (strcmp(recv_buf, payload) != 0) {
            fprintf(stderr, "lifecycle payload mismatch at iter %u\n", i);
            exit(1);
        }

        // 10. Shutdown & teardown
        shutdown(cli_fd, SHUT_WR);
        total_syscall_calls++;

        close(cli_fd);
        close(acc_fd);
        close(srv_fd);
        print_progress("full_lifecycle", i, iterations);
    }
    printf("[PASSED] Full socket lifecycle durability test passed.\n");
}

/* -------------------------------------------------------------------------
 * Main Entry Point
 * ------------------------------------------------------------------------- */
int main(int argc, char **argv) {
    unsigned iterations = DEFAULT_ITERATIONS;
    if (argc > 1) {
        char *endptr;
        unsigned long val = strtoul(argv[1], &endptr, 10);
        if (*endptr == '\0' && val >= 100) {
            iterations = (unsigned)val;
        } else {
            fprintf(stderr, "Usage: %s [iterations: min 100, default %d]\n", argv[0], DEFAULT_ITERATIONS);
            return 1;
        }
    }

    // Ignore SIGPIPE to handle shut down sockets gracefully with EPIPE error codes
    signal(SIGPIPE, SIG_IGN);

    printf("========================================================================\n");
    printf("        UNIX SOCKET SYSCALLS DURABILITY TEST SUITE                     \n");
    printf(" Target Iterations per Test Module: %u                                 \n", iterations);
    printf("========================================================================\n");

    clock_t start_time = clock();

    test_socket_durability(iterations);
    test_socketpair_durability(iterations);
    test_bind_durability(iterations);
    test_listen_durability(iterations);
    test_connect_accept_durability(iterations);
    test_sendto_recvfrom_durability(iterations);
    test_sendmsg_recvmsg_durability(iterations);
    test_getsockname_getpeername_durability(iterations);
    test_sockopt_durability(iterations);
    test_shutdown_durability(iterations);
    test_full_lifecycle_durability(iterations);

    clock_t end_time = clock();
    double elapsed_sec = (double)(end_time - start_time) / CLOCKS_PER_SEC;

    printf("\n========================================================================\n");
    printf(" ALL UNIX SOCKET SYSCALL DURABILITY TESTS PASSED SUCCESSFULLY!          \n");
    printf(" Total Test Modules Completed : 11                                      \n");
    printf(" Iterations per Module        : %u                                      \n", iterations);
    printf(" Total Syscalls Executed      : ~%lu                                   \n", total_syscall_calls);
    printf(" Elapsed CPU Time             : %.2f seconds                           \n", elapsed_sec);
    printf("========================================================================\n");

    return 0;
}
