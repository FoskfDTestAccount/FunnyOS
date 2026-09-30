/*
 * Addressing: the ModRM byte, the effective address it forms, and the
 * segment that address is formed in.
 *
 * ---------------------------------------------------------------------
 * Why this suite exists
 *
 * Every instruction that touches memory goes through decode.c, and a
 * decoding mistake does not fail -- it moves the access somewhere else
 * and returns a plausible number from there. So the tests that matter
 * are the ones where the destination is filled in two places and the
 * answer says which one was reached.
 *
 * The instruction suites cannot see this layer. They leave the base
 * registers at zero or set one segment register, and a decoder that
 * picked the wrong segment, dropped a sign, or let a 32-bit sum carry
 * would agree with them by accident and disagree with a real program.
 * Each test here is built so that the wrong answer is a different byte
 * that is also present in memory.
 *
 * Nothing in this file is an instruction under test; the instructions
 * are only there to make the decoder produce an address.
 */
#include "harness.h"

/* ------------------------------------------------------------------ */
/* Which segment an address is formed in                               */
/* ------------------------------------------------------------------ */

static void test_bp_addressing_defaults_to_the_stack_segment(struct vm86_cpu *cpu)
{
    /* mov al, [bp+2] */
    static const uint8_t code[] = { 0x8A, 0x46, 0x02, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    /* DS and SS are put far apart with the same offset filled
     * differently in each, so the byte that arrives names the segment
     * that was used. BP is the one base register that does not default
     * to DS, and it is what makes a stack frame addressable without the
     * compiler saying which segment each access uses. */
    vm86_set_seg(cpu, VM86_DS, 0x1000);   /* base 0x10000 */
    vm86_set_seg(cpu, VM86_SS, 0x3000);   /* base 0x30000 */
    cpu->bp = 0x0020;

    vm86_mem_write8(cpu->mem, 0x30022, 0x5A);
    vm86_mem_write8(cpu->mem, 0x10022, 0xEE);

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AL", cpu->al, 0x5A);
}

static void test_bp_addressing_takes_an_override_like_anything_else(struct vm86_cpu *cpu)
{
    /* es: mov al, [bp+2] */
    static const uint8_t code[] = { 0x26, 0x8A, 0x46, 0x02, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    /* The SS default is a default, not a rule: a segment override moves
     * it the same way it moves a DS-relative operand. Both segments are
     * filled, so this fails if the override is ignored and also if it is
     * applied to the wrong side. */
    vm86_set_seg(cpu, VM86_SS, 0x3000);
    vm86_set_seg(cpu, VM86_ES, 0x4000);
    cpu->bp = 0x0020;

    vm86_mem_write8(cpu->mem, 0x30022, 0x5A);
    vm86_mem_write8(cpu->mem, 0x40022, 0x3C);

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AL", cpu->al, 0x3C);
}

/* ------------------------------------------------------------------ */
/* How the displacement is added                                       */
/* ------------------------------------------------------------------ */

static void test_a_negative_displacement_is_sign_extended(struct vm86_cpu *cpu)
{
    /* mov al, [bx+si-1] */
    static const uint8_t code[] = { 0x8A, 0x40, 0xFF, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    /* The whole reason the 0x83 form of an instruction exists is that a
     * displacement byte is signed, so 0xFF means minus one and not two
     * hundred and fifty-five. With the base at zero the two readings
     * land on different bytes, one at each end of the segment, and both
     * are filled. */
    vm86_set_seg(cpu, VM86_DS, 0x0000);
    cpu->bx = 0x0000;
    cpu->si = 0x0000;

    vm86_mem_write8(cpu->mem, 0xFFFF, 0x7E);   /* minus one */
    vm86_mem_write8(cpu->mem, 0x00FF, 0x11);   /* plus 255 */

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AL", cpu->al, 0x7E);
}

static void test_the_effective_address_wraps_at_sixteen_bits(struct vm86_cpu *cpu)
{
    /* mov al, [bx+si] */
    static const uint8_t code[] = { 0x8A, 0x00, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    /* An offset is sixteen bits wide and the sum wraps inside the
     * segment: FFFF+FFFF is FFFE, not 1FFFE. The second address is
     * filled as well, and it is a real address in guest memory rather
     * than an out-of-range one -- so a decoder that let the host's
     * 32-bit arithmetic carry would read a byte and return it, which is
     * the quiet version of this bug. */
    vm86_set_seg(cpu, VM86_DS, 0x0000);
    cpu->bx = 0xFFFF;
    cpu->si = 0xFFFF;

    vm86_mem_write8(cpu->mem, 0xFFFE, 0xA5);   /* wrapped */
    vm86_mem_write8(cpu->mem, 0x1FFFE, 0xEE);  /* carried out */

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AL", cpu->al, 0xA5);
}

/* ------------------------------------------------------------------ */

static const struct vm86_test tests[] = {
    { "[bp+disp8] takes the stack segment",
      test_bp_addressing_defaults_to_the_stack_segment },
    { "a segment override still moves [bp+disp8]",
      test_bp_addressing_takes_an_override_like_anything_else },
    { "a negative displacement is sign extended",
      test_a_negative_displacement_is_sign_extended },
    { "the effective address wraps at sixteen bits",
      test_the_effective_address_wraps_at_sixteen_bits },
};

VM86_TEST_MAIN("decode", tests)
