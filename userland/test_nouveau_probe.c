/* C4: identify a real Nouveau DRM node; no rendering/submission yet. */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
struct drm_version {
  int major, minor, patch;
  size_t name_len; char *name;
  size_t date_len; char *date;
  size_t desc_len; char *desc;
};
#define DRM_VERSION _IOWR('d', 0x00, struct drm_version)
int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--software")) {
    puts("NOUVEAU_PROBE: software readiness only; hardware gate closed");
    return 0;
  }
  for (int card = 0; card < 16; card++) {
    char path[64], name[64] = {0};
    snprintf(path, sizeof(path), "/dev/dri/card%d", card);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) continue;
    struct drm_version v = {.name_len = sizeof(name) - 1, .name = name};
    int ret = ioctl(fd, DRM_VERSION, &v);
    close(fd);
    if (!ret && !strcmp(name, "nouveau")) {
#if defined(NOUVEAU_EXPECT_PROBE_FAILURE) || defined(NOUVEAU_EXPECT_NO_PROBE)
#ifdef NOUVEAU_EXPECT_NO_PROBE
      puts("NOUVEAU_PROBE: FAIL Nouveau unexpectedly bound during VFIO-only baseline");
#else
      puts("NOUVEAU_PROBE: FAIL Nouveau unexpectedly bound during missing-firmware test");
#endif
      return 1;
#else
      printf("NOUVEAU_PROBE: DRM registered %s driver=%s version=%d.%d.%d\n",
             path, name, v.major, v.minor, v.patch);
      return 0;
#endif
    }
  }
#ifdef NOUVEAU_EXPECT_NO_PROBE
  puts("NOUVEAU_PROBE: VFIO-only baseline exposes no Nouveau DRM card");
  return 0;
#elif defined(NOUVEAU_EXPECT_PROBE_FAILURE)
  puts("NOUVEAU_PROBE: expected failed probe exposes no Nouveau DRM card");
  return 0;
#else
  puts("NOUVEAU_PROBE: FAIL no Nouveau DRM card; inspect probe/GSP serial log");
  return 1;
#endif
}
