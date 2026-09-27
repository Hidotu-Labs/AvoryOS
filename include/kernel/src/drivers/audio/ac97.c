#include "ac97.h"
#include "hal/hal.h"
#include "../../apic/ioapic.h"
#include "../../apic/lapic.h"
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
#include "../pci/pci.h"

static struct pci_device *ac97_dev = NULL;
static uint16_t nam_base = 0;
static uint16_t nabm_base = 0;
static bool ac97_present = false;
static uint32_t ac97_sample_rate = 48000;
static uint8_t ac97_channels = 2;
static uint8_t ac97_bits = 16;

#define SNDCTL_DSP_SPEED 0xC0045002
#define SNDCTL_DSP_STEREO 0xC0045003
#define SNDCTL_DSP_SETFMT 0xC0045005
#define SNDCTL_DSP_CHANNELS 0xC0045006
#define SNDCTL_DSP_GETODELAY 0x80044D1D
#define AFMT_U8 0x00000008
#define AFMT_S16_LE 0x00000010

// DMA and BDL structures
#define AC97_BDL_ENTRIES 32
static struct ac97_bdl_entry *ac97_bdl = NULL;
static uint64_t ac97_bdl_phys = 0;

#define AC97_BUFFER_SIZE 4096 // 4KB per buffer (~21ms @ 48kHz stereo 16-bit)
static uint8_t *ac97_buffers[AC97_BDL_ENTRIES];
static uint64_t ac97_buffers_phys[AC97_BDL_ENTRIES];

// Ring buffer for VFS
#define AC97_RING_SIZE                                                         \
  (64 * 1024) // 64KB total buffering (~340ms @ 48kHz stereo 16-bit)
static uint8_t *ac97_ring = NULL;
static volatile uint32_t ring_head = 0;
static volatile uint32_t ring_tail = 0;
static volatile uint32_t ring_count = 0;
static volatile bool ac97_is_playing = false;
static volatile uint64_t total_played_bytes = 0;
static volatile uint32_t total_played_blocks = 0;

// Register Access

static void ac97_nam_write(uint8_t reg, uint16_t val) {
  outw(nam_base + reg, val);
}

static uint16_t ac97_nam_read(uint8_t reg) { return inw(nam_base + reg); }

static void ac97_nabm_write8(uint8_t reg, uint8_t val) {
  outb(nabm_base + reg, val);
}

static void ac97_nabm_write16(uint8_t reg, uint16_t val) {
  outw(nabm_base + reg, val);
}

static void ac97_nabm_write32(uint8_t reg, uint32_t val) {
  outl(nabm_base + reg, val);
}

static uint8_t ac97_nabm_read8(uint8_t reg) { return inb(nabm_base + reg); }

static uint16_t ac97_nabm_read16(uint8_t reg) { return inw(nabm_base + reg); }

// ISR

static void ac97_pump_audio(void);

static void ac97_isr(struct registers *regs) {
  (void)regs;
  uint16_t sr = ac97_nabm_read16(AC97_PO_SR);

  if (!(sr & (AC97_SR_BCIS | AC97_SR_LVBCI | AC97_SR_FIFO_ERR))) {
    return; // Not our interrupt or no relevant bit set
  }

  // Clear interrupt bits by writing 1 to them
  ac97_nabm_write16(AC97_PO_SR,
                    sr & (AC97_SR_BCIS | AC97_SR_LVBCI | AC97_SR_FIFO_ERR));

  if (sr & AC97_SR_BCIS) {
    // Buffer completion
    ac97_pump_audio();
  }
}

// Initialization

static void ac97_pump_audio(void) {
  if (ring_count == 0) {
    ac97_is_playing = false;
    return;
  }

  // Try to keep up to 3 buffers queued (approx 63ms look-ahead)
  // This provides a safety margin against jitter without adding too much lag.
  for (int i = 0; i < 3; i++) {
    if (ring_count == 0)
      break;

    // Get current index and next index to fill
    uint8_t civ = ac97_nabm_read8(AC97_PO_CIV);
    uint8_t lvi = ac97_nabm_read8(AC97_PO_LVI);

    // We want to fill the next available slot
    uint8_t next = (lvi + 1) % AC97_BDL_ENTRIES;

    // Check if we are caught up to the hardware
    if (next == civ)
      break;

    uint32_t chunk = ring_count;
    if (chunk > AC97_BUFFER_SIZE)
      chunk = AC97_BUFFER_SIZE;

    // Read from ring buffer, handling wrap
    uint32_t unread1 = AC97_RING_SIZE - ring_tail;
    if (chunk <= unread1) {
      memcpy(ac97_buffers[next], ac97_ring + ring_tail, chunk);
      ring_tail = (ring_tail + chunk) % AC97_RING_SIZE;
    } else {
      memcpy(ac97_buffers[next], ac97_ring + ring_tail, unread1);
      uint32_t rem = chunk - unread1;
      memcpy(ac97_buffers[next] + unread1, ac97_ring, rem);
      ring_tail = rem;
    }
    ring_count -= chunk;
    total_played_bytes += chunk;
    total_played_blocks = (uint32_t)(total_played_bytes / AC97_BUFFER_SIZE);

    // Update BDL entry length (AC97 length is in samples, 1 sample = 2 bytes)
    ac97_bdl[next].length = (uint16_t)(chunk / 2);
    ac97_bdl[next].flags = AC97_BDL_FLAG_IOC; // Interrupt on completion

    // Update LVI to point to this new buffer
    ac97_nabm_write8(AC97_PO_LVI, next);
    ac97_is_playing = true;
  }
}

static void ac97_set_format(uint32_t rate) {
  if (!ac97_present)
    return;

  // Check if VRA (Variable Rate Audio) is supported
  uint16_t ext_id = ac97_nam_read(AC97_NAM_EXT_AUDIO_ID);
  if (ext_id & AC97_EXT_AUDIO_VRA) {
    // Enable VRA
    uint16_t ext_ctrl = ac97_nam_read(AC97_NAM_EXT_AUDIO_CTRL);
    ac97_nam_write(AC97_NAM_EXT_AUDIO_CTRL, ext_ctrl | AC97_EXT_AUDIO_VRA);

    // Set rate
    ac97_nam_write(AC97_NAM_PCM_FRONT_DAC_RATE, (uint16_t)rate);
    ac97_sample_rate = ac97_nam_read(AC97_NAM_PCM_FRONT_DAC_RATE);

    klog_puts("[AC97] VRA enabled, rate set to ");
    klog_uint64(ac97_sample_rate);
    klog_puts(" Hz\n");
  } else {
    klog_puts("[AC97] VRA NOT supported, fixed at 48000 Hz\n");
    ac97_sample_rate = 48000;
  }
}

void ac97_init(void) {
  klog_puts("[AC97] Searching for controller...\n");

  // Look for Intel 82801 (ICH) AC97 Audio
  ac97_dev = pci_find_device_by_id(0x8086, 0x2415);
  if (!ac97_dev) {
    ac97_dev = pci_find_device_by_id(0x8086, 0x2425); // ICH0
  }
  if (!ac97_dev) {
    ac97_dev = pci_find_device_by_id(0x10DE, 0x006A); // nForce
  }
  if (!ac97_dev) {
    ac97_dev = pci_find_device(0x04, 0x01); // Multimedia Audio Controller
  }

  if (!ac97_dev) {
    klog_puts("[AC97] No AC97 controller found.\n");
    return;
  }

  klog_puts("[AC97] Found controller at ");
  klog_uint64(ac97_dev->bus);
  klog_putchar(':');
  klog_uint64(ac97_dev->slot);
  klog_putchar('.');
  klog_uint64(ac97_dev->func);
  klog_puts("\n");

  pci_enable_bus_mastering(ac97_dev);

  nam_base = ac97_dev->bar[0] & ~1;
  nabm_base = ac97_dev->bar[1] & ~1;

  klog_puts("[AC97] NAM base: 0x");
  klog_uint64(nam_base);
  klog_puts(", NABM base: 0x");
  klog_uint64(nabm_base);
  klog_puts("\n");

  // Reset NAM
  ac97_nam_write(AC97_NAM_RESET, 0x1);
  // Reset NABM
  ac97_nabm_write8(AC97_PO_CR, AC97_CR_RR);

  // Warm reset wait
  for (int i = 0; i < 1000; i++)
    io_wait();

  // Set volumes (Mute is bit 15, 0 is max volume for AC97)
  ac97_nam_write(AC97_NAM_MASTER_VOLUME, 0x0000);  // Max volume
  ac97_nam_write(AC97_NAM_PCM_OUT_VOLUME, 0x0000); // Max volume

  // Allocate BDL
  uint64_t bdl_raw = (uint64_t)pmm_alloc_pages(1); // 4KB is plenty
  ac97_bdl_phys = bdl_raw;
  ac97_bdl = (struct ac97_bdl_entry *)(ac97_bdl_phys + pmm_get_hhdm_offset());
  memset(ac97_bdl, 0, 4096);

  // Allocate buffers
  for (int i = 0; i < AC97_BDL_ENTRIES; i++) {
    uint64_t buf_raw = (uint64_t)pmm_alloc_pages(4); // 16KB
    ac97_buffers_phys[i] = buf_raw;
    ac97_buffers[i] = (uint8_t *)(buf_raw + pmm_get_hhdm_offset());

    ac97_bdl[i].pointer = (uint32_t)ac97_buffers_phys[i];
    ac97_bdl[i].length = 0; // Initially empty
    ac97_bdl[i].flags = AC97_BDL_FLAG_IOC;
  }

  // Set BDL base address
  ac97_nabm_write32(AC97_PO_BBA, (uint32_t)ac97_bdl_phys);

  uint8_t irq = ac97_dev->irq_line;
  irq_install_handler(irq, ac97_isr, 0x000F);
  // Wait, let's check what others use. Usually PCI is Level, Active Low.
  // In kernel.c, I saw 0 used for PIT/KBD.
  // ioapic.c: flags bit 0 is polarity (1=low), bit 1 is trigger (1=level).

  // Allocate ring buffer
  ac97_ring = kmalloc(AC97_RING_SIZE);
  memset(ac97_ring, 0, AC97_RING_SIZE);

  ac97_present = true;
  klog_puts("[OK] AC97 Initialized.\n");
}

static uint32_t ac97_vfs_write(struct vfs_node *node, uint32_t offset,
                               uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;
  if (!ac97_present || !buffer || size == 0)
    return 0;

  uint32_t written = 0;
  while (written < size) {
    hal_irq_disable();
    uint32_t space = AC97_RING_SIZE - ring_count;

    // Calculate how many input bytes we can process based on available space in
    // ring buffer Hardware always expects 16-bit stereo (4 bytes per sample
    // frame)
    uint32_t input_bytes_per_frame = (ac97_bits / 8) * ac97_channels;
    uint32_t output_bytes_per_frame = 4; // 16-bit stereo

    if (space < output_bytes_per_frame) {
      hal_irq_enable();
      break; // Buffer full
    }

    // Process one frame at a time for simplicity and to handle format
    // conversion
    if (written + input_bytes_per_frame > size) {
      hal_irq_enable();
      break; // Partial frame at end of user buffer, skip for now
    }

    int16_t left = 0, right = 0;

    if (ac97_bits == 8) {
      // 8-bit unsigned to 16-bit signed
      if (ac97_channels == 1) {
        left = right = (int16_t)(((int16_t)buffer[written] - 128) << 8);
      } else {
        left = (int16_t)(((int16_t)buffer[written] - 128) << 8);
        right = (int16_t)(((int16_t)buffer[written + 1] - 128) << 8);
      }
    } else {
      // 16-bit signed LE
      if (ac97_channels == 1) {
        left = right = (int16_t)(buffer[written] | (buffer[written + 1] << 8));
      } else {
        left = (int16_t)(buffer[written] | (buffer[written + 1] << 8));
        right = (int16_t)(buffer[written + 2] | (buffer[written + 3] << 8));
      }
    }

    // Write frames to ring buffer
    uint8_t frame[4];
    frame[0] = (uint8_t)(left & 0xFF);
    frame[1] = (uint8_t)((left >> 8) & 0xFF);
    frame[2] = (uint8_t)(right & 0xFF);
    frame[3] = (uint8_t)((right >> 8) & 0xFF);

    for (int i = 0; i < 4; i++) {
      ac97_ring[ring_head] = frame[i];
      ring_head = (ring_head + 1) % AC97_RING_SIZE;
    }
    ring_count += 4;
    written += input_bytes_per_frame;

    if (!ac97_is_playing && ring_count >= AC97_BUFFER_SIZE) {
      // Start playing only when we have enough data to fill at least one
      // hardware buffer
      ac97_pump_audio();
      ac97_nabm_write8(AC97_PO_CR, AC97_CR_RPBM | AC97_CR_IOCE | AC97_CR_LVBIE);
    }
    hal_irq_enable();
  }
  return written;
}

int ac97_ioctl(struct vfs_node *node, uint32_t request, uint64_t arg) {
  (void)node;
  if (!ac97_present)
    return -5; // EIO

  switch (request) {
  case 0x5000: // SNDCTL_DSP_RESET
  {
    hal_irq_disable();
    ring_head = ring_tail = ring_count = 0;
    total_played_bytes = 0;
    total_played_blocks = 0;
    ac97_is_playing = false;
    ac97_nabm_write8(AC97_PO_CR, 0);
    hal_irq_enable();
    return 0;
  }
  case 0xC004500A: // SNDCTL_DSP_SETFRAGMENT
  case 0x5001: // SNDCTL_DSP_SYNC
  case 0x5008: // SNDCTL_DSP_POST
  case 0x500B: // SNDCTL_DSP_NONBLOCK (legacy)
  case 0x500E: // SNDCTL_DSP_NONBLOCK
  case 0xC0045009: // SNDCTL_DSP_SUBDIVIDE
    return 0; // Success
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
    *caps = 0x00001000 | 0x00000100 | 0x00000200 | 0x00020000 | 0x00010000; // TRIGGER | DUPLEX | REALTIME | OUTPUT | INPUT
    return 0;
  }
  case 0x80045004: // SOUND_PCM_READ_BLKSIZE
  case 0xC0045004: // SNDCTL_DSP_GETBLKSIZE
  {
    int *blksize = (int *)arg;
    if (!blksize) return -14;
    *blksize = AC97_BUFFER_SIZE;
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
    info->fragsize = AC97_BUFFER_SIZE;
    info->fragstotal = AC97_RING_SIZE / AC97_BUFFER_SIZE;
    info->bytes = AC97_RING_SIZE - ring_count;
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
    info->fragsize = AC97_BUFFER_SIZE;
    info->fragstotal = AC97_RING_SIZE / AC97_BUFFER_SIZE;
    info->bytes = 0;
    info->fragments = 0;
    return 0;
  }
  case 0x800C5012: // SNDCTL_DSP_GETOPTR
  case 0x80105012:
  case 0x80045012:
  case 0x00005012:
  case 0x800C5007:
  case 0x80045007:
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
  case 0x800C5011: // SNDCTL_DSP_GETIPTR
  case 0x80105011:
  case 0x80045011:
  case 0x00005011:
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
      if (!ac97_is_playing) {
        ac97_pump_audio();
        ac97_nabm_write8(AC97_PO_CR, AC97_CR_RPBM | AC97_CR_IOCE | AC97_CR_LVBIE);
      }
    } else if (*trig == 0) {
      if (ac97_is_playing) {
        ac97_nabm_write8(AC97_PO_CR, 0);
        ac97_is_playing = false;
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
    *trig = ac97_is_playing ? 0x02 : 0x00;
    hal_irq_enable();
    return 0;
  }
  case SNDCTL_DSP_SPEED: {
    uint32_t *rate = (uint32_t *)arg;
    if (!rate)
      return -14; // EFAULT
    ac97_set_format(*rate);
    *rate = ac97_sample_rate;
    return 0;
  }
  case SNDCTL_DSP_STEREO: {
    int *stereo = (int *)arg;
    if (!stereo)
      return -14;
    ac97_channels = (*stereo) ? 2 : 1;
    *stereo = (ac97_channels == 2);
    return 0;
  }
  case SNDCTL_DSP_CHANNELS: {
    int *ch = (int *)arg;
    if (!ch)
      return -14;
    if (*ch == 1 || *ch == 2) {
      ac97_channels = (uint8_t)*ch;
    }
    *ch = ac97_channels;
    return 0;
  }
  case SNDCTL_DSP_SETFMT: {
    int *fmt = (int *)arg;
    if (!fmt)
      return -14;
    if (*fmt == AFMT_U8 || *fmt == AFMT_S16_LE) {
      ac97_bits = (*fmt == AFMT_U8) ? 8 : 16;
    }
    *fmt = (ac97_bits == 8) ? AFMT_U8 : AFMT_S16_LE;
    return 0;
  }
  case SNDCTL_DSP_GETODELAY: {
    int *delay = (int *)arg;
    if (!delay)
      return -14;
    hal_irq_disable();
    *delay = (int)ring_count;
    hal_irq_enable();
    return 0;
  }
  default:
    return -25; // ENOTTY
  }
}

static int ac97_vfs_poll(struct vfs_node *node, int events) {
  (void)node;
  if (!ac97_present)
    return 0;

  int revents = 0;
  hal_irq_disable();
  uint32_t cnt = ring_count;
  hal_irq_enable();

  if ((events & (POLLOUT | POLLWRNORM)) && cnt < AC97_RING_SIZE) {
    revents |= (events & (POLLOUT | POLLWRNORM));
  }
  return revents;
}

void ac97_register_vfs(void) {
  if (!ac97_present)
    return;

  vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
  if (!node)
    return;

  vfs_node_init(node);
  strcpy(node->name, "ac97");
  node->flags = FS_CHARDEV;
  node->mask = 0666;
  node->length = 0;
  node->write = ac97_vfs_write;
  node->ioctl = ac97_ioctl;
  node->poll = ac97_vfs_poll;

  // Register as /dev/ac97 only - audio_dsp dispatcher handles /dev/dsp
  fb_register_device_node("ac97", node);
}
