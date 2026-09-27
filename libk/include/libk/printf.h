/*
 * libk —— 极简格式化输出
 *
 * 不依赖任何 libc。格式化结果写入调用者提供的字符输出回调，
 * 这样同一份实现既可用于串口，也可用于后续的帧缓冲控制台。
 */
#ifndef FUNNYOS_LIBK_PRINTF_H
#define FUNNYOS_LIBK_PRINTF_H

#include <stdarg.h>
#include <stddef.h>

/* 字符输出回调：把 c 写到某个目标（串口、控制台、缓冲区） */
typedef void (*putchar_fn)(void *ctx, char c);

/*
 * 格式化到回调。
 * 支持的转换：%s %c %d %i %u %x %X %p %%
 * 支持的标志：- （左对齐）、0 （零填充）、数字宽度
 */
void kvformat(putchar_fn out, void *ctx, const char *fmt, va_list ap);

/* 便捷包装：可变参数版本 */
void kformat(putchar_fn out, void *ctx, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#endif /* FUNNYOS_LIBK_PRINTF_H */
