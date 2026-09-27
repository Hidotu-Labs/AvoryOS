#ifndef UDRIVER_H
#define UDRIVER_H

#include "../../../../include/ascent/udriver.h"
#include <stdbool.h>
#include <stdint.h>

struct udriver_session;

void udriver_init(void);
struct udriver_session *udriver_session_create(uint32_t owner_tgid);
int udriver_session_register(struct udriver_session *session,
                             const struct udrv_register *request);
int udriver_session_claim(struct udriver_session *session,
                          const char *device_name);
int udriver_session_release(struct udriver_session *session,
                            const char *device_name);
int udriver_session_status(struct udriver_session *session,
                           struct udrv_status *status);
void udriver_session_destroy(struct udriver_session *session);
#endif
