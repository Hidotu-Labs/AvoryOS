#include "rtc.h"
#include "../../io/io.h"
#include "../../console/klog.h"
#include "../../lib/string.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint64_t boot_timestamp = 0;

static uint8_t rtc_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static int rtc_is_updating() {
    return rtc_read(0x0A) & 0x80;
}

static uint8_t bcd_to_bin(uint8_t bcd) {
    return ((bcd / 16) * 10) + (bcd & 0x0F);
}

uint64_t rtc_get_timestamp(void) {
    // Wait for update to finish
    while (rtc_is_updating());

    uint8_t second = rtc_read(0x00);
    uint8_t minute = rtc_read(0x02);
    uint8_t hour   = rtc_read(0x04);
    uint8_t day    = rtc_read(0x07);
    uint8_t month  = rtc_read(0x08);
    uint16_t year  = rtc_read(0x09);
    uint8_t status_b = rtc_read(0x0B);

    // Convert BCD to binary if necessary
    if (!(status_b & 0x04)) {
        second = bcd_to_bin(second);
        minute = bcd_to_bin(minute);
        hour   = bcd_to_bin(hour);
        day    = bcd_to_bin(day);
        month  = bcd_to_bin(month);
        year   = bcd_to_bin(year);
    }

    // Convert 12h to 24h if necessary
    if (!(status_b & 0x02) && (hour & 0x80)) {
        hour = ((hour & 0x7F) + 12) % 24;
    }

    // Determine century (assume 21st century if reg 0x32 is not available or 0)
    uint16_t century = rtc_read(0x32);
    if (!(status_b & 0x04)) century = bcd_to_bin(century);
    
    if (century == 0) century = 20; // Default to 20xx
    year += century * 100;

    // Unix timestamp calculation (simplified, but accurate for modern dates)
    // Formula: seconds since 1970-01-01 00:00:00 UTC
    
    uint64_t y = year;
    uint64_t m = month;
    uint64_t d = day;

    // Adjust month/year for the formula
    if (m <= 2) {
        m += 12;
        y -= 1;
    }

    // Days since epoch
    uint64_t days = (d + (153 * m - 457) / 5 + 365 * y + y / 4 - y / 100 + y / 400) - 719469;
    
    uint64_t timestamp = days * 86400 + (uint64_t)hour * 3600 + (uint64_t)minute * 60 + (uint64_t)second;

    return timestamp;
}

void rtc_init(void) {
    boot_timestamp = rtc_get_timestamp();
    klog_puts("[RTC] Initialized. Current Unix timestamp: ");
    klog_uint64(boot_timestamp);
    klog_puts("\n");
}

uint64_t rtc_get_boot_timestamp(void) {
    return boot_timestamp;
}

static const char *month_names[] = {
  "Jan", "Feb", "Mar", "Apr", "May", "Jun",
  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

void rtc_format_datetime(uint64_t timestamp, char *buf, size_t bufsize) {
  // Convert Unix timestamp to date/time string: "May 9 2026 10:35:16"
  uint64_t seconds = timestamp;
  uint64_t days = seconds / 86400;
  uint64_t secs_remaining = seconds % 86400;
  
  uint64_t hour = secs_remaining / 3600;
  uint64_t minute = (secs_remaining % 3600) / 60;
  uint64_t sec = secs_remaining % 60;
  
  // Calculate year, month, day from days since epoch
  uint64_t year = 1970;
  while (1) {
    uint64_t days_in_year = ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0) ? 366 : 365;
    if (days < days_in_year) break;
    days -= days_in_year;
    year++;
  }
  
  uint64_t month = 0;
  uint64_t days_in_months[] = {31, ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0) ? 29 : 28, 
                                31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  while (month < 12 && days >= days_in_months[month]) {
    days -= days_in_months[month];
    month++;
  }
  
  uint64_t day = days + 1;
  month++; // 1-indexed
  
  // Format: "May 9 2026 10:35:16"
  char tmp[32];
  char *p = tmp;
  
  // Month name
  const char *mname = month_names[(month - 1) % 12];
  while (*mname) *p++ = *mname++;
  *p++ = ' ';
  
  // Day
  if (day >= 10) {
    *p++ = '0' + (day / 10);
  }
  *p++ = '0' + (day % 10);
  *p++ = ' ';
  
  // Year
  uint64_t y = year;
  char ybuf[5] = {0};
  for (int i = 3; i >= 0; i--) {
    ybuf[i] = '0' + (y % 10);
    y /= 10;
  }
  for (int i = 0; i < 4; i++) *p++ = ybuf[i];
  
  *p++ = ' ';
  
  // Hour
  if (hour < 10) *p++ = '0';
  else *p++ = '0' + (hour / 10);
  *p++ = '0' + (hour % 10);
  *p++ = ':';
  
  // Minute
  if (minute < 10) *p++ = '0';
  else *p++ = '0' + (minute / 10);
  *p++ = '0' + (minute % 10);
  *p++ = ':';
  
  // Second
  if (sec < 10) *p++ = '0';
  else *p++ = '0' + (sec / 10);
  *p++ = '0' + (sec % 10);
  
  *p = '\0';
  
  size_t len = p - tmp;
  if (len >= bufsize) len = bufsize - 1;
  memcpy(buf, tmp, len + 1);
}
