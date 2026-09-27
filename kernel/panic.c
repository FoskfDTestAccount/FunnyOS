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

    kputs("\nSystem halted.\n");

    /* Mask interrupts and halt forever. Using hlt in a loop rather than
     * spinning keeps the CPU from burning power for no reason. */
    interrupts_disable();
    for (;;)
        cpu_halt();
}
