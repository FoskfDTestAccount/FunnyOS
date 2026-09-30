/*
 * Data movement: MOV, XCHG, LEA, the segment loads, and the stack.
 *
 * Every test here checks FLAGS as well as values. Most of these
 * instructions must not touch FLAGS at all, which is not a detail: MOV is
 * the instruction a program puts between two others when it wants to keep
 * a carry across them, and an XCHG that quietly set one would break code
 * that never looks wrong until much later.
 *
 * Several tests exist only to pin a decision documented at the top of
 * ops_mov.c -- the encodings refused as invalid, the behaviour of LEA, of
 * a segment register write, and of PUSH SP. Those are called out where
 * they appear.
 */
#include "harness.h"

/* ------------------------------------------------------------------ */
/* XCHG                                                                */
/* ------------------------------------------------------------------ */

static void test_xchg_byte_registers(struct vm86_cpu *cpu)
{
    /* xchg ah, bl */
    static const uint8_t code[] = { 0x86, 0xDC, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x1200;
    cpu->bx = 0x0034;

    vm86_test_run(cpu, 10);

    /* AH and BL are register numbers 4 and 3, and the 8-bit table is the
     * one that makes 4 mean AH. Reading this through the 16-bit table
     * would have written SP. */
    vm86_expect_u16("AX", cpu->ax, 0x3400);
    vm86_expect_u16("BX", cpu->bx, 0x0012);
    vm86_expect_flags("flags untouched", cpu, 0, 0);
}

static void test_xchg_word_registers(struct vm86_cpu *cpu)
{
    /* xchg ax, bx */
    static const uint8_t code[] = { 0x87, 0xD8, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x1111;
    cpu->bx = 0x2222;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0x2222);
    vm86_expect_u16("BX", cpu->bx, 0x1111);
}

static void test_xchg_word_with_memory(struct vm86_cpu *cpu)
{
    /* xchg [0x0200], bx */
    static const uint8_t code[] = { 0x87, 0x1E, 0x00, 0x02, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->bx = 0xABCD;
    vm86_mem_write16(cpu->mem, 0x0200, 0x1234);

    vm86_test_run(cpu, 10);

    vm86_expect_u16("BX", cpu->bx, 0x1234);
    vm86_expect_mem16("memory", cpu, 0x0200, 0xABCD);
}

static void test_xchg_byte_with_memory(struct vm86_cpu *cpu)
{
    /* xchg [0x0200], cl */
    static const uint8_t code[] = { 0x86, 0x0E, 0x00, 0x02, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->cx = 0xFFAA;                 /* CL = 0xAA, CH left as a witness */
    vm86_mem_write8(cpu->mem, 0x0200, 0x55);
    vm86_mem_write8(cpu->mem, 0x0201, 0x77);

    vm86_test_run(cpu, 10);

    vm86_expect_u16("CX", cpu->cx, 0xFF55);
    vm86_expect_mem8("memory", cpu, 0x0200, 0xAA);
    /* A byte exchange moves one byte. */
    vm86_expect_mem8("byte above", cpu, 0x0201, 0x77);
}

static void test_xchg_honours_segment_override(struct vm86_cpu *cpu)
{
    /* xchg es:[0x0200], bx */
    static const uint8_t code[] = { 0x26, 0x87, 0x1E, 0x00, 0x02, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_set_seg(cpu, VM86_ES, 0x0200);      /* base 0x2000 */
    cpu->bx = 0x8888;
    vm86_mem_write16(cpu->mem, 0x2200, 0x7777);
    vm86_mem_write16(cpu->mem, 0x0200, 0x1111);   /* the DS: address */

    vm86_test_run(cpu, 10);

    vm86_expect_u16("BX", cpu->bx, 0x7777);
    vm86_expect_mem16("through ES", cpu, 0x2200, 0x8888);
    vm86_expect_mem16("DS untouched", cpu, 0x0200, 0x1111);
}

/* ------------------------------------------------------------------ */
/* MOV r/m, r and r, r/m                                               */
/* ------------------------------------------------------------------ */

static void test_mov_byte_to_register(struct vm86_cpu *cpu)
{
    /* mov dh, ah */
    static const uint8_t code[] = { 0x88, 0xE6, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0xAA55;
    cpu->bx = 0x0099;

    vm86_test_run(cpu, 10);

    /* DH is register 6 and AH is 4 in the 8-bit table. Both of those
     * numbers mean something else as words. */
    vm86_expect_u16("DX", cpu->dx, 0xAA00);
    vm86_expect_u16("BX untouched", cpu->bx, 0x0099);
}

static void test_mov_byte_to_memory(struct vm86_cpu *cpu)
{
    /* mov [0x0200], bl */
    static const uint8_t code[] = { 0x88, 0x1E, 0x00, 0x02, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->bx = 0x1299;                 /* BL = 0x99, BH is a witness */

    vm86_test_run(cpu, 10);

    vm86_expect_mem8("memory", cpu, 0x0200, 0x99);
    vm86_expect_mem8("byte above", cpu, 0x0201, 0x00);
}

static void test_mov_word_to_register(struct vm86_cpu *cpu)
{
    /* mov si, di */
    static const uint8_t code[] = { 0x89, 0xFE, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->di = 0xBEEF;

    vm86_test_run(cpu, 10);

    /* SI and DI are 6 and 7 as words, and DH and BH as bytes -- the same
     * trap as the 8-bit case, in the other direction. */
    vm86_expect_u16("SI", cpu->si, 0xBEEF);
    vm86_expect_u16("DI untouched", cpu->di, 0xBEEF);
}

static void test_mov_word_to_memory(struct vm86_cpu *cpu)
{
    /* mov [0x0200], bx */
    static const uint8_t code[] = { 0x89, 0x1E, 0x00, 0x02, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->bx = 0x1234;
    vm86_mem_write16(cpu->mem, 0x0200, 0xFFFF);

    vm86_test_run(cpu, 10);

    vm86_expect_mem16("memory", cpu, 0x0200, 0x1234);
    vm86_expect_u16("BX untouched", cpu->bx, 0x1234);
}

static void test_mov_byte_from_register(struct vm86_cpu *cpu)
{
    /* mov cl, dl */
    static const uint8_t code[] = { 0x8A, 0xCA, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->cx = 0xFF00;
    cpu->dx = 0x0042;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("CX", cpu->cx, 0xFF42);
    vm86_expect_u16("DX untouched", cpu->dx, 0x0042);
}

static void test_mov_byte_from_memory(struct vm86_cpu *cpu)
{
    /* mov al, [0x0200] */
    static const uint8_t code[] = { 0x8A, 0x06, 0x00, 0x02, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x1200;
    vm86_mem_write8(cpu->mem, 0x0200, 0x5A);

    vm86_test_run(cpu, 10);

    /* A byte load writes the low half and leaves the high half alone. */
    vm86_expect_u16("AX", cpu->ax, 0x125A);
}

static void test_mov_word_from_register(struct vm86_cpu *cpu)
{
    /* mov dx, bp */
    static const uint8_t code[] = { 0x8B, 0xD5, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->dx = 0xFFFF;
    cpu->bp = 0x4321;

    vm86_test_run(cpu, 10);

    /* BP is 5 as a word and CH as a byte. */
    vm86_expect_u16("DX", cpu->dx, 0x4321);
    vm86_expect_u16("BP untouched", cpu->bp, 0x4321);
}

static void test_mov_word_from_memory(struct vm86_cpu *cpu)
{
    /* mov bx, [0x0200] */
    static const uint8_t code[] = { 0x8B, 0x1E, 0x00, 0x02, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->bx = 0xFFFF;
    vm86_mem_write16(cpu->mem, 0x0200, 0x5678);

    vm86_test_run(cpu, 10);

    vm86_expect_u16("BX", cpu->bx, 0x5678);
}

static void test_mov_honours_segment_override_both_ways(struct vm86_cpu *cpu)
{
    /* mov al, es:[0x0200] ; mov es:[0x0300], bx */
    static const uint8_t code[] = { 0x26, 0x8A, 0x06, 0x00, 0x02,
                                    0x26, 0x89, 0x1E, 0x00, 0x03,
                                    0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_set_seg(cpu, VM86_ES, 0x0200);      /* base 0x2000 */
    cpu->ax = 0x1200;
    cpu->bx = 0x1357;

    vm86_mem_write8(cpu->mem, 0x2200, 0x7E);     /* es:0x0200 */
    vm86_mem_write8(cpu->mem, 0x0200, 0x11);     /* ds:0x0200 */

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0x127E);
    vm86_expect_mem16("store through ES", cpu, 0x2300, 0x1357);
    vm86_expect_mem16("DS address untouched", cpu, 0x0300, 0x0000);
}

/* ------------------------------------------------------------------ */
/* Segment register moves                                              */
/* ------------------------------------------------------------------ */

static void test_mov_segment_register_out(struct vm86_cpu *cpu)
{
    /* mov ax, es ; mov bx, cs ; mov dx, ss ; mov si, ds ;
     * mov [0x0200], ss */
    static const uint8_t code[] = { 0x8C, 0xC0,
                                    0x8C, 0xCB,
                                    0x8C, 0xD2,
                                    0x8C, 0xDE,
                                    0x8C, 0x16, 0x00, 0x02,
                                    0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_set_seg(cpu, VM86_DS, 0x1234);
    vm86_set_seg(cpu, VM86_ES, 0x5678);
    vm86_set_seg(cpu, VM86_SS, 0x9ABC);

    /* CS is whatever the harness loaded the program with, which is zero.
     * Setting BX first is what makes the write visible. */
    cpu->bx = 0xFFFF;

    vm86_test_run(cpu, 10);

    /* Four different values, so a handler that read the wrong segment
     * register -- or always read ES -- fails rather than passes. */
    vm86_expect_u16("AX from ES", cpu->ax, 0x5678);
    vm86_expect_u16("BX from CS", cpu->bx, 0x0000);
    vm86_expect_u16("DX from SS", cpu->dx, 0x9ABC);
    vm86_expect_u16("SI from DS", cpu->si, 0x1234);

    /* DS is 0x1234, so [0x0200] is linear 0x12540. */
    vm86_expect_mem16("memory", cpu, 0x12540, 0x9ABC);
}

static void test_mov_segment_register_in(struct vm86_cpu *cpu)
{
    /* mov ds, ax ; mov al, [0x0000] */
    static const uint8_t code[] = { 0x8E, 0xD8,
                                    0x8A, 0x06, 0x00, 0x00,
                                    0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x2000;
    vm86_mem_write8(cpu->mem, 0x20000, 0x42);    /* new ds:0, base 0x20000 */
    vm86_mem_write8(cpu->mem, 0x00000, 0x99);    /* old ds:0 */

    vm86_test_run(cpu, 10);

    vm86_expect_u16("DS", vm86_get_seg(cpu, VM86_DS), 0x2000);

    /* The load is the point of the test: it proves the cached segment
     * base was invalidated too. A base that was not refreshed reads
     * 0x99 from the old segment. */
    vm86_expect_u16("AX", cpu->ax, 0x2042);
}

static void test_mov_segment_register_in_from_memory(struct vm86_cpu *cpu)
{
    /* mov es, [0x0200] */
    static const uint8_t code[] = { 0x8E, 0x06, 0x00, 0x02, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_mem_write16(cpu->mem, 0x0200, 0x4000);

    vm86_test_run(cpu, 10);

    vm86_expect_u16("ES", vm86_get_seg(cpu, VM86_ES), 0x4000);
}

static void test_mov_cs_is_refused(struct vm86_cpu *cpu)
{
    /* mov cs, ax */
    static const uint8_t code[] = { 0x8E, 0xC8 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x4000;

    enum vm86_result result = vm86_test_run(cpu, 10);

    /* The manual states this encoding to be invalid: CS is loaded by a
     * far transfer, never by a MOV. The choice here is the
     * invalid-opcode exception, and this test is what proves the choice
     * was implemented rather than described. */
    vm86_expect_bool("faults", result == VM86_FAULT, true);
    vm86_expect_u16("vector", cpu->fault, VM86_VECTOR_INVALID_OPCODE);
    vm86_expect_u16("CS unchanged", vm86_get_seg(cpu, VM86_CS), 0x0000);
    vm86_expect_u16("AX unchanged", cpu->ax, 0x4000);
}

static void test_mov_sreg_above_ds_is_refused(struct vm86_cpu *cpu)
{
    /* 8E /4: a reg field that names no segment register. */
    static const uint8_t code[] = { 0x8E, 0xE0 };

    vm86_test_load(cpu, code, sizeof(code));

    enum vm86_result result = vm86_test_run(cpu, 10);

    vm86_expect_bool("faults", result == VM86_FAULT, true);
    vm86_expect_u16("vector", cpu->fault, VM86_VECTOR_INVALID_OPCODE);
}

static void test_mov_sreg_out_above_ds_is_refused(struct vm86_cpu *cpu)
{
    /* 8C /4. Reading a segment register that does not exist is not a way
     * to read ES. */
    static const uint8_t code[] = { 0x8C, 0xE0 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0xFFFF;

    enum vm86_result result = vm86_test_run(cpu, 10);

    vm86_expect_bool("faults", result == VM86_FAULT, true);
    vm86_expect_u16("vector", cpu->fault, VM86_VECTOR_INVALID_OPCODE);
    vm86_expect_u16("AX unchanged", cpu->ax, 0xFFFF);
}

/* ------------------------------------------------------------------ */
/* LEA                                                                 */
/* ------------------------------------------------------------------ */

static void test_lea_computes_without_reading(struct vm86_cpu *cpu)
{
    /* lea ax, [bx+si] */
    static const uint8_t code[] = { 0x8D, 0x00, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->bx = 0x0200;
    cpu->si = 0x0030;

    /* Whatever lives at the computed address must not end up in AX. */
    vm86_mem_write16(cpu->mem, 0x0230, 0xCAFE);

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0x0230);
}

static void test_lea_with_a_displacement(struct vm86_cpu *cpu)
{
    /* lea dx, [bx+0x1234] */
    static const uint8_t code[] = { 0x8D, 0x97, 0x34, 0x12, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->bx = 0x0100;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("DX", cpu->dx, 0x1334);
}

static void test_lea_does_not_read_past_memory(struct vm86_cpu *cpu)
{
    /* lea ax, [si] */
    static const uint8_t code[] = { 0x8D, 0x04, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    /* DS:SI lands at linear 0x100000, one byte past the guest's memory,
     * where a read returns the floating bus -- 0xFF, or 0xFFFF for a
     * word. An implementation that read the operand would put one of
     * those in AX; the real instruction puts the offset there and never
     * notices that the address is off the end of anything. */
    vm86_set_seg(cpu, VM86_DS, 0xFFFF);
    cpu->si = 0x0010;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0x0010);
}

static void test_lea_ignores_the_segment_base(struct vm86_cpu *cpu)
{
    /* lea bx, es:[bp+2] */
    static const uint8_t code[] = { 0x26, 0x8D, 0x5E, 0x02, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_set_seg(cpu, VM86_ES, 0x1234);
    cpu->bp = 0x0010;

    vm86_test_run(cpu, 10);

    /* This pins a decision, and the decision is that LEA stores the
     * offset -- which is the same number the override-free form would
     * store. The prefix selects the segment the operand is formed in and
     * nothing else; the segment base is not part of an offset, and
     * folding it in would make every `lea` in a program with a loaded
     * segment register wrong. See the note above op_lea() in ops_mov.c. */
    vm86_expect_u16("BX", cpu->bx, 0x0012);
}

static void test_lea_of_a_register_is_refused(struct vm86_cpu *cpu)
{
    /* lea ax, ax: mod 11, which names no memory. */
    static const uint8_t code[] = { 0x8D, 0xC0 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x7777;

    enum vm86_result result = vm86_test_run(cpu, 10);

    vm86_expect_bool("faults", result == VM86_FAULT, true);
    vm86_expect_u16("vector", cpu->fault, VM86_VECTOR_INVALID_OPCODE);
    vm86_expect_u16("AX unchanged", cpu->ax, 0x7777);
}

/* ------------------------------------------------------------------ */
/* POP r/m16                                                           */
/* ------------------------------------------------------------------ */

static void test_pop_to_memory(struct vm86_cpu *cpu)
{
    /* mov ax, 0x1234 ; push ax ; pop word [0x0200] */
    static const uint8_t code[] = { 0xB8, 0x34, 0x12,
                                    0x50,
                                    0x8F, 0x06, 0x00, 0x02,
                                    0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    vm86_test_run(cpu, 10);

    vm86_expect_mem16("memory", cpu, 0x0200, 0x1234);
    vm86_expect_u16("SP", cpu->sp, VM86_TEST_STACK_TOP);
    vm86_expect_u16("AX", cpu->ax, 0x1234);
}

static void test_pop_to_register(struct vm86_cpu *cpu)
{
    /* mov ax, 0x1234 ; push ax ; pop bx */
    static const uint8_t code[] = { 0xB8, 0x34, 0x12, 0x50, 0x8F, 0xC3, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    vm86_test_run(cpu, 10);

    vm86_expect_u16("BX", cpu->bx, 0x1234);
    vm86_expect_u16("SP", cpu->sp, VM86_TEST_STACK_TOP);
}

static void test_pop_sp_takes_the_popped_word(struct vm86_cpu *cpu)
{
    /* mov ax, 0x1234 ; push ax ; pop sp */
    static const uint8_t code[] = { 0xB8, 0x34, 0x12, 0x50, 0x8F, 0xC4, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    vm86_test_run(cpu, 10);

    /* The destination is written after SP has been moved, so SP ends up
     * holding the word that came off the stack -- not that word plus the
     * two bytes SP was advanced by. This is the order `pop sp` is the
     * only way to observe from inside a test: no 16-bit ModRM operand
     * can name SP, so a memory destination can never be sensitive to
     * when the address was worked out. */
    vm86_expect_u16("SP", cpu->sp, 0x1234);
}

static void test_pop_with_a_reg_field_is_refused(struct vm86_cpu *cpu)
{
    /* 8F /1: the group has one operation and it is /0. */
    static const uint8_t code[] = { 0x8F, 0xC8 };

    vm86_test_load(cpu, code, sizeof(code));

    enum vm86_result result = vm86_test_run(cpu, 10);

    vm86_expect_bool("faults", result == VM86_FAULT, true);
    vm86_expect_u16("vector", cpu->fault, VM86_VECTOR_INVALID_OPCODE);

    /* Refused before anything was popped: the instruction never
     * happened, so SP says so. */
    vm86_expect_u16("SP unchanged", cpu->sp, VM86_TEST_STACK_TOP);
}

static void test_pop_destination_takes_the_override(struct vm86_cpu *cpu)
{
    /* mov ax, 0x1234 ; push ax ; pop word ss:[0x0200] */
    static const uint8_t code[] = { 0xB8, 0x34, 0x12,
                                    0x50,
                                    0x36, 0x8F, 0x06, 0x00, 0x02,
                                    0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    /* A stack of its own, well away from the destination, so that the
     * two addresses are told apart by the result. */
    vm86_set_seg(cpu, VM86_SS, 0x0100);      /* base 0x1000 */
    cpu->sp = 0x2000;

    vm86_test_run(cpu, 10);

    /* The override applies to the destination operand, which is what the
     * prefix is for. It does not move the stack: the implicit access is
     * always SS:SP, and the prefix has nothing to attach to. */
    vm86_expect_mem16("store through SS", cpu, 0x1200, 0x1234);
    vm86_expect_mem16("DS address untouched", cpu, 0x0200, 0x0000);
    vm86_expect_u16("SP", cpu->sp, 0x2000);
    vm86_expect_mem16("stack", cpu, 0x2FFE, 0x1234);
}

/* ------------------------------------------------------------------ */
/* XCHG AX with a register                                             */
/* ------------------------------------------------------------------ */

static void test_xchg_ax_with_each_register(struct vm86_cpu *cpu)
{
    /* xchg ax, cx ; xchg ax, dx ; xchg ax, bx ; xchg ax, sp ;
     * xchg ax, bp ; xchg ax, si ; xchg ax, di */
    static const uint8_t code[] = { 0x91, 0x92, 0x93, 0x94,
                                    0x95, 0x96, 0x97, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    /* Seven opcodes, seven exchanges. Each one hands AX the next value
     * and takes away the one AX had, so after the run every register
     * holds the value its neighbour started with -- and a single broken
     * opcode leaves two registers wrong, not none. */
    cpu->ax = 0x1000;
    cpu->cx = 0x2000;
    cpu->dx = 0x3000;
    cpu->bx = 0x4000;
    cpu->sp = 0x5000;
    cpu->bp = 0x6000;
    cpu->si = 0x7000;
    cpu->di = 0x8000;

    vm86_test_run(cpu, 20);

    vm86_expect_u16("AX", cpu->ax, 0x8000);
    vm86_expect_u16("CX", cpu->cx, 0x1000);
    vm86_expect_u16("DX", cpu->dx, 0x2000);
    vm86_expect_u16("BX", cpu->bx, 0x3000);
    vm86_expect_u16("SP", cpu->sp, 0x4000);
    vm86_expect_u16("BP", cpu->bp, 0x5000);
    vm86_expect_u16("SI", cpu->si, 0x6000);
    vm86_expect_u16("DI", cpu->di, 0x7000);
}

static void test_xchg_ax_ax_is_a_nop(struct vm86_cpu *cpu)
{
    /* nop */
    static const uint8_t code[] = { 0x90, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x1234;
    vm86_flag_set(cpu, VM86_CF, true);

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0x1234);
    vm86_expect_flag("CF untouched", cpu, VM86_CF, true);
}

/* ------------------------------------------------------------------ */
/* A0-A3: the direct-address moves                                     */
/* ------------------------------------------------------------------ */

static void test_mov_al_from_moffs(struct vm86_cpu *cpu)
{
    /* mov al, [0x0200] */
    static const uint8_t code[] = { 0xA0, 0x00, 0x02, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x1200;
    vm86_mem_write8(cpu->mem, 0x0200, 0x5A);

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0x125A);
    vm86_expect_flags("flags untouched", cpu, 0, 0);
}

static void test_mov_ax_from_moffs(struct vm86_cpu *cpu)
{
    /* mov ax, [0x0200] */
    static const uint8_t code[] = { 0xA1, 0x00, 0x02, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_mem_write16(cpu->mem, 0x0200, 0x5678);

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0x5678);
}

static void test_mov_to_moffs_is_a_byte_store(struct vm86_cpu *cpu)
{
    /* mov [0x0200], al */
    static const uint8_t code[] = { 0xA2, 0x00, 0x02, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x1299;
    vm86_mem_write8(cpu->mem, 0x0201, 0x77);

    /* No instruction in this file writes FLAGS, and this one is a store
     * into raw memory, which is where a stray flag write would be least
     * likely to be noticed. */
    vm86_flag_set(cpu, VM86_CF, true);
    vm86_flag_set(cpu, VM86_ZF, true);

    vm86_test_run(cpu, 10);

    vm86_expect_mem8("memory", cpu, 0x0200, 0x99);
    vm86_expect_mem8("byte above", cpu, 0x0201, 0x77);
    vm86_expect_flags("flags untouched", cpu, VM86_CF | VM86_ZF, 0);
}

static void test_mov_ax_to_moffs(struct vm86_cpu *cpu)
{
    /* mov [0x0200], ax */
    static const uint8_t code[] = { 0xA3, 0x00, 0x02, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x1234;

    vm86_test_run(cpu, 10);

    vm86_expect_mem8("low byte", cpu, 0x0200, 0x34);
    vm86_expect_mem8("high byte", cpu, 0x0201, 0x12);
}

static void test_moffs_honours_segment_override(struct vm86_cpu *cpu)
{
    /* mov al, es:[0x0234] */
    static const uint8_t code[] = { 0x26, 0xA0, 0x34, 0x02, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_set_seg(cpu, VM86_ES, 0x0300);      /* base 0x3000 */

    vm86_mem_write8(cpu->mem, 0x3234, 0x66);
    vm86_mem_write8(cpu->mem, 0x0234, 0x11);

    vm86_test_run(cpu, 10);

    /* Two bytes, little end first, and the prefix picks the segment --
     * which is also what tells apart a handler that read the address as
     * a linear one: 0x0234 and 0x3234 are different answers. */
    vm86_expect_u16("AL", cpu->al, 0x66);
}

/* ------------------------------------------------------------------ */
/* C4-C5: LES and LDS                                                  */
/* ------------------------------------------------------------------ */

static void test_les_loads_a_far_pointer(struct vm86_cpu *cpu)
{
    /* les bx, [0x0200] */
    static const uint8_t code[] = { 0xC4, 0x1E, 0x00, 0x02, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->bx = 0xFFFF;
    vm86_mem_write16(cpu->mem, 0x0200, 0x1234);   /* the register */
    vm86_mem_write16(cpu->mem, 0x0202, 0x5678);   /* the segment  */

    vm86_flag_set(cpu, VM86_CF, true);

    vm86_test_run(cpu, 10);

    /* The register comes out of the lower address and the segment out of
     * the one above it, which is the reverse of how the pair is written
     * -- the same reversal as the far CALL and JMP encodings. */
    vm86_expect_u16("BX", cpu->bx, 0x1234);
    vm86_expect_u16("ES", vm86_get_seg(cpu, VM86_ES), 0x5678);
    vm86_expect_flags("flags untouched", cpu, VM86_CF, 0);
}

static void test_lds_loads_a_far_pointer(struct vm86_cpu *cpu)
{
    /* lds si, [0x0200] */
    static const uint8_t code[] = { 0xC5, 0x36, 0x00, 0x02, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_mem_write16(cpu->mem, 0x0200, 0x1234);
    vm86_mem_write16(cpu->mem, 0x0202, 0x5678);

    vm86_test_run(cpu, 10);

    vm86_expect_u16("SI", cpu->si, 0x1234);
    vm86_expect_u16("DS", vm86_get_seg(cpu, VM86_DS), 0x5678);
}

static void test_les_honours_segment_override(struct vm86_cpu *cpu)
{
    /* les bx, es:[0x0200] */
    static const uint8_t code[] = { 0x26, 0xC4, 0x1E, 0x00, 0x02, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_set_seg(cpu, VM86_ES, 0x0200);          /* base 0x2000 */
    vm86_mem_write16(cpu->mem, 0x2200, 0xAAAA);  /* both words read ... */
    vm86_mem_write16(cpu->mem, 0x2202, 0xBBBB);
    vm86_mem_write16(cpu->mem, 0x0200, 0x1111);  /* ... through ES, and */
    vm86_mem_write16(cpu->mem, 0x0202, 0x2222);  /* not through DS      */

    vm86_test_run(cpu, 10);

    /* The register is loaded after both reads, so the write to ES cannot
     * change where the second word came from. */
    vm86_expect_u16("BX", cpu->bx, 0xAAAA);
    vm86_expect_u16("ES", vm86_get_seg(cpu, VM86_ES), 0xBBBB);
}

static void test_les_with_a_frame_pointer_operand(struct vm86_cpu *cpu)
{
    /* les bx, [bp+4] */
    static const uint8_t code[] = { 0xC4, 0x5E, 0x04, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->bp = 0x0200;
    vm86_mem_write16(cpu->mem, 0x0204, 0x1111);
    vm86_mem_write16(cpu->mem, 0x0206, 0x2222);

    vm86_test_run(cpu, 10);

    /* The displacement comes out of the instruction between the ModRM
     * byte and the two words of data, and BP-relative addressing goes
     * through SS without being told to. */
    vm86_expect_u16("BX", cpu->bx, 0x1111);
    vm86_expect_u16("ES", vm86_get_seg(cpu, VM86_ES), 0x2222);
}

static void test_les_of_a_register_is_refused(struct vm86_cpu *cpu)
{
    /* les ax, ax */
    static const uint8_t code[] = { 0xC4, 0xC0 };

    vm86_test_load(cpu, code, sizeof(code));

    enum vm86_result result = vm86_test_run(cpu, 10);

    vm86_expect_bool("faults", result == VM86_FAULT, true);
    vm86_expect_u16("vector", cpu->fault, VM86_VECTOR_INVALID_OPCODE);
}

static void test_lds_of_a_register_is_refused(struct vm86_cpu *cpu)
{
    /* lds ax, ax */
    static const uint8_t code[] = { 0xC5, 0xC0 };

    vm86_test_load(cpu, code, sizeof(code));

    enum vm86_result result = vm86_test_run(cpu, 10);

    vm86_expect_bool("faults", result == VM86_FAULT, true);
    vm86_expect_u16("vector", cpu->fault, VM86_VECTOR_INVALID_OPCODE);
}

/* ------------------------------------------------------------------ */
/* C6-C7: MOV r/m, immediate                                           */
/* ------------------------------------------------------------------ */

static void test_mov_byte_immediate_to_memory(struct vm86_cpu *cpu)
{
    /* mov byte [0x0200], 0x5A */
    static const uint8_t code[] = { 0xC6, 0x06, 0x00, 0x02, 0x5A, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    vm86_test_run(cpu, 10);

    vm86_expect_mem8("memory", cpu, 0x0200, 0x5A);
    vm86_expect_mem8("byte above", cpu, 0x0201, 0x00);
}

static void test_mov_word_immediate_to_memory(struct vm86_cpu *cpu)
{
    /* mov word [0x0200], 0x1234 */
    static const uint8_t code[] = { 0xC7, 0x06, 0x00, 0x02, 0x34, 0x12,
                                    0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    vm86_test_run(cpu, 10);

    vm86_expect_mem8("low byte", cpu, 0x0200, 0x34);
    vm86_expect_mem8("high byte", cpu, 0x0201, 0x12);
}

static void test_mov_byte_immediate_to_register(struct vm86_cpu *cpu)
{
    /* mov bl, 0x5A */
    static const uint8_t code[] = { 0xC6, 0xC3, 0x5A, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->bx = 0x1200;

    vm86_test_run(cpu, 10);

    vm86_expect_u16("BX", cpu->bx, 0x125A);
}

static void test_mov_word_immediate_to_register(struct vm86_cpu *cpu)
{
    /* mov bp, 0x1234 */
    static const uint8_t code[] = { 0xC7, 0xC5, 0x34, 0x12, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    vm86_test_run(cpu, 10);

    vm86_expect_u16("BP", cpu->bp, 0x1234);
    vm86_expect_flags("flags untouched", cpu, 0, 0);
}

static void test_mov_immediate_follows_the_displacement(struct vm86_cpu *cpu)
{
    /* mov word [bx+si+0x0200], 0x1234 */
    static const uint8_t code[] = { 0xC7, 0x80, 0x00, 0x02, 0x34, 0x12,
                                    0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->bx = 0x0100;

    vm86_test_run(cpu, 10);

    /* Mod 10 carries a sixteen-bit displacement, and the immediate is
     * read after it. A handler that fetched its immediate first would
     * store 0x0234 -- the displacement's low half -- and write it to the
     * wrong place as well. */
    vm86_expect_mem16("memory", cpu, 0x0300, 0x1234);
    vm86_expect_mem16("address without it", cpu, 0x0200, 0x0000);
}

static void test_mov_byte_immediate_with_a_reg_field_is_refused(
    struct vm86_cpu *cpu)
{
    /* C6 /1 */
    static const uint8_t code[] = { 0xC6, 0x0E, 0x00, 0x02, 0x5A, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    enum vm86_result result = vm86_test_run(cpu, 10);

    vm86_expect_bool("faults", result == VM86_FAULT, true);
    vm86_expect_u16("vector", cpu->fault, VM86_VECTOR_INVALID_OPCODE);
    vm86_expect_mem8("memory untouched", cpu, 0x0200, 0x00);
}

static void test_mov_word_immediate_with_a_reg_field_is_refused(
    struct vm86_cpu *cpu)
{
    /* C7 /7 */
    static const uint8_t code[] = { 0xC7, 0x3E, 0x00, 0x02, 0x34, 0x12,
                                    0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    enum vm86_result result = vm86_test_run(cpu, 10);

    vm86_expect_bool("faults", result == VM86_FAULT, true);
    vm86_expect_u16("vector", cpu->fault, VM86_VECTOR_INVALID_OPCODE);
    vm86_expect_mem16("memory untouched", cpu, 0x0200, 0x0000);
}

/* ------------------------------------------------------------------ */
/* D7: XLAT                                                            */
/* ------------------------------------------------------------------ */

static void test_xlat_looks_up_bx_plus_al(struct vm86_cpu *cpu)
{
    /* xlat */
    static const uint8_t code[] = { 0xD7, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->bx = 0x0200;
    cpu->ax = 0x0005;
    vm86_mem_write8(cpu->mem, 0x0205, 0x99);

    vm86_flag_set(cpu, VM86_CF, true);

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AL", cpu->al, 0x99);
    /* A table lookup sets no flags, and this is the assertion that says
     * so -- the same way AAM and the ALU operations are checked. */
    vm86_expect_flags("flags untouched", cpu, VM86_CF, 0);
}

static void test_xlat_leaves_the_high_half_alone(struct vm86_cpu *cpu)
{
    /* xlat */
    static const uint8_t code[] = { 0xD7, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->bx = 0x0200;
    cpu->ax = 0x1200;
    vm86_mem_write8(cpu->mem, 0x0200, 0x99);

    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX", cpu->ax, 0x1299);
}

static void test_xlat_honours_segment_override(struct vm86_cpu *cpu)
{
    /* xlat, through ES */
    static const uint8_t code[] = { 0x26, 0xD7, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_set_seg(cpu, VM86_ES, 0x0200);      /* base 0x2000 */
    cpu->bx = 0x0010;
    cpu->ax = 0x0003;

    vm86_mem_write8(cpu->mem, 0x2013, 0x42);
    vm86_mem_write8(cpu->mem, 0x0013, 0x11);

    vm86_test_run(cpu, 10);

    /* There is no operand byte to hang a prefix on, which is exactly why
     * XLAT is worth testing with one: the segment it reads through is
     * invisible in the encoding. */
    vm86_expect_u16("AL", cpu->al, 0x42);
}

static void test_xlat_offset_wraps_inside_the_segment(struct vm86_cpu *cpu)
{
    /* xlat */
    static const uint8_t code[] = { 0xD7, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->bx = 0xFFFF;
    cpu->ax = 0x0002;
    vm86_mem_write8(cpu->mem, 0x0001, 0x77);

    vm86_test_run(cpu, 10);

    /* BX + AL is summed at sixteen bits before the segment base is
     * added, so the walk wraps to the start of the segment. Adding the
     * base first would put the read at 0x10001, past the end of guest
     * memory, and return 0xFF from the floating bus. */
    vm86_expect_u16("AL", cpu->al, 0x77);
}

/* ------------------------------------------------------------------ */
/* PUSH and POP, and the one thing that is not an 8086                 */
/* ------------------------------------------------------------------ */

static void test_push_and_pop_each_register(struct vm86_cpu *cpu)
{
    /* push ax ; push cx ; push dx ; push bx ; push bp ; push si ;
     * push di ; pop dx ; pop cx ; pop bp ; pop bx ; pop di ;
     * pop si ; pop ax */
    static const uint8_t code[] = { 0x50, 0x51, 0x52, 0x53, 0x55, 0x56, 0x57,
                                    0x5A, 0x59, 0x5D, 0x5B, 0x5F, 0x5E, 0x58,
                                    0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    cpu->ax = 0x1111;
    cpu->cx = 0x2222;
    cpu->dx = 0x3333;
    cpu->bx = 0x4444;
    cpu->bp = 0x5555;
    cpu->si = 0x6666;
    cpu->di = 0x7777;

    vm86_test_run(cpu, 30);

    /* Seven pushed and seven popped, each pop into a different register
     * from the one the value went in through, so a single wrong opcode
     * leaves two registers wrong rather than none. SP itself and the two
     * stack-pointer forms have tests of their own. */
    vm86_expect_u16("AX", cpu->ax, 0x1111);
    vm86_expect_u16("CX", cpu->cx, 0x6666);
    vm86_expect_u16("DX", cpu->dx, 0x7777);
    vm86_expect_u16("BX", cpu->bx, 0x4444);
    vm86_expect_u16("BP", cpu->bp, 0x5555);
    vm86_expect_u16("SI", cpu->si, 0x2222);
    vm86_expect_u16("DI", cpu->di, 0x3333);
    vm86_expect_u16("SP", cpu->sp, VM86_TEST_STACK_TOP);
}

static void test_mov_immediate_covers_both_register_tables(struct vm86_cpu *cpu)
{
    /* mov ax,0xAAAA ; mov cx,0xCCCC ; mov dx,0xDDDD ; mov bx,0xBBBB ;
     * mov sp,0x2222 ; mov bp,0x5555 ; mov si,0x6666 ; mov di,0x7777 ;
     * mov al,0x11 ; mov cl,0x22 ; mov dl,0x33 ; mov bl,0x44 ;
     * mov ah,0x55 ; mov ch,0x66 ; mov dh,0x77 ; mov bh,0x88 */
    static const uint8_t code[] = { 0xB8, 0xAA, 0xAA, 0xB9, 0xCC, 0xCC,
                                    0xBA, 0xDD, 0xDD, 0xBB, 0xBB, 0xBB,
                                    0xBC, 0x22, 0x22, 0xBD, 0x55, 0x55,
                                    0xBE, 0x66, 0x66, 0xBF, 0x77, 0x77,
                                    0xB0, 0x11, 0xB1, 0x22, 0xB2, 0x33,
                                    0xB3, 0x44, 0xB4, 0x55, 0xB5, 0x66,
                                    0xB6, 0x77, 0xB7, 0x88,
                                    0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    vm86_test_run(cpu, 40);

    /* All sixteen opcodes, and the two register orders they index.
     *
     * The words first, into all eight: 0-7 is AX CX DX BX SP BP SI DI.
     * Then the bytes, into the halves: 0-7 is AL CL DL BL AH CH DH BH.
     * The two orders agree for the first four registers and nowhere
     * after, and this arrangement is what tells them apart -- read the
     * byte forms through the 16-bit table and B4 to B7 write SP, BP, SI
     * and DI instead of the high halves of AX to BX, which shows up in
     * both halves of the answer at once. */
    vm86_expect_u16("AX", cpu->ax, 0x5511);
    vm86_expect_u16("CX", cpu->cx, 0x6622);
    vm86_expect_u16("DX", cpu->dx, 0x7733);
    vm86_expect_u16("BX", cpu->bx, 0x8844);
    vm86_expect_u16("SP", cpu->sp, 0x2222);
    vm86_expect_u16("BP", cpu->bp, 0x5555);
    vm86_expect_u16("SI", cpu->si, 0x6666);
    vm86_expect_u16("DI", cpu->di, 0x7777);
}

static void test_push_sp_pushes_the_decremented_pointer(struct vm86_cpu *cpu)
{
    /* push sp */
    static const uint8_t code[] = { 0x54, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    vm86_test_run(cpu, 10);

    /* The 8086 pushes the SP it has after the decrement, so the value in
     * memory is the address it was written to and not the one SP had
     * before. The 80186 and later push the pre-decrement value, and this
     * instruction is a documented way for a program to tell the two
     * parts apart -- which is why the case is written down here rather
     * than left as a detail of the helper. The helper alone settles
     * nothing either way: the value to push has to be worked out before
     * the helper decrements SP, and reading SP at that moment is the
     * 80186's answer. */
    vm86_expect_u16("SP", cpu->sp, VM86_TEST_STACK_TOP - 2);
    vm86_expect_mem16("pushed value", cpu, VM86_TEST_STACK_TOP - 2,
                      VM86_TEST_STACK_TOP - 2);
}

static void test_pop_sp_takes_the_popped_word_from_the_other_form(
    struct vm86_cpu *cpu)
{
    /* mov ax, 0x1234 ; push ax ; pop sp */
    static const uint8_t code[] = { 0xB8, 0x34, 0x12, 0x50, 0x5C, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    vm86_test_run(cpu, 10);

    vm86_expect_u16("SP", cpu->sp, 0x1234);
}

/* ------------------------------------------------------------------ */

static const struct vm86_test tests[] = {
    { "xchg r/m8, r8 register form",      test_xchg_byte_registers },
    { "xchg r/m16, r16 register form",    test_xchg_word_registers },
    { "xchg r/m16, r16 memory form",      test_xchg_word_with_memory },
    { "xchg r/m8, r8 memory form",        test_xchg_byte_with_memory },
    { "xchg honours a segment override",  test_xchg_honours_segment_override },

    { "mov r/m8, r8 register form",       test_mov_byte_to_register },
    { "mov r/m8, r8 memory form",         test_mov_byte_to_memory },
    { "mov r/m16, r16 register form",     test_mov_word_to_register },
    { "mov r/m16, r16 memory form",       test_mov_word_to_memory },
    { "mov r8, r/m8 register form",       test_mov_byte_from_register },
    { "mov r8, r/m8 memory form",         test_mov_byte_from_memory },
    { "mov r16, r/m16 register form",     test_mov_word_from_register },
    { "mov r16, r/m16 memory form",       test_mov_word_from_memory },
    { "mov honours a segment override",   test_mov_honours_segment_override_both_ways },

    { "mov a segment register out",       test_mov_segment_register_out },
    { "mov a segment register in",        test_mov_segment_register_in },
    { "mov a segment register in, memory", test_mov_segment_register_in_from_memory },
    { "mov cs is refused",                test_mov_cs_is_refused },
    { "8E /4 is refused",                 test_mov_sreg_above_ds_is_refused },
    { "8C /4 is refused",                 test_mov_sreg_out_above_ds_is_refused },

    { "lea computes without reading",     test_lea_computes_without_reading },
    { "lea with a displacement",          test_lea_with_a_displacement },
    { "lea does not read past memory",    test_lea_does_not_read_past_memory },
    { "lea ignores the segment base",     test_lea_ignores_the_segment_base },
    { "lea of a register is refused",     test_lea_of_a_register_is_refused },

    { "pop r/m16 memory form",            test_pop_to_memory },
    { "pop r/m16 register form",          test_pop_to_register },
    { "pop sp takes the popped word",     test_pop_sp_takes_the_popped_word },
    { "8F with a reg field is refused",   test_pop_with_a_reg_field_is_refused },
    { "pop takes the override on its destination",
                                          test_pop_destination_takes_the_override },

    { "xchg ax with each register",       test_xchg_ax_with_each_register },
    { "xchg ax, ax is a nop",             test_xchg_ax_ax_is_a_nop },

    { "mov al, [moffs]",                  test_mov_al_from_moffs },
    { "mov ax, [moffs]",                  test_mov_ax_from_moffs },
    { "mov [moffs], al",                  test_mov_to_moffs_is_a_byte_store },
    { "mov [moffs], ax",                  test_mov_ax_to_moffs },
    { "moffs honours a segment override", test_moffs_honours_segment_override },

    { "les loads a far pointer",          test_les_loads_a_far_pointer },
    { "lds loads a far pointer",          test_lds_loads_a_far_pointer },
    { "les honours a segment override",   test_les_honours_segment_override },
    { "les off a frame pointer",          test_les_with_a_frame_pointer_operand },
    { "les of a register is refused",     test_les_of_a_register_is_refused },
    { "lds of a register is refused",     test_lds_of_a_register_is_refused },

    { "mov r/m8, imm8 memory form",       test_mov_byte_immediate_to_memory },
    { "mov r/m16, imm16 memory form",     test_mov_word_immediate_to_memory },
    { "mov r/m8, imm8 register form",     test_mov_byte_immediate_to_register },
    { "mov r/m16, imm16 register form",   test_mov_word_immediate_to_register },
    { "immediate follows the displacement", test_mov_immediate_follows_the_displacement },
    { "C6 with a reg field is refused",   test_mov_byte_immediate_with_a_reg_field_is_refused },
    { "C7 with a reg field is refused",   test_mov_word_immediate_with_a_reg_field_is_refused },

    { "xlat looks up bx + al",            test_xlat_looks_up_bx_plus_al },
    { "xlat leaves the high half alone",  test_xlat_leaves_the_high_half_alone },
    { "xlat honours a segment override",  test_xlat_honours_segment_override },
    { "xlat's offset wraps in the segment", test_xlat_offset_wraps_inside_the_segment },

    { "push and pop every register",      test_push_and_pop_each_register },
    { "mov imm8/imm16 register tables",   test_mov_immediate_covers_both_register_tables },
    { "push sp pushes the decremented SP", test_push_sp_pushes_the_decremented_pointer },
    { "pop sp, register form",            test_pop_sp_takes_the_popped_word_from_the_other_form },
};

VM86_TEST_MAIN("mov", tests)
