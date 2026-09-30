/*
 * The string instructions.
 *
 * ---------------------------------------------------------------------
 * Owned opcodes
 *
 *   A4      MOVSB       [TODO]
 *   A5      MOVSW       [TODO]
 *   A6      CMPSB       [TODO]
 *   A7      CMPSW       [TODO]
 *   AA      STOSB       [TODO]
 *   AB      STOSW       [TODO]
 *   AC      LODSB       [TODO]
 *   AD      LODSW       [TODO]
 *   AE      SCASB       [TODO]
 *   AF      SCASW       [TODO]
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
 */
#include <vm86/ops.h>

const vm86_op_fn vm86_ops_str[256] = {
    /* Nothing claimed yet. See the list above. */
};
