#include "sb16.h"
#include "hal/hal.h"
#include "../../acpi/acpi.h"
#include "../../apic/ioapic.h"
#include "../../apic/lapic.h"
#include "../../console/console.h"
#include "../../console/klog.h"
#include "../../cpu/irq.h"
#include "../../cpu/isr.h"
#include "../../dma/dma.h"
#include "../../fb/framebuffer.h"
#include "../../fs/vfs.h"
#include "../../io/io.h"
#include "../../lib/string.h"
#include "../../mm/heap.h"
#include "../../mm/pmm.h"
#include "../../sched/sched.h"

#define SNDCTL_DSP_SPEED 0xC0045002
#define SNDCTL_DSP_STEREO 0xC0045003
#define SNDCTL_DSP_SETFMT 0xC0045005
#define SNDCTL_DSP_CHANNELS 0xC0045006
#define SNDCTL_DSP_GETODELAY 0x80044D1D
#define AFMT_U8 0x00000008
#define AFMT_S16_LE 0x00000010

#define SB16_BASE 0x220
#define MIXER_REG (SB16_BASE + 0x4)
#define MIXER_DATA (SB16_BASE + 0x5)
#define DSP_RESET (SB16_BASE + 0x6)
#define DSP_READ (SB16_BASE + 0xA)
#define DSP_WRITE (SB16_BASE + 0xC)
#define DSP_READ_STATUS (SB16_BASE + 0xE)
#define DSP_INT_ACK_8 (SB16_BASE + 0xE)
#define DSP_INT_ACK_16 (SB16_BASE + 0xF)

volatile uint32_t sb16_irq_fired = 0;
static bool sb16_present = false;
static bool sb16_active_16bit = false;

// OSS device state (defaults match /dev/dsp standard)
static uint32_t dsp_sample_rate = 8000;
static uint8_t dsp_channels = 1;
static uint8_t dsp_bits = 8;

#define SB16_RING_SIZE 65536
static uint8_t sb16_ring[SB16_RING_SIZE];
static volatile uint32_t ring_head = 0;
static volatile uint32_t ring_tail = 0;
static volatile uint32_t ring_count = 0;
static volatile bool sb16_is_playing = false;

// DMA buffer (64KB-aligned within ISA 16MB)
static uint8_t *sb16_dma_buf = 0;
static uint64_t sb16_dma_phys = 0;
static bool sb16_dma_failed = false;

// DSP low-level I/O

static void dsp_write(uint8_t value) {
  while (inb(DSP_WRITE) & 0x80) {
    io_wait();
  }
  outb(DSP_WRITE, value);
}

static void sb16_mixer_write(uint8_t reg, uint8_t val) {
  outb(MIXER_REG, reg);
  outb(MIXER_DATA, val);
}

static uint8_t dsp_read(void) {
  while (!(inb(DSP_READ_STATUS) & 0x80)) {
    io_wait();
  }
  return inb(DSP_READ);
}

// ISR

static void sb16_pump_audio(void);

static void sb16_isr(struct registers *regs) {
  (void)regs;
  if (sb16_active_16bit) {
    inb(DSP_INT_ACK_16);
  } else {
    inb(DSP_INT_ACK_8);
  }

  // Read Mixer IRQ status to clear interrupt (critical for hardware)
  outb(MIXER_REG, 0x82);
  (void)inb(MIXER_DATA);

  sb16_irq_fired = 1;

  sb16_pump_audio();
}

// DMA buffer allocation
//
// ISA DMA constraints:
//   - Buffer must reside below 16 MB (24-bit addressing)
//   - Transfer must not cross a 64 KB boundary  (8-bit channels)
//   - Transfer must not cross a 128 KB boundary (16-bit channels)
//
// Since sb16_pump_audio() caps each DMA transfer at 2048 bytes,
// a single 4 KB page (page-aligned) can never cross either boundary.
// We therefore only need ONE page instead of the previous 32 (128 KB).
//
// Fallback: the first 1 MB of RAM is real memory that the PMM
// intentionally skips.  Address 0x8000 (32 KB) sits safely in
// conventional memory — above the IVT/BDA and below the VGA hole —
// and is guaranteed to exist on every PC.

#define SB16_DMA_FALLBACK_PHYS 0x8000 // 32 KB — conventional memory

static int ensure_dma_buffer(void) {
  if (sb16_dma_buf)
    return 0;
  if (sb16_dma_failed)
    return -1;

  // Try the buddy allocator first (single page, below 16 MB)
  uint64_t phys = (uint64_t)pmm_alloc_pages_constrained(1, 0x1000000);

  if (!phys) {
    // Buddy allocator has nothing below 16 MB.
    // Use a fixed address in conventional memory that the PMM never touches.
    phys = SB16_DMA_FALLBACK_PHYS;
    klog_puts("[SB16] Using conventional-memory fallback for DMA buffer\n");
  }

  sb16_dma_phys = phys; // already page-aligned (or 0x8000 = 32 KB aligned)
  sb16_dma_buf = (uint8_t *)(sb16_dma_phys + pmm_get_hhdm_offset());

  klog_puts("[SB16] DMA buffer: phys=");
  klog_uint64(sb16_dma_phys);
  klog_puts(" [OK]\n");

  return 0;
}

// Early DMA reservation (called before PCI/storage eat low memory)

void sb16_reserve_dma(void) {
  // Quick probe: reset DSP and check for 0xAA ready signal.
  // We don't do full init here — just confirm the card exists and grab DMA RAM.
  outb(DSP_RESET, 1);
  for (int i = 0; i < 10; i++)
    io_wait();
  outb(DSP_RESET, 0);

  uint32_t timeout = 10000;
  while (timeout > 0) {
    if ((inb(DSP_READ_STATUS) & 0x80) && (inb(DSP_READ) == 0xAA))
      break;
    io_wait();
    timeout--;
  }
  if (timeout == 0)
    return; // No SB16 — nothing to reserve

  klog_puts("[SB16] Card detected, reserving ISA DMA buffer early...\n");
  ensure_dma_buffer();
}

// Hardware initialization

void sb16_init(void) {
  // Reset DSP
  outb(DSP_RESET, 1);
  for (int i = 0; i < 10; i++)
    io_wait();
  outb(DSP_RESET, 0);

  // Wait for ready
  uint32_t timeout = 10000;
  while (timeout > 0) {
    if ((inb(DSP_READ_STATUS) & 0x80) && (inb(DSP_READ) == 0xAA)) {
      break;
    }
    io_wait();
    timeout--;
  }

  if (timeout == 0) {
    console_puts("[WARN] SB16 Sound Card not detected.\n");
    return;
  }

  // Get DSP version
  dsp_write(0xE1);
  uint8_t major = dsp_read();
  uint8_t minor = dsp_read();

  console_puts("[INFO] SB16 Sound Card detected (DSP v");
  char c1 = '0' + (major % 10);
  console_putchar((major > 9) ? '1' : c1);
  if (major > 9)
    console_putchar('0' + (major % 10));
  console_putchar('.');
  console_putchar('0' + (minor / 10));
  console_putchar('0' + (minor % 10));
  console_puts(").\n");

  uint8_t irq = 5;
  irq_install_handler(irq, sb16_isr, 0);

  // Mute auxiliary inputs to eliminate analog background sizzle
  sb16_mixer_write(0x22, 0xFF); // Master Vol L/R
  sb16_mixer_write(0x04, 0xFF); // Voice/DAC Vol L/R
  sb16_mixer_write(0x0A, 0x00); // Mic Mute
  sb16_mixer_write(0x26, 0x00); // CD Audio Mute
  sb16_mixer_write(0x2E, 0x00); // Line-In Mute

  sb16_present = true;

  // Eagerly allocate DMA buffer while low memory is still available.
  // If deferred until the first write(), other subsystems may have consumed
  // all memory below 16MB (the ISA DMA ceiling).
  if (ensure_dma_buffer() != 0) {
    klog_puts("[SB16] WARNING: Could not pre-allocate DMA buffer.\n");
    klog_puts("       Audio playback will not be available.\n");
  }
}

// Play a single DMA chunk (internal)

void sb16_play_chunk(uint32_t phys_addr, uint32_t length, uint16_t sample_rate,
                     uint8_t channels, uint8_t bits) {
  if (!sb16_present)
    return;

  sb16_irq_fired = 0;

  static int last_bits = -1;
  static uint16_t last_rate = 0;

  bool same_config = (bits == last_bits && sample_rate == last_rate);

  if (!same_config) {
    // Stop current transfer before changing rate
    if (sb16_irq_fired == 0) { // If a transfer was potentially active
      dsp_write(bits == 16 ? 0xD5 : 0xD0); // Pause/Stop commands
    }

    // SB16 expects: Command 0x41, High Byte, Low Byte
    dsp_write(0x41);
    dsp_write((sample_rate >> 8) & 0xFF);
    dsp_write(sample_rate & 0xFF);

    last_bits = bits;
    last_rate = sample_rate;
  }

  uint8_t mode = 0;
  if (channels == 2)
    mode |= 0x20;

  uint16_t dsp_count;
  if (bits == 16) {
    mode |= 0x10; // Signed PCM
    if (channels == 2)
      dsp_count = (length / 4) - 1;
    else
      dsp_count = (length / 2) - 1;
  } else {
    if (channels == 2)
      dsp_count = (length / 2) - 1;
    else
      dsp_count = length - 1;
  }

  if (bits == 16) {
    mode |= 0x10;
    sb16_active_16bit = true;
    dma_start(5, DMA_MODE_SINGLE | DMA_MODE_READ, phys_addr, length);
    dsp_write(0xB0); // 16-bit Single-cycle
    dsp_write(mode);
    dsp_write(dsp_count & 0xFF);
    dsp_write((dsp_count >> 8) & 0xFF);
  } else {
    sb16_active_16bit = false;
    dma_start(1, DMA_MODE_SINGLE | DMA_MODE_READ, phys_addr, length);
    dsp_write(0xC0); // 8-bit Single-cycle
    dsp_write(mode);
    dsp_write(dsp_count & 0xFF);
    dsp_write((dsp_count >> 8) & 0xFF);
  }
}

// OSS /dev/dsp format control

void sb16_set_format(uint32_t rate, uint8_t channels, uint8_t bits) {
  if (rate >= 4000 && rate <= 48000)
    dsp_sample_rate = rate;
  if (channels == 1 || channels == 2)
    dsp_channels = channels;
  if (bits == 8 || bits == 16)
    dsp_bits = bits;
}

uint32_t sb16_get_sample_rate(void) { return dsp_sample_rate; }
uint8_t sb16_get_channels(void) { return dsp_channels; }
uint8_t sb16_get_bits(void) { return dsp_bits; }

int sb16_ioctl(struct vfs_node *node, uint32_t request, uint64_t arg) {
  (void)node;
  switch (request) {
  case 0x5000: // SNDCTL_DSP_RESET
  {
    hal_irq_disable();
    ring_head = ring_tail = ring_count = 0;
    sb16_is_playing = false;
    dsp_write(0xD0);
    dsp_write(0xD5);
    hal_irq_enable();
    return 0;
  }
  case 0xC004500A: // SNDCTL_DSP_SETFRAGMENT
  case 0x5001: // SNDCTL_DSP_SYNC
  case 0x5008: // SNDCTL_DSP_POST
  case 0x500B: // SNDCTL_DSP_NONBLOCK (legacy)
  case 0x500E: // SNDCTL_DSP_NONBLOCK
  case 0xC0045009: // SNDCTL_DSP_SUBDIVIDE
    return 0;
  case 0x8004500B: // SNDCTL_DSP_GETFMTS
  {
    int *mask = (int *)arg;
    if (!mask) return -14;
    *mask = AFMT_S16_LE | AFMT_U8;
    return 0;
  }
  case 0x8004500F: // SNDCTL_DSP_GETCAPS
  {
    int *caps = (int *)arg;
    if (!caps) return -14;
    *caps = 0x00000002 | 0x00000008 | 0x00000010 | 0x00000020;
    return 0;
  }
  case 0x80045004: // SOUND_PCM_READ_BLKSIZE
  case 0xC0045004: // SNDCTL_DSP_GETBLKSIZE
  {
    int *blksize = (int *)arg;
    if (!blksize) return -14;
    *blksize = 2048;
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
    info->fragsize = 2048;
    info->fragstotal = SB16_RING_SIZE / 2048;
    info->bytes = SB16_RING_SIZE - ring_count;
    info->fragments = info->bytes / 2048;
    return 0;
  }
  case SNDCTL_DSP_SPEED: {
    uint32_t *rate = (uint32_t *)arg;
    if (!rate)
      return -14; // EFAULT
    sb16_set_format(*rate, dsp_channels, dsp_bits);
    *rate = dsp_sample_rate;
    return 0;
  }
  case SNDCTL_DSP_STEREO: {
    int *stereo = (int *)arg;
    if (!stereo)
      return -14;
    sb16_set_format(dsp_sample_rate, (*stereo ? 2 : 1), dsp_bits);
    *stereo = (dsp_channels == 2);
    return 0;
  }
  case SNDCTL_DSP_CHANNELS: {
    int *ch = (int *)arg;
    if (!ch)
      return -14;
    sb16_set_format(dsp_sample_rate, (uint8_t)*ch, dsp_bits);
    *ch = dsp_channels;
    return 0;
  }
  case SNDCTL_DSP_SETFMT: {
    int *fmt = (int *)arg;
    if (!fmt)
      return -14;
    uint8_t bits = (*fmt == AFMT_S16_LE) ? 16 : 8;
    sb16_set_format(dsp_sample_rate, dsp_channels, bits);
    *fmt = (dsp_bits == 16) ? AFMT_S16_LE : AFMT_U8;
    return 0;
  }
  default:
    return -25; // ENOTTY
  }
}

#define SB16_RING_SIZE 65536

static void sb16_pump_audio(void) {
  if (ring_count == 0) {
    sb16_is_playing = false;
    return;
  }

  uint32_t chunk = ring_count;
  if (chunk > 2048)
    chunk = 2048; // Try smaller chunk to reduce latency (2048 bytes = ~46ms)

  // Read from ring buffer, handling wrap
  uint32_t unread1 = SB16_RING_SIZE - ring_tail;
  if (chunk <= unread1) {
    memcpy(sb16_dma_buf, sb16_ring + ring_tail, chunk);
    ring_tail = (ring_tail + chunk) % SB16_RING_SIZE;
  } else {
    memcpy(sb16_dma_buf, sb16_ring + ring_tail, unread1);
    uint32_t rem = chunk - unread1;
    memcpy(sb16_dma_buf + unread1, sb16_ring, rem);
    ring_tail = rem;
  }
  ring_count -= chunk;

  sb16_is_playing = true;
  sb16_play_chunk((uint32_t)sb16_dma_phys, chunk, (uint16_t)dsp_sample_rate,
                  dsp_channels, dsp_bits);
}

static uint32_t dsp_vfs_write(struct vfs_node *node, uint32_t offset,
                              uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;
  if (!sb16_present || !buffer || size == 0)
    return 0;
  if (ensure_dma_buffer() != 0)
    return 0;

  uint32_t to_write = size;
  uint32_t written = 0;
  while (to_write > 0) {
    hal_irq_disable();
    uint32_t space = SB16_RING_SIZE - ring_count;

    if (space == 0) {
      hal_irq_enable();
      // Ring buffer full. Do not block, just return.
      break;
    }

    uint32_t chunk = to_write < space ? to_write : space;

    uint32_t space1 = SB16_RING_SIZE - ring_head;
    if (chunk <= space1) {
      memcpy(sb16_ring + ring_head, buffer + written, chunk);
      ring_head = (ring_head + chunk) % SB16_RING_SIZE;
    } else {
      memcpy(sb16_ring + ring_head, buffer + written, space1);
      uint32_t rem = chunk - space1;
      memcpy(sb16_ring, buffer + written + space1, rem);
      ring_head = rem;
    }
    ring_count += chunk;

    if (!sb16_is_playing) {
      sb16_pump_audio();
    }
    hal_irq_enable();

    written += chunk;
    to_write -= chunk;
  }
  return written;
}

static int sb16_vfs_poll(struct vfs_node *node, int events) {
  (void)node;
  if (!sb16_present)
    return 0;

  int revents = 0;
  hal_irq_disable();
  uint32_t cnt = ring_count;
  hal_irq_enable();

  if ((events & (POLLOUT | POLLWRNORM)) && cnt < SB16_RING_SIZE) {
    revents |= (events & (POLLOUT | POLLWRNORM));
  }
  return revents;
}

// VFS registration

void sb16_register_vfs(void) {
  if (!sb16_present)
    return;

  vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
  if (!node)
    return;

  vfs_node_init(node);
  strcpy(node->name, "sb16");
  node->flags = FS_CHARDEV;
  node->mask = 0666;
  node->length = 0;
  node->write = dsp_vfs_write;
  node->ioctl = sb16_ioctl;
  node->poll = sb16_vfs_poll;

  // Register as /dev/sb16 only - audio_dsp dispatcher handles /dev/dsp
  fb_register_device_node("sb16", node);
}
