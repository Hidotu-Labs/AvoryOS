/*
 * NVMe kernel self-tests (build with NVME_SELFTEST=1).
 *
 * Phase 2 introduced the disable->enable->Identify loop: every controller is
 * taken down and brought back up with its depth-64 admin queue, Identify runs
 * for the controller and each namespace, and the I/O queue is recreated, so a
 * broken queue setup surfaces at boot instead of as a hung root mount.
 *
 * Phase 5 adds structural checks that do not touch media (chained PRP lists,
 * graceful CC.SHN) plus destructive data tests for scratch configurations
 * (NVME_SELFTEST_DATA=1): 4Kn read-modify-write edges, multi-namespace
 * isolation and reset recovery after an injected timeout.
 *
 * Phase 7 adds the non-destructive hardening audit (NVME-AUDIT): namespace
 * geometry bounds, MDTS clamping, request bounds, CID exhaustion/reuse and
 * PRP builder limits, all exercised with synthetic values.
 *
 * scripts/nvme-stress.sh greps the PASS/FAIL/SKIP markers.
 */

#ifdef NVME_SELFTEST

#include "console/klog.h"
#include "drivers/storage/nvme.h"

#ifndef NVME_SELFTEST_CYCLES
#define NVME_SELFTEST_CYCLES 50u
#endif

#ifndef NVME_SELFTEST_IDENTIFY_OPS
#define NVME_SELFTEST_IDENTIFY_OPS 0u
#endif

/* 0 = PASS, >0 = FAIL, <0 = SKIP (nothing to test in this configuration). */
static const char *nvme_test_marker(int rc) {
  if (rc < 0)
    return "SKIP";
  return rc == 0 ? "PASS" : "FAIL";
}

void nvme_selftest(void) {
  unsigned count = nvme_controller_count();
  if (count == 0) {
    klog_puts("NVME-SELFTEST: SKIP (no controllers)\n");
    return;
  }

  unsigned namespaces = nvme_namespace_count();
  int rc = nvme_test_admin_cycles((unsigned)NVME_SELFTEST_CYCLES,
                                  (unsigned)NVME_SELFTEST_IDENTIFY_OPS);

  /* Non-destructive Phase 5 checks. */
  int prp = nvme_test_prp_chain();
  int shn = nvme_test_shutdown_cycle();

  /* Phase 7 hardening audit: geometry/MDTS/bounds/CID/PRP limits. */
  int audit = nvme_test_audit();

  /* Destructive checks are skipped unless built with NVME_SELFTEST_DATA. */
  int four = nvme_test_4kn();
  int multi = nvme_test_multi_ns();
  int recovery = nvme_test_recovery();

  klogf("NVME-PRPCHAIN: %s\n", nvme_test_marker(prp));
  klogf("NVME-SHN: %s\n", nvme_test_marker(shn));
  klogf("NVME-AUDIT: %s\n", nvme_test_marker(audit));
  klogf("NVME-4KN: %s\n", nvme_test_marker(four));
  klogf("NVME-MULTINS: %s\n", nvme_test_marker(multi));
  klogf("NVME-RECOVERY: %s\n", nvme_test_marker(recovery));

  unsigned failures = 0;
  failures += rc != 0 ? 1u : 0u;
  failures += prp > 0 ? (unsigned)prp : 0u;
  failures += shn > 0 ? (unsigned)shn : 0u;
  failures += audit > 0 ? (unsigned)audit : 0u;
  failures += four > 0 ? (unsigned)four : 0u;
  failures += multi > 0 ? (unsigned)multi : 0u;
  failures += recovery > 0 ? (unsigned)recovery : 0u;

  klogf("NVME-SELFTEST: %s (%u controller(s), %u namespace(s), %u cycle(s), "
        "%u identify op(s), %u failure(s))\n",
        failures == 0 ? "PASS" : "FAIL", count, namespaces,
        (unsigned)NVME_SELFTEST_CYCLES, (unsigned)NVME_SELFTEST_IDENTIFY_OPS,
        failures);

#ifdef NVME_SELFTEST_FAULT_TIMEOUT
  klogf("NVME-FAILSTOP: %s\n", failures == 0 ? "PASS" : "FAIL");
#endif
}

#endif /* NVME_SELFTEST */
