/*
 * INT 16h -- the keyboard.
 *
 * The cases here run in two styles, and the split is deliberate.
 *
 * Most of them call `bios16_irq()` and `bios16_service()` directly: they
 * are about what the handler and the service do, and going through the
 * vector table to ask would add a layer that cannot fail in a way the
 * case cares about.
 *
 * The blocking read is the exception. Its whole value is that the guest
 * really waits and the interrupt really arrives in the gap, and neither
 * of those is observable if the service is called by hand. Those cases
 * go through the real trap and a few lines of run loop, because the
 * thing being tested is the interaction and a test that does not have
 * the interaction in it tests nothing.
 *
 * The include is a relative path because the bios module headers are not
 * on the compiler's include path and this milestone does not allow
 * editing dos/Makefile. Every suite that tests a bios module will have
 * the same line.
 */
#include "harness.h"

#include <stdio.h>

#include <vm86/firmware.h>
#include <vm86/host.h>

#include "../bios/bios16.h"

/* ------------------------------------------------------------------ */
/* Plumbing                                                            */
/* ------------------------------------------------------------------ */

static uint32_t bda(uint16_t offset)
{
    return ((uint32_t)VM86_BDA_SEGMENT << 4) + offset;
}

static uint16_t bda16(struct vm86_cpu *cpu, uint16_t offset)
{
    return vm86_mem_read16(cpu->mem, bda(offset));
}

static uint8_t bda8(struct vm86_cpu *cpu, uint16_t offset)
{
    return vm86_mem_read8(cpu->mem, bda(offset));
}

/* Put one scan code through the whole hardware path: into the
 * controller's register, then through INT 09h. */
static void press(struct vm86_cpu *cpu, struct bios16_state *st, uint8_t sc)
{
    bios16_key_arrived(st, sc);
    bios16_irq(cpu, st);
}

/*
 * Where the program would continue once the read comes back.
 *
 * The guest is `int 16h; cli; hlt` at 0x100, so this is the `cli`.
 *
 * The blocking-read cases assert against *this* rather than against the
 * address of a stub, and that choice is deliberate. A retry can be built
 * two ways -- rewind the pointer to the INT and let the guest run it
 * again, or rewind it to the trap the INT landed on -- and both leave
 * the guest waiting, which is the only thing the guest can observe. A
 * test that pinned the pointer to one of those addresses would be
 * deciding an interface question by accident, and would go red the day
 * somebody decided it the other way. "Has the read returned" is what the
 * task asks to know, and it is what is asserted.
 */
#define AFTER_THE_INT (VM86_TEST_CODE_BASE + 2)

/*
 * The blocking-read cases run the machine with vm86_run(), the real run
 * loop. They used to carry their own twenty-line copy of it, because the
 * real one had not landed yet; that copy is gone, and the reason it is
 * worth saying so is that a copy of the loop in a test file can drift
 * from the loop the machine runs, and then the test is measuring the
 * fixture.
 */

/*
 * Stand a machine up: vector table, keyboard service registered, data
 * area initialised. Returns the state the caller owns.
 */
static void install(struct vm86_cpu *cpu, struct bios16_state *st)
{
    vm86_clear_services();
    vm86_install_ivt(cpu);
    bios16_init(cpu, st);
    vm86_register_service(VM86_INT_KEYBOARD_BIOS, bios16_service, st);
    vm86_register_service(VM86_INT_KEYBOARD, bios16_irq, st);
}

/* ------------------------------------------------------------------ */
/* Reading a key (AH=00h)                                              */
/* ------------------------------------------------------------------ */

static void test_read_takes_a_key(struct vm86_cpu *cpu)
{
    struct bios16_state st;

    install(cpu, &st);
    press(cpu, &st, 0x1E);   /* 'a' */

    cpu->ah = 0x00;
    bios16_service(cpu, &st);

    vm86_expect_u16("AL is the character", cpu->al, 'a');
    vm86_expect_u16("AH is the scan code", cpu->ah, 0x1E);
    /*
     * Empty is head == tail, and both have moved on by one entry. A
     * service that only moved the head would leave the ring looking
     * empty and behaving as though one slot had been lost.
     */
    vm86_expect_u16("the head moved up by one entry",
                    bda16(cpu, VM86_BDA_KB_HEAD),
                    (uint16_t)(VM86_BDA_KB_EMPTY + 2));
    vm86_expect_u16("and the tail is where it left it",
                    bda16(cpu, VM86_BDA_KB_TAIL),
                    (uint16_t)(VM86_BDA_KB_EMPTY + 2));
}

static void test_read_does_not_touch_the_other_registers(struct vm86_cpu *cpu)
{
    struct bios16_state st;

    install(cpu, &st);
    press(cpu, &st, 0x1E);

    cpu->ah  = 0x00;
    cpu->cx  = 0x1111;
    cpu->dx  = 0x2222;
    cpu->si  = 0x3333;
    cpu->di  = 0x4444;
    cpu->bp  = 0x5555;
    cpu->bx  = 0x6666;

    bios16_service(cpu, &st);

    vm86_expect_u16("CX untouched", cpu->cx, 0x1111);
    vm86_expect_u16("DX untouched", cpu->dx, 0x2222);
    vm86_expect_u16("SI untouched", cpu->si, 0x3333);
    vm86_expect_u16("DI untouched", cpu->di, 0x4444);
    vm86_expect_u16("BP untouched", cpu->bp, 0x5555);
    vm86_expect_u16("BX untouched", cpu->bx, 0x6666);
}

/*
 * The blocking read, through the vector table.
 *
 * Three things have to be true at once and only the middle one is the
 * point: the guest is left waiting rather than handed a phantom key; a
 * key that arrives while it waits is the key it reads; and the stack is
 * exactly where it started when the read finally completes.
 */
static void test_blocking_read_waits_for_a_key(struct vm86_cpu *cpu)
{
    static const uint8_t program[] = {
        0xCD, 0x16,   /* int 16h -- AH is zero, so this is a read */
        0xFA,         /* cli */
        0xF4,         /* hlt */
    };

    struct bios16_state st;

    install(cpu, &st);
    vm86_test_load(cpu, program, sizeof(program));

    uint16_t sp_before = cpu->sp;

    /* A program that reads the keyboard has interrupts on; the machine
     * the harness resets has them off, because that is the state a
     * processor comes up in. */
    cpu->flags |= VM86_IF;

    vm86_expect_u16("call it and it stays blocked",
                    vm86_run(cpu, 20), VM86_STOP_STEPS);

    /*
     * The pointer was wound back rather than left past the INT. That is
     * the whole of what "try again" means, and it is visible: had the
     * service simply returned, the guest would have carried on with an
     * answer it never received -- and the accumulator is zero, so what
     * it carried on with is a keystroke that does not exist.
     */
    vm86_expect_bool("the read has not come back",
                     cpu->ip == AFTER_THE_INT, false);
    vm86_expect_u16("nothing was consumed",
                    bda16(cpu, VM86_BDA_KB_HEAD),
                    bda16(cpu, VM86_BDA_KB_TAIL));
    vm86_expect_u16("and no phantom key was handed back", cpu->ah, 0x00);

    /*
     * Now the part that makes the retry worth anything: the key arrives
     * while the guest is waiting, as a hardware interrupt, and the run
     * loop delivers it in the gap between two attempts.
     */
    bios16_key_arrived(&st, 0x1E);
    vm86_raise(cpu, VM86_INT_KEYBOARD);

    vm86_expect_u16("the guest finishes",
                    vm86_run(cpu, 40), VM86_STOP_HALT);

    vm86_expect_u16("AL is the character that arrived while it waited",
                    cpu->al, 'a');
    vm86_expect_u16("AH is its scan code", cpu->ah, 0x1E);
    vm86_expect_u16("SP is exactly where it started", cpu->sp, sp_before);
}

/*
 * The other half of the same mechanism, on its own: an interrupt raised
 * while the guest is spinning reaches INT 09h, which is the only way the
 * buffer is ever filled. Without this the blocking read above would wait
 * forever, and the reason would look like a broken service.
 *
 * The key used is a Shift, and that is the point of the case. A Shift
 * makes INT 09h do something visible -- it sets a bit in 0040:0017 --
 * without putting anything in the buffer, so the guest is still waiting
 * afterwards and the evidence that the handler ran cannot be confused
 * with the read having succeeded. A character key would prove less: by
 * the time the run loop stops, the read has taken it and the buffer
 * looks exactly as it did before.
 */
static void test_interrupt_reaches_the_handler_while_waiting(
        struct vm86_cpu *cpu)
{
    static const uint8_t program[] = {
        0xCD, 0x16,
        0xFA, 0xF4,
    };

    struct bios16_state st;

    install(cpu, &st);
    vm86_test_load(cpu, program, sizeof(program));

    cpu->flags |= VM86_IF;

    vm86_run(cpu, 20);

    vm86_expect_bool("still waiting", cpu->ip == AFTER_THE_INT, false);
    vm86_expect_u16("with no shift held", bda8(cpu, VM86_BDA_KEYBOARD_FLAGS),
                    0x00);

    bios16_key_arrived(&st, 0x2A);   /* left shift, pressed */
    vm86_raise(cpu, VM86_INT_KEYBOARD);

    vm86_run(cpu, 20);

    vm86_expect_u16("INT 09h ran while the guest was waiting",
                    bda8(cpu, VM86_BDA_KEYBOARD_FLAGS), 0x02);
    vm86_expect_bool("and the guest is still waiting for a character",
                     cpu->ip == AFTER_THE_INT, false);

    /* Now a real key, and the read that was waiting for one gets it --
     * with the shift that arrived earlier applied. */
    bios16_key_arrived(&st, 0x1E);
    vm86_raise(cpu, VM86_INT_KEYBOARD);

    vm86_expect_u16("the guest finishes", vm86_run(cpu, 40), VM86_STOP_HALT);
    vm86_expect_u16("with the shifted character", cpu->al, 'A');
    vm86_expect_u16("and the scan code", cpu->ah, 0x1E);
}

/* ------------------------------------------------------------------ */
/* Checking for a key (AH=01h)                                         */
/* ------------------------------------------------------------------ */

static void test_check_does_not_take_the_key(struct vm86_cpu *cpu)
{
    struct bios16_state st;

    install(cpu, &st);
    press(cpu, &st, 0x1E);

    cpu->ah = 0x01;
    bios16_service(cpu, &st);

    vm86_expect_u16("AL is the character", cpu->al, 'a');
    vm86_expect_u16("AH is the scan code", cpu->ah, 0x1E);
    vm86_expect_flag("ZF says there was one", cpu, VM86_ZF, false);

    vm86_expect_bool("the key is still there",
                     bda16(cpu, VM86_BDA_KB_HEAD) !=
                     bda16(cpu, VM86_BDA_KB_TAIL), true);

    /* The read that follows must find the same key, not the next one. */
    cpu->ah = 0x00;
    bios16_service(cpu, &st);

    vm86_expect_u16("and the read finds it", cpu->al, 'a');
    vm86_expect_u16("with the same scan code", cpu->ah, 0x1E);
    vm86_expect_bool("and only now is it gone",
                     bda16(cpu, VM86_BDA_KB_HEAD) !=
                     bda16(cpu, VM86_BDA_KB_TAIL), false);
}

static void test_check_on_an_empty_buffer_sets_zf(struct vm86_cpu *cpu)
{
    struct bios16_state st;

    install(cpu, &st);

    cpu->ah = 0x01;
    cpu->al = 0x5A;   /* something the caller left lying around */
    bios16_service(cpu, &st);

    vm86_expect_flag("ZF says there is nothing", cpu, VM86_ZF, true);
    vm86_expect_u16("and AX was not given a key to look like one",
                    cpu->al, 0x5A);
}

/* ------------------------------------------------------------------ */
/* Shift state (AH=02h)                                                */
/* ------------------------------------------------------------------ */

static void test_shift_state_is_read_back(struct vm86_cpu *cpu)
{
    struct bios16_state st;

    install(cpu, &st);

    cpu->ah = 0x02;
    bios16_service(cpu, &st);

    vm86_expect_u16("AL is the flags byte", cpu->al, 0x00);
    /*
     * AH is left exactly as the caller set it. This function reports the
     * shift state and nothing else -- no scan code, no character -- and
     * the task book's original claim that it filled AH was wrong. A
     * service that zeroed it would be answering a question nobody asked,
     * in the one register the caller is most likely to still be using.
     */
    vm86_expect_u16("AH is untouched", cpu->ah, 0x02);

    /* Hold the left shift, and the byte a program reads directly has to
     * agree with what the service hands back. */
    bios16_key_arrived(&st, 0x2A);
    bios16_irq(cpu, &st);

    cpu->ah = 0x02;
    bios16_service(cpu, &st);

    vm86_expect_u16("AL has the left shift bit", cpu->al, 0x02);
    vm86_expect_u16("and 0040:0017 says the same",
                    bda8(cpu, VM86_BDA_KEYBOARD_FLAGS), 0x02);
}

static void test_an_unknown_function_does_nothing(struct vm86_cpu *cpu)
{
    struct bios16_state st;

    install(cpu, &st);
    press(cpu, &st, 0x1E);

    cpu->ah = 0x99;
    cpu->al = 0x5A;
    bios16_service(cpu, &st);

    vm86_expect_u16("AX is untouched", cpu->ax, 0x995A);
    vm86_expect_bool("and the key was not eaten",
                     bda16(cpu, VM86_BDA_KB_HEAD) !=
                     bda16(cpu, VM86_BDA_KB_TAIL), true);
}

/* ------------------------------------------------------------------ */
/* The ring buffer                                                     */
/* ------------------------------------------------------------------ */

/*
 * Fifteen scan codes that each produce a character.
 *
 * Which fifteen does not matter; that each of them produces a character
 * does. A modifier is not buffered, so a run of raw scan codes would
 * quietly push fourteen keys instead of fifteen and the arithmetic under
 * test would never be reached -- the first version of the wrap case
 * below used 0x1E + i and walked straight through the left shift on the
 * way.
 */
static const uint8_t fill_keys[15] = {
    0x1E, 0x30, 0x2E, 0x20, 0x12,   /* a b c d e */
    0x21, 0x22, 0x23, 0x17, 0x24,   /* f g h i j */
    0x25, 0x26, 0x32, 0x31, 0x18,   /* k l m n o */
};

/*
 * The buffer holds fifteen keystrokes, not sixteen: head == tail is the
 * empty state, so one of the sixteen slots is spent telling empty from
 * full. Filling it and watching the pointers is what proves the
 * arithmetic, because a ring that used all sixteen would pass every test
 * that reads one key at a time.
 */
static void test_the_buffer_fills_and_then_refuses(struct vm86_cpu *cpu)
{
    struct bios16_state st;

    install(cpu, &st);

    for (unsigned i = 0; i < 15; i++)
        press(cpu, &st, fill_keys[i]);

    vm86_expect_u16("fifteen in, head still at the start",
                    bda16(cpu, VM86_BDA_KB_HEAD), VM86_BDA_KB_EMPTY);
    vm86_expect_u16("and the tail is one short of the end",
                    bda16(cpu, VM86_BDA_KB_TAIL),
                    VM86_BDA_KB_BUFFER_END - 2);

    /* The sixteenth has nowhere to go. */
    press(cpu, &st, 0x19);   /* 'p' */

    vm86_expect_u16("the tail did not move",
                    bda16(cpu, VM86_BDA_KB_TAIL),
                    VM86_BDA_KB_BUFFER_END - 2);
    vm86_expect_u16("and nothing was written over the head",
                    bda16(cpu, VM86_BDA_KB_HEAD), VM86_BDA_KB_EMPTY);
    /*
     * And nothing else moved. This assertion used to read the other way
     * round: an earlier task book said a full buffer sets bit 7 of
     * 0040:0071, and this suite pinned it. That byte is the Ctrl-Break
     * flag -- programs poll it to find out that the user hit Break -- and
     * the firmware sets nothing there when a key is dropped. See the note
     * on VM86_BDA_CTRL_BREAK in firmware.h.
     */
    vm86_expect_u16("no flag was set anywhere",
                    (uint16_t)(bda8(cpu, VM86_BDA_CTRL_BREAK) & 0x80u), 0x00u);
}

static void test_the_buffer_wraps(struct vm86_cpu *cpu)
{
    struct bios16_state st;

    install(cpu, &st);

    for (unsigned i = 0; i < 15; i++)
        press(cpu, &st, fill_keys[i]);

    /* Make room, then watch the tail cross the end of the ring. */
    cpu->ah = 0x00;
    bios16_service(cpu, &st);

    vm86_expect_u16("the head moved up by one entry",
                    bda16(cpu, VM86_BDA_KB_HEAD), VM86_BDA_KB_BUFFER + 2);

    press(cpu, &st, 0x1E);

    vm86_expect_u16("the tail wrapped to the start of the ring",
                    bda16(cpu, VM86_BDA_KB_TAIL), VM86_BDA_KB_EMPTY);
    vm86_expect_u16("the entry landed in the last slot",
                    vm86_mem_read16(cpu->mem,
                                    bda(VM86_BDA_KB_BUFFER_END - 2)),
                    (uint16_t)((0x1Eu << 8) | 'a'));
}

static void test_the_queue_is_read_in_order(struct vm86_cpu *cpu)
{
    struct bios16_state st;

    install(cpu, &st);
    press(cpu, &st, 0x1E);   /* a */
    press(cpu, &st, 0x30);   /* b */
    press(cpu, &st, 0x2E);   /* c */

    static const char *want = "abc";

    for (unsigned i = 0; i < 3; i++) {
        cpu->ah = 0x00;
        bios16_service(cpu, &st);

        char detail[32];
        snprintf(detail, sizeof(detail), "character %u", i);
        vm86_expect_u16(detail, cpu->al, (uint8_t)want[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Translation                                                         */
/* ------------------------------------------------------------------ */

static void test_letters_come_out_lower_case(struct vm86_cpu *cpu)
{
    struct bios16_state st;

    install(cpu, &st);
    press(cpu, &st, 0x1E);   /* a */
    press(cpu, &st, 0x2C);   /* z */

    cpu->ah = 0x00;
    bios16_service(cpu, &st);
    vm86_expect_u16("a", cpu->al, 'a');
    vm86_expect_u16("a, scan code", cpu->ah, 0x1E);

    cpu->ah = 0x00;
    bios16_service(cpu, &st);
    vm86_expect_u16("z", cpu->al, 'z');
    vm86_expect_u16("z, scan code", cpu->ah, 0x2C);
}

static void test_shift_and_caps_lock(struct vm86_cpu *cpu)
{
    struct bios16_state st;

    install(cpu, &st);

    press(cpu, &st, 0x1E);   /* a */
    press(cpu, &st, 0x2A);   /* left shift down */
    press(cpu, &st, 0x30);   /* b */
    press(cpu, &st, 0xAA);   /* left shift up */
    press(cpu, &st, 0x2E);   /* c */
    press(cpu, &st, 0x3A);   /* caps lock down */
    press(cpu, &st, 0xBA);   /* caps lock up -- must not undo it */
    press(cpu, &st, 0x20);   /* d */

    cpu->ah = 0x00; bios16_service(cpu, &st);
    vm86_expect_u16("plain a", cpu->al, 'a');

    cpu->ah = 0x00; bios16_service(cpu, &st);
    vm86_expect_u16("shifted b", cpu->al, 'B');

    cpu->ah = 0x00; bios16_service(cpu, &st);
    vm86_expect_u16("plain c, shift released", cpu->al, 'c');

    cpu->ah = 0x00; bios16_service(cpu, &st);
    vm86_expect_u16("caps lock makes d upper case", cpu->al, 'D');

    vm86_expect_u16("and 0040:0017 has the caps lock bit",
                    (uint16_t)(bda8(cpu, VM86_BDA_KEYBOARD_FLAGS) & 0x40u),
                    0x40u);
    vm86_expect_u16("with the shift bit clear",
                    (uint16_t)(bda8(cpu, VM86_BDA_KEYBOARD_FLAGS) & 0x03u),
                    0x00u);
}

static void test_shift_flags_track_press_and_release(struct vm86_cpu *cpu)
{
    struct bios16_state st;

    install(cpu, &st);

    press(cpu, &st, 0x2A);   /* left shift down */
    vm86_expect_u16("left shift is bit 1",
                    bda8(cpu, VM86_BDA_KEYBOARD_FLAGS), 0x02);

    press(cpu, &st, 0x36);   /* right shift down */
    vm86_expect_u16("right shift is bit 0",
                    bda8(cpu, VM86_BDA_KEYBOARD_FLAGS), 0x03);

    press(cpu, &st, 0xAA);   /* left shift up */
    vm86_expect_u16("and a release clears it again",
                    bda8(cpu, VM86_BDA_KEYBOARD_FLAGS), 0x01);

    press(cpu, &st, 0x1D);   /* ctrl down */
    vm86_expect_u16("ctrl", bda8(cpu, VM86_BDA_KEYBOARD_FLAGS), 0x05);
    vm86_expect_u16("ctrl is the left one in the second byte",
                    bda8(cpu, VM86_BDA_KEYBOARD_FLAGS2), 0x01);

    press(cpu, &st, 0x38);   /* alt down */
    vm86_expect_u16("alt", bda8(cpu, VM86_BDA_KEYBOARD_FLAGS), 0x0D);
    vm86_expect_u16("alt too",
                    bda8(cpu, VM86_BDA_KEYBOARD_FLAGS2), 0x03);

    press(cpu, &st, 0x9D);   /* ctrl up */
    press(cpu, &st, 0xB8);   /* alt up */
    vm86_expect_u16("both released", bda8(cpu, VM86_BDA_KEYBOARD_FLAGS), 0x01);
    vm86_expect_u16("in both bytes",
                    bda8(cpu, VM86_BDA_KEYBOARD_FLAGS2), 0x00);
}

static void test_a_shift_key_is_not_a_keystroke(struct vm86_cpu *cpu)
{
    struct bios16_state st;

    install(cpu, &st);

    press(cpu, &st, 0x2A);   /* left shift down */
    press(cpu, &st, 0x1D);   /* ctrl down */
    press(cpu, &st, 0x3A);   /* caps lock */
    press(cpu, &st, 0x45);   /* num lock */

    vm86_expect_bool("nothing reached the buffer",
                     bda16(cpu, VM86_BDA_KB_HEAD) !=
                     bda16(cpu, VM86_BDA_KB_TAIL), false);

    cpu->ah = 0x01;
    bios16_service(cpu, &st);
    vm86_expect_flag("and a check agrees", cpu, VM86_ZF, true);
}

static void test_enter_backspace_tab_escape_space(struct vm86_cpu *cpu)
{
    struct bios16_state st;

    install(cpu, &st);
    press(cpu, &st, 0x1C);   /* enter */
    press(cpu, &st, 0x0E);   /* backspace */
    press(cpu, &st, 0x0F);   /* tab */
    press(cpu, &st, 0x01);   /* escape */
    press(cpu, &st, 0x39);   /* space */

    static const struct { uint8_t sc; uint8_t ch; } want[] = {
        { 0x1C, 0x0D }, { 0x0E, 0x08 }, { 0x0F, 0x09 },
        { 0x01, 0x1B }, { 0x39, ' '  },
    };

    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        cpu->ah = 0x00;
        bios16_service(cpu, &st);

        char detail[48];
        snprintf(detail, sizeof(detail), "key %u scan code 0x%02X",
                 i, want[i].sc);

        vm86_expect_u16(detail, cpu->al, want[i].ch);
        vm86_expect_u16("its scan code", cpu->ah, want[i].sc);
    }
}

static void test_digits_and_control_characters(struct vm86_cpu *cpu)
{
    struct bios16_state st;

    install(cpu, &st);
    press(cpu, &st, 0x02);   /* 1 */
    press(cpu, &st, 0x0B);   /* 0 */
    press(cpu, &st, 0x0C);   /* - */

    cpu->ah = 0x00; bios16_service(cpu, &st);
    vm86_expect_u16("1", cpu->al, '1');

    cpu->ah = 0x00; bios16_service(cpu, &st);
    vm86_expect_u16("0", cpu->al, '0');

    cpu->ah = 0x00; bios16_service(cpu, &st);
    vm86_expect_u16("-", cpu->al, '-');

    /* Ctrl-A is 0x01, and it is the same key as plain A. */
    press(cpu, &st, 0x1D);   /* ctrl down */
    press(cpu, &st, 0x1E);   /* a */
    press(cpu, &st, 0x9D);   /* ctrl up */

    cpu->ah = 0x00; bios16_service(cpu, &st);
    vm86_expect_u16("ctrl-a", cpu->al, 0x01);
    vm86_expect_u16("still scan code 0x1E", cpu->ah, 0x1E);
}

/*
 * Alt is a third answer, different from the other two, and it has to be
 * pinned separately.
 *
 * Shift changes the character and still buffers one. A modifier key is
 * not buffered at all. Alt buffers the key with *no* character -- the
 * scan code is the whole answer, the same shape as an arrow key -- and a
 * program that wanted to know Alt was down reads it from 0040:0017.
 *
 * Without this case the Alt branch could return the unshifted character
 * and every other case in the file would still pass.
 */
static void test_alt_is_a_scan_code_with_no_character(struct vm86_cpu *cpu)
{
    struct bios16_state st;

    install(cpu, &st);

    press(cpu, &st, 0x38);   /* alt down */
    press(cpu, &st, 0x1E);   /* a */
    press(cpu, &st, 0xB8);   /* alt up */

    cpu->ah = 0x00;
    bios16_service(cpu, &st);

    vm86_expect_u16("alt-a has no character", cpu->al, 0x00);
    vm86_expect_u16("but is still reported", cpu->ah, 0x1E);

    /* And the key really was buffered, unlike a modifier. */
    vm86_expect_u16("the queue took one entry",
                    bda16(cpu, VM86_BDA_KB_HEAD),
                    (uint16_t)(VM86_BDA_KB_EMPTY + 2));
}

/*
 * Two cells that differ from what is usually remembered. They are in
 * here on their own because the version everyone carries around is the
 * wrong one, and a test that only checked the memorable answer would
 * have agreed with the mistake.
 */
static void test_shift_tab_and_ctrl_space(struct vm86_cpu *cpu)
{
    struct bios16_state st;

    install(cpu, &st);

    press(cpu, &st, 0x2A);   /* shift down */
    press(cpu, &st, 0x0F);   /* tab */
    press(cpu, &st, 0xAA);   /* shift up */
    press(cpu, &st, 0x1D);   /* ctrl down */
    press(cpu, &st, 0x39);   /* space */
    press(cpu, &st, 0x9D);   /* ctrl up */

    cpu->ah = 0x00;
    bios16_service(cpu, &st);
    vm86_expect_u16("shift-tab has no character", cpu->al, 0x00);
    vm86_expect_u16("but is still the tab key", cpu->ah, 0x0F);

    cpu->ah = 0x00;
    bios16_service(cpu, &st);
    vm86_expect_u16("ctrl-space is a space, not a NUL", cpu->al, ' ');
    vm86_expect_u16("and says which key it was", cpu->ah, 0x39);
}

/*
 * The arrow keys have no character, and that is not a failure to
 * translate: the firmware sends the scan code with a zero character, and
 * a program tells an arrow from a digit by exactly that zero. A service
 * that invented a character here would be unusable for cursor movement.
 */
static void test_arrow_keys_have_no_character(struct vm86_cpu *cpu)
{
    struct bios16_state st;

    install(cpu, &st);

    static const uint8_t arrows[] = { 0x48, 0x50, 0x4B, 0x4D };

    for (unsigned i = 0; i < sizeof(arrows); i++)
        press(cpu, &st, arrows[i]);

    for (unsigned i = 0; i < sizeof(arrows); i++) {
        cpu->ah = 0x00;
        bios16_service(cpu, &st);

        char detail[48];
        snprintf(detail, sizeof(detail), "arrow 0x%02X", arrows[i]);

        vm86_expect_u16("no character", cpu->al, 0x00);
        vm86_expect_u16(detail, cpu->ah, arrows[i]);
    }
}

static void test_a_break_code_is_not_a_keystroke(struct vm86_cpu *cpu)
{
    struct bios16_state st;

    install(cpu, &st);
    press(cpu, &st, 0x9E);   /* a, released */

    vm86_expect_bool("nothing was buffered for a key coming up",
                     bda16(cpu, VM86_BDA_KB_HEAD) !=
                     bda16(cpu, VM86_BDA_KB_TAIL), false);
}

static void test_num_lock_turns_the_keypad_into_digits(struct vm86_cpu *cpu)
{
    struct bios16_state st;

    install(cpu, &st);

    press(cpu, &st, 0x48);   /* up arrow */

    cpu->ah = 0x00; bios16_service(cpu, &st);
    vm86_expect_u16("num lock off: no character", cpu->al, 0x00);
    vm86_expect_u16("num lock off: the scan code", cpu->ah, 0x48);

    press(cpu, &st, 0x45);   /* num lock on */
    press(cpu, &st, 0x48);

    cpu->ah = 0x00; bios16_service(cpu, &st);
    vm86_expect_u16("num lock on: the digit", cpu->al, '8');
    vm86_expect_u16("num lock on: the same scan code", cpu->ah, 0x48);
}

static void test_an_empty_controller_does_not_translate(struct vm86_cpu *cpu)
{
    struct bios16_state st;

    install(cpu, &st);

    /*
     * A stray byte in the output register with the controller saying
     * there is nothing in it -- a spurious IRQ1, which real hardware
     * produces and which firmware has to survive.
     *
     * The byte is a real key's scan code on purpose. Leaving the
     * register at zero would make this case pass against an
     * implementation that had forgotten to check the status bit at all,
     * because a zero scan code is not a key and would translate to
     * nothing either way.
     */
    st.controller_output = 0x1E;   /* 'a' */
    st.controller_full   = false;

    bios16_irq(cpu, &st);

    vm86_expect_bool("nothing was buffered",
                     bda16(cpu, VM86_BDA_KB_HEAD) !=
                     bda16(cpu, VM86_BDA_KB_TAIL), false);
}

/* ------------------------------------------------------------------ */

static const struct vm86_test tests[] = {
    { "read takes a key",                   test_read_takes_a_key },
    { "read leaves the other registers",    test_read_does_not_touch_the_other_registers },
    { "blocking read waits for a key",      test_blocking_read_waits_for_a_key },
    { "interrupt arrives while waiting",    test_interrupt_reaches_the_handler_while_waiting },
    { "check does not take the key",        test_check_does_not_take_the_key },
    { "check on empty sets ZF",             test_check_on_an_empty_buffer_sets_zf },
    { "shift state is read back",           test_shift_state_is_read_back },
    { "unknown function does nothing",      test_an_unknown_function_does_nothing },
    { "buffer fills and refuses",           test_the_buffer_fills_and_then_refuses },
    { "buffer wraps",                       test_the_buffer_wraps },
    { "queue is read in order",             test_the_queue_is_read_in_order },
    { "letters are lower case",             test_letters_come_out_lower_case },
    { "shift and caps lock",                test_shift_and_caps_lock },
    { "shift flags track press and release", test_shift_flags_track_press_and_release },
    { "shift keys are not keystrokes",      test_a_shift_key_is_not_a_keystroke },
    { "enter backspace tab escape space",   test_enter_backspace_tab_escape_space },
    { "digits and control characters",      test_digits_and_control_characters },
    { "alt is a scan code with no character", test_alt_is_a_scan_code_with_no_character },
    { "shift-tab and ctrl-space",           test_shift_tab_and_ctrl_space },
    { "arrows have no character",           test_arrow_keys_have_no_character },
    { "a break code is not a keystroke",    test_a_break_code_is_not_a_keystroke },
    { "num lock turns the keypad into digits", test_num_lock_turns_the_keypad_into_digits },
    { "an empty controller does not translate", test_an_empty_controller_does_not_translate },
};

VM86_TEST_MAIN("bios16", tests)
