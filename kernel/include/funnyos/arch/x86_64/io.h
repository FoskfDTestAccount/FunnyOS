/*
 * x86-64 port I/O and low-level inline assembly helpers.
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
    /* Writing to the unused port 0x80 is the standard way to burn
     * roughly one microsecond of I/O bus time. */
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
