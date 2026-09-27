#include "irq.h"
#include "../console/klog.h"
#include "../mm/heap.h"
#include "acpi/acpi.h"
#include "apic/ioapic.h"
#include "apic/lapic.h"
#include "lib/string.h"
#include <stddef.h>

// Linked list node for shared IRQ handlers
typedef struct irq_handler_node {
  isr_t handler;
  uint64_t hits;
  struct irq_handler_node *next;
} irq_handler_node_t;

// Pool for early-boot allocations before heap is ready
#define EARLY_POOL_SIZE 32
static irq_handler_node_t early_node_pool[EARLY_POOL_SIZE];
static uint32_t early_pool_idx = 0;

// Track early pool usage for proper cleanup
static bool early_pool_used[EARLY_POOL_SIZE] = {false};

// Registry tracking all handlers and flags per legacy IRQ
static struct {
  irq_handler_node_t *handlers;
  uint16_t flags;
  bool registered;
} irq_registry[224];

// Master dispatcher called by the IDT for all managed IRQs.
// IMPORTANT: This dispatcher does NOT send EOI. The caller (isr_handler)
// sends EOI after we return, ensuring all shared handlers complete first.
// Individual handlers MUST NOT send EOI themselves - doing so would break
// shared IRQ handling by signaling completion before other handlers run.
// Exception: The timer handler sends EOI early to allow preemption (harmless
// double EOI).
static void irq_master_dispatcher(struct registers *regs) {
  uint8_t irq_no = regs->int_no - 32;
  if (irq_no >= 224)
    return;

  irq_handler_node_t *node = irq_registry[irq_no].handlers;
  while (node) {
    if (node->handler) {
      node->handler(regs);
      node->hits++; // Track that this handler was called
    }
    node = node->next;
  }
}

static irq_handler_node_t *alloc_node(void) {
  if (early_pool_idx < EARLY_POOL_SIZE) {
    early_pool_used[early_pool_idx] = true;
    return &early_node_pool[early_pool_idx++];
  }
  return (irq_handler_node_t *)kmalloc(sizeof(irq_handler_node_t));
}

static void free_node(irq_handler_node_t *node) {
  // Check if node is from early pool
  if (node >= early_node_pool && node < early_node_pool + EARLY_POOL_SIZE) {
    size_t idx = node - early_node_pool;
    early_pool_used[idx] = false;
    // Reset pool state if all nodes are unused
    bool any_used = false;
    for (size_t i = 0; i < EARLY_POOL_SIZE; i++) {
      if (early_pool_used[i]) {
        any_used = true;
        break;
      }
    }
    if (!any_used)
      early_pool_idx = 0;
  } else {
    kfree(node);
  }
}

bool irq_install_handler(uint8_t irq_no, isr_t handler, uint16_t flags) {
  if (irq_no >= 224)
    return false;

  irq_handler_node_t *new_node = alloc_node();
  if (!new_node)
    return false;
  new_node->handler = handler;
  new_node->hits = 0;
  new_node->next = NULL;

  if (irq_registry[irq_no].handlers == NULL) {
    irq_registry[irq_no].handlers = new_node;
    register_interrupt_handler(32 + irq_no, irq_master_dispatcher);
  } else {
    irq_handler_node_t *curr = irq_registry[irq_no].handlers;
    while (curr->next)
      curr = curr->next;
    curr->next = new_node;
  }
  irq_registry[irq_no].flags = flags;
  irq_registry[irq_no].registered = true;

  if (ioapic_is_ready() && lapic_is_ready()) {
    uint32_t gsi = irq_no;
    uint16_t route_flags = flags;
    if (route_flags == 0)
      acpi_get_irq_override(irq_no, &gsi, &route_flags);
    ioapic_route_irq((uint8_t)gsi, 32 + irq_no, (uint8_t)lapic_get_id(),
                     route_flags);
  }
  return true;
}

void irq_manager_sync(void) {
  for (uint8_t i = 0; i < 224; i++) {
    if (irq_registry[i].registered) {
      uint32_t gsi = i;
      uint16_t flags = irq_registry[i].flags;
      if (flags == 0)
        acpi_get_irq_override(i, &gsi, &flags);
      if (ioapic_is_ready() && lapic_is_ready()) {
        ioapic_route_irq((uint8_t)gsi, 32 + i, (uint8_t)lapic_get_id(), flags);
      }
    }
  }
}

bool irq_uninstall_handler(uint8_t irq_no, isr_t handler) {
  if (irq_no >= 224)
    return false;

  irq_handler_node_t **curr = &irq_registry[irq_no].handlers;
  while (*curr) {
    if ((*curr)->handler == handler) {
      irq_handler_node_t *to_remove = *curr;
      *curr = (*curr)->next; // Unlink from list
      free_node(to_remove);

      // If no handlers remain, mark as unregistered
      if (irq_registry[irq_no].handlers == NULL) {
        irq_registry[irq_no].registered = false;
        irq_registry[irq_no].flags = 0;
      }
      return true;
    }
    curr = &(*curr)->next;
  }
  return false; // Handler not found
}

void irq_test_trigger(uint8_t irq_no) {
  uint8_t vector = 32 + irq_no;
  if (vector == 47) {
    __asm__ volatile("int $47");
  } else if (vector == 33) {
    __asm__ volatile("int $33");
  }
}
