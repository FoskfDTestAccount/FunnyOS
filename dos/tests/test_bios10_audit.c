/*
 * An adversarial read of INT 10h.
 *
 * ---------------------------------------------------------------------
 * What this is for, and what it is not
 *
 * dos/tests/test_bios10.c is the author's own suite: 143 assertions that
 * the module does what its author believed it does. This is a second
 * opinion, written without reading that file's expectations, from
 * docs/dos-refs.md section 1 (the video services) and section 7 (the BIOS
 * data area) rather than from bios10.c.
 *
 * The two are not redundant, because of what an author's suite cannot see.
 * A suite written alongside an implementation tests the cases the author
 * thought of, with the register values the author had in mind, and it
 * cannot tell "the code is right" from "the code and the test share a
 * mistake". The cases below are therefore chosen for what the original
 * suite *cannot distinguish*, and that is a deliberate selection:
 *
 *   - Scrolling is exercised with a rectangle whose four registers are
 *     all different. The author's cases scroll a window anchored at
 *     (0,0) with CL = DL = 0, which passes under any permutation of the
 *     four -- including the ones the task book warns about.
 *   - Forty columns is checked against the data area and against where
 *     page 1 starts, not only against a cell address.
 *   - The data area is compared with what INT 10h answers, because those
 *     are two independent readers of the same fact. The author's case
 *     compares the data area with constants.
 *   - The boundary in 06h between "scroll by fewer lines than the window
 *     is tall" and "as many or more" is walked from both sides.
 *
 * Where a case asserts something the sources do not settle, it says so,
 * and docs/tasks/M4-bios10-audit-report.md carries the reasoning and the
 * list of what was found.
 *
 * Nothing here writes to dos/bios/. The module belongs to its author; a
 * finding goes in the report.
 */
#include "harness.h"

#include <string.h>

#include <vm86/firmware.h>

#include "../bios/bios10.h"

/* ------------------------------------------------------------------ */
/* Reading the screen the way a program does                           */
/* ------------------------------------------------------------------ */

#define BDA ((uint32_t)VM86_BDA_SEGMENT << 4)

/*
 * The address of a cell, worked out from the state rather than from a
 * constant -- so a case in forty columns reads the forty-column layout,
 * and a case that wanted to catch a hard-coded 80 would not be fooled by
 * its own helper.
 */
static uint32_t cell_at(const struct bios10_state *st, uint8_t page,
                        uint16_t row, uint16_t col)
{
    return VM86_TEXT_BASE + (uint32_t)page * st->page_bytes
         + ((uint32_t)row * st->columns + col) * 2u;
}

static void poke(struct vm86_cpu *cpu, const struct bios10_state *st,
                 uint8_t page, uint16_t row, uint16_t col,
                 uint8_t ch, uint8_t attr)
{
    vm86_mem_write8(cpu->mem, cell_at(st, page, row, col), ch);
    vm86_mem_write8(cpu->mem, cell_at(st, page, row, col) + 1u, attr);
}

/* An attribute that depends on the row, so that "this cell still holds
 * what was there before" is distinguishable from "this cell holds what
 * the row below it held". */
static uint8_t attr_of(uint16_t row)
{
    return (uint8_t)(0x10u + row);
}

static uint8_t char_of(uint16_t row, uint16_t col)
{
    return (uint8_t)('A' + (row * 13u + col) % 26u);
}

/* Fill the whole page with a pattern that depends on both coordinates,
 * through the guest's own memory rather than through put_cell, so that
 * what is being asserted is the address arithmetic under test. */
static void fill_page(struct vm86_cpu *cpu, const struct bios10_state *st,
                      uint8_t page)
{
    for (uint16_t row = 0; row < BIOS10_ROWS; row++)
        for (uint16_t col = 0; col < st->columns; col++)
            poke(cpu, st, page, row, col, char_of(row, col), attr_of(row));
}

static void expect_cell(struct vm86_cpu *cpu, const struct bios10_state *st,
                        const char *what, uint8_t page,
                        uint16_t row, uint16_t col, uint8_t ch, uint8_t attr)
{
    vm86_expect_mem8(what, cpu, cell_at(st, page, row, col), ch);
    vm86_expect_mem8(what, cpu, cell_at(st, page, row, col) + 1u, attr);
}

/* ------------------------------------------------------------------ */
/* Driving the service                                                 */
/* ------------------------------------------------------------------ */

static void call(struct vm86_cpu *cpu, struct bios10_state *st, uint8_t ah)
{
    cpu->ah = ah;
    bios10_service(cpu, st);
}

/* Mode 3, cleared, the way every case below wants to start. */
static void start(struct vm86_cpu *cpu, struct bios10_state *st)
{
    bios10_reset(st);
    cpu->al = 0x03u;
    call(cpu, st, 0x00u);
}

/* Put the cursor somewhere, through the service rather than by writing
 * the state, so the case goes through the same door a program does. */
static void cursor_to(struct vm86_cpu *cpu, struct bios10_state *st,
                      uint8_t page, uint8_t row, uint8_t col)
{
    cpu->bh = page;
    cpu->dh = row;
    cpu->dl = col;
    call(cpu, st, 0x02u);
}

/* ------------------------------------------------------------------ */
/* Scrolling: the four registers                                       */
/* ------------------------------------------------------------------ */

/*
 * A window whose four corner registers are all different, and are not
 * zero.
 *
 * This is the case the author's suite cannot have: its scrolling cases
 * use CH = 0, CL = 0, and a bottom-right of (2,0) or (0,9), so two of the
 * four registers are zero in every one of them. An implementation that
 * read the rectangle as (CH, DL) to (DH, CL), or that took CL as the row
 * and CH as the column, gives exactly the same answer as the correct one
 * on a window whose left and right are both zero -- and gives a
 * different, entirely plausible answer here.
 *
 * The pattern depends on the row *and* the column, so a rectangle that
 * came out transposed moves different data into the window and is caught
 * by the content as well as by the cells outside it.
 */
static void test_a_window_that_is_not_anchored_at_the_origin(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    start(cpu, &st);
    fill_page(cpu, &st, 0);

    /* Rows 2 through 6, columns 5 through 11: nine rows and fourteen
     * columns away from being the whole screen, in both directions. */
    cpu->al = 1;                    /* one line */
    cpu->bh = 0x4E;                 /* the fill colour */
    cpu->ch = 2;                    /* top row */
    cpu->cl = 5;                    /* left column */
    cpu->dh = 6;                    /* bottom row */
    cpu->dl = 11;                   /* right column */
    call(cpu, &st, 0x06u);

    /* Inside: every row but the last holds what the row below it held. */
    for (uint16_t row = 2; row <= 5; row++)
        for (uint16_t col = 5; col <= 11; col++)
            expect_cell(cpu, &st, "the window scrolled up by one",
                        0, row, col,
                        char_of((uint16_t)(row + 1), col), attr_of((uint16_t)(row + 1)));

    /* The last row of the window is blank, in the colour asked for. */
    for (uint16_t col = 5; col <= 11; col++)
        expect_cell(cpu, &st, "and its bottom row is blank", 0, 6, col,
                    ' ', 0x4E);

    /* Outside, on all four sides, nothing moved. */
    expect_cell(cpu, &st, "the row above the window", 0, 1, 8,
                char_of(1, 8), attr_of(1));
    expect_cell(cpu, &st, "the row below it", 0, 7, 8, char_of(7, 8),
                attr_of(7));
    expect_cell(cpu, &st, "the column left of it", 0, 3, 4, char_of(3, 4),
                attr_of(3));
    expect_cell(cpu, &st, "the column right of it", 0, 3, 12, char_of(3, 12),
                attr_of(3));

    /* And the four corners of the window itself, which a transposed
     * rectangle would fill from somewhere else entirely. */
    expect_cell(cpu, &st, "the window's top-left", 0, 2, 5, char_of(3, 5),
                attr_of(3));
    expect_cell(cpu, &st, "its top-right", 0, 2, 11, char_of(3, 11),
                attr_of(3));
}

/*
 * The same window, scrolled the other way, through 07h.
 */
static void test_the_same_window_scrolled_down(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    start(cpu, &st);
    fill_page(cpu, &st, 0);

    cpu->al = 1;
    cpu->bh = 0x4E;
    cpu->ch = 2;
    cpu->cl = 5;
    cpu->dh = 6;
    cpu->dl = 11;
    call(cpu, &st, 0x07u);

    for (uint16_t row = 3; row <= 6; row++)
        for (uint16_t col = 5; col <= 11; col++)
            expect_cell(cpu, &st, "the window scrolled down by one",
                        0, row, col,
                        char_of((uint16_t)(row - 1), col), attr_of((uint16_t)(row - 1)));

    for (uint16_t col = 5; col <= 11; col++)
        expect_cell(cpu, &st, "and its top row is blank", 0, 2, col, ' ',
                    0x4E);

    expect_cell(cpu, &st, "the row above the window", 0, 1, 8, char_of(1, 8),
                attr_of(1));
    expect_cell(cpu, &st, "the row below it", 0, 7, 8, char_of(7, 8),
                attr_of(7));
    expect_cell(cpu, &st, "the column left of it", 0, 3, 4, char_of(3, 4),
                attr_of(3));
    expect_cell(cpu, &st, "the column right of it", 0, 3, 12, char_of(3, 12),
                attr_of(3));
}

/*
 * The boundary between "fewer lines than the window is tall" and "as many
 * or more".
 *
 * docs/dos-refs.md section 1 gives AL = 0 as "fill the whole window" and
 * the vgabios source used for it clamps a count of more lines than the
 * window has to zero -- which is the same instruction, so both are a
 * blank. Nothing in the reference says what the row in between does, so
 * the case walks across it: two lines of a three-row window is a scroll,
 * and three is a clear.
 */
static void test_the_line_count_walks_across_the_window_height(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    start(cpu, &st);
    fill_page(cpu, &st, 0);

    /* A three-row window, rows 10..12, columns 0..9. */
    cpu->al = 2;                    /* one short of the height */
    cpu->bh = 0x4E;
    cpu->ch = 10;
    cpu->cl = 0;
    cpu->dh = 12;
    cpu->dl = 9;
    call(cpu, &st, 0x06u);

    expect_cell(cpu, &st, "two lines moved row 12 into row 10",
                0, 10, 3, char_of(12, 3), attr_of(12));
    expect_cell(cpu, &st, "row 11 is blank", 0, 11, 3, ' ', 0x4E);
    expect_cell(cpu, &st, "and row 12 below it too", 0, 12, 3, ' ', 0x4E);
    expect_cell(cpu, &st, "the row above the window is untouched",
                0, 9, 3, char_of(9, 3), attr_of(9));

    /* One more line than the window is tall, which is the same thing as
     * asking for none: the whole window is blank. */
    cpu->al = 3;
    cpu->bh = 0x4E;
    cpu->ch = 10;
    cpu->cl = 0;
    cpu->dh = 12;
    cpu->dl = 9;
    call(cpu, &st, 0x06u);

    for (uint16_t row = 10; row <= 12; row++)
        for (uint16_t col = 0; col <= 9; col++)
            expect_cell(cpu, &st, "a window-height scroll blanks the window",
                        0, row, col, ' ', 0x4E);

    expect_cell(cpu, &st, "and still leaves the row above it",
                0, 9, 3, char_of(9, 3), attr_of(9));
    expect_cell(cpu, &st, "and the column right of it",
                0, 11, 10, char_of(11, 10), attr_of(11));
}

/* ------------------------------------------------------------------ */
/* Forty columns                                                       */
/* ------------------------------------------------------------------ */

/*
 * Forty columns is not eighty with a shorter row: the page stride changes
 * with it, and every address that a program or the host computes comes
 * out of those two numbers.
 *
 * doc/dos-refs.md section 7 gives the field as 2048 for a forty-column
 * mode -- 40 x 25 x 2 is 2000, and the field is the page's stride rounded
 * up to a power of two, which is the same reasoning that makes the
 * eighty-column page 4096 rather than 4000.
 *
 * The author's forty-column case checks the two fields and one cell. What
 * it does not check is where *page 1* starts in that mode, which is the
 * only place the stride is visible outside the cell arithmetic -- and the
 * only place a hard-coded 4096 shows up.
 */
static void test_forty_columns_changes_the_page_as_well_as_the_row(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    start(cpu, &st);

    cpu->al = 0x00u;
    call(cpu, &st, 0x00u);

    vm86_expect_u16("columns", st.columns, 40);
    vm86_expect_u16("page stride", st.page_bytes, 2048);

    /* The data area, read directly: a program that sizes its own output
     * reads these two words and not the service. */
    vm86_expect_mem16("0040:004A", cpu, BDA + VM86_BDA_COLUMNS, 40);
    vm86_expect_mem16("0040:004C", cpu, BDA + VM86_BDA_PAGE_BYTES, 2048);

    /* Where the second page is, in this mode: 2048 bytes along, not
     * 4096. Through the accessor the host uses, so this is the address a
     * renderer would be given. */
    cursor_to(cpu, &st, 0, 0, 0);
    cpu->al = 1;
    call(cpu, &st, 0x05u);

    const uint8_t *page1 = bios10_active_page(cpu, &st);

    vm86_expect_bool("page 1 is inside the guest's memory", page1 != NULL, true);
    vm86_expect_u16("and starts 2048 bytes into the video window, not 4096",
                    (uint16_t)((uint32_t)(page1 - cpu->mem->ram)
                               - VM86_TEXT_BASE),
                    2048);
    vm86_expect_u16("which is what the data area says the stride is",
                    vm86_mem_read16(cpu->mem, BDA + VM86_BDA_PAGE_BYTES),
                    2048);

    /* A write on page 1 goes there, and the same bytes of page 0 are not
     * touched: with the wrong stride the two would overlap by half. */
    cpu->al = 'S';
    call(cpu, &st, 0x0Eu);

    vm86_expect_mem8("page 1's first cell", cpu,
                     VM86_TEXT_BASE + 2048u, 'S');
    vm86_expect_mem8("page 0's cell 1024 -- the middle of page 1's own row -- "
                     "is somewhere else", cpu,
                     VM86_TEXT_BASE + 2048u - 2u, ' ');
}

/*
 * The same, for the row: in forty columns a run of characters wraps after
 * forty of them, and the second row starts 80 bytes into the page rather
 * than 160.
 */
static void test_a_run_in_forty_columns_wraps_at_forty(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    start(cpu, &st);
    cpu->al = 0x00u;
    call(cpu, &st, 0x00u);

    cursor_to(cpu, &st, 0, 0, 38);

    cpu->al = 'x';
    cpu->bl = 0x1Fu;
    cpu->bh = 0;
    cpu->cx = 4;                    /* two before the edge, two after */
    call(cpu, &st, 0x09u);

    expect_cell(cpu, &st, "the first two are on row 0", 0, 0, 38, 'x', 0x1F);
    expect_cell(cpu, &st, "and the next", 0, 0, 39, 'x', 0x1F);
    expect_cell(cpu, &st, "the third wrapped to row 1", 0, 1, 0, 'x', 0x1F);
    expect_cell(cpu, &st, "and so did the fourth", 0, 1, 1, 'x', 0x1F);

    /* Which is 80 bytes in, and specifically not 160. */
    vm86_expect_mem8("row 1 starts eighty bytes into the page", cpu,
                     VM86_TEXT_BASE + 80u, 'x');
    vm86_expect_mem8("and nothing was written a hundred and sixty in", cpu,
                     VM86_TEXT_BASE + 160u, ' ');
}

/* ------------------------------------------------------------------ */
/* Which page a write lands on                                         */
/* ------------------------------------------------------------------ */

/*
 * Two functions, two rules, and the reference says so: 09h takes its page
 * in BH, and the teletype takes the page being displayed (docs/dos-refs.md
 * section 1, from the vgabios comment that Ralf Brown's list is wrong on
 * exactly this point).
 *
 * The case is set up so that the two answers differ: page 1 is displayed,
 * and BH names page 0. A program that has selected a page and then prints
 * must land on the page it selected; a program that draws with 09h names
 * its page explicitly and must land where it said.
 */
static void test_the_teletype_follows_the_display_and_09h_follows_bh(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    start(cpu, &st);

    /* Page 0 gets a character at its home cell, through BH. */
    cpu->bh = 0;
    cpu->al = '0';
    cpu->bl = 0x0Au;
    cpu->cx = 1;
    call(cpu, &st, 0x09u);

    /* Page 0's cursor is then moved well away from the origin, so that a
     * write that lands on the right page but the wrong cursor is visible
     * -- and so that the two pages' cursors are not both at the same
     * place. */
    cursor_to(cpu, &st, 0, 0, 5);

    /* Now display page 1 and print, naming page 0 in BH as a program that
     * has not caught up might. */
    cpu->al = 1;
    call(cpu, &st, 0x05u);

    cpu->bh = 0;                    /* ignored by the teletype */
    cpu->al = '1';
    call(cpu, &st, 0x0Eu);

    expect_cell(cpu, &st, "the teletype wrote on the displayed page",
                1, 0, 0, '1', VM86_ATTR_DEFAULT);
    expect_cell(cpu, &st, "and moved that page's cursor", 0, 0, 0, '0', 0x0A);

    /* 09h, with page 1 still displayed and BH still naming page 0.
     *
     * This is the call the case is named for, and the first version of it
     * had the two agreeing -- 09h was called while page 0 was displayed
     * and BH was 0 -- so the mutation that made 09h use the displayed
     * page passed it. They have to disagree for the rule to be under
     * test. */
    cpu->bh = 0;
    cpu->al = 'Z';
    cpu->bl = 0x07;
    cpu->cx = 1;
    call(cpu, &st, 0x09u);

    expect_cell(cpu, &st, "09h wrote on the page BH named, at that page's "
                "own cursor", 0, 0, 5, 'Z', 0x07);
    expect_cell(cpu, &st, "and not on the page being displayed",
                1, 0, 1, ' ', VM86_ATTR_DEFAULT);
    expect_cell(cpu, &st, "and it did not move either cursor",
                0, 0, 0, '0', 0x0A);

    /* Each page keeps its own cursor: printing on page 1 did not move page
     * 0's, and the write above did not move page 1's. */
    cpu->bh = 0;
    call(cpu, &st, 0x03u);

    vm86_expect_u16("page 0's cursor", cpu->dl, 5);
    vm86_expect_u16("and its row", cpu->dh, 0);

    /* And the service agrees with the data area about which page is on
     * screen. */
    call(cpu, &st, 0x0Fu);

    vm86_expect_u16("0Fh reports the active page", cpu->bh, 1);
    vm86_expect_mem8("and 0040:0062 says the same", cpu,
                     BDA + VM86_BDA_ACTIVE_PAGE, 1);
}

/*
 * Reading is the same rule as writing: 08h names its page in BH and reads
 * it whether or not it is the one on screen. A program that keeps a
 * second screenful off to one side and copies parts of it into view reads
 * it this way.
 */
static void test_08h_reads_a_page_that_is_not_displayed(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    start(cpu, &st);

    /* The character is on page 1, and page 1's own cursor is on it. */
    poke(cpu, &st, 1, 4, 7, 'k', 0x2E);
    cursor_to(cpu, &st, 1, 4, 7);

    /* Page 0 is what is displayed, and its cell 4,7 is blank. */
    cpu->al = 0;
    call(cpu, &st, 0x05u);

    cpu->bh = 1;
    call(cpu, &st, 0x08u);

    vm86_expect_u16("the character came from the page BH named", cpu->al,
                    'k');
    vm86_expect_u16("and so did the attribute", cpu->ah, 0x2E);

    /* And asking for the blank page gives the blank page, so the case is
     * not passing because both pages hold the same thing. */
    cpu->bh = 0;
    call(cpu, &st, 0x08u);

    vm86_expect_u16("page 0's cell is empty", cpu->al, ' ');
    vm86_expect_u16("with the attribute the mode set left", cpu->ah,
                    VM86_ATTR_DEFAULT);
}

/* ------------------------------------------------------------------ */
/* The data area and the service are the same answer                   */
/* ------------------------------------------------------------------ */

/*
 * Two readers, one fact.
 *
 * Every video value can be had two ways -- out of 0040:xxxx, or out of an
 * INT 10h whose whole purpose is to hand it back -- and a program is free
 * to use either. They have to agree, and the way to check that is to ask
 * both and compare, not to check each against a constant: two constants
 * can be wrong together, and the author's case for this compares the data
 * area with constants.
 *
 * The comparison is done after a sequence of operations, because the
 * interesting failure is a handler that updates its own copy and forgets
 * the data area. It is done in forty columns on a non-zero page as well
 * as in eighty on page 0, because a value that is right by coincidence in
 * the default mode is the kind that survives.
 */
static void test_the_two_readers_of_the_mode_agree(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    start(cpu, &st);

    /* Leave eighty columns and page 0 behind, so that "everything is at
     * its default" cannot be what makes this pass. */
    cpu->al = 0x01u;                /* forty columns, colour */
    call(cpu, &st, 0x00u);

    cursor_to(cpu, &st, 3, 0, 0);
    cpu->al = 3;
    call(cpu, &st, 0x05u);          /* page 3 is displayed */

    cpu->ch = 0x0B;                 /* a shape that is not the default */
    cpu->cl = 0x0C;
    call(cpu, &st, 0x01u);

    cursor_to(cpu, &st, 3, 9, 21);  /* a position on the displayed page */

    /* Ask the service. */
    call(cpu, &st, 0x0Fu);

    uint8_t mode    = cpu->al;
    uint8_t columns = cpu->ah;
    uint8_t page    = cpu->bh;

    cpu->bh = 3;
    call(cpu, &st, 0x03u);

    uint8_t row = cpu->dh;
    uint8_t col = cpu->dl;
    uint16_t shape = cpu->cx;

    /* Read the data area. */
    vm86_expect_mem8 ("0040:0049 is the mode the service reported",
                      cpu, BDA + VM86_BDA_VIDEO_MODE, mode);
    vm86_expect_mem16("0040:004A is the width it reported",
                      cpu, BDA + VM86_BDA_COLUMNS, columns);
    vm86_expect_mem8 ("0040:0062 is the page it reported",
                      cpu, BDA + VM86_BDA_ACTIVE_PAGE, page);
    vm86_expect_mem16("0040:0050 + 2*page is the cursor it reported",
                      cpu, BDA + VM86_BDA_CURSOR + page * 2u,
                      (uint16_t)((row << 8) | col));
    vm86_expect_mem16("0040:0060 is the shape it reported",
                      cpu, BDA + VM86_BDA_CURSOR_SHAPE, shape);

    /* And the stride, which no INT 10h call returns but which the host
     * and any program computing a page address need: it has to be the
     * distance between page 0 and page 1 as the accessor reports them. */
    cpu->al = 0;
    call(cpu, &st, 0x05u);
    const uint8_t *first = bios10_active_page(cpu, &st);

    cpu->al = 1;
    call(cpu, &st, 0x05u);
    const uint8_t *second = bios10_active_page(cpu, &st);

    vm86_expect_u16("the stride the data area gives is the one the host sees",
                    (uint16_t)((uint32_t)(second - first)),
                    vm86_mem_read16(cpu->mem, BDA + VM86_BDA_PAGE_BYTES));
}

/*
 * The fields of the data area that are not the video's.
 *
 * firmware.h says each field belongs to whichever service owns it and
 * that nothing else may write across the area, and the video service is
 * the one that ends every call by publishing its own fields -- so it is
 * the one that would notice if it published a little too much. The
 * equipment word, the keyboard flags and the tick count are three other
 * people's, and a video call must not touch them.
 */
static void test_a_video_call_writes_only_its_own_fields(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    start(cpu, &st);

    /* Four fields that belong to the machine (11h), the keyboard and the
     * clock. */
    vm86_mem_write16(cpu->mem, BDA + VM86_BDA_EQUIPMENT, 0x21);
    vm86_mem_write8 (cpu->mem, BDA + VM86_BDA_KEYBOARD_FLAGS, 0x42);
    vm86_mem_write8 (cpu->mem, BDA + VM86_BDA_KEYBOARD_FLAGS2, 0x43);
    vm86_mem_write16(cpu->mem, BDA + VM86_BDA_KB_HEAD, 0x1E);
    vm86_mem_write16(cpu->mem, BDA + VM86_BDA_KB_TAIL, 0x1E);
    vm86_mem_write16(cpu->mem, BDA + VM86_BDA_TICK_COUNT, 0x1234);

    /* A sequence that ends on every path that publishes: a mode set, a
     * cursor move, an output, a scroll, and a read. */
    cpu->al = 0x03u;
    call(cpu, &st, 0x00u);
    cursor_to(cpu, &st, 0, 5, 5);
    cpu->al = 'Q';
    call(cpu, &st, 0x0Eu);
    cpu->al = 1;
    cpu->bh = 0x07;
    cpu->ch = 0;
    cpu->cl = 0;
    cpu->dh = 24;
    cpu->dl = 79;
    call(cpu, &st, 0x06u);
    cpu->bh = 0;
    call(cpu, &st, 0x08u);
    call(cpu, &st, 0x0Fu);
    call(cpu, &st, 0xFEu);          /* and one it does not have */

    vm86_expect_mem16("the equipment word", cpu, BDA + VM86_BDA_EQUIPMENT,
                      0x21);
    vm86_expect_mem8("the keyboard flags", cpu, BDA + VM86_BDA_KEYBOARD_FLAGS,
                     0x42);
    vm86_expect_mem8("the second bank of them", cpu,
                     BDA + VM86_BDA_KEYBOARD_FLAGS2, 0x43);
    vm86_expect_mem16("the keyboard queue's head", cpu, BDA + VM86_BDA_KB_HEAD,
                      0x1E);
    vm86_expect_mem16("its tail", cpu, BDA + VM86_BDA_KB_TAIL, 0x1E);
    vm86_expect_mem16("the low half of the tick count", cpu, BDA + 0x006Cu,
                      0x1234);
}

/* ------------------------------------------------------------------ */
/* When the data area is filled                                        */
/* ------------------------------------------------------------------ */

/*
 * This case is about a gap rather than about the module, and it is here
 * so that the gap shows up in a test run and not only in a report.
 *
 * bios10_reset leaves the state describing a machine in mode 3 -- and
 * touches no memory at all, which its header says is deliberate. The
 * firmware's own power-on code (dos/intr/firmware.c) writes the equipment
 * word and the memory size and says the rest belongs to the services,
 * "their resets fill them". Nothing fills them: there is no exported way
 * to publish, and publishing happens as a side effect of servicing a
 * call.
 *
 * So a machine that has been reset but not yet asked anything has a data
 * area that does not describe it, and a program that reads 0040:004A for
 * the screen width -- which is the cheap way to ask, and the reason the
 * field exists -- reads zero. See the report; if this is fixed, this case
 * has to be inverted, and that is the reason it is written down.
 */
static void test_the_data_area_is_empty_until_the_firmware_is_asked(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    bios10_reset(&st);

    vm86_expect_u16("the state is a machine in mode 3", st.mode, 0x03);
    vm86_expect_u16("eighty columns", st.columns, 80);

    vm86_expect_mem8("but 0040:0049 is not filled in yet", cpu,
                     BDA + VM86_BDA_VIDEO_MODE, 0x00);
    vm86_expect_mem16("nor is the width a program would read", cpu,
                      BDA + VM86_BDA_COLUMNS, 0x0000);
    vm86_expect_mem16("nor the page stride", cpu,
                      BDA + VM86_BDA_PAGE_BYTES, 0x0000);

    /* One call -- any call -- publishes the lot, which is what a machine
     * has to do before it runs a program. */
    call(cpu, &st, 0x0Fu);

    vm86_expect_mem8("after one question 0040:0049 is the mode", cpu,
                     BDA + VM86_BDA_VIDEO_MODE, 0x03);
    vm86_expect_mem16("and 0040:004A is the width", cpu,
                      BDA + VM86_BDA_COLUMNS, 80);
    vm86_expect_mem16("and 0040:004C is the stride", cpu,
                      BDA + VM86_BDA_PAGE_BYTES, 4096);

    /* "Any call" includes one this machine does not have. The publish is
     * not inside the switch -- it is what every service does on its way
     * out -- and a version that returned early from the unhandled case
     * would leave the data area describing the machine as it was before
     * the call. That is a difference no other case here can see, because
     * they all ask for something that exists. */
    cpu->al = 0x00u;
    call(cpu, &st, 0x00u);          /* forty columns, and published */
    vm86_expect_mem16("forty columns is published", cpu,
                      BDA + VM86_BDA_COLUMNS, 40);

    cpu->al = 0x03u;
    call(cpu, &st, 0x00u);          /* back to eighty */
    vm86_expect_mem16("and so is eighty", cpu,
                      BDA + VM86_BDA_COLUMNS, 80);

    /* The data area is made wrong on purpose before the next call, so that
     * "it was already right" cannot pass for "it was published". The state
     * is the truth and the data area is a copy of it; a stale copy has to
     * be corrected by anything that runs, including a function this
     * machine does not have. */
    vm86_mem_write16(cpu->mem, BDA + VM86_BDA_COLUMNS, 0x9999);

    call(cpu, &st, 0x7Fu);          /* a function nobody has */

    vm86_expect_mem16("an unknown function publishes too", cpu,
                      BDA + VM86_BDA_COLUMNS, 80);
    vm86_expect_mem8("with the mode the machine was in", cpu,
                     BDA + VM86_BDA_VIDEO_MODE, 0x03);
}

/* ------------------------------------------------------------------ */
/* The attribute a teletype write keeps                                */
/* ------------------------------------------------------------------ */

/*
 * The rule the module's own header calls its first disagreement with the
 * task book: 0Eh writes the character and leaves the attribute byte of
 * the cell alone.
 *
 * The author's case writes into one cell. This one writes a run across
 * cells that have *different* attributes, because the failure the rule is
 * about is not "the attribute changed" -- an implementation that writes a
 * single constant attribute changes every one of them, and one that takes
 * BL changes them to BL -- but "the screen forgot what colour it was".
 */
static void test_a_teletype_run_keeps_each_cell_s_own_colour(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    start(cpu, &st);

    /* Eight cells, eight colours, on row 3. */
    for (uint16_t col = 0; col < 8; col++)
        poke(cpu, &st, 0, 3, col, '.', (uint8_t)(0x01u + col * 0x10u));

    cursor_to(cpu, &st, 0, 3, 0);

    cpu->bl = 0x00;                 /* what a program that never set BL has */
    for (uint16_t col = 0; col < 8; col++) {
        cpu->al = 'Z';
        call(cpu, &st, 0x0Eu);
    }

    for (uint16_t col = 0; col < 8; col++)
        expect_cell(cpu, &st, "the character changed, the colour did not",
                    0, 3, col, 'Z', (uint8_t)(0x01u + col * 0x10u));
}

/* ------------------------------------------------------------------ */

static const struct vm86_test tests[] = {
    { "06h scrolls a window that is not anchored at the origin",
      test_a_window_that_is_not_anchored_at_the_origin },
    { "07h scrolls the same window the other way",
      test_the_same_window_scrolled_down },
    { "the line count walks across the window height",
      test_the_line_count_walks_across_the_window_height },
    { "forty columns changes the page as well as the row",
      test_forty_columns_changes_the_page_as_well_as_the_row },
    { "a run in forty columns wraps at forty",
      test_a_run_in_forty_columns_wraps_at_forty },
    { "the teletype follows the display and 09h follows BH",
      test_the_teletype_follows_the_display_and_09h_follows_bh },
    { "08h reads a page that is not displayed",
      test_08h_reads_a_page_that_is_not_displayed },
    { "the two readers of the mode agree",
      test_the_two_readers_of_the_mode_agree },
    { "a video call writes only its own fields",
      test_a_video_call_writes_only_its_own_fields },
    { "the data area is empty until the firmware is asked",
      test_the_data_area_is_empty_until_the_firmware_is_asked },
    { "a teletype run keeps each cell's own colour",
      test_a_teletype_run_keeps_each_cell_s_own_colour },
};

VM86_TEST_MAIN("bios10_audit", tests)
