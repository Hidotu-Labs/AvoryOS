#ifndef __AVORY_LINUXKPI_I2C_H
#define __AVORY_LINUXKPI_I2C_H

/* AvoryOS overlay for <linux/i2c.h>.
 *
 * Upstream's i2c.h pulls in acpi/of/regulator/irqdomain just to declare the
 * adapter model; this overlay declares the subset the DRM DDC/EDID path and
 * the future amdgpu DM adapter need.  The minimal core behind it lives in
 * linuxkpi/src/i2c.c (Phase 5 C4) and is documented in docs/linuxkpi-gaps.md:
 * no i2c clients/instantiation, no OF/ACPI adapter lookup, no /dev/i2c-N, no
 * SMBus emulation, no bus-segment/mux locking.  Drivers register a
 * master_xfer algorithm, call i2c_transfer() and are done. */

#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/types.h>

struct module;
struct i2c_adapter;
struct i2c_client;
struct i2c_algorithm;
struct fwnode_handle;
union i2c_smbus_data;

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

/* i2c_client flags (upstream values). */
#define I2C_CLIENT_PEC 0x04
#define I2C_CLIENT_TEN 0x10
#define I2C_CLIENT_SLAVE 0x20
#define I2C_CLIENT_HOST_NOTIFY 0x40
#define I2C_CLIENT_WAKE 0x80
#define I2C_CLIENT_SCCB 0x9000

/* To determine what functionality is present (upstream uapi values). */
#define I2C_FUNC_I2C 0x00000001
#define I2C_FUNC_10BIT_ADDR 0x00000002
#define I2C_FUNC_PROTOCOL_MANGLING 0x00000004
#define I2C_FUNC_SMBUS_PEC 0x00000008
#define I2C_FUNC_NOSTART 0x00000010
#define I2C_FUNC_SLAVE 0x00000020
#define I2C_FUNC_SMBUS_BLOCK_PROC_CALL 0x00008000
#define I2C_FUNC_SMBUS_QUICK 0x00010000
#define I2C_FUNC_SMBUS_READ_BYTE 0x00020000
#define I2C_FUNC_SMBUS_WRITE_BYTE 0x00040000
#define I2C_FUNC_SMBUS_READ_BYTE_DATA 0x00080000
#define I2C_FUNC_SMBUS_WRITE_BYTE_DATA 0x00100000
#define I2C_FUNC_SMBUS_READ_WORD_DATA 0x00200000
#define I2C_FUNC_SMBUS_WRITE_WORD_DATA 0x00400000
#define I2C_FUNC_SMBUS_PROC_CALL 0x00800000
#define I2C_FUNC_SMBUS_READ_BLOCK_DATA 0x01000000
#define I2C_FUNC_SMBUS_WRITE_BLOCK_DATA 0x02000000
#define I2C_FUNC_SMBUS_READ_I2C_BLOCK 0x04000000
#define I2C_FUNC_SMBUS_WRITE_I2C_BLOCK 0x08000000
#define I2C_FUNC_SMBUS_HOST_NOTIFY 0x10000000

#define I2C_FUNC_SMBUS_BYTE                                                    \
  (I2C_FUNC_SMBUS_READ_BYTE | I2C_FUNC_SMBUS_WRITE_BYTE)
#define I2C_FUNC_SMBUS_BYTE_DATA                                               \
  (I2C_FUNC_SMBUS_READ_BYTE_DATA | I2C_FUNC_SMBUS_WRITE_BYTE_DATA)
#define I2C_FUNC_SMBUS_WORD_DATA                                               \
  (I2C_FUNC_SMBUS_READ_WORD_DATA | I2C_FUNC_SMBUS_WRITE_WORD_DATA)
#define I2C_FUNC_SMBUS_BLOCK_DATA                                              \
  (I2C_FUNC_SMBUS_READ_BLOCK_DATA | I2C_FUNC_SMBUS_WRITE_BLOCK_DATA)
#define I2C_FUNC_SMBUS_I2C_BLOCK                                               \
  (I2C_FUNC_SMBUS_READ_I2C_BLOCK | I2C_FUNC_SMBUS_WRITE_I2C_BLOCK)

/* i2c adapter classes (bitmask). */
#define I2C_CLASS_HWMON (1 << 0)
#define I2C_CLASS_DDC (1 << 3)
#define I2C_CLASS_SPD (1 << 7)
#define I2C_CLASS_DEPRECATED (1 << 8)

/**
 * struct i2c_adapter_quirks - describe flaws of an i2c adapter
 *
 * Only the layout amdgpu-style drivers initialize is provided; the core does
 * not enforce the limits yet (no P5 consumer needs it).
 */
struct i2c_adapter_quirks {
  u64 flags;
  int max_num_msgs;
  u16 max_write_len;
  u16 max_read_len;
  u16 max_comb_1st_msg_len;
  u16 max_comb_2nd_msg_len;
};

/**
 * struct i2c_algorithm - represent I2C transfer method
 * @master_xfer: issue a set of messages; returns the number of messages
 *   processed or a negative errno
 * @smbus_xfer: SMBus-level access; the core implements no SMBus emulation,
 *   but drivers may publish one (the field must exist for their algorithm
 *   structs)
 * @functionality: I2C_FUNC_* flags supported by the adapter
 */
struct i2c_algorithm {
  int (*master_xfer)(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
  int (*smbus_xfer)(struct i2c_adapter *adap, u16 addr, unsigned short flags,
                    char read_write, u8 command, int size,
                    union i2c_smbus_data *data);
  u32 (*functionality)(struct i2c_adapter *adap);
};

/**
 * struct i2c_client - represent an I2C slave device
 *
 * Minimal form: address/name/adapter and the driver-model device.  There is
 * no client instantiation in P5; drivers that need to build one on the stack
 * (i2c_master_send/recv callers) get this shape.
 */
struct i2c_client {
  unsigned short flags;
  unsigned short addr;
  char name[I2C_NAME_SIZE];
  struct i2c_adapter *adapter;
  struct device dev;
};

/**
 * struct i2c_adapter - represent an I2C bus
 *
 * Field order and names follow upstream where it matters for designated
 * initializers.  bus_lock is a plain LinuxKPI mutex (upstream uses rt_mutex
 * plus I2C_LOCK_SEGMENT lock_ops); i2c_add_adapter() initializes it.
 */
struct i2c_adapter {
  struct module *owner;
  unsigned int class;
  const struct i2c_algorithm *algo;
  void *algo_data;
  struct mutex bus_lock;
  int timeout;
  int retries;
  struct device dev;
  int nr;
  char name[48];
  const struct i2c_adapter_quirks *quirks;
};

#define to_i2c_adapter(d) container_of(d, struct i2c_adapter, dev)
#define to_i2c_client(d) container_of(d, struct i2c_client, dev)

struct i2c_board_info {
  char type[I2C_NAME_SIZE];
  unsigned short flags;
  unsigned short addr;
  void *platform_data;
  struct fwnode_handle *fwnode;
};

extern struct device_type i2c_adapter_type;

static inline void *i2c_get_adapdata(const struct i2c_adapter *adap) {
  return dev_get_drvdata(&adap->dev);
}

static inline void i2c_set_adapdata(struct i2c_adapter *adap, void *data) {
  dev_set_drvdata(&adap->dev, data);
}

static inline int i2c_adapter_id(const struct i2c_adapter *adap) {
  return adap->nr;
}

static inline u32 i2c_get_functionality(struct i2c_adapter *adap) {
  if (!adap->algo || !adap->algo->functionality)
    return 0;
  return adap->algo->functionality(adap);
}

static inline int i2c_check_functionality(struct i2c_adapter *adap, u32 func) {
  return (func & i2c_get_functionality(adap)) == func;
}

static inline u8 i2c_8bit_addr_from_msg(const struct i2c_msg *msg) {
  return (msg->addr << 1) | (msg->flags & I2C_M_RD ? 1 : 0);
}

/* administration */
int i2c_add_adapter(struct i2c_adapter *adap);
int i2c_add_numbered_adapter(struct i2c_adapter *adap);
void i2c_del_adapter(struct i2c_adapter *adap);
struct i2c_adapter *i2c_get_adapter(int nr);
void i2c_put_adapter(struct i2c_adapter *adap);
struct i2c_adapter *i2c_verify_adapter(struct device *dev);

/* transfers */
int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
int __i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
int i2c_transfer_buffer_flags(const struct i2c_client *client, char *buf,
                              int count, u16 flags);
int i2c_master_send(const struct i2c_client *client, const char *buf,
                    int count);
int i2c_master_recv(const struct i2c_client *client, char *buf, int count);

#endif /* __AVORY_LINUXKPI_I2C_H */
