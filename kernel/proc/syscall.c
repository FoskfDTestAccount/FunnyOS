#include <funnyos/syscall.h>

#include <funnyos/arch/x86_64/idt.h>
#include <funnyos/arch/x86_64/io.h>
#include <funnyos/arch/x86_64/irq.h>
#include <funnyos/arch/x86_64/timer.h>
#include <funnyos/console.h>
#include <funnyos/fb.h>
#include <funnyos/kbd.h>
#include <funnyos/kprintf.h>
#include <funnyos/process.h>
#include <funnyos/ramfs.h>
#include <funnyos/vmm.h>

#include <libk/string.h>

/*
 * Every argument a program passes here is a number that was, until a
 * moment ago, under the control of code the kernel does not trust. Two
 * rules follow, and both are applied before the argument is used rather
 * than after it goes wrong:
 *
 *   1. A pointer is checked against the caller's own address space, so a
 *      program cannot ask the kernel to read its arguments out of kernel
 *      memory or hand it a copy of anything it has no mapping for.
 *
 *   2. Everything else is range checked, and a value that names something
 *      the caller does not own is an error rather than a lookup that
 *      happens to fail.
 *
 * The check is a walk of the caller's page tables rather than a fault
 * caught afterwards. That is sound here because nothing can change a
 * process's mappings behind its back -- there is no copy-on-write, no
 * swapping, and no second thread -- and it keeps a bad pointer from
 * becoming a kernel fault, which would take the machine down instead of
 * the program.
 */

static bool user_pointer_ok(uint64_t ptr, uint64_t len)
{
    return vmm_user_range_ok(process_current_pml4(), ptr, len);
}

/* --- Console ------------------------------------------------------- */

static int64_t sys_write(int fd, uint64_t buf, uint64_t len)
{
    if (fd != STDOUT_FILENO && fd != STDERR_FILENO)
        return SYSCALL_EBADF;

    if (!user_pointer_ok(buf, len))
        return SYSCALL_EFAULT;

    /* Copy in pieces rather than trusting the range to be contiguous:
     * the check above already proved every page is mapped, and reading a
     * byte at a time through the validated range is all this needs. */
    const char *text = (const char *)buf;
    for (uint64_t i = 0; i < len; i++)
        kputc(text[i]);

    return (int64_t)len;
}

/* Defined with the rest of the file operations below; declared here
 * because reading a line from the console and reading a file are two
 * cases of the same call. */
static int64_t read_file(int fd, uint64_t buf, uint64_t len);

static int64_t sys_read(int fd, uint64_t buf, uint64_t len)
{
    if (len == 0)
        return 0;

    if (!user_pointer_ok(buf, len))
        return SYSCALL_EFAULT;

    /*
     * Descriptor 0 is the console, and everything above it is a file.
     * Keeping them in one call rather than two is what lets a program
     * read from a file descriptor it was handed without knowing which
     * kind it is -- which is the whole point of a file descriptor.
     */
    if (fd == STDIN_FILENO) {
        /*
         * Editing, echo and the line discipline all live in the kernel,
         * so a program asking for a line of input gets exactly the same
         * behaviour whether it is the shell or something the shell
         * started. The address space is the caller's while this runs,
         * which is what lets the line be written straight into its
         * buffer.
         */
        return (int64_t)console_read_line((char *)buf, len);
    }

    return read_file(fd, buf, len);
}

/* --- Files --------------------------------------------------------- */

/*
 * Copy a NUL-terminated path in from the caller.
 *
 * A user pointer cannot be dereferenced once and then trusted: the
 * process could have asked for a length that runs off the end of a
 * mapping. So the check is per byte, which is slow and obviously correct,
 * and a path is short enough that "slow" never comes up. The check
 * happens as each byte is reached, so it stops exactly at the boundary
 * rather than one byte past it.
 */
static int64_t copy_path_from_user(char *dst, size_t dst_size, uint64_t src)
{
    if (dst_size == 0)
        return SYSCALL_EINVAL;

    for (size_t i = 0; i < dst_size; i++) {
        if (!user_pointer_ok(src + i, 1))
            return SYSCALL_EFAULT;

        char c = ((const char *)src)[i];
        dst[i] = c;

        if (c == '\0')
            return 0;
    }

    /* Ran out of room without finding a terminator. Said plainly rather
     * than truncating, because a truncated name would silently refer to
     * some other file. */
    return SYSCALL_EINVAL;
}

static int64_t sys_open(uint64_t path_ptr, uint64_t flags)
{
    (void)flags;   /* read-only for now; the flag is accepted and ignored */

    struct open_file *files = process_open_files();
    if (!files)
        return SYSCALL_EPERM;

    /* Long enough for any name the filesystem can hold, plus a little,
     * because a caller is allowed to write a longer path and be told no. */
    char path[RAMFS_NAME_MAX * 2];

    int64_t status = copy_path_from_user(path, sizeof(path), path_ptr);
    if (status != 0)
        return status;

    const struct ramfs_file *file = ramfs_find(path);
    if (!file)
        return SYSCALL_ENOENT;

    /* The filesystem hands out an index, not a pointer, so the descriptor
     * a program holds can only name something the kernel put in the
     * table itself. */
    int index = (int)(file - ramfs_at(0));

    /* Descriptors start above the console's, which are not in the table
     * and must not be shadowed by one that is. */
    for (int fd = STDERR_FILENO + 1; fd < PROCESS_MAX_OPEN_FILES; fd++) {
        if (files[fd].used)
            continue;

        files[fd].used   = true;
        files[fd].file   = index;
        files[fd].offset = 0;
        return fd;
    }

    return SYSCALL_EMFILE;
}

static int64_t sys_close(int fd)
{
    struct open_file *files = process_open_files();

    if (!files || fd <= STDERR_FILENO || fd >= PROCESS_MAX_OPEN_FILES)
        return SYSCALL_EBADF;
    if (!files[fd].used)
        return SYSCALL_EBADF;

    files[fd].used = false;
    return 0;
}

/* Read from an open file, advancing its offset. */
static int64_t read_file(int fd, uint64_t buf, uint64_t len)
{
    struct open_file *files = process_open_files();
    if (!files || fd <= STDERR_FILENO || fd >= PROCESS_MAX_OPEN_FILES)
        return SYSCALL_EBADF;
    if (!files[fd].used)
        return SYSCALL_EBADF;

    const struct ramfs_file *file = ramfs_at(files[fd].file);
    if (!file)
        return SYSCALL_EBADF;

    /* Reading at or past the end is not an error; it is how a program
     * tells it has finished. */
    if (files[fd].offset >= file->size)
        return 0;

    uint64_t remaining = file->size - files[fd].offset;
    uint64_t count     = len < remaining ? len : remaining;

    memcpy((void *)buf, file->data + files[fd].offset, (size_t)count);
    files[fd].offset += count;

    return (int64_t)count;
}

/*
 * One directory entry by position.
 *
 * Positional rather than a cursor a program holds, because there is no
 * per-process directory state to keep and nothing to leak. Returns 1 at
 * the end, which is a condition rather than a failure.
 */
static int64_t sys_readdir(uint64_t index, uint64_t out_ptr)
{
    if (!user_pointer_ok(out_ptr, sizeof(struct dirent)))
        return SYSCALL_EFAULT;

    const struct ramfs_file *file = ramfs_at((int)index);
    if (!file)
        return 1;

    struct dirent *out = (struct dirent *)out_ptr;
    memset(out, 0, sizeof(*out));

    size_t length = strlen(file->name);
    if (length > DIRENT_NAME_MAX - 1)
        length = DIRENT_NAME_MAX - 1;

    memcpy(out->name, file->name, length);
    out->name[length] = '\0';
    out->size         = file->size;
    out->is_directory = 0;

    return 0;
}

/* --- Dispatch ------------------------------------------------------ */

static int64_t syscall_dispatch(uint64_t number, uint64_t a0, uint64_t a1,
                                uint64_t a2, uint64_t a3, uint64_t a4,
                                uint64_t a5)
{
    (void)a3;
    (void)a4;
    (void)a5;   /* no call takes more than three arguments yet */

    switch (number) {
    case SYS_WRITE:
        return sys_write((int)a0, a1, a2);

    case SYS_READ:
        return sys_read((int)a0, a1, a2);

    case SYS_OPEN:
        return sys_open(a0, a1);

    case SYS_CLOSE:
        return sys_close((int)a0);

    case SYS_READDIR:
        return sys_readdir(a0, a1);

    case SYS_GETKEY:
        return kbd_getchar();

    case SYS_UPTIME_MS:
        /* Milliseconds since the timer came up. The tick count is the
         * only clock a program gets: it is monotonic, it does not wrap,
         * and it is not affected by anything the program does. */
        return (int64_t)timer_millis();

    case SYS_CLEAR:
        fb_clear();
        return 0;

    case SYS_EXIT:
        /*
         * Records the intent and returns. The program does not resume:
         * the interrupt post-hook sees the flag, sends the CPU back to
         * process_run, and this address space goes away.
         */
        process_request_exit((int)a0);
        return 0;

    default:
        /* Named rather than silent. A program built against a newer
         * interface than the kernel has should be able to tell "not
         * implemented" apart from "failed". */
        return SYSCALL_ENOSYS;
    }
}

static void syscall_handler(struct interrupt_frame *frame, void *ctx)
{
    (void)ctx;

    /*
     * The gate cleared IF on entry, because it is an interrupt gate. Turn
     * it back on.
     *
     * This is not optional. A call that blocks waiting for input does so
     * with hlt, and with interrupts disabled hlt never returns -- the
     * first program to ask for a line of input would hang the machine.
     * Linux re-enables interrupts early in its own system call entry for
     * the same reason.
     *
     * Nested interrupts land on this same kernel stack. That is fine: the
     * stack belongs to the process, is 16 KiB, and the handlers that can
     * run during a system call are the timer and the keyboard.
     */
    interrupts_enable();

    /*
     * The frame holds the register state the program will resume with, so
     * writing the result into rax here is what makes it the return value
     * of the call in the program's own terms.
     */
    frame->rax = (uint64_t)syscall_dispatch(frame->rax,
                                            frame->rdi, frame->rsi,
                                            frame->rdx, frame->r10,
                                            frame->r8,  frame->r9);
}

void syscall_init(void)
{
    /*
     * DPL 3 is what makes the vector reachable from Ring 3. Every other
     * vector stays at DPL 0, so user code cannot raise a fault handler or
     * a device interrupt by executing INT with its number.
     */
    idt_set_dpl(SYSCALL_VECTOR, 3);

    if (!irq_register(SYSCALL_VECTOR, syscall_handler, NULL))
        kprintf("syscall: vector 0x%x is already claimed\n",
                (unsigned)SYSCALL_VECTOR);
}
