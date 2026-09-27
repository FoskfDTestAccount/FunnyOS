#include <funnyos/kprintf.h>
#include <funnyos/serial.h>

#include <libk/printf.h>

/*
 * 当前输出后端。M2 接入帧缓冲控制台后，这里会变成"串口 + 帧缓冲"双写。
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
