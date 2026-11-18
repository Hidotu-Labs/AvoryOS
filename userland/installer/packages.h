#ifndef AVORY_INSTALLER_PACKAGES_H
#define AVORY_INSTALLER_PACKAGES_H

#include "installer.h"

typedef struct pkgsel pkgsel_t;

/* Resolve the package set for cfg->desktop (plus dependencies) from the live
 * system's pacman database. Returns NULL and fills err_buf on failure. */
pkgsel_t *pkgsel_load(const installer_cfg_t *cfg, char *err_buf, size_t err_sz);
void pkgsel_free(pkgsel_t *sel);

int pkgsel_count(const pkgsel_t *sel);
uint64_t pkgsel_installed_bytes(const pkgsel_t *sel);

/* Copy the selected packages' files, their pacman database entries, and files
 * no package owns (AvoryOS configs, firmware, local tools) into TARGET_MOUNT.
 * Progress is reported between pct_lo and pct_hi. */
int pkgsel_copy(const pkgsel_t *sel, install_progress_fn cb, void *ctx,
                int pct_lo, int pct_hi, char *err_buf, size_t err_sz);

#endif /* AVORY_INSTALLER_PACKAGES_H */
