/*
 * Kernel formatted output.
 *
 * All kernel output goes through this layer rather than calling
 * serial_putc() directly. When the framebuffer console is added in M2,
 * only this file needs to change; callers stay untouched.
 */
#ifndef FUNNYOS_KPRINTF_H
#define FUNNYOS_KPRINTF_H

/* Emit a single character to the current output backend. */
void kputc(char c);

/* Emit a NUL-terminated string. */
void kputs(const char *s);

/* Formatted output. */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* FUNNYOS_KPRINTF_H */
