#ifndef LINUXKPI_PROBE_DIAG_H
#define LINUXKPI_PROBE_DIAG_H
/* Live ownership, not PMM cache residency, for bounded probe diagnostics. */
struct linuxkpi_firmware_usage {
  unsigned long headers, buffers, bytes;
};
void linuxkpi_firmware_usage(struct linuxkpi_firmware_usage *);
unsigned long linuxkpi_firmware_load_count(void);
unsigned long linuxkpi_vmap_live_areas(void);
unsigned long linuxkpi_irq_live_actions(void);
unsigned long linuxkpi_workqueue_live_count(void);
#endif
