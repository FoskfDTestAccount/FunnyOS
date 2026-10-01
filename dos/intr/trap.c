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
     * cases worth naming rather than discovering, because both need a
     * guest to hand-write bytes no assembler emits:
     *
     *   - a prefix in front of the trap (F3 FE 38, or a segment override),
     *     where the instruction really began one or two bytes earlier;
     *   - a register-form ModRM other than 38, where a displacement would
     *     sit between the opcode and the service byte.
     *
     * Neither is reachable from the stub. If the dispatcher's own
     * recording is ever removed, this becomes the only answer and those
     * two cases become the whole of the difference.
     */
    cpu->insn_ip = (uint16_t)(cpu->ip - 2u);

    uint8_t service = vm86_fetch8(cpu);

    /*
     * A vector with no service is not an error and not a diagnostic. It
     * is firmware answering a call it has no handler for, which is what a
     * real BIOS does for most of the table: the stub's IRET runs and the
     * program carries on.
     */
    if (g_service[service])
        g_service[service](cpu, g_ctx[service]);

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
