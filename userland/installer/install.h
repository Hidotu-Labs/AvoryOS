#ifndef AVORY_INSTALLER_INSTALL_H
#define AVORY_INSTALLER_INSTALL_H

#include "installer.h"

void install_log(const char *line);

int run_cmd_logged(const char *const argv[], const char *stdin_data,
                   install_progress_fn cb, int pct, const char *stage, void *ctx);

int run_cmd_capture(const char *const argv[], char *out, size_t out_sz);

int install_system(const installer_cfg_t *cfg, const disk_t *disk,
                   install_progress_fn cb, void *ctx,
                   char *err_buf, size_t err_sz);

void install_unmount_target(void);
void install_enter_chroot(void);

#endif /* AVORY_INSTALLER_INSTALL_H */
