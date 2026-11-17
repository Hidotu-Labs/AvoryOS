#include "drivers/i2c/smbus_piix4.h"
#include "console/klog.h"
#include "drivers/pci/pci.h"
#include "io/io.h"
#include "lock/spinlock.h"
#include <stddef.h>

/* PIIX4 / ICH / AMD FCH SMBus I/O register offsets */
#define SMB_HST_STS  0x00
#define SMB_HST_CNT  0x02
#define SMB_HST_CMD  0x03
#define SMB_HST_ADD  0x04
#define SMB_HST_DAT0 0x05
#define SMB_HST_DAT1 0x06

/* SMB_HST_STS bits */
#define SMB_STS_HOST_BUSY 0x01U
#define SMB_STS_INTR      0x02U
#define SMB_STS_DEV_ERR   0x04U
#define SMB_STS_BUS_ERR   0x08U
#define SMB_STS_FAILED    0x10U
#define SMB_STS_INUSE     0x40U
#define SMB_STS_ERR_MASK  (SMB_STS_DEV_ERR | SMB_STS_BUS_ERR | SMB_STS_FAILED)
#define SMB_STS_CLEAR     (SMB_STS_INTR | SMB_STS_ERR_MASK)

/* SMB_HST_CNT commands */
#define SMB_CMD_QUICK     (0x00U << 2)
#define SMB_CMD_BYTE      (0x01U << 2)
#define SMB_CMD_BYTE_DATA (0x02U << 2)
#define SMB_CMD_WORD_DATA (0x03U << 2)
#define SMB_CNT_START     0x40U

/* AMD FCH PMIO index/data ports */
#define AMD_PMIO_INDEX 0xCD6
#define AMD_PMIO_DATA  0xCD7

static spinlock_t smbus_lock = SPINLOCK_INIT;
static uint16_t smb_base = 0;
static bool smb_ready = false;

static uint8_t amd_pmio_read8(uint8_t reg) {
  outb(AMD_PMIO_INDEX, reg);
  return inb(AMD_PMIO_DATA);
}

static uint16_t probe_intel_piix4(struct pci_device *dev) {
  uint8_t hostc = pci_config_read8(dev->bus, dev->slot, dev->func, 0xD2);
  if (!(hostc & 0x01U))
    pci_config_write8(dev->bus, dev->slot, dev->func, 0xD2, hostc | 0x01U);

  uint32_t smb_ba = pci_config_read32(dev->bus, dev->slot, dev->func, 0x90);
  uint16_t base = (uint16_t)(smb_ba & 0xFFF0U);
  return base;
}

static uint16_t probe_intel_ich(struct pci_device *dev) {
  uint8_t hostc = pci_config_read8(dev->bus, dev->slot, dev->func, 0x40);
  if (!(hostc & 0x01U))
    pci_config_write8(dev->bus, dev->slot, dev->func, 0x40, hostc | 0x01U);

  uint32_t bar4 = pci_config_read32(dev->bus, dev->slot, dev->func, 0x20);
  if (!(bar4 & 0x01U))
    return 0;
  return (uint16_t)(bar4 & 0xFFE0U);
}

static uint16_t probe_amd_fch(struct pci_device *dev) {
  uint8_t rev = pci_config_read8(dev->bus, dev->slot, dev->func, 0x08);

  if (dev->vendor_id == 0x1002U && dev->device_id == 0x4385U && rev < 0x40U) {
    uint32_t smb_ba = pci_config_read32(dev->bus, dev->slot, dev->func, 0x90);
    return (uint16_t)(smb_ba & 0xFFF0U);
  }

  /* Kern/Zen FCH (1022:790b rev >= 0x49) moved SMBus0 enable/base to PMIO 0x00/0x01. */
  if (dev->vendor_id == 0x1022U && dev->device_id == 0x790BU && rev >= 0x49U) {
    uint8_t dec = amd_pmio_read8(0x00);
    if (!(dec & 0x10U))
      return 0;
    return (uint16_t)amd_pmio_read8(0x01) << 8;
  }

  uint16_t pmio_val =
      (uint16_t)amd_pmio_read8(0x2C) | ((uint16_t)amd_pmio_read8(0x2D) << 8);
  if (!(pmio_val & 0x0001U))
    return 0;
  return pmio_val & 0xFFE0U;
}

void smbus_piix4_init(void) {
  smb_base = 0;
  smb_ready = false;

  uint32_t count = pci_get_device_count();
  for (uint32_t i = 0; i < count; i++) {
    struct pci_device *dev = asc_pci_get_device(i);
    if (!dev || dev->class_code != 0x0CU || dev->subclass != 0x05U)
      continue;

    uint16_t base = 0;
    const char *kind = NULL;

    if (dev->vendor_id == 0x8086U) {
      if (dev->device_id == 0x7113U) {
        base = probe_intel_piix4(dev);
        kind = "Intel PIIX4";
      } else {
        base = probe_intel_ich(dev);
        kind = "Intel ICH";
      }
    } else if (dev->vendor_id == 0x1022U || dev->vendor_id == 0x1002U) {
      base = probe_amd_fch(dev);
      kind = "AMD FCH";
    }

    if (!base || base == 0xFFE0U || base == 0xFFF0U)
      continue;

    uint16_t cmd = pci_config_read16(dev->bus, dev->slot, dev->func, 0x04);
    if (!(cmd & 0x0001U))
      pci_config_write16(dev->bus, dev->slot, dev->func, 0x04, cmd | 0x0001U);

    smb_base = base;
    smb_ready = true;
    klogf("[SMBUS] %s SMBus (%04x:%04x at %02x:%02x.%x) I/O base 0x%04x\n",
          kind, dev->vendor_id, dev->device_id, dev->bus, dev->slot, dev->func,
          smb_base);
    return;
  }
}

bool smbus_available(void) { return smb_ready; }

uint16_t smbus_io_base(void) { return smb_base; }

static int smbus_xfer(uint8_t addr, bool is_read, uint8_t reg, uint8_t proto,
                      uint16_t write_val, uint16_t *read_out) {
  if (!smb_ready || !smb_base)
    return -19; /* -ENODEV */
  if (addr > 0x7FU)
    return -22; /* -EINVAL */

  spinlock_acquire(&smbus_lock);

  /* Wait for any prior transaction to finish. */
  int timeout = 10000;
  uint8_t sts = inb(smb_base + SMB_HST_STS);
  while ((sts & SMB_STS_HOST_BUSY) && --timeout > 0) {
    io_wait();
    sts = inb(smb_base + SMB_HST_STS);
  }
  if (sts & SMB_STS_HOST_BUSY) {
    spinlock_release(&smbus_lock);
    return -16; /* -EBUSY */
  }

  /* Clear latched status bits before starting. */
  if (sts & SMB_STS_CLEAR)
    outb(smb_base + SMB_HST_STS, sts & SMB_STS_CLEAR);

  outb(smb_base + SMB_HST_ADD, (uint8_t)((addr << 1) | (is_read ? 1U : 0U)));

  if (proto != SMB_CMD_QUICK)
    outb(smb_base + SMB_HST_CMD, reg);

  if (!is_read) {
    if (proto == SMB_CMD_BYTE_DATA || proto == SMB_CMD_WORD_DATA)
      outb(smb_base + SMB_HST_DAT0, (uint8_t)(write_val & 0xFFU));
    if (proto == SMB_CMD_WORD_DATA)
      outb(smb_base + SMB_HST_DAT1, (uint8_t)((write_val >> 8) & 0xFFU));
  }

  outb(smb_base + SMB_HST_CNT, proto | SMB_CNT_START);

  timeout = 50000;
  do {
    io_wait();
    sts = inb(smb_base + SMB_HST_STS);
  } while ((sts & SMB_STS_HOST_BUSY) && --timeout > 0);

  int rc = 0;
  if (timeout <= 0) {
    rc = -110; /* -ETIMEDOUT */
  } else if (sts & SMB_STS_DEV_ERR) {
    rc = -6; /* -ENXIO (NACK / no device) */
  } else if (sts & (SMB_STS_BUS_ERR | SMB_STS_FAILED)) {
    rc = -5; /* -EIO */
  } else if (is_read && read_out) {
    uint16_t lo = inb(smb_base + SMB_HST_DAT0);
    uint16_t hi = (proto == SMB_CMD_WORD_DATA) ? inb(smb_base + SMB_HST_DAT1) : 0;
    *read_out = lo | (hi << 8);
  }

  outb(smb_base + SMB_HST_STS, SMB_STS_CLEAR);
  if (sts & SMB_STS_INUSE)
    outb(smb_base + SMB_HST_STS, SMB_STS_INUSE);

  spinlock_release(&smbus_lock);
  return rc;
}

int smbus_quick(uint8_t addr, bool is_read) {
  return smbus_xfer(addr, is_read, 0, SMB_CMD_QUICK, 0, NULL);
}

int smbus_read_byte(uint8_t addr, uint8_t *out) {
  if (!out)
    return -22;
  uint16_t val = 0;
  int rc = smbus_xfer(addr, true, 0, SMB_CMD_BYTE, 0, &val);
  if (rc == 0)
    *out = (uint8_t)(val & 0xFFU);
  return rc;
}

int smbus_write_byte(uint8_t addr, uint8_t val) {
  return smbus_xfer(addr, false, val, SMB_CMD_BYTE, 0, NULL);
}

int smbus_read_byte_data(uint8_t addr, uint8_t reg, uint8_t *out) {
  if (!out)
    return -22;
  uint16_t val = 0;
  int rc = smbus_xfer(addr, true, reg, SMB_CMD_BYTE_DATA, 0, &val);
  if (rc == 0)
    *out = (uint8_t)(val & 0xFFU);
  return rc;
}

int smbus_write_byte_data(uint8_t addr, uint8_t reg, uint8_t val) {
  return smbus_xfer(addr, false, reg, SMB_CMD_BYTE_DATA, val, NULL);
}

int smbus_read_word_data(uint8_t addr, uint8_t reg, uint16_t *out) {
  if (!out)
    return -22;
  return smbus_xfer(addr, true, reg, SMB_CMD_WORD_DATA, 0, out);
}

int smbus_write_word_data(uint8_t addr, uint8_t reg, uint16_t val) {
  return smbus_xfer(addr, false, reg, SMB_CMD_WORD_DATA, val, NULL);
}
