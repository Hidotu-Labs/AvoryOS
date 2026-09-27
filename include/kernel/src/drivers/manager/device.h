#ifndef DEVICE_H
#define DEVICE_H

#include "lock/spinlock.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    RES_NONE,
    RES_MEM,    // MMIO range
    RES_IO,     // Port I/O range
    RES_IRQ     // Interrupt number
} resource_type_t;

typedef enum {
    ID_ANY,
    ID_PCI,
    ID_ACPI,
    ID_NAME
} device_id_type_t;

typedef enum {
    DEVICE_UNBOUND,
    DEVICE_PROBING,
    DEVICE_BOUND_KERNEL,
    DEVICE_BOUND_USER,
    DEVICE_REMOVING,
    DEVICE_DEAD
} device_state_t;

typedef enum {
    DRIVER_KERNEL,
    DRIVER_USER
} driver_kind_t;

struct device;
struct driver;
struct bus_type;

struct device_id {
    device_id_type_t type;
    union {
        struct {
            uint16_t vendor;
            uint16_t device;
            uint8_t class;
            uint8_t subclass;
            uint8_t prog_if;
            bool match_class;
        } pci;
        const char *acpi_hid;
        const char *name;
    };
};

struct resource {
    resource_type_t type;
    const char *name;
    uint64_t start;
    uint64_t end;
    uint64_t flags;
};

#define MAX_RESOURCES 16
#define DRIVER_OVERRIDE_LEN 64

struct device {
    const char *name;
    struct device *parent;
    
    struct device *first_child;
    struct device *next_sibling;

    struct bus_type *bus;
    struct device *next_bus_device;

    struct resource resources[MAX_RESOURCES];
    size_t resource_count;

    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t pci_class;
    uint8_t pci_subclass;
    uint8_t pci_prog_if;

    struct driver *driver;
    void *driver_data; // Pointer to driver-specific state
    device_state_t state;
    uint32_t refcount;
    spinlock_t lock;
    char driver_override[DRIVER_OVERRIDE_LEN];
};

struct driver {
    const char *name;
    struct device_id *ids;
    size_t id_count;

    int (*probe)(struct device *dev);
    void (*remove)(struct device *dev);

    struct bus_type *bus;
    driver_kind_t kind;
    void *private_data;
    struct driver *next;
};

struct bus_type {
    const char *name;
    bool (*match)(struct device *dev, struct driver *drv);
    struct device *devices;
    struct driver *drivers;
    struct bus_type *next;
    spinlock_t lock;
};

typedef bool (*dm_device_iter_t)(struct device *dev, void *ctx);
typedef bool (*dm_driver_iter_t)(struct driver *drv, void *ctx);

// Initialize the device manager and the root node
void dm_init(void);
struct device *dm_root(void);
int dm_register_bus(struct bus_type *bus);
struct bus_type *dm_find_bus(const char *name);

// Create a new device node as a child of another
struct device *device_create(struct device *parent, const char *name);
struct device *device_create_on_bus(struct bus_type *bus,
                                    struct device *parent,
                                    const char *name);
int device_destroy(struct device *dev);
void device_get(struct device *dev);
void device_put(struct device *dev);

// Add a resource to a device
bool device_add_resource(struct device *dev, resource_type_t type, const char *name, uint64_t start, uint64_t end);

// Find a device by its absolute path (e.g. "/sys/pci/00:02.0")
struct device *device_find_by_path(const char *path);

// Log the device tree to the console (for debugging)
void dm_dump_tree(void);


void dm_register_driver(struct driver *drv);
int dm_unregister_driver(struct driver *drv);
int dm_probe_device(struct device *dev);
int dm_bind_device(struct device *dev, struct driver *drv);
int dm_bind_device_named(struct device *dev, const char *driver_name);
int dm_unbind_device(struct device *dev);
struct driver *dm_find_driver(struct bus_type *bus, const char *name);
bool dm_driver_matches(struct device *dev, struct driver *drv);
void dm_for_each_device(struct bus_type *bus, dm_device_iter_t fn, void *ctx);
void dm_for_each_driver(struct bus_type *bus, dm_driver_iter_t fn, void *ctx);

#endif
