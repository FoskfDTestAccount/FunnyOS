/*
 * INT 10h in text mode, checked against a display that records what it was
 * shown.
 *
 * ---------------------------------------------------------------------
 * Why the tests drive the host themselves
 *
 * Nothing here ever looks at a screen. The video BIOS's job is to leave the
 * right bytes in guest memory and the right values in the BIOS data area,
 * and "the character appeared" is only an assertion if something copies
 * those bytes somewhere a test can compare against. So each case ends by
 * doing what the host does -- asking for the active page and the cursor and
 * handing both to vm86_display_present() -- with a backend that stores
 * them.
 *
 * That arrangement also means the two accessors the host depends on are
 * exercised by every single case, rather than by one test that could rot on
 * its own.
 *
 * ---------------------------------------------------------------------
 * On the assertions that pin a choice
 *
 * Several of these cases assert behaviour that the task book says is a
 * decision rather than a fact, and they are written to say which decision:
 * the attribute a teletype write keeps, the attribute a teletype scroll
 * fills with, and what an unimplemented mode does. A test that asserts a
 * choice is worth having -- it is the thing that stops the choice being
 * changed by accident -- but only if the choice is written down, and that
 * is the report's job rather than this file's.
 */
#include "harness.h"

#include <string.h>

#include <vm86/firmware.h>

#include "../bios/bios10.h"

/*
 * The include above reaches across directories, which nothing else in this
 * directory does. It has to: the header lives next to its source, and the
 * build only puts `include/` and libk on the search path, so a path from
 * here is the one spelling that compiles without editing the Makefile --
 * which is not this task's to edit.
 */

/* ------------------------------------------------------------------ */
/* A display that keeps what it was shown                              */
/* ------------------------------------------------------------------ */

/* Eighty by twenty-five cells, which is the largest page this machine
 * has. A forty-column mode fills the first half of it. */
#define FAKE_CELLS (80u * BIOS10_ROWS)

struct fake_screen {
    uint8_t  cell[FAKE_CELLS][2];   /* character, then attribute        */
    uint16_t cursor;                /* what the host was told to draw   */
    unsigned calls;                 /* how often it was asked           */
    bool     last_had_page;         /* whether it was given a page      */
};

static void fake_present(void *ctx, const uint8_t *cells, uint16_t cursor)
{
    struct fake_screen *f = ctx;

    f->calls++;
    f->cursor          = cursor;
    f->last_had_page   = cells != NULL;

    memset(f->cell, 0, sizeof(f->cell));
    if (cells != NULL)
        memcpy(f->cell, cells, sizeof(f->cell));
}

/* One host refresh, exactly as the real host does it: ask the BIOS where
 * the page is and where the cursor is, then hand both to the backend. */
static void host_render(struct vm86_cpu *cpu, struct bios10_state *st,
                        struct fake_screen *f)
{
    const struct vm86_display display = { fake_present, f };

    vm86_display_present(&display, bios10_active_page(cpu, st),
                         bios10_cursor_cell(cpu, st));
}

static void fake_init(struct fake_screen *f)
{
    memset(f, 0, sizeof(*f));
}

/* ------------------------------------------------------------------ */
/* Driving the service                                                 */
/* ------------------------------------------------------------------ */

static void call(struct vm86_cpu *cpu, struct bios10_state *st, uint8_t ah)
{
    cpu->ah = ah;
    bios10_service(cpu, st);
}

/* A machine that has just set mode 3, which is the state every case below
 * either wants or is about. It comes with the screen cleared, because
 * setting a mode clears it -- a case that wants uncleared memory has to
 * say so by poking it, which is how the "screen not cleared" case reads. */
static void start_text_screen(struct vm86_cpu *cpu, struct bios10_state *st)
{
    bios10_reset(st);
    cpu->al = 0x03;
    call(cpu, st, 0x00);
}

/* Where a cell is, worked out from the state the caller gave rather than
 * from a constant, so that a case in forty columns reads correctly. The
 * stride is page_bytes on purpose: it is the number under test. */
static uint32_t cell_at(const struct bios10_state *st, uint8_t page,
                        uint16_t row, uint16_t col)
{
    return VM86_TEXT_BASE + (uint32_t)page * st->page_bytes
         + ((uint32_t)row * st->columns + col) * 2u;
}

static void expect_cell(struct vm86_cpu *cpu, const struct bios10_state *st,
                        const char *what, uint8_t page,
                        uint16_t row, uint16_t col, uint8_t ch, uint8_t attr)
{
    uint32_t at = cell_at(st, page, row, col);

    vm86_expect_mem8(what, cpu, at, ch);
    vm86_expect_mem8(what, cpu, at + 1u, attr);
}

/* The cursor, read back through the accessor the host uses. */
static void expect_cursor(struct vm86_cpu *cpu, const struct bios10_state *st,
                          const char *what, uint16_t want)
{
    vm86_expect_u16(what, bios10_cursor_cell(cpu, st), want);
}

/* Put a character and attribute on the screen without going through the
 * service, the way a program that writes 0xB8000 itself would. */
static void poke(struct vm86_cpu *cpu, const struct bios10_state *st,
                 uint8_t page, uint16_t row, uint16_t col,
                 uint8_t ch, uint8_t attr)
{
    uint32_t at = cell_at(st, page, row, col);

    vm86_mem_write8(cpu->mem, at, ch);
    vm86_mem_write8(cpu->mem, at + 1u, attr);
}

/* ------------------------------------------------------------------ */
/* AH=0Eh, the teletype                                                */
/* ------------------------------------------------------------------ */

static void test_0e_prints_and_advances(struct vm86_cpu *cpu)
{
    struct bios10_state st;
    struct fake_screen  f;

    fake_init(&f);
    start_text_screen(cpu, &st);

    const char *msg = "Hi";

    for (; *msg != '\0'; msg++) {
        cpu->al = (uint8_t)*msg;
        call(cpu, &st, 0x0E);
    }

    host_render(cpu, &st, &f);

    vm86_expect_u16("present was called once", f.calls, 1);
    vm86_expect_bool("a page was handed over", f.last_had_page, true);
    vm86_expect_u16("cursor is past the text", f.cursor, 2);
    vm86_expect_u16("cell 0 character", f.cell[0][0], 'H');
    vm86_expect_u16("cell 0 attribute", f.cell[0][1], VM86_ATTR_DEFAULT);
    vm86_expect_u16("cell 1 character", f.cell[1][0], 'i');
    vm86_expect_u16("cell 1 attribute", f.cell[1][1], VM86_ATTR_DEFAULT);
    vm86_expect_u16("cell 2 is still blank", f.cell[2][0], ' ');
}

/*
 * The attribute comes from the cell, and BL is not read at all.
 *
 * This is the case that would fail if the teletype took its colour from
 * BL, which is what the task book describes. The cell is given a colour
 * the default is not, BL is given a third colour, and the assertion is
 * that the cell kept its own.
 */
static void test_0e_keeps_the_attribute_of_the_cell(struct vm86_cpu *cpu)
{
    struct bios10_state st;
    struct fake_screen  f;

    fake_init(&f);
    start_text_screen(cpu, &st);

    poke(cpu, &st, 0, 0, 0, ' ', 0x1E);

    cpu->bl = 0x4F;                 /* the colour, if BL were read */
    cpu->al = 'X';
    call(cpu, &st, 0x0E);

    host_render(cpu, &st, &f);

    vm86_expect_u16("character written", f.cell[0][0], 'X');
    vm86_expect_u16("the cell kept its colour", f.cell[0][1], 0x1E);
}

static void test_0e_wraps_at_the_end_of_a_row(struct vm86_cpu *cpu)
{
    struct bios10_state st;
    struct fake_screen  f;

    fake_init(&f);
    start_text_screen(cpu, &st);

    cpu->bh = 0;
    cpu->dh = 0;
    cpu->dl = 79;
    call(cpu, &st, 0x02);

    cpu->al = 'X';
    call(cpu, &st, 0x0E);

    host_render(cpu, &st, &f);

    vm86_expect_u16("last cell of the row", f.cell[79][0], 'X');
    vm86_expect_u16("cursor wrapped to the next row", f.cursor, 80);
    vm86_expect_u16("first cell of the next row is untouched",
                    f.cell[80][0], ' ');
}

/*
 * Scrolling at the bottom, and the attribute the new blank line gets.
 *
 * The screen is prepared so that the two are separable: row 1 has a
 * character in it that has to end up in row 0, and row 0 of the bottom
 * line has a colour that is not the default, so that a fill of a fixed
 * 0x07 and a fill taken from the screen are different answers.
 */
static void test_0e_scrolls_at_the_bottom(struct vm86_cpu *cpu)
{
    struct bios10_state st;
    struct fake_screen  f;

    fake_init(&f);
    start_text_screen(cpu, &st);

    poke(cpu, &st, 0, 0,  0, 'A', VM86_ATTR_DEFAULT);
    poke(cpu, &st, 0, 1,  0, 'B', VM86_ATTR_DEFAULT);
    poke(cpu, &st, 0, 24, 0, ' ', 0x1E);

    cpu->bh = 0;
    cpu->dh = 24;
    cpu->dl = 79;
    call(cpu, &st, 0x02);

    cpu->al = 'Z';
    call(cpu, &st, 0x0E);

    host_render(cpu, &st, &f);

    vm86_expect_u16("the row that was second is now first",
                    f.cell[0][0], 'B');
    vm86_expect_u16("and the row it came from is now blank",
                    f.cell[80][0], ' ');
    vm86_expect_bool("what used to be first is nowhere on the screen",
                     f.cell[0][0] == 'A' || f.cell[80][0] == 'A', false);
    /* The character was written at the last cell and *then* the screen
     * scrolled, so it rode up a row -- which is what a terminal does and
     * what makes the bottom line the one that was vacated. */
    vm86_expect_u16("the character that caused the scroll rode up a row",
                    f.cell[23 * 80 + 79][0], 'Z');
    vm86_expect_u16("the last row is blank where it used to be",
                    f.cell[24 * 80 + 79][0], ' ');
    vm86_expect_u16("and blank in the colour the screen was using",
                    f.cell[24 * 80][1], 0x1E);
    vm86_expect_u16("the cursor is at the start of the last row",
                    f.cursor, 24 * 80);
}

static void test_0e_backspace_stops_at_column_zero(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    start_text_screen(cpu, &st);

    cpu->bh = 0;
    cpu->dh = 3;
    cpu->dl = 0;
    call(cpu, &st, 0x02);

    cpu->al = 0x08;
    call(cpu, &st, 0x0E);           /* backspace at column zero */

    expect_cursor(cpu, &st, "backspace did not wrap to the row above",
                  3 * 80 + 0);

    cpu->dh = 3;
    cpu->dl = 2;
    call(cpu, &st, 0x02);

    cpu->al = 0x08;
    call(cpu, &st, 0x0E);

    expect_cursor(cpu, &st, "backspace moved one column left", 3 * 80 + 1);
}

static void test_0a_and_0d_move_different_things(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    start_text_screen(cpu, &st);

    cpu->bh = 0;
    cpu->dh = 5;
    cpu->dl = 10;
    call(cpu, &st, 0x02);

    cpu->al = 0x0A;                 /* line feed */
    call(cpu, &st, 0x0E);

    expect_cursor(cpu, &st, "line feed moved the row and not the column",
                  6 * 80 + 10);

    cpu->al = 0x0D;                 /* carriage return */
    call(cpu, &st, 0x0E);

    expect_cursor(cpu, &st, "carriage return moved the column and not the row",
                  6 * 80 + 0);
}

static void test_0e_tab_advances_to_the_next_stop(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    start_text_screen(cpu, &st);

    cpu->bh = 0;
    cpu->dh = 0;
    cpu->dl = 0;
    call(cpu, &st, 0x02);

    cpu->al = 0x09;
    call(cpu, &st, 0x0E);

    expect_cursor(cpu, &st, "tab reached column eight", 8);
}

/* ------------------------------------------------------------------ */
/* AH=09h and AH=0Ah                                                   */
/* ------------------------------------------------------------------ */

static void test_09_and_0a_leave_the_cursor_alone(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    start_text_screen(cpu, &st);

    cpu->bh = 0;
    cpu->dh = 2;
    cpu->dl = 3;
    call(cpu, &st, 0x02);

    cpu->al = 'Q';
    cpu->bl = 0x1E;
    cpu->cx = 1;
    call(cpu, &st, 0x09);

    expect_cell(cpu, &st, "09h wrote the character and the attribute",
                0, 2, 3, 'Q', 0x1E);
    expect_cursor(cpu, &st, "09h did not move the cursor", 2 * 80 + 3);

    cpu->al = 'R';
    cpu->bl = 0x4F;                 /* ignored: 0Ah keeps the colour */
    call(cpu, &st, 0x0A);

    expect_cell(cpu, &st, "0Ah wrote the character and kept the attribute",
                0, 2, 3, 'R', 0x1E);
    expect_cursor(cpu, &st, "0Ah did not move the cursor", 2 * 80 + 3);

    cpu->al = 'S';
    call(cpu, &st, 0x0E);

    expect_cursor(cpu, &st, "0Eh did move the cursor", 2 * 80 + 4);
}

static void test_09_repeats_and_wraps(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    start_text_screen(cpu, &st);

    cpu->bh = 0;
    cpu->dh = 0;
    cpu->dl = 0;
    call(cpu, &st, 0x02);

    cpu->al = '-';
    cpu->bl = 0x07;
    cpu->cx = 5;
    call(cpu, &st, 0x09);

    for (uint16_t i = 0; i < 5; i++)
        expect_cell(cpu, &st, "each repeated cell", 0, 0, i, '-', 0x07);

    vm86_expect_mem8("and no further", cpu, cell_at(&st, 0, 0, 5), ' ');

    /* Four more at the end of a row: two on this one, two on the next. */
    cpu->dh = 1;
    cpu->dl = 78;
    call(cpu, &st, 0x02);

    cpu->al = '#';
    cpu->cx = 4;
    call(cpu, &st, 0x09);

    expect_cell(cpu, &st, "wrap 1", 0, 1, 78, '#', 0x07);
    expect_cell(cpu, &st, "wrap 2", 0, 1, 79, '#', 0x07);
    expect_cell(cpu, &st, "wrap 3", 0, 2,  0, '#', 0x07);
    expect_cell(cpu, &st, "wrap 4", 0, 2,  1, '#', 0x07);
    expect_cursor(cpu, &st, "repeating did not move the cursor", 1 * 80 + 78);
}

/*
 * Writing at the bottom row must stop rather than scroll: scrolling is
 * the teletype's behaviour, not this one's.
 *
 * *** THIS IS A DECISION, AND IT IS NOT WHAT VGABIOS DOES ***
 *
 * In text mode vgabios neither wraps nor stops. It writes on linearly,
 * adding two to the address per cell, so it walks off the bottom of the
 * page and into whatever is next in video memory -- over a page of 4000
 * bytes whose stride is 4096, that is 96 bytes of padding and then the
 * next page, and over the last page it is the end of the window.
 *
 * Stopping gives the same screen as that walk for every cell a program
 * can see: the two differ only in what happens to memory past the end of
 * the page. What they do not share is the second effect -- the linear
 * walk can overwrite the page a program is keeping something in, and
 * stopping cannot. So this stops, on purpose.
 */
static void test_09_stops_at_the_bottom(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    start_text_screen(cpu, &st);

    cpu->bh = 0;
    cpu->dh = 24;
    cpu->dl = 79;
    call(cpu, &st, 0x02);

    cpu->al = '@';
    cpu->bl = 0x07;
    cpu->cx = 3;
    call(cpu, &st, 0x09);

    expect_cell(cpu, &st, "the last cell of the page", 0, 24, 79, '@', 0x07);
    expect_cell(cpu, &st, "and nothing beyond it", 0, 0, 0, ' ', 0x07);
    expect_cell(cpu, &st, "nor on the row above", 0, 23, 0, ' ', 0x07);
}

/* ------------------------------------------------------------------ */
/* AH=02h and AH=03h                                                   */
/* ------------------------------------------------------------------ */

static void test_02_and_03_round_trip(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    start_text_screen(cpu, &st);

    cpu->bh = 1;
    cpu->dh = 5;
    cpu->dl = 7;
    call(cpu, &st, 0x02);

    cpu->bh = 1;
    cpu->dh = 0;
    cpu->dl = 0;
    cpu->cx = 0;
    call(cpu, &st, 0x03);

    vm86_expect_u16("row came back", cpu->dh, 5);
    vm86_expect_u16("column came back", cpu->dl, 7);
    vm86_expect_u16("start scan line came back", cpu->ch, st.cursor_start);
    vm86_expect_u16("end scan line came back", cpu->cl, st.cursor_end);
    vm86_expect_u16("the page on the way in is the page on the way out",
                    cpu->bh, 1);
    vm86_expect_u16("AH still says which function this was", cpu->ah, 0x03);

    /* A position on page 0 must not have moved because page 1 did. */
    cpu->bh = 0;
    call(cpu, &st, 0x03);

    vm86_expect_u16("page 0's cursor is its own", cpu->dh, 0);
    vm86_expect_u16("page 0's column is its own", cpu->dl, 0);
}

/* ------------------------------------------------------------------ */
/* AH=05h, selecting a page                                            */
/* ------------------------------------------------------------------ */

static void test_05_selects_a_page_and_09_writes_there(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    start_text_screen(cpu, &st);

    cpu->al = 'P';
    call(cpu, &st, 0x0E);

    expect_cell(cpu, &st, "page 0 took the first character",
                0, 0, 0, 'P', VM86_ATTR_DEFAULT);

    cpu->al = 1;
    call(cpu, &st, 0x05);

    vm86_expect_u16("the active page is now 1", st.active_page, 1);

    cpu->al = 'Q';
    call(cpu, &st, 0x0E);

    expect_cell(cpu, &st, "page 1 took the second character",
                1, 0, 0, 'Q', VM86_ATTR_DEFAULT);
    expect_cell(cpu, &st, "and page 0 is untouched",
                0, 0, 0, 'P', VM86_ATTR_DEFAULT);

    /* 09h names its page in BH and must reach the same place. Page 0's
     * cursor was left at column 1 by the teletype write above, so this
     * also shows that 09h writes at the cursor rather than at the
     * origin. */
    cpu->bh = 0;
    cpu->al = 'Z';
    cpu->bl = 0x07;
    cpu->cx = 1;
    call(cpu, &st, 0x09);

    expect_cell(cpu, &st, "09h with BH=0 wrote on page 0, at its cursor",
                0, 0, 1, 'Z', 0x07);
    expect_cell(cpu, &st, "leaving what was before it alone",
                0, 0, 0, 'P', VM86_ATTR_DEFAULT);
    expect_cell(cpu, &st, "and left page 1 alone",
                1, 0, 0, 'Q', VM86_ATTR_DEFAULT);
}

/* ------------------------------------------------------------------ */
/* AH=06h and AH=07h, scrolling a window                               */
/* ------------------------------------------------------------------ */

static void test_06_clears_the_whole_window_when_al_is_zero(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    start_text_screen(cpu, &st);

    poke(cpu, &st, 0, 0, 0, 'A', VM86_ATTR_DEFAULT);
    poke(cpu, &st, 0, 0, 9, 'B', VM86_ATTR_DEFAULT);
    poke(cpu, &st, 0, 0, 10, 'C', VM86_ATTR_DEFAULT);

    cpu->al = 0;                    /* blank the window, not "scroll none" */
    cpu->bh = 0x1E;                 /* the colour the blanks get          */
    cpu->ch = 0;
    cpu->cl = 0;
    cpu->dh = 0;
    cpu->dl = 9;
    call(cpu, &st, 0x06);

    expect_cell(cpu, &st, "the window was cleared", 0, 0, 0, ' ', 0x1E);
    expect_cell(cpu, &st, "across its full width", 0, 0, 9, ' ', 0x1E);
    expect_cell(cpu, &st, "and stopped at its edge", 0, 0, 10, 'C',
                VM86_ATTR_DEFAULT);
}

static void test_06_scrolls_one_line_and_07_the_other_way(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    start_text_screen(cpu, &st);

    poke(cpu, &st, 0, 0, 0, 'A', VM86_ATTR_DEFAULT);
    poke(cpu, &st, 0, 1, 0, 'B', VM86_ATTR_DEFAULT);
    poke(cpu, &st, 0, 2, 0, 'C', VM86_ATTR_DEFAULT);

    cpu->al = 1;
    cpu->bh = 0x07;
    cpu->ch = 0;
    cpu->cl = 0;
    cpu->dh = 2;
    cpu->dl = 0;
    call(cpu, &st, 0x06);           /* a three-row window, scrolled up */

    expect_cell(cpu, &st, "the second row moved up", 0, 0, 0, 'B',
                VM86_ATTR_DEFAULT);
    expect_cell(cpu, &st, "the third moved up too", 0, 1, 0, 'C',
                VM86_ATTR_DEFAULT);
    expect_cell(cpu, &st, "and the bottom is blank", 0, 2, 0, ' ', 0x07);

    cpu->al = 1;
    cpu->bh = 0x07;
    cpu->ch = 0;
    cpu->cl = 0;
    cpu->dh = 2;
    cpu->dl = 0;
    call(cpu, &st, 0x07);           /* and back down again */

    expect_cell(cpu, &st, "scrolling down put it back", 0, 1, 0, 'B',
                VM86_ATTR_DEFAULT);
    expect_cell(cpu, &st, "in the right place", 0, 2, 0, 'C',
                VM86_ATTR_DEFAULT);
    expect_cell(cpu, &st, "and blanked the top", 0, 0, 0, ' ', 0x07);
}

/*
 * The rectangle is four registers, and every one of them has to be a
 * different number for a case to be able to tell them apart.
 *
 * The two cases above use CL = DL = 0, which cannot distinguish left from
 * right at all: a version that read the window as (CH,CL) with the halves
 * of the top-left corner swapped produces the same screen. Measured, not
 * guessed -- injecting that swap left all 143 assertions green until this
 * case existed.
 *
 * So the window here is rows 1..3 by columns 2..4, with a marker just
 * outside each of its four edges. If the top edge is read one row too
 * high, `above` moves; one column out and `left` or `right` is eaten;
 * swapping the pair makes left > right, which this firmware treats as an
 * invalid rectangle and does nothing about -- and then nothing moves at
 * all and every assertion below fails.
 */
static void test_06_uses_all_four_corner_registers(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    start_text_screen(cpu, &st);

    /* Inside the window, one per row. */
    poke(cpu, &st, 0, 1, 2, 'A', VM86_ATTR_DEFAULT);
    poke(cpu, &st, 0, 2, 2, 'B', VM86_ATTR_DEFAULT);
    poke(cpu, &st, 0, 3, 2, 'C', VM86_ATTR_DEFAULT);

    /* One cell outside each edge. */
    poke(cpu, &st, 0, 0, 2, 'T', VM86_ATTR_DEFAULT);   /* above  */
    poke(cpu, &st, 0, 1, 1, 'L', VM86_ATTR_DEFAULT);   /* left   */
    poke(cpu, &st, 0, 1, 5, 'R', VM86_ATTR_DEFAULT);   /* right  */
    poke(cpu, &st, 0, 4, 2, 'D', VM86_ATTR_DEFAULT);   /* below  */

    cpu->al = 1;
    cpu->bh = 0x07;
    cpu->ch = 1;
    cpu->cl = 2;
    cpu->dh = 3;
    cpu->dl = 4;
    call(cpu, &st, 0x06);

    expect_cell(cpu, &st, "the second row of the window moved up",
                0, 1, 2, 'B', VM86_ATTR_DEFAULT);
    expect_cell(cpu, &st, "and the third",
                0, 2, 2, 'C', VM86_ATTR_DEFAULT);
    expect_cell(cpu, &st, "and the bottom of the window is blank",
                0, 3, 2, ' ', 0x07);

    expect_cell(cpu, &st, "the row above the window is untouched",
                0, 0, 2, 'T', VM86_ATTR_DEFAULT);
    expect_cell(cpu, &st, "the column to its left",
                0, 1, 1, 'L', VM86_ATTR_DEFAULT);
    expect_cell(cpu, &st, "the column to its right",
                0, 1, 5, 'R', VM86_ATTR_DEFAULT);
    expect_cell(cpu, &st, "and the row below",
                0, 4, 2, 'D', VM86_ATTR_DEFAULT);
}

/* ------------------------------------------------------------------ */
/* AH=08h, reading a cell                                              */
/* ------------------------------------------------------------------ */

static void test_08_reads_the_character_and_the_attribute(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    start_text_screen(cpu, &st);

    poke(cpu, &st, 0, 2, 5, 'k', 0x2E);

    cpu->bh = 0;
    cpu->dh = 2;
    cpu->dl = 5;
    call(cpu, &st, 0x02);

    cpu->bh = 0;
    call(cpu, &st, 0x08);

    vm86_expect_u16("the character came back", cpu->al, 'k');
    vm86_expect_u16("the attribute came back in AH", cpu->ah, 0x2E);

    cpu->bh = 0;
    call(cpu, &st, 0x03);

    vm86_expect_u16("and reading did not move the cursor", cpu->dh, 2);
    vm86_expect_u16("nor its column", cpu->dl, 5);
}

/* ------------------------------------------------------------------ */
/* AH=00h and AH=0Fh, the mode                                         */
/* ------------------------------------------------------------------ */

static void test_0f_reports_the_mode(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    start_text_screen(cpu, &st);

    cpu->al = 0;
    cpu->ah = 0;
    cpu->bh = 0;
    call(cpu, &st, 0x0F);

    vm86_expect_u16("mode in AL", cpu->al, 0x03);
    vm86_expect_u16("columns in AH", cpu->ah, 80);
    vm86_expect_u16("active page in BH", cpu->bh, 0);

    cpu->al = 0x00;                 /* forty columns now */
    call(cpu, &st, 0x00);

    cpu->al = 0;
    cpu->ah = 0;
    call(cpu, &st, 0x0F);

    vm86_expect_u16("the mode changed", cpu->al, 0x00);
    vm86_expect_u16("and so did the column count", cpu->ah, 40);
}

/*
 * A graphics mode is refused, and refused by doing nothing at all.
 *
 * The assertion that matters is the one on the mode word. A firmware that
 * recorded mode 13h while leaving the screen in text would have a program
 * addressing 0xA0000 as a linear framebuffer and writing into memory that
 * nothing renders, which fails a long way from its cause.
 */
static void test_00_refuses_a_graphics_mode(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    start_text_screen(cpu, &st);

    poke(cpu, &st, 0, 0, 0, 'A', VM86_ATTR_DEFAULT);

    cpu->al = 0x13;
    call(cpu, &st, 0x00);

    vm86_expect_u16("the mode is still text", st.mode, 0x03);
    vm86_expect_u16("the column count is unchanged", st.columns, 80);
    vm86_expect_u16("the page stride is unchanged", st.page_bytes, 4096);
    expect_cell(cpu, &st, "and the screen was not cleared",
                0, 0, 0, 'A', VM86_ATTR_DEFAULT);

    vm86_expect_mem8("nor did the BIOS data area record it",
                     cpu, (VM86_BDA_SEGMENT << 4) + VM86_BDA_VIDEO_MODE, 0x03);
}

/*
 * AL bit 7 means "do not clear the screen".
 *
 * It is a real part of the interface rather than a legend, and the source
 * is vgabios: its set-video-mode handler takes the top bit of AL as
 * `noclearmem` and skips the clear when it is set, leaving the memory and
 * setting the cursor home anyway. It is how a program changes a mode
 * without wiping a screen it has already drawn.
 */
static void test_00_bit_seven_means_do_not_clear(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    start_text_screen(cpu, &st);

    poke(cpu, &st, 0, 4, 4, 'A', VM86_ATTR_DEFAULT);

    cpu->al = (uint8_t)(0x03 | 0x80);
    call(cpu, &st, 0x00);

    expect_cell(cpu, &st, "the screen survived a mode set without clearing",
                0, 4, 4, 'A', VM86_ATTR_DEFAULT);

    cpu->al = 0x03;
    call(cpu, &st, 0x00);

    expect_cell(cpu, &st, "and an ordinary mode set cleared it",
                0, 4, 4, ' ', VM86_ATTR_DEFAULT);
}

/*
 * Forty columns, which is the case that catches a hard-coded 80.
 *
 * The row stride and the page stride are both different here, so this
 * case fails if either was written as a constant.
 */
static void test_00_forty_columns(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    start_text_screen(cpu, &st);

    cpu->al = 0x00;
    call(cpu, &st, 0x00);

    vm86_expect_u16("columns", st.columns, 40);
    vm86_expect_u16("page stride", st.page_bytes, 2048);

    cpu->bh = 0;
    cpu->dh = 0;
    cpu->dl = 39;
    call(cpu, &st, 0x02);

    cpu->al = 'W';
    call(cpu, &st, 0x0E);

    expect_cell(cpu, &st, "the last cell of a forty-column row",
                0, 0, 39, 'W', VM86_ATTR_DEFAULT);
    expect_cursor(cpu, &st, "which wraps to the next row", 40);
}

/* ------------------------------------------------------------------ */
/* AH=01h, the cursor shape                                            */
/* ------------------------------------------------------------------ */

static void test_01_sets_the_shape_and_can_hide_the_cursor(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    start_text_screen(cpu, &st);

    vm86_expect_u16("the cursor starts visible",
                    bios10_cursor_cell(cpu, &st), 0);

    cpu->ch = 0x20;                 /* the classic way to hide it */
    cpu->cl = 0x00;
    call(cpu, &st, 0x01);

    vm86_expect_u16("a start past the end hides the cursor",
                    bios10_cursor_cell(cpu, &st), VM86_DISPLAY_NO_CURSOR);

    /* And it is still in the data area, where a program reads it. */
    vm86_expect_mem8("start scan line in 0040:0061",
                     cpu, (VM86_BDA_SEGMENT << 4) + VM86_BDA_CURSOR_SHAPE + 1u,
                     0x20);
    vm86_expect_mem8("end scan line in 0040:0060",
                     cpu, (VM86_BDA_SEGMENT << 4) + VM86_BDA_CURSOR_SHAPE,
                     0x00);

    cpu->ch = 0x06;
    cpu->cl = 0x07;
    call(cpu, &st, 0x01);

    vm86_expect_u16("and a real shape shows it again",
                    bios10_cursor_cell(cpu, &st), 0);
}

/* ------------------------------------------------------------------ */
/* The BIOS data area                                                  */
/* ------------------------------------------------------------------ */

/*
 * Read directly, not through the service.
 *
 * Going back through INT 10h would test the service's own arithmetic
 * against itself and pass whether or not the data area was written at all.
 * A program that wants to know where the cursor is reads 0040:0050, and
 * these are the bytes it reads.
 */
static void test_the_data_area_is_synced(struct vm86_cpu *cpu)
{
    struct bios10_state st;
    uint32_t bda = (uint32_t)VM86_BDA_SEGMENT << 4;

    start_text_screen(cpu, &st);

    cpu->bh = 0;
    cpu->dh = 7;
    cpu->dl = 9;
    call(cpu, &st, 0x02);

    cpu->al = 1;
    call(cpu, &st, 0x05);

    vm86_expect_mem8 ("0040:0049 is the mode",
                      cpu, bda + VM86_BDA_VIDEO_MODE, 0x03);
    vm86_expect_mem16("0040:004A is the column count",
                      cpu, bda + VM86_BDA_COLUMNS, 80);
    vm86_expect_mem16("0040:004C is the page stride",
                      cpu, bda + VM86_BDA_PAGE_BYTES, 4096);
    vm86_expect_mem16("0040:0050 is page 0's cursor",
                      cpu, bda + VM86_BDA_CURSOR, (7u << 8) | 9u);
    vm86_expect_mem16("0040:0052 is page 1's cursor",
                      cpu, bda + VM86_BDA_CURSOR + 2u, 0);
    vm86_expect_mem16("0040:0050+2*p is the stride between them",
                      cpu, bda + VM86_BDA_CURSOR + 14u, 0);
    vm86_expect_mem16("0040:0060 is the cursor shape, start high",
                      cpu, bda + VM86_BDA_CURSOR_SHAPE,
                      (uint16_t)((0x06u << 8) | 0x07u));
    vm86_expect_mem8 ("0040:0062 is the active page",
                      cpu, bda + VM86_BDA_ACTIVE_PAGE, 1);
}

/* ------------------------------------------------------------------ */
/* The host's two accessors                                            */
/* ------------------------------------------------------------------ */

static void test_the_active_page_is_at_the_right_address(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    start_text_screen(cpu, &st);

    const uint8_t *page0 = bios10_active_page(cpu, &st);

    vm86_expect_bool("page 0 is the video window",
                     page0 == cpu->mem->ram + VM86_TEXT_BASE, true);

    cpu->al = 1;
    call(cpu, &st, 0x05);

    const uint8_t *page1 = bios10_active_page(cpu, &st);

    vm86_expect_bool("and page 1 is one page stride along, not 4000",
                     page1 == cpu->mem->ram + VM86_TEXT_BASE + 4096u, true);
}

static void test_no_display_is_not_a_crash(struct vm86_cpu *cpu)
{
    struct bios10_state st;
    uint8_t cell[2] = { 'A', 0x07 };

    start_text_screen(cpu, &st);

    vm86_display_present(NULL, cell, 0);

    const struct vm86_display no_backend = { NULL, NULL };
    vm86_display_present(&no_backend, cell, 0);

    const struct vm86_display fine = { fake_present, NULL };
    vm86_display_present(&fine, NULL, 0);           /* a page that is not there */

    vm86_expect_bool("a null page is reported as null",
                     bios10_active_page(cpu, &st) != NULL, true);
}

/* ------------------------------------------------------------------ */
/* An AH this machine does not implement                               */
/* ------------------------------------------------------------------ */

/*
 * The answer is "not here", and it has to leave everything alone to say
 * it. Software of this era tests for an extension by calling it and
 * looking at what came back; a stub that helpfully writes something is a
 * stub that makes a program believe in hardware it does not have.
 */
static void test_an_unknown_function_changes_nothing(struct vm86_cpu *cpu)
{
    struct bios10_state st;

    start_text_screen(cpu, &st);

    poke(cpu, &st, 0, 0, 0, 'A', VM86_ATTR_DEFAULT);

    cpu->ax = 0x3300;
    cpu->bx = 0x1234;
    cpu->cx = 0x5678;
    cpu->dx = 0x9ABC;
    call(cpu, &st, 0x33);

    vm86_expect_u16("AX", cpu->ax, 0x3300);
    vm86_expect_u16("BX", cpu->bx, 0x1234);
    vm86_expect_u16("CX", cpu->cx, 0x5678);
    vm86_expect_u16("DX", cpu->dx, 0x9ABC);
    expect_cell(cpu, &st, "and the screen is untouched",
                0, 0, 0, 'A', VM86_ATTR_DEFAULT);
}

/* ------------------------------------------------------------------ */

static const struct vm86_test tests[] = {
    { "0Eh prints and advances the cursor",
      test_0e_prints_and_advances },
    { "0Eh keeps the attribute already in the cell",
      test_0e_keeps_the_attribute_of_the_cell },
    { "0Eh wraps at the end of a row",
      test_0e_wraps_at_the_end_of_a_row },
    { "0Eh scrolls at the bottom of the screen",
      test_0e_scrolls_at_the_bottom },
    { "0Eh backspace stops at column zero",
      test_0e_backspace_stops_at_column_zero },
    { "0Eh line feed and carriage return move different things",
      test_0a_and_0d_move_different_things },
    { "0Eh tab advances to the next stop",
      test_0e_tab_advances_to_the_next_stop },

    { "09h and 0Ah write without moving the cursor",
      test_09_and_0a_leave_the_cursor_alone },
    { "09h repeats and wraps",
      test_09_repeats_and_wraps },
    { "09h stops at the bottom instead of scrolling",
      test_09_stops_at_the_bottom },

    { "02h and 03h round-trip a page, a position and a shape",
      test_02_and_03_round_trip },

    { "05h selects a page and 09h writes on it",
      test_05_selects_a_page_and_09_writes_there },

    { "06h with AL=0 clears the window",
      test_06_clears_the_whole_window_when_al_is_zero },
    { "06h and 07h scroll opposite ways",
      test_06_scrolls_one_line_and_07_the_other_way },
    { "06h uses all four corner registers",
      test_06_uses_all_four_corner_registers },

    { "08h reads the character and the attribute",
      test_08_reads_the_character_and_the_attribute },

    { "0Fh reports the mode, the columns and the page",
      test_0f_reports_the_mode },
    { "00h refuses a graphics mode by doing nothing",
      test_00_refuses_a_graphics_mode },
    { "00h with bit 7 set keeps the screen",
      test_00_bit_seven_means_do_not_clear },
    { "00h in forty columns",
      test_00_forty_columns },

    { "01h sets the shape and can hide the cursor",
      test_01_sets_the_shape_and_can_hide_the_cursor },

    { "the BIOS data area is kept in step",
      test_the_data_area_is_synced },

    { "the active page is at the right address",
      test_the_active_page_is_at_the_right_address },
    { "no display, or no page, is not a crash",
      test_no_display_is_not_a_crash },

    { "an unknown function changes nothing",
      test_an_unknown_function_changes_nothing },
};

VM86_TEST_MAIN("bios10", tests)
