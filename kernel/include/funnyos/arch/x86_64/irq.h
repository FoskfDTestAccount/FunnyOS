/*
 * Interrupt dispatch.
 *
 * All 256 vectors share one entry stub (see isr.asm) which builds a
 * struct interrupt_frame and calls isr_dispatch(). That function splits
 * the vectors into two populations:
 *
 *   - Vectors 0..31 are CPU exceptions. They are never expected, they
 *     never return, and their job is to explain themselves -- see isr.c.
 *
 *   - Vectors 32..255 are device interrupts raised by the local APIC.
 *     They are registered here by the driver that owns the device, and
 *     they return normally.
 *
 * Keeping the two apart matters because they behave in opposite ways: an
 * unhandled exception is a bug that must stop the machine, while an
 * unhandled device interrupt is usually a device the kernel has not
 * claimed yet and must be absorbed quietly.
 */
#ifndef FUNNYOS_ARCH_X86_64_IRQ_H
#define FUNNYOS_ARCH_X86_64_IRQ_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Register state at the moment of interruption.
 *
 * The field order must match what isr_common pushes, followed by what the
 * vector stub and then the CPU leave on the stack. See isr.asm.
 *
 * The trailing rsp/ss are only present when the CPU crossed a privilege
 * level, so on an interrupt from Ring 3 the frame describes the Ring 3
 * stack. Reading them after a Ring 0 interrupt yields whatever happened
 * to be there; check `cs` before trusting them.
 */
struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;

    uint64_t vector;
    uint64_t error_code;

    /* Pushed by the CPU */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

/* Device interrupts start here; everything below is a CPU exception. */
#define IRQ_VECTOR_BASE     32

/*
 * Fixed vector assignments. These live in one place because a vector
 * collision between two drivers produces an interrupt that reaches the
 * wrong handler, which is a hard bug to see: everything looks like it
 * works, just not the thing that was poked.
 */
#define IRQ_VECTOR_TIMER    32   /* LAPIC timer */
#define IRQ_VECTOR_KEYBOARD 33   /* IOAPIC GSI 1, PS/2 keyboard */

typedef void (*irq_handler_fn)(struct interrupt_frame *frame, void *ctx);

/* Clear the handler table. Called before any driver registers. */
void irq_init(void);

/*
 * Claim a vector. Registering a vector that already has a handler fails
 * rather than silently replacing it -- a mistaken double claim should be
 * visible at boot, not manifest as a device that mysteriously stops
 * responding.
 */
bool irq_register(uint8_t vector, irq_handler_fn fn, void *ctx);

bool irq_unregister(uint8_t vector);

/*
 * Called by isr_dispatch() for every vector.
 *
 * Returns true when the vector had a handler, in which case the local
 * APIC has already been told the interrupt is done and the interrupted
 * code will resume. Returns false when the vector is unclaimed, leaving
 * the caller to decide what to do about it.
 *
 * The end-of-interrupt is sent after the handler runs, not before: a
 * handler is allowed to take as long as it needs, and acknowledging
 * first would let the same interrupt fire again on top of itself.
 */
bool irq_dispatch(struct interrupt_frame *frame);

/* Number of times a vector has fired. */
uint64_t irq_count(uint8_t vector);

/* Total across all vectors. */
uint64_t irq_total_count(void);

/*
 * A hook called after every handled device interrupt, once the end of
 * interrupt has been sent.
 *
 * It exists for the one thing a handler cannot do by returning. A handler
 * can change the register state the interrupted code will resume with --
 * that is how a system call returns a value -- but it cannot leave that
 * context entirely, because the only way out of an interrupt is the iretq
 * at the bottom of the entry stub.
 *
 * Ending a process needs exactly that. A process that exits must not
 * resume; it must unwind to whoever started it. So the syscall handler
 * sets a flag, returns normally, and this hook sees the flag and does not
 * return.
 *
 * It runs with the interrupt still logically in service, which is why the
 * end of interrupt is sent before it rather than after: an interrupt
 * acknowledged after the CPU has been diverted elsewhere is an interrupt
 * that stays in service forever.
 */
typedef void (*irq_post_hook_fn)(void);
void irq_set_post_hook(irq_post_hook_fn hook);

#endif /* FUNNYOS_ARCH_X86_64_IRQ_H */
