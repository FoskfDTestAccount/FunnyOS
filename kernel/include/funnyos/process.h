/*
 * Processes.
 *
 * "Process" here means something smaller than it does on a general-purpose
 * system: an address space, a stack, a kernel stack to land on when it
 * faults or calls a system call, and a place to resume the kernel when it
 * exits. There is no scheduler, because there is nothing to schedule --
 * a program runs until it exits, and its system calls block the CPU while
 * they wait.
 *
 * That is not a shortcut so much as the truth about M2. The DOS programs
 * this system exists to run are single-tasking, and the emulator that will
 * run them has not been written yet. A scheduler arrives when there is
 * more than one thing to run.
 *
 * The isolation is real, though, and it is the point: a program runs in
 * Ring 3 under its own page tables, so a wild pointer faults in a context
 * that cannot touch the kernel. DESIGN.md calls this out as the one place
 * FunnyOS deliberately departs from DOS, and this file is where that
 * promise is kept or broken.
 */
#ifndef FUNNYOS_PROCESS_H
#define FUNNYOS_PROCESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Where a program is loaded, and where its stack lives.
 *
 * Both are in the lower half, which is the half a process owns -- see
 * VMM_KERNEL_PML4_INDEX. The stack top is a round number well clear of
 * the program and of the top of the half, so a modest overrun grows into
 * unmapped space rather than into anything else.
 *
 * The distance between the two is the program's memory, and it is
 * deliberately not small. A process may be given a region many times the
 * size of its image -- the emulator that will run DOS programs wants
 * megabytes of guest RAM -- so the stack has to sit above anything a
 * program can be given, rather than a fixed distance above its code.
 *
 * This used to be 8 MiB, and that was wrong in a way worth recording.
 * The code base at 4 MiB left 4 MiB between the program and the stack,
 * and the stack is mapped *after* the program: a process asked for more
 * memory than that would not have failed, it would have had its own
 * pages silently replaced by the stack's. Nothing tested it, because
 * nothing had ever asked for a large region. See process_create. */
#define PROCESS_CODE_BASE   0x0000000000400000ULL   /* 4 MiB */
#define PROCESS_STACK_TOP   0x0000000004000000ULL   /* 1 GiB */
#define PROCESS_STACK_SIZE  (64 * 1024)

/* Kernel stack used while the process is inside a system call or a fault
 * handler. Reached through the TSS, so an interrupt taken in Ring 3 lands
 * here rather than on the process's own stack -- which a process cannot
 * be trusted to have left in any particular state. */
#define PROCESS_KERNEL_STACK_SIZE (16 * 1024)

/*
 * Exit codes at or above this are not a program's choice: the value is
 * this base plus the fault vector, so that a caller can tell "the program
 * decided to stop" from "the program was stopped".
 *
 * The base is 128, matching the convention Unix uses for the same
 * purpose. It is worth matching because it is a signal rather than a
 * return value, and the two are exactly the things that should not look
 * alike. The cost is that a program which deliberately exits with a code
 * of 128 or more is indistinguishable from one that faulted, which is a
 * trade Unix makes too.
 */
#define PROCESS_EXIT_FAULT_BASE 128

/* Open files a process can hold at once. Deliberately small: the table
 * lives inside the process structure and is zeroed with it, and eight is
 * more than a shell or a DOS program working normally ever needs. */
#define PROCESS_MAX_OPEN_FILES 8

/*
 * One open file.
 *
 * `file` is an index into the read-only filesystem, not a pointer: the
 * number is what a program gets back and hands in again, and keeping the
 * table to indices means a program cannot name something the kernel did
 * not put there.
 */
struct open_file {
    bool     used;
    int      file;
    uint64_t offset;
};

/*
 * Where the kernel left off when it entered Ring 3.
 *
 * The field order is fixed by usermode.asm, which saves and restores it
 * as raw offsets. Only the callee-saved registers are here, which is all
 * the C calling convention requires across a longjmp.
 */
struct kernel_context {
    uint64_t rbx, rbp, r12, r13, r14, r15;
    uint64_t rsp;
    uint64_t rip;
};

/* Longest name a process can carry, including the terminator. Short
 * because it is a diagnostic label, not a path. */
#define PROCESS_NAME_MAX 32

struct process {
    /* The name is owned, not borrowed -- see process_create. */
    char     name[PROCESS_NAME_MAX];

    uint64_t pml4;          /* physical address of the top-level table */
    uint64_t entry;
    uint64_t stack_top;

    /* How much memory this process owns, measured from
     * PROCESS_CODE_BASE -- not how long its image is. The two differ
     * whenever a program asks for a region longer than the bytes copied
     * into it, and teardown frees frames by this range, so recording the
     * image length here instead would leak the whole tail. */
    size_t   memory_size;

    void    *kernel_stack;
    size_t   kernel_stack_size;

    /*
     * This process's floating point state, FPU_STATE_SIZE bytes at
     * FPU_STATE_ALIGN.
     *
     * The kernel never touches a vector register, so an interrupt taken
     * during a floating point calculation needs no save. But control
     * passing *between* two processes does, and this is where that state
     * lives. With one process at a time it goes back exactly where it came
     * from; the field exists now because the alternative is a scheduler
     * that silently interleaves two programs' registers.
     */
    void    *fpu_state;

    /* Set by SYS_EXIT, acted on by the interrupt post-hook. */
    bool     exited;
    int      exit_code;

    struct kernel_context resume;

    struct open_file files[PROCESS_MAX_OPEN_FILES];
};

/* Install the exit hook. Call once, after the interrupt registry exists. */
void process_init(void);

/*
 * Load a flat binary into a fresh address space.
 *
 * Flat, not ELF: there is no dynamic loader, no shared libraries and no
 * relocations to apply, so the two things ELF would buy -- section
 * permissions and a symbol table -- are not worth a parser yet. The front
 * of the region is filled from `image` and that is the whole loader.
 *
 * Two sizes rather than one, which is the distinction ELF draws between
 * p_filesz and p_memsz. `image_size` says how many bytes to copy;
 * `memory_size` says how much address space to build. The difference --
 * the tail -- is mapped, zeroed and never written again.
 *
 * The tail cannot be described by the image itself, and that is the
 * reason for a second parameter at all. A flat binary is exactly its own
 * length, so it says nothing about memory the program will use but does
 * not yet carry; only the linker knows where the program's memory ends,
 * and it has to be asked separately. The point of the arrangement is
 * that a program can want megabytes of zeroed space without those
 * megabytes travelling through the image -- and the image is compiled
 * into the kernel as a C array at six characters a byte, so a large
 * array in the image is a large array in the kernel source.
 *
 * Returns NULL on failure, having released anything it allocated.
 *
 * A `memory_size` below `image_size`, or one large enough to reach the
 * stack, is a caller's mistake rather than a shortage of memory, and is
 * rejected the same way. The second is checked because the alternative
 * is not an error at all: the stack is mapped after the program, so a
 * region that reached it would have its pages quietly replaced.
 */
struct process *process_create(const char *name, const void *image,
                               size_t image_size, size_t memory_size);

/*
 * Run a process to completion and return its exit code.
 *
 * `arg` is handed to the program as its first parameter. There is no
 * argument vector and no environment block yet, so one value in a
 * register is the whole of the interface between whoever starts a program
 * and the program itself -- enough for a program to be started in more
 * than one mode, which is what the tests need.
 *
 * Blocks until the process calls SYS_EXIT. Control returns here through
 * the context saved on entry -- see kernel_longjmp in usermode.asm.
 */
int process_run(struct process *p, uint64_t arg);

/* Release everything a process owns. Must not be called while it is
 * running. */
void process_destroy(struct process *p);

/*
 * End the running process because it faulted. Never returns.
 *
 * The process is unwound exactly as a normal exit unwinds it, with the
 * fault vector as the exit code, so that a caller can tell "ended" from
 * "was killed" without a second mechanism.
 *
 * Only valid when a process is running: a fault with no process behind it
 * is the kernel's own, and this reports that and panics rather than
 * pretending something else went wrong.
 */
void process_abort_on_fault(uint64_t vector) __attribute__((noreturn));

/* The address space currently in use by a running process, or 0 when the
 * kernel is on its own. */
uint64_t process_current_pml4(void);

/*
 * Does `sp` point into the running process's kernel stack?
 *
 * This exists so that one claim can be checked continuously rather than
 * argued about: **an interrupt taken while a process is in Ring 3 lands on
 * that process's kernel stack.** The CPU does it by way of the TSS, so the
 * claim is really about rsp0 -- and rsp0 is the piece of state a nested run
 * has to give back, because leaving it pointing at a finished child means
 * the parent's next interrupt pushes its frame onto freed memory.
 *
 * Watching the machine rather than the variable is the point. Asserting
 * that rsp0 was restored would only assert that the line which restores it
 * ran; this asks where the frame actually went.
 *
 * False when no process is running, which for an interrupt from Ring 3 is
 * itself the violation -- a program was running and the kernel did not
 * know about it.
 */
bool process_kernel_stack_contains(uint64_t sp);

/*
 * The open-file table of the running process, or NULL when the kernel is
 * on its own. The system call layer uses this; nothing else should.
 */
struct open_file *process_open_files(void);

/* --- Called by the system call layer -------------------------------- */

/* Record that the running process wants to end. Does not return to the
 * caller's process; the interrupt post-hook unwinds instead. */
void process_request_exit(int code);

#endif /* FUNNYOS_PROCESS_H */
