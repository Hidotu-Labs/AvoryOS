#include "gdt.h"
#include "../lib/string.h"
#include "../smp/cpu.h"

// 0: Null, 1: KCode, 2: KData, 3: UCode32, 4: UData, 5: UCode64,
// 6..: one 16-byte TSS descriptor per logical CPU.
//
// The TSS is NOT global state: RSP0 is the kernel stack the CPU switches to
// when an interrupt arrives from Ring 3, so every core must have its own.
// With a single shared TSS, a Ring-3 interrupt on one core pushes its frame
// onto whichever kernel stack another core happened to write RSP0 last, and
// the two cores then interleave frames on the same stack.  The visible
// symptom is a wild return (e.g. RIP=0x23, the user SS selector) long after
// the corrupting interrupt.
#define GDT_TSS_BASE 6
#define GDT_ENTRIES (GDT_TSS_BASE + 2 * MAX_CPUS)

static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr gp;
static struct tss_entry tss_table[MAX_CPUS];

extern void gdt_flush(uint64_t);

static inline uint16_t tss_selector(uint32_t cpu_id) {
  return (uint16_t)((GDT_TSS_BASE + 2 * cpu_id) * 8);
}

static inline void ltr(uint16_t sel) {
  __asm__ volatile("ltr %0" : : "r"(sel));
}

static void gdt_set_gate(int num, uint64_t base, uint64_t limit, uint8_t access,
                         uint8_t gran) {
  gdt[num].base_low = (base & 0xFFFF);
  gdt[num].base_middle = (base >> 16) & 0xFF;
  gdt[num].base_high = (base >> 24) & 0xFF;

  gdt[num].limit_low = (limit & 0xFFFF);
  gdt[num].granularity = ((limit >> 16) & 0x0F);

  gdt[num].granularity |= (gran & 0xF0);
  gdt[num].access = access;
}

static void gdt_set_tss(int num, uint64_t base, uint32_t limit) {
  gdt_set_gate(num, base, limit, 0x89, 0x00);
  gdt[num + 1].limit_low = (uint16_t)(base >> 32);
  gdt[num + 1].base_low = (uint16_t)(base >> 48);
  gdt[num + 1].base_middle = 0;
  gdt[num + 1].access = 0;
  gdt[num + 1].granularity = 0;
  gdt[num + 1].base_high = 0;
}

void tss_set_rsp0(uint64_t rsp0) {
  // Identify the calling CPU from the Task Register instead of GS: the TSS is
  // loaded before per-CPU GS data exists, and GS-based lookup would be wrong
  // (or fault) if this is ever called during that window.
  uint16_t sel;
  __asm__ volatile("str %0" : "=r"(sel));
  uint32_t index = (uint32_t)(sel >> 3);
  if (index < GDT_TSS_BASE)
    return;
  uint32_t cpu_id = (index - GDT_TSS_BASE) >> 1;
  if (cpu_id >= MAX_CPUS)
    return;
  tss_table[cpu_id].rsp0 = rsp0;
}

void gdt_init(void) {
  gp.limit = sizeof(gdt) - 1;
  gp.base = (uint64_t)&gdt;

  // 0: Null descriptor
  gdt_set_gate(0, 0, 0, 0, 0);

  // 1: Kernel Code descriptor (0x08)
  gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xAF);

  // 2: Kernel Data descriptor (0x10)
  gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

  // 3: User Code 32-bit (compatibility mode) (0x1B)
  gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);

  // 4: User Data descriptor (0x23)   - DPL 3, Data R/W
  gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);

  // 5: User Code descriptor (0x2B)   - DPL 3, Code Exec/Read, 64-bit
  gdt_set_gate(5, 0, 0xFFFFFFFF, 0xFA, 0xAF);

  // One TSS per CPU: CPU i uses descriptor (6 + 2*i), i.e. selector 0x30+16i.
  for (uint32_t i = 0; i < MAX_CPUS; i++) {
    memset(&tss_table[i], 0, sizeof(struct tss_entry));
    tss_table[i].iopb_offset = sizeof(struct tss_entry);
    gdt_set_tss(GDT_TSS_BASE + 2 * (int)i, (uint64_t)&tss_table[i],
                sizeof(struct tss_entry) - 1);
  }

  gdt_flush((uint64_t)&gp);
  ltr(tss_selector(0));
}

void gdt_load_ap(uint32_t cpu_id) {
  gdt_flush((uint64_t)&gp);
  if (cpu_id < MAX_CPUS)
    ltr(tss_selector(cpu_id));
}
