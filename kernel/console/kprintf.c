#include <funnyos/kprintf.h>
#include <funnyos/serial.h>

#include <libk/printf.h>

/*
 * Current output backend. Once the framebuffer console lands in M2 this
 * becomes a dual write (serial + framebuffer).
 */
static void console_backend_putc(void *ctx, char c)
{
    (void)ctx;
    serial_putc(c);
}

void kputc(char c)
{
    console_backend_putc(NULL, c);
}

void kputs(const char *s)
{
    while (*s)
        kputc(*s++);
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvformat(console_backend_putc, NULL, fmt, ap);
    va_end(ap);
}
