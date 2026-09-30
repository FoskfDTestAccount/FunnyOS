/*
 * The 80186 extensions: PUSHA, POPA, BOUND, the immediate pushes, the
 * multi-operand IMUL, INS, OUTS, ENTER and LEAVE.
 *
 * ---------------------------------------------------------------------
 * What this suite is really testing
 *
 * Most of these instructions are ordinary, and most of what could go
 * wrong with them goes wrong in one of four ways that produce plausible
 * results:
 *
 *   - PUSHA's SP. The value it pushes is the one from before the first
 *     push, and a program that never uses the saved copy cannot tell the
 *     difference. So the test reads the whole frame back out of memory
 *     and checks every slot, including the SP slot: that catches the
 *     order being wrong as well as the value.
 *
 *   - POPA's SP. The failing implementation is the symmetric one, which
 *     writes SP back. The test puts a recognisable garbage value in that
 *     slot, so an implementation that honours it lands on 0x1234 and is
 *     caught rather than merely being inconsistent.
 *
 *   - IMUL's flags. The multi-operand IMUL truncates to sixteen bits and
 *     reports the loss in CF and OF, which is a different question from
 *     the one the one-operand form in ops_alu.c answers. The cases here
 *     include negative products, which is where an unsigned multiply
 *     gives the same result with the wrong flags.
 *
 *   - BOUND's comparison. It is signed, both ends are inside the range,
 *     and the bounds are in memory low word first. The signed test uses
 *     bounds that straddle zero, because that is the only shape where a
 *     signed and an unsigned comparison disagree.
 *
 * ENTER gets the most attention because it is the one instruction here
 * with behaviour a reader is unlikely to have memorised. Level 0 is
 * tested as a round trip through LEAVE, and levels 1 and 2 are tested
 * against the frame the manual's pseudocode builds, chain words and all.
 *
 * ---------------------------------------------------------------------
 * What is not asserted, and why
 *
 * OUTS reads memory and sends it to a port which, on this machine, is
 * nothing at all: there is no device until M4. So the value it read is
 * unobservable, and so is the segment it read it from -- an `es: outsb`
 * is exercised here to show that the prefix does not disturb the loop,
 * but the difference between reading DS:SI and ES:SI cannot be seen
 * through a write that goes nowhere. The same is true of the value INS
 * stores: it is the floating bus's, and what is asserted is only that it
 * is the 0xFF that ops_ctl.c's IN also returns.
 */
#include "harness.h"

/* ------------------------------------------------------------------ */
/* 60, 61: PUSHA and POPA                                             */
/* ------------------------------------------------------------------ */

static void test_pusha_popa_round_trip(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x60, 0x61, 0xF4 };   /* pusha; popa */

    vm86_test_load(cpu, code, sizeof(code));

    cpu->ax = 0x1111;
    cpu->bx = 0x2222;
    cpu->cx = 0x3333;
    cpu->dx = 0x4444;
    cpu->si = 0x5555;
    cpu->di = 0x6666;
    cpu->bp = 0x7777;
    cpu->sp = 0x9000;

    /* A flag outside the arithmetic set, so "the flags are untouched" is
     * an assertion rather than a comparison of two zeroes. */
    vm86_flag_set(cpu, VM86_DF, true);

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0x1111);
    vm86_expect_u16("BX", cpu->bx, 0x2222);
    vm86_expect_u16("CX", cpu->cx, 0x3333);
    vm86_expect_u16("DX", cpu->dx, 0x4444);
    vm86_expect_u16("SI", cpu->si, 0x5555);
    vm86_expect_u16("DI", cpu->di, 0x6666);
    vm86_expect_u16("BP", cpu->bp, 0x7777);

    /* Eight pushes and eight pops, so the stack pointer comes back to
     * where it started -- which it only does if POPA read eight words. */
    vm86_expect_u16("SP back where it started", cpu->sp, 0x9000);

    vm86_expect_flags("flags untouched", cpu, VM86_DF, 0);
}

/*
 * The pushed frame, read straight out of memory.
 *
 * The slot this exists for is the SP one: it must hold 0x9000, the value
 * before the first push, and not 0x8FF8, which is what SP held after the
 * four pushes that came before it. The other seven slots pin the order,
 * which is the register encoding order and not the order PUSHA is
 * written in an assembly listing.
 */
static void test_pusha_pushes_the_original_sp(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x60, 0xF4 };   /* pusha */

    vm86_test_load(cpu, code, sizeof(code));

    cpu->ax = 0x1111;
    cpu->bx = 0x2222;
    cpu->cx = 0x3333;
    cpu->dx = 0x4444;
    cpu->si = 0x5555;
    cpu->di = 0x6666;
    cpu->bp = 0x7777;
    cpu->sp = 0x9000;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("SP after eight pushes", cpu->sp, 0x8FF0);

    vm86_expect_mem16("DI is on top",         cpu, 0x8FF0, 0x6666);
    vm86_expect_mem16("then SI",              cpu, 0x8FF2, 0x5555);
    vm86_expect_mem16("then BP",              cpu, 0x8FF4, 0x7777);
    vm86_expect_mem16("then the old SP",      cpu, 0x8FF6, 0x9000);
    vm86_expect_mem16("then BX",              cpu, 0x8FF8, 0x2222);
    vm86_expect_mem16("then DX",              cpu, 0x8FFA, 0x4444);
    vm86_expect_mem16("then CX",              cpu, 0x8FFC, 0x3333);
    vm86_expect_mem16("then AX",              cpu, 0x8FFE, 0x1111);
}

/*
 * POPA reads the SP slot and throws it away.
 *
 * The frame is written by hand with a stack pointer that has nothing to
 * do with the frame -- 0x1234 -- so an implementation that writes the
 * popped value back into SP ends up somewhere absurd instead of merely
 * being a few bytes off.
 */
static void test_popa_ignores_the_sp_it_pops(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x61, 0xF4 };   /* popa */

    vm86_test_load(cpu, code, sizeof(code));

    vm86_mem_write16(cpu->mem, 0x8FF0, 0x6666);   /* DI */
    vm86_mem_write16(cpu->mem, 0x8FF2, 0x5555);   /* SI */
    vm86_mem_write16(cpu->mem, 0x8FF4, 0x7777);   /* BP */
    vm86_mem_write16(cpu->mem, 0x8FF6, 0x1234);   /* SP: garbage */
    vm86_mem_write16(cpu->mem, 0x8FF8, 0x2222);   /* BX */
    vm86_mem_write16(cpu->mem, 0x8FFA, 0x4444);   /* DX */
    vm86_mem_write16(cpu->mem, 0x8FFC, 0x3333);   /* CX */
    vm86_mem_write16(cpu->mem, 0x8FFE, 0x1111);   /* AX */

    cpu->sp = 0x8FF0;
    cpu->ax = 0xAAAA;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0x1111);
    vm86_expect_u16("BX", cpu->bx, 0x2222);
    vm86_expect_u16("CX", cpu->cx, 0x3333);
    vm86_expect_u16("DX", cpu->dx, 0x4444);
    vm86_expect_u16("SI", cpu->si, 0x5555);
    vm86_expect_u16("DI", cpu->di, 0x6666);
    vm86_expect_u16("BP", cpu->bp, 0x7777);

    /* Sixteen bytes of frame popped, and SP itself untouched by the slot
     * that held 0x1234. */
    vm86_expect_u16("SP past the frame, not the garbage", cpu->sp, 0x9000);
}

/* ------------------------------------------------------------------ */
/* 62: BOUND                                                          */
/* ------------------------------------------------------------------ */

/*
 * Run `bound ax, [bx]` against a pair of bounds.
 *
 * BX points at the two words, low bound first, and AX is the value under
 * test. The code is reloaded each time because the tests below run it
 * several times with different numbers.
 */
static enum vm86_result run_bound(struct vm86_cpu *cpu, uint16_t lower,
                                  uint16_t upper, uint16_t value)
{
    static const uint8_t code[] = { 0x62, 0x07, 0xF4 };   /* bound ax,[bx] */

    vm86_test_load(cpu, code, sizeof(code));

    vm86_mem_write16(cpu->mem, 0x0300, lower);
    vm86_mem_write16(cpu->mem, 0x0302, upper);

    cpu->bx = 0x0300;
    cpu->ax = value;

    return vm86_test_run(cpu, 10);
}

static void test_bound_accepts_values_inside(struct vm86_cpu *cpu)
{
    /* Both ends are included: `value < lower || value > upper` is the
     * test, and writing it with <= on either side makes a bound that is
     * legal unreachable. */
    vm86_expect_bool("exactly the lower bound",
                     run_bound(cpu, 0x0010, 0x0020, 0x0010) == VM86_HALT, true);
    vm86_expect_bool("exactly the upper bound",
                     run_bound(cpu, 0x0010, 0x0020, 0x0020) == VM86_HALT, true);
    vm86_expect_bool("between them",
                     run_bound(cpu, 0x0010, 0x0020, 0x0018) == VM86_HALT, true);

    vm86_expect_u16("no fault recorded", cpu->fault, VM86_NO_FAULT);
}

static void test_bound_below_the_lower_bound_faults(struct vm86_cpu *cpu)
{
    enum vm86_result result = run_bound(cpu, 0x0010, 0x0020, 0x000F);

    vm86_expect_bool("faulted", result == VM86_FAULT, true);
    vm86_expect_u16("vector 5", cpu->fault, VM86_VECTOR_BOUND);
}

static void test_bound_above_the_upper_bound_faults(struct vm86_cpu *cpu)
{
    enum vm86_result result = run_bound(cpu, 0x0010, 0x0020, 0x0021);

    vm86_expect_bool("faulted", result == VM86_FAULT, true);
    vm86_expect_u16("vector 5", cpu->fault, VM86_VECTOR_BOUND);
}

/*
 * The comparison is signed, and this is the case that says so: bounds of
 * -10 and 10 with a value of -4. Unsigned, 0xFFFC is far above 0x000A and
 * the instruction would fault; signed, it is inside.
 */
static void test_bound_compares_signed(struct vm86_cpu *cpu)
{
    vm86_expect_bool("minus four is inside minus ten to ten",
                     run_bound(cpu, 0xFFF6, 0x000A, 0xFFFC) == VM86_HALT, true);

    vm86_expect_bool("minus eleven is outside it",
                     run_bound(cpu, 0xFFF6, 0x000A, 0xFFF5) == VM86_FAULT,
                     true);
    vm86_expect_u16("vector 5", cpu->fault, VM86_VECTOR_BOUND);
}

static void test_bound_register_operand_is_invalid(struct vm86_cpu *cpu)
{
    /* `bound ax, ax` has nowhere to put a pair of words, and the encoding
     * is undefined. See the note in ops_186.c. */
    static const uint8_t code[] = { 0x62, 0xC0, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0005;

    enum vm86_result result = vm86_test_run(cpu, 10);

    vm86_expect_bool("faulted", result == VM86_FAULT, true);
    vm86_expect_u16("vector 6", cpu->fault, VM86_VECTOR_INVALID_OPCODE);
}

/* ------------------------------------------------------------------ */
/* 68, 6A: PUSH an immediate                                          */
/* ------------------------------------------------------------------ */

static void test_push_imm16(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = {
        0x68, 0x34, 0x12,        /* push 0x1234 */
        0x68, 0xFE, 0xFF,        /* push 0xFFFE */
        0xF4,
    };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->sp = 0x9000;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("SP after two pushes", cpu->sp, 0x8FFC);
    vm86_expect_mem16("the second word", cpu, 0x8FFC, 0xFFFE);
    vm86_expect_mem16("the first word",  cpu, 0x8FFE, 0x1234);
}

static void test_push_imm8_sign_extends(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = {
        0x6A, 0xFB,               /* push -5 */
        0x6A, 0x05,               /* push 5 */
        0xF4,
    };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->sp = 0x9000;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("SP after two pushes", cpu->sp, 0x8FFC);
    /* The whole point: 0xFB is -5 and not 251. Pushing 0x00FB here is the
     * failure that a test looking only at a small positive immediate
     * would not see. The sign-extended one was pushed first, so it is the
     * one further up the stack. */
    vm86_expect_mem16("sign extended", cpu, 0x8FFE, 0xFFFB);
    vm86_expect_mem16("unchanged",     cpu, 0x8FFC, 0x0005);
}

/* ------------------------------------------------------------------ */
/* 69, 6B: IMUL with an immediate                                     */
/* ------------------------------------------------------------------ */

/*
 * `imul bx, cx, imm16`: the destination is the ModRM reg field and the
 * source is its r/m field, which is the other way round from how it
 * reads. 0xD9 is mod=11, reg=011 (BX), r/m=001 (CX).
 */
static enum vm86_result run_imul16(struct vm86_cpu *cpu, uint16_t source,
                                   uint16_t immediate)
{
    uint8_t code[] = {
        0x69, 0xD9,
        (uint8_t)(immediate & 0xFF), (uint8_t)(immediate >> 8),
        0xF4,
    };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->cx = source;

    return vm86_test_run(cpu, 10);
}

static void test_imul_imm16_that_fits(struct vm86_cpu *cpu)
{
    (void)run_imul16(cpu, 0x0005, 0x0003);

    vm86_expect_u16("BX", cpu->bx, 0x000F);
    vm86_expect_flag("CF", cpu, VM86_CF, false);
    vm86_expect_flag("OF", cpu, VM86_OF, false);

    /* The largest product that still fits, so the boundary is not off by
     * one: 0x0100 * 0x007F is 0x7F00. */
    (void)run_imul16(cpu, 0x0100, 0x007F);

    vm86_expect_u16("BX at the boundary", cpu->bx, 0x7F00);
    vm86_expect_flag("CF at the boundary", cpu, VM86_CF, false);
    vm86_expect_flag("OF at the boundary", cpu, VM86_OF, false);
}

static void test_imul_imm16_that_does_not_fit(struct vm86_cpu *cpu)
{
    (void)run_imul16(cpu, 0x1000, 0x0010);   /* 0x10000 */

    vm86_expect_u16("BX keeps the low half", cpu->bx, 0x0000);
    vm86_expect_flag("CF", cpu, VM86_CF, true);
    vm86_expect_flag("OF", cpu, VM86_OF, true);

    /* Signed overflow is the same report: -32768 * 2 is -65536, which
     * does not fit in sixteen bits whichever way it is read. */
    (void)run_imul16(cpu, 0x8000, 0x0002);

    vm86_expect_u16("BX keeps the low half", cpu->bx, 0x0000);
    vm86_expect_flag("CF", cpu, VM86_CF, true);
    vm86_expect_flag("OF", cpu, VM86_OF, true);
}

/*
 * A negative product that fits.
 *
 * -10 * 3 is -30, which is 0xFFE2 and fits comfortably. An unsigned
 * multiply of the same two words gets 0xFFF6 * 3 = 0x2FFE2, throws away
 * the top, and reports that the result did not fit -- so CF is what tells
 * the two apart, and the value on its own would not have.
 */
static void test_imul_is_a_signed_multiply(struct vm86_cpu *cpu)
{
    (void)run_imul16(cpu, 0xFFF6, 0x0003);

    vm86_expect_u16("BX", cpu->bx, 0xFFE2);
    vm86_expect_flag("CF", cpu, VM86_CF, false);
    vm86_expect_flag("OF", cpu, VM86_OF, false);

    /* And -1 * -1, which is 1 and not 0xFFFE. */
    (void)run_imul16(cpu, 0xFFFF, 0xFFFF);

    vm86_expect_u16("BX", cpu->bx, 0x0001);
    vm86_expect_flag("CF", cpu, VM86_CF, false);

    /*
     * The smallest case that separates the two readings of "does not
     * fit": -2 * 3 is -6, whose 32-bit product has 0xFFFF in the high
     * half. Non-zero, and still a sign extension of the low half, so the
     * result fits and CF/OF are clear. Reading the rule as "the high half
     * is non-zero" gets every negative product wrong from here down, and
     * this is where it starts.
     */
    (void)run_imul16(cpu, 0xFFFE, 0x0003);

    vm86_expect_u16("BX", cpu->bx, 0xFFFA);
    vm86_expect_flag("CF", cpu, VM86_CF, false);
    vm86_expect_flag("OF", cpu, VM86_OF, false);
}

/*
 * SF, ZF, AF and PF are undefined after a multi-operand IMUL. This
 * implementation leaves them alone, and the test pins that decision
 * rather than leaving it to drift: a program that branches on ZF after
 * multiplying is relying on an accident, and it will get the same
 * accident every time.
 */
static void test_imul_leaves_the_undefined_flags_alone(struct vm86_cpu *cpu)
{
    uint16_t before = (uint16_t)(VM86_ZF | VM86_SF | VM86_AF | VM86_PF);
    static const uint8_t code[] = { 0x69, 0xD9, 0x03, 0x00, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->cx = 0x0002;

    /* Set them before, so that "untouched" is a real assertion rather
     * than two zeroes agreeing with each other. CF and OF are left clear
     * on purpose: the product fits, so a correct implementation clears
     * them and the whole register can be compared. */
    cpu->flags = (uint16_t)(cpu->flags | before);

    vm86_test_run(cpu, 10);

    vm86_expect_flags("undefined flags untouched", cpu, before, 0);
    vm86_expect_u16("BX", cpu->bx, 0x0006);
}

/*
 * The byte-immediate form, where the sign extension is the whole
 * question: 0x80 is -128, so -128 * 256 is -32768 and fits. Read as 128,
 * the same multiply is +32768 and does not.
 */
static void test_imul_imm8_sign_extends(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x6B, 0xD9, 0x80, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->cx = 0x0100;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("BX", cpu->bx, 0x8000);
    vm86_expect_flag("CF", cpu, VM86_CF, false);
    vm86_expect_flag("OF", cpu, VM86_OF, false);
}

static void test_imul_imm8_that_does_not_fit(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x6B, 0xD9, 0x7F, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->cx = 0x0200;                        /* 512 * 127 = 65024 */

    vm86_test_run(cpu, 10);

    vm86_expect_u16("BX keeps the low half", cpu->bx, 0xFE00);
    vm86_expect_flag("CF", cpu, VM86_CF, true);
    vm86_expect_flag("OF", cpu, VM86_OF, true);
}

/*
 * A memory source with a displacement, which is the case that catches an
 * implementation reading its immediate before it has decoded ModRM: the
 * displacement byte would be taken for the low half of the immediate and
 * every operand after it would be wrong.
 *
 * 0x5C is mod=01, reg=011 (BX), r/m=100 ([SI]+disp8).
 */
static void test_imul_reads_the_displacement_before_the_immediate(
    struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x69, 0x5C, 0x04, 0x0A, 0x00, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    vm86_mem_write16(cpu->mem, 0x0300, 0x0003);   /* [si]     */
    vm86_mem_write16(cpu->mem, 0x0304, 0x0007);   /* [si + 4] */

    cpu->si = 0x0300;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("BX from [si+4] * 10", cpu->bx, 0x0046);
    vm86_expect_flag("CF", cpu, VM86_CF, false);
}

/* ------------------------------------------------------------------ */
/* 6C-6F: INS and OUTS                                                */
/* ------------------------------------------------------------------ */

static void test_insb_stores_and_steps_forward(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x6C, 0xF4 };   /* insb */

    vm86_test_load(cpu, code, sizeof(code));

    cpu->di = 0x0400;
    cpu->dx = 0x0060;      /* the port, which does not exist yet */
    cpu->cx = 0x1234;

    vm86_test_run(cpu, 10);

    /* What a floating bus returns, as in ops_ctl.c's IN. */
    vm86_expect_mem8("the byte read from the port", cpu, 0x0400, 0xFF);
    vm86_expect_mem8("nothing past it", cpu, 0x0401, 0x00);
    vm86_expect_u16("DI stepped by one", cpu->di, 0x0401);

    /* No REP, so no count: an implementation that always decrements CX
     * turns a plain INS into something that eats a loop counter. */
    vm86_expect_u16("CX untouched", cpu->cx, 0x1234);
}

static void test_insw_stores_a_word(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x6D, 0xF4 };   /* insw */

    vm86_test_load(cpu, code, sizeof(code));

    cpu->di = 0x0400;
    cpu->dx = 0x0060;

    vm86_test_run(cpu, 10);

    vm86_expect_mem16("the word read from the port", cpu, 0x0400, 0xFFFF);
    vm86_expect_mem16("nothing past it", cpu, 0x0402, 0x0000);
    vm86_expect_u16("DI stepped by two", cpu->di, 0x0402);
}

static void test_rep_insb_stores_cx_bytes(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0x6C, 0xF4 };   /* rep insb */

    vm86_test_load(cpu, code, sizeof(code));

    cpu->di = 0x0400;
    cpu->cx = 3;

    vm86_test_run(cpu, 10);

    vm86_expect_mem8("byte 0", cpu, 0x0400, 0xFF);
    vm86_expect_mem8("byte 1", cpu, 0x0401, 0xFF);
    vm86_expect_mem8("byte 2", cpu, 0x0402, 0xFF);
    vm86_expect_mem8("and no fourth", cpu, 0x0403, 0x00);
    vm86_expect_u16("DI", cpu->di, 0x0403);
    vm86_expect_u16("CX", cpu->cx, 0);
}

/*
 * F2 on INS is REP, not REPNE: there is no comparison here for "not
 * equal" to mean anything about. The shared loop settles that by being
 * told the instruction is unconditional, and the test pins the answer at
 * this end of the call as well, because passing `true` here is a
 * one-character change with no visible cause.
 */
static void test_repne_insb_acts_as_rep(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF2, 0x6C, 0xF4 };   /* repne insb */

    vm86_test_load(cpu, code, sizeof(code));

    cpu->di = 0x0400;
    cpu->cx = 3;

    vm86_test_run(cpu, 10);

    vm86_expect_mem8("byte 2", cpu, 0x0402, 0xFF);
    vm86_expect_u16("DI", cpu->di, 0x0403);
    vm86_expect_u16("CX", cpu->cx, 0);
}

static void test_rep_insb_runs_backward(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0x6C, 0xF4 };   /* rep insb */

    vm86_test_load(cpu, code, sizeof(code));

    /* As in the MOVS tests: the data is arranged so that the two
     * directions land in different places, because on a symmetric buffer
     * they are the same program. */
    vm86_flag_set(cpu, VM86_DF, true);

    cpu->di = 0x0403;
    cpu->cx = 3;

    vm86_test_run(cpu, 10);

    vm86_expect_mem8("byte at 0x0403", cpu, 0x0403, 0xFF);
    vm86_expect_mem8("byte at 0x0402", cpu, 0x0402, 0xFF);
    vm86_expect_mem8("byte at 0x0401", cpu, 0x0401, 0xFF);
    vm86_expect_mem8("nothing at 0x0400", cpu, 0x0400, 0x00);
    vm86_expect_u16("DI", cpu->di, 0x0400);
}

/*
 * INS writes through ES, and a prefix does not move that.
 *
 * Two segments that do not overlap make the write's address visible: with
 * DS = 0x2000 and ES = 0x1000 the same DI names 0x20010 and 0x10010
 * respectively, so a `ds: insb` that let the prefix move its destination
 * would store into the wrong one of them.
 */
static void test_ins_writes_through_es(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x3E, 0x6C, 0xF4 };   /* ds: insb */

    vm86_test_load(cpu, code, sizeof(code));

    vm86_set_seg(cpu, VM86_DS, 0x2000);
    vm86_set_seg(cpu, VM86_ES, 0x1000);

    cpu->di = 0x0010;

    vm86_test_run(cpu, 10);

    vm86_expect_mem8("stored at ES:DI", cpu, 0x10010, 0xFF);
    vm86_expect_mem8("nothing at DS:DI", cpu, 0x20010, 0x00);
}

static void test_outsb_steps_si_and_leaves_cx_alone(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x6E, 0xF4 };   /* outsb */

    vm86_test_load(cpu, code, sizeof(code));

    vm86_mem_write8(cpu->mem, 0x0300, 0x5A);

    cpu->si = 0x0300;
    cpu->dx = 0x0060;
    cpu->cx = 0x1234;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("SI stepped by one", cpu->si, 0x0301);
    vm86_expect_u16("CX untouched", cpu->cx, 0x1234);

    /* The source is read, not consumed: there is nothing here that writes
     * memory, and a body that confused the two directions of INS and OUTS
     * would leave a mark at the port's address. */
    vm86_expect_mem8("the source byte is still there", cpu, 0x0300, 0x5A);
}

static void test_rep_outsw_steps_back_by_two(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0x6F, 0xF4 };   /* rep outsw */

    vm86_test_load(cpu, code, sizeof(code));

    vm86_flag_set(cpu, VM86_DF, true);

    cpu->si = 0x0304;
    cpu->cx = 2;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("SI two words back", cpu->si, 0x0300);
    vm86_expect_u16("CX", cpu->cx, 0);
}

static void test_rep_outsb_with_cx_zero_moves_nothing(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0x6E, 0xF4 };   /* rep outsb */

    vm86_test_load(cpu, code, sizeof(code));

    cpu->si = 0x0300;
    cpu->cx = 0;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("SI", cpu->si, 0x0300);
    vm86_expect_u16("CX", cpu->cx, 0);
}

static void test_outs_writes_nothing_to_memory(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0x6E, 0xF4 };   /* rep outsb */

    vm86_test_load(cpu, code, sizeof(code));

    vm86_mem_write8(cpu->mem, 0x0300, 0x11);
    vm86_mem_write8(cpu->mem, 0x0301, 0x22);
    vm86_mem_write8(cpu->mem, 0x0400, 0xAA);

    cpu->si = 0x0300;
    cpu->di = 0x0400;
    cpu->cx = 2;

    vm86_test_run(cpu, 10);

    /* OUTS has no memory destination at all: a program that used it while
     * DI happened to point somewhere useful would be destroyed by an
     * implementation that stored what it read. */
    vm86_expect_mem8("DI's address is untouched", cpu, 0x0400, 0xAA);
    vm86_expect_u16("SI", cpu->si, 0x0302);
}

static void test_outs_accepts_a_segment_override(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x26, 0x6E, 0xF4 };   /* es: outsb */

    vm86_test_load(cpu, code, sizeof(code));

    vm86_set_seg(cpu, VM86_ES, 0x1000);

    cpu->si = 0x0300;
    cpu->cx = 1;

    vm86_test_run(cpu, 10);

    /*
     * Only that the instruction ran and stepped. Which segment the read
     * came from is not observable while the port write goes nowhere: the
     * loop resolves the override and hands it to the body, and there is
     * no device on the other end to tell ES:SI from DS:SI. Worth stating
     * rather than leaving as an untested claim -- see the report.
     */
    vm86_expect_u16("SI stepped by one", cpu->si, 0x0301);
}

/* ------------------------------------------------------------------ */
/* C8, C9: ENTER and LEAVE                                            */
/* ------------------------------------------------------------------ */

/*
 * The frame ENTER builds, for the levels a program actually emits.
 *
 * The stack starts at 0x8000 and BP points at a caller's frame whose
 * chain entry -- the word at SS:BP-2 -- is 0x2000, which is a value that
 * cannot be confused with anything else on the stack. Every case below
 * uses the same three numbers so that the differences between the levels
 * are the only thing being read.
 */
#define ENTER_SP      0x8000
#define ENTER_BP      0x3000
#define ENTER_CHAIN   0x2000
#define ENTER_CHAIN_AT 0x2FFE    /* SS:BP-2, where the chain word sits */

static void enter_setup(struct vm86_cpu *cpu, const uint8_t *code,
                        uint16_t size)
{
    vm86_test_load(cpu, code, size);

    vm86_mem_write16(cpu->mem, ENTER_CHAIN_AT, ENTER_CHAIN);

    cpu->sp = ENTER_SP;
    cpu->bp = ENTER_BP;
}

static void test_enter_level_zero(struct vm86_cpu *cpu)
{
    /* enter 4, 0 */
    static const uint8_t code[] = { 0xC8, 0x04, 0x00, 0x00, 0xF4 };

    enter_setup(cpu, code, sizeof(code));
    vm86_test_run(cpu, 10);

    /* push BP; BP = SP; SP -= 4 -- the four instructions a compiler
     * writes by hand, spelled in two bytes. */
    vm86_expect_mem16("the caller's BP", cpu, 0x7FFE, ENTER_BP);
    vm86_expect_u16("BP at the frame", cpu->bp, 0x7FFE);
    vm86_expect_u16("SP below four bytes of locals", cpu->sp, 0x7FFA);
}

static void test_enter_level_one_pushes_the_frame_pointer(struct vm86_cpu *cpu)
{
    /* enter 4, 1 */
    static const uint8_t code[] = { 0xC8, 0x04, 0x00, 0x01, 0xF4 };

    enter_setup(cpu, code, sizeof(code));
    vm86_test_run(cpu, 10);

    /* The loop runs from 1 to level-1, which for level 1 is no times at
     * all, but the frame pointer is still pushed. That extra word is the
     * difference between level 0 and level 1, and an implementation that
     * treats them alike is wrong here and only here. */
    vm86_expect_mem16("the caller's BP", cpu, 0x7FFE, ENTER_BP);
    vm86_expect_mem16("the frame pointer, twice", cpu, 0x7FFC, 0x7FFE);
    vm86_expect_u16("BP at the frame", cpu->bp, 0x7FFE);
    vm86_expect_u16("SP", cpu->sp, 0x7FF8);
}

static void test_enter_level_two_links_the_caller_frames(struct vm86_cpu *cpu)
{
    /* enter 6, 2 */
    static const uint8_t code[] = { 0xC8, 0x06, 0x00, 0x02, 0xF4 };

    enter_setup(cpu, code, sizeof(code));
    vm86_test_run(cpu, 10);

    /*
     * Level 2, so the loop runs once: BP is moved to the caller's chain
     * entry and the word there is copied onto the new frame. The chain
     * word 0x2000 is what says the dereference happened at all -- an
     * implementation that pushed BP a second time would put 0x3000 in
     * that slot instead.
     */
    vm86_expect_mem16("the caller's BP", cpu, 0x7FFE, ENTER_BP);
    vm86_expect_mem16("the frame one level out", cpu, 0x7FFC, ENTER_CHAIN);
    vm86_expect_mem16("the frame pointer again", cpu, 0x7FFA, 0x7FFE);
    vm86_expect_u16("BP at the frame", cpu->bp, 0x7FFE);
    vm86_expect_u16("SP", cpu->sp, 0x7FF4);
}

static void test_enter_and_leave_round_trip(struct vm86_cpu *cpu)
{
    /* enter 6, 0; leave */
    static const uint8_t code[] = { 0xC8, 0x06, 0x00, 0x00, 0xC9, 0xF4 };

    enter_setup(cpu, code, sizeof(code));
    vm86_test_run(cpu, 10);

    vm86_expect_u16("SP back where it started", cpu->sp, ENTER_SP);
    vm86_expect_u16("BP back where it started", cpu->bp, ENTER_BP);
}

/*
 * LEAVE after a level 2 ENTER, which does unwind the frame completely --
 * but for a reason worth spelling out, because the opposite is easy to
 * assume from the frame listing above.
 *
 * ENTER pushes the caller's BP first and everything else below it, and
 * then sets BP to the address of that word. So [BP] is always the
 * caller's BP, whatever the nesting level, and LEAVE -- SP <- BP, pop BP
 * -- lands exactly where ENTER started. The chain words and the copy of
 * the frame pointer end up below the restored SP, where they are free
 * space and are simply another caller's problem.
 *
 * That is why a level 2 frame needs no special epilogue, and why the
 * stack pointer comes back to where it was with the frame contents still
 * sitting in unallocated memory.
 */
static void test_leave_after_a_nested_enter(struct vm86_cpu *cpu)
{
    /* enter 6, 2; leave */
    static const uint8_t code[] = { 0xC8, 0x06, 0x00, 0x02, 0xC9, 0xF4 };

    enter_setup(cpu, code, sizeof(code));
    vm86_test_run(cpu, 10);

    vm86_expect_u16("BP back where it started", cpu->bp, ENTER_BP);
    vm86_expect_u16("SP back where it started", cpu->sp, ENTER_SP);

    /* The frame is still in memory, below SP. Dead, but readable, which
     * is how the two assertions above can both be true. */
    vm86_expect_mem16("the chain word is still there",
                      cpu, 0x7FFC, ENTER_CHAIN);
}

static void test_enter_takes_the_level_modulo_32(struct vm86_cpu *cpu)
{
    /* enter 0, 0x22: 0x22 & 0x1F is 2 */
    static const uint8_t code[] = { 0xC8, 0x00, 0x00, 0x22, 0xF4 };

    enter_setup(cpu, code, sizeof(code));
    vm86_test_run(cpu, 10);

    vm86_expect_mem16("linked like level 2", cpu, 0x7FFC, ENTER_CHAIN);
    vm86_expect_u16("BP at the frame", cpu->bp, 0x7FFE);
    vm86_expect_u16("no locals to subtract", cpu->sp, 0x7FFA);
}

/* ------------------------------------------------------------------ */

static const struct vm86_test tests[] = {
    { "pusha/popa round trip",             test_pusha_popa_round_trip },
    { "pusha pushes the original SP",      test_pusha_pushes_the_original_sp },
    { "popa ignores the SP it pops",       test_popa_ignores_the_sp_it_pops },

    { "bound accepts the bounds themselves",
      test_bound_accepts_values_inside },
    { "bound below the range faults",      test_bound_below_the_lower_bound_faults },
    { "bound above the range faults",      test_bound_above_the_upper_bound_faults },
    { "bound compares signed",             test_bound_compares_signed },
    { "bound on a register is invalid",    test_bound_register_operand_is_invalid },

    { "push imm16",                        test_push_imm16 },
    { "push imm8 sign extends",            test_push_imm8_sign_extends },

    { "imul imm16 that fits",              test_imul_imm16_that_fits },
    { "imul imm16 that does not fit",      test_imul_imm16_that_does_not_fit },
    { "imul is a signed multiply",         test_imul_is_a_signed_multiply },
    { "imul leaves the undefined flags",   test_imul_leaves_the_undefined_flags_alone },
    { "imul imm8 sign extends",            test_imul_imm8_sign_extends },
    { "imul imm8 that does not fit",       test_imul_imm8_that_does_not_fit },
    { "imul reads the displacement first",
      test_imul_reads_the_displacement_before_the_immediate },

    { "insb stores and steps",             test_insb_stores_and_steps_forward },
    { "insw stores a word",                test_insw_stores_a_word },
    { "rep insb stores CX bytes",          test_rep_insb_stores_cx_bytes },
    { "F2 on insb acts as F3",             test_repne_insb_acts_as_rep },
    { "rep insb runs backward",            test_rep_insb_runs_backward },
    { "ins writes through es",             test_ins_writes_through_es },
    { "outsb steps SI, keeps CX",          test_outsb_steps_si_and_leaves_cx_alone },
    { "rep outsw steps back by two",       test_rep_outsw_steps_back_by_two },
    { "rep outsb with CX=0",               test_rep_outsb_with_cx_zero_moves_nothing },
    { "outs writes nothing to memory",     test_outs_writes_nothing_to_memory },
    { "outs accepts a segment override",   test_outs_accepts_a_segment_override },

    { "enter level 0",                     test_enter_level_zero },
    { "enter level 1",                     test_enter_level_one_pushes_the_frame_pointer },
    { "enter level 2 links the frames",    test_enter_level_two_links_the_caller_frames },
    { "enter/leave round trip",            test_enter_and_leave_round_trip },
    { "leave after a nested enter",        test_leave_after_a_nested_enter },
    { "enter level modulo 32",             test_enter_takes_the_level_modulo_32 },
};

VM86_TEST_MAIN("186", tests)
