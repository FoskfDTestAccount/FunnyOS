/*
 * The DOS layer: what a program's memory looks like once DOS has loaded it,
 * and what the machine has to be told before it runs.
 *
 * ---------------------------------------------------------------------
 * Why this is separate from the firmware
 *
 * dos/intr installs a machine: an interrupt vector table, four services
 * with devices behind them, and a text page. None of that is DOS. A .COM
 * loaded by this file runs on exactly the same machine, and the difference
 * is the 256 bytes in front of it and the register state it is entered in.
 *
 * Keeping them apart is not tidiness. The firmware's authority is IBM's
 * BIOS, one layer down from any operating system; this layer's authority is
 * MS-DOS, which is a program that ran on top of that BIOS and could have
 * been written by anybody. Two different sets of facts, two different
 * documents -- see docs/dos-refs-dos.md, and dos-refs.md section 9, which
 * recorded this layer as uncovered from M4 until now.
 *
 * ---------------------------------------------------------------------
 * The one place the two layers disagree, and it is on purpose
 *
 * dos/corpus/bios/replay.h froze an entry convention for M3 and M4 samples
 * with **interrupts disabled** (FLAGS = 0xF002). Real DOS enters a program
 * with IF set, and this loader does it DOS's way.
 *
 * So a program loaded through here can be interrupted and one loaded by the
 * corpus replayer cannot, and the two sets of samples keep their own
 * conventions. That is not an inconsistency to be tidied away: the corpus
 * convention exists so that M3's samples did not have to move when a PSP
 * arrived, and replay.h says so where it is defined. What would be wrong is
 * changing one of them to match the other without saying which samples
 * depend on which.
 *
 * ---------------------------------------------------------------------
 * What is in here and what is not
 *
 * In: the PSP, the environment block, and the loader that puts a flat
 * binary behind them and points the registers at it.
 *
 * Not in: the INT 21h dispatcher itself, which is int21.h. What is here is
 * the *map* -- the vectors a DOS program knows by number and the offsets
 * it reads out of its own segment -- and the dispatcher is a service like
 * any other.
 */
#ifndef VM86_DOS_H
#define VM86_DOS_H

#include <stdbool.h>
#include <stdint.h>

#include <vm86/cpu.h>
#include <vm86/firmware.h>

/* ------------------------------------------------------------------ */
/* The DOS interrupt vectors                                           */
/* ------------------------------------------------------------------ */

/*
 * Five numbers every DOS program knows directly, which is why they are
 * here next to the PSP offsets rather than in the file that implements
 * them: they are an interface, not an implementation.
 *
 * They are DOS's, not the BIOS's, and the split matters. firmware.h holds
 * the BIOS's numbers because a program that wants the keyboard calls 16h
 * on any machine IBM ever made; these are MS-DOS's, which is a program
 * that ran on top of that firmware and could have been written by
 * anybody. See the note at the top of this file.
 *
 * 23h has a default Ctrl-C exit service since W6. 22h and 24h still
 * have no DOS handler; all three are named because the
 * loader copies the *previous* contents of those three vectors into the
 * PSP, and a number that is copied without being named is a number
 * nobody can look up. See VM86_PSP_OLD_INT22 and docs/dos-refs-dos.md
 * section 1.
 */
#define VM86_INT_TERMINATE      0x20u  /* INT 20h: the old exit         */
#define VM86_INT_DOS            0x21u  /* the function dispatcher       */
#define VM86_INT_EXIT_ADDRESS   0x22u  /* where a terminated program goes */
#define VM86_INT_CTRL_BREAK     0x23u  /* Ctrl-Break, and Ctrl-C        */
#define VM86_INT_CRITICAL_ERROR 0x24u  /* a critical I/O error          */

/* ------------------------------------------------------------------ */
/* The Program Segment Prefix                                          */
/* ------------------------------------------------------------------ */

/*
 * Two hundred and fifty-six bytes at offset zero of a program's own
 * segment, and the program starts just after at 0x100.
 *
 * The offsets are written out because the whole point of a PSP is that
 * programs read it *directly* -- `mov ax, [2Ch]` rather than a call -- so
 * these are an interface the way a struct layout is, and a program that
 * finds a field somewhere else is a program that reads somebody else's
 * data. dos-refs-dos.md section 1 has the table with the source for each
 * line; what is here is what this loader actually writes.
 */
#define VM86_PSP_BYTES           256u

#define VM86_PSP_EXIT            0x0000u  /* CD 20: INT 20h, the old exit */
#define VM86_PSP_CPM_ENTRY       0x0005u  /* 5 bytes: CALL 5 far entry    */
#define VM86_PSP_MEMORY_TOP      0x0002u  /* segment past the allocation  */
#define VM86_PSP_OLD_INT22       0x000Au  /* dword: the previous 22h      */
#define VM86_PSP_OLD_INT23       0x000Eu  /* dword: the previous 23h      */
#define VM86_PSP_OLD_INT24       0x0012u  /* dword: the previous 24h      */
#define VM86_PSP_PARENT          0x0016u  /* segment of the parent's PSP  */
#define VM86_PSP_JFT             0x0018u  /* 20 bytes, one handle each    */
#define VM86_PSP_ENVIRONMENT     0x002Cu  /* segment of the environment   */
#define VM86_PSP_SAVED_SS_SP     0x002Eu  /* dword: SS:SP at last INT 21h */
#define VM86_PSP_JFT_SIZE        0x0032u  /* word: how many entries       */
#define VM86_PSP_JFT_POINTER     0x0034u  /* dword: where the JFT is      */
#define VM86_PSP_PREVIOUS        0x0038u  /* dword: previous PSP          */
#define VM86_PSP_VERSION         0x0040u  /* word: version to report      */
#define VM86_PSP_INT21_RETF      0x0050u  /* CD 21 CB                     */
#define VM86_PSP_FCB1            0x005Cu  /* 16 bytes: default FCB        */
#define VM86_PSP_FCB2            0x006Cu  /* 20 bytes: the second one     */
#define VM86_PSP_TAIL_LENGTH     0x0080u  /* byte: bytes in the tail      */
#define VM86_PSP_TAIL            0x0081u  /* up to 126, then a 0Dh        */

/* How many file handles the JFT holds, and what an unused one reads as.
 *
 * Twenty is what DOS uses, and 0xFF is the value DOS leaves in every
 * entry of a fresh table. The DOS layer in this machine has no file
 * handles yet -- that is W5 -- so every entry is free, and a program that
 * guesses a handle and calls a function on it gets "invalid handle"
 * rather than somebody else's file. */
#define VM86_PSP_JFT_ENTRIES     20u
#define VM86_PSP_JFT_FREE        0xFFu

/* The command tail is at most 126 bytes: 0x81 to 0xFF is 127, and the
 * last of those is the CR. */
#define VM86_PSP_TAIL_MAX        126u

/*
 * The version this machine reports, both here and through INT 21h AH=30h.
 *
 * **AL is the major version and AH the minor**, as a plain hexadecimal
 * byte -- so 3.30 is 0x1E03, because 30 decimal is 0x1E, and 5.0 is
 * 0x0005. The two halves are in the order a reader of English would not
 * guess, which is why it is worth saying twice: `0x1E03` read the other
 * way round is version 30.3.
 *
 * This comment used to say the opposite -- that AH was the major version
 * -- while the constant beside it was right. That is the worst shape a
 * wrong fact can take here: the number is what the machine sends and the
 * prose is what a person reads before changing it, so the two disagreed
 * without anything failing. docs/dos-refs-dos.md section 9 has the source
 * (AL = major, AH = minor) and says the same thing about the mistake.
 *
 * WHICH version to claim is a decision rather than a fact, and
 * dos-refs-dos.md section 6 records it: 3.30 is the lowest claim that
 * almost every program of the era accepts, and reporting low makes a
 * program take its conservative path. It is not free -- a program that
 * insists on 5.0 or later will refuse to start here -- and when one of
 * those turns up, this is the one constant that changes.
 */
#define VM86_DOS_VERSION         0x1E03u

/* ------------------------------------------------------------------ */
/* The environment block                                               */
/* ------------------------------------------------------------------ */

/*
 * A sequence of NUL-terminated "NAME=VALUE" strings, then one more NUL to
 * end the list, then a word count and the program's full path:
 *
 *     COMSPEC=C:\COMMAND.COM\0
 *     PATH=F:\0
 *     \0                          <- the list ends here
 *     01 00                       <- one string follows the list
 *     F:\PSP.COM\0                <- and this is it
 *
 * The extra NUL is the part that is easy to get wrong: the list ends with
 * an *empty string*, not with the last string. An environment with no
 * variables at all is therefore a single zero byte before the count, and
 * code that searches for two zeros in a row walks off the end of it.
 *
 * The count and the path are DOS 3.0 and later. A program that only knows
 * DOS 2 stops reading at the first NUL after the list, so their presence
 * costs it nothing -- but their *length* is not something anything should
 * assume, which is why the block is walked rather than indexed.
 */

/* ------------------------------------------------------------------ */
/* Starting a program                                                  */
/* ------------------------------------------------------------------ */

/*
 * Everything the caller decides when it starts a program.
 *
 * A structure rather than six parameters, because most of these are
 * independent choices and a call with six positional arguments is a call
 * nobody can read at the place it matters.
 *
 * The two segments are the caller's to choose, and that is deliberate:
 * laying out guest memory is the machine's business, not the loader's.
 * A loader that picked its own addresses would be a loader that has to be
 * edited the day something else wants to live down there.
 */
struct vm86_dos_start {
    /* Where the PSP goes. The program follows it at :0x100, and the whole
     * 64 KiB block from here is what the program is told it owns. */
    uint16_t segment;

    /* Where the environment block goes, and it must not be inside the
     * program's block -- see vm86_dos_load. */
    uint16_t environment;

    /* The parent PSP's segment. A program with no parent passes its own,
     * which is what this machine's single process does today. */
    uint16_t parent;

    /* The program's full path, for the environment's trailer. NULL leaves
     * the trailer empty, which is what a program with no name has. */
    const char *path;

    /* The command tail, verbatim: the bytes after the program name on the
     * line that started it.
     *
     * NOTHING IS ADDED TO THE FRONT OF IT. It is a habit rather than a
     * rule that a tail starts with a blank, and a loader that inserts one
     * is a loader that has adopted the mistake -- see dos-refs-dos.md
     * section 4, where the counterexample is a program invoked as
     * `CHKDSK/V C:`. The caller passes what was typed; the loader copies
     * it and appends the CR. */
    const char *tail;

    /* "NAME=VALUE" strings, NULL when there are none. The loader adds the
     * list terminator, the count and the path. */
    const char *const *vars;
    uint32_t           var_count;
};

/* What came of it. Everything a caller or a test wants to look at without
 * reading it back out of guest memory -- though reading it back out is
 * exactly what a test should do, and what corpus/dos/psp.asm exists for. */
struct vm86_dos_psp {
    uint16_t segment;
    uint16_t memory_top;      /* what went in at PSP:0002          */
    uint16_t environment;     /* PSP:002C                          */
    uint16_t paragraphs;      /* the environment's size, in 16-byte units */
    uint8_t  tail_length;     /* PSP:0080                          */
};

/*
 * Why a load was refused.
 *
 * An enumeration rather than a boolean, because "it would not start" is
 * not a thing anybody can act on. Each of these is a different mistake by
 * the caller and each one is reachable, so a test can make the loader say
 * which; a loader that returned false and nothing else would leave the
 * suite asserting that *something* was refused, which is the shape of
 * assertion that stays green when the wrong thing is refused.
 */
enum vm86_dos_load_result {
    /* The processor is set up and the program is in memory. */
    VM86_DOS_LOADED = 0,

    /* The image plus its PSP does not fit between the segment and 64 KiB
     * later. A .COM is one segment by definition; anything bigger is an
     * EXE and needs the loader this machine does not have yet. */
    VM86_DOS_IMAGE_TOO_BIG,

    /* The command tail is longer than the PSP can hold. */
    VM86_DOS_TAIL_TOO_LONG,

    /* The environment segment is inside the program's own 64 KiB block,
     * so building it would overwrite the program. */
    VM86_DOS_ENVIRONMENT_IN_THE_WAY,

    /* The environment block does not fit in one segment. */
    VM86_DOS_ENVIRONMENT_TOO_BIG,

    /* The environment segment is not inside the guest's memory. */
    VM86_DOS_NO_SUCH_SEGMENT,
};

/*
 * Build an environment block at `segment` and return how many paragraphs
 * it occupies, or 0 if it does not fit in one segment.
 *
 * Separate from the loader because it is separately checkable: a block is
 * a block, and a test can read it back byte for byte without a program
 * having run. The loader calls it; nothing else has to.
 *
 * A return of 0 cannot be confused with a real answer: the smallest block
 * this can build -- no variables, no path -- is one terminator byte, one
 * count word and one path terminator, which is still a paragraph.
 */
uint16_t vm86_dos_environment(struct vm86_cpu *cpu, uint16_t segment,
                              const char *path,
                              const char *const *vars, uint32_t var_count);

/*
 * Write a PSP at `start->segment`, load `image` at :0x100, and set the
 * processor up to run it.
 *
 * Every request that does not describe a program this machine can start is
 * *refused* rather than approximated -- see the enumeration above for what
 * each refusal is. That is the same rule the rest of this tree follows for
 * a name that does not fit: truncating produces something that runs and
 * reads a different world than the caller asked for, and the difference
 * shows up as a program behaving oddly rather than as an error.
 *
 * On success the processor is in the state dos-refs-dos.md section 2
 * describes: CS, DS, ES and SS all the program's segment, IP 0x100, SP
 * 0xFFFE with a zero word at SS:FFFE, and IF set. A near RET at the end of
 * the program therefore pops that zero and arrives at PSP:0000, which is
 * an INT 20h -- the reason a .COM needs no exit code at all.
 *
 * The caller still owns the machine after this: the firmware has to be
 * installed, the services registered and the run loop driven. Loading and
 * running are separate because a test wants to look at a loaded machine
 * before it runs, and because whether a program may run at all is not the
 * loader's decision.
 */
enum vm86_dos_load_result vm86_dos_load(struct vm86_cpu *cpu,
                                        const uint8_t *image,
                                        uint32_t image_size,
                                        const struct vm86_dos_start *start,
                                        struct vm86_dos_psp *out);

#endif /* VM86_DOS_H */
