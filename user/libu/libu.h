/*
 * libu -- the user-space library.
 *
 * Small on purpose. There is no loader, no shared objects and no libc;
 * a program is linked against exactly this and whatever of libk it
 * needs, and everything else it does is a system call.
 *
 * The system call numbers themselves come from the kernel's own header.
 * That is deliberate: the ABI is one file shared by both sides rather
 * than two files that have to be kept in step by hand, and getting it
 * wrong is a class of bug worth designing out.
 */
#ifndef LIBU_H
#define LIBU_H

#include <stddef.h>
#include <stdint.h>

#include <funnyos/syscall.h>

/* --- System calls -------------------------------------------------- */

/* End this process. Never returns. */
void u_exit(int code) __attribute__((noreturn));

/* Write to the console. Returns the number of bytes written, or a
 * negative error. */
long u_write(int fd, const void *buf, size_t len);

/* Read one edited line into buf. Returns its length, which excludes the
 * terminator. Blocks until the user presses Enter. */
long u_read(int fd, void *buf, size_t len);

/* Open a file for reading. Returns a descriptor, or a negative error. */
int u_open(const char *path, int flags);

/* Close a descriptor. */
int u_close(int fd);

/*
 * Fill `out` with the directory entry at `index`.
 *
 * Returns 0 when the entry was filled and 1 when there are no more, so
 * the loop that uses it reads as `while (u_readdir(i, &e) == 0)`. Walking
 * by position rather than through a cursor the program has to hold means
 * there is no directory state to leak when a program forgets to close
 * one.
 */
int u_readdir(unsigned index, struct dirent *out);

/* Choose a file descriptor to read from -- stdin, or a file. Whether it
 * is a console or a file is the kernel's problem, not the program's. */
#define U_STDIN  STDIN_FILENO
#define U_STDOUT STDOUT_FILENO

/* Next key, blocking. Returns a key code; the arrows and the like are
 * values above 0xFF, so compare against the KEY_* names rather than
 * assuming a byte. */
int u_getkey(void);

/* Milliseconds since the kernel's timer came up. */
unsigned long u_uptime_ms(void);

/* Clear the screen. */
void u_clear(void);

/*
 * Run another program to completion and return its exit code.
 *
 * Blocking: control comes back when that program finishes. That is what
 * DOS does when one program runs another, and with no scheduler it is also
 * the only thing the kernel could do -- a caller that did not wait would
 * have nothing to return to.
 *
 * A negative return means the kernel would not start it at all (no such
 * image, or not enough memory). That is never a child's exit code, because
 * an exit code is not negative, so the two cannot be confused.
 */
long u_spawn(const char *image, unsigned long arg);

/* --- Console ------------------------------------------------------- */

void uputc(char c);
void uputs(const char *s);
void uputsln(const char *s);

/* Formatted output. Supports %s %c %d %i %u %x %X %p %% with '-', '0'
 * and a width, which is what kvformat in libk provides. */
void uprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Flush anything buffered. uprintf and uputs do this themselves, so a
 * program only needs it before doing something slow. */
void uflush(void);

#endif /* LIBU_H */
