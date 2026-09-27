/*
 * Global Descriptor Table and Task State Segment.
 *
 * Limine already hands the kernel a working GDT (64-bit code at 0x28,
 * 64-bit data at 0x30), and a plain IDT would work with it. We install
 * our own anyway for two reasons:
 *
 *   - User-mode segments. M2 needs DPL3 code and data descriptors to run
 *     the shell and, later, the DOS emulator in Ring 3.
 *
 *   - Interrupt Stack Tables. Without a TSS there is nowhere to put an
 *     IST, and without an IST a fault that occurs while the stack is
 *     already broken (stack overflow, or a fault inside the fault
 *     handler) escalates to a double fault, then a triple fault, and the
 *     machine reboots with no diagnostic at all. That is exactly the
 *     failure mode M1 exists to eliminate.
 */
#ifndef FUNNYOS_ARCH_X86_64_GDT_H
#define FUNNYOS_ARCH_X86_64_GDT_H

#include <stdint.h>

/* Selector values. Index * 8, plus the requested privilege level in the
 * low two bits. */
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE   0x18
#define GDT_USER_DATA   0x20
#define GDT_TSS         0x28

/* IST slots. IST1 is reserved for the double fault handler and IST2 for
 * NMI, so that both survive a corrupted kernel stack. */
#define IST_DOUBLE_FAULT 1
#define IST_NMI          2

/* Install the GDT and TSS, then reload every segment register. */
void gdt_init(void);

/* Set the kernel stack the CPU switches to when an interrupt arrives
 * while executing in Ring 3. Takes a pointer to the *top* of the stack
 * (stacks grow downwards). */
void tss_set_kernel_stack(uint64_t rsp0);

/* Address of the TSS, for diagnostics. */
uint64_t tss_address(void);

#endif /* FUNNYOS_ARCH_X86_64_GDT_H */
