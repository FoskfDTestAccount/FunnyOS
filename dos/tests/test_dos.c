/*
 * The DOS layer: the Program Segment Prefix, the environment block, and
 * the loader that puts a program behind them.
 *
 * ---------------------------------------------------------------------
 * Where the expected values come from, which is the whole point of this
 * file
 *
 * Every offset and every value asserted below is transcribed from
 * docs/dos-refs-dos.md, not read back out of the loader. That distinction
 * is the only thing standing between this suite and a suite that agrees
 * with the loader about a mistake: if the loader wrote the memory top at
 * 0x04 and the test read 0x04 to check it, both would be wrong together
 * and the run would be green.
 *
 * So the offsets in the assertions are literals -- 0x0002, 0x002C, 0x0080
 * -- and so are the values. Where a value is derived rather than stated
 * (the memory top is the machine's conventional memory, which is 640 KiB
 * and therefore segment 0xA000) the arithmetic is written out next to it.
 * And where a value is a decision this project made rather than a fact it
 * looked up, the test says which decision -- the DOS version above all.
 *
 * The one thing that does come from the program rather than from the
 * reference is the *format* of what it prints: the labels and the column
 * they line up in. That is the program's business. The numbers beside them
 * are not.
 *
 * ---------------------------------------------------------------------
 * Reading a screen rather than a service's answer
 *
 * The acceptance for this work is "a hand-written .COM reads its own PSP
 * and prints the fields", and none of it goes through a service: a program
 * reads a PSP with `mov ax, [2Ch]`. So the only evidence is what ended up
 * on the page, and that is what these assertions read -- guest memory at
 * 0xB8000, exactly as the display backend would.
 */
#include "harness.h"

#include <stdio.h>
#include <string.h>

#include <vm86/dos.h>
#include <vm86/firmware.h>
#include <vm86/host.h>

#include "../bios/bios10.h"

/* The corpus is embedded by the build; see the DOS_CORPUS_SRC rules in
 * dos/Makefile. Declared by hand because the generated file is a .c and
 * not a header -- the alternative is a second generated file, and this is
 * two lines. */
#define DOS_IMAGE(sym)                            \
    extern const unsigned char sym[];             \
    extern const unsigned long sym##_size;

DOS_IMAGE(corpus_psp)

/* ------------------------------------------------------------------ */
/* The test's own memory layout                                        */
/* ------------------------------------------------------------------ */

/*
 * Where this suite puts things. None of it is a fact about the loader:
 * the segment is the caller's to choose and the header says why. These are
 * the test's choices, written down so that the addresses in the assertions
 * below can be read without holding a map in your head.
 */
#define TEST_PROGRAM_SEGMENT     0x1000u
#define TEST_PROGRAM_LINEAR      ((uint32_t)TEST_PROGRAM_SEGMENT << 4)
#define TEST_ENVIRONMENT_SEGMENT 0x0F00u
#define TEST_ENVIRONMENT_LINEAR \
    ((uint32_t)TEST_ENVIRONMENT_SEGMENT << 4)

/* The parent PSP segment. Any value will do -- nothing dereferences it --
 * and a non-zero one is used so that a loader that forgot to write it
 * cannot pass by leaving a zero behind. */
#define TEST_PARENT_SEGMENT      0x1234u

/* What the program is told it is called. */
#define TEST_PATH "F:\\PSP.COM"

/* ------------------------------------------------------------------ */
/* A machine to load onto                                              */
/* ------------------------------------------------------------------ */

/*
 * The firmware, and only the firmware.
 *
 * A DOS program needs a video service to print and nothing else: the PSP
 * comes from memory the loader wrote, and the program reads it with a
 * plain `mov`. So this is deliberately not the whole replay machine --
 * registering the disk and the keyboard would be four devices the loader
 * has no relationship with.
 */
static void power_on(struct vm86_cpu *cpu, struct bios10_state *video)
{
    vm86_mem_clear(cpu->mem);
    vm86_reset(cpu, cpu->mem);

    vm86_clear_services();
    vm86_install_firmware(cpu);

    bios10_reset(video);
    vm86_register_service(VM86_INT_VIDEO, bios10_service, video);
}

/* The default start description, with `tail` and the variables the test
 * wants. A structure rather than ten parameters at each call site. */
static struct vm86_dos_start
start_of(const char *tail, const char *const *vars, uint32_t var_count)
{
    struct vm86_dos_start start = {
        .segment     = TEST_PROGRAM_SEGMENT,
        .environment = TEST_ENVIRONMENT_SEGMENT,
        .parent      = TEST_PARENT_SEGMENT,
        .path        = TEST_PATH,
        .tail        = tail,
        .vars        = vars,
        .var_count   = var_count,
    };

    return start;
}

/* Load the corpus program and assert that the loader accepted it. */
static struct vm86_dos_psp
load_psp_program(struct vm86_cpu *cpu, struct bios10_state *video,
                 const char *tail, const char *const *vars,
                 uint32_t var_count)
{
    struct vm86_dos_start start = start_of(tail, vars, var_count);
    struct vm86_dos_psp   psp;
    enum vm86_dos_load_result result;

    power_on(cpu, video);

    result = vm86_dos_load(cpu, corpus_psp, (uint32_t)corpus_psp_size,
                           &start, &psp);

    vm86_expect_u16("load result", (uint16_t)result,
                    (uint16_t)VM86_DOS_LOADED);

    return psp;
}

/* ------------------------------------------------------------------ */
/* Reading things back out of guest memory                             */
/* ------------------------------------------------------------------ */

static uint32_t program(uint32_t offset)
{
    return TEST_PROGRAM_LINEAR + offset;
}

static void expect_psp_byte(struct vm86_cpu *cpu, const char *what,
                            uint32_t offset, uint8_t want)
{
    vm86_expect_mem8(what, cpu, program(offset), want);
}

static void expect_psp_word(struct vm86_cpu *cpu, const char *what,
                            uint32_t offset, uint16_t want)
{
    vm86_expect_mem16(what, cpu, program(offset), want);
}

/*
 * One row of the text page as characters.
 *
 * Attributes are dropped on purpose: what is under test is whether the
 * program printed the right digits, and a failure that has to be found by
 * reading a screen is one where the attribute is the least of it. The
 * video service's own suite is where attributes are checked.
 */
static void screen_row(struct vm86_cpu *cpu, unsigned row, char *out,
                       size_t size)
{
    unsigned columns = 80;

    if (size == 0)
        return;
    if (size - 1 < columns)
        columns = (unsigned)(size - 1);

    for (unsigned col = 0; col < columns; col++) {
        uint32_t cell = VM86_TEXT_BASE + ((row * 80u) + col) * 2u;

        out[col] = (char)vm86_mem_read8(cpu->mem, cell);
    }

    out[columns] = '\0';
}

/* Strip the padding the page has after the last character printed. */
static void trim(char *s)
{
    size_t length = strlen(s);

    while (length > 0 && s[length - 1] == ' ')
        s[--length] = '\0';
}

static void expect_row(struct vm86_cpu *cpu, const char *what, unsigned row,
                       const char *want)
{
    char got[81];

    screen_row(cpu, row, got, sizeof got);
    trim(got);

    if (strcmp(got, want) != 0) {
        printf("      FAIL  %s: row %u is \"%s\", expected \"%s\"\n",
               what, row, got, want);
        vm86_expect_u16(what, 1, 0);
        return;
    }

    vm86_expect_u16(what, 0, 0);
}

/* ------------------------------------------------------------------ */
/* The tests                                                           */
/* ------------------------------------------------------------------ */

/*
 * The PSP, field by field, read straight out of guest memory.
 *
 * The offsets are the ones in dos-refs-dos.md section 1 and the values are
 * the ones section 6 says this machine puts there. Nothing here goes
 * through the loader's own account of what it did -- `struct
 * vm86_dos_psp` is not looked at -- because that account is the thing
 * under test.
 */
static void test_psp_fields(struct vm86_cpu *cpu)
{
    struct bios10_state video;

    load_psp_program(cpu, &video, "", NULL, 0);

    /* 0x00: CD 20, the INT 20h a bare RET arrives at. Read as a word it is
     * 0x20CD, which is also the signature a tool checks for. */
    expect_psp_word(cpu, "PSP:0000", 0x0000, 0x20CD);

    /* 0x02: the segment past the program's memory. A .COM is given
     * everything up to the top of conventional memory, which this machine
     * reports as 640 KiB -- so the first byte past it is 0x0A0000, which
     * is segment 0x0A00. Writing the arithmetic out rather than the
     * constant is deliberate: this is the assertion that would notice the
     * machine's memory being changed, and it should be readable as the
     * claim it is. */
    expect_psp_word(cpu, "PSP:0002", 0x0002, (640u * 1024u) / 16u);

    /* 0x16: the parent's PSP. */
    expect_psp_word(cpu, "PSP:0016", 0x0016, TEST_PARENT_SEGMENT);

    /* 0x18: the job file table, every entry free. 0xFF is what DOS leaves
     * in an unused slot -- not 0, which is a valid handle. */
    for (unsigned i = 0; i < 20; i++)
        expect_psp_byte(cpu, "PSP:0018 JFT", 0x0018 + i, 0xFF);

    /* 0x2C: the environment. */
    expect_psp_word(cpu, "PSP:002C", 0x002C, TEST_ENVIRONMENT_SEGMENT);

    /* 0x32 and 0x34: the JFT's size and where it is. The pointer is a far
     * pointer -- offset in the low word, segment in the high one -- and
     * that is the field where treating it as a linear address looks the
     * same and is wrong. */
    expect_psp_word(cpu, "PSP:0032", 0x0032, 20);
    expect_psp_word(cpu, "PSP:0034 JFT offset", 0x0034, 0x0018);
    expect_psp_word(cpu, "PSP:0036 JFT segment", 0x0036,
                    TEST_PROGRAM_SEGMENT);

    /* 0x40: the version this machine reports. 0x1E03 is 3.30 -- AH is the
     * major, AL the minor as a plain byte -- and which version to claim is
     * a decision, recorded in dos-refs-dos.md section 6. A test asserting
     * a decision is what stops it being changed by accident, so this one
     * is asserted rather than derived. */
    expect_psp_word(cpu, "PSP:0040", 0x0040, 0x1E03);

    /* 0x50: INT 21h; RETF. Three bytes, not a word -- the CB is the byte
     * that matters and a word comparison would not look at it. */
    expect_psp_byte(cpu, "PSP:0050", 0x0050, 0xCD);
    expect_psp_byte(cpu, "PSP:0051", 0x0051, 0x21);
    expect_psp_byte(cpu, "PSP:0052", 0x0052, 0xCB);
}

/*
 * The three saved vectors, which have to be what was in the table.
 *
 * The field means "where control went before this program existed", and
 * DOS puts the previous handler's address there so that a program that
 * chains or restores one goes somewhere real. A loader that wrote zeroes
 * would be writing an address that a program restoring INT 24h would jump
 * to.
 *
 * Asserted against the table as it was *before* the load, which is the
 * only source for it -- so this one test is the exception to "every value
 * comes from the reference": what the reference says is that the value is
 * the previous contents, and the previous contents are only knowable by
 * looking.
 */
static void test_saved_vectors(struct vm86_cpu *cpu)
{
    struct bios10_state video;
    uint16_t before[3][2];

    power_on(cpu, &video);

    for (unsigned i = 0; i < 3; i++) {
        uint32_t vector = (uint32_t)(0x22u + i) * 4u;

        before[i][0] = vm86_mem_read16(cpu->mem, vector);
        before[i][1] = vm86_mem_read16(cpu->mem, vector + 2u);
    }

    struct vm86_dos_start start = start_of("", NULL, 0);
    struct vm86_dos_psp   psp;

    unsigned result = (unsigned)vm86_dos_load(cpu, corpus_psp,
                                              (uint32_t)corpus_psp_size,
                                              &start, &psp);
    vm86_expect_u16("load result", (uint16_t)result, VM86_DOS_LOADED);
    (void)psp;

    expect_psp_word(cpu, "PSP:000A (INT 22h offset)", 0x000A, before[0][0]);
    expect_psp_word(cpu, "PSP:000C (INT 22h segment)", 0x000C, before[0][1]);
    expect_psp_word(cpu, "PSP:000E (INT 23h offset)", 0x000E, before[1][0]);
    expect_psp_word(cpu, "PSP:0010 (INT 23h segment)", 0x0010, before[1][1]);
    expect_psp_word(cpu, "PSP:0012 (INT 24h offset)", 0x0012, before[2][0]);
    expect_psp_word(cpu, "PSP:0014 (INT 24h segment)", 0x0014, before[2][1]);

    /* And the table itself must be untouched: the loader saves the old
     * vectors, it does not install its own. */
    vm86_expect_u16("the vector table still holds INT 22h",
                    vm86_mem_read16(cpu->mem, 0x22u * 4u), before[0][0]);
}

/*
 * The state the program is entered in.
 *
 * dos-refs-dos.md section 2: all four segment registers the program's own
 * segment, IP 0x100, SP 0xFFFE, a zero word at SS:FFFE, and IF set.
 *
 * IF is the one that differs from this project's other corpus convention,
 * and it is asserted here for that reason: the two conventions exist side
 * by side and a change that made them converge would be invisible in every
 * other test.
 */
static void test_entry_state(struct vm86_cpu *cpu)
{
    struct bios10_state video;
    struct vm86_dos_psp psp = load_psp_program(cpu, &video, "", NULL, 0);

    vm86_expect_u16("CS", cpu->cs, TEST_PROGRAM_SEGMENT);
    vm86_expect_u16("DS", cpu->ds, TEST_PROGRAM_SEGMENT);
    vm86_expect_u16("ES", cpu->es, TEST_PROGRAM_SEGMENT);
    vm86_expect_u16("SS", cpu->ss, TEST_PROGRAM_SEGMENT);
    vm86_expect_u16("IP", cpu->ip, 0x0100);
    vm86_expect_u16("SP", cpu->sp, 0xFFFE);

    vm86_expect_flag("IF", cpu, VM86_IF, true);

    /* The zero word, which is what makes a bare RET an exit. */
    vm86_expect_mem16("the word a RET returns through", cpu,
                      program(0xFFFE), 0x0000);

    /* And the program's own bytes are at 0x100, not at 0. */
    vm86_expect_mem8("the first byte of the image", cpu, program(0x0100),
                     corpus_psp[0]);

    vm86_expect_u16("the loader's account of the segment",
                    psp.segment, TEST_PROGRAM_SEGMENT);
    vm86_expect_u16("the loader's account of the memory top",
                    psp.memory_top, (640u * 1024u) / 16u);
}

/*
 * The environment block, byte for byte.
 *
 * dos-refs-dos.md section 3: "NAME=VALUE" strings, then a NUL ending the
 * list, then a word count, then the program's full path. The trap is that
 * the list ends with an *empty string* rather than with the last one, and
 * the test below reads the whole block out as bytes so that a loader which
 * forgot the extra NUL -- or wrote two of them -- is a visible difference
 * rather than a program that misbehaves later.
 */
static void test_environment(struct vm86_cpu *cpu)
{
    struct bios10_state video;

    static const char *const vars[] = {
        "COMSPEC=F:\\COMMAND.COM",
        "PATH=F:\\",
        NULL,                       /* unset, and must be skipped */
        "PROMPT=$P$G",
    };

    load_psp_program(cpu, &video, "", vars, 4);

    /*
     * The list, then the extra NUL that ends it, then the count word and
     * the path.
     *
     * The count is the one place this machine can differ from a real DOS:
     * COMMAND.COM always knows the program's path, and a caller here may
     * not, so a caller with no path gets a count of 0 and nothing after
     * it. This case has a path, so the count is 1.
     */
    size_t offset = strlen("COMSPEC=F:\\COMMAND.COM") + 1u
                  + strlen("PATH=F:\\") + 1u
                  + strlen("PROMPT=$P$G") + 1u;

    vm86_expect_mem8("the environment's list terminator", cpu,
                     TEST_ENVIRONMENT_LINEAR + offset, 0x00);
    vm86_expect_mem16("the environment's string count", cpu,
                      TEST_ENVIRONMENT_LINEAR + offset + 1u, 1u);

    /* The three set strings, checked byte by byte. The fourth entry in
     * `vars` is NULL and must leave no trace at all -- an unset variable
     * that became an empty "=" string would be a variable a program could
     * find and read. */
    const char *want[3] = { vars[0], vars[1], vars[3] };
    size_t at = 0;

    for (unsigned i = 0; i < 3; i++) {
        for (size_t j = 0; want[i][j]; j++) {
            vm86_expect_mem8("an environment variable", cpu,
                             TEST_ENVIRONMENT_LINEAR + at + j,
                             (uint8_t)want[i][j]);
        }

        vm86_expect_mem8("a variable's terminator", cpu,
                         TEST_ENVIRONMENT_LINEAR + at + strlen(want[i]), 0x00);

        at += strlen(want[i]) + 1u;
    }

    vm86_expect_u16("the three strings and their terminators are all there",
                    (uint16_t)at, (uint16_t)offset);

    /* And the path, right after the count word. */
    const char *path = TEST_PATH;
    for (size_t j = 0; j <= strlen(path); j++)
        vm86_expect_mem8("the program's path in the environment", cpu,
                         TEST_ENVIRONMENT_LINEAR + offset + 3u + j,
                         (uint8_t)path[j]);
}

/*
 * The two default FCBs.
 *
 * dos-refs-dos.md section 5: with no parameters at all, the drive byte is
 * zero and the name and extension are *blanks*. "All zeroes" is the answer
 * that looks equally reasonable and is not what DOS leaves, so this is the
 * assertion that pins the difference -- and it is why the test reads the
 * eleven bytes rather than just the drive.
 */
static void test_fcbs_no_arguments(struct vm86_cpu *cpu)
{
    struct bios10_state video;

    load_psp_program(cpu, &video, "", NULL, 0);

    expect_psp_byte(cpu, "FCB1 drive", 0x005C, 0x00);
    for (unsigned i = 0; i < 11; i++)
        expect_psp_byte(cpu, "FCB1 name", 0x005D + i, ' ');

    expect_psp_byte(cpu, "FCB2 drive", 0x006C, 0x00);
    for (unsigned i = 0; i < 11; i++)
        expect_psp_byte(cpu, "FCB2 name", 0x006D + i, ' ');
}

/*
 * And with arguments, parsed the CP/M way.
 *
 * This is the approximate part -- dos-refs-dos.md section 7 lists what the
 * approximation is and what it leaves out -- so what is asserted here is
 * the part that is documented: an optional drive letter, a name of up to
 * eight characters, an optional dot, an extension of up to three, upper
 * cased and space padded.
 */
static void test_fcbs_with_arguments(struct vm86_cpu *cpu)
{
    struct bios10_state video;

    load_psp_program(cpu, &video, " A:FILE.EXE notes.txt", NULL, 0);

    expect_psp_byte(cpu, "FCB1 drive is A:", 0x005C, 1);

    static const char name1[8] = { 'F', 'I', 'L', 'E', ' ', ' ', ' ', ' ' };
    for (unsigned i = 0; i < 8; i++)
        expect_psp_byte(cpu, "FCB1 name", 0x005D + i, (uint8_t)name1[i]);

    static const char ext1[3] = { 'E', 'X', 'E' };
    for (unsigned i = 0; i < 3; i++)
        expect_psp_byte(cpu, "FCB1 extension", 0x0065 + i, (uint8_t)ext1[i]);

    /* The second parameter has no drive, so it is the default one. */
    expect_psp_byte(cpu, "FCB2 drive is the default", 0x006C, 0);

    static const char name2[8] = { 'N', 'O', 'T', 'E', 'S', ' ', ' ', ' ' };
    for (unsigned i = 0; i < 8; i++)
        expect_psp_byte(cpu, "FCB2 name", 0x006D + i, (uint8_t)name2[i]);

    static const char ext2[3] = { 'T', 'X', 'T' };
    for (unsigned i = 0; i < 3; i++)
        expect_psp_byte(cpu, "FCB2 extension", 0x0075 + i, (uint8_t)ext2[i]);
}

/*
 * The command tail, and the blank that is a habit rather than a rule.
 *
 * dos-refs-dos.md section 4 has the sources, including the counterexample
 * -- a program can be invoked with no blank before its first argument. So
 * the loader copies what it is given, and this pair of tests is what says
 * it does not invent one: the same characters with and without a leading
 * blank must come out the same way they went in.
 */
static void test_command_tail_verbatim(struct vm86_cpu *cpu)
{
    struct bios10_state video;

    static const char tail[] = " A B.C D";
    const uint8_t length = (uint8_t)(sizeof tail - 1u);

    load_psp_program(cpu, &video, tail, NULL, 0);

    expect_psp_byte(cpu, "the tail length", 0x0080, length);

    for (uint8_t i = 0; i < length; i++)
        expect_psp_byte(cpu, "the tail", 0x0081 + i, (uint8_t)tail[i]);

    /* Terminated by a CR, which is not counted in the length. */
    expect_psp_byte(cpu, "the tail's terminator", 0x0081 + length, 0x0D);
}

static void test_command_tail_without_a_blank(struct vm86_cpu *cpu)
{
    struct bios10_state video;

    /* The case the habit misses: no separator between the program name and
     * its first argument. A loader that inserted a blank would report a
     * length one greater than this and shift every byte. */
    static const char tail[] = "/V C:";
    const uint8_t length = (uint8_t)(sizeof tail - 1u);

    load_psp_program(cpu, &video, tail, NULL, 0);

    expect_psp_byte(cpu, "the tail length", 0x0080, length);
    expect_psp_byte(cpu, "the first byte of the tail", 0x0081, '/');
    expect_psp_byte(cpu, "the tail's terminator", 0x0081 + length, 0x0D);
}

/*
 * The acceptance: the program runs, reads its own PSP, and prints it.
 *
 * Everything above asserts the memory the loader left. This asserts the
 * one thing those cannot: that a program entered the way DOS enters one
 * can *find* it -- the four segment registers, the stack word, and the
 * plain `mov ax, [2Ch]` that a real program uses to reach the environment.
 *
 * The values in the expected rows come from the reference and the test's
 * own layout, never from the loader's out-structure.
 */
static void test_the_program_prints_its_own_psp(struct vm86_cpu *cpu)
{
    struct bios10_state video;
    struct vm86_dos_start start = start_of(" A:FILE.EXE", NULL, 0);
    struct vm86_dos_psp psp;
    char expected[64];

    power_on(cpu, &video);

    /*
     * What the vector table holds before anything is loaded.
     *
     * The PSP's three saved-vector fields mean "where control went before
     * this program existed", so the only source for them is the table as
     * it is -- and this test reads them there rather than writing down
     * what this machine's firmware happens to put in it, which would make
     * the assertion a test of the stub layout instead of a test of the
     * loader. test_saved_vectors asserts the same thing about memory; this
     * one asserts that the *program* can see it.
     */
    uint16_t int22_offset  = vm86_mem_read16(cpu->mem, 0x22u * 4u);
    uint16_t int22_segment = vm86_mem_read16(cpu->mem, 0x22u * 4u + 2u);

    vm86_expect_u16("load result",
                    (uint16_t)vm86_dos_load(cpu, corpus_psp,
                                            (uint32_t)corpus_psp_size,
                                            &start, &psp),
                    (uint16_t)VM86_DOS_LOADED);

    enum vm86_result result = vm86_test_run(cpu, 500000);
    vm86_expect_u16("the program halted", (uint16_t)result,
                    (uint16_t)VM86_HALT);

    /* The mode set leaves the cursor at the top left, so the first line the
     * program prints is row 0 and the rest follow one per line. The
     * expected row numbers are the program's own layout, which is the one
     * thing about this printout that is not the reference's. */
    expect_row(cpu, "CS line", 0, "CS       = 1000");
    expect_row(cpu, "DS line", 1, "DS       = 1000");
    expect_row(cpu, "ES line", 2, "ES       = 1000");
    expect_row(cpu, "SS line", 3, "SS       = 1000");

    /* The zero word, popped off the stack by the program itself. */
    expect_row(cpu, "the RET word", 4, "RET word = 0000");

    expect_row(cpu, "PSP:0000", 5, "PSP:0000 = 20CD");

    /*
     * The memory top, as the program reads it. 0xA000 is segment 0xA000,
     * which is 640 KiB -- the arithmetic the other test writes out, here
     * as the four digits the program prints.
     */
    expect_row(cpu, "PSP:0002", 6, "PSP:0002 = A000");

    snprintf(expected, sizeof expected, "PSP:000A = %04X", int22_offset);
    expect_row(cpu, "PSP:000A", 7, expected);

    snprintf(expected, sizeof expected, "PSP:000C = %04X", int22_segment);
    expect_row(cpu, "PSP:000C", 8, expected);

    /* The parent, which the test chose so that a forgotten write cannot
     * pass by leaving a zero behind. */
    expect_row(cpu, "PSP:0016", 9, "PSP:0016 = 1234");

    /* Every JFT entry free, printed as a word because that is the shape
     * the program's reporter has -- so the high half is the zeroes above
     * it and the FF is what matters. */
    expect_row(cpu, "PSP:0018", 10, "PSP:0018 = 00FF");

    expect_row(cpu, "PSP:002C", 11, "PSP:002C = 0F00");
    expect_row(cpu, "PSP:0032", 12, "PSP:0032 = 0014");
    expect_row(cpu, "PSP:0034", 13, "PSP:0034 = 0018");
    expect_row(cpu, "PSP:0036", 14, "PSP:0036 = 1000");
    expect_row(cpu, "PSP:0040", 15, "PSP:0040 = 1E03");
    expect_row(cpu, "PSP:0050", 16, "PSP:0050 = CD21CB");
    expect_row(cpu, "FCB1 drive", 17, "PSP:005C = 0001");

    /* Eight name bytes then three extension bytes: "FILE    EXE". */
    expect_row(cpu, "FCB1 name", 18, "PSP:005D = 46494C4520202020455845");

    expect_row(cpu, "FCB2 drive", 19, "PSP:006C = 0000");

    /* The tail, eleven bytes long and starting with the blank the caller
     * put there -- which is the point: the loader did not add it. */
    expect_row(cpu, "the tail length", 20, "PSP:0080 = 000B");
    expect_row(cpu, "the tail", 21, "TAIL     = \" A:FILE.EXE\"");
}

/* ------------------------------------------------------------------ */
/* The refusals                                                        */
/* ------------------------------------------------------------------ */

/*
 * Each of these is a different mistake and each gets its own test, because
 * "the load failed" is not a thing anybody can act on. A loader that
 * returned a boolean would leave the suite asserting that *something* was
 * refused -- which stays green when the wrong thing is.
 */

static void test_image_too_big_is_refused(struct vm86_cpu *cpu)
{
    struct bios10_state video;
    struct vm86_dos_start start = start_of("", NULL, 0);
    struct vm86_dos_psp psp;

    power_on(cpu, &video);

    /* 0x100 bytes of PSP plus the image has to fit in one segment, so the
     * image cannot be longer than 0xFF00. One byte more must be refused
     * rather than wrapped -- a wrapped .COM is a program whose tail has
     * overwritten its own PSP. */
    static uint8_t big[0xFF01];
    memset(big, 0x90, sizeof big);

    vm86_expect_u16("a 0xFF01-byte image",
                    (uint16_t)vm86_dos_load(cpu, big, sizeof big, &start, &psp),
                    (uint16_t)VM86_DOS_IMAGE_TOO_BIG);

    /* And the boundary itself is allowed, which is the other half of
     * saying where the limit is. */
    vm86_expect_u16("a 0xFF00-byte image",
                    (uint16_t)vm86_dos_load(cpu, big, 0xFF00u, &start, &psp),
                    (uint16_t)VM86_DOS_LOADED);
}

static void test_over_long_tail_is_refused(struct vm86_cpu *cpu)
{
    struct bios10_state video;
    struct vm86_dos_start start = start_of("", NULL, 0);
    struct vm86_dos_psp psp;
    char tail[VM86_PSP_TAIL_MAX + 2u];

    power_on(cpu, &video);

    memset(tail, 'x', sizeof tail);
    tail[sizeof tail - 1u] = '\0';      /* 127 characters */

    start.tail = tail;

    vm86_expect_u16("a 127-byte tail",
                    (uint16_t)vm86_dos_load(cpu, corpus_psp,
                                            (uint32_t)corpus_psp_size,
                                            &start, &psp),
                    (uint16_t)VM86_DOS_TAIL_TOO_LONG);

    tail[126] = '\0';                   /* 126 characters, the maximum */

    vm86_expect_u16("a 126-byte tail",
                    (uint16_t)vm86_dos_load(cpu, corpus_psp,
                                            (uint32_t)corpus_psp_size,
                                            &start, &psp),
                    (uint16_t)VM86_DOS_LOADED);
}

static void test_environment_inside_the_program_is_refused(struct vm86_cpu *cpu)
{
    struct bios10_state video;
    struct vm86_dos_start start = start_of("", NULL, 0);
    struct vm86_dos_psp psp;

    power_on(cpu, &video);

    /* Anywhere in the program's own 64 KiB, including its first and last
     * segments. The program is told it owns all of it, so an environment
     * there is memory the program will run over the moment it uses its
     * heap. */
    start.environment = TEST_PROGRAM_SEGMENT;

    vm86_expect_u16("an environment at the program's own segment",
                    (uint16_t)vm86_dos_load(cpu, corpus_psp,
                                            (uint32_t)corpus_psp_size,
                                            &start, &psp),
                    (uint16_t)VM86_DOS_ENVIRONMENT_IN_THE_WAY);

    start.environment = TEST_PROGRAM_SEGMENT + 0x0FFFu;

    vm86_expect_u16("an environment in the program's last segment",
                    (uint16_t)vm86_dos_load(cpu, corpus_psp,
                                            (uint32_t)corpus_psp_size,
                                            &start, &psp),
                    (uint16_t)VM86_DOS_ENVIRONMENT_IN_THE_WAY);

    /* One segment past the end is outside it again. */
    start.environment = TEST_PROGRAM_SEGMENT + 0x1000u;

    vm86_expect_u16("an environment past the program's block",
                    (uint16_t)vm86_dos_load(cpu, corpus_psp,
                                            (uint32_t)corpus_psp_size,
                                            &start, &psp),
                    (uint16_t)VM86_DOS_LOADED);
}

static void test_environment_out_of_memory_is_refused(struct vm86_cpu *cpu)
{
    struct bios10_state video;
    struct vm86_dos_start start = start_of("", NULL, 0);
    struct vm86_dos_psp psp;

    power_on(cpu, &video);

    /* Past the end of the guest's megabyte. The memory layer drops writes
     * past the end and counts them rather than faulting, so a loader that
     * did not check would produce a program whose environment is silently
     * missing its tail -- and nothing anywhere would say so. */
    start.environment = 0xFFFFu;

    vm86_expect_u16("an environment past the end of guest memory",
                    (uint16_t)vm86_dos_load(cpu, corpus_psp,
                                            (uint32_t)corpus_psp_size,
                                            &start, &psp),
                    (uint16_t)VM86_DOS_NO_SUCH_SEGMENT);
}

static void test_refusals_write_nothing(struct vm86_cpu *cpu)
{
    struct bios10_state video;
    struct vm86_dos_start start = start_of("", NULL, 0);
    struct vm86_dos_psp psp;

    power_on(cpu, &video);

    /* Leave a recognisable pattern where the PSP would go. */
    for (uint32_t i = 0; i < VM86_PSP_BYTES; i++)
        vm86_mem_write8(cpu->mem, program(i), 0xA5u);

    start.environment = TEST_PROGRAM_SEGMENT;   /* refused */

    vm86_expect_u16("the refusal",
                    (uint16_t)vm86_dos_load(cpu, corpus_psp,
                                            (uint32_t)corpus_psp_size,
                                            &start, &psp),
                    (uint16_t)VM86_DOS_ENVIRONMENT_IN_THE_WAY);

    /*
     * And not one byte of it was touched.
     *
     * A loader that writes the PSP and then discovers the environment does
     * not fit has left a program that runs and reads a different world
     * than the caller asked for -- and the difference shows up as the
     * program behaving oddly rather than as an error. The first byte and
     * the last are checked rather than all 256, because a loader that
     * wrote only the last field is the shape this is looking for and one
     * that wrote everything would fail on the first byte anyway.
     */
    expect_psp_byte(cpu, "byte 0 is untouched", 0x0000, 0xA5);
    expect_psp_byte(cpu, "byte 1 is untouched", 0x0001, 0xA5);
    expect_psp_byte(cpu, "the last PSP byte is untouched", 0x00FF, 0xA5);
}

/* ------------------------------------------------------------------ */

static const struct vm86_test tests[] = {
    { "PSP fields",                test_psp_fields },
    { "saved vectors",             test_saved_vectors },
    { "entry state",               test_entry_state },
    { "environment block",         test_environment },
    { "default FCBs, no arguments", test_fcbs_no_arguments },
    { "default FCBs, arguments",   test_fcbs_with_arguments },
    { "command tail, verbatim",    test_command_tail_verbatim },
    { "command tail, no blank",    test_command_tail_without_a_blank },
    { "the program prints its PSP", test_the_program_prints_its_own_psp },

    { "refuses an image too big",  test_image_too_big_is_refused },
    { "refuses an over-long tail", test_over_long_tail_is_refused },
    { "refuses an environment in the way",
      test_environment_inside_the_program_is_refused },
    { "refuses an environment out of memory",
      test_environment_out_of_memory_is_refused },
    { "a refusal writes nothing",  test_refusals_write_nothing },
};

VM86_TEST_MAIN("dos", tests)
