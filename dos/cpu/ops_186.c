/*
 * The 80186 additions.
 *
 * ---------------------------------------------------------------------
 * Owned opcodes
 *
 *   60, 61  PUSHA, POPA
 *   62      BOUND
 *   68, 6A  PUSH an immediate
 *   69, 6B  IMUL with two or three operands
 *   6C, 6D  INSB, INSW
 *   6E, 6F  OUTSB, OUTSW
 *   C0, C1  shift by an immediate count          (claimed by ops_alu.c)
 *   C8, C9  ENTER, LEAVE
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
 * LEAVE is ENTER's inverse and is two instructions: SP <- BP, pop BP.
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
 * one, so the loop is called, not copied.
 *
 * ---------------------------------------------------------------------
 * Decisions this file made, and why
 *
 * 1. THE PORTS DO NOT EXIST. There is no device at any port until M4
 *    brings one, so INS reads 0xFF (0xFFFF for the word form) and OUTS
 *    writes into nothing. This is task D's decision, made for IN and OUT
 *    in ops_ctl.c, applied here because it is the same machine and the
 *    same bus: a floating bus reads as all ones, and a write with nothing
 *    listening is dropped rather than trapped.
 *
 *    The consequence worth stating: both files now answer the port
 *    question independently. When M4 adds the device layer, both have to
 *    be pointed at it, or `in al, 60h` and `insb` will disagree about
 *    what a port is. There is no shared port helper to call, because
 *    there is no port layer yet and inventing one here would put a
 *    guessed interface into a frozen header. Reported rather than
 *    guessed at.
 *
 * 2. BOUND WITH mod == 3 IS AN INVALID OPCODE. `bound ax, bx` is not an
 *    instruction: the second operand is a pair of bounds in memory, and
 *    the register form has nowhere to put them. The encoding is
 *    undefined on the 186, so it raises vector 6 -- the same answer task D
 *    gave `FF` /3 and /5 with mod == 3, for the same reason.
 *
 * 3. IMUL SETS ONLY CF AND OF. SF, ZF, AF and PF are undefined after a
 *    multi-operand IMUL, and this leaves them as it found them rather
 *    than inventing a value. A program that branches on ZF after an IMUL
 *    is relying on an accident, and it will get the same accident it
 *    would have got on hardware that behaves the same way.
 *
 * 4. THE NESTING LEVEL IS TAKEN MODULO 32, as the manual specifies, so
 *    `enter 0, 0x21` builds a level 1 frame.
 */
#include <vm86/ops.h>

/* ------------------------------------------------------------------ */
/* 60, 61: PUSHA and POPA                                             */
/* ------------------------------------------------------------------ */

/*
 * The order is AX CX DX BX SP BP SI DI -- the encoding order, which is
 * not the order anyone would choose and not the order they are written in
 * an operand list.
 *
 * The SP pushed is the one from before the first push. Reading cpu->sp
 * again in the middle of the sequence -- the natural thing to write --
 * gets the value after four pushes, which is wrong by eight and
 * completely invisible until a program actually uses the saved value.
 *
 * This is NOT the `push sp` rule of ops_mov.c, and the two must not be
 * unified: `push sp` on an 8086 stores SP after the decrement, PUSHA on a
 * 186 stores it before any push. Different instructions, different parts,
 * different answers.
 */
static enum vm86_result op_pusha(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    uint16_t original_sp = cpu->sp;

    vm86_push16(cpu, cpu->ax);
    vm86_push16(cpu, cpu->cx);
    vm86_push16(cpu, cpu->dx);
    vm86_push16(cpu, cpu->bx);
    vm86_push16(cpu, original_sp);
    vm86_push16(cpu, cpu->bp);
    vm86_push16(cpu, cpu->si);
    vm86_push16(cpu, cpu->di);

    return VM86_CONTINUE;
}

/*
 * The same eight words, back in the same order, with the SP slot read and
 * thrown away.
 *
 * Writing it back would be the symmetric implementation, and it destroys
 * the stack pointer: the value on the stack is whatever the caller had
 * there, and the whole point of PUSHA is that a program can push eight
 * registers, work freely, and POPA them back. It fails loudly wherever
 * PUSHA and POPA are paired, which is the one mercy in it.
 */
static enum vm86_result op_popa(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    cpu->di = vm86_pop16(cpu);
    cpu->si = vm86_pop16(cpu);
    cpu->bp = vm86_pop16(cpu);
    (void)vm86_pop16(cpu);      /* the saved SP: meaningless, discarded */
    cpu->bx = vm86_pop16(cpu);
    cpu->dx = vm86_pop16(cpu);
    cpu->cx = vm86_pop16(cpu);
    cpu->ax = vm86_pop16(cpu);

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* 62: BOUND                                                          */
/* ------------------------------------------------------------------ */

static enum vm86_result op_bound(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    struct vm86_modrm mr;
    vm86_fetch_modrm(cpu, &mr, 16);

    /* `bound ax, bx` is not an instruction: the bounds are a pair of
     * words in memory and a register has nowhere to hold them. See the
     * note at the top of the file. */
    if (mr.mod == 3) {
        cpu->fault = VM86_VECTOR_INVALID_OPCODE;
        return VM86_FAULT;
    }

    /*
     * Low word first: [address] is the lower bound, [address + 2] the
     * upper, which is the same reversal as the far-pointer operands in
     * LES and LDS.
     *
     * The comparison is SIGNED, and both bounds are inside the range --
     * a value equal to either end passes. An unsigned comparison is the
     * natural thing to write and is wrong for every pair of bounds that
     * straddles zero, which is exactly the case where a program is
     * checking an array index that may be negative.
     */
    int16_t value = (int16_t)vm86_reg_get16(cpu, mr.reg);
    int16_t lower = (int16_t)vm86_mem_read16(cpu->mem, mr.operand.addr);
    int16_t upper = (int16_t)vm86_mem_read16(cpu->mem, mr.operand.addr + 2);

    if (value < lower || value > upper) {
        cpu->fault = VM86_VECTOR_BOUND;
        return VM86_FAULT;
    }

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* 68, 6A: PUSH an immediate                                          */
/* ------------------------------------------------------------------ */

/*
 * Bit 1 is the width of the immediate, not bit 0 as in every other pair
 * in the map: 68 takes a word and 6A takes a byte.
 *
 * The byte form is SIGN-EXTENDED, so `6A FB` pushes 0xFFFB and not
 * 00FB. It is the same rule as the 83 form of the arithmetic group, and
 * it has to be: a compiler emitting `push 5` for a subroutine that
 * expects 16 bits is emitting `push -5` for one that expects -5.
 */
static enum vm86_result op_push_imm(struct vm86_cpu *cpu, uint8_t opcode)
{
    if (opcode & 2)
        vm86_push16(cpu, vm86_sign_extend8(vm86_fetch8(cpu)));
    else
        vm86_push16(cpu, vm86_fetch16(cpu));

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* 69, 6B: IMUL with an immediate                                     */
/* ------------------------------------------------------------------ */

/*
 * `imul r16, r/m16, imm16` and its byte-immediate twin.
 *
 * This is not the IMUL of ops_alu.c, and the difference is not a
 * detail: F6/F7 produce a 32-bit product in DX:AX and set CF when the
 * high half is non-zero, while this one keeps only the low sixteen bits
 * and sets CF and OF when the bits it threw away were not just the sign.
 * Same mnemonic, different instruction, and sharing an implementation
 * between them would give one of the two the wrong flags.
 *
 * The ModRM comes first, then the displacement, then the immediate: the
 * source is resolved before the immediate byte is read, and reading the
 * operand itself does not touch the instruction stream.
 *
 * The product is computed in 32 bits, where it cannot overflow: two
 * sixteen-bit values multiply into at most thirty-one bits, so the cast
 * to int32_t is exact and the test for "did it fit" is a comparison
 * rather than a guess.
 */
static enum vm86_result op_imul_imm(struct vm86_cpu *cpu, uint8_t opcode)
{
    struct vm86_modrm mr;
    vm86_fetch_modrm(cpu, &mr, 16);

    int32_t source = (int16_t)vm86_operand_read(cpu, &mr.operand);
    int32_t immediate = (opcode & 2)
        ? (int32_t)(int16_t)vm86_sign_extend8(vm86_fetch8(cpu))
        : (int32_t)(int16_t)vm86_fetch16(cpu);

    int32_t product = source * immediate;
    uint16_t result = (uint16_t)product;

    vm86_reg_set16(cpu, mr.reg, result);

    /* Fits when the discarded sixteen bits are nothing but a copy of the
     * sign bit of what is left -- which is what comparing the truncated
     * value sign-extended back against the product asks. */
    bool does_not_fit = ((int32_t)(int16_t)result != product);

    vm86_flag_set(cpu, VM86_CF, does_not_fit);
    vm86_flag_set(cpu, VM86_OF, does_not_fit);

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* 6C-6F: INS and OUTS                                                */
/* ------------------------------------------------------------------ */

/*
 * The port problem, restated where the code that depends on it is: no
 * device answers at any port until M4. INS therefore stores the value a
 * floating bus returns and OUTS sends its word to nobody. This is
 * ops_ctl.c's decision for IN and OUT, and it has to be the same decision
 * or the machine would have two ideas about what a port is.
 *
 * The port number is in DX for both, and neither body looks at it --
 * there is nothing yet that could care.
 */

/* ES:DI <- the port, then DI steps. Never through a segment override:
 * the write side of a string instruction is ES on this processor and no
 * prefix moves it. */
static void ins_body(struct vm86_cpu *cpu, uint8_t bits, int16_t delta,
                     enum vm86_seg source)
{
    uint32_t dest = vm86_linear(cpu, VM86_ES, cpu->di);

    (void)source;   /* INS has no DS-side operand to override */

    if (bits == 8)
        vm86_mem_write8(cpu->mem, dest, 0xFF);
    else
        vm86_mem_write16(cpu->mem, dest, 0xFFFF);

    cpu->di = (uint16_t)(cpu->di + delta);
}

/* The port <- DS:SI, then SI steps. `source` is the DS side with the
 * override already applied by the loop, so a prefixed OUTS reads where
 * the prefix says. */
static void outs_body(struct vm86_cpu *cpu, uint8_t bits, int16_t delta,
                      enum vm86_seg source)
{
    uint32_t src = vm86_linear(cpu, source, cpu->si);

    /*
     * The read is performed and the write is dropped, which is what a
     * machine with an empty bus does. Keeping the read visible rather
     * than deleting it is not sentiment: it is the half of the
     * instruction M4 replaces with a real device.
     */
    uint16_t value = (bits == 8) ? vm86_mem_read8(cpu->mem, src)
                                 : vm86_mem_read16(cpu->mem, src);
    (void)value;

    cpu->si = (uint16_t)(cpu->si + delta);
}

/*
 * The REP loop is ops_str.c's, called and not copied: same prefix, same
 * direction flag, same CX=0 rule, same "the DS side takes the override
 * and the ES side never does". Two implementations of that would not
 * stay equal, and the bug they would produce -- `rep movsb` looping and
 * `rep insb` moving one byte -- is one a program would get blamed for.
 *
 * `conditional` is false: there is no comparison here for a flag to stop
 * on, which also makes F2 mean REP without a branch of our own.
 */
static enum vm86_result op_ins(struct vm86_cpu *cpu, uint8_t opcode)
{
    return vm86_str_repeat(cpu, (opcode & 1) ? 16 : 8, ins_body, false);
}

static enum vm86_result op_outs(struct vm86_cpu *cpu, uint8_t opcode)
{
    return vm86_str_repeat(cpu, (opcode & 1) ? 16 : 8, outs_body, false);
}

/* ------------------------------------------------------------------ */
/* C8, C9: ENTER and LEAVE                                            */
/* ------------------------------------------------------------------ */

/*
 * ENTER imm16, imm8: `alloc` bytes of locals, and a nesting level that
 * says how many frames out to link back to.
 *
 * The pseudocode, from the 80186 manual, and it is worth reading once:
 *
 *     push BP
 *     frame = SP
 *     for i = 1 to level - 1:
 *         BP = BP - 2
 *         push word at SS:BP
 *     if level > 0:
 *         push frame
 *     BP = frame
 *     SP = SP - alloc
 *
 * The loop walks the caller's chain of frame pointers, which is what the
 * nesting level is for: it copies the frame pointer of each enclosing
 * scope onto the new frame, so that a block-structured language can reach
 * a variable that belongs to an outer block by following the chain. For
 * level 0 and 1 the loop does not run at all, which is why 0 is so easy
 * to mistake for the whole instruction -- only from level 2 up does the
 * loop execute, and then it executes level - 1 times.
 *
 * BP is decremented inside the loop rather than being read from the
 * stack: `push word at SS:BP` is a dereference of the CALLER's frame
 * pointer, and BP is not assigned the new frame until after the loop.
 *
 * The level is taken modulo 32, as the hardware does. `enter 0, 33` is a
 * level 1 frame.
 */
static enum vm86_result op_enter(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    uint16_t alloc = vm86_fetch16(cpu);
    uint8_t  level = (uint8_t)(vm86_fetch8(cpu) & 0x1Fu);

    vm86_push16(cpu, cpu->bp);

    uint16_t frame = cpu->sp;

    for (uint8_t i = 1; i < level; i++) {
        cpu->bp = (uint16_t)(cpu->bp - 2);
        vm86_push16(cpu, vm86_mem_read16(cpu->mem,
                                         vm86_linear(cpu, VM86_SS, cpu->bp)));
    }

    if (level > 0)
        vm86_push16(cpu, frame);

    cpu->bp = frame;
    cpu->sp = (uint16_t)(cpu->sp - alloc);

    return VM86_CONTINUE;
}

/*
 * LEAVE is the inverse: SP <- BP, then pop BP.
 *
 * It unwinds a frame of any nesting level, which is not obvious from the
 * code above: ENTER pushes the caller's BP first and everything else
 * below it, and then points BP at that first word. So [BP] is the
 * caller's BP whatever the level, SP <- BP lands on it, and the pop
 * finishes where ENTER started. The chain words and the copy of the
 * frame pointer are left below the restored SP -- free space, holding
 * what was a moment ago a frame.
 */
static enum vm86_result op_leave(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    cpu->sp = cpu->bp;
    cpu->bp = vm86_pop16(cpu);

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* The table                                                           */
/* ------------------------------------------------------------------ */

const vm86_op_fn vm86_ops_186[256] = {
    [0x60] = op_pusha,
    [0x61] = op_popa,
    [0x62] = op_bound,

    [0x68] = op_push_imm,
    [0x69] = op_imul_imm,
    [0x6A] = op_push_imm,
    [0x6B] = op_imul_imm,

    [0x6C] = op_ins,  [0x6D] = op_ins,
    [0x6E] = op_outs, [0x6F] = op_outs,

    [0xC8] = op_enter,
    [0xC9] = op_leave,

    /* C0 and C1 are 186 instructions too, and they are not here on
     * purpose: they are shifts, the shift helpers live in ops_alu.c, and
     * that file claims them. A second entry for either would put the
     * opcode in two tables at once, which vm86_ops_build() reports at
     * startup rather than letting one of us silently win. */
};
