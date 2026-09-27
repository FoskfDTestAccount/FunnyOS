/*
 * The system call interface.
 *
 * ---------------------------------------------------------------------
 * ABI
 *
 * Raised with INT 0x80, which is a trap through the IDT rather than the
 * SYSCALL instruction. That is a deliberate choice for now: the IDT
 * entry, the TSS stack switch and the register save are all machinery the
 * kernel already has and has already tested, whereas SYSCALL would need
 * its own MSR setup, its own stack switch and its own entry path -- for a
 * speed difference that does not matter yet. The DOS emulator's guest
 * services are the callers that will make this a hot path, and that is
 * M5's problem.
 *
 *   rax   call number
 *   rdi   argument 0
 *   rsi   argument 1
 *   rdx   argument 2
 *   r10   argument 3
 *   r8    argument 4
 *   r9    argument 5
 *
 *   rax   result, or a negative error code
 *
 * The register order is the one the C calling convention already uses for
 * its first three arguments, so a wrapper in the user library is a
 * function that moves rcx into r10 and executes INT 0x80 -- nothing
 * beyond what a normal call already does.
 *
 * INT 0x80 is a 64-bit vector that had no meaning before IBM gave it to
 * DOS, which makes it a fitting choice here and, more practically, means
 * nothing else in this kernel wants it.
 */
#ifndef FUNNYOS_SYSCALL_H
#define FUNNYOS_SYSCALL_H

#include <stdint.h>

/* The vector Ring 3 code raises. */
#define SYSCALL_VECTOR 0x80

/* Errors. Negative, so a successful call's result is never mistaken for
 * one. */
#define SYSCALL_EPERM   (-1)   /* not permitted */
#define SYSCALL_ENOENT  (-2)   /* no such file */
#define SYSCALL_EBADF   (-3)   /* bad file descriptor */
#define SYSCALL_EFAULT  (-4)   /* pointer outside the caller's memory */
#define SYSCALL_ENOMEM  (-5)   /* out of memory */
#define SYSCALL_EINVAL  (-6)   /* bad argument */
#define SYSCALL_ENOSYS  (-7)   /* no such call */
#define SYSCALL_EMFILE  (-8)   /* too many open files */

/* Call numbers. */
#define SYS_EXIT       0   /* (int code)                        never returns */
#define SYS_WRITE      1   /* (fd, const void *buf, size_t n)   -> n   */
#define SYS_READ       2   /* (fd, void *buf, size_t n)         -> n   */
#define SYS_OPEN       3   /* (const char *path, int flags)     -> fd  */
#define SYS_CLOSE      4   /* (fd)                              -> 0   */
#define SYS_READDIR    5   /* (unsigned index, struct dirent *) -> 0, 1 at end */
#define SYS_GETKEY     6   /* ()                                -> key */
#define SYS_UPTIME_MS  7   /* ()                                -> ms  */
#define SYS_CLEAR      8   /* ()                                -> 0   */
#define SYS_COUNT      9

/* File descriptors 0, 1 and 2 are the console, so that a program can be
 * written without opening anything. Ramfs file descriptors start above
 * them. */
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* Open flags. */
#define O_RDONLY 0
#define O_WRONLY 1

/*
 * One directory entry, as SYS_READDIR writes it.
 *
 * Fixed-width and self-contained on purpose: there is no dynamic loader
 * and no shared headers between kernel and user space beyond this file,
 * so a structure that has to be kept in step by hand should be as small
 * and as obvious as possible.
 */
#define DIRENT_NAME_MAX 32

struct dirent {
    char     name[DIRENT_NAME_MAX];   /* NUL-terminated, uppercase-preserved */
    uint32_t size;
    uint32_t is_directory;
};

/* Install the syscall vector at DPL 3 and register the dispatcher. */
void syscall_init(void);

#endif /* FUNNYOS_SYSCALL_H */
