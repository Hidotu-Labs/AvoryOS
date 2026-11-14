#ifndef LINUXKPI_NATIVE_NET_H
#define LINUXKPI_NATIVE_NET_H

/* C-only boundary between Linux-header frontend code and Avory's native
 * Ethernet core. Keep foreign struct net_device/sk_buff tags on the Linux side
 * and pass byte frames plus opaque handles across this interface. */
struct linuxkpi_native_net;
struct linuxkpi_native_napi;

enum linuxkpi_native_tx_result {
  LINUXKPI_NATIVE_TX_ACCEPTED = 0,
  LINUXKPI_NATIVE_TX_BUSY = 1,
  LINUXKPI_NATIVE_TX_FAILED = 2,
};

struct linuxkpi_native_net_ops {
  int (*open)(void *);
  void (*stop)(void *);
  int (*carrier)(void *);
  enum linuxkpi_native_tx_result (*transmit)(void *, const unsigned char *,
                                              unsigned long);
};

typedef int (*linuxkpi_native_poll_fn)(void *, int budget);
typedef void (*linuxkpi_native_rx_tap_fn)(void *, const void *,
                                          unsigned long length);

struct linuxkpi_native_net *linuxkpi_native_net_register(
    const char *name, const unsigned char mac[6], unsigned short mtu,
    const struct linuxkpi_native_net_ops *, void *context);
int linuxkpi_native_net_unregister(struct linuxkpi_native_net *);
int linuxkpi_native_net_get_name(struct linuxkpi_native_net *, char out[16]);
int linuxkpi_native_net_rx(struct linuxkpi_native_net *, const void *,
                           unsigned long length);
int linuxkpi_native_net_transmit(struct linuxkpi_native_net *, const void *,
                                 unsigned long length);
void linuxkpi_native_net_tx_wake(struct linuxkpi_native_net *);
void linuxkpi_native_net_stats(struct linuxkpi_native_net *,
                               unsigned long long *rx_packets,
                               unsigned long long *tx_packets,
                               unsigned long long *rx_dropped,
                               unsigned long long *tx_dropped);
void linuxkpi_native_net_set_rx_tap(struct linuxkpi_native_net *,
                                    linuxkpi_native_rx_tap_fn, void *context);
unsigned linuxkpi_native_net_references(struct linuxkpi_native_net *);

struct linuxkpi_native_napi *linuxkpi_native_napi_create(
    linuxkpi_native_poll_fn, void *context, unsigned weight);
int linuxkpi_native_napi_enable(struct linuxkpi_native_napi *);
int linuxkpi_native_napi_schedule(struct linuxkpi_native_napi *);
int linuxkpi_native_napi_complete(struct linuxkpi_native_napi *, int work_done);
void linuxkpi_native_napi_disable(struct linuxkpi_native_napi *);
void linuxkpi_native_napi_destroy(struct linuxkpi_native_napi *);

#endif
