#include <libu/libu.h>

#include <libk/printf.h>
#include <libk/string.h>

/* Provided by syscall.asm. */
long u_syscall(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2);

/* --- Raw system calls ---------------------------------------------- */

void u_exit(int code)
{
    u_syscall(SYS_EXIT, (uint64_t)code, 0, 0);

    /* The kernel does not return from this. If it somehow did, spinning
     * is better than returning into a program that believes it ended. */
    for (;;)
        ;
}

long u_write(int fd, const void *buf, size_t len)
{
    return u_syscall(SYS_WRITE, (uint64_t)(int64_t)fd,
                     (uint64_t)(uintptr_t)buf, (uint64_t)len);
}

long u_read(int fd, void *buf, size_t len)
{
    return u_syscall(SYS_READ, (uint64_t)(int64_t)fd,
                     (uint64_t)(uintptr_t)buf, (uint64_t)len);
}

int u_open(const char *path, int flags)
{
    return (int)u_syscall(SYS_OPEN, (uint64_t)(uintptr_t)path,
                          (uint64_t)(int64_t)flags, 0);
}

int u_close(int fd)
{
    return (int)u_syscall(SYS_CLOSE, (uint64_t)(int64_t)fd, 0, 0);
}

int u_readdir(unsigned index, struct dirent *out)
{
    return (int)u_syscall(SYS_READDIR, (uint64_t)index,
                          (uint64_t)(uintptr_t)out, 0);
}

int u_getkey(void)
{
    return (int)u_syscall(SYS_GETKEY, 0, 0, 0);
}

unsigned long u_uptime_ms(void)
{
    return (unsigned long)u_syscall(SYS_UPTIME_MS, 0, 0, 0);
}

void u_clear(void)
{
    u_syscall(SYS_CLEAR, 0, 0, 0);
}

long u_spawn(const char *image, unsigned long arg)
{
    return u_syscall(SYS_SPAWN, (uint64_t)(uintptr_t)image, (uint64_t)arg, 0);
}

/* --- Buffered console output --------------------------------------- */

/*
 * Output is buffered because the alternative is a system call per
 * character. A system call is cheap -- an INT, a stack switch and a
 * return -- but it is not free, and a program that prints a directory
 * listing would make a thousand of them.
 *
 * The buffer is flushed at the end of every public call, so a program
 * that writes and then blocks on input sees its own prompt.
 */
#define OUT_BUFFER_SIZE 512

static char   g_out[OUT_BUFFER_SIZE];
static size_t g_out_len;

void uflush(void)
{
    size_t pending = g_out_len;
    g_out_len = 0;

    if (pending)
        u_write(STDOUT_FILENO, g_out, pending);
}

static void out_char(void *ctx, char c)
{
    (void)ctx;

    if (g_out_len == OUT_BUFFER_SIZE)
        uflush();

    g_out[g_out_len++] = c;
}

void uputc(char c)
{
    out_char(NULL, c);
    uflush();
}

void uputs(const char *s)
{
    while (*s)
        out_char(NULL, *s++);
    uflush();
}

void uputsln(const char *s)
{
    uputs(s);
    uputc('\n');
}

void uprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvformat(out_char, NULL, fmt, ap);
    va_end(ap);

    uflush();
}
