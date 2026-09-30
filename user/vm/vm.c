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

#include <libu/libu.h>

#include <vm86/cpu.h>
#include <vm86/mem.h>
#include <vm86/ops.h>

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
 * The reason it is not larger is not a property of the emulator. Ring 3
 * has no heap, so the guest's memory is a static array -- and the build
 * turns .bss into real zero bytes in the image, which tools/bin2c.py then
 * expands to six characters of C source per byte. Sixteen megabytes here
 * would mean ninety-six megabytes of generated source compiled into the
 * kernel.
 *
 * It is a named constant because it is meant to change. Task G is adding
 * a memory size to the loader (ELF's p_memsz); once a process can own
 * more memory than its image, this becomes a zero region the kernel hands
 * over, and whoever makes that change is the one to raise this number.
 * At that point sixteen megabytes costs nothing.
 */
#define GUEST_RAM_BYTES (1024u * 1024u)

/*
 * Where a guest program is loaded, and where its stack starts. Both come
 * from the convention; neither is a decision this file makes.
 */
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
 * Ring 3 has no heap, so the guest's memory is static storage. See the
 * note on the size above for why it is not bigger.
 */
static uint8_t g_guest_ram[GUEST_RAM_BYTES];

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

int vm_selftest(void)
{
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

    uprintf("\n  VM: RESULT %s\n", ok ? "PASS" : "FAIL");

    return ok ? 0 : 1;
}
