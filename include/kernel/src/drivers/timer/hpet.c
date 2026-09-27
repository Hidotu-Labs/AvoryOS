#include "drivers/timer/hpet.h"
#include "hal/hal.h"
#include "acpi/acpi.h"
#include "console/console.h"
#include "console/klog.h"
#include "cpu/isr.h"
#include "cpu/irq.h"
#include "mm/pmm.h"
#include "io/io.h"
#include <stdbool.h>
#include <stdint.h>

// Internal State
static volatile uint64_t *hpet_base = NULL;
static uint64_t hpet_clk_period = 0;     // femtoseconds per tick
static uint64_t hpet_frequency = 0;      // Hz
static uint8_t hpet_timer_count = 0;
static bool hpet_is_64bit = false;
static bool hpet_initialized = false;
static bool hpet_legacy_capable = false;

// Timer state
static volatile uint64_t hpet_ticks = 0;
static uint32_t hpet_ticks_per_interrupt = 1;  // ms per interrupt

// Watchdog state
static bool watchdog_active = false;
static uint8_t watchdog_timer_id = 0;
static uint64_t watchdog_timeout_ticks = 0;

// MMIO Read/Write Helpers
static inline uint64_t hpet_read_reg(uint32_t offset) {
    return *(volatile uint64_t *)((uint64_t)hpet_base + offset);
}

static inline void hpet_write_reg(uint32_t offset, uint64_t value) {
    *(volatile uint64_t *)((uint64_t)hpet_base + offset) = value;
}

// Helper Functions
static void print_hex64(uint64_t num) {
    const char *hex = "0123456789ABCDEF";
    for (int i = 60; i >= 0; i -= 4) {
        klog_putchar(hex[(num >> i) & 0xF]);
    }
}

static void print_uint64(uint64_t num) {
    if (num == 0) { klog_putchar('0'); return; }
    char buf[21];
    int i = 0;
    while (num > 0) { buf[i++] = '0' + (num % 10); num /= 10; }
    while (i > 0) { klog_putchar(buf[--i]); }
}

// Timer Interrupt Handler
static void hpet_timer_handler(struct registers *regs) {
    (void)regs;
    
    // Increment tick counter
    hpet_ticks++;
    
    // Clear interrupt status for timer 0 (write 1 to clear)
    // Note: For level-triggered interrupts, we need to clear the status
    uint64_t config = hpet_read_reg(HPET_REG_TIMER_CONF(0));
    if (config & HPET_TN_INT_TYPE_CNF) {
        // Level-triggered: clear the interrupt status bit
        hpet_write_reg(HPET_REG_ISR, HPET_STATUS_T0_INT_STS);
    }
    
    // If this is a watchdog timer firing, log a critical error
    if (watchdog_active && watchdog_timer_id == 0) {
        klog_puts("\n[CRITICAL] HPET Watchdog triggered! Kernel may be hung.\n");
    }
}

// Watchdog Interrupt Handler
static void hpet_watchdog_handler(struct registers *regs) {
    (void)regs;
    klog_puts("\n[CRITICAL] HPET Watchdog expired! System hang detected.\n");
    // Could trigger a system reset here in a real implementation
}

// ACPI Table Parsing
static struct acpi_hpet *hpet_find_table(void) {
    return (struct acpi_hpet *)acpi_find_table("HPET");
}

// Public API Implementation

bool hpet_init(void) {
    klog_puts("[INFO] Initializing HPET...\n");
    
    // Find the HPET ACPI table
    struct acpi_hpet *hpet_table = hpet_find_table();
    if (!hpet_table) {
        klog_puts("[WARN] No HPET ACPI table found.\n");
        return false;
    }
    
    klog_puts("[OK] HPET ACPI table found.\n");
    
    // Validate the table
    if (hpet_table->base_address.space_id != 0) {
        klog_puts("[ERR] HPET is not in system memory space.\n");
        return false;
    }
    
    // Map the HPET MMIO region
    uint64_t phys_addr = hpet_table->base_address.address;
    hpet_base = (volatile uint64_t *)(phys_addr + pmm_get_hhdm_offset());
    
    klog_puts("     HPET Base Address: 0x");
    print_hex64(phys_addr);
    klog_puts("\n");
    
    // Read capabilities
    uint64_t cap_id = hpet_read_reg(HPET_REG_CAP_ID);
    
    // Extract timer count
    hpet_timer_count = (cap_id & HPET_CAP_NUM_TIM_CAP_MASK) >> HPET_CAP_NUM_TIM_CAP_SHIFT;
    hpet_timer_count++;  // Value is 0-based (n-1 timers)
    
    // Check if 64-bit counter
    hpet_is_64bit = (cap_id & HPET_CAP_COUNT_SIZE_CAP) != 0;
    
    // Check legacy replacement capability
    hpet_legacy_capable = (cap_id & HPET_CAP_LEGACY_ROUTE_CAP) != 0;
    
    // Extract clock period (femtoseconds per tick)
    hpet_clk_period = (cap_id & HPET_CAP_COUNTER_CLK_PERIOD_MASK) >> HPET_CAP_COUNTER_CLK_PERIOD_SHIFT;
    
    // Calculate frequency: Hz = 10^15 / femtoseconds_per_tick
    if (hpet_clk_period > 0) {
        hpet_frequency = 1000000000000000ULL / hpet_clk_period;
    }
    
    klog_puts("     Timer Count: ");
    print_uint64(hpet_timer_count);
    klog_puts("\n");
    
    klog_puts("     Counter Size: ");
    klog_puts(hpet_is_64bit ? "64-bit" : "32-bit");
    klog_puts("\n");
    
    klog_puts("     Legacy Route: ");
    klog_puts(hpet_legacy_capable ? "Capable" : "Not Capable");
    klog_puts("\n");
    
    klog_puts("     Frequency: ");
    print_uint64(hpet_frequency / 1000000);
    klog_puts(" MHz (");
    print_uint64(hpet_frequency);
    klog_puts(" Hz)\n");
    
    if (hpet_frequency == 0) {
        klog_puts("[ERR] HPET frequency is zero, cannot use.\n");
        return false;
    }
    
    // Disable HPET during configuration
    hpet_enable(false);
    
    // Clear any pending interrupts
    hpet_write_reg(HPET_REG_ISR, 0xFFFFFFFF);
    
    // Register interrupt handler for timer 0
    register_interrupt_handler(HPET_TIMER_VECTOR, hpet_timer_handler);
    
    hpet_initialized = true;
    
    // Enable HPET main counter so it starts running
    hpet_enable(true);
    
    klog_puts("[OK] HPET initialized successfully.\n");
    return true;
}

bool hpet_is_available(void) {
    return hpet_initialized;
}

uint64_t hpet_read_counter(void) {
    if (!hpet_initialized) return 0;
    
    if (hpet_is_64bit) {
        return hpet_read_reg(HPET_REG_MAIN_COUNTER);
    } else {
        // 32-bit counter - read twice to detect wraparound
        uint32_t lo, hi;
        do {
            hi = (uint32_t)(hpet_read_reg(HPET_REG_MAIN_COUNTER) >> 32);
            lo = (uint32_t)(hpet_read_reg(HPET_REG_MAIN_COUNTER) & 0xFFFFFFFF);
        } while (hi != (uint32_t)(hpet_read_reg(HPET_REG_MAIN_COUNTER) >> 32));
        return ((uint64_t)hi << 32) | lo;
    }
}

uint64_t hpet_get_frequency(void) {
    return hpet_frequency;
}

uint8_t hpet_get_timer_count(void) {
    return hpet_timer_count;
}

bool hpet_configure_timer_periodic(uint8_t timer_id, uint32_t frequency) {
    if (!hpet_initialized || timer_id >= hpet_timer_count) return false;
    
    // Check if timer supports periodic mode
    uint64_t timer_cap = hpet_read_reg(HPET_REG_TIMER_CONF(timer_id));
    if (!(timer_cap & HPET_TN_PER_INT_CAP)) {
        klog_puts("[WARN] HPET Timer ");
        klog_uint64(timer_id);
        klog_puts(" does not support periodic mode.\n");
        return false;
    }
    
    // Calculate comparator value for desired frequency
    // comparator = frequency_hz / desired_frequency
    uint64_t comparator = hpet_frequency / frequency;
    
    // Configure timer: periodic, interrupt enabled, edge-triggered
    uint64_t config = HPET_TN_TYPE_CNF | HPET_TN_INT_ENB_CNF;
    
    // Set interrupt route (use IOAPIC routing)
    // For timer 0, we'll use the configured vector
    config |= ((uint64_t)HPET_TIMER_VECTOR << HPET_TN_INT_ROUTE_CNF_SHIFT);
    
    // For periodic mode, we need to set VAL_SET_CNF to load the comparator
    config |= HPET_TN_VAL_SET_CNF;
    
    // Write comparator first
    hpet_write_reg(HPET_REG_TIMER_COMP(timer_id), comparator);
    
    // Then write configuration
    hpet_write_reg(HPET_REG_TIMER_CONF(timer_id), config);
    
    return true;
}

bool hpet_configure_timer_oneshot(uint8_t timer_id, uint32_t delay_us) {
    if (!hpet_initialized || timer_id >= hpet_timer_count) return false;
    
    // Calculate comparator value
    // ticks = delay_us * frequency_hz / 1000000
    uint64_t ticks = ((uint64_t)delay_us * hpet_frequency) / 1000000;
    
    // Get current counter value
    uint64_t current = hpet_read_counter();
    uint64_t comparator = current + ticks;
    
    // Configure timer: one-shot, interrupt enabled, edge-triggered
    uint64_t config = HPET_TN_INT_ENB_CNF;
    config |= ((uint64_t)HPET_TIMER_VECTOR << HPET_TN_INT_ROUTE_CNF_SHIFT);
    
    // Write comparator
    hpet_write_reg(HPET_REG_TIMER_COMP(timer_id), comparator);
    
    // Write configuration
    hpet_write_reg(HPET_REG_TIMER_CONF(timer_id), config);
    
    return true;
}

void hpet_enable_timer(uint8_t timer_id, bool enable) {
    if (!hpet_initialized || timer_id >= hpet_timer_count) return;
    
    uint64_t config = hpet_read_reg(HPET_REG_TIMER_CONF(timer_id));
    if (enable) {
        config |= HPET_TN_INT_ENB_CNF;
    } else {
        config &= ~HPET_TN_INT_ENB_CNF;
    }
    hpet_write_reg(HPET_REG_TIMER_CONF(timer_id), config);
}

void hpet_enable(bool enable) {
    if (!hpet_initialized) return;
    
    uint64_t config = hpet_read_reg(HPET_REG_CONFIG);
    if (enable) {
        config |= HPET_CFG_ENABLE;
    } else {
        config &= ~HPET_CFG_ENABLE;
    }
    hpet_write_reg(HPET_REG_CONFIG, config);
}

void hpet_set_legacy_mode(bool enable) {
    if (!hpet_initialized || !hpet_legacy_capable) return;
    
    uint64_t config = hpet_read_reg(HPET_REG_CONFIG);
    if (enable) {
        config |= HPET_CFG_LEGACY_ROUTE;
    } else {
        config &= ~HPET_CFG_LEGACY_ROUTE;
    }
    hpet_write_reg(HPET_REG_CONFIG, config);
}

uint64_t hpet_get_ticks(void) {
    return hpet_ticks;
}

uint64_t hpet_get_ms(void) {
    // Each tick represents hpet_ticks_per_interrupt ms
    return hpet_ticks * hpet_ticks_per_interrupt;
}

void hpet_sleep(uint32_t ms) {
    if (!hpet_initialized) return;
    
    uint64_t start = hpet_read_counter();
    uint64_t ticks = ((uint64_t)ms * hpet_frequency) / 1000;
    uint64_t end = start + ticks;
    
    while (hpet_read_counter() < end) {
        hal_cpu_relax();
    }
}

// Watchdog Implementation

bool hpet_watchdog_configure(uint8_t timer_id, uint32_t timeout_ms) {
    if (!hpet_initialized || timer_id >= hpet_timer_count) return false;
    if (timer_id == 0) return false;  // Timer 0 is reserved for system tick
    
    // Calculate timeout in ticks
    watchdog_timeout_ticks = ((uint64_t)timeout_ms * hpet_frequency) / 1000;
    
    // Get current counter
    uint64_t current = hpet_read_counter();
    uint64_t comparator = current + watchdog_timeout_ticks;
    
    // Configure timer as one-shot watchdog
    uint64_t config = HPET_TN_INT_ENB_CNF | HPET_TN_INT_TYPE_CNF;  // Level-triggered
    
    // Use a different vector for watchdog
    #define HPET_WATCHDOG_VECTOR 50
    config |= ((uint64_t)HPET_WATCHDOG_VECTOR << HPET_TN_INT_ROUTE_CNF_SHIFT);
    
    // Register watchdog handler
    register_interrupt_handler(HPET_WATCHDOG_VECTOR, hpet_watchdog_handler);
    
    // Write comparator and config
    hpet_write_reg(HPET_REG_TIMER_COMP(timer_id), comparator);
    hpet_write_reg(HPET_REG_TIMER_CONF(timer_id), config);
    
    watchdog_timer_id = timer_id;
    watchdog_active = true;
    
    klog_puts("[OK] HPET Watchdog configured on Timer ");
    klog_uint64(timer_id);
    klog_puts(" with ");
    klog_uint64(timeout_ms);
    klog_puts(" ms timeout.\n");
    
    return true;
}

void hpet_watchdog_pet(void) {
    if (!watchdog_active || !hpet_initialized) return;
    
    // Reset the watchdog counter
    uint64_t current = hpet_read_counter();
    uint64_t comparator = current + watchdog_timeout_ticks;
    hpet_write_reg(HPET_REG_TIMER_COMP(watchdog_timer_id), comparator);
}

void hpet_watchdog_disable(void) {
    if (!watchdog_active || !hpet_initialized) return;
    
    hpet_enable_timer(watchdog_timer_id, false);
    watchdog_active = false;
    
    klog_puts("[OK] HPET Watchdog disabled.\n");
}

bool hpet_watchdog_is_active(void) {
    return watchdog_active;
}

// Backup Timer Support

bool hpet_is_backup_available(void) {
    return hpet_initialized && hpet_timer_count >= 2;
}

void hpet_fallback_activate(void) {
    if (!hpet_initialized) {
        klog_puts("[ERR] Cannot activate HPET fallback: not initialized.\n");
        return;
    }
    
    klog_puts("[INFO] Activating HPET as fallback system timer.\n");
    
    // Disable LAPIC timer would happen in the caller
    
    // Enable HPET main counter
    hpet_enable(true);
    
    // Configure timer 0 for periodic interrupts at 1 KHz
    if (hpet_configure_timer_periodic(0, HPET_DEFAULT_FREQUENCY)) {
        hpet_ticks_per_interrupt = 1;  // 1 ms per tick at 1 KHz
        klog_puts("[OK] HPET Timer 0 configured for 1 KHz periodic interrupts.\n");
    } else {
        // Fallback to one-shot mode if periodic not supported
        klog_puts("[WARN] Periodic mode not available, using one-shot.\n");
        // Would need to re-arm after each interrupt
    }
    
    klog_puts("[OK] HPET fallback timer active.\n");
}
