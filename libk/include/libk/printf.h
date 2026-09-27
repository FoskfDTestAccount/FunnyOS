/*
 * libk -- minimal formatted output.
 *
 * Depends on nothing from a hosted libc. Output is written through a
 * caller-supplied character callback so that one implementation serves
 * both the serial port and, later, the framebuffer console.
 */
#ifndef FUNNYOS_LIBK_PRINTF_H
#define FUNNYOS_LIBK_PRINTF_H

#include <stdarg.h>
#include <stddef.h>

/* Character sink: writes c to some destination (serial, console, buffer). */
typedef void (*putchar_fn)(void *ctx, char c);

/*
 * Format into a callback.
 * Supported conversions: %s %c %d %i %u %x %X %p %%
 * Supported flags: '-' (left align), '0' (zero pad), numeric width
 */
void kvformat(putchar_fn out, void *ctx, const char *fmt, va_list ap);

/* Convenience wrapper: variadic form. */
void kformat(putchar_fn out, void *ctx, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#endif /* FUNNYOS_LIBK_PRINTF_H */
