#ifndef STRING_H
#define STRING_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
size_t strlen(const char *s);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
char *strrchr(const char *s, int c);
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t n);
char *strstr(const char *haystack, const char *needle);
void *memset(void *s, int c, size_t n);

void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
/* Copy WB memory to WC memory. Caller must sfence after the copy batch. */
void *memcpy_to_wc(void *dest, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);
uint32_t atoui(const char *s);
int strcasecmp(const char *s1, const char *s2);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);

#endif
