#ifndef LINUXKPI_RTNETLINK_OVERLAY_H
#define LINUXKPI_RTNETLINK_OVERLAY_H
#ifdef LINUXKPI_NET_FRONTEND
#include <linux/skbuff.h>
#include <linux/rcupdate.h>
#include <linux/bug.h>
struct net_device;
void rtnl_lock(void);
void rtnl_unlock(void);
int rtnl_trylock(void);
int rtnl_is_locked(void);
#define ASSERT_RTNL() WARN_ON(!rtnl_is_locked())
#define rtnl_dereference(p) rcu_dereference(p)
#else
#include_next <linux/rtnetlink.h>
#endif
#endif
