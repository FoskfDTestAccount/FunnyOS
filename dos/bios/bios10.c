/*
 * INT 10h, text modes: the IBM PC's video BIOS, as far as a program that
 * prints through it can tell.
 *
 * ---------------------------------------------------------------------
 * Where the behaviour comes from
 *
 * Nothing here is in the 8086 manual. It is the IBM PC's BIOS, published
 * in 1981 and copied by everything since, and this tree has neither the
 * IBM PC Technical Reference nor Ralf Brown's list. So each decision below
 * says where it came from: a source that was read, a source that was read
 * only through somebody else's summary, or a choice made here because
 * nothing was found.
 *
 * The three that were read in the original are the ones worth naming:
 *
 *   - QEMU's vgabios (qemu/vgabios), the ROM emulators have used for
 *     twenty years, for the teletype path in text mode.
 *   - SeaBIOS's vgasrc/vgabios.c, which is what QEMU ships now, for the
 *     same thing arrived at independently.
 *   - Ralf Brown's MEMORY.LST entry for 0040:0060, for the byte order of
 *     the cursor shape word.
 *
 * ---------------------------------------------------------------------
 * The two places this file disagrees with its task book
 *
 * Both are written up in the report; they are repeated here because a
 * reader who has the task book open will otherwise read the code as the
 * bug.
 *
 * 1. AH=0Eh is documented here as taking its attribute from the cell it is
 *    writing into, not from BL. The task book says BL is the attribute.
 *    In text mode BL is not read at all -- both vgabios and SeaBIOS write
 *    only the character byte and leave the attribute half of the cell
 *    exactly as they found it, and the folklore agrees: "you cannot print
 *    coloured text with function 0Eh, use 09h". BL is the colour in
 *    *graphics* modes, which is where the description in the task book
 *    comes from. This matters beyond pedantry: a program that prints with
 *    0Eh and never sets BL has BL = whatever it left there, and taking the
 *    task book at its word would print that program's output in black.
 *
 * 2. The cursor shape word at 0040:0060 has the start scan line in the
 *    *high* byte. The task book says the low one. Ralf Brown marks the
 *    word big-endian for exactly this reason, and it is the natural
 *    consequence of AH=01h taking the shape in CH:CL and AH=03h handing it
 *    back in CX: storing CX stores CH (the start) high.
 *
 * ---------------------------------------------------------------------
 * What is deliberately not here
 *
 * No shadow copy of the screen. The characters live in guest memory at
 * 0xB8000 and nowhere else -- the host renders from there, and a program
 * that pokes 0xB8000 directly is not doing anything special. Keeping a
 * second copy would mean two answers to "what is on the screen", and they
 * would part company on the first program that wrote to the segment
 * itself, which is most of them.
 */

#include "bios10.h"

#include <stddef.h>

#include <vm86/firmware.h>
#include <vm86/mem.h>

/* ------------------------------------------------------------------ */
/* Where a cell is                                                     */
/* ------------------------------------------------------------------ */

/*
 * All four of these go through cell_linear(), so the page stride and the
 * row stride are spelled once. Three copies of `row * columns + col` is
 * three chances to have one of them be right.
 *
 * None of them range-checks. Every caller has already decided that the
 * cell it wants exists, and the ones that have not are checked where the
 * decision belongs rather than here, where it would be made four times.
 */

static uint32_t page_base(const struct bios10_state *st, uint8_t page)
{
    return VM86_TEXT_BASE + (uint32_t)page * st->page_bytes;
}

static uint32_t cell_linear(const struct bios10_state *st, uint8_t page,
                            uint16_t row, uint16_t col)
{
    return page_base(st, page) + ((uint32_t)row * st->columns + col) * 2u;
}

static uint8_t cell_char(struct vm86_cpu *cpu, const struct bios10_state *st,
                         uint8_t page, uint16_t row, uint16_t col)
{
    return vm86_mem_read8(cpu->mem, cell_linear(st, page, row, col));
}

static uint8_t cell_attr(struct vm86_cpu *cpu, const struct bios10_state *st,
                         uint8_t page, uint16_t row, uint16_t col)
{
    return vm86_mem_read8(cpu->mem, cell_linear(st, page, row, col) + 1u);
}

/* The character byte alone. This is the whole of what a teletype write
 * does to a cell, and it is why the attribute survives. */
static void put_char(struct vm86_cpu *cpu, const struct bios10_state *st,
                     uint8_t page, uint16_t row, uint16_t col, uint8_t ch)
{
    vm86_mem_write8(cpu->mem, cell_linear(st, page, row, col), ch);
}

static void put_cell(struct vm86_cpu *cpu, const struct bios10_state *st,
                     uint8_t page, uint16_t row, uint16_t col,
                     uint8_t ch, uint8_t attr)
{
    uint32_t linear = cell_linear(st, page, row, col);

    vm86_mem_write8(cpu->mem, linear, ch);
    vm86_mem_write8(cpu->mem, linear + 1u, attr);
}

/* ------------------------------------------------------------------ */
/* The BIOS data area                                                  */
/* ------------------------------------------------------------------ */

/*
 * The two display bytes firmware.h does not... did not use to name.
 *
 *   0040:0084  rows on screen minus one          (VM86_BDA_ROWS)
 *   0040:0087  the video control byte, whose     (VM86_BDA_VIDEO_CONTROL)
 *              bit 7 means "do not clear the
 *              screen on a mode set"
 *
 * They were defined here first, because this module needed them and the
 * header is frozen -- the same move bios16.c made for 0040:0071, and for
 * the same reason. The difference is what happened next: an audit found
 * they belonged in the map, they went into the map, and this file uses
 * the map's names now. One fact, one spelling; a second one here would be
 * the thing firmware.h exists to prevent.
 *
 * The row count is written on every call rather than only on a mode set,
 * because it is derived from the state and cannot differ between two
 * calls -- twenty-five rows is 24, always.
 */

/* Bit 7 of 0040:0087. The header names the byte; the bit inside it is
 * this module's because nothing else sets it. */
#define BIOS10_CTL_NO_CLEAR      0x80u

/*
 * Publish the state into 0040:xxxx.
 *
 * Every service ends here, whether or not it changed anything, because
 * the alternative -- each handler remembering which of its fields a
 * program might read -- is the version where one handler forgets. The
 * write is a handful of stores and the loop runs twice per keystroke.
 *
 * Only the video fields are touched. The rest of the data area belongs to
 * the keyboard, the timer and the power-on self test, and a second writer
 * for any of them is how the two would come to disagree.
 */
static void sync_bda(struct vm86_cpu *cpu, const struct bios10_state *st)
{
    struct vm86_mem *mem = cpu->mem;
    uint32_t bda = (uint32_t)VM86_BDA_SEGMENT << 4;

    vm86_mem_write8 (mem, bda + VM86_BDA_VIDEO_MODE, st->mode);
    vm86_mem_write16(mem, bda + VM86_BDA_COLUMNS,    st->columns);
    vm86_mem_write16(mem, bda + VM86_BDA_PAGE_BYTES, st->page_bytes);

    for (uint8_t page = 0; page < BIOS10_PAGES; page++)
        vm86_mem_write16(mem, bda + VM86_BDA_CURSOR + page * 2u,
                         st->cursor[page]);

    /* Start scan line high, end scan line low. See the header. */
    vm86_mem_write16(mem, bda + VM86_BDA_CURSOR_SHAPE,
                     (uint16_t)((st->cursor_start << 8) | st->cursor_end));

    vm86_mem_write8 (mem, bda + VM86_BDA_ACTIVE_PAGE, st->active_page);
    vm86_mem_write8 (mem, bda + VM86_BDA_ROWS,
                     (uint8_t)(BIOS10_ROWS - 1u));
    vm86_mem_write8 (mem, bda + VM86_BDA_VIDEO_CONTROL, st->video_ctl);
}

/* ------------------------------------------------------------------ */
/* Cursors                                                             */
/* ------------------------------------------------------------------ */

/*
 * The cursor is off when the shape says so.
 *
 * Two ways, and the first is the one that matters. `MOV CH,20h` is how
 * every DOS program of the era hides the cursor, and it works on the
 * hardware because scan line 32 is past the bottom of an eight-line
 * character cell -- the range of a scan line is the low five bits, so a
 * start greater than the end is not a shape at all. The second is the
 * later VGA convention of setting bit 5 of CH, which this machine does not
 * have an attribute controller for but which costs one test to honour.
 *
 * Masking to five bits before comparing is what makes the first case work
 * and is also what the CRTC does with the value it is given.
 */
static bool cursor_hidden(const struct bios10_state *st)
{
    if (st->cursor_start & 0x20u)
        return true;

    return (st->cursor_start & 0x1Fu) > (st->cursor_end & 0x1Fu);
}

/* The high and low halves of a page's cursor word, which are a row and a
 * column. Spelled as functions because writing them out at each use is
 * where the halves get swapped. */
static uint16_t cursor_row(uint16_t cursor) { return (uint16_t)(cursor >> 8); }
static uint16_t cursor_col(uint16_t cursor) { return (uint16_t)(cursor & 0xFFu); }

static uint16_t make_cursor(uint16_t row, uint16_t col)
{
    return (uint16_t)((row << 8) | (col & 0xFFu));
}

/* Pages are named by a byte in BH or by AL, and only three bits of it mean
 * anything. Wrapping rather than rejecting: a program that asks for page 9
 * has an off-by-one somewhere, and the hardware reads whatever the address
 * arithmetic lands on rather than refusing. */
static uint8_t page_index(uint8_t asked)
{
    return (uint8_t)(asked & (BIOS10_PAGES - 1u));
}

/* ------------------------------------------------------------------ */
/* Scrolling                                                           */
/* ------------------------------------------------------------------ */

/*
 * Move a rectangle of the screen by `lines` rows, filling what it leaves
 * behind with spaces in `attr`. `lines` of zero means "blank the whole
 * rectangle", which is the documented way to clear a window and is *not*
 * "scroll by no lines" -- those differ by an entire screen.
 *
 * `lines` at least as tall as the window is the same instruction on real
 * hardware: there is nothing to move in. Both cases come out as the clear
 * above, which is why the comparison is `>=` and not `==`.
 *
 * The direction is the only thing 06h and 07h disagree about, so it is a
 * parameter rather than two functions, and the two copy loops are written
 * so that the source is always read before the destination overwrites it:
 * scrolling up walks rows upward, scrolling down walks them downward.
 *
 * A rectangle that runs off the screen is clamped to the screen, and
 * only a crossed one -- top below bottom, or left right of right -- does
 * nothing at all.
 *
 * That split is vgabios's and SeaBIOS's, and the reason to follow it
 * rather than refuse both is a reachable input rather than a principle:
 * the ordinary way to clear the screen is a window of rows 0..24 and
 * columns 0..79, which is what a program written for eighty columns
 * passes. On a forty-column screen 79 is past the end, and a version that
 * refused out-of-range rectangles would do nothing at all -- so the
 * standard clear-screen call would leave the screen alone on exactly the
 * machine that most needs it.
 *
 * The clamp is the far corner only. vgabios leaves the near one alone,
 * which is invisible here: a window starting below the last row ends up
 * with crossed corners and falls out at the check below, and a window
 * starting off the right edge does the same.
 */
static void scroll_window(struct vm86_cpu *cpu, const struct bios10_state *st,
                          uint8_t page, uint16_t lines, uint8_t attr,
                          uint16_t top, uint16_t left,
                          uint16_t bottom, uint16_t right, bool up)
{
    if (top > bottom || left > right)
        return;

    if (bottom >= BIOS10_ROWS)
        bottom = (uint16_t)(BIOS10_ROWS - 1u);
    if (right >= st->columns)
        right = (uint16_t)(st->columns - 1u);

    /* Clamping the far corner can leave the two crossed, and a crossed
     * pair is the same nothing the check above returned for. */
    if (top > bottom || left > right)
        return;

    uint16_t height = (uint16_t)(bottom - top + 1u);

    if (lines == 0 || lines >= height) {
        for (uint16_t row = top; row <= bottom; row++)
            for (uint16_t col = left; col <= right; col++)
                put_cell(cpu, st, page, row, col, ' ', attr);
        return;
    }

    if (up) {
        for (uint16_t row = top; row + lines <= bottom; row++)
            for (uint16_t col = left; col <= right; col++)
                put_cell(cpu, st, page, row, col,
                         cell_char(cpu, st, page, (uint16_t)(row + lines), col),
                         cell_attr(cpu, st, page, (uint16_t)(row + lines), col));

        for (uint16_t row = (uint16_t)(bottom - lines + 1u); row <= bottom; row++)
            for (uint16_t col = left; col <= right; col++)
                put_cell(cpu, st, page, row, col, ' ', attr);
    } else {
        for (uint16_t row = bottom; row >= top + lines; row--)
            for (uint16_t col = left; col <= right; col++)
                put_cell(cpu, st, page, row, col,
                         cell_char(cpu, st, page, (uint16_t)(row - lines), col),
                         cell_attr(cpu, st, page, (uint16_t)(row - lines), col));

        for (uint16_t row = top; row < top + lines; row++)
            for (uint16_t col = left; col <= right; col++)
                put_cell(cpu, st, page, row, col, ' ', attr);
    }
}

/*
 * The attribute a teletype scroll fills the new blank line with.
 *
 * Not a constant. vgabios reads it out of the cell the cursor has ended up
 * on, one row up from the bottom, so a screen that was printed in one
 * colour scrolls in that colour, and this follows vgabios. SeaBIOS, which
 * has the same job, uses the fixed default 0x07 instead, and Ralf Brown
 * does not settle it -- so this is the closest thing to a checked answer
 * available and it is noted in the report as a choice between two working
 * implementations rather than as the documented behaviour.
 *
 * The cell is always inside the page: the column has already wrapped to
 * zero, or is a column the cursor was legally on.
 */
static uint8_t teletype_fill_attr(struct vm86_cpu *cpu,
                                  const struct bios10_state *st,
                                  uint8_t page, uint16_t col)
{
    if (col >= st->columns)
        col = (uint16_t)(st->columns - 1u);

    return cell_attr(cpu, st, page, BIOS10_ROWS - 1u, col);
}

static void teletype_scroll(struct vm86_cpu *cpu, const struct bios10_state *st,
                            uint8_t page, uint16_t col)
{
    scroll_window(cpu, st, page, 1, teletype_fill_attr(cpu, st, page, col),
                  0, 0, BIOS10_ROWS - 1u, (uint16_t)(st->columns - 1u), true);
}

/* ------------------------------------------------------------------ */
/* AH=0Eh, teletype output                                             */
/* ------------------------------------------------------------------ */

/*
 * The character goes into the active page at that page's cursor, and the
 * cursor advances. Which page: the active one, not BH. Ralf Brown lists BH
 * as the page and vgabios's comment says the list is wrong about that --
 * output follows the display, and a program that wants another page
 * selected it with 05h.
 *
 * The control characters are the interesting part and they are four
 * separate behaviours, not one:
 *
 *   BEL   ignored. There is no speaker on this machine.
 *   BS    the column goes back one, and stops at zero. It does *not*
 *         climb to the end of the previous line, which is the answer the
 *         serial-terminal description gives and what vgabios does.
 *   LF    the row goes down one and the column does not move. At the
 *         bottom row this scrolls the screen up by one line.
 *   CR    the column goes to zero and the row does not move.
 *   TAB   spaces to the next multiple of eight. The task book does not
 *         list tab, but the alternative to handling it is printing its
 *         glyph, which is not a thing real firmware has ever done.
 *
 * Anything else is printed. Reaching the end of a row wraps to the start
 * of the next one, and reaching the end of the last row scrolls -- the
 * cursor does not sit parked in the last column, which is the difference
 * between a terminal and a 1981 BIOS that is imitating one.
 */
static void teletype(struct vm86_cpu *cpu, struct bios10_state *st, uint8_t ch)
{
    uint8_t  page = page_index(st->active_page);
    uint16_t row  = cursor_row(st->cursor[page]);
    uint16_t col  = cursor_col(st->cursor[page]);

    if (ch == 0x07u)                                    /* BEL */
        return;

    if (ch == 0x08u) {                                  /* BS */
        if (col > 0)
            col--;
        st->cursor[page] = make_cursor(row, col);
        return;
    }

    if (ch == 0x0Du) {                                  /* CR */
        st->cursor[page] = make_cursor(row, 0);
        return;
    }

    if (ch == 0x0Au) {                                  /* LF */
        row++;
        if (row >= BIOS10_ROWS) {
            teletype_scroll(cpu, st, page, col);
            row = BIOS10_ROWS - 1u;
        }
        st->cursor[page] = make_cursor(row, col);
        return;
    }

    /* A cursor a program parked off the page still has to print
     * somewhere; the edge of the screen is the only answer that keeps the
     * output on the page it belongs to rather than in the next one's
     * memory. */
    if (row >= BIOS10_ROWS)
        row = BIOS10_ROWS - 1u;
    if (col >= st->columns)
        col = (uint16_t)(st->columns - 1u);

    if (ch == 0x09u) {                                  /* TAB */
        do {
            put_char(cpu, st, page, row, col, ' ');
            col++;
            if (col >= st->columns) {
                col = 0;
                row++;
                if (row >= BIOS10_ROWS) {
                    teletype_scroll(cpu, st, page, col);
                    row = BIOS10_ROWS - 1u;
                }
            }
        } while (col % 8u != 0);

        st->cursor[page] = make_cursor(row, col);
        return;
    }

    put_char(cpu, st, page, row, col, ch);
    col++;
    if (col >= st->columns) {
        col = 0;
        row++;
        if (row >= BIOS10_ROWS) {
            teletype_scroll(cpu, st, page, col);
            row = BIOS10_ROWS - 1u;
        }
    }

    st->cursor[page] = make_cursor(row, col);
}

/* ------------------------------------------------------------------ */
/* AH=09h and AH=0Ah, writing at the cursor                            */
/* ------------------------------------------------------------------ */

/*
 * Both write CX copies of AL at the cursor of the page BH names, wrapping
 * to the next row at the end of one and *stopping* at the bottom of the
 * screen rather than scrolling. Neither moves the cursor: the difference
 * between these and the teletype is that these are for drawing, and a
 * program drawing with them wants the cursor left where it put it.
 *
 * The only difference between them is the attribute: 09h takes BL, and 0Ah
 * keeps whatever each cell already had. That is not a smaller version of
 * 09h -- it is how a program underlines text or writes into a box without
 * disturbing its colour.
 */
static void write_run(struct vm86_cpu *cpu, const struct bios10_state *st,
                      uint8_t ch, uint8_t page_asked, uint8_t attr,
                      uint16_t count, bool use_attr)
{
    uint8_t  page = page_index(page_asked);
    uint16_t row  = cursor_row(st->cursor[page]);
    uint16_t col  = cursor_col(st->cursor[page]);

    for (uint16_t i = 0; i < count; i++) {
        if (row >= BIOS10_ROWS)
            break;

        if (use_attr)
            put_cell(cpu, st, page, row, col, ch, attr);
        else
            put_char(cpu, st, page, row, col, ch);

        col++;
        if (col >= st->columns) {
            col = 0;
            row++;
        }
    }
}

/* ------------------------------------------------------------------ */
/* AH=00h, set mode                                                    */
/* ------------------------------------------------------------------ */

/*
 * The four text modes are the whole of what this machine has. A graphics
 * mode is refused by doing nothing whatever -- no mode change, no clear,
 * and above all no writing of the requested mode number into the data
 * area.
 *
 * That last part is the reason for this particular shape of refusal. The
 * three candidates were to ignore the call, to record the mode without
 * changing the display, or to refuse loudly; there is nowhere to refuse
 * loudly to. Recording the mode is the one that does harm: the program
 * then believes it is in, say, mode 12h, addresses 0xA0000 as a linear
 * framebuffer, and draws into memory that nothing renders. Its next
 * hundred writes go somewhere plausible and produce nothing on the screen,
 * and the failure surfaces a long way from its cause. Doing nothing leaves
 * the program in the text mode it was already in, where its output at
 * least still goes to the screen.
 *
 * AL bit 7 means "do not clear the screen", for the modes that are
 * supported. It is a real part of the interface and not a legend: it is
 * how a program changes the cursor and the mode word without wiping a
 * screen it has already drawn.
 *
 * The screen is cleared across all eight pages rather than just the one
 * being displayed. Clearing only page 0 leaves seven pages of the previous
 * mode's attributes in place, and the first program to switch to page 1
 * finds text at an attribute that is no longer meaningful.
 */
static void set_mode(struct vm86_cpu *cpu, struct bios10_state *st, uint8_t al)
{
    uint8_t mode = (uint8_t)(al & 0x7Fu);
    bool    clear = (al & 0x80u) == 0;

    uint8_t  columns;
    uint16_t page_bytes;

    switch (mode) {
    case 0x00u:                 /* 40x25, sixteen colours            */
    case 0x01u:                 /* 40x25, four colours               */
        columns    = 40;
        page_bytes = 2048;      /* 40*25*2 = 2000, rounded up        */
        break;

    case 0x02u:                 /* 80x25, sixteen colours            */
    case 0x03u:                 /* 80x25, four colours               */
        columns    = 80;
        page_bytes = 4096;      /* 80*25*2 = 4000, rounded up        */
        break;

    default:
        return;                 /* not a mode this machine has       */
    }

    st->mode         = mode;
    st->video_ctl    = (uint8_t)(al & BIOS10_CTL_NO_CLEAR);
    st->columns      = columns;
    st->page_bytes   = page_bytes;
    st->active_page  = 0;
    st->cursor_start = 0x06;
    st->cursor_end   = 0x07;

    for (uint8_t page = 0; page < BIOS10_PAGES; page++)
        st->cursor[page] = make_cursor(0, 0);

    if (!clear)
        return;

    for (uint8_t page = 0; page < BIOS10_PAGES; page++)
        for (uint16_t row = 0; row < BIOS10_ROWS; row++)
            for (uint16_t col = 0; col < st->columns; col++)
                put_cell(cpu, st, page, row, col, ' ', VM86_ATTR_DEFAULT);
}

/* ------------------------------------------------------------------ */
/* The remaining functions                                             */
/* ------------------------------------------------------------------ */

/*
 * AH=03h, read cursor position.
 *
 * CH and CL come back as the shape, DH and DL as the row and column, and
 * BH is left alone -- it named the page on the way in and names it on the
 * way out. AH keeps the function number, like almost everything else here:
 * a program that checks whether the firmware is there after a call it made
 * is not unusual, and zeroing AH would answer "no".
 *
 * RBIL records AX coming back as zero on Phoenix BIOSes, which is the one
 * piece of evidence that this is not universal. Nothing in the tree needs
 * that behaviour and the task book asks for the function number, so the
 * function number it is.
 */
static void get_cursor_position(struct vm86_cpu *cpu,
                                const struct bios10_state *st, uint8_t page)
{
    uint16_t cursor = st->cursor[page_index(page)];

    cpu->ch = st->cursor_start;
    cpu->cl = st->cursor_end;
    cpu->dh = (uint8_t)cursor_row(cursor);
    cpu->dl = (uint8_t)cursor_col(cursor);
}

/*
 * AH=02h, set cursor position.
 *
 * The position is stored as given, including a position off the screen.
 * The hardware stores it too -- there is no range check anywhere in the
 * original -- and the BDA has to agree with what the program asked for,
 * because the program reads it back. What keeps it from mattering is that
 * a cursor off the page is not drawn: see bios10_cursor_cell().
 */
static void set_cursor_position(struct bios10_state *st, uint8_t page,
                                uint8_t row, uint8_t col)
{
    st->cursor[page_index(page)] = make_cursor(row, col);
}

/*
 * AH=08h, read the character and attribute at the cursor.
 *
 * Both halves of AX are results, so this is one of the two functions that
 * does not leave AH alone. The page is named by BH and reading does not
 * select it: a program can look at a page it is not displaying.
 */
static void read_char_attr(struct vm86_cpu *cpu, const struct bios10_state *st,
                           uint8_t page_asked)
{
    uint8_t  page = page_index(page_asked);
    uint16_t row  = cursor_row(st->cursor[page]);
    uint16_t col  = cursor_col(st->cursor[page]);

    cpu->al = cell_char(cpu, st, page, row, col);
    cpu->ah = cell_attr(cpu, st, page, row, col);
}

/*
 * AH=0Fh, ask what mode the machine is in.
 *
 * Every register of AX and BH is a result: the mode in AL, the number of
 * columns in AH so that a program does not have to know the width by mode
 * number, and the page being displayed in BH. There is no way to call this
 * and keep AH, which is why it is called out here.
 *
 * AL is not the bare mode number. Its top bit is bit 7 of the video
 * control byte, which is the same bit AH=00h was given: vgabios's handler
 * ORs it in, and a program that tests AL against 0x80 is asking "is there
 * still something on the screen that a mode set would not erase". Reading
 * it from the state rather than from the data area is deliberate -- the
 * state is what this module believes, and a guest that scribbled on
 * 0040:0087 should not be able to make the service answer differently
 * from what it will do.
 */
static void get_mode(struct vm86_cpu *cpu, const struct bios10_state *st)
{
    cpu->al = (uint8_t)(st->mode | (st->video_ctl & BIOS10_CTL_NO_CLEAR));
    cpu->ah = st->columns;
    cpu->bh = st->active_page;
}

/*
 * AH=05h, select the page.
 *
 * The cursor does not move, because there is no single cursor to move:
 * every page keeps its own, and displaying a page shows it where it was
 * last left. That is why the state has eight of them and why the BDA has
 * eight words.
 *
 * A page number the machine does not have is dropped rather than wrapped.
 * The difference from page_index()'s wrapping elsewhere is that this call
 * has a side effect beyond the arithmetic -- it changes what the whole
 * screen shows -- and wrapping 8 to 0 would turn a program's mistake into
 * a screen it did not ask to see.
 */
static void set_active_page(struct bios10_state *st, uint8_t page)
{
    if (page >= BIOS10_PAGES)
        return;

    st->active_page = page;
}

/* ------------------------------------------------------------------ */
/* The service                                                         */
/* ------------------------------------------------------------------ */

void bios10_reset(struct bios10_state *st)
{
    st->mode         = 0x03u;
    st->video_ctl    = 0;
    st->columns      = 80;
    st->page_bytes   = 4096;
    st->active_page  = 0;
    st->cursor_start = 0x06;
    st->cursor_end   = 0x07;

    for (uint8_t page = 0; page < BIOS10_PAGES; page++)
        st->cursor[page] = make_cursor(0, 0);
}

/*
 * AH is the function, and most of these leave it exactly as they found it,
 * which is to say holding the function number. Two do not -- 08h and 0Fh
 * put results in both halves of AX -- and they say so where they are
 * written.
 *
 * An AH this machine does not implement does nothing at all: no register
 * is written, no BDA field is touched. Software probes for the presence of
 * a BIOS extension by calling it and looking at what comes back, so a call
 * that returns with everything intact is the answer "not here", which is
 * the true one. Doing something plausible instead is how a program
 * concludes it has hardware it does not have.
 */
void bios10_service(struct vm86_cpu *cpu, void *ctx)
{
    struct bios10_state *st = ctx;

    if (st == NULL || cpu->mem == NULL)
        return;

    switch (cpu->ah) {
    case 0x00u: set_mode(cpu, st, cpu->al); break;

    case 0x01u:
        st->cursor_start = cpu->ch;
        st->cursor_end   = cpu->cl;
        break;

    case 0x02u: set_cursor_position(st, cpu->bh, cpu->dh, cpu->dl); break;

    case 0x03u: get_cursor_position(cpu, st, cpu->bh); break;

    case 0x05u: set_active_page(st, cpu->al); break;

    case 0x06u:
        scroll_window(cpu, st, page_index(st->active_page), cpu->al, cpu->bh,
                      cpu->ch, cpu->cl, cpu->dh, cpu->dl, true);
        break;

    case 0x07u:
        scroll_window(cpu, st, page_index(st->active_page), cpu->al, cpu->bh,
                      cpu->ch, cpu->cl, cpu->dh, cpu->dl, false);
        break;

    case 0x08u: read_char_attr(cpu, st, cpu->bh); break;

    case 0x09u:
        write_run(cpu, st, cpu->al, cpu->bh, cpu->bl, cpu->cx, true);
        break;

    case 0x0Au:
        write_run(cpu, st, cpu->al, cpu->bh, cpu->bl, cpu->cx, false);
        break;

    case 0x0Eu: teletype(cpu, st, cpu->al); break;

    case 0x0Fu: get_mode(cpu, st); break;

    default:
        break;
    }

    sync_bda(cpu, st);
}

/* ------------------------------------------------------------------ */
/* What the host reads                                                 */
/* ------------------------------------------------------------------ */

uint16_t bios10_cursor_cell(const struct vm86_cpu *cpu,
                            const struct bios10_state *st)
{
    (void)cpu;

    if (st == NULL || cursor_hidden(st))
        return VM86_DISPLAY_NO_CURSOR;

    uint8_t  page   = page_index(st->active_page);
    uint16_t row    = cursor_row(st->cursor[page]);
    uint16_t col    = cursor_col(st->cursor[page]);

    if (row >= BIOS10_ROWS || col >= st->columns)
        return VM86_DISPLAY_NO_CURSOR;

    return (uint16_t)(row * st->columns + col);
}

const uint8_t *bios10_active_page(const struct vm86_cpu *cpu,
                                  const struct bios10_state *st)
{
    if (cpu == NULL || cpu->mem == NULL || st == NULL)
        return NULL;

    uint32_t linear = page_base(st, page_index(st->active_page));
    uint32_t offset = vm86_mem_offset(cpu->mem, linear);

    if (offset == VM86_MEM_UNMAPPED)
        return NULL;

    return cpu->mem->ram + offset;
}

/*
 * Ask the host to catch up.
 *
 * One line, and it exists as a function so that the NULL display needs no
 * branch at any call site -- a headless test and a host that does not care
 * what was drawn both pass NULL and both get nothing, without either of
 * them writing a check.
 *
 * A NULL page is treated the same way. It means the guest's memory has no
 * video window in it, which is a machine with nothing to show, and passing
 * it on to the host would only move the check into every backend.
 */
void vm86_display_present(const struct vm86_display *display,
                          const uint8_t *cells, uint16_t cursor)
{
    if (display == NULL || display->present == NULL || cells == NULL)
        return;

    display->present(display->ctx, cells, cursor);
}
