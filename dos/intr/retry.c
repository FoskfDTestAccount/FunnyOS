/*
 * Undoing an INT so that the guest can try again.
 *
 * ---------------------------------------------------------------------
 * Why this exists at all
 *
 * A blocking BIOS call -- `INT 16h` with AH=00 asking for a keystroke --
 * spins on the real hardware, with interrupts enabled, until the keyboard
 * handler drops a code into its buffer. A host service cannot do that: it
 * is called from the interpreter, and a service that never returns never
 * gives the host its processor back, so the keystroke it is waiting for
 * can never arrive. The wait would be a deadlock of exactly one party.
 *
 * So the wait is moved outside. The service returns having done nothing,
 * the run loop finishes its slice and hands control back, the host feeds
 * the keyboard and raises IRQ1, and when the machine is run again the
 * interrupt is delivered between one attempt at the instruction and the
 * next. The guest really is waiting with interrupts live; what it is not
 * doing is waiting *inside* the handler.
 *
 * ---------------------------------------------------------------------
 * What "undo" means precisely
 *
 * The `INT n` pushed a frame -- FLAGS, CS, IP -- and jumped to the stub.
 * Undoing it means popping that frame back off, which restores the flag
 * register along with the address. Restoring FLAGS is not a detail: the
 * INT cleared IF on entry, so a retry that left the flags alone would
 * leave the guest waiting with interrupts disabled, and the interrupt
 * that is supposed to end the wait could never be delivered. The retry
 * would then repeat forever, which is precisely the failure the whole
 * mechanism exists to avoid -- and it presents as a program that hangs
 * rather than one that is wrong.
 *
 * The address is then wound back to the *instruction*. There are two
 * candidate places it could go to and only one of them works:
 *
 *   - cpu->insn_ip is the start of the instruction currently being
 *     executed, which by now is the stub's `FE 38` and not the INT. Going
 *     there would re-execute the stub with no frame beneath it, and the
 *     stub's own IRET would pop whatever the guest happened to have on
 *     its stack. Wrong, and silently so.
 *
 *   - The address in the frame is the instruction *after* the INT, which
 *     is where the interrupt would have returned to had it completed.
 *     Backing up over the INT itself -- `CD nn`, two bytes -- lands on
 *     the instruction the service wants re-run.
 *
 * The subtraction is hard-coded to two because a blocking BIOS call is
 * always reached by the two-byte form. `CC` and `CE`, the one-byte
 * interrupt encodings, are breakpoint and overflow and are never blocking
 * calls; a service reached through one of those and retried would land a
 * byte short. That is a known limit of this mechanism rather than an
 * oversight, and it is recorded here because the failure mode would be a
 * jump into the middle of an instruction rather than an error message.
 *
 * Nothing is rewound except the address. CX, SI and DI keep whatever the
 * partially executed instruction left in them, and any memory it wrote
 * stays written, because that is what the hardware does: the frame
 * records where to resume, not a snapshot to roll back to.
 */
#include <vm86/host.h>
#include <vm86/ops.h>

void vm86_service_retry(struct vm86_cpu *cpu)
{
    /*
     * The pop order is the reverse of the push order, as it has to be.
     * The frame went on as FLAGS, CS, IP with the stack growing down, so
     * IP is the word nearest the top and comes off first.
     */
    uint16_t ip    = vm86_pop16(cpu);
    uint16_t cs    = vm86_pop16(cpu);
    uint16_t flags = vm86_pop16(cpu);

    vm86_set_seg(cpu, VM86_CS, cs);

    /* A wholesale write to the flag register, so the bits that always
     * read as one have to be forced back. See struct vm86_cpu. */
    cpu->flags = flags;
    vm86_flags_normalise(cpu);

    /* Back over the `INT n` itself. See the note at the top. */
    cpu->ip = (uint16_t)(ip - 2u);
}
