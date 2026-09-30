/*
 * The cases for ops_alu.c: arithmetic, logic, shifts, tests and the
 * decimal adjustments.
 *
 * ---------------------------------------------------------------------
 * How these are arranged
 *
 * In the order the handlers appear in the file, which is roughly the order
 * the opcode map puts them in: the decimal adjustments, the group 1
 * immediate forms, TEST, CBW and CWD, the shifts, AAM and AAD, the group 3
 * block, and FE.
 *
 * Every instruction gets a case that just exercises it, and then cases for
 * the boundaries where it could plausibly be written wrong: the carry out
 * of a shift, the sign extension of 83's immediate, the quotient that does
 * not fit in DIV. Where the manual leaves a flag undefined the test says so
 * with the harness's ignore mask rather than asserting whatever value this
 * implementation happens to leave behind -- an assertion about an undefined
 * flag passes whatever the code does, which is worse than no test at all.
 */
#include "harness.h"

/*
 * The flags the manual does not define after an instruction.
 *
 * OF after a decimal adjustment; AF after a logical operation; OF, SF, ZF
 * and PF after AAA and AAS; everything but the three result flags after AAM
 * and AAD; AF, SF, ZF and PF after a multiply, which defines CF and OF; and
 * all six of them after a divide.
 */
#define UNDEFINED_OF       (VM86_OF)
#define UNDEFINED_LOGIC    (VM86_AF)
#define UNDEFINED_ASCII    (VM86_OF | VM86_SF | VM86_ZF | VM86_PF)
#define UNDEFINED_DECIMAL  (VM86_CF | VM86_OF | VM86_AF)
#define UNDEFINED_AFTER_MUL (VM86_AF | VM86_SF | VM86_ZF | VM86_PF)
#define UNDEFINED_MULDIV   (VM86_CF | VM86_OF | VM86_AF | VM86_SF | \
                            VM86_ZF | VM86_PF)

/* Where the cases that use memory put their data. Any address that is not
 * the code at 0x100 will do; this one is far enough away that an
 * instruction walking off the end of the code cannot reach it. */
#define DATA 0x0400u

/* ------------------------------------------------------------------ */
/* 27: DAA                                                             */
/* ------------------------------------------------------------------ */

static void test_daa_leaves_a_valid_digit_alone(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x27, 0xF4 };   /* daa */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->al = 0x42;

    vm86_test_run(cpu, 5);

    /* 42 is a valid packed BCD pair and neither AF nor CF is set, so there
     * is nothing to correct. PF looks at the low byte: 42h has two bits. */
    vm86_expect_u16("AL", cpu->al, 0x42);
    vm86_expect_flags("flags", cpu, VM86_PF, UNDEFINED_OF);
}

static void test_daa_corrects_the_low_digit(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x27, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->al = 0x3F;      /* what ADD leaves after 19h + 26h */

    vm86_test_run(cpu, 5);

    /* The low nibble is not a digit, so six is added. That did not push AL
     * past 9Fh, so the high digit is still fine and CF stays clear. */
    vm86_expect_u16("AL", cpu->al, 0x45);
    vm86_expect_flags("flags", cpu, VM86_AF, UNDEFINED_OF);
}

static void test_daa_corrects_the_high_digit(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x27, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->al = 0xA2;

    vm86_test_run(cpu, 5);

    /* The low nibble is a digit and nothing carried into it, so only the
     * high one is corrected -- with 60h, which carries out of AL. */
    vm86_expect_u16("AL", cpu->al, 0x02);
    vm86_expect_flags("flags", cpu, VM86_CF, UNDEFINED_OF);
}

static void test_daa_corrects_both_digits(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x27, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->al = 0x9A;

    vm86_test_run(cpu, 5);

    /* The case that shows the second test reading the *corrected* AL: 9Ah
     * becomes A0h on the first correction, and A0h is over 9Fh, so the
     * second one fires too. Reading the original value instead would stop
     * at A0h with the carry clear -- a wrong answer that looks like a
     * plausible one, which is why it is worth a case of its own.
     *
     * A0h + 60h is 100h, so AL wraps to zero and ZF comes on. */
    vm86_expect_u16("AL", cpu->al, 0x00);
    vm86_expect_flags("flags", cpu,
                      VM86_CF | VM86_AF | VM86_ZF | VM86_PF, UNDEFINED_OF);
}

static void test_daa_keeps_an_existing_carry(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x27, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->al = 0x10;
    vm86_flag_set(cpu, VM86_CF, true);

    vm86_test_run(cpu, 5);

    /* DAA reads CF as well as writing it, and a carry out of the addition
     * means the high digit needs correcting whatever AL holds now. */
    vm86_expect_u16("AL", cpu->al, 0x70);
    vm86_expect_flags("flags", cpu, VM86_CF, UNDEFINED_OF);
}

/* ------------------------------------------------------------------ */
/* 2F: DAS                                                             */
/* ------------------------------------------------------------------ */

static void test_das_leaves_a_valid_digit_alone(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x2F, 0xF4 };   /* das */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->al = 0x42;

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AL", cpu->al, 0x42);
    vm86_expect_flags("flags", cpu, VM86_PF, UNDEFINED_OF);
}

static void test_das_corrects_the_low_digit(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x2F, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->al = 0x3F;
    vm86_flag_set(cpu, VM86_AF, true);   /* as the SUB before it left */

    vm86_test_run(cpu, 5);

    /* The manual's own example: 94h minus 55h leaves 3Fh, and DAS turns it
     * into 39h. Six comes off because the low nibble is not a digit. */
    vm86_expect_u16("AL", cpu->al, 0x39);
    vm86_expect_flags("flags", cpu, VM86_AF | VM86_PF, UNDEFINED_OF);
}

static void test_das_corrects_the_high_digit(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x2F, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->al = 0xE5;                      /* what 05h - 20h leaves */
    vm86_flag_set(cpu, VM86_CF, true);   /* the borrow, which is the point */

    vm86_test_run(cpu, 5);

    /* The low nibble was a digit all along, so only 60h comes off -- and
     * the borrow is what says to do it, not the value of AL, which the
     * first correction would have moved out of range anyway. */
    vm86_expect_u16("AL", cpu->al, 0x85);
    vm86_expect_flags("flags", cpu, VM86_CF | VM86_SF, UNDEFINED_OF);
}

static void test_das_high_digit_after_correction(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x2F, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->al = 0x00;
    vm86_flag_set(cpu, VM86_AF, true);

    vm86_test_run(cpu, 5);

    /* Subtracting six from zero wraps to FAh, which is above 9Fh, so the
     * high correction fires as well and takes another 60h. Only the
     * corrected value makes that happen: the original zero matches neither
     * test, and an implementation that tested it would stop at FAh. */
    vm86_expect_u16("AL", cpu->al, 0x9A);
    vm86_expect_flags("flags", cpu,
                      VM86_CF | VM86_AF | VM86_SF | VM86_PF, UNDEFINED_OF);
}

/* ------------------------------------------------------------------ */
/* 37: AAA                                                             */
/* ------------------------------------------------------------------ */

static void test_aaa_leaves_a_valid_digit_alone(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x37, 0xF4 };   /* aaa */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->al = 0x05;
    cpu->ah = 0x11;

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AL", cpu->al, 0x05);
    vm86_expect_u16("AH untouched", cpu->ah, 0x11);
    vm86_expect_flags("flags", cpu, 0, UNDEFINED_ASCII);
}

static void test_aaa_clears_the_high_nibble(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x37, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->al = 0x3A;      /* '0' + 0Ah, which ADD can produce */
    cpu->ah = 0x11;

    vm86_test_run(cpu, 5);

    /* The low nibble is not a digit, so six is added -- and the high
     * nibble, which was never a digit either, is cleared whatever happened.
     * AH takes the carry. */
    vm86_expect_u16("AL", cpu->al, 0x00);
    vm86_expect_u16("AH", cpu->ah, 0x12);
    vm86_expect_flags("flags", cpu, VM86_CF | VM86_AF, UNDEFINED_ASCII);
}

static void test_aaa_corrects_on_af_alone(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x37, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->al = 0x02;      /* a perfectly good digit */
    cpu->ah = 0x00;
    vm86_flag_set(cpu, VM86_AF, true);

    vm86_test_run(cpu, 5);

    /* AF says the digit went past 9 on the way here even though what is
     * left of it looks fine, so the correction happens anyway. A version
     * that only looked at the nibble would return 02 here. */
    vm86_expect_u16("AL", cpu->al, 0x08);
    vm86_expect_u16("AH", cpu->ah, 0x01);
    vm86_expect_flags("flags", cpu, VM86_CF | VM86_AF, UNDEFINED_ASCII);
}

static void test_aaa_writes_only_cf_and_af(struct vm86_cpu *cpu)
{
    /* sub al, al leaves ZF and PF set and everything else clear; AAA then
     * runs on that state and must not disturb what it did not write. The
     * manual leaves SF, ZF and PF undefined here, which is exactly why a
     * program that ran an ADD first cares: on the hardware the ADD's flags
     * are still there afterwards. */
    static const uint8_t code[] = { 0x2C, 0x00,   /* sub al, al */
                                    0x37,         /* aaa        */
                                    0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AL", cpu->al, 0x00);
    vm86_expect_flags("flags", cpu, VM86_ZF | VM86_PF, 0);
}

/* ------------------------------------------------------------------ */
/* 3F: AAS                                                             */
/* ------------------------------------------------------------------ */

static void test_aas_leaves_a_valid_digit_alone(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x3F, 0xF4 };   /* aas */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->al = 0x07;
    cpu->ah = 0x05;

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AL", cpu->al, 0x07);
    vm86_expect_u16("AH untouched", cpu->ah, 0x05);
    vm86_expect_flags("flags", cpu, 0, UNDEFINED_ASCII);
}

static void test_aas_corrects_and_borrows_from_ah(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x3F, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->al = 0x00;
    cpu->ah = 0x05;
    vm86_flag_set(cpu, VM86_AF, true);   /* the borrow, as the SUB left it */

    vm86_test_run(cpu, 5);

    /* Six is subtracted, which wraps, and only the low nibble survives. */
    vm86_expect_u16("AL", cpu->al, 0x0A);
    vm86_expect_u16("AH", cpu->ah, 0x04);
    vm86_expect_flags("flags", cpu, VM86_CF | VM86_AF, UNDEFINED_ASCII);
}

static void test_aas_clears_the_high_nibble(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x3F, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->al = 0x35;      /* the high nibble is not a digit */
    cpu->ah = 0x05;

    vm86_test_run(cpu, 5);

    /* Nothing to correct, so AH keeps its value and the flags are cleared
     * -- but the instruction still says the high nibble is not part of the
     * number. */
    vm86_expect_u16("AL", cpu->al, 0x05);
    vm86_expect_u16("AH", cpu->ah, 0x05);
    vm86_expect_flags("flags", cpu, 0, UNDEFINED_ASCII);
}

/* ------------------------------------------------------------------ */
/* 80-83: group 1, the immediate forms                                 */
/* ------------------------------------------------------------------ */

static void test_group1_add_byte_immediate(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x80, 0xC0, 0x05, 0xF4 };   /* add al,5 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0010;

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AL", cpu->al, 0x15);
    vm86_expect_flags("flags", cpu, 0, 0);
}

static void test_group1_add_carries_out(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x80, 0xC0, 0x01, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x00FF;

    vm86_test_run(cpu, 5);

    /* FFh + 1 is a byte's worth of carry and nothing else. */
    vm86_expect_u16("AL", cpu->al, 0x00);
    vm86_expect_flags("flags", cpu,
                      VM86_CF | VM86_AF | VM86_ZF | VM86_PF, 0);
}

static void test_group1_or(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x80, 0xC8, 0x0F, 0xF4 };  /* or al,0Fh */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0030;
    vm86_flag_set(cpu, VM86_CF, true);   /* to prove it gets cleared */
    vm86_flag_set(cpu, VM86_OF, true);

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AL", cpu->al, 0x3F);
    vm86_expect_flags("flags", cpu, VM86_PF, UNDEFINED_LOGIC);
}

static void test_group1_adc_reads_the_carry(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x80, 0xD0, 0x01, 0xF4 };   /* adc al,1 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x000F;
    vm86_flag_set(cpu, VM86_CF, true);

    vm86_test_run(cpu, 5);

    /* 0Fh + 1 + 1 is 11h: the carry is an input here, not only an output. */
    vm86_expect_u16("AL", cpu->al, 0x11);
    vm86_expect_flags("flags", cpu, VM86_AF | VM86_PF, 0);
}

static void test_group1_sbb_borrows_the_carry(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x80, 0xD8, 0x01, 0xF4 };   /* sbb al,1 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0000;
    vm86_flag_set(cpu, VM86_CF, true);

    vm86_test_run(cpu, 5);

    /* Zero minus one minus one borrows twice over. CF means borrow here,
     * the opposite of what it means after an addition. */
    vm86_expect_u16("AL", cpu->al, 0xFE);
    vm86_expect_flags("flags", cpu, VM86_CF | VM86_AF | VM86_SF, 0);
}

static void test_group1_and_clears_cf_and_of(struct vm86_cpu *cpu)
{
    /* and al, 0Fh */
    static const uint8_t code[] = { 0x80, 0xE0, 0x0F, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x003C;
    vm86_flag_set(cpu, VM86_CF, true);
    vm86_flag_set(cpu, VM86_OF, true);

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AL", cpu->al, 0x0C);
    vm86_expect_flags("flags", cpu, VM86_PF, UNDEFINED_LOGIC);
}

static void test_group1_sub_borrows(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x80, 0xE8, 0x01, 0xF4 };   /* sub al,1 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0000;

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AL", cpu->al, 0xFF);
    vm86_expect_flags("flags", cpu, VM86_CF | VM86_AF | VM86_SF | VM86_PF, 0);
}

static void test_group1_xor(struct vm86_cpu *cpu)
{
    /* xor al, FFh */
    static const uint8_t code[] = { 0x80, 0xF0, 0xFF, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x00AA;

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AL", cpu->al, 0x55);
    vm86_expect_flags("flags", cpu, VM86_PF, UNDEFINED_LOGIC);
}

static void test_group1_cmp_does_not_store(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x80, 0xF8, 0x05, 0xF4 };   /* cmp al,5 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0003;

    vm86_test_run(cpu, 5);

    /* The flags are SUB's, the result is thrown away. AL must still be 3:
     * an implementation that shares its code with SUB and forgets to
     * discard the result passes every test that only looks at the flags. */
    vm86_expect_u16("AL", cpu->al, 0x03);
    vm86_expect_flags("flags", cpu, VM86_CF | VM86_AF | VM86_SF, 0);
}

static void test_group1_cmp_through_memory(struct vm86_cpu *cpu)
{
    /* cmp byte [0400h], 5 */
    static const uint8_t code[] = { 0x80, 0x3E, 0x00, 0x04, 0x05, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_mem_write8(cpu->mem, DATA, 0x03);

    vm86_test_run(cpu, 5);

    /* The same claim as the register case, on the path where storing the
     * result would be a write to memory rather than to a register -- which
     * is the version of the mistake that corrupts a program's data instead
     * of quietly leaving a wrong number in a register. */
    vm86_expect_mem8("operand untouched", cpu, DATA, 0x03);
    vm86_expect_flags("flags", cpu, VM86_CF | VM86_AF | VM86_SF, 0);
}

static void test_group1_82_is_the_same_as_80(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x82, 0xC0, 0x05, 0xF4 };   /* add al,5 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0xFF10;

    vm86_test_run(cpu, 5);

    /* 82 is not in the manual. It is 80's encoding once more, some
     * assemblers emitted it, and the machine executes it as 80 -- so the
     * operation is a byte one and AH must come out untouched. */
    vm86_expect_u16("AX", cpu->ax, 0xFF15);
    vm86_expect_flags("flags", cpu, 0, 0);
}

static void test_group1_81_takes_a_word_immediate(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x81, 0xC0, 0x34, 0x12, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0001;

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AX", cpu->ax, 0x1235);
    vm86_expect_flags("flags", cpu, VM86_PF, 0);
}

static void test_group1_83_sign_extends(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x83, 0xC0, 0xFF, 0xF4 };  /* add ax,-1 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0001;

    vm86_test_run(cpu, 5);

    /* FFh is minus one, not 255. Widening it to 00FFh is what a plain cast
     * does, and it would leave AX at 0100h with no carry -- which is why a
     * negative immediate is the case that matters here. */
    vm86_expect_u16("AX", cpu->ax, 0x0000);
    vm86_expect_flags("flags", cpu,
                      VM86_CF | VM86_AF | VM86_ZF | VM86_PF, 0);
}

static void test_group1_83_immediate_overflows(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x83, 0xC0, 0xFF, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x8000;

    vm86_test_run(cpu, 5);

    /* The most negative word plus minus one: carries, and overflows, which
     * are two different things. PF is parity of the low byte, which is FF
     * even though the word's top byte is 7F. */
    vm86_expect_u16("AX", cpu->ax, 0x7FFF);
    vm86_expect_flags("flags", cpu, VM86_CF | VM86_OF | VM86_PF, 0);
}

static void test_group1_works_through_memory(struct vm86_cpu *cpu)
{
    /* add byte [0400h], 5 -- a direct address, no base register. */
    static const uint8_t code[] = { 0x80, 0x06, 0x00, 0x04, 0x05, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_mem_write8(cpu->mem, DATA, 0x10);

    vm86_test_run(cpu, 5);

    vm86_expect_mem8("stored byte", cpu, DATA, 0x15);
    vm86_expect_flags("flags", cpu, 0, 0);
}

static void test_group1_memory_displacement(struct vm86_cpu *cpu)
{
    /* add byte [bx+0400h], 5 -- the same thing with a base register, which
     * is where the immediate has to be fetched after the displacement. */
    static const uint8_t code[] = { 0x80, 0x87, 0x00, 0x04, 0x05, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->bx = 0x0010;
    vm86_mem_write8(cpu->mem, DATA + 0x10, 0x20);

    vm86_test_run(cpu, 5);

    vm86_expect_mem8("stored byte", cpu, DATA + 0x10, 0x25);
    vm86_expect_flags("flags", cpu, 0, 0);
}

static void test_group1_83_works_through_memory(struct vm86_cpu *cpu)
{
    /* add word [0400h], -1 */
    static const uint8_t code[] = { 0x83, 0x06, 0x00, 0x04, 0xFF, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_mem_write16(cpu->mem, DATA, 0x0001);

    vm86_test_run(cpu, 5);

    vm86_expect_mem16("stored word", cpu, DATA, 0x0000);
    vm86_expect_flags("flags", cpu,
                      VM86_CF | VM86_AF | VM86_ZF | VM86_PF, 0);
}

/* ------------------------------------------------------------------ */
/* 84-85 and A8-A9: TEST                                               */
/* ------------------------------------------------------------------ */

static void test_test_of_a_byte_with_itself(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x84, 0xC0, 0xF4 };   /* test al,al */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0000;

    vm86_test_run(cpu, 5);

    vm86_expect_flags("flags", cpu, VM86_ZF | VM86_PF, UNDEFINED_LOGIC);
}

static void test_test_writes_neither_operand(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x84, 0xC8, 0xF4 };   /* test al,cl */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x000F;
    cpu->cx = 0x00F0;

    vm86_test_run(cpu, 5);

    /* The AND of the two is zero, and the zero is the only thing that comes
     * of it: unlike AND, nothing is written back. */
    vm86_expect_u16("AL", cpu->al, 0x0F);
    vm86_expect_u16("CL", cpu->cl, 0xF0);
    vm86_expect_flags("flags", cpu, VM86_ZF | VM86_PF, UNDEFINED_LOGIC);
}

static void test_test_of_a_word_sets_the_sign(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x85, 0xC0, 0xF4 };   /* test ax,ax */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x8000;

    vm86_test_run(cpu, 5);

    /* SF is bit 15 here, not bit 7, and PF still looks at the low byte. */
    vm86_expect_flags("flags", cpu, VM86_SF | VM86_PF, UNDEFINED_LOGIC);
}

static void test_test_works_through_memory(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x84, 0x06, 0x00, 0x04, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0010;
    vm86_mem_write8(cpu->mem, DATA, 0x0F);

    vm86_test_run(cpu, 5);

    vm86_expect_mem8("operand untouched", cpu, DATA, 0x0F);
    vm86_expect_flags("flags", cpu, VM86_ZF | VM86_PF, UNDEFINED_LOGIC);
}

static void test_test_accumulator_immediate(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xA8, 0x0F, 0xF4 };   /* test al,0Fh */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x000F;

    vm86_test_run(cpu, 5);

    vm86_expect_flags("flags", cpu, VM86_PF, UNDEFINED_LOGIC);
}

static void test_test_accumulator_word_immediate(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xA9, 0x00, 0x80, 0xF4 };  /* ax,8000h */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x8000;

    vm86_test_run(cpu, 5);

    vm86_expect_flags("flags", cpu, VM86_SF | VM86_PF, UNDEFINED_LOGIC);
}

/* ------------------------------------------------------------------ */
/* 98-99: CBW and CWD                                                  */
/* ------------------------------------------------------------------ */

static void test_cbw_extends_a_negative_byte(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x98, 0xF4 };   /* cbw */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0080;

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AX", cpu->ax, 0xFF80);
}

static void test_cbw_extends_a_positive_byte(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x98, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x007F;

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AX", cpu->ax, 0x007F);
}

static void test_cbw_leaves_every_flag_alone(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x98, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0080;

    vm86_flag_set(cpu, VM86_CF, true);
    vm86_flag_set(cpu, VM86_OF, true);
    vm86_flag_set(cpu, VM86_AF, true);
    vm86_flag_set(cpu, VM86_ZF, true);
    vm86_flag_set(cpu, VM86_SF, true);
    vm86_flag_set(cpu, VM86_PF, true);

    vm86_test_run(cpu, 5);

    vm86_expect_flags("flags", cpu,
                      VM86_CF | VM86_OF | VM86_AF |
                      VM86_ZF | VM86_SF | VM86_PF,
                      0);
}

static void test_cwd_extends_a_negative_word(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x99, 0xF4 };   /* cwd */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x8000;
    cpu->dx = 0x1234;

    vm86_test_run(cpu, 5);

    vm86_expect_u16("DX", cpu->dx, 0xFFFF);
    vm86_expect_u16("AX untouched", cpu->ax, 0x8000);
}

static void test_cwd_extends_a_positive_word(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x99, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x7FFF;
    cpu->dx = 0xFFFF;

    vm86_test_run(cpu, 5);

    vm86_expect_u16("DX", cpu->dx, 0x0000);
}

/* ------------------------------------------------------------------ */
/* D0-D3 and C0-C1: the shifts and rotates                             */
/* ------------------------------------------------------------------ */

static void test_shl_byte_by_one(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD0, 0xE0, 0xF4 };   /* shl al,1 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0081;

    vm86_test_run(cpu, 5);

    /* The bit leaving the top lands in CF, and OF says the sign changed --
     * here it did, from negative to positive. */
    vm86_expect_u16("AL", cpu->al, 0x02);
    vm86_expect_flags("flags", cpu, VM86_CF | VM86_OF, VM86_AF);
}

static void test_shl_byte_changes_the_sign(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD0, 0xE0, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0040;

    vm86_test_run(cpu, 5);

    /* The opposite corner: nothing left the top, but the sign changed, so
     * CF is clear and OF is set. A test that only ever looked at CF could
     * not tell OF from a flag that is stuck on. */
    vm86_expect_u16("AL", cpu->al, 0x80);
    vm86_expect_flags("flags", cpu, VM86_OF | VM86_SF, VM86_AF);
}

static void test_shl_word_by_one(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD1, 0xE0, 0xF4 };   /* shl ax,1 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x8000;

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AX", cpu->ax, 0x0000);
    vm86_expect_flags("flags", cpu,
                      VM86_CF | VM86_OF | VM86_ZF | VM86_PF, VM86_AF);
}

static void test_shr_byte_by_one(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD0, 0xE8, 0xF4 };   /* shr al,1 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0081;

    vm86_test_run(cpu, 5);

    /* SHR's OF is the *original* top bit, not anything about the result: a
     * logical shift right of a negative number overflows by coming out of
     * the sign, which is the whole reason anyone tests it after one. */
    vm86_expect_u16("AL", cpu->al, 0x40);
    vm86_expect_flags("flags", cpu, VM86_CF | VM86_OF, VM86_AF);
}

static void test_shr_word_by_one(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD1, 0xE8, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0001;

    vm86_test_run(cpu, 5);

    /* One comes out of the bottom and nothing else moves, so OF is clear
     * because the original sign bit was clear. SAR would have given the
     * same answer here; the case above is the one that separates them. */
    vm86_expect_u16("AX", cpu->ax, 0x0000);
    vm86_expect_flags("flags", cpu,
                      VM86_CF | VM86_ZF | VM86_PF, VM86_AF);
}

static void test_sar_byte_by_one(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD0, 0xF8, 0xF4 };   /* sar al,1 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0081;

    vm86_test_run(cpu, 5);

    /* The sign bit is shifted back in at the top, so the value is halved
     * and rounds towards minus infinity. OF is always clear: an arithmetic
     * shift cannot overflow. */
    vm86_expect_u16("AL", cpu->al, 0xC0);
    vm86_expect_flags("flags", cpu, VM86_CF | VM86_SF | VM86_PF, VM86_AF);
}

static void test_sar_word_by_one(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD1, 0xF8, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x8001;

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AX", cpu->ax, 0xC000);
    vm86_expect_flags("flags", cpu, VM86_CF | VM86_SF | VM86_PF, VM86_AF);
}

static void test_rol_byte_by_one(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD0, 0xC0, 0xF4 };   /* rol al,1 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0081;

    vm86_test_run(cpu, 5);

    /* Nothing leaves a rotate: the bit from the top arrives at the bottom
     * and CF gets a copy of it. OF compares the new top bit with CF. */
    vm86_expect_u16("AL", cpu->al, 0x03);
    vm86_expect_flags("flags", cpu, VM86_CF | VM86_OF | VM86_PF, VM86_AF);
}

static void test_rol_word_by_one(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD1, 0xC0, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x4000;

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AX", cpu->ax, 0x8000);
    vm86_expect_flags("flags", cpu, VM86_OF | VM86_SF | VM86_PF, VM86_AF);
}

static void test_ror_byte_by_one(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD0, 0xC8, 0xF4 };   /* ror al,1 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0001;

    vm86_test_run(cpu, 5);

    /* ROR's OF is about the result's two top bits, not about CF. Here they
     * are 1 and 0, so it is set. */
    vm86_expect_u16("AL", cpu->al, 0x80);
    vm86_expect_flags("flags", cpu, VM86_CF | VM86_OF | VM86_SF, VM86_AF);
}

static void test_ror_word_by_one(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD1, 0xC8, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0001;

    vm86_test_run(cpu, 5);

    /* Bit 14 is what came down from the top; with bit 15 set they disagree
     * and OF comes on. */
    vm86_expect_u16("AX", cpu->ax, 0x8000);
    vm86_expect_flags("flags", cpu,
                      VM86_CF | VM86_OF | VM86_SF | VM86_PF, VM86_AF);
}

static void test_rcl_byte_by_one(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD0, 0xD0, 0xF4 };   /* rcl al,1 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0080;   /* CF is clear, and arrives in bit 0 as a zero */

    vm86_test_run(cpu, 5);

    /* Through the carry: the bit from the top becomes CF and the old CF
     * moves into the bottom. */
    vm86_expect_u16("AL", cpu->al, 0x00);
    vm86_expect_flags("flags", cpu,
                      VM86_CF | VM86_OF | VM86_ZF | VM86_PF, VM86_AF);
}

static void test_rcl_brings_the_carry_in(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD0, 0xD0, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0000;
    vm86_flag_set(cpu, VM86_CF, true);

    vm86_test_run(cpu, 5);

    /* The other direction: the old carry arrives at the bottom, and the new
     * one is what left the top, which was nothing. */
    vm86_expect_u16("AL", cpu->al, 0x01);
    vm86_expect_flags("flags", cpu, 0, VM86_AF);
}

static void test_rcr_byte_by_one(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD0, 0xD8, 0xF4 };   /* rcr al,1 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0001;

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AL", cpu->al, 0x00);
    vm86_expect_flags("flags", cpu,
                      VM86_CF | VM86_ZF | VM86_PF, VM86_AF);
}

static void test_rcr_brings_the_carry_in(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD0, 0xD8, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0001;
    vm86_flag_set(cpu, VM86_CF, true);

    vm86_test_run(cpu, 5);

    /* The old carry arrives at the top, so the result is negative -- and
     * its two top bits are 1 and 0, so OF comes on. */
    vm86_expect_u16("AL", cpu->al, 0x80);
    vm86_expect_flags("flags", cpu, VM86_CF | VM86_OF | VM86_SF, VM86_AF);
}

static void test_shift_by_cl(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD2, 0xE0, 0xF4 };   /* shl al,cl */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x000F;
    cpu->cx = 0x0005;   /* CL = 5 */

    vm86_test_run(cpu, 5);

    /* OF is defined only for a count of one, so this case ignores it: the
     * manual does not promise a value and neither does this code. */
    vm86_expect_u16("AL", cpu->al, 0xE0);
    vm86_expect_flags("flags", cpu, VM86_CF | VM86_SF, VM86_AF | VM86_OF);
}

static void test_shift_by_an_immediate(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xC0, 0xE0, 0x04, 0xF4 };   /* shl al,4 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x001F;

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AL", cpu->al, 0xF0);
    vm86_expect_flags("flags", cpu,
                      VM86_CF | VM86_SF | VM86_PF, VM86_AF | VM86_OF);
}

static void test_shift_word_by_an_immediate(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xC1, 0xE8, 0x04, 0xF4 };   /* shr ax,4 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x000F;

    vm86_test_run(cpu, 5);

    /* Fifteen shifted right four times is zero, and the last bit to leave
     * is the one that set CF on the way past. */
    vm86_expect_u16("AX", cpu->ax, 0x0000);
    vm86_expect_flags("flags", cpu,
                      VM86_CF | VM86_ZF | VM86_PF, VM86_AF | VM86_OF);
}

/*
 * Run one shift whose count is zero and report anything that moved.
 *
 * A count of zero is where the instruction has to do nothing at all -- not
 * the operand, and not one flag -- and it is the easy thing to get wrong: a
 * shift that computes CF from a bit it never shifted out sets CF here, and
 * the program that follows takes the wrong branch several thousand
 * instructions later.
 */
static void expect_zero_count_to_do_nothing(struct vm86_cpu *cpu,
                                            uint8_t opcode, uint8_t modrm,
                                            bool has_immediate,
                                            uint16_t before, const char *what)
{
    uint8_t  code[4];
    uint16_t size = 0;

    code[size++] = opcode;
    code[size++] = modrm;
    if (has_immediate)
        code[size++] = 0x00;    /* the count */
    code[size++] = 0xF4;        /* hlt */

    vm86_test_load(cpu, code, size);

    cpu->ax = before;
    cpu->cx = 0;                /* for the forms that take the count in CL */

    /* Every flag set, so that "nothing moved" is a claim with something
     * behind it rather than an accident of everything already being zero. */
    vm86_flag_set(cpu, VM86_CF, true);
    vm86_flag_set(cpu, VM86_OF, true);
    vm86_flag_set(cpu, VM86_AF, true);
    vm86_flag_set(cpu, VM86_ZF, true);
    vm86_flag_set(cpu, VM86_SF, true);
    vm86_flag_set(cpu, VM86_PF, true);

    vm86_test_run(cpu, 5);

    vm86_expect_u16(what, cpu->ax, before);
    vm86_expect_flags(what, cpu,
                      VM86_CF | VM86_OF | VM86_AF |
                      VM86_ZF | VM86_SF | VM86_PF,
                      0);
}

static void test_shifts_by_zero_do_nothing(struct vm86_cpu *cpu)
{
    static const char *const by_cl[8] = {
        "rol al,cl with CL=0", "ror al,cl with CL=0",
        "rcl al,cl with CL=0", "rcr al,cl with CL=0",
        "shl al,cl with CL=0", "shr al,cl with CL=0",
        "sal al,cl with CL=0", "sar al,cl with CL=0",
    };
    static const char *const by_immediate[8] = {
        "rol ax,0", "ror ax,0", "rcl ax,0", "rcr ax,0",
        "shl ax,0", "shr ax,0", "sal ax,0", "sar ax,0",
    };

    /* All eight operations, in both widths and from both sources of the
     * count, because they are eight separate pieces of code and only one of
     * them has to get this wrong. */
    for (int i = 0; i < 8; i++) {
        expect_zero_count_to_do_nothing(cpu, 0xD2,
                                        (uint8_t)(0xC0 | (i << 3)), false,
                                        0x0081, by_cl[i]);
        expect_zero_count_to_do_nothing(cpu, 0xC1,
                                        (uint8_t)(0xC0 | (i << 3)), true,
                                        0x8001, by_immediate[i]);
    }
}

static void test_shift_counts_are_not_masked(struct vm86_cpu *cpu)
{
    static const uint8_t by_cl[] = { 0xD3, 0xE0, 0xF4 };   /* shl ax,cl */
    static const uint8_t by_imm[] = { 0xC1, 0xE0, 0x21, 0xF4 }; /* ax,33 */

    vm86_test_load(cpu, by_cl, sizeof(by_cl));
    cpu->ax = 0x0001;
    cpu->cx = 0x0021;   /* CL = 33, which is over the width */
    vm86_test_run(cpu, 5);

    /* The 8086 shifts by the whole of CL, so the one bit goes out of the
     * top and is followed by thirty-two shifts of nothing: AX is zero and
     * CF is clear. A 186 masks the count to five bits, shifts once, and
     * leaves 0002 in AX -- so this case is the one that says which machine
     * this is. See the note on the count in ops_alu.c. */
    vm86_expect_u16("shl ax,cl by 33", cpu->ax, 0x0000);
    vm86_expect_flag("shl ax,cl by 33", cpu, VM86_CF, false);

    vm86_test_load(cpu, by_imm, sizeof(by_imm));
    cpu->ax = 0x0001;
    vm86_test_run(cpu, 5);

    /* The 186's own encoding, and the same rule: C0/C1 are this machine's
     * extra encodings, not another machine's, and a count that means one
     * thing in CL and another as an immediate would be worse than being
     * wrong about the 186. */
    vm86_expect_u16("shl ax,33", cpu->ax, 0x0000);
    vm86_expect_flag("shl ax,33", cpu, VM86_CF, false);
}

static void test_shift_reaches_memory(struct vm86_cpu *cpu)
{
    /* shl byte [0400h], 1 */
    static const uint8_t code[] = { 0xD0, 0x26, 0x00, 0x04, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_mem_write8(cpu->mem, DATA, 0x81);

    vm86_test_run(cpu, 5);

    vm86_expect_mem8("stored byte", cpu, DATA, 0x02);
    vm86_expect_flags("flags", cpu, VM86_CF | VM86_OF, VM86_AF);
}

static void test_shift_word_in_memory_by_cl(struct vm86_cpu *cpu)
{
    /* shr word [0400h], cl */
    static const uint8_t code[] = { 0xD3, 0x2E, 0x00, 0x04, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_mem_write16(cpu->mem, DATA, 0x0001);
    cpu->cx = 0x0001;

    vm86_test_run(cpu, 5);

    vm86_expect_mem16("stored word", cpu, DATA, 0x0000);
    vm86_expect_flags("flags", cpu,
                      VM86_CF | VM86_ZF | VM86_PF, VM86_AF);
}

/* ------------------------------------------------------------------ */
/* D4-D5: AAM and AAD                                                  */
/* ------------------------------------------------------------------ */

static void test_aam_divides_by_ten(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD4, 0x0A, 0xF4 };   /* aam */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x001F;   /* 31 */

    vm86_test_run(cpu, 5);

    /* The tens go to AH and the units stay: 31 becomes 0301h. */
    vm86_expect_u16("AX", cpu->ax, 0x0301);
    vm86_expect_flags("flags", cpu, 0, UNDEFINED_DECIMAL);
}

static void test_aam_of_zero(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD4, 0x0A, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x5500;   /* AH is an input of AAD, not of AAM */

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AX", cpu->ax, 0x0000);
    vm86_expect_flags("flags", cpu, VM86_ZF | VM86_PF, UNDEFINED_DECIMAL);
}

static void test_aam_of_ninety_nine(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD4, 0x0A, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0063;   /* 99 */

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AX", cpu->ax, 0x0909);
    vm86_expect_flags("flags", cpu, VM86_PF, UNDEFINED_DECIMAL);
}

static void test_aam_reads_the_base(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD4, 0x10, 0xF4 };   /* aam 16 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x00FF;

    vm86_test_run(cpu, 5);

    /* The manual says only ten is defined and every real use is ten, but
     * the immediate is there and a byte of it is read as the base rather
     * than assumed. */
    vm86_expect_u16("AX", cpu->ax, 0x0F0F);
    vm86_expect_flags("flags", cpu, VM86_PF, UNDEFINED_DECIMAL);
}

static void test_aam_by_zero(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD4, 0x00, 0xF4 };   /* aam 0 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0001;

    enum vm86_result result = vm86_test_run(cpu, 5);

    /* Undefined on the hardware, and on a host it would be a division by
     * zero inside the emulator -- a crash rather than a wrong answer. It is
     * reported as the guest's own divide error so that whatever M4 does
     * with a real divide error happens to this one too. */
    vm86_expect_bool("faulted", result == VM86_FAULT, true);
    vm86_expect_u16("vector", cpu->fault, VM86_VECTOR_DIVIDE_ERROR);
}

static void test_aad_multiplies_the_high_digit(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD5, 0x0A, 0xF4 };   /* aad */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0301;   /* AH = 3, AL = 1 */

    vm86_test_run(cpu, 5);

    /* 3 x 10 + 1 is 31, and the high half is cleared afterwards. */
    vm86_expect_u16("AX", cpu->ax, 0x001F);
    vm86_expect_flags("flags", cpu, 0, UNDEFINED_DECIMAL);
}

static void test_aad_of_two_nines(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD5, 0x0A, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0909;

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AX", cpu->ax, 0x0063);
    vm86_expect_flags("flags", cpu, VM86_PF, UNDEFINED_DECIMAL);
}

static void test_aad_works_in_hexadecimal(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD5, 0x10, 0xF4 };   /* aad 16 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0F0F;

    vm86_test_run(cpu, 5);

    /* 0Fh x 16 + 0Fh is 255, which is where the byte's range ends -- so
     * this is also the boundary of what the instruction can produce. */
    vm86_expect_u16("AX", cpu->ax, 0x00FF);
    vm86_expect_flags("flags", cpu, VM86_SF | VM86_PF, UNDEFINED_DECIMAL);
}

static void test_aam_and_aad_are_inverses(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD4, 0x0A,   /* aam */
                                    0xD5, 0x0A,   /* aad */
                                    0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0057;   /* 87 */

    vm86_test_run(cpu, 10);

    /* AAM unpacks and AAD packs; a program that prints a number uses them
     * in this order, so the pair has to come back where it started. */
    vm86_expect_u16("AX", cpu->ax, 0x0057);
}

/* ------------------------------------------------------------------ */
/* F6-F7: the group 3 block                                            */
/* ------------------------------------------------------------------ */

static void test_group3_test_byte_immediate(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF6, 0xC0, 0x0F, 0xF4 };  /* al,0Fh */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x000F;

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AL untouched", cpu->al, 0x0F);
    vm86_expect_flags("flags", cpu, VM86_PF, UNDEFINED_LOGIC);
}

static void test_group3_test_encoding_one(struct vm86_cpu *cpu)
{
    /* The manual documents /0 only. The machine executes /1 as the same
     * instruction, and there is no reason to be stricter than the hardware
     * about an encoding that costs one line to accept. */
    static const uint8_t code[] = { 0xF6, 0xC8, 0x0F, 0xF4 };  /* al,0Fh */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x00F0;

    vm86_test_run(cpu, 5);

    vm86_expect_flags("flags", cpu, VM86_ZF | VM86_PF, UNDEFINED_LOGIC);
}

static void test_group3_test_word_immediate(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF7, 0xC0, 0x00, 0x80, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x8000;

    vm86_test_run(cpu, 5);

    vm86_expect_flags("flags", cpu, VM86_SF | VM86_PF, UNDEFINED_LOGIC);
}

static void test_group3_not_moves_no_flag_at_all(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF6, 0xD0, 0xF4 };   /* not al */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x000F;

    vm86_flag_set(cpu, VM86_CF, true);
    vm86_flag_set(cpu, VM86_OF, true);
    vm86_flag_set(cpu, VM86_AF, true);
    vm86_flag_set(cpu, VM86_ZF, true);
    vm86_flag_set(cpu, VM86_SF, true);
    vm86_flag_set(cpu, VM86_PF, true);

    vm86_test_run(cpu, 5);

    /* NEG is next to this one in the encoding and writes every flag in
     * sight. NOT writes none of them, which is the whole difference and is
     * worth a case that would fail if the two were ever confused. */
    vm86_expect_u16("AL", cpu->al, 0xF0);
    vm86_expect_flags("flags", cpu,
                      VM86_CF | VM86_OF | VM86_AF |
                      VM86_ZF | VM86_SF | VM86_PF,
                      0);
}

static void test_group3_not_word(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF7, 0xD0, 0xF4 };   /* not ax */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x00FF;
    vm86_flag_set(cpu, VM86_CF, true);

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AX", cpu->ax, 0xFF00);
    vm86_expect_flags("flags", cpu, VM86_CF, 0);
}

static void test_group3_not_through_memory(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF6, 0x16, 0x00, 0x04, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_mem_write8(cpu->mem, DATA, 0x0F);

    vm86_test_run(cpu, 5);

    vm86_expect_mem8("stored byte", cpu, DATA, 0xF0);
}

static void test_group3_neg_is_zero_minus(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF6, 0xD8, 0xF4 };   /* neg al */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0001;

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AL", cpu->al, 0xFF);
    vm86_expect_flags("flags", cpu,
                      VM86_CF | VM86_AF | VM86_SF | VM86_PF, 0);
}

static void test_group3_neg_of_zero(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF6, 0xD8, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0000;
    vm86_flag_set(cpu, VM86_CF, true);   /* to prove it comes back off */

    vm86_test_run(cpu, 5);

    /* CF after NEG is not "the operand was added to something", it is
     * "the subtraction borrowed", and zero minus zero does not. Getting
     * this backwards is invisible until a program negates a zero. */
    vm86_expect_u16("AL", cpu->al, 0x00);
    vm86_expect_flags("flags", cpu, VM86_ZF | VM86_PF, 0);
}

static void test_group3_neg_most_negative_byte(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF6, 0xD8, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0080;

    vm86_test_run(cpu, 5);

    /* 80h is its own negation, and that is the one value the byte cannot
     * hold as a positive number. */
    vm86_expect_u16("AL", cpu->al, 0x80);
    vm86_expect_flags("flags", cpu, VM86_CF | VM86_OF | VM86_SF, 0);
}

static void test_group3_neg_word(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF7, 0xD8, 0xF4 };   /* neg ax */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x8000;

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AX", cpu->ax, 0x8000);
    vm86_expect_flags("flags", cpu,
                      VM86_CF | VM86_OF | VM86_SF | VM86_PF, 0);
}

static void test_group3_mul_byte_high_half(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF6, 0xE3, 0xF4 };   /* mul bl */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0010;
    cpu->bx = 0x0020;

    vm86_test_run(cpu, 5);

    /* The product goes to AX whatever the operands, and CF and OF say
     * whether the part that did not fit is in AH. */
    vm86_expect_u16("AX", cpu->ax, 0x0200);
    vm86_expect_flags("flags", cpu, VM86_CF | VM86_OF, UNDEFINED_AFTER_MUL);
}

static void test_group3_mul_byte_that_fits(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF6, 0xE3, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0010;
    cpu->bx = 0x0001;

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AX", cpu->ax, 0x0010);
    vm86_expect_flags("flags", cpu, 0, UNDEFINED_AFTER_MUL);
}

static void test_group3_mul_word_fills_dx(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF7, 0xE3, 0xF4 };   /* mul bx */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x1000;
    cpu->bx = 0x1000;

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AX", cpu->ax, 0x0000);
    vm86_expect_u16("DX", cpu->dx, 0x0100);
    vm86_expect_flags("flags", cpu, VM86_CF | VM86_OF, UNDEFINED_AFTER_MUL);
}

static void test_group3_mul_word_that_fits(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF7, 0xE3, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x00FF;
    cpu->bx = 0x0002;

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AX", cpu->ax, 0x01FE);
    vm86_expect_u16("DX", cpu->dx, 0x0000);
    vm86_expect_flags("flags", cpu, 0, UNDEFINED_AFTER_MUL);
}

static void test_group3_imul_byte_sign_extends(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF6, 0xEB, 0xF4 };   /* imul bl */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x00FE;   /* -2 */
    cpu->bx = 0x0003;

    vm86_test_run(cpu, 5);

    /* Minus six fits in a byte, so the high half is its sign extension and
     * CF stays clear. A version that treated the operands as unsigned
     * would give FFFAh here too -- which is why the next case exists. */
    vm86_expect_u16("AX", cpu->ax, 0xFFFA);
    vm86_expect_flags("flags", cpu, 0, UNDEFINED_AFTER_MUL);
}

static void test_group3_imul_byte_that_does_not_fit(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF6, 0xEB, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0080;   /* -128 */
    cpu->bx = 0x0002;

    vm86_test_run(cpu, 5);

    /* Minus 256 does not fit in a signed byte, so the product is not the
     * sign extension of its low half and the flags say so. */
    vm86_expect_u16("AX", cpu->ax, 0xFF00);
    vm86_expect_flags("flags", cpu, VM86_CF | VM86_OF, UNDEFINED_AFTER_MUL);
}

static void test_group3_imul_word_negatives(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF7, 0xEB, 0xF4 };   /* imul bx */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0xFFFF;   /* -1 */
    cpu->bx = 0xFFFF;   /* -1 */

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AX", cpu->ax, 0x0001);
    vm86_expect_u16("DX", cpu->dx, 0x0000);
    vm86_expect_flags("flags", cpu, 0, UNDEFINED_AFTER_MUL);
}

static void test_group3_imul_word_that_does_not_fit(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF7, 0xEB, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x4000;
    cpu->bx = 0x0004;

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AX", cpu->ax, 0x0000);
    vm86_expect_u16("DX", cpu->dx, 0x0001);
    vm86_expect_flags("flags", cpu, VM86_CF | VM86_OF, UNDEFINED_AFTER_MUL);
}

static void test_group3_div_byte(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF6, 0xF3, 0xF4 };   /* div bl */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x001E;   /* 30 */
    cpu->bx = 0x0007;

    vm86_test_run(cpu, 5);

    /* The whole of AX is the dividend even though the operands are bytes,
     * and the remainder lands in AH. Every flag is undefined afterwards. */
    vm86_expect_u16("AX", cpu->ax, 0x0204);
    vm86_expect_flags("flags", cpu, 0, UNDEFINED_MULDIV);
}

static void test_group3_div_byte_overflows(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF6, 0xF3, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0200;   /* 512 over a divisor of one */
    cpu->bx = 0x0001;

    enum vm86_result result = vm86_test_run(cpu, 5);

    /* Not a truncated quotient: a fault. Real programs divide by a value
     * they are not sure of and use this to find out, so giving them a
     * plausible wrong number instead is the one answer that breaks them. */
    vm86_expect_bool("faulted", result == VM86_FAULT, true);
    vm86_expect_u16("vector", cpu->fault, VM86_VECTOR_DIVIDE_ERROR);
}

static void test_group3_div_by_zero_faults(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF6, 0xF3, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0001;
    cpu->bx = 0x0000;

    enum vm86_result result = vm86_test_run(cpu, 5);

    vm86_expect_bool("faulted", result == VM86_FAULT, true);
    vm86_expect_u16("vector", cpu->fault, VM86_VECTOR_DIVIDE_ERROR);
}

static void test_group3_div_word(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF7, 0xF3, 0xF4 };   /* div bx */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->dx = 0x0001;
    cpu->ax = 0x0001;   /* DX:AX = 00010001h = 65537 */
    cpu->bx = 0x0002;

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AX", cpu->ax, 0x8000);
    vm86_expect_u16("DX", cpu->dx, 0x0001);
}

static void test_group3_div_word_overflows(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF7, 0xF3, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->dx = 0x0001;
    cpu->ax = 0x0000;   /* 65536, and the divisor is one */
    cpu->bx = 0x0001;

    enum vm86_result result = vm86_test_run(cpu, 5);

    vm86_expect_bool("faulted", result == VM86_FAULT, true);
    vm86_expect_u16("vector", cpu->fault, VM86_VECTOR_DIVIDE_ERROR);
}

static void test_group3_idiv_byte_keeps_the_signs(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF6, 0xFB, 0xF4 };   /* idiv bl */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0xFFF9;   /* -7 */
    cpu->bx = 0x0002;

    vm86_test_run(cpu, 5);

    /* -7 over 2 is -3, and the remainder takes the sign of the dividend:
     * -1, not +1. */
    vm86_expect_u16("AX", cpu->ax, 0xFFFD);
    vm86_expect_flags("flags", cpu, 0, UNDEFINED_MULDIV);
}

static void test_group3_idiv_byte_overflows(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF6, 0xFB, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0080;   /* 128, which is one past the signed byte's top */
    cpu->bx = 0x0001;

    enum vm86_result result = vm86_test_run(cpu, 5);

    /* 128 fits in AX but not in AL, and it is AL the quotient has to fit
     * in -- so the limit is signed here where DIV's is not. */
    vm86_expect_bool("faulted", result == VM86_FAULT, true);
    vm86_expect_u16("vector", cpu->fault, VM86_VECTOR_DIVIDE_ERROR);
}

static void test_group3_idiv_word(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF7, 0xFB, 0xF4 };   /* idiv bx */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->dx = 0xFFFF;
    cpu->ax = 0xFFF9;   /* DX:AX = -7 */
    cpu->bx = 0x0002;

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AX", cpu->ax, 0xFFFD);
    vm86_expect_u16("DX", cpu->dx, 0xFFFF);
}

static void test_group3_idiv_most_negative(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF7, 0xFB, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->dx = 0x8000;
    cpu->ax = 0x0000;   /* the most negative double word */
    cpu->bx = 0xFFFF;   /* -1 */

    enum vm86_result result = vm86_test_run(cpu, 5);

    /* The quotient is 2^31, which fits in neither AX nor a signed int32 --
     * the emulator included. Doing the division at 64 bits is what keeps
     * this a fault in the guest rather than a trap in the host. */
    vm86_expect_bool("faulted", result == VM86_FAULT, true);
    vm86_expect_u16("vector", cpu->fault, VM86_VECTOR_DIVIDE_ERROR);
}

static void test_group3_works_through_memory(struct vm86_cpu *cpu)
{
    /* div byte [0400h] */
    static const uint8_t code[] = { 0xF6, 0x36, 0x00, 0x04, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0064;   /* 100 */
    vm86_mem_write8(cpu->mem, DATA, 0x0A);

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AX", cpu->ax, 0x000A);
}

/* ------------------------------------------------------------------ */
/* FE: increment and decrement a byte in memory                        */
/* ------------------------------------------------------------------ */

static void test_fe_increments_a_byte_in_memory(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xFE, 0x06, 0x00, 0x04, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_mem_write8(cpu->mem, DATA, 0xFF);
    vm86_flag_set(cpu, VM86_CF, true);   /* INC must leave this alone */

    vm86_test_run(cpu, 5);

    vm86_expect_mem8("stored byte", cpu, DATA, 0x00);
    vm86_expect_flags("flags", cpu,
                      VM86_CF | VM86_AF | VM86_ZF | VM86_PF, 0);
}

static void test_fe_decrements_a_byte_in_memory(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xFE, 0x0E, 0x00, 0x04, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_mem_write8(cpu->mem, DATA, 0x01);
    vm86_flag_set(cpu, VM86_CF, true);

    vm86_test_run(cpu, 5);

    vm86_expect_mem8("stored byte", cpu, DATA, 0x00);
    vm86_expect_flags("flags", cpu, VM86_CF | VM86_ZF | VM86_PF, 0);
}

static void test_fe_increment_wraps(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xFE, 0x06, 0x00, 0x04, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_mem_write8(cpu->mem, DATA, 0x7F);

    vm86_test_run(cpu, 5);

    /* The byte form of the same group FF is the word form of, so its
     * overflow rule is the same: the largest positive goes to the smallest
     * negative. */
    vm86_expect_mem8("stored byte", cpu, DATA, 0x80);
    vm86_expect_flags("flags", cpu, VM86_OF | VM86_AF | VM86_SF, 0);
}

static void test_fe_decrement_wraps(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xFE, 0x0E, 0x00, 0x04, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_mem_write8(cpu->mem, DATA, 0x80);

    vm86_test_run(cpu, 5);

    vm86_expect_mem8("stored byte", cpu, DATA, 0x7F);
    vm86_expect_flags("flags", cpu, VM86_OF | VM86_AF, 0);
}

static void test_fe_refuses_unknown_opcode(struct vm86_cpu *cpu)
{
    /* FE /2: the group has eight slots and only two of them exist. */
    static const uint8_t code[] = { 0xFE, 0x16, 0x00, 0x04 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_mem_write8(cpu->mem, DATA, 0x55);

    enum vm86_result result = vm86_test_run(cpu, 5);

    /* The invalid-opcode exception, not a silent increment: an encoding
     * this processor does not have is how a program detects the machine it
     * is on, and answering it with something plausible is the one response
     * that misleads. */
    vm86_expect_bool("faulted", result == VM86_FAULT, true);
    vm86_expect_u16("vector", cpu->fault, VM86_VECTOR_INVALID_OPCODE);
    vm86_expect_mem8("memory untouched", cpu, DATA, 0x55);
}

/* ------------------------------------------------------------------ */
static void test_fe_increments_al(struct vm86_cpu *cpu)
{
    /* inc al -- FE C0. The one-byte forms at 40-47 are inc and dec for
     * the sixteen-bit registers only; a byte register has no short form,
     * so this is how an assembler writes every byte-register increment
     * there is. */
    static const uint8_t code[] = { 0xFE, 0xC0, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x007F;
    vm86_flag_set(cpu, VM86_CF, true);   /* to prove INC leaves it alone */

    vm86_test_run(cpu, 5);

    vm86_expect_u16("AL", cpu->al, 0x80);
    vm86_expect_flags("flags", cpu,
                      VM86_CF | VM86_OF | VM86_AF | VM86_SF, 0);
}

static void test_fe_increments_bl(struct vm86_cpu *cpu)
{
    /* inc bl -- FE C3, a byte register that is not the accumulator. */
    static const uint8_t code[] = { 0xFE, 0xC3, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->bx = 0x00FF;
    vm86_flag_set(cpu, VM86_CF, true);

    vm86_test_run(cpu, 5);

    vm86_expect_u16("BX", cpu->bx, 0x0000);
    vm86_expect_flags("flags", cpu,
                      VM86_CF | VM86_AF | VM86_ZF | VM86_PF, 0);
}

static void test_fe_decrements_cl(struct vm86_cpu *cpu)
{
    /* dec cl -- FE C9, the decrement half of the group. */
    static const uint8_t code[] = { 0xFE, 0xC9, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->cx = 0x0001;
    vm86_flag_set(cpu, VM86_CF, true);

    vm86_test_run(cpu, 5);

    vm86_expect_u16("CX", cpu->cx, 0x0000);
    vm86_expect_flags("flags", cpu, VM86_CF | VM86_ZF | VM86_PF, 0);
}

static void test_fe_reaches_the_high_bytes(struct vm86_cpu *cpu)
{
    /* inc dh -- FE C6. The high bytes are where the 8-bit register order
     * stops agreeing with the 16-bit one, so this is the case that would
     * catch a handler indexing the register file by hand. */
    static const uint8_t code[] = { 0xFE, 0xC6, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->dx = 0x7F00;

    vm86_test_run(cpu, 5);

    vm86_expect_u16("DX", cpu->dx, 0x8000);
    vm86_expect_flags("flags", cpu, VM86_OF | VM86_AF | VM86_SF, 0);
}

/* ------------------------------------------------------------------ */

static const struct vm86_test tests[] = {
    /* 27 DAA */
    { "daa leaves a valid digit",     test_daa_leaves_a_valid_digit_alone },
    { "daa corrects the low digit",   test_daa_corrects_the_low_digit },
    { "daa corrects the high digit",  test_daa_corrects_the_high_digit },
    { "daa corrects both digits",     test_daa_corrects_both_digits },
    { "daa keeps an existing carry",  test_daa_keeps_an_existing_carry },

    /* 2F DAS */
    { "das leaves a valid digit",     test_das_leaves_a_valid_digit_alone },
    { "das corrects the low digit",   test_das_corrects_the_low_digit },
    { "das corrects the high digit",  test_das_corrects_the_high_digit },
    { "das uses the corrected value", test_das_high_digit_after_correction },

    /* 37 AAA */
    { "aaa leaves a valid digit",     test_aaa_leaves_a_valid_digit_alone },
    { "aaa clears the high nibble",   test_aaa_clears_the_high_nibble },
    { "aaa corrects on AF alone",     test_aaa_corrects_on_af_alone },
    { "aaa writes only CF and AF",    test_aaa_writes_only_cf_and_af },

    /* 3F AAS */
    { "aas leaves a valid digit",     test_aas_leaves_a_valid_digit_alone },
    { "aas corrects and borrows",     test_aas_corrects_and_borrows_from_ah },
    { "aas clears the high nibble",   test_aas_clears_the_high_nibble },

    /* 80-83, the group 1 immediate forms */
    { "80 add al, immediate",         test_group1_add_byte_immediate },
    { "80 add carries out",           test_group1_add_carries_out },
    { "80 or",                        test_group1_or },
    { "80 adc reads the carry",       test_group1_adc_reads_the_carry },
    { "80 sbb borrows the carry",     test_group1_sbb_borrows_the_carry },
    { "80 and clears CF and OF",      test_group1_and_clears_cf_and_of },
    { "80 sub borrows",               test_group1_sub_borrows },
    { "80 xor",                       test_group1_xor },
    { "80 cmp does not store",        test_group1_cmp_does_not_store },
    { "80 cmp through memory",        test_group1_cmp_through_memory },
    { "82 is the same as 80",         test_group1_82_is_the_same_as_80 },
    { "81 takes a word immediate",    test_group1_81_takes_a_word_immediate },
    { "83 sign extends",              test_group1_83_sign_extends },
    { "83 immediate overflows",       test_group1_83_immediate_overflows },
    { "80 works through memory",      test_group1_works_through_memory },
    { "80 through a displacement",    test_group1_memory_displacement },
    { "83 works through memory",      test_group1_83_works_through_memory },

    /* 84-85 and A8-A9, TEST */
    { "test al, al",                  test_test_of_a_byte_with_itself },
    { "test writes neither operand",  test_test_writes_neither_operand },
    { "test ax, ax sets the sign",    test_test_of_a_word_sets_the_sign },
    { "test through memory",          test_test_works_through_memory },
    { "test al, immediate",           test_test_accumulator_immediate },
    { "test ax, immediate",           test_test_accumulator_word_immediate },

    /* 98-99, CBW and CWD */
    { "cbw extends a negative byte",  test_cbw_extends_a_negative_byte },
    { "cbw extends a positive byte",  test_cbw_extends_a_positive_byte },
    { "cbw leaves every flag alone",  test_cbw_leaves_every_flag_alone },
    { "cwd extends a negative word",  test_cwd_extends_a_negative_word },
    { "cwd extends a positive word",  test_cwd_extends_a_positive_word },

    /* D0-D3 and C0-C1, the shifts and rotates */
    { "shl byte by one",              test_shl_byte_by_one },
    { "shl changes the sign",         test_shl_byte_changes_the_sign },
    { "shl word by one",              test_shl_word_by_one },
    { "shr byte by one",              test_shr_byte_by_one },
    { "shr word by one",              test_shr_word_by_one },
    { "sar byte by one",              test_sar_byte_by_one },
    { "sar word by one",              test_sar_word_by_one },
    { "rol byte by one",              test_rol_byte_by_one },
    { "rol word by one",              test_rol_word_by_one },
    { "ror byte by one",              test_ror_byte_by_one },
    { "ror word by one",              test_ror_word_by_one },
    { "rcl byte by one",              test_rcl_byte_by_one },
    { "rcl brings the carry in",      test_rcl_brings_the_carry_in },
    { "rcr byte by one",              test_rcr_byte_by_one },
    { "rcr brings the carry in",      test_rcr_brings_the_carry_in },
    { "shl al, cl",                   test_shift_by_cl },
    { "shl al, immediate",            test_shift_by_an_immediate },
    { "shr ax, immediate",            test_shift_word_by_an_immediate },
    { "shifts by zero do nothing",    test_shifts_by_zero_do_nothing },
    { "shift counts are not masked",  test_shift_counts_are_not_masked },
    { "shl memory byte",              test_shift_reaches_memory },
    { "shr memory word by CL",        test_shift_word_in_memory_by_cl },

    /* D4-D5, AAM and AAD */
    { "aam divides by ten",           test_aam_divides_by_ten },
    { "aam of zero",                  test_aam_of_zero },
    { "aam of ninety nine",           test_aam_of_ninety_nine },
    { "aam reads the base",           test_aam_reads_the_base },
    { "aam 0 divides by zero",        test_aam_by_zero },
    { "aad multiplies the high digit", test_aad_multiplies_the_high_digit },
    { "aad of two nines",             test_aad_of_two_nines },
    { "aad in hexadecimal",           test_aad_works_in_hexadecimal },
    { "aam and aad are inverses",     test_aam_and_aad_are_inverses },

    /* F6-F7, the group 3 block */
    { "f6 test byte, immediate",      test_group3_test_byte_immediate },
    { "f6 /1 is test as well",        test_group3_test_encoding_one },
    { "f7 test word, immediate",      test_group3_test_word_immediate },
    { "not moves no flag",            test_group3_not_moves_no_flag_at_all },
    { "not word",                     test_group3_not_word },
    { "not through memory",           test_group3_not_through_memory },
    { "neg al is zero minus al",      test_group3_neg_is_zero_minus },
    { "neg of zero clears CF",        test_group3_neg_of_zero },
    { "neg of the most negative",     test_group3_neg_most_negative_byte },
    { "neg word",                     test_group3_neg_word },
    { "mul byte carries out",         test_group3_mul_byte_high_half },
    { "mul byte that fits",           test_group3_mul_byte_that_fits },
    { "mul word fills DX",            test_group3_mul_word_fills_dx },
    { "mul word that fits",           test_group3_mul_word_that_fits },
    { "imul byte sign extends",       test_group3_imul_byte_sign_extends },
    { "imul byte does not fit",  test_group3_imul_byte_that_does_not_fit },
    { "imul word of negatives",       test_group3_imul_word_negatives },
    { "imul word does not fit",  test_group3_imul_word_that_does_not_fit },
    { "div byte",                     test_group3_div_byte },
    { "div byte overflows",           test_group3_div_byte_overflows },
    { "div by zero faults",           test_group3_div_by_zero_faults },
    { "div word",                     test_group3_div_word },
    { "div word overflows",           test_group3_div_word_overflows },
    { "idiv byte keeps signs",        test_group3_idiv_byte_keeps_the_signs },
    { "idiv byte overflows",          test_group3_idiv_byte_overflows },
    { "idiv word",                    test_group3_idiv_word },
    { "idiv most negative by -1",     test_group3_idiv_most_negative },
    { "div through memory",           test_group3_works_through_memory },

    /* FE */
    { "fe increments memory",         test_fe_increments_a_byte_in_memory },
    { "fe decrements memory",         test_fe_decrements_a_byte_in_memory },
    { "fe increment wraps",           test_fe_increment_wraps },
    { "fe decrement wraps",           test_fe_decrement_wraps },
    { "fe refuses other sub-opcodes", test_fe_refuses_unknown_opcode },
    { "fe increments al",             test_fe_increments_al },
    { "fe increments bl",             test_fe_increments_bl },
    { "fe decrements cl",             test_fe_decrements_cl },
    { "fe reaches the high bytes",    test_fe_reaches_the_high_bytes },
};

VM86_TEST_MAIN("alu", tests)
