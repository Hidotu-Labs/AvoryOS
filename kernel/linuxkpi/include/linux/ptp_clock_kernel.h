#ifndef AVORY_LINUXKPI_PTP_CLOCK_KERNEL_H
#define AVORY_LINUXKPI_PTP_CLOCK_KERNEL_H

/* PHYLIB only needs the disabled PTP interface in the wired profile. */
#if defined(LINUXKPI_NET_FRONTEND) && !defined(CONFIG_PTP_1588_CLOCK) && \
    !defined(CONFIG_PTP_1588_CLOCK_MODULE)
struct ptp_clock;
struct ptp_clock_info;
struct ptp_system_timestamp;
#else
#include_next <linux/ptp_clock_kernel.h>
#endif

#endif
