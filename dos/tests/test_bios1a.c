/*
 * INT 1Ah -- the clock -- and INT 08h, which is what makes it tick.
 *
 * The interesting cases here are the ones that say where a number came
 * from, so the expected values below are written as literals with the
 * arithmetic in a comment next to them rather than computed at run time.
 * A test that computes 18.2065 * 86400 the same way the implementation
 * does agrees with it about being wrong.
 *
 * See docs/dos-refs.md sections 6 and 7 for the constants themselves.
 */
#include "harness.h"

#include <stdio.h>

#include <vm86/firmware.h>
#include <vm86/host.h>

#include "../bios/bios1a.h"

/* ------------------------------------------------------------------ */
/* Plumbing                                                            */
/* ------------------------------------------------------------------ */

static uint32_t bda(uint16_t offset)
{
    return ((uint32_t)VM86_BDA_SEGMENT << 4) + offset;
}

static uint32_t ticks(struct vm86_cpu *cpu)
{
    uint32_t at = bda(VM86_BDA_TICK_COUNT);

    return (uint32_t)vm86_mem_read16(cpu->mem, at)
         | ((uint32_t)vm86_mem_read16(cpu->mem, at + 2) << 16);
}

static uint8_t midnight_flag(struct vm86_cpu *cpu)
{
    return vm86_mem_read8(cpu->mem, bda(VM86_BDA_TICK_ROLLOVER));
}

static void poke(struct vm86_cpu *cpu, uint32_t linear,
                 const uint8_t *bytes, size_t count)
{
    for (size_t i = 0; i < count; i++)
        vm86_mem_write8(cpu->mem, linear + (uint32_t)i, bytes[i]);
}

/*
 * The harness compares sixteen-bit values, and a sum of ticks passes
 * that: an hour is 65543 of them. This reports the number rather than
 * just "not equal", because the whole point of these cases is that a
 * particular number came out.
 */
static void expect_ticks(const char *what, uint32_t got, uint32_t want)
{
    char detail[128];

    snprintf(detail, sizeof(detail), "%s: wanted %u, got %u",
             what, (unsigned)want, (unsigned)got);

    vm86_expect_bool(detail, got == want, true);
}

static void hook_vector(struct vm86_cpu *cpu, uint8_t vector,
                        uint16_t segment, uint16_t offset)
{
    vm86_mem_write16(cpu->mem, (uint32_t)vector * 4u, offset);
    vm86_mem_write16(cpu->mem, (uint32_t)vector * 4u + 2u, segment);
}

/*
 * A few lines of the run loop, for the case that needs one.
 *
 * Same reasoning as the same function in test_bios16.c: the real loop is
 * task A's and has not landed, this is a test fixture and not a second
 * implementation, and each suite being its own program means the two
 * copies cannot drift into each other's way. When vm86_run() exists,
 * both should go.
 */
static enum vm86_stop run(struct vm86_cpu *cpu, uint64_t steps)
{
    for (uint64_t i = 0; i < steps; i++) {
        if (cpu->intr_shadow) {
            cpu->intr_shadow--;
        } else if (vm86_interruptible(cpu)) {
            int vector = vm86_next_pending(cpu);

            vm86_clear_pending(cpu, (uint8_t)vector);
            cpu->halted = false;
            vm86_interrupt(cpu, (uint8_t)vector);
            continue;
        } else if (cpu->halted) {
            return VM86_STOP_HALT;
        }

        enum vm86_result result = vm86_step(cpu);

        if (result == VM86_HALT)
            continue;
        if (result != VM86_CONTINUE)
            return VM86_STOP_FAULT;
    }

    return VM86_STOP_STEPS;
}

/* ------------------------------------------------------------------ */
/* The conversion from the host's clock to the guest's                 */
/* ------------------------------------------------------------------ */

/*
 * The numbers, worked out by hand from 18.2065 ticks a second:
 *
 *     1 000 ms      -> 18      (18.2065 * 1)
 *     60 000 ms     -> 1092    (18.2065 * 60 = 1092.39)
 *     3 600 000 ms  -> 65543   (18.2065 * 3600 = 65543.4)
 *     1 000 000 ms  -> 18206   (18.2065 * 1000 = 18206.5)
 *
 * Every one of them is the floor, and the floor is what the firmware
 * gives: the count is an integer number of ticks and a partial tick has
 * not happened yet.
 */
static void test_one_second_is_eighteen_ticks(struct vm86_cpu *cpu)
{
    struct bios1a_state st;

    (void)cpu;

    bios1a_reset(&st);

    vm86_expect_u16("18.2065 ticks in a second, floored",
                    bios1a_advance(&st, 1000), 18);
}

static void test_the_conversion_is_exact_at_the_tick_boundary(
        struct vm86_cpu *cpu)
{
    struct bios1a_state st;

    (void)cpu;

    /*
     * A tick is 54.925493 ms, which is not a whole number of
     * milliseconds, so the boundary in this unit falls between 54 and
     * 55. The pair pins the rate to better than a millisecond -- one
     * value alone cannot, which is the point of asking for two.
     */
    bios1a_reset(&st);
    vm86_expect_u16("54 ms is not yet a tick",
                    bios1a_advance(&st, 54), 0);

    bios1a_reset(&st);
    vm86_expect_u16("55 ms is",
                    bios1a_advance(&st, 55), 1);
}

static void test_a_minute_and_an_hour(struct vm86_cpu *cpu)
{
    struct bios1a_state st;

    (void)cpu;

    bios1a_reset(&st);
    expect_ticks("a minute", bios1a_advance(&st, 60000), 1092);

    bios1a_reset(&st);
    expect_ticks("an hour -- and note it is just under 65536",
                 bios1a_advance(&st, 3600000), 65543);
}

/*
 * The long run, which is the whole reason the accumulator exists.
 *
 * A thousand seconds is 18206.48 ticks. A conversion that rounded each
 * call would have thrown away 0.48 of a tick by here; this one carries
 * it, so the answer is the floor of the *total* and the error is always
 * less than one tick -- not "small", not "bounded by a fudge factor",
 * but less than one, forever, because nothing is ever discarded.
 */
static void test_the_error_stays_under_one_tick(struct vm86_cpu *cpu)
{
    struct bios1a_state st;

    (void)cpu;

    bios1a_reset(&st);

    uint32_t total = 0;

    for (unsigned i = 0; i < 1000; i++)
        total += bios1a_advance(&st, 1000);

    expect_ticks("18206.48 ticks, floored, after a thousand seconds",
                 total, 18206);

    /* And through a thousand calls of ten milliseconds rather than a
     * hundred of a thousand -- the remainder has to survive calls of any
     * size, not just a convenient one. */
    bios1a_reset(&st);
    total = 0;

    for (unsigned i = 0; i < 1000; i++)
        total += bios1a_advance(&st, 10);

    expect_ticks("ten seconds in ten-millisecond slices", total, 182);
}

static void test_a_zero_millisecond_slice_is_not_a_tick(struct vm86_cpu *cpu)
{
    struct bios1a_state st;

    (void)cpu;

    bios1a_reset(&st);

    vm86_expect_u16("nothing went by", bios1a_advance(&st, 0), 0);

    /* And it has accumulated nothing, so the next tick still needs a
     * whole tick's worth of milliseconds. */
    vm86_expect_u16("and the accumulator did not move",
                    bios1a_advance(&st, 54), 0);
}

/* ------------------------------------------------------------------ */
/* INT 08h                                                             */
/* ------------------------------------------------------------------ */

static void setup(struct vm86_cpu *cpu, struct bios1a_state *st)
{
    vm86_clear_services();
    vm86_install_ivt(cpu);
    bios1a_init(cpu, st);
    vm86_register_service(VM86_INT_TIMER, bios1a_irq, st);
}

static void test_the_handler_counts_ticks(struct vm86_cpu *cpu)
{
    struct bios1a_state st;

    setup(cpu, &st);

    for (unsigned i = 0; i < 5; i++)
        bios1a_irq(cpu, &st);

    vm86_expect_u16("five interrupts, five ticks", (uint16_t)ticks(cpu), 5);
    vm86_expect_u16("and no midnight yet", midnight_flag(cpu), 0x00);
}

/*
 * Midnight is the count going back to zero, not the count overflowing.
 *
 * The distinction is visible: a machine that let the double word run
 * over would reach zero after 4294967296 ticks and would never touch the
 * flag at 0x1800B0. So the test starts a tick short of the day and looks
 * at both the count and the flag.
 */
static void test_midnight_resets_the_count(struct vm86_cpu *cpu)
{
    struct bios1a_state st;

    setup(cpu, &st);

    uint32_t at = bda(VM86_BDA_TICK_COUNT);

    vm86_mem_write16(cpu->mem, at, 0x00AFu);        /* 0x1800AF */
    vm86_mem_write16(cpu->mem, at + 2u, 0x0018u);

    bios1a_irq(cpu, &st);

    vm86_expect_u16("the count is back to zero",
                    (uint16_t)ticks(cpu), 0x0000);
    vm86_expect_u16("and its top half too",
                    vm86_mem_read16(cpu->mem, at + 2u), 0x0000);
    vm86_expect_u16("the midnight flag went up", midnight_flag(cpu), 0x01);
}

/*
 * The flag is set, not counted.
 *
 * Two midnights with nobody reading the count in between leave one flag,
 * and the second is gone. That is a real defect in the firmware -- a
 * program keeping its own calendar off this drifts -- and it is
 * reproduced because programs were written against it. A test that
 * expected the flag to count would be a test demanding a better machine
 * than the one being built.
 */
static void test_a_second_midnight_is_lost(struct vm86_cpu *cpu)
{
    struct bios1a_state st;

    setup(cpu, &st);

    uint32_t at = bda(VM86_BDA_TICK_COUNT);

    for (unsigned i = 0; i < 2; i++) {
        vm86_mem_write16(cpu->mem, at, 0x00AFu);
        vm86_mem_write16(cpu->mem, at + 2u, 0x0018u);
        bios1a_irq(cpu, &st);
    }

    vm86_expect_u16("two midnights, still one flag",
                    midnight_flag(cpu), 0x01);
}

/* ------------------------------------------------------------------ */
/* INT 1Ah                                                             */
/* ------------------------------------------------------------------ */

static void test_read_returns_the_count_and_clears_the_flag(
        struct vm86_cpu *cpu)
{
    struct bios1a_state st;

    setup(cpu, &st);

    uint32_t at = bda(VM86_BDA_TICK_COUNT);

    /* 0x00018000 = 98304, which splits into two halves that are not the
     * same number, so a service that put the high half in DX and the low
     * half in CX would be caught. */
    vm86_mem_write16(cpu->mem, at, 0x8000u);
    vm86_mem_write16(cpu->mem, at + 2u, 0x0001u);

    vm86_mem_write8(cpu->mem, bda(VM86_BDA_TICK_ROLLOVER), 0x01);

    cpu->ah = 0x00;
    bios1a_service(cpu, &st);

    vm86_expect_u16("CX is the high half", cpu->cx, 0x0001);
    vm86_expect_u16("DX is the low half", cpu->dx, 0x8000);
    vm86_expect_u16("AL is the midnight flag", cpu->al, 0x01);
    vm86_expect_u16("which the read then clears",
                    midnight_flag(cpu), 0x00);

    /* A second read sees a cleared flag. */
    cpu->ah = 0x00;
    bios1a_service(cpu, &st);

    vm86_expect_u16("and it stays cleared", cpu->al, 0x00);
    vm86_expect_u16("with the count unchanged", cpu->cx, 0x0001);
    vm86_expect_u16("and unchanged", cpu->dx, 0x8000);
}

static void test_read_does_not_touch_the_other_registers(struct vm86_cpu *cpu)
{
    struct bios1a_state st;

    setup(cpu, &st);

    cpu->ah = 0x00;
    cpu->bx = 0x1111;
    cpu->si = 0x2222;
    cpu->di = 0x3333;
    cpu->bp = 0x4444;

    bios1a_service(cpu, &st);

    vm86_expect_u16("BX untouched", cpu->bx, 0x1111);
    vm86_expect_u16("SI untouched", cpu->si, 0x2222);
    vm86_expect_u16("DI untouched", cpu->di, 0x3333);
    vm86_expect_u16("BP untouched", cpu->bp, 0x4444);
}

static void test_set_writes_the_count(struct vm86_cpu *cpu)
{
    struct bios1a_state st;

    setup(cpu, &st);

    vm86_mem_write8(cpu->mem, bda(VM86_BDA_TICK_ROLLOVER), 0x01);

    cpu->ah = 0x01;
    cpu->cx = 0x0002;
    cpu->dx = 0x4680;

    bios1a_service(cpu, &st);

    vm86_expect_u16("the count is what was written",
                    (uint16_t)ticks(cpu), 0x4680);
    vm86_expect_u16("including its high half",
                    vm86_mem_read16(cpu->mem,
                                    bda(VM86_BDA_TICK_COUNT) + 2u), 0x0002);
    /*
     * Setting the time resets the midnight flag as well. The flag means
     * "midnight has passed since the count was last read or written",
     * and a caller that has just written the count has done the second
     * half of that. Ralf Brown's entry for this function says so; the
     * milestone's own reference file does not, which is why it is called
     * out here.
     */
    vm86_expect_u16("and the midnight flag goes with it",
                    midnight_flag(cpu), 0x00);
}

/*
 * 02h and 04h read the real-time clock, which this machine does not
 * have. The honest answer is to do nothing and leave AH as the caller
 * set it, because that is a failure a program can detect. Answering
 * zeroes would look like 1980-01-01 00:00:00 and a program would believe
 * it -- see the task book, section 6.
 */
static void test_the_rtc_functions_do_nothing(struct vm86_cpu *cpu)
{
    struct bios1a_state st;

    setup(cpu, &st);

    static const uint8_t functions[] = { 0x02, 0x04, 0x99 };

    for (unsigned i = 0; i < sizeof(functions); i++) {
        /* AH carries the function, AL something the caller left behind,
         * so a service that wrote either one is caught. */
        cpu->ax = (uint16_t)(((uint16_t)functions[i] << 8) | 0x45u);
        cpu->cx = 0x1111;
        cpu->dx = 0x2222;

        bios1a_service(cpu, &st);

        char detail[48];
        snprintf(detail, sizeof(detail), "AH=0x%02X leaves AH alone",
                 functions[i]);

        vm86_expect_u16(detail, cpu->ah, functions[i]);
        vm86_expect_u16("and CX", cpu->cx, 0x1111);
        vm86_expect_u16("and DX", cpu->dx, 0x2222);
        vm86_expect_u16("and AL", cpu->al, 0x45);
    }
}

/* ------------------------------------------------------------------ */
/* The chain from INT 08h to INT 1Ch                                   */
/* ------------------------------------------------------------------ */

/*
 * The firmware's timer handler calls INT 1Ch at the end, every tick.
 *
 * That is not a convenience for emulators: INT 1Ch exists to be hooked,
 * precisely because it is called by something that already exists rather
 * than by hardware. So the case here hooks it, runs ticks, and counts --
 * and then unhooks it, which has to go back to being an interrupt that
 * does nothing at all.
 *
 * The guest handler increments a word in its own memory. A handler that
 * silently returned, or one that ran twice per tick, are both things
 * this catches.
 */
static const uint8_t guest_handler[] = {
    0xFF, 0x06, 0x00, 0x81,   /* inc word [0x8100] */
    0xCF,                     /* iret              */
};

/*
 * Where the guest handler and its counter live.
 *
 * Both are well away from the first kilobyte, and that is not arbitrary:
 * the interrupt vector table is at linear 0x0000-0x03FF, so a test that
 * put its own data at 0x0300 would find `vm86_install_ivt()` writing
 * vector 0xC0's stub offset -- which is 0xC0 * 4 = 0x0300 -- straight
 * over it. The first version of this case did exactly that, and the
 * symptom was a counter that read 0x0300 before anything had run and
 * went back to 0x0300 whenever the table was rebuilt.
 */
#define HANDLER_LINEAR 0x8000u
#define COUNTER_LINEAR 0x8100u

/* sti; hlt; jmp $ -- waits for an interrupt, then keeps the machine
 * runnable so more can be delivered. */
static const uint8_t program[] = {
    0xFB,                     /* sti      */
    0xF4,                     /* hlt      */
    0xEB, 0xFE,               /* jmp $    */
};

static uint16_t counter(struct vm86_cpu *cpu)
{
    return vm86_mem_read16(cpu->mem, COUNTER_LINEAR);
}

static void test_the_timer_chains_to_the_user_hook(struct vm86_cpu *cpu)
{
    struct bios1a_state st;

    setup(cpu, &st);
    vm86_test_load(cpu, program, sizeof(program));
    poke(cpu, HANDLER_LINEAR, guest_handler, sizeof(guest_handler));

    /* Let the guest reach its HLT, with interrupts on. */
    run(cpu, 10);

    /* --- nothing hooked: the tick happens and 1Ch does nothing --- */
    vm86_raise(cpu, VM86_INT_TIMER);
    vm86_expect_u16("the machine runs", run(cpu, 60), VM86_STOP_STEPS);
    vm86_expect_u16("one tick happened", (uint16_t)ticks(cpu), 1);
    vm86_expect_u16("and the stub's 1Ch did nothing", counter(cpu), 0);

    /* --- hooked: the guest handler runs, once per tick --- */
    hook_vector(cpu, VM86_INT_USER_TIMER, 0x0000, HANDLER_LINEAR);

    vm86_raise(cpu, VM86_INT_TIMER);
    run(cpu, 60);
    vm86_expect_u16("two ticks", (uint16_t)ticks(cpu), 2);
    vm86_expect_u16("and now the hook has run", counter(cpu), 1);

    vm86_raise(cpu, VM86_INT_TIMER);
    run(cpu, 60);
    vm86_expect_u16("three ticks", (uint16_t)ticks(cpu), 3);
    vm86_expect_u16("twice, not once and not four times", counter(cpu), 2);

    /* --- unhooked again: back to doing nothing --- */
    vm86_install_ivt(cpu);   /* every vector back on its stub */

    vm86_raise(cpu, VM86_INT_TIMER);
    run(cpu, 60);
    vm86_expect_u16("four ticks", (uint16_t)ticks(cpu), 4);
    vm86_expect_u16("and the counter did not move", counter(cpu), 2);
}

/*
 * The chain has to survive a handler that is not there, which is the
 * case a machine spends its whole life in. If the return path through
 * two stubs and two IRETs were wrong, this is where it would show: the
 * tick count would stop after the first one.
 */
static void test_the_chain_returns_through_both_stubs(struct vm86_cpu *cpu)
{
    struct bios1a_state st;

    setup(cpu, &st);
    vm86_test_load(cpu, program, sizeof(program));

    run(cpu, 10);

    for (unsigned i = 0; i < 8; i++) {
        vm86_raise(cpu, VM86_INT_TIMER);
        run(cpu, 60);
    }

    vm86_expect_u16("every tick got through", (uint16_t)ticks(cpu), 8);
}

/* ------------------------------------------------------------------ */

static const struct vm86_test tests[] = {
    { "a second is eighteen ticks",       test_one_second_is_eighteen_ticks },
    { "exact at the tick boundary",       test_the_conversion_is_exact_at_the_tick_boundary },
    { "a minute and an hour",             test_a_minute_and_an_hour },
    { "the error stays under one tick",   test_the_error_stays_under_one_tick },
    { "a zero millisecond slice",         test_a_zero_millisecond_slice_is_not_a_tick },
    { "the handler counts ticks",         test_the_handler_counts_ticks },
    { "midnight resets the count",        test_midnight_resets_the_count },
    { "a second midnight is lost",        test_a_second_midnight_is_lost },
    { "read returns the count",           test_read_returns_the_count_and_clears_the_flag },
    { "00h leaves the other registers",   test_read_does_not_touch_the_other_registers },
    { "set writes the count",             test_set_writes_the_count },
    { "the RTC functions do nothing",     test_the_rtc_functions_do_nothing },
    { "the timer chains to the user hook", test_the_timer_chains_to_the_user_hook },
    { "the chain returns through both stubs", test_the_chain_returns_through_both_stubs },
};

VM86_TEST_MAIN("bios1a", tests)
