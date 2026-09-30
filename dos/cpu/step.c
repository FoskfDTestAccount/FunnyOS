#include <vm86/ops.h>

#include <stddef.h>

#include <vm86/mem.h>

/* ------------------------------------------------------------------ */
/* The merged dispatch table                                           */
/* ------------------------------------------------------------------ */

static vm86_op_fn       g_table[256];
static bool             g_built;
static bool             g_merge_attempted;
static vm86_conflict_fn g_conflict_fn;
static void            *g_conflict_ctx;

void vm86_set_conflict_reporter(vm86_conflict_fn fn, void *ctx)
{
    g_conflict_fn  = fn;
    g_conflict_ctx = ctx;
}

/*
 * Merge the groups in, reporting anything two of them both claim.
 *
 * The report is the point. Two groups claiming the same opcode because
 * somebody misread the map is the characteristic failure of splitting
 * this work up, and without a check it is silent: whichever table was
 * merged in last simply wins, and the instruction produces the wrong
 * answer only for the encoding that was taken.
 *
 * Naming *which two* groups is the other half of that, and it is the
 * half that is easy to leave out. The name of the group being merged in
 * is not enough on its own: the question a reader has is who else wanted
 * the slot, and a report that says a group collided with itself sends
 * them looking for a mistake that is not there. So the owner of each slot
 * is remembered as the tables go in, and a conflict names both.
 *
 * The first claim keeps a contested slot. That is a decision rather than
 * a fact -- the table is broken either way -- but it is the decision that
 * leaves something runnable behind, and the caller is told by the return
 * value and by the reporter that what it has is not what it asked for.
 */
bool vm86_ops_merge(vm86_op_fn *out, const vm86_op_fn *const *groups,
                    const char *const *names, size_t count)
{
    const char *owner[256];
    bool        ok = true;

    for (int i = 0; i < 256; i++) {
        out[i]   = NULL;
        owner[i] = NULL;
    }

    for (size_t g = 0; g < count; g++) {
        for (int i = 0; i < 256; i++) {
            if (!groups[g][i])
                continue;

            if (out[i]) {
                if (g_conflict_fn)
                    g_conflict_fn(g_conflict_ctx, (uint8_t)i, owner[i], names[g]);
                ok = false;
                continue;
            }

            out[i]   = groups[g][i];
            owner[i] = names[g];
        }
    }

    return ok;
}

/* The five groups, in the order they are merged -- which is the order
 * that settles a contested slot, though nothing should be relying on it. */
static const vm86_op_fn *const g_groups[] = {
    vm86_ops_alu, vm86_ops_mov, vm86_ops_str, vm86_ops_ctl, vm86_ops_186,
};

static const char *const g_group_names[] = {
    "alu", "mov", "str", "ctl", "186",
};

bool vm86_ops_build(void)
{
    g_built = vm86_ops_merge(g_table, g_groups, g_group_names,
                             sizeof(g_groups) / sizeof(g_groups[0]));

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

    /*
     * The table is merged once, not once per instruction.
     *
     * vm86_ops_build() answers false when two groups claim the same
     * opcode, and trying again cannot change that answer: the five tables
     * are const. Trying again every instruction would run the whole
     * 1280-slot merge once per instruction executed, which turns a table
     * mistake into a machine that is orders of magnitude too slow and
     * still running the wrong handler for the opcode that was contested.
     *
     * A failure is not swallowed. The reporter installed by
     * vm86_set_conflict_reporter() was called with the opcode and both
     * group names at the moment it was found, and vm86_ops_table()
     * answers NULL from then on -- which is what an embedder that wants
     * to refuse to run checks.
     */
    if (!g_built && !g_merge_attempted) {
        g_merge_attempted = true;
        vm86_ops_build();
    }

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
