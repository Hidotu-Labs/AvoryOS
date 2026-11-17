#ifndef DRIVERS_I2C_SMBUS_PIIX4_H
#define DRIVERS_I2C_SMBUS_PIIX4_H

#include <stdbool.h>
#include <stdint.h>

/* Probe and initialize Intel PIIX4/ICH or AMD FCH SMBus controller. */
void smbus_piix4_init(void);

/* Returns true if a supported SMBus host controller was initialized. */
bool smbus_available(void);

/* Returns the decoded SMBus I/O port base, or 0 if unavailable. */
uint16_t smbus_io_base(void);

/* Synchronous SMBus transactions (7-bit slave address). Return 0 on success,
 * negative errno on error (-19 ENODEV, -16 EBUSY, -5 EIO, -110 ETIMEDOUT). */
int smbus_quick(uint8_t addr, bool is_read);
int smbus_read_byte(uint8_t addr, uint8_t *out);
int smbus_write_byte(uint8_t addr, uint8_t val);
int smbus_read_byte_data(uint8_t addr, uint8_t reg, uint8_t *out);
int smbus_write_byte_data(uint8_t addr, uint8_t reg, uint8_t val);
int smbus_read_word_data(uint8_t addr, uint8_t reg, uint16_t *out);
int smbus_write_word_data(uint8_t addr, uint8_t reg, uint16_t val);

#endif
