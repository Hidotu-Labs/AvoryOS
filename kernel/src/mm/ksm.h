#ifndef MM_KSM_H
#define MM_KSM_H

#include <stdbool.h>
#include <stdint.h>

struct ksm_stats {
  uint64_t pages_shared;       // Unique shared physical frames created
  uint64_t pages_sharing;      // Duplicate frames eliminated (same-page)
  uint64_t zero_pages_merged;  // Zero-filled frames collapsed into zero page
  uint64_t pages_scanned;      // Total pages examined in last pass
  uint64_t scan_cycles;        // Total full scan passes completed
  uint64_t passes_idle;        // Background passes run while RAM was plentiful
  bool     running;            // Background daemon status
};

void     ksm_init(void);
uint64_t ksm_scan_pass(void);
void     ksm_get_stats(struct ksm_stats *out);
void     ksm_set_running(bool run);
bool     ksm_is_running(void);

#endif // MM_KSM_H
