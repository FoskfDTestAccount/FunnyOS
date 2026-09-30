/*
 * Data movement, and the stack.
 *
 * ---------------------------------------------------------------------
 * Owned opcodes
 *
 *   50-5F   PUSH/POP a register                         [DONE - example]
 *   88-8B   MOV r/m, r and r, r/m                               [TODO]
 *   8C-8E   MOV a segment register out and in                   [TODO]
 *   8F      POP r/m16                                           [TODO]
 *   90      NOP                                                 [DONE]
 *   91-97   XCHG AX with a register                             [TODO]
 *   86-87   XCHG r/m with a register                            [TODO]
 *   A0-A3   MOV AL/AX, [moffs]                                  [TODO]
 *   B0-BF   MOV a register, immediate                   [DONE - example]
 *   C6-C7   MOV r/m, immediate                                  [TODO]
 *   D7      XLAT                                                [TODO]
 *
 * The stack helpers below are complete and are what the other groups
 * call. Do not reimplement them.
 *
 * ---------------------------------------------------------------------
 * Two traps worth knowing before you start
 *
 * PUSH SP. On the 8086 it pushes the value of SP *after* the decrement;
 * on the 80186 and later it pushes the value *before*. This is a
 * documented difference and it is how some software tells the two apart,
 * so it is a decision rather than a detail. This emulator claims to be an
 * 8086, so 8086 behaviour is the right answer -- but write down that you
 * made the choice, because the next person will assume the other one.
 *
 * XCHG AX, AX is NOP, and it is also opcode 0x90. There is no separate
 * NOP instruction. That is not trivia: it means NOP must not be given a
 * table entry that shadows the 0x90 XCHG form, because there isn't one.
 */
#include <vm86/ops.h>

/* ------------------------------------------------------------------ */
/* The stack                                                           */
/* ------------------------------------------------------------------ */

void vm86_push16(struct vm86_cpu *cpu, uint16_t value)
{
    /*
     * Decrement first, then store. SP ends up pointing at the word just
     * pushed, which is what makes `pop` able to read it without knowing
     * how it got there.
     */
    cpu->sp = (uint16_t)(cpu->sp - 2);

    vm86_mem_write16(cpu->mem, vm86_linear(cpu, VM86_SS, cpu->sp), value);
}

uint16_t vm86_pop16(struct vm86_cpu *cpu)
{
    uint16_t value =
        vm86_mem_read16(cpu->mem, vm86_linear(cpu, VM86_SS, cpu->sp));

    cpu->sp = (uint16_t)(cpu->sp + 2);

    return value;
}

/* ------------------------------------------------------------------ */
/* B0-BF: move an immediate into a register                            */
/* ------------------------------------------------------------------ */

/*
 * Sixteen opcodes, one function: bit 3 says whether it is a byte or a
 * word, and bits 2-0 are the register number.
 *
 * Note which register table those low three bits index. At 8 bits the
 * order is AL CL DL BL AH CH DH BH; at 16 it is AX CX DX BX SP BP SI DI.
 * They agree for the first four and not after, which is the single most
 * common way to write an 8086 emulator that works until it meets a
 * program using SP, BP, SI or DI.
 */
static enum vm86_result op_mov_imm(struct vm86_cpu *cpu, uint8_t opcode)
{
    uint8_t index = (uint8_t)(opcode & 7);

    if (opcode & 0x08) {
        vm86_reg_set16(cpu, index, vm86_fetch16(cpu));
    } else {
        vm86_reg_set8(cpu, index, vm86_fetch8(cpu));
    }

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* 50-5F: push and pop a register                                      */
/* ------------------------------------------------------------------ */

static enum vm86_result op_push_pop_reg(struct vm86_cpu *cpu, uint8_t opcode)
{
    uint8_t index = (uint8_t)(opcode & 7);

    if (opcode & 0x08)
        vm86_reg_set16(cpu, index, vm86_pop16(cpu));
    else
        vm86_push16(cpu, vm86_reg_get16(cpu, index));

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* 0x90: NOP                                                           */
/* ------------------------------------------------------------------ */

static enum vm86_result op_nop(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)cpu;
    (void)opcode;

    /* XCHG AX, AX. Runs for 3 clock cycles on a real 8086, which is why
     * it was ever a useful thing to have more than one of. */
    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* The table                                                           */
/* ------------------------------------------------------------------ */

const vm86_op_fn vm86_ops_mov[256] = {
    [0x50] = op_push_pop_reg, [0x51] = op_push_pop_reg,
    [0x52] = op_push_pop_reg, [0x53] = op_push_pop_reg,
    [0x54] = op_push_pop_reg, [0x55] = op_push_pop_reg,
    [0x56] = op_push_pop_reg, [0x57] = op_push_pop_reg,

    [0x58] = op_push_pop_reg, [0x59] = op_push_pop_reg,
    [0x5A] = op_push_pop_reg, [0x5B] = op_push_pop_reg,
    [0x5C] = op_push_pop_reg, [0x5D] = op_push_pop_reg,
    [0x5E] = op_push_pop_reg, [0x5F] = op_push_pop_reg,

    [0x90] = op_nop,

    [0xB0] = op_mov_imm, [0xB1] = op_mov_imm, [0xB2] = op_mov_imm,
    [0xB3] = op_mov_imm, [0xB4] = op_mov_imm, [0xB5] = op_mov_imm,
    [0xB6] = op_mov_imm, [0xB7] = op_mov_imm,

    [0xB8] = op_mov_imm, [0xB9] = op_mov_imm, [0xBA] = op_mov_imm,
    [0xBB] = op_mov_imm, [0xBC] = op_mov_imm, [0xBD] = op_mov_imm,
    [0xBE] = op_mov_imm, [0xBF] = op_mov_imm,
};
