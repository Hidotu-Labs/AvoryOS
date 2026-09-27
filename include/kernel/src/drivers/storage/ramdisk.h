#ifndef DRIVERS_STORAGE_RAMDISK_H
#define DRIVERS_STORAGE_RAMDISK_H

#include <limine.h>

// Called early in kmain_high_half, before the root filesystem mount loop.
// Iterates over Limine modules, finds the one named "disk.img", and registers
// it as a RAM-backed block device ("ram0") so the normal ext4/ext2 mount
// path can find and mount it as root.
void ramdisk_init(struct limine_module_response *modules);

#endif
