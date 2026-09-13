/* Phase 5 C3 — firmware loader self-test.
 *
 * Reads /lib/firmware/test_fw.bin, staged into the disk image by the
 * top-level build (a 4 KB file where byte i is (i * 7 + 3) & 0xff), and
 * checks the full size + content, the -ENOENT path and path-traversal
 * rejection.  This is the loader amdgpu's PSP/SMU blobs use from 6c on. */

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/string.h>

#include <linuxkpi/log.h>

#define P5F_SIZE 4096u

static int p5f_failures;

static void p5f_ok(const char *what) {
  klogf("[  OK  ] LinuxKPI: fw %s\n", what);
}

static void p5f_fail(const char *what, long v) {
  p5f_failures++;
  klogf("[FAIL] LinuxKPI: fw %s (%ld)\n", what, v);
}

static unsigned char p5f_expected(unsigned int i) {
  return (unsigned char)((i * 7u + 3u) & 0xFFu);
}

void linuxkpi_test_phase5_firmware(void) {
  const struct firmware *fw = NULL;
  int ret;
  unsigned int i;

  p5f_failures = 0;
  klog_puts("[LINUXKPI] Phase 5 firmware self-test\n");

  ret = request_firmware(&fw, "test_fw.bin", NULL);
  if (ret == 0 && fw && fw->size == P5F_SIZE) {
    for (i = 0; i < P5F_SIZE; i++) {
      if (fw->data[i] != p5f_expected(i))
        break;
    }
    if (i == P5F_SIZE)
      p5f_ok("request_firmware size + full content match");
    else
      p5f_fail("firmware content", (long)i);
  } else {
    p5f_fail("request_firmware", ret);
  }
  if (fw)
    release_firmware(fw);
  fw = NULL;

  ret = request_firmware(&fw, "definitely_missing.bin", NULL);
  if (ret == -ENOENT && fw == NULL)
    p5f_ok("missing file returns -ENOENT");
  else
    p5f_fail("missing file", ret);

  ret = request_firmware(&fw, "../etc/passwd", NULL);
  if (ret == -EINVAL && fw == NULL)
    p5f_ok("path traversal rejected");
  else
    p5f_fail("path traversal", ret);

  if (p5f_failures == 0)
    klog_puts("[  OK  ] LinuxKPI: fw suite complete\n");
  else
    klogf("[FAIL] LinuxKPI: fw suite had %d failure(s)\n", p5f_failures);
}
