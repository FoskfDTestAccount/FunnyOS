/*
 * Arithmetic, logic, shifts and tests.
 *
 * ---------------------------------------------------------------------
 * Owned opcodes
 *
 *   00-25, 28-2D, 30-35, 38-3D
 *           the eight ALU operations in six forms, including the four
 *           segment push and pop encodings               [DONE - example]
 *   27,2F,37,3F
 *           DAA, DAS, AAA, AAS                                  [TODO]
 *   40-4F   INC/DEC a register                                 [TODO]
 *   80-83   the same eight operations, immediate forms          [TODO]
 *   84-85   TEST                                                [TODO]
 *   98-99   CBW, CWD                                            [TODO]
 *   A8-A9   TEST AL/AX, immediate                               [TODO]
 *   C0-C1   shifts by an immediate count (186)                  [TODO]
 *   D0-D3   shifts by one, by CL, and by an immediate           [TODO]
 *   D4-D5   AAM, AAD                                            [TODO]
 *   F6-F7   the group 3 block: TEST/NOT/NEG/MUL/IMUL/DIV/IDIV   [TODO]
 *   FE      INC/DEC a byte in memory                            [TODO]
 *
 * D7 (XLAT) is deliberately absent. It is a table lookup and belongs to
 * ops_mov.c, even though it sits between two things that are mine. This
 * header claimed it too until the two lists were compared -- which is the
 * sort of mistake the conflict check in vm86_ops_build() exists to catch,
 * and it is cheaper to catch it by reading than at startup.
 *
 * The helpers at the top are complete and are what the other groups
 * call. Do not reimplement them; if one of them is wrong, everything
 * downstream is wrong in the same direction.
 *
 * ---------------------------------------------------------------------
 * A note on the gaps in the table
 *
 * 0x26, 0x2E, 0x36 and 0x3E are absent deliberately: they are the
 * segment override prefixes, the run loop consumes them, and a table
 * entry for them would be unreachable. 0x27, 0x2F, 0x37 and 0x3F are
 * absent because they are unimplemented -- they fall through to the
 * invalid-opcode path until somebody writes them, which is the same
 * answer an unclaimed opcode gets everywhere else.
 */
#include <vm86/ops.h>

/* ------------------------------------------------------------------ */
/* The shared arithmetic                                               */
/* ------------------------------------------------------------------ */

uint16_t vm86_alu(struct vm86_cpu *cpu, enum vm86_alu_op op, uint8_t width,
                  uint16_t dst, uint16_t src)
{
    uint16_t mask = (width == 8) ? 0x00FFu : 0xFFFFu;
    uint16_t sign = (uint16_t)((mask >> 1) + 1);   /* bit 7, or bit 15 */

    dst &= mask;
    src &= mask;

    uint32_t lhs   = dst;
    uint32_t rhs   = src;
    uint32_t carry = vm86_flag_test(cpu, VM86_CF) ? 1u : 0u;

    uint32_t wide      = 0;
    bool     cf        = false;
    bool     is_logic  = false;

    switch (op) {
    case VM86_ALU_ADD:
        wide = lhs + rhs;
        cf   = wide > mask;
        break;

    case VM86_ALU_ADC:
        wide = lhs + rhs + carry;
        cf   = wide > mask;
        break;

    case VM86_ALU_SUB:
    case VM86_ALU_CMP:
        /*
         * Carry means BORROW here, which is the opposite of what the name
         * suggests to anyone coming from an addition -- and it is the
         * single most common place to get an 8086 wrong.
         *
         * Subtraction sets CF when the first operand is smaller. `SUB AX,
         * 1` on AX=0 leaves CF=1, not CF=0.
         */
        cf   = lhs < rhs;
        wide = lhs - rhs;
        break;

    case VM86_ALU_SBB:
        cf   = lhs < rhs + carry;
        wide = lhs - rhs - carry;
        break;

    case VM86_ALU_AND: wide = lhs & rhs; is_logic = true; break;
    case VM86_ALU_OR:  wide = lhs | rhs; is_logic = true; break;
    case VM86_ALU_XOR: wide = lhs ^ rhs; is_logic = true; break;

    default:
        return 0;
    }

    uint16_t result = (uint16_t)(wide & mask);

    if (is_logic) {
        vm86_flags_logic(cpu, width, result);
        return result;
    }

    /*
     * Overflow: the operands agreed on a sign and the result takes the
     * other one. Written as comparisons against the result rather than a
     * range check on the operands, so that it works identically at both
     * widths and in both directions.
     */
    bool of;
    if (op == VM86_ALU_ADD || op == VM86_ALU_ADC)
        of = ((result ^ dst) & (result ^ src) & sign) != 0;
    else
        of = ((dst ^ src) & (dst ^ result) & sign) != 0;

    /*
     * Auxiliary carry: the carry out of bit 3.
     *
     * One expression covers addition and subtraction both, and both
     * widths, because exclusive-oring the operands with the result
     * isolates the bit the operation carried through. Note that AF is
     * always about bit 3 regardless of the operand width -- it does not
     * move to bit 7 for a byte or bit 15 for a word.
     */
    bool af = ((dst ^ src ^ result) & 0x0010u) != 0;

    vm86_flag_set(cpu, VM86_CF, cf);
    vm86_flag_set(cpu, VM86_OF, of);
    vm86_flag_set(cpu, VM86_AF, af);

    vm86_flags_result(cpu, width, result);

    return result;
}

uint16_t vm86_alu_incdec(struct vm86_cpu *cpu, uint8_t width, uint16_t value,
                         bool increment)
{
    uint16_t mask = (width == 8) ? 0x00FFu : 0xFFFFu;
    uint16_t sign = (uint16_t)((mask >> 1) + 1);

    value &= mask;

    uint16_t result = (uint16_t)((value + (increment ? 1 : -1)) & mask);

    /*
     * Carry is deliberately not touched. That is the entire difference
     * between INC and ADD 1, and it is not pedantry: a loop written
     * `inc cx / jc somewhere` is using a carry left over from earlier
     * work, and an emulator that sets it here breaks the loop in a way
     * that will be blamed on the program.
     */

    /* Overflow is a wrap of the sign -- incrementing the largest positive
     * reaches the smallest negative, and the other way round. So it is
     * exactly "the result or the input landed on the sign bit". */
    bool of = increment ? (result == sign) : (value == sign);

    bool af = ((value ^ result) & 0x0010u) != 0;

    vm86_flag_set(cpu, VM86_OF, of);
    vm86_flag_set(cpu, VM86_AF, af);

    vm86_flags_result(cpu, width, result);

    return result;
}

/* ------------------------------------------------------------------ */
/* 00-25 and friends: eight operations in six forms                    */
/* ------------------------------------------------------------------ */

/*
 * One function for the whole block, because the opcode says everything:
 *
 *   bits 5-3   which operation: ADD OR ADC SBB AND SUB XOR CMP
 *   bits 2-0   which form:      r/m8,r8    r/m16,r16   r8,r/m8
 *                               r16,r/m16  AL,imm8     AX,imm16
 *                               PUSH seg   POP seg
 *
 * The four segment push and pop encodings are in the two leftover form
 * slots the operation field left over -- and because the segment number
 * is the same two bits as the operation number, it is already sitting in
 * the opcode when they arrive.
 */
static enum vm86_result op_alu_block(struct vm86_cpu *cpu, uint8_t opcode)
{
    uint8_t form      = (uint8_t)(opcode & 7);
    uint8_t operation = (uint8_t)((opcode >> 3) & 7);

    if (form >= 6) {
        /*
         * Only the first four operations have a segment form. Anything
         * else in these two form slots is not this instruction: the
         * override prefixes never reach dispatch, and the decimal
         * adjustments are not claimed by this file yet, so the table has
         * no entry for them. Reaching here means one was added by
         * mistake.
         */
        uint8_t segment = (uint8_t)((opcode >> 3) & 3);

        if (form == 7 && segment == VM86_CS) {
            /*
             * POP CS never existed. The 8086 left the encoding unused and
             * the 386 took it for the two-byte opcode prefix, so on this
             * processor it genuinely is not an instruction.
             */
            cpu->fault = VM86_VECTOR_INVALID_OPCODE;
            return VM86_FAULT;
        }

        if (form == 6)
            vm86_push16(cpu, vm86_get_seg(cpu, (enum vm86_seg)segment));
        else
            vm86_set_seg(cpu, (enum vm86_seg)segment, vm86_pop16(cpu));

        return VM86_CONTINUE;
    }

    uint8_t width = (form == 0 || form == 2 || form == 4) ? 8 : 16;

    struct vm86_operand dst_operand;
    uint16_t dst, src;

    if (form == 4 || form == 5) {
        /*
         * AL or AX with an immediate. No ModRM byte at all -- the
         * accumulator is implied, which is what makes these the shortest
         * encoding of their operation and what an assembler picks
         * whenever it can.
         */
        dst_operand.kind  = VM86_OPERAND_REGISTER;
        dst_operand.width = width;
        dst_operand.reg   = VM86_REG_AX;

        dst = vm86_operand_read(cpu, &dst_operand);
        src = (width == 8) ? vm86_fetch8(cpu) : vm86_fetch16(cpu);
    } else {
        struct vm86_modrm mr;
        vm86_fetch_modrm(cpu, &mr, width);

        uint8_t reg_field = mr.reg;

        if (form == 0 || form == 1) {
            /* r/m, reg: the ModRM operand is the destination. */
            dst_operand = mr.operand;
            dst         = vm86_operand_read(cpu, &dst_operand);
            src         = (width == 8) ? vm86_reg_get8(cpu, reg_field)
                                       : vm86_reg_get16(cpu, reg_field);
        } else {
            /* reg, r/m: the register is the destination. */
            dst_operand.kind  = VM86_OPERAND_REGISTER;
            dst_operand.width = width;
            dst_operand.reg   = reg_field;
            dst               = vm86_operand_read(cpu, &dst_operand);
            src               = vm86_operand_read(cpu, &mr.operand);
        }
    }

    uint16_t result = vm86_alu(cpu, (enum vm86_alu_op)operation, width,
                               dst, src);

    /* CMP computes and discards; every other operation in this block
     * stores. That is the whole difference between them, and it is why
     * vm86_alu() does not write anything itself. */
    if (operation != VM86_ALU_CMP)
        vm86_operand_write(cpu, &dst_operand, result);

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* 40-4F: increment and decrement a register                           */
/* ------------------------------------------------------------------ */

static enum vm86_result op_incdec_reg(struct vm86_cpu *cpu, uint8_t opcode)
{
    uint8_t index     = (uint8_t)(opcode & 7);
    bool    increment = (opcode & 8) == 0;

    uint16_t value = vm86_reg_get16(cpu, index);

    vm86_reg_set16(cpu, index, vm86_alu_incdec(cpu, 16, value, increment));

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* The table                                                           */
/* ------------------------------------------------------------------ */

/* The four ranges this file claims, written out one opcode at a time so
 * that the gaps where the prefixes and the decimal adjustments live are
 * visible rather than implied. */
const vm86_op_fn vm86_ops_alu[256] = {
    [0x00] = op_alu_block, [0x01] = op_alu_block, [0x02] = op_alu_block,
    [0x03] = op_alu_block, [0x04] = op_alu_block, [0x05] = op_alu_block,
    [0x06] = op_alu_block, [0x07] = op_alu_block,
    [0x08] = op_alu_block, [0x09] = op_alu_block, [0x0A] = op_alu_block,
    [0x0B] = op_alu_block, [0x0C] = op_alu_block, [0x0D] = op_alu_block,
    [0x0E] = op_alu_block, [0x0F] = op_alu_block,
    [0x10] = op_alu_block, [0x11] = op_alu_block, [0x12] = op_alu_block,
    [0x13] = op_alu_block, [0x14] = op_alu_block, [0x15] = op_alu_block,
    [0x16] = op_alu_block, [0x17] = op_alu_block,
    [0x18] = op_alu_block, [0x19] = op_alu_block, [0x1A] = op_alu_block,
    [0x1B] = op_alu_block, [0x1C] = op_alu_block, [0x1D] = op_alu_block,
    [0x1E] = op_alu_block, [0x1F] = op_alu_block,
    [0x20] = op_alu_block, [0x21] = op_alu_block, [0x22] = op_alu_block,
    [0x23] = op_alu_block, [0x24] = op_alu_block, [0x25] = op_alu_block,
    /* 0x26 prefix, 0x27 DAA */
    [0x28] = op_alu_block, [0x29] = op_alu_block, [0x2A] = op_alu_block,
    [0x2B] = op_alu_block, [0x2C] = op_alu_block, [0x2D] = op_alu_block,
    /* 0x2E prefix, 0x2F DAS */
    [0x30] = op_alu_block, [0x31] = op_alu_block, [0x32] = op_alu_block,
    [0x33] = op_alu_block, [0x34] = op_alu_block, [0x35] = op_alu_block,
    /* 0x36 prefix, 0x37 AAA */
    [0x38] = op_alu_block, [0x39] = op_alu_block, [0x3A] = op_alu_block,
    [0x3B] = op_alu_block, [0x3C] = op_alu_block, [0x3D] = op_alu_block,
    /* 0x3E prefix, 0x3F AAS */

    [0x40] = op_incdec_reg, [0x41] = op_incdec_reg, [0x42] = op_incdec_reg,
    [0x43] = op_incdec_reg, [0x44] = op_incdec_reg, [0x45] = op_incdec_reg,
    [0x46] = op_incdec_reg, [0x47] = op_incdec_reg,

    [0x48] = op_incdec_reg, [0x49] = op_incdec_reg, [0x4A] = op_incdec_reg,
    [0x4B] = op_incdec_reg, [0x4C] = op_incdec_reg, [0x4D] = op_incdec_reg,
    [0x4E] = op_incdec_reg, [0x4F] = op_incdec_reg,
};
