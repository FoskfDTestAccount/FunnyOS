/*
 * funnycom -- the FunnyOS shell.
 *
 * A Ring 3 program. Nothing here is privileged: every file it lists, every
 * byte it prints and every key it reads goes through a system call, which
 * is the point. The shell is the first thing in the project that has to
 * work from outside the kernel, and it is the acceptance criterion for
 * M2.
 *
 * It is deliberately shaped like a DOS shell rather than a Unix one --
 * case-insensitive commands, `dir` and `type` rather than `ls` and `cat`,
 * and "Bad command or file name" when it does not recognise what was
 * typed. That is the reference this project is built against, and the
 * muscle memory it is aiming at is worth more than the consistency of
 * borrowing command names from somewhere else.
 */
#include <libu/libu.h>
#include <libk/string.h>
#include <vm/vm.h>

#include <stdbool.h>

#define SHELL_NAME    "FunnyCOM"
#define SHELL_VERSION "0.1"

#define LINE_MAX 128
#define ARGV_MAX 8

/*
 * The kernel passes one argument at startup: there is no argument vector
 * yet, and the test suite needs to start the same image in different
 * modes without building it three times.
 */
#define ARG_NORMAL     0
#define ARG_FAULT_TEST 1
#define ARG_EXIT_TEST  2
#define ARG_FPU_TEST   3

/*
 * Run the 8086 interpreter instead of the shell, and exit with what it
 * says. The switch is `vm=1` on the kernel command line rather than
 * another `selftest=`, because `selftest=` means "inject a fault on
 * purpose" and this is not a fault -- it is M3's whole deliverable being
 * exercised.
 */
#define ARG_VM         4

/* Exit code for the exit test. Deliberately not zero, so that a test
 * which passes has proved the code travelled back through the kernel
 * rather than the kernel reporting whatever it had to hand. */
#define EXIT_TEST_CODE 7

/*
 * One program starting another.
 *
 * The parent asks the kernel to run this same image in the child's mode
 * and gets the child's exit code back. What is being tested is not the
 * code that travels back -- it is everything the kernel had to put back
 * afterwards: the address space, the current process, and the kernel stack
 * an interrupt lands on. See process_run, and the "Stack check" line in
 * the boot log that says whether it worked.
 */
#define ARG_SPAWN_TEST  5
#define ARG_SPAWN_CHILD 6

/* Deliberately not zero, for the same reason EXIT_TEST_CODE is not. */
#define SPAWN_CHILD_CODE 42

/*
 * How long each side spins, in milliseconds.
 *
 * Long enough for several timer ticks at 100 Hz, and that is the point
 * rather than incidental: a tick has to arrive *while* each side is in Ring
 * 3, or the check has nothing to look at. The one that matters most is the
 * parent's second spin -- the one after the child is gone, when a stack
 * that was not given back would be the child's, already freed.
 */
#define SPAWN_SPIN_MS 60

/*
 * Spin for `ms`, burning the time in Ring 3 rather than in the kernel.
 *
 * The inner loop is what makes that true, and it is not a detail. Asking
 * the clock every turn -- which is the obvious way to write this, and what
 * the floating point check does -- spends nearly all of the wall time
 * inside a system call, and a system call is the kernel. A tick arriving
 * there is on the kernel's own stack with no program behind it, so the
 * question this whole test asks never comes up. The outer loop still asks
 * the clock, but only once every few hundred microseconds of arithmetic.
 *
 * The count is not calibrated to anything. It does not need to be: what
 * matters is that the program is running rather than waiting, and a
 * thousand times too long is the same as a thousand times too short.
 */
static void spin_ms(unsigned long ms)
{
    unsigned long until = u_uptime_ms() + ms;

    volatile unsigned long sink = 0;

    while (u_uptime_ms() < until) {
        for (unsigned long i = 0; i < 200000; i++)
            sink += i;
    }
}

/* ------------------------------------------------------------------ */
/* The memory the kernel builds and the image does not contain         */
/* ------------------------------------------------------------------ */

/*
 * Two sizes are in play from here on, and this array is the difference
 * between them.
 *
 * It lives in .guestram, which user/link.ld marks NOLOAD. The section
 * takes up address space and not one byte of the flat image, so the
 * kernel maps it and zeroes it instead of copying it -- and the size the
 * kernel is given comes from `_image_end`, the linker's idea of where
 * this program's memory ends, not from the length of the image.
 *
 * That distinction is the whole reason for the second size. An array of
 * this size in .bss would be forced into the image and then into the C
 * source the image is compiled into, at six characters a byte. The
 * emulator that will run DOS programs needs sixteen megabytes of guest
 * RAM and declares it the same way; this is the small version that can
 * be checked on every boot.
 *
 * Deliberately much larger than this program's code, and checked at its
 * last byte -- which is the last byte of the program's memory, and so
 * the first byte an off-by-one-page in the loader would leave out.
 */
#define GUEST_RAM_BYTES (256 * 1024)

static unsigned char g_guest_ram[GUEST_RAM_BYTES]
    __attribute__((section(".guestram")));

/*
 * Read back the memory the loader was asked to build.
 *
 * Three things, checked separately because they fail separately, and
 * because only one of the three failures announces itself:
 *
 *   Mapped.     Reading the last byte at all is the check. A page the
 *               loader never mapped does not return a wrong value, it
 *               faults, and the kernel ends the process.
 *   Zeroed.     Every byte is scanned rather than sampled. A page that
 *               was mapped but not cleared holds whatever the last
 *               process left in that frame, and that is a value that
 *               looks entirely reasonable -- which is how it survives
 *               review. There is one process today; there will be more.
 *   Writable.   A pattern is written to both ends and read back. The far
 *               end is the last byte of the region, so this crosses the
 *               final page boundary as well.
 *
 * The order matters and is the reason this is one function: every byte is
 * read before anything is written, so the zeroes are the loader's and not
 * this check's own leftovers.
 */
static bool check_loader_memory(void)
{
    volatile unsigned char *ram = g_guest_ram;

    unsigned long not_zero = 0;

    for (unsigned long i = 0; i < GUEST_RAM_BYTES; i++) {
        if (ram[i] != 0)
            not_zero++;
    }

    bool zeroed = (not_zero == 0);

    /* Only now, with the scan behind us. */
    ram[0] = 0xA5;
    ram[GUEST_RAM_BYTES - 1] = 0x5A;

    bool writable = (ram[0] == 0xA5 && ram[GUEST_RAM_BYTES - 1] == 0x5A);

    /* Put back what was there, so that a later reader of this memory --
     * or a later run of this check -- is not looking at this one. */
    ram[0] = 0;
    ram[GUEST_RAM_BYTES - 1] = 0;

    uprintf("  Memory tail    : %lu bytes past the end of the image\n",
            (unsigned long)GUEST_RAM_BYTES);
    uprintf("  Tail mapped    : yes\n");
    uprintf("  Tail zeroed    : %s\n",
            zeroed ? "yes" : "NO, some bytes were not zero");
    uprintf("  Tail writable  : %s\n",
            writable ? "yes" : "NO, a value did not read back");

    if (!zeroed) {
        uprintf("                   %lu of %lu bytes were not zero\n",
                not_zero, (unsigned long)GUEST_RAM_BYTES);
    }

    return zeroed && writable;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Compare without regard to case, because a DOS command line is
 * case-insensitive and pretending otherwise would make `DIR` fail. */
static int same_command(const char *a, const char *b)
{
    for (;;) {
        char x = *a++;
        char y = *b++;

        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');

        if (x != y)
            return 0;
        if (x == '\0')
            return 1;
    }
}

/*
 * Split a line into words in place, writing NULs over the separators.
 *
 * Returns the number of words. Extra spaces are skipped rather than
 * producing empty arguments, so `type  NOTES.TXT` and `type NOTES.TXT`
 * behave the same.
 */
static int split(char *line, char **argv, int max)
{
    int argc = 0;
    char *p = line;

    while (*p && argc < max) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            break;

        argv[argc++] = p;

        while (*p && *p != ' ' && *p != '\t')
            p++;
        if (*p)
            *p++ = '\0';
    }

    return argc;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static void cmd_help(int argc, char **argv);

static void cmd_ver(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    uprintf("%s %s\n", SHELL_NAME, SHELL_VERSION);
    uprintf("FunnyOS, a native x86-64 system with a built-in 8086 emulator.\n");
    uprintf("This shell runs in Ring 3 and is unprivileged; everything it\n");
    uprintf("does goes through a system call.\n");
}

static void cmd_uptime(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    unsigned long ms = u_uptime_ms();
    uprintf("Up %lu.%03lu seconds\n", ms / 1000, ms % 1000);
}

static void cmd_cls(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    u_clear();
}

static void cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            uputc(' ');
        uputs(argv[i]);
    }
    uputc('\n');
}

static void cmd_dir(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    struct dirent entry;
    unsigned index = 0, files = 0, bytes = 0;

    uputs("\n Volume in drive F has no label\n");
    uputs(" Directory of F:\\\n\n");

    while (u_readdir(index++, &entry) == 0) {
        uprintf("%-16s %8u bytes\n", entry.name, entry.size);
        files++;
        bytes += entry.size;
    }

    uprintf("%16s %u file(s)   %u bytes\n", "", files, bytes);
}

static void cmd_type(int argc, char **argv)
{
    if (argc < 2) {
        uputs("Syntax: TYPE <filename>\n");
        return;
    }

    int fd = u_open(argv[1], O_RDONLY);
    if (fd < 0) {
        uprintf("File not found: %s\n", argv[1]);
        return;
    }

    char buffer[256];
    long count;

    /* Reading until zero rather than asking for a size first: the size is
     * knowable, but a program that trusts it has to be rewritten the day
     * the file grows between the question and the answer. */
    while ((count = u_read(fd, buffer, sizeof(buffer))) > 0)
        u_write(U_STDOUT, buffer, (size_t)count);

    u_close(fd);
}

static void cmd_exit(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    uputs("Goodbye.\n");
    u_exit(0);
}

/* ------------------------------------------------------------------ */
/* The command table                                                   */
/* ------------------------------------------------------------------ */

struct command {
    const char *name;
    const char *syntax;
    const char *description;
    void      (*run)(int argc, char **argv);
};

static const struct command g_commands[] = {
    { "help",   "HELP",              "list the commands",              cmd_help   },
    { "dir",    "DIR",               "list the files",                 cmd_dir    },
    { "type",   "TYPE <file>",       "print a file",                   cmd_type   },
    { "echo",   "ECHO <text>",       "print the text",                 cmd_echo   },
    { "ver",    "VER",               "show version information",       cmd_ver    },
    { "uptime", "UPTIME",            "time since the timer started",   cmd_uptime },
    { "cls",    "CLS",               "clear the screen",               cmd_cls    },
    { "exit",   "EXIT",              "end the shell",                  cmd_exit   },
};

#define COMMAND_COUNT ((int)(sizeof(g_commands) / sizeof(g_commands[0])))

static void cmd_help(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    uputs("\nAvailable commands:\n\n");
    for (int i = 0; i < COMMAND_COUNT; i++)
        uprintf("  %-16s %s\n", g_commands[i].syntax, g_commands[i].description);
    uputs("\n");
}

static const struct command *find_command(const char *name)
{
    for (int i = 0; i < COMMAND_COUNT; i++)
        if (same_command(g_commands[i].name, name))
            return &g_commands[i];
    return NULL;
}

/* ------------------------------------------------------------------ */

static void banner(void)
{
    uprintf("\n%s %s -- type HELP for a list of commands\n", SHELL_NAME, SHELL_VERSION);
}

static void run_fault_test(void)
{
    /*
     * Deliberately touch an address the kernel never mapped for us. The
     * claim this checks is the one FunnyOS makes against DOS: a program
     * going wrong stops being a program, not a machine.
     */
    uputs("Fault self-test: writing to an address that is not mapped.\n");

    volatile unsigned long *unmapped =
        (volatile unsigned long *)0x00000000DEADB000UL;
    *unmapped = 1;

    /* Reached only if the write did not fault, which would mean the
     * address space has a mapping it should not. */
    uputs("FAILED: the write did not fault.\n");
}

/*
 * Floating point, and whether it survives being interrupted.
 *
 * Two things are being checked and only the second is interesting.
 *
 * The first is that floating point works at all: that the kernel enabled
 * SSE and the x87 unit, and that a program compiled to use them runs
 * instead of faulting.
 *
 * The second is that control leaving the program and coming back does not
 * destroy its arithmetic. Both halves of the saved state are exercised,
 * because they are separate things a handler could damage independently:
 * SSE lives in XMM, and x87 lives on its own register stack, reached
 * through long double.
 *
 * What this does NOT prove, and cannot: that the kernel never touches a
 * vector register. Whether these loops happen to be holding a value in
 * the particular register a hypothetical bad handler would clobber is the
 * compiler's decision, not this test's, and a check that depends on
 * register allocation is a check that will one day report a false pass.
 *
 * That claim is settled statically instead -- see
 * tools/check-no-vector-regs.sh, which disassembles the linked kernel and
 * fails the build if a single vector register appears. This is the
 * end-to-end companion to that: proof that the arrangement works, next to
 * proof that its precondition holds.
 *
 * Everything here is exact in binary floating point, so the comparisons
 * can be equalities rather than epsilons. A test that needed a tolerance
 * would be a test that could not tell corruption from rounding.
 */
#define FPU_LOOP_MS 40

static void run_fpu_test(void)
{
    volatile double      two   = 2.0;
    volatile long double three = 3.0L;   /* volatile, or it all folds away */

    unsigned long start = u_uptime_ms();
    unsigned long now   = start;

    /* --- SSE: 2^30 in a register the compiler keeps in XMM --- */
    unsigned long sse_rounds = 0;
    unsigned long sse_wrong  = 0;

    do {
        double value = 1.0;
        for (int i = 0; i < 30; i++)
            value = value * two;         /* 2^30, exact */

        value = value - 1073741824.0;    /* exactly zero if nothing was lost */
        if (value != 0.0)
            sse_wrong++;

        sse_rounds++;
        now = u_uptime_ms();
    } while (now - start < FPU_LOOP_MS);

    unsigned long sse_ticks = now - start;

    /* --- x87: 3^20 on the register stack, reached via long double --- */
    unsigned long x87_rounds = 0;
    unsigned long x87_wrong  = 0;

    start = u_uptime_ms();
    now   = start;

    do {
        long double value = 1.0L;
        for (int i = 0; i < 20; i++)
            value = value * three;       /* 3^20 = 3486784401, exact */

        value = value - 3486784401.0L;
        if (value != 0.0L)
            x87_wrong++;

        x87_rounds++;
        now = u_uptime_ms();
    } while (now - start < FPU_LOOP_MS);

    unsigned long x87_ticks = now - start;

    /*
     * And a value carried across system calls.
     *
     * Note this one tests the path rather than the registers: the C
     * compiler is entitled to spill a local around a call, and does, so
     * what this proves is that a round trip through the kernel does not
     * corrupt the program's arithmetic -- not that a specific register
     * survived. The loops above are the ones that pin registers down.
     */
    double held = 3.0;
    (void)u_uptime_ms();
    held = held * 4.0;
    (void)u_uptime_ms();
    held = held + 1.0;

    uprintf("  SSE rounds    : %lu in %lu ms (%lu timer ticks)\n",
            sse_rounds, sse_ticks, sse_ticks / 10);
    uprintf("  x87 rounds    : %lu in %lu ms (%lu timer ticks)\n",
            x87_rounds, x87_ticks, x87_ticks / 10);
    uprintf("  exact results : %lu wrong out of %lu\n",
            sse_wrong + x87_wrong, sse_rounds + x87_rounds);
    uprintf("  across a call : %s (expected 13)\n",
            held == 13.0 ? "intact" : "CLOBBERED");

    bool ok = sse_wrong == 0 && x87_wrong == 0 &&
              held == 13.0 && sse_rounds > 0 && x87_rounds > 0;

    uprintf("  result        : %s\n", ok ? "PASS" : "FAIL");
}

int u_main(uint64_t arg)
{
    /*
     * First, and on every boot rather than under a flag.
     *
     * The loader is the one part of the kernel every program goes
     * through, and the failures it can have here are the quiet kind, so
     * this is worth the microseconds every time. A boot log with no
     * "Tail mapped" line in it is a boot where the program died before
     * reaching this point.
     */
    if (!check_loader_memory())
        uputs("  Loader memory  : FAILED, and this is a kernel bug\n");

    if (arg == ARG_FAULT_TEST) {
        run_fault_test();
        return 1;
    }

    if (arg == ARG_EXIT_TEST) {
        /* Proves the other way a process can end: by deciding to, with a
         * code, and with the kernel resuming as if it had been called. */
        uputs("Exit self-test: returning a known code.\n");
        return EXIT_TEST_CODE;
    }

    if (arg == ARG_FPU_TEST) {
        run_fpu_test();
        return 0;
    }

    if (arg == ARG_SPAWN_CHILD) {
        uputs("spawn: child running\n");
        spin_ms(SPAWN_SPIN_MS);
        return SPAWN_CHILD_CODE;
    }

    if (arg == ARG_SPAWN_TEST) {
        uputs("spawn: parent running\n");

        /* Before, so that a tick lands on this side while the machine is
         * still the parent's own. */
        spin_ms(SPAWN_SPIN_MS);

        long code = u_spawn("funnycom", ARG_SPAWN_CHILD);
        uprintf("spawn: child returned %ld\n", code);

        /* And after, which is the one that matters: the child is gone by
         * now, and if the kernel did not take its kernel stack back, this
         * is where the next interrupt pushes a frame onto freed memory. */
        spin_ms(SPAWN_SPIN_MS);
        return 0;
    }

    if (arg == ARG_VM) {
        /* The exit code is the interpreter's verdict, so it travels back
         * to the kernel and shows up in the boot log as the result. */
        return vm_selftest();
    }

    banner();

    static char line[LINE_MAX];
    static char *argv[ARGV_MAX];

    for (;;) {
        /*
         * The prompt names a drive because that is the shape of the thing
         * being imitated, and F is simply what this filesystem is
         * called -- there is one, and there are no letters waiting behind
         * it. When FAT images and their drive letters arrive, this stops
         * being a naming convention and starts being a real one.
         */
        uputs("\nF:\\> ");

        long length = u_read(U_STDIN, line, sizeof(line));
        if (length < 0) {
            /* A failed read would otherwise spin: the loop would ask
             * again immediately and fail again. Say so and carry on. */
            uprintf("Read failed (%ld)\n", length);
            continue;
        }

        int argc = split(line, argv, ARGV_MAX);
        if (argc == 0)
            continue;

        const struct command *command = find_command(argv[0]);
        if (!command) {
            uprintf("Bad command or file name: %s\n", argv[0]);
            continue;
        }

        command->run(argc, argv);
    }
}
