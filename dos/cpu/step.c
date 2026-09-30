#include <vm86/ops.h>

#include <stddef.h>

#include <vm86/mem.h>

/* ------------------------------------------------------------------ */
/* The merged dispatch table                                           */
/* ------------------------------------------------------------------ */

static vm86_op_fn       g_table[256];
static const char      *g_owner[256];   /* which group owns each slot */
static bool             g_built;
static vm86_conflict_fn g_conflict_fn;
static void            *g_conflict_ctx;

void vm86_set_conflict_reporter(vm86_conflict_fn fn, void *ctx)
{
    g_conflict_fn  = fn;
    g_conflict_ctx = ctx;
}

/*
 * Merge one group's table in, reporting anything it claims that is
 * already spoken for.
 *
 * The report is the point. Two groups claiming the same opcode because
 * somebody misread the map is the characteristic failure of splitting
 * this work up, and without a check it is silent: whichever table was
 * merged last simply wins, and the instruction produces the wrong answer
 * only for the encoding that was taken.
 *
 * Naming *which two* groups is the other half of that, and it is the
 * half that is easy to leave out. The name of the group being merged in
 * is not enough on its own: the question a reader has is who else wanted
 * the slot, and a report that says a group collided with itself sends
 * them looking for a mistake that is not there. So the owner of each
 * slot is remembered as the tables go in, and a conflict names both.
 */
static bool install(const vm86_op_fn *from, const char *name, bool *conflict)
{
    for (int i = 0; i < 256; i++) {
        if (!from[i])
            continue;

        if (g_table[i]) {
            if (g_conflict_fn)
                g_conflict_fn(g_conflict_ctx, (uint8_t)i, g_owner[i], name);
            *conflict = true;
            continue;
        }

        g_table[i] = from[i];
        g_owner[i] = name;
    }

    return true;
}

bool vm86_ops_build(void)
{
    bool conflict = false;

    for (int i = 0; i < 256; i++) {
        g_table[i] = NULL;
        g_owner[i] = NULL;
    }

    install(vm86_ops_alu, "alu", &conflict);
    install(vm86_ops_mov, "mov", &conflict);
    install(vm86_ops_str, "str", &conflict);
    install(vm86_ops_ctl, "ctl", &conflict);
    install(vm86_ops_186, "186", &conflict);

    g_built = !conflict;
    return g_built;
}

const vm86_op_fn *vm86_ops_table(void)
{
    return g_built ? g_table : NULL;
}

/* ------------------------------------------------------------------ */
/* The run loop                                                        */
/* ------------------------------------------------------------------ */

/*
 * The prefix bytes.
 *
 * They are handled here rather than as table entries because they do not
 * execute anything: a prefix changes how the instruction after it is
 * decoded and is then forgotten. Handling them in the loop also makes
 * repetitions work for free -- a program can write F3 F3 F3 AA and the
 * hardware will accept it, so this does too.
 */
static bool apply_prefix(struct vm86_cpu *cpu, uint8_t byte)
{
    switch (byte) {
    case 0x26: cpu->prefix.segment = VM86_ES; return true;
    case 0x2E: cpu->prefix.segment = VM86_CS; return true;
    case 0x36: cpu->prefix.segment = VM86_SS; return true;
    case 0x3E: cpu->prefix.segment = VM86_DS; return true;

    /*
     * LOCK.
     *
     * Accepted and ignored. There is exactly one processor here, so the
     * memory ordering it asks for is already guaranteed -- and a program
     * that emits it is asking for something this machine cannot fail to
     * provide. Rejecting it would break software for no reason; honouring
     * it would mean doing nothing more slowly.
     */
    case 0xF0: cpu->prefix.lock = true; return true;

    case 0xF2:
    case 0xF3:
        cpu->prefix.repeat = byte;
        return true;

    default:
        return false;
    }
}

enum vm86_result vm86_step(struct vm86_cpu *cpu)
{
    /*
     * Prefixes belong to exactly one instruction. Clearing them here --
     * rather than at the end of the previous instruction -- means a
     * handler that returns early, or one that faults, cannot leave a
     * segment override behind to silently apply to whatever runs next.
     */
    cpu->prefix.segment = VM86_NO_SEGMENT;
    cpu->prefix.repeat  = 0;
    cpu->prefix.lock    = false;

    uint8_t opcode;

    for (;;) {
        opcode = vm86_fetch8(cpu);
        if (!apply_prefix(cpu, opcode))
            break;
    }

    cpu->insn_count++;

    if (!g_built)
        vm86_ops_build();

    vm86_op_fn handler = g_table[opcode];

    if (!handler) {
        /*
         * Nothing claims this opcode, so this processor does not have
         * that instruction. Raising the invalid-opcode exception is the
         * right answer: it is what the hardware does, and a program that
         * probes for a 186 by trying one of its instructions relies on
         * getting a clean refusal rather than a wrong result.
         */
        cpu->fault = VM86_VECTOR_INVALID_OPCODE;
        return VM86_FAULT;
    }

    return handler(cpu, opcode);
}
