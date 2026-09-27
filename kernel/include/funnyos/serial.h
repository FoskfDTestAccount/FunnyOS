/*
 * Serial port driver (16550 UART, COM1).
 *
 * Why M0 already needs this: QEMU can run headless and pipe the serial
 * port to stdout, which makes kernel output capturable and assertable by
 * scripts. Until the framebuffer console exists in M2 (and afterwards, for
 * kernel diagnostics) this is the only reliable observation channel.
 */
#ifndef FUNNYOS_SERIAL_H
#define FUNNYOS_SERIAL_H

#include <stdbool.h>

/* Initialise COM1. Must be called before any output. */
void serial_init(void);

/* Emit a single character. Undefined before serial_init(). */
void serial_putc(char c);

/* Emit a NUL-terminated string. */
void serial_write(const char *s);

/* Whether the serial port was initialised and passed its self-test. */
bool serial_is_ready(void);

#endif /* FUNNYOS_SERIAL_H */
