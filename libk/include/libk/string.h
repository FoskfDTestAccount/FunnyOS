/*
 * libk —— 内核基础库：内存与字符串操作
 *
 * 注意：使用 -ffreestanding 编译时，GCC 仍会为结构体赋值、大块拷贝等
 * 生成对 memcpy/memset/memmove/memcmp 的调用。这些函数必须由内核自己
 * 提供，否则链接会失败。
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
