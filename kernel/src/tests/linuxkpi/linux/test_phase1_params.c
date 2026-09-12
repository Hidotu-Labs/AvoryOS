/* Phase 1 — kernel module parameters and __setup() handlers. */

#include <linux/moduleparam.h>
#include <linux/string.h>

#include <linuxkpi/log.h>

static int kpi_test_int = -1;
module_param(kpi_test_int, int, 0);

static bool kpi_test_bool = false;
module_param(kpi_test_bool, bool, 0);

static char kpi_test_str[16];
module_param_string(kpi_test_str, kpi_test_str, sizeof(kpi_test_str), 0);

static int kpi_test_setup_called;
static int __init kpi_test_setup_fn(char *arg) {
  if (!strcmp(arg, "abc"))
    kpi_test_setup_called = 1;
  return 0;
}
__setup("kpi_test=", kpi_test_setup_fn);

static bool test_params(void) {
  kpi_test_int = -1;
  kpi_test_bool = false;
  kpi_test_str[0] = '\0';
  kpi_test_setup_called = 0;

  linuxkpi_param_parse(
      "kpi_test_int=42 kpi_test_bool kpi_test_str=hello kpi_test=abc other=1");

  return kpi_test_int == 42 && kpi_test_bool &&
         strcmp(kpi_test_str, "hello") == 0 && kpi_test_setup_called == 1;
}

static bool test_params_off(void) {
  kpi_test_bool = true;
  linuxkpi_param_parse("kpi_test_bool=0");
  return !kpi_test_bool;
}

void linuxkpi_test_phase1_params(void) {
  static const struct {
    const char *name;
    bool (*fn)(void);
  } tests[] = {
      {"module_param/__setup", test_params},
      {"module_param bool off", test_params_off},
  };

  klog_puts("[LINUXKPI] Phase 1 params self-test\n");

  for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    if (tests[i].fn())
      klogf("[  OK  ] LinuxKPI: %s correct\n", tests[i].name);
    else
      klogf("[ FAIL ] LinuxKPI: %s wrong result\n", tests[i].name);
  }
}
