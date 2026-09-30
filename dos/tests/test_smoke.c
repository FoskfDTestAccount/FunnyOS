/*
 * A smoke test for the machinery, not for any one instruction group.
 *
 * This suite exists to prove that the pieces fit together: that the
 * dispatch table merges, that decoding resolves an operand, that the
 * flags come out of vm86_alu() the way they should, and that a program
 * made of bytes runs and stops.
 *
 * It covers only what was implemented as a worked example. The real
 * coverage for each instruction lives in that instruction's own suite.
 */
#include "harness.h"

/* ------------------------------------------------------------------ */
/* Running and stopping                                                */
/* ------------------------------------------------------------------ */

static void test_mov_ax_imm(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xB8, 0x34, 0x12, 0xF4 };  /* mov ax,1234h */

    vm86_test_load(cpu, code, sizeof(code));

    enum vm86_result result = vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0x1234);
    vm86_expect_bool("halted", result == VM86_HALT, true);
}

static void test_unimplemented_opcode_traps(struct vm86_cpu *cpu)
{
    /* 0xD6 is SALC, which is undocumented on the 8086 and claimed by
     * nobody here. An opcode no group claims must raise the
     * invalid-opcode exception rather than doing nothing. */
    static const uint8_t code[] = { 0xD6 };

    vm86_test_load(cpu, code, sizeof(code));

    enum vm86_result result = vm86_test_run(cpu, 10);

    vm86_expect_bool("trapped", result == VM86_FAULT, true);
    vm86_expect_u16("fault vector", cpu->fault, VM86_VECTOR_INVALID_OPCODE);
}

/* ------------------------------------------------------------------ */
/* The ALU, both directions                                            */
/* ------------------------------------------------------------------ */

static void test_add_wraps_and_sets_carry(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xB8, 0xFF, 0xFF,   /* mov ax, 0xFFFF */
                                    0x05, 0x01, 0x00,   /* add ax, 1      */
                                    0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0x0000);
    vm86_expect_flag("CF", cpu, VM86_CF, true);
    vm86_expect_flag("ZF", cpu, VM86_ZF, true);
    /* Everything had the same sign and the result did too, so this is a
     * carry but not an overflow. Those two are not the same flag and a
     * test that checks only one will not notice them being swapped. */
    vm86_expect_flag("OF", cpu, VM86_OF, false);
}

static void test_add_overflows_without_carrying(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xB0, 0x7F,   /* mov al, 0x7F */
                                    0x04, 0x01,   /* add al, 1    */
                                    0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_test_run(cpu, 10);

    vm86_expect_u16("AL", cpu->al, 0x80);
    vm86_expect_flag("OF", cpu, VM86_OF, true);
    vm86_expect_flag("CF", cpu, VM86_CF, false);
    vm86_expect_flag("SF", cpu, VM86_SF, true);
}

static void test_subtraction_sets_carry_on_borrow(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xB0, 0x00,   /* mov al, 0 */
                                    0x2C, 0x01,   /* sub al, 1 */
                                    0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_test_run(cpu, 10);

    vm86_expect_u16("AL", cpu->al, 0xFF);

    /* This is the assertion the whole case exists for. Subtraction sets
     * CF when it borrows -- exactly the opposite of the intuition that
     * carry belongs to addition. */
    vm86_expect_flag("CF", cpu, VM86_CF, true);
    vm86_expect_flag("SF", cpu, VM86_SF, true);
    vm86_expect_flag("ZF", cpu, VM86_ZF, false);
}

/* ------------------------------------------------------------------ */
/* The stack                                                           */
/* ------------------------------------------------------------------ */

static void test_push_and_pop_round_trip(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xB8, 0xCD, 0xAB,   /* mov ax, 0xABCD */
                                    0x50,               /* push ax */
                                    0x5B,               /* pop bx  */
                                    0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_test_run(cpu, 10);

    vm86_expect_u16("BX", cpu->bx, 0xABCD);
    vm86_expect_u16("SP", cpu->sp, VM86_TEST_STACK_TOP);
}

static void test_push_lands_where_the_stack_pointer_says(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xB8, 0x77, 0x66,   /* mov ax, 0x6677 */
                                    0x50,               /* push ax */
                                    0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_test_run(cpu, 10);

    /* SP is decremented before the store, so it ends up pointing at the
     * word that was pushed -- which is what lets POP find it. */
    vm86_expect_u16("SP", cpu->sp, VM86_TEST_STACK_TOP - 2);
    vm86_expect_mem16("stack top", cpu, VM86_TEST_STACK_TOP - 2, 0x6677);
}

/* ------------------------------------------------------------------ */
/* The flags INC must not touch                                        */
/* ------------------------------------------------------------------ */

static void test_increment_leaves_carry_alone(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xB8, 0x00, 0x00,   /* mov ax, 0 */
                                    0x40,               /* inc ax   */
                                    0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    /* Set the carry by hand: nothing implemented so far sets it in a way
     * a test can arrange, and the point is what INC does with it. */
    vm86_flag_set(cpu, VM86_CF, true);

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0x0001);

    /* INC is not ADD 1. This is the entire difference, and a program that
     * loops on the carry left over from earlier work depends on it. */
    vm86_expect_flag("CF preserved", cpu, VM86_CF, true);
}

/* ------------------------------------------------------------------ */

static const struct vm86_test tests[] = {
    { "mov ax, immediate",            test_mov_ax_imm },
    { "unclaimed opcode traps",       test_unimplemented_opcode_traps },
    { "add wraps and sets carry",     test_add_wraps_and_sets_carry },
    { "add overflows without carry",  test_add_overflows_without_carrying },
    { "sub borrows and sets carry",   test_subtraction_sets_carry_on_borrow },
    { "push then pop round trip",     test_push_and_pop_round_trip },
    { "push writes where SP says",    test_push_lands_where_the_stack_pointer_says },
    { "inc leaves carry alone",       test_increment_leaves_carry_alone },
};

VM86_TEST_MAIN("smoke", tests)
