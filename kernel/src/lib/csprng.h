#ifndef LIB_CSPRNG_H
#define LIB_CSPRNG_H

#include <stddef.h>
#include <stdint.h>

void csprng_get_bytes(void *out, size_t len);
void csprng_stir(const void *data, size_t len);

#endif
