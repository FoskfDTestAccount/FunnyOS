/*
 * x86-64 端口 I/O 与底层内联汇编封装
 */
#ifndef FUNNYOS_ARCH_X86_64_IO_H
#define FUNNYOS_ARCH_X86_64_IO_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t result;
    __asm__ volatile("inb %1, %0" : "=a"(result) : "Nd"(port) : "memory");
    return result;
}

static inline void io_wait(void)
{
    /* 向未使用的端口 0x80 写 0 是一种标准的、约 1 微秒的延迟手段 */
    outb(0x80, 0);
}

static inline void cpu_halt(void)
{
    __asm__ volatile("hlt");
}

static inline void interrupts_enable(void)
{
    __asm__ volatile("sti");
}

static inline void interrupts_disable(void)
{
    __asm__ volatile("cli");
}

#endif /* FUNNYOS_ARCH_X86_64_IO_H */
