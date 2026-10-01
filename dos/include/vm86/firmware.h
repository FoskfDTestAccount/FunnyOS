/*
 * The vocabulary of the machine being emulated: which interrupt does what,
 * where the firmware keeps its variables, and what the text display looks
 * like.
 *
 * ---------------------------------------------------------------------
 * Why this is one header and not three
 *
 * The video, keyboard and time services do not share code, but they do
 * share state, and the sharing is invisible if each one spells it out for
 * itself. Three files that each write `0x400 + 0x49` are three files
 * holding the same fact, and the day one of them is wrong about the
 * offset the symptom is a program behaving oddly rather than a build
 * error.
 *
 * So the addresses live here once. Nothing in this header is
 * implementation; it is the map.
 *
 * ---------------------------------------------------------------------
 * Where the authority comes from
 *
 * Unlike the instruction set, none of this is in the 8086 manual. It is
 * the IBM PC and its BIOS -- published in the IBM PC Technical Reference,
 * and documented a second time by everybody who ever wrote an emulator.
 * The repository has neither document.
 *
 * That is the same gap M3 had with the instruction manual and it is worth
 * stating plainly rather than rediscovering: an offset in this file that
 * is wrong will be wrong consistently, in every service that uses it, and
 * no test written against the same belief will catch it. Where a value
 * here cannot be checked, it says so.
 */
#ifndef VM86_FIRMWARE_H
#define VM86_FIRMWARE_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Interrupt vectors                                                   */
/* ------------------------------------------------------------------ */

/*
 * The ones this machine's firmware owns. The numbers are the PC's, not a
 * choice: a program that wants the keyboard calls 16h because that is
 * what the hardware it was written for answers on.
 *
 * The two groups are worth telling apart. 08h-0Fh and 70h-77h are the
 * hardware lines as the 8259 presents them; the rest are entries the
 * firmware publishes for software to call. A guest can hook any of them,
 * and the run loop delivers hardware interrupts to whatever the vector
 * points at by the time they fire.
 */
#define VM86_INT_DIVIDE_ERROR   0x00u   /* also the 8086's own fault     */
#define VM86_INT_TIMER          0x08u   /* IRQ0, 18.2 Hz                 */
#define VM86_INT_KEYBOARD       0x09u   /* IRQ1                          */
#define VM86_INT_VIDEO          0x10u
#define VM86_INT_EQUIPMENT      0x11u
#define VM86_INT_MEMORY_SIZE    0x12u
#define VM86_INT_DISK           0x13u
#define VM86_INT_SERIAL         0x14u
#define VM86_INT_SYSTEM         0x15u
#define VM86_INT_KEYBOARD_BIOS  0x16u
#define VM86_INT_PRINTER        0x17u
#define VM86_INT_TIME           0x1Au
#define VM86_INT_USER_TIMER     0x1Cu   /* called by the 08h handler     */

/* ------------------------------------------------------------------ */
/* The BIOS data area                                                  */
/* ------------------------------------------------------------------ */

/*
 * A fixed hundred-odd bytes at segment 0040 that the firmware keeps its
 * state in, and that programs read *directly* rather than through a
 * service call.
 *
 * This is not an optimisation anyone chose. It is the interface: a
 * program that wants to know how long it has been running reads the tick
 * count out of 0040:006C far more often than it calls INT 1Ah, because
 * that is cheaper and always has been. A machine that answers INT 1Ah
 * correctly but leaves the memory word at zero is a machine where some
 * programs work and others do not, for a reason nobody will guess.
 *
 * Each field is written by whichever service owns it -- the mode word by
 * the video service, the tick count by the timer, and so on. Nothing
 * else may write across the whole area, because a second writer is how
 * two services would come to disagree.
 *
 * Field numbers here are the documented ones. The ones marked with a note
 * are the ones whose exact meaning is easy to get subtly wrong.
 */
#define VM86_BDA_SEGMENT        0x0040u

#define VM86_BDA_EQUIPMENT      0x0010u  /* word: what the machine has   */
#define VM86_BDA_MEMORY_KB      0x0013u  /* word: KB of conventional RAM */
#define VM86_BDA_KEYBOARD_FLAGS 0x0017u  /* byte: shift states, see 0x18 */
#define VM86_BDA_KEYBOARD_FLAGS2 0x0018u /* byte: the second bank of them */
#define VM86_BDA_KB_HEAD        0x001Au  /* word: offset into the buffer */
#define VM86_BDA_KB_TAIL        0x001Cu  /* word: offset into the buffer */
#define VM86_BDA_KB_BUFFER      0x001Eu  /* 32 bytes, a circular queue  */
#define VM86_BDA_KB_BUFFER_END  0x003Eu  /* one past the last entry      */

#define VM86_BDA_VIDEO_MODE     0x0049u  /* byte */
#define VM86_BDA_COLUMNS        0x004Au  /* word: characters per row     */
#define VM86_BDA_PAGE_BYTES     0x004Cu  /* word: bytes per display page */
#define VM86_BDA_CURSOR         0x0050u  /* 8 words, one per page        */
#define VM86_BDA_CURSOR_END     0x0060u  /* one past the last            */
#define VM86_BDA_CURSOR_SHAPE   0x0060u  /* word: start scan, end scan   */
#define VM86_BDA_ACTIVE_PAGE    0x0062u  /* byte */

/*
 * Two more the video service owns, and they are here for a reason that is
 * about this file rather than about them: they were missing, so a service
 * that needed them defined them locally and the map stopped being the map.
 *
 * Rows is stored as one less than the count -- twenty-five rows is 24 --
 * which is the same off-by-one shape as the cursor and the disk geometry,
 * and the same place to get it wrong.
 *
 * The control byte's bit 7 is "do not clear memory when a mode is set".
 * Bit 6 selects 200 or 400 scan lines on later adapters; this machine has
 * neither, so it is written as zero at power-on and read back faithfully.
 */
#define VM86_BDA_ROWS           0x0084u  /* byte: rows on screen minus one */
#define VM86_BDA_VIDEO_CONTROL  0x0087u  /* byte: bit 7 = don't clear RAM  */
#define VM86_BDA_TICK_COUNT     0x006Cu  /* dword: at 18.2 Hz            */
#define VM86_BDA_TICK_ROLLOVER  0x0070u  /* byte: set when the dword wraps;
                                          * a program that reads the tick
                                          * count clears it itself */

/*
 * The Ctrl-Break flag. Bit 7 is set by the keyboard handler when
 * Ctrl-Break has been pressed, and a program polls it -- FreeDOS's
 * break.c reads this address directly -- so what is here matters even
 * though almost nothing calls a service to find out.
 *
 * It is *not* the keyboard buffer's overflow flag. That distinction cost
 * a round: an early task book said an overflowing buffer sets bit 7 here,
 * the note was the only document that mentioned the offset at all, and
 * the module wrote it on every dropped keystroke. Every dropped key then
 * looked to a polling program like the user pressing Break.
 *
 * The lesson is written here rather than only in the report because this
 * header is the map, and the failure was a service believing a note over
 * the map. A field that is not in here and is needed anyway gets added
 * here first.
 *
 * This machine does not maintain it -- Ctrl-Break is not decoded -- and
 * that is a stated gap rather than an overflow flag wearing its name.
 */
#define VM86_BDA_CTRL_BREAK     0x0071u

/*
 * The keyboard buffer is a ring. Both offsets are *relative to the start
 * of the segment*, not to the start of the buffer, and they always stay
 * inside 0x1E..0x3E.
 *
 * The queue holds a word per keystroke, not a byte: the scan code goes in
 * the high half and the character in the low half, and a program that
 * reads only the low byte is asking for the character. The head and the
 * tail are equal when the queue is empty -- which is also what they are
 * when it is full, one wrap apart. That ambiguity is the design's, not an
 * accident here, and a service that tries to be clever about it will
 * disagree with programs that are not.
 */
#define VM86_BDA_KB_EMPTY       (VM86_BDA_KB_BUFFER)

/* ------------------------------------------------------------------ */
/* The text display                                                    */
/* ------------------------------------------------------------------ */

/*
 * Mode 3: eighty columns, twenty-five rows, sixteen colours, two bytes
 * per cell, page 0 at the bottom of the video window.
 *
 * The window is at 0xA0000 in the guest's address space and the text page
 * starts 0x8000 into it. In this machine the window is ordinary guest
 * memory: the host renders from it, and it does not redirect the writes.
 * That is a deliberate simplification, recorded in DESIGN.md -- what it
 * costs is that the graphics modes (M7) will need the redirection, and
 * what it buys is that a program which writes 0xB8000 directly and then
 * reads it back sees what it wrote, which is a thing programs do.
 */
#define VM86_TEXT_BASE         0xB8000u
#define VM86_TEXT_COLUMNS      80u
#define VM86_TEXT_ROWS         25u
#define VM86_TEXT_CELLS        (VM86_TEXT_COLUMNS * VM86_TEXT_ROWS)
#define VM86_TEXT_PAGE_BYTES   (VM86_TEXT_CELLS * 2u)

/*
 * The stride from one text page to the next, which is not the size of a
 * page.
 *
 * Eighty columns of twenty-five rows is 4000 bytes of characters and
 * attributes, and pages are laid out 4096 apart -- the 96 bytes in
 * between are padding no program ever sees. 0040:004C holds *this*
 * number, and a program that uses the other one gets page 0 right, which
 * is the page almost everything uses, and page 1 wrong. Both numbers are
 * here so that the wrong one has to be named on purpose.
 */
#define VM86_TEXT_PAGE_STRIDE  0x1000u

/* The attribute byte: the low nibble is the foreground, the next three
 * bits the background, and the top bit makes the foreground blink -- or
 * the background bright, depending on a bit in the attribute controller
 * that is far outside anything this machine models. Sixteen backgrounds
 * and sixteen foregrounds is what a program can rely on. */
#define VM86_ATTR_FG_MASK      0x0Fu
#define VM86_ATTR_BG_SHIFT     4
#define VM86_ATTR_BG_MASK      0x70u
#define VM86_ATTR_BLINK        0x80u

/* The attribute a page is filled with when a mode is set, and the one
 * this machine's own console uses.
 *
 * It is deliberately not described as "what a teletype write uses": in
 * text mode AH=0Eh keeps whatever attribute is already in the cell it is
 * writing to and ignores the colour it was handed. That correction was
 * made once already and this line was left behind by it, still saying the
 * old thing. */
#define VM86_ATTR_DEFAULT      0x07u

#endif /* VM86_FIRMWARE_H */
