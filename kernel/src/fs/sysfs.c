#include "fs/sysfs.h"
#include "fs/sysfs_pci.h"
#include "fs/tmpfs.h"
#include "console/klog.h"
#include "drivers/pci/pci.h"
#include "drivers/gpu/virtio_gpu/virtio_gpu.h"
#include "drivers/storage/block.h"
#include "fs/ramfs.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/numa.h"
#include "net/core.h"
#include "net/ipv4.h"
#include "net/ipv6.h"
#include "smp/cpu.h"

#include <linuxkpi/native_sysfs.h>

#define SYSFS_MAGIC 0x62656572

static int sysfs_statfs(vfs_node_t *node, struct statfs_buf *buf) {
  (void)node;
  if (!buf)
    return -1;
  buf->f_type = SYSFS_MAGIC;
  buf->f_bsize = 4096;
  buf->f_blocks = 0;
  buf->f_bfree = 0;
  buf->f_bavail = 0;
  buf->f_files = 0;
  buf->f_ffree = 0;
  buf->f_fsid[0] = 0;
  buf->f_fsid[1] = 0;
  buf->f_namelen = 255;
  buf->f_frsize = 4096;
  buf->f_flags = 0;
  return 0;
}

// GPU device path for netlink uevents
char sysfs_gpu_devpath[128] = "/devices/pci0000:00/0000:00:01.0/drm/card0";
char sysfs_gpu_connector_devpath[128] =
    "/devices/pci0000:00/0000:00:01.0/drm/card0/card0-HDMI-A-1";
char sysfs_gpu2_devpath[128] = "/devices/pci0000:00/0000:00:01.0/drm/card2";
static vfs_node_t *net_class_root;
static vfs_node_t *gpu_status_nodes[VIRTIO_GPU_MAX_SCANOUTS];
static vfs_node_t *gpu_enabled_nodes[VIRTIO_GPU_MAX_SCANOUTS];
static vfs_node_t *gpu_modes_nodes[VIRTIO_GPU_MAX_SCANOUTS];
static vfs_node_t *gpu_dpms_nodes[VIRTIO_GPU_MAX_SCANOUTS];

// Helpers

static const struct pci_device *sysfs_display_pci(bool amd) {
  const struct pci_device *native = virtio_gpu_pci_device();
  const struct pci_device *first = NULL;
  for (uint32_t i = 0; i < pci_get_device_count(); i++) {
    struct pci_device *pd = asc_pci_get_device(i);
    if (!pd || pd->class_code != 0x03)
      continue;
    if (!first)
      first = pd;
    if (amd && pd->vendor_id == 0x1002)
      return pd;
  }
  return native ? native : first;
}

static void sysfs_display_address(char *buf, size_t size,
                                  const struct pci_device *pci) {
  if (pci)
    snprintf(buf, size, "0000:%02x:%02x.%u", pci->bus, pci->slot, pci->func);
  else
    snprintf(buf, size, "0000:00:01.0");
}

static void u32_to_hex(uint32_t val, char *buf, int width) {
  const char *hex = "0123456789abcdef";
  buf[width] = '\0';
  for (int i = width - 1; i >= 0; i--) {
    buf[i] = hex[val & 0xF];
    val >>= 4;
  }
}

static void u64_to_dec(uint64_t val, char *buf) {
  if (val == 0) {
    buf[0] = '0';
    buf[1] = '\0';
    return;
  }
  char tmp[24];
  int i = 0;
  while (val > 0) {
    tmp[i++] = '0' + (val % 10);
    val /= 10;
  }
  int j = 0;
  while (i > 0)
    buf[j++] = tmp[--i];
  buf[j] = '\0';
}

// Create a directory under parent and return the new node.
static vfs_node_t *sysfs_mkdir(vfs_node_t *parent, const char *name) {
  vfs_node_t *existing = parent ? vfs_finddir(parent, (char *)name) : NULL;
  if (existing && (existing->flags & FS_TYPE_MASK) == FS_DIRECTORY)
    return existing;

  vfs_node_t *dir = kmalloc(sizeof(vfs_node_t));
  if (!dir)
    return NULL;
  vfs_node_init(dir);
  strncpy(dir->name, name, 127);
  dir->flags = FS_DIRECTORY | FS_PERSISTENT;
  dir->mask = 0555;
  ramfs_mount_on(dir);
  ramfs_mount_node(parent, dir);
  vfs_dentry_invalidate(parent, name);
  return dir;
}

// Symlink read callback — target stored in node->ptr (cast to char *)
static int sysfs_readlink_cb(vfs_node_t *node, char *buf, uint32_t size) {
  if (!node || !node->ptr || !buf || size == 0)
    return -1;
  const char *target = (const char *)node->ptr;
  uint32_t len = (uint32_t)strlen(target);
  if (len > size)
    len = size;
  memcpy(buf, target, len);
  return (int)len;
}

// Create a symlink under parent pointing to target.
static void sysfs_symlink(vfs_node_t *parent, const char *name,
                          const char *target) {
  if (!parent || vfs_finddir(parent, (char *)name))
    return;

  vfs_node_t *sl = kmalloc(sizeof(vfs_node_t));
  if (!sl)
    return;
  vfs_node_init(sl);
  strncpy(sl->name, name, 127);
  sl->flags = FS_SYMLINK | FS_PERSISTENT;
  sl->mask = 0777;
  sl->readlink = sysfs_readlink_cb;
  // Store target string — allocate a copy
  char *tgt = kmalloc(strlen(target) + 1);
  if (tgt)
    strcpy(tgt, target);
  sl->ptr = (vfs_node_t *)tgt; // reuse ptr field for the string
  ramfs_mount_node(parent, sl);
  vfs_dentry_invalidate(parent, name);
}

// Create a read-only file under parent with the given content.
static vfs_node_t *sysfs_mkfile(vfs_node_t *parent, const char *name,
                         const char *content) {
  vfs_node_t *existing = parent ? vfs_finddir(parent, (char *)name) : NULL;
  if (existing)
    return existing;

  vfs_node_t *f = kmalloc(sizeof(vfs_node_t));
  if (!f)
    return NULL;
  vfs_node_init(f);
  strncpy(f->name, name, 127);
  f->flags = FS_FILE | FS_PERSISTENT;
  f->mask = 0444;

  // Allocate a ramfs file backing store so read/write work
  ramfs_file_t *rf = kmalloc(sizeof(ramfs_file_t));
  if (!rf) {
    kfree(f);
    return NULL;
  }
  rf->data = NULL;
  rf->capacity = 0;
  rf->data_is_pmm = 0;
  f->device = rf;
  f->read = ramfs_read;
  f->write = ramfs_write;

  ramfs_mount_node(parent, f);
  vfs_dentry_invalidate(parent, name);

  uint32_t len = (uint32_t)strlen(content);
  vfs_write(f, 0, len, (uint8_t *)content);
  return f;
}

static void sysfs_replace(vfs_node_t *node,const char *text){if(!node||!text)return;node->length=0;vfs_write(node,0,(uint32_t)strlen(text),(uint8_t *)text);}
void sysfs_gpu_update_connector(uint32_t scanout,bool connected,const char *modes){if(scanout>=VIRTIO_GPU_MAX_SCANOUTS)return;sysfs_replace(gpu_status_nodes[scanout],connected?"connected\n":"disconnected\n");sysfs_replace(gpu_enabled_nodes[scanout],connected?"enabled\n":"disabled\n");sysfs_replace(gpu_modes_nodes[scanout],connected&&modes?modes:"");sysfs_replace(gpu_dpms_nodes[scanout],connected?"On\n":"Off\n");}

// Block class population

static void sysfs_populate_block(vfs_node_t *block_class_dir) {
  int count = block_count();
  for (int i = 0; i < count; i++) {
    struct block_device *bd = block_get(i);
    if (!bd)
      continue;

    vfs_node_t *bdir = sysfs_mkdir(block_class_dir, bd->name);
    if (!bdir)
      continue;

    /* dev: major:minor — must match /proc/partitions, /proc/diskstats,
     * /proc/self/mountinfo and stat()->st_rdev or UDisks2/Solid cannot
     * match a mount to its block device and Plasma Disk Usage stays at
     * 0 B. Numbers come from block_register() (259:N for NVMe, 8:N
     * otherwise). */
    char devbuf[32];
    char majbuf[16], minbuf[16];
    u64_to_dec(bd->linux_major, majbuf);
    u64_to_dec(bd->linux_minor, minbuf);
    strcpy(devbuf, majbuf);
    strcat(devbuf, ":");
    strcat(devbuf, minbuf);
    strcat(devbuf, "\n");
    sysfs_mkfile(bdir, "dev", devbuf);

    // size: device capacity in 512-byte sectors (Linux convention; userspace
    // multiplies by 512).  sector_size only describes the logical unit.
    uint64_t bytes = (uint64_t)bd->total_sectors *
                     (bd->sector_size ? bd->sector_size : 512);
    char sbuf[32];
    u64_to_dec(bytes / 512, sbuf);
    strcat(sbuf, "\n");
    sysfs_mkfile(bdir, "size", sbuf);

    sysfs_mkfile(bdir, "removable", "0\n");
    sysfs_mkfile(bdir, "ro", "0\n");

    // uevent: what udev/libblkid parse to identify a block device.
    // Partitions must advertise DEVTYPE=partition (not disk) or UDisks2
    // treats every partition as a whole disk with no filesystem, and the
    // Disk Usage widget finds no mountable volume (Total 0 B).
    char uevent[160];
    snprintf(uevent, sizeof(uevent),
             "MAJOR=%u\nMINOR=%u\nDEVNAME=%s\nDEVTYPE=%s\n",
             bd->linux_major, bd->linux_minor, bd->name,
             block_is_partition(bd) ? "partition" : "disk");
    sysfs_mkfile(bdir, "uevent", uevent);

    // stat: 11 space-separated counters (reads, merges, sectors, ms, ...).
    // Per-device I/O accounting does not exist yet, so report zeros.
    sysfs_mkfile(bdir, "stat", "0 0 0 0 0 0 0 0 0 0 0\n");

    // holders/: stacked devices (dm/md).  libblkid stats this directory for
    // every candidate whole-disk device; it must exist even when empty.
    sysfs_mkdir(bdir, "holders");

    // queue/: request-queue properties read by lsblk, libblkid, udev.
    vfs_node_t *queue = sysfs_mkdir(bdir, "queue");
    if (queue) {
      uint64_t ss = bd->sector_size ? bd->sector_size : 512;
      char qbuf[32];
      u64_to_dec(ss, qbuf);
      strcat(qbuf, "\n");
      sysfs_mkfile(queue, "logical_block_size", qbuf);
      sysfs_mkfile(queue, "physical_block_size", qbuf);
      sysfs_mkfile(queue, "hw_sector_size", qbuf);
      sysfs_mkfile(queue, "minimum_io_size", qbuf);
      sysfs_mkfile(queue, "optimal_io_size", "0\n");
      sysfs_mkfile(queue, "rotational", "0\n");
      sysfs_mkfile(queue, "max_sectors_kb", "1024\n");
      sysfs_mkfile(queue, "nr_requests", "128\n");
    }
  }
}

// /sys/block — Linux's top-level alias for the block class.  Tools such as
// libblkid (and therefore GIO's volume monitors) enumerate devices here and
// follow the entry to read queue/holders/dev attributes.  Entries are
// symlinks into /sys/class/block, which holds the real nodes.
static void sysfs_populate_block_alias(vfs_node_t *block_root) {
  if (!block_root)
    return;
  int count = block_count();
  for (int i = 0; i < count; i++) {
    struct block_device *bd = block_get(i);
    if (!bd)
      continue;
    char target[96];
    snprintf(target, sizeof(target), "../class/block/%s", bd->name);
    sysfs_symlink(block_root, bd->name, target);
  }
}

static void append_dec_line(vfs_node_t *dir, const char *name, uint64_t value) {
  char buf[32];
  u64_to_dec(value, buf);
  strcat(buf, "\n");
  sysfs_mkfile(dir, name, buf);
}

static void sysfs_populate_net(vfs_node_t *net_class_dir) {
  struct net_device *dev = net_device_default();
  if (!dev)
    return;
  vfs_node_t *ndir = sysfs_mkdir(net_class_dir, dev->name);
  if (!ndir)
    return;

  static const char hex[] = "0123456789abcdef";
  char mac[19];
  for (int i = 0; i < 6; i++) {
    mac[i * 3] = hex[dev->mac[i] >> 4];
    mac[i * 3 + 1] = hex[dev->mac[i] & 15];
    mac[i * 3 + 2] = i == 5 ? '\n' : ':';
  }
  mac[18] = '\0';
  sysfs_mkfile(ndir, "address", mac);
  sysfs_mkfile(ndir, "operstate",
               dev->ops->link_up(dev) ? "up\n" : "down\n");
  sysfs_mkfile(ndir, "type", "1\n");
  append_dec_line(ndir, "mtu", dev->mtu);

  const struct ipv4_config *cfg = ipv4_get_config();
  char ip[20];
  uint32_t values[3] = {cfg->address, cfg->netmask, cfg->gateway};
  const char *names[3] = {"ipv4_address", "ipv4_netmask", "ipv4_gateway"};
  for (int n = 0; n < 3; n++) {
    ip[0] = '\0';
    for (int octet = 3; octet >= 0; octet--) {
      char part[4];
      u64_to_dec((values[n] >> (octet * 8)) & 0xff, part);
      strcat(ip, part);
      strcat(ip, octet ? "." : "\n");
    }
    sysfs_mkfile(ndir, names[n], ip);
  }

  const struct ipv6_config *cfg6 = ipv6_get_config();
  char ip6[41], group[5];
  ip6[0] = '\0';
  for (int i = 0; i < 8; i++) {
    u32_to_hex(((uint16_t)cfg6->link_local[i * 2] << 8) |
                   cfg6->link_local[i * 2 + 1], group, 4);
    strcat(ip6, group);
    strcat(ip6, i == 7 ? "\n" : ":");
  }
  sysfs_mkfile(ndir, "ipv6_link_local", ip6);
  if (cfg6->global_valid) {
    ip6[0] = '\0';
    for (int i = 0; i < 8; i++) {
      u32_to_hex(((uint16_t)cfg6->global[i * 2] << 8) |
                     cfg6->global[i * 2 + 1], group, 4);
      strcat(ip6, group);
      strcat(ip6, i == 7 ? "\n" : ":");
    }
    sysfs_mkfile(ndir, "ipv6_global", ip6);
  }

  uint32_t queued, in_use;
  net_queue_snapshot(&queued, &in_use);
  append_dec_line(ndir, "rx_queue_depth", queued);
  append_dec_line(ndir, "packet_buffers_in_use", in_use);
  vfs_node_t *stats = sysfs_mkdir(ndir, "statistics");
  if (!stats)
    return;
#define NET_STAT(field) append_dec_line(stats, #field, dev->stats.field)
  NET_STAT(rx_packets); NET_STAT(tx_packets);
  NET_STAT(rx_bytes); NET_STAT(tx_bytes);
  NET_STAT(rx_dropped); NET_STAT(tx_dropped);
  NET_STAT(rx_errors); NET_STAT(tx_errors);
  NET_STAT(interrupts); NET_STAT(resets); NET_STAT(queue_full);
  NET_STAT(link_changes); NET_STAT(rx_overflows); NET_STAT(tx_underruns);
#undef NET_STAT
}

void sysfs_populate_network(void) {
  if (net_class_root)
    sysfs_populate_net(net_class_root);
}

// CPU devices population

static void sysfs_populate_cpus(vfs_node_t *cpu_dir) {
  uint32_t count = cpu_get_count();
  char name[16];
  for (uint32_t i = 0; i < count; i++) {
    strcpy(name, "cpu");
    u64_to_dec(i, name + 3);
    vfs_node_t *cdir = sysfs_mkdir(cpu_dir, name);
    if (!cdir)
      continue;
    sysfs_mkfile(cdir, "online", "1\n");
    /* Linux normalizes maximum CPU capacity to 1024. AvoryOS currently
     * treats every CPU equally, so expose that homogeneous capacity on
     * each CPU for Qt and other userspace topology probes. */
    sysfs_mkfile(cdir, "cpu_capacity", "1024\n");
    int node = numa_cpu_to_node(i);
    char nodename[16];
    strcpy(nodename, "node");
    u64_to_dec(node, nodename + 4);
    sysfs_mkdir(cdir, nodename);
  }
  // Also add a 'possible' and 'present' file at the cpu/ level
  char cpumask[16];
  strcpy(cpumask, "0-");
  u64_to_dec(count - 1, cpumask + 2);
  strcat(cpumask, "\n");
  sysfs_mkfile(cpu_dir, "possible", cpumask);
  sysfs_mkfile(cpu_dir, "present", cpumask);
  sysfs_mkfile(cpu_dir, "online", cpumask);
}

static void sysfs_populate_nodes(vfs_node_t *node_dir) {
  uint32_t count = numa_node_count();
  char nodestr[32];

  if (count > 1) {
    strcpy(nodestr, "0-");
    u64_to_dec(count - 1, nodestr + 2);
    strcat(nodestr, "\n");
  } else {
    strcpy(nodestr, "0\n");
  }

  sysfs_mkfile(node_dir, "online", nodestr);
  sysfs_mkfile(node_dir, "possible", nodestr);
  sysfs_mkfile(node_dir, "has_cpu", nodestr);
  sysfs_mkfile(node_dir, "has_normal_memory", nodestr);

  for (uint32_t i = 0; i < count; i++) {
    struct numa_node *n = numa_get_node(i);
    if (!n || !n->online)
      continue;

    char nname[16];
    strcpy(nname, "node");
    u64_to_dec(i, nname + 4);
    vfs_node_t *ndir = sysfs_mkdir(node_dir, nname);
    if (!ndir)
      continue;

    // cpumap
    char cpumap[32];
    u32_to_hex((uint32_t)n->cpu_mask, cpumap, 8);
    strcat(cpumap, "\n");
    sysfs_mkfile(ndir, "cpumap", cpumap);

    // cpulist
    char cpulist[32];
    int first_cpu = -1, last_cpu = -1;
    for (int c = 0; c < 64; c++) {
      if (n->cpu_mask & (1ULL << c)) {
        if (first_cpu < 0) first_cpu = c;
        last_cpu = c;
      }
    }
    if (first_cpu >= 0) {
      if (first_cpu == last_cpu) {
        u64_to_dec(first_cpu, cpulist);
        strcat(cpulist, "\n");
      } else {
        u64_to_dec(first_cpu, cpulist);
        strcat(cpulist, "-");
        u64_to_dec(last_cpu, cpulist + strlen(cpulist));
        strcat(cpulist, "\n");
      }
    } else {
      strcpy(cpulist, "0\n");
    }
    sysfs_mkfile(ndir, "cpulist", cpulist);

    // distance
    char distance[128] = "";
    for (uint32_t j = 0; j < count; j++) {
      char dstr[16];
      u64_to_dec(numa_distance(i, j), dstr);
      strcat(distance, dstr);
      if (j + 1 < count)
        strcat(distance, " ");
      else
        strcat(distance, "\n");
    }
    sysfs_mkfile(ndir, "distance", distance);

    // meminfo
    char meminfo[256];
    strcpy(meminfo, "Node ");
    u64_to_dec(i, meminfo + strlen(meminfo));
    strcat(meminfo, " MemTotal: ");
    u64_to_dec(n->total_memory / 1024, meminfo + strlen(meminfo));
    strcat(meminfo, " kB\nNode ");
    u64_to_dec(i, meminfo + strlen(meminfo));
    strcat(meminfo, " MemFree: ");
    u64_to_dec(n->usable_memory / 1024, meminfo + strlen(meminfo));
    strcat(meminfo, " kB\nNode ");
    u64_to_dec(i, meminfo + strlen(meminfo));
    strcat(meminfo, " MemUsed: ");
    uint64_t used = (n->total_memory > n->usable_memory) ? (n->total_memory - n->usable_memory) : 0;
    u64_to_dec(used / 1024, meminfo + strlen(meminfo));
    strcat(meminfo, " kB\n");
    sysfs_mkfile(ndir, "meminfo", meminfo);
  }
}

// Main init

void sysfs_init(void) {
  if (!fs_root)
    return;

  // Find or create the /sys directory on the root filesystem
  vfs_node_t *sys_dir = vfs_finddir(fs_root, "sys");
  if (!sys_dir && fs_root->mkdir) {
    fs_root->mkdir(fs_root, "sys", 0755);
    /* The first lookup may have cached ENOENT. Publish the newly created
     * mountpoint before looking it up again. */
    vfs_dentry_invalidate(fs_root, "sys");
    sys_dir = vfs_finddir(fs_root, "sys");
  }
  if (!sys_dir) {
    klog_puts("[SYSFS] Could not find/create /sys\n");
    return;
  }

  // Create a fresh ramfs root and mount it over /sys — same pattern as procfs
  vfs_node_t *sysfs_root = kmalloc(sizeof(vfs_node_t));
  if (!sysfs_root) {
    vfs_close(sys_dir);
    return;
  }
  vfs_node_init(sysfs_root);
  strcpy(sysfs_root->name, "sys");
  ramfs_mount_on(sysfs_root);
  sysfs_root->statfs = sysfs_statfs;
  vfs_mount_ex(sys_dir, sysfs_root, "sysfs", "sysfs", "/sys");
  vfs_close(sys_dir);

  // /sys/bus/pci/devices
  vfs_node_t *bus_dir = sysfs_mkdir(sysfs_root, "bus");
  vfs_node_t *pci_dir = sysfs_mkdir(bus_dir, "pci");

  // /sys/module/user_namespace/parameters/enable (queried by bwrap/flatpak)
  vfs_node_t *module_dir = sysfs_mkdir(sysfs_root, "module");
  if (module_dir) {
    vfs_node_t *userns_dir = sysfs_mkdir(module_dir, "user_namespace");
    if (userns_dir) {
      vfs_node_t *params_dir = sysfs_mkdir(userns_dir, "parameters");
      if (params_dir) {
        sysfs_mkfile(params_dir, "enable", "N\n");
      }
    }
  }

  // /sys/class
  vfs_node_t *class_dir = sysfs_mkdir(sysfs_root, "class");

  // /sys/devices
  // Must be created before class/drm so the symlink target dirs exist
  vfs_node_t *devices_dir = sysfs_mkdir(sysfs_root, "devices");
  sysfs_pci_init(pci_dir, devices_dir);
  vfs_node_t *system_dir = sysfs_mkdir(devices_dir, "system");
  vfs_node_t *cpu_dir = sysfs_mkdir(system_dir, "cpu");
  sysfs_populate_cpus(cpu_dir);
  vfs_node_t *node_dir = sysfs_mkdir(system_dir, "node");
  sysfs_populate_nodes(node_dir);

  // /sys/class/block
  vfs_node_t *block_class = sysfs_mkdir(class_dir, "block");
  sysfs_populate_block(block_class);

  // /sys/block — top-level alias for the block class.  Created after the
  // class so the symlink targets exist.
  vfs_node_t *block_alias = sysfs_mkdir(sysfs_root, "block");
  sysfs_populate_block_alias(block_alias);

  // /sys/class/net
  net_class_root = sysfs_mkdir(class_dir, "net");

  // /sys/class/power_supply
  vfs_node_t *ps_class = sysfs_mkdir(class_dir, "power_supply");
  if (ps_class) {
    vfs_node_t *acad = sysfs_mkdir(ps_class, "ACAD");
    if (acad) {
      sysfs_mkfile(acad, "type", "Mains\n");
      sysfs_mkfile(acad, "online", "1\n");
    }
  }

  // /sys/class/drm/card0
  // card0 is a symlink to the real device path (wlroots uses readlink on it)
  // Target: ../../devices/pci0000:00/0000:BB:SS.F/drm/card0
  // Find the display controller (PCI class 0x03)
  vfs_node_t *drm_class = sysfs_mkdir(class_dir, "drm");
  {
    const struct pci_device *native_pci = sysfs_display_pci(false);
    const struct pci_device *amd_pci = sysfs_display_pci(true);
    char pci_addr[20], amd_addr[20];
    sysfs_display_address(pci_addr, sizeof(pci_addr), native_pci);
    sysfs_display_address(amd_addr, sizeof(amd_addr), amd_pci);
    // Build symlink target as a relative path from /sys/class/drm/card0.
    // wlroots calls readlink() and then resolves the result relative to the
    // symlink's parent directory (/sys/class/drm/), so we must use a relative
    // path — exactly what the real Linux kernel provides.
    // From /sys/class/drm/ we need: ../../devices/pci0000:00/<addr>/drm/card0
    char sl_target[128];
    strcpy(sl_target, "../../devices/pci0000:00/");
    strcat(sl_target, pci_addr);
    strcat(sl_target, "/drm/card0");
    sysfs_symlink(drm_class, "card0", sl_target);

    char conn_sl_target[160];
    strcpy(conn_sl_target, "../../devices/pci0000:00/");
    strcat(conn_sl_target, pci_addr);
    strcat(conn_sl_target, "/drm/card0/card0-HDMI-A-1");
    sysfs_symlink(drm_class, "card0-HDMI-A-1", conn_sl_target);

    /* Phase 6 C7: the imported amdgpu DRM device is card2 (LinuxKPI minor
     * 2).  Weston's --drm-device and udev resolve a name through
     * /sys/class/<subsystem>/<sysname> (eudev's subsystem_sysname_dirs), so
     * the node must exist here or `weston --drm-device=card2` logs "not a
     * KMS device" and never opens it.  The devnode name comes from the
     * uevent file's DEVNAME. */
    char sl_target2[128];
    strcpy(sl_target2, "../../devices/pci0000:00/");
    strcat(sl_target2, amd_addr);
    strcat(sl_target2, "/drm/card2");
    sysfs_symlink(drm_class, "card2", sl_target2);

    /* amdgpu's render node.  Mesa's loader (loader_get_driver_for_fd ->
     * drmGetDevice2) resolves it through /sys/dev/char/226:129/device/drm
     * and the PCI subsystem IDs; without this entry it cannot identify the
     * GPU and falls back to llvmpipe. */
    char sl_target3[128];
    strcpy(sl_target3, "../../devices/pci0000:00/");
    strcat(sl_target3, amd_addr);
    strcat(sl_target3, "/drm/renderD129");
    sysfs_symlink(drm_class, "renderD129", sl_target3);

    // Also create the real device directory that the symlink points to
    vfs_node_t *pci_seg = sysfs_mkdir(devices_dir, "pci0000:00");
    vfs_node_t *gpu_dev = sysfs_mkdir(pci_seg, pci_addr);
    vfs_node_t *drm_dev = sysfs_mkdir(gpu_dev, "drm");
    vfs_node_t *card0_dir = sysfs_mkdir(drm_dev, "card0");
    sysfs_mkfile(card0_dir, "dev", "226:0\n");
    sysfs_mkfile(card0_dir, "uevent",
                 "MAJOR=226\nMINOR=0\nDEVNAME=dri/card0\n"
                 "DEVTYPE=drm_minor\nSUBSYSTEM=drm\n");

    vfs_node_t *conn_dir = sysfs_mkdir(card0_dir, "card0-HDMI-A-1");
    char live_modes[512];bool live_connected=false;strcpy(live_modes,"1280x800\n");virtio_gpu_scanout_summary(0,&live_connected,live_modes,sizeof(live_modes));
    gpu_status_nodes[0]=sysfs_mkfile(conn_dir, "status", live_connected?"connected\n":"disconnected\n");
    gpu_enabled_nodes[0]=sysfs_mkfile(conn_dir, "enabled", live_connected?"enabled\n":"disabled\n");
    gpu_modes_nodes[0]=sysfs_mkfile(conn_dir, "modes", live_connected?live_modes:"");
    gpu_dpms_nodes[0]=sysfs_mkfile(conn_dir, "dpms", live_connected?"On\n":"Off\n");
    sysfs_mkfile(conn_dir, "uevent",
                 "DEVTYPE=drm_connector\nSUBSYSTEM=drm\nHOTPLUG=1\n"
                 "CONNECTOR=HDMI-A-1\n");
    sysfs_symlink(conn_dir, "subsystem", "../../../../../../class/drm");
    sysfs_symlink(conn_dir, "device", "../../..");
    uint32_t live_heads=virtio_gpu_scanout_count();if(live_heads>VIRTIO_GPU_MAX_SCANOUTS)live_heads=VIRTIO_GPU_MAX_SCANOUTS;
    for(uint32_t head=1;head<live_heads;head++){char nbuf[12],cname[40];u64_to_dec(head+1,nbuf);strcpy(cname,"card0-HDMI-A-");strcat(cname,nbuf);vfs_node_t *cdir=sysfs_mkdir(card0_dir,cname);char hmodes[512];bool hconnected=false;virtio_gpu_scanout_summary(head,&hconnected,hmodes,sizeof(hmodes));gpu_status_nodes[head]=sysfs_mkfile(cdir,"status",hconnected?"connected\n":"disconnected\n");gpu_enabled_nodes[head]=sysfs_mkfile(cdir,"enabled",hconnected?"enabled\n":"disabled\n");gpu_modes_nodes[head]=sysfs_mkfile(cdir,"modes",hconnected?hmodes:"");gpu_dpms_nodes[head]=sysfs_mkfile(cdir,"dpms",hconnected?"On\n":"Off\n");char hue[128];strcpy(hue,"DEVTYPE=drm_connector\nSUBSYSTEM=drm\nHOTPLUG=1\nCONNECTOR=HDMI-A-");strcat(hue,nbuf);strcat(hue,"\n");sysfs_mkfile(cdir,"uevent",hue);sysfs_symlink(cdir,"subsystem","../../../../../../class/drm");sysfs_symlink(cdir,"device","../../..");char target[160];strcpy(target,"../../devices/pci0000:00/");strcat(target,pci_addr);strcat(target,"/drm/card0/");strcat(target,cname);sysfs_symlink(drm_class,cname,target);}
    // subsystem symlink inside card0 → points back to /sys/class/drm
    // wlroots walks up the tree using this to identify the subsystem.
    // Relative from /sys/devices/pci0000:00/<addr>/drm/card0/ to
    // /sys/class/drm/
    sysfs_symlink(card0_dir, "subsystem", "../../../../../class/drm");
    sysfs_symlink(card0_dir, "device", "../..");

    vfs_node_t *native_render = sysfs_mkdir(drm_dev, "renderD128");
    sysfs_mkfile(native_render, "dev", "226:128\n");
    sysfs_mkfile(native_render, "uevent",
                 "MAJOR=226\nMINOR=128\nDEVNAME=dri/renderD128\n"
                 "DEVTYPE=drm_minor\nSUBSYSTEM=drm\n");
    sysfs_symlink(native_render, "subsystem", "../../../../../class/drm");
    sysfs_symlink(native_render, "device", "../..");
    char native_render_target[128];
    snprintf(native_render_target, sizeof(native_render_target),
             "../../devices/pci0000:00/%s/drm/renderD128", pci_addr);
    sysfs_symlink(drm_class, "renderD128", native_render_target);

    /* Native card0 belongs to VirtIO; AMD's nodes belong to its own PCI
     * parent. Sharing the first display-class parent made udev/Xorg identify
     * the virtual display as AMD whenever AMD appeared first on the bus. */
    vfs_node_t *amd_dev = sysfs_mkdir(pci_seg, amd_addr);
    vfs_node_t *amd_drm = sysfs_mkdir(amd_dev, "drm");
    vfs_node_t *card2_dir = sysfs_mkdir(amd_drm, "card2");
    sysfs_mkfile(card2_dir, "dev", "226:2\n");
    sysfs_mkfile(card2_dir, "uevent",
                 "MAJOR=226\nMINOR=2\nDEVNAME=dri/card2\n"
                 "DEVTYPE=drm_minor\nSUBSYSTEM=drm\n");
    sysfs_symlink(card2_dir, "subsystem", "../../../../../class/drm");
    sysfs_symlink(card2_dir, "device", "../..");

    /* renderD129 (amdgpu render node, DRM minor 129). */
    vfs_node_t *render_dir = sysfs_mkdir(amd_drm, "renderD129");
    sysfs_mkfile(render_dir, "dev", "226:129\n");
    sysfs_mkfile(render_dir, "uevent",
                 "MAJOR=226\nMINOR=129\nDEVNAME=dri/renderD129\n"
                 "DEVTYPE=drm_minor\nSUBSYSTEM=drm\n");
    sysfs_symlink(render_dir, "subsystem", "../../../../../class/drm");
    sysfs_symlink(render_dir, "device", "../..");
    // Keep the intermediate drm/ and pci0000:00/ directories as plain sysfs
    // containers. Giving either a uevent file makes libudev treat it as a
    // device, but neither has a subsystem; Xorg then dereferences a NULL
    // subsystem while walking from card0 to its PCI parent.
    char gpu_uevent[128];
    snprintf(gpu_uevent, sizeof(gpu_uevent),
             "DRIVER=virtio-pci\nPCI_ID=%04x:%04x\nPCI_SLOT_NAME=%s\nSUBSYSTEM=pci\n",
             native_pci ? native_pci->vendor_id : 0x1234,
             native_pci ? native_pci->device_id : 0x1111, pci_addr);
    sysfs_mkfile(gpu_dev, "uevent", gpu_uevent);
    sysfs_symlink(gpu_dev, "subsystem", "../../../bus/pci");

    // Mesa reads vendor/device/class from /sys/dev/char/226:0/device/vendor
    // etc. The path resolves: 226:0 -> card0_dir, then "device" -> gpu_dev.
    // gpu_dev is a freshly created node (separate from sysfs_populate_pci's
    // node), so we must add vendor/device/class/irq files here explicitly.
    {
      uint32_t vid = 0x1234, did = 0x1111, cls = 0x030000;
      uint32_t sub_vid = 0, sub_did = 0;
      uint32_t pci_cnt = pci_get_device_count();
      for (uint32_t ii = 0; ii < pci_cnt; ii++) {
        struct pci_device *gpd = asc_pci_get_device(ii);
        if (gpd && gpd == native_pci) {
          vid = gpd->vendor_id;
          did = gpd->device_id;
          cls = ((uint32_t)gpd->class_code << 16) |
                ((uint32_t)gpd->subclass << 8) | gpd->prog_if;
          /* libdrm's drmGetDevice2()/drmParsePciIds() reads all four IDs;
           * without subsystem_vendor/device Mesa cannot identify the GPU
           * and silently falls back to llvmpipe (Phase 6 C7). */
          uint32_t sub = pci_config_read32(gpd->bus, gpd->slot, gpd->func,
                                           0x2c);
          sub_vid = sub & 0xffff;
          sub_did = sub >> 16;
          break;
        }
      }
      char ssvbuf[10], ssdbuf[10];
      ssvbuf[0] = '0';
      ssvbuf[1] = 'x';
      u32_to_hex(sub_vid, ssvbuf + 2, 4);
      ssvbuf[6] = '\n';
      ssvbuf[7] = '\0';
      ssdbuf[0] = '0';
      ssdbuf[1] = 'x';
      u32_to_hex(sub_did, ssdbuf + 2, 4);
      ssdbuf[6] = '\n';
      ssdbuf[7] = '\0';
      char vbuf[10], dbuf[10], cbuf[12];
      vbuf[0] = '0';
      vbuf[1] = 'x';
      u32_to_hex(vid, vbuf + 2, 4);
      vbuf[6] = '\n';
      vbuf[7] = '\0';
      dbuf[0] = '0';
      dbuf[1] = 'x';
      u32_to_hex(did, dbuf + 2, 4);
      dbuf[6] = '\n';
      dbuf[7] = '\0';
      cbuf[0] = '0';
      cbuf[1] = 'x';
      u32_to_hex(cls, cbuf + 2, 6);
      cbuf[8] = '\n';
      cbuf[9] = '\0';
      sysfs_mkfile(gpu_dev, "vendor", vbuf);
      sysfs_mkfile(gpu_dev, "device", dbuf);
      sysfs_mkfile(gpu_dev, "subsystem_vendor", ssvbuf);
      sysfs_mkfile(gpu_dev, "subsystem_device", ssdbuf);
      sysfs_mkfile(gpu_dev, "class", cbuf);
      sysfs_mkfile(gpu_dev, "irq", "11\n");
      sysfs_mkfile(gpu_dev, "enable", "1\n");
    }

    // Store GPU DEVPATH for netlink fake uevents
    strcpy(sysfs_gpu_devpath, "/devices/pci0000:00/");
    strcat(sysfs_gpu_devpath, pci_addr);
    strcat(sysfs_gpu_devpath, "/drm/card0");
    strcpy(sysfs_gpu_connector_devpath, sysfs_gpu_devpath);
    strcat(sysfs_gpu_connector_devpath, "/card0-HDMI-A-1");
    strcpy(sysfs_gpu2_devpath, "/devices/pci0000:00/");
    strcat(sysfs_gpu2_devpath, amd_addr);
    strcat(sysfs_gpu2_devpath, "/drm/card2");

    // Add devices/ directory under drm_class so that
    // /sys/subsystem/drm/devices/ enumeration finds card0
    vfs_node_t *drm_devices_dir = sysfs_mkdir(drm_class, "devices");
    if (drm_devices_dir) {
      // Relative from /sys/class/drm/devices/ → /sys/devices/pci0000:00/...
      // wlroots does readlink() then resolves relative to symlink parent
      char card0_rel[128];
      strcpy(card0_rel, "../../../devices/pci0000:00/");
      strcat(card0_rel, pci_addr);
      strcat(card0_rel, "/drm/card0");
      sysfs_symlink(drm_devices_dir, "card0", card0_rel);

      char conn_rel[160];
      strcpy(conn_rel, "../../../devices/pci0000:00/");
      strcat(conn_rel, pci_addr);
      strcat(conn_rel, "/drm/card0/card0-HDMI-A-1");
      sysfs_symlink(drm_devices_dir, "card0-HDMI-A-1", conn_rel);

      /* Phase 6 C7: same for amdgpu's card2 and render node. */
      char card2_rel[128];
      strcpy(card2_rel, "../../../devices/pci0000:00/");
      strcat(card2_rel, amd_addr);
      strcat(card2_rel, "/drm/card2");
      sysfs_symlink(drm_devices_dir, "card2", card2_rel);

      char render_rel[128];
      strcpy(render_rel, "../../../devices/pci0000:00/");
      strcat(render_rel, amd_addr);
      strcat(render_rel, "/drm/renderD129");
      sysfs_symlink(drm_devices_dir, "renderD129", render_rel);
    }
  }

  // /sys/devices/virtual/input/input0/event0
  vfs_node_t *virtual_dir = sysfs_mkdir(devices_dir, "virtual");
  sysfs_mkfile(virtual_dir, "uevent", "SUBSYSTEM=virtual\n");
  sysfs_symlink(virtual_dir, "subsystem", "../../class");
  vfs_node_t *virtual_input_dir = sysfs_mkdir(virtual_dir, "input");
  sysfs_mkfile(virtual_input_dir, "uevent", "SUBSYSTEM=input\nID_INPUT=1\n");
  sysfs_symlink(virtual_input_dir, "subsystem", "../../../class/input");
  vfs_node_t *input0_dir = sysfs_mkdir(virtual_input_dir, "input0");
  vfs_node_t *event0_dir = sysfs_mkdir(input0_dir, "event0");

  sysfs_mkfile(event0_dir, "dev", "13:64\n");
  sysfs_mkfile(event0_dir, "uevent",
               "MAJOR=13\nMINOR=64\nDEVNAME=input/event0\n"
               "SUBSYSTEM=input\nID_INPUT=1\nID_INPUT_KEYBOARD=1\n"
               "ID_BUS=isa\n"
               "PRODUCT=3/1/1/ab41\n"
               "ID_SERIAL=ascentos_kbd\nNAME=\"AscentOS Keyboard\"\n"
               "ID_SEAT=seat0\n");
  sysfs_mkfile(input0_dir, "uevent",
               "SUBSYSTEM=input\nID_INPUT=1\nID_INPUT_KEYBOARD=1\n"
               "NAME=\"AscentOS Keyboard\"\n"
               "PRODUCT=3/1/1/ab41\n"
               "ID_SEAT=seat0\n");
  sysfs_mkfile(input0_dir, "name", "AscentOS Keyboard\n");
  vfs_node_t *id0_dir = sysfs_mkdir(input0_dir, "id");
  sysfs_mkfile(id0_dir, "bustype", "0011\n");
  sysfs_mkfile(id0_dir, "vendor", "0001\n");
  sysfs_mkfile(id0_dir, "product", "0001\n");
  sysfs_mkfile(id0_dir, "version", "ab41\n");

  sysfs_symlink(input0_dir, "subsystem", "../../../../class/input");
  sysfs_symlink(event0_dir, "subsystem", "../../../../../class/input");
  sysfs_symlink(event0_dir, "device", "..");

  // /sys/class/input/event0 symlink
  vfs_node_t *input_class = sysfs_mkdir(class_dir, "input");
  sysfs_symlink(input_class, "event0",
                "../../devices/virtual/input/input0/event0");

  // /sys/devices/virtual/input/input1/event1
  vfs_node_t *input1_dir = sysfs_mkdir(virtual_input_dir, "input1");
  vfs_node_t *event1_dir = sysfs_mkdir(input1_dir, "event1");

  sysfs_mkfile(event1_dir, "dev", "13:65\n");
  sysfs_mkfile(event1_dir, "uevent",
               "MAJOR=13\nMINOR=65\nDEVNAME=input/event1\n"
               "SUBSYSTEM=input\nID_INPUT=1\nID_INPUT_MOUSE=1\n"
               "ID_SERIAL=ascentos_mouse\nNAME=\"AscentOS Mouse\"\n"
               "ID_SEAT=seat0\n");
  sysfs_mkfile(input1_dir, "uevent",
               "SUBSYSTEM=input\nID_INPUT=1\nID_INPUT_MOUSE=1\n"
               "NAME=\"AscentOS Mouse\"\n"
               "PRODUCT=3/1/1/ab42\n"
               "ID_SEAT=seat0\n");
  sysfs_mkfile(input1_dir, "name", "AscentOS Mouse\n");
  vfs_node_t *id1_dir = sysfs_mkdir(input1_dir, "id");
  sysfs_mkfile(id1_dir, "bustype", "0011\n");
  sysfs_mkfile(id1_dir, "vendor", "0002\n");
  sysfs_mkfile(id1_dir, "product", "0005\n");
  sysfs_mkfile(id1_dir, "version", "0000\n");

  sysfs_symlink(input1_dir, "subsystem", "../../../../class/input");
  sysfs_symlink(event1_dir, "subsystem", "../../../../../class/input");
  sysfs_symlink(event1_dir, "device", "..");

  // /sys/class/input/event1 symlink
  sysfs_symlink(input_class, "event1",
                "../../devices/virtual/input/input1/event1");

  // Add devices/ directory under input_class so that
  // /sys/subsystem/input/devices/ enumeration finds input devices
  vfs_node_t *input_devices_dir = sysfs_mkdir(input_class, "devices");
  if (input_devices_dir) {
    sysfs_symlink(input_devices_dir, "event0", "../event0");
    sysfs_symlink(input_devices_dir, "event1", "../event1");
  }

  // /sys/class/sound — HDA card0 (major 116, see alsa_emu.c). Without these
  // nodes the ALSA monitor enumerates zero cards and rescans in a
  // ppoll(timeout=0) loop (wireplumber 100% CPU); with a stable card0 it
  // enumerates once then blocks on uevent hotplug like Linux.
  vfs_node_t *sound_class = sysfs_mkdir(class_dir, "sound");
  if (sound_class) {
    vfs_node_t *card0_dir = sysfs_mkdir(sound_class, "card0");
    if (card0_dir) {
      sysfs_mkfile(card0_dir, "number", "0\n");
      sysfs_mkfile(card0_dir, "uevent",
                   "MAJOR=116\nMINOR=0\nDEVNAME=snd/controlC0\n"
                   "SUBSYSTEM=sound\nSOUND_CLASS=control\n");
      sysfs_symlink(card0_dir, "subsystem", "../../class/sound");
    }
    vfs_node_t *ctl_dir = sysfs_mkdir(sound_class, "controlC0");
    if (ctl_dir) {
      sysfs_mkfile(ctl_dir, "dev", "116:0\n");
      sysfs_mkfile(ctl_dir, "uevent",
                   "MAJOR=116\nMINOR=0\nDEVNAME=snd/controlC0\n"
                   "SUBSYSTEM=sound\n");
      sysfs_symlink(ctl_dir, "subsystem", "../../class/sound");
    }
    vfs_node_t *pcm_dir = sysfs_mkdir(sound_class, "pcmC0D0p");
    if (pcm_dir) {
      sysfs_mkfile(pcm_dir, "dev", "116:24\n");
      sysfs_mkfile(pcm_dir, "uevent",
                   "MAJOR=116\nMINOR=24\nDEVNAME=snd/pcmC0D0p\n"
                   "SUBSYSTEM=sound\n");
      sysfs_symlink(pcm_dir, "subsystem", "../../class/sound");
    }
    vfs_node_t *snd_devices_dir = sysfs_mkdir(sound_class, "devices");
    if (snd_devices_dir) {
      sysfs_symlink(snd_devices_dir, "card0", "../card0");
      sysfs_symlink(snd_devices_dir, "controlC0", "../controlC0");
      sysfs_symlink(snd_devices_dir, "pcmC0D0p", "../pcmC0D0p");
    }
  }

  // /sys/class/tty/tty0/active — elogind manager_connect_console() opens it
  // to watch foreground VT switches; seat_read_active_vt() parses the content
  // with vtnr_from_tty(), which expects a TTY *name* ("tty1"), not a bare
  // number ("0"/"1" fail its "tty" prefix check with EINVAL). AvoryOS has a
  // single console; report VT 1. Without the file elogind warns "System has
  // /dev/tty0 but not .../active".
  vfs_node_t *tty_class = sysfs_mkdir(class_dir, "tty");
  if (tty_class) {
    vfs_node_t *tty0_dir = sysfs_mkdir(tty_class, "tty0");
    if (tty0_dir)
      sysfs_mkfile(tty0_dir, "active", "tty1\n");
  }

  // /sys/dev/block  /sys/dev/char
  vfs_node_t *dev_dir = sysfs_mkdir(sysfs_root, "dev");
  vfs_node_t *dev_block_dir = sysfs_mkdir(dev_dir, "block");
  /* UDisks2 resolves every block device through /sys/dev/block/<major>:<minor>
   * (libudev's sysname path). Without these symlinks it sees /sys/block but
   * cannot map a /dev node back to its sysfs entry, so no StorageVolume gets
   * a Size or MountPoints and Plasma Disk Usage stays at 0 B. */
  if (dev_block_dir) {
    int bcount = block_count();
    for (int bi = 0; bi < bcount; bi++) {
      struct block_device *bd = block_get(bi);
      if (!bd || !bd->name[0])
        continue;
      char link_name[32];
      char majbuf[16], minbuf[16];
      u64_to_dec(bd->linux_major, majbuf);
      u64_to_dec(bd->linux_minor, minbuf);
      strcpy(link_name, majbuf);
      strcat(link_name, ":");
      strcat(link_name, minbuf);
      char link_target[96];
      snprintf(link_target, sizeof(link_target), "../../class/block/%s",
               bd->name);
      sysfs_symlink(dev_block_dir, link_name, link_target);
    }
  }
  vfs_node_t *char_dir = sysfs_mkdir(dev_dir, "char");
  {
    char dev_target[160];
    snprintf(dev_target, sizeof(dev_target), "../..%s", sysfs_gpu_devpath);
    sysfs_symlink(char_dir, "226:0", dev_target);
    snprintf(dev_target, sizeof(dev_target), "../..%s", sysfs_gpu2_devpath);
    sysfs_symlink(char_dir, "226:2", dev_target);
    char amd_addr[20];
    sysfs_display_address(amd_addr, sizeof(amd_addr), sysfs_display_pci(true));
    snprintf(dev_target, sizeof(dev_target),
             "../../devices/pci0000:00/%s/drm/renderD129", amd_addr);
    sysfs_symlink(char_dir, "226:129", dev_target);
    char native_addr[20];
    sysfs_display_address(native_addr, sizeof(native_addr), sysfs_display_pci(false));
    snprintf(dev_target, sizeof(dev_target),
             "../../devices/pci0000:00/%s/drm/renderD128", native_addr);
    sysfs_symlink(char_dir, "226:128", dev_target);
  }

  // Input char devices: /sys/dev/char/13:64 -> symlink to virtual device
  // Relative from /sys/dev/char/13:64 :
  // ../../devices/virtual/input/input0/event0
  sysfs_symlink(char_dir, "13:64", "../../devices/virtual/input/input0/event0");

  // Input char devices: /sys/dev/char/13:65 -> symlink to virtual device
  // Relative from /sys/dev/char/13:65 :
  // ../../devices/virtual/input/input1/event1
  sysfs_symlink(char_dir, "13:65", "../../devices/virtual/input/input1/event1");

  // Sound char devices (major 116, see alsa_emu.c): libudev resolves
  // /dev/snd/* back to /sys/class/sound through these.
  sysfs_symlink(char_dir, "116:0", "../../class/sound/controlC0");
  sysfs_symlink(char_dir, "116:24", "../../class/sound/pcmC0D0p");

  // /sys/subsystem
  // libinput and others expect this to exist for device discovery.
  // Modern Linux has /sys/subsystem/ where entries are symlinks to bus or
  // class.
  vfs_node_t *subsystem_dir = sysfs_mkdir(sysfs_root, "subsystem");
  if (subsystem_dir) {
    sysfs_symlink(subsystem_dir, "pci", "../bus/pci");
    sysfs_symlink(subsystem_dir, "input", "../class/input");
    sysfs_symlink(subsystem_dir, "drm", "../class/drm");
    sysfs_symlink(subsystem_dir, "sound", "../class/sound");
  }

  // /sys/kernel
  vfs_node_t *kernel_dir = sysfs_mkdir(sysfs_root, "kernel");
  sysfs_mkfile(kernel_dir, "hostname", "ascentos\n");

  // /sys/power
  vfs_node_t *power_dir = sysfs_mkdir(sysfs_root, "power");
  sysfs_mkfile(power_dir, "state", "mem\n");

  // /sys/fs/cgroup mountpoint for cgroup2. OpenRC/elogind mount cgroup2
  // here; without the dirs mount(2) fails with ENOENT and elogind fails
  // with "Cannot determine cgroup we are running in: No data available".
  // NOTE: dirs only, no pre-mount. Hybrid layout is tmpfs root + cgroup2/v1
  // subdirs (mounted by OpenRC/elogind). Pre-mounting cgroup2 at root breaks
  // their tmpfs-root expectation (EBUSY) and shadows the container.
  vfs_node_t *fs_dir = sysfs_mkdir(sysfs_root, "fs");
  if (fs_dir)
    sysfs_mkdir(fs_dir, "cgroup");

  // Hybrid root must be tmpfs (container), not sysfs dir: elogind stats
  // /sys/fs/cgroup expecting tmpfs (hybrid) or cgroup2 (unified); sysfs magic
  // yields ENOMEDIUM without further reads. Pre-mount tmpfs here so the first
  // statfs already sees tmpfs; submounts (unified/elogind) stack on top.
  tmpfs_mount_at("/sys/fs/cgroup");

  // /run/udev/data population
  // libinput often falls back to the udev database if uevent is insufficient.
  // We ensure /run/udev/data is present and populated with required flags.
  vfs_node_t *run_dir = vfs_resolve_path("/run");
  if (!run_dir) {
    // If /run doesn't exist, create it in root.
    if (fs_root && fs_root->mkdir) {
      fs_root->mkdir(fs_root, "run", 0755);
      run_dir = vfs_resolve_path("/run");
    }
  }

  if (run_dir) {
    vfs_node_t *udev_dir = vfs_finddir(run_dir, "udev");
    if (!udev_dir)
      udev_dir = sysfs_mkdir(run_dir, "udev");

    vfs_node_t *data_dir = udev_dir ? vfs_finddir(udev_dir, "data") : NULL;
    if (!data_dir && udev_dir)
      data_dir = sysfs_mkdir(udev_dir, "data");

    if (data_dir) {
      sysfs_mkfile(data_dir, "c13:64",
                   "P:/devices/virtual/input/input0/event0\n"
                   "E:ID_INPUT=1\n"
                   "E:ID_INPUT_KEYBOARD=1\n"
                   "E:ID_SEAT=seat0\n"
                   "E:NAME=\"AscentOS Keyboard\"\n");
      sysfs_mkfile(data_dir, "c13:65",
                   "P:/devices/virtual/input/input1/event1\n"
                   "E:ID_INPUT=1\n"
                   "E:ID_INPUT_MOUSE=1\n"
                   "E:ID_SEAT=seat0\n"
                   "E:NAME=\"AscentOS Mouse\"\n");
      sysfs_mkfile(data_dir, "c116:0",
                   "P:/class/sound/controlC0\n"
                   "E:SUBSYSTEM=sound\n"
                   "E:SOUND_CLASS=control\n"
                   "E:ID_SEAT=seat0\n");
      sysfs_mkfile(data_dir, "c116:24",
                   "P:/class/sound/pcmC0D0p\n"
                   "E:SUBSYSTEM=sound\n"
                   "E:ID_SEAT=seat0\n");
    }


    if (data_dir) vfs_close(data_dir);
    if (udev_dir) vfs_close(udev_dir);
    vfs_close(run_dir);
  }

  klog_puts("[OK] SysFS initialized at /sys\n");
}

/* ── dynamic class/device/attribute nodes (LinuxKPI device core) ─────────── */

/* Dynamic sysfs nodes created by the LinuxKPI bridge.  `impl` carries a magic
 * so the removal path (asc_sysfs_remove/sysfs_remove_children) can release the
 * context and the record before the ramfs core frees the node — ramfs_unlink()
 * would otherwise mistake node->device for a ramfs_file_t. */
#define SYSFS_DYN_ATTR_MAGIC 0x53444154u /* "SDAT" */
#define SYSFS_DYN_BIN_MAGIC 0x5344424Eu  /* "SDB N" */

struct sysfs_dyn_attr {
  void *ctx;
  int (*show)(void *ctx, char *buf, unsigned int size);
  int (*store)(void *ctx, const char *buf, unsigned int size);
  void (*release)(void *ctx);
};

struct sysfs_dyn_bin {
  void *ctx;
  int (*read)(void *ctx, unsigned int offset, char *buf, unsigned int size);
  int (*write)(void *ctx, unsigned int offset, const char *buf,
               unsigned int size);
  void (*release)(void *ctx);
};

static uint32_t sysfs_dyn_attr_read(vfs_node_t *node, uint32_t offset,
                                    uint32_t size, uint8_t *buffer) {
  struct sysfs_dyn_attr *attr = node ? (struct sysfs_dyn_attr *)node->device : NULL;
  char *tmp;
  int len;
  uint32_t n;

  if (!attr || !attr->show || !size)
    return 0;

  tmp = kmalloc(PAGE_SIZE);
  if (!tmp)
    return 0;
  len = attr->show(attr->ctx, tmp, PAGE_SIZE);
  if (len < 0)
    len = 0;
  if (offset >= (uint32_t)len) {
    kfree(tmp);
    return 0;
  }
  n = (uint32_t)len - offset;
  if (n > size)
    n = size;
  memcpy(buffer, tmp + offset, n);
  kfree(tmp);
  return n;
}

static uint32_t sysfs_dyn_bin_read(vfs_node_t *node, uint32_t offset,
                                   uint32_t size, uint8_t *buffer) {
  struct sysfs_dyn_bin *bin = node ? (struct sysfs_dyn_bin *)node->device : NULL;
  int n;

  if (!bin || !bin->read || !size)
    return 0;
  n = bin->read(bin->ctx, offset, (char *)buffer, size);
  return n < 0 ? 0 : (uint32_t)n;
}

static uint32_t sysfs_dyn_attr_write(vfs_node_t *node, uint32_t offset,
                                     uint32_t size, uint8_t *buffer) {
  struct sysfs_dyn_attr *attr = node ? (struct sysfs_dyn_attr *)node->device : NULL;
  char *tmp;
  int ret;

  (void)offset;
  if (!attr || !attr->store || !size)
    return 0;

  tmp = kmalloc((size_t)size + 1);
  if (!tmp)
    return 0;
  memcpy(tmp, buffer, size);
  tmp[size] = '\0';
  ret = attr->store(attr->ctx, tmp, size);
  kfree(tmp);
  return ret < 0 ? 0 : size;
}

static uint32_t sysfs_dyn_bin_write(vfs_node_t *node, uint32_t offset,
                                    uint32_t size, uint8_t *buffer) {
  struct sysfs_dyn_bin *bin = node ? (struct sysfs_dyn_bin *)node->device : NULL;
  char *tmp;
  int ret;

  if (!bin || !bin->write || !size)
    return 0;
  tmp = kmalloc((size_t)size + 1);
  if (!tmp)
    return 0;
  memcpy(tmp, buffer, size);
  tmp[size] = '\0';
  ret = bin->write(bin->ctx, offset, tmp, size);
  kfree(tmp);
  return ret < 0 ? 0 : size;
}

void *asc_sysfs_class_dir(const char *name) {
  vfs_node_t *class_root;
  vfs_node_t *dir;

  if (!name)
    return NULL;
  class_root = vfs_resolve_path("/sys/class");
  if (!class_root)
    return NULL;
  dir = sysfs_mkdir(class_root, name);
  vfs_close(class_root);
  return dir;
}

void *asc_sysfs_device_dir(void *class_dir, const char *dev_name) {
  if (!class_dir || !dev_name || !*dev_name)
    return NULL;
  return sysfs_mkdir((vfs_node_t *)class_dir, dev_name);
}

/* Release the bridge record attached to a dynamic node before the ramfs core
 * frees the node itself.  Plain sysfs files (ramfs_file_t) are left alone. */
static void sysfs_dyn_node_release(vfs_node_t *child) {
  if (!child)
    return;

  if (child->impl == SYSFS_DYN_ATTR_MAGIC) {
    struct sysfs_dyn_attr *a = child->device;

    if (a) {
      if (a->release)
        a->release(a->ctx);
      kfree(a);
    }
    child->device = NULL;
  } else if (child->impl == SYSFS_DYN_BIN_MAGIC) {
    struct sysfs_dyn_bin *b = child->device;

    if (b) {
      if (b->release)
        b->release(b->ctx);
      kfree(b);
    }
    child->device = NULL;
  }
}

static void sysfs_remove_file_child(vfs_node_t *dir, vfs_node_t *child) {
  sysfs_dyn_node_release(child);
  vfs_unlink(dir, child->name);
}

static void sysfs_remove_children(vfs_node_t *dir) {
  for (;;) {
    bool removed = false;
    uint32_t index = 0;
    struct dirent *de;

    while ((de = vfs_readdir(dir, index)) != NULL) {
      char name[128];
      vfs_node_t *child;

      if (strcmp(de->name, ".") == 0 || strcmp(de->name, "..") == 0) {
        index++;
        continue;
      }
      /* Copy before anything can recurse: ramfs_readdir returns a shared
       * static dirent, and removing a subdirectory reads it again. */
      strncpy(name, de->name, sizeof(name) - 1);
      name[sizeof(name) - 1] = '\0';

      child = vfs_finddir(dir, name);
      if (!child) {
        index++;
        continue;
      }
      if ((child->flags & FS_TYPE_MASK) == FS_DIRECTORY) {
        sysfs_remove_children(child);
        vfs_rmdir(dir, name);
      } else {
        sysfs_remove_file_child(dir, child);
      }
      removed = true;
      break; /* enumeration shifted; restart from the first entry */
    }
    if (!removed)
      break;
  }
}

void asc_sysfs_remove(void *dir, const char *name) {
  vfs_node_t *d = dir;
  vfs_node_t *child;

  if (!d || !name)
    return;
  child = vfs_finddir(d, (char *)name);
  if (!child)
    return;
  if ((child->flags & FS_TYPE_MASK) == FS_DIRECTORY) {
    sysfs_remove_children(child);
    vfs_rmdir(d, (char *)name);
  } else {
    sysfs_remove_file_child(d, child);
  }
}

int asc_sysfs_attr_file(void *dir, const char *name, unsigned int mode,
                        void *ctx,
                        int (*show)(void *ctx, char *buf, unsigned int size),
                        int (*store)(void *ctx, const char *buf,
                                     unsigned int size),
                        void (*release)(void *ctx)) {
  vfs_node_t *d = dir;
  struct sysfs_dyn_attr *attr;
  vfs_node_t *file;

  if (!d || !name || !*name)
    return -22; /* EINVAL */
  if (vfs_finddir(d, (char *)name))
    return -17; /* EEXIST */

  attr = kmalloc(sizeof(*attr));
  file = kmalloc(sizeof(*file));
  if (!attr || !file) {
    kfree(attr);
    kfree(file);
    return -12; /* ENOMEM */
  }
  attr->ctx = ctx;
  attr->show = show;
  attr->store = store;
  attr->release = release;

  vfs_node_init(file);
  strncpy(file->name, name, sizeof(file->name) - 1);
  file->name[sizeof(file->name) - 1] = '\0';
  file->flags = FS_FILE | FS_PERSISTENT;
  file->mask = (uint16_t)mode;
  file->impl = SYSFS_DYN_ATTR_MAGIC;
  file->device = attr;
  file->read = sysfs_dyn_attr_read;
  file->write = sysfs_dyn_attr_write;

  ramfs_mount_node(d, file);
  vfs_dentry_invalidate(d, (char *)name);
  return 0;
}

int asc_sysfs_bin_file(void *dir, const char *name, unsigned int mode,
                       void *ctx,
                       int (*read)(void *ctx, unsigned int offset, char *buf,
                                   unsigned int size),
                       int (*write)(void *ctx, unsigned int offset,
                                    const char *buf, unsigned int size),
                       void (*release)(void *ctx)) {
  vfs_node_t *d = dir;
  struct sysfs_dyn_bin *bin;
  vfs_node_t *file;

  if (!d || !name || !*name)
    return -22; /* EINVAL */
  if (vfs_finddir(d, (char *)name))
    return -17; /* EEXIST */

  bin = kmalloc(sizeof(*bin));
  file = kmalloc(sizeof(*file));
  if (!bin || !file) {
    kfree(bin);
    kfree(file);
    return -12; /* ENOMEM */
  }
  bin->ctx = ctx;
  bin->read = read;
  bin->write = write;
  bin->release = release;

  vfs_node_init(file);
  strncpy(file->name, name, sizeof(file->name) - 1);
  file->name[sizeof(file->name) - 1] = '\0';
  file->flags = FS_FILE | FS_PERSISTENT;
  file->mask = (uint16_t)mode;
  file->impl = SYSFS_DYN_BIN_MAGIC;
  file->device = bin;
  file->read = sysfs_dyn_bin_read;
  file->write = sysfs_dyn_bin_write;

  ramfs_mount_node(d, file);
  vfs_dentry_invalidate(d, (char *)name);
  return 0;
}

void *asc_sysfs_dir_by_path(const char *path) {
  vfs_node_t *node;

  if (!path)
    return NULL;
  node = vfs_resolve_path(path);
  if (!node)
    return NULL;
  if ((node->flags & FS_TYPE_MASK) != FS_DIRECTORY) {
    vfs_close(node);
    return NULL;
  }
  /* Match asc_sysfs_class_dir(): return the tree pointer, not the extra
   * lookup reference.  Sysfs directories live for the kernel's lifetime. */
  vfs_close(node);
  return node;
}
