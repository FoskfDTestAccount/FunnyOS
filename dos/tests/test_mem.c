/*
 * The guest's memory: what an address means, and what a program sees
 * when there is nothing there.
 *
 * ---------------------------------------------------------------------
 * Why this suite exists
 *
 * mem.c is the only place a linear address becomes an offset into the
 * host's array, and it is the only thing standing between a guest's
 * arithmetic and the host's memory. So the cases here are the edges: the
 * last byte, one past the last byte, an address above the twentieth line
 * with the gate open and closed, and what a clear is allowed to forget.
 *
 * The rest of the machine cannot reach the gate at all -- it is set when
 * the region is attached and only the host can change it -- so nothing
 * else in the tree would notice if the fold stopped working.
 */
#include "harness.h"

/* ------------------------------------------------------------------ */
/* The one address above the machine                                   */
/* ------------------------------------------------------------------ */

static void test_a20_off_folds_the_top_of_memory(struct vm86_cpu *cpu)
{
    /* One byte of program, which is not what gets fetched below. */
    static const uint8_t code[] = { 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    /*
     * CS=FFFF:IP=0010 is the case the fold exists for: the segment base
     * is FFFF0, so the address is 0x100000 -- past the end of the
     * machine's memory and past the twentieth address line.
     */
    cpu->mem->a20 = false;

    /* Where the address folds to. Nothing else writes there, so a fetch
     * that arrives is the fold and nothing else. */
    vm86_mem_write8(cpu->mem, 0x000000, 0xF4);

    vm86_set_seg(cpu, VM86_CS, 0xFFFF);
    cpu->ip = 0x0010;

    enum vm86_result result = vm86_test_run(cpu, 5);

    vm86_expect_bool("halted", result == VM86_HALT, true);
    vm86_expect_u16("IP", cpu->ip, 0x0011);
}

static void test_a20_on_leaves_the_top_of_memory_unmapped(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    /*
     * With the gate open the same address is a real one with nothing
     * behind it, which is a different answer from a fold and is what a
     * machine with HIMEM loaded looks like. The gate is on by default,
     * so this is the state every other suite runs in.
     */
    vm86_expect_bool("the gate is open", cpu->mem->a20, true);
    vm86_expect_u16("read above the top of memory",
                    vm86_mem_read8(cpu->mem, 0x100000), 0xFF);
    vm86_expect_bool("the read was counted",
                     cpu->mem->unmapped_reads == 1, true);
}

/* ------------------------------------------------------------------ */
/* Reading and writing where there is nothing                          */
/* ------------------------------------------------------------------ */

static void test_an_unmapped_write_is_dropped(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    /* A write with nothing behind it is dropped rather than faulted or
     * folded somewhere useful. The counter is part of the contract: a
     * program that does nothing else is worth being able to notice, and
     * it is the only way to tell a dropped write from a write that
     * landed. */
    vm86_mem_write8(cpu->mem, 0x100000, 0x5A);

    vm86_expect_bool("the address really is outside",
                     vm86_mem_offset(cpu->mem, 0x100000) == VM86_MEM_UNMAPPED,
                     true);
    vm86_expect_bool("the write was dropped and counted",
                     cpu->mem->dropped_writes == 1, true);
}

static void test_a_word_at_the_top_is_half_floating_bus(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    /*
     * The last byte of the region is real and the one after it is not,
     * and a 16-bit access is two 8-bit ones -- so the answer is half
     * memory and half floating bus rather than a read past the end of
     * the host's array. Both halves are asserted: the low one because it
     * has to survive, the high one because it has to be the bus and not
     * whatever the host keeps next.
     */
    vm86_mem_write8(cpu->mem, 0x0FFFFF, 0x34);

    vm86_expect_u16("the word", vm86_mem_read16(cpu->mem, 0x0FFFFF), 0xFF34);
}

/* ------------------------------------------------------------------ */
/* The two things a clear must leave alone                             */
/* ------------------------------------------------------------------ */

static void test_clear_keeps_the_region_and_the_gate(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    /*
     * A clear is what a harness does between cases, so it resets the
     * state and leaves the configuration. The gate and the region size
     * are configuration: a clear that reset the gate would change what
     * the next case was measuring, and one that forgot the size would
     * not be a clear at all.
     */
    uint32_t size = cpu->mem->size;

    cpu->mem->a20            = false;
    cpu->mem->unmapped_reads = 5;
    cpu->mem->dropped_writes = 7;
    vm86_mem_write8(cpu->mem, 0x0400, 0x99);

    vm86_mem_clear(cpu->mem);

    vm86_expect_bool("the gate is as it was", cpu->mem->a20, false);
    vm86_expect_bool("the region is as it was",
                     cpu->mem->size == size, true);
    vm86_expect_bool("reads start from zero again",
                     cpu->mem->unmapped_reads == 0, true);
    vm86_expect_bool("writes start from zero again",
                     cpu->mem->dropped_writes == 0, true);
    vm86_expect_u16("memory is zeroed", vm86_mem_read8(cpu->mem, 0x0400), 0x00);
}

static void test_offset_marks_an_address_outside_the_region(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    /*
     * The last byte of the region has an offset and the one past it does
     * not. The sentinel is all ones, which is also what an unguardedly
     * used offset would look like if it were truncated -- so every
     * caller has to compare before it indexes, and this pins both ends
     * of the range.
     */
    vm86_expect_bool("the last byte is at its own offset",
                     vm86_mem_offset(cpu->mem, 0x0FFFFF) == 0x0FFFFF, true);
    vm86_expect_bool("one past the end is the sentinel",
                     vm86_mem_offset(cpu->mem, 0x100000) == VM86_MEM_UNMAPPED,
                     true);
}

/* ------------------------------------------------------------------ */

static const struct vm86_test tests[] = {
    { "the gate closed folds the top of memory",
      test_a20_off_folds_the_top_of_memory },
    { "the gate open leaves the same address unmapped",
      test_a20_on_leaves_the_top_of_memory_unmapped },
    { "a write where there is nothing is dropped",
      test_an_unmapped_write_is_dropped },
    { "a word at the top of memory is half floating bus",
      test_a_word_at_the_top_is_half_floating_bus },
    { "a clear keeps the region and the gate",
      test_clear_keeps_the_region_and_the_gate },
    { "an address outside the region has no offset",
      test_offset_marks_an_address_outside_the_region },
};

VM86_TEST_MAIN("mem", tests)
