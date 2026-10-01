/*
 * The DOS layer, part one: the Program Segment Prefix, the environment
 * block, and the loader that puts a flat binary behind them.
 *
 * ---------------------------------------------------------------------
 * Where the facts come from
 *
 * docs/dos-refs-dos.md, which is the companion to dos-refs.md and exists
 * for the same reason: this repository has no copy of Ralf Brown's
 * interrupt list, and a field offset written from memory is a field offset
 * that is wrong consistently -- in the loader and in every test written
 * against the loader, so the suite stays green while the machine is wrong.
 *
 * Every offset and every layout in this file is checked against a source
 * named in that document, and the places where the sources disagree or say
 * nothing are listed there too. Two of them are decided here rather than
 * found, and both say so where they are: the version this machine reports,
 * which is dos.h's, and the FCB parse, which is deliberately approximate.
 *
 * ---------------------------------------------------------------------
 * What this file refuses to do
 *
 * Nothing here guesses. A request that does not describe a program this
 * machine can start comes back as the specific reason it was refused, and
 * nothing is written before the last check has passed -- a half-built PSP
 * is a program that runs and reads a different world than the caller asked
 * for, and the symptom of that is an odd behaviour rather than an error.
 *
 * That is why vm86_dos_load computes everything it can before it writes
 * anything. The environment is the one exception: it is built first
 * because its size is not known until it has been built, and a failure
 * there leaves a block nobody can reach rather than a corrupt PSP.
 */
#include <vm86/dos.h>

#include <libk/string.h>

/* ------------------------------------------------------------------ */
/* Reaching guest memory by segment and offset                         */
/* ------------------------------------------------------------------ */

static uint32_t at(uint16_t segment, uint16_t offset)
{
    return ((uint32_t)segment << 4) + offset;
}

static void put8(struct vm86_mem *mem, uint32_t where, uint8_t value)
{
    vm86_mem_write8(mem, where, value);
}

static void put16(struct vm86_mem *mem, uint32_t where, uint16_t value)
{
    vm86_mem_write16(mem, where, value);
}

/*
 * A 32-bit field is two 16-bit writes, low half first.
 *
 * Written this way rather than as a byte loop because it is the same thing
 * the guest's own `mov [x], ax` does, and because the two halves of a
 * far pointer are the two things a reader wants to see in the source.
 */
static void put32(struct vm86_mem *mem, uint32_t where, uint32_t value)
{
    vm86_mem_write16(mem, where, (uint16_t)(value & 0xFFFFu));
    vm86_mem_write16(mem, where + 2u, (uint16_t)(value >> 16));
}

/* Copy a NUL-terminated string, terminator included. */
static uint32_t put_string(struct vm86_mem *mem, uint32_t where,
                           const char *s)
{
    uint32_t length = 0;

    for (; s[length]; length++)
        vm86_mem_write8(mem, where + length, (uint8_t)s[length]);

    vm86_mem_write8(mem, where + length, 0u);

    return length + 1u;
}

/* ------------------------------------------------------------------ */
/* The environment block                                               */
/* ------------------------------------------------------------------ */

/*
 * How many bytes the block will need, or 0 if it cannot fit in a segment.
 *
 * Worked out before a byte is written, for the reason the file header
 * gives: a block that ran off the end of its segment would wrap to offset
 * zero and start overwriting its own first strings, producing an
 * environment that is *nearly* right, which is the hardest kind to read.
 *
 * `vars` is allowed to hold NULLs, which are skipped. An empty entry is
 * the natural way for a caller to say "this one is not set" -- the
 * alternative is an array that has to be rebuilt on every change -- and
 * skipping it here keeps the count word honest.
 */
static uint32_t environment_size(const char *path,
                                 const char *const *vars, uint32_t count)
{
    uint32_t need = 1u + 2u;   /* the list terminator, then the count word */

    for (uint32_t i = 0; i < count; i++) {
        if (!vars[i])
            continue;

        need += (uint32_t)strlen(vars[i]) + 1u;

        if (need > 0x10000u)
            return 0;
    }

    if (path)
        need += (uint32_t)strlen(path) + 1u;

    return need > 0x10000u ? 0u : need;
}

uint16_t vm86_dos_environment(struct vm86_cpu *cpu, uint16_t segment,
                              const char *path,
                              const char *const *vars, uint32_t count)
{
    struct vm86_mem *mem = cpu->mem;
    uint32_t where = (uint32_t)segment << 4;
    uint32_t start = where;
    uint32_t strings = 0;

    if (environment_size(path, vars, count) == 0)
        return 0;

    for (uint32_t i = 0; i < count; i++) {
        if (!vars[i])
            continue;

        where += put_string(mem, where, vars[i]);
        strings++;
    }

    /*
     * The list ends with an empty string, not with the last one.
     *
     * With no variables at all that is a single zero byte, and code that
     * goes looking for two zeros in a row walks straight past the count
     * word and into the path -- which is a real bug that real programs
     * have had, and the reason dos-refs-dos.md section 3 says it twice.
     */
    put8(mem, where, 0u);
    where += 1u;

    /*
     * DOS 3.0 and later: how many strings follow the list, then the
     * program's own full path.
     *
     * The count is 1 when there is a path and 0 when there is not, which
     * is the one place this can be said to differ from a real DOS --
     * COMMAND.COM always knows the path, and a caller here may not. A
     * count of 0 with nothing after it is what a reader of the format
     * would find, so it is the honest encoding of "there isn't one".
     */
    put16(mem, where, path ? 1u : 0u);
    where += 2u;

    if (path)
        where += put_string(mem, where, path);

    /* Rounded up to whole paragraphs, which is the unit DOS allocates an
     * environment in and the unit the caller has to leave room in. */
    return (uint16_t)((where - start + 15u) / 16u);
}

/* ------------------------------------------------------------------ */
/* The two default FCBs                                                */
/* ------------------------------------------------------------------ */

/* The twelve bytes an unopened FCB starts with, before the fields only an
 * open one has. Twice this: the second FCB overlaps the first, and the
 * third parameter would land in it, which is why DOS does not parse one. */
struct fcb_head {
    uint8_t drive;      /* 0 = the default drive, 1 = A:, 2 = B:, ... */
    uint8_t name[8];    /* space padded, upper case                    */
    uint8_t ext[3];     /* space padded, upper case                    */
};

static uint8_t upper(uint8_t c)
{
    return (c >= 'a' && c <= 'z') ? (uint8_t)(c - 'a' + 'A') : c;
}

static const char *skip_blanks(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;

    return p;
}

static bool is_blank_or_end(char c)
{
    return c == '\0' || c == ' ' || c == '\t';
}

/*
 * Parse one parameter into an FCB head and return where it stopped.
 *
 * ---------------------------------------------------------------------
 * This is an approximation, and it is worth being exact about which part
 * is the approximation
 *
 * The *shape* is documented and is implemented as documented: an optional
 * drive letter and colon, a name of up to eight characters, an optional
 * dot, an extension of up to three. The name and extension are upper
 * cased and space padded, because CP/M's were and because that is what a
 * program comparing an FCB field against a literal expects.
 *
 * What is NOT implemented is the rest of what real DOS does: wildcards are
 * copied through rather than expanded, there is no check of which
 * characters are legal, and `*` has none of the fill-with-question-marks
 * behaviour it has in a real FCB. Those matter only to a program that
 * opens one of these FCBs, and opening one is an FCB function call --
 * which is M6 and does not exist here. So the caller-visible consequence
 * today is limited to what a program sees when it *reads* the field, and
 * for a read the rules above are the whole of it.
 *
 * It is written down rather than left implicit because "approximate" is
 * not a property anybody can check. This is the list of what the
 * approximation is.
 */
static const char *fcb_parse(struct fcb_head *fcb, const char *p)
{
    memset(fcb->name, ' ', sizeof fcb->name);
    memset(fcb->ext, ' ', sizeof fcb->ext);
    fcb->drive = 0;

    /* A drive letter, and only if a colon follows it. `A.B` is a file
     * called "A" with extension "B", not drive A and a file. */
    if (p[0] && p[1] == ':') {
        fcb->drive = (uint8_t)(upper((uint8_t)p[0]) - 'A' + 1u);
        p += 2;
    }

    unsigned i = 0;
    while (i < sizeof fcb->name && !is_blank_or_end(*p) && *p != '.' && *p != ':')
        fcb->name[i++] = upper((uint8_t)*p++);

    if (*p == '.') {
        p++;
        i = 0;
        while (i < sizeof fcb->ext && !is_blank_or_end(*p) && *p != '.')
            fcb->ext[i++] = upper((uint8_t)*p++);
    }

    /* Past the rest of whatever the parameter was, so that a name longer
     * than eight characters does not leave its tail to be read as the
     * next parameter. */
    while (!is_blank_or_end(*p))
        p++;

    return p;
}

/*
 * Both FCBs, at 5Ch and 6Ch.
 *
 * With a parameter, its first twelve bytes are parsed over the zeroes the
 * caller left. With no parameter at all the answer is NOT zeroes: it is a
 * zero drive byte followed by blanks for the name and the extension, and
 * that is the shape a loader gets wrong by doing the obvious thing.
 * dos-refs-dos.md section 5 has the source. So the blank form is written
 * either way and the parse only overwrites it when there is something to
 * parse.
 *
 * The twenty-byte gap from 6Ch to 7Fh is left alone, which is what makes
 * the second FCB twenty bytes rather than twelve. It is not padding: an
 * extended FCB wants seven bytes of prefix borrowed from higher up, and a
 * program that reads the second FCB's later fields reads them here.
 */
static void write_fcbs(struct vm86_mem *mem, uint32_t psp, const char *tail)
{
    static const uint32_t at_offset[2] = { VM86_PSP_FCB1, VM86_PSP_FCB2 };

    const char *p = tail ? tail : "";

    for (int which = 0; which < 2; which++) {
        struct fcb_head fcb;

        /* The blank form, which is also what fcb_parse() starts from --
         * and which is the whole answer for a program started with no
         * arguments. */
        fcb.drive = 0;
        memset(fcb.name, ' ', sizeof fcb.name);
        memset(fcb.ext, ' ', sizeof fcb.ext);

        p = skip_blanks(p);
        if (*p)
            p = fcb_parse(&fcb, p);

        uint32_t where = psp + at_offset[which];

        put8 (mem, where, fcb.drive);
        for (unsigned i = 0; i < sizeof fcb.name; i++)
            put8(mem, where + 1u + i, fcb.name[i]);
        for (unsigned i = 0; i < sizeof fcb.ext; i++)
            put8(mem, where + 9u + i, fcb.ext[i]);
    }
}

/* ------------------------------------------------------------------ */
/* The loader                                                          */
/* ------------------------------------------------------------------ */

/* A .COM is one segment by definition: the PSP, the image, and the stack
 * all live inside the same 64 KiB, and a program that overruns it wraps
 * to the start rather than reaching for more. */
#define SEGMENT_BYTES 0x10000u

/*
 * What goes in PSP:0002 -- the segment of the first byte past the memory
 * DOS has given this program.
 *
 * DOS hands a .COM everything from its PSP up to the top of conventional
 * memory, which is why the field is about the machine's size and not about
 * the program's. This machine's conventional memory is firmware.h's
 * constant, and the whole point of it living there is that this answer and
 * INT 12h's are the same answer.
 *
 * The exception is an environment placed *above* the program: the
 * allocation stops where that block begins, and saying otherwise would
 * have the program treat the environment as spare heap.
 */
static uint16_t memory_top_for(const struct vm86_dos_start *start)
{
    if (start->environment > start->segment &&
        start->environment < VM86_CONVENTIONAL_SEGMENT)
        return start->environment;

    return VM86_CONVENTIONAL_SEGMENT;
}

enum vm86_dos_load_result
vm86_dos_load(struct vm86_cpu *cpu, const uint8_t *image, uint32_t image_size,
              const struct vm86_dos_start *start, struct vm86_dos_psp *out)
{
    struct vm86_mem *mem = cpu->mem;

    uint32_t psp      = at(start->segment, 0);
    uint32_t env_at   = (uint32_t)start->environment << 4;
    uint32_t tail_len = start->tail ? (uint32_t)strlen(start->tail) : 0u;
    uint32_t top      = SEGMENT_BYTES - VM86_PSP_BYTES;   /* room for code */
    uint16_t memory_top = memory_top_for(start);

    /* --- Everything that can be refused, before anything is written --- */

    if (image_size > top)
        return VM86_DOS_IMAGE_TOO_BIG;

    if (tail_len > VM86_PSP_TAIL_MAX)
        return VM86_DOS_TAIL_TOO_LONG;

    /* Both blocks have to be wholly inside the guest's memory, not merely
     * to start inside it. A block that ran off the end would have its
     * writes dropped by the memory layer -- which counts them rather than
     * faulting -- and the program would come up reading a PSP whose tail
     * is missing, with nothing anywhere saying so. */
    if (env_at + SEGMENT_BYTES > mem->size)
        return VM86_DOS_NO_SUCH_SEGMENT;

    if (psp + SEGMENT_BYTES > mem->size)
        return VM86_DOS_NO_SUCH_SEGMENT;

    /* The program owns the whole 64 KiB from its PSP, so an environment
     * anywhere inside that is an environment the program will run over. */
    if ((uint32_t)start->environment >= start->segment &&
        (uint32_t)start->environment < start->segment + SEGMENT_BYTES / 16u)
        return VM86_DOS_ENVIRONMENT_IN_THE_WAY;

    /* --- The environment, which has to be built to know its size --- */

    uint16_t paragraphs = vm86_dos_environment(cpu, start->environment,
                                               start->path,
                                               start->vars, start->var_count);

    if (paragraphs == 0)
        return VM86_DOS_ENVIRONMENT_TOO_BIG;

    /* --- The PSP --- */

    memset(mem->ram + vm86_mem_offset(mem, psp), 0, VM86_PSP_BYTES);

    /* The CP/M exit. A near RET at the end of a program pops the zero word
     * off the stack, arrives at IP 0, and lands here. */
    put8(mem, psp + VM86_PSP_EXIT, 0xCDu);
    put8(mem, psp + VM86_PSP_EXIT + 1u, 0x20u);

    put16(mem, psp + VM86_PSP_MEMORY_TOP, memory_top);

    /*
     * PSP:05h is left as the zeroes the memset put there, and that is a
     * decision rather than an omission -- dos-refs-dos.md section 1 works
     * out what DOS puts in those five bytes and why this machine does not.
     * Briefly: it is a CP/M-80 entry that reaches the INT 30h vector by
     * way of a 1 MiB address wrap, this machine has no INT 30h handler,
     * and filling in bytes that look right and lead nowhere is worse than
     * leaving a hole where a reader can see it.
     */

    /* The three vectors DOS is about to take over, saved so that a
     * program can put them back. Read out of the table rather than written
     * as constants: the field means "where control went before this
     * program existed", and a made-up value is somewhere a program that
     * restores them would jump to. */
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t vector = (uint32_t)(0x22u + i) * 4u;

        put16(mem, psp + VM86_PSP_OLD_INT22 + i * 4u,
              vm86_mem_read16(mem, vector));
        put16(mem, psp + VM86_PSP_OLD_INT22 + i * 4u + 2u,
              vm86_mem_read16(mem, vector + 2u));
    }

    put16(mem, psp + VM86_PSP_PARENT, start->parent);

    /* Every handle free. This machine has no file table yet -- W5 -- and
     * 0xFF is what DOS leaves in an unused slot, so a program that guesses
     * a handle is told it is invalid rather than handed somebody else's. */
    for (unsigned i = 0; i < VM86_PSP_JFT_ENTRIES; i++)
        put8(mem, psp + VM86_PSP_JFT + i, VM86_PSP_JFT_FREE);

    put16(mem, psp + VM86_PSP_ENVIRONMENT, start->environment);

    /* Saved SS:SP of the last INT 21h, which has not happened yet. Zero is
     * not a value DOS would leave here, but nothing reads it until a
     * function has been called, and the reference records that what DOS
     * leaves at load time was not found. */
    put32(mem, psp + VM86_PSP_SAVED_SS_SP, 0u);

    put16(mem, psp + VM86_PSP_JFT_SIZE, VM86_PSP_JFT_ENTRIES);

    /* A far pointer, so the offset and the segment are two halves of one
     * dword rather than one linear address. They happen to look the same
     * for everything else in this file, and this is the field where they
     * do not. */
    put32(mem, psp + VM86_PSP_JFT_POINTER,
          VM86_PSP_JFT | ((uint32_t)start->segment << 16));

    /* No previous PSP. See the header on `parent` for what this machine
     * does about a process that has no parent. */
    put32(mem, psp + VM86_PSP_PREVIOUS, 0u);

    put16(mem, psp + VM86_PSP_VERSION, VM86_DOS_VERSION);

    /* INT 21h; RETF -- the entry a program that predates INT 21h knows. */
    put8(mem, psp + VM86_PSP_INT21_RETF, 0xCDu);
    put8(mem, psp + VM86_PSP_INT21_RETF + 1u, 0x21u);
    put8(mem, psp + VM86_PSP_INT21_RETF + 2u, 0xCBu);

    write_fcbs(mem, psp, start->tail);

    put8(mem, psp + VM86_PSP_TAIL_LENGTH, (uint8_t)tail_len);

    /* The tail is copied verbatim and NOTHING is put in front of it. A
     * habit is not a rule: dos-refs-dos.md section 4 has the
     * counterexample, and a loader that inserts the blank it usually sees
     * has adopted somebody else's mistake. */
    for (uint32_t i = 0; i < tail_len; i++)
        put8(mem, psp + VM86_PSP_TAIL + i, (uint8_t)start->tail[i]);

    put8(mem, psp + VM86_PSP_TAIL + tail_len, 0x0Du);

    /* --- The program --- */

    for (uint32_t i = 0; i < image_size; i++)
        vm86_mem_write8(mem, psp + VM86_PSP_BYTES + i, image[i]);

    /* --- The machine --- */

    /*
     * The stack, and the zero word that makes a bare RET an exit.
     *
     * DOS sets SP to zero and pushes, which leaves SP at 0xFFFE with the
     * zero word at SS:FFFE -- the same SP this machine's M3 and M4 corpus
     * convention uses, arrived at the other way round. SP wraps to zero
     * when the RET pops it, and IP becomes zero, and that is PSP:0000.
     */
    put16(mem, psp + 0xFFFEu, 0u);

    vm86_set_seg(cpu, VM86_CS, start->segment);
    vm86_set_seg(cpu, VM86_DS, start->segment);
    vm86_set_seg(cpu, VM86_ES, start->segment);
    vm86_set_seg(cpu, VM86_SS, start->segment);
    vm86_flush_segments(cpu);

    cpu->ip    = VM86_PSP_BYTES;
    cpu->sp    = 0xFFFEu;
    cpu->flags = VM86_FLAG_ALWAYS_SET | VM86_IF;

    cpu->halted = false;

    if (out) {
        out->segment     = start->segment;
        out->memory_top  = memory_top;
        out->environment = start->environment;
        out->paragraphs  = paragraphs;
        out->tail_length = (uint8_t)tail_len;
    }

    return VM86_DOS_LOADED;
}
