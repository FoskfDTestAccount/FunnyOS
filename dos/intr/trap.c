/*
 * The host trap: the one instruction a guest can use to reach the host.
 *
 * See the top of host.h for why the encoding is FE 38 and why the stub
 * ends in its own IRET. This file is the receiving end: it reads which
 * service was asked for, calls it, and leaves the instruction pointer on
 * the stub's IRET so that the next step performs the return.
 *
 * ---------------------------------------------------------------------
 * The registry is process-wide
 *
 * A table of two hundred and fifty-six function pointers and their
 * contexts, one per vector. Two hundred and fifty-six pointers is four
 * kilobytes, which is not a thing to put inside a struct vm86_cpu that
 * tests allocate on the stack; and there is one firmware in the machine,
 * so a machine-scoped table would be a per-instance copy of something
 * that never differs between instances.
 *
 * vm86_clear_services() is what a test suite calls between cases. Without
 * it, a service registered by one case is still there for the next one,
 * and the failure it produces -- a vector answering when it should not --
 * reads as a bug in the case that did not register it.
 */
#include <vm86/host.h>

#include <vm86/decode.h>

static vm86_service_fn g_service[256];
static void           *g_ctx[256];

void vm86_register_service(uint8_t vector, vm86_service_fn fn, void *ctx)
{
    g_service[vector] = fn;
    g_ctx[vector]     = ctx;
}

void vm86_clear_services(void)
{
    for (unsigned i = 0; i < 256; i++) {
        g_service[i] = NULL;
        g_ctx[i]     = NULL;
    }
}

/*
 * Put the flags back where the IRET will find them.
 *
 * ---------------------------------------------------------------------
 * Why this is needed at all
 *
 * The stub ends in a real IRET, and IRET loads FLAGS off the frame the
 * guest's INT pushed. So a service that sets a flag in cpu->flags has it
 * silently overwritten on the way out -- the write lands in the register
 * and the IRET immediately reloads the register from memory.
 *
 * That is not a corner. INT 13h reports every failure with the carry
 * flag, and INT 16h AH=01h answers "is a key waiting" with ZF. Both are
 * flags. Without this, an INT 13h that fails looks to the program like
 * one that succeeded, with the error code sitting ignored in AH -- and
 * the failure is invisible to any test that calls the service directly,
 * which is how every service's own suite tests it. It took a guest that
 * branched on the carry to see it.
 *
 * ---------------------------------------------------------------------
 * Why taking the whole register back is safe
 *
 * A service that changes nothing must leave the frame as it found it, or
 * this would corrupt the flags of every existing caller. It does:
 * vm86_interrupt() pushes the caller's FLAGS and then clears IF and TF in
 * the register, so at the moment a service starts, cpu->flags is the
 * caller's flags with exactly those two bits missing -- and the frame
 * holds the two bits. Merging them back reproduces the caller's flags
 * exactly.
 *
 * IF and TF therefore come from the frame rather than from the register.
 * That is what makes this a return value rather than a bypass: a service
 * cannot use it to leave interrupts disabled on the way out, and it
 * cannot arm the single-step trap on a guest that did not ask for one.
 *
 * ---------------------------------------------------------------------
 * Which is only true when there is a frame to write into
 *
 * A stub is reached two ways and they do not leave the same stack. Through
 * an INT there are three words on it -- FLAGS, CS, IP -- and the flags sit
 * at SP+4. Through a TSR that chains in with a far call there are two, and
 * SP+4 is whatever the chaining handler had there; for a chain made from
 * inside an interrupt handler it is the saved instruction pointer of the
 * interrupt that got there first. Writing flags over that corrupts a word
 * the handler was going to use, and the damage surfaces much later as
 * something else being wrong.
 *
 * vm86_interrupt() records the stack pointer it left behind, and this
 * declines to write when the two disagree. A chained call therefore gets
 * no write-back, and that is the direction to fail in: it loses a
 * service's flags rather than gaining a corrupted word.
 *
 * The address is worked out before the service runs rather than after. A
 * service is allowed to deliver another interrupt -- the timer handler
 * chains to 1Ch that way -- and that moves the recorded pointer out from
 * under a check made later.
 */
static bool frame_flags_address(struct vm86_cpu *cpu, uint32_t *out)
{
    if (cpu->sp != cpu->intr_frame_sp)
        return false;

    /* It is the stack, so the segment is SS with no override -- the same
     * addressing the push used. */
    *out = ((uint32_t)vm86_get_seg(cpu, VM86_SS) << 4)
           + (uint16_t)(cpu->sp + 4u);

    return true;
}

static void write_flags_back(struct vm86_cpu *cpu, uint32_t at)
{
    uint16_t frame = vm86_mem_read16(cpu->mem, at);

    uint16_t merged = (uint16_t)((cpu->flags & (uint16_t)~(VM86_IF | VM86_TF))
                                 | (frame & (VM86_IF | VM86_TF)));

    vm86_mem_write16(cpu->mem, at, merged);
}

enum vm86_result vm86_host_trap(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    /*
     * Where this instruction began, for a service that decides it has to
     * run again.
     *
     * The dispatcher records this for every instruction, so this looks
     * redundant -- and it is, for a stub reached with no prefix in front
     * of it, which is every stub this firmware installs. It is here
     * because a service that retries rewinds to this address, and if the
     * dispatcher's line were ever missing the guest would jump to address
     * zero and run whatever is there. One store against a memory read and
     * a call is not worth the alternative.
     *
     * The two bytes of FE 38 are behind us by the time this runs: the
     * ModRM was fetched before the group handler looked at it. So the
     * trap began two bytes back -- and that arithmetic is wrong in two
     * cases worth naming, because a retry rewinds to what this holds and
     * a wrong value sends the guest somewhere no instruction ever was:
     *
     *   - a prefix in front of the trap (F3 FE 38, or a segment override),
     *     where the instruction really began one or two bytes earlier and
     *     this lands on the FE;
     *   - any encoding of this group whose ModRM is not the last byte --
     *     mod = 01 or 10, or mod = 00 with r/m = 6 -- where a displacement
     *     follows it. FE 7F 05 would record the address of the ModRM, and
     *     a retry would rewind onto the 7F and execute it as `jg rel8`.
     *
     * None of it is reachable from the stub, which is always the register
     * form with r/m = 0 and no displacement, and no assembler emits FE /7
     * in any form. If the dispatcher's own recording is ever removed, this
     * becomes the only answer and those encodings become the whole of the
     * difference.
     */
    cpu->insn_ip = (uint16_t)(cpu->ip - 2u);

    /*
     * Where a service's flags would go, worked out now rather than after
     * the call. See the note on frame_flags_address().
     */
    uint32_t flags_at   = 0;
    bool     have_frame = frame_flags_address(cpu, &flags_at);

    uint8_t service = vm86_fetch8(cpu);

    /*
     * A vector with no service is not an error and not a diagnostic. It
     * is firmware answering a call it has no handler for, which is what a
     * real BIOS does for most of the table: the stub's IRET runs and the
     * program carries on.
     *
     * The flags go back whether or not anything ran, and that is not
     * tidiness: a program is entitled to reach an unhandled vector and
     * find its flags exactly as it left them, and going through the merge
     * on every path is one fewer place for that to be got wrong.
     */
    if (g_service[service])
        g_service[service](cpu, g_ctx[service]);

    if (have_frame)
        write_flags_back(cpu, flags_at);

    return VM86_CONTINUE;
}

void vm86_service_retry(struct vm86_cpu *cpu)
{
    /*
     * Nothing else is touched. See the note on the declaration: popping
     * the frame here would resume past the instruction instead of
     * re-running it, and would be wrong for a stub reached by a chained
     * far call rather than by an INT.
     */
    cpu->ip = cpu->insn_ip;

    /*
     * And interrupts go back on.
     *
     * This line is the difference between a working blocking call and a
     * deadlock, and it is not obvious. The INT that got us here cleared
     * IF -- correctly, that is what entering a handler does -- and the
     * run loop refuses to deliver anything while IF is clear. So a
     * service that retries without turning them back on is waiting for a
     * keyboard interrupt the machine is now forbidden to deliver: the
     * buffer stays empty, the guest spins, and nothing ever reports an
     * error. It took a probe that ran the run loop's boundary logic by
     * hand to see it, because reading the two files separately shows
     * nothing wrong with either.
     *
     * Turning them on is what the handler would have done. A real BIOS
     * keyboard routine runs `sti` before it waits, for this exact reason.
     * So this is deliberately not "restore the guest's IF" -- it is a
     * handler deciding to wait with interrupts live, which is a thing a
     * handler may decide, and it overrides a guest that called a blocking
     * read with interrupts disabled.
     */
    vm86_flag_set(cpu, VM86_IF, true);
}
