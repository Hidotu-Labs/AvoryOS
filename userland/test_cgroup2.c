#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int write_str(const char *path, const char *s) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t n = write(fd, s, strlen(s));
    close(fd);
    return n == (ssize_t)strlen(s) ? 0 : -1;
}

static int read_str(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return 0;
}

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int main(void) {
    char buf[512];
    char p1_path[256], p2_path[512], p3_path[512];
    const char *cg_root = "/sys/fs/cgroup";
    bool is_pid1 = (getpid() == 1);

    if (!file_exists("/proc/pressure/cpu")) {
        mkdir("/proc", 0755);
        mount("proc", "/proc", "proc", 0, NULL);
    }

    if (file_exists("/sys/fs/cgroup/unified/cgroup.controllers")) {
        cg_root = "/sys/fs/cgroup/unified";
    } else if (!file_exists("/sys/fs/cgroup/cgroup.controllers")) {
        mkdir("/sys", 0755);
        mkdir("/sys/fs", 0755);
        mkdir("/sys/fs/cgroup", 0755);
        if (mount("cgroup2", "/sys/fs/cgroup", "cgroup2", 0, NULL) != 0) {
            perror("mount cgroup2");
            goto fail;
        }
    }
    printf("Using cgroup2 root: %s\n", cg_root);

    /* 1. PSI */
    if (read_str("/proc/pressure/cpu", buf, sizeof(buf)) != 0 ||
        !strstr(buf, "some avg10=")) {
        printf("FAIL: /proc/pressure/cpu: %s\n", buf);
        goto fail;
    }
    printf("PSI cpu: %s", buf);

    if (read_str("/proc/pressure/memory", buf, sizeof(buf)) != 0 ||
        !strstr(buf, "full avg10=")) {
        printf("FAIL: /proc/pressure/memory: %s\n", buf);
        goto fail;
    }
    printf("PSI mem: %s", buf);

    /* 2. Controllers */
    snprintf(p1_path, sizeof(p1_path), "%s/cg_test", cg_root);
    rmdir(p1_path);
    if (mkdir(p1_path, 0755) != 0) {
        perror("mkdir cg_test");
        goto fail;
    }
    snprintf(p2_path, sizeof(p2_path), "%s/cgroup.controllers", p1_path);
    if (read_str(p2_path, buf, sizeof(buf)) != 0 ||
        !strstr(buf, "cpu") || !strstr(buf, "memory") || !strstr(buf, "pids")) {
        printf("FAIL: controllers=%s\n", buf);
        goto fail;
    }
    printf("cgroup.controllers: PASS\n");

    /* 3. pids.max = 1 */
    snprintf(p2_path, sizeof(p2_path), "%s/pids.max", p1_path);
    if (write_str(p2_path, "1\n") != 0) {
        perror("write pids.max");
        goto fail;
    }
    snprintf(p2_path, sizeof(p2_path), "%s/cgroup.procs", p1_path);
    pid_t pid = fork();
    if (pid == 0) {
        if (write_str(p2_path, "0\n") != 0) _exit(2);
        pid_t child = fork();
        if (child == 0) _exit(0);
        if (child < 0 && errno == EAGAIN) _exit(0);
        _exit(3);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        printf("FAIL: pids.max status=0x%x\n", st);
        goto fail;
    }
    snprintf(p2_path, sizeof(p2_path), "%s/pids.events", p1_path);
    if (read_str(p2_path, buf, sizeof(buf)) != 0 || !strstr(buf, "max 1")) {
        printf("FAIL: pids.events=%s\n", buf);
        goto fail;
    }
    printf("pids.max enforcement: PASS\n");

    /* 4. cpu.weight & cpu.max */
    snprintf(p2_path, sizeof(p2_path), "%s/cpu.weight", p1_path);
    if (write_str(p2_path, "250\n") != 0 ||
        read_str(p2_path, buf, sizeof(buf)) != 0 ||
        strncmp(buf, "250", 3) != 0) {
        printf("FAIL: cpu.weight=%s\n", buf);
        goto fail;
    }
    snprintf(p2_path, sizeof(p2_path), "%s/cpu.max", p1_path);
    if (write_str(p2_path, "20000 100000\n") != 0 ||
        read_str(p2_path, buf, sizeof(buf)) != 0 ||
        strncmp(buf, "20000 100000", 12) != 0) {
        printf("FAIL: cpu.max=%s\n", buf);
        goto fail;
    }
    printf("cpu.weight & cpu.max config: PASS\n");
    rmdir(p1_path);

    /* 5. memory.max = 2 MiB OOM killer */
    snprintf(p1_path, sizeof(p1_path), "%s/cg_oom", cg_root);
    rmdir(p1_path);
    if (mkdir(p1_path, 0755) != 0) {
        perror("mkdir cg_oom");
        goto fail;
    }
    snprintf(p2_path, sizeof(p2_path), "%s/memory.max", p1_path);
    if (write_str(p2_path, "2097152\n") != 0) {
        perror("write memory.max");
        goto fail;
    }
    snprintf(p2_path, sizeof(p2_path), "%s/cgroup.procs", p1_path);
    pid_t poom = fork();
    if (poom == 0) {
        write_str(p2_path, "0\n");
        size_t sz = 8 * 1024 * 1024;
        volatile char *p = malloc(sz);
        if (!p) _exit(1);
        for (size_t off = 0; off < sz; off += 4096)
            p[off] = (char)(off >> 12);
        _exit(0);
    }
    waitpid(poom, &st, 0);
    if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGKILL) {
        printf("FAIL: memory.max OOM expected SIGKILL, got st=0x%x\n", st);
        goto fail;
    }
    snprintf(p2_path, sizeof(p2_path), "%s/memory.events", p1_path);
    if (read_str(p2_path, buf, sizeof(buf)) != 0 || !strstr(buf, "oom_kill 1")) {
        printf("FAIL: memory.events=%s\n", buf);
        goto fail;
    }
    printf("memory.max OOM killer: PASS\n");
    rmdir(p1_path);

    /* 6. cgroup.freeze & cgroup.kill */
    snprintf(p1_path, sizeof(p1_path), "%s/cg_kill", cg_root);
    rmdir(p1_path);
    if (mkdir(p1_path, 0755) != 0) {
        perror("mkdir cg_kill");
        goto fail;
    }
    snprintf(p2_path, sizeof(p2_path), "%s/cgroup.procs", p1_path);
    pid_t pk = fork();
    if (pk == 0) {
        write_str(p2_path, "0\n");
        for (;;) pause();
    }
    usleep(50000);
    snprintf(p2_path, sizeof(p2_path), "%s/cgroup.freeze", p1_path);
    snprintf(p3_path, sizeof(p3_path), "%s/cgroup.events", p1_path);
    write_str(p2_path, "1\n");
    if (read_str(p3_path, buf, sizeof(buf)) != 0 || !strstr(buf, "frozen 1")) {
        printf("FAIL: cgroup.freeze=1 events=%s\n", buf);
        goto fail;
    }
    write_str(p2_path, "0\n");
    snprintf(p2_path, sizeof(p2_path), "%s/cgroup.kill", p1_path);
    write_str(p2_path, "1\n");
    waitpid(pk, &st, 0);
    if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGKILL) {
        printf("FAIL: cgroup.kill expected SIGKILL, got st=0x%x\n", st);
        goto fail;
    }
    printf("cgroup.freeze & cgroup.kill: PASS\n");
    rmdir(p1_path);

    puts("CGROUP2_SMOKE: PASS");
    fflush(stdout);
    if (is_pid1)
        reboot(RB_POWER_OFF);
    return 0;

fail:
    puts("CGROUP2_SMOKE: FAIL");
    fflush(stdout);
    if (is_pid1)
        reboot(RB_POWER_OFF);
    return 1;
}
