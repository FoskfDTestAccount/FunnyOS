/*
 * The corpus suite: run each assembled sample and grade the machine.
 *
 * ---------------------------------------------------------------------
 * What this suite is for, and what it is not
 *
 * Every other suite in this directory measures one instruction. This one
 * runs programs, because a whole class of mistakes only exists between
 * instructions: a carry an ADC inherits from the instruction before it, a
 * REP whose count is zero, a prefix that belongs to the instruction after
 * it, a loop whose exit test reads a flag that a shift by zero did not
 * write. None of those can be seen from a single step, and all of them
 * are what the milestone's acceptance sentence is actually about -- it
 * says the emulator executes a *binary*, and a binary is a program.
 *
 * The expected states below were worked out from the manual and written
 * down before the samples were run. Where an expectation and the machine
 * disagreed the reading of the manual was checked again, and the outcome
 * is recorded in the sample that raised it and in the report that came
 * with this work. The rule the assignment gives is the right one, and
 * worth restating because it is the whole value of doing this at all: an
 * expectation edited until it matches the machine proves nothing, since
 * the machine is what it would have been derived from.
 *
 * ---------------------------------------------------------------------
 * How the samples get here
 *
 * dos/Makefile assembles the corpus sources with nasm and turns each one
 * into a C array with tools/bin2c.py, the same way the kernel embeds its
 * init program. So the suite has no files to find at run time, no path to
 * be told, and nothing to clean up: the programs are in the binary.
 *
 * That is also the shape the samples need for the second half of their
 * job. dos/ runs on the host today and inside a guest later, and a guest
 * has no filesystem; a sample that arrives as bytes in memory arrives the
 * same way in both places. Only this driver is host-only -- the printf
 * lives here, and nowhere in corpus/replay.c.
 */
#include "harness.h"

#include <stdio.h>
#include <stdlib.h>

#include "../corpus/replay.h"

/* ---------------------------------------------------------------- */
/* The images                                                        */
/* ---------------------------------------------------------------- */

/*
 * One pair per sample in dos/corpus, emitted by the build -- the bytes
 * and their length. The names are the file names, so which source file a
 * failing expectation belongs to is never a question.
 */
#define CORPUS_IMAGE(sym)                        \
    extern const unsigned char sym[];            \
    extern const unsigned long sym##_size;

CORPUS_IMAGE(corpus_adc_sbb_chain)
CORPUS_IMAGE(corpus_chip_lea)
CORPUS_IMAGE(corpus_chip_movcs)
CORPUS_IMAGE(corpus_chip_pushsp)
CORPUS_IMAGE(corpus_chip_segb)
CORPUS_IMAGE(corpus_chip_shift_count)
CORPUS_IMAGE(corpus_ext_enter)
CORPUS_IMAGE(corpus_ext_imul)
CORPUS_IMAGE(corpus_ext_pusha)
CORPUS_IMAGE(corpus_ext_shifts)
CORPUS_IMAGE(corpus_illegal_ff7)
CORPUS_IMAGE(corpus_rep_movs)
CORPUS_IMAGE(corpus_rep_zero)
CORPUS_IMAGE(corpus_repne_scasb)
CORPUS_IMAGE(corpus_seg_override)
CORPUS_IMAGE(corpus_seg_override_str)
CORPUS_IMAGE(corpus_shift_loop)

/* ---------------------------------------------------------------- */
/* Memory the samples are graded on                                  */
/* ---------------------------------------------------------------- */

/*
 * Named and declared outside the table rather than inlined, because
 * several cases want more than one run of bytes and a compound literal
 * inside a struct literal is harder to read than a name.
 *
 * Each one is a place a decoder or a careless loop would have put
 * something else, so that "these bytes are still here" is an assertion
 * rather than a tautology about memory nobody was going to write.
 */
static const struct corpus_memory movs_destination[] = {
    { 0x0000, 0x0300, 5, { 0x11, 0x22, 0x33, 0x44, 0x55 } },
};
static const struct corpus_memory scas_haystack[] = {
    /* SCAS compares; it must not have written to its own input. */
    { 0x0000, 0x0102, 5, { 0x11, 0x22, 0x33, 0x44, 0x55 } },
};
static const struct corpus_memory zero_run_destination[] = {
    /* Zero length: the destination is where it started -- zero, even
     * though the source held 0xABCD for a copy to carry away. */
    { 0x3000, 0x0300, 4, { 0x00, 0x00, 0x00, 0x00 } },
    /* And the read did not write: the source is still there. */
    { 0x2000, 0x0200, 2, { 0xAB, 0xCD } },
};
static const struct corpus_memory override_words[] = {
    { 0x2000, 0x0010, 2, { 0x11, 0x11 } },
    { 0x3000, 0x0010, 2, { 0x22, 0x22 } },
};
static const struct corpus_memory override_string[] = {
    /* DS:0x20 is the decoy at the destination's offset in the segment
     * the write must never reach. It is still 0xDD. */
    { 0x4000, 0x0010, 1, { 0xEE } },
    { 0x4000, 0x0020, 1, { 0xDD } },
    { 0x3000, 0x0010, 1, { 0xAA } },
    { 0x3000, 0x0020, 1, { 0xAA } },
};
static const struct corpus_memory pusha_stack[] = {
    { 0x0000, 0xFFF4, 2, { 0xFE, 0xFF } },   /* the SP PUSHA saved */
    { 0x0000, 0xFFEE, 2, { 0x66, 0x66 } },   /* DI, pushed last */
};
static const struct corpus_memory enter_frame[] = {
    { 0x0000, 0xFFF8, 2, { 0xFC, 0xFF } },   /* the frame pointer */
    { 0x0000, 0xFFFA, 2, { 0xCD, 0xAB } },   /* the chained frame */
    { 0x0000, 0xFFFC, 2, { 0x00, 0x10 } },   /* the BP ENTER saved */
};
static const struct corpus_memory pushsp_word[] = {
    { 0x0000, 0x7FFE, 2, { 0xFE, 0x7F } },
};
static const struct corpus_memory segbase_bytes[] = {
    { 0x2000, 0x0000, 1, { 0x22 } },
    { 0x3000, 0x0000, 1, { 0x44 } },
    { 0x0000, 0x0000, 1, { 0x33 } },
};

/* ---------------------------------------------------------------- */
/* Reporting                                                         */
/* ---------------------------------------------------------------- */

/*
 * The sink the replayer writes its differences to.
 *
 * This is the half of the arrangement that is allowed to print: the
 * replayer hands over text and the driver decides what to do with it,
 * which in a host test is to put the sample's name in front of it. A
 * guest would pass a sink that writes to the console, and the replayer
 * would not change.
 */
struct sink_state {
    const char *name;
    int        *differences;
};

static void report_difference(void *ctx, const char *text, size_t length)
{
    struct sink_state *state = ctx;

    printf("      FAIL  %s: %.*s\n", state->name, (int)length, text);
    (*state->differences)++;
}

/*
 * The kind of failure, said in the summary line as well as in the sink.
 *
 * A run that hit the instruction limit and a run that produced the wrong
 * answer are different problems, and a summary that calls both of them
 * "failed" sends the reader after arithmetic that is not there. The
 * replayer already keeps them apart; this makes the distinction visible
 * without reading the detail.
 */
static const char *outcome_name(enum corpus_outcome outcome)
{
    switch (outcome) {
    case CORPUS_PASS:         return "pass";
    case CORPUS_MISMATCH:     return "state differs";
    case CORPUS_WRONG_ENDING: return "stopped the wrong way";
    case CORPUS_WRONG_FAULT:  return "wrong fault vector";
    case CORPUS_INSN_LIMIT:   return "instruction limit";
    }

    return "?";
}

/*
 * Two opcode groups claiming one opcode makes every result in every suite
 * meaningless, so it is checked before anything runs, exactly as the
 * harness does. Repeated here because this suite runs its own main.
 */
static void report_conflict(void *ctx, uint8_t opcode,
                            const char *first, const char *second)
{
    (void)ctx;

    printf("  !     opcode %02X is claimed by both %s and %s\n",
           opcode, first, second);
}

/* ---------------------------------------------------------------- */
/* The runner                                                        */
/* ---------------------------------------------------------------- */

static bool run_case(struct vm86_mem *mem, struct corpus_case c,
                     int *differences)
{
    struct sink_state  sink = { c.name, differences };
    enum corpus_outcome outcome;

    /*
     * Every sample finishes by halting on its last byte, so the
     * instruction pointer it stops with is a function of the image: one
     * past the end. Filling it in here rather than writing it into
     * seventeen literals keeps a number nobody can check by eye out of
     * the expectations -- and it is the one expectation that would have
     * to be rewritten every time anybody touched a sample above it.
     *
     * A sample whose HLT is not its last byte would break this, which is
     * why it is a rule rather than a convenience: the convention for a
     * corpus sample is that it ends with the instruction it stops on.
     */
    c.final.ip = (uint16_t)(VM86_CORPUS_LOAD_OFFSET + c.image_size);

    outcome = vm86_corpus_replay(mem, &c, report_difference, &sink);

    if (outcome == CORPUS_PASS) {
        printf("  ok  %s\n", c.name);
        return true;
    }

    printf("  --  %s (%s)\n", c.name, outcome_name(outcome));
    return false;
}

int main(void)
{
    /*
     * The expectations.
     *
     * A local rather than a file-scope table because the image lengths
     * come from the build as objects, not as constants, and only an
     * automatic initialiser may use them. The alternative -- naming each
     * length twice, once here and once in a parallel array -- is a second
     * place for the two to drift apart, and this file is nothing but the
     * expectations: a table that no longer matches its samples is the one
     * bug it cannot check for itself.
     *
     * `ip` is left out on purpose; see run_case(). Everything else a
     * sample does not name is zero, which is what it started at.
     */
    const struct corpus_case cases[] = {
        {
            .name  = "adc_sbb_chain",
            .image = corpus_adc_sbb_chain,
            .image_size = (uint16_t)corpus_adc_sbb_chain_size,
            .ending = CORPUS_ENDS_HALTED,
            .final = {
                .ax = 0xFFFF, .bx = 0xFFFF,
                .si = 0x0001,
                .sp = VM86_CORPUS_STACK_TOP,
                .flags = VM86_ZF | VM86_PF,
            },
        },
        {
            .name  = "shift_loop",
            .image = corpus_shift_loop,
            .image_size = (uint16_t)corpus_shift_loop_size,
            .ending = CORPUS_ENDS_HALTED,
            .final = {
                .ax = 0x1234, .dx = 0x0001,
                .bp = 0x0010,
                .sp = VM86_CORPUS_STACK_TOP,
                .flags = VM86_ZF | VM86_PF,
            },
            /* The XOR wrote the flags last, and leaves AF undefined. */
            .flags_ignore = VM86_AF,
        },
        {
            .name  = "rep_movs",
            .image = corpus_rep_movs,
            .image_size = (uint16_t)corpus_rep_movs_size,
            .ending = CORPUS_ENDS_HALTED,
            .final = {
                .si = 0x0107, .di = 0x0305,
                .sp = VM86_CORPUS_STACK_TOP,
                .flags = 0,
            },
            .memory = movs_destination,
            .memory_count = 1,
        },
        {
            .name  = "repne_scasb",
            .image = corpus_repne_scasb,
            .image_size = (uint16_t)corpus_repne_scasb_size,
            .ending = CORPUS_ENDS_HALTED,
            .final = {
                .ax = 0x0033, .cx = 0x0002, .di = 0x0105,
                .sp = VM86_CORPUS_STACK_TOP,
                .flags = VM86_ZF | VM86_PF,
            },
            .memory = scas_haystack,
            .memory_count = 1,
        },
        {
            .name  = "rep_zero",
            .image = corpus_rep_zero,
            .image_size = (uint16_t)corpus_rep_zero_size,
            .ending = CORPUS_ENDS_HALTED,
            .final = {
                .ax = 0xFFFF,
                .si = 0x0200, .di = 0x0300,
                .sp = VM86_CORPUS_STACK_TOP,
                .ds = 0x2000, .es = 0x3000,
                .flags = VM86_CF | VM86_AF | VM86_SF | VM86_PF,
            },
            .memory = zero_run_destination,
            .memory_count = 2,
        },
        {
            .name  = "seg_override",
            .image = corpus_seg_override,
            .image_size = (uint16_t)corpus_seg_override_size,
            .ending = CORPUS_ENDS_HALTED,
            .final = {
                .ax = 0x3000, .bx = 0x1111, .dx = 0x2222,
                .sp = VM86_CORPUS_STACK_TOP,
                .ds = 0x2000, .es = 0x3000,
                .flags = 0,
            },
            .memory = override_words,
            .memory_count = 2,
        },
        {
            .name  = "seg_override_str",
            .image = corpus_seg_override_str,
            .image_size = (uint16_t)corpus_seg_override_str_size,
            .ending = CORPUS_ENDS_HALTED,
            .final = {
                .ax = 0x3000,
                .si = 0x0011, .di = 0x0021,
                .sp = VM86_CORPUS_STACK_TOP,
                .ds = 0x4000, .es = 0x3000,
                .flags = 0,
            },
            .memory = override_string,
            .memory_count = 4,
        },
        {
            .name  = "ext_pusha",
            .image = corpus_ext_pusha,
            .image_size = (uint16_t)corpus_ext_pusha_size,
            .ending = CORPUS_ENDS_HALTED,
            .final = {
                .ax = 0xFFFE, .bx = 0x2222, .cx = 0x3333, .dx = 0x4444,
                .si = 0x5555, .di = 0x6666,
                .bp = 0xFFEE, .sp = 0xFFEE,
                .flags = VM86_ZF | VM86_PF,
            },
            /* The XOR wrote the flags last, as in shift_loop. */
            .flags_ignore = VM86_AF,
            .memory = pusha_stack,
            .memory_count = 2,
        },
        {
            .name  = "ext_enter",
            .image = corpus_ext_enter,
            .image_size = (uint16_t)corpus_ext_enter_size,
            .ending = CORPUS_ENDS_HALTED,
            .final = {
                .ax = 0x2000,
                .bp = 0x1000,
                .sp = VM86_CORPUS_STACK_TOP,
                .ds = 0x2000,
                .flags = 0,
            },
            .memory = enter_frame,
            .memory_count = 3,
        },
        {
            .name  = "ext_imul",
            .image = corpus_ext_imul,
            .image_size = (uint16_t)corpus_ext_imul_size,
            .ending = CORPUS_ENDS_HALTED,
            .final = {
                .ax = 0x0002, .bx = 0x0006, .cx = 0x0100,
                .si = 0x0001,
                .bp = 0xFFFA,
                .sp = VM86_CORPUS_STACK_TOP,
                .flags = 0,             /* CF and OF clear: it fitted */
            },
            /* IMUL defines CF and OF and leaves the rest undefined. */
            .flags_ignore = VM86_ZF | VM86_SF | VM86_AF | VM86_PF,
        },
        {
            .name  = "ext_shifts",
            .image = corpus_ext_shifts,
            .image_size = (uint16_t)corpus_ext_shifts_size,
            .ending = CORPUS_ENDS_HALTED,
            .final = {
                .bx = 0x0001,
                .si = 0xFFFF,
                .sp = VM86_CORPUS_STACK_TOP,
                .flags = VM86_SF | VM86_PF,
            },
            /* AF after any shift; OF because the count is not one. */
            .flags_ignore = VM86_AF | VM86_OF,
        },
        {
            .name  = "chip_pushsp",
            .image = corpus_chip_pushsp,
            .image_size = (uint16_t)corpus_chip_pushsp_size,
            .ending = CORPUS_ENDS_HALTED,
            .final = {
                .bx = 0x7FFE,
                .sp = 0x8000,
                .flags = 0,
            },
            .memory = pushsp_word,
            .memory_count = 1,
        },
        {
            .name  = "chip_lea",
            .image = corpus_chip_lea,
            .image_size = (uint16_t)corpus_chip_lea_size,
            .ending = CORPUS_ENDS_HALTED,
            .final = {
                .ax = 0x1234, .bx = 0x0010, .dx = 0x0023,
                .si = 0x0020, .di = 0x0030,
                .bp = 0x0010,
                .sp = VM86_CORPUS_STACK_TOP,
                .es = 0x1234,
                .flags = 0,
            },
        },
        {
            .name  = "chip_segb",
            .image = corpus_chip_segb,
            .image_size = (uint16_t)corpus_chip_segb_size,
            .ending = CORPUS_ENDS_HALTED,
            .final = {
                .ax = 0x3000, .bx = 0x4422,
                .sp = VM86_CORPUS_STACK_TOP,
                .ds = 0x2000, .ss = 0x3000,
                .flags = 0,
            },
            .memory = segbase_bytes,
            .memory_count = 3,
        },
        {
            .name  = "chip_shift_count",
            .image = corpus_chip_shift_count,
            .image_size = (uint16_t)corpus_chip_shift_count_size,
            .ending = CORPUS_ENDS_HALTED,
            .final = {
                .cx = 0x0021,
                .sp = VM86_CORPUS_STACK_TOP,
                .flags = VM86_ZF | VM86_PF,
            },
            /* AF after any shift; OF because the counts are not one. */
            .flags_ignore = VM86_AF | VM86_OF,
        },
        {
            /* See corpus/chip_movcs.asm: this one records a decision the
             * project made, not what the 8086's silicon does. */
            .name   = "chip_movcs",
            .image  = corpus_chip_movcs,
            .image_size = (uint16_t)corpus_chip_movcs_size,
            .ending = CORPUS_ENDS_FAULTED,
            .fault  = VM86_VECTOR_INVALID_OPCODE,
        },
        {
            .name   = "illegal_ff7",
            .image  = corpus_illegal_ff7,
            .image_size = (uint16_t)corpus_illegal_ff7_size,
            .ending = CORPUS_ENDS_FAULTED,
            .fault  = VM86_VECTOR_INVALID_OPCODE,
        },
    };

    const size_t cases_count = sizeof(cases) / sizeof(cases[0]);
    uint8_t     *guest_memory;
    struct vm86_mem mem;
    int          differences = 0;
    int          failed      = 0;

    vm86_set_conflict_reporter(report_conflict, NULL);

    if (!vm86_ops_build()) {
        printf("corpus: two opcode groups claim the same opcode\n");
        printf("      the conflict reporter above says which\n");
        printf("      every test in every suite is unreliable until that "
               "is fixed\n");
        return 2;
    }

    guest_memory = calloc(1, VM86_TEST_MEMORY);
    if (!guest_memory) {
        printf("corpus: cannot allocate guest memory\n");
        return 2;
    }

    vm86_mem_attach(&mem, guest_memory, VM86_TEST_MEMORY);

    printf("\n=== corpus: %zu cases ===\n", cases_count);

    for (size_t i = 0; i < cases_count; i++) {
        if (!run_case(&mem, cases[i], &differences))
            failed++;
    }

    printf("=== corpus: %zu case(s), %d difference(s), %d failed ===\n",
           cases_count, differences, failed);

    free(guest_memory);

    return failed == 0 ? 0 : 1;
}
