#include "drivers/storage/ata.h"
#include "console/console.h"
#include "console/klog.h"
#include "drivers/storage/block.h"
#include "io/io.h"
#include "lib/string.h"
#include "lock/spinlock.h"
#include <stddef.h>

static spinlock_t ata_lock = SPINLOCK_INIT;

// Internal data

struct ata_drive {
  uint16_t io_base;
  uint16_t ctrl_base;
  uint8_t slave; // 0 = master, 1 = slave
  bool present;
  uint64_t total_sectors;
  char model[41];
  struct block_device blkdev;
};

#define MAX_ATA_DRIVES 4
static struct ata_drive ata_drives[MAX_ATA_DRIVES];
static int ata_drive_count = 0;

// Helpers

static void print_uint64(uint64_t num) {
  if (num == 0) {
    console_putchar('0');
    return;
  }
  char buf[20];
  int i = 0;
  while (num > 0) {
    buf[i++] = '0' + (num % 10);
    num /= 10;
  }
  while (i > 0) {
    console_putchar(buf[--i]);
  }
}

static void ata_400ns_delay(uint16_t ctrl_base) {
  // Reading the alt status port 4 times wastes ~400ns
  inb(ctrl_base);
  inb(ctrl_base);
  inb(ctrl_base);
  inb(ctrl_base);
}

static int ata_wait_bsy(uint16_t io_base) {
  // Wait for BSY to clear, with a timeout
  for (int i = 0; i < 100000; i++) {
    uint8_t status = inb(io_base + ATA_REG_STATUS);
    if (!(status & ATA_SR_BSY))
      return 0;
  }
  return -1; // Timeout
}

static int ata_wait_drq(uint16_t io_base) {
  for (int i = 0; i < 100000; i++) {
    uint8_t status = inb(io_base + ATA_REG_STATUS);
    if (status & ATA_SR_BSY)
      continue;
    if (status & ATA_SR_ERR)
      return -1;
    if (status & ATA_SR_DF)
      return -1;
    if (status & ATA_SR_DRQ)
      return 0;
  }
  return -1; // Timeout
}

// IDENTIFY

static bool ata_identify_unlocked(struct ata_drive *drive) {
  uint16_t io = drive->io_base;
  uint16_t ctrl = drive->ctrl_base;

  // Select drive
  outb(io + ATA_REG_DRIVE, 0xA0 | (drive->slave << 4));
  ata_400ns_delay(ctrl);

  // Zero out sector count and LBA registers
  outb(io + ATA_REG_SECCOUNT, 0);
  outb(io + ATA_REG_LBA_LO, 0);
  outb(io + ATA_REG_LBA_MID, 0);
  outb(io + ATA_REG_LBA_HI, 0);

  // Send IDENTIFY command
  outb(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
  ata_400ns_delay(ctrl);

  // Check if drive exists
  uint8_t status = inb(io + ATA_REG_STATUS);
  if (status == 0)
    return false; // No drive

  // Wait for BSY to clear
  if (ata_wait_bsy(io) < 0)
    return false;

  // Check if this is an ATA (not ATAPI or SATA)
  uint8_t lba_mid = inb(io + ATA_REG_LBA_MID);
  uint8_t lba_hi = inb(io + ATA_REG_LBA_HI);
  if (lba_mid != 0 || lba_hi != 0)
    return false; // Not ATA

  // Wait for DRQ or ERR
  if (ata_wait_drq(io) < 0)
    return false;

  // Read 256 words of identification data
  uint16_t identify_data[256];
  insw(io + ATA_REG_DATA, identify_data, 256);

  // Extract total sectors (LBA28 at words 60-61, LBA48 at words 100-103)
  uint64_t lba48_sectors = (uint64_t)identify_data[100] |
                           ((uint64_t)identify_data[101] << 16) |
                           ((uint64_t)identify_data[102] << 32) |
                           ((uint64_t)identify_data[103] << 48);

  uint32_t lba28_sectors =
      (uint32_t)identify_data[60] | ((uint32_t)identify_data[61] << 16);

  drive->total_sectors = lba48_sectors ? lba48_sectors : lba28_sectors;

  // Extract model string (words 27-46, byte-swapped)
  for (int i = 0; i < 20; i++) {
    drive->model[i * 2] = (char)(identify_data[27 + i] >> 8);
    drive->model[i * 2 + 1] = (char)(identify_data[27 + i] & 0xFF);
  }
  drive->model[40] = '\0';

  // Trim trailing spaces from model
  for (int i = 39; i >= 0; i--) {
    if (drive->model[i] == ' ')
      drive->model[i] = '\0';
    else
      break;
  }

  drive->present = true;
  return true;
}

static bool ata_identify(struct ata_drive *drive) {
  spinlock_acquire(&ata_lock);
  bool ret = ata_identify_unlocked(drive);
  spinlock_release(&ata_lock);
  return ret;
}

// PIO Read

static int ata_pio_read_unlocked(struct block_device *dev, uint64_t lba, uint32_t count,
                                 void *buf) {
  struct ata_drive *drive = (struct ata_drive *)dev->driver_data;
  uint16_t io = drive->io_base;

  uint8_t *ptr = (uint8_t *)buf;

  for (uint32_t s = 0; s < count; s++) {
    uint64_t sector = lba + s;

    // Wait for drive to be ready
    if (ata_wait_bsy(io) < 0)
      return -1;

    // Select drive + LBA mode + top 4 bits of LBA
    outb(io + ATA_REG_DRIVE,
         0xE0 | (drive->slave << 4) | ((sector >> 24) & 0x0F));
    ata_400ns_delay(drive->ctrl_base);
    outb(io + ATA_REG_SECCOUNT, 1);
    outb(io + ATA_REG_LBA_LO, (uint8_t)(sector & 0xFF));
    outb(io + ATA_REG_LBA_MID, (uint8_t)((sector >> 8) & 0xFF));
    outb(io + ATA_REG_LBA_HI, (uint8_t)((sector >> 16) & 0xFF));
    outb(io + ATA_REG_COMMAND, ATA_CMD_READ_PIO);
    ata_400ns_delay(drive->ctrl_base);

    // Wait for data
    if (ata_wait_drq(io) < 0)
      return -1;

    // Read 256 words (512 bytes)
    insw(io + ATA_REG_DATA, ptr, 256);
    ptr += 512;
  }

  return 0;
}

static int ata_pio_read(struct block_device *dev, uint64_t lba, uint32_t count,
                        void *buf) {
  spinlock_acquire(&ata_lock);
  int ret = ata_pio_read_unlocked(dev, lba, count, buf);
  spinlock_release(&ata_lock);
  return ret;
}

// PIO Write

static int ata_pio_write_unlocked(struct block_device *dev, uint64_t lba, uint32_t count,
                                  const void *buf) {
  struct ata_drive *drive = (struct ata_drive *)dev->driver_data;
  uint16_t io = drive->io_base;

  const uint8_t *ptr = (const uint8_t *)buf;

  for (uint32_t s = 0; s < count; s++) {
    uint64_t sector = lba + s;

    if (ata_wait_bsy(io) < 0)
      return -1;

    outb(io + ATA_REG_DRIVE,
         0xE0 | (drive->slave << 4) | ((sector >> 24) & 0x0F));
    ata_400ns_delay(drive->ctrl_base);
    outb(io + ATA_REG_SECCOUNT, 1);
    outb(io + ATA_REG_LBA_LO, (uint8_t)(sector & 0xFF));
    outb(io + ATA_REG_LBA_MID, (uint8_t)((sector >> 8) & 0xFF));
    outb(io + ATA_REG_LBA_HI, (uint8_t)((sector >> 16) & 0xFF));
    outb(io + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);
    ata_400ns_delay(drive->ctrl_base);

    if (ata_wait_drq(io) < 0)
      return -1;

    // Write 256 words (512 bytes)
    outsw(io + ATA_REG_DATA, ptr, 256);
    ptr += 512;
  }

  return 0;
}

static int ata_pio_write(struct block_device *dev, uint64_t lba, uint32_t count,
                         const void *buf) {
  spinlock_acquire(&ata_lock);
  int ret = ata_pio_write_unlocked(dev, lba, count, buf);
  spinlock_release(&ata_lock);
  return ret;
}

// Initialization

static void ata_probe_channel(uint16_t io_base, uint16_t ctrl_base,
                              int drive_index_start) {
  for (int slave = 0; slave <= 1; slave++) {
    struct ata_drive *drive = &ata_drives[drive_index_start + slave];
    drive->io_base = io_base;
    drive->ctrl_base = ctrl_base;
    drive->slave = (uint8_t)slave;
    drive->present = false;

    if (ata_identify(drive)) {
      uint64_t size_mb = (drive->total_sectors * 512) / (1024 * 1024);

      console_puts("     ata");
      console_putchar('0' + ata_drive_count);
      console_puts(": ");
      console_puts(drive->model);
      console_puts(" (");
      print_uint64(size_mb);
      console_puts(" MB, ");
      print_uint64(drive->total_sectors);
      console_puts(" sectors)\n");

      // Set up block device
      struct block_device *blk = &drive->blkdev;
      blk->name[0] = 'a';
      blk->name[1] = 't';
      blk->name[2] = 'a';
      blk->name[3] = '0' + ata_drive_count;
      blk->name[4] = '\0';
      blk->sector_size = 512;
      blk->total_sectors = drive->total_sectors;
      blk->read_sectors = ata_pio_read;
      blk->write_sectors = ata_pio_write;
      blk->driver_data = drive;

      block_register(blk);
      ata_drive_count++;
    }
  }
}

int ata_init(void) {
  ata_drive_count = 0;
  console_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
                              " Probing ATA drives...\n");

  // Probe primary channel (master + slave)
  ata_probe_channel(ATA_PRIMARY_IO, ATA_PRIMARY_CTRL, 0);

  // Probe secondary channel (master + slave)
  ata_probe_channel(ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, 2);

  if (ata_drive_count == 0) {
    console_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET
                              " No ATA drives detected.\n");
  } else {
    console_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " ATA: ");
    print_uint64(ata_drive_count);
    console_puts(" drive(s) registered.\n");
  }
  return ata_drive_count;
}
