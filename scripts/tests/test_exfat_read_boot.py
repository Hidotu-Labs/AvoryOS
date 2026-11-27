"""Exercise exFAT mounts, data I/O, allocation and directory mutations in QEMU."""

import struct
import subprocess

from boot_probe import ROOT, cmd_run, run_probe


CLUSTER_SIZE = 4096
TEST_NAME = 'exfat-read'

INIT = r"""
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CLUSTER_SIZE 4096
#ifndef FALLOC_FL_KEEP_SIZE
#define FALLOC_FL_KEEP_SIZE 1
#endif

static int mount_exfat(unsigned long flags, const char *options) {
    if (mount("/dev/nvme0n1", "/mnt", "exfat", flags, options) == 0)
        return 0;
    return mount("/dev/nvme1n1", "/mnt", "exfat", flags, options);
}

static int exfat_volume_dirty(void) {
    for (int i = 0; i < 2; i++) {
        const char *name = i ? "/dev/nvme1n1" : "/dev/nvme0n1";
        int disk = open(name, O_RDONLY);
        if (disk < 0)
            continue;
        unsigned char boot[512];
        ssize_t size = pread(disk, boot, sizeof(boot), 0);
        close(disk);
        if (size == sizeof(boot) && memcmp(boot + 3, "EXFAT   ", 8) == 0)
            return !!(boot[106] & 2);
    }
    return -1;
}

static int write_alternating_bytes(int fd, int start, const char *byte) {
    for (int i = start; i < 32; i += 2) {
        if (pwrite(fd, byte, 1, i) != 1) {
            fprintf(stderr, "exFAT concurrent pwrite at %d failed (errno %d)\n", i, errno);
            return -1;
        }
    }
    return 0;
}

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "exFAT read/write check failed at line %d: %s (errno %d)\n", \
                __LINE__, #condition, errno); \
        return -1; \
    } \
} while (0)

static int test_writable(void) {
    const char *original = "/mnt/Source/caf\xc3\xa9-\xf0\x9f\xa7\xaa.bin";
    const char *uppercase = "/mnt/source/CAF\xc3\x89-\xf0\x9f\xa7\xaa.BIN";
    const char *dest = "/mnt/Target/renamed.bin";
    struct stat st;
    struct statfs before, after;
    char bytes[16];
    const char zeros[sizeof(bytes)] = {0};

    REQUIRE(mount_exfat(0, NULL) == 0);
    REQUIRE(statfs("/mnt", &before) == 0 && !(before.f_flags & 1));
    REQUIRE(mkdir("/mnt/Source", 0755) == 0);
    REQUIRE(mkdir("/mnt/Target", 0755) == 0);
    REQUIRE(mkdir("/mnt/Source/Child", 0755) == 0);
    REQUIRE(rename("/mnt/Source", "/mnt/Source/Child/Cycle") == -1 && errno == EINVAL);
    REQUIRE(rmdir("/mnt/Source") == -1 && errno == ENOTEMPTY);
    int fd = open(original, O_CREAT | O_EXCL | O_RDWR, 0644);
    REQUIRE(fd >= 0 && fstat(fd, &st) == 0 && st.st_size == 0);
    REQUIRE(open(uppercase, O_CREAT | O_EXCL | O_RDWR, 0644) == -1 && errno == EEXIST);
    int view = open(uppercase, O_RDONLY);
    REQUIRE(view >= 0);
    REQUIRE(pwrite(fd, "end", 3, 2 * CLUSTER_SIZE + 7) == 3);
    REQUIRE(fstat(view, &st) == 0 && st.st_size == 2 * CLUSTER_SIZE + 10);
    REQUIRE(pread(view, bytes, sizeof(bytes), CLUSTER_SIZE - 8) == sizeof(bytes));
    REQUIRE(memcmp(bytes, zeros, sizeof(bytes)) == 0);
    REQUIRE(pread(view, bytes, 3, 2 * CLUSTER_SIZE + 7) == 3 && !memcmp(bytes, "end", 3));
    REQUIRE(pwrite(fd, "head", 4, 0) == 4);
    char *mapped = mmap(NULL, CLUSTER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    REQUIRE(mapped != MAP_FAILED && !memcmp(mapped, "head", 4));
    memcpy(mapped + 8, "map!", 4);
    REQUIRE(msync(mapped, CLUSTER_SIZE, MS_SYNC) == 0);
    REQUIRE(pread(view, bytes, 4, 8) == 4 && !memcmp(bytes, "map!", 4));
    REQUIRE(pwrite(fd, "sync", 4, 8) == 4 && !memcmp(mapped + 8, "sync", 4));
    REQUIRE(munmap(mapped, CLUSTER_SIZE) == 0);

    REQUIRE(ftruncate(fd, CLUSTER_SIZE + 3) == 0);
    REQUIRE(ftruncate(fd, 3 * CLUSTER_SIZE) == 0);
    REQUIRE(pread(view, bytes, sizeof(bytes), 2 * CLUSTER_SIZE + 7) == sizeof(bytes));
    REQUIRE(!memcmp(bytes, zeros, sizeof(bytes)));
    int app = open(original, O_WRONLY | O_APPEND);
    REQUIRE(app >= 0 && write(app, "tail", 4) == 4 && close(app) == 0);
    REQUIRE(fstat(view, &st) == 0 && st.st_size == 3 * CLUSTER_SIZE + 4);

    /* Dedicated persistence checks for mmap + msync, munmap dirty writeback,
     * mremap(MREMAP_MAYMOVE) on shared and private file mappings, fallocate,
     * chmod (EXFAT_ATTR_READONLY), and utimensat. */
    const char *mp_path = "/mnt/Target/mmap_persist.bin";
    int mfd = open(mp_path, O_CREAT | O_EXCL | O_RDWR, 0644);
    REQUIRE(mfd >= 0);
    REQUIRE(fallocate(mfd, 0, 0, 2 * CLUSTER_SIZE) == 0);
    REQUIRE(fallocate(mfd, FALLOC_FL_KEEP_SIZE, 0, 4 * CLUSTER_SIZE) == -1 && errno == EOPNOTSUPP);
    REQUIRE(fstat(mfd, &st) == 0 && st.st_size == 2 * CLUSTER_SIZE);
    REQUIRE(pwrite(mfd, "base", 4, 0) == 4);
    REQUIRE(pwrite(mfd, "page2", 5, CLUSTER_SIZE + 4) == 5);

    char *guard = mmap(NULL, 2 * CLUSTER_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    REQUIRE(guard != MAP_FAILED);
    REQUIRE(munmap(guard, CLUSTER_SIZE) == 0);
    /* Place a 1-page shared mapping directly before guard+CLUSTER_SIZE to force move. */
    char *sh = mmap(guard, CLUSTER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, mfd, 0);
    REQUIRE(sh == guard);
    memcpy(sh + 8, "map!", 4);
    REQUIRE(msync(sh, CLUSTER_SIZE, MS_SYNC) == 0);
    char *moved = mremap(sh, CLUSTER_SIZE, 2 * CLUSTER_SIZE, MREMAP_MAYMOVE);
    REQUIRE(moved != MAP_FAILED && moved != sh);
    REQUIRE(!memcmp(moved, "base", 4) && !memcmp(moved + 8, "map!", 4));
    REQUIRE(!memcmp(moved + CLUSTER_SIZE + 4, "page2", 5));
    memcpy(moved + 16, "munm", 4);
    memcpy(moved + CLUSTER_SIZE + 16, "mrp!", 4);
    REQUIRE(munmap(moved, 2 * CLUSTER_SIZE) == 0);
    REQUIRE(munmap(guard + CLUSTER_SIZE, CLUSTER_SIZE) == 0);

    char *pv = mmap(NULL, 2 * CLUSTER_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, mfd, 0);
    REQUIRE(pv != MAP_FAILED);
    memcpy(pv, "PRIV", 4);
    REQUIRE(munmap(pv + CLUSTER_SIZE, CLUSTER_SIZE) == 0);
    char *pv_guard = mmap(pv + CLUSTER_SIZE, CLUSTER_SIZE, PROT_READ,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    REQUIRE(pv_guard == pv + CLUSTER_SIZE);
    char *pv_moved = mremap(pv, CLUSTER_SIZE, 2 * CLUSTER_SIZE, MREMAP_MAYMOVE);
    REQUIRE(pv_moved != MAP_FAILED && pv_moved != pv);
    REQUIRE(!memcmp(pv_moved, "PRIV", 4));
    REQUIRE(!memcmp(pv_moved + CLUSTER_SIZE + 4, "page2", 5));
    REQUIRE(munmap(pv_moved, 2 * CLUSTER_SIZE) == 0);
    REQUIRE(munmap(pv_guard, CLUSTER_SIZE) == 0);
    REQUIRE(close(mfd) == 0);

    int view_mp = open(mp_path, O_RDONLY);
    REQUIRE(view_mp >= 0);
    REQUIRE(chmod(mp_path, 0444) == 0);
    REQUIRE(fstat(view_mp, &st) == 0 && (st.st_mode & 0222) == 0);
    REQUIRE(open(mp_path, O_WRONLY) == -1 && (errno == EROFS || errno == EACCES));
    REQUIRE(chmod(mp_path, 0644) == 0);
    REQUIRE(fstat(view_mp, &st) == 0 && (st.st_mode & 0222) != 0);
    REQUIRE(close(view_mp) == 0);
    mfd = open(mp_path, O_RDWR);
    REQUIRE(mfd >= 0 && close(mfd) == 0);

    int ro_keep = open("/mnt/Target/ro_keep.bin", O_CREAT | O_EXCL | O_RDWR, 0644);
    REQUIRE(ro_keep >= 0 && write(ro_keep, "ro", 2) == 2 && close(ro_keep) == 0);
    REQUIRE(chmod("/mnt/Target/ro_keep.bin", 0444) == 0);

    const struct timespec ts[2] = {{1700000002, 0}, {1700000001, 0}};
    REQUIRE(utimensat(AT_FDCWD, mp_path, ts, 0) == 0);

    /* Deep multi-level directory rename & cycle detection when intermediate
     * ancestors are closed (not pinned in mnt->nodes). */
    REQUIRE(mkdir("/mnt/Deep", 0755) == 0);
    REQUIRE(mkdir("/mnt/Deep/A", 0755) == 0);
    REQUIRE(mkdir("/mnt/Deep/A/B", 0755) == 0);
    REQUIRE(mkdir("/mnt/Deep/A/B/C", 0755) == 0);
    int deep_f = open("/mnt/Deep/A/B/C/leaf.txt", O_CREAT | O_EXCL | O_WRONLY, 0644);
    REQUIRE(deep_f >= 0 && write(deep_f, "deep", 4) == 4 && close(deep_f) == 0);
    REQUIRE(mkdir("/mnt/Deep/Dst", 0755) == 0);
    REQUIRE(mkdir("/mnt/Deep/Dst/X", 0755) == 0);
    REQUIRE(mkdir("/mnt/Deep/Dst/X/Y", 0755) == 0);
    REQUIRE(rename("/mnt/Deep/A/B/C", "/mnt/Deep/Dst/X/Y/C") == 0);
    REQUIRE(rename("/mnt/Deep/Dst", "/mnt/Deep/Dst/X/Y/C/BadCycle") == -1 && errno == EINVAL);

    REQUIRE(mkdir("/mnt/Target/GoneDir", 0755) == 0);
    int gone_dir_fd = open("/mnt/Target/GoneDir", O_RDONLY | O_DIRECTORY);
    REQUIRE(gone_dir_fd >= 0 && rmdir("/mnt/Target/GoneDir") == 0);
    DIR *gd = fdopendir(gone_dir_fd);
    REQUIRE(gd != NULL && readdir(gd) == NULL && closedir(gd) == 0);

    int replaced = open(dest, O_CREAT | O_EXCL | O_RDWR, 0644);
    REQUIRE(replaced >= 0 && write(replaced, "old", 3) == 3);
    REQUIRE(rename(original, dest) == 0 && access(original, F_OK) == -1 && errno == ENOENT);
    REQUIRE(pread(replaced, bytes, 3, 0) == 3 && !memcmp(bytes, "old", 3));
    REQUIRE(close(replaced) == 0);
    REQUIRE(pwrite(fd, "HEAD", 4, 0) == 4);
    REQUIRE(rename(dest, "/mnt/Target/RENAMED.BIN") == 0);
    REQUIRE(rename("/mnt/Source/Child", "/mnt/Target/Child") == 0);
    REQUIRE(rmdir("/mnt/Target/Child") == 0 && rmdir("/mnt/Source") == 0);
    REQUIRE(close(view) == 0 && close(fd) == 0);

    for (int i = 0; i < 48; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/mnt/Target/entry-%02d.txt", i);
        int item = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
        REQUIRE(item >= 0 && close(item) == 0);
    }
    REQUIRE(access("/mnt/target/ENTRY-47.TXT", F_OK) == 0);
    DIR *td = opendir("/mnt/Target");
    REQUIRE(td != NULL);
    int entry_seen = 0;
    struct dirent *de;
    while ((de = readdir(td)) != NULL) {
        if (!strcmp(de->d_name, "entry-47.txt") || !strcmp(de->d_name, "entry-00.txt"))
            entry_seen++;
    }
    REQUIRE(entry_seen == 2 && closedir(td) == 0);

    int probe = open("/mnt/Target/enospc", O_CREAT | O_EXCL | O_RDWR, 0644);
    REQUIRE(probe >= 0 && statfs("/mnt", &before) == 0);
    REQUIRE(ftruncate(probe, (off_t)before.f_blocks * before.f_bsize) == -1 && errno == ENOSPC);
    REQUIRE(fstat(probe, &st) == 0 && st.st_size == 0);
    REQUIRE(statfs("/mnt", &after) == 0 && before.f_bfree == after.f_bfree);
    REQUIRE(write(probe, "orphan", 6) == 6 && statfs("/mnt", &before) == 0);
    REQUIRE(unlink("/mnt/Target/enospc") == 0);
    REQUIRE(exfat_volume_dirty() == 1);
    REQUIRE(access("/mnt/Target/enospc", F_OK) == -1 && errno == ENOENT);
    REQUIRE(pread(probe, bytes, 6, 0) == 6 && !memcmp(bytes, "orphan", 6));
    REQUIRE(pwrite(probe, "!", 1, 6) == 1);
    REQUIRE(statfs("/mnt", &after) == 0 && before.f_bfree == after.f_bfree);
    REQUIRE(close(probe) == 0);
    REQUIRE(exfat_volume_dirty() == 0);
    REQUIRE(statfs("/mnt", &after) == 0 && after.f_bfree == before.f_bfree + 1);

    REQUIRE(umount("/mnt") == 0 && mount_exfat(MS_RDONLY, NULL) == 0);
    fd = open(dest, O_RDONLY);
    REQUIRE(fd >= 0 && pread(fd, bytes, 4, 0) == 4 && !memcmp(bytes, "HEAD", 4));
    REQUIRE(pread(fd, bytes, 4, 8) == 4 && !memcmp(bytes, "sync", 4));
    REQUIRE(pread(fd, bytes, sizeof(bytes), 3 * CLUSTER_SIZE) == 4 && !memcmp(bytes, "tail", 4));
    REQUIRE(close(fd) == 0 && access("/mnt/Target/entry-47.txt", F_OK) == 0);
    REQUIRE(stat(mp_path, &st) == 0 && st.st_size == 2 * CLUSTER_SIZE);
    REQUIRE(st.st_mtime == 1700000001 && st.st_atime == 1700000002);
    mfd = open(mp_path, O_RDONLY);
    REQUIRE(mfd >= 0);
    REQUIRE(pread(mfd, bytes, 4, 0) == 4 && !memcmp(bytes, "base", 4));
    REQUIRE(pread(mfd, bytes, 4, 8) == 4 && !memcmp(bytes, "map!", 4));
    REQUIRE(pread(mfd, bytes, 4, 16) == 4 && !memcmp(bytes, "munm", 4));
    REQUIRE(pread(mfd, bytes, 4, CLUSTER_SIZE + 16) == 4 && !memcmp(bytes, "mrp!", 4));
    REQUIRE(close(mfd) == 0);
    REQUIRE(stat("/mnt/Target/ro_keep.bin", &st) == 0 && (st.st_mode & 0222) == 0);
    deep_f = open("/mnt/Deep/Dst/X/Y/C/leaf.txt", O_RDONLY);
    REQUIRE(deep_f >= 0 && pread(deep_f, bytes, 4, 0) == 4 && !memcmp(bytes, "deep", 4) && close(deep_f) == 0);
    REQUIRE(open(dest, O_RDWR) == -1 && errno == EROFS && umount("/mnt") == 0);
    return 0;
}

int main(void) {
    struct stat st;
    unsigned char data[16];
    if (mount_exfat(MS_RDONLY, "overwrite") != 0) {
        perror("mount exfat");
        goto poweroff;
    }
    int fd = open("/mnt/tail.bin", O_RDONLY);
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size != 2 * CLUSTER_SIZE) {
        puts("exFAT file open or size check failed");
        goto poweroff;
    }
    const unsigned char expected[sizeof(data)] = "AAXYZ";
    if (pread(fd, data, sizeof(data), CLUSTER_SIZE - 2) != sizeof(data) ||
        memcmp(data, expected, sizeof(data)) != 0) {
        puts("exFAT boundary read returned incorrect data");
        goto poweroff;
    }
    if (pread(fd, data, sizeof(data), 2 * CLUSTER_SIZE - 4) != 4 ||
        memcmp(data, "\0\0\0\0", 4) != 0 ||
        pread(fd, data, sizeof(data), 2 * CLUSTER_SIZE) != 0) {
        puts("exFAT tail or EOF read returned incorrect data");
        goto poweroff;
    }
    close(fd);
    if (open("/mnt/tail.bin", O_RDWR) != -1 || errno != EROFS) {
        puts("exFAT read-only mount permitted a writable open");
        goto poweroff;
    }
    if (umount("/mnt") != 0) {
        perror("umount exfat");
        goto poweroff;
    }
    puts("EXFAT_READ: PASS");

    if (mount_exfat(0, "overwrite") != 0) {
        perror("mount exfat overwrite");
        goto poweroff;
    }
    fd = open("/mnt/tail.bin", O_RDWR);
    int observer = open("/mnt/tail.bin", O_RDONLY);
    if (fd < 0 || observer < 0) {
        perror("open exfat overwrite file");
        goto poweroff;
    }
    if (pread(observer, data, 5, CLUSTER_SIZE - 2) != 5 ||
        memcmp(data, "AAXYZ", 5) != 0 ||
        pwrite(fd, "abcde", 5, CLUSTER_SIZE - 2) != 5 ||
        pread(observer, data, sizeof(data), CLUSTER_SIZE - 4) != sizeof(data)) {
        puts("exFAT overwrite I/O failed");
        goto poweroff;
    }
    const unsigned char updated[sizeof(data)] = "AAabcde";
    if (memcmp(data, updated, sizeof(data)) != 0) {
        puts("exFAT overwrite changed neighboring bytes or returned stale data");
        goto poweroff;
    }
    if (pwrite(fd, "!!!", 3, CLUSTER_SIZE + 2) != -1 || errno != EOPNOTSUPP ||
        ftruncate(fd, 0) != -1 ||
        mmap(NULL, CLUSTER_SIZE, PROT_READ, MAP_SHARED, fd, 0) != MAP_FAILED ||
        errno != EOPNOTSUPP) {
        puts("exFAT overwrite accepted an unsupported operation");
        goto poweroff;
    }

    int fragmented = open("/mnt/fragment.bin", O_RDWR);
    if (fragmented < 0 ||
        pwrite(fragmented, "fghij", 5, CLUSTER_SIZE - 2) != 5 ||
        close(fragmented) != 0) {
        perror("overwrite exfat FAT chain");
        goto poweroff;
    }

    int result_pipe[2];
    if (pipe(result_pipe) != 0)
        goto poweroff;
    pid_t child = fork();
    if (child == 0) {
        close(result_pipe[0]);
        char result = write_alternating_bytes(fd, 1, "R") == 0 ? 'Y' : 'N';
        close(fd);
        close(observer);
        _exit(write(result_pipe[1], &result, 1) != 1);
    }
    close(result_pipe[1]);
    char result = 0;
    int parent_result = child < 0 ? -1 : write_alternating_bytes(fd, 0, "L");
    ssize_t received;
    do {
        received = read(result_pipe[0], &result, 1);
    } while (received < 0 && errno == EINTR);
    close(result_pipe[0]);
    if (child > 0)
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
    if (parent_result != 0 || received != 1 || result != 'Y') {
        puts("exFAT concurrent overwrites failed");
        goto poweroff;
    }
    close(observer);
    close(fd);
    if (umount("/mnt") != 0 || mount_exfat(MS_RDONLY, NULL) != 0) {
        perror("remount exfat");
        goto poweroff;
    }
    fd = open("/mnt/tail.bin", O_RDONLY);
    if (fd < 0 || pread(fd, data, sizeof(data), CLUSTER_SIZE - 4) != sizeof(data) ||
        memcmp(data, updated, sizeof(data)) != 0 ||
        pread(fd, data, sizeof(data), 0) != sizeof(data) ||
        memcmp(data, "LRLRLRLRLRLRLRLR", sizeof(data)) != 0 ||
        pread(fd, data, sizeof(data), sizeof(data)) != sizeof(data) ||
        memcmp(data, "LRLRLRLRLRLRLRLR", sizeof(data)) != 0) {
        puts("exFAT overwrite was not persisted");
        goto poweroff;
    }
    close(fd);
    fragmented = open("/mnt/fragment.bin", O_RDONLY);
    const unsigned char fragmented_data[sizeof(data)] = "AAfghij";
    if (fragmented < 0 ||
        pread(fragmented, data, sizeof(data), CLUSTER_SIZE - 4) != sizeof(data) ||
        memcmp(data, fragmented_data, sizeof(data)) != 0) {
        puts("exFAT FAT-chain overwrite was not persisted");
        goto poweroff;
    }
    close(fragmented);
    puts("EXFAT_OVERWRITE: PASS");
    if (umount("/mnt") != 0 || test_writable() != 0)
        goto poweroff;
    puts("EXFAT_READ_WRITE: PASS");

poweroff:
    fflush(stdout);
    reboot(RB_POWER_OFF);
    return 1;
}
"""


def checksum(data):
    result = 0
    for value in data:
        result = ((result >> 1) + ((result & 1) << 15) + value) & 0xffff
    return result


def file_entries(name, first_cluster, contiguous, *, length=2 * CLUSTER_SIZE,
                 valid_length=CLUSTER_SIZE + 3):
    entries = bytearray(3 * 32)
    entries[0:2] = bytes([0x85, 2])
    struct.pack_into('<HIII', entries, 4, 0x20, *([0x5c210000] * 3))
    entries[32:34] = bytes([0xc0, 3 if contiguous else 1])
    entries[35] = len(name)
    struct.pack_into('<H', entries, 36, checksum(name.upper().encode('utf-16le')))
    struct.pack_into('<QIIQ', entries, 40, valid_length, 0, first_cluster, length)
    entries[64] = 0xc1
    entries[66:66 + 2 * len(name)] = name.encode('utf-16le')
    struct.pack_into('<H', entries, 2, checksum(entries[:2] + entries[4:]))
    return entries


def convert_boot_to_4k(image):
    # The cluster heap and FAT of a freshly formatted fixture are aligned.
    # Expand the boot regions and express the same byte offsets in 4 KiB units.
    with image.open('r+b') as disk:
        original = disk.read(12 * 512)
        assert original[108] == 9 and original[109] >= 3
        boot = bytearray(12 * 4096)
        boot[:512] = original[:512]
        for offset, fmt in ((72, '<Q'), (80, '<I'), (84, '<I'), (88, '<I')):
            value, = struct.unpack_from(fmt, original, offset)
            if offset != 84:
                assert value % 8 == 0
            struct.pack_into(fmt, boot, offset, (value + 7) // 8)
        boot[108] = 12
        boot[109] = original[109] - 3
        for sector in range(1, 11):
            start = sector * 512
            target = sector * 4096
            boot[target:target + 508] = original[start:start + 508]
            if sector <= 8:
                boot[target + 4092:target + 4096] = original[start + 508:start + 512]
        checksum32 = 0
        for index, value in enumerate(boot[:11 * 4096]):
            if index not in (106, 107, 112):
                checksum32 = ((checksum32 >> 1) + ((checksum32 & 1) << 31) + value) & 0xffffffff
        boot[11 * 4096:] = struct.pack('<I', checksum32) * 1024
        disk.seek(0)
        disk.write(boot + boot)


def make_image(_root_disk, out, sector_size=512):
    image = out / 'exfat.img'
    with image.open('wb') as disk:
        disk.truncate(32 * 1024**2)
    cmd_run(['mkfs.exfat', '-c', CLUSTER_SIZE, image])
    if sector_size == 4096:
        convert_boot_to_4k(image)

    with image.open('r+b') as disk:
        boot = disk.read(512)
        sector_size = 1 << boot[108]
        cluster_size = sector_size << boot[109]
        fat_offset, = struct.unpack_from('<I', boot, 80)
        heap, cluster_count, root = struct.unpack_from('<III', boot, 88)
        assert cluster_size == CLUSTER_SIZE

        def cluster_offset(cluster):
            return heap * sector_size + (cluster - 2) * cluster_size

        root_offset = cluster_offset(root)
        disk.seek(root_offset)
        directory = disk.read(cluster_size)
        bitmap_entry = next(pos for pos in range(0, cluster_size, 32)
                            if directory[pos] == 0x81)
        bitmap, bitmap_length = struct.unpack_from('<IQ', directory, bitmap_entry + 20)
        assert bitmap_length <= cluster_size
        disk.seek(cluster_offset(bitmap))
        allocated = bytearray(disk.read(bitmap_length))
        first = next(index for index in range(cluster_count - 4)
                     if not any(allocated[i // 8] & (1 << (i % 8))
                                for i in range(index, index + 5)))
        for index in (first, first + 1, first + 2, first + 4):
            allocated[index // 8] |= 1 << (index % 8)
        disk.seek(cluster_offset(bitmap))
        disk.write(allocated)

        # Both files have a partially initialized second cluster. Leave
        # visible bytes after ValidDataLength to detect raw reads.
        contiguous = first + 2, first + 3
        fragmented = first + 4, first + 6
        for head, tail in (contiguous, fragmented):
            disk.seek(cluster_offset(head))
            disk.write(b'A' * cluster_size)
            disk.seek(cluster_offset(tail))
            disk.write(b'XYZ' + b'Z' * (cluster_size - 3))

        for cluster, next_cluster in zip(fragmented, (fragmented[1], 0xffffffff)):
            disk.seek(fat_offset * sector_size + cluster * 4)
            disk.write(struct.pack('<I', next_cluster))

        entries = (file_entries('tail.bin', contiguous[0], True) +
                   file_entries('fragment.bin', fragmented[0], False))

        end = next(pos for pos in range(0, cluster_size, 32) if directory[pos] == 0)
        assert end + len(entries) + 32 <= cluster_size
        disk.seek(root_offset + end)
        disk.write(entries)

    cmd_run(['fsck.exfat', '-n', image])


LARGE_INIT = r"""
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/uio.h>
#include <unistd.h>

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "exFAT large-file check failed at line %d: %s\n", \
                __LINE__, #condition); \
        goto poweroff; \
    } \
} while (0)

int main(void) {
    const off_t high = 1LL << 32;
    struct stat st;
    struct statfs before, after;
    char bytes[4];
    char span[16];
    REQUIRE(mount("/dev/nvme0n1", "/mnt", "exfat", 0, NULL) == 0 ||
            mount("/dev/nvme1n1", "/mnt", "exfat", 0, NULL) == 0);
    int fd = open("/mnt/large.bin", O_RDWR);
    REQUIRE(fd >= 0 && fstat(fd, &st) == 0 && st.st_size == high + 4096);
    REQUIRE(lseek(fd, -4096, SEEK_END) == high);
    REQUIRE(read(fd, bytes, sizeof(bytes)) == 4 && !memcmp(bytes, "high", 4));
    struct iovec vec[] = {{.iov_base = "64", .iov_len = 2},
                          {.iov_base = "ok", .iov_len = 2}};
    REQUIRE(pwritev(fd, vec, 2, high + 8) == 4);
    REQUIRE(pread(fd, bytes, 4, high + 8) == 4 && !memcmp(bytes, "64ok", 4));
    REQUIRE(pread(fd, bytes, 4, 0) == 4 && !memcmp(bytes, "low!", 4));
    /* Cross the 4 GiB page-cache boundary [0xFFFFF000 .. 0x100000010). */
    REQUIRE(pwrite(fd, "EDGE", 4, high - 2) == 4);
    REQUIRE(pread(fd, span, sizeof(span), high - 8) == sizeof(span));
    REQUIRE(!memcmp(span + 6, "EDGE", 4));
    REQUIRE(!memcmp(span + 8, "GEgh", 4));
    /* Page 0xFFFFF000 + 4096 overflows 32-bit page cache via mmap/mremap. */
    REQUIRE(mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, high - 4096) == MAP_FAILED &&
            errno == EOVERFLOW);
    char *near4g = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, high - 8192);
    REQUIRE(near4g != MAP_FAILED);
    REQUIRE(mremap(near4g, 4096, 8192, 0) == MAP_FAILED && errno == EOVERFLOW);
    REQUIRE(munmap(near4g, 4096) == 0);
    int append = open("/mnt/large.bin", O_WRONLY | O_APPEND);
    REQUIRE(append >= 0 && write(append, "tail", 4) == 4);
    REQUIRE(lseek(append, 0, SEEK_CUR) == high + 4100 && close(append) == 0);
    REQUIRE(close(fd) == 0 && umount("/mnt") == 0);
    REQUIRE(mount("/dev/nvme0n1", "/mnt", "exfat", MS_RDONLY, NULL) == 0 ||
            mount("/dev/nvme1n1", "/mnt", "exfat", MS_RDONLY, NULL) == 0);
    fd = open("/mnt/large.bin", O_RDONLY);
    REQUIRE(fd >= 0 && fstat(fd, &st) == 0 && st.st_size == high + 4100);
    REQUIRE(pread(fd, span, 8, high - 4) == 8 && !memcmp(span + 2, "EDGE", 4));
    REQUIRE(pread(fd, bytes, 4, high + 8) == 4 && !memcmp(bytes, "64ok", 4));
    REQUIRE(pread(fd, bytes, 4, high + 4096) == 4 && !memcmp(bytes, "tail", 4));
    REQUIRE(close(fd) == 0 && umount("/mnt") == 0);

    REQUIRE(mount("/dev/nvme0n1", "/mnt", "exfat", 0, NULL) == 0 ||
            mount("/dev/nvme1n1", "/mnt", "exfat", 0, NULL) == 0);
    fd = open("/mnt/large.bin", O_RDWR);
    REQUIRE(fd >= 0 && statfs("/mnt", &before) == 0);
    REQUIRE(ftruncate(fd, 0) == 0 && fstat(fd, &st) == 0 && st.st_size == 0);
    REQUIRE(statfs("/mnt", &after) == 0 && after.f_bfree > before.f_bfree);
    REQUIRE(ftruncate(fd, high + 4096) == 0);
    REQUIRE(lseek(fd, 0, SEEK_END) == high + 4096);
    REQUIRE(pread(fd, bytes, 4, high + 8) == 4 && !memcmp(bytes, "\0\0\0\0", 4));
    REQUIRE(pwrite(fd, "new!", 4, 0) == 4 && close(fd) == 0);
    REQUIRE(umount("/mnt") == 0);
    REQUIRE(mount("/dev/nvme0n1", "/mnt", "exfat", MS_RDONLY, NULL) == 0 ||
            mount("/dev/nvme1n1", "/mnt", "exfat", MS_RDONLY, NULL) == 0);
    fd = open("/mnt/large.bin", O_RDONLY);
    REQUIRE(fd >= 0 && fstat(fd, &st) == 0 && st.st_size == high + 4096);
    REQUIRE(pread(fd, bytes, 4, high + 8) == 4 && !memcmp(bytes, "\0\0\0\0", 4));
    REQUIRE(pread(fd, bytes, 4, 0) == 4 && !memcmp(bytes, "new!", 4));
    REQUIRE(close(fd) == 0 && umount("/mnt") == 0);
    puts("EXFAT_LARGE: PASS");
poweroff:
    fflush(stdout);
    reboot(RB_POWER_OFF);
    return 1;
}
"""


def make_large_image(_root_disk, out):
    image = out / 'exfat.img'
    cluster_size = 1024**2
    data_length = 2**32 + 4096
    clusters_needed = (data_length + cluster_size - 1) // cluster_size
    with image.open('wb') as disk:
        disk.truncate(5 * 1024**3)
    cmd_run(['mkfs.exfat', '-c', cluster_size, image])
    with image.open('r+b') as disk:
        boot = disk.read(512)
        sector_size = 1 << boot[108]
        heap, cluster_count, root = struct.unpack_from('<III', boot, 88)
        assert sector_size << boot[109] == cluster_size

        def cluster_offset(cluster):
            return heap * sector_size + (cluster - 2) * cluster_size

        root_offset = cluster_offset(root)
        disk.seek(root_offset)
        directory = disk.read(cluster_size)
        bitmap_entry = next(pos for pos in range(0, cluster_size, 32)
                            if directory[pos] == 0x81)
        bitmap, bitmap_len = struct.unpack_from('<IQ', directory, bitmap_entry + 20)
        assert bitmap_len <= cluster_size
        disk.seek(cluster_offset(bitmap))
        allocated = bytearray(disk.read(bitmap_len))
        # mkfs places the initial metadata before the free data area.
        first = next(index for index in range(cluster_count)
                     if not allocated[index // 8] & (1 << (index % 8)))
        assert first + clusters_needed <= cluster_count
        for index in range(first, first + clusters_needed):
            bit = 1 << (index % 8)
            assert not allocated[index // 8] & bit
            allocated[index // 8] |= bit
        disk.seek(cluster_offset(bitmap))
        disk.write(allocated)
        start = cluster_offset(first + 2)
        disk.seek(start)
        disk.write(b'low!')
        disk.seek(start + 2**32)
        disk.write(b'high')
        end = next(pos for pos in range(0, cluster_size, 32) if not directory[pos])
        disk.seek(root_offset + end)
        disk.write(file_entries('large.bin', first + 2, True, length=data_length,
                                valid_length=data_length))
    cmd_run(['fsck.exfat', '-n', image])


ROLLBACK_INIT = r"""
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

int main(void) {
    struct statfs before, after;
    if (mount("/dev/nvme0n1", "/mnt", "exfat", 0, NULL) != 0 &&
        mount("/dev/nvme1n1", "/mnt", "exfat", 0, NULL) != 0) {
        perror("mount rollback probe");
        goto poweroff;
    }
    if (statfs("/mnt", &before) != 0 || mkdir("/mnt/retry", 0755) != -1 || errno != EIO) {
        perror("expected a failed exFAT metadata write");
        goto poweroff;
    }
    if (access("/mnt/retry", F_OK) != -1 || errno != ENOENT ||
        statfs("/mnt", &after) != 0 || before.f_bfree != after.f_bfree ||
        mkdir("/mnt/retry", 0755) != 0 || rmdir("/mnt/retry") != 0 || umount("/mnt") != 0) {
        perror("exFAT transaction rollback or retry failed");
        goto poweroff;
    }
    puts("EXFAT_ROLLBACK: PASS");
poweroff:
    fflush(stdout);
    reboot(RB_POWER_OFF);
    return 1;
}
"""


def make_fault_image(root_disk, out):
    make_image(root_disk, out)
    with (out / 'exfat.img').open('rb') as image:
        fat_lba, = struct.unpack_from('<I', image.read(512), 80)
    (out / 'write-error.conf').write_text(
        '[inject-error]\n'
        'event = "write_aio"\n'
        'errno = "5"\n'
        f'sector = "{fat_lba}"\n'
        'once = "on"\n'
        'immediately = "on"\n'
    )


def run_exfat_probe(name, init, marker, setup_image, timeout=120, device_sector=512,
                    inject_error=False):
    image = ROOT / 'build' / name / 'exfat.img'
    backing = f'blkdebug:{image.with_name("write-error.conf")}:{image}' if inject_error else image
    qemu_disk = ('-drive', f'file={backing},format=raw,if=none,id=exfat',
                 '-device', 'nvme,id=exfat-ctrl,serial=exfat-test',
                 '-device', f'nvme-ns,bus=exfat-ctrl,drive=exfat,'
                            f'logical_block_size={device_sector},physical_block_size={device_sector}')
    out = run_probe(name, init, marker, accel='tcg', timeout=timeout,
                    setup=lambda root: (root / 'mnt').mkdir(), prepare_disk=setup_image,
                    qemu_args=qemu_disk)
    subprocess.run(['fsck.exfat', '-n', image], check=True)
    print(marker, out / 'serial.log')


if __name__ == '__main__':
    run_exfat_probe(TEST_NAME, INIT, 'EXFAT_READ_WRITE: PASS', make_image)
    for device_sector in (512, 4096):
        run_exfat_probe(f'exfat-4k-{device_sector}', INIT, 'EXFAT_READ_WRITE: PASS',
                        lambda root, out: make_image(root, out, 4096), device_sector=device_sector)
    run_exfat_probe('exfat-large', LARGE_INIT, 'EXFAT_LARGE: PASS', make_large_image, 180)
    run_exfat_probe('exfat-rollback', ROLLBACK_INIT, 'EXFAT_ROLLBACK: PASS', make_fault_image,
                    inject_error=True)
