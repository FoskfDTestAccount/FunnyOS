/*
 * Interrupt Descriptor Table.
 */
#ifndef FUNNYOS_ARCH_X86_64_IDT_H
#define FUNNYOS_ARCH_X86_64_IDT_H

#include <stdint.h>

#define IDT_VECTOR_COUNT 256

/* Install the IDT and enable it with lidt(). */
void idt_init(void);

/* Register a handler for one vector.
 *
 * `ist` selects an Interrupt Stack Table slot (0 = keep using the current
 * stack, 1..7 = the stack configured in the TSS). `dpl` must be 3 for
 * vectors that Ring 3 code is allowed to raise with INT n, and 0
 * otherwise -- leaving it at 0 for everything the kernel does not
 * explicitly expose keeps user code from invoking kernel handlers. */
void idt_set_handler(uint8_t vector, void (*handler)(void), uint8_t ist,
                     uint8_t dpl);

/*
 * Change the privilege level of a vector that is already installed.
 *
 * Raising a vector's DPL to 3 is what lets Ring 3 code reach it with
 * INT n. It is a separate call from installation because the two are
 * genuinely different decisions: every vector gets a handler, and only
 * the ones deliberately exposed to user code get DPL 3.
 */
void idt_set_dpl(uint8_t vector, uint8_t dpl);

#endif /* FUNNYOS_ARCH_X86_64_IDT_H */
