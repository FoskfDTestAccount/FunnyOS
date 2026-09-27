/*
 * 内核格式化输出
 *
 * 所有内核输出都经过这一层，不直接调用 serial_putc。这样后续接入
 * 帧缓冲控制台时只需改这一处，调用方不用动。
 */
#ifndef FUNNYOS_KPRINTF_H
#define FUNNYOS_KPRINTF_H

/* 输出单个字符到当前输出后端 */
void kputc(char c);

/* 输出字符串 */
void kputs(const char *s);

/* 格式化输出 */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* FUNNYOS_KPRINTF_H */
