/*
 * Who is on the screen.
 *
 * ---------------------------------------------------------------------
 * Why this is a separate layer
 *
 * The console is a log: it scrolls, everything written to it is written
 * forever, and it has one colour. A program that has a screen of its own
 * -- and the DOS emulator is exactly that, because a DOS program's whole
 * interface is the text page it leaves at 0xB8000 -- cannot be shown by a
 * log. Append eighty characters to a scrolling log and what a person sees
 * is eighty characters in a log.
 *
 * So the screen is a thing that can be *held*. The kernel holds it by
 * default and prints into it. A program asks for it, and while it has it
 * the console's output goes into the log's own grid without being drawn,
 * and the program's pages are what a person sees. Giving it back repaints
 * the log, which is intact because it never stopped being maintained.
 *
 * That is the whole design, and it is deliberately small. What it is not
 * is a window system: there is one screen and one holder.
 *
 * ---------------------------------------------------------------------
 * Why the kernel can always take it back
 *
 * The alternative -- the emulator owns the screen until it says otherwise,
 * as DOS did with the display -- has a failure mode this project exists to
 * avoid. A guest program that hangs leaves a user with a screen they
 * cannot get out of and no way to say so. So the screen is *borrowed*: a
 * panic takes it back and prints, and the holder's exit gives it back
 * whether the holder meant to or not.
 *
 * See DESIGN.md's note on this, which is where the decision is recorded.
 * ---------------------------------------------------------------------
 */
#ifndef FUNNYOS_SCREEN_H
#define FUNNYOS_SCREEN_H

#include <stdbool.h>
#include <stdint.h>

#include <funnyos/process.h>

/*
 * The shape of a page a program may hand over: twenty-five rows, as every
 * text mode of the emulated machine's display has, and up to
 * SCREEN_MAX_COLUMNS columns, which is the widest character grid this
 * console itself can hold.
 *
 * The row count is the emulator's fact and is written out here rather than
 * included from it: the kernel cannot see dos/include, and it is not meant
 * to. What the kernel is doing is not emulation -- it is drawing a block
 * of cells somebody gave it, and 25 rows is the shape it accepts. A
 * program that hands over a page of another height is refused, which is
 * the honest answer for a kernel with one screen geometry.
 */
#define SCREEN_ROWS        25u
#define SCREEN_MAX_COLUMNS 256u

/* Whether there is a screen to hand over at all. False when the machine
 * booted with no usable framebuffer, in which case a program is told so
 * rather than being allowed to think it is being seen. */
bool screen_ready(void);

/*
 * Take the screen on behalf of `who`.
 *
 * Returns true if the caller has it afterwards, which includes the case
 * where it already did: a program that asks twice is not an error, and
 * treating it as one would make the caller's bookkeeping the kernel's
 * problem.
 *
 * False when there is no screen, when `who` is NULL (the kernel does not
 * acquire what it already holds), or when somebody else has it.
 */
bool screen_acquire(const struct process *who);

/*
 * Draw one page, in guest video memory's own layout, and make it what is
 * on the screen.
 *
 * `columns` may change between calls -- a program that switches to a
 * forty-column mode presents half as many -- and the screen is cleared
 * when it does, so the wider page's leftovers do not stay visible beside
 * the narrower one.
 *
 * `cursor` is a cell index, or 0xFFFF -- which the system call interface
 * calls SCREEN_NO_CURSOR and the framebuffer calls FB_NO_CURSOR. Neither
 * of those headers can see the other, so screen.c includes both and asks
 * the compiler whether they still agree.
 *
 * False when nobody is holding the screen or the page does not fit.
 */
bool screen_present(const uint8_t *cells, unsigned columns, uint16_t cursor);

/* Give the screen back to the kernel's log. Does nothing when the screen
 * is already the kernel's. */
void screen_release(void);

/* Give the screen back if `who` is the one holding it. This is what a
 * process's teardown calls: a program that exits without releasing is the
 * ordinary case, not an error. */
void screen_release_if_held_by(const struct process *who);

/* Take the screen back from whoever has it, without a way to say no. For
 * the panic path, where the alternative is a message nobody can read. */
void screen_take_back(void);

struct mouse_event;
/* Called at safe syscall/poll boundaries, never from a hardware ISR. */
void screen_poll_input(void);
void screen_request_tab(unsigned ordinal);
bool screen_keyboard_focus(const struct process *who);
bool screen_mouse_poll(const struct process *who,struct mouse_event *out);
#endif /* FUNNYOS_SCREEN_H */
