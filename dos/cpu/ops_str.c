/*
 * The string instructions.
 *
 * ---------------------------------------------------------------------
 * Owned opcodes
 *
 *   A4  MOVSB   A5  MOVSW     move DS:SI to ES:DI
 *   A6  CMPSB   A7  CMPSW     compare DS:SI with ES:DI
 *   AA  STOSB   AB  STOSW     store AL/AX to ES:DI
 *   AC  LODSB   AD  LODSW     load DS:SI into AL/AX
 *   AE  SCASB   AF  SCASW     compare AL/AX with ES:DI
 *
 * Ten opcodes and the smallest file in the set by count, which is
 * misleading: these are the instructions with the most behaviour behind
 * them, because every one of them is also a REP loop.
 *
 * ---------------------------------------------------------------------
 * What makes these different from everything else here
 *
 * A string instruction is a loop the hardware writes for you. Executing
 * `rep movsb` with CX=1000 is one instruction dispatch that moves a
 * kilobyte, and the run loop only sees it return once.
 *
 * The prefixes arrive in cpu->prefix.repeat:
 *
 *   0xF3   REP, or REPE when the instruction is CMPS or SCAS
 *   0xF2   REPNE, which only means anything for CMPS and SCAS
 *
 * The trap is that REP and REPE are the SAME BYTE. Which one it means is
 * decided by the instruction: for MOVS, STOS and LODS the count in CX is
 * all that matters and the zero flag is irrelevant; for CMPS and SCAS the
 * loop also stops when the comparison stops satisfying ZF. A handler that
 * treats them as one thing will run a `repe cmpsb` to the end of the
 * buffer instead of stopping at the first difference, which is exactly
 * what the program was using it to find.
 *
 * Two more things that are easy to miss:
 *
 *   - The direction flag. DF=0 runs forward and increments SI and DI;
 *     DF=1 runs backward and decrements them. Both are legal and both are
 *     used. Getting this backwards produces a program that appears to
 *     work on buffers that happen to be symmmetric.
 *
 *   - CX=0. A REP with no count executes zero times -- not one, and not
 *     forever. The check has to come before the first iteration, not
 *     after it.
 *
 * And the segment rules are not uniform: LODS, MOVS and CMPS read through
 * DS and can be overridden by a prefix; STOS and SCAS read only through
 * ES, which cannot be overridden; and every one of them writes through
 * ES, which likewise cannot. SI is the DS-side pointer, DI is the ES-side
 * one, always, whichever way round the operands read.
 *
 * ---------------------------------------------------------------------
 * The handover: vm86_str_repeat()
 *
 * INS and OUTS (0x6C-0x6F, ops_186.c) are string instructions as well:
 * the same REP prefix, the same direction flag, the same CX=0 rule, the
 * same "the DS side takes a segment override and the ES side never
 * does". Two implementations of that would not stay equal, and the bug
 * they would produce is a machine where `rep movsb` works and `rep insb`
 * copies one byte -- which is the sort of thing that gets blamed on the
 * DOS program.
 *
 * So the loop is not private to this file. It is vm86_str_repeat(), it
 * is declared in ops.h with the callback it takes, and ops_186.c calls
 * it with its own two bodies. The contract lives there, once; what is
 * written in this file is the part of it that is easy to get wrong
 * rather than easy to read, and it sits next to the code that depends
 * on it.
 *
 * ---------------------------------------------------------------------
 * How this file is arranged
 *
 * One loop, five bodies. The byte and word forms of each instruction
 * share a body: the width is a parameter, and bit 0 of the opcode is the
 * only thing that selects the form.
 */
#include <vm86/ops.h>

/* ------------------------------------------------------------------ */
/* The one loop                                                        */
/* ------------------------------------------------------------------ */

/*
 * Which way the pointers move, from the direction flag.
 *
 * The step is the operand width, not one: a word operation moves SI and
 * DI by two. The sum is taken in the host's int and cast back down so
 * that the register wraps at sixteen bits the way the hardware does.
 */
static int16_t str_delta(const struct vm86_cpu *cpu, uint8_t bits)
{
    int step = bits / 8;

    return (int16_t)(vm86_flag_test(cpu, VM86_DF) ? -step : step);
}

/* Memory, through a segment the caller has already resolved. The linear
 * address is worked out per access rather than kept and stepped: it is
 * segment base plus offset either way, and a cached address that has to
 * be re-wrapped when the offset rolls over the end of a segment is a bug
 * waiting for a buffer that ends at 0xFFFF. */
static uint16_t str_load(struct vm86_cpu *cpu, enum vm86_seg seg,
                         uint16_t offset, uint8_t bits)
{
    uint32_t linear = vm86_linear(cpu, seg, offset);

    if (bits == 8)
        return vm86_mem_read8(cpu->mem, linear);

    return vm86_mem_read16(cpu->mem, linear);
}

static void str_store(struct vm86_cpu *cpu, enum vm86_seg seg,
                      uint16_t offset, uint8_t bits, uint16_t value)
{
    uint32_t linear = vm86_linear(cpu, seg, offset);

    if (bits == 8)
        vm86_mem_write8(cpu->mem, linear, (uint8_t)value);
    else
        vm86_mem_write16(cpu->mem, linear, value);
}

/*
 * The loop, exported. The comments inside it are the parts of the
 * contract that are easy to get wrong rather than easy to read, so they
 * are stated where the code that depends on them is.
 */
enum vm86_result vm86_str_repeat(struct vm86_cpu *cpu, uint8_t bits,
                                 vm86_str_body body, bool conditional)
{
    int16_t delta = str_delta(cpu, bits);

    /*
     * The DS side, with any override applied, worked out once for the
     * whole loop.
     *
     * The ES side is reached directly by the bodies, never through
     * vm86_effective_seg(). That is the whole of the segment rules: a
     * prefix moves the *read* -- `es: movsb` reads ES:SI -- and there is
     * no prefix on this processor that makes a string write land
     * anywhere but ES:DI. Asking for "the effective segment" of the
     * write would turn `ds: stosb` into a store to the data segment,
     * which is not an instruction the hardware has.
     */
    enum vm86_seg source = vm86_effective_seg(cpu, VM86_DS);

    if (cpu->prefix.repeat == 0) {
        body(cpu, bits, delta, source);
        return VM86_CONTINUE;
    }

    /*
     * F2 counts as F3 for the instructions that have no comparison.
     *
     * The manual does not define `repne movs`, and there is nothing for
     * "not equal" to mean where no comparison happens. This treats it as
     * REP, which is the reading every part since has taken; the
     * alternative -- ignoring the prefix and executing once -- would
     * silently copy one byte of a buffer instead of the whole thing, and
     * that is a much worse way to be wrong. Tested in test_str.c and
     * named in the report, because it is a decision rather than a fact.
     */
    bool stop_when_zf_set = (cpu->prefix.repeat == 0xF2);

    /* The test is here, before the first iteration, and not after it:
     * CX=0 must execute zero times. Written as a do/while the count would
     * wrap to 0xFFFF on the first pass and move sixty-five thousand
     * elements -- which is the classic REP bug, and the reason this
     * comment is longer than the loop. */
    while (cpu->cx != 0) {
        body(cpu, bits, delta, source);

        /* Decremented once per iteration including the last, so a loop
         * that ran out of count leaves CX=0 and one stopped by the flag
         * leaves CX holding the elements it never looked at. */
        cpu->cx--;

        if (conditional && vm86_flag_test(cpu, VM86_ZF) == stop_when_zf_set)
            break;
    }

    return VM86_CONTINUE;
}

/*
 * ---------------------------------------------------------------------
 * What this deliberately does not do: interruptibility
 *
 * On real hardware a repeated string instruction is not atomic. The
 * processor checks for interrupts between iterations, so a `rep movsw`
 * that moves a megabyte lets the timer and the keyboard in while it
 * works. This implementation runs the whole loop inside one vm86_step()
 * call and returns when it is finished.
 *
 * That is a deviation with consequences, and it is written down here
 * rather than left implicit because M4 has to deal with it: once there
 * is an interrupt model, an instruction that never yields makes a
 * program that reads a large file -- that is, any program that reads a
 * large file -- go deaf to the timer and the keyboard for as long as the
 * copy takes. It is not a corner case; it is every file load.
 *
 * The fix does not belong in this file. Making the loop resumable means
 * the run loop has to be able to stop in the middle of an instruction
 * and come back, which is a change to how vm86_step() is called, and it
 * is M4's interrupt model that makes it worth doing. Adding a half
 * version of it here would be rewritten there, so it is left out and
 * reported instead.
 */

/* ------------------------------------------------------------------ */
/* The five bodies                                                     */
/* ------------------------------------------------------------------ */

static void str_movs(struct vm86_cpu *cpu, uint8_t bits, int16_t delta,
                     enum vm86_seg source)
{
    uint16_t value = str_load(cpu, source, cpu->si, bits);

    str_store(cpu, VM86_ES, cpu->di, bits, value);

    cpu->si = (uint16_t)(cpu->si + delta);
    cpu->di = (uint16_t)(cpu->di + delta);
}

static void str_cmps(struct vm86_cpu *cpu, uint8_t bits, int16_t delta,
                     enum vm86_seg source)
{
    uint16_t lhs = str_load(cpu, source, cpu->si, bits);
    uint16_t rhs = str_load(cpu, VM86_ES, cpu->di, bits);

    /*
     * CMPS is a CMP whose operands happen to come from memory: DS:SI
     * minus ES:DI, in that order. The flags are the shared arithmetic's,
     * not a second implementation -- which is what makes `repe cmpsb`
     * work at all, since the loop condition reads the ZF this sets.
     */
    vm86_alu(cpu, VM86_ALU_CMP, bits, lhs, rhs);

    cpu->si = (uint16_t)(cpu->si + delta);
    cpu->di = (uint16_t)(cpu->di + delta);
}

static void str_stos(struct vm86_cpu *cpu, uint8_t bits, int16_t delta,
                     enum vm86_seg source)
{
    uint16_t value = (bits == 8) ? (uint16_t)cpu->al : cpu->ax;

    (void)source;   /* STOS has no DS-side operand to override */

    str_store(cpu, VM86_ES, cpu->di, bits, value);

    cpu->di = (uint16_t)(cpu->di + delta);
}

static void str_lods(struct vm86_cpu *cpu, uint8_t bits, int16_t delta,
                     enum vm86_seg source)
{
    uint16_t value = str_load(cpu, source, cpu->si, bits);

    /* Through the 8-bit view, so a LODSB leaves AH alone. The union
     * makes that automatic; writing cpu->ax would not. */
    if (bits == 8)
        cpu->al = (uint8_t)value;
    else
        cpu->ax = value;

    cpu->si = (uint16_t)(cpu->si + delta);
}

static void str_scas(struct vm86_cpu *cpu, uint8_t bits, int16_t delta,
                     enum vm86_seg source)
{
    uint16_t accumulator = (bits == 8) ? (uint16_t)cpu->al : cpu->ax;
    uint16_t memory      = str_load(cpu, VM86_ES, cpu->di, bits);

    (void)source;   /* and neither has SCAS */

    /*
     * SCAS is the same comparison with the operands the other way round
     * from CMPS: the accumulator against ES:DI. Both use DI; that is
     * what tells SCAS from CMPS, and neither of them reads a DS-side
     * pointer it does not have.
     */
    vm86_alu(cpu, VM86_ALU_CMP, bits, accumulator, memory);

    cpu->di = (uint16_t)(cpu->di + delta);
}

/* ------------------------------------------------------------------ */
/* The handlers                                                        */
/* ------------------------------------------------------------------ */

/* Byte or word is bit 0 of the opcode, in all five pairs. The table
 * below is the only thing that relies on it, and it is written out one
 * opcode at a time so the pairing is visible. */
static uint8_t str_bits(uint8_t opcode)
{
    return (uint8_t)((opcode & 1) ? 16 : 8);
}

static enum vm86_result op_movs(struct vm86_cpu *cpu, uint8_t opcode)
{
    return vm86_str_repeat(cpu, str_bits(opcode), str_movs, false);
}

/* Only these two stop on ZF, and only they can be REPE or REPNE. */
static enum vm86_result op_cmps(struct vm86_cpu *cpu, uint8_t opcode)
{
    return vm86_str_repeat(cpu, str_bits(opcode), str_cmps, true);
}

static enum vm86_result op_stos(struct vm86_cpu *cpu, uint8_t opcode)
{
    return vm86_str_repeat(cpu, str_bits(opcode), str_stos, false);
}

static enum vm86_result op_lods(struct vm86_cpu *cpu, uint8_t opcode)
{
    return vm86_str_repeat(cpu, str_bits(opcode), str_lods, false);
}

static enum vm86_result op_scas(struct vm86_cpu *cpu, uint8_t opcode)
{
    return vm86_str_repeat(cpu, str_bits(opcode), str_scas, true);
}

/* ------------------------------------------------------------------ */
/* The table                                                           */
/* ------------------------------------------------------------------ */

const vm86_op_fn vm86_ops_str[256] = {
    [0xA4] = op_movs, [0xA5] = op_movs,
    [0xA6] = op_cmps, [0xA7] = op_cmps,
    [0xAA] = op_stos, [0xAB] = op_stos,
    [0xAC] = op_lods, [0xAD] = op_lods,
    [0xAE] = op_scas, [0xAF] = op_scas,
};
