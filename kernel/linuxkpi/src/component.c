/* Component registration without an aggregate/master driver.
 *
 * component_add() registers a candidate; it does not promise a bound master.
 * GPU display drivers rely on registration succeeding when no HDA master is
 * present. Keep registrations until component_del(), without invoking bind
 * callbacks or claiming that HDMI audio has become available.
 */
#include <linux/component.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/slab.h>

struct kpi_component {
  struct kpi_component *next;
  struct device *dev;
  const struct component_ops *ops;
};
static DEFINE_MUTEX(component_lock);
static struct kpi_component *components;

int component_add(struct device *dev, const struct component_ops *ops) {
  if (!dev || !ops)
    return -EINVAL;
  struct kpi_component *entry = kzalloc(sizeof(*entry), GFP_KERNEL);
  if (!entry)
    return -ENOMEM;
  entry->dev = dev;
  entry->ops = ops;
  mutex_lock(&component_lock);
  for (struct kpi_component *p = components; p; p = p->next) {
    if (p->dev == dev && p->ops == ops) {
      mutex_unlock(&component_lock);
      kfree(entry);
      return -EBUSY;
    }
  }
  entry->next = components;
  components = entry;
  mutex_unlock(&component_lock);
  return 0;
}

void component_del(struct device *dev, const struct component_ops *ops) {
  struct kpi_component *entry = NULL;
  mutex_lock(&component_lock);
  for (struct kpi_component **p = &components; *p; p = &(*p)->next) {
    if ((*p)->dev == dev && (*p)->ops == ops) {
      entry = *p;
      *p = entry->next;
      break;
    }
  }
  mutex_unlock(&component_lock);
  kfree(entry);
}
