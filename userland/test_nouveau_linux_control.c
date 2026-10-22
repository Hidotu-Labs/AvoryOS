/* PID 1 for an isolated Linux/Nouveau control VM, never a host executable.
 * No shell, disks, networking, or installed host configuration is needed. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/reboot.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/klog.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

static int finish(int failed) {
    /* Keep kernel evidence even if its console log level suppresses output. */
    char *log = malloc(1024 * 1024);
    if (log) {
        int n = klogctl(3, log, 1024 * 1024);
        puts("CONTROL kernel log begin");
        if (n > 0) fwrite(log, 1, n, stdout);
        else perror("kernel log");
        puts("CONTROL kernel log end");
        free(log);
    }
    printf("NOUVEAU_LINUX_CONTROL: END status=%s\n", failed ? "fail" : "pass");
    fflush(stdout);
    sync();
    reboot(RB_POWER_OFF);
    for (;;) pause();
    return failed;
}

int main(void) {
    if (getpid() != 1) {
        fprintf(stderr, "This program only runs as VM init.\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    if (mount("proc", "/proc", "proc", 0, NULL) ||
        mount("sysfs", "/sys", "sysfs", 0, NULL) ||
        mount("devtmpfs", "/dev", "devtmpfs", 0, NULL)) {
        perror("mount");
        return finish(1);
    }
    int console = open("/dev/console", O_RDWR);
    if (console >= 0) {
        dup2(console, 0); dup2(console, 1); dup2(console, 2);
        if (console > 2) close(console);
    }
    char firmware_version[64] = "unknown";
    FILE *version_file = fopen("/firmware-version", "r");
    if (version_file) {
        if (!fgets(firmware_version, sizeof(firmware_version), version_file))
            strcpy(firmware_version, "unknown");
        fclose(version_file);
    }
    firmware_version[strcspn(firmware_version, "\n")] = 0;
    printf("NOUVEAU_LINUX_CONTROL: START firmware=%s\n", firmware_version);
    struct utsname os;
    if (!uname(&os)) printf("CONTROL kernel=%s %s\n", os.sysname, os.release);
    DIR *devices = opendir("/sys/bus/pci/devices");
    if (devices) {
        struct dirent *entry;
        while ((entry = readdir(devices))) {
            if (entry->d_name[0] != '.') printf("CONTROL PCI device=%s\n", entry->d_name);
        }
        closedir(devices);
    }
    FILE *list = fopen("/modules.txt", "r");
    if (!list) { perror("modules list"); return finish(1); }
    char path[256];
    while (fgets(path, sizeof(path), list)) {
        path[strcspn(path, "\n")] = 0;
        int fd = open(path, O_RDONLY);
        if (fd < 0) { perror(path); fclose(list); return finish(1); }
        const char *args = strstr(path, "nouveau.ko") ?
            "modeset=1 runpm=0 config=NvGspRm=1 debug=info,gsp=debug" : "";
        int rc = syscall(SYS_finit_module, fd, args, 0);
        int saved = errno;
        close(fd);
        printf("CONTROL module=%s result=%d errno=%d\n", path, rc, rc ? saved : 0);
        if (rc && saved != EEXIST) { fclose(list); return finish(1); }
    }
    fclose(list);
    const char *pci = "/sys/bus/pci/devices/0000:00:03.0";
    char filename[512], buf[512];
    snprintf(filename, sizeof(filename), "%s/vendor", pci);
    FILE *vendor = fopen(filename, "r");
    if (!vendor) {
        FILE *cmd = fopen("/proc/cmdline", "r");
        int software = cmd && fgets(buf, sizeof(buf), cmd) && strstr(buf, "control_software=1");
        if (cmd) fclose(cmd);
        puts("CONTROL no GPU at reviewed guest slot");
        return finish(!software);
    }
    int nvidia = fgets(buf, sizeof(buf), vendor) && !strncmp(buf, "0x10de", 6);
    fclose(vendor);
    if (!nvidia) { puts("CONTROL unexpected PCI vendor"); return finish(1); }
    snprintf(filename, sizeof(filename), "%s/driver", pci);
    ssize_t n = readlink(filename, buf, sizeof(buf) - 1);
    if (n < 0) { puts("CONTROL NVIDIA probe unbound"); return finish(1); }
    buf[n] = 0;
    printf("CONTROL NVIDIA driver=%s\n", buf);
    if (!strstr(buf, "/nouveau")) return finish(1);
    snprintf(filename, sizeof(filename), "%s/drm", pci);
    DIR *drm = opendir(filename);
    unsigned cards = 0;
    if (drm) {
        struct dirent *entry;
        while ((entry = readdir(drm))) {
            if (!strncmp(entry->d_name, "card", 4)) cards++;
            if (entry->d_name[0] != '.') printf("CONTROL DRM node=%s\n", entry->d_name);
        }
        closedir(drm);
    }
    if (!cards) { puts("CONTROL no NVIDIA DRM card"); return finish(1); }
    puts("CONTROL initialized; removing Nouveau before poweroff");
    if (syscall(SYS_delete_module, "nouveau", O_NONBLOCK)) {
        perror("nouveau removal");
        return finish(1);
    }
    puts("CONTROL Nouveau removal completed");
    return finish(0);
}
