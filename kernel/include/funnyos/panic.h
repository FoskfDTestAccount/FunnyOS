/*
 * 内核致命错误处理
 *
 * M0 阶段没有 IDT，任何异常都会三重故障并重启机器，
 * 因此 panic() 只能在软件检出错误时主动调用（例如引导协议版本不符）。
 * M1 接入 IDT 后，异常处理程序也会汇入这里。
 */
#ifndef FUNNYOS_PANIC_H
#define FUNNYOS_PANIC_H

/* 打印诊断信息后永久停机。不会返回。 */
void panic(const char *fmt, ...)
    __attribute__((noreturn, format(printf, 1, 2)));

#endif /* FUNNYOS_PANIC_H */
