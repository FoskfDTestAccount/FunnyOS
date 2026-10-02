/*
 * Linear framebuffer text console.
 *
 * Limine leaves us a linear framebuffer (GOP on UEFI, VBE on BIOS) and
 * maps it into the HHDM. This driver turns that into a plain character
 * grid: 8x16 cells, cursor tracking, line wrap and scrollback.
 *
 * Scope note: this is deliberately the minimum that makes the machine
 * visible without a serial cable -- monochrome text only. VGA text mode
 * compatibility, attribute bytes and the 0xB8000 window belong to the
 * DOS subsystem and land later.
 */
#ifndef FUNNYOS_FB_H
#define FUNNYOS_FB_H

#include <stdbool.h>
#include <stdint.h>

/* Bring up the console from boot information. Returns false when no
 * usable framebuffer was provided, in which case callers should fall
 * back to serial-only output. */
bool fb_init(void);

/* Whether the console is usable. */
bool fb_is_ready(void);

/* Clear the screen and reset the cursor. */
void fb_clear(void);

/* Emit one character. Handles \n, \r, \b and \t. */
void fb_putc(char c);

/* 24-bit RGB colour helpers; 0xRRGGBB. */
void fb_set_fg(uint32_t rgb);
void fb_set_bg(uint32_t rgb);
void fb_reset_color(void);

/*
 * ---------------------------------------------------------------------
 * Cells that are not the console's
 *
 * The functions above draw the console's own grid: the scrolling log the
 * kernel prints into, one colour, one cursor. A program that has to show a
 * screen of its own -- a DOS program's text page, which is what the
 * emulator exists to put on a display -- needs something the console
 * cannot give it: a rectangle of the screen it owns, its own colours, and
 * a cursor that is the guest's rather than the console's.
 *
 * So this is a second way to put characters on the same framebuffer. The
 * two do not interleave: a program on the screen and the kernel's log on
 * the screen at the same time would each be drawing over the other. Which
 * one is drawing is decided a level up, in screen.c.
 * ---------------------------------------------------------------------
 */

/* The console's grid, in cells. False when there is no console at all, in
 * which case neither output is set. */
bool fb_grid_size(uint64_t *cols, uint64_t *rows);

/*
 * Whether the console's own output reaches the screen.
 *
 * Off means the grid keeps being maintained -- kprintf still scrolls, the
 * cursor still advances, a later repaint still shows everything that was
 * printed -- and none of it is drawn. That is the difference between
 * "the log is not on the screen right now" and "the log was thrown away",
 * and the first is what a program holding the screen needs.
 */
void fb_set_output(bool enabled);

/* Paint the console grid onto the screen from scratch. */
void fb_repaint(void);

/* Fill the screen with the console's background colour and leave the
 * character grid alone -- the screen is somebody else's now. */
void fb_fill_screen(void);

/*
 * Draw a block of text cells in the layout guest video memory uses: one
 * byte of character, one byte of attribute, left to right and top to
 * bottom, `columns` cells per row.
 *
 * The attribute is an IBM PC text attribute -- foreground in the low
 * nibble, background in bits 4 to 6, bit 7 the blink this machine does
 * not model -- and it is rendered, because a program that sets a colour
 * and does not see it is a program running on a machine that is lying
 * about its video hardware.
 *
 * A cell index into `cells`, or FB_NO_CURSOR. The cursor is drawn as an
 * underline rather than as a block: an underline needs no inverse video
 * and so is visible whatever colours the cell is using.
 */
#define FB_NO_CURSOR 0xFFFFu

void fb_draw_page(uint64_t col, uint64_t row, uint64_t columns,
                  uint64_t rows, const uint8_t *cells, uint16_t cursor);

/* Geometry and write-only compositor primitives. Rectangles restore from
 * authoritative RAM, never by sampling WC framebuffer pixels. */
void fb_set_overlay(void (*overlay)(void));
void fb_set_top(unsigned rows);
bool fb_geometry(uint64_t *width,uint64_t *height,unsigned *cell_w,unsigned *cell_h);
void fb_background_rect(uint64_t x,uint64_t y,uint64_t w,uint64_t h);
void fb_console_rect(uint64_t x,uint64_t y,uint64_t w,uint64_t h);
void fb_page_rect(uint64_t col,uint64_t row,unsigned columns,const uint8_t *cells,
                  uint16_t cursor,uint64_t x,uint64_t y,uint64_t w,uint64_t h);
void fb_pointer(unsigned x,unsigned y);
#endif /* FUNNYOS_FB_H */
