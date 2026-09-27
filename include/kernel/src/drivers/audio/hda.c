#include "hda.h"
#include "audio_dsp.h"
#include "hal/hal.h"
#include "../../apic/ioapic.h"
#include "../../apic/lapic.h"
#include "../../apic/lapic_timer.h"
#include "../../console/console.h"
#include "../../console/klog.h"
#include "../../cpu/irq.h"
#include "../../cpu/isr.h"
#include "../../fb/framebuffer.h"
#include "../../fs/vfs.h"
#include "../../io/io.h"
#include "../../lib/string.h"
#include "../../mm/heap.h"
#include "../../mm/pmm.h"
#include "../../mm/vmm.h"
#include "../../sched/sched.h"
#include "../../sched/wait.h"
#include "../pci/pci.h"

// Number of cyclic DMA buffers and buffer size (16 cyclic buffers = 64KB DMA ring, ~370ms lookahead)
#define HDA_BDL_ENTRIES 16
#define HDA_BUFFER_SIZE 4096 // 4KB per buffer (~23.2ms @ 44.1kHz / ~21.3ms @ 48kHz 16-bit stereo)
#define HDA_RING_SIZE   (512 * 1024) // 512KB ring buffer (~3.0s under heavy load)

// Hardware PCI & Base MMIO
static struct pci_device *hda_pci = NULL;
static volatile uint8_t *hda_base = NULL;
static bool hda_present = false;
static uint8_t hda_iss = 0;  // Input Streams count
static uint8_t hda_oss = 0;  // Output Streams count
static uint32_t hda_stream_offset = 0; // Stream 0 MMIO offset

// Active Codec & Widget Route
static uint8_t active_codec = 0;
static uint8_t active_dac_nid = 0;
static uint8_t active_pin_nid = 0;

// Active Stream Format
static uint32_t current_sample_rate = 48000;
static uint8_t  current_channels = 2;
static uint8_t  current_bits = 16;

// DMA Structures
static struct hda_bdl_entry *hda_bdl = NULL;
static uint64_t hda_bdl_phys = 0;
static uint8_t *hda_buffers[HDA_BDL_ENTRIES];
static uint64_t hda_buffers_phys[HDA_BDL_ENTRIES];
static volatile uint8_t hda_refill_idx = 0;
static volatile uint32_t hda_underflow_count = 0;

// Kernel Audio Ring Buffer
static uint8_t *hda_ring = NULL;
static volatile uint32_t ring_head = 0;
static volatile uint32_t ring_tail = 0;
static volatile uint32_t ring_count = 0;
static volatile bool hda_is_playing = false;
static volatile uint64_t total_played_bytes = 0;
static volatile uint32_t total_played_blocks = 0;

// Wait Queue for blocking writes & poll
static wait_queue_t hda_wait_queue;

// MMIO Access Helpers
static inline uint8_t hda_read8(uint32_t reg) {
    return *(volatile uint8_t *)(hda_base + reg);
}

static inline uint16_t hda_read16(uint32_t reg) {
    return *(volatile uint16_t *)(hda_base + reg);
}

static inline uint32_t hda_read32(uint32_t reg) {
    return *(volatile uint32_t *)(hda_base + reg);
}

static inline void hda_write8(uint32_t reg, uint8_t val) {
    *(volatile uint8_t *)(hda_base + reg) = val;
}

static inline void hda_write16(uint32_t reg, uint16_t val) {
    *(volatile uint16_t *)(hda_base + reg) = val;
}

static inline void hda_write32(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(hda_base + reg) = val;
}

static inline uint8_t hda_sd_read8(uint32_t reg) {
    return *(volatile uint8_t *)(hda_base + hda_stream_offset + reg);
}

static inline uint16_t hda_sd_read16(uint32_t reg) {
    return *(volatile uint16_t *)(hda_base + hda_stream_offset + reg);
}

static inline uint32_t hda_sd_read32(uint32_t reg) {
    return *(volatile uint32_t *)(hda_base + hda_stream_offset + reg);
}

static inline void hda_sd_write8(uint32_t reg, uint8_t val) {
    *(volatile uint8_t *)(hda_base + hda_stream_offset + reg) = val;
}

static inline void hda_sd_write16(uint32_t reg, uint16_t val) {
    *(volatile uint16_t *)(hda_base + hda_stream_offset + reg) = val;
}

static inline void hda_sd_write32(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(hda_base + hda_stream_offset + reg) = val;
}

// Immediate Command Execution
static uint32_t hda_exec_verb(uint8_t codec, uint8_t nid, uint16_t verb, uint8_t param) {
    if (!hda_present && !hda_base)
        return 0;

    int timeout = 10000;
    while ((hda_read16(HDA_REG_ICS) & HDA_ICS_ICB) && --timeout) {
        io_wait();
    }
    if (timeout <= 0) return 0;

    uint32_t cmd = ((uint32_t)codec << 28) |
                   ((uint32_t)nid << 20) |
                   ((uint32_t)(verb & 0xFFF) << 8) |
                   (uint32_t)param;

    hda_write32(HDA_REG_IC, cmd);
    hda_write16(HDA_REG_ICS, HDA_ICS_ICB | HDA_ICS_IRV);

    timeout = 10000;
    while ((hda_read16(HDA_REG_ICS) & HDA_ICS_ICB) && --timeout) {
        io_wait();
    }
    if (timeout <= 0) return 0;

    timeout = 10000;
    while (!(hda_read16(HDA_REG_ICS) & HDA_ICS_IRV) && --timeout) {
        io_wait();
    }

    return hda_read32(HDA_REG_IR);
}

static uint32_t hda_exec_verb_16(uint8_t codec, uint8_t nid, uint8_t verb, uint16_t param) {
    if (!hda_present && !hda_base)
        return 0;

    int timeout = 10000;
    while ((hda_read16(HDA_REG_ICS) & HDA_ICS_ICB) && --timeout) {
        io_wait();
    }
    if (timeout <= 0) return 0;

    uint32_t cmd = ((uint32_t)codec << 28) |
                   ((uint32_t)nid << 20) |
                   ((uint32_t)(verb & 0x0F) << 16) |
                   (uint32_t)param;

    hda_write32(HDA_REG_IC, cmd);
    hda_write16(HDA_REG_ICS, HDA_ICS_ICB | HDA_ICS_IRV);

    timeout = 10000;
    while ((hda_read16(HDA_REG_ICS) & HDA_ICS_ICB) && --timeout) {
        io_wait();
    }
    if (timeout <= 0) return 0;

    timeout = 10000;
    while (!(hda_read16(HDA_REG_ICS) & HDA_ICS_IRV) && --timeout) {
        io_wait();
    }

    return hda_read32(HDA_REG_IR);
}

// Compute Intel HDA Format Bitmask
static uint16_t hda_compute_format(uint32_t rate, uint8_t channels, uint8_t bits) {
    uint16_t fmt = 0;

    if (rate == 44100) {
        fmt |= (1U << 14); // 44.1 kHz base
    } else if (rate == 88200) {
        fmt |= (1U << 14) | (1U << 11);
    } else if (rate == 176400) {
        fmt |= (1U << 14) | (3U << 11);
    } else if (rate == 96000) {
        fmt |= (1U << 11);
    } else if (rate == 192000) {
        fmt |= (3U << 11);
    } else if (rate == 32000) {
        fmt |= (2U << 8);
    }

    if (bits == 24) {
        fmt |= (4U << 4);
    } else if (bits == 32) {
        fmt |= (5U << 4);
    } else if (bits == 8) {
        fmt |= (0U << 4);
    } else {
        fmt |= (1U << 4); // 16-bit
    }

    if (channels > 0 && channels <= 16) {
        fmt |= (uint16_t)(channels - 1);
    }

    return fmt;
}

// Apply Hardware Format
static void hda_apply_format(uint32_t rate, uint8_t channels, uint8_t bits) {
    if (!hda_present) return;

    uint16_t fmt = hda_compute_format(rate, channels, bits);

    uint8_t ctl = hda_sd_read8(HDA_SD_CTL);
    hda_sd_write8(HDA_SD_CTL, ctl & ~HDA_SD_CTL_RUN);

    hda_sd_write16(HDA_SD_FMT, fmt);

    if (active_dac_nid) {
        hda_exec_verb_16(active_codec, active_dac_nid, 0x2, fmt);
    }

    if (hda_is_playing) {
        hda_sd_write8(HDA_SD_CTL, ctl | HDA_SD_CTL_RUN);
    }
}

// Initial Audio Pump: fills DMA buffers at stream launch
static void hda_pump_audio(void) {
    if (!hda_present)
        return;

    for (int i = 0; i < HDA_BDL_ENTRIES; i++) {
        uint8_t *dst = hda_buffers[i];

        if (ring_count >= HDA_BUFFER_SIZE) {
            uint32_t chunk = HDA_BUFFER_SIZE;
            uint32_t unread1 = HDA_RING_SIZE - ring_tail;
            if (chunk <= unread1) {
                memcpy(dst, hda_ring + ring_tail, chunk);
                ring_tail = (ring_tail + chunk) % HDA_RING_SIZE;
            } else {
                memcpy(dst, hda_ring + ring_tail, unread1);
                uint32_t rem = chunk - unread1;
                memcpy(dst + unread1, hda_ring, rem);
                ring_tail = rem;
            }
            ring_count -= chunk;
            total_played_bytes += chunk;
            total_played_blocks = (uint32_t)(total_played_bytes / HDA_BUFFER_SIZE);
        } else {
            memset(dst, 0, HDA_BUFFER_SIZE);
        }
    }
    hda_refill_idx = 0;
}

// Hardware Interrupt Service Routine
static void hda_isr(struct registers *regs) {
    (void)regs;
    if (!hda_present)
        return;

    uint32_t intsts = hda_read32(HDA_REG_INTSTS);
    if (!(intsts & (1U << 31))) {
        return;
    }

    uint8_t sts = hda_sd_read8(HDA_SD_STS);
    if (!(sts & (HDA_SD_STS_BCIS | HDA_SD_STS_FIFOE | HDA_SD_STS_DESE))) {
        return;
    }

    // Clear interrupt status bits
    hda_sd_write8(HDA_SD_STS, sts);

    if (sts & HDA_SD_STS_BCIS) {
        uint8_t slot = hda_refill_idx;
        hda_refill_idx = (slot + 1) % HDA_BDL_ENTRIES;

        uint8_t *dst = hda_buffers[slot];

        if (ring_count >= HDA_BUFFER_SIZE) {
            hda_underflow_count = 0;
            uint32_t chunk = HDA_BUFFER_SIZE;
            uint32_t unread1 = HDA_RING_SIZE - ring_tail;
            if (chunk <= unread1) {
                memcpy(dst, hda_ring + ring_tail, chunk);
                ring_tail = (ring_tail + chunk) % HDA_RING_SIZE;
            } else {
                memcpy(dst, hda_ring + ring_tail, unread1);
                uint32_t rem = chunk - unread1;
                memcpy(dst + unread1, hda_ring, rem);
                ring_tail = rem;
            }
            ring_count -= chunk;
            total_played_bytes += chunk;
            total_played_blocks = (uint32_t)(total_played_bytes / HDA_BUFFER_SIZE);
        } else {
            // Underflow: pad slot with silence without advancing unaligned user data
            memset(dst, 0, HDA_BUFFER_SIZE);
            hda_underflow_count++;
            if (hda_underflow_count >= HDA_BDL_ENTRIES) {
                // All DMA buffers drained to silence; stop DMA engine so next write restarts cleanly at buffer 0!
                uint8_t ctl0 = hda_sd_read8(HDA_SD_CTL);
                hda_sd_write8(HDA_SD_CTL, ctl0 & ~HDA_SD_CTL_RUN);
                hda_is_playing = false;
                hda_refill_idx = 0;
                hda_underflow_count = 0;
            }
        }

        // Wake all threads blocked on wait_queue / poll
        wait_queue_wake_all(&hda_wait_queue);
    }
}

// Discover and Route Codec Audio Path
static bool hda_setup_codec(void) {
    uint16_t statests = hda_read16(HDA_REG_STATESTS);
    if (!statests) {
        klog_puts("[HDA] No codecs detected on STATESTS.\n");
        return false;
    }

    active_codec = 0xFF;
    for (uint8_t c = 0; c < 15; c++) {
        if (statests & (1 << c)) {
            active_codec = c;
            break;
        }
    }
    if (active_codec == 0xFF) return false;

    // Discover Function Groups
    uint32_t root_subnodes = hda_exec_verb(active_codec, 0, HDA_VERB_GET_PARAM, HDA_PARAM_SUB_NODE_COUNT);
    uint8_t fg_start = (root_subnodes >> 16) & 0xFF;
    uint8_t fg_count = root_subnodes & 0xFF;

    uint8_t afg_nid = 0;
    for (uint8_t i = 0; i < fg_count; i++) {
        uint8_t nid = fg_start + i;
        uint32_t fg_type = hda_exec_verb(active_codec, nid, HDA_VERB_GET_PARAM, HDA_PARAM_FUNC_GROUP_TYPE);
        if ((fg_type & 0xFF) == 0x01) { // Audio Function Group
            afg_nid = nid;
            break;
        }
    }
    if (!afg_nid) afg_nid = 1;

    // Power up Audio Function Group (D0)
    hda_exec_verb(active_codec, afg_nid, HDA_VERB_SET_POWER_STATE, 0x00);

    // Enumerate Widgets in AFG
    uint32_t afg_subnodes = hda_exec_verb(active_codec, afg_nid, HDA_VERB_GET_PARAM, HDA_PARAM_SUB_NODE_COUNT);
    uint8_t w_start = (afg_subnodes >> 16) & 0xFF;
    uint8_t w_count = afg_subnodes & 0xFF;

    active_dac_nid = 0;
    active_pin_nid = 0;

    for (uint8_t i = 0; i < w_count; i++) {
        uint8_t nid = w_start + i;
        uint32_t wcap = hda_exec_verb(active_codec, nid, HDA_VERB_GET_PARAM, HDA_PARAM_AUDIO_WIDGET_CAP);
        uint8_t wtype = (wcap >> 20) & 0x0F;

        // Power up widget
        hda_exec_verb(active_codec, nid, HDA_VERB_SET_POWER_STATE, 0x00);

        if (wtype == HDA_WIDGET_AUDIO_OUTPUT && !active_dac_nid) {
            active_dac_nid = nid;
        } else if (wtype == HDA_WIDGET_PIN_COMPLEX && !active_pin_nid) {
            uint32_t pincap = hda_exec_verb(active_codec, nid, HDA_VERB_GET_PARAM, HDA_PARAM_PIN_CAP);
            if (pincap & (1 << 4)) { // Output capable
                active_pin_nid = nid;
            }
        }
    }

    if (!active_dac_nid) active_dac_nid = 0x02;
    if (!active_pin_nid) active_pin_nid = 0x03;

    // Configure Pin Complex: Enable Output + EAPD + 0dB Gain
    hda_exec_verb(active_codec, active_pin_nid, HDA_VERB_SET_PIN_WIDGET_CTRL, 0x40); // Pin Out Enable
    hda_exec_verb(active_codec, active_pin_nid, HDA_VERB_SET_EAPD_BTLENABLE, 0x02);  // EAPD On
    hda_exec_verb_16(active_codec, active_pin_nid, 0x3, 0xB07F); // Unmute + Max Amp Gain Left/Right

    // Configure DAC: Unmute + Max Gain + Route to Stream 1 Channel 0
    hda_exec_verb_16(active_codec, active_dac_nid, 0x3, 0xB07F); // Unmute + Max Amp Gain Left/Right
    hda_exec_verb(active_codec, active_dac_nid, HDA_VERB_SET_CONV_STREAM_CHAN, (1 << 4) | 0); // Stream 1, Channel 0
    hda_exec_verb_16(active_codec, active_dac_nid, 0x2, HDA_FMT_48KHZ_16BIT_STEREO);

    return true;
}

// Initialize Intel HDA Hardware
void hda_init(void) {
    klog_puts("[HDA] Probing for High Definition Audio controller...\n");

    hda_pci = pci_find_device_by_id(0x8086, 0x2668); // ICH6
    if (!hda_pci) hda_pci = pci_find_device_by_id(0x8086, 0x27D8); // ICH7 / QEMU
    if (!hda_pci) hda_pci = pci_find_device_by_id(0x8086, 0x284B); // ICH8
    if (!hda_pci) hda_pci = pci_find_device_by_id(0x8086, 0x293E); // ICH9
    if (!hda_pci) hda_pci = pci_find_device_by_id(0x1002, 0x4383); // AMD
    if (!hda_pci) hda_pci = pci_find_device(0x04, 0x03);          // Audio Controller Class

    if (!hda_pci) {
        klog_puts("[HDA] No Intel HDA device found.\n");
        return;
    }

    klog_puts("[HDA] Controller found at PCI ");
    klog_uint64(hda_pci->bus);
    klog_putchar(':');
    klog_uint64(hda_pci->slot);
    klog_putchar('.');
    klog_uint64(hda_pci->func);
    klog_puts("\n");

    pci_enable_bus_mastering(hda_pci);
    uint16_t cmd = pci_config_read16(hda_pci->bus, hda_pci->slot, hda_pci->func, 0x04);
    pci_config_write16(hda_pci->bus, hda_pci->slot, hda_pci->func, 0x04, cmd | 0x0006);

    uint64_t mmio_phys = hda_pci->bar[0] & ~0xFULL;
    if (((hda_pci->bar[0] >> 1) & 3U) == 2U) {
        mmio_phys |= ((uint64_t)hda_pci->bar[1] << 32);
    }
    if (!mmio_phys) {
        klog_puts("[HDA] Invalid BAR0 address!\n");
        return;
    }

    uint64_t hhdm = pmm_get_hhdm_offset();
    uint64_t *pml4 = vmm_get_active_pml4();
    uint64_t flags = PAGE_FLAG_PRESENT | PAGE_FLAG_RW | PAGE_FLAG_PCD | PAGE_FLAG_PWT;

    for (uint64_t page = (mmio_phys & ~0xFFFULL); page <= ((mmio_phys + 0x4000 - 1) & ~0xFFFULL); page += 4096) {
        uint64_t virt = page + hhdm;
        if (!vmm_virt_to_phys(pml4, virt)) {
            vmm_map_page(pml4, virt, page, flags);
        }
        vmm_flush_tlb(virt);
    }

    hda_base = (volatile uint8_t *)(mmio_phys + hhdm);

    // Controller Reset Sequence
    uint32_t gctl = hda_read32(HDA_REG_GCTL);
    hda_write32(HDA_REG_GCTL, gctl & ~HDA_GCTL_CRST);

    int timeout = 10000;
    while ((hda_read32(HDA_REG_GCTL) & HDA_GCTL_CRST) && --timeout) io_wait();

    hda_write32(HDA_REG_GCTL, HDA_GCTL_CRST);
    timeout = 10000;
    while (!(hda_read32(HDA_REG_GCTL) & HDA_GCTL_CRST) && --timeout) io_wait();

    if (timeout <= 0) {
        klog_puts("[HDA] Controller reset failed!\n");
        return;
    }

    for (int i = 0; i < 2000; i++) io_wait();

    uint16_t gcap = hda_read16(HDA_REG_GCAP);
    hda_iss = (gcap >> 8) & 0x0F;
    hda_oss = (gcap >> 12) & 0x0F;
    hda_stream_offset = HDA_SD_BASE + (hda_iss * HDA_SD_SIZE);

    if (!hda_setup_codec()) {
        klog_puts("[HDA] Codec setup failed.\n");
        return;
    }

    // Allocate physical memory for BDL
    hda_bdl_phys = (uint64_t)pmm_alloc_pages(1);
    hda_bdl = (struct hda_bdl_entry *)(hda_bdl_phys + hhdm);
    memset(hda_bdl, 0, 4096);

    // Allocate physical memory for DMA buffers
    for (int i = 0; i < HDA_BDL_ENTRIES; i++) {
        hda_buffers_phys[i] = (uint64_t)pmm_alloc_pages(1);
        hda_buffers[i] = (uint8_t *)(hda_buffers_phys[i] + hhdm);
        memset(hda_buffers[i], 0, HDA_BUFFER_SIZE);

        hda_bdl[i].addr_low = (uint32_t)(hda_buffers_phys[i] & 0xFFFFFFFF);
        hda_bdl[i].addr_high = (uint32_t)(hda_buffers_phys[i] >> 32);
        hda_bdl[i].length = HDA_BUFFER_SIZE;
        hda_bdl[i].flags = HDA_BDL_FLAG_IOC;
    }

    // Allocate software ring buffer
    hda_ring = kmalloc(HDA_RING_SIZE);
    if (!hda_ring) return;
    memset(hda_ring, 0, HDA_RING_SIZE);

    // Reset Output Stream
    hda_sd_write8(HDA_SD_CTL, HDA_SD_CTL_SRST);
    timeout = 10000;
    while (!(hda_sd_read8(HDA_SD_CTL) & HDA_SD_CTL_SRST) && --timeout) io_wait();

    hda_sd_write8(HDA_SD_CTL, 0);
    timeout = 10000;
    while ((hda_sd_read8(HDA_SD_CTL) & HDA_SD_CTL_SRST) && --timeout) io_wait();

    hda_sd_write8(HDA_SD_STS, 0x1C);

    // Program Stream Descriptor
    hda_sd_write32(HDA_SD_BDLPL, (uint32_t)(hda_bdl_phys & 0xFFFFFFFF));
    hda_sd_write32(HDA_SD_BDLPU, (uint32_t)(hda_bdl_phys >> 32));
    hda_sd_write32(HDA_SD_CBL, HDA_BDL_ENTRIES * HDA_BUFFER_SIZE);
    hda_sd_write16(HDA_SD_LVI, HDA_BDL_ENTRIES - 1);
    hda_sd_write16(HDA_SD_FMT, HDA_FMT_48KHZ_16BIT_STEREO);

    // Install IRQ Handler (Level-triggered active-low)
    uint8_t irq = hda_pci->irq_line;
    irq_install_handler(irq, hda_isr, 0x000F);

    // Enable Controller & Stream Interrupts
    uint32_t intctl = hda_read32(HDA_REG_INTCTL);
    hda_write32(HDA_REG_INTCTL, intctl | HDA_INTCTL_GIE | HDA_INTCTL_CIE | (1U << hda_iss));

    hda_sd_write8(HDA_SD_CTL + 2, (1 << 4)); // Stream Tag 1
    hda_sd_write8(HDA_SD_CTL, HDA_SD_CTL_IOCE | HDA_SD_CTL_FEIE | HDA_SD_CTL_DEIE);

    hda_present = true;
    klog_puts("[OK] Intel High Definition Audio (HDA) Initialized.\n");
}

bool hda_is_present(void) {
    return hda_present;
}

// PCM Write with Blocking Wait Queue
uint32_t hda_write_pcm(const void *buffer, uint32_t bytes, uint32_t rate, uint8_t channels, uint8_t bits) {
    if (!hda_present || !buffer || bytes == 0)
        return 0;

    if (rate != current_sample_rate || channels != current_channels || bits != current_bits) {
        current_sample_rate = rate;
        current_channels = channels;
        current_bits = bits;
        hda_apply_format(rate, channels, bits);
    }

    const uint8_t *src = (const uint8_t *)buffer;
    uint32_t written = 0;

    while (written < bytes) {
        hal_irq_disable();
        while ((HDA_RING_SIZE - ring_count) < 4) {
            wait_queue_entry_t wq_entry;
            struct thread *t = sched_get_current();
            if (t) {
                wq_entry.thread = t;
                wq_entry.next = NULL;
                wq_entry.thread_next = NULL;
                wq_entry.thread_prev = NULL;
                wq_entry.wq = &hda_wait_queue;
                wait_queue_add(&hda_wait_queue, &wq_entry);

                t->state = THREAD_BLOCKED;
                t->wakeup_ticks = lapic_timer_get_ticks() + 50; // 50ms safety timeout

                hal_irq_enable();
                sched_yield();
                hal_irq_disable();

                wait_queue_remove(&hda_wait_queue, &wq_entry);
                t->state = THREAD_RUNNING;
                t->wakeup_ticks = 0;
            } else {
                hal_irq_enable();
                sched_yield();
                hal_irq_disable();
            }
        }

        uint32_t space = HDA_RING_SIZE - ring_count;
        uint32_t to_write = bytes - written;
        if (to_write > space) to_write = space;
        to_write = (to_write / 4) * 4;
        if (to_write == 0) {
            hal_irq_enable();
            sched_yield();
            continue;
        }

        uint32_t unwritten1 = HDA_RING_SIZE - ring_head;
        if (to_write <= unwritten1) {
            memcpy(hda_ring + ring_head, src + written, to_write);
            ring_head = (ring_head + to_write) % HDA_RING_SIZE;
        } else {
            memcpy(hda_ring + ring_head, src + written, unwritten1);
            uint32_t rem = to_write - unwritten1;
            memcpy(hda_ring, src + written + unwritten1, rem);
            ring_head = rem;
        }
        ring_count += to_write;
        written += to_write;

        // Start hardware playback if not running
        if (!hda_is_playing && ring_count >= HDA_BUFFER_SIZE) {
            hda_pump_audio();
            uint8_t ctl0 = hda_sd_read8(HDA_SD_CTL);
            hda_sd_write8(HDA_SD_CTL, ctl0 | HDA_SD_CTL_RUN | HDA_SD_CTL_IOCE | HDA_SD_CTL_FEIE | HDA_SD_CTL_DEIE);
            hda_is_playing = true;
        }

        hal_irq_enable();
    }

    return written;
}

uint32_t hda_get_ring_count(void) {
    return ring_count;
}

uint64_t hda_get_played_bytes(void) {
    return total_played_bytes;
}

void *hda_get_wait_queue(void) {
    return &hda_wait_queue;
}

void hda_reset_stream(void) {
    if (!hda_present) return;
    hal_irq_disable();
    ring_head = ring_tail = ring_count = 0;
    total_played_bytes = 0;
    total_played_blocks = 0;
    hda_is_playing = false;
    hda_refill_idx = 0;
    hda_underflow_count = 0;
    uint32_t ctl = hda_sd_read32(HDA_SD_CTL);
    hda_sd_write32(HDA_SD_CTL, ctl & ~HDA_SD_CTL_RUN);
    hal_irq_enable();
}

void hda_set_format(uint32_t rate, uint8_t channels, uint8_t bits) {
    if (!hda_present) return;
    current_sample_rate = rate;
    current_channels = channels;
    current_bits = bits;
    hda_apply_format(rate, channels, bits);
}

// VFS Callbacks
static uint32_t hda_vfs_write(struct vfs_node *node, uint32_t offset,
                              uint32_t size, uint8_t *buffer) {
    (void)node;
    (void)offset;
    return hda_write_pcm(buffer, size, current_sample_rate, current_channels, current_bits);
}

int hda_ioctl_handler(uint32_t request, uint64_t arg) {
    if (!hda_present)
        return -5; // EIO

    switch (request) {
    case 0x5000: // SNDCTL_DSP_RESET
    {
        hal_irq_disable();
        ring_head = ring_tail = ring_count = 0;
        total_played_bytes = 0;
        total_played_blocks = 0;
        hda_is_playing = false;
        uint32_t ctl = hda_sd_read32(HDA_SD_CTL);
        hda_sd_write32(HDA_SD_CTL, ctl & ~HDA_SD_CTL_RUN);
        hal_irq_enable();
        return 0;
    }
    case 0x5001: // SNDCTL_DSP_SYNC
    case 0x5008: // SNDCTL_DSP_POST
    case 0x500B: // SNDCTL_DSP_NONBLOCK (legacy)
    case 0x500E: // SNDCTL_DSP_NONBLOCK
    case 0xC0045009: // SNDCTL_DSP_SUBDIVIDE
    case 0xC004500A: // SNDCTL_DSP_SETFRAGMENT
        return 0; // Success

    case 0x8004500B: // SNDCTL_DSP_GETFMTS
    {
        int *mask = (int *)arg;
        if (!mask) return -14;
        *mask = 0x00000010 | 0x00000008 | 0x00000020 | 0x00001000 | 0x00000040 | 0x00000080;
        return 0;
    }
    case 0x8004500F: // SNDCTL_DSP_GETCAPS
    {
        int *caps = (int *)arg;
        if (!caps) return -14;
        *caps = 0x00001000 | 0x00000100 | 0x00000200 | 0x00020000 | 0x00010000;
        return 0;
    }
    case 0x80045004: // SOUND_PCM_READ_BLKSIZE
    case 0xC0045004: // SNDCTL_DSP_GETBLKSIZE
    {
        int *blksize = (int *)arg;
        if (!blksize) return -14;
        *blksize = HDA_BUFFER_SIZE;
        return 0;
    }
    case 0x8010500C: // SNDCTL_DSP_GETOSPACE
    {
        struct {
            int fragments;
            int fragstotal;
            int fragsize;
            int bytes;
        } *info = (void *)arg;
        if (!info) return -14;
        info->fragsize = HDA_BUFFER_SIZE;
        info->fragstotal = HDA_RING_SIZE / HDA_BUFFER_SIZE;
        info->bytes = HDA_RING_SIZE - ring_count;
        info->fragments = info->bytes / info->fragsize;
        return 0;
    }
    case 0x8010500D: // SNDCTL_DSP_GETISPACE
    {
        struct {
            int fragments;
            int fragstotal;
            int fragsize;
            int bytes;
        } *info = (void *)arg;
        if (!info) return -14;
        info->fragsize = HDA_BUFFER_SIZE;
        info->fragstotal = HDA_RING_SIZE / HDA_BUFFER_SIZE;
        info->bytes = 0;
        info->fragments = 0;
        return 0;
    }
    case 0x800C5012: // SNDCTL_DSP_GETOPTR (OSS 3.x/4.x count_info)
    case 0x80105012: // SNDCTL_DSP_GETOPTR (16-byte count_info)
    case 0x80045012: // SNDCTL_DSP_GETOPTR
    case 0x00005012: // SNDCTL_DSP_GETOPTR
    case 0x800C5007: // SNDCTL_DSP_GETPTR (legacy OSS)
    case 0x80045007: // SNDCTL_DSP_GETPTR (legacy OSS)
    {
        struct {
            int bytes;
            int blocks;
            int ptr;
        } *info = (void *)arg;
        if (!info) return -14;
        hal_irq_disable();
        info->bytes = (int)(total_played_bytes & 0x7FFFFFFF);
        info->blocks = (int)(total_played_blocks & 0x7FFFFFFF);
        info->ptr = (int)ring_tail;
        hal_irq_enable();
        return 0;
    }
    case 0x800C5011: // SNDCTL_DSP_GETIPTR (OSS 3.x/4.x count_info)
    case 0x80105011: // SNDCTL_DSP_GETIPTR (16-byte count_info)
    case 0x80045011: // SNDCTL_DSP_GETIPTR
    case 0x00005011: // SNDCTL_DSP_GETIPTR
    {
        struct {
            int bytes;
            int blocks;
            int ptr;
        } *info = (void *)arg;
        if (!info) return -14;
        hal_irq_disable();
        info->bytes = (int)(total_played_bytes & 0x7FFFFFFF);
        info->blocks = (int)(total_played_blocks & 0x7FFFFFFF);
        info->ptr = (int)ring_tail;
        hal_irq_enable();
        return 0;
    }
    case 0x40045010: // SNDCTL_DSP_SETTRIGGER
    case 0xC0045010:
    case 0x00005010:
    {
        int *trig = (int *)arg;
        if (!trig) return -14;
        hal_irq_disable();
        if (*trig & 0x02) { // PCM_ENABLE_OUTPUT
            if (!hda_is_playing && ring_count >= HDA_BUFFER_SIZE) {
                hda_pump_audio();
                uint8_t ctl0 = hda_sd_read8(HDA_SD_CTL);
                hda_sd_write8(HDA_SD_CTL, ctl0 | HDA_SD_CTL_RUN | HDA_SD_CTL_IOCE | HDA_SD_CTL_FEIE | HDA_SD_CTL_DEIE);
                hda_is_playing = true;
            }
        } else if (*trig == 0) {
            if (hda_is_playing) {
                uint8_t ctl0 = hda_sd_read8(HDA_SD_CTL);
                hda_sd_write8(HDA_SD_CTL, ctl0 & ~HDA_SD_CTL_RUN);
                hda_is_playing = false;
            }
        }
        hal_irq_enable();
        return 0;
    }
    case 0x80045010: // SNDCTL_DSP_GETTRIGGER
    {
        int *trig = (int *)arg;
        if (!trig) return -14;
        hal_irq_disable();
        *trig = hda_is_playing ? 0x02 : 0x00;
        hal_irq_enable();
        return 0;
    }
    case 0x80044D1D: // SNDCTL_DSP_GETODELAY (OSS 4.0)
    case 0x80045017: // SNDCTL_DSP_GETODELAY (OSS 3.x)
    {
        int *delay = (int *)arg;
        if (!delay) return -14;
        hal_irq_disable();
        *delay = (int)ring_count;
        hal_irq_enable();
        return 0;
    }
    case 0x80045002: // SOUND_PCM_READ_RATE
    case 0xC0045002: // SNDCTL_DSP_SPEED
    {
        uint32_t *rate = (uint32_t *)arg;
        if (!rate) return -14;
        if (*rate >= 8000 && *rate <= 192000) {
            current_sample_rate = *rate;
            hda_apply_format(current_sample_rate, current_channels, current_bits);
        }
        *rate = current_sample_rate;
        return 0;
    }
    case 0xC0045003: // SNDCTL_DSP_STEREO
    {
        int *stereo = (int *)arg;
        if (!stereo) return -14;
        current_channels = (*stereo) ? 2 : 1;
        hda_apply_format(current_sample_rate, current_channels, current_bits);
        *stereo = (current_channels == 2);
        return 0;
    }
    case 0x80045006: // SOUND_PCM_READ_CHANNELS
    case 0xC0045006: // SNDCTL_DSP_CHANNELS
    {
        int *ch = (int *)arg;
        if (!ch) return -14;
        if (*ch == 1 || *ch == 2) {
            current_channels = (uint8_t)*ch;
            hda_apply_format(current_sample_rate, current_channels, current_bits);
        }
        *ch = current_channels;
        return 0;
    }
    case 0x80045005: // SOUND_PCM_READ_BITS
    case 0xC0045005: // SNDCTL_DSP_SETFMT
    {
        int *fmt = (int *)arg;
        if (!fmt) return -14;
        if (*fmt == 0x00000008) { // AFMT_U8
            current_bits = 8;
        } else if (*fmt == 0x00000010 || *fmt == 0x00000020 || *fmt == 0x00001000) { // AFMT_S16_LE / BE / S32
            current_bits = 16;
        }
        hda_apply_format(current_sample_rate, current_channels, current_bits);
        *fmt = (current_bits == 8) ? 0x00000008 : 0x00000010;
        return 0;
    }
    case 0x80044D76: // OSS_GETVERSION
    {
        int *ver = (int *)arg;
        if (!ver) return -14;
        *ver = 0x040000;
        return 0;
    }
    case 0x80044D00: // SOUND_MIXER_READ_VOLUME
    case 0x80044D04: // SOUND_MIXER_READ_PCM
    case 0xC0044D00: // SOUND_MIXER_WRITE_VOLUME
    case 0xC0044D04: // SOUND_MIXER_WRITE_PCM
    {
        int *vol = (int *)arg;
        if (!vol) return -14;
        *vol = 0x6464; // 100% Left, 100% Right
        return 0;
    }
    case 0x80044DFF: // SOUND_MIXER_READ_DEVMASK
    case 0x80044DFE: // SOUND_MIXER_READ_RECMASK
    case 0x80044DFD: // SOUND_MIXER_READ_STEREODEVS
    {
        int *mask = (int *)arg;
        if (!mask) return -14;
        *mask = 0x11;
        return 0;
    }
    default:
        return -25; // ENOTTY
    }
}

int hda_poll_handler(int events) {
    if (!hda_present)
        return 0;

    int revents = 0;
    hal_irq_disable();
    uint32_t cnt = ring_count;
    hal_irq_enable();

    if ((events & (POLLOUT | POLLWRNORM)) && (HDA_RING_SIZE - cnt) >= HDA_BUFFER_SIZE) {
        revents |= (events & (POLLOUT | POLLWRNORM));
    }
    return revents;
}

static int hda_vfs_ioctl(struct vfs_node *node, uint32_t request, uint64_t arg) {
    (void)node;
    return hda_ioctl_handler(request, arg);
}

static int hda_vfs_poll(struct vfs_node *node, int events) {
    (void)node;
    return hda_poll_handler(events);
}

void hda_register_vfs(void) {
    if (!hda_present)
        return;

    vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
    if (!node)
        return;

    vfs_node_init(node);
    strcpy(node->name, "hda_audio");
    node->flags = FS_CHARDEV;
    node->mask = 0666;
    node->length = 0;
    node->write = hda_vfs_write;
    node->ioctl = hda_vfs_ioctl;
    node->poll = hda_vfs_poll;
    wait_queue_init(&hda_wait_queue);
    node->wait_queue = &hda_wait_queue;

    fb_register_device_node("hda_audio", node);
}