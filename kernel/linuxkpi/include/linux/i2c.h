#ifndef __AVORY_LINUXKPI_I2C_H
#define __AVORY_LINUXKPI_I2C_H

/* AvoryOS overlay for <linux/i2c.h>.
 *
 * Upstream's i2c.h pulls in acpi/of/regulator/irqdomain just to declare the
 * adapter model; the DRM core only speaks to an adapter through pointers and
 * i2c_transfer() (EDID DDC).  The real i2c core lands with the display phase
 * (Phase 5); until then linuxkpi/src/drm_compat.c provides a -EIO
 * i2c_transfer() so EDID probing degrades cleanly instead of linking against
 * a full bus implementation we do not have. */

#include <linux/types.h>
#include <linux/device.h>

struct module;
struct i2c_adapter;
struct i2c_client;
struct i2c_algorithm;
struct fwnode_handle;

#define I2C_NAME_SIZE 20

/* i2c_msg flags (upstream uapi values). */
#define I2C_M_RD 0x0001
#define I2C_M_TEN 0x0010
#define I2C_M_DMA_SAFE 0x0200
#define I2C_M_RECV_LEN 0x0400
#define I2C_M_NO_RD_ACK 0x0800
#define I2C_M_IGNORE_NAK 0x1000
#define I2C_M_REV_DIR_ADDR 0x2000
#define I2C_M_NOSTART 0x4000
#define I2C_M_STOP 0x8000

struct i2c_msg {
  __u16 addr;
  __u16 flags;
  __u16 len;
  __u8 *buf;
};

struct i2c_adapter {
  struct module *owner;
  unsigned int class;
  const struct i2c_algorithm *algo;
  void *algo_data;
  struct device dev; /* embedded, as upstream */
  int nr;
  char name[48];
};

struct i2c_board_info {
  char type[I2C_NAME_SIZE];
  unsigned short flags;
  unsigned short addr;
  void *platform_data;
  struct fwnode_handle *fwnode;
};

int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
int i2c_master_send(const struct i2c_client *client, const char *buf, int count);
int i2c_master_recv(const struct i2c_client *client, char *buf, int count);

#endif /* __AVORY_LINUXKPI_I2C_H */
