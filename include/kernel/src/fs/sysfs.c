#include "fs/sysfs.h"
#include "fs/sysfs_pci.h"
#include "console/klog.h"
#include "drivers/pci/pci.h"
#include "drivers/gpu/virtio_gpu/virtio_gpu.h"
#include "drivers/storage/block.h"
#include "fs/ramfs.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "net/core.h"
#include "net/ipv4.h"
#include "net/ipv6.h"
#include "smp/cpu.h"

// GPU device path for netlink uevents
char sysfs_gpu_devpath[128] = "/devices/pci0000:00/0000:00:01.0/drm/card0";
char sysfs_gpu_connector_devpath[128] =
    "/devices/pci0000:00/0000:00:01.0/drm/card0/card0-HDMI-A-1";
static vfs_node_t *net_class_root;
static vfs_node_t *gpu_status_nodes[VIRTIO_GPU_MAX_SCANOUTS];
static vfs_node_t *gpu_enabled_nodes[VIRTIO_GPU_MAX_SCANOUTS];
static vfs_node_t *gpu_modes_nodes[VIRTIO_GPU_MAX_SCANOUTS];
static vfs_node_t *gpu_dpms_nodes[VIRTIO_GPU_MAX_SCANOUTS];

// Helpers

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

    // dev: major:minor  (8:N for SCSI/ATA)
    char devbuf[16];
    devbuf[0] = '8';
    devbuf[1] = ':';
    u64_to_dec(i, devbuf + 2);
    strcat(devbuf, "\n");
    sysfs_mkfile(bdir, "dev", devbuf);

    // size: total bytes
    char sbuf[32];
    uint64_t bytes =
        (uint64_t)bd->total_sectors * (bd->sector_size ? bd->sector_size : 512);
    u64_to_dec(bytes, sbuf);
    strcat(sbuf, "\n");
    sysfs_mkfile(bdir, "size", sbuf);

    sysfs_mkfile(bdir, "removable", "0\n");
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
  }
  // Also add a 'possible' and 'present' file at the cpu/ level
  char cpumask[16];
  strcpy(cpumask, "0-");
  u64_to_dec(count - 1, cpumask + 2);
  strcat(cpumask, "\n");
  sysfs_mkfile(cpu_dir, "possible", cpumask);
  sysfs_mkfile(cpu_dir, "present", cpumask);
}

// Main init

void sysfs_init(void) {
  if (!fs_root)
    return;

  // Find or create the /sys directory on the root filesystem
  vfs_node_t *sys_dir = vfs_finddir(fs_root, "sys");
  if (!sys_dir && fs_root->mkdir) {
    fs_root->mkdir(fs_root, "sys", 0755);
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
  vfs_mount(sys_dir, sysfs_root);
  vfs_close(sys_dir);

  // /sys/bus/pci/devices
  vfs_node_t *bus_dir = sysfs_mkdir(sysfs_root, "bus");
  vfs_node_t *pci_dir = sysfs_mkdir(bus_dir, "pci");

  // /sys/class
  vfs_node_t *class_dir = sysfs_mkdir(sysfs_root, "class");

  // /sys/devices
  // Must be created before class/drm so the symlink target dirs exist
  vfs_node_t *devices_dir = sysfs_mkdir(sysfs_root, "devices");
  sysfs_pci_init(pci_dir, devices_dir);
  vfs_node_t *system_dir = sysfs_mkdir(devices_dir, "system");
  vfs_node_t *cpu_dir = sysfs_mkdir(system_dir, "cpu");
  sysfs_populate_cpus(cpu_dir);

  // /sys/class/block
  vfs_node_t *block_class = sysfs_mkdir(class_dir, "block");
  sysfs_populate_block(block_class);

  // /sys/class/net
  net_class_root = sysfs_mkdir(class_dir, "net");

  // /sys/class/drm/card0
  // card0 is a symlink to the real device path (wlroots uses readlink on it)
  // Target: ../../devices/pci0000:00/0000:BB:SS.F/drm/card0
  // Find the display controller (PCI class 0x03)
  vfs_node_t *drm_class = sysfs_mkdir(class_dir, "drm");
  {
    char pci_addr[20] = "0000:00:01.0"; // fallback
    uint32_t pci_count = pci_get_device_count();
    for (uint32_t i = 0; i < pci_count; i++) {
      struct pci_device *pd = pci_get_device(i);
      if (pd && pd->class_code == 0x03) {
        char tmp[4];
        pci_addr[0] = '0';
        pci_addr[1] = '0';
        pci_addr[2] = '0';
        pci_addr[3] = '0';
        pci_addr[4] = ':';
        u32_to_hex(pd->bus, tmp, 2);
        pci_addr[5] = tmp[0];
        pci_addr[6] = tmp[1];
        pci_addr[7] = ':';
        u32_to_hex(pd->slot, tmp, 2);
        pci_addr[8] = tmp[0];
        pci_addr[9] = tmp[1];
        pci_addr[10] = '.';
        pci_addr[11] = '0' + (pd->func & 7);
        pci_addr[12] = '\0';
        break;
      }
    }
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
    // Keep the intermediate drm/ and pci0000:00/ directories as plain sysfs
    // containers. Giving either a uevent file makes libudev treat it as a
    // device, but neither has a subsystem; Xorg then dereferences a NULL
    // subsystem while walking from card0 to its PCI parent.
    char gpu_uevent[128],ue_vid[5],ue_did[5];uint32_t uvid=0,udid=0;for(uint32_t ui=0;ui<pci_get_device_count();ui++){struct pci_device *upd=pci_get_device(ui);if(upd&&upd->class_code==0x03){uvid=upd->vendor_id;udid=upd->device_id;break;}}u32_to_hex(uvid,ue_vid,4);u32_to_hex(udid,ue_did,4);strcpy(gpu_uevent,"DRIVER=virtio-pci\nPCI_ID=");strcat(gpu_uevent,ue_vid);strcat(gpu_uevent,":");strcat(gpu_uevent,ue_did);strcat(gpu_uevent,"\nSUBSYSTEM=pci\n");sysfs_mkfile(gpu_dev,"uevent",gpu_uevent);
    sysfs_symlink(gpu_dev, "subsystem", "../../../bus/pci");

    // Mesa reads vendor/device/class from /sys/dev/char/226:0/device/vendor
    // etc. The path resolves: 226:0 -> card0_dir, then "device" -> gpu_dev.
    // gpu_dev is a freshly created node (separate from sysfs_populate_pci's
    // node), so we must add vendor/device/class/irq files here explicitly.
    {
      uint32_t vid = 0x1234, did = 0x1111, cls = 0x030000;
      uint32_t pci_cnt = pci_get_device_count();
      for (uint32_t ii = 0; ii < pci_cnt; ii++) {
        struct pci_device *gpd = pci_get_device(ii);
        if (gpd && gpd->class_code == 0x03) {
          vid = gpd->vendor_id;
          did = gpd->device_id;
          cls = ((uint32_t)gpd->class_code << 16) |
                ((uint32_t)gpd->subclass << 8) | gpd->prog_if;
          break;
        }
      }
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

  // /sys/dev/block  /sys/dev/char
  vfs_node_t *dev_dir = sysfs_mkdir(sysfs_root, "dev");
  sysfs_mkdir(dev_dir, "block");
  vfs_node_t *char_dir = sysfs_mkdir(dev_dir, "char");
  {
    // Find the GPU PCI address (same logic as above)
    char pci_addr2[20] = "0000:00:01.0";
    uint32_t pci_count2 = pci_get_device_count();
    for (uint32_t i = 0; i < pci_count2; i++) {
      struct pci_device *pd = pci_get_device(i);
      if (pd && pd->class_code == 0x03) {
        char tmp2[4];
        pci_addr2[0] = '0';
        pci_addr2[1] = '0';
        pci_addr2[2] = '0';
        pci_addr2[3] = '0';
        pci_addr2[4] = ':';
        u32_to_hex(pd->bus, tmp2, 2);
        pci_addr2[5] = tmp2[0];
        pci_addr2[6] = tmp2[1];
        pci_addr2[7] = ':';
        u32_to_hex(pd->slot, tmp2, 2);
        pci_addr2[8] = tmp2[0];
        pci_addr2[9] = tmp2[1];
        pci_addr2[10] = '.';
        pci_addr2[11] = '0' + (pd->func & 7);
        pci_addr2[12] = '\0';
        break;
      }
    }
    // /sys/dev/char/226:0 -> symlink to the PCI device directory
    // Relative from /sys/dev/char/226:0/ :
    // ../../devices/pci0000:00/<addr>/drm/card0
    char dev_target[128];
    strcpy(dev_target, "../../devices/pci0000:00/");
    strcat(dev_target, pci_addr2);
    strcat(dev_target, "/drm/card0");
    sysfs_symlink(char_dir, "226:0", dev_target);
  }

  // Input char devices: /sys/dev/char/13:64 -> symlink to virtual device
  // Relative from /sys/dev/char/13:64 :
  // ../../devices/virtual/input/input0/event0
  sysfs_symlink(char_dir, "13:64", "../../devices/virtual/input/input0/event0");

  // Input char devices: /sys/dev/char/13:65 -> symlink to virtual device
  // Relative from /sys/dev/char/13:65 :
  // ../../devices/virtual/input/input1/event1
  sysfs_symlink(char_dir, "13:65", "../../devices/virtual/input/input1/event1");

  // /sys/subsystem
  // libinput and others expect this to exist for device discovery.
  // Modern Linux has /sys/subsystem/ where entries are symlinks to bus or
  // class.
  vfs_node_t *subsystem_dir = sysfs_mkdir(sysfs_root, "subsystem");
  if (subsystem_dir) {
    sysfs_symlink(subsystem_dir, "pci", "../bus/pci");
    sysfs_symlink(subsystem_dir, "input", "../class/input");
    sysfs_symlink(subsystem_dir, "drm", "../class/drm");
  }

  // /sys/kernel
  vfs_node_t *kernel_dir = sysfs_mkdir(sysfs_root, "kernel");
  sysfs_mkfile(kernel_dir, "hostname", "ascentos\n");

  // /sys/power
  vfs_node_t *power_dir = sysfs_mkdir(sysfs_root, "power");
  sysfs_mkfile(power_dir, "state", "mem\n");

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
    }

    // Mark system as container-like environment so WebKitGTK and sandbox tools
    // detect that unprivileged user namespaces are not available and use their
    // native non-sandboxed process execution path automatically.
    sysfs_mkfile(run_dir, ".containerenv", "engine=avoryos\n");

    if (data_dir) vfs_close(data_dir);
    if (udev_dir) vfs_close(udev_dir);
    vfs_close(run_dir);
  }

  klog_puts("[OK] SysFS initialized at /sys\n");
}
