/*
 * The run loop: the prefixes it consumes, and the dispatch table it
 * builds out of the five groups.
 *
 * ---------------------------------------------------------------------
 * Why this suite exists
 *
 * step.c is what every instruction passes through, and both of its jobs
 * fail in ways that are invisible one instruction at a time. A prefix
 * that outlives its instruction is correct on the first instruction and
 * wrong on the second. A dispatch table that merged badly still runs --
 * it runs whichever group reached the slot first, which for an opcode
 * two groups claimed is a coin toss nobody can see from the outside.
 *
 * The prefix cases are here rather than in an instruction suite because
 * no single instruction is under test: what is under test is that the
 * state a prefix leaves behind does not survive into what runs next.
 *
 * The merge cases bring their own tables. The real five cannot be made
 * to collide, so the conflict path -- which is the failure mode of
 * splitting an opcode map between several people -- is unreachable any
 * other way. What they exercise is vm86_ops_merge(), which is the same
 * function vm86_ops_build() calls to build the table the machine runs on.
 */
#include "harness.h"

/* ------------------------------------------------------------------ */
/* Prefixes belong to one instruction                                  */
/* ------------------------------------------------------------------ */

static void test_a_prefix_is_gone_by_the_next_instruction(struct vm86_cpu *cpu)
{
    /* es: movsb ; movsb */
    static const uint8_t code[] = { 0x26, 0xA4, 0xA4, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    /*
     * The two source bytes differ from each other and from the two in
     * the data segment, so the second move says which segment it read:
     * with the prefix forgotten it is a plain DS:SI, and with the prefix
     * still in effect it reads ES:SI again. Four distinct bytes, so the
     * result cannot be right by coincidence.
     */
    vm86_set_seg(cpu, VM86_DS, 0x1000);   /* base 0x10000 */
    vm86_set_seg(cpu, VM86_ES, 0x2000);   /* base 0x20000 */
    cpu->si = 0x0010;
    cpu->di = 0x0020;

    vm86_mem_write8(cpu->mem, 0x10010, 0xAA);
    vm86_mem_write8(cpu->mem, 0x10011, 0xCC);
    vm86_mem_write8(cpu->mem, 0x20010, 0xBB);
    vm86_mem_write8(cpu->mem, 0x20011, 0xDD);

    vm86_test_run(cpu, 20);

    vm86_expect_mem8("the prefixed move read ES:SI", cpu, 0x20020, 0xBB);
    vm86_expect_mem8("the plain move read DS:SI",   cpu, 0x20021, 0xCC);
}

static void test_the_last_segment_prefix_wins(struct vm86_cpu *cpu)
{
    /* es: ds: movsb */
    static const uint8_t code[] = { 0x26, 0x3E, 0xA4, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    /*
     * Prefix bytes are state, not a list: each one overwrites what the
     * last one set, so what matters is the one nearest the instruction.
     * A parser that collected them and applied the first would read the
     * other segment, and both are filled.
     */
    vm86_set_seg(cpu, VM86_DS, 0x1000);
    vm86_set_seg(cpu, VM86_ES, 0x2000);
    cpu->si = 0x0010;
    cpu->di = 0x0020;

    vm86_mem_write8(cpu->mem, 0x10010, 0xAA);
    vm86_mem_write8(cpu->mem, 0x20010, 0xBB);

    vm86_test_run(cpu, 10);

    vm86_expect_mem8("the move read DS:SI", cpu, 0x20020, 0xAA);
}

/* ------------------------------------------------------------------ */
/* Merging the groups                                                  */
/* ------------------------------------------------------------------ */

static int         g_reports;
static uint8_t     g_reported_opcode;
static const char *g_reported_first;
static const char *g_reported_second;

static void note_conflict(void *ctx, uint8_t opcode,
                          const char *first, const char *second)
{
    (void)ctx;

    g_reports++;
    g_reported_opcode = opcode;
    g_reported_first  = first;
    g_reported_second = second;
}

static enum vm86_result handler_a(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)cpu;
    (void)opcode;
    return VM86_CONTINUE;
}

static enum vm86_result handler_b(struct vm86_cpu *cpu, uint8_t opcode)
{
    (void)cpu;
    (void)opcode;
    return VM86_CONTINUE;
}

/*
 * Take the reporter over for the duration of one case and hand it back.
 *
 * The harness installs its own before running any test, to report a
 * collision in the real tables -- and a real collision stops the suite
 * before it gets here, so taking the reporter over cannot hide one.
 * What it catches is a collision in the tables this test brings.
 */
static void tests_begin_reporting(void)
{
    g_reports = 0;
    vm86_set_conflict_reporter(note_conflict, NULL);
}

static void tests_stop_reporting(void)
{
    vm86_set_conflict_reporter(NULL, NULL);
}

static void test_a_contested_opcode_is_reported(struct vm86_cpu *cpu)
{
    vm86_op_fn first[256]  = { 0 };
    vm86_op_fn second[256] = { 0 };
    vm86_op_fn merged[256];

    const vm86_op_fn *groups[2] = { first, second };
    const char *names[2] = { "first", "second" };

    (void)cpu;

    first[0x42]  = handler_a;
    second[0x42] = handler_b;

    tests_begin_reporting();
    bool ok = vm86_ops_merge(merged, groups, names, 2);
    tests_stop_reporting();

    vm86_expect_bool("the merge reports a failure", ok, false);
    vm86_expect_bool("the collision was reported once", g_reports == 1, true);
    vm86_expect_u16("the opcode was named", g_reported_opcode, 0x42);
    vm86_expect_bool("so was the group that got there first",
                     g_reported_first == names[0], true);
    vm86_expect_bool("and the group that collided with it",
                     g_reported_second == names[1], true);

    /*
     * The first claim keeps the slot. That is a decision rather than a
     * fact -- the encoding is broken either way -- but it is a decision
     * that has to be made: a merge that left the slot empty would turn
     * a table mistake into an invalid-opcode exception somewhere else.
     */
    vm86_expect_bool("the earlier claim kept the slot",
                     merged[0x42] == handler_a, true);
}

static void test_a_clean_merge_installs_every_claim(struct vm86_cpu *cpu)
{
    vm86_op_fn first[256]  = { 0 };
    vm86_op_fn second[256] = { 0 };
    vm86_op_fn merged[256];

    const vm86_op_fn *groups[2] = { first, second };
    const char *names[2] = { "first", "second" };

    (void)cpu;

    first[0x42]  = handler_a;
    second[0x43] = handler_b;

    tests_begin_reporting();
    bool ok = vm86_ops_merge(merged, groups, names, 2);
    tests_stop_reporting();

    vm86_expect_bool("the merge succeeds", ok, true);
    vm86_expect_bool("and reports nothing", g_reports == 0, true);
    vm86_expect_bool("the first group's claim arrived",
                     merged[0x42] == handler_a, true);
    vm86_expect_bool("the second group's claim arrived too",
                     merged[0x43] == handler_b, true);
    vm86_expect_bool("a slot nobody claims is empty",
                     merged[0x44] == NULL, true);
}

static void test_the_five_groups_do_not_collide(struct vm86_cpu *cpu)
{
    vm86_op_fn merged[256];

    const vm86_op_fn *groups[5] = {
        vm86_ops_alu, vm86_ops_mov, vm86_ops_str, vm86_ops_ctl, vm86_ops_186,
    };
    const char *names[5] = { "alu", "mov", "str", "ctl", "186" };

    (void)cpu;

    /*
     * The property the split was designed around: no two files touch the
     * same opcode. Every other test in the project depends on it -- a
     * collision means some instruction runs a handler written for a
     * different one -- so it is asserted here as well as checked by the
     * harness at startup, and this one says which two collided.
     */
    tests_begin_reporting();
    bool ok = vm86_ops_merge(merged, groups, names, 5);
    tests_stop_reporting();

    vm86_expect_bool("the five groups partition the map", ok, true);
    vm86_expect_bool("with nothing to report", g_reports == 0, true);
}

/* ------------------------------------------------------------------ */

static const struct vm86_test tests[] = {
    { "a prefix does not outlive its instruction",
      test_a_prefix_is_gone_by_the_next_instruction },
    { "the last segment prefix wins",
      test_the_last_segment_prefix_wins },
    { "a contested opcode is reported",
      test_a_contested_opcode_is_reported },
    { "a clean merge installs every claim",
      test_a_clean_merge_installs_every_claim },
    { "the five groups do not collide",
      test_the_five_groups_do_not_collide },
};

VM86_TEST_MAIN("step", tests)
