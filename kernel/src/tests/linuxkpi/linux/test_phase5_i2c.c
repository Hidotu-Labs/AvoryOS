/* Phase 5 C4 — minimal I2C core (DDC/EDID) self-test.
 *
 * The core is self-authored (no i2c-core-base.c import; see
 * docs/linuxkpi-gaps.md, P5 C4).  This suite covers:
 *
 *   1. adapter registration (dynamic + numbered), lookup and verify,
 *   2. message roundtrip through a scripted backend: address/flag/length
 *      plumbing, I2C_M_RD, i2c_master_send/recv, zero-length messages,
 *   3. retry semantics: -EAGAIN re-runs the transfer up to adap->retries,
 *   4. serialization: two threads transfer through one adapter and the
 *      backend never sees overlapping calls,
 *   5. lifecycle: ~1k add/del cycles with id reuse and a PMM baseline,
 *   6. the exact DDC transfer sequence drm_edid.c uses (2 messages for the
 *      base block, 3 with the DDC segment write for block 2), with a
 *      synthetic 128-byte EDID validated by the imported
 *      drm_edid_block_valid().
 *
 * The full connector-level drm_edid_read_ddc() path needs the display
 * plumbing and stays P6e evidence; C4 proves the message semantics it rides
 * on. */

#include <drm/drm_edid.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/kthread.h>
#include <linux/sprintf.h>
#include <linux/string.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native_mm.h>

#define P5C4_ADAPTER_CYCLES 1000
#define P5C4_THREAD_XFERS 50
#define P5C4_GENERAL_ADDR 0x18
#define P5C4_RD_PATTERN 0xA0
/* E-DDC segment pointer (drm_edid.c defines the same value locally). */
#define P5C4_DDC_SEG_ADDR 0x30

struct p5c4_fake {
  struct i2c_adapter adap;
  int xfers;       /* master_xfer calls that reached the backend   */
  int msgs;        /* messages accepted                            */
  int fail_eagain; /* the next N calls return -EAGAIN              */
  int delay_ms;    /* backend service time (serialization window)  */
  volatile int in_flight; /* transfers inside master_xfer           */
  volatile int overlap;   /* set if a second transfer is seen       */
  volatile int max_in_flight;
  u16 last_addr;
  u16 last_flags;
  u16 last_len;
  u8 last_byte;
};

struct p5c4_ddc {
  struct i2c_adapter adap;
  u8 edid[EDID_LENGTH];
  u8 offset;
  int xfers;
  int reads;
  int segments;
  int bad_addr;
};

struct p5c4_thread_arg {
  struct i2c_adapter *adap;
  struct completion done;
  volatile int xfers;
  volatile int ok;
};

static int p5c4_failures;
static struct p5c4_fake p5c4_general;
static struct p5c4_fake p5c4_numbered;
static struct p5c4_ddc p5c4_ddc;

static void p5c4_ok(const char *what) {
  klogf("[  OK  ] LinuxKPI: i2c %s\n", what);
}

static void p5c4_fail(const char *what, long v) {
  p5c4_failures++;
  klogf("[FAIL] LinuxKPI: i2c %s (%ld)\n", what, v);
}

/* ── scripted backends ──────────────────────────────────────────────────── */

static u32 p5c4_func(struct i2c_adapter *adap) {
  (void)adap;
  return I2C_FUNC_I2C;
}

static int p5c4_general_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
                             int num) {
  struct p5c4_fake *f = i2c_get_adapdata(adap);
  int i, j;

  f->xfers++;
  if (f->fail_eagain > 0) {
    f->fail_eagain--;
    return -EAGAIN;
  }

  if (f->in_flight)
    f->overlap = 1;
  f->in_flight++;
  if (f->in_flight > f->max_in_flight)
    f->max_in_flight = f->in_flight;

  for (i = 0; i < num; i++) {
    struct i2c_msg *m = &msgs[i];

    f->last_addr = m->addr;
    f->last_flags = m->flags;
    f->last_len = m->len;
    if (m->len > 0 && !m->buf) {
      f->in_flight--;
      return -EINVAL;
    }
    if (m->flags & I2C_M_RD) {
      for (j = 0; j < m->len; j++)
        m->buf[j] = (u8)(P5C4_RD_PATTERN + j);
    } else if (m->len > 0) {
      f->last_byte = m->buf[m->len - 1];
    }
  }

  if (f->delay_ms)
    msleep(f->delay_ms);
  f->msgs += num;
  f->in_flight--;
  return num;
}

static int p5c4_ddc_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
                         int num) {
  struct p5c4_ddc *d = i2c_get_adapdata(adap);
  int i, j;

  d->xfers++;
  for (i = 0; i < num; i++) {
    struct i2c_msg *m = &msgs[i];

    /* E-DDC segment pointer (only for extension blocks). */
    if (m->addr == P5C4_DDC_SEG_ADDR && !(m->flags & I2C_M_RD)) {
      d->segments++;
      continue;
    }
    if (m->addr != DDC_ADDR) {
      d->bad_addr = 1;
      return -ENXIO;
    }
    if (m->flags & I2C_M_RD) {
      d->reads++;
      if (!m->buf)
        return -EINVAL;
      for (j = 0; j < m->len; j++)
        m->buf[j] = d->edid[(d->offset + j) % EDID_LENGTH];
    } else {
      if (m->len != 1 || !m->buf)
        return -EINVAL;
      d->offset = m->buf[0];
    }
  }
  return num;
}

static const struct i2c_algorithm p5c4_general_algo = {
    .master_xfer = p5c4_general_xfer,
    .functionality = p5c4_func,
};

static const struct i2c_algorithm p5c4_ddc_algo = {
    .master_xfer = p5c4_ddc_xfer,
    .functionality = p5c4_func,
};

static void p5c4_init_adap(struct i2c_adapter *a, const char *name,
                           const struct i2c_algorithm *algo, void *data) {
  memset(a, 0, sizeof(*a));
  a->class = I2C_CLASS_DDC;
  a->algo = algo;
  a->nr = -1;
  snprintf(a->name, sizeof(a->name), "%s", name);
  i2c_set_adapdata(a, data);
}

/* ── serialization threads ──────────────────────────────────────────────── */

static int p5c4_thread(void *arg) {
  struct p5c4_thread_arg *t = arg;
  int i;

  for (i = 0; i < P5C4_THREAD_XFERS; i++) {
    u8 w = (u8)i, r = 0;
    struct i2c_msg msgs[2] = {
        {.addr = P5C4_GENERAL_ADDR, .flags = 0, .len = 1, .buf = &w},
        {.addr = P5C4_GENERAL_ADDR, .flags = I2C_M_RD, .len = 1, .buf = &r},
    };

    if (i2c_transfer(t->adap, msgs, 2) != 2 ||
        r != (u8)P5C4_RD_PATTERN) {
      t->ok = 0;
      break;
    }
    t->xfers++;
  }
  complete(&t->done);
  return 0;
}

/* ── EDID construction ──────────────────────────────────────────────────── */

static void p5c4_build_edid(u8 *e) {
  struct edid *edid = (struct edid *)e;
  u32 sum = 0;
  int i;

  memset(e, 0, EDID_LENGTH);
  edid->header[0] = 0x00;
  memset(&edid->header[1], 0xff, 6);
  edid->header[7] = 0x00;
  edid->mfg_id[0] = 0x04;
  edid->mfg_id[1] = 0x21;
  edid->prod_code[0] = 0x34;
  edid->prod_code[1] = 0x12;
  edid->version = 1;
  edid->revision = 4;
  edid->input = 0x80; /* digital */
  edid->width_cm = 16;
  edid->height_cm = 10;
  edid->gamma = 120;
  edid->extensions = 1;

  for (i = 0; i < EDID_LENGTH - 1; i++)
    sum += e[i];
  e[EDID_LENGTH - 1] = (u8)(0 - sum);
}

/* ── suite ──────────────────────────────────────────────────────────────── */

void linuxkpi_test_phase5_i2c(void) {
  struct i2c_adapter *found;
  struct i2c_client client;
  struct p5c4_thread_arg t1, t2;
  struct task_struct *k1, *k2;
  u8 wbuf[3] = {0xde, 0xad, 0x01};
  u8 rbuf[8];
  int ret, first_id = -1;
  int numbered_nr;
  int i;

  p5c4_failures = 0;
  memset(&p5c4_general, 0, sizeof(p5c4_general));
  memset(&p5c4_numbered, 0, sizeof(p5c4_numbered));
  memset(&p5c4_ddc, 0, sizeof(p5c4_ddc));
  memset(rbuf, 0, sizeof(rbuf));
  klog_puts("[LINUXKPI] Phase 5 i2c self-test\n");

  /* Pick a numbered bus no other adapter has taken.  With CONFIG_DRM_AMD_DC
   * live the boot has already registered the amdgpu DM DDC buses, so a fixed
   * number (the old P5C4_NUMBERED_NR = 7) can collide and fail the suite. */
  numbered_nr = -1;
  for (i = 16; i < 64; i++) {
    if (!i2c_get_adapter(i)) {
      numbered_nr = i;
      break;
    }
  }
  if (numbered_nr < 0) {
    p5c4_fail("no free numbered i2c bus", 0);
    return;
  }

  /* 1: dynamic and numbered registration, lookup, verify. */
  p5c4_init_adap(&p5c4_general.adap, "p5c4-general", &p5c4_general_algo,
                 &p5c4_general);
  ret = i2c_add_adapter(&p5c4_general.adap);
  if (ret == 0 && p5c4_general.adap.nr >= 0)
    p5c4_ok("i2c_add_adapter gives a bus number");
  else
    p5c4_fail("i2c_add_adapter", ret);

  p5c4_init_adap(&p5c4_numbered.adap, "p5c4-numbered", &p5c4_general_algo,
                 &p5c4_numbered);
  p5c4_numbered.adap.nr = numbered_nr;
  ret = i2c_add_numbered_adapter(&p5c4_numbered.adap);
  if (ret == 0 && p5c4_numbered.adap.nr == numbered_nr)
    p5c4_ok("i2c_add_numbered_adapter keeps the requested bus number");
  else
    p5c4_fail("i2c_add_numbered_adapter", ret);

  {
    struct p5c4_fake clash;
    int clash_ret;

    memset(&clash, 0, sizeof(clash));
    p5c4_init_adap(&clash.adap, "p5c4-clash", &p5c4_general_algo, &clash);
    clash.adap.nr = numbered_nr;
    clash_ret = i2c_add_numbered_adapter(&clash.adap);
    if (clash_ret == -EBUSY)
      p5c4_ok("numbered clash fails with -EBUSY");
    else
      p5c4_fail("numbered clash", clash_ret);
  }

  {
    struct p5c4_fake bad;
    int bad_ret;

    memset(&bad, 0, sizeof(bad));
    p5c4_init_adap(&bad.adap, "p5c4-bad", &p5c4_general_algo, &bad);
    bad.adap.nr = -1;
    bad_ret = i2c_add_numbered_adapter(&bad.adap);
    if (bad_ret == -EINVAL)
      p5c4_ok("numbered adapter without a number fails with -EINVAL");
    else
      p5c4_fail("numbered without number", bad_ret);
  }

  found = i2c_get_adapter(numbered_nr);
  if (found == &p5c4_numbered.adap)
    p5c4_ok("i2c_get_adapter finds the registered bus");
  else
    p5c4_fail("i2c_get_adapter", (long)(found != NULL));
  i2c_put_adapter(found);

  if (i2c_get_adapter(1000) == NULL)
    p5c4_ok("i2c_get_adapter rejects an unknown bus");
  else
    p5c4_fail("i2c_get_adapter unknown", 0);

  if (i2c_verify_adapter(&p5c4_numbered.adap.dev) == &p5c4_numbered.adap &&
      i2c_verify_adapter(NULL) == NULL) {
    struct device plain;

    memset(&plain, 0, sizeof(plain));
    if (i2c_verify_adapter(&plain) == NULL)
      p5c4_ok("i2c_verify_adapter checks the device type");
    else
      p5c4_fail("i2c_verify_adapter plain device", 0);
  } else {
    p5c4_fail("i2c_verify_adapter", 0);
  }

  if (i2c_check_functionality(&p5c4_general.adap, I2C_FUNC_I2C) &&
      i2c_get_functionality(&p5c4_general.adap) == I2C_FUNC_I2C)
    p5c4_ok("i2c_get_functionality / i2c_check_functionality");
  else
    p5c4_fail("adapter functionality", 0);

  /* 2: message roundtrip through the scripted backend. */
  {
    struct i2c_msg msgs[2] = {
        {.addr = P5C4_GENERAL_ADDR, .flags = 0, .len = 3, .buf = wbuf},
        {.addr = P5C4_GENERAL_ADDR,
         .flags = I2C_M_RD,
         .len = 4,
         .buf = rbuf},
    };

    ret = i2c_transfer(&p5c4_general.adap, msgs, 2);
    if (ret == 2 && p5c4_general.last_addr == P5C4_GENERAL_ADDR &&
        p5c4_general.last_flags == I2C_M_RD && p5c4_general.last_len == 4 &&
        rbuf[0] == P5C4_RD_PATTERN && rbuf[3] == P5C4_RD_PATTERN + 3)
      p5c4_ok("write+read roundtrip with address/flag plumbing");
    else
      p5c4_fail("roundtrip", ret);
  }

  {
    struct i2c_msg zero = {.addr = P5C4_GENERAL_ADDR, .flags = 0, .len = 0};

    ret = i2c_transfer(&p5c4_general.adap, &zero, 1);
    if (ret == 1 && p5c4_general.last_len == 0)
      p5c4_ok("zero-length message passes through");
    else
      p5c4_fail("zero-length message", ret);
  }

  memset(&client, 0, sizeof(client));
  client.addr = P5C4_GENERAL_ADDR;
  client.adapter = &p5c4_general.adap;
  ret = i2c_master_send(&client, "AB", 2);
  if (ret == 2 && p5c4_general.last_len == 2 && p5c4_general.last_byte == 'B')
    p5c4_ok("i2c_master_send goes through i2c_transfer");
  else
    p5c4_fail("i2c_master_send", ret);

  ret = i2c_master_recv(&client, (char *)rbuf, 3);
  if (ret == 3 && rbuf[0] == P5C4_RD_PATTERN && rbuf[2] == P5C4_RD_PATTERN + 2)
    p5c4_ok("i2c_master_recv goes through i2c_transfer");
  else
    p5c4_fail("i2c_master_recv", ret);

  {
    struct i2c_adapter no_algo;
    struct i2c_msg no_algo_msg = {.addr = P5C4_GENERAL_ADDR,
                                  .flags = 0,
                                  .len = 1,
                                  .buf = wbuf};

    memset(&no_algo, 0, sizeof(no_algo));
    ret = i2c_transfer(&no_algo, &no_algo_msg, 1);
    if (ret == -EOPNOTSUPP)
      p5c4_ok("transfer without master_xfer fails with -EOPNOTSUPP");
    else
      p5c4_fail("no master_xfer", ret);
  }

  /* 3: retry semantics. */
  {
    struct i2c_msg msg = {.addr = P5C4_GENERAL_ADDR, .flags = 0, .len = 1,
                          .buf = wbuf};
    int before;

    p5c4_general.adap.retries = 1;
    p5c4_general.fail_eagain = 1;
    before = p5c4_general.xfers;
    ret = i2c_transfer(&p5c4_general.adap, &msg, 1);
    if (ret == 1 && p5c4_general.xfers - before == 2)
      p5c4_ok("retries once after -EAGAIN");
    else
      p5c4_fail("retry after -EAGAIN", ret);

    p5c4_general.fail_eagain = 2;
    before = p5c4_general.xfers;
    ret = i2c_transfer(&p5c4_general.adap, &msg, 1);
    if (ret == -EAGAIN && p5c4_general.xfers - before == 2)
      p5c4_ok("gives up after adap->retries attempts");
    else
      p5c4_fail("retries exhausted", ret);

    p5c4_general.fail_eagain = 0;
    p5c4_general.adap.retries = 0;
  }

  /* 4: serialization through the adapter lock. */
  memset(&t1, 0, sizeof(t1));
  memset(&t2, 0, sizeof(t2));
  t1.adap = &p5c4_general.adap;
  t2.adap = &p5c4_general.adap;
  t1.ok = t2.ok = 1;
  init_completion(&t1.done);
  init_completion(&t2.done);
  p5c4_general.delay_ms = 1;
  p5c4_general.overlap = 0;
  p5c4_general.max_in_flight = 0;
  k1 = kthread_run(p5c4_thread, &t1, "kpi/i2c1");
  k2 = kthread_run(p5c4_thread, &t2, "kpi/i2c2");
  if (IS_ERR(k1) || IS_ERR(k2)) {
    p5c4_fail("serialization kthreads", (long)(IS_ERR(k1) ? PTR_ERR(k1)
                                                          : PTR_ERR(k2)));
  } else {
    wait_for_completion_timeout(&t1.done, 5000);
    wait_for_completion_timeout(&t2.done, 5000);
    if (t1.ok && t2.ok && t1.xfers == P5C4_THREAD_XFERS &&
        t2.xfers == P5C4_THREAD_XFERS && p5c4_general.overlap == 0 &&
        p5c4_general.max_in_flight == 1)
      p5c4_ok("two threads serialize through one adapter");
    else
      p5c4_fail("serialization",
                (long)(t1.xfers + t2.xfers + p5c4_general.overlap * 100000));
    kthread_stop(k1);
    kthread_stop(k2);
  }
  p5c4_general.delay_ms = 0;

  /* 5: add/del lifecycle with id reuse and a PMM baseline. */
  {
    unsigned long pmm_before = asc_pmm_get_free_pages();

    for (i = 0; i < P5C4_ADAPTER_CYCLES; i++) {
      struct i2c_adapter *adap = kzalloc(sizeof(*adap), GFP_KERNEL);

      if (!adap) {
        p5c4_fail("lifecycle alloc", i);
        break;
      }
      adap->algo = &p5c4_general_algo;
      adap->nr = -1;
      ret = i2c_add_adapter(adap);
      if (ret != 0) {
        p5c4_fail("lifecycle add", ret);
        kfree(adap);
        break;
      }
      if (first_id < 0)
        first_id = adap->nr;
      else if (adap->nr != first_id) {
        p5c4_fail("lifecycle id reuse", adap->nr);
        i2c_del_adapter(adap);
        kfree(adap);
        break;
      }
      i2c_del_adapter(adap);
      if (i2c_get_adapter(first_id) != NULL) {
        p5c4_fail("lifecycle del not visible", first_id);
        kfree(adap);
        break;
      }
      kfree(adap);
    }
    if (i == P5C4_ADAPTER_CYCLES)
      p5c4_ok("1000 add/del cycles with id reuse");

    if (asc_pmm_get_free_pages() + 2 >= pmm_before)
      p5c4_ok("adapter lifecycle PMM stable");
    else
      klogf("[FAIL] LinuxKPI: i2c PMM baseline=%lu final=%lu\n", pmm_before,
            asc_pmm_get_free_pages());
  }

  /* 6: the drm_edid.c DDC sequence against a synthetic EDID. */
  p5c4_build_edid(p5c4_ddc.edid);
  p5c4_init_adap(&p5c4_ddc.adap, "p5c4-ddc", &p5c4_ddc_algo, &p5c4_ddc);
  ret = i2c_add_adapter(&p5c4_ddc.adap);
  if (ret != 0) {
    p5c4_fail("ddc adapter add", ret);
  } else {
    unsigned char start = 0;
    u8 block[EDID_LENGTH];
    struct i2c_msg msgs[2] = {
        {.addr = DDC_ADDR, .flags = 0, .len = 1, .buf = &start},
        {.addr = DDC_ADDR,
         .flags = I2C_M_RD,
         .len = EDID_LENGTH,
         .buf = block},
    };
    bool corrupt = true;

    memset(block, 0, sizeof(block));
    ret = i2c_transfer(&p5c4_ddc.adap, msgs, 2);
    if (ret == 2 && p5c4_ddc.xfers == 1 && p5c4_ddc.reads == 1 &&
        p5c4_ddc.bad_addr == 0 && drm_edid_block_valid(block, 0, false, &corrupt) &&
        !corrupt)
      p5c4_ok("DDC 2-message read validates via drm_edid_block_valid");
    else
      p5c4_fail("DDC base block", ret);

    /* Block 2 adds the 0x30 segment write (the xfers == 3 branch). */
    {
      unsigned char start2 = (unsigned char)(2 * EDID_LENGTH);
      unsigned char segment = 2 >> 1;
      u8 block2[EDID_LENGTH];
      struct i2c_msg msgs3[3] = {
          {.addr = P5C4_DDC_SEG_ADDR, .flags = 0, .len = 1, .buf = &segment},
          {.addr = DDC_ADDR, .flags = 0, .len = 1, .buf = &start2},
          {.addr = DDC_ADDR,
           .flags = I2C_M_RD,
           .len = EDID_LENGTH,
           .buf = block2},
      };

      memset(block2, 0, sizeof(block2));
      ret = i2c_transfer(&p5c4_ddc.adap, msgs3, 3);
      if (ret == 3 && p5c4_ddc.segments == 1 &&
          memcmp(block2, p5c4_ddc.edid, EDID_LENGTH) == 0)
        p5c4_ok("DDC 3-message segment sequence");
      else
        p5c4_fail("DDC segment block", ret);
    }

    i2c_del_adapter(&p5c4_ddc.adap);
  }

  i2c_del_adapter(&p5c4_numbered.adap);
  i2c_del_adapter(&p5c4_general.adap);

  if (p5c4_failures == 0)
    klog_puts("[  OK  ] LinuxKPI: i2c suite complete\n");
  else
    klogf("[FAIL] LinuxKPI: i2c suite had %d failure(s)\n", p5c4_failures);
}
