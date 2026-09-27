#include "panic_screen.h"

#include "isr.h"
#include "../apic/lapic.h"
#include "../font/font.h"
#include "../mm/vmm.h"
#include "../smp/cpu.h"
#include <limine.h>
#include <stddef.h>
#include <stdint.h>

#define PANIC_MAX_FB_BYTES (128ULL * 1024 * 1024)
#define PANIC_LINE_HEIGHT 20u
#define PANIC_MARGIN 24u

struct panic_framebuffer {
  volatile uint8_t *base;
  uint64_t size;
  uint32_t width;
  uint32_t height;
  uint32_t pitch;
  uint32_t bpp;
  uint8_t bytes_per_pixel;
  uint8_t red_shift, red_size;
  uint8_t green_shift, green_size;
  uint8_t blue_shift, blue_size;
};

struct text_line {
  char text[160];
  uint32_t length;
};

static struct panic_framebuffer panic_fb;
static volatile uint32_t panic_active;
static volatile uint32_t panic_render_started;
static volatile uint64_t panic_target_mask;
static volatile uint64_t panic_stopped_mask;

static bool canonical_address(uint64_t address) {
  uint64_t upper = address >> 48;
  return upper == 0 || upper == 0xFFFF;
}

static bool valid_channel(uint8_t shift, uint8_t length, uint32_t bpp) {
  return length != 0 && length <= 16 && shift < bpp &&
         (uint32_t)shift + length <= bpp;
}

/* Do not consult GS/current-thread state on the panic path. The fault may
 * itself have been caused by corrupt per-CPU state. CPUID gives the hardware
 * APIC ID without touching kernel memory. */
uint32_t panic_screen_current_apic_id(void) {
  uint32_t eax, ebx, ecx, edx;
  __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                   : "a"(0), "c"(0));
  uint32_t max_leaf = eax;
  if (max_leaf >= 0x0B) {
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0x0B), "c"(0));
    if (ebx != 0)
      return edx;
  }
  __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                   : "a"(1), "c"(0));
  return ebx >> 24;
}

void panic_screen_init(const struct limine_framebuffer *framebuffer) {
  panic_fb.base = NULL;
  if (!framebuffer || !framebuffer->address || framebuffer->width < 640 ||
      framebuffer->height < 448 || framebuffer->width > 16384 ||
      framebuffer->height > 16384 ||
      (framebuffer->bpp != 16 && framebuffer->bpp != 24 &&
       framebuffer->bpp != 32))
    return;

  uint32_t bytes_per_pixel = (uint32_t)(framebuffer->bpp + 7) / 8;
  uint64_t minimum_pitch = (uint64_t)framebuffer->width * bytes_per_pixel;
  uint64_t size = (uint64_t)framebuffer->height * framebuffer->pitch;
  uint64_t address = (uint64_t)(uintptr_t)framebuffer->address;
  if (framebuffer->pitch < minimum_pitch || size == 0 ||
      size > PANIC_MAX_FB_BYTES || size / framebuffer->pitch != framebuffer->height ||
      !canonical_address(address) ||
      !valid_channel(framebuffer->red_mask_shift, framebuffer->red_mask_size,
                     framebuffer->bpp) ||
      !valid_channel(framebuffer->green_mask_shift,
                     framebuffer->green_mask_size, framebuffer->bpp) ||
      !valid_channel(framebuffer->blue_mask_shift,
                     framebuffer->blue_mask_size, framebuffer->bpp))
    return;

  panic_fb.base = (volatile uint8_t *)framebuffer->address;
  panic_fb.size = size;
  panic_fb.width = (uint32_t)framebuffer->width;
  panic_fb.height = (uint32_t)framebuffer->height;
  panic_fb.pitch = (uint32_t)framebuffer->pitch;
  panic_fb.bpp = (uint32_t)framebuffer->bpp;
  panic_fb.bytes_per_pixel = (uint8_t)bytes_per_pixel;
  panic_fb.red_shift = (uint8_t)framebuffer->red_mask_shift;
  panic_fb.red_size = (uint8_t)framebuffer->red_mask_size;
  panic_fb.green_shift = (uint8_t)framebuffer->green_mask_shift;
  panic_fb.green_size = (uint8_t)framebuffer->green_mask_size;
  panic_fb.blue_shift = (uint8_t)framebuffer->blue_mask_shift;
  panic_fb.blue_size = (uint8_t)framebuffer->blue_mask_size;
}

bool panic_screen_stop_other_cpus(void) {
  uint32_t expected_active = 0;
  if (!__atomic_compare_exchange_n(&panic_active, &expected_active, 1, false,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return false;

  __atomic_store_n(&panic_target_mask, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&panic_stopped_mask, 0, __ATOMIC_RELAXED);
  if (!lapic_is_ready())
    return true;

  uint32_t current_id = panic_screen_current_apic_id();
  uint32_t count = cpu_get_count();
  uint64_t targets = 0;
  for (uint32_t i = 0; i < count; i++) {
    struct cpu_info *cpu = cpu_get_info(i);
    if (!cpu || cpu->apic_id == current_id)
      continue;
    uint8_t status = __atomic_load_n(&cpu->status, __ATOMIC_ACQUIRE);
    if (status == CPU_STATUS_BSP || status == CPU_STATUS_ONLINE) {
      targets |= 1ULL << i;
      lapic_send_nmi(cpu->apic_id);
    }
  }
  __atomic_store_n(&panic_target_mask, targets, __ATOMIC_RELEASE);

  /* NMI delivery is asynchronous. Wait briefly for online peers to acknowledge
   * their stop before painting the shared scanout, then report any stragglers. */
  for (uint32_t spins = 0; spins < 10000000; spins++) {
    uint64_t stopped = __atomic_load_n(&panic_stopped_mask, __ATOMIC_ACQUIRE);
    if ((stopped & targets) == targets)
      break;
    __asm__ volatile("pause" ::: "memory");
  }
  return true;
}

bool panic_screen_is_active(void) {
  return __atomic_load_n(&panic_active, __ATOMIC_ACQUIRE) != 0;
}

void panic_screen_ack_stopped_cpu(void) {
  uint32_t apic_id = panic_screen_current_apic_id();
  uint32_t count = cpu_get_count();
  for (uint32_t i = 0; i < count; i++) {
    struct cpu_info *cpu = cpu_get_info(i);
    if (cpu && cpu->apic_id == apic_id) {
      __atomic_fetch_or(&panic_stopped_mask, 1ULL << i, __ATOMIC_RELEASE);
      return;
    }
  }
}

void panic_screen_stop_this_cpu(void) {
  for (;;) {
    __asm__ volatile("cli; hlt" ::: "memory");
  }
}

static uint32_t scale_channel(uint8_t value, uint8_t shift, uint8_t length) {
  uint32_t max_value = (1u << length) - 1u;
  return (((uint32_t)value * max_value + 127u) / 255u) << shift;
}

static uint32_t pack_rgb(uint32_t rgb) {
  uint32_t pixel = scale_channel((uint8_t)(rgb >> 16), panic_fb.red_shift,
                                 panic_fb.red_size) |
                   scale_channel((uint8_t)(rgb >> 8), panic_fb.green_shift,
                                 panic_fb.green_size) |
                   scale_channel((uint8_t)rgb, panic_fb.blue_shift,
                                 panic_fb.blue_size);
  return pixel;
}

static void put_pixel(uint32_t x, uint32_t y, uint32_t rgb) {
  if (x >= panic_fb.width || y >= panic_fb.height)
    return;
  uint64_t offset = (uint64_t)y * panic_fb.pitch +
                    (uint64_t)x * panic_fb.bytes_per_pixel;
  if (offset + panic_fb.bytes_per_pixel > panic_fb.size)
    return;
  uint32_t pixel = pack_rgb(rgb);
  volatile uint8_t *dst = panic_fb.base + offset;
  for (uint8_t i = 0; i < panic_fb.bytes_per_pixel; i++)
    dst[i] = (uint8_t)(pixel >> (i * 8));
}

static void fill_screen(uint32_t rgb) {
  uint32_t pixel = pack_rgb(rgb);
  for (uint32_t y = 0; y < panic_fb.height; y++) {
    volatile uint8_t *row = panic_fb.base + (uint64_t)y * panic_fb.pitch;
    uint32_t visible = panic_fb.width;
    for (uint32_t x = 0; x < visible; x++) {
      uint64_t offset = (uint64_t)x * panic_fb.bytes_per_pixel;
      for (uint8_t b = 0; b < panic_fb.bytes_per_pixel; b++)
        row[offset + b] = (uint8_t)(pixel >> (b * 8));
    }
  }
}

static void draw_char(uint32_t x, uint32_t y, char c, uint32_t foreground,
                      uint32_t background) {
  const uint8_t *glyph = font_get_glyph((uint8_t)c);
  if (!glyph)
    return;
  for (uint32_t row = 0; row < 16; row++) {
    uint8_t bits = glyph[row];
    for (uint32_t col = 0; col < 8; col++)
      put_pixel(x + col, y + row,
                (bits & (0x80u >> col)) ? foreground : background);
  }
}

static void draw_line(uint32_t row, const char *text, uint32_t length,
                      uint32_t foreground, uint32_t background) {
  uint64_t y = 30ULL + (uint64_t)row * PANIC_LINE_HEIGHT;
  if (y + 16 > panic_fb.height)
    return;
  uint32_t x = PANIC_MARGIN;
  uint32_t max_x = panic_fb.width > PANIC_MARGIN ?
                       panic_fb.width - PANIC_MARGIN : panic_fb.width;
  for (uint32_t i = 0; i < length && x + 8 <= max_x; i++, x += 8)
    draw_char(x, (uint32_t)y, text[i], foreground, background);
}

static void line_init(struct text_line *line) { line->length = 0; }

static void line_text(struct text_line *line, const char *text) {
  if (!text)
    return;
  while (*text && line->length + 1 < sizeof(line->text))
    line->text[line->length++] = *text++;
}

static void line_hex(struct text_line *line, uint64_t value) {
  static const char hex[] = "0123456789ABCDEF";
  line_text(line, "0x");
  for (int shift = 60; shift >= 0; shift -= 4) {
    char digit[2] = {hex[(value >> shift) & 0xF], 0};
    line_text(line, digit);
  }
}

static void line_dec(struct text_line *line, uint64_t value) {
  char digits[21];
  uint32_t count = 0;
  do {
    digits[count++] = (char)('0' + (value % 10));
    value /= 10;
  } while (value && count < sizeof(digits));
  while (count && line->length + 1 < sizeof(line->text))
    line->text[line->length++] = digits[--count];
}

static uint32_t count_bits(uint64_t value) {
  uint32_t count = 0;
  while (value) {
    value &= value - 1;
    count++;
  }
  return count;
}

static void line_field(struct text_line *line, const char *name,
                       uint64_t value) {
  line_text(line, name);
  line_hex(line, value);
  line_text(line, "  ");
}

static void emit_line(uint32_t row, struct text_line *line,
                      uint32_t foreground) {
  draw_line(row, line->text, line->length, foreground, 0x171923);
}

static void draw_register_pair(uint32_t row, const char *a_name, uint64_t a,
                               const char *b_name, uint64_t b) {
  struct text_line line;
  line_init(&line);
  line_field(&line, a_name, a);
  if (b_name)
    line_field(&line, b_name, b);
  emit_line(row, &line, 0xCDD6F4);
}

bool panic_screen_render(const char *reason, struct registers *regs,
                         bool has_cr2, uint64_t cr2) {
  if (!panic_fb.base || !regs ||
      __atomic_exchange_n(&panic_render_started, 1, __ATOMIC_ACQ_REL))
    return false;

  /* The faulting process may not map the framebuffer aperture. Diagnostics
   * have already captured its CR3 over serial, so switch to the permanent
   * kernel map for raw scanout writes and remain there while halting. */
  uint64_t fault_cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(fault_cr3));
  uint64_t kernel_cr3 =
      (uint64_t)(uintptr_t)vmm_get_kernel_pml4() & PAGE_MASK;
  if (kernel_cr3 && (fault_cr3 & PAGE_MASK) != kernel_cr3)
    __asm__ volatile("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");

  uint64_t cr0, cr4;
  __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
  __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));

  fill_screen(0x171923);
  uint32_t banner_color = pack_rgb(0x9C2536);
  for (uint32_t y = 0; y < 26 && y < panic_fb.height; y++) {
    volatile uint8_t *row = panic_fb.base + (uint64_t)y * panic_fb.pitch;
    for (uint32_t x = 0; x < panic_fb.width; x++) {
      uint64_t offset = (uint64_t)x * panic_fb.bytes_per_pixel;
      if (offset + panic_fb.bytes_per_pixel > panic_fb.size)
        break;
      for (uint8_t b = 0; b < panic_fb.bytes_per_pixel; b++)
        row[offset + b] = (uint8_t)(banner_color >> (b * 8));
    }
  }

  draw_line(0, "AVORYOS KERNEL PANIC - DISPLAY CAPTURED", 39, 0xFFFFFF,
            0x9C2536);

  struct text_line line;
  line_init(&line);
  line_text(&line, "Reason: ");
  line_text(&line, reason);
  emit_line(1, &line, 0xF9E2AF);

  line_init(&line);
  line_text(&line, "Vector ");
  line_dec(&line, regs->int_no);
  line_text(&line, "  error=");
  line_hex(&line, regs->err_code);
  if (regs->int_no < 32) {
    extern const char *exception_messages[];
    line_text(&line, "  ");
    line_text(&line, exception_messages[regs->int_no]);
  }
  emit_line(2, &line, 0xCDD6F4);

  line_init(&line);
  line_field(&line, "RIP=", regs->rip);
  line_field(&line, "CS=", regs->cs);
  line_field(&line, "RFLAGS=", regs->rflags);
  emit_line(3, &line, 0xFFFFFF);

  line_init(&line);
  line_field(&line, "CR0=", cr0);
  if (has_cr2) {
    line_field(&line, "CR2=", cr2);
  } else {
    line_text(&line, "CR2=<not captured>");
  }
  emit_line(4, &line, 0xCDD6F4);

  line_init(&line);
  line_field(&line, "CR3=", fault_cr3);
  line_field(&line, "CR4=", cr4);
  emit_line(5, &line, 0xCDD6F4);

  line_init(&line);
  line_text(&line, "Fault-time RSP=");
  if ((regs->cs & 3) || regs->int_no == 8)
    line_hex(&line, regs->rsp);
  else
    line_hex(&line, (uint64_t)(uintptr_t)&regs->rsp);
  line_text(&line, (regs->cs & 3) ? "  from user frame" :
                                      (regs->int_no == 8 ? "  saved by IST1" :
                                                           "  reconstructed from ring-0 frame"));
  emit_line(6, &line, 0xCDD6F4);

  line_init(&line);
  line_text(&line, "Hardware APIC ID=");
  line_dec(&line, panic_screen_current_apic_id());
  emit_line(7, &line, 0xCDD6F4);

  draw_register_pair(8, "RAX=", regs->rax, "RBX=", regs->rbx);
  draw_register_pair(9, "RCX=", regs->rcx, "RDX=", regs->rdx);
  draw_register_pair(10, "RSI=", regs->rsi, "RDI=", regs->rdi);
  draw_register_pair(11, "RBP=", regs->rbp, "R8=", regs->r8);
  draw_register_pair(12, "R9=", regs->r9, "R10=", regs->r10);
  draw_register_pair(13, "R11=", regs->r11, "R12=", regs->r12);
  draw_register_pair(14, "R13=", regs->r13, "R14=", regs->r14);
  draw_register_pair(15, "R15=", regs->r15, NULL, 0);

  line_init(&line);
  line_text(&line, "Peer NMI stop acknowledgements: ");
  uint64_t targets = __atomic_load_n(&panic_target_mask, __ATOMIC_ACQUIRE);
  uint64_t stopped = __atomic_load_n(&panic_stopped_mask, __ATOMIC_ACQUIRE);
  line_dec(&line, count_bits(stopped & targets));
  line_text(&line, "/");
  line_dec(&line, count_bits(targets));
  line_text(&line, " online peers");
  emit_line(17, &line, 0x94E2D5);
  line_init(&line);
  line_text(&line, "Direct scanout captured; DRM and backbuffer paths bypassed.");
  emit_line(18, &line, 0x94E2D5);
  line_init(&line);
  line_text(&line, regs->int_no == 8
                       ? "Serial has the compact double-fault register and control-state dump."
                       : "Serial log has the page walk, code bytes and stack trace.");
  emit_line(19, &line, 0x94E2D5);
  /* The firmware scanout is write-combining; push the completed crash screen
   * out before the owner enters its permanent halt loop. */
  __asm__ volatile("sfence" ::: "memory");
  return true;
}
