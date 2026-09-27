#ifndef HPET_H
#define HPET_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// HPET ACPI Table
struct acpi_hpet {
    char signature[4];           // "HPET"
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
    // HPET-specific fields
    uint8_t hardware_rev_id;
    uint8_t comparator_count:5;
    uint8_t counter_size:1;
    uint8_t reserved:1;
    uint8_t legacy_replacement:1;
    uint16_t pci_vendor_id;
    struct {
        uint8_t space_id;         // 0 = system memory, 1 = I/O space
        uint8_t register_bit_width;
        uint8_t register_bit_offset;
        uint8_t reserved;
        uint64_t address;
    } __attribute__((packed)) base_address;
    uint8_t hpet_number;
    uint16_t minimum_tick;
    uint8_t page_protection;
} __attribute__((packed));

// HPET MMIO Registers
// General Capabilities and ID Register (offset 0x00)
#define HPET_CAP_COUNTER_CLK_PERIOD_MASK   0xFFFFFFFF00000000ULL
#define HPET_CAP_COUNTER_CLK_PERIOD_SHIFT  32
#define HPET_CAP_VENDOR_ID_MASK            0x00000000FFFF0000ULL
#define HPET_CAP_VENDOR_ID_SHIFT           16
#define HPET_CAP_LEGACY_ROUTE_CAP          (1 << 15)
#define HPET_CAP_COUNT_SIZE_CAP            (1 << 13)  // 1 = 64-bit, 0 = 32-bit
#define HPET_CAP_NUM_TIM_CAP_MASK          0x0000000000001F00ULL
#define HPET_CAP_NUM_TIM_CAP_SHIFT         8
#define HPET_CAP_REV_ID_MASK               0x00000000000000FFULL

// General Configuration Register (offset 0x10)
#define HPET_CFG_ENABLE                    (1 << 0)   // Overall enable
#define HPET_CFG_LEGACY_ROUTE              (1 << 1)   // Legacy replacement route

// General Status Register (offset 0x20)
#define HPET_STATUS_T0_INT_STS             (1 << 0)
#define HPET_STATUS_T1_INT_STS             (1 << 1)
#define HPET_STATUS_T2_INT_STS             (1 << 2)
// ... up to T31

// Main Counter Value Register (offset 0xF0)
// 64-bit (or 32-bit if 32-bit mode) counter

// Timer N Configuration and Capability Register (offset 0x100 + 0x20 * N)
#define HPET_TN_INT_ROUTE_CAP_MASK         0xFFFFFFFF00000000ULL
#define HPET_TN_INT_ROUTE_CAP_SHIFT        32
#define HPET_TN_FSB_INT_DEL_CAP            (1 << 15)
#define HPET_TN_FSB_EN_CNF                 (1 << 14)
#define HPET_TN_FSB_INT_ADDR_CAP           (1 << 13)
#define HPET_TN_INT_ROUTE_CNF_MASK         0x00003E0000000000ULL
#define HPET_TN_INT_ROUTE_CNF_SHIFT        41
#define HPET_TN_32MODE_CNF                 (1 << 8)
#define HPET_TN_VAL_SET_CNF                (1 << 6)
#define HPET_TN_SIZE_CAP                   (1 << 5)   // 1 = 64-bit, 0 = 32-bit
#define HPET_TN_PER_INT_CAP                (1 << 4)   // Periodic interrupt capable
#define HPET_TN_TYPE_CNF                   (1 << 3)   // 1 = periodic, 0 = one-shot
#define HPET_TN_INT_ENB_CNF                (1 << 2)   // Interrupt enable
#define HPET_TN_INT_TYPE_CNF               (1 << 1)   // 1 = level, 0 = edge

// Timer N Comparator Value Register (offset 0x108 + 0x20 * N)
// Timer N FSB Interrupt Route Register (offset 0x110 + 0x20 * N)

// Register Offsets
#define HPET_REG_CAP_ID        0x00
#define HPET_REG_CONFIG        0x10
#define HPET_REG_ISR           0x20
#define HPET_REG_MAIN_COUNTER  0xF0
#define HPET_REG_TIMER_CONF(n) (0x100 + (n) * 0x20)
#define HPET_REG_TIMER_COMP(n) (0x108 + (n) * 0x20)
#define HPET_REG_TIMER_FSB(n)  (0x110 + (n) * 0x20)

// HPET Timer Configuration
#define HPET_TIMER_VECTOR      49    // Must not collide with LAPIC_TIMER_VECTOR (48)
#define HPET_DEFAULT_FREQUENCY 1000  // 1 KHz = 1ms per tick

// Public API

// Initialize HPET from ACPI table. Returns true if HPET is available.
bool hpet_init(void);

// Returns true if HPET is present and initialized.
bool hpet_is_available(void);

// Read the main counter value (64-bit or 32-bit depending on hardware).
uint64_t hpet_read_counter(void);

// Get the counter frequency in Hz (derived from clk_period).
uint64_t hpet_get_frequency(void);

// Get the number of comparator timers available.
uint8_t hpet_get_timer_count(void);

// Configure a timer for periodic interrupts.
// timer_id: 0 to (timer_count - 1)
// frequency: desired interrupt frequency in Hz
// Returns true on success.
bool hpet_configure_timer_periodic(uint8_t timer_id, uint32_t frequency);

// Configure a timer for one-shot interrupt.
// timer_id: 0 to (timer_count - 1)
// delay_us: delay in microseconds before interrupt
// Returns true on success.
bool hpet_configure_timer_oneshot(uint8_t timer_id, uint32_t delay_us);

// Enable/disable a specific timer interrupt.
void hpet_enable_timer(uint8_t timer_id, bool enable);

// Enable/disable HPET main counter.
void hpet_enable(bool enable);

// Enable/disable legacy replacement route (IRQ0 -> Timer 0, IRQ8 -> Timer 1).
void hpet_set_legacy_mode(bool enable);

// Get ticks since HPET timer started (for uptime/sleep).
uint64_t hpet_get_ticks(void);

// Get uptime in milliseconds.
uint64_t hpet_get_ms(void);

// Sleep for approximately `ms` milliseconds using HPET.
void hpet_sleep(uint32_t ms);

// Watchdog Capability

// Configure a timer as a watchdog. The watchdog will fire if not petted
// within the specified timeout. Useful for detecting kernel hangs.
// timer_id: timer to use as watchdog
// timeout_ms: watchdog timeout in milliseconds
// Returns true on success.
bool hpet_watchdog_configure(uint8_t timer_id, uint32_t timeout_ms);

// Pet the watchdog (reset the countdown).
void hpet_watchdog_pet(void);

// Disable the watchdog.
void hpet_watchdog_disable(void);

// Check if watchdog is active.
bool hpet_watchdog_is_active(void);

// Backup Timer Support

// Returns true if HPET can be used as a backup for LAPIC timer.
bool hpet_is_backup_available(void);

// Switch to HPET as the primary system timer (fallback mode).
// Called if LAPIC timer fails.
void hpet_fallback_activate(void);

#endif
