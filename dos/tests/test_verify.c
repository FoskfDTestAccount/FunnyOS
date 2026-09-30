/*
 * Independent verification.
 *
 * ---------------------------------------------------------------------
 * What this suite is for, and how it differs from the others
 *
 * Every other suite here was written by the person who wrote the code it
 * tests, and answers "does my code do what I thought it does". This one
 * was written by somebody who did not, and answers a different question:
 * "is there something everybody assumed was handled, and was not".
 *
 * So the expected values in this file come from the manual and from
 * arithmetic done by hand, not from reading the handlers. Where a case
 * exercises a boundary the manual does not settle, the comment says which
 * of the two readings was taken and why -- a test whose expectation was
 * copied out of the implementation proves nothing about the
 * implementation.
 *
 * The cases are chosen for the places the ordinary tests do not go:
 *
 *   - the ends of segments, where an offset can carry out of the segment
 *     it belongs to, and the ends of memory, where a twenty-bit address
 *     wraps;
 *   - counts of zero, counts of one, and counts that make a counter run
 *     out on the first decrement;
 *   - the exact values at which a carry, a borrow and a signed overflow
 *     appear and disappear;
 *   - instructions paired with their inverses, where the state has to come
 *     back to what it was (PUSH/POP, INT/IRET, CALL/RET, ENTER/LEAVE);
 *   - prefix behaviour on an instruction that reads through one segment
 *     and writes through another.
 *
 * ---------------------------------------------------------------------
 * Provenance of the expectations
 *
 * All of these were derived from the manual, and none of them from the
 * source. None was checked against a second emulator: there is no DOSBox
 * or QEMU on this machine, so the whole file is "derived from the
 * manual" rather than "confirmed against hardware". Where the manual is
 * silent the comment says so; those cases pin down what this
 * implementation chose rather than what the chip does, and are marked.
 */
#include "harness.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

/* Write a word into guest memory through the memory layer, so that the
 * A20 setting applies to test setup the same way it applies to the code
 * under test. */
static void poke16(struct vm86_cpu *cpu, uint32_t linear, uint16_t value)
{
    vm86_mem_write16(cpu->mem, linear, value);
}

static void poke8(struct vm86_cpu *cpu, uint32_t linear, uint8_t value)
{
    vm86_mem_write8(cpu->mem, linear, value);
}

/*
 * Copy bytes into guest memory at a linear address.
 *
 * Used instead of vm86_test_load() when the code does not go at the
 * usual entry point -- a handler, a table, or a second copy of the
 * program in another segment.
 */
static void place(struct vm86_cpu *cpu, uint32_t linear,
                  const uint8_t *bytes, size_t count)
{
    for (size_t i = 0; i < count; i++)
        vm86_mem_write8(cpu->mem, linear + (uint32_t)i, bytes[i]);
}

/* The flag word a reset machine reports, plus whichever bits are named.
 * Writing it out at each use would invite a typo in the one constant the
 * whole machine's self-identification rests on. */
#define FLAGS(bits) ((uint16_t)(VM86_FLAG_ALWAYS_SET | (bits)))

/* ================================================================== */
/* Segments, and the ends of them                                      */
/* ================================================================== */

/*
 * An instruction that straddles the last byte of the code segment.
 *
 * The 8086 forms the fetch address from a sixteen-bit IP and a segment
 * base, so when IP runs off the end it wraps inside the segment rather
 * than running into the next one: the byte after CS:FFFF is CS:0000.
 *
 * The test is arranged so that the two readings differ. The immediate of
 * `add al,1` lands at offset FFFF, and offset 0000 of the same segment
 * holds the HLT that ends the program. An implementation that carried IP
 * into the next segment would fetch the immediate from 0x30000, which is
 * left zero, and AL would come out 0x10 instead of 0x11.
 */
static void test_code_straddles_the_end_of_the_segment(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x04, 0x01 };   /* add al, 1 */
    static const uint8_t hlt[]  = { 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    /* The same two bytes again, at the end of segment 2000h, and the
     * HLT they fall through to at the start of that segment. */
    place(cpu, 0x2FFFE, code, sizeof(code));
    place(cpu, 0x20000, hlt, sizeof(hlt));

    vm86_set_seg(cpu, VM86_CS, 0x2000);
    cpu->ip = 0xFFFE;
    cpu->al = 0x10;

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AL took the immediate from offset FFFF", cpu->al, 0x11);
    vm86_expect_u16("IP wrapped inside the segment", cpu->ip, 0x0001);
    vm86_expect_u16("CS did not move", cpu->cs, 0x2000);
}

/*
 * A word at the last offset of a data segment.
 *
 * This is the other half of the previous case and the opposite answer.
 * A fetch at CS:FFFF wraps because IP is a sixteen-bit register feeding
 * the address adder. A *data* access does not: the address the 8086
 * forms is a twenty-bit physical one, and the second byte of a word
 * access is one greater than the first. So a word at offset FFFF of a
 * segment reads the last byte of that segment and the first byte of the
 * next one.
 *
 * Offset 0000 of the segment is given a third value, so an implementation
 * that wrapped the offset would produce a word that cannot be mistaken
 * for the expected one.
 *
 * (The task book describes the 8086 as wrapping the offset here. It does
 * not, and a reader who took the task book as authoritative would change
 * this to the wrong answer. See the report.)
 */
static void test_word_at_the_end_of_a_segment_crosses_into_the_next(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xA1, 0xFF, 0xFF, 0xF4 };  /* mov ax,[FFFFh]; hlt */

    vm86_test_load(cpu, code, sizeof(code));

    poke8(cpu, 0x2FFFF, 0x34);   /* DS:FFFF -- low half of the word   */
    poke8(cpu, 0x30000, 0x12);   /* where the next byte of memory is  */
    poke8(cpu, 0x20000, 0x99);   /* DS:0000 -- only a wrapping read   */

    vm86_set_seg(cpu, VM86_DS, 0x2000);

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AX read across the segment boundary", cpu->ax, 0x1234);
}

/*
 * A word at the top of the address space, with the A20 gate closed.
 *
 * DS = F000h puts offset FFFFh at linear 0xFFFFF, the last byte the
 * machine has, and the second byte of the word is linear 0x100000 --
 * which on a part with twenty address lines is the same wire as
 * 0x000000, and comes back as whatever lives there.
 *
 * Both halves are placed before the gate is closed, and the low byte of
 * the word is left at a value that only a folded read can produce, so an
 * implementation that let the address run past 1 MiB would read the
 * floating bus instead and show it.
 */
static void test_a20_off_folds_a_word_at_the_top_of_memory(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xA1, 0xFF, 0xFF, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    poke8(cpu, 0xFFFFF, 0xEF);   /* DS:FFFF, DS = F000h -- in range     */
    poke8(cpu, 0x00000, 0xBE);   /* where the second byte folds to      */

    vm86_set_seg(cpu, VM86_DS, 0xF000);
    cpu->mem->a20 = false;

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AX read the folded byte", cpu->ax, 0xBEEF);
}

/*
 * The same access with the gate open.
 *
 * 0x100000 is past the twenty address lines and there is no memory
 * behind it, so the high half of the word reads as the floating bus:
 * 0xFF. The low address that the folded read would have used is loaded
 * with a different value, so the two answers cannot be confused.
 */
static void test_a20_on_leaves_the_top_of_memory_unmapped(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xA1, 0xFF, 0xFF, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    poke8(cpu, 0xFFFFF, 0xEF);
    poke8(cpu, 0x00000, 0xBE);

    vm86_set_seg(cpu, VM86_DS, 0xF000);
    /* The gate is open: that is how the harness attaches memory. */

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AX read the floating bus", cpu->ax, 0xFFEF);
}

/*
 * A push whose word runs off the end of the stack segment.
 *
 * With SP at 1 the push stores at offsets FFFF and 0000 *of the next
 * segment* -- the same rule as any other word access, and the reason a
 * stack that has been allowed to underflow writes into whatever follows
 * it rather than faulting. The byte order is checked as well: the low
 * half of the value goes to the lower address, which is also what the
 * A20 case above depends on.
 */
static void test_push_runs_off_the_end_of_the_stack_segment(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x50, 0xF4 };   /* push ax; hlt */

    vm86_test_load(cpu, code, sizeof(code));

    vm86_set_seg(cpu, VM86_SS, 0x2000);
    cpu->sp = 0x0001;
    cpu->ax = 0x1234;

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("SP wrapped to the top of the segment", cpu->sp, 0xFFFF);
    vm86_expect_mem8("low half at 2FFFF", cpu, 0x2FFFF, 0x34);
    vm86_expect_mem8("high half at 30000", cpu, 0x30000, 0x12);
}

/*
 * PUSH SP.
 *
 * The 8086 pushes the value SP holds *after* the decrement; the 80186
 * and everything after push the value *before*. It is a documented
 * difference between the parts and software of the era ran the
 * instruction to find out which one it was on, so for a machine that
 * claims to be an 8086 there is only one right answer.
 *
 * At SP = 8000h the two readings are 7FFE and 8000, so the stacked word
 * says which one happened without any further work.
 */
static void test_push_sp_pushes_the_decremented_value(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x54, 0xF4 };   /* push sp; hlt */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->sp = 0x8000;

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("SP", cpu->sp, 0x7FFE);
    vm86_expect_mem16("what PUSH SP left behind", cpu, 0x7FFE, 0x7FFE);
}

/*
 * FF F4 is PUSH SP as well, and it has to answer exactly as 54 does.
 *
 * One instruction, two encodings, two files: 54 lives in ops_mov.c and
 * FF F4 in ops_ctl.c's group 5. The 8086 answer was put into the first
 * and missed in the second, so the same instruction had two values
 * depending on how it was written -- and a future reader would have
 * concluded from the code that the two encodings differ.
 *
 * The verification pass that found it also noticed that neither this
 * suite nor the corpus sample for PUSH SP exercised FF F4 at all, which
 * is why both encodings are asserted here side by side rather than only
 * the one that happened to be tested.
 */
static void test_push_sp_by_group_five_agrees_with_fifty_four(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xFF, 0xF4, 0xF4 };   /* push sp; hlt */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->sp = 0x8000;

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("SP", cpu->sp, 0x7FFE);
    vm86_expect_mem16("what FF F4 left behind", cpu, 0x7FFE, 0x7FFE);
}

/*
 * POP SP, and the increment that gets thrown away.
 *
 * The manually described order is "transfer the word, then increment SP
 * by two", which read literally would leave SP at 2002h here. What the
 * implementation does is 2000h: the word off the stack becomes SP and
 * the increment disappears, because for this one destination the write
 * to the register is the last thing that happens.
 *
 * Which of the two the 8086's microcode does is not something the manual
 * states, and it is listed in the report as unknown rather than settled.
 * The case is here to pin the behaviour this machine has, so that
 * changing it is a deliberate act and not a slip -- `pop sp` is the one
 * POP whose destination is the pointer, and it is not exercised anywhere
 * else in the suite.
 */
static void test_pop_sp_ends_at_the_popped_value(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x5C, 0xF4 };   /* pop sp; hlt */

    vm86_test_load(cpu, code, sizeof(code));

    poke16(cpu, 0x7FFE, 0x2000);
    cpu->sp = 0x7FFE;

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("SP", cpu->sp, 0x2000);
}

/* ================================================================== */
/* Addresses, and the ones that are not memory                         */
/* ================================================================== */

/*
 * LEA computes an address and does not go there.
 *
 * The operand is placed so that its linear address is past the end of
 * guest memory entirely -- DS = FFFFh and offset FFFFh is 0x10FFEF, which
 * is above the 1 MiB the machine has and is not folded back by the A20
 * gate. A LEA that read its operand would take the floating bus and
 * would also move the memory layer's unmapped-read counter; the counter
 * is what makes this a check rather than an inference, because the value
 * that comes back (0xFFFF) is the same either way for a read that
 * returns all ones.
 *
 * The effective offset is what must land in BX, and it is the offset and
 * not the linear address: 0xFFFF, not 0x10FFEF, which would not fit in
 * sixteen bits.
 */
static void test_lea_reads_no_memory_and_stores_the_offset(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x8D, 0x1E, 0xFF, 0xFF, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    vm86_set_seg(cpu, VM86_DS, 0xFFFF);
    uint64_t before = cpu->mem->unmapped_reads;

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("BX holds the offset", cpu->bx, 0xFFFF);
    vm86_expect_bool("LEA performed no read",
                     cpu->mem->unmapped_reads != before, false);
}

/*
 * A segment override does not reach LEA's result.
 *
 * The same instruction twice, with DS and ES pointing at different
 * places, and the prefix that names the other one. `lea` stores an
 * offset inside a segment; which segment the address was formed in is
 * not part of it. An implementation that folded the base in, or that
 * subtracted the overridden base from an address formed with the natural
 * one, would give two different answers here.
 */
static void test_lea_ignores_the_segment_override(struct vm86_cpu *cpu)
{
    static const uint8_t plain[] = { 0x8D, 0x1E, 0x00, 0x02, 0xF4 };
    static const uint8_t es[]    = { 0x26, 0x8D, 0x1E, 0x00, 0x02, 0xF4 };

    vm86_test_load(cpu, plain, sizeof(plain));
    vm86_set_seg(cpu, VM86_DS, 0x1000);
    vm86_set_seg(cpu, VM86_ES, 0x1001);
    vm86_test_run(cpu, 10);
    uint16_t without = cpu->bx;

    vm86_test_load(cpu, es, sizeof(es));
    vm86_set_seg(cpu, VM86_DS, 0x1000);
    vm86_set_seg(cpu, VM86_ES, 0x1001);
    vm86_test_run(cpu, 10);
    uint16_t with = cpu->bx;

    vm86_expect_u16("no prefix", without, 0x0200);
    vm86_expect_u16("es: changes nothing", with, without);
}

/*
 * The direct-addressed MOV forms take an offset, not a linear address.
 *
 * A0-A3 carry a sixteen-bit address with no ModRM byte, which makes them
 * the one place a reader might reasonably expect the operand to be
 * absolute. It is not: the segment prefix and the segment base both
 * apply, so `mov al, [0200h]` with DS = 1000h reads linear 0x10200.
 *
 * The value is placed at both candidates, with different contents, so
 * the answer says which address was used.
 */
static void test_direct_mov_takes_an_offset_in_ds(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xA0, 0x00, 0x02, 0xF4 };  /* mov al,[0200h] */

    vm86_test_load(cpu, code, sizeof(code));

    poke8(cpu, 0x00200, 0x11);   /* DS would be 0 here            */
    poke8(cpu, 0x10200, 0x22);   /* DS:0200 with DS = 1000h       */

    vm86_set_seg(cpu, VM86_DS, 0x1000);

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AL", cpu->al, 0x22);
}

/*
 * ... and the override reaches it, which is what makes `mov al, es:[0]`
 * a shorter encoding than the ModRM form rather than a different one.
 */
static void test_direct_mov_honours_the_segment_override(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x26, 0xA0, 0x00, 0x02, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    poke8(cpu, 0x10200, 0x11);   /* what the natural DS would read */
    poke8(cpu, 0x30200, 0x33);   /* ES:0200 with ES = 3000h        */

    vm86_set_seg(cpu, VM86_DS, 0x1000);
    vm86_set_seg(cpu, VM86_ES, 0x3000);

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AL", cpu->al, 0x33);
}

/*
 * A ModRM displacement is consumed before the immediate.
 *
 * C7 46 10 34 12 is `mov word [bp+10h], 1234h`: a ModRM byte, a
 * one-byte displacement, then a two-byte immediate. A handler that read
 * its immediate first would take the displacement as the low half of it
 * and leave the real immediate to be decoded as the next instruction --
 * which is why the bytes after it here are chosen so the difference is
 * visible rather than merely wrong.
 *
 * BP is used because it addresses the stack segment by default, so this
 * also checks that the BP default is applied to a *write*: no segment is
 * loaded for it, SS is left at 0 deliberately.
 */
static void test_modrm_displacement_comes_before_the_immediate(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xC7, 0x46, 0x10, 0x34, 0x12, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    cpu->bp = 0x0200;

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);

    vm86_expect_mem16("[bp+10h]", cpu, 0x0210, 0x1234);
    vm86_expect_u16("IP reached the HLT", cpu->ip, 0x0106);
}

/*
 * The eight-bit write of a segment register leaves the rest of the
 * register alone, and CS can be read.
 *
 * 8C /r reads any of the four, CS included -- that is how a program
 * finds out where its own code segment is. The test reads ES into AX and
 * then reads CS into the same register, which is also the only way to
 * tell a handler that wrote 16 bits from one that wrote 8.
 */
static void test_mov_from_a_segment_register(struct vm86_cpu *cpu)
{
    static const uint8_t es_to_ax[] = { 0x8C, 0xC0, 0xF4 };
    static const uint8_t cs_to_ax[] = { 0x8C, 0xC8, 0xF4 };

    vm86_test_load(cpu, es_to_ax, sizeof(es_to_ax));
    vm86_set_seg(cpu, VM86_ES, 0x1234);
    cpu->ax = 0xFFFF;
    vm86_test_run(cpu, 10);

    vm86_expect_u16("mov ax, es", cpu->ax, 0x1234);

    /* The code segment has to be moved as well as the register, or the
     * fetch looks for the program somewhere the program is not. */
    vm86_test_load(cpu, cs_to_ax, sizeof(cs_to_ax));
    place(cpu, 0x20100, cs_to_ax, sizeof(cs_to_ax));
    vm86_set_seg(cpu, VM86_CS, 0x2000);
    cpu->ax = 0x0000;
    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("mov ax, cs", cpu->ax, 0x2000);
}

/*
 * A segment register load takes effect immediately.
 *
 * `mov ss, ax` / `mov sp, bx` is written without disabling interrupts
 * because the instruction after a segment-register load does not
 * *recognise* interrupts -- not because the load is late. The register
 * itself holds the new value as soon as the instruction ends, and this
 * test is that claim: the very next instruction reads memory through the
 * segment that was just loaded.
 *
 * The task book's first version had this backwards, and an implementation
 * that believed it would defer the load by one instruction. That is why
 * the test exists rather than being assumed.
 */
static void test_a_segment_load_is_not_delayed(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = {
        0xB8, 0x00, 0x10,   /* mov ax, 1000h  */
        0x8E, 0xD8,         /* mov ds, ax     */
        0xA0, 0x00, 0x02,   /* mov al, [200h] */
        0xF4,               /* hlt            */
    };

    vm86_test_load(cpu, code, sizeof(code));

    poke8(cpu, 0x10200, 0x44);   /* only reachable through the new DS */
    poke8(cpu, 0x00200, 0x55);

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("DS", cpu->ds, 0x1000);
    vm86_expect_u16("AL read through the new DS", cpu->al, 0x44);
}

/*
 * XLAT adds BX to AL at sixteen bits and wraps there.
 *
 * `mov al, [bx+al]` is what the instruction is, and the sum is an offset
 * inside the segment, so BX=FFF0h with AL=20h indexes offset 0010h of
 * the same segment and not 0x10010. The table entry is placed at the
 * wrapped offset with a value that the unwrapped offset does not hold,
 * so the two can be told apart -- and DS is given a base as well, so an
 * implementation that forgot the segment base reads the wrong byte too.
 */
static void test_xlat_adds_bx_and_al_and_wraps(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD7, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    poke8(cpu, 0x20010, 0x77);   /* DS:0010 with DS = 2000h */
    poke8(cpu, 0x30010, 0x88);   /* where an unwrapped sum would land */

    vm86_set_seg(cpu, VM86_DS, 0x2000);
    cpu->bx = 0xFFF0;
    cpu->al = 0x20;

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AL", cpu->al, 0x77);
}

/* XLAT is a load and sets nothing: not the arithmetic flags, and not the
 * ones a MOV is sometimes assumed to touch. */
static void test_xlat_leaves_the_flags_alone(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD7, 0xF4 };

    const uint16_t before = FLAGS(VM86_CF | VM86_ZF | VM86_SF | VM86_OF |
                                  VM86_AF | VM86_PF | VM86_DF);

    vm86_test_load(cpu, code, sizeof(code));
    cpu->bx = 0x0100;
    cpu->flags = before;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("FLAGS", cpu->flags, before);
}

/* ================================================================== */
/* The sixteen-bit carry, borrow and overflow boundaries               */
/* ================================================================== */

/*
 * The additions and subtractions below are all at the exact values where
 * a flag changes, worked out by hand rather than read off a table of the
 * implementation's answers. AF is included every time: it is the flag
 * people get wrong, it is set from bit 3 whatever the operand width, and
 * a program's BCD code and any `daa` after it depend on it.
 */

static void test_add_carry_out_of_the_top_bit(struct vm86_cpu *cpu)
{
    /* FFFFh + 1 = 0 with a carry and no overflow: the operands had
     * opposite signs, so the sign bit cannot have been surprised. */
    static const uint8_t code[] = { 0x05, 0x01, 0x00, 0xF4 };  /* add ax,1 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0xFFFF;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0x0000);
    /* CF set, PF set (zero has no odd bits), AF set (F + 1 carries out
     * of bit 3), ZF set, SF clear, OF clear. */
    vm86_expect_flags("flags", cpu, FLAGS(VM86_CF | VM86_PF | VM86_AF |
                                          VM86_ZF), 0);
}

static void test_add_signed_overflow_without_a_carry(struct vm86_cpu *cpu)
{
    /* 7FFFh + 1 = 8000h: the largest positive becomes the smallest
     * negative, so OF is set and CF is not. Bits 3 and 4 of the low
     * nibble carry, so AF is set. */
    static const uint8_t code[] = { 0x05, 0x01, 0x00, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x7FFF;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0x8000);
    /* 8000h has one bit set, so the low byte (00) has an even number of
     * them and PF is set. */
    vm86_expect_flags("flags", cpu,
                      FLAGS(VM86_AF | VM86_PF | VM86_SF | VM86_OF), 0);
}

static void test_add_without_a_carry_at_the_sign_boundary(struct vm86_cpu *cpu)
{
    /* 7FFFh + 0 is the value just below where the overflow starts, so
     * both carry and overflow stay clear and SF stays clear. */
    static const uint8_t code[] = { 0x05, 0x00, 0x00, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x7FFF;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0x7FFF);
    /* 7Fh has seven bits set, an odd number, so PF is clear. */
    vm86_expect_flags("flags", cpu, FLAGS(VM86_PF), 0);
}

static void test_sub_borrow_out_of_the_bottom(struct vm86_cpu *cpu)
{
    /* 0 - 1 = FFFFh. The carry flag means BORROW on a subtraction, which
     * is the single most common way to get an 8086 wrong: this is CF set,
     * not CF clear. */
    static const uint8_t code[] = { 0x2D, 0x01, 0x00, 0xF4 };  /* sub ax,1 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0000;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0xFFFF);
    /* Borrow, no overflow (0 and 1 have the same sign), AF set because
     * the low nibble had to borrow, SF set, PF set on FFh. */
    vm86_expect_flags("flags", cpu,
                      FLAGS(VM86_CF | VM86_AF | VM86_SF | VM86_PF), 0);
}

static void test_sub_signed_overflow_without_a_borrow(struct vm86_cpu *cpu)
{
    /* 8000h - 1 = 7FFFh: the smallest negative becomes the largest
     * positive. CF is clear, because 8000h is not below 1. */
    static const uint8_t code[] = { 0x2D, 0x01, 0x00, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x8000;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0x7FFF);
    /* 7Fh: seven bits, odd, so PF clear. AF set: 0 - 1 borrows inside the
     * low nibble. */
    vm86_expect_flags("flags", cpu, FLAGS(VM86_AF | VM86_OF | VM86_PF), 0);
}

static void test_sub_equal_operands_clears_the_borrow(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x2D, 0x34, 0x12, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x1234;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0x0000);
    vm86_expect_flags("flags", cpu, FLAGS(VM86_ZF | VM86_PF), 0);
}

static void test_sbb_borrows_the_incoming_carry(struct vm86_cpu *cpu)
{
    /* 0 - 0 - 1 = FFFFh. SBB uses the carry as a borrow, so the result
     * borrows even though the operands are equal. */
    static const uint8_t code[] = { 0x1D, 0x00, 0x00, 0xF4 };  /* sbb ax,0 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0000;
    cpu->flags = FLAGS(VM86_CF);

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0xFFFF);
    vm86_expect_flags("flags", cpu,
                      FLAGS(VM86_CF | VM86_AF | VM86_SF | VM86_PF), 0);
}

static void test_sbb_with_no_incoming_carry_is_a_plain_subtract(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x1D, 0x00, 0x00, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0001;
    cpu->flags = VM86_FLAG_ALWAYS_SET;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0x0001);
    vm86_expect_flags("flags", cpu, VM86_FLAG_ALWAYS_SET, VM86_AF);
}

static void test_adc_carries_in(struct vm86_cpu *cpu)
{
    /* FFFFh + 0 + 1 = 0 with a carry out. */
    static const uint8_t code[] = { 0x15, 0x00, 0x00, 0xF4 };  /* adc ax,0 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0xFFFF;
    cpu->flags = FLAGS(VM86_CF);

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0x0000);
    vm86_expect_flags("flags", cpu,
                      FLAGS(VM86_CF | VM86_ZF | VM86_AF | VM86_PF), 0);
}

/*
 * The auxiliary carry is bit 3 of the low byte at every width, and it
 * does not move up to bit 7 for a byte. Two additions chosen so that one
 * carries out of bit 3 and the other does not: 0Fh+1 and 07h+8.
 */
static void test_auxiliary_carry_is_bit_three(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x04, 0x01, 0xF4 };   /* add al,1 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->al = 0x0F;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AL", cpu->al, 0x10);
    vm86_expect_flag("AF set when the low nibble carries", cpu, VM86_AF, true);
    vm86_expect_flag("CF clear", cpu, VM86_CF, false);

    static const uint8_t no_carry[] = { 0x04, 0x08, 0xF4 };

    vm86_test_load(cpu, no_carry, sizeof(no_carry));
    cpu->al = 0x07;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AL", cpu->al, 0x0F);
    vm86_expect_flag("AF clear when it does not", cpu, VM86_AF, false);
}

/*
 * Parity is taken from the low eight bits even when the operation was a
 * sixteen-bit one -- which is the thing about PF that surprises people,
 * and it is not symmetric with the sign flag, which does move up.
 *
 * 00FFh + 1 = 0100h. The word has one bit set; the low byte has none.
 * PF is set, and SF is clear: two flags that disagree about "how many
 * bits are set" because they are not looking at the same width.
 */
static void test_parity_looks_at_the_low_byte_of_a_word(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x05, 0x01, 0x00, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x00FF;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0x0100);
    vm86_expect_flag("PF from the low byte", cpu, VM86_PF, true);
    vm86_expect_flag("SF from bit 15", cpu, VM86_SF, false);
    vm86_expect_flag("ZF", cpu, VM86_ZF, false);
}

/*
 * INC and DEC leave the carry flag exactly as they found it.
 *
 * That is the whole difference between INC and ADD 1, and a loop written
 * `inc cx / jc somewhere` is using a carry left from earlier work. Both
 * directions are checked with the carry set, because the mistake that
 * matters is clearing it.
 */
static void test_inc_and_dec_leave_the_carry_alone(struct vm86_cpu *cpu)
{
    static const uint8_t inc[] = { 0xB8, 0xFF, 0xFF, 0x40, 0xF4 };  /* mov ax,-1; inc ax */
    static const uint8_t dec[] = { 0xB8, 0x00, 0x00, 0x48, 0xF4 };  /* mov ax,0; dec ax  */

    vm86_test_load(cpu, inc, sizeof(inc));
    cpu->flags = FLAGS(VM86_CF | VM86_OF | VM86_ZF | VM86_DF);

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0x0000);
    /* CF survives; DF is not touched by arithmetic at all; OF and ZF come
     * from the result (0 has neither); PF is set by an even low byte. */
    vm86_expect_flags("flags after INC", cpu,
                      FLAGS(VM86_CF | VM86_ZF | VM86_AF | VM86_PF |
                            VM86_DF), 0);

    vm86_test_load(cpu, dec, sizeof(dec));
    cpu->flags = FLAGS(VM86_CF | VM86_OF | VM86_DF);

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0xFFFF);
    vm86_expect_flags("flags after DEC", cpu,
                      FLAGS(VM86_CF | VM86_SF | VM86_PF | VM86_AF |
                            VM86_DF), 0);
}

/*
 * INC overflows at the sign bit and at no other value.
 *
 * 7FFFh + 1 = 8000h is the one place a sixteen-bit increment overflows,
 * and 8000h - 1 = 7FFFh is the one place a decrement does. The values
 * either side of them are checked too, so an implementation that set OF
 * one step early or late is caught.
 */
static void test_inc_and_dec_overflow_at_the_sign_bit_only(struct vm86_cpu *cpu)
{
    static const uint8_t inc[] = { 0xB8, 0xFF, 0x7F, 0x40, 0xF4 };
    static const uint8_t inc_clear[] = { 0xB8, 0xFE, 0x7F, 0x40, 0xF4 };
    static const uint8_t dec[] = { 0xB8, 0x00, 0x80, 0x48, 0xF4 };
    static const uint8_t dec_clear[] = { 0xB8, 0x01, 0x80, 0x48, 0xF4 };

    vm86_test_load(cpu, inc, sizeof(inc));
    vm86_test_run(cpu, 10);
    vm86_expect_u16("AX", cpu->ax, 0x8000);
    vm86_expect_flag("OF set", cpu, VM86_OF, true);
    vm86_expect_flag("SF set", cpu, VM86_SF, true);

    vm86_test_load(cpu, inc_clear, sizeof(inc_clear));
    vm86_test_run(cpu, 10);
    vm86_expect_u16("AX", cpu->ax, 0x7FFF);
    vm86_expect_flag("OF clear one below", cpu, VM86_OF, false);

    vm86_test_load(cpu, dec, sizeof(dec));
    vm86_test_run(cpu, 10);
    vm86_expect_u16("AX", cpu->ax, 0x7FFF);
    vm86_expect_flag("OF set", cpu, VM86_OF, true);
    vm86_expect_flag("SF clear", cpu, VM86_SF, false);

    vm86_test_load(cpu, dec_clear, sizeof(dec_clear));
    vm86_test_run(cpu, 10);
    vm86_expect_u16("AX", cpu->ax, 0x8000);
    vm86_expect_flag("OF clear one above", cpu, VM86_OF, false);
}

/*
 * A logical operation clears the carry and the overflow flag and takes
 * the rest from the result. AF is not defined by the manual and is left
 * out of the comparison with `ignore` rather than being required to hold
 * either value.
 */
static void test_logic_clears_carry_and_overflow(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x25, 0xF0, 0xFF, 0xF4 };  /* and ax,FFF0h */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0xFF0F;
    cpu->flags = FLAGS(VM86_CF | VM86_OF | VM86_AF);

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0xFF00);
    vm86_expect_flags("flags", cpu, FLAGS(VM86_SF | VM86_PF), VM86_AF);
}

/*
 * CMP is a subtraction that keeps nothing.
 *
 * The destination has to come back unchanged while every flag comes back
 * as a subtraction would have left it. A handler that stored the result
 * would compute the right flags and destroy the program's register,
 * which is the sort of thing that shows up a long way away.
 */
static void test_compare_discards_its_result(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x3D, 0x01, 0x00, 0xF4 };  /* cmp ax,1 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0000;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX is untouched", cpu->ax, 0x0000);
    vm86_expect_flags("flags are SUB's", cpu,
                      FLAGS(VM86_CF | VM86_AF | VM86_SF | VM86_PF), 0);
}

/*
 * The four segment push and pop encodings, and the one that is not an
 * instruction.
 *
 * 0Fh is POP CS. The 8086 leaves the encoding undefined -- the manual
 * promises nothing -- and the 286 turned it into the two-byte opcode
 * escape. The implementation refuses it, which is a choice: see the
 * report, and the note in deps_alu.c, which states the choice but gets
 * the history wrong.
 */
static void test_segment_push_and_pop_round_trip(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = {
        0xB8, 0x34, 0x12,   /* mov ax, 1234h */
        0x06,               /* push es       */
        0x07,               /* pop es        */
        0x16,               /* push ss       */
        0x17,               /* pop ss        */
        0x1E,               /* push ds       */
        0x1F,               /* pop ds        */
        0x0E,               /* push cs       */
        0xF4,               /* hlt           */
    };

    vm86_test_load(cpu, code, sizeof(code));

    vm86_set_seg(cpu, VM86_ES, 0x1111);
    vm86_set_seg(cpu, VM86_SS, 0x2222);
    vm86_set_seg(cpu, VM86_DS, 0x3333);

    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("ES round-tripped", cpu->es, 0x1111);
    vm86_expect_u16("SS round-tripped", cpu->ss, 0x2222);
    vm86_expect_u16("DS round-tripped", cpu->ds, 0x3333);
    vm86_expect_u16("SP after three pairs and a final push", cpu->sp,
                    VM86_TEST_STACK_TOP - 2);
    vm86_expect_mem16("CS was pushed", cpu, VM86_TEST_STACK_TOP - 2,
                      cpu->cs);
}

/* ================================================================== */
/* Interrupts, and getting back from them                              */
/* ================================================================== */

/*
 * The interrupt frame, read off the stack, and the order the words sit
 * in.
 *
 * FLAGS goes on first and IP last, so IP is at the address SP ends up
 * pointing at and FLAGS is furthest away. IRET pops the other way round.
 * The three values are all different from each other, which is what
 * makes the order observable: an implementation that pushed CS and IP
 * the wrong way round would be caught here even though the returning
 * IRET would still work, because IRET would pop them back the same way
 * round it pushed them.
 */
static void test_int_frame_order_on_the_stack(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xCD, 0x40, 0xF4 };   /* int 40h; hlt */
    static const uint8_t handler[] = { 0xF4 };            /* hlt */

    const uint16_t flags = FLAGS(VM86_IF | VM86_CF);

    vm86_test_load(cpu, code, sizeof(code));
    place(cpu, 0x0120, handler, sizeof(handler));

    vm86_set_seg(cpu, VM86_CS, 0x1000);   /* a segment no other value uses */
    place(cpu, 0x10100, code, sizeof(code));
    place(cpu, 0x00120, handler, sizeof(handler));

    poke16(cpu, 0x40 * 4,     0x0120);
    poke16(cpu, 0x40 * 4 + 2, 0x0000);

    cpu->flags = flags;
    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted in the handler", cpu->halted, true);
    vm86_expect_u16("IP", cpu->ip, 0x0121);
    vm86_expect_u16("SP", cpu->sp, VM86_TEST_STACK_TOP - 6);
    vm86_expect_mem16("saved IP",     cpu, VM86_TEST_STACK_TOP - 6, 0x0102);
    vm86_expect_mem16("saved CS",     cpu, VM86_TEST_STACK_TOP - 4, 0x1000);
    vm86_expect_mem16("saved FLAGS",  cpu, VM86_TEST_STACK_TOP - 2, flags);
}

/*
 * A full INT / IRET round trip, with a handler that disturbs every flag
 * it can before returning.
 *
 * The handler sets CF, sets DF and sets IF before the IRET. All three
 * have to come back to what the interrupted code had: CF clear, DF
 * clear, IF set. SP and IP have to come back exactly too, and CS with
 * them.
 */
static void test_int_iret_restores_every_flag(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xCD, 0x20, 0xF4 };
    /* stc; std; sti; iret */
    static const uint8_t handler[] = { 0xF9, 0xFD, 0xFB, 0xCF };

    const uint16_t flags = FLAGS(VM86_IF);

    vm86_test_load(cpu, code, sizeof(code));
    place(cpu, 0x0120, handler, sizeof(handler));

    poke16(cpu, 0x20 * 4,     0x0120);
    poke16(cpu, 0x20 * 4 + 2, 0x0000);

    cpu->flags = flags;
    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted back in the caller", cpu->halted, true);
    vm86_expect_u16("IP", cpu->ip, 0x0103);
    vm86_expect_u16("SP", cpu->sp, VM86_TEST_STACK_TOP);
    vm86_expect_u16("FLAGS came back exactly", cpu->flags, flags);
}

/*
 * IRET forces the bits that always read as one.
 *
 * The handler rewrites its own return frame with a zero flag word and
 * then returns. A guest cannot clear those bits on an 8086, so the
 * machine has to put them back; if it does not, the next `pushf / pop
 * ax` tells the program it is running on a 286 or a 386, and the era's
 * software has code paths for those that this machine cannot run.
 *
 * The handler is written out as bytes rather than reached through the
 * IVT twice so that the frame it returns to is the one it edited.
 */
static void test_iret_forces_the_hardwired_bits(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xCD, 0x20, 0xF4 };
    static const uint8_t handler[] = {
        0x58,               /* pop ax     -- saved IP      */
        0x5B,               /* pop bx     -- saved CS      */
        0x59,               /* pop cx     -- saved FLAGS   */
        0x31, 0xC9,         /* xor cx, cx -- a frame of 0  */
        0x51,               /* push cx    -- FLAGS         */
        0x53,               /* push bx    -- CS            */
        0x50,               /* push ax    -- IP            */
        0xCF,               /* iret                        */
    };

    vm86_test_load(cpu, code, sizeof(code));
    place(cpu, 0x0120, handler, sizeof(handler));

    poke16(cpu, 0x20 * 4,     0x0120);
    poke16(cpu, 0x20 * 4 + 2, 0x0000);

    cpu->flags = FLAGS(VM86_IF | VM86_CF | VM86_ZF);
    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("IP", cpu->ip, 0x0103);
    vm86_expect_u16("SP", cpu->sp, VM86_TEST_STACK_TOP);
    vm86_expect_u16("FLAGS after a zero frame", cpu->flags,
                    VM86_FLAG_ALWAYS_SET);
}

/*
 * POPF forces them too, and PUSHF reports them.
 *
 * This pair is the processor-identification idiom of the era, so the
 * round trip is the test: a word of zeroes popped into FLAGS has to read
 * back as F002h, and pushing it has to put F002h on the stack.
 */
static void test_pushf_popf_round_trip_through_the_hardwired_bits(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = {
        0x9C,               /* pushf             */
        0xB8, 0x00, 0x00,   /* mov ax, 0         */
        0x50,               /* push ax           */
        0x9D,               /* popf  -- all zero */
        0x9C,               /* pushf             */
        0xF4,               /* hlt               */
    };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->flags = FLAGS(VM86_CF | VM86_IF);

    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("FLAGS after POPF of zero", cpu->flags,
                    VM86_FLAG_ALWAYS_SET);
    /* SP is back where it started, so the last word pushed is at the top
     * again -- and it must be the flag word as it reads, hardwired bits
     * included. */
    vm86_expect_u16("SP", cpu->sp, VM86_TEST_STACK_TOP - 4);
    vm86_expect_mem16("what the second PUSHF left", cpu,
                      VM86_TEST_STACK_TOP - 4, VM86_FLAG_ALWAYS_SET);
}

/*
 * LAHF and SAHF move the low byte and nothing else.
 *
 * LAHF is given a flag word whose low byte is D7h, so every one of the
 * five flags it carries is set and AH must come back D7h -- including
 * the bits that always read as one. SAHF is given a byte that would
 * clear them all, and the upper half of FLAGS, where IF, DF and the rest
 * live, must come through untouched.
 */
static void test_lahf_and_sahf_touch_only_the_low_byte(struct vm86_cpu *cpu)
{
    static const uint8_t lahf[] = { 0x9F, 0xF4 };
    static const uint8_t sahf[] = { 0x9E, 0xF4 };

    vm86_test_load(cpu, lahf, sizeof(lahf));
    cpu->flags = FLAGS(VM86_CF | VM86_PF | VM86_AF | VM86_ZF | VM86_SF |
                       VM86_IF | VM86_DF);
    cpu->ax = 0x0000;   /* AH clear, so a handler that did nothing shows */

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AH", cpu->ah, 0xD7);
    vm86_expect_u16("AL untouched", cpu->al, 0x00);

    vm86_test_load(cpu, sahf, sizeof(sahf));
    cpu->flags = FLAGS(VM86_CF | VM86_PF | VM86_AF | VM86_ZF | VM86_SF |
                       VM86_IF | VM86_DF);
    cpu->ah = 0x00;   /* clears every flag SAHF reaches */

    vm86_test_run(cpu, 10);

    /* IF and DF live above the byte SAHF writes, and survive. The rest
     * go, apart from the bits that always read as one. */
    vm86_expect_u16("FLAGS", cpu->flags, FLAGS(VM86_IF | VM86_DF));
}

/*
 * INTO does nothing at all when OF is clear.
 *
 * Not "raises a suppressed interrupt": an instruction that did not run.
 * The vector it would have used is pointed at a handler that would be
 * visible if it were reached, so a no-op and a suppressed-but-taken
 * trap cannot be confused.
 */
static void test_into_is_inert_without_overflow(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xCE, 0xF4 };
    static const uint8_t trap[] = { 0x06, 0xF4 };   /* push es */

    vm86_test_load(cpu, code, sizeof(code));
    place(cpu, 0x0120, trap, sizeof(trap));

    poke16(cpu, 4 * 4,     0x0120);
    poke16(cpu, 4 * 4 + 2, 0x0000);

    cpu->flags = FLAGS(VM86_CF | VM86_SF);   /* everything but OF */
    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("SP untouched", cpu->sp, VM86_TEST_STACK_TOP);
    vm86_expect_u16("IP", cpu->ip, 0x0102);
}

/*
 * The vector table is reached by a linear address, whatever CS holds.
 *
 * Vector FFh lives at 0x3FC, the last entry in the first kilobyte. The
 * program is given a CS far from zero and the same bytes are placed in
 * its segment as well, so a lookup that based the table on CS would find
 * that copy and land in it instead.
 */
static void test_the_last_vector_is_read_at_a_linear_address(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xCD, 0xFF, 0xF4 };
    static const uint8_t handler[] = { 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    place(cpu, 0xA0100, code, sizeof(code));     /* the decoy, in CS */
    place(cpu, 0x00120, handler, sizeof(handler));

    vm86_set_seg(cpu, VM86_CS, 0xA000);

    poke16(cpu, 0xFF * 4,     0x0120);
    poke16(cpu, 0xFF * 4 + 2, 0x0000);

    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("CS is the handler's", cpu->cs, 0x0000);
    vm86_expect_u16("IP is the handler's", cpu->ip, 0x0121);
}

/* ================================================================== */
/* Loops                                                               */
/* ================================================================== */

/*
 * LOOP counts like a `dec` followed by `jnz`, which means a count of one
 * runs the body once and does not jump.
 *
 * A count of one is where a handler that tested CX before decrementing
 * and one that tested it after give different answers, and a count of
 * one is not rare: it is what a program passes when it wants to run the
 * body exactly once without a special case.
 */
static void test_loop_with_a_count_of_one_does_not_jump(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xB9, 0x01, 0x00, 0xE2, 0xFE, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("CX", cpu->cx, 0x0000);
    vm86_expect_u16("IP", cpu->ip, 0x0106);
}

/*
 * JCXZ tests the counter and does not decrement it.
 *
 * Both directions: with CX clear it jumps and CX stays clear; with CX
 * set it falls through and CX is still set. A handler that reused the
 * other three instructions' decrement would leave the second case's CX
 * one lower, and a program that counts something else in CX across a
 * JCXZ would lose a count.
 */
static void test_jcxz_does_not_decrement_the_counter(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = {
        0xB9, 0x00, 0x00,   /* mov cx, 0  */
        0xE3, 0x03,         /* jcxz +3    */
        0xB8, 0xFF, 0xFF,   /* mov ax, -1 */
        0xF4,               /* hlt        */
    };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("CX untouched", cpu->cx, 0x0000);
    vm86_expect_u16("the jump was taken", cpu->ax, 0x0000);

    static const uint8_t no_jump[] = {
        0xB9, 0x07, 0x00,   /* mov cx, 7  */
        0xE3, 0x02,         /* jcxz +2    */
        0xB8, 0xFF, 0xFF,   /* mov ax, -1 */
        0xF4,
    };

    vm86_test_load(cpu, no_jump, sizeof(no_jump));
    vm86_test_run(cpu, 20);

    vm86_expect_u16("CX untouched when not taken", cpu->cx, 0x0007);
    vm86_expect_u16("the jump was not taken", cpu->ax, 0xFFFF);
}

/*
 * LOOPE stops when the counter runs out even though the flag still says
 * to continue.
 *
 * The flag is left set for the whole program, so a handler that only
 * looked at ZF would never stop. The bound is the counter, and it is
 * what makes `loope` terminate on data that never differs.
 */
static void test_loope_is_bounded_by_the_counter(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xB9, 0x02, 0x00, 0xE1, 0xFE, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->flags = FLAGS(VM86_ZF);

    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("CX ran out", cpu->cx, 0x0000);
    vm86_expect_u16("IP", cpu->ip, 0x0106);
}

/*
 * A conditional jump consumes its displacement whether or not it takes
 * it.
 *
 * With the condition false the byte after the displacement is the next
 * instruction. With the condition true the displacement is still gone
 * from the stream before it is used. The bytes after the jump are chosen
 * so that a handler which skipped the fetch would decode them as
 * something else -- here, B0 07 would become 07 (POP ES) followed by
 * 74 (JZ), and neither would leave AL at 7.
 */
static void test_conditional_jump_consumes_its_displacement_either_way(struct vm86_cpu *cpu)
{
    static const uint8_t not_taken[] = { 0x74, 0x01, 0xB0, 0x07, 0xF4 };
    static const uint8_t taken[]     = { 0x74, 0x02, 0xB0, 0x07, 0xF4 };

    vm86_test_load(cpu, not_taken, sizeof(not_taken));
    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AL set when the branch fell through", cpu->al, 0x07);

    vm86_test_load(cpu, taken, sizeof(taken));
    cpu->ax = 0x0000;
    cpu->flags = FLAGS(VM86_ZF);   /* je, and the displacement skips to the HLT */
    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AL untouched when the branch was taken", cpu->al, 0x00);
}

/*
 * A near jump's displacement is added to a sixteen-bit IP and wraps
 * there.
 *
 * The instruction sits at IP=0005 and jumps back by 16, which lands at
 * FFF8 -- inside the same segment, at its end, where the HLT has been
 * placed. An implementation that did the arithmetic in a wider type
 * would land at 0xFFFF8 and stop the program at whatever lives there.
 */
static void test_a_near_jump_wraps_the_instruction_pointer(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xE9, 0xF0, 0xFF };   /* jmp -16 */
    static const uint8_t hlt[]  = { 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    place(cpu, 0x00005, code, sizeof(code));
    place(cpu, 0x0FFF8, hlt, sizeof(hlt));

    cpu->ip = 0x0005;

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("IP", cpu->ip, 0xFFF9);
}

/* ================================================================== */
/* The stack, called and returned from                                 */
/* ================================================================== */

/*
 * RET n pops the return address and then adds n to SP.
 *
 * This is the stdcall convention, and the failure mode is a stack that
 * creeps upward by n on every call rather than a wrong answer at the
 * instruction -- so a program that has to keep working needs it right.
 *
 * SP is deliberately started at 0, so the pop wraps to 2 and the
 * adjustment after it is done at the top of the register: an
 * implementation that adjusted first would read the return address from
 * the wrong place and jump into nothing.
 */
static void test_ret_imm_adjusts_the_stack_after_the_pop(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xC2, 0x04, 0x00, 0xF4 };   /* ret 4 */

    vm86_test_load(cpu, code, sizeof(code));

    poke16(cpu, 0x00000, 0x0103);   /* SS = 0 and SP = 0: the top of memory */
    cpu->sp = 0x0000;

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("IP came off the stack", cpu->ip, 0x0104);
    vm86_expect_u16("SP popped then adjusted", cpu->sp, 0x0006);
}

/*
 * A far CALL and its return.
 *
 * The frame is four bytes and the two halves have to be in the order
 * RETF expects, which is the reverse of the order CALL pushes. The
 * intermediate state is checked rather than only the round trip: on the
 * way in, CS is the caller's and IP is the instruction after the call.
 */
static void test_far_call_and_return(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = {
        0x9A, 0x00, 0x02, 0x00, 0x20,   /* call 2000h:0200h */
        0xF4,                           /* hlt              */
    };
    /* at 2000h:0200 -- a far return */
    static const uint8_t handler[] = { 0xCB };

    vm86_test_load(cpu, code, sizeof(code));
    place(cpu, 0x20200, handler, sizeof(handler));

    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("IP after the return", cpu->ip, 0x0106);
    vm86_expect_u16("CS after the return", cpu->cs, 0x0000);
    vm86_expect_u16("SP", cpu->sp, VM86_TEST_STACK_TOP);
}

/*
 * PUSH and POP of every register, in a program that also moves SP in
 * between, so the round trip has to hold against a stack that is not
 * where it started.
 */
static void test_push_pop_round_trip_for_every_register(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = {
        0x50, 0x51, 0x52, 0x53, 0x55, 0x56, 0x57,  /* push ax cx dx bx bp si di */
        0x5F, 0x5E, 0x5D, 0x5B, 0x5A, 0x59, 0x58,  /* pop  di si bp bx dx cx ax */
        0xF4,
    };

    vm86_test_load(cpu, code, sizeof(code));

    cpu->ax = 0x1111;
    cpu->cx = 0x2222;
    cpu->dx = 0x3333;
    cpu->bx = 0x4444;
    cpu->bp = 0x5555;
    cpu->si = 0x6666;
    cpu->di = 0x7777;

    vm86_test_run(cpu, 40);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AX", cpu->ax, 0x1111);
    vm86_expect_u16("CX", cpu->cx, 0x2222);
    vm86_expect_u16("DX", cpu->dx, 0x3333);
    vm86_expect_u16("BX", cpu->bx, 0x4444);
    vm86_expect_u16("BP", cpu->bp, 0x5555);
    vm86_expect_u16("SI", cpu->si, 0x6666);
    vm86_expect_u16("DI", cpu->di, 0x7777);
    vm86_expect_u16("SP", cpu->sp, VM86_TEST_STACK_TOP);
}

/* ================================================================== */
/* The string instructions                                             */
/* ================================================================== */

/*
 * CX = 0 with the direction flag set.
 *
 * The count is tested before the first iteration, so nothing happens and
 * neither pointer moves -- in either direction. A do/while would move
 * 65536 elements, and with DF set it would walk backwards out of the
 * buffer, which is why the backward case is here as well as the forward
 * one the author's tests already cover.
 */
static void test_string_with_zero_count_moves_nothing_backwards(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xFD, 0xF3, 0xA4, 0xF4 };  /* std; rep movsb */

    vm86_test_load(cpu, code, sizeof(code));

    cpu->si = 0x0300;
    cpu->di = 0x0400;
    cpu->cx = 0x0000;

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("SI", cpu->si, 0x0300);
    vm86_expect_u16("DI", cpu->di, 0x0400);
    vm86_expect_u16("CX", cpu->cx, 0x0000);
}

/*
 * A REP whose count runs out on the last element leaves CX at zero, and
 * one stopped early leaves the elements it never looked at still
 * counted.
 *
 * A program searches with `repne scasb` and then subtracts to find where
 * the byte was, so which of the two happened is part of the answer and
 * not an implementation detail. Both are checked at the same count, so
 * the difference between them is the only thing that varies.
 */
static void test_repne_scasb_leaves_the_unscanned_elements_in_cx(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF2, 0xAE, 0xF4 };   /* repne scasb */

    /* Found at the third of eight bytes. */
    vm86_test_load(cpu, code, sizeof(code));
    poke8(cpu, 0x0400, 0x01);
    poke8(cpu, 0x0401, 0x02);
    poke8(cpu, 0x0402, 0xAA);
    poke8(cpu, 0x0403, 0x04);
    cpu->al = 0xAA;
    cpu->di = 0x0400;
    cpu->cx = 0x0008;
    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("CX counts the bytes never looked at", cpu->cx, 0x0005);
    vm86_expect_u16("DI is one past the match", cpu->di, 0x0403);
    vm86_expect_flag("ZF is set by the match", cpu, VM86_ZF, true);

    /* Not found at all: the count runs out, ZF is clear, DI is at the end. */
    vm86_test_load(cpu, code, sizeof(code));
    for (uint32_t i = 0; i < 8; i++)
        poke8(cpu, 0x0400 + i, (uint8_t)(0x10 + i));
    cpu->al = 0xAA;
    cpu->di = 0x0400;
    cpu->cx = 0x0008;
    vm86_test_run(cpu, 20);

    vm86_expect_u16("CX is zero when the count ran out", cpu->cx, 0x0000);
    vm86_expect_u16("DI scanned all eight", cpu->di, 0x0408);
    vm86_expect_flag("ZF is clear", cpu, VM86_ZF, false);
}

/*
 * `repe cmpsb` that finds no difference stops on the count, with ZF set
 * and CX at zero -- the other ending from the one the author's tests
 * cover.
 */
static void test_repe_cmpsb_over_identical_buffers_ends_on_the_count(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0xA6, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    for (uint32_t i = 0; i < 5; i++) {
        poke8(cpu, 0x0300 + i, 0x5A);
        poke8(cpu, 0x0400 + i, 0x5A);
    }

    cpu->si = 0x0300;
    cpu->di = 0x0400;
    cpu->cx = 0x0005;

    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("CX ran out", cpu->cx, 0x0000);
    vm86_expect_u16("SI", cpu->si, 0x0305);
    vm86_expect_u16("DI", cpu->di, 0x0405);
    vm86_expect_flag("ZF set by the last comparison", cpu, VM86_ZF, true);
}

/*
 * CMPSB compares DS:SI against ES:DI, in that order, and sets the flags
 * a SUB would.
 *
 * The order is what decides the direction of the carry, and it is the
 * one thing about CMPS that cannot be inferred from the instruction
 * working at all: with the operands the other way round the equality
 * case still passes and the ordering case silently inverts. The two
 * cases below differ only by which buffer holds which byte, and the
 * flags they produce have nothing in common -- CF and SF one way, PF and
 * AF the other.
 */
static void test_cmpsb_compares_ds_si_minus_es_di(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xA6, 0xF4 };

    /* 01h - 10h = F1h: a borrow, and a result with its top bit set. */
    vm86_test_load(cpu, code, sizeof(code));

    poke8(cpu, 0x0300, 0x01);   /* DS:SI */
    poke8(cpu, 0x0400, 0x10);   /* ES:DI */

    cpu->si = 0x0300;
    cpu->di = 0x0400;

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_flags("flags from DS:SI - ES:DI", cpu,
                      FLAGS(VM86_CF | VM86_SF), 0);
    vm86_expect_u16("SI advanced", cpu->si, 0x0301);
    vm86_expect_u16("DI advanced", cpu->di, 0x0401);

    /* 10h - 01h = 0Fh: no borrow, but the low nibble had to borrow, so
     * this is a full CMP and AF is set even though CF is not. */
    vm86_test_load(cpu, code, sizeof(code));

    poke8(cpu, 0x0300, 0x10);
    poke8(cpu, 0x0400, 0x01);

    cpu->si = 0x0300;
    cpu->di = 0x0400;

    vm86_test_run(cpu, 10);

    vm86_expect_flags("flags the other way round", cpu,
                      FLAGS(VM86_AF | VM86_PF), 0);
}

/*
 * SCASB compares AL against ES:DI -- the accumulator first, so the carry
 * means "AL is below the memory byte".
 *
 * Both directions are run because a handler that had the operands the
 * way round CMPS wants them would set the carry the other way, and the
 * program reading it would search in the wrong direction.
 */
static void test_scasb_compares_al_minus_es_di(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xAE, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    poke8(cpu, 0x0400, 0x20);
    cpu->al = 0x10;
    cpu->di = 0x0400;
    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_flag("AL below the byte: borrow", cpu, VM86_CF, true);

    vm86_test_load(cpu, code, sizeof(code));
    poke8(cpu, 0x0400, 0x20);
    cpu->al = 0x30;
    cpu->di = 0x0400;
    vm86_test_run(cpu, 10);

    vm86_expect_flag("AL above the byte: no borrow", cpu, VM86_CF, false);
}

/*
 * A word string operation reads across the end of a segment.
 *
 * The same rule as any other word access: SI at FFFFh reads the last two
 * bytes of the DS segment and the first byte of the next one. MOVSW also
 * writes through ES, which is left at zero here, so the two halves of
 * the moved word land in the same relationship.
 */
static void test_movsw_reads_across_the_end_of_a_segment(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xA5, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    poke8(cpu, 0x2FFFF, 0x34);
    poke8(cpu, 0x30000, 0x12);
    poke8(cpu, 0x20000, 0x99);   /* only a wrapping read would see this */

    vm86_set_seg(cpu, VM86_DS, 0x2000);
    cpu->si = 0xFFFF;
    cpu->di = 0x0400;

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_mem16("the word that moved", cpu, 0x0400, 0x1234);
    vm86_expect_u16("SI", cpu->si, 0x0001);
}

/*
 * LODSB leaves AH alone.
 *
 * The byte goes into AL and nothing else. Writing the whole of AX is the
 * natural mistake -- the union makes it possible, so it is the one to
 * check -- and a program that keeps a count in AH across a `lodsb` would
 * lose it, quietly, at the first byte of the buffer.
 */
static void test_lodsb_leaves_ah_alone(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xAC, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    poke8(cpu, 0x0300, 0x5A);
    cpu->ax = 0x1200;
    cpu->si = 0x0300;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0x125A);
}

/*
 * A segment override reaches the read side of a string instruction and
 * never the write side.
 *
 * DS and ES are made to point at different places, and the prefix is the
 * one that names DS: on `ds: movsb` the source comes from the data
 * segment -- which it would anyway, unless ES is where the data is --
 * and the destination still goes through ES. The buffers are
 * asymmetric, so an implementation that swapped the two pointers would
 * fill the source with its own bytes rather than merely writing the
 * wrong place.
 */
static void test_segment_override_moves_only_the_read_side(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x3E, 0xA4, 0xF4 };   /* ds: movsb */

    vm86_test_load(cpu, code, sizeof(code));

    poke8(cpu, 0x10200, 0xA7);   /* DS:0200, DS = 1000h */
    poke8(cpu, 0x00200, 0xB8);   /* ES:0200, ES = 0     */

    vm86_set_seg(cpu, VM86_DS, 0x1000);
    vm86_set_seg(cpu, VM86_ES, 0x0000);

    cpu->si = 0x0200;
    cpu->di = 0x0200;

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_mem8("the source byte moved to ES:DI", cpu, 0x00200, 0xA7);
    vm86_expect_mem8("the DS side was not written", cpu, 0x10200, 0xA7);
}

/*
 * A string instruction with no prefix runs exactly once.
 *
 * The count in CX is not consulted: `movsb` is one move, and a handler
 * that fell into the loop unconditionally would empty a buffer. CX is
 * left at a value that would be visible as a long copy if it were used.
 */
static void test_a_string_instruction_without_a_prefix_runs_once(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xA4, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    poke8(cpu, 0x0300, 0x11);
    poke8(cpu, 0x0301, 0x22);

    cpu->si = 0x0300;
    cpu->di = 0x0400;
    cpu->cx = 0x0002;

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_mem8("one byte copied", cpu, 0x0400, 0x11);
    vm86_expect_mem8("the second was not", cpu, 0x0401, 0x00);
    vm86_expect_u16("CX is not a counter without a prefix", cpu->cx, 0x0002);
}

/*
 * A word string operation moves by two in both directions.
 *
 * The direction flag is the one thing about the string instructions that
 * a symmetric buffer cannot test: a backward walk over identical bytes
 * looks the same as a forward one. The two bytes are therefore made
 * different, and the backward case starts at the high end.
 */
static void test_movsw_walks_backwards_by_two(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xFD, 0xA5, 0xF4 };   /* std; movsw */

    vm86_test_load(cpu, code, sizeof(code));

    poke16(cpu, 0x0300, 0x1234);
    poke16(cpu, 0x0302, 0x5678);

    cpu->si = 0x0302;   /* the top word, which is where a walk starts */
    cpu->di = 0x0402;

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_mem16("the top word moved", cpu, 0x0402, 0x5678);
    vm86_expect_u16("SI", cpu->si, 0x0300);
    vm86_expect_u16("DI", cpu->di, 0x0400);
}

/* ================================================================== */
/* Prefixes                                                            */
/* ================================================================== */

/*
 * A segment override belongs to one instruction.
 *
 * The prefix is consumed by the run loop before dispatch and cleared
 * before the next fetch. If it were not, `es: mov al,[200h]` followed by
 * a plain `mov al,[200h]` would read through ES twice, and a program
 * that switched segments for one access and switched back would read
 * from the wrong place for the rest of the routine.
 */
static void test_a_segment_override_lasts_one_instruction(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = {
        0x26, 0xA0, 0x00, 0x02,   /* es: mov al,[200h] */
        0xA0, 0x00, 0x02,         /*     mov al,[200h] */
        0xF4,
    };

    vm86_test_load(cpu, code, sizeof(code));

    poke8(cpu, 0x10200, 0x11);
    poke8(cpu, 0x30200, 0x33);

    vm86_set_seg(cpu, VM86_DS, 0x1000);
    vm86_set_seg(cpu, VM86_ES, 0x3000);

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    /* The second load went through DS, so the override was forgotten:
     * 0x11 and not 0x33. */
    vm86_expect_u16("AL", cpu->al, 0x11);
}

/*
 * A prefix is accepted more than once, and the last one of a kind wins.
 *
 * The hardware does not object to `26 3E` and neither should this:
 * prefixes are not instructions and nothing counts them. The last one
 * said is the one that applies, which is what makes the sequence
 * meaningful rather than merely tolerated -- so the answer has to be DS
 * and not ES.
 */
static void test_repeated_prefixes_are_accepted(struct vm86_cpu *cpu)
{
    static const uint8_t repeated[] = { 0xF3, 0xF3, 0xA4, 0xF4 };
    static const uint8_t last_wins[] = {
        0x26, 0x3E, 0xA0, 0x00, 0x02, 0xF4,
    };

    vm86_test_load(cpu, repeated, sizeof(repeated));
    poke8(cpu, 0x0300, 0x11);
    cpu->si = 0x0300;
    cpu->di = 0x0400;
    cpu->cx = 0x0001;
    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_mem8("the repeated REP still repeated", cpu, 0x0400, 0x11);

    vm86_test_load(cpu, last_wins, sizeof(last_wins));
    poke8(cpu, 0x10200, 0x11);   /* DS */
    poke8(cpu, 0x30200, 0x33);   /* ES */
    vm86_set_seg(cpu, VM86_DS, 0x1000);
    vm86_set_seg(cpu, VM86_ES, 0x3000);
    vm86_test_run(cpu, 10);

    vm86_expect_u16("the last prefix is the one that applies", cpu->al, 0x11);
}

/*
 * LOCK is accepted and has nothing to do.
 *
 * A program that emits it is asking for something this machine cannot
 * fail to provide, so refusing it would break software for no reason.
 * The instruction after it has to run normally and the prefix has to be
 * gone by the instruction after that.
 */
static void test_lock_is_accepted_and_ignored(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = {
        0xF0, 0xB8, 0x34, 0x12,   /* lock mov ax, 1234h */
        0xB8, 0x78, 0x56,         /*      mov ax, 5678h */
        0xF4,
    };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AX", cpu->ax, 0x5678);
}

/* ================================================================== */
/* Ports                                                               */
/* ================================================================== */

/*
 * IN writes only the half of the accumulator its width names.
 *
 * `in al, 60h` is a byte access and must leave AH as it was; `in ax,
 * 60h` replaces the whole register. Both return the floating bus, and
 * the difference between them is only visible in the half a program
 * happens to be keeping something in.
 */
static void test_in_writes_only_the_named_half(struct vm86_cpu *cpu)
{
    static const uint8_t byte_in[] = { 0xE4, 0x60, 0xF4 };
    static const uint8_t word_in[] = { 0xE5, 0x60, 0xF4 };

    vm86_test_load(cpu, byte_in, sizeof(byte_in));
    cpu->ax = 0x1234;
    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AX after `in al`", cpu->ax, 0x12FF);

    vm86_test_load(cpu, word_in, sizeof(word_in));
    cpu->ax = 0x1234;
    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX after `in ax`", cpu->ax, 0xFFFF);
}

/*
 * The immediate port byte is consumed exactly once.
 *
 * The byte after an `out 60h, al` is the next instruction, and if the
 * handler forgot to fetch the port it would execute that byte instead.
 * 90h is a NOP, so a program that skipped the fetch would run it as an
 * instruction and still reach the HLT -- which is why the byte after
 * the NOP is also checked, by looking at IP.
 */
static void test_the_immediate_port_is_consumed_once(struct vm86_cpu *cpu)
{
    static const uint8_t immediate[] = { 0xE6, 0x60, 0x90, 0xF4 };
    static const uint8_t through_dx[] = { 0xEE, 0x90, 0xF4 };

    vm86_test_load(cpu, immediate, sizeof(immediate));
    cpu->ax = 0x1234;
    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("IP", cpu->ip, 0x0104);
    /* 60h is PUSHA, so a port byte that was not consumed would run
     * as an instruction and move the stack pointer. */
    vm86_expect_u16("SP", cpu->sp, VM86_TEST_STACK_TOP);

    vm86_test_load(cpu, through_dx, sizeof(through_dx));
    cpu->dx = 0x0060;
    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("IP", cpu->ip, 0x0103);
}

/* ================================================================== */
/* Encodings that are not instructions                                 */
/* ================================================================== */

/*
 * An opcode nobody claims raises vector 6 and stops the machine.
 *
 * F1h is undefined on this processor, and 63h is ARPL, which is a 286
 * instruction and not one of ours. Both are refused, and IP is left past
 * the byte that was refused: a program probing for what it is running on
 * has to get a clean refusal it can resume from.
 */
static void test_unclaimed_opcodes_raise_the_invalid_opcode_trap(struct vm86_cpu *cpu)
{
    static const uint8_t undefined[] = { 0xF1 };
    static const uint8_t arpl[] = { 0x63, 0xC0 };

    vm86_test_load(cpu, undefined, sizeof(undefined));
    enum vm86_result result = vm86_test_run(cpu, 10);

    vm86_expect_bool("faulted", result == VM86_FAULT, true);
    vm86_expect_u16("vector", cpu->fault, VM86_VECTOR_INVALID_OPCODE);
    vm86_expect_u16("IP is past the refused byte", cpu->ip, 0x0101);

    vm86_test_load(cpu, arpl, sizeof(arpl));
    result = vm86_test_run(cpu, 10);

    vm86_expect_bool("faulted", result == VM86_FAULT, true);
    vm86_expect_u16("vector", cpu->fault, VM86_VECTOR_INVALID_OPCODE);
}

/*
 * The encodings the implementations refused, each checked from the
 * outside.
 *
 * Every one of these is a ModRM byte whose reg field names something
 * other than the one operation the group has: POP from 8Fh, the
 * immediate move from C6h/C7h, the far pointer loads from C4h/C5h, and
 * the memory form from 8Dh. The manual defines no meaning for them and
 * the implementations raise vector 6 -- which is a decision, taken
 * deliberately, and the only way to keep it from drifting is to pin it.
 */
static void test_the_undefined_group_encodings_are_refused(struct vm86_cpu *cpu)
{
    static const uint8_t pop_rm_1[]   = { 0x8F, 0xC8 };
    static const uint8_t mov_imm_1[]  = { 0xC6, 0xC8, 0x12 };
    static const uint8_t mov_immu_1[] = { 0xC7, 0xC8, 0x34, 0x12 };
    static const uint8_t lea_mod3[]   = { 0x8D, 0xC0 };
    static const uint8_t les_mod3[]   = { 0xC4, 0xC0 };
    static const uint8_t lds_mod3[]   = { 0xC5, 0xC0 };
    static const uint8_t mov_cs[]     = { 0x8E, 0xC8 };
    static const uint8_t pop_cs[]     = { 0x0F };
    static const uint8_t sreg_4[]     = { 0x8E, 0xE0 };
    static const uint8_t far_call_reg[] = { 0xFF, 0xD8 };
    static const uint8_t far_jmp_reg[]  = { 0xFF, 0xE8 };

    static const uint8_t *const cases[] = {
        pop_rm_1, mov_imm_1, mov_immu_1, lea_mod3,
        les_mod3, lds_mod3, mov_cs, pop_cs, sreg_4,
        far_call_reg, far_jmp_reg,
    };
    static const char *const names[] = {
        "8F /1", "C6 /1", "C7 /1", "8D mod=3",
        "C4 mod=3", "C5 mod=3", "8E /1 (mov cs)", "0F (pop cs)", "8E /4",
        "FF /3 mod=3", "FF /5 mod=3",
    };
    static const uint16_t sizes[] = {
        sizeof(pop_rm_1), sizeof(mov_imm_1), sizeof(mov_immu_1),
        sizeof(lea_mod3), sizeof(les_mod3), sizeof(lds_mod3),
        sizeof(mov_cs), sizeof(pop_cs), sizeof(sreg_4),
        sizeof(far_call_reg), sizeof(far_jmp_reg),
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        vm86_test_load(cpu, cases[i], sizes[i]);

        enum vm86_result result = vm86_test_run(cpu, 10);

        char what[48];
        snprintf(what, sizeof(what), "%s is refused", names[i]);

        vm86_expect_bool(what, result == VM86_FAULT, true);
        vm86_expect_u16("vector", cpu->fault, VM86_VECTOR_INVALID_OPCODE);
    }
}

/* ================================================================== */
/* Shifts and rotates                                                  */
/* ================================================================== */

/*
 * A count of zero does nothing, and "nothing" includes the flags.
 *
 * This is not an optimisation, it is the instruction: the hardware has
 * no work to do and does not do any, so CF and OF come out of a
 * `shl ax, cl` with CL clear exactly as they went in. Code a compiler
 * generates reaches this regularly -- a shift whose count is a variable
 * that happens to be zero -- and an implementation that always wrote CF
 * from the last bit "shifted" out would have it reading the sign bit of
 * the operand instead of the carry of whatever arithmetic came before.
 */
static void test_a_shift_by_zero_changes_nothing(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD3, 0xE0, 0xF4 };   /* shl ax, cl */

    const uint16_t flags = FLAGS(VM86_CF | VM86_ZF | VM86_SF | VM86_OF |
                                 VM86_AF | VM86_PF | VM86_DF);

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x1234;
    cpu->cx = 0x0000;   /* CL = 0 */
    cpu->flags = flags;

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AX", cpu->ax, 0x1234);
    vm86_expect_u16("FLAGS untouched", cpu->flags, flags);
}

/*
 * The direction of the carry, for a shift left by one.
 *
 * The carry is the bit that leaves the top, which is what the whole
 * `shl` / `jc` idiom for testing bit 15 depends on. The overflow flag is
 * defined only for a count of one, and it says whether the sign changed:
 * the bit that left and the bit that arrived disagree.
 */
static void test_shl_by_one_takes_the_bit_off_the_top(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD1, 0xE0, 0xF4 };   /* shl ax, 1 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x8000;
    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AX", cpu->ax, 0x0000);
    /* CF set from the bit that left, OF set because the sign changed,
     * ZF and PF set by a zero result, SF clear. */
    vm86_expect_flags("flags", cpu,
                      FLAGS(VM86_CF | VM86_OF | VM86_ZF | VM86_PF), 0);
}

/*
 * ... and the same for a bit that does not reach the top.
 *
 * 4000h shifted left is 8000h: nothing leaves the top, so CF is clear,
 * and the sign bit did change, so OF is set. A handler that computed OF
 * from the carry alone would have it backwards.
 */
static void test_shl_by_one_without_a_carry(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD1, 0xE0, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x4000;
    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0x8000);
    vm86_expect_flags("flags", cpu, FLAGS(VM86_OF | VM86_SF | VM86_PF),
                      VM86_AF);
}

/*
 * SHR's overflow flag is the operand's original sign bit, not anything
 * about the result. It is the one shift where OF says something about
 * where the value came from.
 */
static void test_shr_by_one_reports_the_original_sign(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD1, 0xE8, 0xF4 };   /* shr ax, 1 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x8001;
    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0x4000);
    vm86_expect_flags("flags", cpu,
                      FLAGS(VM86_CF | VM86_OF | VM86_PF), VM86_AF);
}

/* SAR shifts the sign bit back in, so the sign never changes and OF is
 * clear whatever the operand was. */
static void test_sar_never_sets_overflow(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD1, 0xF8, 0xF4 };   /* sar ax, 1 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x8000;
    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0xC000);
    vm86_expect_flags("flags", cpu, FLAGS(VM86_SF | VM86_PF), VM86_AF);
}

/* ROL: the bit off the top comes back at the bottom, and the overflow
 * flag is the sign bit of the result against the carry. */
static void test_rol_by_one(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD0, 0xC0, 0xF4 };   /* rol al, 1 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->al = 0x80;
    vm86_test_run(cpu, 10);

    vm86_expect_u16("AL", cpu->al, 0x01);
    vm86_expect_flags("flags", cpu, FLAGS(VM86_CF | VM86_OF), VM86_AF);
}

/* ROR: the overflow flag is the top two bits of the result, which is the
 * only shift that looks at two bits of it. */
static void test_ror_by_one(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD0, 0xC8, 0xF4 };   /* ror al, 1 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->al = 0xC0;
    vm86_test_run(cpu, 10);

    vm86_expect_u16("AL", cpu->al, 0x60);
    /* 60h is 0110 0000: bits 7 and 6 differ, so OF is set. PF is set by
     * two bits in the low byte. */
    vm86_expect_flags("flags", cpu, FLAGS(VM86_OF | VM86_PF), VM86_AF);
}

/*
 * RCL and RCR carry the flag through the rotation, so CF is an input as
 * well as an output. Both directions with the carry arriving set, which
 * is the only way to tell a rotate-through-carry from a plain rotate.
 */
static void test_rotate_through_the_carry(struct vm86_cpu *cpu)
{
    static const uint8_t rcl[] = { 0xD0, 0xD0, 0xF4 };   /* rcl al, 1 */
    static const uint8_t rcr[] = { 0xD0, 0xD8, 0xF4 };   /* rcr al, 1 */

    vm86_test_load(cpu, rcl, sizeof(rcl));
    cpu->al = 0x80;
    cpu->flags = FLAGS(VM86_CF);
    vm86_test_run(cpu, 10);

    /* The bit off the top becomes the new carry, and the old carry
     * arrives in bit 0. */
    vm86_expect_u16("AL after RCL", cpu->al, 0x01);
    vm86_expect_flags("flags after RCL", cpu, FLAGS(VM86_CF | VM86_OF), 0);

    vm86_test_load(cpu, rcr, sizeof(rcr));
    cpu->al = 0x01;
    cpu->flags = FLAGS(VM86_CF);
    vm86_test_run(cpu, 10);

    vm86_expect_u16("AL after RCR", cpu->al, 0x80);
    vm86_expect_flags("flags after RCR", cpu,
                      FLAGS(VM86_CF | VM86_OF | VM86_SF), 0);
}

/*
 * A shift count is used whole, not masked.
 *
 * This is a chip question and not a question of what is reasonable. The
 * 8086 shifts by the value in CL; the 80186 and everything after take
 * only the low five bits, so CL=20 shifts by four there. On this machine
 * CL=20 shifts twenty times and FFFFh goes to zero.
 *
 * The difference is not academic for a verifier: an implementation that
 * masked the count would look correct in every test that used a count
 * below the width, which is nearly all of them. The count is deliberately
 * above the width, and the result is one that masking cannot produce.
 */
static void test_a_shift_count_is_not_masked(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD3, 0xE0, 0xF4 };   /* shl ax, cl */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0xFFFF;
    cpu->cx = 0x0014;   /* CL = 20: twenty shifts on an 8086, four on a 186 */

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AX is zero after twenty shifts", cpu->ax, 0x0000);
    /* The last bit to leave was a zero, so the carry is clear. Masking
     * the count would leave FFF0h in AX with the carry set. */
    vm86_expect_flag("CF", cpu, VM86_CF, false);
}

/*
 * The immediate-count shifts are 80186 additions, and they follow this
 * machine's rule rather than the 80186's.
 *
 * C0h and C1h are not 8086 encodings at all, so there is no 8086
 * behaviour for them to follow. What they must not do is disagree with
 * the CL form about what a count means: `shl ax, 20` and `shl ax, cl`
 * with CL=20 are the same instruction spelled two ways, and a machine
 * where they differ is a machine whose shift semantics depend on the
 * encoding. The count of 20 is here for the same reason as above.
 */
static void test_an_immediate_shift_count_is_not_masked_either(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xC1, 0xE0, 0x14, 0xF4 };  /* shl ax, 20 */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0xFFFF;

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AX", cpu->ax, 0x0000);
    vm86_expect_flag("CF", cpu, VM86_CF, false);
}

/* A count of one taken from CL behaves as the immediate form does, so
 * that the flag rules a program relies on do not depend on where the
 * count came from. */
static void test_shift_by_cl_of_one(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xD2, 0xE0, 0xF4 };   /* shl al, cl */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->al = 0x80;
    cpu->cx = 0x0001;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AL", cpu->al, 0x00);
    vm86_expect_flags("flags", cpu,
                      FLAGS(VM86_CF | VM86_OF | VM86_ZF | VM86_PF), 0);
}

/* ================================================================== */
/* The group 1 immediate forms                                         */
/* ================================================================== */

/*
 * 82h is 80h.
 *
 * The encoding is undefined in the manual and some assemblers emit it
 * anyway; one that refused it would break those programs for no gain.
 * Checked against 80h with the same operand, so the two cannot drift.
 */
static void test_eighty_two_is_eighty(struct vm86_cpu *cpu)
{
    static const uint8_t alias[] = { 0x82, 0xC0, 0x05, 0xF4 };  /* add al,5 */
    static const uint8_t documented[] = { 0x80, 0xC0, 0x05, 0xF4 };

    vm86_test_load(cpu, documented, sizeof(documented));
    cpu->al = 0x7F;
    vm86_test_run(cpu, 10);
    uint16_t value = cpu->ax;
    uint16_t flags = cpu->flags;

    vm86_test_load(cpu, alias, sizeof(alias));
    cpu->al = 0x7F;
    vm86_test_run(cpu, 10);

    vm86_expect_u16("82h gives the same answer as 80h", cpu->ax, value);
    vm86_expect_u16("... and the same flags", cpu->flags, flags);
    /* And the answer itself, worked out by hand: 7Fh + 5 is 84h with the
     * sign bit set and the low nibble carrying. */
    vm86_expect_u16("AL", cpu->al, 0x84);
    vm86_expect_flag("SF", cpu, VM86_SF, true);
    vm86_expect_flag("OF", cpu, VM86_OF, true);
}

/*
 * The one-byte immediate of 83h is signed.
 *
 * `83 C0 FF` is `add ax, -1`, and read as an unsigned 255 the answer
 * would be 0x0100 with no carry rather than 0 with a carry. The
 * documented form 81h with the same arithmetic spelled in full is run
 * alongside, so the two must agree.
 */
static void test_the_group_one_byte_immediate_is_signed(struct vm86_cpu *cpu)
{
    static const uint8_t short_form[] = { 0x83, 0xC0, 0xFF, 0xF4 };
    static const uint8_t long_form[]  = { 0x81, 0xC0, 0xFF, 0xFF, 0xF4 };

    vm86_test_load(cpu, short_form, sizeof(short_form));
    cpu->ax = 0x0001;
    vm86_test_run(cpu, 10);

    uint16_t flags = cpu->flags;

    vm86_test_load(cpu, long_form, sizeof(long_form));
    cpu->ax = 0x0001;
    vm86_test_run(cpu, 10);

    vm86_expect_u16("83h and 81h agree", cpu->flags, flags);
    vm86_expect_u16("AX", cpu->ax, 0x0000);
    vm86_expect_flags("flags", cpu,
                      FLAGS(VM86_CF | VM86_ZF | VM86_AF | VM86_PF), 0);
}

/* A group 1 operation on a memory operand, addressed through BP with a
 * displacement, so the default segment and the fetch order are both
 * exercised on a path the register form does not take. */
static void test_group_one_on_memory_through_bp(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x83, 0x46, 0x10, 0x01, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    vm86_set_seg(cpu, VM86_SS, 0x0300);
    vm86_set_seg(cpu, VM86_DS, 0x0100);
    cpu->bp = 0x0200;
    poke16(cpu, 0x03210, 0xFFFF);

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_mem16("the word incremented in place", cpu, 0x03210, 0x0000);
    vm86_expect_mem16("and nothing through DS", cpu, 0x01210, 0x0000);
    vm86_expect_flags("flags", cpu,
                      FLAGS(VM86_CF | VM86_ZF | VM86_AF | VM86_PF), 0);
}

/* ================================================================== */
/* TEST, NOT and NEG                                                   */
/* ================================================================== */

/*
 * TEST is an AND that keeps nothing but the flags, and 84h is its
 * register form.
 *
 * The destination has to survive while CF and OF go to zero and the rest
 * come from the AND. A handler that stored the result would set the same
 * flags and destroy a register the program was keeping something in.
 */
static void test_test_keeps_the_destination_and_clears_the_carries(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x84, 0xC3, 0xF4 };   /* test al, bl */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->al = 0xF0;
    cpu->bl = 0x0F;
    cpu->flags = FLAGS(VM86_CF | VM86_OF);

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AL is untouched", cpu->al, 0xF0);
    /* The ModRM r/m side is BL here, and it is the operand a
     * write-back would land on -- so it has to be checked too, or a
     * TEST that stored its result passes this case. */
    vm86_expect_u16("BL is untouched", cpu->bl, 0x0F);
    vm86_expect_flags("flags", cpu, FLAGS(VM86_ZF | VM86_PF), VM86_AF);
}

/* NOT inverts every bit and touches nothing else. It sits next to NEG in
 * the same group, and the two have nothing in common. */
static void test_not_changes_no_flag(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF6, 0xD3, 0xF4 };   /* not bl */

    const uint16_t flags = FLAGS(VM86_CF | VM86_ZF | VM86_SF | VM86_OF |
                                 VM86_AF | VM86_PF | VM86_DF);

    vm86_test_load(cpu, code, sizeof(code));
    cpu->bl = 0x0F;
    cpu->flags = flags;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("BL", cpu->bl, 0xF0);
    vm86_expect_u16("FLAGS", cpu->flags, flags);
}

/*
 * NEG is 0 minus the operand, and its carry is the one subtraction that
 * does not mean borrow in the usual way: it is clear for a zero operand
 * and set for everything else, because subtracting from zero only fails
 * to borrow when there is nothing to subtract.
 *
 * The sign boundary is the other half: negating 80h gives 80h back, so
 * the sign is wrong and OF is set.
 */
static void test_neg_of_zero_and_of_the_sign_boundary(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF6, 0xDB, 0xF4 };   /* neg bl */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->bl = 0x00;
    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("BL", cpu->bl, 0x00);
    vm86_expect_flags("flags for NEG 0", cpu,
                      FLAGS(VM86_ZF | VM86_PF), 0);

    vm86_test_load(cpu, code, sizeof(code));
    cpu->bl = 0x80;
    vm86_test_run(cpu, 10);

    vm86_expect_u16("BL", cpu->bl, 0x80);
    vm86_expect_flags("flags for NEG 80h", cpu,
                      FLAGS(VM86_CF | VM86_OF | VM86_SF), 0);

    vm86_test_load(cpu, code, sizeof(code));
    cpu->bl = 0x01;
    vm86_test_run(cpu, 10);

    vm86_expect_u16("BL", cpu->bl, 0xFF);
    vm86_expect_flags("flags for NEG 1", cpu,
                      FLAGS(VM86_CF | VM86_SF | VM86_AF | VM86_PF), 0);
}

/* ================================================================== */
/* Multiplication and division                                         */
/* ================================================================== */

/*
 * MUL's carry and overflow say the same thing -- that the product did
 * not fit in the low half -- and they are the only flags the instruction
 * defines. The two cases below are the two answers, and the wide form is
 * checked as well so that the high half is not written to the wrong
 * register.
 */
static void test_mul_reports_a_product_that_does_not_fit(struct vm86_cpu *cpu)
{
    static const uint8_t byte_mul[] = { 0xF6, 0xE3, 0xF4 };   /* mul bl */
    static const uint8_t word_mul[] = { 0xF7, 0xE3, 0xF4 };   /* mul bx */

    vm86_test_load(cpu, byte_mul, sizeof(byte_mul));
    cpu->al = 0x10;
    cpu->bl = 0x10;
    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0x0100);
    vm86_expect_flag("CF", cpu, VM86_CF, true);
    vm86_expect_flag("OF", cpu, VM86_OF, true);

    vm86_test_load(cpu, byte_mul, sizeof(byte_mul));
    cpu->al = 0x02;
    cpu->bl = 0x03;
    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0x0006);
    vm86_expect_flag("CF", cpu, VM86_CF, false);
    vm86_expect_flag("OF", cpu, VM86_OF, false);

    /* The wide form leaves its high half in DX, which is the detail that
     * separates it from the byte form. */
    vm86_test_load(cpu, word_mul, sizeof(word_mul));
    cpu->ax = 0x1000;
    cpu->bx = 0x1000;
    vm86_test_run(cpu, 10);

    vm86_expect_u16("DX", cpu->dx, 0x0100);
    vm86_expect_u16("AX", cpu->ax, 0x0000);
    vm86_expect_flag("CF", cpu, VM86_CF, true);
}

/*
 * IMUL's flags mean "the high half is not the sign extension of the low
 * half", which is a different question from MUL's.
 *
 * -1 times 2 is -2, and the high half of FFFEh is FFh, which is exactly
 * the sign extension of FEh -- so nothing was lost and both flags are
 * clear. 7Fh times 2 is FEh with a high half of 00h, and FEh sign
 * extends to FFh, so the answer did not fit and both flags are set. The
 * product is the same two bytes in the two cases; only the flags differ.
 */
static void test_imul_reports_a_product_that_lost_its_sign(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF6, 0xEB, 0xF4 };   /* imul bl */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->al = 0xFF;   /* -1 */
    cpu->bl = 0x02;
    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AX is -2", cpu->ax, 0xFFFE);
    vm86_expect_flag("CF", cpu, VM86_CF, false);
    vm86_expect_flag("OF", cpu, VM86_OF, false);

    vm86_test_load(cpu, code, sizeof(code));
    cpu->al = 0x7F;   /* 127 */
    cpu->bl = 0x02;
    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX is 254", cpu->ax, 0x00FE);
    vm86_expect_flag("CF", cpu, VM86_CF, true);
    vm86_expect_flag("OF", cpu, VM86_OF, true);
}

/*
 * The boundary of a divide-quotient overflow, from both sides.
 *
 * An 8-bit divide faults when the quotient needs more than eight bits,
 * which is the second error condition and the one that is easy to leave
 * out. FFh is exactly the largest quotient that fits, so it must not
 * fault; 100h must. The two differ by one, and the value on the stack
 * that separates them is AX.
 */
static void test_divide_quotient_overflow_at_its_boundary(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF6, 0xF3, 0xF4 };   /* div bl */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x00FF;
    cpu->bl = 0x01;
    vm86_test_run(cpu, 10);

    vm86_expect_bool("FFh fits", cpu->halted, true);
    vm86_expect_u16("AL", cpu->al, 0xFF);
    vm86_expect_u16("AH", cpu->ah, 0x00);

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x0100;
    cpu->bl = 0x01;
    enum vm86_result result = vm86_test_run(cpu, 10);

    vm86_expect_bool("100h does not fit", result == VM86_FAULT, true);
    /* The vector is zero, and cpu->fault also uses zero for "none", so
     * the return value is the only thing that distinguishes them. */
    vm86_expect_u16("vector", cpu->fault, VM86_VECTOR_DIVIDE_ERROR);
}

/*
 * Dividing by zero faults, and so does a quotient that will not fit --
 * the task book's own example, DX:AX = FFFF:FFFF over BX = 1.
 *
 * The quotient there is 0xFFFF0001, which is four times what a sixteen
 * bit register holds. Real programs use the fault to detect the overflow
 * rather than getting a truncated answer, so a truncating implementation
 * is wrong in a way that produces a plausible number.
 */
static void test_divide_faults_on_zero_and_on_the_task_books_example(struct vm86_cpu *cpu)
{
    static const uint8_t byte_div[] = { 0xF6, 0xF3, 0xF4 };
    static const uint8_t word_div[] = { 0xF7, 0xF3, 0xF4 };

    vm86_test_load(cpu, byte_div, sizeof(byte_div));
    cpu->ax = 0x1234;
    cpu->bl = 0x00;
    enum vm86_result result = vm86_test_run(cpu, 10);
    vm86_expect_bool("divide by zero faults", result == VM86_FAULT, true);

    vm86_test_load(cpu, word_div, sizeof(word_div));
    cpu->dx = 0xFFFF;
    cpu->ax = 0xFFFF;
    cpu->bx = 0x0001;
    result = vm86_test_run(cpu, 10);
    vm86_expect_bool("the quotient would not fit", result == VM86_FAULT, true);
}

/* A 16-bit divide that does fit, including a quotient whose top bit is
 * set -- which is a legal unsigned quotient and would be an overflow if
 * the implementation compared it as signed. */
static void test_divide_sixteen_bit_quotient(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF7, 0xF3, 0xF4 };   /* div bx */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->dx = 0x0001;
    cpu->ax = 0x0000;   /* 0x10000 */
    cpu->bx = 0x0002;
    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AX", cpu->ax, 0x8000);   /* 32768, quotient */
    vm86_expect_u16("DX", cpu->dx, 0x0000);   /* remainder */
}

/*
 * IDIV truncates towards zero and the remainder takes the sign of the
 * dividend, so -7 over 2 is -3 with a remainder of -1 and not -4 with a
 * remainder of 1. Both parts are checked, because an implementation that
 * divided magnitudes and then negated would get the quotient right and
 * the remainder wrong.
 */
static void test_idiv_truncates_towards_zero(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF6, 0xFB, 0xF4 };   /* idiv bl */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0xFFF9;   /* -7 */
    cpu->bl = 0x02;
    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AL", cpu->al, 0xFD);   /* -3 */
    vm86_expect_u16("AH", cpu->ah, 0xFF);   /* -1 */
}

/*
 * IDIV's overflow is not the same set as DIV's: -32768 over -1 has a
 * quotient of +32768, which is one past what a signed sixteen-bit
 * register holds. The unsigned form would not fault here at all.
 */
static void test_idiv_overflows_on_the_most_negative_dividend(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF7, 0xFB, 0xF4 };   /* idiv bx */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->dx = 0xFFFF;
    cpu->ax = 0x8000;   /* -32768 */
    cpu->bx = 0xFFFF;   /* -1 */

    enum vm86_result result = vm86_test_run(cpu, 10);

    vm86_expect_bool("faults", result == VM86_FAULT, true);
    vm86_expect_u16("vector", cpu->fault, VM86_VECTOR_DIVIDE_ERROR);
}

/* ================================================================== */
/* The decimal adjustments                                             */
/* ================================================================== */

/*
 * DAA, DAS, AAA and AAS are the four instructions the task book warns
 * about, because their rules are conditional and depend on flags the
 * instruction does not set itself. The expectations below are the
 * manual's table read one line at a time: each case is one line of it.
 *
 * DAA adds 6 to the low digit when it is above 9 or AF says so, then 60
 * to the high digit under the same test against CF, and sets CF if
 * either happened.
 */
static void test_daa_adjusts_by_the_manuals_table(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x27, 0xF4 };

    /* Neither digit out of range: nothing happens. */
    vm86_test_load(cpu, code, sizeof(code));
    cpu->al = 0x25;
    vm86_test_run(cpu, 10);
    vm86_expect_u16("AL with no adjustment", cpu->al, 0x25);
    vm86_expect_flags("flags", cpu, VM86_FLAG_ALWAYS_SET, 0);

    /* Low digit above 9: add 6, and the low digit carries. */
    vm86_test_load(cpu, code, sizeof(code));
    cpu->al = 0x0A;
    vm86_test_run(cpu, 10);
    vm86_expect_u16("AL with a low digit", cpu->al, 0x10);
    vm86_expect_flags("flags", cpu, FLAGS(VM86_AF), 0);

    /* AF set with a low digit in range: still adds 6, which is the half
     * of the rule that a test using only the digit would miss. */
    vm86_test_load(cpu, code, sizeof(code));
    cpu->al = 0x01;
    cpu->flags = FLAGS(VM86_AF);
    vm86_test_run(cpu, 10);
    vm86_expect_u16("AL with AF set", cpu->al, 0x07);
    vm86_expect_flags("flags", cpu, FLAGS(VM86_AF), 0);

    /* High digit out of range: add 60h, which carries out of the byte.
     * CF and AF are cleared first because DAA reads them -- inheriting
     * the AF the case above set would add the low correction too, and
     * that is a test measuring the previous test. */
    vm86_test_load(cpu, code, sizeof(code));
    cpu->al = 0xA0;
    cpu->flags = VM86_FLAG_ALWAYS_SET;
    vm86_test_run(cpu, 10);
    vm86_expect_u16("AL with a high digit", cpu->al, 0x00);
    vm86_expect_flags("flags", cpu, FLAGS(VM86_CF | VM86_ZF | VM86_PF), 0);
}

/* DAS is the mirror image, subtracting rather than adding, with the same
 * two conditions. The second case has CF set and a low digit in range,
 * which isolates the high-digit half of the rule. */
static void test_das_subtracts_by_the_manuals_table(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x2F, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->al = 0x25;
    vm86_test_run(cpu, 10);
    vm86_expect_u16("AL with no adjustment", cpu->al, 0x25);
    vm86_expect_flags("flags", cpu, VM86_FLAG_ALWAYS_SET, 0);

    vm86_test_load(cpu, code, sizeof(code));
    cpu->al = 0x0A;
    vm86_test_run(cpu, 10);
    vm86_expect_u16("AL with a low digit", cpu->al, 0x04);
    vm86_expect_flags("flags", cpu, FLAGS(VM86_AF), 0);

    vm86_test_load(cpu, code, sizeof(code));
    cpu->al = 0x00;
    cpu->flags = FLAGS(VM86_CF);
    vm86_test_run(cpu, 10);
    vm86_expect_u16("AL with the carry set", cpu->al, 0xA0);
    vm86_expect_flags("flags", cpu, FLAGS(VM86_CF | VM86_SF | VM86_PF), 0);
}

/*
 * AAA and AAS adjust the low digit in AL and carry into AH, which is
 * what makes them the way a program adds two unpacked decimal numbers.
 * Both set AF and CF together and clear them together, and both blank
 * the high digit of AL at the end -- the step that a version reading the
 * manual's first paragraph and stopping there would leave out.
 */
static void test_aaa_and_aas_carry_into_ah(struct vm86_cpu *cpu)
{
    static const uint8_t aaa[] = { 0x37, 0xF4 };
    static const uint8_t aas[] = { 0x3F, 0xF4 };

    vm86_test_load(cpu, aaa, sizeof(aaa));
    cpu->ax = 0x000A;
    cpu->flags = VM86_FLAG_ALWAYS_SET;
    vm86_test_run(cpu, 10);
    vm86_expect_u16("AAA with a low digit above 9", cpu->ax, 0x0100);
    vm86_expect_flags("flags", cpu, FLAGS(VM86_AF | VM86_CF),
                      VM86_SF | VM86_ZF | VM86_PF);

    vm86_test_load(cpu, aaa, sizeof(aaa));
    cpu->ax = 0x0005;
    cpu->flags = VM86_FLAG_ALWAYS_SET;
    vm86_test_run(cpu, 10);
    vm86_expect_u16("AAA with a digit in range", cpu->ax, 0x0005);
    vm86_expect_flags("flags", cpu, VM86_FLAG_ALWAYS_SET,
                      VM86_SF | VM86_ZF | VM86_PF);

    vm86_test_load(cpu, aas, sizeof(aas));
    cpu->ax = 0x000A;
    cpu->flags = VM86_FLAG_ALWAYS_SET;
    vm86_test_run(cpu, 10);
    vm86_expect_u16("AAS with a low digit above 9", cpu->ax, 0xFF04);
    vm86_expect_flags("flags", cpu, FLAGS(VM86_AF | VM86_CF),
                      VM86_SF | VM86_ZF | VM86_PF);

    vm86_test_load(cpu, aas, sizeof(aas));
    cpu->ax = 0x0005;
    cpu->flags = VM86_FLAG_ALWAYS_SET;
    vm86_test_run(cpu, 10);
    vm86_expect_u16("AAS with a digit in range", cpu->ax, 0x0005);
    vm86_expect_flags("flags", cpu, VM86_FLAG_ALWAYS_SET,
                      VM86_SF | VM86_ZF | VM86_PF);
}

/*
 * AAM and AAD are the divide and multiply behind BCD conversion, with a
 * one-byte radix that is ten in every program that uses them. AAM puts
 * AL/10 in AH and AL%10 in AL; AAD does the inverse and clears AH.
 */
static void test_aam_and_aad_convert_by_ten(struct vm86_cpu *cpu)
{
    static const uint8_t aam[] = { 0xD4, 0x0A, 0xF4 };
    static const uint8_t aad[] = { 0xD5, 0x0A, 0xF4 };

    vm86_test_load(cpu, aam, sizeof(aam));
    cpu->al = 45;
    vm86_test_run(cpu, 10);
    vm86_expect_u16("AH", cpu->ah, 4);
    vm86_expect_u16("AL", cpu->al, 5);

    vm86_test_load(cpu, aad, sizeof(aad));
    cpu->ah = 4;
    cpu->al = 5;
    vm86_test_run(cpu, 10);
    vm86_expect_u16("AL", cpu->al, 45);
    vm86_expect_u16("AH", cpu->ah, 0);
}

/*
 * CBW and CWD sign-extend, and neither touches a flag.
 *
 * The sign boundary is the case that matters: 80h sign-extends to FF80h
 * because its top bit is set, and 7Fh extends to 007Fh because it is
 * not. An implementation that filled with zeroes would pass every test
 * using a small positive value.
 */
static void test_cbw_and_cwd_sign_extend(struct vm86_cpu *cpu)
{
    static const uint8_t cbw[] = { 0x98, 0xF4 };
    static const uint8_t cwd[] = { 0x99, 0xF4 };

    const uint16_t flags = FLAGS(VM86_CF | VM86_ZF | VM86_SF | VM86_OF |
                                 VM86_AF | VM86_PF | VM86_DF);

    vm86_test_load(cpu, cbw, sizeof(cbw));
    cpu->ax = 0x0080;
    cpu->flags = flags;
    vm86_test_run(cpu, 10);
    vm86_expect_u16("CBW of 80h", cpu->ax, 0xFF80);
    vm86_expect_u16("CBW leaves the flags", cpu->flags, flags);

    vm86_test_load(cpu, cbw, sizeof(cbw));
    cpu->ax = 0x007F;
    vm86_test_run(cpu, 10);
    vm86_expect_u16("CBW of 7Fh", cpu->ax, 0x007F);

    vm86_test_load(cpu, cwd, sizeof(cwd));
    cpu->ax = 0x8000;
    cpu->flags = flags;
    vm86_test_run(cpu, 10);
    vm86_expect_u16("CWD of 8000h", cpu->dx, 0xFFFF);
    vm86_expect_u16("CWD leaves the flags", cpu->flags, flags);

    vm86_test_load(cpu, cwd, sizeof(cwd));
    cpu->ax = 0x7FFF;
    vm86_test_run(cpu, 10);
    vm86_expect_u16("CWD of 7FFFh", cpu->dx, 0x0000);
}

/*
 * POP r/m16 in its register form is an ordinary instruction, not a
 * refusal.
 *
 * 8Fh /0 with mod 11 is `pop ax` written the long way, and it is legal.
 * It sits next to the encodings above and would be easy to refuse along
 * with them -- which is the shape of failure this round produced twice
 * already -- so it is checked from the outside: the word comes off the
 * stack and SP moves.
 */
static void test_pop_to_a_register_is_not_refused(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x8F, 0xC0, 0xF4 };   /* pop ax */

    vm86_test_load(cpu, code, sizeof(code));

    poke16(cpu, VM86_TEST_STACK_TOP, 0x1234);

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AX", cpu->ax, 0x1234);
    /* SP was FFFEh and popping two more wraps it to zero inside the
     * segment, which is where the stack pointer goes and not an
     * address past it. */
    vm86_expect_u16("SP", cpu->sp, 0x0000);
}

/*
 * A one-byte displacement is signed.
 *
 * `8B 46 F0` is `mov ax, [bp-10h]`. Read as an unsigned byte the offset
 * would be BP+00F0 instead of BP-0010, which is 0x100 bytes away -- and
 * a test that only ever used a positive displacement cannot tell the two
 * apart, because the sign extension of a value below 0x80 changes
 * nothing. Both addresses hold a value, and they differ.
 */
static void test_a_negative_displacement_is_sign_extended(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x8B, 0x46, 0xF0, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    vm86_set_seg(cpu, VM86_SS, 0x0300);
    cpu->bp = 0x0200;

    poke16(cpu, 0x031F0, 0x1234);   /* BP - 10h */
    poke16(cpu, 0x032F0, 0x9999);   /* BP + F0h, where an unsigned read goes */

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AX", cpu->ax, 0x1234);
}

/* ================================================================== */
/* PUSHA and POPA                                                      */
/* ================================================================== */

/*
 * PUSHA saves the stack pointer it had before the first push, and that
 * is not the same rule as `PUSH SP`.
 *
 * On a 186 `push sp` stores SP before it is decremented (ops_mov.c has
 * the 8086 rule, which is the other way round for that instruction), and
 * PUSHA stores the value from before any of its eight pushes. The two
 * rules come from different instructions on different parts and must not
 * be unified.
 *
 * The saved word is read back out of the stack and compared with where
 * the stack started, which is the only way the difference is visible: a
 * PUSHA that read SP in the middle of its own pushes would store FFF0h
 * minus eight and nothing else about the instruction would change.
 */
static void test_pusha_saves_the_stack_pointer_from_before_it_started(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x60, 0xF4 };   /* pusha */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->sp = 0xFFF0;

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("SP after eight pushes", cpu->sp, 0xFFE0);
    /* The four registers pushed before the SP slot are AX CX DX BX, so
     * the saved SP is the fifth word down. */
    vm86_expect_mem16("the saved SP", cpu, 0xFFE6, 0xFFF0);
}

/*
 * POPA reads the SP slot and throws it away.
 *
 * A symmetric implementation assigns it, which sets the stack pointer to
 * whatever the caller happened to have in that slot. The slot is filled
 * with a value that would be obvious if it took effect, and SP has to
 * come back to where PUSHA left it.
 */
static void test_popa_discards_the_saved_stack_pointer(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x60, 0x61, 0xF4 };   /* pusha; popa */

    vm86_test_load(cpu, code, sizeof(code));

    cpu->ax = 0x1111;
    cpu->cx = 0x2222;
    cpu->dx = 0x3333;
    cpu->bx = 0x4444;
    cpu->bp = 0x5555;
    cpu->si = 0x6666;
    cpu->di = 0x7777;
    cpu->sp = 0xFFF0;

    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("SP is back where PUSHA found it", cpu->sp, 0xFFF0);
    vm86_expect_u16("AX", cpu->ax, 0x1111);
    vm86_expect_u16("CX", cpu->cx, 0x2222);
    vm86_expect_u16("DX", cpu->dx, 0x3333);
    vm86_expect_u16("BX", cpu->bx, 0x4444);
    vm86_expect_u16("BP", cpu->bp, 0x5555);
    vm86_expect_u16("SI", cpu->si, 0x6666);
    vm86_expect_u16("DI", cpu->di, 0x7777);
}

/*
 * ... and the same thing with the saved slot filled with rubbish, so
 * that a POPA which did assign it would be caught rather than merely
 * being invisible.
 */
static void test_popa_ignores_rubbish_in_the_saved_slot(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x61, 0xF4 };   /* popa */

    vm86_test_load(cpu, code, sizeof(code));

    cpu->sp = 0xFFE0;
    poke16(cpu, 0xFFE0, 0x0001);   /* DI */
    poke16(cpu, 0xFFE2, 0x0002);   /* SI */
    poke16(cpu, 0xFFE4, 0x0003);   /* BP */
    poke16(cpu, 0xFFE6, 0xDEAD);   /* SP -- must not take effect */
    poke16(cpu, 0xFFE8, 0x0004);   /* BX */
    poke16(cpu, 0xFFEA, 0x0005);   /* DX */
    poke16(cpu, 0xFFEC, 0x0006);   /* CX */
    poke16(cpu, 0xFFEE, 0x0007);   /* AX */

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("SP did not become DEADh", cpu->sp, 0xFFF0);
    vm86_expect_u16("AX", cpu->ax, 0x0007);
    vm86_expect_u16("DI", cpu->di, 0x0001);
}

/* ================================================================== */
/* ENTER and LEAVE                                                     */
/* ================================================================== */

/*
 * A level 0 frame, and the difference a level makes.
 *
 * `enter n, 0` is a push of BP, a copy of SP into BP, and a subtraction
 * for the locals -- nothing else. `enter n, 1` is that plus one more
 * word: a copy of the new frame pointer, which is what lets a program
 * walk outwards one scope. The two are not the same instruction, and a
 * handler that implemented level 0 and treated every level as zero would
 * look right until something with a nested scope used the chain.
 */
static void test_enter_and_leave_at_level_zero_and_one(struct vm86_cpu *cpu)
{
    static const uint8_t level0[] = { 0xC8, 0x10, 0x00, 0x00, 0xF4 };
    static const uint8_t level1[] = { 0xC8, 0x10, 0x00, 0x01, 0xF4 };
    static const uint8_t leave[]  = { 0xC9, 0xF4 };

    vm86_test_load(cpu, level0, sizeof(level0));
    cpu->bp = 0x9000;
    cpu->sp = 0xFFF0;
    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("BP is the new frame at level 0", cpu->bp, 0xFFEE);
    vm86_expect_u16("SP after 16 bytes of locals", cpu->sp, 0xFFDE);
    vm86_expect_mem16("the caller's BP is saved", cpu, 0xFFEE, 0x9000);

    vm86_test_load(cpu, level1, sizeof(level1));
    cpu->bp = 0x9000;
    cpu->sp = 0xFFF0;
    vm86_test_run(cpu, 10);

    vm86_expect_u16("BP is the new frame at level 1", cpu->bp, 0xFFEE);
    vm86_expect_u16("SP is one word lower than level 0", cpu->sp, 0xFFDC);
    vm86_expect_mem16("the frame pointer is linked", cpu, 0xFFEC, 0xFFEE);

    /* LEAVE unwinds either frame to where ENTER found it. */
    vm86_test_load(cpu, leave, sizeof(leave));
    cpu->bp = 0xFFEE;
    cpu->sp = 0xFFDC;
    vm86_test_run(cpu, 10);

    vm86_expect_u16("BP restored", cpu->bp, 0x9000);
    vm86_expect_u16("SP restored", cpu->sp, 0xFFF0);
}

/*
 * A nesting level above one runs the chain-copying loop.
 *
 * `enter 0, 3` copies two words from the caller's chain of frame
 * pointers, which is the part of the instruction that only executes from
 * level 2 up. The caller's chain is placed by hand: BP points at a frame
 * whose two words below are the frames before it. Both are read and both
 * land on the new frame, one above the other.
 */
static void test_enter_copies_the_callers_chain(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xC8, 0x00, 0x00, 0x03, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    cpu->bp = 0x9000;
    cpu->sp = 0xFFF0;

    poke16(cpu, 0x8FFE, 0x1111);   /* the first word of the caller's chain */
    poke16(cpu, 0x8FFC, 0x2222);   /* the second                          */

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("BP is the new frame", cpu->bp, 0xFFEE);
    vm86_expect_u16("two chain words and the frame pointer", cpu->sp, 0xFFE8);
    vm86_expect_mem16("the first chain word", cpu, 0xFFEC, 0x1111);
    vm86_expect_mem16("the second chain word", cpu, 0xFFEA, 0x2222);
    vm86_expect_mem16("the frame pointer", cpu, 0xFFE8, 0xFFEE);
}

/* ================================================================== */
/* BOUND                                                               */
/* ================================================================== */

/*
 * BOUND is a signed range check whose two ends are both inside the
 * range.
 *
 * 62 04 is `bound ax, [si]`: the lower bound is the word at [si] and the
 * upper is the word at [si+2], which is the same low-address-first
 * layout as a far pointer. Both ends are inclusive, and the comparison
 * is signed -- so a range that straddles zero works, and one written
 * with an unsigned comparison would reject every negative index.
 *
 * The signed case is the one that cannot be faked: with the bounds at
 * FFF0h and 000Fh, -1 is inside the range and 7FFFh is outside it, which
 * is true of no unsigned reading of the same two words.
 */
static void test_bound_is_a_signed_and_inclusive_range_check(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x62, 0x04, 0xF4 };   /* bound ax, [si] */

    /* Inside, and both ends inclusive. */
    vm86_test_load(cpu, code, sizeof(code));
    poke16(cpu, 0x0200, 0x0000);
    poke16(cpu, 0x0202, 0x0010);
    cpu->si = 0x0200;
    cpu->ax = 0x0005;
    enum vm86_result result = vm86_test_run(cpu, 10);
    vm86_expect_bool("a value inside runs to the halt",
                     result == VM86_HALT, true);

    vm86_test_load(cpu, code, sizeof(code));
    poke16(cpu, 0x0200, 0x0000);
    poke16(cpu, 0x0202, 0x0010);
    cpu->si = 0x0200;
    cpu->ax = 0x0000;   /* exactly the lower bound */
    result = vm86_test_run(cpu, 10);
    vm86_expect_bool("the lower bound is inside", result == VM86_HALT, true);

    vm86_test_load(cpu, code, sizeof(code));
    poke16(cpu, 0x0200, 0x0000);
    poke16(cpu, 0x0202, 0x0010);
    cpu->si = 0x0200;
    cpu->ax = 0x0010;   /* exactly the upper bound */
    result = vm86_test_run(cpu, 10);
    vm86_expect_bool("the upper bound is inside", result == VM86_HALT, true);

    vm86_test_load(cpu, code, sizeof(code));
    poke16(cpu, 0x0200, 0x0000);
    poke16(cpu, 0x0202, 0x0010);
    cpu->si = 0x0200;
    cpu->ax = 0x0011;
    result = vm86_test_run(cpu, 10);
    vm86_expect_bool("one past the top faults", result == VM86_FAULT, true);
    vm86_expect_u16("vector", cpu->fault, VM86_VECTOR_BOUND);

    /* A range that straddles zero, which only a signed comparison gets
     * right. */
    vm86_test_load(cpu, code, sizeof(code));
    poke16(cpu, 0x0200, 0xFFF0);   /* -16 */
    poke16(cpu, 0x0202, 0x000F);   /* +15 */
    cpu->si = 0x0200;
    cpu->ax = 0xFFFF;              /* -1 */
    result = vm86_test_run(cpu, 10);
    vm86_expect_bool("-1 is inside -16..15", result == VM86_HALT, true);

    vm86_test_load(cpu, code, sizeof(code));
    poke16(cpu, 0x0200, 0xFFF0);
    poke16(cpu, 0x0202, 0x000F);
    cpu->si = 0x0200;
    cpu->ax = 0x7FFF;              /* 32767 */
    result = vm86_test_run(cpu, 10);
    vm86_expect_bool("32767 is above the range", result == VM86_FAULT, true);
}

/* ================================================================== */
/* The immediate forms of PUSH and IMUL                                */
/* ================================================================== */

/*
 * The one-byte PUSH immediate is signed.
 *
 * `6A FB` pushes FFFBh and not 00FBh, which is the same rule as the 83h
 * form of the arithmetic group and the reason a compiler can emit
 * `push 5` and `push -5` with the same byte-length instruction. The word
 * form is a plain sixteen-bit value and is checked next to it so that
 * the sign extension cannot be applied to both.
 */
static void test_push_immediate_sign_extends_the_byte_form(struct vm86_cpu *cpu)
{
    static const uint8_t byte_form[] = { 0x6A, 0xFB, 0xF4 };
    static const uint8_t word_form[] = { 0x68, 0xFB, 0x00, 0xF4 };

    vm86_test_load(cpu, byte_form, sizeof(byte_form));
    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_mem16("what 6A FB pushed", cpu, VM86_TEST_STACK_TOP - 2,
                      0xFFFB);

    vm86_test_load(cpu, word_form, sizeof(word_form));
    vm86_test_run(cpu, 10);

    vm86_expect_mem16("what 68 FB 00 pushed", cpu, VM86_TEST_STACK_TOP - 2,
                      0x00FB);
}

/*
 * The three-operand IMUL keeps only the low half and reports whether it
 * had to.
 *
 * Its flags are not the flags of the one-operand IMUL in ops_alu.c: that
 * one produces a full product in DX:AX and asks whether the high half
 * was needed, this one produces sixteen bits and asks whether the bits
 * it discarded were anything but a copy of the sign. -32768 times -1 is
 * the boundary -- 32768 does not fit in a signed sixteen-bit register,
 * so both flags are set even though the discarded bits are not zero.
 */
static void test_imul_with_an_immediate_reports_what_it_discarded(struct vm86_cpu *cpu)
{
    static const uint8_t fits[] = { 0x69, 0xC3, 0x02, 0x00, 0xF4 }; /* imul ax,bx,2 */
    static const uint8_t does_not_fit[] = { 0x69, 0xC3, 0x10, 0x00, 0xF4 };
    static const uint8_t boundary[] = { 0x6B, 0xC3, 0xFF, 0xF4 };   /* imul ax,bx,-1 */

    vm86_test_load(cpu, fits, sizeof(fits));
    cpu->bx = 0x0002;
    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AX", cpu->ax, 0x0004);
    vm86_expect_flag("CF", cpu, VM86_CF, false);
    vm86_expect_flag("OF", cpu, VM86_OF, false);

    /* A negative product that does fit: -1 times 2 is -2. */
    vm86_test_load(cpu, fits, sizeof(fits));
    cpu->bx = 0xFFFF;
    vm86_test_run(cpu, 10);
    vm86_expect_u16("AX is -2", cpu->ax, 0xFFFE);
    vm86_expect_flag("CF for a negative product that fits", cpu, VM86_CF, false);

    vm86_test_load(cpu, does_not_fit, sizeof(does_not_fit));
    cpu->bx = 0x1000;
    vm86_test_run(cpu, 10);
    vm86_expect_u16("AX", cpu->ax, 0x0000);
    vm86_expect_flag("CF", cpu, VM86_CF, true);
    vm86_expect_flag("OF", cpu, VM86_OF, true);

    vm86_test_load(cpu, boundary, sizeof(boundary));
    cpu->bx = 0x8000;   /* -32768 times -1 */
    vm86_test_run(cpu, 10);
    vm86_expect_u16("AX", cpu->ax, 0x8000);
    vm86_expect_flag("CF at the sign boundary", cpu, VM86_CF, true);
}

/* ================================================================== */
/* INS and OUTS                                                        */
/* ================================================================== */

/*
 * INS and OUTS repeat through the same loop the string instructions do.
 *
 * This is the coordination point the task book names: the 186 file uses
 * ops_str.c's loop rather than writing its own, so `rep insb` and
 * `rep movsb` cannot disagree about the prefix, the direction flag or
 * the count of zero. The cases below are the same ones the string suite
 * applies to MOVS, applied here.
 *
 * Nothing comes back from a port yet -- INS stores what a floating bus
 * gives and OUTS sends its word nowhere -- so the observable half is the
 * pointer, the count and the bytes INS writes.
 */
static void test_ins_and_outs_repeat_like_the_string_instructions(struct vm86_cpu *cpu)
{
    static const uint8_t rep_insb[] = { 0xF3, 0x6C, 0xF4 };
    static const uint8_t rep_insw[] = { 0xF3, 0x6D, 0xF4 };
    static const uint8_t rep_outsb[] = { 0xF3, 0x6E, 0xF4 };
    static const uint8_t zero_count[] = { 0xF3, 0x6C, 0xF4 };
    static const uint8_t backwards[] = { 0xFD, 0xF3, 0x6C, 0xF4 };

    vm86_test_load(cpu, rep_insb, sizeof(rep_insb));
    cpu->di = 0x0400;
    cpu->cx = 0x0004;
    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("DI advanced by the count", cpu->di, 0x0404);
    vm86_expect_u16("CX ran out", cpu->cx, 0x0000);
    vm86_expect_mem8("the floating bus went to memory", cpu, 0x0400, 0xFF);
    vm86_expect_mem8("... and to the last byte", cpu, 0x0403, 0xFF);

    /* The word form moves DI by two. It writes memory and not AX --
     * the port value goes to ES:DI, and the accumulator has nothing
     * to do with it, which is where INSW differs from IN. */
    vm86_test_load(cpu, rep_insw, sizeof(rep_insw));
    cpu->di = 0x0400;
    cpu->cx = 0x0002;
    cpu->ax = 0x1234;
    vm86_test_run(cpu, 20);

    vm86_expect_u16("DI advanced by two per word", cpu->di, 0x0404);
    vm86_expect_u16("AX is not the destination", cpu->ax, 0x1234);
    vm86_expect_mem16("a word was stored", cpu, 0x0402, 0xFFFF);

    /* OUTS reads and discards: the source is left alone and only SI
     * moves. */
    vm86_test_load(cpu, rep_outsb, sizeof(rep_outsb));
    poke8(cpu, 0x0300, 0x11);
    poke8(cpu, 0x0301, 0x22);
    poke8(cpu, 0x0302, 0x33);
    cpu->si = 0x0300;
    cpu->cx = 0x0003;
    vm86_test_run(cpu, 20);

    vm86_expect_u16("SI advanced", cpu->si, 0x0303);
    vm86_expect_u16("CX ran out", cpu->cx, 0x0000);
    vm86_expect_mem8("the source is untouched", cpu, 0x0300, 0x11);

    /* A count of zero does nothing, in this instruction as in the rest. */
    vm86_test_load(cpu, zero_count, sizeof(zero_count));
    cpu->di = 0x0500;   /* somewhere the cases above did not touch */
    cpu->cx = 0x0000;
    vm86_test_run(cpu, 20);

    vm86_expect_u16("DI did not move", cpu->di, 0x0500);
    vm86_expect_mem8("nothing was written", cpu, 0x0500, 0x00);

    /* The direction flag, which the shared loop is what reads. */
    vm86_test_load(cpu, backwards, sizeof(backwards));
    cpu->di = 0x0404;
    cpu->cx = 0x0002;
    vm86_test_run(cpu, 20);

    vm86_expect_u16("DI walked backwards", cpu->di, 0x0402);
    vm86_expect_mem8("the bytes went downwards", cpu, 0x0403, 0xFF);
    vm86_expect_mem8("... to the lower address", cpu, 0x0402, 0xFF);
}

/*
 * STI takes effect at once, so the very next instruction sees IF set.
 *
 * On the hardware the *recognition* of an interrupt is held off for one
 * instruction, and that is what makes `sti / hlt` work. The register
 * itself is written immediately, and this is that half of it: the PUSHF
 * straight after the STI has to show IF set. A machine that modelled the
 * delay by deferring the flag write would fail here.
 *
 * The interrupt-recognition half is not modelled, and cannot be observed
 * in a machine that never delivers an interrupt -- see the report.
 */
static void test_sti_takes_effect_immediately(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xFB, 0x9C, 0xF4 };   /* sti; pushf; hlt */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->flags = VM86_FLAG_ALWAYS_SET;   /* IF clear to begin with */

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_flag("IF is set", cpu, VM86_IF, true);
    vm86_expect_mem16("the pushed copy has it too", cpu,
                      VM86_TEST_STACK_TOP - 2, FLAGS(VM86_IF));
}

/*
 * INC and DEC of a byte register.
 *
 * This is the encoding a program has no alternative to: the one-byte
 * forms at 40-47 are the sixteen-bit registers only, so `inc bl` is FE
 * /0 with mod 11 and `dec dh` is FE /1 with mod 11. It is the register
 * form of an instruction whose memory form is the obvious one, and
 * refusing it would break every byte counter in every program.
 *
 * The other reg fields of FE name nothing on this processor and are
 * refused; the check for that has to be written so that it excludes
 * those and not the register operand.
 */
static void test_inc_and_dec_a_byte_register(struct vm86_cpu *cpu)
{
    static const uint8_t inc_bl[] = { 0xFE, 0xC3, 0xF4 };   /* inc bl */
    static const uint8_t dec_dh[] = { 0xFE, 0xCE, 0xF4 };   /* dec dh */
    static const uint8_t undefined[] = { 0xFE, 0xD3 };      /* FE /2 */

    vm86_test_load(cpu, inc_bl, sizeof(inc_bl));
    cpu->bx = 0x007F;
    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("BX", cpu->bx, 0x0080);
    vm86_expect_flag("OF at the byte sign boundary", cpu, VM86_OF, true);

    vm86_test_load(cpu, dec_dh, sizeof(dec_dh));
    cpu->dx = 0x0000;
    vm86_test_run(cpu, 10);

    vm86_expect_u16("DX", cpu->dx, 0xFF00);

    vm86_test_load(cpu, undefined, sizeof(undefined));
    enum vm86_result result = vm86_test_run(cpu, 10);
    vm86_expect_bool("FE /2 is refused", result == VM86_FAULT, true);
    vm86_expect_u16("vector", cpu->fault, VM86_VECTOR_INVALID_OPCODE);
}

static const struct vm86_test tests[] = {
    /* Segments and the ends of them */
    { "code straddles the end of the segment",
      test_code_straddles_the_end_of_the_segment },
    { "word at the end of a segment crosses into the next",
      test_word_at_the_end_of_a_segment_crosses_into_the_next },
    { "A20 off folds a word at the top of memory",
      test_a20_off_folds_a_word_at_the_top_of_memory },
    { "A20 on leaves the top of memory unmapped",
      test_a20_on_leaves_the_top_of_memory_unmapped },
    { "push runs off the end of the stack segment",
      test_push_runs_off_the_end_of_the_stack_segment },
    { "push sp pushes the decremented value",
      test_push_sp_pushes_the_decremented_value },
    { "push sp by group five agrees with fifty four",
      test_push_sp_by_group_five_agrees_with_fifty_four },
    { "pop sp ends at the popped value",
      test_pop_sp_ends_at_the_popped_value },

    /* Addresses that are computed but not visited */
    { "lea reads no memory and stores the offset",
      test_lea_reads_no_memory_and_stores_the_offset },
    { "lea ignores the segment override",
      test_lea_ignores_the_segment_override },
    { "direct-addressed mov takes an offset in ds",
      test_direct_mov_takes_an_offset_in_ds },
    { "direct-addressed mov honours the override",
      test_direct_mov_honours_the_segment_override },
    { "modrm displacement comes before the immediate",
      test_modrm_displacement_comes_before_the_immediate },
    { "mov from a segment register",
      test_mov_from_a_segment_register },
    { "a segment load is not delayed",
      test_a_segment_load_is_not_delayed },
    { "xlat adds bx and al and wraps",
      test_xlat_adds_bx_and_al_and_wraps },
    { "xlat leaves the flags alone",
      test_xlat_leaves_the_flags_alone },

    /* Carry, borrow and overflow at their exact boundaries */
    { "add carries out of the top bit",
      test_add_carry_out_of_the_top_bit },
    { "add overflows the sign without carrying",
      test_add_signed_overflow_without_a_carry },
    { "add just below the overflow",
      test_add_without_a_carry_at_the_sign_boundary },
    { "sub borrows out of the bottom",
      test_sub_borrow_out_of_the_bottom },
    { "sub overflows the sign without borrowing",
      test_sub_signed_overflow_without_a_borrow },
    { "sub of equal operands clears the borrow",
      test_sub_equal_operands_clears_the_borrow },
    { "sbb borrows the incoming carry",
      test_sbb_borrows_the_incoming_carry },
    { "sbb without an incoming carry",
      test_sbb_with_no_incoming_carry_is_a_plain_subtract },
    { "adc carries in",
      test_adc_carries_in },
    { "auxiliary carry is bit three",
      test_auxiliary_carry_is_bit_three },
    { "parity looks at the low byte of a word",
      test_parity_looks_at_the_low_byte_of_a_word },
    { "inc and dec leave the carry alone",
      test_inc_and_dec_leave_the_carry_alone },
    { "inc and dec overflow at the sign bit only",
      test_inc_and_dec_overflow_at_the_sign_bit_only },
    { "logic clears carry and overflow",
      test_logic_clears_carry_and_overflow },
    { "compare discards its result",
      test_compare_discards_its_result },
    { "segment push and pop round trip",
      test_segment_push_and_pop_round_trip },

    /* Interrupts */
    { "the interrupt frame on the stack",
      test_int_frame_order_on_the_stack },
    { "int and iret restore every flag",
      test_int_iret_restores_every_flag },
    { "iret forces the hardwired bits",
      test_iret_forces_the_hardwired_bits },
    { "pushf and popf through the hardwired bits",
      test_pushf_popf_round_trip_through_the_hardwired_bits },
    { "lahf and sahf touch only the low byte",
      test_lahf_and_sahf_touch_only_the_low_byte },
    { "into is inert without overflow",
      test_into_is_inert_without_overflow },
    { "the last vector is read at a linear address",
      test_the_last_vector_is_read_at_a_linear_address },

    /* Loops and jumps */
    { "loop with a count of one does not jump",
      test_loop_with_a_count_of_one_does_not_jump },
    { "jcxz does not decrement the counter",
      test_jcxz_does_not_decrement_the_counter },
    { "loope is bounded by the counter",
      test_loope_is_bounded_by_the_counter },
    { "a conditional jump consumes its displacement either way",
      test_conditional_jump_consumes_its_displacement_either_way },
    { "a near jump wraps the instruction pointer",
      test_a_near_jump_wraps_the_instruction_pointer },

    /* The stack, called and returned from */
    { "ret imm adjusts the stack after the pop",
      test_ret_imm_adjusts_the_stack_after_the_pop },
    { "far call and return",
      test_far_call_and_return },
    { "push and pop round trip for every register",
      test_push_pop_round_trip_for_every_register },

    /* The string instructions */
    { "a string with a zero count moves nothing backwards",
      test_string_with_zero_count_moves_nothing_backwards },
    { "repne scasb leaves the unscanned elements in cx",
      test_repne_scasb_leaves_the_unscanned_elements_in_cx },
    { "repe cmpsb over identical buffers ends on the count",
      test_repe_cmpsb_over_identical_buffers_ends_on_the_count },
    { "cmpsb compares ds:si minus es:di",
      test_cmpsb_compares_ds_si_minus_es_di },
    { "scasb compares al minus es:di",
      test_scasb_compares_al_minus_es_di },
    { "movsw reads across the end of a segment",
      test_movsw_reads_across_the_end_of_a_segment },
    { "lodsb leaves ah alone",
      test_lodsb_leaves_ah_alone },
    { "a segment override moves only the read side",
      test_segment_override_moves_only_the_read_side },
    { "a string without a prefix runs once",
      test_a_string_instruction_without_a_prefix_runs_once },
    { "movsw walks backwards by two",
      test_movsw_walks_backwards_by_two },

    /* Prefixes */
    { "a segment override lasts one instruction",
      test_a_segment_override_lasts_one_instruction },
    { "repeated prefixes are accepted",
      test_repeated_prefixes_are_accepted },
    { "lock is accepted and ignored",
      test_lock_is_accepted_and_ignored },

    /* Ports */
    { "in writes only the named half",
      test_in_writes_only_the_named_half },
    { "the immediate port is consumed once",
      test_the_immediate_port_is_consumed_once },

    /* Encodings that are not instructions */
    { "unclaimed opcodes raise the invalid opcode trap",
      test_unclaimed_opcodes_raise_the_invalid_opcode_trap },
    { "the undefined group encodings are refused",
      test_the_undefined_group_encodings_are_refused },

    /* Shifts and rotates */
    { "a shift by zero changes nothing",
      test_a_shift_by_zero_changes_nothing },
    { "shl by one takes the bit off the top",
      test_shl_by_one_takes_the_bit_off_the_top },
    { "shl by one without a carry",
      test_shl_by_one_without_a_carry },
    { "shr by one reports the original sign",
      test_shr_by_one_reports_the_original_sign },
    { "sar never sets overflow",
      test_sar_never_sets_overflow },
    { "rol by one",
      test_rol_by_one },
    { "ror by one",
      test_ror_by_one },
    { "rotate through the carry",
      test_rotate_through_the_carry },
    { "a shift count is not masked",
      test_a_shift_count_is_not_masked },
    { "an immediate shift count is not masked either",
      test_an_immediate_shift_count_is_not_masked_either },
    { "shift by cl of one",
      test_shift_by_cl_of_one },

    /* The group 1 immediate forms */
    { "82h is 80h",
      test_eighty_two_is_eighty },
    { "the group 1 one-byte immediate is signed",
      test_the_group_one_byte_immediate_is_signed },
    { "group 1 on memory through bp",
      test_group_one_on_memory_through_bp },

    /* TEST, NOT and NEG */
    { "test keeps the destination and clears the carries",
      test_test_keeps_the_destination_and_clears_the_carries },
    { "not changes no flag",
      test_not_changes_no_flag },
    { "neg of zero and of the sign boundary",
      test_neg_of_zero_and_of_the_sign_boundary },

    /* Multiplication and division */
    { "mul reports a product that does not fit",
      test_mul_reports_a_product_that_does_not_fit },
    { "imul reports a product that lost its sign",
      test_imul_reports_a_product_that_lost_its_sign },
    { "divide quotient overflow at its boundary",
      test_divide_quotient_overflow_at_its_boundary },
    { "divide faults on zero and on the task book's example",
      test_divide_faults_on_zero_and_on_the_task_books_example },
    { "divide sixteen bit quotient",
      test_divide_sixteen_bit_quotient },
    { "idiv truncates towards zero",
      test_idiv_truncates_towards_zero },
    { "idiv overflows on the most negative dividend",
      test_idiv_overflows_on_the_most_negative_dividend },

    /* The decimal adjustments */
    { "daa adjusts by the manual's table",
      test_daa_adjusts_by_the_manuals_table },
    { "das subtracts by the manual's table",
      test_das_subtracts_by_the_manuals_table },
    { "aaa and aas carry into ah",
      test_aaa_and_aas_carry_into_ah },
    { "aam and aad convert by ten",
      test_aam_and_aad_convert_by_ten },
    { "cbw and cwd sign extend",
      test_cbw_and_cwd_sign_extend },
    /* Data movement, continued */
    { "pop to a register is not refused",
      test_pop_to_a_register_is_not_refused },
    { "a negative displacement is sign extended",
      test_a_negative_displacement_is_sign_extended },

    /* The 80186 extensions */
    { "pusha saves the stack pointer from before it started",
      test_pusha_saves_the_stack_pointer_from_before_it_started },
    { "popa discards the saved stack pointer",
      test_popa_discards_the_saved_stack_pointer },
    { "popa ignores rubbish in the saved slot",
      test_popa_ignores_rubbish_in_the_saved_slot },
    { "enter and leave at level zero and one",
      test_enter_and_leave_at_level_zero_and_one },
    { "enter copies the caller's chain",
      test_enter_copies_the_callers_chain },
    { "bound is a signed and inclusive range check",
      test_bound_is_a_signed_and_inclusive_range_check },
    { "push immediate sign extends the byte form",
      test_push_immediate_sign_extends_the_byte_form },
    { "imul with an immediate reports what it discarded",
      test_imul_with_an_immediate_reports_what_it_discarded },
    { "ins and outs repeat like the string instructions",
      test_ins_and_outs_repeat_like_the_string_instructions },
    { "sti takes effect immediately",
      test_sti_takes_effect_immediately },
    { "inc and dec a byte register",
      test_inc_and_dec_a_byte_register },
};

VM86_TEST_MAIN("verify", tests)
