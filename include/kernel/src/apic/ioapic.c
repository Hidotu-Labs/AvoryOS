#include "apic/ioapic.h"
#include "mm/pmm.h"
#include "console/console.h"

// MMIO layout
// The I/O APIC uses an indirect register scheme:
//   Offset 0x00  →  IOREGSEL  (index register, 32-bit)
//   Offset 0x10  →  IOWIN     (data window,    32-bit)
//
// We cast the physical base (+ HHDM) to a volatile uint32_t* and index:
//   [0]  = IOREGSEL  (byte offset 0x00 / 4)
//   [4]  = IOWIN     (byte offset 0x10 / 4)

static volatile uint32_t *ioapic_base = NULL;
static uint32_t ioapic_gsi_base = 0;

bool ioapic_is_ready(void) {
    return ioapic_base != NULL;
}

// Helper: print 32-bit hex
static void print_hex32(uint32_t num) {
    const char *hex = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4) {
        console_putchar(hex[(num >> i) & 0xF]);
    }
}

static void print_uint32(uint32_t num) {
    if (num == 0) { console_putchar('0'); return; }
    char buf[10];
    int i = 0;
    while (num > 0) { buf[i++] = '0' + (num % 10); num /= 10; }
    while (i > 0) { console_putchar(buf[--i]); }
}

// Indirect register read/write

static uint32_t ioapic_read(uint8_t reg) {
    ioapic_base[0] = (uint32_t)reg;   // IOREGSEL
    return ioapic_base[4];            // IOWIN
}

static void ioapic_write(uint8_t reg, uint32_t value) {
    ioapic_base[0] = (uint32_t)reg;   // IOREGSEL
    ioapic_base[4] = value;           // IOWIN
}

// Max redirection entries
static uint32_t ioapic_get_max_redirections(void) {
    return ((ioapic_read(IOAPIC_REG_VER) >> 16) & 0xFF) + 1;
}

// Route a GSI through the I/O APIC

void ioapic_route_irq(uint8_t gsi, uint8_t vector, uint8_t dest_apic_id,
                      uint16_t flags) {
    uint32_t entry_index = gsi - ioapic_gsi_base;

    // Start with the destination vector
    uint64_t entry = (uint64_t)vector;

    // Decode the MADT ISO polarity field (bits 1:0 of flags)
    //   0b00 = conform to bus (ISA = active high)
    //   0b01 = active high
    //   0b11 = active low
    uint8_t polarity = flags & 0x03;
    if (polarity == 0x03) {
        entry |= IOAPIC_REDIR_ACTIVELOW;
    }

    // Decode the MADT ISO trigger mode field (bits 3:2 of flags)
    //   0b00 = conform to bus (ISA = edge)
    //   0b01 = edge triggered
    //   0b11 = level triggered
    uint8_t trigger = (flags >> 2) & 0x03;
    if (trigger == 0x03) {
        entry |= IOAPIC_REDIR_LEVEL;
    }

    // Physical destination mode, fixed delivery — destination in bits 56-63
    entry |= ((uint64_t)dest_apic_id << 56);

    // Write high 32 bits first (includes destination), then low 32 bits
    uint8_t reg_low  = IOAPIC_REG_REDTBL + (entry_index * 2);
    uint8_t reg_high = IOAPIC_REG_REDTBL + (entry_index * 2) + 1;

    ioapic_write(reg_high, (uint32_t)(entry >> 32));
    ioapic_write(reg_low,  (uint32_t)(entry & 0xFFFFFFFF));
}

// Mask / unmask individual GSI pins

void ioapic_mask_irq(uint8_t gsi) {
    uint32_t entry_index = gsi - ioapic_gsi_base;
    uint8_t reg_low = IOAPIC_REG_REDTBL + (entry_index * 2);
    uint32_t low = ioapic_read(reg_low);
    low |= (1 << 16);   // set mask bit
    ioapic_write(reg_low, low);
}

void ioapic_unmask_irq(uint8_t gsi) {
    uint32_t entry_index = gsi - ioapic_gsi_base;
    uint8_t reg_low = IOAPIC_REG_REDTBL + (entry_index * 2);
    uint32_t low = ioapic_read(reg_low);
    low &= ~(1 << 16);  // clear mask bit
    ioapic_write(reg_low, low);
}

// Initialization

void ioapic_init(uint64_t base_phys, uint32_t gsi_base) {
    ioapic_base     = (volatile uint32_t *)(base_phys + pmm_get_hhdm_offset());
    ioapic_gsi_base = gsi_base;

    uint32_t id         = (ioapic_read(IOAPIC_REG_ID) >> 24) & 0xF;
    uint32_t max_redir  = ioapic_get_max_redirections();

    console_puts("[OK] I/O APIC ID: 0x");
    print_hex32(id);
    console_puts(", Max Redirections: ");
    print_uint32(max_redir);
    console_puts(", GSI Base: ");
    print_uint32(gsi_base);
    console_puts("\n");

    // Mask all redirection entries by default
    for (uint32_t i = 0; i < max_redir; i++) {
        ioapic_mask_irq(gsi_base + i);
    }
}
