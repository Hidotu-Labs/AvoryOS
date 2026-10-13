#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

struct cap_header { uint32_t version; int pid; };
struct cap_data { uint32_t effective, permitted, inheritable; };
static int failures;
static FILE *test_log;

static void check(int ok, const char *message) {
    printf("[%s] %s\n", ok ? "OK" : "FAIL", message);
    if (test_log) { fprintf(test_log, "[%s] %s\n", ok ? "OK" : "FAIL", message); fflush(test_log); }
    if (!ok) failures++;
}

static void test_capget(void) {
    struct cap_header hdr = {0};
    struct cap_data data[2];
    check(syscall(SYS_capget, &hdr, NULL) == 0 && hdr.version == 0x20080522,
          "capget NULL-data ABI discovery");
    hdr.version = 0;
    errno = 0;
    check(syscall(SYS_capget, &hdr, data) == -1 && errno == EINVAL &&
          hdr.version == 0x20080522, "capget invalid version with data");
    hdr.version = 0x19980330;
    memset(data, 0x5a, sizeof(data));
    check(syscall(SYS_capget, &hdr, data) == 0 && hdr.version == 0x19980330 &&
          data[1].effective == 0x5a5a5a5a, "capget v1 preserves ABI and writes one word");
    hdr.version = 0x20080522;
    hdr.pid = -1;
    errno = 0;
    check(syscall(SYS_capget, &hdr, data) == -1 && errno == EINVAL,
          "capget rejects negative PID");
    hdr.pid = INT_MAX;
    errno = 0;
    check(syscall(SYS_capget, &hdr, data) == -1 && errno == ESRCH,
          "capget rejects missing PID");
    hdr.pid = 0;
    check(syscall(SYS_capget, &hdr, data) == 0 &&
          ((data[1].effective | data[1].permitted) & ~0x1ffu) == 0,
          "capget excludes undefined capabilities");
    struct cap_header *ro = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ro == MAP_FAILED) { check(0, "capget test mapping"); return; }
    *ro = hdr;
    mprotect(ro, 4096, PROT_READ);
    check(syscall(SYS_capget, ro, data) == 0,
          "capget accepts supported version in read-only header");
    mprotect(ro, 4096, PROT_READ | PROT_WRITE);
    ro->version = 0;
    mprotect(ro, 4096, PROT_READ);
    errno = 0;
    check(syscall(SYS_capget, ro, NULL) == -1 && errno == EFAULT,
          "capget cannot negotiate into read-only header");
    munmap(ro, 4096);
}

static void test_peer_groups(void) {
    if (geteuid() != 0) return;
    gid_t saved[64], groups[2] = {101, 102};
    int count = getgroups(64, saved);
    if (count < 0 || setgroups(2, groups) != 0) {
        check(0, "setgroups for socket test"); return;
    }
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        check(0, "socketpair"); setgroups(count, saved); return;
    }
    gid_t out[2] = {0};
    socklen_t len = sizeof(gid_t);
    errno = 0;
    check(getsockopt(pair[0], SOL_SOCKET, 59 /* SO_PEERGROUPS */, out, &len) == -1 &&
          errno == ERANGE && len == sizeof(out), "SO_PEERGROUPS preserves ERANGE and required size");
    len = sizeof(out);
    check(getsockopt(pair[0], SOL_SOCKET, 59, out, &len) == 0 &&
          len == sizeof(out) && out[0] == groups[0] && out[1] == groups[1],
          "SO_PEERGROUPS returns real groups");
    check(setgroups(0, NULL) == 0, "change groups after socketpair");
    len = sizeof(out);
    check(getsockopt(pair[0], SOL_SOCKET, 59, out, &len) == 0 &&
          len == sizeof(out) && out[0] == groups[0] && out[1] == groups[1],
          "SO_PEERGROUPS retains connection credential snapshot");
    close(pair[1]);
    len = sizeof(out);
    check(getsockopt(pair[0], SOL_SOCKET, 59, out, &len) == 0 && len == sizeof(out),
          "SO_PEERGROUPS retains credentials after peer close");
    close(pair[0]);
    setgroups(count, saved);
}

static void test_shm(void) {
    struct shm_info before, after;
    struct shminfo limits;
    struct shmid_ds ds;
    check(shmctl(0, IPC_INFO, (struct shmid_ds *)&limits) >= 0 &&
          limits.shmmax > 0 && limits.shmmni > 0, "IPC_INFO reports limits");
    int got_before = shmctl(0, SHM_INFO, (struct shmid_ds *)&before) >= 0;
    check(got_before, "SHM_INFO reports usage");
    int id = shmget(IPC_PRIVATE, 4097, IPC_CREAT | 0600);
    if (id < 0) { check(0, "shmget"); return; }
    void *mapping = shmat(id, NULL, 0);
    check(mapping != (void *)-1, "shmat");
    if (mapping != (void *)-1) {
        /* Linux allocates pages lazily, AvoryOS allocates at shmget. */
        ((volatile unsigned char *)mapping)[0] = 1;
        ((volatile unsigned char *)mapping)[4096] = 2;
    }
    check(shmctl(id, IPC_STAT, &ds) == 0 && ds.shm_segsz == 4097 &&
          ds.shm_perm.cuid == geteuid(), "IPC_STAT reports original size and creator");
    int highest = shmctl(0, SHM_INFO, (struct shmid_ds *)&after);
    check(highest >= 0 && got_before && after.used_ids == before.used_ids + 1 &&
          after.shm_tot == before.shm_tot + 2 && after.shm_rss == before.shm_rss + 2,
          "SHM_INFO tracks resident pages");
    int found = 0;
    for (int i = 0; i <= highest; i++) {
        if (shmctl(i, SHM_STAT_ANY, &ds) == id) { found = 1; break; }
    }
    check(found, "SHM_STAT_ANY enumerates by slot and returns segment ID");
    if (found) {
        ds.shm_perm.mode = 0640;
        if (geteuid() == 0) { ds.shm_perm.uid = 123; ds.shm_perm.gid = 456; }
        check(shmctl(id, IPC_SET, &ds) == 0 && shmctl(id, IPC_STAT, &ds) == 0 &&
              (ds.shm_perm.mode & 0777) == 0640 && ds.shm_perm.cuid == geteuid() &&
              (geteuid() != 0 || (ds.shm_perm.uid == 123 && ds.shm_perm.gid == 456)),
              "IPC_SET updates ownership and permissions, preserves creator");
    }
    errno = 0;
    check(syscall(SYS_shmctl, id, IPC_STAT, (void *)1) == -1 && errno == EFAULT,
          "IPC_STAT invalid buffer returns EFAULT");
    errno = 0;
    check(syscall(SYS_shmctl, id, IPC_SET, (void *)1) == -1 && errno == EFAULT,
          "IPC_SET invalid buffer returns EFAULT");
    check(shmctl(id, IPC_RMID, NULL) == 0, "IPC_RMID");
    if (mapping != (void *)-1) {
        check(shmctl(id, IPC_STAT, &ds) == 0 && (ds.shm_perm.mode & 01000) &&
              ds.shm_nattch == 1, "IPC_RMID defers destruction until detach");
        check(shmdt(mapping) == 0, "shmdt releases removed segment");
    }
    errno = 0;
    check(shmctl(id, IPC_STAT, &ds) == -1 && (errno == EINVAL || errno == EIDRM),
          "destroyed segment is unavailable");
}

static void test_nonroot(void) {
    if (geteuid() != 0) return;
    int id = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
    if (id < 0) { check(0, "permission test segment"); return; }
    pid_t pid = fork();
    if (pid < 0) { check(0, "permission test fork"); shmctl(id, IPC_RMID, NULL); return; }
    if (pid == 0) {
        if (setgid(65534) || setuid(65534)) _exit(1);
        struct cap_header hdr = {0x20080522, 0};
        struct cap_data data[2];
        if (syscall(SYS_capget, &hdr, data) || data[0].effective || data[1].effective)
            _exit(2);
        struct shmid_ds ds = {0};
        errno = 0;
        if (shmctl(id, IPC_STAT, &ds) != -1 || errno != EACCES) _exit(3);
        errno = 0;
        if (shmctl(id, IPC_SET, &ds) != -1 || errno != EPERM) _exit(4);
        _exit(0);
    }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    check(pid > 0 && waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "non-root capability and SHM permission checks");
    shmctl(id, IPC_RMID, NULL);
}

static void test_sysfs(void) {
    /* AvoryOS exposes this compatibility switch; Linux need not expose it. */
    FILE *enable = fopen("/sys/module/user_namespace/parameters/enable", "r");
    if (!enable) return;
    check(fgetc(enable) == 'N', "sysfs reports unavailable user namespaces");
    fclose(enable);
    char value[64] = {0};
    FILE *online = fopen("/sys/devices/system/cpu/online", "r");
    check(online && fgets(value, sizeof(value), online) && value[0] == '0',
          "CPU online sysfs node is readable");
    if (online) fclose(online);
    FILE *capacity = fopen("/sys/devices/system/cpu/cpu0/cpu_capacity", "r");
    check(capacity && fgets(value, sizeof(value), capacity) && !strcmp(value, "1024\n"),
          "CPU capacity sysfs node is readable");
    if (capacity) fclose(capacity);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_log = fopen("/dev/kmsg", "w");
    test_capget(); test_peer_groups(); test_shm(); test_nonroot(); test_sysfs();
    const char *message = failures ? "SYSCOMPAT: FAIL\n" : "SYSCOMPAT: PASS\n";
    fputs(message, stdout);
    if (test_log) { fputs(message, test_log); fclose(test_log); }
    return failures ? 1 : 0;
}
