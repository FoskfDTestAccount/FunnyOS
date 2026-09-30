/*
 * Control flow, interrupts, flags, and I/O.
 *
 * ---------------------------------------------------------------------
 * Owned opcodes
 *
 *   70-7F   the conditional jumps                              [TODO]
 *   9A, EA  far CALL and far JMP                              [TODO]
 *   C2, C3  RET near, with and without a stack adjustment      [TODO]
 *   CA, CB  RET far                                            [TODO]
 *   CC      INT 3                                              [TODO]
 *   CD      INT imm8                                           [TODO]
 *   CE      INTO                                               [TODO]
 *   CF      IRET                                               [TODO]
 *   E0-E3   LOOPNE, LOOPE, LOOP, JCXZ                          [TODO]
 *   E8, E9  CALL and JMP near                                  [TODO]
 *   EB      JMP short                                          [TODO]
 *   F4      HLT                                        [DONE - example]
 *   F5      CMC                                                [TODO]
 *   F8-FD   CLC, STC, CLI, STI, CLD, STD                       [TODO]
 *   9B      WAIT                                               [TODO]
 *   9C-9F   PUSHF, POPF, SAHF, LAHF                            [TODO]
 *   E4-E7   IN and OUT with an immediate port                  [TODO]
 *   EC-EF   IN and OUT through DX                              [TODO]
 *   FF      group 5: INC/DEC memory, CALL, JMP, PUSH           [TODO]
 *
 * ---------------------------------------------------------------------
 * Notes that will save you time
 *
 * The conditional jumps are sixteen opcodes that differ only in which
 * flags they test, and the flags they test are in the opcode's low four
 * bits. A table of sixteen conditions is the whole implementation; resist
 * writing sixteen functions.
 *
 * Near jumps and calls are RELATIVE. The displacement is signed and is
 * added to IP -- which has already been advanced past the instruction by
 * the time you read it. Every emulator gets this wrong once. The
 * arithmetic is `ip = ip + (int16_t)displacement` where `ip` is the value
 * after the operand has been fetched, not before.
 *
 * Far jumps and calls take a segment and an offset, in that order in the
 * encoding (offset first, then segment) -- which is the opposite of how
 * they are written in assembly. Check it against a disassembly before
 * believing it: every assembler prints `jmp 0x1234:0x5678` for bytes that
 * store 5678 then 1234.
 *
 * Flags worth care:
 *
 *   POPF and IRET write the flags register wholesale, so both must call
 *   vm86_flags_normalise() afterwards or the bits that are supposed to
 *   read as one will take whatever the guest put there. See cpu.h.
 *
 *   IRET pops IP, CS and FLAGS, in that order. The order is not the same
 *   as the order they were pushed by INT, and getting it wrong tends to
 *   look like a jump to a random address.
 *
 *   STI does not take effect until after the instruction following it.
 *   That is real hardware behaviour, it exists so `sti / hlt` works as an
 *   idle loop, and if interrupts are delivered one instruction early then
 *   that idiom hangs the machine.
 *
 *   CLI, STI and the rest of the flag instructions do not touch anything
 *   but their own flag.
 *
 * IN and OUT are here rather than in their own file because there is
 * nothing for them to talk to yet. A real implementation needs a port
 * dispatch table, which arrives with the hardware emulation in M4; until
 * then the right behaviour is to accept the encoding and do nothing, so
 * that a program probing a port gets a defined answer rather than a
 * fault. Decide and write down what answer that is.
 *
 * Opcode FF is group 5 and this file owns it, but two of its eight
 * sub-operations are INC and DEC, which are arithmetic. Call
 * vm86_alu_incdec() from ops.h rather than writing a second copy -- the
 * flags have to come out identical to the ones FE produces, and two
 * implementations will not stay identical.
 */
#include <vm86/ops.h>

/* ------------------------------------------------------------------ */
/* F4: HLT                                                             */
/* ------------------------------------------------------------------ */

static enum vm86_result op_hlt(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    /*
     * Stop, and tell the run loop so. It is not a fault and it is not a
     * no-op: a program that halts is waiting for an interrupt, and
     * spinning instead would burn the host's CPU while changing nothing
     * about what the guest can observe.
     */
    cpu->halted = true;

    return VM86_HALT;
}

/* ------------------------------------------------------------------ */
/* The table                                                           */
/* ------------------------------------------------------------------ */

const vm86_op_fn vm86_ops_ctl[256] = {
    [0xF4] = op_hlt,
};
