#include <funnyos/panic.h>
#include <funnyos/kprintf.h>
#include <funnyos/arch/x86_64/io.h>

#include <stdarg.h>
#include <libk/printf.h>

static void panic_putc(void *ctx, char c)
{
    (void)ctx;
    kputc(c);
}

void panic(const char *fmt, ...)
{
    kputs("\n*** FunnyOS PANIC ***\n");

    va_list ap;
    va_start(ap, fmt);
    kvformat(panic_putc, NULL, fmt, ap);
    va_end(ap);

    kputs("\n系统已停止。\n");

    /* 关中断并永久停机。用 hlt 循环而不是忙等，避免空转烧 CPU。 */
    interrupts_disable();
    for (;;)
        cpu_halt();
}
