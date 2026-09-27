#include "drivers/manager/udriver.h"
#include "apic/lapic.h"
#include "console/klog.h"
#include "cpu/irq.h"
#include "drivers/manager/device.h"
#include "drivers/pci/pci.h"
#include "drivers/pci/pci_irq.h"
#include "fs/devfs.h"
#include "fs/sysfs_pci.h"
#include "fs/vfs.h"
#include "io/io.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/dma_alloc.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "net/core.h"
#include "sched/sched.h"
#include "sched/wait.h"
#include "socket/epoll.h"

#define UDRIVER_MAX_CLAIMS 16U
#define UDRIVER_MAX_MAPPINGS 32U
#define UDRIVER_RDEV ((240U << 8) | 0U)
#define UDRIVER_MAX_IRQ_DISPATCH 64U
#define UDRIVER_MAX_DMA_BUFFERS 16U
#define UDRIVER_DMA_MAX_ALLOCATION (16ULL * 1024 * 1024)
#define UDRIVER_DMA_MAX_TOTAL (64ULL * 1024 * 1024)
#define PCI_COMMAND_INTX_DISABLE (1U << 10)
#define UDRIVER_NET_TX_DEPTH 16U

struct udriver_session;

struct udriver_net_endpoint {
  struct udriver_session *session;
  struct device *device;
  struct net_device netdev;
  struct udrv_net_frame tx[UDRIVER_NET_TX_DEPTH];
  uint32_t tx_head;
  uint32_t tx_tail;
  bool active;
  bool link_up;
};

struct udriver_dma_buffer {
  struct device *device;
  dma_buffer_t *buffer;
  uint32_t flags;
  uint16_t handle;
};

struct udriver_irq_binding {
  struct udriver_session *session;
  struct device *device;
  struct pci_device *pci;
  struct pci_irq irq;
  uint64_t pending;
  uint64_t total;
  uint32_t mode;
  uint8_t vector;
  bool active;
  bool legacy;
  bool masked;
  bool overflow;
  bool hardware_backed;
};

struct udriver_session {
  uint32_t owner_tgid;
  bool registered;
  char driver_name[UDRV_NAME_LEN];
  char bus_name[UDRV_BUS_LEN];
  char id_names[UDRV_MAX_IDS][UDRV_DEVICE_LEN];
  struct device_id ids[UDRV_MAX_IDS];
  struct driver driver;
  struct device *claims[UDRIVER_MAX_CLAIMS];
  uint16_t claim_tokens[UDRIVER_MAX_CLAIMS];
  bool claim_bus_master[UDRIVER_MAX_CLAIMS];
  uint16_t next_token;
  uint16_t next_dma_handle;
  size_t claim_count;
  struct {
    struct device *device;
    uint64_t address;
    uint64_t length;
    uint64_t *pml4;
    uint16_t dma_handle;
  } mappings[UDRIVER_MAX_MAPPINGS];
  struct udriver_irq_binding irqs[UDRIVER_MAX_CLAIMS];
  struct udriver_dma_buffer dma[UDRIVER_MAX_DMA_BUFFERS];
  uint64_t dma_total;
  struct udriver_net_endpoint net;
  wait_queue_t irq_waitq;
  vfs_node_t *control_node;
  spinlock_t lock;
};

static vfs_node_t driverctl_metadata;
static spinlock_t irq_dispatch_lock = SPINLOCK_INIT;
static int claimed_index(struct udriver_session *session,
                         struct device *device) {
  if (!session || !device)
    return -1;
  for (size_t i = 0; i < session->claim_count; i++)
    if (session->claims[i] == device)
      return (int)i;
  return -1;
}

static struct pci_device *pci_for_device(struct device *device) {
  if (!device || device->bus != pci_bus_type())
    return NULL;
  for (uint32_t i = 0; i < pci_get_device_count(); i++) {
    struct pci_device *pci = pci_get_device(i);
    if (pci && pci->kernel_device == device)
      return pci;
  }
  return NULL;
}

static struct udriver_irq_binding *irq_dispatch[UDRIVER_MAX_IRQ_DISPATCH];
static bool legacy_irq_installed[224];
static bool session_is_owner(struct udriver_session *session);
static void revoke_device_mappings(struct udriver_session *session, struct device *device);
static void release_device_irqs(struct udriver_session *session,
                                struct device *device);
static void release_irq_binding(struct udriver_irq_binding *binding);
static void disable_device_bus_master(struct udriver_session *session,
                                      struct device *device, bool execute);
static void free_device_dma(struct udriver_session *session,
                            struct device *device);
static void release_device_net(struct udriver_session *session,
                               struct device *device);

static bool bounded_name(const char *name, size_t capacity) {
  if (!name || !name[0])
    return false;
  for (size_t i = 0; i < capacity; i++)
    if (name[i] == '\0')
      return true;
  return false;
}

static struct device *find_bus_device(struct bus_type *bus, const char *name) {
  if (!bus || !name)
    return NULL;
  for (struct device *dev = bus->devices; dev; dev = dev->next_bus_device)
    if (strcmp(dev->name, name) == 0)
      return dev;
  return NULL;
}

static int udriver_probe(struct device *dev) {
  if (!dev || !dev->driver || dev->driver->kind != DRIVER_USER)
    return -1;
  struct udriver_session *session = dev->driver->private_data;
  if (!session || !session->registered)
    return -1;

  spinlock_acquire(&session->lock);
  if (session->claim_count >= UDRIVER_MAX_CLAIMS) {
    spinlock_release(&session->lock);
    return -1;
  }
  for (size_t i = 0; i < session->claim_count; i++) {
    if (session->claims[i] == dev) {
      spinlock_release(&session->lock);
      return -1;
    }
  }
  uint16_t token = 0;
  for (size_t attempt = 0; attempt < UDRIVER_MAX_CLAIMS + 1; attempt++) {
    session->next_token++;
    if (!session->next_token || session->next_token > UDRV_MMAP_TOKEN_MAX)
      session->next_token = 1;
    bool used = false;
    for (size_t i = 0; i < session->claim_count; i++)
      if (session->claim_tokens[i] == session->next_token)
        used = true;
    if (!used) {
      token = session->next_token;
      break;
    }
  }
  if (!token) {
    spinlock_release(&session->lock);
    return -1;
  }
  session->claim_tokens[session->claim_count] = token;
  session->claim_bus_master[session->claim_count] = false;
  session->claims[session->claim_count] = dev;
  session->claim_count++;
  dev->driver_data = session;
  spinlock_release(&session->lock);
  return 0;
}

static void udriver_remove(struct device *dev) {
  struct udriver_session *session = dev ? dev->driver_data : NULL;
  if (!session)
    return;
  release_device_net(session, dev);
  disable_device_bus_master(session, dev, true);
  release_device_irqs(session, dev);
  revoke_device_mappings(session, dev);
  free_device_dma(session, dev);
  spinlock_acquire(&session->lock);
  for (size_t i = 0; i < session->claim_count; i++) {
    if (session->claims[i] != dev)
      continue;
    session->claims[i] = session->claims[session->claim_count - 1];
    session->claim_tokens[i] = session->claim_tokens[session->claim_count - 1];
    session->claim_bus_master[i] =
        session->claim_bus_master[session->claim_count - 1];
    session->claims[session->claim_count - 1] = NULL;
    session->claim_tokens[session->claim_count - 1] = 0;
    session->claim_bus_master[session->claim_count - 1] = false;
    session->claim_count--;
    break;
  }
  spinlock_release(&session->lock);
}

struct udriver_session *udriver_session_create(uint32_t owner_tgid) {
  struct udriver_session *session = kmalloc(sizeof(*session));
  if (!session)
    return NULL;
  memset(session, 0, sizeof(*session));
  session->owner_tgid = owner_tgid;
  spinlock_init(&session->lock);
  wait_queue_init(&session->irq_waitq);
  return session;
}

static int copy_matches(struct udriver_session *session,
                        const struct udrv_register *request) {
  for (uint32_t i = 0; i < request->id_count; i++) {
    const struct udrv_match *source = &request->ids[i];
    struct device_id *target = &session->ids[i];
    memset(target, 0, sizeof(*target));

    if (source->flags == UDRV_MATCH_NAME) {
      if (!bounded_name(source->name, sizeof(source->name)))
        return -22;
      strncpy(session->id_names[i], source->name,
              sizeof(session->id_names[i]) - 1);
      target->type = ID_NAME;
      target->name = session->id_names[i];
    } else if (source->flags == UDRV_MATCH_PCI_ID) {
      target->type = ID_PCI;
      target->pci.vendor = source->vendor;
      target->pci.device = source->device;
      target->pci.match_class = false;
    } else if (source->flags == UDRV_MATCH_PCI_CLASS) {
      target->type = ID_PCI;
      target->pci.class = source->class_code;
      target->pci.subclass = source->subclass;
      target->pci.prog_if = source->prog_if;
      target->pci.match_class = true;
    } else {
      return -22;
    }
  }
  return 0;
}

int udriver_session_register(struct udriver_session *session,
                             const struct udrv_register *request) {
  if (!session || !request || session->registered)
    return -22;
  if (request->abi_version != UDRV_ABI_VERSION || !request->id_count ||
      request->id_count > UDRV_MAX_IDS ||
      !bounded_name(request->driver_name, sizeof(request->driver_name)) ||
      !bounded_name(request->bus_name, sizeof(request->bus_name)))
    return -22;

  struct bus_type *bus = dm_find_bus(request->bus_name);
  if (!bus)
    return -19;
  if (dm_find_driver(bus, request->driver_name))
    return -17;

  strncpy(session->driver_name, request->driver_name,
          sizeof(session->driver_name) - 1);
  strncpy(session->bus_name, request->bus_name,
          sizeof(session->bus_name) - 1);
  int result = copy_matches(session, request);
  if (result != 0)
    return result;

  session->driver.name = session->driver_name;
  session->driver.ids = session->ids;
  session->driver.id_count = request->id_count;
  session->driver.probe = udriver_probe;
  session->driver.remove = udriver_remove;
  session->driver.bus = bus;
  session->driver.kind = DRIVER_USER;
  session->driver.private_data = session;
  session->registered = true;

  dm_register_driver(&session->driver);
  if (dm_find_driver(bus, session->driver_name) != &session->driver) {
    session->registered = false;
    return -17;
  }
  sysfs_pci_driver_registered(&session->driver);
  return 0;
}

int udriver_session_claim(struct udriver_session *session,
                          const char *device_name) {
  if (!session || !session->registered ||
      !bounded_name(device_name, UDRV_DEVICE_LEN))
    return -22;
  struct device *dev = find_bus_device(session->driver.bus, device_name);
  if (!dev)
    return -2;
  if (dev->driver || dev->state != DEVICE_UNBOUND)
    return -16;
  if (dm_bind_device(dev, &session->driver) != 0)
    return -19;
  sysfs_pci_device_bound(dev);
  return 0;
}

int udriver_session_release(struct udriver_session *session,
                            const char *device_name) {
  if (!session || !session->registered ||
      !bounded_name(device_name, UDRV_DEVICE_LEN))
    return -22;
  struct device *dev = find_bus_device(session->driver.bus, device_name);
  if (!dev)
    return -2;
  if (dev->driver != &session->driver)
    return -1;
  sysfs_pci_device_unbinding(dev);
  return dm_unbind_device(dev) == 0 ? 0 : -16;
}

int udriver_session_status(struct udriver_session *session,
                           struct udrv_status *status) {
  if (!session || !status)
    return -22;
  memset(status, 0, sizeof(*status));
  status->abi_version = UDRV_ABI_VERSION;
  status->owner_tgid = session->owner_tgid;
  status->registered = session->registered ? 1U : 0U;
  spinlock_acquire(&session->lock);
  status->claim_count = (uint32_t)session->claim_count;
  spinlock_release(&session->lock);
  strncpy(status->driver_name, session->driver_name,
          sizeof(status->driver_name) - 1);
  strncpy(status->bus_name, session->bus_name, sizeof(status->bus_name) - 1);
  return 0;
}

void udriver_session_destroy(struct udriver_session *session) {
  if (!session)
    return;
  while (session->claim_count) {
    spinlock_acquire(&session->lock);
    struct device *dev = session->claims[session->claim_count - 1];
    spinlock_release(&session->lock);
    if (!dev)
      break;
    sysfs_pci_device_unbinding(dev);
    if (dm_unbind_device(dev) != 0)
      break;
  }
  for (size_t i = 0; i < UDRIVER_MAX_CLAIMS; i++)
    if (session->irqs[i].active)
      release_irq_binding(&session->irqs[i]);
  if (session->registered) {
    dm_unregister_driver(&session->driver);
    session->registered = false;
  }
  kfree(session);
}

static struct device *claimed_device(struct udriver_session *session,
                                     const char *name, uint16_t *token) {
  if (!session || !name)
    return NULL;
  for (size_t i = 0; i < session->claim_count; i++)
    if (session->claims[i] && strcmp(session->claims[i]->name, name) == 0) {
      if (token)
        *token = session->claim_tokens[i];
      return session->claims[i];
    }
  return NULL;
}

static uint64_t resource_length(const struct resource *resource) {
  return resource && resource->end >= resource->start
             ? resource->end - resource->start + 1
             : 0;
}

static int get_device_info(struct udriver_session *session,
                           struct udrv_device_info *info) {
  struct device *dev = claimed_device(session, info->device_name, NULL);
  if (!dev)
    return -1;
  info->resource_count = (uint32_t)dev->resource_count;
  info->vendor = dev->vendor_id;
  info->device = dev->device_id;
  info->class_code = dev->pci_class;
  info->subclass = dev->pci_subclass;
  info->prog_if = dev->pci_prog_if;
  return 0;
}

static int get_resource_info(struct udriver_session *session,
                             struct udrv_resource_info *info) {
  uint16_t token = 0;
  struct device *dev = claimed_device(session, info->device_name, &token);
  if (!dev || info->index >= dev->resource_count)
    return -1;
  struct resource *resource = &dev->resources[info->index];
  info->type = resource->type == RES_MEM ? UDRV_RESOURCE_MEM :
               resource->type == RES_IO ? UDRV_RESOURCE_IO :
               resource->type == RES_IRQ ? UDRV_RESOURCE_IRQ :
               UDRV_RESOURCE_NONE;
  info->flags = resource->type == RES_MEM ? UDRV_RESOURCE_F_MMAP :
                resource->type == RES_IO ? UDRV_RESOURCE_F_PORT_IO :
                resource->type == RES_IRQ ? UDRV_RESOURCE_F_IRQ : 0;
  info->start = resource->start;
  info->length = resource_length(resource);
  info->mmap_offset =
      resource->type == RES_MEM
          ? ((uint64_t)token << UDRV_MMAP_TOKEN_SHIFT) |
                ((uint64_t)info->index << UDRV_MMAP_RESOURCE_SHIFT)
          : 0;
  return 0;
}

static struct pci_device *claimed_pci(struct udriver_session *session,
                                      const char *name) {
  struct device *dev = claimed_device(session, name, NULL);
  if (!dev || dev->bus != pci_bus_type())
    return NULL;
  for (uint32_t i = 0; i < pci_get_device_count(); i++) {
    struct pci_device *pci = pci_get_device(i);
    if (pci && pci->kernel_device == dev)
      return pci;
  }
  return NULL;
}

static struct udriver_irq_binding *
find_irq_binding_locked(struct udriver_session *session, struct device *device) {
  for (size_t i = 0; i < UDRIVER_MAX_CLAIMS; i++)
    if (session->irqs[i].active && session->irqs[i].device == device)
      return &session->irqs[i];
  return NULL;
}

static bool irq_mask_locked(struct udriver_irq_binding *binding, bool masked) {
  if (!binding || !binding->active)
    return false;
  bool ok = true;
  if (binding->hardware_backed) {
    if (binding->legacy) {
      uint16_t command =
          pci_config_read16(binding->pci->bus, binding->pci->slot,
                            binding->pci->func, 0x04);
      if (masked)
        command |= PCI_COMMAND_INTX_DISABLE;
      else
        command &= ~PCI_COMMAND_INTX_DISABLE;
      pci_config_write16(binding->pci->bus, binding->pci->slot,
                         binding->pci->func, 0x04, command);
    } else {
      ok = pci_irq_mask(&binding->irq, 0, masked);
    }
  }
  if (ok)
    binding->masked = masked;
  return ok;
}

static void udriver_irq_handler(struct registers *regs) {
  if (!regs || regs->int_no >= 256)
    return;
  spinlock_acquire(&irq_dispatch_lock);
  for (size_t i = 0; i < UDRIVER_MAX_IRQ_DISPATCH; i++) {
    struct udriver_irq_binding *binding = irq_dispatch[i];
    if (!binding || !binding->active || binding->vector != regs->int_no)
      continue;
    if (!binding->masked)
      irq_mask_locked(binding, true);
    if (binding->pending == ~0ULL || binding->total == ~0ULL) {
      binding->overflow = true;
    } else {
      binding->pending++;
      binding->total++;
    }
    wait_queue_wake_all(&binding->session->irq_waitq);
    epoll_notify_event(binding->session->control_node, POLLIN);
  }
  spinlock_release(&irq_dispatch_lock);
}

static bool register_irq_dispatch_locked(struct udriver_irq_binding *binding) {
  for (size_t i = 0; i < UDRIVER_MAX_IRQ_DISPATCH; i++) {
    if (irq_dispatch[i])
      continue;
    irq_dispatch[i] = binding;
    binding->active = true;
    return true;
  }
  return false;
}

static void unregister_irq_dispatch_locked(
    struct udriver_irq_binding *binding) {
  for (size_t i = 0; i < UDRIVER_MAX_IRQ_DISPATCH; i++)
    if (irq_dispatch[i] == binding)
      irq_dispatch[i] = NULL;
  binding->active = false;
}

static void release_irq_binding(struct udriver_irq_binding *binding) {
  if (!binding)
    return;
  struct udriver_session *session = binding->session;
  bool release_pci_irq = false;
  spinlock_acquire(&irq_dispatch_lock);
  if (binding->active) {
    irq_mask_locked(binding, true);
    unregister_irq_dispatch_locked(binding);
    release_pci_irq = binding->hardware_backed && !binding->legacy;
  }
  spinlock_release(&irq_dispatch_lock);
  if (release_pci_irq)
    pci_irq_release(&binding->irq);
  memset(binding, 0, sizeof(*binding));
  if (session)
    wait_queue_wake_all(&session->irq_waitq);
}

static void release_device_irqs(struct udriver_session *session,
                                struct device *device) {
  if (!session || !device)
    return;
  for (size_t i = 0; i < UDRIVER_MAX_CLAIMS; i++)
    if (session->irqs[i].active && session->irqs[i].device == device)
      release_irq_binding(&session->irqs[i]);
}

static int irq_setup(struct udriver_session *session,
                     struct udrv_irq_setup *request) {
  if (!session || !request ||
      !bounded_name(request->device_name, sizeof(request->device_name)) ||
      request->vector_count != 1 ||
      request->flags != UDRV_IRQ_F_MASK_ON_EVENT)
    return -22;
  struct device *device =
      claimed_device(session, request->device_name, NULL);
  if (!device || request->resource_index >= device->resource_count)
    return -1;
  struct resource *resource = &device->resources[request->resource_index];
  struct pci_device *pci = claimed_pci(session, request->device_name);
  if (!pci || resource->type != RES_IRQ || resource_length(resource) != 1)
    return -19;

  spinlock_acquire(&irq_dispatch_lock);
  if (find_irq_binding_locked(session, device)) {
    spinlock_release(&irq_dispatch_lock);
    return -16;
  }
  struct udriver_irq_binding *binding = NULL;
  for (size_t i = 0; i < UDRIVER_MAX_CLAIMS; i++)
    if (!session->irqs[i].active && !session->irqs[i].device) {
      binding = &session->irqs[i];
      break;
    }
  if (!binding) {
    spinlock_release(&irq_dispatch_lock);
    return -28;
  }

  memset(binding, 0, sizeof(*binding));
  binding->session = session;
  binding->device = device;
  binding->pci = pci;
  binding->hardware_backed = true;
  isr_t handler = udriver_irq_handler;
  if (pci_irq_request(pci, &binding->irq, &handler, 1,
                      (uint8_t)lapic_get_id())) {
    binding->mode = binding->irq.mode == PCI_IRQ_MSIX
                        ? UDRV_IRQ_MODE_MSIX
                        : UDRV_IRQ_MODE_MSI;
    binding->vector = pci_irq_vector(&binding->irq, 0);
    binding->active = true;
    irq_mask_locked(binding, true);
  } else {
    if (pci->irq_line >= 224 || resource->start != pci->irq_line) {
      memset(binding, 0, sizeof(*binding));
      spinlock_release(&irq_dispatch_lock);
      return -19;
    }
    binding->legacy = true;
    binding->mode = UDRV_IRQ_MODE_INTX;
    binding->vector = (uint8_t)(32 + pci->irq_line);
    binding->active = true;
    irq_mask_locked(binding, true);
    if (!legacy_irq_installed[pci->irq_line]) {
      if (!irq_install_handler(pci->irq_line, udriver_irq_handler, 0x000f)) {
        memset(binding, 0, sizeof(*binding));
        spinlock_release(&irq_dispatch_lock);
        return -16;
      }
      legacy_irq_installed[pci->irq_line] = true;
    }
  }

  binding->active = false;
  if (!register_irq_dispatch_locked(binding)) {
    bool release_pci_irq = !binding->legacy;
    binding->active = true;
    irq_mask_locked(binding, true);
    binding->active = false;
    spinlock_release(&irq_dispatch_lock);
    if (release_pci_irq)
      pci_irq_release(&binding->irq);
    memset(binding, 0, sizeof(*binding));
    return -28;
  }
  if (!irq_mask_locked(binding, false)) {
    unregister_irq_dispatch_locked(binding);
    bool release_pci_irq = !binding->legacy;
    spinlock_release(&irq_dispatch_lock);
    if (release_pci_irq)
      pci_irq_release(&binding->irq);
    memset(binding, 0, sizeof(*binding));
    return -5;
  }

  request->mode = binding->mode;
  request->vector = binding->vector;
  request->reserved = 0;
  spinlock_release(&irq_dispatch_lock);
  return 0;
}

static int irq_control(struct udriver_session *session,
                       struct udrv_irq_control *request) {
  if (!session || !request ||
      !bounded_name(request->device_name, sizeof(request->device_name)))
    return -22;
  struct device *device =
      claimed_device(session, request->device_name, NULL);
  if (!device)
    return -1;
  if (request->action == UDRV_IRQ_ACTION_RELEASE) {
    spinlock_acquire(&irq_dispatch_lock);
    struct udriver_irq_binding *binding =
        find_irq_binding_locked(session, device);
    spinlock_release(&irq_dispatch_lock);
    if (!binding)
      return -19;
    release_irq_binding(binding);
    memset(request, 0, sizeof(*request));
    strncpy(request->device_name, device->name,
            sizeof(request->device_name) - 1);
    return 0;
  }

  spinlock_acquire(&irq_dispatch_lock);
  struct udriver_irq_binding *binding =
      find_irq_binding_locked(session, device);
  if (!binding) {
    spinlock_release(&irq_dispatch_lock);
    return -19;
  }
  int result = 0;
  if (request->action == UDRV_IRQ_ACTION_MASK) {
    if (!irq_mask_locked(binding, true))
      result = -5;
  } else if (request->action == UDRV_IRQ_ACTION_ACK ||
             request->action == UDRV_IRQ_ACTION_UNMASK) {
    if (binding->pending)
      result = -16;
    else if (!irq_mask_locked(binding, false))
      result = -5;
  } else if (request->action != UDRV_IRQ_ACTION_STATUS) {
    result = -22;
  }
  request->mode = binding->mode;
  request->pending = binding->pending;
  request->total = binding->total;
  request->masked = binding->masked ? 1U : 0U;
  request->reserved = 0;
  spinlock_release(&irq_dispatch_lock);
  return result;
}

static bool udriver_net_link_up(struct net_device *netdev) {
  struct udriver_net_endpoint *endpoint =
      netdev ? netdev->driver_private : NULL;
  return endpoint && endpoint->active && endpoint->link_up;
}

static void udriver_net_stop(struct net_device *netdev) {
  struct udriver_net_endpoint *endpoint =
      netdev ? netdev->driver_private : NULL;
  if (endpoint)
    endpoint->link_up = false;
}

static int udriver_net_transmit(struct net_device *netdev, const void *frame,
                                size_t length) {
  struct udriver_net_endpoint *endpoint =
      netdev ? netdev->driver_private : NULL;
  if (!endpoint || !endpoint->active || !endpoint->session || !frame ||
      length < 14 || length > UDRV_NET_FRAME_MAX)
    return -1;
  struct udriver_session *session = endpoint->session;
  spinlock_acquire(&session->lock);
  uint32_t next = (endpoint->tx_head + 1U) % UDRIVER_NET_TX_DEPTH;
  if (next == endpoint->tx_tail) {
    netdev->stats.tx_dropped++;
    netdev->stats.queue_full++;
    spinlock_release(&session->lock);
    return -1;
  }
  struct udrv_net_frame *queued = &endpoint->tx[endpoint->tx_head];
  memset(queued, 0, sizeof(*queued));
  strncpy(queued->device_name, endpoint->device->name,
          sizeof(queued->device_name) - 1);
  queued->length = (uint16_t)length;
  memcpy(queued->data, frame, length);
  endpoint->tx_head = next;
  netdev->stats.tx_packets++;
  netdev->stats.tx_bytes += length;
  spinlock_release(&session->lock);
  wait_queue_wake_all(&session->irq_waitq);
  if (session->control_node)
    epoll_notify_event(session->control_node, POLLIN);
  return 0;
}

static const struct net_device_ops udriver_net_ops = {
    .transmit = udriver_net_transmit,
    .link_up = udriver_net_link_up,
    .stop = udriver_net_stop,
};

static bool valid_net_mac(const uint8_t mac[6]) {
  bool zero = true;
  bool broadcast = true;
  for (int i = 0; i < 6; i++) {
    zero = zero && mac[i] == 0;
    broadcast = broadcast && mac[i] == 0xff;
  }
  return !zero && !broadcast && !(mac[0] & 1U);
}

static int net_register_endpoint(struct udriver_session *session,
                                 const struct udrv_net_register *request) {
  if (!session || !request ||
      !bounded_name(request->device_name, sizeof(request->device_name)) ||
      !bounded_name(request->interface_name, sizeof(request->interface_name)) ||
      request->mtu == 0 || request->mtu > NET_MTU_ETHERNET ||
      (request->flags & ~UDRV_NET_F_LINK_UP) ||
      !valid_net_mac(request->mac))
    return -22;
  struct device *device =
      claimed_device(session, request->device_name, NULL);
  if (!device)
    return -1;
  if (session->net.active || net_device_find(request->interface_name))
    return -16;

  struct udriver_net_endpoint *endpoint = &session->net;
  memset(endpoint, 0, sizeof(*endpoint));
  endpoint->session = session;
  endpoint->device = device;
  endpoint->link_up = (request->flags & UDRV_NET_F_LINK_UP) != 0;
  endpoint->active = true;
  strncpy(endpoint->netdev.name, request->interface_name,
          sizeof(endpoint->netdev.name) - 1);
  memcpy(endpoint->netdev.mac, request->mac, sizeof(endpoint->netdev.mac));
  endpoint->netdev.mtu = request->mtu;
  endpoint->netdev.ops = &udriver_net_ops;
  endpoint->netdev.driver_private = endpoint;
  if (net_device_register(&endpoint->netdev) != 0) {
    memset(endpoint, 0, sizeof(*endpoint));
    return -16;
  }
  return 0;
}

static void release_device_net(struct udriver_session *session,
                               struct device *device) {
  if (!session || !device || !session->net.active ||
      session->net.device != device)
    return;
  session->net.link_up = false;
  session->net.active = false;
  if (session->net.netdev.registered)
    net_device_unregister(&session->net.netdev);
  memset(&session->net, 0, sizeof(session->net));
  wait_queue_wake_all(&session->irq_waitq);
}

static int net_receive_frame(struct udriver_session *session,
                             const struct udrv_net_frame *frame) {
  if (!session || !frame || !session->net.active ||
      !bounded_name(frame->device_name, sizeof(frame->device_name)) ||
      session->net.device !=
          claimed_device(session, frame->device_name, NULL) ||
      frame->length < 14 || frame->length > UDRV_NET_FRAME_MAX)
    return -22;
  return net_rx_submit_irq(&session->net.netdev, frame->data, frame->length)
             ? 0
             : -11;
}

static int net_dequeue_frame(struct udriver_session *session,
                             struct udrv_net_frame *frame) {
  if (!session || !frame || !session->net.active ||
      !bounded_name(frame->device_name, sizeof(frame->device_name)) ||
      session->net.device !=
          claimed_device(session, frame->device_name, NULL))
    return -22;
  spinlock_acquire(&session->lock);
  if (session->net.tx_tail == session->net.tx_head) {
    spinlock_release(&session->lock);
    return -11;
  }
  memcpy(frame, &session->net.tx[session->net.tx_tail], sizeof(*frame));
  session->net.tx_tail =
      (session->net.tx_tail + 1U) % UDRIVER_NET_TX_DEPTH;
  spinlock_release(&session->lock);
  return 0;
}

static int net_get_state(struct udriver_session *session,
                         struct udrv_net_state *state) {
  if (!session || !state || !session->net.active ||
      !bounded_name(state->device_name, sizeof(state->device_name)) ||
      session->net.device !=
          claimed_device(session, state->device_name, NULL) ||
      (state->flags & ~UDRV_NET_F_LINK_UP))
    return -22;
  session->net.link_up = (state->flags & UDRV_NET_F_LINK_UP) != 0;
  spinlock_acquire(&session->lock);
  state->tx_queued =
      (session->net.tx_head + UDRIVER_NET_TX_DEPTH - session->net.tx_tail) %
      UDRIVER_NET_TX_DEPTH;
  spinlock_release(&session->lock);
  state->flags = session->net.link_up ? UDRV_NET_F_LINK_UP : 0;
  state->rx_packets = session->net.netdev.stats.rx_packets;
  state->tx_packets = session->net.netdev.stats.tx_packets;
  state->rx_dropped = session->net.netdev.stats.rx_dropped;
  state->tx_dropped = session->net.netdev.stats.tx_dropped;
  return 0;
}

static bool driverctl_is_nonblocking(vfs_node_t *node) {
  struct thread *current = sched_get_current();
  if (!current)
    return false;
  for (int fd = 0; fd < MAX_FDS; fd++)
    if (current->fds[fd] == node)
      return (current->fd_flags[fd] & 0x800U) != 0;
  return false;
}

static uint32_t driverctl_read(vfs_node_t *node, uint32_t offset,
                               uint32_t size, uint8_t *buffer) {
  (void)offset;
  struct udriver_session *session = node ? node->device : NULL;
  if (!session_is_owner(session) || !buffer ||
      size < sizeof(struct udrv_irq_event))
    return (uint32_t)-22;
  struct thread *current = sched_get_current();
  if (!current)
    return (uint32_t)-1;

  for (;;) {
    spinlock_acquire(&irq_dispatch_lock);
    bool active = false;
    for (size_t i = 0; i < UDRIVER_MAX_CLAIMS; i++) {
      struct udriver_irq_binding *binding = &session->irqs[i];
      if (!binding->active)
        continue;
      active = true;
      if (!binding->pending)
        continue;
      struct udrv_irq_event event;
      memset(&event, 0, sizeof(event));
      strncpy(event.device_name, binding->device->name,
              sizeof(event.device_name) - 1);
      event.count = binding->pending;
      event.total = binding->total;
      event.mode = binding->mode;
      event.vector = binding->vector;
      event.flags = UDRV_IRQ_EVENT_F_NEEDS_ACK;
      if (binding->overflow)
        event.flags |= UDRV_IRQ_EVENT_F_OVERFLOW;
      binding->pending = 0;
      binding->overflow = false;
      spinlock_release(&irq_dispatch_lock);
      memcpy(buffer, &event, sizeof(event));
      return sizeof(event);
    }
    if (!active) {
      spinlock_release(&irq_dispatch_lock);
      return (uint32_t)-19;
    }
    if (driverctl_is_nonblocking(node)) {
      spinlock_release(&irq_dispatch_lock);
      return (uint32_t)-11;
    }
    wait_queue_entry_t entry = {.thread = current, .next = NULL};
    wait_queue_add(&session->irq_waitq, &entry);
    current->state = THREAD_BLOCKED;
    spinlock_release(&irq_dispatch_lock);
    sched_yield();
    current->state = THREAD_RUNNING;
    wait_queue_remove(&session->irq_waitq, &entry);
  }
}

static int driverctl_poll(vfs_node_t *node, int events) {
  struct udriver_session *session = node ? node->device : NULL;
  if (!session)
    return POLLNVAL;
  for (size_t i = 0; i < UDRIVER_MAX_CLAIMS; i++)
    if (__atomic_load_n(&session->irqs[i].active, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&session->irqs[i].pending, __ATOMIC_ACQUIRE))
      return events & POLLIN;
  if (__atomic_load_n(&session->net.active, __ATOMIC_ACQUIRE) &&
      __atomic_load_n(&session->net.tx_head, __ATOMIC_ACQUIRE) !=
          __atomic_load_n(&session->net.tx_tail, __ATOMIC_ACQUIRE))
    return events & POLLIN;
  return 0;
}

static bool config_request_valid(const struct udrv_pci_config *request) {
  if (!request || (request->width != 1 && request->width != 2 &&
                   request->width != 4) ||
      request->offset + request->width > 256 ||
      (request->offset & (request->width - 1)))
    return false;
  if (!request->write)
    return true;
  return request->offset == 4 && request->width == 2 &&
         (request->value & ~3U) == 0;
}

static int pci_config_access(struct udriver_session *session,
                             struct udrv_pci_config *request) {
  if (!config_request_valid(request))
    return -22;
  struct pci_device *pci = claimed_pci(session, request->device_name);
  if (!pci)
    return -19;
  uint16_t aligned = request->offset & ~3U;
  uint32_t value =
      pci_config_read32(pci->bus, pci->slot, pci->func, aligned);
  unsigned shift = (request->offset & 3U) * 8U;
  uint32_t mask = request->width == 4 ? 0xffffffffU :
                  request->width == 2 ? 0xffffU : 0xffU;
  if (!request->write) {
    request->value = (value >> shift) & mask;
    return 0;
  }
  value = (value & ~(mask << shift)) | ((request->value & mask) << shift);
  pci_config_write32(pci->bus, pci->slot, pci->func, aligned, value);
  return 0;
}

static int port_io_access(struct udriver_session *session,
                          struct udrv_port_io *request, bool execute) {
  struct device *dev = claimed_device(session, request->device_name, NULL);
  if (!dev || request->resource_index >= dev->resource_count ||
      (request->width != 1 && request->width != 2 && request->width != 4))
    return -22;
  struct resource *resource = &dev->resources[request->resource_index];
  uint64_t length = resource_length(resource);
  if (resource->type != RES_IO || request->offset > length ||
      request->width > length - request->offset ||
      (request->offset & (request->width - 1)) ||
      resource->start > 0x10000ULL - request->width - request->offset)
    return -1;
  if (!execute)
    return 0;
  uint16_t port = (uint16_t)(resource->start + request->offset);
  if (request->write) {
    if (request->width == 1) outb(port, (uint8_t)request->value);
    else if (request->width == 2) outw(port, (uint16_t)request->value);
    else outl(port, request->value);
  } else {
    request->value = request->width == 1 ? inb(port) :
                     request->width == 2 ? inw(port) : inl(port);
  }
  return 0;
}

static bool device_bus_master_enabled(struct udriver_session *session,
                                      struct device *device) {
  int index = claimed_index(session, device);
  return index >= 0 && session->claim_bus_master[index];
}

static struct udriver_dma_buffer *
find_dma_buffer(struct udriver_session *session, struct device *device,
                uint16_t handle) {
  for (size_t i = 0; i < UDRIVER_MAX_DMA_BUFFERS; i++)
    if (session->dma[i].buffer && session->dma[i].device == device &&
        session->dma[i].handle == handle)
      return &session->dma[i];
  return NULL;
}

static void revoke_dma_mappings(struct udriver_session *session,
                                uint16_t handle) {
  for (size_t i = 0; i < UDRIVER_MAX_MAPPINGS; i++) {
    if (session->mappings[i].dma_handle != handle)
      continue;
    for (uint64_t off = 0; off < session->mappings[i].length; off += 4096)
      vmm_unmap_page(session->mappings[i].pml4,
                     session->mappings[i].address + off);
    memset(&session->mappings[i], 0, sizeof(session->mappings[i]));
  }
}

static int fill_dma_info(struct udriver_session *session,
                         struct device *device, struct udrv_dma_info *info) {
  if (!session || !device || !info)
    return -22;
  uint64_t allocated = 0;
  uint32_t count = 0;
  spinlock_acquire(&session->lock);
  for (size_t i = 0; i < UDRIVER_MAX_DMA_BUFFERS; i++)
    if (session->dma[i].buffer && session->dma[i].device == device) {
      allocated += session->dma[i].buffer->size;
      count++;
    }
  info->capabilities =
      UDRV_DMA_CAP_COHERENT | UDRV_DMA_CAP_TRUSTED_ONLY;
  info->max_buffers = UDRIVER_MAX_DMA_BUFFERS;
  info->max_allocation = UDRIVER_DMA_MAX_ALLOCATION;
  info->max_total = UDRIVER_DMA_MAX_TOTAL;
  info->allocated = allocated;
  info->buffer_count = count;
  info->bus_master_enabled =
      device_bus_master_enabled(session, device) ? 1U : 0U;
  spinlock_release(&session->lock);
  return 0;
}

static int dma_info(struct udriver_session *session,
                    struct udrv_dma_info *info) {
  if (!session || !info ||
      !bounded_name(info->device_name, sizeof(info->device_name)))
    return -22;
  struct device *device =
      claimed_device(session, info->device_name, NULL);
  if (!device || !pci_for_device(device))
    return -19;
  return fill_dma_info(session, device, info);
}

static int dma_allocate_for_device(struct udriver_session *session,
                                   struct device *device,
                                   struct udrv_dma_alloc *request) {
  uint32_t known = UDRV_DMA_F_32BIT | UDRV_DMA_F_64BIT |
                   UDRV_DMA_F_REQUIRE_IOMMU | UDRV_DMA_F_TRUSTED;
  uint32_t address_mode =
      request ? request->flags & (UDRV_DMA_F_32BIT | UDRV_DMA_F_64BIT) : 0;
  if (!session || !device || !request || !request->size ||
      request->size > UDRIVER_DMA_MAX_ALLOCATION ||
      (request->flags & ~known) ||
      (address_mode != UDRV_DMA_F_32BIT &&
       address_mode != UDRV_DMA_F_64BIT))
    return -22;
  if (request->flags & UDRV_DMA_F_REQUIRE_IOMMU)
    return -95;
  if (!(request->flags & UDRV_DMA_F_TRUSTED))
    return -1;
  if (request->size > ~0ULL - 4095)
    return -22;
  uint64_t length = (request->size + 4095) & ~4095ULL;

  spinlock_acquire(&session->lock);
  if (length > UDRIVER_DMA_MAX_TOTAL - session->dma_total) {
    spinlock_release(&session->lock);
    return -28;
  }
  struct udriver_dma_buffer *slot = NULL;
  for (size_t i = 0; i < UDRIVER_MAX_DMA_BUFFERS; i++)
    if (!session->dma[i].buffer) {
      slot = &session->dma[i];
      break;
    }
  if (!slot) {
    spinlock_release(&session->lock);
    return -28;
  }

  uint16_t handle = 0;
  for (size_t attempt = 0; attempt < UDRIVER_MAX_DMA_BUFFERS + 1; attempt++) {
    session->next_dma_handle++;
    if (!session->next_dma_handle ||
        session->next_dma_handle > UDRV_MMAP_DMA_HANDLE_MASK)
      session->next_dma_handle = 1;
    bool used = false;
    for (size_t i = 0; i < UDRIVER_MAX_DMA_BUFFERS; i++)
      if (session->dma[i].buffer &&
          session->dma[i].handle == session->next_dma_handle)
        used = true;
    if (!used) {
      handle = session->next_dma_handle;
      break;
    }
  }
  if (!handle) {
    spinlock_release(&session->lock);
    return -28;
  }

  uint32_t dma_flags = address_mode == UDRV_DMA_F_32BIT
                           ? DMA_FLAG_32BIT
                           : DMA_FLAG_ANYWHERE;
  dma_buffer_t *buffer = dma_alloc((size_t)length, dma_flags);
  if (!buffer) {
    spinlock_release(&session->lock);
    return -12;
  }
  slot->device = device;
  slot->buffer = buffer;
  slot->flags = request->flags;
  slot->handle = handle;
  session->dma_total += length;
  request->handle = handle;
  request->dma_address = buffer->phys;
  request->length = length;
  request->mmap_offset =
      UDRV_MMAP_DMA_TAG |
      ((uint64_t)handle << UDRV_MMAP_TOKEN_SHIFT);
  spinlock_release(&session->lock);
  return 0;
}

static int dma_allocate(struct udriver_session *session,
                        struct udrv_dma_alloc *request) {
  if (!session || !request ||
      !bounded_name(request->device_name, sizeof(request->device_name)))
    return -22;
  struct device *device =
      claimed_device(session, request->device_name, NULL);
  if (!device || !pci_for_device(device))
    return -19;
  return dma_allocate_for_device(session, device, request);
}

static int dma_free_buffer(struct udriver_session *session,
                           struct udrv_dma_free *request) {
  if (!session || !request || !request->handle ||
      !bounded_name(request->device_name, sizeof(request->device_name)))
    return -22;
  struct device *device =
      claimed_device(session, request->device_name, NULL);
  if (!device)
    return -1;
  spinlock_acquire(&session->lock);
  if (device_bus_master_enabled(session, device)) {
    spinlock_release(&session->lock);
    return -16;
  }
  struct udriver_dma_buffer *slot =
      find_dma_buffer(session, device, (uint16_t)request->handle);
  if (!slot) {
    spinlock_release(&session->lock);
    return -2;
  }
  dma_buffer_t *buffer = slot->buffer;
  uint16_t handle = slot->handle;
  session->dma_total -= buffer->size;
  memset(slot, 0, sizeof(*slot));
  spinlock_release(&session->lock);

  revoke_dma_mappings(session, handle);
  memset(buffer->virt, 0, buffer->pages * PAGE_SIZE);
  dma_free(buffer);
  return 0;
}

static int bus_master_control_for_device(
    struct udriver_session *session, struct device *device,
    struct pci_device *pci, struct udrv_bus_master *request, bool execute) {
  if (!session || !device || !request || request->enable > 1)
    return -22;
  spinlock_acquire(&session->lock);
  int index = claimed_index(session, device);
  if (index < 0) {
    spinlock_release(&session->lock);
    return -1;
  }
  if (request->enable) {
    bool has_dma = false;
    for (size_t i = 0; i < UDRIVER_MAX_DMA_BUFFERS; i++)
      if (session->dma[i].buffer && session->dma[i].device == device)
        has_dma = true;
    if (!has_dma) {
      spinlock_release(&session->lock);
      return -1;
    }
  }
  if (execute) {
    if (!pci) {
      spinlock_release(&session->lock);
      return -19;
    }
    pci_set_bus_mastering(pci, request->enable != 0);
  }
  session->claim_bus_master[index] = request->enable != 0;
  request->enabled = session->claim_bus_master[index] ? 1U : 0U;
  spinlock_release(&session->lock);
  return 0;
}

static int bus_master_control(struct udriver_session *session,
                              struct udrv_bus_master *request) {
  if (!session || !request ||
      !bounded_name(request->device_name, sizeof(request->device_name)))
    return -22;
  struct device *device =
      claimed_device(session, request->device_name, NULL);
  struct pci_device *pci = pci_for_device(device);
  if (!device || !pci)
    return -19;
  return bus_master_control_for_device(session, device, pci, request, true);
}

static void disable_device_bus_master(struct udriver_session *session,
                                      struct device *device, bool execute) {
  if (!session || !device)
    return;
  struct udrv_bus_master request;
  memset(&request, 0, sizeof(request));
  request.enable = 0;
  (void)bus_master_control_for_device(
      session, device, pci_for_device(device), &request, execute);
}

static void free_device_dma(struct udriver_session *session,
                            struct device *device) {
  if (!session || !device)
    return;
  for (size_t i = 0; i < UDRIVER_MAX_DMA_BUFFERS; i++) {
    struct udriver_dma_buffer *slot = &session->dma[i];
    if (!slot->buffer || slot->device != device)
      continue;
    dma_buffer_t *buffer = slot->buffer;
    uint16_t handle = slot->handle;
    if (session->dma_total >= buffer->size)
      session->dma_total -= buffer->size;
    memset(slot, 0, sizeof(*slot));
    revoke_dma_mappings(session, handle);
    memset(buffer->virt, 0, buffer->pages * PAGE_SIZE);
    dma_free(buffer);
  }
}

static void revoke_device_mappings(struct udriver_session *session,
                                   struct device *device) {
  for (size_t i = 0; i < UDRIVER_MAX_MAPPINGS; i++) {
    if (session->mappings[i].device != device)
      continue;
    for (uint64_t off = 0; off < session->mappings[i].length; off += 4096)
      vmm_unmap_page(session->mappings[i].pml4, session->mappings[i].address + off);
    memset(&session->mappings[i], 0, sizeof(session->mappings[i]));
  }
}

static uint64_t driverctl_mmap(vfs_node_t *node, uint64_t address,
                               uint64_t length, uint64_t prot, uint64_t flags,
                               uint64_t offset) {
  struct udriver_session *session = node ? node->device : NULL;
  if (!session || !session_is_owner(session) || !length ||
      !(flags & 1U) || (prot & 4U) || (address & 0xfffU))
    return (uint64_t)-1;
  uint16_t token = (uint16_t)(offset >> UDRV_MMAP_TOKEN_SHIFT);
  if ((token & 0xc000U) == 0x4000U) {
    uint16_t handle = token & UDRV_MMAP_DMA_HANDLE_MASK;
    uint64_t dma_inner = offset & UDRV_MMAP_DMA_INNER_MASK;
    struct udriver_dma_buffer *allocation = NULL;
    for (size_t i = 0; i < UDRIVER_MAX_DMA_BUFFERS; i++)
      if (session->dma[i].buffer && session->dma[i].handle == handle)
        allocation = &session->dma[i];
    if (!allocation || (dma_inner & 0xfffU) ||
        length + 4095 < length)
      return (uint64_t)-1;
    uint64_t aligned_length = (length + 4095) & ~4095ULL;
    uint64_t available = allocation->buffer->size;
    if (dma_inner > available || aligned_length > available - dma_inner)
      return (uint64_t)-1;
    uint64_t page_flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER |
                          PAGE_FLAG_PWT | PAGE_FLAG_PCD | PAGE_FLAG_NX;
    if (prot & 2U)
      page_flags |= PAGE_FLAG_RW;
    uint64_t mapped = 0;
    for (; mapped < aligned_length; mapped += 4096)
      if (!vmm_map_page(vmm_get_active_pml4(), address + mapped,
                        allocation->buffer->phys + dma_inner + mapped,
                        page_flags)) {
        while (mapped) {
          mapped -= 4096;
          vmm_unmap_page(vmm_get_active_pml4(), address + mapped);
        }
        return (uint64_t)-1;
      }
    for (size_t i = 0; i < UDRIVER_MAX_MAPPINGS; i++)
      if (!session->mappings[i].device) {
        session->mappings[i].device = allocation->device;
        session->mappings[i].address = address;
        session->mappings[i].length = aligned_length;
        session->mappings[i].pml4 = vmm_get_active_pml4();
        session->mappings[i].dma_handle = handle;
        return address;
      }
    for (uint64_t off = 0; off < aligned_length; off += 4096)
      vmm_unmap_page(vmm_get_active_pml4(), address + off);
    return (uint64_t)-1;
  }
  uint8_t index = (uint8_t)(offset >> UDRV_MMAP_RESOURCE_SHIFT);
  uint64_t inner = offset & UDRV_MMAP_INNER_MASK;
  struct device *dev = NULL;
  for (size_t i = 0; i < session->claim_count; i++)
    if (session->claim_tokens[i] == token)
      dev = session->claims[i];
  if (!dev || index >= dev->resource_count || (inner & 0xfffU))
    return (uint64_t)-1;
  struct resource *resource = &dev->resources[index];
  if (length + 4095 < length)
    return (uint64_t)-1;
  uint64_t aligned_length = (length + 4095) & ~4095ULL;
  uint64_t resource_size = resource_length(resource);
  if (resource->type != RES_MEM || (resource->start & 0xfffU) ||
      inner > resource_size || aligned_length > resource_size - inner)
    return (uint64_t)-1;
  uint64_t page_flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER |
                        PAGE_FLAG_PWT | PAGE_FLAG_PCD | PAGE_FLAG_NX;
  if (prot & 2U)
    page_flags |= PAGE_FLAG_RW;
  uint64_t mapped = 0;
  for (; mapped < aligned_length; mapped += 4096)
    if (!vmm_map_page(vmm_get_active_pml4(), address + mapped,
                      resource->start + inner + mapped, page_flags)) {
      while (mapped) {
        mapped -= 4096;
        vmm_unmap_page(vmm_get_active_pml4(), address + mapped);
      }
      return (uint64_t)-1;
    }
  for (size_t i = 0; i < UDRIVER_MAX_MAPPINGS; i++)
    if (!session->mappings[i].device) {
      session->mappings[i].device = dev;
      session->mappings[i].address = address;
      session->mappings[i].length = aligned_length;
      session->mappings[i].pml4 = vmm_get_active_pml4();
      session->mappings[i].dma_handle = 0;
      return address;
    }
  for (uint64_t off = 0; off < aligned_length; off += 4096)
    vmm_unmap_page(vmm_get_active_pml4(), address + off);
  return (uint64_t)-1;
}

static bool session_is_owner(struct udriver_session *session) {
  struct thread *current = sched_get_current();
  return session && current && current->tgid == session->owner_tgid;
}

static int driverctl_ioctl(vfs_node_t *node, uint32_t request, uint64_t arg) {
  struct udriver_session *session = node ? node->device : NULL;
  if (!session_is_owner(session))
    return -1;
  if (!arg)
    return -14;

  switch (request) {
  case UDRV_IOCTL_REGISTER: {
    struct udrv_register copy;
    memcpy(&copy, (const void *)(uintptr_t)arg, sizeof(copy));
    return udriver_session_register(session, &copy);
  }
  case UDRV_IOCTL_CLAIM: {
    struct udrv_claim copy;
    memcpy(&copy, (const void *)(uintptr_t)arg, sizeof(copy));
    return udriver_session_claim(session, copy.device_name);
  }
  case UDRV_IOCTL_RELEASE: {
    struct udrv_claim copy;
    memcpy(&copy, (const void *)(uintptr_t)arg, sizeof(copy));
    return udriver_session_release(session, copy.device_name);
  }
  case UDRV_IOCTL_GET_STATUS:
    return udriver_session_status(session, (struct udrv_status *)(uintptr_t)arg);
  case UDRV_IOCTL_GET_DEVICE: {
    struct udrv_device_info copy; memcpy(&copy, (void *)(uintptr_t)arg, sizeof(copy));
    int result = get_device_info(session, &copy);
    if (!result) memcpy((void *)(uintptr_t)arg, &copy, sizeof(copy));
    return result;
  }
  case UDRV_IOCTL_GET_RESOURCE: {
    struct udrv_resource_info copy; memcpy(&copy, (void *)(uintptr_t)arg, sizeof(copy));
    int result = get_resource_info(session, &copy);
    if (!result) memcpy((void *)(uintptr_t)arg, &copy, sizeof(copy));
    return result;
  }
  case UDRV_IOCTL_PCI_CONFIG: {
    struct udrv_pci_config copy; memcpy(&copy, (void *)(uintptr_t)arg, sizeof(copy));
    int result = pci_config_access(session, &copy);
    if (!result) memcpy((void *)(uintptr_t)arg, &copy, sizeof(copy));
    return result;
  }
  case UDRV_IOCTL_PORT_IO: {
    struct udrv_port_io copy; memcpy(&copy, (void *)(uintptr_t)arg, sizeof(copy));
    int result = port_io_access(session, &copy, true);
    if (!result) memcpy((void *)(uintptr_t)arg, &copy, sizeof(copy));
    return result;
  }
  case UDRV_IOCTL_IRQ_SETUP: {
    struct udrv_irq_setup copy; memcpy(&copy, (void *)(uintptr_t)arg, sizeof(copy));
    int result = irq_setup(session, &copy);
    if (!result) memcpy((void *)(uintptr_t)arg, &copy, sizeof(copy));
    return result;
  }
  case UDRV_IOCTL_IRQ_CONTROL: {
    struct udrv_irq_control copy; memcpy(&copy, (void *)(uintptr_t)arg, sizeof(copy));
    int result = irq_control(session, &copy);
    if (!result) memcpy((void *)(uintptr_t)arg, &copy, sizeof(copy));
    return result;
  }
  case UDRV_IOCTL_DMA_INFO: {
    struct udrv_dma_info copy; memcpy(&copy, (void *)(uintptr_t)arg, sizeof(copy));
    int result = dma_info(session, &copy);
    if (!result) memcpy((void *)(uintptr_t)arg, &copy, sizeof(copy));
    return result;
  }
  case UDRV_IOCTL_DMA_ALLOC: {
    struct udrv_dma_alloc copy; memcpy(&copy, (void *)(uintptr_t)arg, sizeof(copy));
    int result = dma_allocate(session, &copy);
    if (!result) memcpy((void *)(uintptr_t)arg, &copy, sizeof(copy));
    return result;
  }
  case UDRV_IOCTL_DMA_FREE: {
    struct udrv_dma_free copy; memcpy(&copy, (void *)(uintptr_t)arg, sizeof(copy));
    return dma_free_buffer(session, &copy);
  }
  case UDRV_IOCTL_BUS_MASTER: {
    struct udrv_bus_master copy; memcpy(&copy, (void *)(uintptr_t)arg, sizeof(copy));
    int result = bus_master_control(session, &copy);
    if (!result) memcpy((void *)(uintptr_t)arg, &copy, sizeof(copy));
    return result;
  }
  case UDRV_IOCTL_NET_REGISTER: {
    struct udrv_net_register copy;
    memcpy(&copy, (void *)(uintptr_t)arg, sizeof(copy));
    return net_register_endpoint(session, &copy);
  }
  case UDRV_IOCTL_NET_RX: {
    struct udrv_net_frame copy;
    memcpy(&copy, (void *)(uintptr_t)arg, sizeof(copy));
    return net_receive_frame(session, &copy);
  }
  case UDRV_IOCTL_NET_TX: {
    struct udrv_net_frame copy;
    memcpy(&copy, (void *)(uintptr_t)arg, sizeof(copy));
    int result = net_dequeue_frame(session, &copy);
    if (!result) memcpy((void *)(uintptr_t)arg, &copy, sizeof(copy));
    return result;
  }
  case UDRV_IOCTL_NET_STATE: {
    struct udrv_net_state copy;
    memcpy(&copy, (void *)(uintptr_t)arg, sizeof(copy));
    int result = net_get_state(session, &copy);
    if (!result) memcpy((void *)(uintptr_t)arg, &copy, sizeof(copy));
    return result;
  }
  default:
    return -25;
  }
}

static void driverctl_close(vfs_node_t *node) {
  if (!node)
    return;
  udriver_session_destroy(node->device);
  node->device = NULL;
}

static vfs_node_t *driverctl_open_instance(vfs_node_t *metadata) {
  (void)metadata;
  struct thread *current = sched_get_current();
  if (!current)
    return NULL;
  struct udriver_session *session =
      udriver_session_create(current->tgid);
  if (!session)
    return NULL;
  vfs_node_t *node = kmalloc(sizeof(*node));
  if (!node) {
    udriver_session_destroy(session);
    return NULL;
  }
  vfs_node_init(node);
  strcpy(node->name, "driverctl");
  node->flags = FS_CHARDEV;
  node->mask = 0600;
  node->inode = UDRIVER_RDEV;
  node->device = session;
  node->ioctl = driverctl_ioctl;
  node->read = driverctl_read;
  node->poll = driverctl_poll;
  node->mmap = driverctl_mmap;
  node->close = driverctl_close;
  node->wait_queue = &session->irq_waitq;
  session->control_node = node;
  node->refcount = 0;
  return node;
}

void udriver_init(void) {
  vfs_node_init(&driverctl_metadata);
  strcpy(driverctl_metadata.name, "driverctl");
  driverctl_metadata.flags = FS_CHARDEV | FS_PERSISTENT;
  driverctl_metadata.mask = 0600;
  driverctl_metadata.inode = UDRIVER_RDEV;
  driverctl_metadata.open_instance = driverctl_open_instance;
  devfs_register_node("driverctl", &driverctl_metadata);
  klog_puts("[UDRIVER] /dev/driverctl ABI v1 ready\n");
}
