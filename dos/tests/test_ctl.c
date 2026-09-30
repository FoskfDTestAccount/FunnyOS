/*
 * Control flow, interrupts, flags and I/O.
 *
 * ---------------------------------------------------------------------
 * What these cases are for
 *
 * The displacement of a jump is measured from the end of the instruction,
 * and a sign error there is invisible until something lands three bytes
 * off. So there are jumps in both directions, and the tests assert the
 * resulting IP rather than only that the program halted somewhere.
 *
 * The conditional jumps are checked as a table against a second,
 * independently written reading of the manual's condition list: every one
 * of the sixteen is run with flag states that make it taken and with flag
 * states that make it not taken, and the expected answer comes from the
 * flags that were set rather than from the same table the implementation
 * uses.
 *
 * The interrupt tests look at the stack the interrupt left behind, not
 * just at where control went. The pushed frame is the only place the
 * push order is visible, and getting that order wrong is what makes an
 * IRET jump to an address assembled out of flag bits.
 */
#include "harness.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Relative jumps                                                     */
/* ------------------------------------------------------------------ */

static void test_short_jump_forward(struct vm86_cpu *cpu)
{
    /* 0x100  jmp short 0x105    EB 03
     * 0x102  mov ax,0FFFFh      B8 FF FF   <- the jump skips this
     * 0x105  hlt                F4
     */
    static const uint8_t code[] = { 0xEB, 0x03, 0xB8, 0xFF, 0xFF, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AX untouched by the skipped mov", cpu->ax, 0x0000);
    vm86_expect_u16("IP", cpu->ip, 0x0106);
}

static void test_near_jump_forward(struct vm86_cpu *cpu)
{
    /* 0x100  jmp 0x105      E9 02 00
     * 0x103  inc ax         <- 2 bytes, skipped
     * 0x105  hlt            F4
     */
    static const uint8_t code[] = { 0xE9, 0x02, 0x00, 0x40, 0x40, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AX", cpu->ax, 0x0000);
    vm86_expect_u16("IP", cpu->ip, 0x0106);
}

static void test_near_jump_backward(struct vm86_cpu *cpu)
{
    /* 0x100  mov ax,0        B8 00 00
     * 0x103  mov bx,3        BB 03 00
     * 0x106  inc ax          40
     * 0x107  dec bx          4B
     * 0x108  jnz 0x106       75 FC
     * 0x10A  hlt             F4
     *
     * The displacement is negative, which is the whole point: a jump back
     * to a lower address is the only way to write a loop, and an
     * emulator that treats the byte as unsigned lands at 0x108 + 252.
     */
    static const uint8_t code[] = {
        0xB8, 0x00, 0x00,
        0xBB, 0x03, 0x00,
        0x40,
        0x4B,
        0x75, 0xFC,
        0xF4,
    };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_test_run(cpu, 50);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AX counts the iterations", cpu->ax, 0x0003);
    vm86_expect_u16("BX counts down to zero", cpu->bx, 0x0000);
    vm86_expect_u16("IP", cpu->ip, 0x010B);
}

/* ------------------------------------------------------------------ */
/* The sixteen conditional jumps                                       */
/* ------------------------------------------------------------------ */

static bool any_flag(uint16_t flags, uint16_t flag)
{
    return (flags & flag) != 0;
}

/*
 * Whether a condition holds, read straight off the manual's table for
 * opcodes 70-7F. This is deliberately not the shape the implementation
 * uses: a table of predicates written the same way twice would agree with
 * itself even when both are wrong, and the point of the test is to be a
 * second opinion.
 */
static bool condition_holds(uint8_t condition, uint16_t flags)
{
    bool cf = any_flag(flags, VM86_CF);
    bool pf = any_flag(flags, VM86_PF);
    bool zf = any_flag(flags, VM86_ZF);
    bool sf = any_flag(flags, VM86_SF);
    bool of = any_flag(flags, VM86_OF);

    switch (condition) {
    case 0x0: return of;                     /* O    overflow            */
    case 0x1: return !of;                    /* NO   not overflow        */
    case 0x2: return cf;                     /* B    below, C, NAE       */
    case 0x3: return !cf;                    /* AE   above or equal      */
    case 0x4: return zf;                     /* E    equal, Z            */
    case 0x5: return !zf;                    /* NE   not equal, NZ       */
    case 0x6: return cf || zf;               /* BE   below or equal      */
    case 0x7: return !cf && !zf;             /* A    above               */
    case 0x8: return sf;                     /* S    sign                */
    case 0x9: return !sf;                    /* NS   not sign            */
    case 0xA: return pf;                     /* P    parity even         */
    case 0xB: return !pf;                    /* NP   parity odd          */
    case 0xC: return sf != of;               /* L    less                */
    case 0xD: return sf == of;               /* GE   greater or equal    */
    case 0xE: return zf || (sf != of);       /* LE   less or equal       */
    case 0xF: return !zf && (sf == of);      /* G    greater             */
    default:  return false;
    }
}

static void test_conditional_jumps(struct vm86_cpu *cpu)
{
    static const char *const names[16] = {
        "jo", "jno", "jb/jc/jnae", "jae/jnb/jnc",
        "je/jz", "jne/jnz", "jbe/jna", "ja/jnbe",
        "js", "jns", "jp/jpe", "jnp/jpo",
        "jl/jnge", "jge/jnl", "jle/jng", "jg/jnle",
    };

    /*
     * Flag states chosen so that every one of the sixteen conditions is
     * exercised both ways: once with the flags that make it take the
     * branch and once with flags that do not. The single-flag states
     * catch a condition reading the wrong bit; the combination states
     * catch the ones that combine two flags with the wrong operator.
     */
    static const uint16_t states[] = {
        0x0000,
        VM86_CF,
        VM86_ZF,
        VM86_SF,
        VM86_OF,
        VM86_PF,
        VM86_CF | VM86_ZF,
        VM86_SF | VM86_OF,
        VM86_ZF | VM86_SF,
        VM86_CF | VM86_SF | VM86_ZF,
    };

    for (uint8_t condition = 0; condition < 16; condition++) {
        for (size_t s = 0; s < sizeof(states) / sizeof(states[0]); s++) {
            /* 0x100  jCC 0x104     7x 02
             * 0x102  mov al,1      B0 01   <- runs only if not taken
             * 0x104  hlt           F4
             */
            uint8_t code[] = { (uint8_t)(0x70 + condition), 0x02,
                               0xB0, 0x01, 0xF4 };

            vm86_test_load(cpu, code, sizeof(code));
            cpu->flags  = (uint16_t)(VM86_FLAG_ALWAYS_SET | states[s]);
            cpu->ax     = 0x0000;
            cpu->halted = false;

            vm86_test_run(cpu, 10);

            char what[64];
            snprintf(what, sizeof(what), "%s with FLAGS 0x%04X",
                     names[condition],
                     (unsigned)(VM86_FLAG_ALWAYS_SET | states[s]));

            bool taken = condition_holds(condition, states[s]);

            /* AL is 1 exactly when the branch was not taken, so a
             * condition that reads the wrong flag shows up as a wrong
             * value rather than as a wrong destination. */
            vm86_expect_u16(what, cpu->al, taken ? 0x00 : 0x01);
            vm86_expect_bool("halted", cpu->halted, true);
        }
    }
}

/* ------------------------------------------------------------------ */
/* CALL and RET                                                       */
/* ------------------------------------------------------------------ */

static void test_near_call_and_ret(struct vm86_cpu *cpu)
{
    /* 0x100  call 0x108     E8 05 00
     * 0x103  hlt            F4
     * 0x104  mov ax,0FFFFh  B8 FF FF   <- runs only if RET comes back to
     * 0x107  nop            90            the wrong place
     * 0x108  inc ax         40
     * 0x109  ret            C3
     */
    static const uint8_t code[] = {
        0xE8, 0x05, 0x00,
        0xF4,
        0xB8, 0xFF, 0xFF,
        0x90,
        0x40,
        0xC3,
    };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    /* The return address is the instruction after the CALL: an emulator
     * that adds the displacement to the wrong IP comes back three bytes
     * out and executes the trap above. */
    vm86_expect_u16("AX", cpu->ax, 0x0001);
    vm86_expect_u16("IP", cpu->ip, 0x0104);
    vm86_expect_u16("SP restored", cpu->sp, VM86_TEST_STACK_TOP);
}

static void test_ret_imm16_adjusts_the_stack(struct vm86_cpu *cpu)
{
    /* 0x100  mov ax,1234h    B8 34 12
     * 0x103  push ax         50
     * 0x104  call 0x10C      E8 05 00
     * 0x107  hlt             F4
     * 0x108  mov ax,0FFFFh   B8 FF FF   <- trap, as above
     * 0x10B  nop             90
     * 0x10C  ret 2           C2 02 00
     *
     * A stdcall callee drops its arguments with `ret n`. SP has to end up
     * back at the top, with the pushed argument left behind, discarded --
     * if the adjustment is missing SP is two bytes short and the caller's
     * stack creeps by two on every call.
     */
    static const uint8_t code[] = {
        0xB8, 0x34, 0x12,
        0x50,
        0xE8, 0x05, 0x00,
        0xF4,
        0xB8, 0xFF, 0xFF,
        0x90,
        0xC2, 0x02, 0x00,
    };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AX", cpu->ax, 0x1234);
    vm86_expect_u16("IP", cpu->ip, 0x0108);
    vm86_expect_u16("SP includes the adjustment", cpu->sp,
                    VM86_TEST_STACK_TOP);
    vm86_expect_mem16("the discarded argument", cpu,
                      VM86_TEST_STACK_TOP - 2, 0x1234);
}

static void test_far_call_and_retf(struct vm86_cpu *cpu)
{
    /* 0x100  call 1000h:0100h   9A 00 01 00 10   (offset first)
     * 0x105  hlt                F4
     * linear 10100h: retf       CB
     *
     * The far pointer is written offset first and segment second, which
     * is the reverse of how it is spelled in assembly. Reading the pair
     * the other way round sends control to 0100h:1000h, which is linear
     * 0x2000 and full of zeroes, so the program never halts.
     */
    static const uint8_t code[] = { 0x9A, 0x00, 0x01, 0x00, 0x10, 0xF4 };
    static const uint8_t subroutine[] = { 0xCB };

    vm86_test_load(cpu, code, sizeof(code));
    memcpy(cpu->mem->ram + 0x10100, subroutine, sizeof(subroutine));

    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("CS restored", cpu->cs, 0x0000);
    vm86_expect_u16("IP", cpu->ip, 0x0106);
    vm86_expect_u16("SP", cpu->sp, VM86_TEST_STACK_TOP);

    /*
     * The frame RETF popped, still in memory. CS was pushed first, so it
     * is the word at the higher address; the return offset was pushed
     * second and is the word SP pointed at while the far RET was running.
     */
    vm86_expect_mem16("saved CS", cpu, VM86_TEST_STACK_TOP - 2, 0x0000);
    vm86_expect_mem16("saved IP", cpu, VM86_TEST_STACK_TOP - 4, 0x0105);
}

static void test_retf_imm16_adjusts_the_stack(struct vm86_cpu *cpu)
{
    /* 0x100  mov ax,1234h        B8 34 12
     * 0x103  push ax             50
     * 0x104  call 1000h:0100h    9A 00 01 00 10
     * 0x109  hlt                 F4
     * linear 10100h: retf 2      CA 02 00
     */
    static const uint8_t code[] = {
        0xB8, 0x34, 0x12,
        0x50,
        0x9A, 0x00, 0x01, 0x00, 0x10,
        0xF4,
    };
    static const uint8_t subroutine[] = { 0xCA, 0x02, 0x00 };

    vm86_test_load(cpu, code, sizeof(code));
    memcpy(cpu->mem->ram + 0x10100, subroutine, sizeof(subroutine));

    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("CS", cpu->cs, 0x0000);
    vm86_expect_u16("IP", cpu->ip, 0x010A);
    vm86_expect_u16("SP includes the adjustment", cpu->sp,
                    VM86_TEST_STACK_TOP);
    vm86_expect_mem16("the discarded argument", cpu,
                      VM86_TEST_STACK_TOP - 2, 0x1234);
}

static void test_far_jump_loads_both_halves(struct vm86_cpu *cpu)
{
    /* 0x100  jmp 1000h:0100h   EA 00 01 00 10
     * 0x105  mov ax,0FFFFh     B8 FF FF   <- never reached
     * 0x108  hlt               F4
     * linear 10100h: hlt       F4
     */
    static const uint8_t code[] = {
        0xEA, 0x00, 0x01, 0x00, 0x10,
        0xB8, 0xFF, 0xFF,
        0xF4,
    };
    static const uint8_t target[] = { 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    memcpy(cpu->mem->ram + 0x10100, target, sizeof(target));

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("CS", cpu->cs, 0x1000);
    vm86_expect_u16("IP", cpu->ip, 0x0101);
    vm86_expect_u16("AX", cpu->ax, 0x0000);
    /* A jump saves nothing: unlike CALL it must leave the stack alone. */
    vm86_expect_u16("SP", cpu->sp, VM86_TEST_STACK_TOP);
}

/* ------------------------------------------------------------------ */
/* Interrupts                                                          */
/* ------------------------------------------------------------------ */

static void test_int3_pushes_flags_cs_ip(struct vm86_cpu *cpu)
{
    /* 0x100  int 3      CC          <- one byte, no vector immediate
     * 0x101  hlt        F4
     * 0x110  iret       CF
     */
    static const uint8_t code[] = { 0xCC, 0xF4 };
    static const uint8_t handler[] = { 0xCF };

    const uint16_t saved_flags = (uint16_t)(VM86_FLAG_ALWAYS_SET | VM86_CF |
                                            VM86_ZF | VM86_IF | VM86_DF);

    vm86_test_load(cpu, code, sizeof(code));
    memcpy(cpu->mem->ram + 0x110, handler, sizeof(handler));
    vm86_mem_write16(cpu->mem, 3 * 4, 0x0110);      /* IP   */
    vm86_mem_write16(cpu->mem, 3 * 4 + 2, 0x0000);  /* CS   */

    cpu->flags = saved_flags;
    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("IP after IRET", cpu->ip, 0x0102);
    vm86_expect_u16("SP", cpu->sp, VM86_TEST_STACK_TOP);
    vm86_expect_u16("FLAGS restored", cpu->flags, saved_flags);

    /*
     * The frame, read out of memory where the pops left it. The stack
     * grows down, so the word pushed first is at the highest address and
     * the one pushed last is where SP ended up: IP at the bottom of the
     * three, CS above it, FLAGS above that. That is the reverse of the
     * order IRET pops them, which is the point -- an interrupt that
     * pushed them the other way round still returns here, and the next
     * IRET, of a frame it did not push, jumps to an address made of flag
     * bits.
     */
    vm86_expect_mem16("saved IP", cpu, VM86_TEST_STACK_TOP - 6, 0x0101);
    vm86_expect_mem16("saved CS", cpu, VM86_TEST_STACK_TOP - 4, 0x0000);
    vm86_expect_mem16("saved FLAGS", cpu, VM86_TEST_STACK_TOP - 2,
                      saved_flags);
}

static void test_int_clears_the_interrupt_and_trap_flags(struct vm86_cpu *cpu)
{
    /*
     * 0x100  int 3    CC
     * 0x110  hlt      F4        <- the handler never returns, so the flags
     *                              it was entered with can be inspected
     *
     * Taking an interrupt clears IF and TF, and the task book does not say
     * so -- it only fixes the order of the stack frame. Without the clear,
     * an interrupt handler that does not enable interrupts itself would
     * run with them enabled, which is the opposite of what the hardware
     * does and the difference is invisible until interrupts are actually
     * delivered. The caller's values are what went on the stack, so an
     * IRET still restores them exactly.
     */
    static const uint8_t code[] = { 0xCC, 0xF4 };
    static const uint8_t handler[] = { 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    memcpy(cpu->mem->ram + 0x110, handler, sizeof(handler));
    vm86_mem_write16(cpu->mem, 3 * 4, 0x0110);
    vm86_mem_write16(cpu->mem, 3 * 4 + 2, 0x0000);

    cpu->flags = (uint16_t)(VM86_FLAG_ALWAYS_SET | VM86_IF | VM86_TF |
                            VM86_CF);
    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted in the handler", cpu->halted, true);
    vm86_expect_u16("IP", cpu->ip, 0x0111);
    vm86_expect_u16("FLAGS on entry to the handler", cpu->flags,
                    (uint16_t)(VM86_FLAG_ALWAYS_SET | VM86_CF));
    /* The copy that went on the stack is the caller's, IF and TF
     * included. */
    vm86_expect_mem16("saved FLAGS", cpu, VM86_TEST_STACK_TOP - 2,
                      (uint16_t)(VM86_FLAG_ALWAYS_SET | VM86_IF |
                                 VM86_TF | VM86_CF));
}

static void test_int_imm8_reads_the_ivt_linearly(struct vm86_cpu *cpu)
{
    /*
     * The program runs in segment 1000h, so CS:0100 is linear 0x10100
     * and the same bytes are placed at both. The vector table is not part
     * of any program's segment: vector 21h must be read at linear 0x84 no
     * matter what CS holds. An implementation that based the lookup on CS
     * reads the copy of this program instead, and lands in it.
     */
    static const uint8_t code[] = { 0xCD, 0x21, 0xF4 };   /* int 21h; hlt */
    static const uint8_t handler[] = { 0xCF };

    vm86_test_load(cpu, code, sizeof(code));
    memcpy(cpu->mem->ram + 0x10100, code, sizeof(code));
    memcpy(cpu->mem->ram + 0x110, handler, sizeof(handler));

    vm86_set_seg(cpu, VM86_CS, 0x1000);

    vm86_mem_write16(cpu->mem, 0x21 * 4, 0x0110);
    vm86_mem_write16(cpu->mem, 0x21 * 4 + 2, 0x0000);

    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("CS", cpu->cs, 0x1000);
    vm86_expect_u16("IP", cpu->ip, 0x0103);
    vm86_expect_u16("SP", cpu->sp, VM86_TEST_STACK_TOP);
    /* Two bytes long, so the return address is the HLT: an INT that did
     * not consume its vector byte would come back one byte into it. */
    vm86_expect_mem16("saved IP", cpu, VM86_TEST_STACK_TOP - 6, 0x0102);
    vm86_expect_mem16("saved CS", cpu, VM86_TEST_STACK_TOP - 4, 0x1000);
    vm86_expect_mem16("saved FLAGS", cpu, VM86_TEST_STACK_TOP - 2,
                      VM86_FLAG_ALWAYS_SET);
}

static void test_into_takes_the_overflow_trap(struct vm86_cpu *cpu)
{
    /* 0x100  into    CE
     * 0x101  hlt     F4
     * 0x110  iret    CF
     */
    static const uint8_t code[] = { 0xCE, 0xF4 };
    static const uint8_t handler[] = { 0xCF };

    const uint16_t saved_flags =
        (uint16_t)(VM86_FLAG_ALWAYS_SET | VM86_OF | VM86_SF);

    vm86_test_load(cpu, code, sizeof(code));
    memcpy(cpu->mem->ram + 0x110, handler, sizeof(handler));
    vm86_mem_write16(cpu->mem, 4 * 4, 0x0110);
    vm86_mem_write16(cpu->mem, 4 * 4 + 2, 0x0000);

    cpu->flags = saved_flags;
    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("IP", cpu->ip, 0x0102);
    vm86_expect_u16("SP", cpu->sp, VM86_TEST_STACK_TOP);
    vm86_expect_mem16("saved IP", cpu, VM86_TEST_STACK_TOP - 6, 0x0101);
    vm86_expect_mem16("saved CS", cpu, VM86_TEST_STACK_TOP - 4, 0x0000);
    vm86_expect_mem16("saved FLAGS", cpu, VM86_TEST_STACK_TOP - 2,
                      saved_flags);
}

static void test_into_does_nothing_without_overflow(struct vm86_cpu *cpu)
{
    /* With OF clear the instruction is a complete no-op. Vector 4 is
     * pointed at a trap that would be visible if it fired: reaching it
     * would set AX and leave SP moved. */
    static const uint8_t code[] = { 0xCE, 0xF4 };
    static const uint8_t trap[] = { 0xB8, 0xFF, 0xFF, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    memcpy(cpu->mem->ram + 0x120, trap, sizeof(trap));
    vm86_mem_write16(cpu->mem, 4 * 4, 0x0120);
    vm86_mem_write16(cpu->mem, 4 * 4 + 2, 0x0000);

    cpu->flags = VM86_FLAG_ALWAYS_SET;   /* OF clear */
    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("IP", cpu->ip, 0x0102);
    vm86_expect_u16("SP untouched", cpu->sp, VM86_TEST_STACK_TOP);
    vm86_expect_u16("AX", cpu->ax, 0x0000);
}

/* ------------------------------------------------------------------ */
/* The loop instructions                                              */
/* ------------------------------------------------------------------ */

static void test_loop_counts_down_and_stops_at_zero(struct vm86_cpu *cpu)
{
    /* 0x100  mov cx,3    B9 03 00
     * 0x103  loop 0x103  E2 FE     <- the body is the loop itself
     * 0x105  hlt         F4
     *
     * LOOP ignores the flags. The zero flag is left clear here, so an
     * implementation that used LOOPE's condition would stop after one
     * decrement with CX at 2 instead of running it out.
     */
    static const uint8_t code[] = { 0xB9, 0x03, 0x00, 0xE2, 0xFE, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->flags = VM86_FLAG_ALWAYS_SET;
    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("CX ran out", cpu->cx, 0x0000);
    vm86_expect_u16("IP", cpu->ip, 0x0106);
}

static void test_loope_follows_the_zero_flag(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xB9, 0x03, 0x00, 0xE1, 0xFE, 0xF4 };

    /* Zero set: the loop runs until the counter is exhausted. */
    vm86_test_load(cpu, code, sizeof(code));
    cpu->flags = (uint16_t)(VM86_FLAG_ALWAYS_SET | VM86_ZF);
    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("CX with ZF set", cpu->cx, 0x0000);

    /* Zero clear: one decrement, no jump. */
    vm86_test_load(cpu, code, sizeof(code));
    cpu->flags  = VM86_FLAG_ALWAYS_SET;
    cpu->halted = false;
    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("CX with ZF clear", cpu->cx, 0x0002);
}

static void test_loopne_follows_the_zero_flag(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xB9, 0x03, 0x00, 0xE0, 0xFE, 0xF4 };

    /* Zero clear: loops until the counter is exhausted. */
    vm86_test_load(cpu, code, sizeof(code));
    cpu->flags = VM86_FLAG_ALWAYS_SET;
    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("CX with ZF clear", cpu->cx, 0x0000);

    /* Zero set: one decrement, no jump. */
    vm86_test_load(cpu, code, sizeof(code));
    cpu->flags = (uint16_t)(VM86_FLAG_ALWAYS_SET | VM86_ZF);
    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("CX with ZF set", cpu->cx, 0x0002);
}

static void test_jcxz_tests_the_counter_without_touching_it(
    struct vm86_cpu *cpu)
{
    /* 0x100  jcxz 0x104    E3 02
     * 0x102  inc ax       <- 2 bytes, skipped when CX is zero
     * 0x104  hlt          F4
     */
    static const uint8_t code[] = { 0xE3, 0x02, 0x40, 0x40, 0xF4 };

    /* CX is zero: the branch is taken. */
    vm86_test_load(cpu, code, sizeof(code));
    cpu->cx = 0x0000;
    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AX when taken", cpu->ax, 0x0000);

    /* CX is not zero: no branch, and CX must come out unchanged. JCXZ is
     * the one member of the family that does not decrement. */
    vm86_test_load(cpu, code, sizeof(code));
    cpu->cx     = 0x0001;
    cpu->halted = false;
    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AX when not taken", cpu->ax, 0x0002);
    vm86_expect_u16("CX not decremented", cpu->cx, 0x0001);
}

/* ------------------------------------------------------------------ */
/* The flags register, pushed and split                               */
/* ------------------------------------------------------------------ */

static void test_pushf_pushes_the_register_as_it_reads(struct vm86_cpu *cpu)
{
    /* 0x100  pushf   9C
     * 0x101  pop ax  58
     * 0x102  hlt     F4
     */
    static const uint8_t code[] = { 0x9C, 0x58, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->flags = (uint16_t)(VM86_FLAG_ALWAYS_SET | VM86_CF | VM86_ZF);
    vm86_test_run(cpu, 10);

    /* The hardwired bits come with it. A program reads them to find out
     * what it is running on. */
    vm86_expect_u16("AX", cpu->ax, 0xF043);
    vm86_expect_u16("SP", cpu->sp, VM86_TEST_STACK_TOP);
}

static void test_popf_restores_and_forces_the_hardwired_bits(
    struct vm86_cpu *cpu)
{
    /* 0x100  mov ax,0001h    B8 01 00
     * 0x103  push ax        50
     * 0x104  popf           9D
     * 0x105  hlt            F4
     *
     * The guest asks for carry and nothing else. Bit 1 and bits 12-15
     * read as one on an 8086 whatever is written there, and a program
     * that pops FLAGS and inspects them sees 0xF003.
     */
    static const uint8_t code[] = { 0xB8, 0x01, 0x00, 0x50, 0x9D, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("FLAGS", cpu->flags, 0xF003);
}

static void test_lahf_takes_the_low_byte(struct vm86_cpu *cpu)
{
    /* 0x100  lahf   9F
     * 0x101  hlt    F4
     */
    static const uint8_t code[] = { 0x9F, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->flags = (uint16_t)(VM86_FLAG_ALWAYS_SET | VM86_CF | VM86_ZF |
                            VM86_SF | VM86_DF);
    vm86_test_run(cpu, 10);

    /* SF ZF 0 AF 0 PF 1 CF, top to bottom: 0xC3. DF is above the byte
     * LAHF reaches and must not appear in it. */
    vm86_expect_u16("AH", cpu->ah, 0xC3);
}

static void test_sahf_writes_only_the_low_byte(struct vm86_cpu *cpu)
{
    /* 0x100  mov ah,0C3h   B4 C3
     * 0x102  sahf          9E
     * 0x103  hlt           F4
     */
    static const uint8_t code[] = { 0xB4, 0xC3, 0x9E, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_test_run(cpu, 10);

    vm86_expect_flags("flags from AH",
                      cpu, VM86_CF | VM86_ZF | VM86_SF, 0);

    /* And with the upper half of the register deliberately occupied:
     * SAHF must leave TF, IF and DF where they were. */
    static const uint8_t clear_code[] = { 0xB4, 0x00, 0x9E, 0xF4 };

    vm86_test_load(cpu, clear_code, sizeof(clear_code));
    cpu->flags = (uint16_t)(VM86_FLAG_ALWAYS_SET | VM86_CF | VM86_ZF |
                            VM86_IF | VM86_DF);
    vm86_test_run(cpu, 10);

    /* AH of zero clears the low byte; the high byte survives, and the
     * hardwired bits are still forced even though the guest asked for
     * zero. */
    vm86_expect_u16("FLAGS", cpu->flags,
                    (uint16_t)(VM86_FLAG_ALWAYS_SET | VM86_IF | VM86_DF));
}

/* ------------------------------------------------------------------ */
/* The single-flag instructions                                       */
/* ------------------------------------------------------------------ */

static void test_flag_instructions_touch_only_their_flag(
    struct vm86_cpu *cpu)
{
    static const struct {
        uint8_t     opcode;
        uint16_t    flag;
        bool        value;
        const char *name;
    } cases[] = {
        { 0xF8, VM86_CF, false, "clc" },
        { 0xF9, VM86_CF, true,  "stc" },
        { 0xFA, VM86_IF, false, "cli" },
        { 0xFB, VM86_IF, true,  "sti" },
        { 0xFC, VM86_DF, false, "cld" },
        { 0xFD, VM86_DF, true,  "std" },
    };

    /* Flags that must survive untouched. Without them the test passes for
     * an implementation that clears the whole register. */
    const uint16_t others = (uint16_t)(VM86_ZF | VM86_SF | VM86_OF |
                                       VM86_PF | VM86_AF);

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t code[] = { cases[i].opcode, 0xF4 };

        vm86_test_load(cpu, code, sizeof(code));

        /* Start from the opposite of what the instruction produces, so
         * an instruction that does nothing at all fails. */
        cpu->flags = (uint16_t)(VM86_FLAG_ALWAYS_SET | others);
        if (!cases[i].value)
            cpu->flags = (uint16_t)(cpu->flags | cases[i].flag);

        vm86_test_run(cpu, 10);

        uint16_t expected = (uint16_t)(others);
        if (cases[i].value)
            expected = (uint16_t)(expected | cases[i].flag);

        vm86_expect_flags(cases[i].name, cpu, expected, 0);
    }
}

static void test_cmc_inverts_only_the_carry(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF5, 0xF4 };

    /* Carry set: cleared, everything else stays. */
    vm86_test_load(cpu, code, sizeof(code));
    cpu->flags = (uint16_t)(VM86_FLAG_ALWAYS_SET | VM86_CF |
                            VM86_ZF | VM86_OF);
    vm86_test_run(cpu, 10);

    vm86_expect_flags("carry was set",
                      cpu, VM86_ZF | VM86_OF, 0);

    /* Carry clear: set. */
    vm86_test_load(cpu, code, sizeof(code));
    cpu->flags = (uint16_t)(VM86_FLAG_ALWAYS_SET | VM86_ZF | VM86_OF);
    vm86_test_run(cpu, 10);

    vm86_expect_flags("carry was clear",
                      cpu, VM86_CF | VM86_ZF | VM86_OF, 0);
}

static void test_wait_is_a_no_op(struct vm86_cpu *cpu)
{
    /* 0x100  wait   9B
     * 0x101  hlt    F4
     */
    static const uint8_t code[] = { 0x9B, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->flags = (uint16_t)(VM86_FLAG_ALWAYS_SET | VM86_CF | VM86_ZF);
    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("IP", cpu->ip, 0x0102);
    vm86_expect_u16("FLAGS", cpu->flags,
                    (uint16_t)(VM86_FLAG_ALWAYS_SET | VM86_CF | VM86_ZF));
}

/* ------------------------------------------------------------------ */
/* IN and OUT                                                         */
/* ------------------------------------------------------------------ */

static void test_in_returns_the_floating_bus(struct vm86_cpu *cpu)
{
    /* Nothing answers at any port, so a read returns 0xFF -- the same
     * answer an unmapped memory address gives, and the one a program
     * probing for hardware is written to recognise. */
    static const uint8_t in_al_imm[] = { 0xE4, 0x60, 0xF4 };
    static const uint8_t in_ax_imm[] = { 0xE5, 0x60, 0xF4 };
    static const uint8_t in_al_dx[]  = { 0xEC, 0xF4 };
    static const uint8_t in_ax_dx[]  = { 0xED, 0xF4 };

    vm86_test_load(cpu, in_al_imm, sizeof(in_al_imm));
    cpu->ax = 0x1200;
    vm86_test_run(cpu, 10);

    /* A byte read must not disturb AH: the 8-bit views are halves of one
     * register, and the width is part of the opcode. */
    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AX after in al,60h", cpu->ax, 0x12FF);
    vm86_expect_u16("IP", cpu->ip, 0x0103);

    vm86_test_load(cpu, in_ax_imm, sizeof(in_ax_imm));
    cpu->halted = false;
    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX after in ax,60h", cpu->ax, 0xFFFF);
    vm86_expect_u16("IP", cpu->ip, 0x0103);

    vm86_test_load(cpu, in_al_dx, sizeof(in_al_dx));
    cpu->ax     = 0x1200;
    cpu->dx     = 0x03F8;
    cpu->halted = false;
    vm86_test_run(cpu, 10);

    /* The DX form carries no port byte: if it consumed one it would eat
     * the HLT and run off the end of the program. */
    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AX after in al,dx", cpu->ax, 0x12FF);
    vm86_expect_u16("IP", cpu->ip, 0x0102);

    vm86_test_load(cpu, in_ax_dx, sizeof(in_ax_dx));
    cpu->dx     = 0x03F8;
    cpu->halted = false;
    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX after in ax,dx", cpu->ax, 0xFFFF);
    vm86_expect_u16("IP", cpu->ip, 0x0102);
}

static void test_out_is_dropped_and_consumes_its_operands(
    struct vm86_cpu *cpu)
{
    static const uint8_t out_al_imm[] = { 0xB8, 0x34, 0x12, 0xE6, 0x61, 0xF4 };
    static const uint8_t out_ax_imm[] = { 0xB8, 0x34, 0x12, 0xE7, 0x61, 0xF4 };
    static const uint8_t out_al_dx[]  = { 0xBA, 0xF8, 0x03, 0xEE, 0xF4 };
    static const uint8_t out_ax_dx[]  = { 0xBA, 0xF8, 0x03, 0xEF, 0xF4 };

    vm86_test_load(cpu, out_al_imm, sizeof(out_al_imm));
    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("AX unchanged", cpu->ax, 0x1234);
    vm86_expect_u16("IP", cpu->ip, 0x0106);

    vm86_test_load(cpu, out_ax_imm, sizeof(out_ax_imm));
    vm86_test_run(cpu, 10);

    vm86_expect_u16("AX unchanged", cpu->ax, 0x1234);
    vm86_expect_u16("IP", cpu->ip, 0x0106);

    vm86_test_load(cpu, out_al_dx, sizeof(out_al_dx));
    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("DX unchanged", cpu->dx, 0x03F8);
    vm86_expect_u16("IP", cpu->ip, 0x0105);

    vm86_test_load(cpu, out_ax_dx, sizeof(out_ax_dx));
    vm86_test_run(cpu, 10);

    vm86_expect_u16("DX unchanged", cpu->dx, 0x03F8);
    vm86_expect_u16("IP", cpu->ip, 0x0105);
}

/* ------------------------------------------------------------------ */
/* Group 5 (FF)                                                       */
/* ------------------------------------------------------------------ */

static void test_group5_inc_dec_agrees_with_the_register_form(
    struct vm86_cpu *cpu)
{
    static const uint16_t values[] = {
        0x0000, 0x0001, 0x7FFF, 0x8000, 0xFFFF, 0x1234,
    };

    static const uint8_t inc_memory[]   = { 0xFF, 0x06, 0x30, 0x01, 0xF4 };
    static const uint8_t dec_memory[]   = { 0xFF, 0x0E, 0x30, 0x01, 0xF4 };
    static const uint8_t inc_register[] = { 0x40, 0xF4 };
    static const uint8_t dec_register[] = { 0x48, 0xF4 };

    for (int decrement = 0; decrement < 2; decrement++) {
        const uint8_t *memory_code = decrement ? dec_memory : inc_memory;
        const uint8_t *reg_code    = decrement ? dec_register : inc_register;
        uint16_t memory_size = decrement ? (uint16_t)sizeof(dec_memory)
                                         : (uint16_t)sizeof(inc_memory);
        uint16_t reg_size    = decrement ? (uint16_t)sizeof(dec_register)
                                         : (uint16_t)sizeof(inc_register);
        const char *name     = decrement ? "dec" : "inc";

        for (size_t v = 0; v < sizeof(values) / sizeof(values[0]); v++) {
            uint16_t expected =
                (uint16_t)(decrement ? values[v] - 1 : values[v] + 1);

            /* The memory form, through opcode FF. */
            vm86_test_load(cpu, memory_code, memory_size);
            vm86_mem_write16(cpu->mem, 0x0130, values[v]);

            /* Carry set from earlier work: increment and decrement must
             * leave it exactly as they found it, in both forms. */
            cpu->flags  = (uint16_t)(VM86_FLAG_ALWAYS_SET | VM86_CF);
            cpu->halted = false;
            vm86_test_run(cpu, 10);

            uint16_t memory_result = vm86_mem_read16(cpu->mem, 0x0130);
            uint16_t memory_flags  = cpu->flags;

            /* The register form of the same operation, for comparison. */
            vm86_test_load(cpu, reg_code, reg_size);
            cpu->ax     = values[v];
            cpu->flags  = (uint16_t)(VM86_FLAG_ALWAYS_SET | VM86_CF);
            cpu->halted = false;
            vm86_test_run(cpu, 10);

            char what[64];

            snprintf(what, sizeof(what), "%s of 0x%04X in memory",
                     name, values[v]);
            vm86_expect_u16(what, memory_result, expected);

            snprintf(what, sizeof(what), "%s of 0x%04X in a register",
                     name, values[v]);
            vm86_expect_u16(what, cpu->ax, expected);

            snprintf(what, sizeof(what), "%s of 0x%04X: flags",
                     name, values[v]);
            vm86_expect_u16(what, memory_flags, cpu->flags);

            snprintf(what, sizeof(what), "%s of 0x%04X: carry preserved",
                     name, values[v]);
            vm86_expect_u16(what, (uint16_t)(memory_flags & VM86_CF), VM86_CF);
        }
    }
}

static void test_group5_inc_at_the_sign_boundary(struct vm86_cpu *cpu)
{
    /* 0x100  inc word [0130h]   FF 06 30 01
     * 0x104  hlt                F4
     */
    static const uint8_t code[] = { 0xFF, 0x06, 0x30, 0x01, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_mem_write16(cpu->mem, 0x0130, 0x7FFF);
    vm86_test_run(cpu, 10);

    /* The largest positive becomes the smallest negative: overflow, and
     * no carry, which is what separates INC from ADD. */
    vm86_expect_u16("memory", vm86_mem_read16(cpu->mem, 0x0130), 0x8000);
    vm86_expect_flag("OF", cpu, VM86_OF, true);
    vm86_expect_flag("CF", cpu, VM86_CF, false);
    vm86_expect_flag("SF", cpu, VM86_SF, true);
}

static void test_group5_call_near(struct vm86_cpu *cpu)
{
    /* 0x100  call word [0130h]   FF 16 30 01
     * 0x104  hlt                 F4
     * 0x110  ret                 C3
     * [0130h] = 0110h
     */
    static const uint8_t code[] = { 0xFF, 0x16, 0x30, 0x01, 0xF4 };
    static const uint8_t subroutine[] = { 0xC3 };

    vm86_test_load(cpu, code, sizeof(code));
    memcpy(cpu->mem->ram + 0x110, subroutine, sizeof(subroutine));
    vm86_mem_write16(cpu->mem, 0x0130, 0x0110);

    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("IP", cpu->ip, 0x0105);
    vm86_expect_u16("SP", cpu->sp, VM86_TEST_STACK_TOP);
    vm86_expect_mem16("return address", cpu, VM86_TEST_STACK_TOP - 2,
                      0x0104);
}

static void test_group5_call_near_through_a_register(struct vm86_cpu *cpu)
{
    /* 0x100  call ax    FF D0
     * 0x102  hlt        F4
     * 0x110  ret        C3
     */
    static const uint8_t code[] = { 0xFF, 0xD0, 0xF4 };
    static const uint8_t subroutine[] = { 0xC3 };

    vm86_test_load(cpu, code, sizeof(code));
    memcpy(cpu->mem->ram + 0x110, subroutine, sizeof(subroutine));
    cpu->ax = 0x0110;

    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("IP", cpu->ip, 0x0103);
    vm86_expect_mem16("return address", cpu, VM86_TEST_STACK_TOP - 2,
                      0x0102);
}

static void test_group5_jmp_near(struct vm86_cpu *cpu)
{
    /* 0x100  jmp word [0130h]   FF 26 30 01
     * 0x104  mov ax,0FFFFh      B8 FF FF   <- never reached
     * 0x107  hlt                F4
     * 0x110  hlt                F4
     * [0130h] = 0110h
     */
    static const uint8_t code[] = {
        0xFF, 0x26, 0x30, 0x01,
        0xB8, 0xFF, 0xFF,
        0xF4,
    };
    static const uint8_t target[] = { 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    memcpy(cpu->mem->ram + 0x110, target, sizeof(target));
    vm86_mem_write16(cpu->mem, 0x0130, 0x0110);

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("IP", cpu->ip, 0x0111);
    vm86_expect_u16("AX", cpu->ax, 0x0000);
    vm86_expect_u16("SP", cpu->sp, VM86_TEST_STACK_TOP);
}

static void test_group5_jmp_near_through_a_register(struct vm86_cpu *cpu)
{
    /* 0x100  jmp ax    FF E0 */
    static const uint8_t code[] = {
        0xFF, 0xE0,
        0xB8, 0xFF, 0xFF,
        0xF4,
    };
    static const uint8_t target[] = { 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    memcpy(cpu->mem->ram + 0x110, target, sizeof(target));
    cpu->ax = 0x0110;

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("IP", cpu->ip, 0x0111);
    /* AX still holds the jump target, so the `mov ax,0FFFFh` the jump was
     * meant to step over did not run. */
    vm86_expect_u16("AX", cpu->ax, 0x0110);
}

static void test_group5_call_far(struct vm86_cpu *cpu)
{
    /* 0x100  call dword [0130h]  FF 1E 30 01
     * 0x104  hlt                 F4
     * [0130h] = offset 0100h, [0132h] = segment 1000h
     * linear 10100h: retf        CB
     */
    static const uint8_t code[] = { 0xFF, 0x1E, 0x30, 0x01, 0xF4 };
    static const uint8_t subroutine[] = { 0xCB };

    vm86_test_load(cpu, code, sizeof(code));
    memcpy(cpu->mem->ram + 0x10100, subroutine, sizeof(subroutine));
    vm86_mem_write16(cpu->mem, 0x0130, 0x0100);   /* offset  */
    vm86_mem_write16(cpu->mem, 0x0132, 0x1000);   /* segment */

    vm86_test_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("CS restored", cpu->cs, 0x0000);
    vm86_expect_u16("IP", cpu->ip, 0x0105);
    vm86_expect_u16("SP", cpu->sp, VM86_TEST_STACK_TOP);
    vm86_expect_mem16("saved CS", cpu, VM86_TEST_STACK_TOP - 2, 0x0000);
    vm86_expect_mem16("saved IP", cpu, VM86_TEST_STACK_TOP - 4, 0x0104);
}

static void test_group5_jmp_far(struct vm86_cpu *cpu)
{
    /* 0x100  jmp dword [0130h]   FF 2E 30 01
     * 0x104  mov ax,0FFFFh       B8 FF FF   <- never reached
     * 0x107  hlt                 F4
     * [0130h] = offset 0100h, [0132h] = segment 1000h
     * linear 10100h: hlt         F4
     */
    static const uint8_t code[] = {
        0xFF, 0x2E, 0x30, 0x01,
        0xB8, 0xFF, 0xFF,
        0xF4,
    };
    static const uint8_t target[] = { 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    memcpy(cpu->mem->ram + 0x10100, target, sizeof(target));
    vm86_mem_write16(cpu->mem, 0x0130, 0x0100);
    vm86_mem_write16(cpu->mem, 0x0132, 0x1000);

    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("CS", cpu->cs, 0x1000);
    vm86_expect_u16("IP", cpu->ip, 0x0101);
    vm86_expect_u16("AX", cpu->ax, 0x0000);
}

static void test_group5_push(struct vm86_cpu *cpu)
{
    /* 0x100  push word [0130h]   FF 36 30 01
     * 0x104  hlt                 F4
     * [0130h] = BEEFh
     */
    static const uint8_t memory_code[] = { 0xFF, 0x36, 0x30, 0x01, 0xF4 };

    vm86_test_load(cpu, memory_code, sizeof(memory_code));
    vm86_mem_write16(cpu->mem, 0x0130, 0xBEEF);
    vm86_test_run(cpu, 10);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("SP", cpu->sp, VM86_TEST_STACK_TOP - 2);
    vm86_expect_mem16("the pushed word", cpu, VM86_TEST_STACK_TOP - 2,
                      0xBEEF);

    /* 0x100  push ax   FF F0
     * 0x102  hlt       F4
     */
    static const uint8_t register_code[] = { 0xFF, 0xF0, 0xF4 };

    vm86_test_load(cpu, register_code, sizeof(register_code));
    cpu->ax     = 0x1234;
    cpu->halted = false;
    vm86_test_run(cpu, 10);

    vm86_expect_u16("SP", cpu->sp, VM86_TEST_STACK_TOP - 2);
    vm86_expect_mem16("the pushed word", cpu, VM86_TEST_STACK_TOP - 2,
                      0x1234);
}

static void test_group5_refuses_the_undefined_encodings(struct vm86_cpu *cpu)
{
    static const uint8_t sub_op_7_register[] = { 0xFF, 0xF8 };
    static const uint8_t sub_op_7_memory[]   = { 0xFF, 0x3E, 0x30, 0x01 };
    static const uint8_t far_call_register[] = { 0xFF, 0xD8 };
    static const uint8_t far_jmp_register[]  = { 0xFF, 0xE8 };

    static const uint8_t *const cases[] = {
        sub_op_7_register, sub_op_7_memory,
        far_call_register, far_jmp_register,
    };
    static const uint16_t sizes[] = {
        sizeof(sub_op_7_register), sizeof(sub_op_7_memory),
        sizeof(far_call_register), sizeof(far_jmp_register),
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        vm86_test_load(cpu, cases[i], sizes[i]);
        cpu->halted = false;

        enum vm86_result result = vm86_test_run(cpu, 10);

        vm86_expect_bool("trapped", result == VM86_FAULT, true);
        vm86_expect_u16("vector", cpu->fault,
                        VM86_VECTOR_INVALID_OPCODE);
    }
}

/* ------------------------------------------------------------------ */

static const struct vm86_test tests[] = {
    { "jmp short forward",             test_short_jump_forward },
    { "jmp near forward",              test_near_jump_forward },
    { "jmp near backward",             test_near_jump_backward },
    { "conditional jumps, 70-7F",      test_conditional_jumps },
    { "call near and ret",             test_near_call_and_ret },
    { "ret imm16 adjusts SP",          test_ret_imm16_adjusts_the_stack },
    { "far call and retf",             test_far_call_and_retf },
    { "retf imm16 adjusts SP",         test_retf_imm16_adjusts_the_stack },
    { "far jmp loads CS and IP",       test_far_jump_loads_both_halves },
    { "int 3 pushes FLAGS, CS, IP",    test_int3_pushes_flags_cs_ip },
    { "int clears IF and TF",          test_int_clears_the_interrupt_and_trap_flags },
    { "int imm8 reads the IVT",        test_int_imm8_reads_the_ivt_linearly },
    { "into traps when OF is set",     test_into_takes_the_overflow_trap },
    { "into does nothing otherwise",   test_into_does_nothing_without_overflow },
    { "loop counts down",              test_loop_counts_down_and_stops_at_zero },
    { "loope follows ZF",              test_loope_follows_the_zero_flag },
    { "loopne follows ZF",             test_loopne_follows_the_zero_flag },
    { "jcxz tests CX without it",      test_jcxz_tests_the_counter_without_touching_it },
    { "pushf",                         test_pushf_pushes_the_register_as_it_reads },
    { "popf and the hardwired bits",   test_popf_restores_and_forces_the_hardwired_bits },
    { "lahf",                          test_lahf_takes_the_low_byte },
    { "sahf touches the low byte",     test_sahf_writes_only_the_low_byte },
    { "clc/stc/cli/sti/cld/std",       test_flag_instructions_touch_only_their_flag },
    { "cmc inverts only CF",           test_cmc_inverts_only_the_carry },
    { "wait is a no-op",               test_wait_is_a_no_op },
    { "in returns the floating bus",   test_in_returns_the_floating_bus },
    { "out is dropped",                test_out_is_dropped_and_consumes_its_operands },
    { "ff /0 and /1 match 40-4F",      test_group5_inc_dec_agrees_with_the_register_form },
    { "ff /0 at the sign boundary",    test_group5_inc_at_the_sign_boundary },
    { "ff /2 call near",               test_group5_call_near },
    { "ff /2 call through a register", test_group5_call_near_through_a_register },
    { "ff /4 jmp near",                test_group5_jmp_near },
    { "ff /4 jmp through a register",  test_group5_jmp_near_through_a_register },
    { "ff /3 call far",                test_group5_call_far },
    { "ff /5 jmp far",                 test_group5_jmp_far },
    { "ff /6 push",                    test_group5_push },
    { "ff undefined encodings trap",   test_group5_refuses_the_undefined_encodings },
};

VM86_TEST_MAIN("ctl", tests)
