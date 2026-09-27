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

#endif /* FUNNYOS_FB_H */
