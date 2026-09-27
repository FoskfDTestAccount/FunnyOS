/*
 * libk -- kernel support library: memory and string operations.
 *
 * Note: even when compiling with -ffreestanding, GCC still emits calls to
 * memcpy/memset/memmove/memcmp for struct assignment and bulk copies.
 * The kernel must provide these itself, or linking fails.
 */
#ifndef FUNNYOS_LIBK_STRING_H
#define FUNNYOS_LIBK_STRING_H

#include <stddef.h>

void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);

#endif /* FUNNYOS_LIBK_STRING_H */
