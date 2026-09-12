#ifndef __AVORY_LINUXKPI_PAGEVEC_H
#define __AVORY_LINUXKPI_PAGEVEC_H

/* AvoryOS overlay for <linux/pagevec.h>.
 *
 * The upstream helper is self-contained apart from __folio_batch_release()
 * (mm/swap.c) and check_move_unevictable_folios() (swap reclaim).  Pages are
 * order-0 here, so a folio_batch is simply an array of order-0 folios and the
 * unevictable move is a no-op until reclaim exists. */

#include <linux/types.h>

struct folio;

#define PAGEVEC_SIZE 15

struct folio_batch {
  unsigned char nr;
  bool percpu_pvec_drained;
  struct folio *folios[PAGEVEC_SIZE];
};

static inline void folio_batch_init(struct folio_batch *fbatch) {
  fbatch->nr = 0;
  fbatch->percpu_pvec_drained = false;
}

static inline void folio_batch_reinit(struct folio_batch *fbatch) {
  fbatch->nr = 0;
}

static inline unsigned int folio_batch_count(struct folio_batch *fbatch) {
  return fbatch->nr;
}

static inline unsigned int folio_batch_space(struct folio_batch *fbatch) {
  return PAGEVEC_SIZE - fbatch->nr;
}

/* Returns the number of slots still available, like upstream. */
static inline unsigned int folio_batch_add(struct folio_batch *fbatch,
                                           struct folio *folio) {
  fbatch->folios[fbatch->nr++] = folio;
  return folio_batch_space(fbatch);
}

/* Implemented in linuxkpi/src/page.c: drops one reference per folio. */
void __folio_batch_release(struct folio_batch *pvec);

static inline void folio_batch_release(struct folio_batch *fbatch) {
  if (folio_batch_count(fbatch))
    __folio_batch_release(fbatch);
}

static inline void check_move_unevictable_folios(struct folio_batch *fbatch) {
  (void)fbatch;
}

#endif /* __AVORY_LINUXKPI_PAGEVEC_H */
