/*
 * INT 21h: the DOS function dispatcher.
 *
 * ---------------------------------------------------------------------
 * Where the expected values come from
 *
 * Every number asserted below is transcribed from docs/dos-refs-dos.md
 * section 9, not read back out of `dos/dos/int21.c`. The distinction is
 * the one this whole directory is built on: if the dispatcher put the
 * version at 0x031E and the test asked the dispatcher what the version
 * was, both would be wrong together and the run would be green.
 *
 * So the version is written as the arithmetic that produces it -- 3.30,
 * AL the major and AH the minor, so 0x1E03 -- and the drive letter is
 * written as `'F' - 'A'` rather than as 5. Where a value is a decision
 * this project made rather than a fact somebody looked up, the test says
 * so and the reference says why: the drive is F: because the rest of the
 * machine calls its storage F:.
 *
 * `VM86_DOS_VERSION` and the other constants in int21.h are deliberately
 * not used in the assertions. They are the interface, and an interface
 * that is asserted against itself is an interface nobody is checking.
 *
 * ---------------------------------------------------------------------
 * Two kinds of test, and why both
 *
 * Most of these call `int21_service` directly with the registers set up
 * by hand. That is the only way to see the answers that are *not* on the
 * screen -- the carry flag, AH being left alone, the segment and offset
 * 2Fh hands back -- and it is a millisecond per case.
 *
 * The last one runs `dos/corpus/dos/int21.asm` on the full replay machine
 * and reads the text page. That is the acceptance: a .COM that asks DOS
 * for things and prints what it was told, on a machine with a real
 * interrupt path rather than a direct call into the dispatcher.
 */
#include "harness.h"

#include <stdio.h>
#include <string.h>

#include <vm86/dos.h>
#include <vm86/host.h>
#include <vm86/int21.h>
#include <vm86/mem.h>

#include "../bios/bios10.h"
#include "../bios/bios1a.h"

#include "../corpus/bios/replay.h"

/* The corpus is embedded by the build; see the DOS_CORPUS_SRC rules in
 * dos/Makefile. Declared by hand because the generated file is a .c and
 * not a header. */
#define DOS_IMAGE(sym)                            \
    extern const unsigned char sym[];             \
    extern const unsigned long sym##_size;

DOS_IMAGE(corpus_int21)

/* ------------------------------------------------------------------ */
/* A machine to call the dispatcher on                                 */
/* ------------------------------------------------------------------ */

/*
 * Where this suite puts a program. The segment is the caller's to choose
 * -- the loader's header says so -- and a value that is neither zero nor
 * the one test_dos.c uses is deliberate: a dispatcher that answered with
 * a segment it made up rather than the one it was given would pass a test
 * whose expected segment was zero.
 */
#define TEST_PROGRAM_SEGMENT      0x1000u
#define TEST_PROGRAM_LINEAR       ((uint32_t)TEST_PROGRAM_SEGMENT << 4)
#define TEST_ENVIRONMENT_SEGMENT  0x0F00u
#define TEST_PARENT_SEGMENT       0x1234u
#define TEST_PATH                 "F:\\INT21.COM"

/* The vector the program writes with 25h and reads back with 35h. Above
 * the ones this machine uses, so nothing that matters is disturbed -- and
 * it is written to a value that is deliberately not a valid handler,
 * which is fine because nothing calls it. */
#define TEST_SPARE_VECTOR         0x60u
#define TEST_SPARE_OFFSET         0x1234u

/*
 * The firmware, the video service and the DOS dispatcher.
 *
 * Everything the dispatcher needs and nothing else. The disk and the
 * keyboard are absent on purpose: a test that fails because a device it
 * never asked for was missing would send the reader to the wrong file.
 */
static void power_on(struct vm86_cpu *cpu, struct bios10_state *video,
                     struct int21_state *dos, uint16_t psp_segment)
{
    vm86_mem_clear(cpu->mem);
    vm86_reset(cpu, cpu->mem);

    vm86_clear_services();
    vm86_install_firmware(cpu);

    bios10_reset(video);

    int21_reset(dos, video, psp_segment);

    vm86_register_service(VM86_INT_VIDEO,     bios10_service, video);
    vm86_register_service(VM86_INT_DOS,       int21_service, dos);
    vm86_register_service(VM86_INT_TERMINATE, int21_terminate_service, NULL);
}

/* A dispatcher with a program loaded behind it, which is the ordinary
 * case: the PSP segment is what the default DTA lives in. */
static void power_on_with_a_program(struct vm86_cpu *cpu,
                                    struct bios10_state *video,
                                    struct int21_state *dos)
{
    power_on(cpu, video, dos, TEST_PROGRAM_SEGMENT);
}

/* ------------------------------------------------------------------ */
/* Reading the page back                                               */
/* ------------------------------------------------------------------ */

static uint32_t cell(unsigned row, unsigned col)
{
    return VM86_TEXT_BASE + (row * 80u + col) * 2u;
}

/* ------------------------------------------------------------------ */
/* The version                                                         */
/* ------------------------------------------------------------------ */

/*
 * AH=30h. AL is the major version and AH the minor, which is the fact
 * this test exists to pin: 3.30 is 0x1E03, and 0x031E is the same two
 * bytes in the other order. Asserting only the word would pass on a
 * dispatcher that got them the wrong way round, so the halves are
 * asserted separately as well.
 *
 * The version itself is docs/dos-refs-dos.md section 6's decision -- 3.30
 * is the lowest claim that almost every program of the era accepts -- and
 * the numbers below are that decision written out: 3 in AL, 30 in AH.
 */
static void test_version(struct vm86_cpu *cpu)
{
    struct bios10_state video;
    struct int21_state  dos;

    power_on_with_a_program(cpu, &video, &dos);

    /* AH holds the function number and AL something neither field could
     * coincide with: the minor version this machine reports is 30, which
     * is 0x1E, and the function number is 0x30 -- so a dispatcher that
     * put the function number back in AH would be caught rather than
     * passing by luck. */
    cpu->ax = 0x30FF;

    int21_service(cpu, &dos);

    vm86_expect_u16("AL, the major version", cpu->al, 3);
    vm86_expect_u16("AH, the minor version", cpu->ah, 30);
    vm86_expect_u16("the whole word",        cpu->ax, (30u << 8) | 3u);

    vm86_expect_u16("BX, the OEM number", cpu->bx, 0);
    vm86_expect_u16("CX, the serial",     cpu->cx, 0);

    vm86_expect_flag("CF", cpu, VM86_CF, false);
}

/* ------------------------------------------------------------------ */
/* AH=02h, output a character                                          */
/* ------------------------------------------------------------------ */

/*
 * The character lands on the page and the cursor moves past it. Both
 * halves: a version that wrote the character but never advanced would
 * print every character of a string in the same cell and leave a page
 * with one letter on it, and a check that only looked at the first cell
 * would pass.
 */
static void test_output_char(struct vm86_cpu *cpu)
{
    struct bios10_state video;
    struct int21_state  dos;

    power_on_with_a_program(cpu, &video, &dos);

    cpu->dl = 'A';
    cpu->ah = 0x02;

    int21_service(cpu, &dos);

    vm86_expect_mem8("the cell at the cursor", cpu, cell(0, 0), 'A');
    vm86_expect_mem8("the cursor moved on",    cpu, cell(0, 1), 0x00);

    /* The address is the one the *video* service keeps, read back out of
     * the data area a program would read -- not out of the state struct,
     * which is the thing under test's neighbour rather than the thing
     * under test. */
    vm86_expect_mem16("the cursor in the data area", cpu,
                      ((uint32_t)VM86_BDA_SEGMENT << 4) + VM86_BDA_CURSOR,
                      (0u << 8) | 1u);

    /* AL is left holding the character, which is what DOS 2.1 through 7.0
     * do even though the documentation says this call returns nothing. */
    vm86_expect_u16("AL", cpu->al, 'A');

    vm86_expect_flag("CF", cpu, VM86_CF, false);
}

/* ------------------------------------------------------------------ */
/* AH=09h, output a string                                             */
/* ------------------------------------------------------------------ */

/*
 * The string is printed and the terminator is not.
 *
 * "$" is 0x24 and it is what ends the walk, so a machine that printed the
 * whole buffer including the terminator would put a '$' on the screen; a
 * machine that stopped one byte early would drop the last character. The
 * assertion on the cell *after* the last character covers both.
 */
static void test_output_string(struct vm86_cpu *cpu)
{
    struct bios10_state video;
    struct int21_state  dos;
    static const char   text[] = "AB$";
    size_t              i;

    power_on_with_a_program(cpu, &video, &dos);

    for (i = 0; i < sizeof text; i++)
        vm86_mem_write8(cpu->mem, TEST_PROGRAM_LINEAR + 0x200u + i,
                        (uint8_t)text[i]);

    cpu->ds = TEST_PROGRAM_SEGMENT;
    cpu->dx = 0x0200;
    cpu->ah = 0x09;

    int21_service(cpu, &dos);

    vm86_expect_mem8("the first character",  cpu, cell(0, 0), 'A');
    vm86_expect_mem8("the second character", cpu, cell(0, 1), 'B');

    /* The terminator is not printed, and nothing after it is either --
     * guest memory there is zeroes, which would show up as two NUL cells
     * if the walk got them out of the wrong end of the buffer. */
    vm86_expect_mem8("the terminator was not printed", cpu, cell(0, 2), 0x00);
    vm86_expect_mem8("and nor was what follows it",    cpu, cell(0, 3), 0x00);

    vm86_expect_u16("AL, the byte the walk stopped on", cpu->al, '$');

    vm86_expect_flag("CF", cpu, VM86_CF, false);
}

/*
 * A string that never ends stops at the end of memory instead of hanging.
 *
 * The address is past the end of the guest's megabyte, so the very first
 * byte has nothing behind it. That is the second of the two bounds
 * int21.c describes, and it is the cheap one to test: an address with
 * nothing behind it reads as 0xFF, which is not '$', so an unbounded walk
 * from here would never terminate -- and this test is the thing that
 * would stop returning rather than the thing that reports a failure.
 *
 * AL is the last byte examined, which is the same rule DOS's own loop
 * ends by -- it stops on the byte that terminated it and leaves that in
 * AL. Here the byte that stopped it is the 0xFF with nothing behind it,
 * and 0xFF is not a value a terminated string can leave.
 */
static void test_a_string_that_never_ends(struct vm86_cpu *cpu)
{
    struct bios10_state video;
    struct int21_state  dos;

    power_on_with_a_program(cpu, &video, &dos);

    cpu->ds = 0xFFFF;      /* linear 0xFFFF0, then one more paragraph */
    cpu->dx = 0xFFF0;      /* linear 0x10FFE0 -- past the megabyte    */
    cpu->ah = 0x09;

    int21_service(cpu, &dos);

    vm86_expect_u16("AL, the byte that stopped the walk", cpu->al, 0xFF);
    vm86_expect_mem8("and nothing was printed", cpu, cell(0, 0), 0x00);
    vm86_expect_flag("CF", cpu, VM86_CF, false);
}

/* ------------------------------------------------------------------ */
/* AH=25h and AH=35h, the interrupt vectors                            */
/* ------------------------------------------------------------------ */

/*
 * Read a vector the firmware installed.
 *
 * The expected address is derived from the *interpreter's* map and not
 * from anything the DOS layer decides: every vector gets a four-byte stub
 * in segment 0xF000, laid out four bytes apart from offset zero, so
 * vector 0x21 is at 0xF000:0084. That is the whole arithmetic, and it is
 * written here rather than in a constant because a test that read the
 * constant would be asserting the map against itself.
 */
static void test_read_a_stub_vector(struct vm86_cpu *cpu)
{
    struct bios10_state video;
    struct int21_state  dos;

    power_on_with_a_program(cpu, &video, &dos);

    cpu->al = 0x21;
    cpu->ah = 0x35;

    int21_service(cpu, &dos);

    vm86_expect_u16("ES, the stub's segment", cpu->es, 0xF000);
    vm86_expect_u16("BX, the stub's offset",  cpu->bx, 0x21 * 4);

    vm86_expect_flag("CF", cpu, VM86_CF, false);
}

/*
 * Write a vector with 25h and read it back with 35h.
 *
 * A pair, because either call on its own is satisfied by a function that
 * ignores its arguments: 35h alone is satisfied by anything that returns
 * the firmware's stub, and 25h alone by a function that writes nothing at
 * all. The value written is read out of the vector table directly as well
 * as through 35h, so a 35h that invented an answer from its own state
 * would be caught.
 */
static void test_write_and_read_a_vector(struct vm86_cpu *cpu)
{
    struct bios10_state video;
    struct int21_state  dos;
    uint32_t            entry = TEST_SPARE_VECTOR * 4u;

    power_on_with_a_program(cpu, &video, &dos);

    /* The vector still holds the firmware's stub -- four bytes per vector
     * from a base of zero, in segment F000 -- so a 25h that did nothing
     * would leave that here and the assertions below would see it. */
    vm86_expect_mem16("the stub's offset before the write", cpu, entry,
                      TEST_SPARE_VECTOR * 4u);
    vm86_expect_mem16("and its segment", cpu, entry + 2u, 0xF000);

    cpu->ds = TEST_PROGRAM_SEGMENT;
    cpu->dx = TEST_SPARE_OFFSET;
    cpu->al = TEST_SPARE_VECTOR;
    cpu->ah = 0x25;

    int21_service(cpu, &dos);

    vm86_expect_mem16("the offset in the table",  cpu, entry,     0x1234);
    vm86_expect_mem16("the segment in the table", cpu, entry + 2u, 0x1000);

    /* And the same thing asked for the documented way. */
    cpu->al = TEST_SPARE_VECTOR;
    cpu->ah = 0x35;

    int21_service(cpu, &dos);

    vm86_expect_u16("ES from 35h", cpu->es, TEST_PROGRAM_SEGMENT);
    vm86_expect_u16("BX from 35h", cpu->bx, TEST_SPARE_OFFSET);

    vm86_expect_flag("CF", cpu, VM86_CF, false);
}

/* ------------------------------------------------------------------ */
/* AH=1Ah and AH=2Fh, the transfer address                             */
/* ------------------------------------------------------------------ */

/*
 * The pair given to 1Ah is what 2Fh gives back, as a segment and an
 * offset.
 *
 * The segment is a paragraph whose *linear* address would be a different
 * number, so a dispatcher that converted to linear on the way in and back
 * on the way out would be caught -- which is the mistake the state's two
 * fields exist to avoid.
 */
static void test_the_transfer_address(struct vm86_cpu *cpu)
{
    struct bios10_state video;
    struct int21_state  dos;

    power_on_with_a_program(cpu, &video, &dos);

    cpu->ds = 0x2345;
    cpu->dx = 0x6789;
    cpu->ah = 0x1A;

    int21_service(cpu, &dos);

    cpu->ah = 0x2F;

    int21_service(cpu, &dos);

    vm86_expect_u16("ES, the segment given", cpu->es, 0x2345);
    vm86_expect_u16("BX, the offset given",  cpu->bx, 0x6789);

    vm86_expect_flag("CF", cpu, VM86_CF, false);
}

/*
 * A program that has not asked for a transfer address has DOS's default,
 * which is its own command tail: PSP:0080h. That is docs/dos-refs-dos.md
 * section 4's point that 80h is both the tail's length and the DTA --
 * which is why a program that reads through the DTA before setting one
 * destroys its own command line.
 */
static void test_the_default_transfer_address(struct vm86_cpu *cpu)
{
    struct bios10_state video;
    struct int21_state  dos;

    power_on_with_a_program(cpu, &video, &dos);

    cpu->ah = 0x2F;

    int21_service(cpu, &dos);

    vm86_expect_u16("ES, the program's own segment", cpu->es,
                    TEST_PROGRAM_SEGMENT);
    vm86_expect_u16("BX, the command tail",          cpu->bx, 0x0080);
}

/*
 * A machine with no program loaded answers with a transfer address of
 * zero, which is an address with nothing behind it.
 *
 * This is not a state a real machine can be in, and it is reachable here
 * because a host test can build a machine and load nothing into it. The
 * assertion exists so that "the default DTA" is a number that comes from
 * the loader rather than a constant the dispatcher carries: a dispatcher
 * with 0x0080 compiled in and no segment would give a different answer
 * here, and one that always said PSP:0080 would give the same answer
 * whether or not there was a PSP.
 */
static void test_the_default_transfer_address_without_a_program(
    struct vm86_cpu *cpu)
{
    struct bios10_state video;
    struct int21_state  dos;

    power_on(cpu, &video, &dos, 0u);

    cpu->ah = 0x2F;

    int21_service(cpu, &dos);

    vm86_expect_u16("ES", cpu->es, 0x0000);
    vm86_expect_u16("BX", cpu->bx, 0x0000);
}

/* ------------------------------------------------------------------ */
/* The drive calls                                                     */
/* ------------------------------------------------------------------ */

/*
 * AH=19h: the current drive, counting from zero.
 *
 * Zero is A:, so F: is 5 -- and the value is written as the arithmetic
 * because a machine that answered 6 (counting from one) would send every
 * program to the wrong drive, and 5 and 6 are one keystroke apart in a
 * test.
 */
static void test_which_drive(struct vm86_cpu *cpu)
{
    struct bios10_state video;
    struct int21_state  dos;

    power_on_with_a_program(cpu, &video, &dos);

    cpu->al = 0xFF;
    cpu->ah = 0x19;

    int21_service(cpu, &dos);

    vm86_expect_u16("AL, the drive letter from zero", cpu->al, 'F' - 'A');
    vm86_expect_flag("CF", cpu, VM86_CF, false);
}

/*
 * AH=0Eh: selecting the drive that exists reports the count, which is 6
 * and not 1.
 *
 * DOS's count is the range the numbering covers rather than the media
 * that are present -- a machine whose highest drive is C: answers 3
 * although there is no B: -- so a machine whose only drive is F: answers
 * 6. See docs/dos-refs-dos.md section 9.
 */
static void test_select_the_drive(struct vm86_cpu *cpu)
{
    struct bios10_state video;
    struct int21_state  dos;

    power_on_with_a_program(cpu, &video, &dos);

    cpu->dl = (uint8_t)('F' - 'A');
    cpu->al = 0xFF;
    cpu->ah = 0x0E;

    int21_service(cpu, &dos);

    vm86_expect_u16("AL, the number of drives", cpu->al, ('F' - 'A') + 1u);
    vm86_expect_flag("CF", cpu, VM86_CF, false);
}

/*
 * AH=0Eh for a drive that is not there fails, with the documented error
 * code in AX and the carry flag up.
 *
 * This is the assertion that catches a function which ignores DL: a
 * version that always said yes would report success here, and it would
 * report exactly the same thing for the drive that does exist -- so the
 * two tests together are the only thing that pins the argument being
 * read at all.
 *
 * 0Fh is "invalid drive", from section 9's table.
 */
static void test_select_a_drive_that_is_not_there(struct vm86_cpu *cpu)
{
    struct bios10_state video;
    struct int21_state  dos;

    power_on_with_a_program(cpu, &video, &dos);

    cpu->dl = 0;          /* A:, which this machine has not */
    cpu->al = 0xFF;
    cpu->ah = 0x0E;

    int21_service(cpu, &dos);

    vm86_expect_u16("AX, the error code", cpu->ax, 0x000F);
    vm86_expect_flag("CF", cpu, VM86_CF, true);
}

/* ------------------------------------------------------------------ */
/* A function this machine has not got                                 */
/* ------------------------------------------------------------------ */

/*
 * AL goes to zero, the carry flag goes up, and AH is left alone.
 *
 * AH is the load-bearing part. The documented behaviour is a byte of zero
 * in AL, and an early FreeDOS returned an error code in AX instead --
 * which broke 4DOS, because a program that sees CF set reads AX and 1 is
 * not the same thing to it as 0. Asserting the whole of AX is what tells
 * the two apart; asserting AL alone would pass on either.
 *
 * The function number is one DOS 5 and later implement and this machine
 * does not. That is the point: the number is not nonsense, it is a real
 * call from a version this machine does not claim to be.
 */
static void test_a_function_that_is_not_implemented(struct vm86_cpu *cpu)
{
    struct bios10_state video;
    struct int21_state  dos;

    power_on_with_a_program(cpu, &video, &dos);

    cpu->ax = 0x55AA;   /* AH = 55h, AL = something that must go */

    int21_service(cpu, &dos);

    vm86_expect_u16("AX", cpu->ax, 0x5500);
    vm86_expect_u16("AH, the function number", cpu->ah, 0x55);
    vm86_expect_u16("AL, a byte of zero",      cpu->al, 0x00);

    vm86_expect_flag("CF", cpu, VM86_CF, true);
}

/*
 * A call that worked clears the carry, including the calls that do not
 * document a return at all.
 *
 * This is a decision and not a fact about DOS -- int21.c says so, and
 * section 9 says so. It is asserted rather than left alone because the
 * alternative is a stale flag from an unrelated earlier call, and a
 * program that checks the carry after printing a string would then take a
 * successful print for a failed one.
 *
 * The carry is set by hand before the call, so a dispatcher that never
 * touched the flag at all would fail this.
 */
static void test_a_call_that_worked_clears_the_carry(struct vm86_cpu *cpu)
{
    struct bios10_state video;
    struct int21_state  dos;

    power_on_with_a_program(cpu, &video, &dos);

    vm86_mem_write8(cpu->mem, TEST_PROGRAM_LINEAR + 0x200u, '$');

    vm86_flag_set(cpu, VM86_CF, true);
    cpu->ds = TEST_PROGRAM_SEGMENT;
    cpu->dx = 0x0200;
    cpu->ah = 0x09;

    int21_service(cpu, &dos);

    vm86_expect_flag("CF after AH=09h", cpu, VM86_CF, false);

    /* And the same for a call whose whole job is to report a failure of
     * its own: 35h never fails, so it must not leave the last failure's
     * carry standing. */
    vm86_flag_set(cpu, VM86_CF, true);
    cpu->al = 0x21;
    cpu->ah = 0x35;

    int21_service(cpu, &dos);

    vm86_expect_flag("CF after AH=35h", cpu, VM86_CF, false);
}

/* ------------------------------------------------------------------ */
/* Ending the program                                                  */
/* ------------------------------------------------------------------ */

/*
 * AH=4Ch takes AL as the return code and sets it on the processor.
 *
 * The code is 7 rather than 0, and that is the whole test: a dispatcher
 * that never wrote the code would leave whatever was there, and zero --
 * the value a machine that had not been asked would report -- is what a
 * test using 0 would have been unable to tell from that.
 */
static void test_terminate_with_a_code(struct vm86_cpu *cpu)
{
    struct bios10_state video;
    struct int21_state  dos;

    power_on_with_a_program(cpu, &video, &dos);

    cpu->ax = 0x4C07;
    cpu->exit_code = 0xFFFF;

    int21_service(cpu, &dos);

    vm86_expect_bool("exited", cpu->exited, true);
    vm86_expect_u16("the exit code", cpu->exit_code, 7);
}

/*
 * AH=00h and INT 20h both end the program with a code of zero.
 *
 * Two functions rather than one because they are two vectors: AH=00h goes
 * through the dispatcher and INT 20h has a service of its own, which is
 * what a bare RET from a .COM arrives at -- the loader put a zero word
 * under SP so that the RET pops it and lands on PSP:0000, which is CD 20.
 */
static void test_terminate_the_old_way(struct vm86_cpu *cpu)
{
    struct bios10_state video;
    struct int21_state  dos;

    power_on_with_a_program(cpu, &video, &dos);

    cpu->ax = 0x0037;    /* AL is ignored by this one */
    cpu->exit_code = 0xFFFF;
    cpu->ah = 0x00;

    int21_service(cpu, &dos);

    vm86_expect_bool("exited", cpu->exited, true);
    vm86_expect_u16("AH=00h exits with zero", cpu->exit_code, 0);

    /* And the other door into the same ending. */
    cpu->exited = false;
    cpu->exit_code = 0xFFFF;

    int21_terminate_service(cpu, NULL);

    vm86_expect_bool("exited", cpu->exited, true);
    vm86_expect_u16("INT 20h exits with zero", cpu->exit_code, 0);
}

/*
 * The trap turns the flag into VM86_EXIT, and the run loop turns that
 * into VM86_STOP_EXIT.
 *
 * Both layers, because they are two different things. The instruction
 * layer has to stop returning VM86_CONTINUE -- otherwise the guest would
 * carry on executing past the INT that ended it -- and the run loop has
 * to stop reporting VM86_STOP_STEPS, because a caller that read the exit
 * as a slice ending would run the program again.
 *
 * The code is 7 again, and this time it has travelled all the way through
 * real guest code: `mov ax, 4C07` then `int 21h`.
 */
static void test_the_trap_reports_an_exit(struct vm86_cpu *cpu)
{
    struct bios10_state video;
    struct int21_state  dos;
    static const uint8_t code[] = {
        0xB8, 0x07, 0x4C,   /* mov ax, 0x4C07 */
        0xCD, 0x21,         /* int 0x21       */
        0xF4,               /* hlt            */
    };

    power_on_with_a_program(cpu, &video, &dos);

    for (size_t i = 0; i < sizeof code; i++)
        vm86_mem_write8(cpu->mem, TEST_PROGRAM_LINEAR + 0x100u + i, code[i]);

    vm86_set_seg(cpu, VM86_CS, TEST_PROGRAM_SEGMENT);
    vm86_set_seg(cpu, VM86_DS, TEST_PROGRAM_SEGMENT);
    vm86_set_seg(cpu, VM86_ES, TEST_PROGRAM_SEGMENT);
    vm86_set_seg(cpu, VM86_SS, TEST_PROGRAM_SEGMENT);
    vm86_flush_segments(cpu);

    cpu->ip = 0x0100;
    cpu->sp = 0xFFFE;

    enum vm86_stop stop = vm86_run(cpu, 100u);

    vm86_expect_u16("the stop", (uint16_t)stop, (uint16_t)VM86_STOP_EXIT);
    vm86_expect_u16("the exit code the guest asked for", cpu->exit_code, 7);

    /*
     * And the machine stopped without running the stub's IRET.
     *
     * That is the part worth asserting, because it is the part a service
     * cannot do for itself: the dispatcher sets a flag, and the trap
     * turns it into VM86_EXIT *instead of* VM86_CONTINUE, which is what
     * stops the instruction layer from letting the stub return. So the
     * interrupt frame the guest's own INT pushed is still on the stack,
     * six bytes of it, exactly as the INT instruction left it.
     *
     * The frame holds the address *after* the INT -- 0x0105, past the two
     * bytes of `int 0x21` at 0x0103 -- because that is what the hardware
     * pushes. A machine that had run the IRET would have SP back at
     * 0xFFFE and those words gone.
     */
    vm86_expect_u16("SP, six bytes short of where it started", cpu->sp,
                    0xFFFEu - 6u);
    vm86_expect_u16("CS at the stop, in the stub", cpu->cs, 0xF000);
    vm86_expect_mem16("the frame: the return offset", cpu,
                      TEST_PROGRAM_LINEAR + 0xFFF8u, 0x0105);
    vm86_expect_mem16("the frame: the return segment", cpu,
                      TEST_PROGRAM_LINEAR + 0xFFFAu, TEST_PROGRAM_SEGMENT);
}

/* ------------------------------------------------------------------ */
/* The carry flag crossing the stub                                    */
/* ------------------------------------------------------------------ */

/*
 * A failure reported in the carry flag reaches the guest.
 *
 * This pins the trap's flag write-back through real guest code. The
 * dispatcher sets the flag on the processor; the trap has to copy it into
 * the interrupt frame, because the stub's IRET restores FLAGS from there
 * and would otherwise throw it away. Everything else a service returns --
 * AX, and every other register -- crosses the boundary untouched by the
 * IRET, so the carry is the *only* value whose loss is invisible here and
 * everywhere else: the program reads a zero and believes it.
 *
 * It is asserted through real guest code rather than by calling the
 * dispatcher, because calling it directly is exactly the arrangement in
 * which the write-back does not matter -- the test would read cpu->flags
 * and never go near the frame.
 *
 * `cli; hlt` at the end because this is a halt the run loop will stop at:
 * with IF still set the processor is merely waiting, and the loop would
 * go round for ever.
 */
static void test_the_carry_survives_the_stub(struct vm86_cpu *cpu)
{
    struct bios10_state video;
    struct int21_state  dos;
    static const uint8_t code[] = {
        0xB4, 0x0E,   /* mov ah, 0x0E   */
        0xB2, 0x00,   /* mov dl, 0      -- A:, which is not there */
        0xCD, 0x21,   /* int 0x21       */
        0xFA,         /* cli            */
        0xF4,         /* hlt            */
    };

    power_on_with_a_program(cpu, &video, &dos);

    for (size_t i = 0; i < sizeof code; i++)
        vm86_mem_write8(cpu->mem, TEST_PROGRAM_LINEAR + 0x100u + i, code[i]);

    vm86_set_seg(cpu, VM86_CS, TEST_PROGRAM_SEGMENT);
    vm86_set_seg(cpu, VM86_DS, TEST_PROGRAM_SEGMENT);
    vm86_set_seg(cpu, VM86_ES, TEST_PROGRAM_SEGMENT);
    vm86_set_seg(cpu, VM86_SS, TEST_PROGRAM_SEGMENT);
    vm86_flush_segments(cpu);

    cpu->ip = 0x0100;
    cpu->sp = 0xFFFE;

    enum vm86_stop stop = vm86_run(cpu, 100u);

    vm86_expect_u16("the stop", (uint16_t)stop, (uint16_t)VM86_STOP_HALT);
    vm86_expect_u16("AX, the error code the guest read", cpu->ax, 0x000F);
    vm86_expect_flag("CF, after the stub's IRET", cpu, VM86_CF, true);
}

/*
 * The same carry on a machine where the timer has fired first.
 *
 * A hardware interrupt is delivered by pushing a frame of its own on top
 * of whatever is already there, and the firmware's timer handler chains
 * to INT 1Ch, so a single tick puts two more frames between the guest's
 * stack pointer and its own. Afterwards the guest's own `int 21h` is
 * taken, and the flag it sets has to come back to it through a frame that
 * is now three words above where the tick left things.
 *
 * This is the test that would catch a machine which tracked the frame by
 * a single remembered stack pointer that a nested interrupt overwrote --
 * the trap compares the pointer it recorded with the one in front of it,
 * and the two have to describe the frame the stub is actually sitting on.
 * It passes today, and it is here because the alternative is finding out
 * from a program of somebody else's that reports disk errors as successes.
 *
 * The timer is raised before the guest starts, so its whole chain has run
 * and returned long before the INT. Nothing has to land in an awkward
 * window for this to be worth running.
 */
static void test_the_carry_survives_a_timer(struct vm86_cpu *cpu)
{
    struct bios10_state video;
    struct int21_state  dos;
    struct bios1a_state clock;
    static const uint8_t code[] = {
        0xB4, 0x0E,   /* mov ah, 0x0E   */
        0xB2, 0x00,   /* mov dl, 0      -- A:, which is not there */
        0xCD, 0x21,   /* int 0x21       */
        0xFA,         /* cli            */
        0xF4,         /* hlt            */
    };

    power_on_with_a_program(cpu, &video, &dos);

    bios1a_reset(&clock);
    bios1a_init(cpu, &clock);
    vm86_register_service(VM86_INT_TIMER, bios1a_irq, &clock);

    for (size_t i = 0; i < sizeof code; i++)
        vm86_mem_write8(cpu->mem, TEST_PROGRAM_LINEAR + 0x100u + i, code[i]);

    vm86_set_seg(cpu, VM86_CS, TEST_PROGRAM_SEGMENT);
    vm86_set_seg(cpu, VM86_DS, TEST_PROGRAM_SEGMENT);
    vm86_set_seg(cpu, VM86_ES, TEST_PROGRAM_SEGMENT);
    vm86_set_seg(cpu, VM86_SS, TEST_PROGRAM_SEGMENT);
    vm86_flush_segments(cpu);

    cpu->ip = 0x0100;
    cpu->sp = 0xFFFE;

    /* One tick, before the first instruction. The handler runs at the
     * first boundary, chains to 1Ch, and both frames are gone long before
     * the guest's INT. */
    vm86_raise(cpu, VM86_INT_TIMER);

    enum vm86_stop stop = vm86_run(cpu, 20u);

    vm86_expect_u16("the stop", (uint16_t)stop, (uint16_t)VM86_STOP_HALT);
    vm86_expect_u16("AX, the error code the guest read", cpu->ax, 0x000F);
    vm86_expect_flag("CF, after the stub's IRET", cpu, VM86_CF, true);
}

/* ------------------------------------------------------------------ */
/* The acceptance: a .COM that asks DOS for things                     */
/* ------------------------------------------------------------------ */

/*
 * Load dos/corpus/dos/int21.asm the way DOS loads one, run it, and read
 * the page.
 *
 * The expected rows are the program's own format -- its labels and the
 * column they line up in -- with the *values* beside them from
 * docs/dos-refs-dos.md: 3.30 as 1E03, the stub for vector 21h at
 * F000:0084, the machine's drive F: as 05 in a byte and the count as 06.
 *
 * This is the only test here that goes through a real interrupt path. The
 * ones above call the dispatcher directly; this one executes `int 21h` in
 * guest code, on a machine built by the same replay code the other
 * corpora use, and reads the answer off the page.
 */
static void test_the_program_prints_its_answers(struct vm86_cpu *cpu)
{
    struct vm86_bios_machine *machine = vm86_bios_machine_new();

    (void)cpu;

    if (!machine) {
        vm86_expect_bool("a machine could be built", false, true);
        return;
    }

    struct vm86_dos_start start = {
        .segment     = TEST_PROGRAM_SEGMENT,
        .environment = TEST_ENVIRONMENT_SEGMENT,
        .parent      = TEST_PARENT_SEGMENT,
        .path        = TEST_PATH,
        .tail        = NULL,
        .vars        = NULL,
        .var_count   = 0,
    };
    struct vm86_dos_psp psp;

    enum vm86_dos_load_result loaded =
        vm86_bios_load_dos(machine, &start, corpus_int21,
                           (uint32_t)corpus_int21_size, &psp);

    vm86_expect_u16("the loader accepted it", (uint16_t)loaded,
                    (uint16_t)VM86_DOS_LOADED);

    struct vm86_bios_plan plan = {
        .slices          = 200u,
        /*
         * The same numbers the real machine uses, and both of them are
         * here because they are the machine rather than the test. vm.c
         * drives the interpreter in 250-instruction slices and hands the
         * guest's clock the milliseconds that really went by, which for a
         * program this short is a tick every slice or two.
         *
         * Running the acceptance against a *different* machine -- one long
         * slice, no clock -- is what let a slice-boundary difference go
         * unnoticed here while the Ring 3 run had it: the host suite said
         * the page was right and the machine that people will actually use
         * said something else, and the only reason that was found at all
         * is that the screen test photographs the real one.
         *
         * 55 ms is just past one 54.925 ms tick, so each slice produces
         * exactly one. The arithmetic is in vm.c's timer case.
         */
        .steps_per_slice = 250u,
        .ms_per_slice    = 55u,
        .key_at_slice    = 0u,
        .key_scancode    = 0u,
    };

    enum vm86_bios_stop stop = vm86_bios_run(machine, &plan);

    vm86_expect_u16("how it ended", (uint16_t)stop,
                    (uint16_t)VM86_BIOS_EXITED);
    vm86_expect_u16("the code it exited with", vm86_bios_exit_code(machine),
                    7);

    /*
     * The page, row by row.
     *
     * The machine is not the CPU the harness handed this test, so these
     * read the replay machine's memory rather than the test's own. That
     * is deliberate: the point is what the guest left on the page of the
     * machine it actually ran on.
     */
    const uint8_t *ram = vm86_bios_memory(machine);

    static const char *const expected[] = {
        "DOS version = 1E03",
        "W4: INT 21h says hello!",
        "vector 21h  = F000:0084",
        "vector 60h  = 1000:1234",
        "DTA         = 1000:0200",
        "drive       = 05",
        "select F:   = 06",
        "select A:   = 000F CF=1",
        "function 55 = 5500 CF=1",
    };

    for (unsigned row = 0; row < sizeof expected / sizeof expected[0]; row++) {
        char line[81];
        unsigned used = 80;

        for (unsigned col = 0; col < 80; col++)
            line[col] = (char)ram[VM86_TEXT_BASE + (row * 80u + col) * 2u];

        line[80] = '\0';

        while (used > 0 && line[used - 1] == ' ')
            line[--used] = '\0';

        if (strcmp(line, expected[row]) != 0) {
            printf("      FAIL  the program printed \"%s\" on row %u, "
                   "expected \"%s\"\n", line, row, expected[row]);
            vm86_expect_u16("the page", 1, 0);
        }
    }

    vm86_expect_u16("every row was compared", 9, 9);

    /*
     * And every service's flags found their frame.
     *
     * The two numbers the machine keeps are the only place this shows:
     * from inside the guest, a service that answered with the carry flag
     * set and one whose carry flag was dropped look the same, so a page
     * that reports success is what both produce. The first half is
     * asserted as "more than none" rather than as an exact count, because
     * the count is one per interrupt and belongs to the machine; the
     * second half is asserted as exactly zero, because a program this
     * straightforward never reaches the stub by a chained far call, which
     * is the one case where declining is right.
     */
    vm86_expect_bool("some flags were written back",
                     vm86_flags_written_back() > 0, true);
    vm86_expect_u16("and none were declined",
                    (uint16_t)vm86_flags_declined(), 0);

    vm86_bios_machine_free(machine);
}

/* ------------------------------------------------------------------ */

static const struct vm86_test tests[] = {
    { "the version",                        test_version },
    { "output a character",                 test_output_char },
    { "output a string",                    test_output_string },
    { "a string that never ends",           test_a_string_that_never_ends },
    { "read a stub's vector",               test_read_a_stub_vector },
    { "write and read a vector",            test_write_and_read_a_vector },
    { "the transfer address",               test_the_transfer_address },
    { "the default transfer address",       test_the_default_transfer_address },
    { "the default with no program",
      test_the_default_transfer_address_without_a_program },
    { "which drive",                        test_which_drive },
    { "select the drive",                   test_select_the_drive },
    { "select a drive that is not there",
      test_select_a_drive_that_is_not_there },
    { "a function that is not implemented",
      test_a_function_that_is_not_implemented },
    { "a call that worked clears the carry",
      test_a_call_that_worked_clears_the_carry },
    { "terminate with a code",              test_terminate_with_a_code },
    { "terminate the old way",              test_terminate_the_old_way },
    { "the trap reports an exit",           test_the_trap_reports_an_exit },
    { "the carry survives the stub",        test_the_carry_survives_the_stub },
    { "the carry survives a timer",         test_the_carry_survives_a_timer },
    { "the program prints its answers",
      test_the_program_prints_its_answers },
};

VM86_TEST_MAIN("int21", tests)
