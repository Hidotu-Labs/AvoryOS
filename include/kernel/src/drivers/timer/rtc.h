#ifndef RTC_H
#define RTC_H

#include <stdint.h>
#include <stddef.h>

void rtc_init(void);
uint64_t rtc_get_timestamp(void);
uint64_t rtc_get_boot_timestamp(void);
void rtc_format_datetime(uint64_t timestamp, char *buf, size_t bufsize);

#endif
