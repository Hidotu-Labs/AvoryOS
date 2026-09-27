#ifndef DRIVERS_STORAGE_AHCI_H
#define DRIVERS_STORAGE_AHCI_H

#include <stdint.h>
#include <stdbool.h>

#define SATA_SIG_ATA    0x00000101
#define SATA_SIG_ATAPI  0xEB140101
#define SATA_SIG_SEMB   0xC33C0101
#define SATA_SIG_PM     0x96690101

#define AHCI_PORT_DET_PRESENT    3
#define AHCI_PORT_IPM_ACTIVE     1

#define AHCI_CMD_ST   (1 << 0)
#define AHCI_CMD_FRE  (1 << 4)
#define AHCI_CMD_FR   (1 << 14)
#define AHCI_CMD_CR   (1 << 15)

// Frame Information Structure type
typedef enum {
    FIS_TYPE_REG_H2D    = 0x27, // Register FIS - host to device
    FIS_TYPE_REG_D2H    = 0x34, // Register FIS - device to host
    FIS_TYPE_DMA_ACT    = 0x39, // DMA activate FIS - device to host
    FIS_TYPE_DMA_SETUP  = 0x41, // DMA setup FIS - bidirectional
    FIS_TYPE_DATA       = 0x46, // Data FIS - bidirectional
    FIS_TYPE_BIST       = 0x58, // BIST activate FIS - bidirectional
    FIS_TYPE_PIO_SETUP  = 0x5F, // PIO setup FIS - device to host
    FIS_TYPE_DEV_BITS   = 0xA1, // Set device bits FIS - device to host
} fis_type_t;

// Host to Device Register FIS
typedef struct {
    uint8_t  fis_type;  // FIS_TYPE_REG_H2D
    uint8_t  pmport:4;  // Port multiplier
    uint8_t  rsv0:3;    // Reserved
    uint8_t  c:1;       // 1: Command, 0: Control
    uint8_t  command;   // Command register
    uint8_t  featurel;  // Feature register, 7:0
    uint8_t  lba0;      // LBA low register, 7:0
    uint8_t  lba1;      // LBA mid register, 15:8
    uint8_t  lba2;      // LBA high register, 23:16
    uint8_t  device;    // Device register
    uint8_t  lba3;      // LBA register, 31:24
    uint8_t  lba4;      // LBA register, 39:32
    uint8_t  lba5;      // LBA register, 47:40
    uint8_t  featureh;  // Feature register, 15:8
    uint8_t  countl;    // Count register, 7:0
    uint8_t  counth;    // Count register, 15:8
    uint8_t  icc;       // Isochronous command completion
    uint8_t  control;   // Control register
    uint8_t  rsv1[4];   // Reserved
} __attribute__((packed)) fis_reg_h2d_t;

// AHCI Command Header (32 bytes)
typedef struct {
    uint8_t  cfl:5;     // Command FIS length in DWORDS, 2~16
    uint8_t  a:1;       // ATAPI
    uint8_t  w:1;       // Write, 1: H2D, 0: D2H
    uint8_t  p:1;       // Prefetchable
    uint8_t  r:1;       // Reset
    uint8_t  b:1;       // BIST
    uint8_t  c:1;       // Clear busy upon R_OK
    uint8_t  rsv0:1;    // Reserved
    uint8_t  pmp:4;     // Port multiplier port
    uint16_t prdtl;     // Physical region descriptor table length in entries
    volatile uint32_t prdbc; // Physical region descriptor byte count transferred
    uint32_t ctba;      // Command table descriptor base address
    uint32_t ctbau;     // Command table descriptor base address upper 32 bits
    uint32_t rsv1[4];   // Reserved
} __attribute__((packed)) ahci_command_header_t;

// Physical Region Descriptor Table Entry
typedef struct {
    uint32_t dba;       // Data base address
    uint32_t dbau;      // Data base address upper 32 bits
    uint32_t rsv0;      // Reserved
    uint32_t dbc:22;    // Byte count, 4M max
    uint32_t rsv1:9;    // Reserved
    uint32_t i:1;       // Interrupt on completion
} __attribute__((packed)) ahci_prdt_entry_t;

// AHCI Command Table
#define AHCI_PRDT_MAX_ENTRIES 8
#define AHCI_PRDT_MAX_BYTES (4u * 1024 * 1024) /* 22-bit DBC field */

typedef struct {
    uint8_t  cfis[64];  // Command FIS
    uint8_t  acmd[16];  // ATAPI command, 12 or 16 bytes
    uint8_t  rsv[48];   // Reserved
    ahci_prdt_entry_t prdt_entry[AHCI_PRDT_MAX_ENTRIES];
} __attribute__((packed)) ahci_command_table_t;

// AHCI Port Registers
typedef struct {
    volatile uint32_t clb;       // 0x00, command list base address, 1K-byte aligned
    volatile uint32_t clbu;      // 0x04, command list base address upper 32 bits
    volatile uint32_t fb;        // 0x08, FIS base address, 256-byte aligned
    volatile uint32_t fbu;       // 0x0C, FIS base address upper 32 bits
    volatile uint32_t is;        // 0x10, interrupt status
    volatile uint32_t ie;        // 0x14, interrupt enable
    volatile uint32_t cmd;       // 0x18, command and status
    volatile uint32_t rsv0;      // 0x1C, Reserved
    volatile uint32_t tfd;       // 0x20, task file data
    volatile uint32_t sig;       // 0x24, signature
    volatile uint32_t ssts;      // 0x28, SATA status (SCR0:SStatus)
    volatile uint32_t sctl;      // 0x2C, SATA control (SCR2:SControl)
    volatile uint32_t serr;      // 0x30, SATA error (SCR1:SError)
    volatile uint32_t sact;      // 0x34, SATA active (SCR3:SActive)
    volatile uint32_t ci;        // 0x38, command issue
    volatile uint32_t sntf;      // 0x3C, SATA notification (SCR4:SNotification)
    volatile uint32_t fbs;       // 0x40, FIS-based switch control
    volatile uint32_t rsv1[11];  // 0x44 ~ 0x6F, Reserved
    volatile uint32_t vendor[4]; // 0x70 ~ 0x7F, vendor specific
} ahci_port_t;

// AHCI HBA Memory Space (ABAR)
typedef struct {
    volatile uint32_t cap;       // 0x00, Host capability
    volatile uint32_t ghc;       // 0x04, Global host control
    volatile uint32_t is;        // 0x08, Interrupt status
    volatile uint32_t pi;        // 0x0C, Port implemented
    volatile uint32_t vs;        // 0x10, Version
    volatile uint32_t ccc_ctl;   // 0x14, Command completion coalescing control
    volatile uint32_t ccc_pts;   // 0x18, Command completion coalescing ports
    volatile uint32_t em_loc;    // 0x1C, Enclosure management location
    volatile uint32_t em_ctl;    // 0x20, Enclosure management control
    volatile uint32_t cap2;      // 0x24, Host capabilities extended
    volatile uint32_t bohc;      // 0x28, BIOS/OS handoff control and status
    volatile uint8_t  rsv[0xA0-0x2C]; // 0x2C ~ 0x9F, Reserved
    volatile uint8_t  vendor[0x100-0xA0]; // 0xA0 ~ 0xFF, Vendor specific registers
    ahci_port_t ports[32];       // 0x100 ~ 0x10FF, Port control registers
} ahci_hba_mem_t;

int ahci_init(void);

#endif
