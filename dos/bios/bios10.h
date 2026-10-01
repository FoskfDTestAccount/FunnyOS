/*
 * The video BIOS: what a guest's INT 10h calls do in text mode, and what
 * the host has to be able to read in order to draw the result.
 *
 * ---------------------------------------------------------------------
 * Why the state lives here and not in a static
 *
 * The mode, the active page, the cursor and its shape are the BIOS's
 * variables, not the host's, and a program reads them back -- through
 * INT 10h and, more often, straight out of the BIOS data area at 0040:xx.
 * So they have to exist somewhere the service can update and the host can
 * read, and that somewhere is this struct.
 *
 * It is the caller's to own rather than a file-scope static for the two
 * reasons a global is ever wrong: a test wants two machines at once, and
 * FunnyOS wants the emulator's firmware state in the process that runs
 * the emulator rather than in the emulator's text.
 *
 * ---------------------------------------------------------------------
 * What the host reads it for
 *
 * Two fields, and only two, are part of the host's contract:
 *
 *   columns, rows   how big a page is. The host renders columns*rows
 *                   cells, which is not a constant -- a 40-column mode
 *                   is a supported mode and its page is half the width.
 *   active_page     which page those cells come from.
 *
 * Everything else here is the BIOS's business. The host gets a pointer to
 * the page from bios10_active_page() rather than computing it, precisely
 * so that `page_bytes` -- which is *not* rows*columns*2, see below -- never
 * has to be right in two places.
 *
 * ---------------------------------------------------------------------
 * page_bytes is 4096, and 4000 is the number that looks right
 *
 * An 80x25 text page holds 80*25*2 = 4000 bytes of characters and
 * attributes, and the BDA at 0040:004C holds the stride between pages,
 * which is 4096. The 96 bytes between the end of one page and the start of
 * the next are padding that no program sees.
 *
 * Using 4000 as a stride therefore looks correct and is correct for page
 * 0 -- which is the page almost everything uses -- and puts page 1 at
 * 0xB8FA0 instead of 0xB9000. A program that writes page 1 and reads it
 * back through the same wrong arithmetic sees its own data and notices
 * nothing.
 */
#ifndef VM86_BIOS10_H
#define VM86_BIOS10_H

#include <stdbool.h>
#include <stdint.h>

#include <vm86/cpu.h>
#include <vm86/display.h>

/* Text pages are twenty-five rows in every mode this machine has. The
 * page size (0040:004C) answers "how many bytes", not "how many rows",
 * so the row count lives here rather than in the state. */
#define BIOS10_ROWS   25u

/* The BIOS keeps eight cursor positions, one per page, whether or not the
 * current mode has that many pages of memory behind them. See the note on
 * bios10_cursor_cell() below for what that costs. */
#define BIOS10_PAGES  8u

/*
 * The state a text-mode INT 10h service and a display backend share.
 *
 * Field for field it is the BIOS data area's video half (0040:0049
 * onwards), in the same shapes, because every one of these has a
 * program-visible counterpart and the two are written together. Read
 * `cursor` as the pair of bytes it is in memory: row in the high half,
 * column in the low half.
 */
struct bios10_state {
    uint8_t  mode;                  /* 0040:0049 */
    uint8_t  video_ctl;             /* 0040:0087, bit 7 only; see bios10.c */
    uint8_t  columns;               /* 0040:004A, the low byte of it */
    uint16_t page_bytes;            /* 0040:004C */
    uint8_t  active_page;           /* 0040:0062 */
    uint8_t  cursor_start;          /* 0040:0061, the high byte of 0040:0060 */
    uint8_t  cursor_end;            /* 0040:0060, the low byte of it */
    uint16_t cursor[BIOS10_PAGES];  /* 0040:0050, eight of them */
};

/* Bring the state up the way the firmware's power-on self test leaves it:
 * mode 3, page 0, the cursor at the home position and the default shape.
 *
 * It does not touch guest memory, so it does not clear the screen -- the
 * screen is cleared by setting a mode, which is what a real machine's POST
 * does and what almost every program does for itself before printing.
 * A program that prints through INT 10h without ever setting a mode gets
 * the text at whatever attribute the memory already holds, which is what
 * the hardware does too. */
void bios10_reset(struct bios10_state *st);

/* Service an INT 10h. `ctx` is a `struct bios10_state *`.
 *
 * Reads its arguments out of the CPU's registers and writes its results
 * back the same way. Never touches the interrupt frame. */
void bios10_service(struct vm86_cpu *cpu, void *ctx);

/*
 * Write one character the way AH=0Eh does.
 *
 * Exposed because the DOS layer's console output is exactly this and
 * nothing else. DOS's AH=02h and AH=09h printed by reaching the BIOS's
 * teletype routine, so on a real machine all three ended up in the same
 * code; sharing one implementation here is what the machine did rather
 * than a shortcut. A second copy would be a second place for the four
 * control characters to be wrong, and the two would disagree on the day
 * one of them was fixed.
 *
 * The state is a parameter and not reached through the video service's
 * registry slot on purpose: `ctx` belongs to whoever registered the
 * service, and a caller that has the state in hand -- as the DOS layer
 * does -- should not have to go looking for it. `vm86_register_service`
 * is process-wide and its table is private to trap.c.
 */
void bios10_tty(struct vm86_cpu *cpu, struct bios10_state *st, uint8_t ch);

/*
 * Where the cursor is, as a cell index (row * columns + column), or
 * VM86_DISPLAY_NO_CURSOR when there is nothing to draw.
 *
 * Two ways to get the no-cursor answer, and they are different things
 * wearing one constant. The first is that the program turned the cursor
 * off; the second is that it moved the cursor somewhere off the page -- a
 * program can set a row of 40 in eighty-column mode and the hardware
 * simply does not draw a cursor there.
 *
 * Note that the position comes from the *active* page's slot, because a
 * cursor is drawn on the page being displayed and not on whichever page a
 * previous call happened to name.
 */
uint16_t bios10_cursor_cell(const struct vm86_cpu *cpu,
                            const struct bios10_state *st);

/*
 * The active page, as it sits in guest memory: character in the low byte,
 * attribute in the high byte, left to right, top to bottom.
 *
 * The caller must read `columns * BIOS10_ROWS` cells from it -- 2000 for
 * eighty columns, 1000 for forty -- and must not assume the page runs to
 * the next one, because it does not: the stride is page_bytes and there is
 * padding between.
 *
 * Returns NULL when the page falls outside the guest's memory, which is a
 * thing a host has to cope with rather than a thing that cannot happen.
 * The pointer stays valid for as long as the guest's memory does.
 */
const uint8_t *bios10_active_page(const struct vm86_cpu *cpu,
                                  const struct bios10_state *st);

#endif /* VM86_BIOS10_H */
