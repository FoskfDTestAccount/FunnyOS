#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The harness runs on the host, so it can use the host's standard
 * library. The interpreter it is testing cannot, and does not -- that
 * boundary is the point of the arrangement, and it is worth keeping
 * visible: nothing in dos/cpu or dos/mem includes a system header.
 */

static const char *g_test_name;
static int         g_failures_in_test;
static int         g_failures_total;
static int         g_checks_total;

/* ------------------------------------------------------------------ */
/* Running                                                             */
/* ------------------------------------------------------------------ */

void vm86_test_load(struct vm86_cpu *cpu, const uint8_t *code, uint16_t size)
{
    /*
     * A .COM program's world: one 64 KiB segment holding code, data and
     * stack together, entered at offset 0x100 because the first 256 bytes
     * were the program segment prefix. Reproducing that layout costs
     * nothing and means the bytes in a test look like the bytes in a real
     * program, so a test case can be copied out of a disassembly.
     */
    memcpy(cpu->mem->ram + VM86_TEST_CODE_BASE, code, size);

    vm86_set_seg(cpu, VM86_CS, 0);
    vm86_set_seg(cpu, VM86_DS, 0);
    vm86_set_seg(cpu, VM86_ES, 0);
    vm86_set_seg(cpu, VM86_SS, 0);
    vm86_flush_segments(cpu);

    cpu->ip = VM86_TEST_CODE_BASE;
    cpu->sp = VM86_TEST_STACK_TOP;
}

enum vm86_result vm86_test_run(struct vm86_cpu *cpu, uint64_t limit)
{
    for (uint64_t i = 0; i < limit; i++) {
        enum vm86_result result = vm86_step(cpu);

        if (result != VM86_CONTINUE)
            return result;
    }

    /*
     * Out of budget. Reported as a failure by the caller looking at what
     * it expected, which is the right place: the harness does not know
     * whether the test wanted a halt or a fault.
     */
    return VM86_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Assertion plumbing                                                  */
/* ------------------------------------------------------------------ */

static void report_failure(const char *what, const char *detail)
{
    printf("      FAIL  %s: %s\n", what, detail);
    g_failures_in_test++;
    g_failures_total++;
}

static void count_check(void)
{
    g_checks_total++;
}

/* ------------------------------------------------------------------ */
/* Assertions                                                          */
/* ------------------------------------------------------------------ */

void vm86_expect_u16(const char *what, uint16_t got, uint16_t want)
{
    char detail[128];

    count_check();

    if (got == want)
        return;

    snprintf(detail, sizeof(detail), "expected 0x%04X, got 0x%04X", want, got);
    report_failure(what, detail);
}

void vm86_expect_bool(const char *what, bool got, bool want)
{
    count_check();

    if (got == want)
        return;

    report_failure(what, want ? "expected true, got false"
                              : "expected false, got true");
}

void vm86_expect_flag(const char *what, const struct vm86_cpu *cpu,
                      uint16_t flag, bool want)
{
    char detail[128];

    count_check();

    bool got = (cpu->flags & flag) != 0;
    if (got == want)
        return;

    snprintf(detail, sizeof(detail), "%s should be %s and is %s",
             flag & VM86_CF ? "CF" : flag & VM86_ZF ? "ZF" :
             flag & VM86_SF ? "SF" : flag & VM86_OF ? "OF" :
             flag & VM86_AF ? "AF" : flag & VM86_PF ? "PF" :
             flag & VM86_DF ? "DF" : flag & VM86_IF ? "IF" : "flag",
             want ? "set" : "clear", got ? "set" : "clear");
    report_failure(what, detail);
}

void vm86_expect_flags(const char *what, const struct vm86_cpu *cpu,
                       uint16_t expected, uint16_t ignore)
{
    char detail[160];

    count_check();

    /*
     * The bits that always read as one are not the guest's to control, so
     * they are masked out before comparing rather than being written into
     * everyone's expected value.
     */
    uint16_t mask = (uint16_t)(VM86_FLAG_MASK & ~ignore);

    uint16_t got = (uint16_t)(cpu->flags & mask);
    uint16_t want = (uint16_t)(expected & mask);

    if (got == want)
        return;

    snprintf(detail, sizeof(detail),
             "flags expected 0x%04X, got 0x%04X (mask 0x%04X)",
             want, got, mask);
    report_failure(what, detail);
}

uint8_t vm86_expect_mem8(const char *what, struct vm86_cpu *cpu,
                         uint32_t linear, uint8_t want)
{
    char detail[128];

    count_check();

    uint8_t got = vm86_mem_read8(cpu->mem, linear);
    if (got == want)
        return got;

    snprintf(detail, sizeof(detail),
             "at 0x%05X expected 0x%02X, got 0x%02X", linear, want, got);
    report_failure(what, detail);

    return got;
}

uint16_t vm86_expect_mem16(const char *what, struct vm86_cpu *cpu,
                           uint32_t linear, uint16_t want)
{
    char detail[128];

    count_check();

    uint16_t got = vm86_mem_read16(cpu->mem, linear);
    if (got == want)
        return got;

    snprintf(detail, sizeof(detail),
             "at 0x%05X expected 0x%04X, got 0x%04X", linear, want, got);
    report_failure(what, detail);

    return got;
}

/* ------------------------------------------------------------------ */
/* The runner                                                          */
/* ------------------------------------------------------------------ */

int vm86_test_main(const char *suite, const struct vm86_test *tests,
                   size_t count)
{
    uint8_t *memory = calloc(1, VM86_TEST_MEMORY);

    if (!memory) {
        printf("%s: cannot allocate guest memory\n", suite);
        return 2;
    }

    /*
     * Check the dispatch table before running anything.
     *
     * If two opcode groups claim the same byte, every test for that
     * instruction fails for a reason that has nothing to do with the
     * instruction, and debugging it from the failures is miserable. So it
     * is checked first and reported as its own thing.
     */
    if (!vm86_ops_build()) {
        printf("%s: two opcode groups claim the same opcode\n", suite);
        printf("      the conflict reporter above says which\n");
        printf("      every test in every suite is unreliable until that "
               "is fixed\n");
        free(memory);
        return 2;
    }

    printf("\n=== %s: %zu cases ===\n", suite, count);

    for (size_t i = 0; i < count; i++) {
        struct vm86_mem mem;
        struct vm86_cpu cpu;

        vm86_mem_attach(&mem, memory, VM86_TEST_MEMORY);
        vm86_mem_clear(&mem);

        vm86_reset(&cpu, &mem);

        g_test_name         = tests[i].name;
        g_failures_in_test  = 0;

        tests[i].fn(&cpu);

        if (g_failures_in_test == 0)
            printf("  ok  %s\n", g_test_name);
        else
            printf("  --  %s (%d failed)\n", g_test_name, g_failures_in_test);
    }

    printf("=== %s: %d check(s), %d failure(s) ===\n",
           suite, g_checks_total, g_failures_total);

    free(memory);

    return g_failures_total == 0 ? 0 : 1;
}
