/*
 * The string instructions: MOVS, CMPS, STOS, LODS and SCAS, with and
 * without REP.
 *
 * ---------------------------------------------------------------------
 * What this suite is really testing
 *
 * Ten opcodes, and the reason they get a suite twice the size of a
 * larger group's is that the instruction is not the interesting part.
 * Each of them is a loop, and the loop has four ways to be wrong that
 * produce plausible results on well-chosen data:
 *
 *   - The count. CX=0 must execute zero times, not once and not 65536
 *     times. Every instruction is tested with CX=0 for exactly that
 *     reason; an implementation that checks the count after the first
 *     iteration passes every other test in this file.
 *
 *   - The direction flag. A copy that ignores DF works perfectly on
 *     palindromes, all-zero buffers and single bytes. So the data here
 *     is deliberately asymmetric -- ABCD, words that differ from each
 *     other, buffers with one byte out of place -- and both directions
 *     are exercised, because on symmetric data the two are the same
 *     program.
 *
 *   - The termination condition. `repe cmpsb` stops at the first
 *     difference; that is what it is for, and an implementation that
 *     treats REPE as plain REP runs to the end of the buffer and reports
 *     "identical" or "scanned everything" instead of "byte 3 differs".
 *     The CX value on return is part of the result and is asserted.
 *
 *   - The segments. SI is the DS-side pointer and DI is the ES-side one
 *     whichever way the operands read, a prefix moves the DS side only,
 *     and no prefix moves a write off ES. The tests below use two
 *     segments that do not overlap and check both ends.
 *
 * The flags are asserted in full rather than one at a time where the
 * instruction sets them, because CMPS and SCAS are CMPs and a CMP that
 * gets the borrow the wrong way round is the classic silent 8086 bug.
 */
#include "harness.h"

/*
 * A flag pattern for the instructions that must not touch flags at all.
 *
 * Every writable bit that these instructions could plausibly disturb is
 * set, so "unchanged" is a real assertion rather than a comparison of
 * two zeroes.
 */
#define FLAGS_UNDER_TEST (VM86_CF | VM86_PF | VM86_AF | VM86_SF | VM86_OF)

/* ------------------------------------------------------------------ */
/* MOVS                                                                */
/* ------------------------------------------------------------------ */

static void test_movsb_copies_and_steps_forward(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xA4, 0xF4 };   /* movsb; hlt */

    vm86_test_load(cpu, code, sizeof(code));
    vm86_mem_write8(cpu->mem, 0x0300, 0x5A);

    cpu->si = 0x0300;
    cpu->di = 0x0400;
    cpu->cx = 0x1234;

    vm86_test_run(cpu, 10);

    vm86_expect_mem8("copied byte", cpu, 0x0400, 0x5A);
    vm86_expect_mem8("byte past the copy", cpu, 0x0401, 0x00);
    vm86_expect_u16("SI", cpu->si, 0x0301);
    vm86_expect_u16("DI", cpu->di, 0x0401);

    /* A MOVS without a REP is one element, not a loop: it must leave the
     * count alone. An implementation that always decrements CX turns a
     * plain copy into something that eats a program's loop counter. */
    vm86_expect_u16("CX untouched", cpu->cx, 0x1234);
}

static void test_movsw_steps_backward_by_two(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xA5, 0xF4 };   /* movsw; hlt */

    vm86_test_load(cpu, code, sizeof(code));
    vm86_flag_set(cpu, VM86_DF, true);

    /* Two different words, so a copy that read the wrong one is visible. */
    vm86_mem_write16(cpu->mem, 0x0300, 0x1234);
    vm86_mem_write16(cpu->mem, 0x0302, 0x5678);

    cpu->si = 0x0302;
    cpu->di = 0x0402;

    vm86_test_run(cpu, 10);

    vm86_expect_mem16("copied word", cpu, 0x0402, 0x5678);
    /* The word below the destination is what a forward copy would have
     * written. It is the assertion that tells the two directions apart. */
    vm86_expect_mem16("word below the copy", cpu, 0x0400, 0x0000);
    vm86_expect_u16("SI", cpu->si, 0x0300);
    vm86_expect_u16("DI", cpu->di, 0x0400);
}

static void test_rep_movsb_copies_cx_bytes(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0xA4, 0xF4 };   /* rep movsb */

    vm86_test_load(cpu, code, sizeof(code));

    vm86_mem_write8(cpu->mem, 0x0300, 0x11);
    vm86_mem_write8(cpu->mem, 0x0301, 0x22);
    vm86_mem_write8(cpu->mem, 0x0302, 0x33);
    vm86_mem_write8(cpu->mem, 0x0303, 0x44);

    cpu->si = 0x0300;
    cpu->di = 0x0400;
    cpu->cx = 4;

    vm86_test_run(cpu, 10);

    vm86_expect_mem8("byte 0", cpu, 0x0400, 0x11);
    vm86_expect_mem8("byte 1", cpu, 0x0401, 0x22);
    vm86_expect_mem8("byte 2", cpu, 0x0402, 0x33);
    vm86_expect_mem8("byte 3", cpu, 0x0403, 0x44);
    vm86_expect_mem8("byte past the copy", cpu, 0x0404, 0x00);

    vm86_expect_u16("CX after the loop", cpu->cx, 0x0000);
    vm86_expect_u16("SI", cpu->si, 0x0304);
    vm86_expect_u16("DI", cpu->di, 0x0404);
}

static void test_rep_movsw_counts_words(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0xA5, 0xF4 };   /* rep movsw */

    vm86_test_load(cpu, code, sizeof(code));

    vm86_mem_write16(cpu->mem, 0x0300, 0x1111);
    vm86_mem_write16(cpu->mem, 0x0302, 0x2222);
    vm86_mem_write16(cpu->mem, 0x0304, 0x3333);

    cpu->si = 0x0300;
    cpu->di = 0x0400;
    cpu->cx = 2;      /* two WORDS: four bytes, and four of pointer */

    vm86_test_run(cpu, 10);

    vm86_expect_mem16("word 0", cpu, 0x0400, 0x1111);
    vm86_expect_mem16("word 1", cpu, 0x0402, 0x2222);
    /* CX counts elements, not bytes. An implementation that counted bytes
     * would be here with two more words still to go. */
    vm86_expect_mem16("word past the copy", cpu, 0x0404, 0x0000);

    vm86_expect_u16("CX after the loop", cpu->cx, 0x0000);
    vm86_expect_u16("SI", cpu->si, 0x0304);
    vm86_expect_u16("DI", cpu->di, 0x0404);
}

static void test_rep_movsw_backward_moves_the_block_down(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0xA5, 0xF4 };   /* rep movsw */

    vm86_test_load(cpu, code, sizeof(code));
    vm86_flag_set(cpu, VM86_DF, true);

    vm86_mem_write16(cpu->mem, 0x0300, 0x1111);
    vm86_mem_write16(cpu->mem, 0x0302, 0x2222);
    vm86_mem_write16(cpu->mem, 0x0304, 0x3333);

    cpu->si = 0x0304;   /* the last word */
    cpu->di = 0x0410;
    cpu->cx = 3;

    vm86_test_run(cpu, 10);

    /* Walking backward from the end of the source onto the end of the
     * destination: the words land in reverse order of the order they
     * were read, which is what makes this different from the forward
     * case rather than a copy that happens to look the same. */
    vm86_expect_mem16("last word written", cpu, 0x040C, 0x1111);
    vm86_expect_mem16("middle word written", cpu, 0x040E, 0x2222);
    vm86_expect_mem16("first word written", cpu, 0x0410, 0x3333);
    vm86_expect_mem16("above the block", cpu, 0x0412, 0x0000);

    vm86_expect_u16("CX after the loop", cpu->cx, 0x0000);
    vm86_expect_u16("SI", cpu->si, 0x02FE);
    vm86_expect_u16("DI", cpu->di, 0x040A);
}

static void test_rep_movsb_with_cx_zero_copies_nothing(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0xA4, 0xF4 };   /* rep movsb */

    vm86_test_load(cpu, code, sizeof(code));

    vm86_mem_write8(cpu->mem, 0x0300, 0x55);
    vm86_mem_write8(cpu->mem, 0x0400, 0x99);

    cpu->si = 0x0300;
    cpu->di = 0x0400;
    cpu->cx = 0;

    vm86_test_run(cpu, 10);

    /* The classic REP bug, caught: a loop written to run at least once
     * decrements CX from zero to 0xFFFF and then moves 65535 bytes. The
     * destination being untouched is what says it did not. */
    vm86_expect_mem8("destination untouched", cpu, 0x0400, 0x99);
    vm86_expect_u16("SI unchanged", cpu->si, 0x0300);
    vm86_expect_u16("DI unchanged", cpu->di, 0x0400);
    vm86_expect_u16("CX unchanged", cpu->cx, 0x0000);
}

static void test_rep_movsw_with_cx_zero_copies_nothing(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0xA5, 0xF4 };   /* rep movsw */

    vm86_test_load(cpu, code, sizeof(code));

    vm86_mem_write16(cpu->mem, 0x0300, 0x5555);
    vm86_mem_write16(cpu->mem, 0x0400, 0x9999);

    cpu->si = 0x0300;
    cpu->di = 0x0400;
    cpu->cx = 0;

    vm86_test_run(cpu, 10);

    vm86_expect_mem16("destination untouched", cpu, 0x0400, 0x9999);
    vm86_expect_u16("SI unchanged", cpu->si, 0x0300);
    vm86_expect_u16("DI unchanged", cpu->di, 0x0400);
}

/* ------------------------------------------------------------------ */
/* CMPS                                                                */
/* ------------------------------------------------------------------ */

static void test_cmpsb_sets_the_whole_flag_word(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xA6, 0xF4 };   /* cmpsb */

    vm86_test_load(cpu, code, sizeof(code));

    vm86_mem_write8(cpu->mem, 0x0300, 0x00);
    vm86_mem_write8(cpu->mem, 0x0400, 0x01);

    cpu->si = 0x0300;
    cpu->di = 0x0400;

    vm86_test_run(cpu, 10);

    /* CMPS is a CMP: DS:SI minus ES:DI, with the borrow and the overflow
     * worked out the same way SUB does it. 0x00 - 0x01 = 0xFF borrows,
     * goes negative, keeps an even number of bits, and carries out of
     * bit 3 -- five flags, checked together because a handler that got
     * one of them from somewhere else is exactly the bug this catches. */
    vm86_expect_flags("cmpsb flags", cpu,
                      VM86_CF | VM86_PF | VM86_AF | VM86_SF, 0);

    vm86_expect_u16("SI", cpu->si, 0x0301);
    vm86_expect_u16("DI", cpu->di, 0x0401);
}

static void test_cmpsw_compares_words_and_steps_by_two(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xA7, 0xF4 };   /* cmpsw */

    vm86_test_load(cpu, code, sizeof(code));

    vm86_mem_write16(cpu->mem, 0x0300, 0x1234);
    vm86_mem_write16(cpu->mem, 0x0400, 0x1235);

    cpu->si = 0x0300;
    cpu->di = 0x0400;

    vm86_test_run(cpu, 10);

    vm86_expect_flags("cmpsw flags", cpu,
                      VM86_CF | VM86_PF | VM86_AF | VM86_SF, 0);
    vm86_expect_u16("SI", cpu->si, 0x0302);
    vm86_expect_u16("DI", cpu->di, 0x0402);
}

static void test_repe_cmpsb_runs_to_the_end_when_equal(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0xA6, 0xF4 };   /* repe cmpsb */

    vm86_test_load(cpu, code, sizeof(code));

    vm86_mem_write8(cpu->mem, 0x0300, 0x10);
    vm86_mem_write8(cpu->mem, 0x0301, 0x20);
    vm86_mem_write8(cpu->mem, 0x0302, 0x30);
    vm86_mem_write8(cpu->mem, 0x0303, 0x40);
    vm86_mem_write8(cpu->mem, 0x0400, 0x10);
    vm86_mem_write8(cpu->mem, 0x0401, 0x20);
    vm86_mem_write8(cpu->mem, 0x0402, 0x30);
    vm86_mem_write8(cpu->mem, 0x0403, 0x40);

    cpu->si = 0x0300;
    cpu->di = 0x0400;
    cpu->cx = 4;

    vm86_test_run(cpu, 10);

    /* Nothing ever cleared ZF, so REPE ran the count out. */
    vm86_expect_flag("ZF", cpu, VM86_ZF, true);
    vm86_expect_u16("CX consumed", cpu->cx, 0x0000);
    vm86_expect_u16("SI", cpu->si, 0x0304);
    vm86_expect_u16("DI", cpu->di, 0x0404);
}

static void test_repe_cmpsb_stops_at_the_first_difference(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0xA6, 0xF4 };   /* repe cmpsb */

    vm86_test_load(cpu, code, sizeof(code));

    vm86_mem_write8(cpu->mem, 0x0300, 0x10);
    vm86_mem_write8(cpu->mem, 0x0301, 0x20);
    vm86_mem_write8(cpu->mem, 0x0302, 0x30);
    vm86_mem_write8(cpu->mem, 0x0303, 0x40);
    vm86_mem_write8(cpu->mem, 0x0400, 0x10);
    vm86_mem_write8(cpu->mem, 0x0401, 0x20);
    vm86_mem_write8(cpu->mem, 0x0402, 0x99);   /* the one that differs */
    vm86_mem_write8(cpu->mem, 0x0403, 0x40);

    cpu->si = 0x0300;
    cpu->di = 0x0400;
    cpu->cx = 4;

    vm86_test_run(cpu, 10);

    /* This is the assertion the whole instruction exists for. The bytes
     * agree at 0, 1 and 3; a REPE that behaves like REP compares all
     * four, ends with CX=0 and reports success. Stopping where it did
     * leaves the count, the pointers and ZF all saying "the third pair
     * is where they parted company". */
    vm86_expect_u16("CX after stopping early", cpu->cx, 0x0001);
    vm86_expect_u16("SI at the difference", cpu->si, 0x0303);
    vm86_expect_u16("DI at the difference", cpu->di, 0x0403);
    vm86_expect_flag("ZF", cpu, VM86_ZF, false);
    vm86_expect_flag("CF", cpu, VM86_CF, true);   /* 0x30 < 0x99 */
}

static void test_repne_cmpsb_stops_at_the_first_match(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF2, 0xA6, 0xF4 };   /* repne cmpsb */

    vm86_test_load(cpu, code, sizeof(code));

    vm86_mem_write8(cpu->mem, 0x0300, 0x01);
    vm86_mem_write8(cpu->mem, 0x0301, 0x02);
    vm86_mem_write8(cpu->mem, 0x0302, 0x03);
    vm86_mem_write8(cpu->mem, 0x0303, 0x04);
    vm86_mem_write8(cpu->mem, 0x0400, 0x09);
    vm86_mem_write8(cpu->mem, 0x0401, 0x09);
    vm86_mem_write8(cpu->mem, 0x0402, 0x03);   /* the first match */
    vm86_mem_write8(cpu->mem, 0x0403, 0x09);

    cpu->si = 0x0300;
    cpu->di = 0x0400;
    cpu->cx = 4;

    vm86_test_run(cpu, 10);

    /* F2 is the mirror of F3: it continues while the bytes differ and
     * stops on the first pair that matches. The two prefixes are the
     * same instruction apart from this, and a handler that treated them
     * alike would run to the end of the buffer here. */
    vm86_expect_u16("CX after the match", cpu->cx, 0x0001);
    vm86_expect_u16("SI at the match", cpu->si, 0x0303);
    vm86_expect_u16("DI at the match", cpu->di, 0x0403);
    vm86_expect_flag("ZF", cpu, VM86_ZF, true);
}

static void test_repe_cmpsb_backward_stops_at_the_last_pair(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0xA6, 0xF4 };   /* repe cmpsb */

    vm86_test_load(cpu, code, sizeof(code));
    vm86_flag_set(cpu, VM86_DF, true);

    /* Equal except for the last pair. Forward, this compares all four
     * and runs the count out; backward, the first pair it looks at is
     * the mismatched one and it stops there. Same instruction, same
     * bytes, completely different answer -- which is the only kind of
     * test that catches a direction flag read the wrong way round. */
    vm86_mem_write8(cpu->mem, 0x0300, 0xAA);
    vm86_mem_write8(cpu->mem, 0x0301, 0xBB);
    vm86_mem_write8(cpu->mem, 0x0302, 0xCC);
    vm86_mem_write8(cpu->mem, 0x0303, 0xDD);
    vm86_mem_write8(cpu->mem, 0x0400, 0xAA);
    vm86_mem_write8(cpu->mem, 0x0401, 0xBB);
    vm86_mem_write8(cpu->mem, 0x0402, 0xCC);
    vm86_mem_write8(cpu->mem, 0x0403, 0xEE);

    cpu->si = 0x0303;
    cpu->di = 0x0403;
    cpu->cx = 4;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("CX after stopping", cpu->cx, 0x0003);
    vm86_expect_u16("SI", cpu->si, 0x0302);
    vm86_expect_u16("DI", cpu->di, 0x0402);
    vm86_expect_flag("ZF", cpu, VM86_ZF, false);
    vm86_expect_flag("CF", cpu, VM86_CF, true);   /* 0xDD < 0xEE */
}

static void test_repe_cmpsb_with_cx_zero_compares_nothing(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0xA6, 0xF4 };   /* repe cmpsb */

    vm86_test_load(cpu, code, sizeof(code));

    /* Bytes that differ, so a comparison would clear ZF. */
    vm86_mem_write8(cpu->mem, 0x0300, 0x00);
    vm86_mem_write8(cpu->mem, 0x0400, 0xFF);

    cpu->si = 0x0300;
    cpu->di = 0x0400;
    cpu->cx = 0;

    /* Neither flag belongs to the instruction unless it runs. ZF is set
     * as if a previous comparison had matched. */
    vm86_flag_set(cpu, VM86_ZF, true);
    vm86_flag_set(cpu, VM86_CF, true);

    vm86_test_run(cpu, 10);

    vm86_expect_flag("ZF untouched", cpu, VM86_ZF, true);
    vm86_expect_flag("CF untouched", cpu, VM86_CF, true);
    vm86_expect_u16("SI unchanged", cpu->si, 0x0300);
    vm86_expect_u16("DI unchanged", cpu->di, 0x0400);
}

static void test_repe_cmpsw_with_cx_zero_compares_nothing(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0xA7, 0xF4 };   /* repe cmpsw */

    vm86_test_load(cpu, code, sizeof(code));

    /* Words that differ, so a comparison would clear ZF. */
    vm86_mem_write16(cpu->mem, 0x0300, 0x1111);
    vm86_mem_write16(cpu->mem, 0x0400, 0x2222);

    cpu->si = 0x0300;
    cpu->di = 0x0400;
    cpu->cx = 0;

    vm86_flag_set(cpu, VM86_ZF, true);

    vm86_test_run(cpu, 10);

    vm86_expect_flag("ZF untouched", cpu, VM86_ZF, true);
    vm86_expect_u16("SI unchanged", cpu->si, 0x0300);
    vm86_expect_u16("DI unchanged", cpu->di, 0x0400);
}

/* ------------------------------------------------------------------ */
/* STOS                                                                */
/* ------------------------------------------------------------------ */

static void test_stosb_stores_and_steps_forward(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xAA, 0xF4 };   /* stosb */

    vm86_test_load(cpu, code, sizeof(code));

    cpu->al = 0x7E;
    cpu->ah = 0x00;   /* so the byte below the store is unambiguous */
    cpu->di = 0x0400;
    cpu->cx = 0x1234;

    vm86_test_run(cpu, 10);

    vm86_expect_mem8("stored byte", cpu, 0x0400, 0x7E);
    vm86_expect_mem8("byte past the store", cpu, 0x0401, 0x00);
    vm86_expect_u16("DI", cpu->di, 0x0401);
    vm86_expect_u16("CX untouched", cpu->cx, 0x1234);
}

static void test_stosw_steps_backward_by_two(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xAB, 0xF4 };   /* stosw */

    vm86_test_load(cpu, code, sizeof(code));
    vm86_flag_set(cpu, VM86_DF, true);

    cpu->ax = 0xBEEF;
    cpu->di = 0x0404;

    vm86_test_run(cpu, 10);

    vm86_expect_mem16("stored word", cpu, 0x0404, 0xBEEF);
    vm86_expect_mem16("word above the store", cpu, 0x0406, 0x0000);
    vm86_expect_u16("DI", cpu->di, 0x0402);
}

static void test_rep_stosb_fills_cx_bytes(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0xAA, 0xF4 };   /* rep stosb */

    vm86_test_load(cpu, code, sizeof(code));

    cpu->al = 0x7E;
    cpu->di = 0x0400;
    cpu->cx = 4;

    vm86_test_run(cpu, 10);

    vm86_expect_mem8("byte 0", cpu, 0x0400, 0x7E);
    vm86_expect_mem8("byte 1", cpu, 0x0401, 0x7E);
    vm86_expect_mem8("byte 2", cpu, 0x0402, 0x7E);
    vm86_expect_mem8("byte 3", cpu, 0x0403, 0x7E);
    vm86_expect_mem8("byte past the fill", cpu, 0x0404, 0x00);
    vm86_expect_u16("CX after the loop", cpu->cx, 0x0000);
    vm86_expect_u16("DI", cpu->di, 0x0404);
}

static void test_rep_stosb_backward_fills_downward(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0xAA, 0xF4 };   /* rep stosb */

    vm86_test_load(cpu, code, sizeof(code));
    vm86_flag_set(cpu, VM86_DF, true);

    cpu->al = 0x7E;
    cpu->di = 0x0403;   /* the last byte of the block */
    cpu->cx = 4;

    vm86_test_run(cpu, 10);

    /* The block ends up filled either way round, which is why a fill is
     * a poor test of the direction flag on its own. Where DI finishes
     * and which byte was left alone are not. */
    vm86_expect_mem8("byte 0", cpu, 0x0400, 0x7E);
    vm86_expect_mem8("byte 3", cpu, 0x0403, 0x7E);
    vm86_expect_mem8("byte above the block", cpu, 0x0404, 0x00);
    vm86_expect_u16("DI", cpu->di, 0x03FF);
}

static void test_rep_stosw_counts_words(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0xAB, 0xF4 };   /* rep stosw */

    vm86_test_load(cpu, code, sizeof(code));

    cpu->ax = 0xBEEF;
    cpu->di = 0x0400;
    cpu->cx = 2;      /* two words: four bytes */

    vm86_test_run(cpu, 10);

    vm86_expect_mem16("word 0", cpu, 0x0400, 0xBEEF);
    vm86_expect_mem16("word 1", cpu, 0x0402, 0xBEEF);
    vm86_expect_mem16("word past the fill", cpu, 0x0404, 0x0000);
    vm86_expect_u16("CX after the loop", cpu->cx, 0x0000);
    vm86_expect_u16("DI", cpu->di, 0x0404);
}

static void test_rep_stosb_with_cx_zero_stores_nothing(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0xAA, 0xF4 };   /* rep stosb */

    vm86_test_load(cpu, code, sizeof(code));

    cpu->al = 0x7E;
    cpu->di = 0x0400;
    cpu->cx = 0;

    vm86_test_run(cpu, 10);

    vm86_expect_mem8("memory untouched", cpu, 0x0400, 0x00);
    vm86_expect_u16("DI unchanged", cpu->di, 0x0400);
    vm86_expect_u16("CX unchanged", cpu->cx, 0x0000);
}

static void test_rep_stosw_with_cx_zero_stores_nothing(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0xAB, 0xF4 };   /* rep stosw */

    vm86_test_load(cpu, code, sizeof(code));

    cpu->ax = 0xBEEF;
    cpu->di = 0x0400;
    cpu->cx = 0;

    vm86_test_run(cpu, 10);

    vm86_expect_mem16("memory untouched", cpu, 0x0400, 0x0000);
    vm86_expect_u16("DI unchanged", cpu->di, 0x0400);
}

/* ------------------------------------------------------------------ */
/* LODS                                                                */
/* ------------------------------------------------------------------ */

static void test_lodsb_loads_and_steps_forward(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xAC, 0xF4 };   /* lodsb */

    vm86_test_load(cpu, code, sizeof(code));

    vm86_mem_write8(cpu->mem, 0x0300, 0x5A);

    cpu->ax = 0xFF00;   /* AH is not part of LODSB and must survive */
    cpu->si = 0x0300;
    cpu->cx = 0x1234;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AL", cpu->al, 0x5A);
    vm86_expect_u16("AH survived", cpu->ah, 0xFF);
    vm86_expect_u16("SI", cpu->si, 0x0301);
    vm86_expect_u16("CX untouched", cpu->cx, 0x1234);
}

static void test_lodsw_steps_backward_by_two(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xAD, 0xF4 };   /* lodsw */

    vm86_test_load(cpu, code, sizeof(code));
    vm86_flag_set(cpu, VM86_DF, true);

    vm86_mem_write16(cpu->mem, 0x0300, 0xABCD);
    cpu->si = 0x0300;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0xABCD);
    vm86_expect_u16("SI", cpu->si, 0x02FE);
}

static void test_rep_lodsb_leaves_the_last_byte_in_al(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0xAC, 0xF4 };   /* rep lodsb */

    vm86_test_load(cpu, code, sizeof(code));

    vm86_mem_write8(cpu->mem, 0x0300, 0x11);
    vm86_mem_write8(cpu->mem, 0x0301, 0x22);
    vm86_mem_write8(cpu->mem, 0x0302, 0x33);

    cpu->si = 0x0300;
    cpu->cx = 3;

    vm86_test_run(cpu, 10);

    /* Each iteration overwrites AL, so what is left is the last byte
     * read -- which is how a program using LODS to walk a string steps
     * the pointer and keeps one byte. */
    vm86_expect_u16("AL", cpu->al, 0x33);
    vm86_expect_u16("SI", cpu->si, 0x0303);
    vm86_expect_u16("CX after the loop", cpu->cx, 0x0000);
}

static void test_rep_lodsb_with_cx_zero_loads_nothing(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0xAC, 0xF4 };   /* rep lodsb */

    vm86_test_load(cpu, code, sizeof(code));

    vm86_mem_write8(cpu->mem, 0x0300, 0x00);

    cpu->al = 0x99;
    cpu->si = 0x0300;
    cpu->cx = 0;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AL unchanged", cpu->al, 0x99);
    vm86_expect_u16("SI unchanged", cpu->si, 0x0300);
}

static void test_rep_lodsw_with_cx_zero_loads_nothing(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0xAD, 0xF4 };   /* rep lodsw */

    vm86_test_load(cpu, code, sizeof(code));

    vm86_mem_write16(cpu->mem, 0x0300, 0x0000);

    cpu->ax = 0x1234;
    cpu->si = 0x0300;
    cpu->cx = 0;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX unchanged", cpu->ax, 0x1234);
    vm86_expect_u16("SI unchanged", cpu->si, 0x0300);
}

/* ------------------------------------------------------------------ */
/* SCAS                                                                */
/* ------------------------------------------------------------------ */

static void test_scasb_sets_the_whole_flag_word(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xAE, 0xF4 };   /* scasb */

    vm86_test_load(cpu, code, sizeof(code));

    vm86_mem_write8(cpu->mem, 0x0400, 0x01);

    cpu->al = 0x00;
    cpu->di = 0x0400;

    vm86_test_run(cpu, 10);

    /* SCAS is CMPS with the operands the other way round: the
     * accumulator is the minuend, and the memory operand is the one it
     * borrows from. Which way round they go is what decides the carry,
     * and a program scanning for a character branches on it. */
    vm86_expect_flags("scasb flags", cpu,
                      VM86_CF | VM86_PF | VM86_AF | VM86_SF, 0);
    vm86_expect_u16("DI", cpu->di, 0x0401);
}

static void test_scasw_compares_words_and_steps_by_two(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xAF, 0xF4 };   /* scasw */

    vm86_test_load(cpu, code, sizeof(code));

    vm86_mem_write16(cpu->mem, 0x0400, 0x1235);

    cpu->ax = 0x1234;
    cpu->di = 0x0400;

    vm86_test_run(cpu, 10);

    vm86_expect_flags("scasw flags", cpu,
                      VM86_CF | VM86_PF | VM86_AF | VM86_SF, 0);
    vm86_expect_u16("DI", cpu->di, 0x0402);
}

static void test_repne_scasb_scans_for_a_byte(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF2, 0xAE, 0xF4 };   /* repne scasb */

    vm86_test_load(cpu, code, sizeof(code));

    /* A buffer of eight bytes to be searched for 0x03, which is at
     * offset two. */
    vm86_mem_write8(cpu->mem, 0x0400, 0x01);
    vm86_mem_write8(cpu->mem, 0x0401, 0x02);
    vm86_mem_write8(cpu->mem, 0x0402, 0x03);
    vm86_mem_write8(cpu->mem, 0x0403, 0x04);

    cpu->al = 0x03;
    cpu->di = 0x0400;
    cpu->cx = 8;

    vm86_test_run(cpu, 10);

    /* The idiom this instruction exists for: search a buffer, and on
     * return let CX say how far there is left to look. DI is one past
     * the byte found, and the count is what was not scanned. */
    vm86_expect_flag("ZF", cpu, VM86_ZF, true);
    vm86_expect_u16("DI", cpu->di, 0x0403);
    vm86_expect_u16("CX remaining", cpu->cx, 0x0005);
}

static void test_repe_scasb_stops_at_a_difference(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0xAE, 0xF4 };   /* repe scasb */

    vm86_test_load(cpu, code, sizeof(code));

    vm86_mem_write8(cpu->mem, 0x0400, 0x55);
    vm86_mem_write8(cpu->mem, 0x0401, 0x55);
    vm86_mem_write8(cpu->mem, 0x0402, 0x56);   /* the odd one out */
    vm86_mem_write8(cpu->mem, 0x0403, 0x55);

    cpu->al = 0x55;
    cpu->di = 0x0400;
    cpu->cx = 4;

    vm86_test_run(cpu, 10);

    /* REPE on SCAS: continue while the bytes match, stop at the first
     * one that does not. F3 and F2 differ in nothing else. */
    vm86_expect_u16("DI at the difference", cpu->di, 0x0403);
    vm86_expect_u16("CX remaining", cpu->cx, 0x0001);
    vm86_expect_flag("ZF", cpu, VM86_ZF, false);
    vm86_expect_flag("CF", cpu, VM86_CF, true);   /* 0x55 < 0x56 */
}

static void test_repne_scasb_with_cx_zero_scans_nothing(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF2, 0xAE, 0xF4 };   /* repne scasb */

    vm86_test_load(cpu, code, sizeof(code));

    /* The byte would match, so a scan that ran would set ZF. */
    vm86_mem_write8(cpu->mem, 0x0400, 0x42);

    cpu->al = 0x42;
    cpu->di = 0x0400;
    cpu->cx = 0;

    vm86_flag_set(cpu, VM86_ZF, false);

    vm86_test_run(cpu, 10);

    vm86_expect_flag("ZF untouched", cpu, VM86_ZF, false);
    vm86_expect_u16("DI unchanged", cpu->di, 0x0400);
    vm86_expect_u16("CX unchanged", cpu->cx, 0x0000);
}

static void test_repne_scasw_with_cx_zero_scans_nothing(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF2, 0xAF, 0xF4 };   /* repne scasw */

    vm86_test_load(cpu, code, sizeof(code));

    /* The word would match, so a scan that ran would set ZF. */
    vm86_mem_write16(cpu->mem, 0x0400, 0xBEEF);

    cpu->ax = 0xBEEF;
    cpu->di = 0x0400;
    cpu->cx = 0;

    vm86_flag_set(cpu, VM86_ZF, false);

    vm86_test_run(cpu, 10);

    vm86_expect_flag("ZF untouched", cpu, VM86_ZF, false);
    vm86_expect_u16("DI unchanged", cpu->di, 0x0400);
    vm86_expect_u16("CX unchanged", cpu->cx, 0x0000);
}

/* ------------------------------------------------------------------ */
/* F2 on the instructions that have no comparison                      */
/* ------------------------------------------------------------------ */

/*
 * `repne movs` is not a documented instruction: there is nothing for
 * "not equal" to mean where nothing is compared. This implementation
 * treats F2 exactly as F3 for MOVS, STOS and LODS -- repeat CX times --
 * and these three cases are what pins that decision down. The
 * alternative, executing once and ignoring the prefix, would copy one
 * byte of a buffer instead of the buffer, which is the worse failure.
 */

static void test_f2_on_movs_repeats_like_f3(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF2, 0xA4, 0xF4 };   /* repne movsb */

    vm86_test_load(cpu, code, sizeof(code));

    vm86_mem_write8(cpu->mem, 0x0300, 0x11);
    vm86_mem_write8(cpu->mem, 0x0301, 0x22);
    vm86_mem_write8(cpu->mem, 0x0302, 0x33);

    cpu->si = 0x0300;
    cpu->di = 0x0400;
    cpu->cx = 3;

    vm86_test_run(cpu, 10);

    vm86_expect_mem8("byte 0", cpu, 0x0400, 0x11);
    vm86_expect_mem8("byte 1", cpu, 0x0401, 0x22);
    vm86_expect_mem8("byte 2", cpu, 0x0402, 0x33);
    vm86_expect_u16("CX after the loop", cpu->cx, 0x0000);
    vm86_expect_u16("DI", cpu->di, 0x0403);
}

static void test_f2_on_stos_repeats_like_f3(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF2, 0xAA, 0xF4 };   /* repne stosb */

    vm86_test_load(cpu, code, sizeof(code));

    cpu->al = 0x5C;
    cpu->di = 0x0400;
    cpu->cx = 3;

    vm86_test_run(cpu, 10);

    vm86_expect_mem8("byte 0", cpu, 0x0400, 0x5C);
    vm86_expect_mem8("byte 1", cpu, 0x0401, 0x5C);
    vm86_expect_mem8("byte 2", cpu, 0x0402, 0x5C);
    vm86_expect_mem8("byte past the fill", cpu, 0x0403, 0x00);
    vm86_expect_u16("CX after the loop", cpu->cx, 0x0000);
}

static void test_f2_on_lods_repeats_like_f3(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF2, 0xAC, 0xF4 };   /* repne lodsb */

    vm86_test_load(cpu, code, sizeof(code));

    vm86_mem_write8(cpu->mem, 0x0300, 0x01);
    vm86_mem_write8(cpu->mem, 0x0301, 0x02);
    vm86_mem_write8(cpu->mem, 0x0302, 0x03);

    cpu->si = 0x0300;
    cpu->cx = 3;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AL", cpu->al, 0x03);
    vm86_expect_u16("SI", cpu->si, 0x0303);
    vm86_expect_u16("CX after the loop", cpu->cx, 0x0000);
}

/* ------------------------------------------------------------------ */
/* Flags: the movers touch none, the comparers set them all            */
/* ------------------------------------------------------------------ */

static void test_movs_leaves_every_flag_alone(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0xA4, 0xF4 };   /* rep movsb */

    vm86_test_load(cpu, code, sizeof(code));

    vm86_mem_write8(cpu->mem, 0x0300, 0x11);
    vm86_mem_write8(cpu->mem, 0x0301, 0x22);

    cpu->si = 0x0300;
    cpu->di = 0x0400;
    cpu->cx = 2;

    cpu->flags |= FLAGS_UNDER_TEST;

    vm86_test_run(cpu, 10);

    vm86_expect_flags("movs leaves the flags alone", cpu,
                      FLAGS_UNDER_TEST, 0);

    /* And the loop did run: a test that only proves nothing changed
     * would pass on a handler that did nothing at all. */
    vm86_expect_u16("CX ran out", cpu->cx, 0x0000);
    vm86_expect_mem8("byte 1", cpu, 0x0401, 0x22);
}

static void test_stos_leaves_every_flag_alone(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0xAA, 0xF4 };   /* rep stosb */

    vm86_test_load(cpu, code, sizeof(code));

    cpu->al = 0x7E;
    cpu->di = 0x0400;
    cpu->cx = 2;

    cpu->flags |= FLAGS_UNDER_TEST;

    vm86_test_run(cpu, 10);

    vm86_expect_flags("stos leaves the flags alone", cpu,
                      FLAGS_UNDER_TEST, 0);
    vm86_expect_u16("CX ran out", cpu->cx, 0x0000);
    vm86_expect_mem8("byte 1", cpu, 0x0401, 0x7E);
}

static void test_lods_leaves_every_flag_alone(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0xAD, 0xF4 };   /* rep lodsw */

    vm86_test_load(cpu, code, sizeof(code));

    vm86_mem_write16(cpu->mem, 0x0300, 0x1111);
    vm86_mem_write16(cpu->mem, 0x0302, 0x2222);

    cpu->si = 0x0300;
    cpu->cx = 2;

    cpu->flags |= FLAGS_UNDER_TEST;

    vm86_test_run(cpu, 10);

    vm86_expect_flags("lods leaves the flags alone", cpu,
                      FLAGS_UNDER_TEST, 0);
    vm86_expect_u16("CX ran out", cpu->cx, 0x0000);
    vm86_expect_u16("AX", cpu->ax, 0x2222);
}

/* ------------------------------------------------------------------ */
/* Segments                                                            */
/* ------------------------------------------------------------------ */

/*
 * These cases run with DS and ES 0x1000 apart, which is the only way to
 * tell which of the two a memory access went through. The two loads and
 * stores below are placed so that a handler using the wrong segment
 * reads a different byte rather than the same byte somewhere else.
 */

static void test_es_override_moves_the_source_side(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x26, 0xA4, 0xF4 };   /* es: movsb */

    vm86_test_load(cpu, code, sizeof(code));
    vm86_set_seg(cpu, VM86_DS, 0x1000);
    vm86_set_seg(cpu, VM86_ES, 0x2000);
    vm86_flush_segments(cpu);

    vm86_mem_write8(cpu->mem, 0x20010, 0xE5);   /* ES:SI -- the prefix's pick */
    vm86_mem_write8(cpu->mem, 0x10010, 0xD5);   /* DS:SI -- what it must ignore */

    cpu->si = 0x0010;
    cpu->di = 0x0020;

    vm86_test_run(cpu, 10);

    /* The override is on the read side: the byte that moved is the one
     * at ES:SI. */
    vm86_expect_mem8("written through ES:DI", cpu, 0x20020, 0xE5);
    /* And nowhere else. DS:DI is what a handler that applied the prefix
     * to the store as well would have written. */
    vm86_expect_mem8("DS:DI untouched", cpu, 0x10020, 0x00);
    vm86_expect_mem8("DS:SI untouched", cpu, 0x10010, 0xD5);
}

static void test_ds_override_still_writes_through_es(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x3E, 0xA4, 0xF4 };   /* ds: movsb */

    vm86_test_load(cpu, code, sizeof(code));
    vm86_set_seg(cpu, VM86_DS, 0x1000);
    vm86_set_seg(cpu, VM86_ES, 0x2000);
    vm86_flush_segments(cpu);

    vm86_mem_write8(cpu->mem, 0x10010, 0x11);   /* DS:SI */

    cpu->si = 0x0010;
    cpu->di = 0x0020;

    vm86_test_run(cpu, 10);

    /* A prefix naming DS on an instruction whose read side is already DS
     * changes nothing, which is the point: it must not drag the store
     * across with it. There is no encoding on this processor that makes
     * a string write land anywhere but ES:DI. */
    vm86_expect_mem8("written through ES:DI", cpu, 0x20020, 0x11);
    vm86_expect_mem8("DS:DI untouched", cpu, 0x10020, 0x00);
}

static void test_ds_override_does_not_move_a_stos(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x3E, 0xAA, 0xF4 };   /* ds: stosb */

    vm86_test_load(cpu, code, sizeof(code));
    vm86_set_seg(cpu, VM86_DS, 0x1000);
    vm86_set_seg(cpu, VM86_ES, 0x2000);
    vm86_flush_segments(cpu);

    cpu->al = 0x77;
    cpu->di = 0x0040;

    vm86_test_run(cpu, 10);

    /* STOS has no DS-side operand at all, so a prefix on it has nothing
     * to attach to and is ignored rather than redirected. */
    vm86_expect_mem8("written through ES:DI", cpu, 0x20040, 0x77);
    vm86_expect_mem8("DS:DI untouched", cpu, 0x10040, 0x00);
}

static void test_ds_override_does_not_move_a_scas(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x3E, 0xAE, 0xF4 };   /* ds: scasb */

    vm86_test_load(cpu, code, sizeof(code));
    vm86_set_seg(cpu, VM86_DS, 0x1000);
    vm86_set_seg(cpu, VM86_ES, 0x2000);
    vm86_flush_segments(cpu);

    vm86_mem_write8(cpu->mem, 0x20060, 0x55);   /* ES:DI -- equal to AL */
    vm86_mem_write8(cpu->mem, 0x10060, 0x00);   /* DS:DI -- would not be */

    cpu->al = 0x55;
    cpu->di = 0x0060;

    vm86_test_run(cpu, 10);

    /* ZF set says the byte it compared was the one at ES:DI. The same
     * instruction compared against DS:DI would have found 0x00 and
     * cleared the flag. */
    vm86_expect_flag("ZF", cpu, VM86_ZF, true);
}

static void test_es_override_moves_the_source_side_of_cmps(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x26, 0xA6, 0xF4 };   /* es: cmpsb */

    vm86_test_load(cpu, code, sizeof(code));
    vm86_set_seg(cpu, VM86_DS, 0x1000);
    vm86_set_seg(cpu, VM86_ES, 0x2000);
    vm86_flush_segments(cpu);

    vm86_mem_write8(cpu->mem, 0x20070, 0x42);   /* ES:SI, the prefixed source */
    vm86_mem_write8(cpu->mem, 0x10070, 0x99);   /* DS:SI, ignored */
    vm86_mem_write8(cpu->mem, 0x20080, 0x42);   /* ES:DI, never overridable */

    cpu->si = 0x0070;
    cpu->di = 0x0080;

    vm86_test_run(cpu, 10);

    /* CMPS reads two memory operands and only the first obeys the
     * prefix. Equal says the source came from ES:SI; the 0x99 sitting at
     * DS:SI is what would have made it unequal. */
    vm86_expect_flag("ZF", cpu, VM86_ZF, true);
}

static void test_ds_override_leaves_the_cmps_second_operand_in_es(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x3E, 0xA6, 0xF4 };   /* ds: cmpsb */

    vm86_test_load(cpu, code, sizeof(code));
    vm86_set_seg(cpu, VM86_DS, 0x1000);
    vm86_set_seg(cpu, VM86_ES, 0x2000);
    vm86_flush_segments(cpu);

    vm86_mem_write8(cpu->mem, 0x100A0, 0x42);   /* DS:SI */
    vm86_mem_write8(cpu->mem, 0x200B0, 0x42);   /* ES:DI -- the match */
    vm86_mem_write8(cpu->mem, 0x100B0, 0x99);   /* DS:DI -- not the match */

    cpu->si = 0x00A0;
    cpu->di = 0x00B0;

    vm86_test_run(cpu, 10);

    /* CMPS has two memory operands and a prefix reaches one of them. An
     * implementation that read the second from the same segment as the
     * first -- which is what "the prefix applies to the instruction"
     * means if it is taken literally -- would compare 0x42 against 0x99
     * and clear ZF. */
    vm86_expect_flag("ZF", cpu, VM86_ZF, true);
}

static void test_es_override_moves_the_source_side_of_lods(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x26, 0xAC, 0xF4 };   /* es: lodsb */

    vm86_test_load(cpu, code, sizeof(code));
    vm86_set_seg(cpu, VM86_DS, 0x1000);
    vm86_set_seg(cpu, VM86_ES, 0x2000);
    vm86_flush_segments(cpu);

    vm86_mem_write8(cpu->mem, 0x20090, 0x5A);
    vm86_mem_write8(cpu->mem, 0x10090, 0x00);

    cpu->si = 0x0090;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AL", cpu->al, 0x5A);
    vm86_expect_u16("SI", cpu->si, 0x0091);
}

/* ------------------------------------------------------------------ */

static const struct vm86_test tests[] = {
    { "movsb copies forward",            test_movsb_copies_and_steps_forward },
    { "movsw steps backward by two",     test_movsw_steps_backward_by_two },
    { "rep movsb copies CX bytes",       test_rep_movsb_copies_cx_bytes },
    { "rep movsw counts words",          test_rep_movsw_counts_words },
    { "rep movsw backward",              test_rep_movsw_backward_moves_the_block_down },
    { "rep movsb CX=0 copies nothing",   test_rep_movsb_with_cx_zero_copies_nothing },
    { "rep movsw CX=0 copies nothing",   test_rep_movsw_with_cx_zero_copies_nothing },

    { "cmpsb sets the CMP flags",        test_cmpsb_sets_the_whole_flag_word },
    { "cmpsw compares words",            test_cmpsw_compares_words_and_steps_by_two },
    { "repe cmpsb to the end",           test_repe_cmpsb_runs_to_the_end_when_equal },
    { "repe cmpsb stops at a difference", test_repe_cmpsb_stops_at_the_first_difference },
    { "repne cmpsb stops at a match",    test_repne_cmpsb_stops_at_the_first_match },
    { "repe cmpsb backward",             test_repe_cmpsb_backward_stops_at_the_last_pair },
    { "repe cmpsb CX=0 compares nothing", test_repe_cmpsb_with_cx_zero_compares_nothing },
    { "repe cmpsw CX=0 compares nothing", test_repe_cmpsw_with_cx_zero_compares_nothing },

    { "stosb stores forward",            test_stosb_stores_and_steps_forward },
    { "stosw steps backward by two",     test_stosw_steps_backward_by_two },
    { "rep stosb fills CX bytes",        test_rep_stosb_fills_cx_bytes },
    { "rep stosb backward",              test_rep_stosb_backward_fills_downward },
    { "rep stosw counts words",          test_rep_stosw_counts_words },
    { "rep stosb CX=0 stores nothing",   test_rep_stosb_with_cx_zero_stores_nothing },
    { "rep stosw CX=0 stores nothing",   test_rep_stosw_with_cx_zero_stores_nothing },

    { "lodsb loads forward",             test_lodsb_loads_and_steps_forward },
    { "lodsw steps backward by two",     test_lodsw_steps_backward_by_two },
    { "rep lodsb keeps the last byte",   test_rep_lodsb_leaves_the_last_byte_in_al },
    { "rep lodsb CX=0 loads nothing",    test_rep_lodsb_with_cx_zero_loads_nothing },
    { "rep lodsw CX=0 loads nothing",    test_rep_lodsw_with_cx_zero_loads_nothing },

    { "scasb sets the CMP flags",        test_scasb_sets_the_whole_flag_word },
    { "scasw compares words",            test_scasw_compares_words_and_steps_by_two },
    { "repne scasb scans for a byte",    test_repne_scasb_scans_for_a_byte },
    { "repe scasb stops at a difference", test_repe_scasb_stops_at_a_difference },
    { "repne scasb CX=0 scans nothing",  test_repne_scasb_with_cx_zero_scans_nothing },
    { "repne scasw CX=0 scans nothing",  test_repne_scasw_with_cx_zero_scans_nothing },

    { "F2 on movs acts as F3",           test_f2_on_movs_repeats_like_f3 },
    { "F2 on stos acts as F3",           test_f2_on_stos_repeats_like_f3 },
    { "F2 on lods acts as F3",           test_f2_on_lods_repeats_like_f3 },

    { "movs leaves flags alone",         test_movs_leaves_every_flag_alone },
    { "stos leaves flags alone",         test_stos_leaves_every_flag_alone },
    { "lods leaves flags alone",         test_lods_leaves_every_flag_alone },

    { "es: moves the movs source",       test_es_override_moves_the_source_side },
    { "ds: still writes through es",     test_ds_override_still_writes_through_es },
    { "ds: does not move a stos",        test_ds_override_does_not_move_a_stos },
    { "ds: does not move a scas",        test_ds_override_does_not_move_a_scas },
    { "es: moves the cmps source",       test_es_override_moves_the_source_side_of_cmps },
    { "ds: leaves the cmps second in es", test_ds_override_leaves_the_cmps_second_operand_in_es },
    { "es: moves the lods source",       test_es_override_moves_the_source_side_of_lods },
};

VM86_TEST_MAIN("str", tests)
