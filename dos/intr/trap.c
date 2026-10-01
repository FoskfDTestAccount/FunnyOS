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
     * The dispatcher records this for every instruction, and this looks
     * redundant until you notice what is being defended against: a
     * service that retries rewinds to this address, and if it were zero
     * the guest would jump to address zero and run whatever is there.
     * Setting it here costs one store on a path that is already doing a
     * memory read and a call, and it makes the trap correct on its own
     * rather than correct only in combination with a line in another
     * file.
     *
     * The two bytes of FE 38 are behind us by the time this runs: the ModRM
     * was fetched before the group handler looked at it. So the trap began
     * two bytes back. That only holds for the register-form ModRM the stub
     * uses; a guest that emitted FE /7 with a displacement would get a
     * rewind into the middle of its own instruction, which is a thing no
     * assembler can produce and therefore not a thing worth carrying
     * offsets around for.
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
}
