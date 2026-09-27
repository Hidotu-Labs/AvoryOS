#ifndef FS_SYSFS_PCI_H
#define FS_SYSFS_PCI_H

#include "fs/vfs.h"
#include <stdbool.h>

struct device;
struct driver;

void sysfs_pci_init(vfs_node_t *pci_bus_dir, vfs_node_t *devices_root);void sysfs_pci_driver_registered(struct driver *driver);
void sysfs_pci_device_bound(struct device *device);
void sysfs_pci_device_unbinding(struct device *device);

#endif
