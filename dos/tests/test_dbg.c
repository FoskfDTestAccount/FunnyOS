/*
 * The single-step debugger.
 *
 * The interpreter has its own suites; what is tested here is what the
 * debugger adds on top of it, and the two things it could get wrong
 * without anyone noticing:
 *
 *   - the length of an instruction, which is decoded rather than measured
 *     and therefore can disagree with the emulator it is describing;
 *   - the text, which is either faithful to the machine or quietly
 *     helpful in a way that hides the very thing it was asked to show.
 *
 * The sweep in the middle is the one that matters. Everything else is a
 * spot check.
 */
#include "harness.h"

#include <stdio.h>
#include <string.h>

#include <vm86/dbg.h>

/* ------------------------------------------------------------------ */
/* Catching the output                                                 */
/* ------------------------------------------------------------------ */

static char   g_text[8192];
static size_t g_len;
static int    g_writes;
static bool   g_nul_inside;

static void capture(void *ctx, const char *text, size_t length)
{
    (void)ctx;

    g_writes++;

    /*
     * The sink is told how long the text is rather than left to work it
     * out, so a NUL inside a chunk would mean the length and the contents
     * disagree about where the text ends -- and the host build would
     * truncate where the kernel build would not.
     */
    if (memchr(text, '\0', length) != NULL)
        g_nul_inside = true;

    if (g_len + length + 1u >= sizeof(g_text))
        return;

    memcpy(g_text + g_len, text, length);
    g_len += length;
    g_text[g_len] = '\0';
}

static struct vm86_dbg_out make_sink(void)
{
    struct vm86_dbg_out out = { capture, NULL };

    g_text[0]    = '\0';
    g_len        = 0;
    g_writes     = 0;
    g_nul_inside = false;

    return out;
}

static void expect_text(const char *what, const char *needle)
{
    bool found = strstr(g_text, needle) != NULL;

    if (!found)
        printf("      (no \"%s\" in:\n%s      )\n", needle, g_text);

    vm86_expect_bool(what, found, true);
}

static void expect_no_text(const char *what, const char *needle)
{
    bool found = strstr(g_text, needle) != NULL;

    if (found)
        printf("      (\"%s\" should not be in:\n%s      )\n", needle, g_text);

    vm86_expect_bool(what, !found, true);
}

/* ------------------------------------------------------------------ */
/* Stepping                                                            */
/* ------------------------------------------------------------------ */

static void test_a_step_executes_exactly_one_instruction(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x01, 0xD8, 0xF4 };   /* add ax,bx ; hlt */
    struct vm86_dbg_trace trace;

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ax = 0x1000;
    cpu->bx = 0x0234;

    enum vm86_result result = vm86_dbg_step(cpu, &trace);

    vm86_expect_bool("it continued", result == VM86_CONTINUE, true);
    vm86_expect_bool("and the record says so", trace.result == VM86_CONTINUE, true);

    vm86_expect_u16("cs before", trace.cs, 0x0000);
    vm86_expect_u16("ip before", trace.ip, VM86_TEST_CODE_BASE);
    vm86_expect_u16("length", trace.length, 2);
    vm86_expect_u16("prefix bytes", trace.prefixes, 0);
    vm86_expect_u16("bytes recorded", trace.shown, 2);
    vm86_expect_u16("first byte", trace.bytes[0], 0x01);
    vm86_expect_u16("second byte", trace.bytes[1], 0xD8);

    vm86_expect_u16("ax after", trace.after.ax, 0x1234);
    vm86_expect_u16("bx unchanged", trace.after.bx, 0x0234);
    vm86_expect_u16("ip after", cpu->ip, VM86_TEST_CODE_BASE + 2);

    vm86_expect_bool("ip moved by the decoded length", trace.ip_advance_ok, true);
    vm86_expect_bool("it is not a control transfer", trace.transfers_control, false);
}

/*
 * A repeated string instruction with a count of zero.
 *
 * The interesting question is what the instruction pointer does, and the
 * answer is that it moves by two -- the prefix and the opcode, which were
 * fetched before the loop ever looked at CX. The loop body runs zero
 * times, so SI, DI and the destination byte are all untouched.
 *
 * This is the case the task book expected to be the counter-example to
 * "length by subtraction" -- an instruction whose pointer "does not
 * move". It is not one: on this emulator, and on the hardware, IP ends up
 * past the whole instruction. The counter-examples are the jump that
 * lands on itself and the instruction that faults, and those have their
 * own tests below. The claim is worth pinning down here rather than in
 * prose, because the version of it that is wrong is the version that
 * would be argued from in a review.
 */
static void test_rep_movsb_with_cx_zero_still_consumes_the_instruction(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0xA4, 0xF4 };   /* rep movsb ; hlt */
    struct vm86_dbg_trace trace;

    vm86_test_load(cpu, code, sizeof(code));
    cpu->cx   = 0;
    cpu->si   = 0x0200;
    cpu->di   = 0x0300;
    vm86_mem_write8(cpu->mem, 0x0200, 0x5A);

    vm86_dbg_step(cpu, &trace);

    vm86_expect_u16("ip after", cpu->ip, VM86_TEST_CODE_BASE + 2);
    vm86_expect_u16("length", trace.length, 2);
    vm86_expect_u16("prefix bytes", trace.prefixes, 1);
    vm86_expect_u16("count", cpu->cx, 0);
    vm86_expect_u16("si", cpu->si, 0x0200);
    vm86_expect_u16("di", cpu->di, 0x0300);
    vm86_expect_mem8("the destination was not written", cpu, 0x0300, 0x00);
    vm86_expect_bool("ip moved by the decoded length", trace.ip_advance_ok, true);
}

static void test_rep_movsb_with_a_count_moves_everything(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF3, 0xA4, 0xF4 };
    struct vm86_dbg_trace trace;

    vm86_test_load(cpu, code, sizeof(code));
    cpu->cx = 3;
    cpu->si = 0x0200;
    cpu->di = 0x0300;
    vm86_mem_write8(cpu->mem, 0x0200, 0x11);
    vm86_mem_write8(cpu->mem, 0x0201, 0x22);
    vm86_mem_write8(cpu->mem, 0x0202, 0x33);

    vm86_dbg_step(cpu, &trace);

    vm86_expect_u16("ip after", cpu->ip, VM86_TEST_CODE_BASE + 2);
    vm86_expect_u16("count", cpu->cx, 0);
    vm86_expect_u16("si", cpu->si, 0x0203);
    vm86_expect_u16("di", cpu->di, 0x0303);
    vm86_expect_mem8("first byte", cpu, 0x0300, 0x11);
    vm86_expect_mem8("third byte", cpu, 0x0302, 0x33);
}

/*
 * The instruction that proves the length cannot be measured.
 *
 * EB FE is a jump of minus two from the end of itself, which is the start
 * of itself. One step executes it and leaves the pointer exactly where it
 * began, so a length worked out as "after minus before" is zero -- and
 * the trace would say the instruction was empty, or, with the subtraction
 * done in unsigned arithmetic, that it was sixty-five thousand bytes
 * long.
 */
static void test_a_jump_to_itself_leaves_the_pointer_where_it_was(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xEB, 0xFE, 0xF4 };   /* jmp $ ; hlt */
    struct vm86_dbg_trace trace;

    vm86_test_load(cpu, code, sizeof(code));

    vm86_dbg_step(cpu, &trace);

    vm86_expect_u16("ip after", cpu->ip, VM86_TEST_CODE_BASE);
    vm86_expect_u16("length", trace.length, 2);
    vm86_expect_u16("bytes recorded", trace.shown, 2);
    vm86_expect_bool("it is a control transfer", trace.transfers_control, true);
    vm86_expect_bool("which is why that is not a disagreement",
                     trace.ip_advance_ok, true);
    vm86_expect_mem16("the bytes are still the jump", cpu, VM86_TEST_CODE_BASE,
                      0xFEEB);
}

/*
 * A backward jump, which is the other half of the same problem: the
 * subtraction is negative, and unsigned it would be enormous.
 */
static void test_a_backward_jump_records_a_shorter_ip(struct vm86_cpu *cpu)
{
    /* At 0102: EB FC, which jumps back four from 0104 to 0100. */
    static const uint8_t code[] = { 0xF4, 0xF4, 0xEB, 0xFC };
    struct vm86_dbg_trace trace;

    vm86_test_load(cpu, code, sizeof(code));
    cpu->ip = VM86_TEST_CODE_BASE + 2;

    vm86_dbg_step(cpu, &trace);

    vm86_expect_u16("ip after", cpu->ip, VM86_TEST_CODE_BASE);
    vm86_expect_u16("ip before", trace.ip, VM86_TEST_CODE_BASE + 2);
    vm86_expect_u16("length", trace.length, 2);
    vm86_expect_bool("the pointer went backwards", cpu->ip < trace.ip, true);
    vm86_expect_bool("and that is not a disagreement", trace.ip_advance_ok, true);
}

/*
 * A faulting instruction.
 *
 * What the record has to hold is the state the machine is actually in,
 * not the state a completed instruction would have left it in. For a
 * division by zero that means the pointer is already past both bytes,
 * because both were fetched before the handler looked at the divisor --
 * a fault is raised after the instruction has been read, not instead of
 * reading it.
 */
static void test_a_fault_records_the_state_the_instruction_left(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF7, 0xF6, 0xF4 };   /* div si ; hlt */
    struct vm86_dbg_trace trace;

    vm86_test_load(cpu, code, sizeof(code));
    cpu->si = 0;
    cpu->ax = 1;
    cpu->dx = 0;

    enum vm86_result result = vm86_dbg_step(cpu, &trace);

    vm86_expect_bool("the step reported a fault", result == VM86_FAULT, true);
    vm86_expect_bool("and so does the record", trace.result == VM86_FAULT, true);
    vm86_expect_u16("the vector", trace.fault, VM86_VECTOR_DIVIDE_ERROR);
    vm86_expect_u16("the machine agrees", cpu->fault, VM86_VECTOR_DIVIDE_ERROR);
    vm86_expect_u16("length", trace.length, 2);
    vm86_expect_u16("ip after", trace.after.ip, VM86_TEST_CODE_BASE + 2);
    vm86_expect_bool("which is a full instruction's worth",
                     trace.ip_advance_ok, true);
}

static void test_an_opcode_nothing_claims_is_one_byte(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF1, 0xF4 };
    struct vm86_dbg_trace trace;

    vm86_test_load(cpu, code, sizeof(code));

    vm86_dbg_step(cpu, &trace);

    vm86_expect_bool("it faulted", trace.result == VM86_FAULT, true);
    vm86_expect_u16("with the invalid opcode vector", trace.fault,
                    VM86_VECTOR_INVALID_OPCODE);
    vm86_expect_u16("length", trace.length, 1);
    vm86_expect_u16("ip after", trace.after.ip, VM86_TEST_CODE_BASE + 1);
}

/* A step has to leave the machine able to be stepped again: the trace
 * must not have moved the pointer, and the instruction must still be
 * there to run. */
static void test_stepping_does_not_disturb_the_machine(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x01, 0xD8, 0xF4 };
    struct vm86_dbg_trace first, second;

    vm86_test_load(cpu, code, sizeof(code));

    vm86_dbg_step(cpu, &first);
    vm86_expect_u16("the first step ran the instruction", cpu->ax, 0x0000);

    cpu->ax = 0x1000;
    cpu->bx = 0x0234;
    cpu->ip = VM86_TEST_CODE_BASE;

    vm86_dbg_step(cpu, &second);

    vm86_expect_u16("the second step ran it too", cpu->ax, 0x1234);
    vm86_expect_u16("and read the same bytes", second.bytes[0], first.bytes[0]);
    vm86_expect_u16("the same length", second.length, first.length);
}

/* ------------------------------------------------------------------ */
/* The sweep: every opcode, decoded against executed                   */
/* ------------------------------------------------------------------ */

static bool prefix_byte(uint8_t byte)
{
    return byte == 0x26 || byte == 0x2E || byte == 0x36 || byte == 0x3E
        || byte == 0xF0 || byte == 0xF2 || byte == 0xF3;
}

static bool control_transfer(uint8_t opcode)
{
    if (opcode >= 0x70 && opcode <= 0x7F) return true;   /* the jumps */
    if (opcode >= 0xE0 && opcode <= 0xE3) return true;   /* loop, jcxz */

    switch (opcode) {
    case 0x9A:                                   /* call far */
    case 0xC2: case 0xC3:                        /* ret */
    case 0xCA: case 0xCB:                        /* retf */
    case 0xCC: case 0xCD: case 0xCE: case 0xCF:  /* the interrupts */
    case 0xE8: case 0xE9: case 0xEA: case 0xEB:  /* call, jmp */
        return true;
    default:
        /* 0xFF has call and jump forms too, but which ones depends on the
         * ModRM byte, and the four reachable with the fills below are
         * INC, which is not one of them. */
        return false;
    }
}

/*
 * The decoder is a second opinion about an encoding the emulator already
 * implements, and a second opinion that is quietly wrong is worse than
 * none: it would print the operand bytes of the next instruction as part
 * of this one, and nothing would say so.
 *
 * So every opcode that is not a control transfer is put through both, in
 * four versions that between them reach all four ModRM modes, and the
 * bytes the emulator consumed are compared with the length that was
 * predicted. This is the test that makes the length table trustworthy;
 * without it the table is a guess with good manners.
 */
static void test_every_decoded_length_matches_what_the_step_consumes(struct vm86_cpu *cpu)
{
    static const uint8_t fills[] = { 0x00, 0x40, 0x80, 0xC0 };
    const vm86_op_fn *table = vm86_ops_table();

    if (!table) {
        vm86_expect_bool("the dispatch table is built", false, true);
        return;
    }

    int compared  = 0;
    int unclaimed = 0;
    int wrong     = 0;

    for (unsigned opcode = 0; opcode < 256u; opcode++) {
        if (prefix_byte((uint8_t)opcode) || control_transfer((uint8_t)opcode))
            continue;

        for (unsigned f = 0; f < sizeof(fills); f++) {
            uint8_t code[VM86_DBG_MAX_BYTES];

            memset(code, fills[f], sizeof(code));
            code[0] = (uint8_t)opcode;

            vm86_reset(cpu, cpu->mem);
            vm86_test_load(cpu, code, sizeof(code));

            struct vm86_dbg_decode decoded;
            vm86_dbg_decode(cpu, &decoded);

            uint16_t before  = cpu->ip;
            vm86_step(cpu);
            int16_t  advance = (int16_t)(uint16_t)(cpu->ip - before);

            int expected;

            if (!table[opcode]) {
                /*
                 * Nothing claims it, so the run loop fetches the opcode
                 * and raises the invalid-opcode exception without looking
                 * at the byte after it: one byte, whatever the encoding
                 * would have meant on a machine that had the instruction.
                 */
                unclaimed++;
                expected = 1;
            } else {
                compared++;
                expected = decoded.length;
            }

            if (advance != expected) {
                wrong++;
                printf("      %02X (fill %02X): decoded %u, consumed %d\n",
                       opcode, fills[f], (unsigned)expected, (int)advance);
            }
        }
    }

    printf("      %d claimed pairs compared, %d unclaimed\n", compared, unclaimed);

    vm86_expect_bool("every length agreed", wrong == 0, true);
    vm86_expect_bool("the sweep really ran", compared > 700, true);
    vm86_expect_bool("and saw the unclaimed opcodes", unclaimed > 20, true);
}

/*
 * The control transfers are excluded from the sweep, so their lengths are
 * pinned here by name -- otherwise "the transfers are handled elsewhere"
 * would be a hole rather than a delegation.
 */
static void test_the_control_transfers_decode_to_their_real_lengths(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = {
        0xEB, 0x00,                    /* 0: jmp rel8     -- 2 */
        0xE9, 0x00, 0x00,              /* 2: jmp rel16    -- 3 */
        0xEA, 0x00, 0x00, 0x00, 0x00,  /* 5: jmp far      -- 5 */
        0xE8, 0x00, 0x00,              /* 10: call rel16  -- 3 */
        0x9A, 0x00, 0x00, 0x00, 0x00,  /* 13: call far    -- 5 */
        0x70, 0x00,                    /* 18: jo rel8     -- 2 */
        0xCD, 0x21,                    /* 20: int 21h     -- 2 */
        0xE2, 0x00,                    /* 22: loop rel8   -- 2 */
    };
    static const struct { uint16_t at; uint8_t length; } expected[] = {
        { 0,  2 }, { 2,  3 }, { 5,  5 }, { 10, 3 },
        { 13, 5 }, { 18, 2 }, { 20, 2 }, { 22, 2 },
    };

    vm86_test_load(cpu, code, sizeof(code));

    for (unsigned i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        struct vm86_dbg_decode decoded;

        cpu->ip = (uint16_t)(VM86_TEST_CODE_BASE + expected[i].at);
        vm86_dbg_decode(cpu, &decoded);

        vm86_expect_u16("length", decoded.length, expected[i].length);
        vm86_expect_bool("is a control transfer", decoded.transfers_control, true);
    }

    /* And the one-byte ones, which need no operand bytes at all. */
    static const uint8_t singles[] = { 0xC3, 0xCB, 0xCC, 0xCE, 0xCF };

    for (unsigned i = 0; i < sizeof(singles); i++) {
        struct vm86_dbg_decode decoded;

        vm86_mem_write8(cpu->mem, VM86_TEST_CODE_BASE, singles[i]);
        cpu->ip = VM86_TEST_CODE_BASE;
        vm86_dbg_decode(cpu, &decoded);

        vm86_expect_u16("one byte", decoded.length, 1);
        vm86_expect_bool("and a transfer", decoded.transfers_control, true);
    }
}

static void test_the_decoder_counts_prefix_bytes(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = {
        0x26, 0x01, 0xD8,        /* es: add ax, bx    -- 3, one prefix */
        0xF3, 0xA4,              /* rep movsb         -- 2, one prefix */
        0x2E, 0x3E, 0xC3,        /* cs: ds: ret       -- 3, two prefixes */
    };
    struct vm86_dbg_decode decoded;

    vm86_test_load(cpu, code, sizeof(code));

    cpu->ip = VM86_TEST_CODE_BASE;
    vm86_dbg_decode(cpu, &decoded);
    vm86_expect_u16("es: add ax,bx length", decoded.length, 3);
    vm86_expect_u16("es: prefix count", decoded.prefixes, 1);

    cpu->ip = VM86_TEST_CODE_BASE + 3;
    vm86_dbg_decode(cpu, &decoded);
    vm86_expect_u16("rep movsb length", decoded.length, 2);
    vm86_expect_u16("rep prefix count", decoded.prefixes, 1);

    cpu->ip = VM86_TEST_CODE_BASE + 5;
    vm86_dbg_decode(cpu, &decoded);
    vm86_expect_u16("two prefixes", decoded.prefixes, 2);
    vm86_expect_u16("and the ret after them", decoded.length, 3);
}

/*
 * The addressing modes, which is where a length decoder goes wrong
 * quietly: every form but one is right, and the one is the direct
 * address, which carries a displacement with no base register.
 */
static void test_the_modrm_byte_decides_the_length(struct vm86_cpu *cpu)
{
    struct vm86_dbg_decode decoded;

    /* 8B is mov r16, r/m16: one ModRM byte, and a displacement if the
     * mode says so. */
    static const struct { uint8_t modrm; uint8_t length; } forms[] = {
        { 0x00, 2 },   /* [bx+si]        no displacement */
        { 0x06, 4 },   /* [disp16]       the direct address, a word */
        { 0x40, 3 },   /* [bx+si+disp8]  a byte */
        { 0x80, 4 },   /* [bx+si+disp16] a word */
        { 0xC0, 2 },   /* the register form: no displacement at all */
    };

    for (unsigned i = 0; i < sizeof(forms) / sizeof(forms[0]); i++) {
        uint8_t code[4] = { 0x8B, forms[i].modrm, 0x00, 0x00 };

        /* Loaded inside the loop: the decoder reads the guest's memory,
         * not this array, so writing the byte into the array and not into
         * memory would test nothing at all. */
        vm86_test_load(cpu, code, sizeof(code));

        cpu->ip = VM86_TEST_CODE_BASE;
        vm86_dbg_decode(cpu, &decoded);

        vm86_expect_u16("length", decoded.length, forms[i].length);
    }
}

/* ------------------------------------------------------------------ */
/* The flags text                                                      */
/* ------------------------------------------------------------------ */

static void test_the_flags_text_names_every_flag(struct vm86_cpu *cpu)
{
    struct vm86_dbg_out out = make_sink();

    cpu->flags = (uint16_t)(VM86_FLAG_ALWAYS_SET | VM86_CF | VM86_ZF | VM86_IF);

    vm86_dbg_dump_flags(cpu->flags, &out);

    expect_text("the word", "FLAGS ");
    expect_text("carry", "CF=1");
    expect_text("zero", "ZF=1");
    expect_text("interrupt enable", "IF=1");
    expect_text("parity", "PF=0");
    expect_text("auxiliary carry", "AF=0");
    expect_text("sign", "SF=0");
    expect_text("trap", "TF=0");
    expect_text("direction", "DF=0");
    expect_text("overflow", "OF=0");

    vm86_expect_bool("it wrote something", g_writes > 0, true);
    vm86_expect_bool("and never a NUL inside a chunk", !g_nul_inside, true);
}

/*
 * The bits that always read as one.
 *
 * Software of the era worked out which processor it was running on by
 * pushing FLAGS and looking at bit 1 and bits 12-15. A debugger that hid
 * them, or that printed the register verbatim, would make that test look
 * like a bug in the emulator -- so they are named on the line and they
 * read as one.
 */
static void test_the_flags_text_shows_the_bits_that_always_read_as_one(struct vm86_cpu *cpu)
{
    struct vm86_dbg_out out = make_sink();

    cpu->flags = VM86_FLAG_ALWAYS_SET;      /* what a reset machine holds */

    vm86_dbg_dump_flags(cpu->flags, &out);

    expect_text("bit 1 is named", "b1=");
    expect_text("and reads as one", "b1=1");
    expect_text("bits 12 to 15 are named", "b12-15=");
    expect_text("and read as ones", "b12-15=1111");
    expect_text("the value a program would read", "FLAGS F002");
}

/*
 * A register that has lost them.
 *
 * Nothing on an 8086 can produce this, so it means something assigned to
 * the register without normalising it. The line still shows what a
 * program would read -- because that is what the program sees -- and says
 * what the register actually holds, because that is the bug.
 */
static void test_the_flags_text_reports_a_register_missing_the_reserved_bits(struct vm86_cpu *cpu)
{
    struct vm86_dbg_out out = make_sink();

    cpu->flags = 0x0000;

    vm86_dbg_dump_flags(cpu->flags, &out);

    expect_text("what a program reads", "FLAGS F002");
    expect_text("still as ones", "b12-15=1111");
    expect_text("and the register is named", "stored 0000");
}

/* ------------------------------------------------------------------ */
/* The segment text                                                    */
/* ------------------------------------------------------------------ */

static void test_the_state_line_shows_a_segment_and_its_cached_base(struct vm86_cpu *cpu)
{
    struct vm86_dbg_out out = make_sink();

    vm86_set_seg(cpu, VM86_DS, 0x1234);
    vm86_flush_segments(cpu);               /* as the next access would */

    vm86_dbg_dump_state(cpu, &out);

    expect_text("the register", "DS=1234");
    expect_text("and its base", "base=12340");
    expect_no_text("with no complaint", "base=12340*");
    expect_no_text("and no disagreement", "base=12340!");
}

/*
 * The other half of showing both numbers: a base that has been marked
 * stale is not the address the machine will use, and saying so is the
 * point of printing it at all. Quietly recomputing the base from the
 * register would look the same here and would hide exactly the bug this
 * line exists to find.
 */
static void test_the_state_line_marks_a_base_that_has_not_been_rebuilt(struct vm86_cpu *cpu)
{
    struct vm86_dbg_out out = make_sink();

    vm86_set_seg(cpu, VM86_DS, 0x1234);     /* marks the cache stale */

    vm86_dbg_dump_state(cpu, &out);

    expect_text("the register", "DS=1234");
    expect_text("the stale cache, marked", "base=00000*");
    expect_no_text("and not the value it will become", "base=12340");
}

static void test_the_state_line_shows_every_register(struct vm86_cpu *cpu)
{
    struct vm86_dbg_out out = make_sink();

    cpu->ax = 0x1111; cpu->bx = 0x2222; cpu->cx = 0x3333; cpu->dx = 0x4444;
    cpu->sp = 0x5555; cpu->bp = 0x6666; cpu->si = 0x7777; cpu->di = 0x8888;
    cpu->ip = 0x9999;
    cpu->flags = VM86_FLAG_ALWAYS_SET;

    vm86_dbg_dump_state(cpu, &out);

    expect_text("ax", "AX=1111");
    expect_text("bx", "BX=2222");
    expect_text("cx", "CX=3333");
    expect_text("dx", "DX=4444");
    expect_text("sp", "SP=5555");
    expect_text("bp", "BP=6666");
    expect_text("si", "SI=7777");
    expect_text("di", "DI=8888");
    expect_text("ip", "IP=9999");
    expect_text("and the flags", "FLAGS F002");
    expect_text("with every segment", "CS=0000");
    expect_text("named", "SS=0000");
}

/* ------------------------------------------------------------------ */
/* The memory dump                                                     */
/* ------------------------------------------------------------------ */

static void test_the_memory_dump_shows_hex_and_ascii(struct vm86_cpu *cpu)
{
    struct vm86_dbg_out out = make_sink();

    for (uint32_t i = 0; i < 32u; i++)
        vm86_mem_write8(cpu->mem, 0x0400 + i, (uint8_t)('A' + i));

    vm86_dbg_dump_memory(cpu, 0x0400, 32, &out);

    expect_text("the address", "00400");
    expect_text("the first byte, in hex", "41");
    expect_text("the next one", "42");
    expect_text("a second line", "00410");
    expect_text("the printable column", "|ABCDEFGHIJKLMNOP|");
}

/*
 * Past the end of memory, which is the case the dump has to go through
 * the memory layer for: a floating bus reads as all ones, and a dump that
 * dereferenced the array would read past it instead, or print zeroes and
 * make an absent device look like a present one.
 */
static void test_the_memory_dump_past_the_end_reads_the_floating_bus(struct vm86_cpu *cpu)
{
    struct vm86_dbg_out out = make_sink();

    vm86_dbg_dump_memory(cpu, VM86_TEST_MEMORY - 4u, 16, &out);

    expect_text("the last real bytes", "FFFFC");
    expect_text("are the zeros the memory holds", "00 00 00 00");
    expect_text("and past them the bus floats high", "FF FF FF FF");
    expect_text("with nothing printable", "|................|");

    vm86_expect_bool("it survived it", g_writes > 0, true);
}

/*
 * An address must not wrap on the way to the screen.
 *
 * The megabyte is the 8086's whole address space and almost every dump
 * lives inside it, where the conventional five hex columns are enough.
 * Past the end they are not: a line at 0x10000C printed in five would
 * read as `0000C`, a low address, and a dump that misreports where the
 * bytes are is worse than no dump at all.
 */
static void test_the_memory_dump_keeps_the_address_past_the_megabyte(struct vm86_cpu *cpu)
{
    struct vm86_dbg_out out = make_sink();

    vm86_dbg_dump_memory(cpu, VM86_TEST_MEMORY - 4u, 32, &out);

    expect_text("the last of the real memory", "FFFFC");
    expect_text("and the line past it, not wrapped", "10000C");
    expect_text("with the bus floating above", "FF FF FF FF");
}

static void test_the_memory_dump_shows_unprintable_bytes_as_dots(struct vm86_cpu *cpu)
{
    struct vm86_dbg_out out = make_sink();

    vm86_mem_write8(cpu->mem, 0x0600, 0x00);
    vm86_mem_write8(cpu->mem, 0x0601, 0x1F);
    vm86_mem_write8(cpu->mem, 0x0602, 0x41);
    vm86_mem_write8(cpu->mem, 0x0603, 0x7F);

    vm86_dbg_dump_memory(cpu, 0x0600, 4, &out);

    expect_text("the bytes", "00 1F 41 7F");
    /* The character column is padded out to sixteen, so only its start is
     * a fixed string: two unprintable bytes, a letter, and another
     * unprintable one -- 0x7F is a control character, not a letter. */
    expect_text("and only the middle one is a character", "|..A.");
}

/* ------------------------------------------------------------------ */
/* The trace line                                                      */
/* ------------------------------------------------------------------ */

static void test_the_trace_line_shows_the_address_bytes_and_length(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0x01, 0xD8, 0xF4 };
    struct vm86_dbg_trace trace;
    struct vm86_dbg_out   out;

    vm86_test_load(cpu, code, sizeof(code));
    vm86_dbg_step(cpu, &trace);

    out = make_sink();
    vm86_dbg_dump_trace(&trace, &out);

    expect_text("where it was", "0000:0100");
    expect_text("the raw bytes", "01 D8");
    expect_text("the length", "(2)");
    expect_text("what happened", "continue");
    expect_text("and the state it left", "IP=0102");
    expect_no_text("with no mnemonic pretending to be one", "add ");
}

static void test_the_trace_line_names_a_fault_and_its_vector(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF1, 0xF4 };
    struct vm86_dbg_trace trace;
    struct vm86_dbg_out   out;

    vm86_test_load(cpu, code, sizeof(code));
    vm86_dbg_step(cpu, &trace);

    out = make_sink();
    vm86_dbg_dump_trace(&trace, &out);

    expect_text("the outcome", "fault");
    expect_text("the vector", "fault 06");
    expect_text("the byte", "F1");
}

/*
 * The trace of a jump to itself, which is where a measured length would
 * show up: the line has to say two bytes even though the state below it
 * shows the pointer back at the start.
 */
static void test_the_trace_of_a_self_jump_still_reports_two_bytes(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xEB, 0xFE, 0xF4 };
    struct vm86_dbg_trace trace;
    struct vm86_dbg_out   out;

    vm86_test_load(cpu, code, sizeof(code));
    vm86_dbg_step(cpu, &trace);

    out = make_sink();
    vm86_dbg_dump_trace(&trace, &out);

    expect_text("the bytes", "EB FE");
    expect_text("the length", "(2)");
    expect_text("and the pointer where it started", "IP=0100");
    expect_no_text("without claiming a disagreement", "did not move");
}

/* ------------------------------------------------------------------ */

static const struct vm86_test tests[] = {
    { "a step executes one instruction",
      test_a_step_executes_exactly_one_instruction },
    { "rep movsb with cx=0 still consumes the instruction",
      test_rep_movsb_with_cx_zero_still_consumes_the_instruction },
    { "rep movsb with a count moves everything",
      test_rep_movsb_with_a_count_moves_everything },
    { "a jump to itself leaves the pointer where it was",
      test_a_jump_to_itself_leaves_the_pointer_where_it_was },
    { "a backward jump records a shorter ip",
      test_a_backward_jump_records_a_shorter_ip },
    { "a fault records the state the instruction left",
      test_a_fault_records_the_state_the_instruction_left },
    { "an opcode nothing claims is one byte",
      test_an_opcode_nothing_claims_is_one_byte },
    { "stepping does not disturb the machine",
      test_stepping_does_not_disturb_the_machine },

    { "every decoded length matches what the step consumes",
      test_every_decoded_length_matches_what_the_step_consumes },
    { "the control transfers decode to their real lengths",
      test_the_control_transfers_decode_to_their_real_lengths },
    { "the decoder counts prefix bytes",
      test_the_decoder_counts_prefix_bytes },
    { "the modrm byte decides the length",
      test_the_modrm_byte_decides_the_length },

    { "the flags text names every flag",
      test_the_flags_text_names_every_flag },
    { "the flags text shows the bits that always read as one",
      test_the_flags_text_shows_the_bits_that_always_read_as_one },
    { "the flags text reports a register missing the reserved bits",
      test_the_flags_text_reports_a_register_missing_the_reserved_bits },

    { "the state line shows a segment and its cached base",
      test_the_state_line_shows_a_segment_and_its_cached_base },
    { "the state line marks a base that has not been rebuilt",
      test_the_state_line_marks_a_base_that_has_not_been_rebuilt },
    { "the state line shows every register",
      test_the_state_line_shows_every_register },

    { "the memory dump shows hex and ascii",
      test_the_memory_dump_shows_hex_and_ascii },
    { "the memory dump past the end reads the floating bus",
      test_the_memory_dump_past_the_end_reads_the_floating_bus },
    { "the memory dump keeps the address past the megabyte",
      test_the_memory_dump_keeps_the_address_past_the_megabyte },
    { "the memory dump shows unprintable bytes as dots",
      test_the_memory_dump_shows_unprintable_bytes_as_dots },

    { "the trace line shows the address bytes and length",
      test_the_trace_line_shows_the_address_bytes_and_length },
    { "the trace line names a fault and its vector",
      test_the_trace_line_names_a_fault_and_its_vector },
    { "the trace of a self jump still reports two bytes",
      test_the_trace_of_a_self_jump_still_reports_two_bytes },
};

VM86_TEST_MAIN("dbg", tests)
