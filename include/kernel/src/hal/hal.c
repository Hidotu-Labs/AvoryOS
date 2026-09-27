#include "hal.h"
#include "hal_arch.h"

static bool initialized;

bool hal_init(void) {
  if (initialized)
    return true;

  if (!hal_arch_init())
    return false;

  initialized = true;
  return true;
}

bool hal_is_initialized(void) { return initialized; }

const char *hal_arch_name(void) { return hal_arch_name_impl(); }
