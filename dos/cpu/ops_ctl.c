/*
 * Control flow, interrupts, flags, and I/O.
 *
 * ---------------------------------------------------------------------
 * What is here
 *
 *   70-7F   the sixteen conditional jumps: one function, one table
 *   9A, EA  far CALL and far JMP
 *   9B      WAIT
 *   9C-9F   PUSHF, POPF, SAHF, LAHF
 *   C2, C3  RET near, with and without a stack adjustment
 *   CA, CB  RET far, likewise
 *   CC      INT 3
 *   CD      INT imm8
 *   CE      INTO
 *   CF      IRET
 *   E0-E3   LOOPNE, LOOPE, LOOP, JCXZ
 *   E4-E7   IN and OUT with an immediate port
 *   E8, E9  CALL and JMP near
 *   EB      JMP short
 *   EC-EF   IN and OUT through DX
 *   F4      HLT
 *   F5      CMC
 *   F8-FD   CLC, STC, CLI, STI, CLD, STD
 *   FF      group 5: INC/DEC memory, CALL, JMP, PUSH
 *
 * ---------------------------------------------------------------------
 * Notes that will save you time
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
 * store 5678 then 1234. A far pointer in memory is laid out the same way:
 * offset at the low address, segment two bytes above it.
 *
 * Flags worth care:
 *
 *   POPF, SAHF and IRET write part or all of the flags register, so all
 *   three call vm86_flags_normalise() afterwards or the bits that are
 *   supposed to read as one take whatever the guest put there. See cpu.h:
 *   a program that pushes FLAGS to find out what it is running on is not
 *   an unusual thing for this era, and getting those bits wrong sends it
 *   down a code path for a different processor.
 *
 *   IRET pops IP, CS and FLAGS, in that order, which is the reverse of
 *   the order INT pushed them. Getting it wrong tends to look like a jump
 *   to a random address, because that is what it is.
 *
 *   CLI, STI and the rest of the flag instructions do not touch anything
 *   but their own flag.
 *
 *   STI does not take effect on real hardware until the instruction after
 *   it has executed. That is what makes the `sti / hlt` idle loop work: if
 *   the interrupt were taken between the two, the HLT would follow an
 *   interrupt that has already been consumed and would wait forever.
 *   What waits is *recognition*, not the flag: STI sets IF at once, and
 *   only the taking of an interrupt is held off. Modelling it the other
 *   way -- deferring the write to IF -- would break the `pushf` straight
 *   after an STI, and that mistake has been made once in this project
 *   already. The delay is a count in cpu->intr_shadow, which the run loop
 *   decrements at each instruction boundary and honours by not
 *   delivering while it is nonzero. See op_set_flag().
 *
 * IN and OUT are here rather than in their own file because there is
 * nothing for them to talk to yet. A real implementation needs a port
 * dispatch table, which arrives with the hardware emulation in M4. Until
 * then they accept the encoding and do nothing observable: a read returns
 * 0xFF, which is what a floating bus gives back and the same answer an
 * unmapped memory address gives, and a write is dropped. Neither raises
 * an exception. A program that probes a port and expects a defined answer
 * has to keep working, and one that probes by reading until it sees
 * something other than 0xFF has to terminate rather than hang.
 *
 * Opcode FF is group 5 and this file owns it, but two of its eight
 * sub-operations are INC and DEC, which are arithmetic. They call
 * vm86_alu_incdec() from ops.h rather than being a second copy: the flags
 * have to come out identical to the ones FE produces, and two
 * implementations will not stay identical.
 */
#include <vm86/ops.h>

#include <vm86/host.h>

/* ------------------------------------------------------------------ */
/* Relative branches                                                   */
/* ------------------------------------------------------------------ */

/*
 * A relative displacement is measured from the END of the instruction,
 * and by the time it has been fetched IP already points at whatever comes
 * next. That is the base it is added to -- the order cannot be reversed,
 * and reading it before the fetch gives an answer that is wrong by the
 * length of the instruction. The failure is a quiet one: a call to a
 * plausible-looking wrong address rather than a crash.
 *
 * The addition is done signed and assigned back through a 16-bit field,
 * so a branch off either end of the segment wraps inside it the way the
 * hardware's instruction pointer does.
 */
static void branch_short(struct vm86_cpu *cpu)
{
    int8_t displacement = (int8_t)vm86_fetch8(cpu);

    cpu->ip = (uint16_t)(cpu->ip + displacement);
}

static void branch_near(struct vm86_cpu *cpu)
{
    int16_t displacement = (int16_t)vm86_fetch16(cpu);

    cpu->ip = (uint16_t)(cpu->ip + displacement);
}

/* ------------------------------------------------------------------ */
/* 70-7F: the conditional jumps                                        */
/* ------------------------------------------------------------------ */

typedef bool (*vm86_condition_fn)(const struct vm86_cpu *);

static bool cond_overflow(const struct vm86_cpu *cpu)
{
    return vm86_flag_test(cpu, VM86_OF);
}

static bool cond_no_overflow(const struct vm86_cpu *cpu)
{
    return !vm86_flag_test(cpu, VM86_OF);
}

static bool cond_carry(const struct vm86_cpu *cpu)
{
    return vm86_flag_test(cpu, VM86_CF);
}

static bool cond_no_carry(const struct vm86_cpu *cpu)
{
    return !vm86_flag_test(cpu, VM86_CF);
}

static bool cond_zero(const struct vm86_cpu *cpu)
{
    return vm86_flag_test(cpu, VM86_ZF);
}

static bool cond_not_zero(const struct vm86_cpu *cpu)
{
    return !vm86_flag_test(cpu, VM86_ZF);
}

static bool cond_below_or_equal(const struct vm86_cpu *cpu)
{
    return vm86_flag_test(cpu, VM86_CF) || vm86_flag_test(cpu, VM86_ZF);
}

static bool cond_above(const struct vm86_cpu *cpu)
{
    return !vm86_flag_test(cpu, VM86_CF) && !vm86_flag_test(cpu, VM86_ZF);
}

static bool cond_sign(const struct vm86_cpu *cpu)
{
    return vm86_flag_test(cpu, VM86_SF);
}

static bool cond_no_sign(const struct vm86_cpu *cpu)
{
    return !vm86_flag_test(cpu, VM86_SF);
}

static bool cond_parity(const struct vm86_cpu *cpu)
{
    return vm86_flag_test(cpu, VM86_PF);
}

static bool cond_no_parity(const struct vm86_cpu *cpu)
{
    return !vm86_flag_test(cpu, VM86_PF);
}

static bool cond_less(const struct vm86_cpu *cpu)
{
    return vm86_flag_test(cpu, VM86_SF) != vm86_flag_test(cpu, VM86_OF);
}

static bool cond_greater_or_equal(const struct vm86_cpu *cpu)
{
    return vm86_flag_test(cpu, VM86_SF) == vm86_flag_test(cpu, VM86_OF);
}

static bool cond_less_or_equal(const struct vm86_cpu *cpu)
{
    return vm86_flag_test(cpu, VM86_ZF) ||
           (vm86_flag_test(cpu, VM86_SF) != vm86_flag_test(cpu, VM86_OF));
}

static bool cond_greater(const struct vm86_cpu *cpu)
{
    return !vm86_flag_test(cpu, VM86_ZF) &&
           (vm86_flag_test(cpu, VM86_SF) == vm86_flag_test(cpu, VM86_OF));
}

/*
 * The condition is the low four bits of the opcode, so the whole
 * instruction is this table.
 *
 * Sixteen functions rather than sixteen cases in a switch would give
 * "carry and zero swapped" sixteen separate places to happen; this way it
 * can happen once, and the entry that is wrong is the entry that is
 * named. The four aliases below are not duplication -- 2 and 6/7 and the
 * signed/unsigned pairs are the same flag bit read under two conventions
 * that the hardware genuinely has, and both readings are in the manual
 * because both are in the assembly language.
 */
static const vm86_condition_fn vm86_conditions[16] = {
    cond_overflow,          /* 0  O   overflow                     */
    cond_no_overflow,       /* 1  NO  not overflow                 */
    cond_carry,             /* 2  B   below, C, NAE: borrow        */
    cond_no_carry,          /* 3  AE  above or equal, NB, NC       */
    cond_zero,              /* 4  E   equal, Z: zero               */
    cond_not_zero,          /* 5  NE  not equal, NZ                */
    cond_below_or_equal,    /* 6  BE  below or equal, NA           */
    cond_above,             /* 7  A   above, NBE                   */
    cond_sign,              /* 8  S   sign                         */
    cond_no_sign,           /* 9  NS  not sign                     */
    cond_parity,            /* A  P   parity, PE: even low byte    */
    cond_no_parity,         /* B  NP  not parity, PO               */
    cond_less,              /* C  L   less, NGE: signed            */
    cond_greater_or_equal,  /* D  GE  greater or equal, NL         */
    cond_less_or_equal,     /* E  LE  less or equal, NG            */
    cond_greater,           /* F  G   greater, NLE                 */
};

static enum vm86_result op_jcc(struct vm86_cpu *cpu, uint8_t opcode)
{
    /*
     * The displacement belongs to the instruction whether or not the
     * branch is taken, so it is consumed before the condition is
     * consulted. Skipping the fetch on the not-taken path leaves IP
     * pointing into the middle of the instruction.
     */
    int8_t displacement = (int8_t)vm86_fetch8(cpu);

    if (vm86_conditions[opcode & 0x0F](cpu))
        cpu->ip = (uint16_t)(cpu->ip + displacement);

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* CALL and JMP                                                       */
/* ------------------------------------------------------------------ */

static enum vm86_result op_call_near(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    /*
     * The return address is the IP the fetch left behind: the instruction
     * after this one, which is what RET has to come back to. The
     * displacement is read first because it belongs to the instruction
     * stream and the push does not -- they only disagree if SP points into
     * the code being executed, which is legal and does happen.
     */
    int16_t displacement = (int16_t)vm86_fetch16(cpu);

    vm86_push16(cpu, cpu->ip);

    cpu->ip = (uint16_t)(cpu->ip + displacement);

    return VM86_CONTINUE;
}

static enum vm86_result op_jmp_near(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    branch_near(cpu);

    return VM86_CONTINUE;
}

static enum vm86_result op_jmp_short(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    branch_short(cpu);

    return VM86_CONTINUE;
}

static enum vm86_result op_far_call(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    /*
     * The operand is written offset first, segment second, which is the
     * reverse of how the pair is spelled in assembly. Both words are read
     * before anything is pushed, so that the instruction's own bytes are
     * consumed before the stack moves.
     */
    uint16_t offset  = vm86_fetch16(cpu);
    uint16_t segment = vm86_fetch16(cpu);

    /*
     * CS is pushed first and IP second, which puts IP on top of the stack
     * -- and RETF pops IP first. The two orders are not the same and are
     * not supposed to be: one is how the machine saves, the other is how
     * it restores, and the stack being last-in-first-out is what makes
     * them look reversed.
     */
    vm86_push16(cpu, cpu->cs);
    vm86_push16(cpu, cpu->ip);

    vm86_set_seg(cpu, VM86_CS, segment);
    cpu->ip = offset;

    return VM86_CONTINUE;
}

static enum vm86_result op_far_jmp(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    uint16_t offset  = vm86_fetch16(cpu);
    uint16_t segment = vm86_fetch16(cpu);

    vm86_set_seg(cpu, VM86_CS, segment);
    cpu->ip = offset;

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* RET                                                                */
/* ------------------------------------------------------------------ */

static enum vm86_result op_ret(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    cpu->ip = vm86_pop16(cpu);

    return VM86_CONTINUE;
}

static enum vm86_result op_ret_imm(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    /*
     * `RET n` pops the return address and then discards another n bytes
     * of arguments the callee never popped. This is the stdcall calling
     * convention, and a missing adjustment does not fail where it is
     * written: the caller's stack creeps upward by n on every call, and
     * the program goes wrong at some later call that happens to notice.
     *
     * The adjustment is fetched before the pop for the same reason as in
     * CALL: instruction bytes first, then the stack.
     */
    uint16_t adjustment = vm86_fetch16(cpu);

    cpu->ip = vm86_pop16(cpu);
    cpu->sp = (uint16_t)(cpu->sp + adjustment);

    return VM86_CONTINUE;
}

static enum vm86_result op_ret_far(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    /* IP first, then CS -- the reverse of the order CALL pushed them. */
    uint16_t offset  = vm86_pop16(cpu);
    uint16_t segment = vm86_pop16(cpu);

    vm86_set_seg(cpu, VM86_CS, segment);
    cpu->ip = offset;

    return VM86_CONTINUE;
}

static enum vm86_result op_ret_far_imm(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    uint16_t adjustment = vm86_fetch16(cpu);

    uint16_t offset  = vm86_pop16(cpu);
    uint16_t segment = vm86_pop16(cpu);

    vm86_set_seg(cpu, VM86_CS, segment);
    cpu->ip = offset;

    cpu->sp = (uint16_t)(cpu->sp + adjustment);

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Interrupts                                                         */
/* ------------------------------------------------------------------ */

/*
 * The overflow trap. ops.h names the vectors the interpreter raises on
 * its own; this one is raised by an instruction, and INTO is the only one
 * that raises it, so it is named where it is used rather than in the
 * frozen header.
 */
#define VECTOR_OVERFLOW 4

/*
 * Transfer control to an interrupt handler.
 *
 * This used to be a static function here, and it is not one any more. The
 * host layer has it as vm86_interrupt(), in dos/intr/, because three
 * things have to agree on it: this instruction, the run loop delivering
 * a hardware interrupt, and the run loop delivering an exception. They
 * differ in when they happen and not in what they do, and the moment
 * they are written twice they stop matching -- so there is one copy, and
 * this file calls it.
 *
 * The frame layout, why the vector table is addressed linearly, and the
 * clearing of IF and TF on entry are documented there, next to the
 * implementation they describe.
 */

static enum vm86_result op_int3(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    /*
     * The breakpoint vector, and the entire reason CC exists: it is
     * CD 03 with the vector folded into the opcode, which makes it one
     * byte instead of two. There is no immediate byte to fetch -- reading
     * one would eat the first byte of whatever follows.
     */
    vm86_interrupt(cpu, 3);

    return VM86_CONTINUE;
}

static enum vm86_result op_int_imm(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    uint8_t vector = vm86_fetch8(cpu);

    vm86_interrupt(cpu, vector);

    return VM86_CONTINUE;
}

static enum vm86_result op_into(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    /*
     * INTO is conditional, and with OF clear it is a complete no-op --
     * not an interrupt to vector 4 that gets suppressed, but an
     * instruction that did nothing at all. It has the same encoding
     * effect as INT 4 when it does fire and a different meaning: INT 4
     * raises the overflow trap unconditionally, which is a way of calling
     * a handler, while INTO raises it only when the arithmetic says so,
     * which is a way of checking the arithmetic.
     */
    if (!vm86_flag_test(cpu, VM86_OF))
        return VM86_CONTINUE;

    vm86_interrupt(cpu, VECTOR_OVERFLOW);

    return VM86_CONTINUE;
}

static enum vm86_result op_iret(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    uint16_t offset  = vm86_pop16(cpu);
    uint16_t segment = vm86_pop16(cpu);

    cpu->flags = vm86_pop16(cpu);

    vm86_set_seg(cpu, VM86_CS, segment);
    cpu->ip = offset;

    /* FLAGS was written wholesale, so the hardwired bits have to be
     * forced back before anything reads them again. See cpu.h. */
    vm86_flags_normalise(cpu);

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* E0-E3: the loop instructions                                        */
/* ------------------------------------------------------------------ */

static enum vm86_result op_loop(struct vm86_cpu *cpu, uint8_t opcode)
{
    int8_t displacement = (int8_t)vm86_fetch8(cpu);

    /*
     * JCXZ tests the counter and leaves it alone; the other three
     * decrement it first and then test it. That is the difference between
     * a loop body that runs CX times and one that runs CX+1 or CX-1
     * times, and it is silent until something depends on the count.
     */
    if (opcode == 0xE3) {
        if (cpu->cx == 0)
            cpu->ip = (uint16_t)(cpu->ip + displacement);

        return VM86_CONTINUE;
    }

    cpu->cx = (uint16_t)(cpu->cx - 1);

    /* Running the counter out ends the loop even when the condition still
     * holds, which is what bounds `loope` and `loopne` -- without it, a
     * loop waiting for a flag that never changes never ends. */
    if (cpu->cx == 0)
        return VM86_CONTINUE;

    bool take = true;

    if (opcode == 0xE0)
        take = !vm86_flag_test(cpu, VM86_ZF);
    else if (opcode == 0xE1)
        take = vm86_flag_test(cpu, VM86_ZF);

    if (take)
        cpu->ip = (uint16_t)(cpu->ip + displacement);

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Flag instructions                                                  */
/* ------------------------------------------------------------------ */

/*
 * CLC, STC, CLI, STI, CLD and STD are one instruction with six settings:
 * which flag and which value are the whole of the opcode's meaning, and
 * none of them touches anything else.
 */
static enum vm86_result op_set_flag(struct vm86_cpu *cpu, uint8_t opcode)
{
    switch (opcode) {
    case 0xF8: vm86_flag_set(cpu, VM86_CF, false); break;
    case 0xF9: vm86_flag_set(cpu, VM86_CF, true);  break;
    case 0xFA: vm86_flag_set(cpu, VM86_IF, false); break;

    case 0xFB:
        /*
         * STI takes effect one instruction late on real hardware: an
         * interrupt that arrives between STI and the next instruction is
         * not delivered until that instruction has run. The classic use
         * is `sti / hlt`, and the delay is the whole reason it works --
         * an interrupt taken between them would be consumed, and the HLT
         * would then wait for an interrupt that has already been handled,
         * which is a machine that hangs at idle rather than one that
         * computes a wrong answer.
         *
         * IF is set here, immediately, and what waits is the recognition
         * of an interrupt. That distinction is the whole of this
         * instruction and it was written down the other way once already
         * in this project, at the cost of two task books: deferring the
         * flag write is wrong, because a PUSHF straight after an STI
         * shows IF set on the hardware, and test_verify.c pins it.
         *
         * The shadow is a count of instruction boundaries rather than a
         * flag because that is what it is -- one instruction of grace.
         * The run loop decrements it and skips delivery while it is
         * nonzero, and nothing else decrements it. A second writer would
         * turn the one instruction into two or into none, and the only
         * place that shows up is a dense stream of interrupts.
         */
        vm86_flag_set(cpu, VM86_IF, true);
        cpu->intr_shadow = 1;
        break;

    case 0xFC: vm86_flag_set(cpu, VM86_DF, false); break;
    case 0xFD: vm86_flag_set(cpu, VM86_DF, true);  break;

    default: break;
    }

    return VM86_CONTINUE;
}

static enum vm86_result op_cmc(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    /* One flag, inverted. A program that uses CMC to test whether some
     * earlier operation carried is relying on it touching nothing else. */
    vm86_flag_set(cpu, VM86_CF, !vm86_flag_test(cpu, VM86_CF));

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* 9C-9F: the flags register, pushed, popped and split                 */
/* ------------------------------------------------------------------ */

static enum vm86_result op_pushf(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    /* What is pushed is the register as it reads, hardwired bits
     * included -- which is exactly what a program checking its own
     * processor with `pushf / pop ax` is looking at. */
    vm86_push16(cpu, cpu->flags);

    return VM86_CONTINUE;
}

static enum vm86_result op_popf(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    cpu->flags = vm86_pop16(cpu);

    /* A wholesale write, so normalise. Without it a program could clear
     * bits that cannot be cleared on an 8086, and a program that checks
     * for them would conclude it is running on something else. */
    vm86_flags_normalise(cpu);

    return VM86_CONTINUE;
}

static enum vm86_result op_sahf(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    /* The low byte only: the top half of the register, TF, IF and DF
     * among it, is not reachable from SAHF. */
    cpu->flags = (uint16_t)((cpu->flags & 0xFF00u) | cpu->ah);

    vm86_flags_normalise(cpu);

    return VM86_CONTINUE;
}

static enum vm86_result op_lahf(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    /* The mirror of SAHF: CF, PF, AF, ZF and SF land in AH, in their
     * architectural positions inside that byte rather than packed. */
    cpu->ah = (uint8_t)(cpu->flags & 0x00FFu);

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* 9B: WAIT                                                           */
/* ------------------------------------------------------------------ */

static enum vm86_result op_wait(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)cpu;
    (void)opcode;

    /*
     * WAIT is "wait for the coprocessor", and the TEST pin is strapped
     * inactive on a machine with no 8087 -- which is this machine, and
     * was most machines of the era. So it completes immediately: an
     * instruction that does nothing, and not by omission. Leaving it out
     * of the table would raise the invalid-opcode exception at every
     * FWAIT an assembler emitted before a floating point escape.
     */
    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* E4-E7, EC-EF: IN and OUT                                           */
/* ------------------------------------------------------------------ */

/*
 * The port number is an immediate byte for E4-E7 and DX for EC-EF; bit 3
 * of the opcode is what tells them apart, and bit 0 is the width. Reading
 * the immediate on a DX form, or failing to read it on an immediate form,
 * both shift the instruction stream by a byte.
 */
static uint16_t io_port(struct vm86_cpu *cpu, uint8_t opcode)
{
    if (opcode & 0x08)
        return cpu->dx;

    return vm86_fetch8(cpu);
}

static enum vm86_result op_in(struct vm86_cpu *cpu, uint8_t opcode)
{
    uint16_t port = io_port(cpu, opcode);

    /* No device exists at any port yet. See the note at the top of the
     * file for why the answer is 0xFF rather than an exception. */
    (void)port;

    if (opcode & 1)
        cpu->ax = 0xFFFF;
    else
        cpu->al = 0xFF;

    return VM86_CONTINUE;
}

static enum vm86_result op_out(struct vm86_cpu *cpu, uint8_t opcode)
{
    uint16_t port = io_port(cpu, opcode);

    /* The write happens and nothing hears it, which is what a machine
     * with an empty bus does. Dropping it is not an error condition. */
    (void)port;

    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* FF: group 5                                                        */
/* ------------------------------------------------------------------ */

static enum vm86_result op_group5(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)opcode;

    /*
     * Sixteen-bit operands throughout: the group has no byte form.
     *
     * The r/m operand is resolved before the sub-operation is looked at,
     * so that the displacement it may carry is consumed either way.
     */
    struct vm86_modrm mr;
    vm86_fetch_modrm(cpu, &mr, 16);

    switch (mr.reg) {
    case 0:     /* INC r/m16 */
    case 1: {   /* DEC r/m16 */
        /*
         * Arithmetic, and it has to be the same arithmetic FE and 40-4F
         * use -- the flags a program sees from `inc word [bx]` and from
         * `inc ax` are indistinguishable on the hardware, and two
         * implementations of the same rule drift apart at the edges
         * (the sign boundary, and the carry that must not move).
         */
        uint16_t value = vm86_operand_read(cpu, &mr.operand);

        vm86_operand_write(cpu, &mr.operand,
                           vm86_alu_incdec(cpu, 16, value, mr.reg == 0));
        break;
    }

    case 2: {   /* CALL r/m16, near */
        uint16_t target = vm86_operand_read(cpu, &mr.operand);

        vm86_push16(cpu, cpu->ip);
        cpu->ip = target;
        break;
    }

    case 3:     /* CALL m16:16, far */
    case 5: {   /* JMP m16:16, far */
        /*
         * A far pointer is four bytes and only memory has them. The
         * register forms of these two sub-operations are not encodings
         * this processor defines, so they are refused rather than
         * guessed at as near calls.
         */
        if (mr.operand.kind != VM86_OPERAND_MEMORY) {
            cpu->fault = VM86_VECTOR_INVALID_OPCODE;
            return VM86_FAULT;
        }

        uint16_t offset  = vm86_mem_read16(cpu->mem, mr.operand.addr);
        uint16_t segment = vm86_mem_read16(cpu->mem, mr.operand.addr + 2u);

        if (mr.reg == 3) {
            /* Save the frame before the segment changes: CS is part of
             * what has to be restored, so it has to be read while it is
             * still the caller's. */
            vm86_push16(cpu, cpu->cs);
            vm86_push16(cpu, cpu->ip);
        }

        vm86_set_seg(cpu, VM86_CS, segment);
        cpu->ip = offset;
        break;
    }

    case 4:     /* JMP r/m16, near */
        cpu->ip = vm86_operand_read(cpu, &mr.operand);
        break;

    case 6:     /* PUSH r/m16 */
        /*
         * FF F4 is PUSH SP, and it has to answer exactly as 54 does.
         *
         * The register form of SP is the one operand in this group whose
         * value cannot simply be read out of the register file: the 8086
         * pushes the SP it holds *after* the decrement, so handing the
         * helper the current SP is the 80186 answer. That is the same
         * mistake 54 was corrected for, and it survived here in a second
         * file because 54 and FF F4 are separate encodings of one
         * instruction and nothing made them agree.
         *
         * Memory forms are untouched. FF 36 pushes what is at the
         * address, and the address is formed from SP before the push
         * either way.
         */
        if (mr.operand.kind == VM86_OPERAND_REGISTER &&
            mr.operand.reg == VM86_REG_SP) {
            vm86_push16(cpu, (uint16_t)(cpu->sp - 2));
        } else {
            vm86_push16(cpu, vm86_operand_read(cpu, &mr.operand));
        }
        break;

    case 7:     /* not an instruction on this processor */
    default:
        cpu->fault = VM86_VECTOR_INVALID_OPCODE;
        return VM86_FAULT;
    }

    return VM86_CONTINUE;
}

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
    [0x70] = op_jcc, [0x71] = op_jcc, [0x72] = op_jcc, [0x73] = op_jcc,
    [0x74] = op_jcc, [0x75] = op_jcc, [0x76] = op_jcc, [0x77] = op_jcc,
    [0x78] = op_jcc, [0x79] = op_jcc, [0x7A] = op_jcc, [0x7B] = op_jcc,
    [0x7C] = op_jcc, [0x7D] = op_jcc, [0x7E] = op_jcc, [0x7F] = op_jcc,

    [0x9A] = op_far_call,
    [0x9B] = op_wait,
    [0x9C] = op_pushf,
    [0x9D] = op_popf,
    [0x9E] = op_sahf,
    [0x9F] = op_lahf,

    [0xC2] = op_ret_imm,
    [0xC3] = op_ret,
    [0xCA] = op_ret_far_imm,
    [0xCB] = op_ret_far,
    [0xCC] = op_int3,
    [0xCD] = op_int_imm,
    [0xCE] = op_into,
    [0xCF] = op_iret,

    [0xE0] = op_loop,
    [0xE1] = op_loop,
    [0xE2] = op_loop,
    [0xE3] = op_loop,

    [0xE4] = op_in,
    [0xE5] = op_in,
    [0xE6] = op_out,
    [0xE7] = op_out,

    [0xE8] = op_call_near,
    [0xE9] = op_jmp_near,
    [0xEA] = op_far_jmp,
    [0xEB] = op_jmp_short,

    [0xEC] = op_in,
    [0xED] = op_in,
    [0xEE] = op_out,
    [0xEF] = op_out,

    [0xF4] = op_hlt,
    [0xF5] = op_cmc,

    [0xF8] = op_set_flag,
    [0xF9] = op_set_flag,
    [0xFA] = op_set_flag,
    [0xFB] = op_set_flag,
    [0xFC] = op_set_flag,
    [0xFD] = op_set_flag,

    [0xFF] = op_group5,
};
