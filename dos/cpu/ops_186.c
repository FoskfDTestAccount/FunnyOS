/*
 * The 80186 additions.
 *
 * ---------------------------------------------------------------------
 * Owned opcodes
 *
 *   60, 61  PUSHA, POPA                                         [TODO]
 *   62      BOUND                                               [TODO]
 *   68, 6A  PUSH an immediate                                   [TODO]
 *   69, 6B  IMUL with two or three operands                     [TODO]
 *   6C, 6D  INSB, INSW                                          [TODO]
 *   6E, 6F  OUTSB, OUTSW                                        [TODO]
 *   C0, C1  shift by an immediate count          (claimed by ops_alu.c)
 *   C8, C9  ENTER, LEAVE                                        [TODO]
 *
 * ---------------------------------------------------------------------
 * Why this file exists at all
 *
 * DESIGN.md puts the 80186 extensions in the "must do" tier rather than
 * the "later" tier, and the reason is not completeness for its own sake:
 * by the late 1980s a great deal of DOS software was compiled to assume
 * an 186 or better, because that was the floor the compilers targeted.
 * Programs that use PUSHA are not rare curiosities.
 *
 * So these are ordinary instructions, not a museum piece.
 *
 * ---------------------------------------------------------------------
 * The ones that are genuinely subtle
 *
 * PUSHA pushes AX, CX, DX, BX, the ORIGINAL SP, BP, SI and DI -- in that
 * order, which is the register encoding order and not the order they are
 * written in the assembly. The SP it pushes is the value from before the
 * first push, not the value partway through, and getting that wrong is
 * invisible unless a program actually uses the saved SP.
 *
 * POPA reads them back in the same order and ignores the SP it reads --
 * deliberately, because that value is meaningless by then. A
 * straightforward symmetric implementation writes SP at the end and
 * destroys the stack pointer, which is a spectacular way to fail.
 *
 * ENTER builds a stack frame from two immediates: how many bytes of local
 * space, and a nesting level. The nesting level is almost always zero,
 * and the zero case is simple enough that it is tempting to implement
 * only that. Do not: level zero and level one differ, real compilers emit
 * level zero, and a program that emits the other will produce a corrupted
 * frame rather than an obvious failure. See the 80186 manual for the
 * loop; it is eight lines and the alternative is eight lines of guessing.
 *
 * LEAVE is ENTER's inverse and is three instructions: SP <- BP, pop BP.
 *
 * BOUND compares a signed value against a pair of signed bounds in
 * memory and raises vector 5 if it is outside them. It is the one
 * instruction here that produces a fault; see VM86_VECTOR_BOUND in
 * ops.h. The comparison is SIGNED, and the bounds are stored low word
 * first.
 *
 * IMUL with two or three operands truncates to sixteen bits and sets CF
 * and OF if the result does not fit -- unlike the one-operand form in
 * ops_alu.c, which produces a full 32-bit product in DX:AX. They are
 * different instructions that share a mnemonic, and the flags mean
 * different things in each.
 *
 * INS and OUTS are the string versions of IN and OUT, and they are
 * REP-able like every other string instruction: SI for the port side of
 * OUTS, DI for the memory side of INS, and the port number in DX. They
 * belong to ops_str.c's problem -- the REP loop -- as much as to this
 * one. Coordinate: the loop is the same, only the body differs.
 */
#include <vm86/ops.h>

const vm86_op_fn vm86_ops_186[256] = {
    /* Nothing claimed yet. Note that C0 and C1 belong to ops_alu.c even
     * though they are 186 instructions, because they are shifts and the
     * shift helpers live there. If you claim them here as well,
     * vm86_ops_build() will say so at startup rather than letting one of
     * us silently win. */
};
