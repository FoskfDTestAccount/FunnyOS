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
 *           DAA, DAS, AAA, AAS                                      [DONE]
 *   40-4F   INC/DEC a register                                     [DONE]
 *   80-83   the same eight operations, immediate forms             [DONE]
 *   84-85   TEST                                                   [DONE]
 *   98-99   CBW, CWD                                               [DONE]
 *   A8-A9   TEST AL/AX, immediate                                  [DONE]
 *   C0-C1   shifts by an immediate count (186)                     [DONE]
 *   D0-D3   shifts by one, by CL, and by an immediate              [DONE]
 *   D4-D5   AAM, AAD                                               [DONE]
 *   F6-F7   the group 3 block: TEST/NOT/NEG/MUL/IMUL/DIV/IDIV      [DONE]
 *   FE      INC/DEC a byte, register or memory                     [DONE]
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
 * entry for them would be unreachable. The single bytes between the ALU
 * ranges -- 0x27, 0x2F, 0x37 and 0x3F -- are where DAA, DAS, AAA and AAS
 * live. They are this file's too, but they are nothing like the ALU
 * block, so they have their own handler further down rather than being
 * folded into it.
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
         * adjustments have handlers of their own, so the table has no
         * entry for them here. Reaching this point means one was added
         * by mistake.
         */
        uint8_t segment = (uint8_t)((opcode >> 3) & 3);

        if (form == 7 && segment == VM86_CS) {
            /*
             * 0F is POP CS on the 8086, and the two-byte escape from the
             * 80186 onward.
             *
             * Refusing it is a choice about which machine this is, not a
             * fact about the encoding. The opcode map here follows the
             * 186 -- that is what makes PUSHA and the rest possible --
             * so the escape stays reserved and POP CS is not implemented.
             *
             * The comment that used to sit here said POP CS never existed
             * and that the 386 took the encoding. Both were wrong, and it
             * is worth leaving a note about why that mattered: a reason
             * that reads as complete stops the next reader from checking,
             * which is exactly how a false one survives. The behaviour
             * did not change when this was corrected; only the reason.
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
/* 27, 2F, 37, 3F: the decimal adjustments                             */
/* ------------------------------------------------------------------ */

/*
 * DAA and DAS: correct a packed BCD result in AL, one digit at a time.
 *
 * The two instructions are the same instruction with the correction's
 * sign flipped, so they are one function and the manual's two tables
 * become its two halves. Both read the flags they write, which is what
 * makes them unlike anything else here: the low nibble of AL together
 * with AF decides whether the low digit is corrected, and the value AL
 * has *after* that correction, together with CF, decides whether the
 * high digit is. The second test being on the updated AL is the whole
 * reason DAA on 9Ah gives 00 with a carry rather than A0: 9Ah corrects to
 * A0h, which is now over 9Fh, so the high digit is corrected too.
 *
 * SF, ZF and PF come from the final AL. OF is left exactly as it was
 * found: the manual does not define it after these instructions, and
 * inventing a value would be a claim this code cannot back.
 */
static void daa_das(struct vm86_cpu *cpu, bool subtract)
{
    uint8_t al = cpu->al;
    bool    cf = vm86_flag_test(cpu, VM86_CF);
    bool    af = vm86_flag_test(cpu, VM86_AF);

    if ((al & 0x0Fu) > 9 || af) {
        al = (uint8_t)(al + (subtract ? -6 : 6));
        af = true;
    } else {
        af = false;
    }

    if (al > 0x9Fu || cf) {
        al = (uint8_t)(al + (subtract ? -0x60 : 0x60));
        cf = true;
    } else {
        cf = false;
    }

    cpu->al = al;

    vm86_flag_set(cpu, VM86_CF, cf);
    vm86_flag_set(cpu, VM86_AF, af);

    vm86_flags_result(cpu, 8, al);
}

/*
 * AAA and AAS: the same idea for unpacked digits, and not the same
 * instruction at all.
 *
 * Here only the low nibble of AL holds a digit and AH holds what carried
 * out of it, so the correction lands in AH as well, and AL is truncated
 * to its low nibble afterwards whatever happened -- including when
 * nothing was adjusted. Only CF and AF are written. OF, SF, ZF and PF are
 * undefined afterwards and are deliberately left alone: the instruction
 * before is an ADD or a SUB that set all four to something meaningful,
 * and clearing them would throw away information the hardware keeps.
 */
static void aaa_aas(struct vm86_cpu *cpu, bool subtract)
{
    uint8_t al     = cpu->al;
    bool    adjust = (al & 0x0Fu) > 9 || vm86_flag_test(cpu, VM86_AF);

    if (adjust) {
        al       = (uint8_t)(al + (subtract ? -6 : 6));
        cpu->ah  = (uint8_t)(cpu->ah + (subtract ? -1 : 1));
    }

    cpu->al = (uint8_t)(al & 0x0Fu);

    vm86_flag_set(cpu, VM86_CF, adjust);
    vm86_flag_set(cpu, VM86_AF, adjust);
}

static enum vm86_result op_decimal(struct vm86_cpu *cpu, uint8_t opcode)
{
    switch (opcode) {
    case 0x27: daa_das(cpu, false); break;
    case 0x2F: daa_das(cpu, true);  break;
    case 0x37: aaa_aas(cpu, false); break;
    case 0x3F: aaa_aas(cpu, true);  break;
    default:   break;   /* unreachable: the table maps only these four */
    }

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* 80-83: the same eight operations with an immediate                  */
/* ------------------------------------------------------------------ */

/*
 * The ModRM byte's reg field is not a register here, it is the operation,
 * which is what lets one function serve sixteen opcodes. The opcode says
 * the width and the shape of the immediate:
 *
 *   80   r/m8,  imm8
 *   81   r/m16, imm16
 *   82   r/m8,  imm8       -- the same as 80, see below
 *   83   r/m16, imm8 sign-extended to a word
 *
 * 82 is not in the manual: it is the encoding of 80 appearing a second
 * time, and the 8086 executes it as 80. Assemblers of the era emitted it,
 * so it is not hypothetical, and honouring it costs one table entry.
 *
 * The sign extension in 83 is the part worth being careful about. The
 * immediate really is a signed byte standing for a word, so `add ax, -1`
 * encodes as 83 C0 FF and has to add FFFFh. Widening FFh to 00FFh is what
 * a plain cast does, it is right for every positive value, and it is
 * wrong for every negative one.
 */
_Static_assert(VM86_ALU_ADD == 0 && VM86_ALU_OR  == 1 &&
               VM86_ALU_ADC == 2 && VM86_ALU_SBB == 3 &&
               VM86_ALU_AND == 4 && VM86_ALU_SUB == 5 &&
               VM86_ALU_XOR == 6 && VM86_ALU_CMP == 7,
               "the ALU enum must stay in the order the encodings number");

static enum vm86_result op_group1(struct vm86_cpu *cpu, uint8_t opcode)
{
    uint8_t width = (opcode & 1) ? 16 : 8;

    struct vm86_modrm mr;
    vm86_fetch_modrm(cpu, &mr, width);

    uint16_t dst = vm86_operand_read(cpu, &mr.operand);

    uint16_t src;
    if (opcode == 0x83)
        src = vm86_sign_extend8(vm86_fetch8(cpu));
    else if (width == 8)
        src = vm86_fetch8(cpu);
    else
        src = vm86_fetch16(cpu);

    uint16_t result = vm86_alu(cpu, (enum vm86_alu_op)mr.reg, width,
                               dst, src);

    if (mr.reg != VM86_ALU_CMP)
        vm86_operand_write(cpu, &mr.operand, result);

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* 84-85 and A8-A9: TEST                                               */
/* ------------------------------------------------------------------ */

/*
 * TEST is an AND whose result is thrown away: the flags it sets are the
 * point of the instruction and the value it computes is not wanted. So it
 * is written by calling the shared AND and discarding what comes back,
 * rather than by computing the AND again next to it. CF and OF are
 * cleared, ZF, SF and PF come from the result, and AF is left undefined.
 */
static enum vm86_result op_test_rm(struct vm86_cpu *cpu, uint8_t opcode)
{
    uint8_t width = (opcode & 1) ? 16 : 8;

    struct vm86_modrm mr;
    vm86_fetch_modrm(cpu, &mr, width);

    uint16_t operand = vm86_operand_read(cpu, &mr.operand);
    uint16_t reg     = (width == 8) ? vm86_reg_get8(cpu, mr.reg)
                                    : vm86_reg_get16(cpu, mr.reg);

    (void)vm86_alu(cpu, VM86_ALU_AND, width, operand, reg);

    return VM86_CONTINUE;
}

static enum vm86_result op_test_accum(struct vm86_cpu *cpu, uint8_t opcode)
{
    uint8_t  width = (opcode & 1) ? 16 : 8;
    uint16_t imm   = (width == 8) ? vm86_fetch8(cpu) : vm86_fetch16(cpu);

    (void)vm86_alu(cpu, VM86_ALU_AND, width, cpu->ax, imm);

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* 98-99: CBW and CWD                                                  */
/* ------------------------------------------------------------------ */

/*
 * Sign extension, and no flags at all. CBW copies AL's sign through AH;
 * CWD copies AX's sign through the whole of DX, which is what puts a
 * signed word into the pair IDIV and IMUL expect it in.
 */
static enum vm86_result op_cbw(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    cpu->ah = (cpu->al & 0x80u) ? 0xFFu : 0x00u;

    return VM86_CONTINUE;
}

static enum vm86_result op_cwd(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    cpu->dx = (cpu->ax & 0x8000u) ? 0xFFFFu : 0x0000u;

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* C0-C1 and D0-D3: the shifts and rotates                             */
/* ------------------------------------------------------------------ */

/* The operations, in the order the ModRM reg field numbers them. SAL and
 * SHL are the same encoding under two names, which is why there are
 * eight of these and only seven things to do. */
enum vm86_shift_op {
    VM86_SHIFT_ROL = 0, VM86_SHIFT_ROR, VM86_SHIFT_RCL, VM86_SHIFT_RCR,
    VM86_SHIFT_SHL = 4, VM86_SHIFT_SHR, VM86_SHIFT_SAL, VM86_SHIFT_SAR,
};

/*
 * One function for all ten shift and rotate encodings, because they
 * differ only in where the bit leaving the end goes:
 *
 *   D0/D1      count 1
 *   D2/D3      count is CL
 *   C0/C1      count is an immediate byte (186)
 *
 * and in two things the bytes say between them: bit 0 of the opcode is the
 * width, and the ModRM reg field is the operation -- the same eight-way
 * extension field the group encodings use, which is why this is one
 * function and not ten.
 *
 * The shifts are a loop over single steps rather than a closed form for
 * each count. A closed form has to handle the counts that leave no bit at
 * all -- a rotate by a multiple of the width, a shift past the end --
 * and CF is defined as "the last bit that left", which those cases make
 * awkward to state. A loop cannot get that wrong: if it ran at all, the
 * last bit it moved is the last bit that left.
 *
 * THE COUNT IS NOT TRUNCATED, and that is a decision rather than an
 * oversight. The 8086 shifts by the whole of CL: a count of 20 shifts
 * twenty times and leaves zero. The 186 and everything after it mask the
 * count to five bits, so the same instruction shifts four times and
 * leaves a plausible-looking nonzero result. This emulator is an 8086 --
 * cpu.h makes the same choice for the same reason when it decides what
 * the high bits of FLAGS read as -- so the 8086's rule is the one
 * implemented. The 186's immediate forms (C0/C1) follow it too: they are
 * not a different machine's encodings, they are this machine's extra
 * ones, and having `shl al, 3` and `shl al, cl` disagree about what a
 * count means would be worse than being wrong about the 186.
 *
 * Whoever comes to change this: it is the count that decides, and every
 * test with a count above the operand width is asserting the 8086 answer
 * on purpose. Masking here is a one-line change and a different machine.
 *
 * OF is defined only when the count is 1. For any other count the manual
 * leaves it undefined, so it is not touched at all -- not cleared, not
 * computed. A test that checks it for those counts is checking nothing,
 * and there is an `ignore` argument in the harness for exactly that.
 */
static enum vm86_result op_shift(struct vm86_cpu *cpu, uint8_t opcode)
{
    uint8_t  width     = (opcode & 1) ? 16 : 8;
    uint16_t mask      = (width == 8) ? 0x00FFu : 0xFFFFu;
    uint16_t msb       = (uint16_t)(mask ^ (mask >> 1));

    struct vm86_modrm mr;
    vm86_fetch_modrm(cpu, &mr, width);

    uint8_t operation = mr.reg;

    uint8_t count;
    switch (opcode) {
    case 0xC0: case 0xC1: count = vm86_fetch8(cpu); break;
    case 0xD0: case 0xD1: count = 1;                break;
    default:              count = cpu->cl;          break;   /* D2, D3 */
    }

    /*
     * A count of zero does nothing at all, flags included. That is not a
     * special case bolted on to make tests pass -- it is what "shift by
     * nothing" means, and it is the reason `shl ax, cl` with CL=0 has to
     * leave CF and OF exactly as the branch before it left them. Code
     * generated by a compiler reaches this with CL=0 regularly.
     */
    if (count == 0)
        return VM86_CONTINUE;

    uint16_t value    = vm86_operand_read(cpu, &mr.operand);
    uint16_t original = value;

    /*
     * The carry flag is an input as well as an output for RCL and RCR, so
     * the loop has to start from what is in it rather than from nothing.
     * Every other operation overwrites it on the first step, so this
     * matters only for those two -- and only when they are asked to bring
     * a carry in.
     */
    bool carry = vm86_flag_test(cpu, VM86_CF);

    for (uint8_t i = 0; i < count; i++) {
        switch (operation) {
        case VM86_SHIFT_ROL:
            carry = (value & msb) != 0;
            value = (uint16_t)(((value << 1) | (carry ? 1u : 0u)) & mask);
            break;

        case VM86_SHIFT_ROR:
            carry = (value & 1) != 0;
            value = (uint16_t)((value >> 1) | (carry ? msb : 0u));
            break;

        case VM86_SHIFT_RCL: {
            /* Through the carry: the bit leaving the top becomes the new
             * carry, and the old carry arrives in bit 0. */
            bool out = (value & msb) != 0;

            value = (uint16_t)(((value << 1) | (carry ? 1u : 0u)) & mask);
            carry = out;
            break;
        }

        case VM86_SHIFT_RCR: {
            bool out = (value & 1) != 0;

            value = (uint16_t)((value >> 1) | (carry ? msb : 0u));
            carry = out;
            break;
        }

        case VM86_SHIFT_SHL:   /* SAL is the same shift: the bits that
                                * leave the top are not the ones a signed
                                * reading would keep. */
        case VM86_SHIFT_SAL:
            carry = (value & msb) != 0;
            value = (uint16_t)((value << 1) & mask);
            break;

        case VM86_SHIFT_SHR:
            carry = (value & 1) != 0;
            value = (uint16_t)(value >> 1);
            break;

        default:               /* SAR: the sign bit does not move, so it
                                * is shifted back in at the top. */
            carry = (value & 1) != 0;
            value = (uint16_t)((value >> 1) | (value & msb));
            break;
        }
    }

    vm86_operand_write(cpu, &mr.operand, value);
    vm86_flag_set(cpu, VM86_CF, carry);

    if (count == 1) {
        bool of;

        switch (operation) {
        case VM86_SHIFT_ROL:
        case VM86_SHIFT_RCL:
        case VM86_SHIFT_SHL:
        case VM86_SHIFT_SAL:
            /* The sign bit changed if the bit that left and the bit that
             * arrived disagree. */
            of = ((value & msb) != 0) != carry;
            break;

        case VM86_SHIFT_ROR:
        case VM86_SHIFT_RCR: {
            /* For the rotates that come down, the two bits that moved
             * into the top are the two that have to agree. */
            bool top    = (value & msb) != 0;
            bool second = (value & (uint16_t)(msb >> 1)) != 0;

            of = top != second;
            break;
        }

        case VM86_SHIFT_SHR:
            /* The original sign bit, which is now the carry. */
            of = (original & msb) != 0;
            break;

        default:   /* SAR never overflows: it is a divide by two. */
            of = false;
            break;
        }

        vm86_flag_set(cpu, VM86_OF, of);
    }

    vm86_flags_result(cpu, width, value);

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* D4-D5: AAM and AAD                                                  */
/* ------------------------------------------------------------------ */

/*
 * Both take an immediate byte that is the base, even though the manual
 * says anything but ten is undefined and every real use of them is base
 * ten. It is read as the base rather than assumed, because it costs
 * nothing and because D4 0A is what a program actually contains.
 *
 * AAM is a division and can therefore fail the way a division fails, but
 * not in a way a program can have been written to catch: a zero base is
 * documented nowhere and appears in no program, and on a host it would be
 * a division by zero inside the emulator -- taking the emulator down
 * rather than the guest's program. So it is a decision rather than a
 * reading: the fault is reported as the guest's divide error, which keeps
 * it inside the guest, where M4 can deliver it the way it delivers any
 * other.
 */
static enum vm86_result op_aam(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    uint8_t base = vm86_fetch8(cpu);

    if (base == 0) {
        cpu->fault = VM86_VECTOR_DIVIDE_ERROR;
        return VM86_FAULT;
    }

    uint8_t al = cpu->al;

    cpu->ah = (uint8_t)(al / base);
    cpu->al = (uint8_t)(al % base);

    vm86_flags_result(cpu, 8, cpu->al);

    return VM86_CONTINUE;
}

static enum vm86_result op_aad(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    uint8_t base = vm86_fetch8(cpu);
    uint8_t al   = (uint8_t)(cpu->al + (uint8_t)(cpu->ah * base));

    cpu->al = al;
    cpu->ah = 0;

    vm86_flags_result(cpu, 8, al);

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* F6-F7: the group 3 block                                            */
/* ------------------------------------------------------------------ */

/*
 * Eight operations sharing one encoding, selected by the ModRM reg field.
 * They have almost nothing else in common, which is why this switch is
 * the shape it is: NOT and NEG sit next to each other and go out of their
 * way to have opposite effects on the flags, and the four arithmetic ones
 * have four different flag rules.
 */
static enum vm86_result op_group3(struct vm86_cpu *cpu, uint8_t opcode)
{
    uint8_t  width = (opcode & 1) ? 16 : 8;
    uint16_t mask  = (width == 8) ? 0x00FFu : 0xFFFFu;

    struct vm86_modrm mr;
    vm86_fetch_modrm(cpu, &mr, width);

    uint16_t value = vm86_operand_read(cpu, &mr.operand);
    uint16_t ax    = cpu->ax;

    switch (mr.reg) {
    case 0:
    case 1:   /* TEST. The manual documents /0 only; /1 is the same
               * instruction under a second encoding, as 82 is to 80. */
        {
            uint16_t imm = (width == 8) ? vm86_fetch8(cpu)
                                        : vm86_fetch16(cpu);

            (void)vm86_alu(cpu, VM86_ALU_AND, width, value, imm);
        }
        break;

    case 2:   /* NOT. Not one flag moves -- not CF, not OF, not the
               * undefined ones. It is the only instruction in this block
               * that writes a result and touches the flags at all. */
        vm86_operand_write(cpu, &mr.operand, (uint16_t)(~value & mask));
        break;

    case 3:   /* NEG is 0 minus the operand, and has SUB's flags exactly:
               * CF is set unless the operand was zero, and OF is set for
               * the most negative operand, whose negation is itself and
               * therefore does not fit. Routing it through the shared
               * subtraction is not a shortcut -- it is what the
               * instruction is, and it keeps the two agreeing. */
        vm86_operand_write(cpu, &mr.operand,
                           vm86_alu(cpu, VM86_ALU_SUB, width, 0, value));
        break;

    case 4:   /* MUL: unsigned. CF and OF say whether the high half of the
               * product holds anything, and every other flag is left
               * undefined -- so it is left alone. */
        if (width == 8) {
            cpu->ax = (uint16_t)((uint8_t)ax * (uint8_t)value);
            vm86_flag_set(cpu, VM86_CF, cpu->ah != 0);
            vm86_flag_set(cpu, VM86_OF, cpu->ah != 0);
        } else {
            uint32_t product = (uint32_t)ax * (uint32_t)value;

            cpu->ax = (uint16_t)product;
            cpu->dx = (uint16_t)(product >> 16);
            vm86_flag_set(cpu, VM86_CF, cpu->dx != 0);
            vm86_flag_set(cpu, VM86_OF, cpu->dx != 0);
        }
        break;

    case 5:   /* IMUL: signed, and the product goes in the same place. The
               * sign extension is of the result, not of the operands:
               * CF and OF are set when the high half is not a copy of the
               * sign of the low half, which is the same thing as saying
               * the product did not fit. */
        if (width == 8) {
            int16_t product = (int16_t)((int16_t)(int8_t)ax *
                                        (int16_t)(int8_t)value);
            bool    fits    = product == (int16_t)(int8_t)product;

            cpu->ax = (uint16_t)product;
            vm86_flag_set(cpu, VM86_CF, !fits);
            vm86_flag_set(cpu, VM86_OF, !fits);
        } else {
            int32_t product = (int32_t)(int16_t)ax * (int32_t)(int16_t)value;
            bool    fits    = product == (int32_t)(int16_t)product;

            cpu->ax = (uint16_t)product;
            cpu->dx = (uint16_t)((uint32_t)product >> 16);
            vm86_flag_set(cpu, VM86_CF, !fits);
            vm86_flag_set(cpu, VM86_OF, !fits);
        }
        break;

    case 6:   /* DIV: unsigned, and two ways to fail rather than one.
               *
               * A zero divisor is the obvious one. The other is a
               * quotient that will not fit -- 16 bits of dividend over an
               * 8-bit divisor can exceed FFh -- and it is not a corner
               * case to be tidied away: real DOS programs divide by a
               * value they suspect and use the fault to find out, so
               * truncating the quotient instead would silently give them
               * the wrong answer. */
        if (width == 8) {
            if (value == 0 || ax / value > 0xFFu) {
                cpu->fault = VM86_VECTOR_DIVIDE_ERROR;
                return VM86_FAULT;
            }

            cpu->al = (uint8_t)(ax / value);
            cpu->ah = (uint8_t)(ax % value);
        } else {
            uint32_t dividend = ((uint32_t)cpu->dx << 16) | ax;

            if (value == 0 || dividend / value > 0xFFFFu) {
                cpu->fault = VM86_VECTOR_DIVIDE_ERROR;
                return VM86_FAULT;
            }

            cpu->ax = (uint16_t)(dividend / value);
            cpu->dx = (uint16_t)(dividend % value);
        }
        break;

    default:  /* 7: IDIV. Signed, and the same two failures with signed
               * limits. The arithmetic is done at 64 bits so that the one
               * division the manual's range covers but the host's does
               * not -- the most negative dividend over -1 -- reaches the
               * range check instead of trapping in the emulator. */
        {
            int64_t dividend = (width == 8)
                ? (int64_t)(int16_t)ax
                : (int64_t)(int32_t)(((uint32_t)cpu->dx << 16) | ax);
            int64_t divisor = (width == 8) ? (int64_t)(int8_t)value
                                           : (int64_t)(int16_t)value;
            int64_t lowest  = (width == 8) ? -128 : -32768;
            int64_t highest = (width == 8) ?  127 :  32767;

            if (divisor == 0) {
                cpu->fault = VM86_VECTOR_DIVIDE_ERROR;
                return VM86_FAULT;
            }

            int64_t quotient  = dividend / divisor;
            int64_t remainder = dividend % divisor;

            if (quotient < lowest || quotient > highest) {
                cpu->fault = VM86_VECTOR_DIVIDE_ERROR;
                return VM86_FAULT;
            }

            if (width == 8) {
                cpu->al = (uint8_t)quotient;
                cpu->ah = (uint8_t)remainder;
            } else {
                cpu->ax = (uint16_t)quotient;
                cpu->dx = (uint16_t)remainder;
            }
        }
        break;
    }

    /* DIV and IDIV leave every flag undefined, and so does everything
     * else here that is not shown setting one. Nothing is done about
     * them: an undefined flag is not one this code can get right. */

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* FE: increment and decrement a byte                                  */
/* ------------------------------------------------------------------ */

static enum vm86_result op_incdec_rm8(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    struct vm86_modrm mr;
    vm86_fetch_modrm(cpu, &mr, 8);

    /*
     * FE is the byte half of the group FF is the word half of, and only
     * two of its eight sub-opcodes exist: /0 increment and /1 decrement.
     * The rest are not instructions on this part, and a program that
     * probes for one expects a clean refusal rather than a plausible
     * wrong answer.
     *
     * The operand may be a register or a memory byte. Worth stating
     * because the register case is not a curiosity: 40-47, the
     * one-byte forms, are inc and dec for the sixteen-bit registers
     * only. A byte register has no short form, so every `inc bl` and
     * `dec dh` a program contains arrives here -- there is no other
     * encoding an assembler could have emitted.
     */
    if (mr.reg > 1) {
        cpu->fault = VM86_VECTOR_INVALID_OPCODE;
        return VM86_FAULT;
    }

    uint16_t value = vm86_operand_read(cpu, &mr.operand);

    vm86_operand_write(cpu, &mr.operand,
                       vm86_alu_incdec(cpu, 8, value, mr.reg == 0));

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* The table                                                           */
/* ------------------------------------------------------------------ */

/* Everything this file owns, written out one opcode at a time so that the
 * gaps where the segment override prefixes live are visible rather than
 * implied. The four decimal adjustments are the single bytes between the
 * ALU ranges, and they are why 0x27, 0x2F, 0x37 and 0x3F appear on lines
 * of their own instead of being folded into the range above them. */
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
    /* 0x26 is the ES override prefix */
    [0x27] = op_decimal,
    [0x28] = op_alu_block, [0x29] = op_alu_block, [0x2A] = op_alu_block,
    [0x2B] = op_alu_block, [0x2C] = op_alu_block, [0x2D] = op_alu_block,
    /* 0x2E is the CS override prefix */
    [0x2F] = op_decimal,
    [0x30] = op_alu_block, [0x31] = op_alu_block, [0x32] = op_alu_block,
    [0x33] = op_alu_block, [0x34] = op_alu_block, [0x35] = op_alu_block,
    /* 0x36 is the SS override prefix */
    [0x37] = op_decimal,
    [0x38] = op_alu_block, [0x39] = op_alu_block, [0x3A] = op_alu_block,
    [0x3B] = op_alu_block, [0x3C] = op_alu_block, [0x3D] = op_alu_block,
    /* 0x3E is the DS override prefix */
    [0x3F] = op_decimal,

    [0x40] = op_incdec_reg, [0x41] = op_incdec_reg, [0x42] = op_incdec_reg,
    [0x43] = op_incdec_reg, [0x44] = op_incdec_reg, [0x45] = op_incdec_reg,
    [0x46] = op_incdec_reg, [0x47] = op_incdec_reg,

    [0x48] = op_incdec_reg, [0x49] = op_incdec_reg, [0x4A] = op_incdec_reg,
    [0x4B] = op_incdec_reg, [0x4C] = op_incdec_reg, [0x4D] = op_incdec_reg,
    [0x4E] = op_incdec_reg, [0x4F] = op_incdec_reg,

    [0x80] = op_group1, [0x81] = op_group1,
    [0x82] = op_group1, [0x83] = op_group1,

    [0x84] = op_test_rm, [0x85] = op_test_rm,

    [0x98] = op_cbw,     [0x99] = op_cwd,

    [0xA8] = op_test_accum, [0xA9] = op_test_accum,

    [0xC0] = op_shift, [0xC1] = op_shift,

    [0xD0] = op_shift, [0xD1] = op_shift,
    [0xD2] = op_shift, [0xD3] = op_shift,
    [0xD4] = op_aam,   [0xD5] = op_aad,

    [0xF6] = op_group3, [0xF7] = op_group3,

    [0xFE] = op_incdec_rm8,
};
