/*
 * Interrupt delivery.
 *
 * vm86_interrupt() is the whole of what an interrupt *is* on this
 * processor: push a frame, and vector through the table. Three things
 * call it and they differ only in when -- the INT instruction, the run
 * loop delivering a hardware interrupt, and the run loop delivering an
 * exception the guest hooked. Written out three times they would be three
 * chances to disagree about the order of the pushes, and the disagreement
 * would surface as a program that returns to the wrong place only when it
 * was interrupted at the wrong moment.
 *
 * This file deliberately does not decide *whether* to deliver. The INT
 * instruction is never gated on anything; a hardware interrupt is gated on
 * IF and on the shadow; an exception is gated on whether the guest hooked
 * the vector. Those are three different questions and each caller answers
 * its own.
 */
#include <vm86/host.h>

#include <vm86/ops.h>

void vm86_interrupt(struct vm86_cpu *cpu, uint8_t vector)
{
    /*
     * FLAGS first, and it is pushed *before* IF is cleared. The copy on
     * the stack has to carry the caller's IF, because IRET restores FLAGS
     * from it and a handler that returned with interrupts disabled would
     * be a machine that stops taking them the first time one arrives.
     */
    vm86_push16(cpu, cpu->flags);
    vm86_push16(cpu, cpu->cs);
    vm86_push16(cpu, cpu->ip);

    uint32_t entry = (uint32_t)vector * 4u;

    uint16_t offset  = vm86_mem_read16(cpu->mem, entry);
    uint16_t segment = vm86_mem_read16(cpu->mem, entry + 2u);

    vm86_set_seg(cpu, VM86_CS, segment);
    cpu->ip = offset;

    /*
     * Remember where the frame is, so that a service reached through the
     * stub can write its flags back into the right place. See the note on
     * the field: the trap cannot derive this, because the same stub is
     * reached by chains that push a different number of words.
     */
    cpu->intr_frame_sp = cpu->sp;
    cpu->intr_frame_ss = cpu->ss;
    if(cpu->intr_depth<64) {
        cpu->intr_frames[cpu->intr_depth].sp=cpu->sp;
        cpu->intr_frames[cpu->intr_depth].ss=cpu->ss;
    }
    cpu->intr_depth++;

    vm86_flag_set(cpu, VM86_IF, false);
    vm86_flag_set(cpu, VM86_TF, false);

    /*
     * cpu->halted is left alone on purpose.
     *
     * Clearing it here would look tidier and would be wrong in one case:
     * vm86_interrupt() is also how the timer handler chains to INT 1Ch,
     * and a service running that chain is not waking a halted processor
     * -- it *is* the interrupt. The run loop clears halted at the one
     * place where waking is what is happening.
     */
}

void vm86_raise(struct vm86_cpu *cpu, uint8_t vector)
{
    /*
     * OR, not assignment. A vector raised while it is already pending
     * stays raised once: a line asserted twice before its interrupt has
     * been taken is one interrupt, which is what the 8259 does and what
     * keeps a device that is faster than the guest from building a
     * backlog the guest can never work off.
     */
    cpu->intr_pending[vector >> 3] |= (uint8_t)(1u << (vector & 7u));
}

void vm86_clear_pending(struct vm86_cpu *cpu, uint8_t vector)
{
    cpu->intr_pending[vector >> 3] &= (uint8_t)~(1u << (vector & 7u));
}

int vm86_next_pending(const struct vm86_cpu *cpu)
{
    for (unsigned byte = 0; byte < sizeof(cpu->intr_pending); byte++) {
        uint8_t bits = cpu->intr_pending[byte];

        if (!bits)
            continue;

        /* The lowest set bit, which is the lowest vector in this byte. */
        unsigned bit = 0;
        while (!(bits & (1u << bit)))
            bit++;

        return (int)(byte * 8u + bit);
    }

    return -1;
}

bool vm86_interruptible(const struct vm86_cpu *cpu)
{
    /*
     * The shadow before the pending check, because it is the cheaper test
     * and because it is true far more often -- it is one byte against a
     * thirty-two byte scan.
     */
    if (cpu->intr_shadow)
        return false;

    if (!vm86_flag_test(cpu, VM86_IF))
        return false;

    return vm86_next_pending(cpu) >= 0;
}
