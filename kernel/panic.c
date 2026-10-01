#include <funnyos/panic.h>
#include <funnyos/kprintf.h>
#include <funnyos/screen.h>
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
    /*
     * Take the screen back before writing a word of it.
     *
     * This is the promise the screen layer is built around: a program may
     * hold the display, and the kernel may take it back at any moment
     * without asking. A panic is the case the whole arrangement exists
     * for, because a kernel that cannot print why it stopped has stopped
     * for no stated reason -- and the program holding the screen is quite
     * often why.
     *
     * It is also the one caller that prints to a screen it does not
     * necessarily own without any bookkeeping, which is why
     * screen_take_back cannot fail and does not report.
     */
    screen_take_back();

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
