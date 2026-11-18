#ifndef CPU_FRED_H
#define CPU_FRED_H

#include <stdbool.h>
#include <stdint.h>

#define IA32_FRED_RSP0 0x1CCU
#define IA32_FRED_RSP1 0x1CDU
#define IA32_FRED_RSP2 0x1CEU
#define IA32_FRED_RSP3 0x1CFU
#define IA32_FRED_STKLVLS 0x1D0U
#define IA32_FRED_SSP1 0x1D1U
#define IA32_FRED_SSP2 0x1D2U
#define IA32_FRED_SSP3 0x1D3U
#define IA32_FRED_CONFIG 0x1D4U

#define CR4_FRED (1ULL << 32)

/* FRED event types encoded in bits [51:48] of the stacked SS qword. */
#define FRED_EVENT_TYPE_EXTINT 0U
#define FRED_EVENT_TYPE_NMI 2U
#define FRED_EVENT_TYPE_HW_EXC 3U
#define FRED_EVENT_TYPE_SW_INT 4U
#define FRED_EVENT_TYPE_PRIV_SW_EXC 5U
#define FRED_EVENT_TYPE_SW_EXC 6U
#define FRED_EVENT_TYPE_OTHER 7U

/* Initialize Intel FRED on the calling CPU if supported by hardware. */
void fred_init_cpu(void);

#endif
