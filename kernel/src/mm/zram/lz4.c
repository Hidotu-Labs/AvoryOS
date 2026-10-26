#include "lz4.h"
#include "lib/string.h"
#include <stdint.h>

static uint32_t word(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
static int extension(uint8_t *out, size_t cap, size_t *at, size_t n) {
  while (n >= 255) {
    if (*at >= cap)
      return -1;
    out[(*at)++] = 255;
    n -= 255;
  }
  if (*at >= cap)
    return -1;
  out[(*at)++] = (uint8_t)n;
  return 0;
}
size_t zram_lz4_compress(const void *src, size_t n, void *dst, size_t cap) {
  if (!src || !dst || !n || n > 65535)
    return 0;
  const uint8_t *in = src;
  uint8_t *out = dst;
  uint16_t table[512] = {0};
  size_t pos = 0, anchor = 0, at = 0;
  while (pos + 12 <= n) {
    unsigned h = (word(in + pos) * 2654435761u) >> 23;
    uint16_t old = table[h];
    table[h] = (uint16_t)(pos + 1);
    size_t ref = old ? (size_t)old - 1 : pos;
    if (!old || ref >= pos || word(in + ref) != word(in + pos)) {
      pos++;
      continue;
    }
    size_t match = 4;
    while (pos + match < n - 5 && in[ref + match] == in[pos + match])
      match++;
    size_t literal = pos - anchor, extra = match - 4;
    if (at >= cap)
      return 0;
    out[at++] = (uint8_t)((literal < 15 ? literal : 15) << 4) |
                (uint8_t)(extra < 15 ? extra : 15);
    if (literal >= 15 && extension(out, cap, &at, literal - 15))
      return 0;
    if (literal > cap - at)
      return 0;
    memcpy(out + at, in + anchor, literal);
    at += literal;
    if (cap - at < 2)
      return 0;
    size_t distance = pos - ref;
    out[at++] = (uint8_t)distance;
    out[at++] = (uint8_t)(distance >> 8);
    if (extra >= 15 && extension(out, cap, &at, extra - 15))
      return 0;
    pos += match;
    anchor = pos;
  }
  size_t literal = n - anchor;
  if (at >= cap)
    return 0;
  out[at++] = (uint8_t)((literal < 15 ? literal : 15) << 4);
  if (literal >= 15 && extension(out, cap, &at, literal - 15))
    return 0;
  if (literal > cap - at)
    return 0;
  memcpy(out + at, in + anchor, literal);
  return at + literal;
}
static int length(const uint8_t *in, size_t n, size_t *at, size_t *value) {
  uint8_t b;
  do {
    if (*at >= n)
      return -1;
    b = in[(*at)++];
    if (*value > (size_t)-1 - b)
      return -1;
    *value += b;
  } while (b == 255);
  return 0;
}
int zram_lz4_decompress(const void *src, size_t n, void *dst, size_t expected) {
  if (!src || !dst || !n)
    return -1;
  const uint8_t *in = src;
  uint8_t *out = dst;
  size_t at = 0, done = 0;
  while (at < n) {
    uint8_t token = in[at++];
    size_t literal = token >> 4;
    if (literal == 15 && length(in, n, &at, &literal))
      return -1;
    if (literal > n - at || literal > expected - done)
      return -1;
    memcpy(out + done, in + at, literal);
    done += literal;
    at += literal;
    if (at == n)
      return done == expected ? 0 : -1;
    if (n - at < 2)
      return -1;
    size_t distance = in[at] | (size_t)in[at + 1] << 8;
    at += 2;
    if (!distance || distance > done)
      return -1;
    size_t match = (token & 15) + 4;
    if ((token & 15) == 15 && length(in, n, &at, &match))
      return -1;
    if (match > expected - done)
      return -1;
    for (size_t i = 0; i < match; i++)
      out[done + i] = out[done + i - distance];
    done += match;
  }
  return -1;
}
