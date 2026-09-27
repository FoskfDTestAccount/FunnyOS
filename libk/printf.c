#include <libk/printf.h>
#include <libk/string.h>

#include <stdint.h>

/* 数字输出的临时缓冲。64 位二进制最长 64 字符，留足空间。 */
#define NUMBUF_SIZE 72

static void emit_padding(putchar_fn out, void *ctx, char pad, int count)
{
    while (count-- > 0)
        out(ctx, pad);
}

/*
 * 无符号整数转字符串。base 取 2/8/10/16。
 * 返回写入缓冲的字符数，字符串右对齐放在缓冲尾部。
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

    /* tmp 中是个位的逆序，反转进调用者缓冲的尾部 */
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

        /* 解析标志 */
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

        /* 解析宽度 */
        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }

        /* 解析长度修饰符 */
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

        case 'c': {
            /* 字符走通用路径：先放进 cbuf */
            cbuf[0] = (char)va_arg(ap, int);
            cbuf[1] = '\0';
            str = cbuf;
            slen = 1;
            total = 1;
            padchar = ' ';
            break;
        }

        case 'd':
        case 'i': {
            long long v = is_long ? va_arg(ap, long long) : (long long)va_arg(ap, int);
            unsigned long long mag;
            if (v < 0) {
                prefix = '-';
                mag = (unsigned long long)(-(v + 1)) + 1ULL; /* 避免 INT64_MIN 溢出 */
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
            prefix = 'x'; /* 配合下面的 '0' 输出 "0x" */
            break;
        }

        case '%':
            out(ctx, '%');
            continue;

        default:
            /* 未知转换：原样输出，便于发现格式化串写错 */
            out(ctx, '%');
            out(ctx, *p);
            continue;
        }

        int padding = width > total ? width - total : 0;

        if (!left_align) {
            if (padchar == '0' && prefix) {
                /* 零填充时符号/前缀必须在填充之前 */
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
