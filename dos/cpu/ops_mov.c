/*
 * Data movement, and the stack.
 *
 * ---------------------------------------------------------------------
 * Owned opcodes
 *
 *   50-5F   PUSH/POP a register                         [DONE - example]
 *   86-87   XCHG r/m with a register                            [DONE]
 *   88-8B   MOV r/m, r and r, r/m                               [DONE]
 *   8C-8E   MOV a segment register out and in                   [DONE]
 *   8D      LEA                                                 [DONE]
 *   8F      POP r/m16                                           [DONE]
 *   90      NOP                                                 [DONE]
 *   91-97   XCHG AX with a register                             [DONE]
 *   A0-A3   MOV AL/AX, [moffs]                                  [DONE]
 *   B0-BF   MOV a register, immediate                   [DONE - example]
 *   C4-C5   LES, LDS                                            [DONE]
 *   C6-C7   MOV r/m, immediate                                  [DONE]
 *   D7      XLAT                                                [DONE]
 *
 * The stack helpers below are complete and are what the other groups
 * call. Do not reimplement them.
 *
 * ---------------------------------------------------------------------
 * Traps worth knowing before you start
 *
 * LEA computes an effective address and does NOT read memory.
 *
 * That is the entire point of the instruction and it is easy to lose
 * sight of: `lea bx, [bp+si]` runs the addressing hardware and stores
 * the resulting offset in BX, with no access to the address it computed.
 * A handler written by reaching for vm86_operand_read() will read memory
 * the program never asked for -- which usually does no harm, which is
 * what makes it a bad bug to find later.
 *
 * The segment override prefix still affects a LEA, in the narrow sense
 * that it selects the segment the operand is formed in. It does not
 * change the answer: what LEA stores is the offset, and the segment base
 * is not part of the offset. `lea bx, es:[bp+si]` and `lea bx, [bp+si]`
 * differ in which segment's base the addressing hardware would have
 * added, and they agree in what ends up in BX. Programs use LEA exactly
 * because it hands back an offset, so an implementation that folded a
 * segment base into the result would be wrong for every program that
 * has a segment register loaded with anything but zero -- and right for
 * everything in a test suite whose segments are all zero. See
 * op_lea() for the arithmetic that gets the offset back out.
 *
 * LES and LDS load a far pointer: two words from memory, one into a
 * register and one into ES or DS.
 *
 * `les di, [bx]` reads DI from [bx] and ES from [bx+2]. The register
 * comes first in memory and the segment second, which is the opposite of
 * how the pair is written -- and the same reversal as the far CALL and
 * JMP encodings, so it is worth learning once.
 *
 * PUSH SP. On the 8086 it pushes the value of SP *after* the decrement;
 * on the 80186 and later it pushes the value *before*. This is a
 * documented difference and it is how some software tells the two apart,
 * so it is a decision rather than a detail. This emulator claims to be an
 * 8086, so 8086 behaviour is the right answer -- but write down that you
 * made the choice, because the next person will assume the other one.
 *
 * PUSH SP is the one case in this range where the register file cannot be
 * read for the value to push: vm86_push16() moves SP itself, so reading
 * SP and passing the result in would push the value SP had before the
 * decrement, which is the 80186's answer and not this part's.
 * op_push_pop_reg() computes that one case itself.
 *
 * XCHG AX, AX is NOP, and it is also opcode 0x90. There is no separate
 * NOP instruction. That is not trivia: it means NOP must not be given a
 * table entry that shadows the 0x90 XCHG form, because there isn't one.
 * Practical consequence here: op_xchg_ax() must not be installed at
 * 0x90, or it would be indistinguishable from correct -- which is why
 * the table spells 0x90 out as op_nop rather than filling the range.
 *
 * ---------------------------------------------------------------------
 * Encodings that name no operation
 *
 * The manual states some encodings outright to be invalid and promises
 * nothing about others; some of those the silicon is reported to carry
 * out anyway. Every one of them raises the invalid-opcode exception here
 * (vector 6) rather than being executed as whatever it resembles:
 *
 *   8E /1           MOV CS, r/m16. CS is not a destination a MOV may
 *                   name -- changing CS is what a far transfer does, and
 *                   it does it with the new IP in the same instruction.
 *                   The 8086 silicon is reported to execute the encoding
 *                   regardless, setting CS and continuing at the new CS
 *                   with the old IP; the manual promises nothing either
 *                   way. Refusing is the choice that keeps a
 *                   mis-assembled far jump at the instruction that is
 *                   wrong instead of transferring control somewhere no
 *                   source line asks for.
 *   8C, 8E /4-7     reg fields above DS, which name no segment register.
 *   8F /1-7         the group has one operation, POP r/m16, and it is /0.
 *   C6, C7 /1-7     likewise: the immediate forms are /0.
 *   8D mod 11       LEA takes a memory operand; there is no address to
 *   C4, C5 mod 11   compute or load a far pointer from. Both read memory
 *                   and neither has a register form.
 *
 * Refusing them is the same answer the run loop already gives an opcode
 * nobody claims: on this processor it is not an instruction, so say so
 * and stop, rather than inventing a meaning that the next reader will
 * take as deliberate. It is chosen over aliasing (masking the reg field
 * down to three bits, say) because aliasing is silent, and over doing
 * nothing because doing nothing is worse: the instruction leaves no
 * trace of having been reached.
 *
 * What it costs: a program written for a later processor that does alias
 * these encodings breaks loudly here instead of quietly working. No
 * 8086-era program is in that category -- the encodings are undefined on
 * the part they were written for, so an assembler would not emit one
 * unless it was trying to be undefined.
 *
 * ---------------------------------------------------------------------
 * MOV to a segment register, and the instruction that follows it
 *
 * `mov ss, ax` / `mov sp, stack_top` is a pair that must not be
 * interruptible in the middle: an interrupt arriving between the two
 * would run on a stack pointer half of which belongs to the old stack.
 * The pair is nevertheless written without disabling interrupts, and it
 * is correct as written, because on the 8086 the instruction after a
 * load of SS does not recognise interrupts.
 *
 * What is deferred is interrupt recognition, not the write. The segment
 * register holds its new value as soon as the instruction that wrote it
 * has finished, and the instruction after it reads the new value. That
 * is the 8086's behaviour, so changing the register at once, as
 * vm86_set_seg() does, is the correct implementation and not a
 * deviation from one.
 *
 * One instruction of grace, and only after SS. For the record, since the
 * repository holds no 8086 manual: this is the documented behaviour of
 * the part, and there is a further wrinkle on top of it. The inhibition
 * Intel added is wider than the problem it fixes -- on the original 8086
 * it applies after any MOV or POP to a segment register, not only SS.
 * That wider reading is deliberately not implemented. struct vm86_cpu
 * specifies the shadow as "STI or a load of SS", and everything that
 * depends on any of this depends on the SS case, which is the one that
 * keeps a stack switch atomic. Recorded here rather than left silent
 * because a reader who knows the erratum would otherwise read the code
 * as a mistake.
 *
 * The shadow is a count in cpu->intr_shadow: set here, decremented at
 * each instruction boundary by the run loop, and written by nothing
 * else. POP SS carries the same assignment, in ops_alu.c -- the segment
 * pop encodings live in the arithmetic block, not here.
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
/* Operand plumbing                                                    */
/* ------------------------------------------------------------------ */

/*
 * A register operand, for the instructions whose register side is named
 * by the ModRM reg field rather than by the opcode.
 *
 * Built by hand because vm86_fetch_modrm() resolves only the r/m side.
 * Once built it is the same kind of thing to every accessor, which is
 * what lets `mov [bx], ax` and `mov ax, [bx]` share one function and
 * differ only in which way round the two operands are passed.
 */
static struct vm86_operand reg_operand(uint8_t width, uint8_t index)
{
    struct vm86_operand op;

    op.kind  = VM86_OPERAND_REGISTER;
    op.width = width;
    op.reg   = index;
    op.addr  = 0;
    op.value = 0;
    op.seg   = VM86_NO_SEGMENT;

    return op;
}

/*
 * An instruction this processor does not have, reached with a ModRM byte
 * that has already been consumed.
 *
 * IP is left pointing past the encoding, which is where the run loop's
 * own invalid-opcode path leaves it too: it fetches the opcode and then
 * faults. Delivering the exception -- pushing a return address, reading
 * the vector out of the IVT -- is M4's work, and when it exists it will
 * want an IP that can be used the same way on both paths.
 */
static enum vm86_result invalid_instruction(struct vm86_cpu *cpu)
{
    cpu->fault = VM86_VECTOR_INVALID_OPCODE;
    return VM86_FAULT;
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

    if (opcode & 0x08) {
        vm86_reg_set16(cpu, index, vm86_pop16(cpu));
    } else if (index == VM86_REG_SP) {
        /*
         * PUSH SP, the one opcode in this range whose value cannot be
         * read out of the register file. The 8086 pushes the SP it has
         * *after* the decrement, so what the helper is handed has to be
         * the value the decrement will leave behind -- pushing
         * vm86_reg_get16(cpu, SP) instead, as every other register in
         * this range does, is the 80186 behaviour, and programs of the
         * era run this instruction precisely to find out which of the
         * two they are on.
         */
        vm86_push16(cpu, (uint16_t)(cpu->sp - 2));
    } else {
        vm86_push16(cpu, vm86_reg_get16(cpu, index));
    }

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
/* 88-8B: MOV, the general case                                        */
/* ------------------------------------------------------------------ */

/*
 * Four opcodes, one function:
 *
 *   bit 0   width: 0 for a byte, 1 for a word
 *   bit 1   direction: 0 for `to r/m`, 1 for `to the register`
 *
 * Nothing here or anywhere else in this file writes to FLAGS. MOV is
 * what a program puts between two other instructions when it wants to
 * keep a value across them, and a MOV that set flags would make the
 * carry out of the first instruction unavailable a line later.
 */
static enum vm86_result op_mov(struct vm86_cpu *cpu, uint8_t opcode)
{
    uint8_t width       = (opcode & 1) ? 16 : 8;
    bool    to_register = (opcode & 2) != 0;

    struct vm86_modrm mr;
    vm86_fetch_modrm(cpu, &mr, width);

    struct vm86_operand reg = reg_operand(width, mr.reg);

    if (to_register)
        vm86_operand_write(cpu, &reg, vm86_operand_read(cpu, &mr.operand));
    else
        vm86_operand_write(cpu, &mr.operand, vm86_operand_read(cpu, &reg));

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* 86-87: XCHG r/m with a register, and 91-97                          */
/* ------------------------------------------------------------------ */

static enum vm86_result op_xchg(struct vm86_cpu *cpu, uint8_t opcode)
{
    uint8_t width = (opcode & 1) ? 16 : 8;

    struct vm86_modrm mr;
    vm86_fetch_modrm(cpu, &mr, width);

    struct vm86_operand reg = reg_operand(width, mr.reg);

    /*
     * Both operands are read before either is written, and it matters:
     * with mod 11 the r/m side and the reg side can be the same register
     * -- every `xchg ax, ax` is, which is why 0x90 is a NOP -- and a
     * write-back in between would lose one of the two values.
     *
     * The hardware takes an implicit LOCK on the memory form. There is
     * one processor here, so there is nothing for it to exclude, and the
     * prefix is accepted and ignored by the run loop. Worth knowing
     * anyway: on a machine with two of them, this is the instruction
     * where getting it wrong stops being theoretical.
     */
    uint16_t from_memory   = vm86_operand_read(cpu, &mr.operand);
    uint16_t from_register = vm86_operand_read(cpu, &reg);

    vm86_operand_write(cpu, &mr.operand, from_register);
    vm86_operand_write(cpu, &reg, from_memory);

    return VM86_CONTINUE;
}

/*
 * 91-97: XCHG AX with one of the other registers. 0x90 is the same
 * encoding with index 0 -- XCHG AX, AX -- and is served by op_nop
 * instead, because the two are the same instruction and only one of
 * them has ever been called by name.
 */
static enum vm86_result op_xchg_ax(struct vm86_cpu *cpu, uint8_t opcode)
{
    uint8_t index = (uint8_t)(opcode & 7);

    uint16_t other = vm86_reg_get16(cpu, index);

    vm86_reg_set16(cpu, index, cpu->ax);
    cpu->ax = other;

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* 8C-8E: moving segment registers out and in                          */
/* ------------------------------------------------------------------ */

/*
 * Both directions are one function because the ModRM byte is read the
 * same way and only the direction of the transfer differs.
 *
 * 8C reads any of the four segment registers into any r/m16, CS
 * included: reading CS is how a program finds out where its own code
 * segment is.
 *
 * 8E writes one, and reg = 1 is MOV CS, r/m16, which the programming
 * model does not allow: CS changes only through a far transfer, which is
 * the one place where the new value and the new IP arrive together. What
 * the part does with the encoding anyway is in the note at the top of
 * this file.
 */
static enum vm86_result op_mov_sreg(struct vm86_cpu *cpu, uint8_t opcode)
{
    struct vm86_modrm mr;
    vm86_fetch_modrm(cpu, &mr, 16);

    if (mr.reg > VM86_DS || (opcode == 0x8E && mr.reg == VM86_CS))
        return invalid_instruction(cpu);

    if (opcode == 0x8C) {
        vm86_operand_write(cpu, &mr.operand,
                           vm86_get_seg(cpu, (enum vm86_seg)mr.reg));
        return VM86_CONTINUE;
    }

    /*
     * The operand is read first. It is a resolved address by now, so the
     * order does not change which bytes are read -- but `mov ds, [bx]`
     * is a program reading its new data segment through its old one, and
     * doing the read before the write says so.
     */
    uint16_t value = vm86_operand_read(cpu, &mr.operand);

    vm86_set_seg(cpu, (enum vm86_seg)mr.reg, value);

    /*
     * 8E /2 is MOV SS, and the instruction after it does not recognise
     * interrupts -- which is the whole reason `mov ss, ax` / `mov sp, ...`
     * is written without a CLI around it. See the note at the top of this
     * file for what is deferred and what is not.
     *
     * The shadow is set here rather than inside vm86_set_seg() because a
     * segment write is not the same event as a guest loading SS: the far
     * transfers and the interrupt frame restore CS through the same
     * helper, and neither of those is followed by a grace instruction.
     */
    if (mr.reg == VM86_SS)
        cpu->intr_shadow = 1;

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* 8D: LEA                                                            */
/* ------------------------------------------------------------------ */

/*
 * The effective address, and deliberately not the memory at it.
 *
 * Running the addressing hardware and keeping the result is the entire
 * point of the instruction: no read, no write, and no fault where the
 * address points past the end of memory. So there is no
 * vm86_operand_read() here, and a version with one would pass most
 * tests -- it would be wrong only for a program that uses LEA to compute
 * an address outside its buffer on the way to filling it in.
 *
 * vm86_fetch_modrm() hands back a linear address, so the offset is that
 * with the segment base taken back off. The arithmetic is exact:
 * decode.c wraps the offset at sixteen bits and only then adds the base,
 * so addr - base is the offset the hardware would have stored. The
 * ModRM r/m field also names the destination register, which is why LEA
 * is one of the few instructions whose reg and r/m sides are not a
 * source and a destination.
 *
 * A segment override changes which base is subtracted and nothing else,
 * so `lea bx, es:[si]` and `lea bx, [si]` put the same number in BX.
 * That is what the hardware does: the segment base is not part of the
 * "offset" LEA is named for, and a version that added it would be wrong
 * for every program whose segments are not all zero.
 *
 * mod 11 is refused: LEA has no register operand, and the register
 * number the encoding would then name is not an address to compute.
 */
static enum vm86_result op_lea(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    struct vm86_modrm mr;
    vm86_fetch_modrm(cpu, &mr, 16);

    if (mr.operand.kind != VM86_OPERAND_MEMORY)
        return invalid_instruction(cpu);

    uint32_t base = vm86_linear(cpu, mr.operand.seg, 0);

    vm86_reg_set16(cpu, mr.reg, (uint16_t)(mr.operand.addr - base));

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* 8F: POP r/m16                                                       */
/* ------------------------------------------------------------------ */

/*
 * The order of the three steps is the whole instruction, and the
 * hardware's order is: work out where the destination is, read the word
 * off the stack, move SP past it, store the word.
 *
 * The destination is worked out first because the ModRM decode above
 * does it -- and the operand carries a linear address from that moment,
 * so nothing that happens to SP afterwards can change where the store
 * lands. SP is moved before the store for the reason a `pop sp` relies
 * on: the register SP ends up holding the word that was popped, not that
 * word plus two.
 */
static enum vm86_result op_pop_rm(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    struct vm86_modrm mr;
    vm86_fetch_modrm(cpu, &mr, 16);

    /* POP r/m16 is the whole of group 8F, and it is /0. */
    if (mr.reg != 0)
        return invalid_instruction(cpu);

    uint16_t value = vm86_pop16(cpu);

    vm86_operand_write(cpu, &mr.operand, value);

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* A0-A3: MOV AL/AX, and the other way, with a direct address          */
/* ------------------------------------------------------------------ */

/*
 * The odd ones out: no ModRM byte at all. The operand is the sixteen-bit
 * address that follows the opcode, taken as an offset in DS -- the
 * segment override prefix still applies, and it is why
 * `mov al, es:[0x1234]` has a shorter encoding than the ModRM form
 * rather than the same one.
 *
 * The address is an offset, not a linear address, so it goes through
 * vm86_linear() like any other operand. Reading it as a linear address
 * would work in a test whose segments are zero and nowhere else.
 */
static enum vm86_result op_mov_moffs(struct vm86_cpu *cpu, uint8_t opcode)
{
    uint8_t       width = (opcode & 1) ? 16 : 8;
    bool          store = (opcode & 2) != 0;
    enum vm86_seg seg   = vm86_effective_seg(cpu, VM86_DS);

    uint16_t offset = vm86_fetch16(cpu);

    struct vm86_operand mem;
    mem.kind  = VM86_OPERAND_MEMORY;
    mem.width = width;
    mem.reg   = 0;
    mem.addr  = vm86_linear(cpu, seg, offset);
    mem.value = 0;
    mem.seg   = seg;

    /* Index 0 is AL at eight bits and AX at sixteen, so one operand
     * serves both widths. */
    struct vm86_operand acc = reg_operand(width, VM86_REG_AX);

    if (store)
        vm86_operand_write(cpu, &mem, vm86_operand_read(cpu, &acc));
    else
        vm86_operand_write(cpu, &acc, vm86_operand_read(cpu, &mem));

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* C4-C5: LES and LDS                                                  */
/* ------------------------------------------------------------------ */

/*
 * Two words out of memory: the register first, the segment second. The
 * pair is written the other way round -- `les di, [bx]` names DI before
 * ES -- and the encoding follows the memory layout rather than the
 * syntax. Far CALL and far JMP are laid out the same way, so this is the
 * second of three places a reader meets it and the last one that is easy
 * to get backwards.
 *
 * Both loads are done before either is written. The address is resolved
 * by then, so it cannot matter, but LES through an override is a real
 * case: `26 C4 1E ...` reads through ES and then loads ES, and a
 * version that wrote the segment register first would read the wrong
 * bytes. Writing the register after the reads is what makes the order
 * impossible to get wrong.
 *
 * mod 11 is refused: both instructions read two words from memory, and
 * there is no memory to read them from.
 */
static enum vm86_result op_load_far_ptr(struct vm86_cpu *cpu, uint8_t opcode)
{
    struct vm86_modrm mr;
    vm86_fetch_modrm(cpu, &mr, 16);

    if (mr.operand.kind != VM86_OPERAND_MEMORY)
        return invalid_instruction(cpu);

    uint16_t offset  = vm86_mem_read16(cpu->mem, mr.operand.addr);
    uint16_t segment = vm86_mem_read16(cpu->mem, mr.operand.addr + 2);

    vm86_reg_set16(cpu, mr.reg, offset);
    vm86_set_seg(cpu, opcode == 0xC4 ? VM86_ES : VM86_DS, segment);

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* C6-C7: MOV r/m, immediate                                           */
/* ------------------------------------------------------------------ */

/*
 * Group 11 has one operation, so the reg field is a constant zero and an
 * encoding that puts anything else there is not this instruction.
 *
 * The immediate is fetched after vm86_fetch_modrm() has consumed the
 * displacement. That ordering is the whole reason decode.h says so at
 * length: with mod 01 or 10 the displacement comes first, and a handler
 * that reads its immediate before decoding ModRM takes the displacement
 * byte for the low half of the immediate, on every instruction where the
 * displacement is not zero and silently.
 */
static enum vm86_result op_mov_imm_rm(struct vm86_cpu *cpu, uint8_t opcode)
{
    uint8_t width = (opcode == 0xC7) ? 16 : 8;

    struct vm86_modrm mr;
    vm86_fetch_modrm(cpu, &mr, width);

    if (mr.reg != 0)
        return invalid_instruction(cpu);

    uint16_t value = (width == 8) ? vm86_fetch8(cpu) : vm86_fetch16(cpu);

    vm86_operand_write(cpu, &mr.operand, value);

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* D7: XLAT                                                            */
/* ------------------------------------------------------------------ */

/*
 * `mov al, [bx + al]`, with three details that are the whole of it.
 *
 * The base register is BX and nothing else. The offset is summed at
 * sixteen bits before the segment base goes on, so a table that runs off
 * the end of a segment wraps to the start of it rather than into the
 * next one -- the same order decode.c uses for a ModRM operand, and the
 * reason the addition is written out here rather than folded into
 * vm86_linear().
 *
 * The segment is DS unless a prefix says otherwise. That is worth
 * stating because XLAT is the one instruction where the segment register
 * being read is not visible in the operand -- there is no operand.
 *
 * No flags are touched, at all: not the ones a table lookup might be
 * expected to set, and not the ones a MOV would.
 */
static enum vm86_result op_xlat(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    enum vm86_seg seg = vm86_effective_seg(cpu, VM86_DS);

    uint16_t offset = (uint16_t)(cpu->bx + cpu->al);

    cpu->al = vm86_mem_read8(cpu->mem, vm86_linear(cpu, seg, offset));

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

    /* XCHG comes in two shapes: with any register at 86/87, and with AX
     * alone at 91-97. 0x90 belongs to the second shape and is spelled
     * out as the NOP it is. */
    [0x86] = op_xchg, [0x87] = op_xchg,

    [0x88] = op_mov, [0x89] = op_mov, [0x8A] = op_mov, [0x8B] = op_mov,

    [0x8C] = op_mov_sreg, [0x8D] = op_lea, [0x8E] = op_mov_sreg,
    [0x8F] = op_pop_rm,

    [0x90] = op_nop,

    [0x91] = op_xchg_ax, [0x92] = op_xchg_ax, [0x93] = op_xchg_ax,
    [0x94] = op_xchg_ax, [0x95] = op_xchg_ax, [0x96] = op_xchg_ax,
    [0x97] = op_xchg_ax,

    [0xA0] = op_mov_moffs, [0xA1] = op_mov_moffs,
    [0xA2] = op_mov_moffs, [0xA3] = op_mov_moffs,

    [0xB0] = op_mov_imm, [0xB1] = op_mov_imm, [0xB2] = op_mov_imm,
    [0xB3] = op_mov_imm, [0xB4] = op_mov_imm, [0xB5] = op_mov_imm,
    [0xB6] = op_mov_imm, [0xB7] = op_mov_imm,

    [0xB8] = op_mov_imm, [0xB9] = op_mov_imm, [0xBA] = op_mov_imm,
    [0xBB] = op_mov_imm, [0xBC] = op_mov_imm, [0xBD] = op_mov_imm,
    [0xBE] = op_mov_imm, [0xBF] = op_mov_imm,

    [0xC4] = op_load_far_ptr, [0xC5] = op_load_far_ptr,

    [0xC6] = op_mov_imm_rm, [0xC7] = op_mov_imm_rm,

    [0xD7] = op_xlat,
};
