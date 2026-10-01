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
 * No second program, and no shell command. `g_current` is a single global
 * and `process_run()` is not reentrant, so a shell cannot start a process
 * from inside a process -- the shell already *is* the process. That is a
 * limitation of the M2 process model rather than of the emulator, and
 * lifting it is M4's work. M3 has to show that the operating system can
 * execute guest instructions; it does not have to show that the shell can
 * launch one.
 *
 * No file loading either. The guest programs are the byte arrays below.
 * SYS_OPEN and SYS_READ exist, but turning a file into a loaded .COM is
 * M5.
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
#include <vm86/firmware.h>
#include <vm86/host.h>
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
 * The display, which is this console.
 *
 * Not driven by the services. A program that writes 0xB8000 directly never
 * passes through one -- direct.asm does exactly that -- so the host reads
 * the page on its own schedule, which is what display.h's header argues
 * for and the only arrangement in which a direct write is visible at all.
 */
struct vm_screen {
    const struct bios10_state *video;
    uint8_t                    frame[VM_PAGE_CELLS * 2u];
    bool                       drawn;
};

static struct bios10_state g_video;
static struct bios16_state g_keyboard;
static struct bios1a_state g_clock;
static struct vm_screen    g_screen;
static struct vm86_display g_display;

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
 * What the display last drew, one row at a time, exactly as it was written
 * out.
 *
 * Kept because "the line reached the screen" is not a statement anybody
 * can check by looking at the frame the backend copied: a backend that
 * ignored the guest's memory and printed a string of its own would satisfy
 * it. What can be checked is that the rows drawn ARE the guest's screen,
 * and that needs the rows as they went out.
 */
static char g_drawn[BIOS10_ROWS][VM86_TEXT_COLUMNS + 1u];

/*
 * One row at a time: the characters, and the attributes dropped.
 *
 * A console has one colour and the guest's attributes select a palette
 * this machine does not have, so what can be shown is the text -- which is
 * what the acceptance sentence is about. Characters outside printable
 * ASCII become spaces rather than being written out: a real screen shows a
 * glyph for most of them, a terminal would interpret them, and a control
 * character in a text page turning into a cursor movement is a worse
 * answer than a blank.
 */
static void draw_frame(const struct bios10_state *video, const uint8_t *cells)
{
    char line[VM86_TEXT_COLUMNS + 1u];

    if (video->columns == 0 || video->columns > VM86_TEXT_COLUMNS)
        return;

    /*
     * Cleared before drawing, so that what this records is only what THIS
     * frame put on the screen.
     *
     * Without it, a backend that drew some rows and skipped the rest would
     * leave the skipped rows holding the previous frame's text, and a
     * check of "what was drawn against what the guest's memory holds"
     * would pass for every row whose contents had not changed since. That
     * is exactly the shape of hole this whole arrangement exists to close:
     * a screen nobody drew, agreeing with the guest by being out of date.
     */
    for (uint16_t row = 0; row < BIOS10_ROWS; row++)
        for (uint16_t col = 0; col <= video->columns; col++)
            g_drawn[row][col] = '\0';

    for (uint16_t row = 0; row < BIOS10_ROWS; row++) {
        for (uint16_t col = 0; col < video->columns; col++) {
            uint8_t ch = cells[(row * video->columns + col) * 2u];

            line[col] = (ch >= 0x20u && ch < 0x7Fu) ? (char)ch : ' ';
        }

        line[video->columns] = '\0';

        for (uint16_t col = 0; col <= video->columns; col++)
            g_drawn[row][col] = line[col];

        uputsln(line);
    }
}

static void screen_present(void *ctx, const uint8_t *cells, uint16_t cursor)
{
    struct vm_screen *screen = ctx;

    /*
     * The cursor is not drawn.
     *
     * A guest cursor is a cell index and this console has no addressing, so
     * the honest answer is that this backend shows the text and nothing
     * else -- including when the guest has turned its cursor off, which is
     * why VM86_DISPLAY_NO_CURSOR needs no branch here. display.h leaves
     * what the cursor looks like to the host, and this host has nowhere to
     * put one.
     */
    (void)cursor;

    if (!cells || !screen->video)
        return;

    /*
     * One redraw per change, not one per call. A program that prints
     * character by character changes the page nearly every slice, and two
     * slices that changed nothing produce the same 4000 bytes and leave
     * the console alone. The comparison is of the whole page, so this is
     * exactly the "redraw everything, unless nothing moved" the task book
     * asks about -- not a per-cell diff with its own bugs.
     */
    if (screen->drawn &&
        frame_same(screen->frame, cells, (uint32_t)sizeof screen->frame))
        return;

    for (uint32_t i = 0; i < sizeof screen->frame; i++)
        screen->frame[i] = cells[i];

    screen->drawn = true;
    draw_frame(screen->video, cells);
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
static void machine_build(struct vm86_mem *mem, struct vm86_cpu *cpu,
                          const uint8_t *image, uint32_t size)
{
    vm86_mem_attach(mem, g_guest_ram, (uint32_t)GUEST_RAM_BYTES);
    vm86_mem_clear(mem);
    vm86_reset(cpu, mem);

    vm86_clear_services();
    vm86_install_firmware(cpu);

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

    /*
     * The services' own state has to be stood up here; the data area the
     * firmware wrote is not the same thing. The mode, the cursor and the
     * page are this struct's until a service changes them.
     */
    bios10_reset(&g_video);
    bios16_reset(&g_keyboard);
    bios1a_reset(&g_clock);

    g_screen.video   = &g_video;
    g_screen.drawn   = false;
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
 * One program from dos/corpus/bios, driven the way the machine will be
 * driven: in slices, with the display refreshed between them, because the
 * host has a screen to draw and a clock to advance and a guest never asks
 * it to do either.
 *
 * What is deliberately NOT here is the keyboard. The kernel hands out
 * decoded key codes rather than scan codes, and the only call that gets
 * one blocks until a key arrives -- so a guest waiting on INT 16h would
 * wait forever and the machine's clock would stop with it. key.asm is a
 * host-suite case. M4-README.md section 8 records the gap in full; nothing
 * here pretends to close it.
 */
static bool run_corpus(const char *name, const uint8_t *image, uint32_t size)
{
    enum vm86_stop  stop = VM86_STOP_STEPS;
    unsigned        slices;
    unsigned long   last_ms;

    uprintf("\n  --- guest case \"%s\" ---\n", name);

    machine_build(&g_mem, &g_cpu, image, size);

    uprintf("  image  : %u bytes at %04X:%04X, SP=%04X\n",
            (unsigned)size, (unsigned)g_cpu.cs, (unsigned)g_cpu.ip,
            (unsigned)g_cpu.sp);

    last_ms = u_uptime_ms();

    for (slices = 0; slices < VM_RUN_MAX_TURNS; slices++) {
        uint64_t retired = g_cpu.insn_count;

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

        if (stop == VM86_STOP_FAULT || stop == VM86_STOP_BROKEN)
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

    uprintf("  result : halted after %u slice(s), %u ms of real time\n",
            slices + 1u, (unsigned)g_ms_advanced);

    return true;
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
 * What the display drew, against what the guest's memory holds.
 *
 * This is the assertion a backend that does not read video memory cannot
 * pass. "The guest's line reached the screen" says only that something
 * emitted that string, and something can: a draw_frame() that ignored its
 * argument and printed a line of its own satisfied it with every other
 * assertion green, which is how this was found.
 *
 * A screen that is a picture of the guest's memory has to be reproducible
 * from the guest's memory, so this reads the page itself -- independently
 * of the copy the backend kept, and independently of anything the backend
 * recorded -- and compares it with the rows that actually went out.
 *
 * Characters only. This console has one colour, the attributes are dropped
 * on purpose, and what is being compared is what a screen could show.
 */
static void check_drawn(const char *what, const struct vm86_cpu *cpu,
                        unsigned *checks, unsigned *failures)
{
    const uint8_t *page    = bios10_active_page(cpu, &g_video);
    uint16_t       columns = g_video.columns;

    (*checks)++;

    if (!page || columns == 0 || columns > VM86_TEXT_COLUMNS) {
        uprintf("      FAIL  %s: there is no page to compare the drawn "
                "rows with\n", what);
        (*failures)++;
        return;
    }

    for (uint16_t row = 0; row < BIOS10_ROWS; row++) {
        for (uint16_t col = 0; col < columns; col++) {
            uint8_t ch   = page[((uint32_t)row * columns + col) * 2u];
            char    want = (ch >= 0x20u && ch < 0x7Fu) ? (char)ch : ' ';

            if (g_drawn[row][col] == want)
                continue;

            uprintf("      FAIL  %s: row %u column %u was drawn as %02X and "
                    "the screen should show %02X there (the page holds "
                    "%02X) -- the screen is not a picture of the guest's "
                    "memory\n", what, (unsigned)row, (unsigned)col,
                    (unsigned char)g_drawn[row][col], (unsigned char)want,
                    (unsigned)ch);
            (*failures)++;
            return;
        }
    }
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

    if (!run_corpus("hello", vm_corpus_hello,
                    (uint32_t)vm_corpus_hello_size)) {
        uprintf("  VM: case hello: FAIL\n");
        return false;
    }

    check_page("hello", TEXT, text_length, VM86_ATTR_DEFAULT,
               ' ', VM86_ATTR_DEFAULT, &g_cpu, &checks, &failures);
    check_drawn("hello", &g_cpu, &checks, &failures);

    uprintf("  VM: case hello: %s\n", failures ? "FAIL" : "PASS");

    before = failures;

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
     * M4-8, and this is the assertion that can see it. hello.asm's
     * characters reach the screen through a service and direct.asm's never
     * pass through one: a display kept as a second copy of the screen
     * would still show hello and would not show direct. What says the two
     * paths end in the same place is the drawn rows being a picture of the
     * guest's memory -- for the program that used the BIOS and, here, for
     * the one that did not.
     */
    check_drawn("direct (M4-8)", &g_cpu, &checks, &failures);

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
    unsigned long   started;
    unsigned long   elapsed;
    uint32_t        bda;
    uint32_t        counter;
    uint32_t        ticks;
    uint16_t        counted;
    char            shown[2];

    started = u_uptime_ms();
    g_ms_advanced = 0;

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
    check_drawn("timer", &g_cpu, &checks, &failures);

    checks++;

    if (ticks != VM_TICKS_WANTED) {
        uprintf("      FAIL  timer: the firmware counted %u ticks, "
                "expected %u\n", (unsigned)ticks, VM_TICKS_WANTED);
        failures++;
    }

    checks++;

    if (counted != ticks) {
        uprintf("      FAIL  timer: the guest counted %u and the firmware "
                "counted %u, so the 08h -> 1Ch chain lost or invented a "
                "tick\n", (unsigned)counted, (unsigned)ticks);
        failures++;
    }

    checks++;

    if (g_ms_advanced < VM_TICKS_MIN_MS || g_ms_advanced > VM_TICKS_MAX_MS) {
        uprintf("      FAIL  timer: %u ticks came out of %u ms handed to "
                "the clock, expected between %u and %u\n", VM_TICKS_WANTED,
                (unsigned)g_ms_advanced, VM_TICKS_MIN_MS, VM_TICKS_MAX_MS);
        failures++;
    } else {
        uprintf("  clock  : %u ticks out of %u ms of real time\n",
                VM_TICKS_WANTED, (unsigned)g_ms_advanced);
    }

    uprintf("  clock  : %u ms of clock, %u ms of wall time\n",
            (unsigned)g_ms_advanced, (unsigned)elapsed);

    uprintf("  VM: case timer: %s\n", failures ? "FAIL" : "PASS");
    uprintf("  clock  : %u clock reads, %u ticks raised\n",
            (unsigned)g_clock_reads, (unsigned)g_ticks_raised);
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

    ok = run_acceptance() && ok;
    ok = run_ticks_case() && ok;

    uprintf("\n  VM: RESULT %s\n", ok ? "PASS" : "FAIL");
    uprintf("  VM: took %u ms\n", (unsigned)(u_uptime_ms() - started));

    return ok ? 0 : 1;
}
