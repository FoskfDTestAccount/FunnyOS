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
 * The table cases install their own groups, through the function the
 * machine itself builds its table with. The five real groups cannot be
 * made to collide, so the failure this file is most about -- two groups
 * claiming one opcode, which is the characteristic failure of splitting
 * an opcode map between several people -- cannot be produced from them at
 * all. Each case puts the machine's own table back before it returns,
 * because the next case runs on it.
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
/* Building the dispatch table                                         */
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
 * before it gets here, so taking the reporter over cannot hide one. What
 * it catches is a collision in the tables these cases bring.
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

    const vm86_op_fn *groups[2] = { first, second };
    const char *names[2] = { "first", "second" };

    (void)cpu;

    first[0x42]  = handler_a;
    second[0x42] = handler_b;

    tests_begin_reporting();
    bool ok = vm86_ops_build_from(groups, names, 2);
    tests_stop_reporting();

    const vm86_op_fn *table = vm86_ops_table();

    vm86_ops_build();   /* the machine's own table, for the cases after this */

    vm86_expect_bool("the build reports a failure", ok, false);
    vm86_expect_bool("the collision was reported once", g_reports == 1, true);
    vm86_expect_u16("the opcode was named", g_reported_opcode, 0x42);
    vm86_expect_bool("so was the group that got there first",
                     g_reported_first == names[0], true);
    vm86_expect_bool("and the group that collided with it",
                     g_reported_second == names[1], true);

    /*
     * The table the run loop would have dispatched through held the first
     * claim in the contested slot -- a decision rather than a fact, since
     * the encoding is broken either way, but a decision that had to be
     * made. vm86_ops_table() refuses to hand it over: a caller that asked
     * for a table and got a broken one is better served by NULL than by
     * something that looks usable.
     */
    vm86_expect_bool("a table that did not merge is not handed out",
                     table == NULL, true);
}

static void test_a_clean_merge_installs_every_claim(struct vm86_cpu *cpu)
{
    vm86_op_fn first[256]  = { 0 };
    vm86_op_fn second[256] = { 0 };

    const vm86_op_fn *groups[2] = { first, second };
    const char *names[2] = { "first", "second" };

    (void)cpu;

    first[0x42]  = handler_a;
    second[0x43] = handler_b;

    tests_begin_reporting();
    bool ok = vm86_ops_build_from(groups, names, 2);
    tests_stop_reporting();

    /*
     * The slots are read out before the machine's own table is put back.
     * vm86_ops_table() returns a pointer into the table itself and a
     * build writes through that pointer, so anything read through it has
     * to be read before the next call -- otherwise the pointer quietly
     * starts showing handlers this case never installed, which is how
     * this case failed the first time it was written.
     */
    const vm86_op_fn *table = vm86_ops_table();
    vm86_op_fn claim_a = table ? table[0x42] : NULL;
    vm86_op_fn claim_b = table ? table[0x43] : NULL;
    vm86_op_fn empty   = table ? table[0x44] : NULL;

    vm86_ops_build();   /* the machine's own table, for the cases after this */

    vm86_expect_bool("the build succeeds", ok, true);
    vm86_expect_bool("and reports nothing", g_reports == 0, true);
    vm86_expect_bool("the table is available", table != NULL, true);
    vm86_expect_bool("the first group's claim arrived",
                     claim_a == handler_a, true);
    vm86_expect_bool("the second group's claim arrived too",
                     claim_b == handler_b, true);
    vm86_expect_bool("a slot nobody claims is empty", empty == NULL, true);
}

static void test_a_table_that_did_not_merge_is_not_a_halt(struct vm86_cpu *cpu)
{
    /* mov ax,1234h ; hlt -- an ordinary program, which is the point: the
     * machine refuses to run it rather than running it through a table it
     * could not build. */
    static const uint8_t code[] = { 0xB8, 0x34, 0x12, 0xF4 };

    vm86_op_fn first[256]  = { 0 };
    vm86_op_fn second[256] = { 0 };

    const vm86_op_fn *groups[2] = { first, second };
    const char *names[2] = { "first", "second" };

    vm86_test_load(cpu, code, sizeof(code));

    first[0xB8]  = handler_a;
    second[0xB8] = handler_b;

    tests_begin_reporting();
    bool built = vm86_ops_build_from(groups, names, 2);
    tests_stop_reporting();

    vm86_expect_bool("the table did not merge", built, false);

    enum vm86_result result = vm86_step(cpu);

    vm86_ops_build();   /* the machine's own table, for the cases after this */

    vm86_expect_bool("stepping refuses to dispatch",
                     result == VM86_INTERNAL_ERROR, true);

    /*
     * The whole reason this case exists.
     *
     * A caller reads VM86_HALT as "the program finished", and the
     * integration's acceptance test reads it exactly that way -- it
     * checks that the guest got there by halting. So a table failure that
     * answered VM86_HALT would let a broken emulator pass that test,
     * which is worse than any of the ways it could fail loudly. The two
     * values have to stay different, and this assertion is what says so.
     */
    vm86_expect_bool("and it is not a halt", result != VM86_HALT, true);
}

static void test_the_five_groups_do_not_collide(struct vm86_cpu *cpu)
{
    (void)cpu;

    /*
     * The property the split was designed around: no two files touch the
     * same opcode. Every other test in the project depends on it -- a
     * collision means some instruction runs a handler written for a
     * different one -- so it is asserted here as well as checked by the
     * harness at startup, and this one can say which two collided.
     */
    tests_begin_reporting();
    bool ok = vm86_ops_build();
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
    { "a table that did not merge is not a halt",
      test_a_table_that_did_not_merge_is_not_a_halt },
    { "the five groups do not collide",
      test_the_five_groups_do_not_collide },
};

VM86_TEST_MAIN("step", tests)
