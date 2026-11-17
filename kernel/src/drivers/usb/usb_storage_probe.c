/* Boot-time Bulk-Only Transport self-test (opt-in: usb_storage_probe=1).
 *
 * Runs on any device whose configuration has an interface of class 08,
 * subclass 06 (SCSI transparent), protocol 50 (BBB).  It drives raw CBW/CSW
 * traffic through the usb_bulk_* API, with no SCSI class driver involved:
 *   1. INQUIRY into a 255-byte buffer: short IN packet is success.
 *   2. READ(10) of 256 sectors (128 KiB, crosses 64 KiB windows), twice, then
 *      300 single-sector reads to wrap the bulk ring many times.
 *   3. A 30-byte CBW: the device stalls; BOMSR, clear-halt IN and OUT, then
 *      TEST UNIT READY must succeed.
 *   4. A bulk IN with nothing pending and a 200 ms timeout: must time out,
 *      then TEST UNIT READY must succeed.
 * Any other device class is ignored. */
#include "usb.h"
#include "../../console/klog.h"
#include "../../lib/string.h"
#include "../../mm/dma_alloc.h"

#define ST_CBW_SIG 0x43425355U
#define ST_CSW_SIG 0x53425355U
#define ST_CBW_LEN 31U
#define ST_CSW_LEN 13U
#define ST_CSW_OFFSET 64U /* CSW sits in the command page after the CBW */
#define ST_DATA_BYTES (128U * 1024U)
#define ST_SECTOR 512U
#define ST_RING_LOOP 300U
#define ST_CMD_TIMEOUT_MS 1000U
#define ST_DATA_TIMEOUT_MS 2000U

struct st_ep {
  uint8_t addr;
  uint16_t mps;
  uint8_t burst;
};

struct st_ctx {
  struct usb_device *dev;
  uint8_t iface;
  struct usb_bulk_pipe *in;
  struct usb_bulk_pipe *out;
  dma_buffer_t *cmd;  /* CBW at offset 0, CSW at ST_CSW_OFFSET */
  dma_buffer_t *data; /* ST_DATA_BYTES */
  uint32_t tag;
  uint32_t last_tag;
  uint8_t status;
  int last_rc;
  int failed_step;
  const char *detail;
};

static uint32_t st_get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void st_put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static void st_log_int(int v) {
  if (v < 0) {
    klog_puts("-");
    klog_uint64((uint64_t)(-(int64_t)v));
  } else {
    klog_uint64((uint64_t)v);
  }
}

static bool st_fail(struct st_ctx *c, int step, const char *detail) {
  c->failed_step = step;
  c->detail = detail;
  return false;
}

static bool usb_storage_probe_requested(void) {
  extern const char *kernel_boot_cmdline;
  if (!kernel_boot_cmdline)
    return false;
  const char *p = kernel_boot_cmdline;
  const char *option = "usb_storage_probe=1";
  size_t option_len = strlen(option);
  while ((p = strstr(p, option)) != NULL) {
    if ((p == kernel_boot_cmdline || p[-1] == ' ') &&
        (p[option_len] == '\0' || p[option_len] == ' '))
      return true;
    p += option_len;
  }
  return false;
}

/* Finds the first alternate-0 BBB interface with one bulk IN and one bulk OUT
 * endpoint.  Returns false for anything else, which the caller ignores. */
static bool st_find_bot(struct usb_device *dev, uint8_t *iface_out,
                        uint8_t *cfg_out, struct st_ep *in, struct st_ep *out) {
  struct usb_control_request req = {
      .request_type = 0x80,
      .request = USB_REQ_GET_DESCRIPTOR,
      .value = (uint16_t)(USB_DESC_CONFIGURATION << 8),
      .index = 0,
      .length = 9,
  };
  uint8_t head[9];
  if (usb_control_transfer(dev, &req, head, 9) < 0)
    return false;
  uint16_t total = (uint16_t)(head[2] | (head[3] << 8));
  if (total < 9 || total > 512)
    return false;
  static uint8_t cfg[512];
  req.length = total;
  if (usb_control_transfer(dev, &req, cfg, total) < 0)
    return false;

  bool found = false, have_in = false, have_out = false;
  bool in_bot = false;
  struct st_ep *cur = NULL;
  uint8_t iface = 0;
  for (uint16_t p = 0; p + 2 <= total;) {
    uint8_t len = cfg[p];
    uint8_t type = cfg[p + 1];
    if (len < 2 || p + len > total)
      break;
    if (type == USB_DESC_INTERFACE && len >= 9) {
      in_bot = !found && cfg[p + 3] == 0 && cfg[p + 5] == 0x08 &&
               cfg[p + 6] == 0x06 && cfg[p + 7] == 0x50;
      if (in_bot) {
        iface = cfg[p + 2];
        found = true;
      }
      cur = NULL;
    } else if (type == USB_DESC_ENDPOINT && len >= 6 && in_bot) {
      uint8_t addr = cfg[p + 2];
      if ((cfg[p + 3] & 3U) == 2U) {
        bool is_in = (addr & 0x80U) != 0;
        cur = is_in ? in : out;
        if (is_in ? have_in : have_out) {
          cur = NULL; /* duplicate bulk endpoint: keep the first */
        } else {
          cur->addr = addr;
          cur->mps = (uint16_t)(cfg[p + 4] | (cfg[p + 5] << 8));
          cur->burst = 0;
          if (is_in)
            have_in = true;
          else
            have_out = true;
        }
      } else {
        cur = NULL;
      }
    } else if (type == 0x30 && len >= 6 && cur) {
      cur->burst = cfg[p + 2]; /* SuperSpeed companion: bMaxBurst */
    }
    p = (uint16_t)(p + len);
  }
  if (!found || !have_in || !have_out)
    return false;
  *iface_out = iface;
  *cfg_out = cfg[5];
  return true;
}

static int st_send_cbw(struct st_ctx *c, uint32_t xfer, bool in,
                       const uint8_t *cdb, uint8_t cdb_len, uint32_t send_len) {
  uint8_t *cbw = c->cmd->virt;
  memset(cbw, 0, ST_CSW_OFFSET);
  c->tag++;
  c->last_tag = c->tag;
  st_put32(cbw, ST_CBW_SIG);
  st_put32(cbw + 4, c->tag);
  st_put32(cbw + 8, xfer);
  cbw[12] = in ? 0x80 : 0x00;
  cbw[13] = 0;
  cbw[14] = cdb_len;
  memcpy(cbw + 15, cdb, cdb_len);
  uint32_t actual = 0;
  c->last_rc = usb_bulk_transfer(c->out, cbw, c->cmd->phys, send_len, &actual,
                                 ST_CMD_TIMEOUT_MS);
  return c->last_rc;
}

static int st_read_csw(struct st_ctx *c) {
  uint8_t *csw = (uint8_t *)c->cmd->virt + ST_CSW_OFFSET;
  uint64_t csw_phys = c->cmd->phys + ST_CSW_OFFSET;
  uint32_t actual = 0;
  memset(csw, 0, ST_CSW_LEN);
  c->last_rc = usb_bulk_transfer(c->in, csw, csw_phys, ST_CSW_LEN, &actual,
                                 ST_CMD_TIMEOUT_MS);
  if (c->last_rc != USB_BULK_OK)
    return c->last_rc;
  if (actual != ST_CSW_LEN || st_get32(csw) != ST_CSW_SIG ||
      st_get32(csw + 4) != c->last_tag) {
    c->last_rc = USB_BULK_ERR;
    return c->last_rc;
  }
  c->status = csw[12];
  return USB_BULK_OK;
}

/* One complete command: CBW, optional IN data phase, CSW.  *actual_out gets
 * the bytes returned by the data phase. */
static int st_command(struct st_ctx *c, uint32_t xfer, bool in,
                      const uint8_t *cdb, uint8_t cdb_len,
                      uint32_t *actual_out) {
  if (actual_out)
    *actual_out = 0;
  int r = st_send_cbw(c, xfer, in, cdb, cdb_len, ST_CBW_LEN);
  if (r != USB_BULK_OK)
    return r;
  if (xfer) {
    uint32_t actual = 0;
    r = usb_bulk_transfer(in ? c->in : c->out, c->data->virt, c->data->phys,
                          xfer, &actual, ST_DATA_TIMEOUT_MS);
    c->last_rc = r;
    if (r != USB_BULK_OK)
      return r;
    if (actual_out)
      *actual_out = actual;
  }
  return st_read_csw(c);
}

static int st_read10(struct st_ctx *c, uint32_t lba, uint16_t blocks,
                     uint32_t *actual) {
  uint8_t cdb[10] = {
      0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
      (uint8_t)(lba >> 8), (uint8_t)lba, 0, (uint8_t)(blocks >> 8),
      (uint8_t)blocks, 0};
  return st_command(c, (uint32_t)blocks * ST_SECTOR, true, cdb, 10, actual);
}

/* TEST UNIT READY.  A check condition (e.g. unit attention after a BOT reset)
 * is cleared with REQUEST SENSE and the test is repeated. */
static int st_test_unit_ready(struct st_ctx *c) {
  static const uint8_t tur[6] = {0};
  static const uint8_t req_sense[6] = {0x03, 0, 0, 0, 18, 0};
  for (int attempt = 0; attempt < 3; attempt++) {
    int r = st_command(c, 0, true, tur, 6, NULL);
    if (r != USB_BULK_OK)
      return r;
    if (c->status == 0)
      return USB_BULK_OK;
    r = st_command(c, 18, true, req_sense, 6, NULL);
    if (r != USB_BULK_OK)
      return r;
  }
  c->last_rc = USB_BULK_ERR;
  return USB_BULK_ERR;
}

static uint32_t st_checksum(const uint8_t *p, uint32_t len) {
  uint32_t sum = 0;
  for (uint32_t i = 0; i < len; i++)
    sum = sum * 31U + p[i];
  return sum;
}

static bool st_run(struct st_ctx *c) {
  static const uint8_t inquiry[6] = {0x12, 0, 0, 0, 36, 0};
  static const uint8_t tur[6] = {0};
  uint32_t actual = 0;
  int r;

  /* 1. INQUIRY: 255-byte IN, device returns 36.  Short packet is success. */
  r = st_command(c, 255, true, inquiry, 6, &actual);
  if (r != USB_BULK_OK)
    return st_fail(c, 1, "INQUIRY transfer");
  /* BOT case Hi > Di: the device may end with a short packet or pad with
   * fill data up to the host length, so anything from 36..255 is valid. */
  klog_puts("[USB-STORAGE-PROBE] INQUIRY actual=");
  klog_uint64(actual);
  klog_puts(" csw_status=");
  klog_uint64(c->status);
  klog_puts("\n");
  if (actual < 36 || actual > 255 || c->status != 0)
    return st_fail(c, 1, "INQUIRY short read or CSW status");

  /* 2. READ(10) 128 KiB twice (contents must match), then wrap the ring. */
  r = st_read10(c, 0, 256, &actual);
  if (r != USB_BULK_OK || actual != ST_DATA_BYTES || c->status != 0)
    return st_fail(c, 2, "READ(10) 256 sectors");
  uint32_t sum1 = st_checksum(c->data->virt, ST_DATA_BYTES);
  r = st_read10(c, 0, 256, &actual);
  if (r != USB_BULK_OK || actual != ST_DATA_BYTES || c->status != 0)
    return st_fail(c, 2, "READ(10) 256 sectors, second pass");
  if (st_checksum(c->data->virt, ST_DATA_BYTES) != sum1)
    return st_fail(c, 2, "READ(10) data differs between passes");
  for (uint32_t i = 0; i < ST_RING_LOOP; i++) {
    r = st_read10(c, 0, 1, &actual);
    if (r != USB_BULK_OK || actual != ST_SECTOR || c->status != 0)
      return st_fail(c, 2, "single-sector READ(10) during ring wrap");
  }

  /* 3. Invalid 30-byte CBW.  The device stalls the next bulk operation. */
  r = st_send_cbw(c, 0, true, tur, 6, 30);
  if (r != USB_BULK_OK && r != USB_BULK_STALL)
    return st_fail(c, 3, "30-byte CBW transfer error");
  if (r == USB_BULK_OK) {
    if (st_read_csw(c) != USB_BULK_STALL)
      return st_fail(c, 3, "expected STALL after invalid CBW");
  }
  /* BOT reset recovery: BOMSR, then clear-halt IN, then clear-halt OUT. */
  struct usb_control_request bomsr = {
      .request_type = 0x21,
      .request = 0xFF,
      .value = 0,
      .index = c->iface,
      .length = 0,
  };
  if (usb_control_transfer(c->dev, &bomsr, NULL, 0) < 0)
    return st_fail(c, 3, "Bulk-Only Reset request");
  if (usb_clear_halt(c->in) != USB_BULK_OK)
    return st_fail(c, 3, "clear halt on IN");
  if (usb_clear_halt(c->out) != USB_BULK_OK)
    return st_fail(c, 3, "clear halt on OUT");
  if (st_test_unit_ready(c) != USB_BULK_OK)
    return st_fail(c, 3, "TEST UNIT READY after reset");

  /* 4. Bulk IN with no command pending: must time out, then recover. */
  r = usb_bulk_transfer(c->in, c->data->virt, c->data->phys, ST_CSW_LEN,
                        &actual, 200);
  c->last_rc = r;
  /* Real devices NAK here (timeout); QEMU usb-msd STALLs an IN token while
   * waiting for a CBW.  Either must leave the pipe recoverable. */
  klog_puts(r == USB_BULK_TIMEOUT ? "[USB-STORAGE-PROBE] idle IN: timeout\n"
                                  : "[USB-STORAGE-PROBE] idle IN: stall\n");
  if (r == USB_BULK_STALL) {
    if (usb_clear_halt(c->in) != USB_BULK_OK)
      return st_fail(c, 4, "clear halt on IN after idle stall");
  } else if (r != USB_BULK_TIMEOUT) {
    return st_fail(c, 4, "expected timeout or stall on idle bulk IN");
  }
  if (st_test_unit_ready(c) != USB_BULK_OK)
    return st_fail(c, 4, "TEST UNIT READY after timeout");
  return true;
}

void usb_storage_probe_smoke(struct usb_device *dev) {
  if (!usb_storage_probe_requested() || !dev || !dev->connected)
    return;
  struct st_ep in = {0}, out = {0};
  uint8_t iface = 0, cfg_value = 0;
  if (!st_find_bot(dev, &iface, &cfg_value, &in, &out))
    return;

  klog_puts("[USB-STORAGE-PROBE] BBB interface found; running bulk self-test\n");
  struct usb_control_request set_cfg = {
      .request_type = 0x00,
      .request = USB_REQ_SET_CONFIGURATION,
      .value = cfg_value,
      .index = 0,
      .length = 0,
  };
  if (usb_control_transfer(dev, &set_cfg, NULL, 0) < 0) {
    klog_puts("USB bulk selftest: FAIL step 0 SET_CONFIGURATION\n");
    return;
  }
  dev->configured = true;
  dev->configuration_value = cfg_value;

  struct st_ctx c = {0};
  c.dev = dev;
  c.iface = iface;
  c.in = usb_bulk_open(dev, in.addr, in.mps, in.burst);
  c.out = usb_bulk_open(dev, out.addr, out.mps, out.burst);
  c.cmd = dma_alloc(ST_CSW_OFFSET + ST_CSW_LEN, DMA_FLAG_32BIT);
  c.data = dma_alloc(ST_DATA_BYTES, DMA_FLAG_32BIT);
  if (!c.in || !c.out || !c.cmd || !c.data) {
    klog_puts("USB bulk selftest: FAIL step 0 open or DMA allocation\n");
  } else if (st_run(&c)) {
    klog_puts("USB bulk selftest: PASS\n");
  } else {
    klog_puts("USB bulk selftest: FAIL step ");
    st_log_int(c.failed_step);
    klog_puts(" ");
    klog_puts(c.detail);
    klog_puts(" rc=");
    st_log_int(c.last_rc);
    klog_puts("\n");
  }

  usb_bulk_close(c.in);
  usb_bulk_close(c.out);
  if (c.cmd)
    dma_free(c.cmd);
  if (c.data)
    dma_free(c.data);
}
