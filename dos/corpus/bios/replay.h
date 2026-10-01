/*
 * The BIOS corpus replayer: run one assembled .COM against a whole machine
 * and grade what it left behind.
 *
 * ---------------------------------------------------------------------
 * Why this is not corpus/replay.c
 *
 * The M3 replayer runs a program on a bare 8086: it steps the interpreter
 * until the machine halts and compares registers and memory. That is the
 * right thing for a program that only computes.
 *
 * These programs do not compute, they talk. They execute INT 10h and INT
 * 13h, they wait to be interrupted, and what they are graded on is what
 * ended up on a screen rather than in a register. None of that exists on a
 * bare processor: the vectors have to be installed, four services have to
 * be registered with devices behind them, the host has to take the
 * processor back at intervals to advance a clock and poll a keyboard, and
 * the screen has to be read out of guest memory because a program may
 * write it without asking anybody.
 *
 * So this file builds a machine rather than a processor, and drives it the
 * way the real one will be driven: in slices, with the display refreshed
 * between them. What makes it a test rather than a second implementation
 * is that every device is a double -- the display records instead of
 * drawing, the disk is an array built in the case, the keyboard is fed by
 * the plan, the clock is advanced by hand. Nothing here is a device; they
 * all just answer.
 *
 * ---------------------------------------------------------------------
 * The entry convention, which is the one corpus/replay.h froze
 *
 * A flat binary assembled with `org 0x100`, entered at 0x0000:0x0100, with
 * every segment zero, SP = 0xFFFE and FLAGS = 0xF002.
 *
 * The one thing worth restating, because every sample that uses an
 * interrupt runs into it: 0xF002 has IF CLEAR, and real DOS enters a
 * program with IF set. That deviation is the corpus convention's, kept so
 * that adding a PSP in M5 does not move M3's samples, and its consequence
 * is that a sample needing an interrupt has to say `sti` itself. Each .asm
 * in corpus/bios says whether it does.
 *
 * ---------------------------------------------------------------------
 * What a service may and may not put on the stack, since two samples
 * depend on it
 *
 * A retry does NOT pop the interrupt frame. vm86_service_retry re-runs the
 * trap and leaves the frame where the guest's own INT put it, so a blocked
 * INT 16h holds six bytes of stack for as long as it waits, and the stub's
 * IRET unwinds them once, when the read finally succeeds. key.asm asserts
 * both halves of that: SP == 0xFFF8 while waiting, 0xFFFE afterwards.
 */
#ifndef VM86_BIOS_REPLAY_H
#define VM86_BIOS_REPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include <vm86/cpu.h>
#include <vm86/firmware.h>

/*
 * Is there firmware in this tree to build a machine out of?
 *
 * Tasks A-D own the four service modules and the headers that describe
 * them; until those land, this file and its suite are still written but
 * there is nothing to register. The check is on the headers rather than on
 * a build flag because the headers are exactly what is missing, and
 * because it makes the suite self-correcting: the day bios10.h appears,
 * the tests start running, with nobody editing this file.
 *
 * The relative paths are the ones the modules' own suites have to use:
 * dos/Makefile compiles with -Iinclude, which reaches dos/include and not
 * dos/, so a header at dos/bios/bios10.h is reachable only as a relative
 * include from the file that wants it.
 */
#if defined(__has_include)
#  if __has_include("../../bios/bios10.h") && \
      __has_include("../../bios/bios13.h") && \
      __has_include("../../bios/bios16.h") && \
      __has_include("../../bios/bios1a.h")
#    define VM86_BIOS_REPLAY_HAVE_SERVICES 1
#  endif
#endif

#ifndef VM86_BIOS_REPLAY_HAVE_SERVICES
#  define VM86_BIOS_REPLAY_HAVE_SERVICES 0
#endif

/* ------------------------------------------------------------------ */
/* The convention, as constants                                        */
/* ------------------------------------------------------------------ */

/*
 * The entry convention, and the one place it differs from M3's.
 *
 * M3's samples run on a bare processor with no firmware in it, and
 * corpus/replay.h loads them at linear 0x100 with every segment zero.
 * That is fine when nothing else lives at the bottom of memory.
 *
 * A machine has something else there. The interrupt vector table is at
 * 0x0000-0x03FF -- it is where the 8086 looks when it takes an interrupt,
 * and nothing in this machine moves it -- so a program loaded at linear
 * 0x100 with CS = 0 sits *on top of the table*, and the first 768 bytes of
 * its image ARE vector entries 64 and up.
 *
 * That was survivable by luck, and only by luck. Nothing in this corpus
 * uses a vector above 0x1C, so nothing was ever overwritten in a way that
 * showed; and the program's own bytes were safe because the firmware is
 * installed *before* the program is loaded. Both are accidents of what
 * these seven programs happen to do. A TSR -- which is what M5 is for --
 * hooks a vector of its own choosing, conventionally somewhere above 0x60,
 * and would find its handler's entry buried under its own code.
 *
 * So a program here gets a segment of its own, the way a real .COM does:
 * DOS loads one at a PSP segment and points CS, DS, ES and SS at it, and
 * that is exactly why real .COM programs never had this problem. All four
 * and not just CS, because a program's data and stack are in its image
 * too -- `mov si, message` against DS = 0 reads the vector table rather
 * than the program.
 *
 * 0x1000 is a choice and not a fact: any paragraph that keeps the image
 * out of the table's way and leaves room for a stack below it will do.
 * 0x1000 gives a 64 KiB block from linear 0x10000, the stack at 0x1FFFE,
 * and the scratch a sample keeps for itself from 0x10800 up -- all of it
 * inside the program's own memory and none of it near a vector entry.
 *
 * The absolute addresses the samples still use are the ones that are
 * genuinely absolute: the vector table at 0x0000 (timer.asm hooks 1Ch),
 * the data area at segment 0x0040, and video memory at 0xB8000. Those take
 * an explicit segment, which is what the hardware requires and what a
 * program of the era wrote.
 *
 * M5 inherits this. A loader that builds a PSP will put the program at
 * PSP:0x100 and point the four segment registers at the PSP, and nothing
 * here has to change for that to work.
 */
#define VM86_BIOS_LOAD_SEGMENT  0x1000u
#define VM86_BIOS_LOAD_OFFSET   0x0100u
#define VM86_BIOS_STACK_TOP     0xFFFEu
#define VM86_BIOS_INITIAL_FLAGS VM86_FLAG_ALWAYS_SET

/* Where that segment lands in guest memory. A sample's own scratch is
 * addressed from its segment, so the suite adds this to read it back. */
#define VM86_BIOS_LOAD_LINEAR   ((uint32_t)VM86_BIOS_LOAD_SEGMENT << 4)

/*
 * Guest memory a case gets. A megabyte, which is what an 8086 can address,
 * and therefore the whole of conventional memory plus the 0xF0000 the
 * stubs are written into.
 */
#define VM86_BIOS_MEMORY        (1u * 1024u * 1024u)

/* The disk a machine has: one 1.44 MB floppy, the only geometry M4 knows. */
#define VM86_BIOS_DISK_CYLINDERS 80u
#define VM86_BIOS_DISK_HEADS     2u
#define VM86_BIOS_DISK_SECTORS   18u
#define VM86_BIOS_DISK_SECTOR    512u
#define VM86_BIOS_DISK_BYTES \
    (VM86_BIOS_DISK_CYLINDERS * VM86_BIOS_DISK_HEADS * \
     VM86_BIOS_DISK_SECTORS * VM86_BIOS_DISK_SECTOR)

/*
 * How many bytes the image behind that disk actually has.
 *
 * The geometry and the image are two different things -- bios13_disk
 * carries both, and bios13_init() gives every disk the standard floppy
 * geometry whatever length the array is -- so the bytes do not have to
 * fill the geometry out. Eight sectors is enough to hold the one
 * disk.asm reads, which is what makes this a test rather than a
 * 1 474 560-byte allocation of zeroes.
 */
#define VM86_BIOS_DISK_IMAGE_BYTES 4096u

/* The text page, in the units the assertions want to talk about. */
#define VM86_BIOS_CELLS_PER_ROW 80u
#define VM86_BIOS_ROWS          25u
#define VM86_BIOS_TEXT_BASE     0xB8000u

/* ------------------------------------------------------------------ */
/* The screen, recorded                                                */
/* ------------------------------------------------------------------ */

/*
 * What the display was last shown.
 *
 * This is the double that stands in for a screen, and it is the only
 * reason "the character appeared" can be an assertion instead of somebody
 * looking at a monitor. It holds a copy rather than a pointer because the
 * pointer handed to present() is into guest memory and is valid only for
 * the length of the call -- the contract display.h states, and the one
 * thing a recording display has to get right.
 */
struct vm86_bios_screen {
    uint8_t  cells[VM86_TEXT_CELLS * 2u];   /* 4000 bytes, chars and attrs */
    uint16_t cursor;                        /* cell index, or NO_CURSOR   */
    uint32_t presents;                      /* how many frames arrived    */
};

/* One cell, in the form the assertions want to compare it in. */
struct vm86_bios_cell {
    uint8_t character;
    uint8_t attribute;
};

/* ------------------------------------------------------------------ */
/* What the host is told to do while the program runs                  */
/* ------------------------------------------------------------------ */

/*
 * The plan.
 *
 * The host must not become a scheduler: every decision about when a key
 * arrives or how much time passes is made by the case that is running, not
 * by the machine. That is what keeps a sample that waits for a keystroke a
 * test of the waiting rather than a race.
 *
 * `slices` is the backstop. A sample that never finishes must be reported
 * as one that never finished -- not as a wrong screen, and certainly not
 * as a suite that hangs -- so the run is bounded in host iterations and
 * running out is its own outcome.
 */
struct vm86_bios_plan {
    uint32_t slices;            /* host iterations to allow          */
    uint32_t steps_per_slice;   /* instructions per vm86_run call    */

    /* Milliseconds to hand the clock between slices, or 0 for a machine
     * whose timer never fires. 55 ms a slice makes the clock produce
     * exactly one tick per slice, which is what makes a sample that
     * counts ticks deterministic: 55 ms is just past the 54.925 ms of
     * one tick (docs/dos-refs.md section 6). */
    uint32_t ms_per_slice;

    /* Deliver the key once, on slice `key_at_slice` (counting from 1),
     * and never if it is 0. */
    uint32_t key_at_slice;
    uint8_t  key_scancode;
};

/* ------------------------------------------------------------------ */
/* What came of it                                                     */
/* ------------------------------------------------------------------ */

enum vm86_bios_stop {
    /* The program ended the way an M4 program ends: `cli; hlt`. */
    VM86_BIOS_HALTED = 0,

    /* It faulted with no handler, or the host's opcode tables did not
     * merge. Neither is the sample's fault, and they are kept apart from
     * each other and from the rest for that reason. */
    VM86_BIOS_FAULT,
    VM86_BIOS_BROKEN,

    /* The plan ran out of slices with the machine still runnable. The
     * sample is wrong -- most likely it is waiting for something the plan
     * never delivers -- and saying so in these words is the difference
     * between a reader looking at the sample and a reader looking at a
     * screen that will never change. */
    VM86_BIOS_UNFINISHED,

    /* There are no services in this build to run anything against. See
     * the note on the header check above. */
    VM86_BIOS_NO_FIRMWARE,
};

/* ------------------------------------------------------------------ */
/* The machine                                                         */
/* ------------------------------------------------------------------ */

/*
 * Opaque on purpose. The service states belong to the modules that
 * implement them, and a header that named their fields would be a header
 * that has to change every time one of those modules does -- the same
 * reason the registry is a table of function pointers rather than a struct
 * of four known services.
 */
struct vm86_bios_machine;

/* Build a machine, or return NULL if this build has no firmware for it. */
struct vm86_bios_machine *vm86_bios_machine_new(void);
void                      vm86_bios_machine_free(struct vm86_bios_machine *);

/*
 * The disk the machine reads from: VM86_BIOS_DISK_BYTES of it, zeroed. A
 * case plants whatever it wants the firmware to find. NULL without
 * firmware.
 */
uint8_t *vm86_bios_disk(struct vm86_bios_machine *);

/*
 * Put `image` into guest memory at the convention's address, reset the
 * processor, install the firmware and register the four services.
 *
 * Loading and running are separate calls because one sample needs two
 * runs: key.asm has to be looked at while it is still waiting and again
 * after the key arrives, and that is two `vm86_bios_run` calls on one
 * loaded machine, not two machines.
 */
void vm86_bios_load(struct vm86_bios_machine *,
                    const uint8_t *image, uint16_t image_size);

/*
 * Drive the machine until the program finishes or the plan runs out.
 *
 * A halt is the end of a program only when IF is clear: `cli; hlt`. A halt
 * with IF set is the processor waiting for an interrupt -- M4-6 -- and the
 * host's job there is to advance the clock, or feed a key, or both, and
 * come back. That distinction is the whole reason `sti; hlt` works as an
 * idle loop and it is the host's to make, not the run loop's.
 */
enum vm86_bios_stop vm86_bios_run(struct vm86_bios_machine *,
                                  const struct vm86_bios_plan *);

/* The screen as of the moment the program last stopped. */
const struct vm86_bios_screen *vm86_bios_screen(
    const struct vm86_bios_machine *);

/* Guest memory, for the cases that grade a buffer rather than a screen. */
const uint8_t *vm86_bios_memory(const struct vm86_bios_machine *);
uint32_t       vm86_bios_memory_size(void);

/* The processor, for the cases that grade it -- the stack pointer after a
 * retried blocking read, above all. */
const struct vm86_cpu *vm86_bios_cpu(const struct vm86_bios_machine *);

/* One cell of a recorded screen. */
struct vm86_bios_cell vm86_bios_cell_at(const struct vm86_bios_screen *,
                                        uint16_t cell);

#endif /* VM86_BIOS_REPLAY_H */
