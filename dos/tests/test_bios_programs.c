/*
 * The BIOS corpus suite: run each assembled program against a whole
 * machine and grade what it left on the screen.
 *
 * ---------------------------------------------------------------------
 * What this suite is, and why it is the milestone's acceptance
 *
 * M4's acceptance sentence is "a .COM program can print through the BIOS
 * and appear on the screen". Every other suite in this directory measures
 * something narrower than that: a service answering a register correctly,
 * a trap returning to the right address, an instruction producing the
 * right flag. This is the only one that runs whole programs the way a user
 * would, through INT 10h and INT 13h and hardware interrupts, and asks
 * what the machine ended up looking like.
 *
 * So it is also where a mistake that every narrower test agrees on is
 * supposed to show up. Each sample's own header says which decision it is
 * there to keep honest; this file says what the machine must look like
 * afterwards, and where a number came from -- docs/dos-refs.md by section
 * where that document covers it, and "this is ours" where it does not.
 *
 * ---------------------------------------------------------------------
 * On the two programs that must agree
 *
 * hello.asm goes through INT 10h and direct.asm writes 0xB8000 by hand,
 * and the whole 4000-byte page each leaves behind has to be identical.
 * That comparison is M4-8's only test: it fails the moment the display
 * becomes two pieces of state instead of one block of guest memory. It is
 * run as a single case rather than two so that the comparison cannot be
 * lost by somebody reordering a table.
 *
 * ---------------------------------------------------------------------
 * On the one case expected to fail today
 *
 * disk.asm reads a sector that is there and a sector that is not, and
 * tells them apart by the CARRY FLAG, which is how INT 13h reports
 * (docs/dos-refs.md section 4). As host.h and trap.c stand, a host service
 * cannot return CF: the stub ends in a real IRET, which pops FLAGS from
 * the frame the guest's own INT pushed. The gap is measured rather than
 * argued, and it is written up in M4-E-report.md.
 *
 * The case below is written to the specification and will report FAIL
 * until that is settled. It says so in its own failure text, because a
 * reader who finds a disk sample printing FAIL will otherwise go looking
 * for a disk bug, and there is not one.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "harness.h"

#include <vm86/host.h>

#include "../corpus/bios/replay.h"

/* ------------------------------------------------------------------ */
/* The images                                                          */
/* ------------------------------------------------------------------ */

/*
 * One pair per sample in dos/corpus/bios, emitted by the build. The symbol
 * names come from bin2c.py and keep only the file's base name, so the
 * subdirectory does not appear in them.
 */
#define BIOS_IMAGE(sym)                       \
    extern const unsigned char sym[];         \
    extern const unsigned long sym##_size;

BIOS_IMAGE(corpus_hello)
BIOS_IMAGE(corpus_direct)
BIOS_IMAGE(corpus_cursor)
BIOS_IMAGE(corpus_scroll)
BIOS_IMAGE(corpus_timer)
BIOS_IMAGE(corpus_key)
BIOS_IMAGE(corpus_disk)

/* ------------------------------------------------------------------ */
/* Reporting                                                           */
/* ------------------------------------------------------------------ */

static int g_failures;

static void fail(const char *fmt, ...)
{
    va_list args;

    printf("      FAIL  ");
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");

    g_failures++;
}

static const char *stop_name(enum vm86_bios_stop stop)
{
    switch (stop) {
    case VM86_BIOS_HALTED:     return "halting (cli; hlt)";
    case VM86_BIOS_FAULT:      return "a fault with no handler";
    case VM86_BIOS_BROKEN:     return "the emulator's opcode tables";
    case VM86_BIOS_UNFINISHED: return "never finishing";
    case VM86_BIOS_NO_FIRMWARE:return "no firmware to run against";
    }

    return "?";
}

static void expect_stop(const char *what, enum vm86_bios_stop got,
                        enum vm86_bios_stop want)
{
    if (got == want)
        return;

    fail("%s: the machine stopped by %s, expected %s",
         what, stop_name(got), stop_name(want));
}

/* ------------------------------------------------------------------ */
/* Reading the machine back                                            */
/* ------------------------------------------------------------------ */

static uint8_t mem8(const struct vm86_bios_machine *machine, uint32_t linear)
{
    return vm86_bios_memory(machine)[linear];
}

static uint16_t mem16(const struct vm86_bios_machine *machine,
                      uint32_t linear)
{
    const uint8_t *memory = vm86_bios_memory(machine);

    return (uint16_t)(memory[linear] | ((uint16_t)memory[linear + 1u] << 8));
}

static uint32_t mem32(const struct vm86_bios_machine *machine,
                      uint32_t linear)
{
    return (uint32_t)mem16(machine, linear) |
           ((uint32_t)mem16(machine, linear + 2u) << 16);
}

/* A byte of the BIOS data area, which programs read directly. */
static uint16_t bda16(const struct vm86_bios_machine *m, uint16_t offset)
{
    return mem16(m, ((uint32_t)VM86_BDA_SEGMENT << 4) + offset);
}

/* How far apart two pages are, as the firmware reports it. Read rather
 * than assumed: it is 0x1000 and not the 4000 bytes of content
 * (docs/dos-refs.md section 7), and a suite that wrote its own number down
 * would be testing its own arithmetic. */
static uint16_t page_stride(const struct vm86_bios_machine *m)
{
    return bda16(m, VM86_BDA_PAGE_BYTES);
}

/* A cell of a page of the text window, straight out of guest memory. */
static struct vm86_bios_cell page_cell(const struct vm86_bios_machine *m,
                                       uint16_t page, uint16_t cell)
{
    uint32_t base = VM86_BIOS_TEXT_BASE + (uint32_t)page_stride(m) * page +
                    (uint32_t)cell * 2u;
    struct vm86_bios_cell result;

    result.character = mem8(m, base);
    result.attribute = mem8(m, base + 1u);

    return result;
}

/* ------------------------------------------------------------------ */
/* Assertions                                                          */
/* ------------------------------------------------------------------ */

static void expect_cell(const char *what, const struct vm86_bios_screen *screen,
                        uint16_t cell, uint8_t character, uint8_t attribute)
{
    struct vm86_bios_cell got = vm86_bios_cell_at(screen, cell);

    if (got.character == character && got.attribute == attribute)
        return;

    fail("%s: cell %u is (%02X, %02X), expected (%02X, %02X)",
         what, cell, got.character, got.attribute, character, attribute);
}

static void expect_page_cell(const char *what,
                             const struct vm86_bios_machine *machine,
                             uint16_t page, uint16_t cell,
                             uint8_t character, uint8_t attribute)
{
    struct vm86_bios_cell got = page_cell(machine, page, cell);

    if (got.character == character && got.attribute == attribute)
        return;

    fail("%s: page %u cell %u is (%02X, %02X), expected (%02X, %02X)",
         what, page, cell, got.character, got.attribute, character, attribute);
}

/*
 * A run of cells that must all hold the same character and attribute,
 * reporting at most one difference: a wrong screen of two thousand cells
 * should not produce two thousand lines to scroll through.
 */
static void expect_run(const char *what, const struct vm86_bios_screen *screen,
                       uint16_t first, uint16_t last,
                       uint8_t character, uint8_t attribute)
{
    for (uint16_t cell = first; cell < last; cell++) {
        struct vm86_bios_cell got = vm86_bios_cell_at(screen, cell);

        if (got.character == character && got.attribute == attribute)
            continue;

        fail("%s: cell %u is (%02X, %02X), expected (%02X, %02X)",
             what, cell, got.character, got.attribute, character, attribute);
        return;
    }
}

/* One whole row of the active page. */
static void expect_row(const char *what, const struct vm86_bios_screen *screen,
                       uint16_t row, uint8_t character, uint8_t attribute)
{
    uint16_t first = (uint16_t)(row * VM86_BIOS_CELLS_PER_ROW);

    expect_run(what, screen, first,
               (uint16_t)(first + VM86_BIOS_CELLS_PER_ROW),
               character, attribute);
}

static void expect_mem8(const char *what, const struct vm86_bios_machine *m,
                        uint32_t linear, uint8_t want)
{
    uint8_t got = mem8(m, linear);

    if (got != want)
        fail("%s: memory at %04X:%04X is %02X, expected %02X",
             what, (unsigned)(linear >> 4), (unsigned)(linear & 0xFu),
             got, want);
}

static void expect_mem16(const char *what, const struct vm86_bios_machine *m,
                         uint32_t linear, uint16_t want)
{
    uint16_t got = mem16(m, linear);

    if (got != want)
        fail("%s: memory at %06X is %04X, expected %04X",
             what, (unsigned)linear, got, want);
}

static void expect_u16(const char *what, uint16_t got, uint16_t want)
{
    if (got != want)
        fail("%s: %04X, expected %04X", what, got, want);
}

static void expect_u32(const char *what, uint32_t got, uint32_t want)
{
    if (got != want)
        fail("%s: %u, expected %u", what, (unsigned)got, (unsigned)want);
}

/* The stop the program is supposed to reach, with the plan spelled out, so
 * that a case reads as one line of intent plus its expectations. */
static void run_to_a_halt(const char *what, struct vm86_bios_machine *m,
                          const uint8_t *image, uint32_t size)
{
    const struct vm86_bios_plan plan = {
        .slices          = 400,
        .steps_per_slice = 2000,
        .ms_per_slice    = 0,
        .key_at_slice    = 0,
        .key_scancode    = 0,
    };

    vm86_bios_load(m, image, (uint16_t)size);
    expect_stop(what, vm86_bios_run(m, &plan), VM86_BIOS_HALTED);
}

/* ------------------------------------------------------------------ */
/* The cases                                                           */
/* ------------------------------------------------------------------ */

/*
 * hello.asm through INT 10h, and direct.asm by hand, and the text the two
 * leave behind compared down to the attribute.
 *
 * hello.asm sets mode 3 first, so its page is the mode set's clear -- a
 * space with attribute 0x07 in every cell -- with the 22 characters
 * written into it. direct.asm executes no interrupt at all, so its page is
 * the cleared one, zero everywhere, with the same 22 cells written by
 * hand. The cells the message occupies must therefore be identical in the
 * two, attributes included; around them each page must have the shape its
 * own program left.
 *
 * This is M4-8's test, and it is two claims rather than one. hello.asm's
 * characters reach the screen through a service and direct.asm's never
 * pass through one, so a display kept as a second copy of the screen
 * would show hello's text and not direct's -- which is why a frame is
 * asserted to have been recorded for both, and not only the guest's memory
 * compared. And the two pages agreeing about those 22 cells is what says
 * the service and the direct write land in the same place, byte for byte,
 * attribute included.
 *
 * The cursor is asserted separately for each, because it is the one thing
 * the two must NOT agree about: writing memory does not move the
 * firmware's cursor, and running a teletype does.
 */
static void case_hello_and_direct(struct vm86_bios_machine *m)
{
    static const char message[] = "M4 hello from the BIOS";
    const uint16_t message_length = (uint16_t)(sizeof message - 1u);

    struct vm86_bios_screen hello_screen;

    run_to_a_halt("hello", m, corpus_hello, corpus_hello_size);
    hello_screen = *vm86_bios_screen(m);

    for (uint16_t i = 0; i < message_length; i++)
        expect_cell("hello: the text", &hello_screen, i,
                    (uint8_t)message[i], VM86_ATTR_DEFAULT);

    expect_run("hello: the rest of the page, which the mode set cleared",
               &hello_screen, message_length, VM86_TEXT_CELLS,
               ' ', VM86_ATTR_DEFAULT);

    expect_u16("hello: where the cursor stopped",
               hello_screen.cursor, message_length);

    if (hello_screen.presents == 0)
        fail("hello: the host was never handed a frame, so nothing shows "
             "the text the program printed ever reached a screen");

    run_to_a_halt("direct", m, corpus_direct, corpus_direct_size);

    for (uint16_t i = 0; i < message_length; i++)
        expect_cell("direct: the text", vm86_bios_screen(m), i,
                    (uint8_t)message[i], VM86_ATTR_DEFAULT);

    expect_run("direct: the rest of the page, which it must not have "
               "touched at all", vm86_bios_screen(m), message_length,
               VM86_TEXT_CELLS, 0x00, 0x00);

    expect_u16("direct: the cursor must not have moved",
               vm86_bios_screen(m)->cursor, 0);

    if (vm86_bios_screen(m)->presents == 0)
        fail("direct: the host was never handed a frame, so nothing shows "
             "it renders from the memory this program wrote by hand");

    for (uint32_t byte = 0; byte < (uint32_t)message_length * 2u; byte++) {
        if (hello_screen.cells[byte] == vm86_bios_screen(m)->cells[byte])
            continue;

        fail("M4-8: hello.asm and direct.asm left different text -- byte %u "
             "of the page is %02X after hello and %02X after direct "
             "(cell %u, %s)",
             (unsigned)byte, hello_screen.cells[byte],
             vm86_bios_screen(m)->cells[byte], (unsigned)(byte / 2u),
             (byte & 1u) ? "attribute" : "character");
        break;
    }
}

/*
 * The vector table is where it is, and the program is not on top of it.
 *
 * This is the whole reason the convention gives a program a segment of its
 * own, and it is the one assertion that would have caught the old one: a
 * program loaded at linear 0x100 with CS = 0 puts its own first 768 bytes
 * at vector entries 64 and up. Two entries are checked -- the first the
 * image would have covered, and one well inside -- because a TSR, which is
 * what M5 is for, hooks whatever vector it likes, conventionally somewhere
 * above 0x60.
 *
 * What is checked is the stub the firmware wrote. An entry holding program
 * bytes is not that stub, whatever else it looks like, and the failure
 * says so rather than reporting a bare mismatch.
 */
static void case_vectors(struct vm86_bios_machine *m)
{
    run_to_a_halt("vectors", m, corpus_hello, corpus_hello_size);

    for (uint16_t vector = 64; vector <= 128; vector += 64) {
        uint32_t entry   = (uint32_t)vector * 4u;
        uint16_t offset  = mem16(m, entry);
        uint16_t segment = mem16(m, entry + 2u);

        if (offset == (uint16_t)(vector * VM86_TRAP_STRIDE) &&
            segment == VM86_TRAP_SEGMENT)
            continue;

        fail("vectors: entry %u holds %04X:%04X, which is not the "
             "firmware's stub -- the program's image is lying on the "
             "vector table", (unsigned)vector, segment, offset);
    }
}

/*
 * cursor.asm: where the cursor is, which page it is on, and the rule that
 * 09h does not move it while 0Eh does.
 *
 * Page 1's cell is read out of guest memory rather than out of the
 * recorded screen, because the recorded screen follows the ACTIVE page --
 * which the program leaves on page 1. Reading both pages out of memory is
 * what shows that the page 0 write stayed on page 0 and did not follow the
 * page switch, and the two cells 410 and 163 are checked against each
 * other because a write that ignored BH would land on both.
 *
 * The cursor words are (row << 8) | column, and the values here are the
 * positions the sample asked for with 02h -- not the cell indices, which
 * are what the display reports.
 */
static void case_cursor(struct vm86_bios_machine *m)
{
    run_to_a_halt("cursor", m, corpus_cursor, corpus_cursor_size);

    expect_page_cell("cursor: the page 0 write", m, 0, 410, 'A', 0x1E);
    expect_page_cell("cursor: page 0 where page 1 was written", m,
                     0, 163, ' ', VM86_ATTR_DEFAULT);
    expect_page_cell("cursor: the page 1 write", m, 1, 163, 'B', 0x2F);

    /*
     * The BIOS data area's video half, which this program established by
     * setting the mode and which a program reads directly rather than
     * through a service. 0x1000 and not 4000: it is the stride between
     * pages, rounded up to a 4 KiB boundary, and the header of bios10.h
     * says the same thing (docs/dos-refs.md section 7).
     */
    expect_mem8 ("cursor: the mode the program set", m,
                 ((uint32_t)VM86_BDA_SEGMENT << 4) + VM86_BDA_VIDEO_MODE, 3);
    expect_mem16("cursor: the columns a mode 3 page has", m,
                 ((uint32_t)VM86_BDA_SEGMENT << 4) + VM86_BDA_COLUMNS, 80);
    expect_mem16("cursor: the page stride", m,
                 ((uint32_t)VM86_BDA_SEGMENT << 4) + VM86_BDA_PAGE_BYTES,
                 0x1000u);

    expect_mem16("cursor: page 0 cursor", m,
                 ((uint32_t)VM86_BDA_SEGMENT << 4) + VM86_BDA_CURSOR, 0x050A);
    expect_mem16("cursor: page 1 cursor", m,
                 ((uint32_t)VM86_BDA_SEGMENT << 4) + VM86_BDA_CURSOR + 2u,
                 0x0203);
    expect_mem8 ("cursor: the active page", m,
                 ((uint32_t)VM86_BDA_SEGMENT << 4) + VM86_BDA_ACTIVE_PAGE, 1);

    /* What AH=03h returned, stored by the program itself. */
    /* What AH=03h returned, stored by the program itself -- addressed from
     * the program's own segment, which is where its data is. */
    expect_mem8("cursor: the row 03h reported",  m, VM86_BIOS_LOAD_LINEAR + 0x0800u, 0x05);
    expect_mem8("cursor: the column 03h reported", m, VM86_BIOS_LOAD_LINEAR + 0x0801u, 0x0A);
}

/*
 * scroll.asm: the window rectangle, the fill attribute, and the teletype
 * scrolling itself off the bottom.
 *
 * The whole final screen, row by row. The band at rows 9..13 is the AL=0
 * fill and its attribute 0x20 is what distinguishes a fill from a scroll
 * that happened to move blank lines; row 24's 0x30 is the 06h scroll's own
 * fill attribute, still visible because nothing after it scrolls again.
 *
 * Row 22 is the erratum of 2026-10-01: the sample sets BL to 0x0F before
 * every teletype call and these cells must come back with the 0x07 they
 * already had, because 0Eh writes no attribute byte (docs/dos-refs.md
 * section 1). An implementation that writes BL passes every other
 * assertion in this program.
 *
 * Row 23 is blank and its attribute is NOT asserted. The teletype's own
 * scroll fills the row it brings in at the bottom, and what it fills it
 * with is the implementation's to choose -- docs/dos-refs.md records that
 * 0Eh scrolls by calling 06h internally but not what attribute it passes.
 */
static void case_scroll(struct vm86_bios_machine *m)
{
    const struct vm86_bios_screen *screen;

    run_to_a_halt("scroll", m, corpus_scroll, corpus_scroll_size);
    screen = vm86_bios_screen(m);

    for (uint16_t row = 0; row <= 8; row++)
        expect_row("scroll: the letters below the bands", screen, row,
                   (uint8_t)('C' + row), 0x07);

    for (uint16_t row = 9; row <= 13; row++)
        expect_row("scroll: the band AL=0 filled (attribute 0x20)", screen,
                   row, 0x20, 0x20);

    for (uint16_t row = 14; row <= 21; row++)
        expect_row("scroll: the letters above the teletype band", screen, row,
                   (uint8_t)('Q' + (row - 14)), 0x07);

    expect_row("scroll: the teletype's eighty characters (attribute 0x07, "
               "NOT the BL of 0x0F)", screen, 22, 'x', 0x07);

    for (uint16_t column = 0; column < VM86_BIOS_CELLS_PER_ROW; column++) {
        uint16_t cell = (uint16_t)(23u * VM86_BIOS_CELLS_PER_ROW + column);

        if (vm86_bios_cell_at(screen, cell).character != 0x20) {
            expect_cell("scroll: the row the teletype's scroll brought in "
                        "(character only; the attribute is not asserted)",
                        screen, cell, 0x20,
                        vm86_bios_cell_at(screen, cell).attribute);
            break;
        }
    }

    expect_row("scroll: the row 06h filled (attribute 0x30)", screen, 24,
               0x20, 0x30);
}

/*
 * timer.asm: an interrupt the guest did not ask for, and the 08h -> 1Ch
 * chain.
 *
 * The plan hands the clock 55 ms per slice, which is just past the 54.925
 * ms of one tick at 18.2 Hz (docs/dos-refs.md section 6), so every slice
 * produces exactly one tick and the guest counts to five after five
 * slices. Both counts are asserted and they are different facts: the one
 * at 0040:006C is the firmware's, written by the INT 08h service, and the
 * one at 0000:0802 is the sample's own, written by the handler it hooked
 * onto 1Ch. A machine where only one of them reaches 5 has a chain that
 * breaks in a particular place, and the two failures read very differently.
 */
static void case_timer(struct vm86_bios_machine *m)
{
    const struct vm86_bios_plan plan = {
        .slices          = 60,
        .steps_per_slice = 2000,
        .ms_per_slice    = 55,
        .key_at_slice    = 0,
        .key_scancode    = 0,
    };

    vm86_bios_load(m, corpus_timer, (uint16_t)corpus_timer_size);
    expect_stop("timer", vm86_bios_run(m, &plan), VM86_BIOS_HALTED);

    expect_cell("timer: the count it printed", vm86_bios_screen(m), 0,
                '5', VM86_ATTR_DEFAULT);
    expect_u16("timer: the cursor", vm86_bios_screen(m)->cursor, 1);

    expect_u32("timer: the firmware's tick count",
               mem32(m, ((uint32_t)VM86_BDA_SEGMENT << 4) + VM86_BDA_TICK_COUNT),
               5u);
    expect_mem16("timer: our own INT 1Ch handler's count", m, VM86_BIOS_LOAD_LINEAR + 0x0802u, 5u);
}

/*
 * key.asm: the blocking read really blocks.
 *
 * Two runs of one loaded machine. The first gets no key at all and must
 * NOT finish -- that is the whole claim, and a service that answered an
 * empty queue with whatever was in the accumulator would finish here and
 * print a character nobody typed. The second feeds the key and the read
 * is answered.
 *
 * The stack pointer is the other half. A retry does not pop the frame
 * (vm86_service_retry re-runs the trap and leaves the frame alone), so a
 * blocked read holds six bytes for as long as it waits and gives them back
 * once. 0xFFF8 while waiting and 0xFFFE afterwards is that, exactly, and
 * it is the assertion that would catch a retry that unwound the frame
 * early or failed to unwind it at all.
 *
 * The echo is a lowercase 'a': scancode 0x1E with no shift held. What the
 * scan code table maps that to is task C's decision -- docs/dos-refs.md
 * section 9 says the table is deliberately not in that document -- so the
 * expectation is written down here as this project's, not as a document's.
 */
static void case_key(struct vm86_bios_machine *m)
{
    const struct vm86_bios_plan waiting = {
        .slices          = 5,
        .steps_per_slice = 200,
        .ms_per_slice    = 0,
        .key_at_slice    = 0,
        .key_scancode    = 0,
    };
    const struct vm86_bios_plan arriving = {
        .slices          = 20,
        .steps_per_slice = 200,
        .ms_per_slice    = 0,
        .key_at_slice    = 1,
        .key_scancode    = 0x1E,        /* 'A', unshifted                */
    };

    vm86_bios_load(m, corpus_key, (uint16_t)corpus_key_size);

    expect_stop("key: with no key fed", vm86_bios_run(m, &waiting),
                VM86_BIOS_UNFINISHED);

    if (vm86_bios_cell_at(vm86_bios_screen(m), 0).character == 'a')
        fail("key: it echoed a character before any key was fed -- a "
             "blocking read must not answer with what was left in AL");

    expect_u16("key: the stack while waiting", vm86_bios_cpu(m)->sp, 0xFFF8u);

    expect_stop("key: with the key fed", vm86_bios_run(m, &arriving),
                VM86_BIOS_HALTED);

    expect_cell("key: the echo", vm86_bios_screen(m), 0, 'a',
                VM86_ATTR_DEFAULT);
    expect_u16("key: the cursor", vm86_bios_screen(m)->cursor, 1);
    expect_u16("key: the stack after the read was answered",
               vm86_bios_cpu(m)->sp, VM86_BIOS_STACK_TOP);
}

/*
 * disk.asm: a read that must work, a read that must fail, and the buffer
 * the failed one must not touch.
 *
 * The disk image is built here rather than by tools/mkfloppy.py, for the
 * reason the corpus has always given: a test that depends on a working
 * directory fails for reasons that have nothing to do with the machine.
 * It is the full 1 474 560 bytes and not a few dozen, because the firmware
 * checks a request against the geometry it was given and an undersized
 * array would make the bounds arithmetic meaningless.
 *
 * This case fails until the CF gap is closed. See the top of this file.
 */
static void case_disk(struct vm86_bios_machine *m)
{
    uint8_t *disk = vm86_bios_disk(m);

    disk[0] = 'M';
    disk[1] = '4';
    disk[2] = 0x0D;
    disk[3] = 0x0A;

    run_to_a_halt("disk", m, corpus_disk, corpus_disk_size);

    if (vm86_bios_cell_at(vm86_bios_screen(m), 0).character == 'F') {
        fail("disk: the sample printed FAIL, which is what it does when a "
             "disk read does not report the way INT 13h reports. As "
             "host.h and trap.c stand, a host service cannot return CF at "
             "all -- the stub's IRET pops FLAGS from the frame the "
             "guest's INT pushed -- so this failure is the interface's "
             "and not the disk's. See M4-E-report.md.");
        return;
    }

    expect_cell("disk: the verdict", vm86_bios_screen(m), 0, 'O',
                VM86_ATTR_DEFAULT);
    expect_cell("disk: the verdict", vm86_bios_screen(m), 1, 'K',
                VM86_ATTR_DEFAULT);
    expect_u16("disk: the cursor", vm86_bios_screen(m)->cursor, 2);

    /* The sector really arrived. */
    expect_mem8("disk: the first byte of sector 1", m, VM86_BIOS_LOAD_LINEAR + 0x0600u, 0x4D);
    expect_mem8("disk: the second byte of sector 1", m, VM86_BIOS_LOAD_LINEAR + 0x0601u, 0x34);
    expect_mem8("disk: the third byte of sector 1", m, VM86_BIOS_LOAD_LINEAR + 0x0602u, 0x0D);
    expect_mem8("disk: the fourth byte of sector 1", m, VM86_BIOS_LOAD_LINEAR + 0x0603u, 0x0A);

    /* And the buffer the failed read was pointed at is exactly as it was
     * planted: half-filling it would be worse than failing loudly. */
    expect_mem8("disk: the untouched buffer, byte 0", m, VM86_BIOS_LOAD_LINEAR + 0x0900u, 0xCD);
    expect_mem8("disk: the untouched buffer, byte 1", m, VM86_BIOS_LOAD_LINEAR + 0x0901u, 0xAB);
    expect_mem8("disk: the untouched buffer, byte 2", m, VM86_BIOS_LOAD_LINEAR + 0x0902u, 0x34);
    expect_mem8("disk: the untouched buffer, byte 3", m, VM86_BIOS_LOAD_LINEAR + 0x0903u, 0x12);
}

/* ------------------------------------------------------------------ */

struct bios_case {
    const char *name;
    void (*run)(struct vm86_bios_machine *machine);
};

static const struct bios_case cases[] = {
    { "hello/direct",     case_hello_and_direct },
    { "vectors",          case_vectors },
    { "cursor",           case_cursor },
    { "scroll",           case_scroll },
    { "timer",            case_timer },
    { "key",              case_key },
    { "disk",             case_disk },
};

int main(void)
{
    const size_t case_count = sizeof cases / sizeof cases[0];
    struct vm86_bios_machine *machine;

    if (!vm86_ops_build()) {
        printf("bios programs: two opcode groups claim the same opcode\n");
        printf("               every result below would be unreliable\n");
        return 2;
    }

    machine = vm86_bios_machine_new();

    if (!machine) {
        /*
         * Not a failure. There is no firmware in this tree yet, so there
         * is nothing to run the samples against, and a suite that failed
         * here would be reporting the absence of somebody else's work as
         * a defect in the corpus.
         */
        printf("\n=== bios programs: 7 samples, none run -- the four "
               "service modules are not in this tree yet ===\n");
        return 0;
    }

    printf("\n=== bios programs: %zu cases, 7 samples ===\n", case_count);
    for (size_t i = 0; i < case_count; i++) {
        int before = g_failures;

        cases[i].run(machine);

        printf("  %s  %s\n", g_failures == before ? "ok" : "--",
               cases[i].name);
    }

    printf("=== bios programs: %zu cases, %d difference(s) ===\n",
           case_count, g_failures);

    vm86_bios_machine_free(machine);

    return g_failures == 0 ? 0 : 1;
}
