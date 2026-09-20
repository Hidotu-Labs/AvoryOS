#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <errno.h>

#define WATCHDOG_IOCTL_BASE 'W'

struct watchdog_info {
    unsigned int options;          /* Options the card/driver supports */
    unsigned int firmware_version; /* Firmware version of the card */
    unsigned char identity[32];    /* Identity of the board */
};

#define WDIOC_GETSUPPORT    0x80285700
#define WDIOC_GETSTATUS     0x80045701
#define WDIOC_GETBOOTSTATUS 0x80045702
#define WDIOC_GETTEMP       0x80045703
#define WDIOC_SETOPTIONS    0x80045704
#define WDIOC_KEEPALIVE     0x80045705
#define WDIOC_SETTIMEOUT    0xc0045706
#define WDIOC_GETTIMEOUT    0x80045707
#define WDIOC_SETPRETIMEOUT 0xc0045708
#define WDIOC_GETPRETIMEOUT 0x80045709
#define WDIOC_GETTIMELEFT   0x8004570a

#define WDIOF_SETTIMEOUT    0x0080
#define WDIOF_MAGICCLOSE    0x0100
#define WDIOF_KEEPALIVEPING 0x8000

#define WDIOS_DISABLECARD   0x0001
#define WDIOS_ENABLECARD    0x0002

static int tests_run = 0;
static int tests_passed = 0;

#define TEST_ASSERT(cond, msg, ...) do { \
    tests_run++; \
    if (cond) { \
        printf("  [PASS] " msg "\n", ##__VA_ARGS__); \
        tests_passed++; \
    } else { \
        printf("  [FAIL] " msg " (errno: %s)\n", ##__VA_ARGS__, strerror(errno)); \
    } \
} while (0)

static void test_watchdog_device(const char *dev_path) {
    printf("\n=== Testing Watchdog Device: %s ===\n", dev_path);

    int fd = open(dev_path, O_WRONLY);
    if (fd < 0) {
        // Try O_RDWR
        fd = open(dev_path, O_RDWR);
    }
    TEST_ASSERT(fd >= 0, "Open %s", dev_path);
    if (fd < 0) return;

    // 1. WDIOC_GETSUPPORT
    struct watchdog_info info;
    memset(&info, 0, sizeof(info));
    int ret = ioctl(fd, WDIOC_GETSUPPORT, &info);
    TEST_ASSERT(ret == 0, "WDIOC_GETSUPPORT ioctl returned 0");
    TEST_ASSERT(strlen((char *)info.identity) > 0, "Watchdog identity: '%s'", (char *)info.identity);
    TEST_ASSERT((info.options & WDIOF_SETTIMEOUT) != 0, "Watchdog supports WDIOF_SETTIMEOUT (options=0x%x)", info.options);
    TEST_ASSERT((info.options & WDIOF_MAGICCLOSE) != 0, "Watchdog supports WDIOF_MAGICCLOSE");
    TEST_ASSERT((info.options & WDIOF_KEEPALIVEPING) != 0, "Watchdog supports WDIOF_KEEPALIVEPING");

    // 2. WDIOC_GETTIMEOUT
    int timeout = 0;
    ret = ioctl(fd, WDIOC_GETTIMEOUT, &timeout);
    TEST_ASSERT(ret == 0 && timeout > 0, "WDIOC_GETTIMEOUT returned %d seconds", timeout);

    // 3. WDIOC_SETTIMEOUT
    int new_timeout = 120;
    ret = ioctl(fd, WDIOC_SETTIMEOUT, &new_timeout);
    TEST_ASSERT(ret == 0 && new_timeout == 120, "WDIOC_SETTIMEOUT to 120s (got %d)", new_timeout);

    // Verify it stuck
    int verify_timeout = 0;
    ret = ioctl(fd, WDIOC_GETTIMEOUT, &verify_timeout);
    TEST_ASSERT(ret == 0 && verify_timeout == 120, "Verify new timeout is 120s");

    // Restore to 60s
    new_timeout = 60;
    ret = ioctl(fd, WDIOC_SETTIMEOUT, &new_timeout);
    TEST_ASSERT(ret == 0 && new_timeout == 60, "Reset timeout to 60s");

    // 4. WDIOC_GETTIMELEFT
    int timeleft = 0;
    ret = ioctl(fd, WDIOC_GETTIMELEFT, &timeleft);
    TEST_ASSERT(ret == 0 && timeleft > 0 && timeleft <= 60, "WDIOC_GETTIMELEFT returned %d seconds left", timeleft);

    // 5. WDIOC_KEEPALIVE (ioctl ping)
    ret = ioctl(fd, WDIOC_KEEPALIVE, 0);
    TEST_ASSERT(ret == 0, "WDIOC_KEEPALIVE ping");

    // 6. Write keepalive ping
    ssize_t written = write(fd, "ping", 4);
    TEST_ASSERT(written == 4, "Write keepalive ping ('ping')");

    // 7. WDIOC_SETOPTIONS (disable and re-enable)
    int opt = WDIOS_DISABLECARD;
    ret = ioctl(fd, WDIOC_SETOPTIONS, &opt);
    TEST_ASSERT(ret == 0, "WDIOC_SETOPTIONS disable card");

    opt = WDIOS_ENABLECARD;
    ret = ioctl(fd, WDIOC_SETOPTIONS, &opt);
    TEST_ASSERT(ret == 0, "WDIOC_SETOPTIONS re-enable card");

    // 8. Hardening: every pointer is validated and copied through the
    //    exception-table uaccess helpers, so a bogus or partially mapped
    //    buffer must come back as EFAULT instead of faulting the kernel.
    errno = 0;
    ret = ioctl(fd, WDIOC_GETTIMEOUT, (void *)0x1);
    TEST_ASSERT(ret == -1 && errno == EFAULT,
                "WDIOC_GETTIMEOUT rejects an unmapped pointer");

    errno = 0;
    ret = ioctl(fd, WDIOC_GETSUPPORT, (void *)0x1);
    TEST_ASSERT(ret == -1 && errno == EFAULT,
                "WDIOC_GETSUPPORT rejects an unmapped pointer");

    errno = 0;
    ssize_t bad_write = write(fd, (void *)0x1, 4);
    TEST_ASSERT(bad_write == -1 && errno == EFAULT,
                "write rejects an unmapped pointer");

    // A buffer that starts readable and runs into a PROT_NONE page: the
    // driver copies in bounded chunks, so the fault lands in copy_from_user
    // and surfaces as EFAULT rather than a kernel-mode page fault.
    long page = sysconf(_SC_PAGESIZE);
    uint8_t *guard = mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (guard != MAP_FAILED) {
        mprotect(guard + page, (size_t)page, PROT_NONE);
        errno = 0;
        bad_write = write(fd, guard + page - 2, 4);
        TEST_ASSERT(bad_write == -1 && errno == EFAULT,
                    "write rejects a buffer crossing into a PROT_NONE page");
        munmap(guard, (size_t)page * 2);
    }

    // 9. Magic close - 'V' marks this descriptor for disarm; the timer goes
    //    away when the last descriptor closes.  The explicit disable below
    //    keeps the machine safe even if a later test step fails.
    written = write(fd, "V", 1);
    TEST_ASSERT(written == 1, "Write magic close character 'V'");

    opt = WDIOS_DISABLECARD;
    ret = ioctl(fd, WDIOC_SETOPTIONS, &opt);
    TEST_ASSERT(ret == 0, "Disarm watchdog before close");

    ret = close(fd);
    TEST_ASSERT(ret == 0, "Close watchdog cleanly");
}

int main(int argc, char **argv) {
    printf("====================================================\n");
    printf("        AvoryOS Linux-Compatible Watchdog Test       \n");
    printf("====================================================\n");

    test_watchdog_device("/dev/watchdog");
    test_watchdog_device("/dev/watchdog0");

    printf("\n====================================================\n");
    printf("Test Results: %d / %d tests passed (%d%%)\n",
           tests_passed, tests_run, (tests_run > 0) ? (tests_passed * 100 / tests_run) : 0);
    printf("====================================================\n");

    if (argc > 1 && strcmp(argv[1], "--feed") == 0) {
        printf("\nStarting continuous watchdog feed daemon (Ctrl+C to stop)...\n");
        int fd = open("/dev/watchdog", O_WRONLY);
        if (fd < 0) {
            perror("Failed to open /dev/watchdog for feeding");
            return 1;
        }
        int count = 0;
        while (1) {
            sleep(5);
            count++;
            write(fd, "ping", 4);
            int timeleft = 0;
            ioctl(fd, WDIOC_GETTIMELEFT, &timeleft);
            printf("[%d] Fed watchdog, timeleft=%ds\n", count, timeleft);
        }
    }

    return (tests_passed == tests_run) ? 0 : 1;
}
