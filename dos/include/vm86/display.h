/*
 * What the host has to be able to do with a display.
 *
 * ---------------------------------------------------------------------
 * Why there is a contract at all
 *
 * The guest's text lives in its own memory, at 0xB8000, and the host
 * renders from there rather than being told about each character. That
 * sounds like it needs no interface -- the host can just read the array.
 *
 * It needs one anyway, because *when* to render is not obvious and
 * getting it wrong is invisible. A program that writes a character
 * through INT 10h and one that pokes 0xB8000 directly must both show up,
 * and only one of those passes through a service that could call the
 * display. So rendering is driven from outside the services, on the
 * host's schedule, and this is the shape of the thing it calls.
 *
 * ---------------------------------------------------------------------
 * The two sides
 *
 * The emulator side (dos/bios/bios10.c) owns the mode, the active page
 * and the cursor position, because those are the BIOS's variables and
 * programs read them. The host side is anything that can put eighty
 * twenty-five characters on a screen -- the FunnyOS console in the real
 * machine, and a recorder in a test, which is the only way "the character
 * appeared" can be an assertion rather than somebody looking at it.
 */
#ifndef VM86_DISPLAY_H
#define VM86_DISPLAY_H

#include <stdint.h>

/*
 * Where the host's display is.
 *
 * `present` is handed the guest's active page and where the cursor is, in
 * the guest's own layout -- character in the low byte, attribute in the
 * high byte, left to right, top to bottom -- so that neither side has to
 * translate. The pointer is into guest memory and stays valid for as long
 * as the call; a host that wants to keep it must copy.
 */
struct vm86_display {
    void  (*present)(void *ctx, const uint8_t *cells, uint16_t cursor);
    void   *ctx;
};

/*
 * No cursor to draw. A cell index rather than a flag, because zero is a
 * perfectly good cell and the difference between "the cursor is at the
 * top-left" and "there is no cursor" has to be sayable.
 */
#define VM86_DISPLAY_NO_CURSOR  0xFFFFu

/*
 * Ask the display to catch up.
 *
 * A thin wrapper, here rather than at each call site, so that a NULL
 * display needs no branch anywhere: a host that has no screen -- a
 * headless test, a program checking the emulator without caring what it
 * drew -- passes NULL and this does nothing.
 */
void vm86_display_present(const struct vm86_display *display,
                          const uint8_t *cells, uint16_t cursor);

#endif /* VM86_DISPLAY_H */
