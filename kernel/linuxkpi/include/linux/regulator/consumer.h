#ifndef __AVORY_LINUXKPI_REGULATOR_CONSUMER_H
#define __AVORY_LINUXKPI_REGULATOR_CONSUMER_H

/* AvoryOS overlay for <linux/regulator/consumer.h>.
 *
 * Upstream's header pulls suspend.h (and through it swap/memcontrol/writeback)
 * to express system-wide regulator state.  AvoryOS has no regulator framework
 * and no OF node population, so the few calls simpledrm makes always report
 * "no supply"; linuxkpi/src/drm_compat.c implements them. */

#include <linux/types.h>

struct device;
struct regulator;
struct device_node;

int regulator_enable(struct regulator *regulator);
int regulator_disable(struct regulator *regulator);
int regulator_is_enabled(struct regulator *regulator);
int regulator_set_voltage(struct regulator *regulator, int min_uV, int max_uV);
int regulator_get_voltage(struct regulator *regulator);
struct regulator *regulator_get(struct device *dev, const char *id);
struct regulator *regulator_get_optional(struct device *dev, const char *id);
void regulator_put(struct regulator *regulator);

#endif /* __AVORY_LINUXKPI_REGULATOR_CONSUMER_H */
