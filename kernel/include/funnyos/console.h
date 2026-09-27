/*
 * Console line discipline.
 *
 * The keyboard driver produces keys; this turns them into edited lines.
 * It is deliberately thin, and deliberately in the kernel: whether a
 * program is the shell or something the shell started, the editing rules
 * are the same, and putting them here means every program that asks for
 * input gets backspace and echo without implementing either.
 */
#ifndef FUNNYOS_CONSOLE_H
#define FUNNYOS_CONSOLE_H

#include <stddef.h>

/*
 * Read one edited line, blocking until Enter.
 *
 * The line is echoed as it is typed, on both output channels, so it shows
 * up on screen and in the serial log. Backspace erases. The result is
 * NUL-terminated and excludes the newline; the return value is its
 * length, which is what callers actually want and what they would
 * otherwise have to recompute with strlen.
 *
 * A line longer than `size - 1` has its excess dropped rather than
 * wrapping around or overrunning -- the buffer is the limit, and the
 * program reading it is entitled to assume that.
 */
size_t console_read_line(char *buf, size_t size);

/* Write a prompt and leave the cursor after it. Convenience, because
 * every caller wants one. */
void console_prompt(const char *text);

#endif /* FUNNYOS_CONSOLE_H */
