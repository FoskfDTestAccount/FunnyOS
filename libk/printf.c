#include <libk/printf.h>
#include <libk/string.h>

#include <stdint.h>

/* Scratch buffer for numeric output. A 64-bit binary number is at most
 * 64 digits, so this leaves generous headroom. */
#define NUMBUF_SIZE 72

static void emit_padding(putchar_fn out, void *ctx, char pad, int count)
{
    while (count-- > 0)
        out(ctx, pad);
}

/*
 * Convert an unsigned integer to a string in the given base (2/8/10/16).
 * Returns the digit count; the string is right-aligned within buf.
 */
static int utoa(unsigned long long value, unsigned base, int uppercase,
                char *buf, int bufsize)
{
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[NUMBUF_SIZE];
    int n = 0;

    if (value == 0) {
        tmp[n++] = '0';
    } else {
        while (value > 0) {
            tmp[n++] = digits[value % (unsigned long long)base];
            value /= (unsigned long long)base;
        }
    }

    /* tmp holds the digits in reverse; flip them into the tail of the
     * caller's buffer. */
    if (n > bufsize)
        n = bufsize;
    for (int i = 0; i < n; i++)
        buf[bufsize - 1 - i] = tmp[i];
    for (int i = 0; i < bufsize - n; i++)
        buf[i] = ' ';

    return n;
}

void kvformat(putchar_fn out, void *ctx, const char *fmt, va_list ap)
{
    char numbuf[NUMBUF_SIZE];
    char cbuf[2];

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            out(ctx, *p);
            continue;
        }

        p++;
        if (*p == '\0')
            break;

        /* Parse flags */
        int left_align = 0;
        int zero_pad = 0;
        for (;;) {
            if (*p == '-') {
                left_align = 1;
                p++;
            } else if (*p == '0') {
                zero_pad = 1;
                p++;
            } else {
                break;
            }
        }

        /* Parse width */
        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }

        /* Parse length modifiers. This implementation treats 'l', 'll'
         * and 'z' identically: all promote to 64-bit, which is what the
         * x86-64 calling convention passes in a single register anyway. */
        int is_long = 0;
        while (*p == 'l' || *p == 'z') {
            if (*p == 'l')
                is_long = 1;
            p++;
        }

        const char *str = NULL;
        int         slen = 0;
        char        padchar = zero_pad && !left_align ? '0' : ' ';
        char        prefix = '\0';
        int         total = 0;

        switch (*p) {
        case 's':
            str = va_arg(ap, const char *);
            if (!str)
                str = "(null)";
            slen = (int)strlen(str);
            total = slen;
            break;

        case 'c':
            /* Characters take the generic path via cbuf. */
            cbuf[0] = (char)va_arg(ap, int);
            cbuf[1] = '\0';
            str = cbuf;
            slen = 1;
            total = 1;
            padchar = ' ';
            break;

        case 'd':
        case 'i': {
            long long v = is_long ? va_arg(ap, long long) : (long long)va_arg(ap, int);
            unsigned long long mag;
            if (v < 0) {
                prefix = '-';
                /* Negate via -(v+1)+1 so that INT64_MIN cannot overflow. */
                mag = (unsigned long long)(-(v + 1)) + 1ULL;
            } else {
                mag = (unsigned long long)v;
            }
            int n = utoa(mag, 10, 0, numbuf, NUMBUF_SIZE);
            str = numbuf + (NUMBUF_SIZE - n);
            slen = n;
            total = n + (prefix ? 1 : 0);
            break;
        }

        case 'u': {
            unsigned long long v = is_long ? va_arg(ap, unsigned long long)
                                           : (unsigned long long)va_arg(ap, unsigned int);
            int n = utoa(v, 10, 0, numbuf, NUMBUF_SIZE);
            str = numbuf + (NUMBUF_SIZE - n);
            slen = n;
            total = n;
            break;
        }

        case 'x':
        case 'X': {
            unsigned long long v = is_long ? va_arg(ap, unsigned long long)
                                           : (unsigned long long)va_arg(ap, unsigned int);
            int n = utoa(v, 16, *p == 'X', numbuf, NUMBUF_SIZE);
            str = numbuf + (NUMBUF_SIZE - n);
            slen = n;
            total = n;
            break;
        }

        case 'p': {
            unsigned long long v = (unsigned long long)(uintptr_t)va_arg(ap, void *);
            int n = utoa(v, 16, 0, numbuf, NUMBUF_SIZE);
            str = numbuf + (NUMBUF_SIZE - n);
            slen = n;
            total = n + 2;
            prefix = 'x'; /* combined with the '0' below to print "0x" */
            break;
        }

        case '%':
            out(ctx, '%');
            continue;

        default:
            /* Unknown conversion: echo it verbatim, which makes a typo
             * in a format string immediately visible. */
            out(ctx, '%');
            out(ctx, *p);
            continue;
        }

        int padding = width > total ? width - total : 0;

        if (!left_align) {
            if (padchar == '0' && prefix) {
                /* With zero padding the sign or prefix must come before
                 * the padding, not after it. */
                if (prefix == 'x') {
                    out(ctx, '0');
                    out(ctx, 'x');
                } else {
                    out(ctx, prefix);
                }
                prefix = '\0';
                total--;
            }
            emit_padding(out, ctx, padchar, padding);
        }

        if (prefix) {
            if (prefix == 'x') {
                out(ctx, '0');
                out(ctx, 'x');
            } else {
                out(ctx, prefix);
            }
        }

        for (int i = 0; i < slen; i++)
            out(ctx, str[i]);

        if (left_align)
            emit_padding(out, ctx, ' ', padding);
    }
}

void kformat(putchar_fn out, void *ctx, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvformat(out, ctx, fmt, ap);
    va_end(ap);
}
