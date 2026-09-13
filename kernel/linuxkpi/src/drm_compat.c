/* Phase 3 compatibility stubs for infrastructure the DRM canaries compile
 * against but that AvoryOS implements in later phases.  Everything here is
 * weak: the real implementation (aperture/VBIOS in Phase 6) overrides these
 * without touching the callers.  Each stub is recorded in
 * docs/linuxkpi-gaps.md.  The i2c stubs that used to live here were replaced
 * by the real minimal core (linuxkpi/src/i2c.c, Phase 5 C4). */

#include <asm/pgtable_types.h>
#include <linux/aperture.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/regulator/consumer.h>

/* ── page protection (x86 PAT is not modeled) ───────────────────────────── */

__attribute__((weak)) pgprot_t pgprot_writecombine(pgprot_t prot) {
  return prot;
}

/* ── regulators (simpledrm supplies) ─────────────────────────────────────── */

__attribute__((weak)) int regulator_enable(struct regulator *regulator) {
  (void)regulator;
  return 0;
}

__attribute__((weak)) int regulator_disable(struct regulator *regulator) {
  (void)regulator;
  return 0;
}

__attribute__((weak)) int regulator_is_enabled(struct regulator *regulator) {
  (void)regulator;
  return 0;
}

__attribute__((weak)) int regulator_set_voltage(struct regulator *regulator,
                                                int min_uV, int max_uV) {
  (void)regulator;
  (void)min_uV;
  (void)max_uV;
  return 0;
}

__attribute__((weak)) int regulator_get_voltage(struct regulator *regulator) {
  (void)regulator;
  return 0;
}

__attribute__((weak)) struct regulator *regulator_get(struct device *dev,
                                                      const char *id) {
  (void)dev;
  (void)id;
  return ERR_PTR(-ENODEV);
}

__attribute__((weak)) struct regulator *regulator_get_optional(struct device *dev,
                                                               const char *id) {
  (void)dev;
  (void)id;
  return NULL;
}

__attribute__((weak)) void regulator_put(struct regulator *regulator) {
  (void)regulator;
}

/* ── legacy chardev registration ─────────────────────────────────────────── */

/* The DRM core still registers major 226 for its legacy stub fops; AvoryOS
 * has no major registry and registers /dev/dri nodes through the native
 * devfs, so taking the major is a successful no-op. */
int __attribute__((weak)) register_chrdev(unsigned int major, const char *name,
                                          const struct file_operations *fops) {
  (void)major;
  (void)name;
  (void)fops;
  return 0;
}

void __attribute__((weak)) unregister_chrdev(unsigned int major,
                                             const char *name) {
  (void)major;
  (void)name;
}

/* ── file cloning (drm_lease) ────────────────────────────────────────────── */

struct file;
struct file * __attribute__((weak)) file_clone_open(struct file *file) {
  (void)file;
  return ERR_PTR(-ENODEV);
}

/* ── firmware aperture (simpledrm) ───────────────────────────────────────── */

__attribute__((weak)) int devm_aperture_acquire_from_firmware(struct device *dev,
                                                              resource_size_t base,
                                                              resource_size_t size) {
  (void)dev;
  (void)base;
  (void)size;
  return 0;
}

/* ── nomodeset policy (drivers/video/aperture.c, not imported) ──────────── */

/* drm_module_*_driver_if_modeset() gates registration on this; AvoryOS always
 * allows the modeset driver.  Weak so the real aperture.c wins when imported. */
bool __attribute__((weak)) video_firmware_drivers_only(void) {
  return false;
}

/* ── PCI framebuffer aperture (bochs probe, Phase 4 C5) ──────────────────── */

/* The real implementation lives in drm_aperture.c, which is not imported yet;
 * AvoryOS has no framebuffer hand-over registry, so claiming the aperture is
 * a no-op.  Weak so the imported version overrides it without edits. */
struct drm_driver;
struct pci_dev;

__attribute__((weak)) int drm_aperture_remove_conflicting_pci_framebuffers(
    struct pci_dev *pdev, const struct drm_driver *req_driver) {
  (void)pdev;
  (void)req_driver;
  return 0;
}
