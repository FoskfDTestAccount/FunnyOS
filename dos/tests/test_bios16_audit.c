/*
 * An adversarial pass over INT 16h and INT 1Ah.
 *
 * ---------------------------------------------------------------------
 * Why this file exists next to test_bios16.c and test_bios1a.c
 *
 * Those two suites are thorough and they are the author's. This one came
 * from the other direction: from dos-refs.md, from the task book's own
 * claims checked against the sources rather than against the code, and
 * from looking for the places where a test that passes can still be
 * testing nothing.
 *
 * Two things it deliberately does differently:
 *
 *   - It drives the blocking read through the REAL run loop. Both of the
 *     author's suites carry their own few lines of vm86_run (written
 *     before it existed -- the comment in test_bios16.c says so) and that
 *     replica is not the thing that ships. The case here is the only one
 *     in the tree that completes a blocking keyboard read on the loop
 *     that M5 will actually be running on.
 *
 *   - It pins the right and left Shift bits separately. Nothing in either
 *     suite distinguishes them, and dos-refs.md section 7 warns that this
 *     is the pair most easily written backwards.
 *
 * A case whose name starts with KNOWN is pinning behaviour that is
 * believed wrong, with the corrected assertion written in the comment
 * above it.
 */
#include "harness.h"

#include <string.h>

#include <vm86/firmware.h>
#include <vm86/host.h>

#include "../bios/bios16.h"
#include "../bios/bios1a.h"

/* ------------------------------------------------------------------ */
/* The data area                                                       */
/* ------------------------------------------------------------------ */

#define BDA(offset) (((uint32_t)VM86_BDA_SEGMENT << 4) + (uint32_t)(offset))

static uint8_t bda8(struct vm86_cpu *cpu, uint16_t offset)
{
    return vm86_mem_read8(cpu->mem, BDA(offset));
}

static uint16_t bda16(struct vm86_cpu *cpu, uint16_t offset)
{
    return vm86_mem_read16(cpu->mem, BDA(offset));
}

static uint32_t bda32(struct vm86_cpu *cpu, uint16_t offset)
{
    return (uint32_t)bda16(cpu, offset)
         | ((uint32_t)bda16(cpu, (uint16_t)(offset + 2u)) << 16);
}

/* ------------------------------------------------------------------ */
/* A machine with a keyboard and a clock on it                         */
/* ------------------------------------------------------------------ */

struct machine {
    struct bios16_state kb;
    struct bios1a_state timer;
};

static void install(struct vm86_cpu *cpu, struct machine *m)
{
    vm86_clear_services();
    vm86_install_ivt(cpu);

    bios16_init(cpu, &m->kb);
    bios1a_init(cpu, &m->timer);

    vm86_register_service(VM86_INT_KEYBOARD,      bios16_irq,     &m->kb);
    vm86_register_service(VM86_INT_KEYBOARD_BIOS, bios16_service, &m->kb);
    vm86_register_service(VM86_INT_TIMER,         bios1a_irq,     &m->timer);
    vm86_register_service(VM86_INT_TIME,          bios1a_service, &m->timer);
}

/* A keystroke, from the controller's register through INT 09h. */
static void press(struct vm86_cpu *cpu, struct bios16_state *kb,
                  uint8_t scancode)
{
    bios16_key_arrived(kb, scancode);
    bios16_irq(cpu, kb);
}

/* What the shift-flag byte says. */
static uint16_t flags(struct vm86_cpu *cpu)
{
    return bda8(cpu, VM86_BDA_KEYBOARD_FLAGS);
}

static uint32_t ticks(struct vm86_cpu *cpu)
{
    return bda32(cpu, VM86_BDA_TICK_COUNT);
}

/* ------------------------------------------------------------------ */
/* The shift flags                                                     */
/* ------------------------------------------------------------------ */

/*
 * The right Shift key is bit 0 and the left one is bit 1.
 *
 * That is not the order anyone guesses -- "left, then right" reads better
 * and is wrong -- and dos-refs.md section 7 flags this pair by name as
 * the one most easily written backwards. Getting it wrong costs nothing
 * visible: a program that tests "is Shift down" masks both bits and works
 * either way, and only one that distinguishes them, to tell which side a
 * character came from, notices.
 *
 * Neither of the author's suites presses the two Shift keys separately.
 */
static void test_right_and_left_shift_are_different_bits(struct vm86_cpu *cpu)
{
    struct machine m;

    install(cpu, &m);

    press(cpu, &m.kb, 0x36);            /* right shift, make */
    vm86_expect_u16("right shift is bit 0", flags(cpu), 0x01u);

    press(cpu, &m.kb, 0x2A);            /* left shift, make */
    vm86_expect_u16("and left shift is bit 1", flags(cpu), 0x03u);

    press(cpu, &m.kb, 0xB6);            /* right shift, break */
    vm86_expect_u16("releasing one leaves the other held",
                    flags(cpu), 0x02u);

    press(cpu, &m.kb, 0xAA);            /* left shift, break */
    vm86_expect_u16("and then nothing is held", flags(cpu), 0x00u);
}

/*
 * Ctrl is bit 2 and Alt is bit 3, and both are also published in the
 * second flag byte at 0040:0018.
 *
 * The second byte is the part of bios16.c its own comment calls the
 * weakest: sources disagree about bits 2 and 3 of it, so only the two
 * every source agrees on are written. That decision is worth a case so
 * that it stays a decision -- if bits 2 and 3 are ever added, this says
 * which two were there first.
 */
static void test_ctrl_and_alt_are_the_upper_two_bits(struct vm86_cpu *cpu)
{
    struct machine m;

    install(cpu, &m);

    press(cpu, &m.kb, 0x1D);            /* ctrl */
    vm86_expect_u16("ctrl is bit 2", flags(cpu), 0x04u);
    vm86_expect_u16("and shows up in the second byte",
                    bda8(cpu, VM86_BDA_KEYBOARD_FLAGS2), 0x01u);

    press(cpu, &m.kb, 0x38);            /* alt */
    vm86_expect_u16("alt is bit 3", flags(cpu), 0x0Cu);
    vm86_expect_u16("and is the second bit there",
                    bda8(cpu, VM86_BDA_KEYBOARD_FLAGS2), 0x03u);

    press(cpu, &m.kb, 0x9D);
    press(cpu, &m.kb, 0xB8);
    vm86_expect_u16("both released", flags(cpu), 0x00u);
    vm86_expect_u16("in both bytes",
                    bda8(cpu, VM86_BDA_KEYBOARD_FLAGS2), 0x00u);
}

/*
 * The low nibble and the high nibble are two different kinds of thing.
 *
 * The four low bits say what is held at this instant and are recomputed
 * on every keystroke; the four high bits are locks, which stay set after
 * their key comes back up and therefore cannot be derived from what is
 * held. The write is a read-modify-write for that reason, and a case that
 * presses a shift key while a lock is on is what proves the two halves do
 * not erase each other.
 */
static void test_a_lock_survives_its_key_and_the_shift_bits(struct vm86_cpu *cpu)
{
    struct machine m;

    install(cpu, &m);

    press(cpu, &m.kb, 0x3A);            /* caps lock, make */
    vm86_expect_u16("caps lock is bit 6", flags(cpu), 0x40u);

    press(cpu, &m.kb, 0xBA);            /* caps lock, break */
    vm86_expect_u16("and a break code does not undo it",
                    flags(cpu), 0x40u);

    press(cpu, &m.kb, 0x2A);            /* left shift, make */
    vm86_expect_u16("a shift key does not clear it", flags(cpu), 0x42u);

    press(cpu, &m.kb, 0xAA);
    press(cpu, &m.kb, 0x3A);            /* caps off again */
    vm86_expect_u16("and the lock clears when it is pressed again",
                    flags(cpu), 0x00u);

    press(cpu, &m.kb, 0x45);            /* num lock */
    press(cpu, &m.kb, 0x46);            /* scroll lock */
    vm86_expect_u16("num lock is bit 5 and scroll lock bit 4",
                    flags(cpu), 0x30u);
}

/*
 * Insert is both a lock key and a key.
 *
 * It flips the state at 0040:0017 bit 7, and it also produces a keystroke
 * like any other -- a program that reads keys sees the Insert key, and a
 * program that watches the flag sees the mode change. This is the only
 * key on the keyboard that does both.
 */
static void test_insert_is_a_lock_and_a_keystroke(struct vm86_cpu *cpu)
{
    struct machine m;
    uint16_t word;

    install(cpu, &m);

    press(cpu, &m.kb, 0x52);            /* insert */

    vm86_expect_u16("the insert bit flipped", flags(cpu), 0x80u);
    vm86_expect_u16("and the queue has something",
                    bda16(cpu, VM86_BDA_KB_HEAD) != bda16(cpu, VM86_BDA_KB_TAIL),
                    1u);

    cpu->ah = 0x00;
    bios16_service(cpu, &m.kb);
    word = cpu->ax;

    vm86_expect_u16("with Insert's own scan code in it", (uint16_t)(word >> 8),
                    0x52u);

    press(cpu, &m.kb, 0xD2);            /* insert, break */
    vm86_expect_u16("and the break code does not flip it back",
                    flags(cpu), 0x80u);
}

/*
 * A shift key is held, not pressed.
 *
 * Holding Shift puts nothing in the queue: a program that reads keys does
 * not get a keystroke for the modifier, and it must not, or every capital
 * letter would look like two keys.
 */
static void test_a_shift_key_never_reaches_the_queue(struct vm86_cpu *cpu)
{
    struct machine m;

    install(cpu, &m);

    press(cpu, &m.kb, 0x2A);
    press(cpu, &m.kb, 0x1D);
    press(cpu, &m.kb, 0x38);
    press(cpu, &m.kb, 0xAA);

    vm86_expect_u16("nothing was queued",
                    bda16(cpu, VM86_BDA_KB_HEAD), bda16(cpu, VM86_BDA_KB_TAIL));
    vm86_expect_u16("and the pointers are still at the start of the ring",
                    bda16(cpu, VM86_BDA_KB_HEAD), VM86_BDA_KB_EMPTY);
}

/* ------------------------------------------------------------------ */
/* The queue                                                           */
/* ------------------------------------------------------------------ */

/*
 * Fifteen keys fit and the sixteenth is the one that is lost.
 *
 * The ring is sixteen words and holds fifteen, because head == tail means
 * both empty and full: the last slot is spent telling the two apart. The
 * key that is dropped is the NEWEST one, which is the recoverable choice
 * -- the person typing can press it again, and a ring that overwrote its
 * oldest entry instead would be corrupting data a program is about to
 * read.
 *
 * Read back through the service rather than by looking at the pointers,
 * so the case is about what a program gets and not about the arithmetic
 * behind it.
 */
static void test_the_ring_holds_fifteen_and_drops_the_newest(struct vm86_cpu *cpu)
{
    struct machine m;

    /* Fifteen distinct make codes: the top row, then the home row. */
    static const uint8_t keys[15] = {
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
        0x1E, 0x1F, 0x20, 0x21, 0x22,
    };

    install(cpu, &m);

    for (unsigned i = 0; i < 15; i++)
        press(cpu, &m.kb, keys[i]);

    /* The sixteenth has nowhere to go. */
    press(cpu, &m.kb, 0x23);

    unsigned got = 0;
    for (;;) {
        cpu->ah = 0x01;
        bios16_service(cpu, &m.kb);

        if (cpu->flags & VM86_ZF)
            break;

        cpu->ah = 0x00;
        bios16_service(cpu, &m.kb);
        got++;
        if (got > 100)
            break;      /* a ring that will not drain is a failure of its
                         * own, and this keeps the case from hanging */
    }

    vm86_expect_u16("fifteen keys came back", got, 15u);

    /* And the one that was dropped is the last one pressed: its scan code
     * is not in the queue. */
    cpu->ah = 0x01;
    bios16_service(cpu, &m.kb);
    vm86_expect_bool("the queue is empty afterwards",
                     (cpu->flags & VM86_ZF) != 0, true);
}

/*
 * The pointers never leave the ring.
 *
 * They are offsets relative to segment 0040, so they run from 0x1E to
 * 0x3C and step by two. A pointer that walked off either end would make
 * the next keystroke land on the equipment word or on the tick count --
 * and the symptom would be a machine whose clock jumps when a program
 * types.
 */
static void test_the_pointers_stay_inside_the_ring(struct vm86_cpu *cpu)
{
    struct machine m;

    install(cpu, &m);

    bool inside = true;

    /* Far enough to wrap the ring several times, pushing and taking out
     * of step so the pointers visit every position. */
    for (unsigned i = 0; i < 40; i++) {
        press(cpu, &m.kb, (uint8_t)(0x10 + (i % 10)));

        uint16_t head = bda16(cpu, VM86_BDA_KB_HEAD);
        uint16_t tail = bda16(cpu, VM86_BDA_KB_TAIL);

        if (head < VM86_BDA_KB_BUFFER || head >= VM86_BDA_KB_BUFFER_END)
            inside = false;
        if (tail < VM86_BDA_KB_BUFFER || tail >= VM86_BDA_KB_BUFFER_END)
            inside = false;
        if ((head & 1u) != 0 || (tail & 1u) != 0)
            inside = false;

        if ((i % 3) == 0) {
            cpu->ah = 0x00;
            bios16_service(cpu, &m.kb);
        }
    }

    vm86_expect_bool("every pointer stayed in 0x1E..0x3D and on an even "
                     "offset", inside, true);
}

/*
 * *** PINS A KNOWN DEFECT ***  -- see M4-bios16-audit-report.md
 *
 * 0040:0071 is the Ctrl-Break flag. Ralf Brown's memory list gives it as
 * "bit 7 is set when Ctrl-Break has been pressed", cross-referenced to INT
 * 1Bh, and that is the byte everything from FreeDOS to DOSBox reads to
 * find out whether the user hit the break key. It is not an overflow flag,
 * and the real firmware sets nothing there when the keyboard buffer fills
 * -- it beeps and discards the keystroke.
 *
 * The code writes bit 7 there on overflow, because the task book's
 * pitfalls list says to (M4-C-kbd-time.md, "把 0040:0071 的 bit 7 置位").
 * The module's own comment notices that the offset is not in firmware.h's
 * map and not in the list of fields the module owns -- and then trusts the
 * pitfall note over the ownership rule, which is exactly the rule that
 * would have caught this.
 *
 * The consequence is not cosmetic. A program polling 40:71 bit 7 to
 * detect Ctrl-Break -- which is the ordinary way to do it -- sees the
 * break flag set every time it falls behind on reading keys, and a
 * program that treats the bit as "stop what you are doing" stops.
 *
 * When this is fixed, the last assertion becomes `0x00`.
 */
static void test_an_overflow_touches_the_ctrl_break_flag(struct vm86_cpu *cpu)
{
    struct machine m;

    static const uint8_t keys[15] = {
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
        0x1E, 0x1F, 0x20, 0x21, 0x22,
    };

    install(cpu, &m);

    vm86_expect_u16("0040:0071 starts clear", bda8(cpu, 0x0071u), 0x00u);

    for (unsigned i = 0; i < 15; i++)
        press(cpu, &m.kb, keys[i]);

    vm86_expect_u16("and is still clear with the buffer merely full",
                    bda8(cpu, 0x0071u), 0x00u);

    press(cpu, &m.kb, 0x23);            /* the sixteenth */

    vm86_expect_u16("but overflowing it sets bit 7 of the Ctrl-Break flag",
                    (uint16_t)(bda8(cpu, 0x0071u) & 0x80u), 0x80u);
}

/* ------------------------------------------------------------------ */
/* INT 16h, the service                                                */
/* ------------------------------------------------------------------ */

/*
 * AH=02h answers with the shift byte in AL and touches nothing else.
 *
 * The task book originally asked for the scan code in AH with a zero when
 * there was none -- which would have been the AH=00h/01h answer written
 * one row too far down the table, and would have told a program that a
 * key with scan code zero had arrived. dos-refs.md section 5 records the
 * correction and the source disagreement behind it.
 *
 * AH is where the function number lives, so leaving it alone is also how
 * a program can tell that the call did something.
 */
static void test_02h_answers_in_al_and_leaves_ah_alone(struct vm86_cpu *cpu)
{
    struct machine m;

    install(cpu, &m);

    press(cpu, &m.kb, 0x1D);            /* hold ctrl */

    cpu->ax = 0x02FF;
    bios16_service(cpu, &m.kb);

    vm86_expect_u16("the shift byte came back in AL", cpu->al, 0x04u);
    vm86_expect_u16("and AH still says which function this was",
                    cpu->ah, 0x02u);
}

/*
 * AH=01h on an empty queue sets ZF and leaves AX exactly as it found it.
 *
 * The published tables disagree about whether AX is zeroed; this firmware
 * decided not to, and the reason is worth a case: a program that ignores
 * ZF and reads AX anyway would act on a keystroke with scan code zero and
 * character zero, which is a key that no keyboard can produce. Leaving
 * the register untouched means such a program acts on whatever it had.
 */
static void test_01h_leaves_ax_alone_when_there_is_nothing(struct vm86_cpu *cpu)
{
    struct machine m;

    install(cpu, &m);

    cpu->ah = 0x01;
    cpu->al = 0x34;
    bios16_service(cpu, &m.kb);

    vm86_expect_bool("nothing waiting", (cpu->flags & VM86_ZF) != 0, true);
    vm86_expect_u16("AX untouched, both halves", cpu->ax, 0x0134u);
}

/*
 * A function number this firmware does not have changes nothing.
 *
 * AH=10h and above are the enhanced-keyboard calls. A program uses them
 * to find out whether it is on an AT: it calls one and looks at what came
 * back. Leaving AH as the caller set it is the answer "not here", which
 * is the true one, and it is only true if nothing else is written either.
 */
static void test_an_unimplemented_function_changes_nothing(struct vm86_cpu *cpu)
{
    struct machine m;

    install(cpu, &m);

    cpu->ax = 0x11AB;
    cpu->bx = 0x1234;
    cpu->cx = 0x5678;
    cpu->dx = 0x9ABC;
    bios16_service(cpu, &m.kb);

    vm86_expect_u16("AX", cpu->ax, 0x11ABu);
    vm86_expect_u16("BX", cpu->bx, 0x1234u);
    vm86_expect_u16("CX", cpu->cx, 0x5678u);
    vm86_expect_u16("DX", cpu->dx, 0x9ABCu);
}

/*
 * The scan code is the high half of the word in the ring and the
 * character the low half.
 *
 * A program that reads the queue itself -- which is ordinary, the layout
 * is published -- gets one word per keystroke, and swapping the halves
 * would turn every letter into a character with a nonsense code above it.
 * The service and the raw word are checked against each other so that the
 * two ways of asking agree.
 */
static void test_the_scan_code_is_the_high_half(struct vm86_cpu *cpu)
{
    struct machine m;

    install(cpu, &m);

    press(cpu, &m.kb, 0x1E);            /* 'a' */

    uint16_t raw = vm86_mem_read16(cpu->mem, BDA(bda16(cpu, VM86_BDA_KB_HEAD)));

    cpu->ah = 0x00;
    bios16_service(cpu, &m.kb);

    vm86_expect_u16("the word in the ring has the scan code high",
                    raw, 0x1E61u);
    vm86_expect_u16("and the service returns the same word", cpu->ax, raw);
}

/* ------------------------------------------------------------------ */
/* The blocking read, on the run loop that ships                       */
/* ------------------------------------------------------------------ */

/*
 * A blocking read is satisfied by an interrupt that arrives while it
 * waits.
 *
 * This is the case neither of the author's suites can write, because both
 * of them carry their own copy of the run loop. It is also the only case
 * in this suite that exercises the whole path: INT 16h through the vector
 * table to the stub, the trap, the service deciding it cannot answer,
 * vm86_service_retry, the run loop spending a boundary, IRQ1 being
 * delivered to the keyboard handler, the buffer filling, and the guest
 * re-executing the INT and finding the key.
 *
 * It is also the case that pins the decision NOT to arm the interrupt
 * shadow in the service. The retry re-enters the stub from the top on
 * every attempt, so a shadow armed on each attempt would be re-armed
 * before the boundary that would have cleared it -- and the machine would
 * never deliver anything, which is a blocking read that never returns.
 * The budget is what keeps that failure a failed assertion rather than a
 * hung suite.
 */
static void test_a_blocking_read_is_satisfied_while_it_waits(struct vm86_cpu *cpu)
{
    struct machine m;

    install(cpu, &m);

    /* A key is in the controller's register and the interrupt that reads
     * it has not been delivered yet: the state a host leaves behind. */
    bios16_key_arrived(&m.kb, 0x1E);    /* 'a' */
    vm86_raise(cpu, VM86_INT_KEYBOARD);

    /* mov ah, 0 ; int 16h ; hlt */
    static const uint8_t code[] = { 0xB4, 0x00, 0xCD, 0x16, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    enum vm86_stop result = vm86_run(cpu, 200);

    vm86_expect_u16("the read returned the scan code in AH", cpu->ah, 0x1Eu);
    vm86_expect_u16("and the character in AL", cpu->al, 'a');
    vm86_expect_bool("the program carried on to its halt",
                     result == VM86_STOP_HALT, true);
    vm86_expect_u16("with the queue drained",
                    bda16(cpu, VM86_BDA_KB_HEAD),
                    bda16(cpu, VM86_BDA_KB_TAIL));
}

/*
 * And a blocking read that is never satisfied gives the slice back.
 *
 * A service that waits is a service that must not hold the host: the
 * whole reason the retry returns to the run loop instead of spinning is
 * that the host has to get control back to feed a key. So a read with
 * nothing behind it runs out of budget and returns, with the machine
 * still runnable and nothing invented -- and the caller can call again.
 */
static void test_a_blocking_read_with_no_key_gives_the_slice_back(
        struct vm86_cpu *cpu)
{
    struct machine m;

    install(cpu, &m);

    static const uint8_t code[] = { 0xB4, 0x00, 0xCD, 0x16, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    enum vm86_stop result = vm86_run(cpu, 50);

    vm86_expect_bool("the slice ended rather than the service spinning",
                     result == VM86_STOP_STEPS, true);
    vm86_expect_bool("the machine is still runnable", cpu->halted == false,
                     true);
    vm86_expect_u16("no key was invented", cpu->al, 0x00u);
    vm86_expect_u16("and the queue is still empty",
                    bda16(cpu, VM86_BDA_KB_HEAD),
                    bda16(cpu, VM86_BDA_KB_TAIL));

    /* Calling again makes progress rather than starting over: the machine
     * is inside the retry, not at the beginning of the program. */
    enum vm86_stop again = vm86_run(cpu, 50);

    vm86_expect_bool("and a second slice behaves the same",
                     again == VM86_STOP_STEPS, true);
}

/* ------------------------------------------------------------------ */
/* The clock                                                           */
/* ------------------------------------------------------------------ */

/*
 * The tick rate is the crystal divided by the counter, twice.
 *
 * 1.193182 MHz through a sixteen-bit divider is 18.2065 Hz, so a second
 * is eighteen ticks and a day is 0x1800B0 of them. This pins the
 * arithmetic rather than the constant: 1000 ms must be exactly 18, and
 * the leftover has to be carried, or a host polling every few
 * milliseconds loses a tick every few milliseconds.
 */
static void test_the_tick_rate_is_eighteen_and_a_bit(struct vm86_cpu *cpu)
{
    struct bios1a_state st;

    (void)cpu;

    bios1a_reset(&st);
    vm86_expect_u16("one second is eighteen ticks",
                    bios1a_advance(&st, 1000u), 18u);

    /*
     * And the remainder is carried -- which a one-call assertion cannot
     * show. Fifty milliseconds is 0.91 of a tick, so truncating each call
     * gives zero for ever and a host polling every 50 ms would lose every
     * call. Fifty-five of those slices is 2.75 seconds, which is fifty
     * ticks by the rate either way; what tells the two apart is that the
     * carry has to remember them.
     */
    bios1a_reset(&st);

    uint32_t carried = 0;
    for (unsigned i = 0; i < 55u; i++)
        carried += bios1a_advance(&st, 50u);

    vm86_expect_u16("2.75 seconds in 50 ms slices is fifty ticks",
                    (uint16_t)carried, 50u);

    bios1a_reset(&st);
    vm86_expect_u16("a minute is 1092 ticks",
                    bios1a_advance(&st, 60000u), 1092u);

    bios1a_reset(&st);
    uint32_t hour = bios1a_advance(&st, 3600000u);
    vm86_expect_u16("an hour is 65543", (uint16_t)(hour >> 16), 0x01u);
    vm86_expect_u16("in the low half too", (uint16_t)(hour & 0xFFFFu),
                    0x0007u);
}

/*
 * *** THE HEADER'S ARITHMETIC IS WRONG, THE CONSTANT IS RIGHT ***
 *
 * bios1a.h says BIOS1A_TICKS_PER_DAY is "eighteen and a bit ticks a
 * second, times 86400, rounded down, the same rounding the firmware
 * does". Worked out, that is not 0x1800B0.
 *
 *   1193182 / 65536 * 86400 = 1573042.68  -> 1573042 = 0x1800B2
 *
 * 0x1800B0 is 1573040, two ticks short. The constant is right and the
 * reason is wrong: 0x1800B0 is the value the IBM firmware uses, and it is
 * short because the BIOS clock is short -- a real machine's day boundary
 * arrives about 2.7 ticks before 24 hours of real time have gone by.
 *
 * This matters because of which half a reader would trust. Someone who
 * checked the arithmetic and found it did not produce the constant would
 * "fix" the constant, and the machine would then disagree with every
 * program written against the real one about when midnight is.
 */
static void test_the_bios_day_is_shorter_than_a_real_day(struct vm86_cpu *cpu)
{
    struct bios1a_state st;

    (void)cpu;

    bios1a_reset(&st);
    uint32_t day = bios1a_advance(&st, 86400000u);

    vm86_expect_u16("86400 seconds of milliseconds is 0x1800B2 ticks",
                    (uint16_t)(day >> 16), 0x18u);
    vm86_expect_u16("in the low half", (uint16_t)(day & 0xFFFFu), 0x00B2u);

    vm86_expect_u16("and the firmware's day is 0x1800B0",
                    (uint16_t)(BIOS1A_TICKS_PER_DAY >> 16), 0x18u);
    vm86_expect_u16("in the low half",
                    (uint16_t)(BIOS1A_TICKS_PER_DAY & 0xFFFFu), 0x00B0u);

    vm86_expect_bool("so a real day is longer than the machine's",
                     day > BIOS1A_TICKS_PER_DAY, true);
}

/*
 * The count rolls on the tick after the last one of the day.
 *
 * A day is 0x1800B0 ticks, counting from zero, so the last value a
 * program can see is 0x1800AF and the tick that would make it 0x1800B0
 * makes it zero and raises the flag instead. An implementation that
 * rolled at the wrong end would be a day that is one tick short or long
 * and a flag raised at the wrong moment -- and nothing else would notice,
 * because the count looks reasonable either way.
 */
static void test_the_count_rolls_on_the_tick_after_the_last(struct vm86_cpu *cpu)
{
    struct machine m;

    install(cpu, &m);

    /* The last value a program can see before midnight. */
    uint32_t last = BIOS1A_TICKS_PER_DAY - 1u;

    vm86_mem_write16(cpu->mem, BDA(VM86_BDA_TICK_COUNT),
                     (uint16_t)(last & 0xFFFFu));
    vm86_mem_write16(cpu->mem, BDA(VM86_BDA_TICK_COUNT) + 2u,
                     (uint16_t)(last >> 16));

    vm86_expect_u16("the count is at the last tick of the day",
                    (uint16_t)ticks(cpu), 0x00AFu);
    vm86_expect_u16("with the flag down", bda8(cpu, VM86_BDA_TICK_ROLLOVER), 0);

    /* One more tick. */
    bios1a_irq(cpu, &m.timer);

    vm86_expect_u16("the count went back to zero", (uint16_t)ticks(cpu), 0u);
    vm86_expect_u16("and the flag is up", bda8(cpu, VM86_BDA_TICK_ROLLOVER),
                    0x01u);

    bios1a_irq(cpu, &m.timer);
    vm86_expect_u16("the next tick counts normally", (uint16_t)ticks(cpu), 1u);
}

/*
 * The midnight flag is set, never counted.
 *
 * Two midnights with nobody reading the count between them leave the flag
 * exactly as one did, and that is a real defect in the machine being
 * emulated rather than here: DOS dates have been known to stall because
 * of it. It is reproduced rather than repaired, because a program written
 * against the real machine expects it.
 */
static void test_two_midnights_leave_one_flag(struct vm86_cpu *cpu)
{
    struct machine m;

    install(cpu, &m);

    uint16_t day_low = (uint16_t)((BIOS1A_TICKS_PER_DAY - 1u) & 0xFFFFu);
    uint16_t day_high = (uint16_t)((BIOS1A_TICKS_PER_DAY - 1u) >> 16);

    for (unsigned midnight = 0; midnight < 2; midnight++) {
        vm86_mem_write16(cpu->mem, BDA(VM86_BDA_TICK_COUNT), day_low);
        vm86_mem_write16(cpu->mem, BDA(VM86_BDA_TICK_COUNT) + 2u, day_high);
        bios1a_irq(cpu, &m.timer);
    }

    vm86_expect_u16("two midnights, one flag",
                    bda8(cpu, VM86_BDA_TICK_ROLLOVER), 0x01u);
}

/*
 * AH=00h hands back the flag it found and clears it on the way out.
 *
 * The order is what makes the answer usable: a program that spans
 * midnight compares the count with the flag, and it can only do that if
 * the flag describes the count it was just given. Clearing it is what
 * makes the next read mean "since this one".
 */
static void test_00h_returns_the_flag_and_clears_it(struct vm86_cpu *cpu)
{
    struct machine m;

    install(cpu, &m);

    vm86_mem_write8(cpu->mem, BDA(VM86_BDA_TICK_ROLLOVER), 0x01u);
    vm86_mem_write16(cpu->mem, BDA(VM86_BDA_TICK_COUNT), 0x1234u);
    vm86_mem_write16(cpu->mem, BDA(VM86_BDA_TICK_COUNT) + 2u, 0x0000u);

    cpu->ah = 0x00;
    bios1a_service(cpu, &m.timer);

    vm86_expect_u16("the old flag came back in AL", cpu->al, 0x01u);
    vm86_expect_u16("the count came back in DX", cpu->dx, 0x1234u);
    vm86_expect_u16("and in CX", cpu->cx, 0x0000u);
    vm86_expect_u16("and the flag is now clear",
                    bda8(cpu, VM86_BDA_TICK_ROLLOVER), 0x00u);

    cpu->ah = 0x00;
    bios1a_service(cpu, &m.timer);
    vm86_expect_u16("so the next read says no midnight", cpu->al, 0x00u);
}

/*
 * AH=01h sets the count and puts the flag down with it.
 *
 * A program that has just written the count has read it in every sense
 * that matters, so leaving a flag up would tell it that a day boundary
 * happened at an unknown point in a count it is defining. Ralf Brown's
 * entry says the call resets the flag; dos-refs.md section 6 does not
 * mention it, which is why it is written down here.
 */
static void test_01h_sets_the_count_and_clears_the_flag(struct vm86_cpu *cpu)
{
    struct machine m;

    install(cpu, &m);

    vm86_mem_write8(cpu->mem, BDA(VM86_BDA_TICK_ROLLOVER), 0x01u);

    cpu->ah = 0x01;
    cpu->cx = 0x0001u;
    cpu->dx = 0x2345u;
    bios1a_service(cpu, &m.timer);

    vm86_expect_u16("the count is CX:DX", (uint16_t)ticks(cpu), 0x2345u);
    vm86_expect_u16("with CX as the high half",
                    (uint16_t)(ticks(cpu) >> 16), 0x0001u);
    vm86_expect_u16("and the flag went down with it",
                    bda8(cpu, VM86_BDA_TICK_ROLLOVER), 0x00u);
}

/*
 * The chain from INT 08h to INT 1Ch leaves the stack where it found it.
 *
 * The timer handler is entered through the stub and calls INT 1Ch from
 * inside, so the 1Ch frame is pushed on top of the 08h frame and both
 * have to come back off in the right order. A guest that hooks 1Ch gets
 * the same treatment, and the stack balance is the thing that catches an
 * implementation whose frame order is wrong in a way the count cannot
 * show -- a counter that increments correctly while the machine's stack
 * creeps downward six bytes per tick.
 *
 * The program is C's: wait at an HLT so ticks keep arriving.
 */
static const uint8_t GUEST_TICK_HANDLER[] = {
    0xFF, 0x06, 0x00, 0x09,   /* inc word [0x0900] */
    0xCF,                     /* iret              */
};

#define HANDLER_LINEAR 0x0800u
#define COUNTER_LINEAR 0x0900u

static void test_the_timer_chain_balances_the_stack(struct vm86_cpu *cpu)
{
    struct machine m;

    install(cpu, &m);

    /* sti ; hlt ; jmp $ */
    static const uint8_t program[] = { 0xFB, 0xF4, 0xEB, 0xFE };

    vm86_test_load(cpu, program, sizeof(program));

    for (unsigned i = 0; i < sizeof(GUEST_TICK_HANDLER); i++)
        vm86_mem_write8(cpu->mem, HANDLER_LINEAR + i, GUEST_TICK_HANDLER[i]);

    vm86_mem_write16(cpu->mem, (uint32_t)VM86_INT_USER_TIMER * 4u,
                     HANDLER_LINEAR);
    vm86_mem_write16(cpu->mem, (uint32_t)VM86_INT_USER_TIMER * 4u + 2u, 0x0000u);

    vm86_run(cpu, 10);                  /* let the guest reach its HLT */

    uint16_t sp_before = cpu->sp;

    for (unsigned i = 0; i < 5; i++) {
        vm86_raise(cpu, VM86_INT_TIMER);
        vm86_run(cpu, 40);
    }

    vm86_expect_u16("five ticks were counted", (uint16_t)ticks(cpu), 5u);
    vm86_expect_u16("the guest's hook ran five times",
                    vm86_mem_read16(cpu->mem, COUNTER_LINEAR), 5u);
    vm86_expect_u16("and the stack is where it started",
                    cpu->sp, sp_before);
}

/* ------------------------------------------------------------------ */

static const struct vm86_test tests[] = {
    { "right and left shift are different bits",
      test_right_and_left_shift_are_different_bits },
    { "ctrl and alt are the upper two bits of the low nibble",
      test_ctrl_and_alt_are_the_upper_two_bits },
    { "a lock survives its key and the shift bits",
      test_a_lock_survives_its_key_and_the_shift_bits },
    { "insert is a lock and a keystroke",
      test_insert_is_a_lock_and_a_keystroke },
    { "a shift key never reaches the queue",
      test_a_shift_key_never_reaches_the_queue },

    { "the ring holds fifteen and drops the newest",
      test_the_ring_holds_fifteen_and_drops_the_newest },
    { "the pointers stay inside the ring",
      test_the_pointers_stay_inside_the_ring },
    { "KNOWN DEFECT: an overflow touches the Ctrl-Break flag",
      test_an_overflow_touches_the_ctrl_break_flag },

    { "02h answers in AL and leaves AH alone",
      test_02h_answers_in_al_and_leaves_ah_alone },
    { "01h leaves AX alone when there is nothing",
      test_01h_leaves_ax_alone_when_there_is_nothing },
    { "an unimplemented function changes nothing",
      test_an_unimplemented_function_changes_nothing },
    { "the scan code is the high half",
      test_the_scan_code_is_the_high_half },

    { "a blocking read is satisfied while it waits",
      test_a_blocking_read_is_satisfied_while_it_waits },
    { "a blocking read with no key gives the slice back",
      test_a_blocking_read_with_no_key_gives_the_slice_back },

    { "the tick rate is eighteen and a bit",
      test_the_tick_rate_is_eighteen_and_a_bit },
    { "the BIOS day is shorter than a real day",
      test_the_bios_day_is_shorter_than_a_real_day },
    { "the count rolls on the tick after the last",
      test_the_count_rolls_on_the_tick_after_the_last },
    { "two midnights leave one flag",
      test_two_midnights_leave_one_flag },
    { "00h returns the flag and clears it",
      test_00h_returns_the_flag_and_clears_it },
    { "01h sets the count and clears the flag",
      test_01h_sets_the_count_and_clears_the_flag },
    { "the timer chain balances the stack",
      test_the_timer_chain_balances_the_stack },
};

VM86_TEST_MAIN("bios16_audit", tests)
