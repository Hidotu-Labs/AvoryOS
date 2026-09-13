/* Phase 5 C3 — firmware loader self-test.
 *
 * Reads /lib/firmware/test_fw.bin, staged into the disk image by the
 * top-level build (a 4 KB file where byte i is (i * 7 + 3) & 0xff), and
 * checks the full size + content, the -ENOENT path and path-traversal
 * rejection.
 *
 * Phase 6 C4 adds the amdgpu manifest check: the staging pass
 * (scripts/linux-firmware-install.sh) writes kpi_fw_manifest.h with the
 * name/size/zlib-CRC of every blob it installed, and this suite re-reads
 * each one through request_firmware() and compares.  Builds without
 * firmware staged simply skip that half. */

#include <linux/crc32.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/string.h>

#include <linuxkpi/log.h>

#if defined(__has_include)
#  if __has_include(<kpi_fw_manifest.h>)
#    include <kpi_fw_manifest.h>
#    define P5F_HAVE_MANIFEST 1
#  endif
#endif

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

#ifdef P5F_HAVE_MANIFEST
  {
    int manifest_bad = 0;

    for (i = 0; i < (unsigned int)KPI_FW_MANIFEST_COUNT; i++) {
      const struct kpi_fw_manifest_entry *entry = &kpi_fw_manifest[i];

      ret = request_firmware(&fw, entry->name, NULL);
      if (ret == 0 && fw) {
        u32 crc = crc32_le(~0u, fw->data, fw->size) ^ ~0u;

        if (fw->size != entry->size || crc != entry->crc32) {
          p5f_fail(entry->name, (long)crc);
          manifest_bad++;
        }
        release_firmware(fw);
        fw = NULL;
      } else {
        p5f_fail(entry->name, ret);
        manifest_bad++;
      }
    }
    if (manifest_bad == 0)
      klogf("[  OK  ] LinuxKPI: fw %u staged amdgpu blobs match host CRC\n",
            (unsigned int)KPI_FW_MANIFEST_COUNT);
  }
#else
  klog_puts("[SKIP] LinuxKPI: fw amdgpu manifest not staged\n");
#endif

  if (p5f_failures == 0)
    klog_puts("[  OK  ] LinuxKPI: fw suite complete\n");
  else
    klogf("[FAIL] LinuxKPI: fw suite had %d failure(s)\n", p5f_failures);
}
