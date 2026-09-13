/* Minimal Linux I2C core for LinuxKPI (Phase 5 C4).
 *
 * Scope: adapter registration and master transfers for the DRM DDC/EDID path
 * and the future amdgpu DM adapter.  This is deliberately not i2c-core-base.c
 * (the plan rules that import out of P5): no clients/instantiation, no
 * OF/ACPI adapter lookup, no SMBus emulation, no /dev/i2c-N, no
 * suspend/resume or mux/segment locking.  Every divergence is recorded in
 * docs/linuxkpi-gaps.md (P5 C4). */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/sprintf.h>

#define I2C_KPI_MAX_ADAPTERS 64

struct device_type i2c_adapter_type = {
    .name = "i2c_adapter",
};

/* Adapter registry.  The array caps the bus count at 64; slots are taken
 * lowest-free-first (dynamic) or at adap->nr (numbered). */
static struct i2c_adapter *i2c_kpi_adapters[I2C_KPI_MAX_ADAPTERS];
static DEFINE_MUTEX(i2c_kpi_adapters_lock);

static int i2c_kpi_register(struct i2c_adapter *adap, int nr) {
  int id = nr;

  if (!adap || !adap->algo)
    return -EINVAL;

  mutex_lock(&i2c_kpi_adapters_lock);
  if (id < 0) {
    for (id = 0; id < I2C_KPI_MAX_ADAPTERS; id++)
      if (!i2c_kpi_adapters[id])
        break;
    if (id == I2C_KPI_MAX_ADAPTERS) {
      mutex_unlock(&i2c_kpi_adapters_lock);
      return -ENOSPC;
    }
  } else if (id >= I2C_KPI_MAX_ADAPTERS) {
    mutex_unlock(&i2c_kpi_adapters_lock);
    return -EINVAL;
  } else if (i2c_kpi_adapters[id]) {
    mutex_unlock(&i2c_kpi_adapters_lock);
    return -EBUSY;
  }

  mutex_init(&adap->bus_lock);
  adap->nr = id;
  adap->dev.type = &i2c_adapter_type;
  if (!adap->name[0])
    snprintf(adap->name, sizeof(adap->name), "i2c-%d", id);
  i2c_kpi_adapters[id] = adap;
  mutex_unlock(&i2c_kpi_adapters_lock);
  return 0;
}

int i2c_add_adapter(struct i2c_adapter *adap) { return i2c_kpi_register(adap, -1); }

int i2c_add_numbered_adapter(struct i2c_adapter *adap) {
  if (adap->nr < 0)
    return -EINVAL;
  return i2c_kpi_register(adap, adap->nr);
}

void i2c_del_adapter(struct i2c_adapter *adap) {
  if (!adap)
    return;

  mutex_lock(&i2c_kpi_adapters_lock);
  if (adap->nr >= 0 && adap->nr < I2C_KPI_MAX_ADAPTERS &&
      i2c_kpi_adapters[adap->nr] == adap)
    i2c_kpi_adapters[adap->nr] = NULL;
  mutex_unlock(&i2c_kpi_adapters_lock);
}

struct i2c_adapter *i2c_get_adapter(int nr) {
  struct i2c_adapter *adap = NULL;

  if (nr < 0 || nr >= I2C_KPI_MAX_ADAPTERS)
    return NULL;

  mutex_lock(&i2c_kpi_adapters_lock);
  adap = i2c_kpi_adapters[nr];
  mutex_unlock(&i2c_kpi_adapters_lock);
  return adap;
}

/* No adapter reference counting: adapters live as long as their driver owns
 * them and i2c_del_adapter() only unregisters (documented in the gap log). */
void i2c_put_adapter(struct i2c_adapter *adap) { (void)adap; }

struct i2c_adapter *i2c_verify_adapter(struct device *dev) {
  if (!dev || dev->type != &i2c_adapter_type)
    return NULL;
  return to_i2c_adapter(dev);
}

int __i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num) {
  int ret = -EOPNOTSUPP;
  int retries, try;

  if (!adap || !adap->algo || !adap->algo->master_xfer)
    return -EOPNOTSUPP;

  /* Unlocked flavor: assume the caller already holds adap->bus_lock.  Retry
   * the whole transfer on -EAGAIN up to adap->retries extra attempts. */
  retries = adap->retries > 0 ? adap->retries : 0;
  for (try = 0; try <= retries; try++) {
    ret = adap->algo->master_xfer(adap, msgs, num);
    if (ret != -EAGAIN)
      break;
  }
  return ret;
}

int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num) {
  int ret;

  if (!adap || !adap->algo || !adap->algo->master_xfer)
    return -EOPNOTSUPP;

  mutex_lock(&adap->bus_lock);
  ret = __i2c_transfer(adap, msgs, num);
  mutex_unlock(&adap->bus_lock);
  return ret;
}

int i2c_transfer_buffer_flags(const struct i2c_client *client, char *buf,
                              int count, u16 flags) {
  struct i2c_msg msg = {
      .addr = client->addr,
      .flags = flags | (client->flags & I2C_CLIENT_TEN ? I2C_M_TEN : 0),
      .len = (u16)count,
      .buf = (u8 *)buf,
  };
  int ret;

  ret = i2c_transfer(client->adapter, &msg, 1);
  return ret == 1 ? count : ret;
}

int i2c_master_send(const struct i2c_client *client, const char *buf,
                    int count) {
  return i2c_transfer_buffer_flags(client, (char *)buf, count, 0);
}

int i2c_master_recv(const struct i2c_client *client, char *buf, int count) {
  return i2c_transfer_buffer_flags(client, buf, count, I2C_M_RD);
}
