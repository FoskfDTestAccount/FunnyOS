#include <funnyos/kprintf.h>
#include <funnyos/serial.h>
#include <funnyos/fb.h>

#include <libk/printf.h>

/*
 * Output fans out to both channels.
 *
 * Serial is the machine-readable channel: QEMU captures it headlessly and
 * the boot test asserts against it. The framebuffer is the human channel:
 * it is what shows up on a real screen, or in VMware/VMPlayer. Keeping
 * both means the automated tests keep working while a physical boot is
 * still visible without a serial cable.
 */
static void console_backend_putc(void *ctx, char c)
{
    (void)ctx;
    serial_putc(c);
    fb_putc(c);
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
