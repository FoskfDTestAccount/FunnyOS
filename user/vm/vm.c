/*
 * The 8086 interpreter, running inside FunnyOS as an ordinary process.
 *
 * ---------------------------------------------------------------------
 * Why this file exists
 *
 * Until now the interpreter was a library that passed its tests in a host
 * process. Nothing in the kernel and nothing in a user program linked
 * against it, so "it can execute a binary" was true only of the host test
 * binary -- FunnyOS itself had never executed a single guest instruction.
 *
 * DESIGN.md's decision D4 says where the interpreter belongs: in Ring 3,
 * as an unprivileged process. This is that decision, running.
 *
 * ---------------------------------------------------------------------
 * What is deliberately missing
 *
 * No shell command, and no file loading.
 *
 * The first used to be a limitation of the process model -- `g_current`
 * was a single global and process_run() was not reentrant, so a shell
 * could not start a process from inside a process. That was lifted in W1
 * of M5 and this paragraph outlived it, still explaining a wall that is
 * no longer there. What is still missing is the command in funnycom's
 * table that would call into here.
 *
 * The second is unchanged: the guest programs are the byte arrays below.
 * SYS_OPEN and SYS_READ exist, but turning a file into a loaded .COM is
 * W3 of M5.
 *
 * ---------------------------------------------------------------------
 * Where the guest's screen goes
 *
 * This process takes the console's screen and paints the guest's text page
 * onto it, through the kernel -- see screen.h on the kernel side and
 * SYS_SCREEN_* in the system call header. The alternative was printing the
 * page out as eighty characters and a newline, twenty-five times, into the
 * console's scrolling log, which is what this file did until W2. It put
 * the text on the screen in the sense that a printout is on a screen.
 *
 * The page is handed over in guest video memory's own layout, straight out
 * of the guest's RAM at 0xB8000. Nothing is repacked on the way, which is
 * what makes "what the screen shows is what the guest's memory holds" a
 * property of two lines of code rather than of a drawing routine that
 * could read memory and then draw something else.
 *
 * That is true while the pointer is only ever drawn from, and it is worth
 * saying what it rests on: the kernel draws the page out of this process's
 * memory during the call, and this process is alive and not running for
 * the length of it. A screen that keeps pages around -- one with tabs, so
 * that a page can be switched away from and back -- has to hold its own
 * copy instead, and then the property is weaker and needs its own
 * argument. See docs/tasks/M4-G-mouse-tabs.md, section two, G3.
 *
 * ---------------------------------------------------------------------
 * Where the guest bytes come from
 *
 * Each array was produced by `nasm -f bin` from the assembly quoted above
 * it. The entry convention is the one frozen in
 * docs/tasks/M3-I-corpus.md section 2: a flat binary loaded at linear
 * 0x100, entered at CS:IP 0:0x100, with DS, ES and SS zero, SP 0xFFFE and
 * FLAGS 0x0002. Task I is building a corpus to that same convention, so
 * these two can be replaced by it without changing anything else here.
 *
 * ---------------------------------------------------------------------
 * On assertions
 *
 * Every case is compared against the terminal state the Intel manual says
 * the instructions produce, and a mismatch is printed. The register dump
 * below it is for a human reading a failure, not for the test: a test
 * that consists of somebody looking at a register dump is not a test.
 */
#include <vm/vm.h>
#include <vm/corpus.h>

#include <libu/libu.h>

#include <vm86/cpu.h>
#include <vm86/display.h>
#include <vm86/dos.h>
#include <vm86/firmware.h>
#include <vm86/host.h>
#include <vm86/int21.h>
#include <vm86/fat.h>
#include <vm86/mem.h>
#include <vm86/ops.h>

/*
 * The four services.
 *
 * Their implementations are already part of this image -- the top-level
 * Makefile globs dos/bios and dos/intr into the user build the same way
 * dos/Makefile globs them into the host one -- so all this file needs from
 * them is their headers. Those are reached by relative path because the
 * user build's include path reaches dos/include and not dos/ itself.
 */
#include "../../dos/bios/bios10.h"
#include "../../dos/bios/bios13.h"
#include "../../dos/bios/bios16.h"
#include "../../dos/bios/bios1a.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* The machine this version emulates                                   */
/* ------------------------------------------------------------------ */

/*
 * How much guest memory the emulated machine is given.
 *
 * One megabyte is the whole address space of the chip being emulated: the
 * 8086 has twenty address lines and stops at 0x100000, which is the
 * constant `VM86_ADDR_TOP` names. It is also the floor the corpus
 * convention asks for, so samples written to that convention will fit
 * here unchanged.
 *
 * Sixteen megabytes is the whole address space this machine models: the
 * megabyte the chip itself can address, plus the extended pool the
 * design provides for. It is a named constant because it is meant to be
 * read, not because it is still expected to move.
 *
 * It must not live in .bss. The build marks .bss loadable, so an array
 * there is emitted into the flat image and from there into the kernel as
 * a C array at six characters a byte -- these sixteen megabytes would
 * become ninety-six megabytes of generated source. The .guestram section
 * in user/link.ld is NOLOAD, which is the whole point: it counts toward
 * the size the loader maps and contributes nothing at all to the image.
 * The loader then zeroes it, which is what a guest expects to find.
 *
 * The attribute is the price of that, and forgetting it is visible
 * rather than quiet: the generated source explodes and the build is
 * obviously wrong, at build time, before anything runs.
 */
#define GUEST_RAM_BYTES (16u * 1024u * 1024u)

/*
 * Where a guest program is loaded and where its stack starts.
 *
 * A .COM gets a segment of its own -- CS, DS, ES and SS all pointing at
 * it -- and this is the convention dos/corpus/bios/replay.h froze for the
 * host suite, so the same bytes run in both places with nothing kept in
 * step by hand.
 *
 * The segment is not decoration. Loading at linear 0x100 with CS = 0 puts
 * the program inside the interrupt vector table at 0x0000-0x03FF: its
 * first 768 bytes become vector entries 64 and up, which works only until
 * a program hooks a vector above 0x60 -- where TSRs conventionally live --
 * and finds its own handler buried under its own code.
 */
#define GUEST_SEGMENT      0x1000u
#define GUEST_LOAD_ADDRESS 0x0100u
#define GUEST_STACK_TOP    0xFFFEu

/*
 * How many guest instructions a case may execute before it is called a
 * runaway.
 *
 * This has to exist. A guest program with a bug in it, or an interpreter
 * bug, would otherwise spin forever and hang the test instead of failing
 * it. It also has to be reported as its own outcome rather than as a
 * fault: a stuck guest reported as an exception sends the reader looking
 * for an exception that is not there.
 */
#define GUEST_INSN_LIMIT 100000u

/*
 * Ring 3 has no heap, so the guest's memory is static storage -- but in
 * the section that is mapped and not loaded. See the note above, and the
 * one on .guestram in user/link.ld.
 */
/*
 * The emulator's "no cursor" and the console's have to be one value.
 *
 * bios10_cursor_cell answers in the emulator's vocabulary and the answer
 * is handed to the kernel, which reads it in its own -- so a disagreement
 * here is a guest whose cursor is drawn at cell 65535 rather than not
 * drawn at all, or the reverse. The emulator cannot see the kernel's
 * headers and should not, so this is where the two are introduced, and the
 * compiler is asked whether they still match.
 */
_Static_assert(VM86_DISPLAY_NO_CURSOR == SCREEN_NO_CURSOR,
               "the emulator and the system call interface disagree about "
               "what 'no cursor' is");

static uint8_t g_guest_ram[GUEST_RAM_BYTES]
    __attribute__((section(".guestram")));

/* ------------------------------------------------------------------ */
/* What a case expects                                                 */
/* ------------------------------------------------------------------ */

/*
 * How many values a terminal state holds: eight general registers, four
 * segment registers, IP and FLAGS. The names, the expected array in the
 * case table and the gatherer below are three views of one list, and this
 * constant is what keeps them the same length.
 */
#define STATE_COUNT 14

/*
 * One byte of guest memory a case must leave behind, by linear address.
 * Addresses rather than offsets because the interesting ones are far
 * apart: 0x0200 for what the arithmetic stored, 0x0400 for the copy.
 */
struct expect_byte {
    uint32_t address;
    uint8_t  value;
};

struct guest_case {
    const char    *name;
    const uint8_t *image;
    uint32_t       size;

    /*
     * How it should end. `faults` is a separate flag rather than a
     * sentinel vector, because zero is a real vector -- the divide error
     * -- and "no fault" must not be spelled the same way.
     */
    bool           faults;
    uint8_t        vector;

    /*
     * The terminal state, in the order `STATE_NAMES` lists it.
     */
    uint16_t       state[STATE_COUNT];

    const struct expect_byte *memory;
    uint32_t                  memory_count;
};

/* ------------------------------------------------------------------ */
/* The guest programs                                                  */
/* ------------------------------------------------------------------ */

/*
 * Case one: a program that runs to completion.
 *
 *     org 0x100
 *
 *     mov     ax, 0x1234
 *     mov     bx, 0x00FF
 *     add     ax, bx          ; AX = 0x1333, CF = 0
 *     adc     ax, 0           ; the carry chain, with CF from the ADD
 *
 *     mov     cx, 8
 *     mov     dx, 1
 * .next:
 *     shl     dx, 1           ; 1 -> 0x0100 in eight shifts, CF = 0
 *     dec     cx
 *     jnz     .next           ; the last DEC leaves ZF=1 and PF=1
 *
 *     mov     [0x0200], ax
 *     mov     [0x0202], dx
 *
 *     mov     byte [0x0300], 0xDE
 *     mov     byte [0x0301], 0xAD
 *     mov     byte [0x0302], 0xBE
 *     mov     byte [0x0303], 0xEF
 *     mov     si, 0x0300
 *     mov     di, 0x0400
 *     mov     cx, 4
 *     rep     movsb           ; four bytes, and no flag is touched
 *
 *     hlt
 *
 * Chosen so that the terminal state is not reproducible by accident: the
 * sum exercises the ALU and a carry chain, the loop exercises a shift and
 * a conditional jump, the stores put results where the test can read them
 * back, and the copy exercises a REP prefix and the segment rules. It is
 * sixteen opcodes drawn from four of the five group files, in forty-one
 * instructions.
 */
static const uint8_t alu_loop_image[] = {
    0xB8, 0x34, 0x12,        /* mov ax, 0x1234          */
    0xBB, 0xFF, 0x00,        /* mov bx, 0x00FF          */
    0x01, 0xD8,              /* add ax, bx              */
    0x83, 0xD0, 0x00,        /* adc ax, 0               */
    0xB9, 0x08, 0x00,        /* mov cx, 8               */
    0xBA, 0x01, 0x00,        /* mov dx, 1               */
    0xD1, 0xE2,              /* .next: shl dx, 1        */
    0x49,                    /* dec cx                  */
    0x75, 0xFB,              /* jnz .next               */
    0xA3, 0x00, 0x02,        /* mov [0x0200], ax        */
    0x89, 0x16, 0x02, 0x02,  /* mov [0x0202], dx        */
    0xC6, 0x06, 0x00, 0x03, 0xDE,   /* mov byte [0x0300], 0xDE */
    0xC6, 0x06, 0x01, 0x03, 0xAD,   /* mov byte [0x0301], 0xAD */
    0xC6, 0x06, 0x02, 0x03, 0xBE,   /* mov byte [0x0302], 0xBE */
    0xC6, 0x06, 0x03, 0x03, 0xEF,   /* mov byte [0x0303], 0xEF */
    0xBE, 0x00, 0x03,        /* mov si, 0x0300          */
    0xBF, 0x00, 0x04,        /* mov di, 0x0400          */
    0xB9, 0x04, 0x00,        /* mov cx, 4               */
    0xF3, 0xA4,              /* rep movsb               */
    0xF4,                    /* hlt                     */
};

/*
 * What case one must leave behind.
 *
 * Read off the manual rather than off the implementation, and written out
 * one instruction at a time in the commentary above: AX is 0x1234+0x00FF
 * with no carry into the ADC, DX is 1 doubled eight times, and the loop's
 * last DEC is the last instruction to touch a flag, so FLAGS is its ZF
 * and PF plus the bits the register always reads as set (0xF002).
 */
static const struct expect_byte alu_loop_memory[] = {
    { 0x0200, 0x33 },   /* AX, low byte  */
    { 0x0201, 0x13 },   /* AX, high byte */
    { 0x0202, 0x00 },   /* DX, low byte  */
    { 0x0203, 0x01 },   /* DX, high byte */
    { 0x0400, 0xDE },   /* the four bytes REP MOVSB moved */
    { 0x0401, 0xAD },
    { 0x0402, 0xBE },
    { 0x0403, 0xEF },
};

/*
 * Case two: a program that must fault.
 *
 *     org 0x100
 *
 *     mov     ax, 0x0BAD
 *     mov     bx, 0x0F00
 *     mov     cx, 0x1234
 *     db      0xFF, 0x38      ; FF /7 -- mod=00, reg=111, rm=000
 *     hlt                     ; never reached
 *
 * The eight-way group at FF has no seventh sub-operation on this
 * processor. The point of this case is that it is refused: an
 * interpreter that fell through to `push` or `jmp` would run here happily
 * and leave a state no assertion would ever notice was wrong, because
 * nothing else in the run depends on it.
 *
 * The three MOVs are there so that a fault which clobbered registers
 * would be visible. MOV touches no flag, so the expected FLAGS is the
 * reset value, 0xF002.
 */
static const uint8_t bad_opcode_image[] = {
    0xB8, 0xAD, 0x0B,        /* mov ax, 0x0BAD          */
    0xBB, 0x00, 0x0F,        /* mov bx, 0x0F00          */
    0xB9, 0x34, 0x12,        /* mov cx, 0x1234          */
    0xFF, 0x38,              /* FF /7, not an instruction */
    0xF4,                    /* hlt, never reached      */
};

/* ------------------------------------------------------------------ */
/* The case table                                                      */
/* ------------------------------------------------------------------ */

/*
 * The names, in the order `state[]` holds the values. Kept next to
 * `gather_state()` below, which fills an array in the same order -- the
 * two are only correct together, so they are only readable together.
 */
static const char *const STATE_NAMES[STATE_COUNT] = {
    "AX", "BX", "CX", "DX", "SI", "DI", "BP", "SP",
    "CS", "DS", "ES", "SS", "IP", "FLAGS",
};

static const struct guest_case g_cases[] = {
    {
        .name   = "alu-loop",
        .image  = alu_loop_image,
        .size   = (uint32_t)sizeof(alu_loop_image),
        .faults = false,
        .state  = {
            0x1333, 0x00FF, 0x0000, 0x0100,   /* AX BX CX DX  */
            0x0304, 0x0404, 0x0000, 0xFFFE,   /* SI DI BP SP  */
            0x0000, 0x0000, 0x0000, 0x0000,   /* CS DS ES SS  */
            0x013D, 0xF046,                   /* IP FLAGS     */
        },
        .memory       = alu_loop_memory,
        .memory_count = (uint32_t)(sizeof(alu_loop_memory) /
                                   sizeof(alu_loop_memory[0])),
    },
    {
        .name   = "bad-opcode",
        .image  = bad_opcode_image,
        .size   = (uint32_t)sizeof(bad_opcode_image),
        .faults = true,
        .vector = VM86_VECTOR_INVALID_OPCODE,

        /*
         * IP has moved past the two bytes of the encoding, because the
         * ModRM byte is fetched before the sub-operation is looked at --
         * which is also what makes a displacement get consumed on the
         * way to a fault.
         */
        .state  = {
            0x0BAD, 0x0F00, 0x1234, 0x0000,   /* AX BX CX DX  */
            0x0000, 0x0000, 0x0000, 0xFFFE,   /* SI DI BP SP  */
            0x0000, 0x0000, 0x0000, 0x0000,   /* CS DS ES SS  */
            0x010B, 0xF002,                   /* IP FLAGS     */
        },
        .memory       = NULL,
        .memory_count = 0,
    },
};

#define CASE_COUNT (sizeof(g_cases) / sizeof(g_cases[0]))

/* ------------------------------------------------------------------ */
/* The machine: firmware, four services, and a screen                  */
/* ------------------------------------------------------------------ */

/*
 * M3 ran guest bytes on a bare processor: step until the machine stops,
 * and the terminal state is the whole answer. M4's acceptance sentence is
 * not about a processor, it is about a computer -- a .COM printing through
 * the BIOS and appearing on a screen -- and that needs what a PC has above
 * the chip: an interrupt vector table, four services with devices behind
 * them, and something that draws.
 *
 * All of it is in dos/ and already compiled into this image. What is here
 * is the part that belongs to this program. The four devices are static
 * storage because Ring 3 has no heap, and the display is this console.
 */

/*
 * How long a slice is, and how many of them before the guest is called a
 * runaway.
 *
 * A slice is the host's unit of attention: the run loop returns when it is
 * used up, and the host refreshes the screen, advances the clock and gets
 * out of the way. Too long and the machine stops looking like a machine;
 * too short and the interpreter spends its time in the loop rather than in
 * the guest. A quarter of a thousand instructions is small enough that a
 * program that prints is still drawn while it prints, and large enough
 * that the per-slice work is not the run.
 *
 * The count is the backstop, for the same reason GUEST_INSN_LIMIT is: a
 * guest that never finishes has to be reported as one that never finished.
 */
#define VM_SLICE_STEPS 250u

/* How often the host asks what time it is, in turns of the slice loop.
 * A power of two so the test is a mask. See the note where it is used. */
#define VM_CLOCK_EVERY 512u

/*
 * How long the host will drive a machine that is not getting anywhere,
 * in milliseconds of real time, and a turn count as a second backstop.
 *
 * Milliseconds, and not turns, because the thing a guest can be waiting
 * for is real time: timer.asm waits for five ticks of a clock that is
 * 275 ms of wall time whatever the emulator is doing. A budget in turns
 * would be a budget that means something different on every machine --
 * two million turns took 270 ms on the machine this was written on, which
 * is *less* than the 275 ms the guest was waiting for, so the run failed
 * for being too fast. Five seconds leaves seventeen times the room needed
 * and still stops a guest that is genuinely stuck.
 */
#define VM_RUN_MAX_MS      5000u
#define VM_RUN_MAX_TURNS   200000000u

/* The largest page this machine has: eighty columns of twenty-five rows.
 * How many cells the host must READ is not this -- it is `columns * 25`,
 * and a 40-column mode is a legal mode whose page is half the width. */
#define VM_PAGE_CELLS (VM86_TEXT_COLUMNS * VM86_TEXT_ROWS)

/*
 * The display, which is the console's screen, borrowed.
 *
 * Not driven by the services. A program that writes 0xB8000 directly never
 * passes through one -- direct.asm does exactly that -- so the host reads
 * the page on its own schedule, which is what display.h's header argues
 * for and the only arrangement in which a direct write is visible at all.
 *
 * `frame` is the last page handed to the kernel, kept so that a page which
 * has not changed is not presented again: each present is a system call
 * and a rectangle of the framebuffer, and a guest that is waiting for the
 * clock presents nothing for hundreds of turns. The cursor is part of what
 * is compared, because the guest's cursor is not in the page -- it is a
 * register the page cannot show -- and a run where only the cursor moved
 * is a run where the screen still has to be told.
 */
struct vm_screen {
    const struct bios10_state *video;

    uint8_t  frame[VM_PAGE_CELLS * 2u];
    uint16_t cursor;
    bool     drawn;      /* `frame` holds a page                             */

    bool     holding;    /* this process has the console's screen            */
};

static struct bios10_state g_video;
static struct bios16_state g_keyboard;
static struct fat_volume g_files;
static bool g_mounted, g_raw_active;
static unsigned g_resource_hold;
static uint8_t g_disk[720u*512u] __attribute__((section(".guestram"), aligned(4096)));
static uint8_t g_com[65280] __attribute__((section(".guestram"), aligned(4096)));
static struct bios1a_state g_clock;
static struct int21_state  g_dos21;
static struct vm_screen    g_screen;
static struct vm86_display g_display;

/*
 * Pages handed to the kernel and pages it refused, over the whole run.
 *
 * Counted rather than only reported, because the interesting failure is
 * the quiet one: a run in which nothing was ever presented passes every
 * assertion about the page the guest left in its own memory. See
 * check_screen.
 */
static unsigned g_presents;
static unsigned g_refused;

/*
 * The machine itself, at file scope and not on run_corpus's stack.
 *
 * The processor holds a pointer to the memory, and the cases that read the
 * guest's screen after a run go through that pointer -- so a processor
 * that outlived its own stack frame would be a processor pointing at
 * whatever the next call left there. That is not hypothetical: it was this
 * file's shape until the screen assertions started reading guest memory
 * directly, at which point it was a page fault in Ring 3 reading an
 * address 35 KiB below the guest's RAM.
 *
 * One machine at a time, which is what the rest of this file assumes
 * anyway -- the four services each have one device too.
 */
static struct vm86_mem g_mem;
static struct vm86_cpu g_cpu;

/* How much time the host handed the clock over the whole run. Asserted
 * rather than merely printed: it is the host's own account of the time
 * that really passed, taken from the kernel's millisecond counter, and it
 * is not derived from anything the clock said -- which is what makes "five
 * ticks took 275 ms" a statement about the rate rather than a tautology. */
static uint32_t g_ms_advanced;

/*
 * Ticks the clock has produced and the host has not raised yet.
 *
 * A vector raised while it is already pending is one interrupt and not
 * two -- that is how the 8259 behaves and it is written into host.h -- so
 * the host cannot hand over a batch of them at once. Holding the count
 * here and raising one per turn as the processor becomes free is what
 * keeps the guest's count from falling behind the clock.
 */
static uint32_t g_ticks_owed;
static uint32_t g_ticks_raised;   /* diagnostic */
static uint32_t g_clock_reads;    /* diagnostic */

static bool frame_same(const uint8_t *a, const uint8_t *b, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        if (a[i] != b[i])
            return false;

    return true;
}

/*
 * Hand the guest's page to the kernel, if anything changed.
 *
 * The page pointer comes from bios10_active_page and points into the
 * guest's own RAM, so what the kernel is given is literally the memory a
 * DOS program wrote -- not a transcription of it. There is no repacking
 * here and no attribute lookup: the kernel reads the same two bytes per
 * cell that the guest does.
 *
 * A backend that draws nothing is what this replaced. Until W2 this
 * function formatted each of the twenty-five rows into a string and
 * printed it into the console's log, and the thing that made that wrong is
 * not that it was ugly: the log scrolls, so the guest's screen went past
 * like a printout, and the second frame was appended under the first.
 */
static void screen_present(void *ctx, const uint8_t *cells, uint16_t cursor)
{
    struct vm_screen *screen = ctx;
    unsigned          columns;
    size_t            bytes;

    if (!cells || !screen->video)
        return;

    columns = screen->video->columns;
    if (columns == 0 || columns > VM86_TEXT_COLUMNS)
        return;

    bytes = (size_t)columns * BIOS10_ROWS * 2u;

    if (screen->drawn && screen->cursor == cursor &&
        frame_same(screen->frame, cells, (uint32_t)bytes))
        return;

    for (size_t i = 0; i < bytes; i++)
        screen->frame[i] = cells[i];

    screen->drawn  = true;
    screen->cursor = cursor;

    /*
     * Nothing is counted when the screen is not this process's. A run with
     * no console still has to work -- the guest's page is asserted out of
     * guest memory whatever happens to the display -- and a count of
     * presents that never happened would make that look like success.
     */
    if (!screen->holding)
        return;

    g_presents++;

    if (u_screen_present(cells, columns, cursor) != 0)
        g_refused++;
}

static void present(const struct vm86_cpu *cpu)
{
    vm86_display_present(&g_display,
                         bios10_active_page(cpu, &g_video),
                         bios10_cursor_cell(cpu, &g_video));
}

/*
 * Power the machine on and load one program into it.
 *
 * The firmware goes in before the program, which is the order the real
 * machine does it in and which used to be load-bearing: the table is built
 * at 0x0000-0x03FF, and under the old convention a program went to linear
 * 0x100 with CS = 0 and landed inside it. The program has a segment of its
 * own now -- see GUEST_SEGMENT -- so the two cannot collide, and the order
 * stays because it is the honest one.
 *
 * The disk is registered with a NULL context, which is what bios13.h calls
 * a machine with no disk: every function answers with the status that says
 * so rather than being silently omitted.
 */
/*
 * Power the machine on: guest RAM attached and cleared, no program in it,
 * the firmware installed and the six services registered with devices
 * behind them.
 *
 * Split out of machine_build because there are now two ways to put a
 * program into a machine -- the M3/M4 convention these samples use, and
 * the way DOS loads a .COM -- and only one way to build the machine.
 *
 * The services' own state has to be stood up here; the data area the
 * firmware wrote is not the same thing. The mode, the cursor and the page
 * are these structs' until a service changes them.
 */
static void power_on(struct vm86_mem *mem, struct vm86_cpu *cpu)
{
    vm86_mem_attach(mem, g_guest_ram, (uint32_t)GUEST_RAM_BYTES);
    vm86_mem_clear(mem);
    vm86_reset(cpu, mem);

    vm86_clear_services();
    vm86_install_firmware(cpu);

    bios10_reset(&g_video);
    bios16_reset(&g_keyboard);
    bios1a_reset(&g_clock);

    /*
     * The DOS dispatcher comes up with no program behind it, and
     * machine_build_dos gives it the real PSP afterwards -- the default
     * transfer address lives inside the PSP, so it cannot be set until
     * the loader has chosen a segment. A machine built by
     * machine_build, above, never loads a program at all and keeps the
     * empty one.
     */
    int21_reset(&g_dos21, &g_video, 0u);

    /*
     * `drawn` is cleared so that the first present of a case reaches the
     * kernel whatever the previous case left in `frame`, and `holding` is
     * deliberately not: the console, once taken, is this process's across
     * every case in the run.
     */
    g_screen.video   = &g_video;
    g_screen.drawn   = false;
    g_screen.cursor  = VM86_DISPLAY_NO_CURSOR;
    g_display.present = screen_present;
    g_display.ctx     = &g_screen;

    g_ticks_owed  = 0;
    g_ms_advanced = 0;
    g_ticks_raised = 0;
    g_clock_reads = 0;

    vm86_register_service(VM86_INT_VIDEO,         bios10_service, &g_video);
    vm86_register_service(VM86_INT_KEYBOARD_BIOS, bios16_service, &g_keyboard);
    vm86_register_service(VM86_INT_KEYBOARD,      bios16_irq,     &g_keyboard);
    vm86_register_service(VM86_INT_TIME,          bios1a_service, &g_clock);
    vm86_register_service(VM86_INT_TIMER,         bios1a_irq,     &g_clock);
    vm86_register_service(VM86_INT_DISK,          bios13_service, NULL);
    vm86_register_service(VM86_INT_CTRL_BREAK, int21_break_service, NULL);
    vm86_register_service(VM86_INT_DOS,           int21_service, &g_dos21);
    vm86_register_service(VM86_INT_TERMINATE,     int21_terminate_service,
                          NULL);
}

static void machine_build(struct vm86_mem *mem, struct vm86_cpu *cpu,
                          const uint8_t *image, uint32_t size)
{
    power_on(mem, cpu);

    for (uint32_t i = 0; i < size; i++)
        vm86_mem_write8(mem,
                        ((uint32_t)GUEST_SEGMENT << 4) +
                            GUEST_LOAD_ADDRESS + i,
                        image[i]);

    vm86_set_seg(cpu, VM86_CS, GUEST_SEGMENT);
    vm86_set_seg(cpu, VM86_DS, GUEST_SEGMENT);
    vm86_set_seg(cpu, VM86_ES, GUEST_SEGMENT);
    vm86_set_seg(cpu, VM86_SS, GUEST_SEGMENT);
    vm86_flush_segments(cpu);

    cpu->ip = GUEST_LOAD_ADDRESS;
    cpu->sp = GUEST_STACK_TOP;
}

/* ------------------------------------------------------------------ */
/* Starting a program the way DOS starts one                           */
/* ------------------------------------------------------------------ */

/*
 * Where a DOS program's environment block goes.
 *
 * Just below the program's own 64 KiB block, which is free memory on this
 * machine -- the vector table is at the bottom of segment zero and the
 * stubs are at 0xF000 -- and outside the block, which the loader insists
 * on, because the program is told it owns the whole of that.
 */
#define GUEST_ENVIRONMENT_SEGMENT 0x0F00u

/* What a program is told it is called. The drive letter matches the one
 * the shell's prompt uses, which is what this filesystem is called -- and
 * the same letter the DOS layer answers INT 21h AH=19h with, so a program
 * that asks which drive it is on and one that reads its own path back out
 * of the environment agree. */
#define GUEST_PROGRAM_PATH "F:\\PSP.COM"
#define GUEST_INT21_PATH   "F:\\INT21.COM"

/*
 * The environment a started program inherits.
 *
 * COMSPEC names a command interpreter and there is not one on a disk yet:
 * the file system is W5 and the shell is still compiled into the kernel
 * rather than being a file. So the value names what a program expects to
 * find and nothing is there yet, which is worth saying because a program
 * that goes looking will fail in a way that looks like a bug in W5's
 * absence rather than in this line.
 */
static const char *const g_dos_environment[] = {
    "COMSPEC=F:\\COMMAND.COM",
    "PATH=F:\\",
    "PROMPT=$P$G",
};

#define GUEST_ENVIRONMENT_COUNT \
    (sizeof g_dos_environment / sizeof g_dos_environment[0])

/*
 * Power the machine on and load a program the way DOS loads one.
 *
 * The same machine and the same services as machine_build above; what
 * differs is the 256 bytes in front of the program, the zero word on the
 * stack, and the interrupt flag being set. See dos/include/vm86/dos.h for
 * why that last one is not a detail.
 */
static enum vm86_dos_load_result
machine_build_dos(struct vm86_mem *mem, struct vm86_cpu *cpu,
                  const uint8_t *image, uint32_t size, const char *tail,
                  const char *path, struct vm86_dos_psp *out)
{
    struct vm86_dos_start start = {
        .segment     = GUEST_SEGMENT,
        .environment = GUEST_ENVIRONMENT_SEGMENT,
        .parent      = GUEST_SEGMENT,
        .path        = path,
        .tail        = tail,
        .vars        = g_dos_environment,
        .var_count   = (uint32_t)GUEST_ENVIRONMENT_COUNT,
    };

    power_on(mem, cpu);

    enum vm86_dos_load_result result =
        vm86_dos_load(cpu, image, size, &start, out);

    /*
     * The dispatcher's default transfer address is inside the PSP, so it
     * can only be set once the loader has said which segment that is.
     * Only on success: a refused load left no program for a transfer
     * address to belong to.
     */
    if (result == VM86_DOS_LOADED) {
        int21_reset(&g_dos21, &g_video, out->segment);
        if(g_mounted) g_dos21.files=&g_files;
    }

    return result;
}

/* ------------------------------------------------------------------ */
/* The conflict report                                                 */
/* ------------------------------------------------------------------ */

/*
 * Two opcode groups claiming the same byte is the characteristic failure
 * of splitting the opcode map between five files, and without a reporter
 * it is silent -- the last table merged simply wins, and the instruction
 * that was taken produces a wrong answer only for the encoding nobody
 * looked at. Naming the opcode and both claimants turns that into a
 * one-line answer.
 */
static void report_conflict(void *ctx, uint8_t opcode,
                            const char *first, const char *second)
{
    (void)ctx;

    uprintf("      CONFLICT  opcode %02X is claimed by both %s and %s\n",
            opcode, first, second);
}

/* ------------------------------------------------------------------ */
/* Running a case                                                      */
/* ------------------------------------------------------------------ */

static void load_guest(struct vm86_cpu *cpu, const uint8_t *image,
                       uint32_t size)
{
    for (uint32_t i = 0; i < size; i++)
        vm86_mem_write8(cpu->mem, GUEST_LOAD_ADDRESS + i, image[i]);
}

/*
 * The machine's state as the flat array the expectations are written in.
 *
 * The order here and the order of STATE_NAMES above are the same order,
 * and `state[]` in the case table is a third copy of it. They are three
 * views of one list and are only meaningful together.
 */
static void gather_state(const struct vm86_cpu *cpu,
                         uint16_t out[STATE_COUNT])
{
    out[0]  = cpu->ax;   out[1]  = cpu->bx;   out[2]  = cpu->cx;
    out[3]  = cpu->dx;   out[4]  = cpu->si;   out[5]  = cpu->di;
    out[6]  = cpu->bp;   out[7]  = cpu->sp;   out[8]  = cpu->cs;
    out[9]  = cpu->ds;   out[10] = cpu->es;   out[11] = cpu->ss;
    out[12] = cpu->ip;   out[13] = cpu->flags;
}

static unsigned flag_bit(uint16_t flags, uint16_t bit)
{
    return (flags & bit) ? 1u : 0u;
}

/*
 * The terminal state, and every flag by name.
 *
 * The named line is the point: 0xF046 tells a reader nothing without a
 * manual open next to them, and what a failing run needs is to be read.
 */
static void print_state(const struct vm86_cpu *cpu)
{
    uprintf("  AX=%04X BX=%04X CX=%04X DX=%04X\n",
            cpu->ax, cpu->bx, cpu->cx, cpu->dx);
    uprintf("  SI=%04X DI=%04X BP=%04X SP=%04X\n",
            cpu->si, cpu->di, cpu->bp, cpu->sp);
    uprintf("  CS=%04X DS=%04X ES=%04X SS=%04X\n",
            cpu->cs, cpu->ds, cpu->es, cpu->ss);
    uprintf("  IP=%04X FLAGS=%04X\n", cpu->ip, cpu->flags);

    uprintf("  flags  CF=%u PF=%u AF=%u ZF=%u SF=%u TF=%u IF=%u "
            "DF=%u OF=%u\n",
            flag_bit(cpu->flags, VM86_CF), flag_bit(cpu->flags, VM86_PF),
            flag_bit(cpu->flags, VM86_AF), flag_bit(cpu->flags, VM86_ZF),
            flag_bit(cpu->flags, VM86_SF), flag_bit(cpu->flags, VM86_TF),
            flag_bit(cpu->flags, VM86_IF), flag_bit(cpu->flags, VM86_DF),
            flag_bit(cpu->flags, VM86_OF));
}

static const char *vector_name(uint8_t vector)
{
    switch (vector) {
    case VM86_VECTOR_DIVIDE_ERROR:   return "divide error";
    case VM86_VECTOR_BOUND:          return "bound range exceeded";
    case VM86_VECTOR_INVALID_OPCODE: return "invalid opcode";
    default:                         return "unrecognised";
    }
}

/*
 * Assertion plumbing.
 *
 * Same shape as dos/tests/harness.c, and deliberately so: the failures
 * have to read alike on both sides of the boundary, because the same
 * person reads both. Nothing stops at the first failure -- one run should
 * tell you everything that is wrong, not the first thing.
 */
static void check_word(unsigned *checks, unsigned *failures,
                       const char *name, uint16_t got, uint16_t want)
{
    (*checks)++;

    if (got == want)
        return;

    uprintf("      FAIL  %-5s expected %04X, got %04X\n",
            name, want, got);
    (*failures)++;
}

static void check_byte(unsigned *checks, unsigned *failures,
                       struct vm86_mem *mem, uint32_t address,
                       uint8_t want)
{
    uint8_t got = vm86_mem_read8(mem, address);

    (*checks)++;

    if (got == want)
        return;

    uprintf("      FAIL  [%04X] expected %02X, got %02X\n",
            address, want, got);
    (*failures)++;
}

static bool run_case(const struct guest_case *c)
{
    struct vm86_mem mem;
    struct vm86_cpu cpu;
    unsigned        checks   = 0;
    unsigned        failures = 0;

    uprintf("\n  --- guest case \"%s\" ---\n", c->name);

    vm86_mem_attach(&mem, g_guest_ram, (uint32_t)GUEST_RAM_BYTES);
    vm86_mem_clear(&mem);
    vm86_reset(&cpu, &mem);

    load_guest(&cpu, c->image, c->size);

    /*
     * The convention's segment setup. `vm86_reset` has already left FLAGS
     * holding the bits that always read as one and nothing else, which is
     * the 0x0002 the convention asks for once the hardwired bits are
     * masked off -- so there is nothing to set here.
     */
    vm86_set_seg(&cpu, VM86_CS, 0);
    vm86_set_seg(&cpu, VM86_DS, 0);
    vm86_set_seg(&cpu, VM86_ES, 0);
    vm86_set_seg(&cpu, VM86_SS, 0);
    vm86_flush_segments(&cpu);

    cpu.ip = GUEST_LOAD_ADDRESS;
    cpu.sp = GUEST_STACK_TOP;

    uprintf("  image  : %u bytes at %04X:%04X, SP=%04X\n",
            (unsigned)c->size, (unsigned)cpu.cs, (unsigned)cpu.ip,
            (unsigned)cpu.sp);

    enum vm86_result result = VM86_CONTINUE;
    unsigned         steps  = 0;

    /*
     * `while` rather than `for`, and the count goes up before the test, so
     * that the instruction which ended the run is counted as executed.
     * A `for` whose increment is skipped by the `break` reports one fewer
     * instructions than ran, which is a small lie printed next to a
     * register dump that a reader will take as fact.
     */
    while (steps < GUEST_INSN_LIMIT) {
        result = vm86_step(&cpu);
        steps++;

        if (result != VM86_CONTINUE)
            break;
    }

    /*
     * Out of budget with the machine still running. This is neither a
     * halt nor a fault, and saying so is the whole reason the outcome is
     * tested for separately: reporting a stuck guest as an exception
     * would send the reader hunting for one that was never raised.
     */
    if (result == VM86_CONTINUE) {
        uprintf("  result : RUN LIMIT reached, %u instructions, the "
                "guest never stopped\n", steps);
        uprintf("  VM: case %s: FAIL (runaway guest)\n", c->name);
        return false;
    }

    /*
     * The emulator failed, not the guest: the opcode table did not merge,
     * so nothing in this run reached the handler it should have. Reported
     * before anything is judged and failed on the spot, because every
     * check below is about what the guest did and would read a broken
     * machine as a well-behaved one.
     */
    if (result == VM86_INTERNAL_ERROR) {
        uprintf("  result : INTERNAL ERROR, the opcode table did not "
                "merge; no instruction in this run can be trusted\n");
        uprintf("  VM: case %s: FAIL (broken emulator)\n", c->name);
        return false;
    }

    if (result == VM86_FAULT) {
        uprintf("  result : fault, vector %u (%s)\n",
                (unsigned)cpu.fault, vector_name(cpu.fault));
    } else {
        uprintf("  result : halted after %u instructions\n", steps);
    }

    print_state(&cpu);

    /* --- How it ended --- */
    checks++;

    if (!c->faults) {
        if (result != VM86_HALT) {
            uprintf("      FAIL  expected the guest to halt\n");
            failures++;
        }
    } else if (result != VM86_FAULT) {
        uprintf("      FAIL  expected an exception, the guest ran on\n");
        failures++;
    } else if (cpu.fault != c->vector) {
        uprintf("      FAIL  expected vector %u, got %u\n",
                (unsigned)c->vector, (unsigned)cpu.fault);
        failures++;
    }

    /* --- What it left behind --- */
    uint16_t got[STATE_COUNT];
    gather_state(&cpu, got);

    for (size_t i = 0; i < STATE_COUNT; i++)
        check_word(&checks, &failures, STATE_NAMES[i], got[i], c->state[i]);

    for (uint32_t i = 0; i < c->memory_count; i++)
        check_byte(&checks, &failures, &mem, c->memory[i].address,
                   c->memory[i].value);

    uprintf("  checks : %u ok, %u failed\n", checks - failures, failures);
    uprintf("  VM: case %s: %s\n", c->name, failures ? "FAIL" : "PASS");

    return failures == 0;
}

/* ------------------------------------------------------------------ */
/* Running a corpus program                                            */
/* ------------------------------------------------------------------ */

/*
 * Drive a machine that has already been built and has a program in it,
 * until the program stops or the budget runs out: in slices, with the
 * display refreshed between them, because the host has a screen to draw
 * and a clock to advance and a guest never asks it to do either.
 *
 * Split out of the loaders below because there are two ways to put a
 * program into a machine and one way to drive it. Loading and driving are
 * separate for the reason the loader's header gives: a test wants to look
 * at a loaded machine before it runs, and whether a program may run at all
 * is not the loader's decision.
 *
 * W6 acquires a raw set-1 stream for mounted DOS programs. Byte delivery
 * happens between slices, never by reverse-mapping translated host keys,
 * and never by blocking the interpreter's host thread.

 */
static bool drive(const char *name)
{
    enum vm86_stop  stop = VM86_STOP_STEPS;
    unsigned        slices;
    unsigned long   last_ms = u_uptime_ms();
    bool input_ready = g_resource_hold != 6;

    for (slices = 0; slices < VM_RUN_MAX_TURNS; slices++) {
        uint64_t retired = g_cpu.insn_count;

        /* Never overwrite the emulated 8042's one-byte latch. Prefix,
         * modifier, make and break bytes each traverse INT 09h separately. */
        if(g_raw_active && input_ready && !g_keyboard.controller_full && vm86_next_pending(&g_cpu)<0) {
            int scan=u_kbd_poll();
            if(scan==-10) { uputs("  VM: keyboard stream overflow: FAIL\n"); return false; }
            if(scan>=0) {
                bios16_key_arrived(&g_keyboard,(uint8_t)scan);
                vm86_raise(&g_cpu,VM86_INT_KEYBOARD);
            }
        }
        stop = vm86_run(&g_cpu, VM_SLICE_STEPS);

        /*
         * Draw only when the guest actually executed something.
         *
         * A halted machine cannot have written video memory, and a guest
         * waiting for a tick is halted for nearly all of the run -- so
         * comparing a 4000-byte frame on every one of those iterations is
         * the difference between a machine that keeps time and one that
         * spends its time looking at a screen that did not change. The
         * count the core keeps is what makes this exact rather than a
         * guess about which stop means "it ran".
         */
        if (g_cpu.insn_count != retired)
            present(&g_cpu);

        /* The hardware acceptance sample first proves an empty BIOS/DOS
         * queue. Announce readiness only after the guest has actually
         * printed its prompt, not merely after raw acquisition. Queued
         * physical keys stay in the kernel until then, so slow TCG cannot
         * turn a boot-time race into an "empty poll" failure. */
        if(!input_ready) {
            static const char prompt[]="W6 BIOS ready";
            bool visible=true;
            for(unsigned i=0;i<sizeof(prompt)-1;i++)
                if(vm86_mem_read8(g_cpu.mem,VM86_TEXT_BASE+i*2u)!=(uint8_t)prompt[i]) visible=false;
            if(visible) { input_ready=true; uputs("W6 input ready\n"); }
        }

        if (stop == VM86_STOP_FAULT || stop == VM86_STOP_BROKEN ||
            stop == VM86_STOP_EXIT)
            break;

        /*
         * A halt ends the program only with interrupts off -- `cli; hlt`.
         * With IF set the processor is waiting, and the way out is to give
         * it something to wake for, which is the clock below.
         */
        if (stop == VM86_STOP_HALT && !vm86_flag_test(&g_cpu, VM86_IF))
            break;

        /*
         * Out of patience, in real time. Checked here rather than as a
         * turn count because the clock below is what a waiting guest is
         * waiting for -- see VM_RUN_MAX_MS.
         */
        if (g_ms_advanced > VM_RUN_MAX_MS)
            break;

        /*
         * The clock is read every so many turns, not every one.
         *
         * Every reading is a system call, and a guest waiting on a tick is
         * halted for hundreds of thousands of turns -- so at one call per
         * turn the machine spends its time asking what time it is, and the
         * whole run takes seconds of real time. Five hundred turns is a
         * small fraction of a millisecond, far below the 55 ms a tick is
         * worth, so the rate the guest sees is unchanged.
         */
        if ((slices & (VM_CLOCK_EVERY - 1u)) == 0u) {
            unsigned long now = u_uptime_ms();
            uint32_t delta = (uint32_t)(now - last_ms);

            last_ms = now;
            g_ms_advanced += delta;
            g_ticks_owed += bios1a_advance(&g_clock, delta);
            g_clock_reads++;
        }

        /* One raise per turn, and only when nothing is still pending. */
        if (g_ticks_owed > 0u && vm86_next_pending(&g_cpu) < 0) {
            g_ticks_owed--;
            g_ticks_raised++;
            vm86_raise(&g_cpu, VM86_INT_TIMER);
        }
    }

    present(&g_cpu);

    /*
     * A service whose flags never reached the guest, reported and only
     * reported when it happens.
     *
     * The guest cannot tell the two apart: a service that answered "no"
     * with the carry flag and one whose carry flag was dropped both leave
     * the program reading CF. So the machine says which happened, here,
     * because there is nowhere else it can be seen -- and it says it only
     * when something went wrong, so that its absence is the all-clear. See
     * the note on the accessors in host.h, and dos/tests/test_int21.c,
     * which asserts the same two numbers where they can be checked in a
     * millisecond.
     */
    if (vm86_flags_declined() != 0)
        uprintf("  flags  : WARNING -- %u of %u service flag write-backs "
                "found no frame to go into\n",
                (unsigned)vm86_flags_declined(),
                (unsigned)(vm86_flags_declined() +
                           vm86_flags_written_back()));

    if (stop == VM86_STOP_BROKEN) {
        uprintf("  result : INTERNAL ERROR, the opcode table did not "
                "merge\n");
        uprintf("  VM: case %s: FAIL (broken emulator)\n", name);
        return false;
    }

    if (stop == VM86_STOP_FAULT) {
        uprintf("  result : fault, vector %u (%s)\n",
                (unsigned)g_cpu.fault, vector_name(g_cpu.fault));
        uprintf("  VM: case %s: FAIL (the guest faulted)\n", name);
        return false;
    }

    if (stop == VM86_STOP_STEPS) {
        uprintf("  result : RUN LIMIT, %u turns and %u ms of real time, "
                "and the guest never stopped\n",
                slices, (unsigned)g_ms_advanced);
        uprintf("  VM: case %s: FAIL (runaway guest)\n", name);
        return false;
    }

    /*
     * The program ended itself, which is a normal ending and not a
     * failure. The code is printed because it is the whole difference
     * between this and a halt: a caller that did not say it would be
     * saying that AH=4Ch and `cli; hlt` are the same event.
     */
    if (stop == VM86_STOP_EXIT) {
        uprintf("  result : the program ended itself with code %u, after "
                "%u slice(s) and %u ms of real time\n",
                (unsigned)g_cpu.exit_code, slices + 1u,
                (unsigned)g_ms_advanced);
        return true;
    }

    uprintf("  result : halted after %u slice(s), %u ms of real time\n",
            slices + 1u, (unsigned)g_ms_advanced);

    return true;
}

/* One program from dos/corpus/bios, loaded by the convention M3 and M4
 * froze: a flat binary at 0x100 of its own segment, interrupts off. */
static bool run_corpus(const char *name, const uint8_t *image, uint32_t size)
{
    uprintf("\n  --- guest case \"%s\" ---\n", name);

    machine_build(&g_mem, &g_cpu, image, size);

    uprintf("  image  : %u bytes at %04X:%04X, SP=%04X\n",
            (unsigned)size, (unsigned)g_cpu.cs, (unsigned)g_cpu.ip,
            (unsigned)g_cpu.sp);

    return drive(name);
}

/*
 * One program from dos/corpus/dos, loaded the way DOS loads one: a Program
 * Segment Prefix at the segment, the program behind it, and interrupts on.
 *
 * The entry state is printed rather than only asserted, because these are
 * the values a reader comparing this run against docs/dos-refs-dos.md
 * needs in front of them -- and because a loader that refused the program
 * has to say so in words that name the reason.
 */
static bool run_dos_program(const char *name, const uint8_t *image,
                            uint32_t size, const char *tail,
                            const char *path)
{
    struct vm86_dos_psp psp;

    uprintf("\n  --- guest case \"%s\" ---\n", name);

    enum vm86_dos_load_result result =
        machine_build_dos(&g_mem, &g_cpu, image, size, tail, path, &psp);

    if (result != VM86_DOS_LOADED) {
        uprintf("  result : the loader refused it (reason %u)\n",
                (unsigned)result);
        uprintf("  VM: case %s: FAIL (the loader refused it)\n", name);
        return false;
    }

    uprintf("  image  : %u bytes, PSP at %04X, memory top %04X, "
            "environment %04X\n",
            (unsigned)size, (unsigned)psp.segment,
            (unsigned)psp.memory_top, (unsigned)psp.environment);
    uprintf("  entry  : CS=%04X IP=%04X SP=%04X FLAGS=%04X\n",
            (unsigned)g_cpu.cs, (unsigned)g_cpu.ip, (unsigned)g_cpu.sp,
            (unsigned)g_cpu.flags);

    return drive(name);
}

/*
 * The whole page the guest left, not just the cells the message is on.
 *
 * Two thousand cells in an eighty-column mode, each checked against what
 * the sample's own header says it leaves there: the message where the
 * message went, and the fill everywhere else. Reporting stops at the first
 * difference, because a wrong screen of two thousand cells should not
 * produce two thousand lines.
 *
 * This reads the page out of guest memory rather than out of the copy the
 * backend kept, so that a backend which recorded one thing and drew
 * another is not judged against its own record.
 *
 * The earlier version of this looked at the twenty-two cells of text and
 * nothing else. That is a whole-screen claim tested by looking at one line
 * of it: a machine whose mode set cleared the first row and left the rest
 * of the page as it found it passed every assertion here.
 */
static void check_page(const char *what, const char *message, uint32_t length,
                       uint8_t message_attr, uint8_t fill_char,
                       uint8_t fill_attr, const struct vm86_cpu *cpu,
                       unsigned *checks, unsigned *failures)
{
    const uint8_t *page    = bios10_active_page(cpu, &g_video);
    uint16_t       columns = g_video.columns;

    (*checks)++;

    if (!page) {
        uprintf("      FAIL  %s: the active page is not in guest memory\n",
                what);
        (*failures)++;
        return;
    }

    if (columns == 0 || columns > VM86_TEXT_COLUMNS)
        columns = VM86_TEXT_COLUMNS;

    for (uint32_t cell = 0; cell < (uint32_t)columns * BIOS10_ROWS; cell++) {
        uint8_t want_char = cell < length ? (uint8_t)message[cell] : fill_char;
        uint8_t want_attr = cell < length ? message_attr : fill_attr;
        uint8_t got_char  = page[cell * 2u];
        uint8_t got_attr  = page[cell * 2u + 1u];

        if (got_char == want_char && got_attr == want_attr)
            continue;

        uprintf("      FAIL  %s: cell %u is (%02X, %02X), expected "
                "(%02X, %02X)\n", what, (unsigned)cell, got_char, got_attr,
                want_char, want_attr);
        (*failures)++;
        return;
    }
}

/*
 * What the display was handed, at the point where it matters.
 *
 * This replaces an assertion that compared the rows the backend printed
 * against the guest's memory. That check is gone, and not because it
 * stopped being interesting: the question it asked is answered by a much
 * shorter argument now. The backend is handed the address of the guest's
 * page and passes that address to the kernel, which draws out of it during
 * the call -- so there is no transcription step for a drawing routine to
 * get wrong.
 *
 * What that argument rests on is written down at the top of this file and
 * is not unconditional: it holds while the page is drawn from the caller's
 * memory, which is today, and stops holding the day the console keeps
 * pages of its own.
 *
 * What can still go wrong is quieter, and this is what watches for it. A
 * run in which nothing was ever presented passes every assertion about the
 * page the guest left behind, because that page is in guest memory and
 * this process can read it whether or not anybody was told about it. So
 * the count is the assertion, and a zero is reported as its own outcome
 * rather than as a pass -- the same shape as the kernel's stack check, for
 * the same reason.
 *
 * The pixels are a different question and this cannot answer it. See
 * tools/run-screen-test.sh, which reads them out of a screendump.
 */
static void check_screen(const char *what, unsigned pages, unsigned refused,
                         unsigned *checks, unsigned *failures)
{
    (*checks)++;

    if (pages == 0) {
        uprintf("      FAIL  %s: NOTHING WAS PRESENTED -- the guest ran and "
                "the console was never handed a page, so there is nothing to "
                "say about the screen at all\n", what);
        (*failures)++;
        return;
    }

    if (refused) {
        uprintf("      FAIL  %s: the kernel refused %u of the %u page(s) it "
                "was handed\n", what, refused, pages);
        (*failures)++;
        return;
    }

    uprintf("  screen : %s -- %u page(s) presented, none refused\n",
            what, pages);
}

/*
 * Every row of the page that has something on it, read out of the guest's
 * own memory.
 *
 * This is what puts the acceptance sentence's text into the serial log now
 * that the page is no longer printed a row at a time, and it is what a
 * screendump is judged against -- see tools/check-screen-pixels.py, which
 * reads these lines and then looks for exactly these characters on the
 * screen.
 *
 * It is a report and not an assertion. What it says is what the guest's
 * memory holds, which check_page has already asserted cell by cell against
 * the manual and the sample's own header. Whether any of it reached a
 * framebuffer is the question the screendump answers.
 *
 * Blank rows are left out. A row nobody mentions is expected to be blank,
 * and saying so twenty-five times would bury the rows that carry the
 * answer -- which is the failure mode of a report that prints everything
 * it knows.
 */
static void report_rows(const char *what, const struct vm86_cpu *cpu)
{
    const uint8_t *page    = bios10_active_page(cpu, &g_video);
    uint16_t       columns = g_video.columns;
    char           line[VM86_TEXT_COLUMNS + 1u];
    unsigned       reported = 0;

    if (!page || columns == 0 || columns > VM86_TEXT_COLUMNS)
        return;

    uprintf("  screen : %s %ux%u\n", what, (unsigned)columns,
            (unsigned)BIOS10_ROWS);

    for (uint16_t row = 0; row < BIOS10_ROWS; row++) {
        uint16_t used = columns;

        for (uint16_t col = 0; col < columns; col++) {
            uint8_t ch = page[((uint32_t)row * columns + col) * 2u];

            line[col] = (ch >= 0x20u && ch < 0x7Fu) ? (char)ch : ' ';
        }

        line[columns] = '\0';

        /* Trailing spaces are the page's, not the program's: a teletype
         * write stops where it stops, and a line padded to eighty columns
         * is a line nobody wants to read in a log. */
        while (used > 0 && line[used - 1] == ' ')
            line[--used] = '\0';

        if (used == 0)
            continue;

        uprintf("  screen : %s row %u = \"%s\"\n", what, (unsigned)row, line);
        reported++;
    }

    uprintf("  screen : %s has %u row(s) with something on them\n",
            what, reported);
}

/*
 * Take the console's screen, and give it back.
 *
 * A failure is reported and not treated as fatal: everything this file
 * asserts about the guest's page is about the guest's memory, and a
 * machine with no framebuffer still has to be able to check that. What is
 * not allowed is a run that could not take the screen and then reporting
 * the screen as fine -- which is what the flag is for, and why
 * screen_present counts presents only while it is set.
 */
static bool screen_take(void)
{
    int problem = u_screen_acquire();

    if (problem != 0) {
        uprintf("  screen : the console is NOT AVAILABLE (%d) -- the guest's "
                "screen cannot be shown, and nothing below tests a "
                "display\n", problem);
        return false;
    }

    g_screen.holding = true;

    uprintf("  screen : the console screen is this program's until the end "
            "of the run\n");

    return true;
}

static void screen_give_back(void)
{
    if (!g_screen.holding)
        return;

    g_screen.holding = false;

    if (u_screen_release() != 0)
        uputs("  screen : the kernel would not take the screen back\n");
    else
        uputs("  screen : the console screen has been given back\n");
}

/*
 * The acceptance case.
 *
 * hello.asm is the program M4's acceptance sentence is about: a .COM that
 * prints one line through INT 10h and appears on a screen. direct.asm
 * writes the same line straight into 0xB8000 without executing a single
 * interrupt, and the cells the two leave must agree -- so a machine that
 * ever keeps the display as a second copy of the screen keeps passing
 * hello and stops showing direct, and the failure lands where it can be
 * read.
 *
 * The attribute is checked as well as the character, and not for
 * completeness. INT 10h AH=0Eh writes the character and keeps whatever
 * attribute the cell already has, and what gives the page its 0x07 is the
 * mode set -- so a machine that came up with a zeroed page would put
 * black-on-black text in memory and pass every assertion that only looked
 * at the characters.
 */
static bool run_acceptance(void)
{
    static const char TEXT[] = "M4 hello from the BIOS";
    const uint32_t text_length = (uint32_t)(sizeof TEXT - 1u);

    unsigned        checks   = 0;
    unsigned        failures = 0;
    unsigned        before;
    unsigned        pages;
    unsigned        refused;

    pages   = g_presents;
    refused = g_refused;

    if (!run_corpus("hello", vm_corpus_hello,
                    (uint32_t)vm_corpus_hello_size)) {
        uprintf("  VM: case hello: FAIL\n");
        return false;
    }

    check_page("hello", TEXT, text_length, VM86_ATTR_DEFAULT,
               ' ', VM86_ATTR_DEFAULT, &g_cpu, &checks, &failures);
    check_screen("hello", g_presents - pages, g_refused - refused,
                 &checks, &failures);
    report_rows("hello", &g_cpu);

    uprintf("  VM: case hello: %s\n", failures ? "FAIL" : "PASS");

    before  = failures;
    pages   = g_presents;
    refused = g_refused;

    if (!run_corpus("direct", vm_corpus_direct,
                    (uint32_t)vm_corpus_direct_size)) {
        uprintf("  VM: case direct: FAIL\n");
        return false;
    }

    /*
     * direct.asm executes no interrupt at all, so its page is the one the
     * machine was cleared to -- zeroes -- with the same twenty-two cells
     * written by hand. That the two pages agree where the message is, and
     * differ everywhere else in exactly the way each program's own header
     * says, follows from the two whole-page calls: if both pass, the
     * message cells are the same bytes by construction.
     */
    check_page("direct", TEXT, text_length, VM86_ATTR_DEFAULT,
               0x00, 0x00, &g_cpu, &checks, &failures);

    /*
     * M4-8, which was found by a display that kept a second copy of the
     * screen: such a display would still show hello.asm, whose characters
     * arrive through a service, and would not show direct.asm, whose never
     * pass through one.
     *
     * The pair of checks below is what still catches that, and it catches
     * it for a different reason than the old one did. The page is what the
     * guest's memory holds -- asserted above, for both programs, cell by
     * cell -- and the present count is what says the display was told about
     * it. A display that only ever learned about pages that came through a
     * service would present once here and not twice, and direct.asm's
     * twenty-two cells are in a page nothing else would have shown.
     */
    check_screen("direct (M4-8)", g_presents - pages, g_refused - refused,
                 &checks, &failures);
    report_rows("direct", &g_cpu);

    uprintf("  VM: case direct: %s\n", failures == before ? "PASS" : "FAIL");
    uprintf("  checks : %u ok, %u failed\n", checks - failures, failures);

    return failures == 0;
}

/*
 * The clock, driven by the one device on this machine that is not a
 * pretend one.
 *
 * timer.asm counts five interrupts and prints the digit. The digit is not
 * the interesting part: what matters is that the host hands
 * bios1a_advance() the milliseconds that REALLY passed -- the kernel's
 * calibrated LAPIC timer, through SYS_UPTIME_MS -- so that five ticks is
 * five times 54.925 ms of wall time rather than five times however fast
 * the emulator happened to run. A host that raised one tick per slice
 * would satisfy every assertion about the guest and be lying about the
 * only thing the clock service is for.
 *
 * Which is why the budget is enormous and the assertion is on the wall
 * clock. The guest is halted between ticks, so this takes about a quarter
 * of a second of real time and a great many iterations of a loop that
 * mostly does nothing -- and that is not a defect in the loop, it is what
 * a machine keeping time looks like when the program is waiting for the
 * clock to move.
 */

/*
 * 18.2 Hz: 65536 / 1.193182 MHz, the divisor the PC's timer has used since
 * 1981 and the one docs/dos-refs.md section 6 gives. Five ticks is
 * 274.627 ms.
 *
 * WHERE THE BOUNDS COME FROM, in full, because this is the comment a
 * person changing the clock rate will be reading and the arithmetic is not
 * where they would look for it.
 *
 *   Elapsed time is measured with SYS_UPTIME_MS, which is timer_millis()
 *   in kernel/arch/x86_64/timer.c:
 *
 *       return g_ticks * 1000 / TIMER_HZ;
 *
 *   with TIMER_HZ = 100. So the milliseconds this program hands the clock
 *   on any turn are always a multiple of ten, and nothing between two
 *   multiples is reachable.
 *
 *   Five ticks need 274.627 ms. The first multiple of ten at or above that
 *   is 280, so 280 is the smallest reading this machine can produce for
 *   five ticks -- and 270 is the largest it can produce for four.
 *
 *   The lower bound of 274 therefore sits between the two values the
 *   guest's counting can actually reach. It holds, and it holds because
 *   274.627 is not itself a multiple of ten.
 *
 *   That is a margin created by quantisation and not one chosen here. It
 *   survives TIMER_HZ = 250 (readings are multiples of four, the smallest
 *   five-tick reading 276) and TIMER_HZ = 1000 (multiples of one, 275).
 *   It does NOT survive a rate coarse enough that the step is wider than
 *   the 55 ms between four ticks and five: at TIMER_HZ = 10 the readings
 *   are multiples of a hundred, four ticks reads 300, and this bound stops
 *   telling four from five at all. Anyone moving TIMER_HZ downward has to
 *   work these numbers out again; nothing in this file will tell them.
 *
 *   What quantisation did not do is let a wrong host through. A host that
 *   raised a tick per turn instead of per tick's worth of time finishes in
 *   microseconds, and zero is below 274 however finely it is measured.
 *
 * An earlier failure is worth recording next to this one because it looked
 * like it and was not. Two million turns of this loop was 270 ms, which is
 * under the 275 ms the guest was waiting for -- so the guest was one tick
 * short when the budget ran out. That was the budget counting turns
 * instead of counting time; see VM_RUN_MAX_MS. Having these numbers is
 * what made the two tellable apart: 270 is what four ticks reads, and four
 * ticks was exactly what the guest had.
 */
#define VM_TICKS_WANTED       5u
#define VM_TICKS_MIN_MS       274u
#define VM_TICKS_MAX_MS       340u

/* A reading of wall time taken around the whole case, kept only because a
 * number nobody can see is a number nobody can argue with. It is printed
 * and NOT asserted: the kernel's millisecond counter is quantised and QEMU
 * does not schedule this process on time, so one run of this took 370 ms
 * and another 450 ms for the same 275 ms of clock. A bound tight enough to
 * be interesting would be a bound that fails at random, and the two that
 * are tight -- the ms handed to the clock, and VM_RUN_MAX_MS -- are taken
 * where the numbers mean something. */

static bool run_ticks_case(void)
{
    unsigned        checks   = 0;
    unsigned        failures = 0;
    unsigned        pages;
    unsigned        refused;
    unsigned long   started;
    unsigned long   elapsed;
    uint32_t        bda;
    uint32_t        counter;
    uint32_t        ticks;
    uint16_t        counted;
    char            shown[2];

    started = u_uptime_ms();
    g_ms_advanced = 0;
    pages   = g_presents;
    refused = g_refused;

    if (!run_corpus("timer", vm_corpus_timer,
                    (uint32_t)vm_corpus_timer_size)) {
        uprintf("  VM: case timer: FAIL\n");
        return false;
    }

    elapsed = u_uptime_ms() - started;

    /* The firmware's own count, read out of the data area rather than
     * through the service -- a program reads it directly and so does this,
     * which is the only way the two can be caught disagreeing. */
    bda = ((uint32_t)VM86_BDA_SEGMENT << 4) + VM86_BDA_TICK_COUNT;

    ticks = (uint32_t)g_guest_ram[bda] |
            ((uint32_t)g_guest_ram[bda + 1u] << 8) |
            ((uint32_t)g_guest_ram[bda + 2u] << 16) |
            ((uint32_t)g_guest_ram[bda + 3u] << 24);

    /* And the one the guest kept, which is the one it printed. */
    counter = ((uint32_t)GUEST_SEGMENT << 4) + 0x0802u;

    counted = (uint16_t)(g_guest_ram[counter] |
                         ((uint16_t)g_guest_ram[counter + 1u] << 8));

    uprintf("  clock  : the firmware counted %u, the guest counted %u\n",
            (unsigned)ticks, (unsigned)counted);

    /*
     * The character the screen must show, computed from the guest's own
     * counter rather than written here as '5'.
     *
     * That is the difference between an assertion and a coincidence. A
     * literal is a string a display backend that never read video memory
     * could have printed; a character computed from a number this test
     * reads out of the guest's RAM is not.
     */
    shown[0] = (char)('0' + (counted & 0x0Fu));
    shown[1] = '\0';

    check_page("timer", shown, 1u, VM86_ATTR_DEFAULT, ' ', VM86_ATTR_DEFAULT,
               &g_cpu, &checks, &failures);
    check_screen("timer", g_presents - pages, g_refused - refused,
                 &checks, &failures);

    /*
     * Two questions that used to share one message, and they point at
     * completely different places.
     *
     * FIRST: did the guest and the firmware both count five? That is about
     * the guest, the clock service and the 08h -> 1Ch chain. If it is not
     * five then the rate means nothing, so the rate is not looked at -- a
     * wrong count and a wrong rate are different faults with different
     * causes, and reading one as the other sends the reader somewhere the
     * problem is not.
     *
     * SECOND, and only when the count is right: did five ticks take a sane
     * amount of real time? The lower bound is structural -- five ticks
     * cannot have come out of less than 274.6 ms of clock, whatever the
     * machine did. The upper bound is NOT structural: it allows one tick of
     * slack for a host that hands the clock time as it passes, and this
     * process can be taken off the CPU for longer than that. So tripping
     * the upper bound with the count right is a statement about the MACHINE
     * rather than about the guest, and the message says so.
     */
    checks++;

    if (counted != VM_TICKS_WANTED || ticks != VM_TICKS_WANTED) {
        uprintf("      FAIL  timer: THE COUNT IS WRONG -- the guest counted "
                "%u ticks and the firmware counted %u, and the case waits "
                "for exactly %u. That is the clock or the 08h -> 1Ch chain, "
                "not the scheduler: fewer means the interrupts did not "
                "arrive and more means something counted twice. The rate is "
                "not judged until the count is right.\n", (unsigned)counted,
                (unsigned)ticks, VM_TICKS_WANTED);
        failures++;
    } else {
        checks++;

        if (g_ms_advanced < VM_TICKS_MIN_MS) {
            uprintf("      FAIL  timer: THE CLOCK RAN FAST -- %u ticks came "
                    "out of only %u ms of real time, and five of them are "
                    "worth %u. The guest counted right, so what is wrong is "
                    "the rate the host hands the clock.\n",
                    (unsigned)ticks, (unsigned)g_ms_advanced,
                    VM_TICKS_MIN_MS);
            failures++;
        } else if (g_ms_advanced > VM_TICKS_MAX_MS) {
            uprintf("      FAIL  timer: THE MACHINE WAS DESCHEDULED -- the "
                    "guest counted %u ticks, which is right, but %u ms of "
                    "real time went by while it did and this bound is %u. "
                    "The guest is not at fault and neither is the clock; "
                    "this process was taken off the CPU.\n",
                    (unsigned)ticks, (unsigned)g_ms_advanced,
                    VM_TICKS_MAX_MS);
            failures++;
        } else {
            uprintf("  clock  : %u ticks out of %u ms of real time\n",
                    (unsigned)ticks, (unsigned)g_ms_advanced);
        }
    }

    uprintf("  clock  : %u ms of clock, %u ms of wall time, %u reads, "
            "%u ticks raised\n", (unsigned)g_ms_advanced, (unsigned)elapsed,
            (unsigned)g_clock_reads, (unsigned)g_ticks_raised);

    uprintf("  VM: case timer: %s\n", failures ? "FAIL" : "PASS");
    uprintf("  checks : %u ok, %u failed\n", checks - failures, failures);

    return failures == 0;
}

/* ------------------------------------------------------------------ */

int vm_selftest(void)
{
    unsigned long started = u_uptime_ms();

    uputs("\n[vm86 interpreter]\n");

    vm86_set_conflict_reporter(report_conflict, NULL);

    /*
     * The dispatch table first, before anything is executed. A conflict
     * here would make every result below meaningless for a reason that
     * has nothing to do with the instruction being tested, and the run
     * would fail somewhere else entirely.
     */
    if (!vm86_ops_build()) {
        uputs("  Dispatch table : FAILED, two groups claim the same "
              "opcode\n");
        uputs("  The conflict above names them. Nothing below would be\n");
        uputs("  trustworthy until it is fixed.\n");
        uputs("  VM: RESULT FAIL\n");
        return 1;
    }

    uprintf("  Dispatch table : built from the five group tables, "
            "no conflicts\n");
    uprintf("  Guest memory   : %u bytes, static because Ring 3 has no "
            "heap\n", (unsigned)GUEST_RAM_BYTES);
    uprintf("  Runaway limit  : %u instructions per case\n",
            (unsigned)GUEST_INSN_LIMIT);

    bool ok = true;

    for (size_t i = 0; i < CASE_COUNT; i++) {
        /* `&& ok` rather than `ok &=`: every case must run even after one
         * has failed, so that a single run reports all of them. */
        ok = run_case(&g_cases[i]) && ok;
    }

    /*
     * And then the milestone's own sentence: a .COM, through the BIOS, on
     * a screen. The two cases above run bytes on a bare processor; these
     * run programs on a machine.
     */
    uputs("\n[8086 machine]\n");

    /*
     * The console is taken here rather than at the top of the file: what
     * runs above this line is arithmetic on a processor with no devices,
     * and its output belongs in the log with the rest of the report.
     *
     * A machine that cannot give the screen over is a machine where the
     * acceptance sentence above is not met, so it fails the run -- and it
     * is said plainly rather than left to the per-case counts, because
     * "the console was busy" and "the display was never reached" are
     * different faults with different causes.
     */
    bool have_screen = screen_take();

    ok = run_acceptance() && ok;
    ok = run_ticks_case() && ok;

    screen_give_back();

    ok = have_screen && ok;

    uprintf("\n  VM: RESULT %s\n", ok ? "PASS" : "FAIL");
    uprintf("  VM: took %u ms\n", (unsigned)(u_uptime_ms() - started));
    uprintf("  screen : %u page(s) presented in total, %u refused\n",
            g_presents, g_refused);

    return ok ? 0 : 1;
}

/*
 * The acceptance case, with the screen left up.
 *
 * This exists for one reason: a screendump has to be taken while the page
 * is on the screen, and the run above gives the screen back as soon as it
 * is done -- so there is no moment a photograph could be taken at.
 *
 * Waiting for a key is how that moment is made, and it is not a trick. A
 * DOS program that finishes by waiting for a keypress is the most ordinary
 * thing there is, and the alternative -- a program that spins for a while
 * and hopes somebody looked -- is a test that would fail on a slow machine
 * and pass on a fast one. A key that never comes is a screen that never
 * changes, which is what makes tools/run-screen-test.sh able to look at it
 * whenever it gets around to it.
 *
 * The counters below are the same ones the run above uses, reported for
 * the same reason: a page that was never presented would leave the
 * assertions about the guest's memory green and the screen empty.
 */
int vm_screen_hold(void)
{
    static const char TEXT[] = "M4 hello from the BIOS";
    const uint32_t text_length = (uint32_t)(sizeof TEXT - 1u);

    unsigned checks   = 0;
    unsigned failures = 0;

    uputs("\n[vm86 screen]\n");

    if (!screen_take())
        return 1;

    if (!run_corpus("hello", vm_corpus_hello,
                    (uint32_t)vm_corpus_hello_size)) {
        uprintf("  VM: case hello: FAIL\n");
        screen_give_back();
        return 1;
    }

    check_page("hello", TEXT, text_length, VM86_ATTR_DEFAULT,
               ' ', VM86_ATTR_DEFAULT, &g_cpu, &checks, &failures);
    check_screen("hello", g_presents, g_refused, &checks, &failures);
    report_rows("hello", &g_cpu);

    uprintf("  screen : the cursor is at cell %u\n",
            (unsigned)bios10_cursor_cell(&g_cpu, &g_video));

    uprintf("  VM: case hello: %s\n", failures ? "FAIL" : "PASS");
    uprintf("  checks : %u ok, %u failed\n", checks - failures, failures);
    uprintf("  VM: RESULT %s\n", failures ? "FAIL" : "PASS");

    if (failures) {
        screen_give_back();
        return 1;
    }

    uprintf("  screen : the page stays up until a key arrives\n");
    uflush();

    /*
     * The hold. A key that never arrives leaves this here for as long as
     * the machine is up, which is the point; a key that does arrive is
     * somebody at a keyboard, and then the console comes back.
     *
     * A zero from u_getkey means the kernel had no key to give, which it
     * only says when interrupts are off -- so this can spin rather than
     * block. Spinning is still a screen that does not change, and the
     * alternative is returning and taking the page down.
     */
    while (u_getkey() == 0)
        ;

    screen_give_back();

    return 0;
}

/*
 * The W4 acceptance, with the screen left up.
 *
 * A .COM that asks DOS for things and prints what it was told, on the
 * real machine rather than in a host suite. The host suite is where the
 * bytes are checked -- dos/tests/test_int21.c, with every expected value
 * transcribed from docs/dos-refs-dos.md and sixteen injections proving
 * the assertions can fail -- and what this adds is the same thing vm=psp
 * added for the loader: that it works in the address space it will be
 * used in, and that what it printed is *visible*.
 *
 * The program ends with INT 21h AH=4Ch, so this is the first run in the
 * project whose result is an exit code rather than a halt. drive() prints
 * it; tools/run-screen-test.sh asserts on the line.
 *
 * No command tail: this program reads nothing out of its PSP except the
 * segment registers, and a tail here would be a second thing the two runs
 * had in common rather than a second thing checked.
 */
int vm_int21_hold(void)
{
    unsigned checks   = 0;
    unsigned failures = 0;

    uputs("\n[vm86 int21]\n");

    if (!screen_take())
        return 1;

    if (!run_dos_program("int21", vm_corpus_dos_int21,
                         (uint32_t)vm_corpus_dos_int21_size, NULL,
                         GUEST_INT21_PATH)) {
        screen_give_back();
        return 1;
    }

    check_screen("int21", g_presents, g_refused, &checks, &failures);
    report_rows("int21", &g_cpu);

    uprintf("  screen : the cursor is at cell %u\n",
            (unsigned)bios10_cursor_cell(&g_cpu, &g_video));

    uprintf("  VM: case int21: %s\n", failures ? "FAIL" : "PASS");
    uprintf("  checks : %u ok, %u failed\n", checks - failures, failures);
    uprintf("  VM: RESULT %s\n", failures ? "FAIL" : "PASS");

    if (failures) {
        screen_give_back();
        return 1;
    }

    uprintf("  screen : the page stays up until a key arrives\n");
    uflush();

    while (u_getkey() == 0)
        ;

    screen_give_back();

    return 0;
}

/*
 * The DOS acceptance, with the screen left up.
 *
 * This is the first program in the project that is loaded the way DOS
 * loads one, running in the place it will be used rather than only in a
 * host suite. That distinction is the same one W2 made about the display:
 * the loader is a library in dos/, and a library exercised only by its own
 * suite is a library whose first real use is also its first real test.
 *
 * The tail is passed rather than defaulted, because the whole point of
 * that field is that it carries what the caller was given -- and a program
 * started with an argument here exercises the FCBs, which is the part of
 * the PSP a loader gets wrong by doing the obvious thing.
 *
 * The command-line values are not asserted here. The host suite owns the
 * loader, with every expectation transcribed from docs/dos-refs-dos.md and
 * five injections proving the assertions can fail. What this run adds is
 * that the same loader works in the address space it will be used in, and
 * that what it loaded is *visible* -- which is what the screendump in
 * tools/run-screen-test.sh reads back off the framebuffer.
 */
#define VM_PSP_TAIL " A:FILE.EXE"

int vm_psp_hold(void)
{
    unsigned checks   = 0;
    unsigned failures = 0;

    uputs("\n[vm86 dos]\n");

    if (!screen_take())
        return 1;

    if (!run_dos_program("psp", vm_corpus_dos_psp,
                         (uint32_t)vm_corpus_dos_psp_size, VM_PSP_TAIL,
                         GUEST_PROGRAM_PATH)) {
        screen_give_back();
        return 1;
    }

    check_screen("psp", g_presents, g_refused, &checks, &failures);
    report_rows("psp", &g_cpu);

    uprintf("  screen : the cursor is at cell %u\n",
            (unsigned)bios10_cursor_cell(&g_cpu, &g_video));

    uprintf("  VM: case psp: %s\n", failures ? "FAIL" : "PASS");
    uprintf("  checks : %u ok, %u failed\n", checks - failures, failures);
    uprintf("  VM: RESULT %s\n", failures ? "FAIL" : "PASS");

    if (failures) {
        screen_give_back();
        return 1;
    }

    uprintf("  screen : the page stays up until a key arrives\n");
    uflush();

    while (u_getkey() == 0)
        ;

    screen_give_back();

    return 0;
}

/* Resource acquisition is outside DOS. The FAT implementation can be fed
 * the same bytes by a host suite, and never knows about SYS_OPEN/READ. */
static bool mount_disk(void)
{
    g_mounted=false;
    int fd=u_open("DOS.IMG",O_RDONLY);
    if(fd<0) { uputs("  VM: DOS.IMG missing: FAIL\n"); return false; }
    unsigned used=0;
    while(used<sizeof(g_disk)) {
        long n=u_read(fd,g_disk+used,sizeof(g_disk)-used);
        if(n<=0) break;
        used+=(unsigned)n;
    }
    uint8_t extra;
    long trailing=u_read(fd,&extra,1);
    u_close(fd);
    if(used!=sizeof(g_disk) || trailing!=0 || fat_mount(&g_files,g_disk,used)) {
        uputs("  VM: invalid FAT image: FAIL\n"); return false;
    }
    g_mounted=true;
    uputs("  VM: FAT12 mounted from DOS.IMG\n");
    return true;
}
static bool load_com(const char *name,unsigned *size)
{
    uint16_t handle;
    if(fat_open(&g_files,name,0,false,0,&handle)) return false;
    uint32_t n=0; int e=fat_read(&g_files,handle,g_com,sizeof(g_com),&n);
    uint8_t extra; uint32_t trailing=0;
    if(!e) e=fat_read(&g_files,handle,&extra,1,&trailing);
    fat_close(&g_files,handle);
    if(e || trailing || !n) return false;
    *size=n; return true;
}
int vm_run_file(const char *name,const char *tail)
{
    unsigned size;
    if(!mount_disk() || !load_com(name,&size)) { uprintf("  VM: cannot load %s\n",name); return 1; }
    if(!screen_take()) { g_mounted=false; return 1; }
    if(u_kbd_acquire()!=0) { screen_give_back(); g_mounted=false; return 1; }
    g_raw_active=true;
    bool ran=run_dos_program(name,g_com,size,tail,name);
    int code=ran && g_cpu.exited ? g_cpu.exit_code : 1;
    report_rows(g_resource_hold ? (g_resource_hold==5 ? "files" : "keys") : name,&g_cpu);
    uprintf("  screen : the cursor is at cell %u\n",(unsigned)bios10_cursor_cell(&g_cpu,&g_video));
    g_raw_active=false;
    u_kbd_release();
    if(g_resource_hold && ran && code==0) {
        uprintf("W%u screen ready\n",g_resource_hold);
        /* W6's translated nonblocking primitive has a real consumer too:
         * the hold observes empty polls, then the test's release key. */
        while(u_pollkey()==0) { }
        uputs("  VM: translated nonblocking release key: PASS\n");
    }
    screen_give_back(); g_mounted=false;
    return code;
}
int vm_resources_test(int keyboard)
{
    const char *name=keyboard ? "KEYS.COM" : "FILES.COM";
    g_resource_hold=keyboard ? 6u : 5u;
    int code=vm_run_file(name,NULL);
    g_resource_hold=0;
    uprintf("W%u resource acceptance: %s\n",keyboard ? 6u : 5u,code==0 ? "PASS" : "FAIL");
    return code;
}
